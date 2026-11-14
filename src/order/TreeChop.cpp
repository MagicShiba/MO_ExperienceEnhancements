/* TreeChop.cpp - Ctrl+Z+中键快速砍树。
 * 按住显示 Range x Range 菱形框, 松开在当前鼠标格执行。
 * 框内目标用地形扫描找 (可砍前缀名单见 TC_IsTreeID / g_TcChopPfx),
 * 参战单位要先过弹头关 (TC_UnitCanChop): 没武器或弹头没写 Wood=yes 的留在原地。
 * 单位/树按 max 轮转全覆盖分配 (k % nu / k % nt, IsPlanningEvent=1 排队):
 * 树多一单位规划多棵且覆盖选区内全部树, 树少多单位集火同一棵。
 * 画框走引擎 CoordsToClient 投影格线角点, 与扫描同格。
 * 只读查询 + 标准事件, 不写模拟内存。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"
#include "ScaleBar.h"

#define TC_SECTION "TreeChop"

/* 砍树私有地址 (共享部分见 OrderUtil.h) */

/* CellClass::GetTerrain (0x47C550): 返回格上地形或空。 */
typedef void* __attribute__((thiscall)) (*Fn_GetTerrain)(void* cell, int alt);
#define FN_GETTERRAIN ((Fn_GetTerrain)(void*)0x47C550u)

/* 可砍地形限名单 (按地形 ID 前缀, 命中即砍)。
 * 树 TREE*; 城市小品 红绿灯 TRFF* / 交通标志 SIGN* /
 * 路灯 LT_GEN* LT_EUR* LT_SGN* (带牌路灯)。全是 rulesmo.ini 里 Armor=tree 的可摧毁地形。
 * TIBTRE01~03 (Tiberium Tree, 矿柱) 是 Immune=yes 的无敌地形, 绝不进名单:
 * 派兵过去只会白跑, 单位还被钉在那儿。VEINTREE 同理 (IsVeinhole)。 */
static const char* const g_TcChopPfx[] =
{
    "TREE", "TRFF", "SIGN", "LT_GEN", "LT_EUR", "LT_SGN"
};

/* 配置 */
static int g_TcInit = 0;
static int g_TcEnable = 1;
static OU_Hotkey g_TcHk;
static int g_TcRange = 4;
static int g_TcWoodCheck = 1;   /* 只派"弹头写了 Wood=yes"的单位 ([TreeChop] WoodCheck=0 关掉) */

static void TC_EnsureInit(void)
{
    char buf[96];
    unsigned q = 0;
    if (g_TcInit) return;
    g_TcInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_TcEnable = GetPrivateProfileIntA(TC_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    OU_ParseHotkey("Ctrl+Z+Middle", &g_TcHk);
    OU_ReadHotkey(TC_SECTION, "Hotkey", &g_TcHk);
    if (g_TcHk.disabled) g_TcEnable = 0;
    g_TcRange = GetPrivateProfileIntA(TC_SECTION, "Range", 4, g_IniPath);
    if (g_TcRange < 2) g_TcRange = 2;
    if (g_TcRange > 16) g_TcRange = 16;
    g_TcWoodCheck = GetPrivateProfileIntA(TC_SECTION, "WoodCheck", 1, g_IniPath) ? 1 : 0;
    q = AppendCStr(buf, q, "treechop: ON (ctrl+Z+middle) range=", sizeof(buf));
    q = OU_AppendU(buf, q, (unsigned)g_TcRange, sizeof(buf));
    q = AppendCStr(buf, q, g_TcWoodCheck ? " woodcheck=on" : " woodcheck=OFF", sizeof(buf));
    (void)q;
    LogLine(buf);
    if (!g_TcEnable) LogLine("treechop: OFF");
}

/* 设置页改完热键后回读 (ini 已写盘)。 */
void TreeChop_ReloadHotkey(void)
{
    TC_EnsureInit();
    g_TcEnable = GetPrivateProfileIntA(TC_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    OU_ParseHotkey("Ctrl+Z+Middle", &g_TcHk);
    OU_ReadHotkey(TC_SECTION, "Hotkey", &g_TcHk);
    if (g_TcHk.disabled) g_TcEnable = 0;
}

/* 地面单位才能砍树。 */
static int TC_IsChopper(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY) ? 1 : 0;
}

/* 强制攻击树 (MegaMission Attack + IsPlanningEvent=1)。 */
static int TC_FireAttack(int houseIdx, int srcID, unsigned char srcRtti,
                         int tgtID, unsigned char tgtRtti,
                         void* unit, int uid, int idxOk)
{
    return OU_FireMega(houseIdx, srcID, srcRtti, MISSION_ATTACK,
                       tgtID, tgtRtti, 0, 0, 0, 0, 1, unit, uid, idxOk);
}

/* ---------------- 弹头能不能伤木 (能不能砍) ----------------
 * 判据只有一条: 弹头写了 `Wood=yes`。
 * 不看 Verses, 也不看建筑的木甲 (`Armor=tree`) —— 那是另一套东西, 自定义护甲一加,
 * wood 档伤害就跟"能不能拆木结构"脱节。
 * 数据从引擎的 rules INIClass 现读: 武器节 `Warhead=` → 弹头节 `Wood=`。
 * 只读查询, 联机安全。 */

typedef void* (__attribute__((thiscall)) *Fn_TC_GetWeapon)(void* self, int idx);
typedef int (__attribute__((thiscall)) *Fn_TC_IniStr)(void* ini, const char* sec,
    const char* key, const char* def, char* buf, unsigned size);

#define FN_TC_INISTR ((Fn_TC_IniStr)(void*)FN_INI_READSTRING)
#define TC_CHOP_CACHE 32         /* 武器 → 能不能砍 缓存条数 */
#define TC_SEC_POOL 32           /* 弹头节名池 (见 TC_SecName) */

/* 类型 ID 可读: 非空 + 全可打印。别换成 OU_StrOk —— 它只认大写, 驼峰武器 ID
 * (`MissileLauncher` / `120mm`) 会被整条判坏, 带导弹的单位反而"不能砍"。 */
static int TC_IdOk(const char* s)
{
    int n;
    if (!s) return 0;
    for (n = 0; n < 23 && s[n]; n++)
        if ((unsigned char)s[n] < 0x20 || (unsigned char)s[n] > 0x7E) return 0;
    return n > 0;
}

/* 弹头节名要持久、且每个名字一个地址: INIClass 按节名**指针**缓存上一节
 * (EngineAddr.h 那条坑), 传栈上缓冲会让后面的查询命中上一节的数据。 */
static char s_TcSec[TC_SEC_POOL][32];
static int s_TcSecN = 0;

static const char* TC_SecName(const char* name)
{
    int i;
    for (i = 0; i < s_TcSecN; i++)
        if (!lstrcmpA(s_TcSec[i], name)) return s_TcSec[i];
    if (s_TcSecN >= TC_SEC_POOL) return name;
    lstrcpynA(s_TcSec[s_TcSecN], name, 32);
    return s_TcSec[s_TcSecN++];
}

static int TC_IniStr(void* ini, const char* sec, const char* key, char* out, unsigned cap)
{
    out[0] = 0;
    if (!ini || !IsPlausiblePtr(ini) || !sec || !sec[0]) return 0;
    FN_TC_INISTR(ini, sec, key, "", out, cap);
    return out[0] ? 1 : 0;
}

/* yes/no → 1/0, 没写或不认识 → -1。 */
static int TC_Flag3(const char* v)
{
    if (!v || !v[0]) return -1;
    if (v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' || v[0] == '1') return 1;
    if (v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F' || v[0] == '0') return 0;
    return -1;
}

/* 一把武器砍不砍得动: 1=弹头 Wood=yes; 0=其余 (没武器/读不到/没写/写 no)。 */
static int TC_WeaponChops(void* ini, void* wt)
{
    const char* wsec;
    char wh[32], v[8];

    if (!wt || !IsPlausiblePtr(wt)) return 0;
    wsec = (const char*)((char*)wt + OFF_TYPE_ID);   /* 武器节名 = 武器 ID */
    if (MemReadableBytes(wsec, 0x18) < 0x18 || !TC_IdOk(wsec)) return 0;
    wh[0] = 0;
    v[0] = 0;
    if (!TC_IniStr(ini, wsec, "Warhead", wh, sizeof(wh))) return 0;
    if (!TC_IniStr(ini, TC_SecName(wh), "Wood", v, sizeof(v))) return 0;
    return (TC_Flag3(v) == 1) ? 1 : 0;
}

static void* s_TcWp[TC_CHOP_CACHE];
static int s_TcChops[TC_CHOP_CACHE];
static int s_TcWpN = 0;
static void* s_TcWpIni = 0;

/* 按武器缓存判据 (换局换 rules 指针即清)。 */
static int TC_WeaponChopsCached(void* ini, void* wt)
{
    int i;
    if (ini != s_TcWpIni) { s_TcWpIni = ini; s_TcWpN = 0; s_TcSecN = 0; }
    for (i = 0; i < s_TcWpN; i++)
        if (s_TcWp[i] == wt) return s_TcChops[i];
    if (s_TcWpN >= TC_CHOP_CACHE) s_TcWpN = 0;
    s_TcWp[s_TcWpN] = wt;
    s_TcChops[s_TcWpN] = TC_WeaponChops(ini, wt);
    return s_TcChops[s_TcWpN++];
}

/* 单位当前武器 (0=主武器, 没有再试 2=精英/部署)。0 = 没有武器。 */
static void* TC_UnitWeapon(void* u)
{
    void** vt;
    Fn_TC_GetWeapon gw;
    void* ws;
    void* wt;
    int idx;

    if (!u || !IsPlausiblePtr(u)) return 0;
    vt = *(void***)u;
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    gw = (Fn_TC_GetWeapon)vt[OFF_VT_GETWEAPON / 4];
    if (!gw || !IsPlausiblePtr((const void*)gw)) return 0;
    for (idx = 0; idx <= 2; idx += 2)
    {
        ws = gw(u, idx);
        if (!ws || !IsPlausiblePtr(ws)) continue;
        wt = *(void**)ws;
        if (wt && IsPlausiblePtr(wt)) return wt;
    }
    return 0;
}

/* 能砍树吗: 有武器 且 弹头 Wood=yes。没武器 (= 不能攻击) 一律不动。
 * rules 读不到时放行, 免得一次取数失败把整队筛空。 */
static int TC_UnitCanChop(void* u)
{
    void* ini;
    void* wt;

    if (!g_TcWoodCheck) return 1;
    wt = TC_UnitWeapon(u);
    if (!wt) return 0;
    ini = *(void**)ADDR_INI_RULES;
    if (!ini || !IsPlausiblePtr(ini)) return 1;
    return TC_WeaponChopsCached(ini, wt);
}

/* 画框投影: 格线角点走引擎 CoordsToClient, 每角取框内格高。 */

typedef int __attribute__((thiscall)) (*Fn_TC_C2C)(void* tac,
    const CoordStruct* c, Point2D* out);
#define FN_TC_C2C ((Fn_TC_C2C)(void*)0x6D2140u)
#define TC_TAC_INSTANCE 0x887324u

/* 角点高度 = 其框内格高度, 失败回落 fb。 */
static int TC_CornerZ(int cx, int cy, int fb, int* zOut)
{
    if (OU_CellFloorZ(cx, cy, zOut)) return 1;
    *zOut = fb;
    return 1;
}

static int TC_ProjLep(int lx, int ly, int lz, int* px, int* py)
{
    void* tac = *(void**)TC_TAC_INSTANCE;
    CoordStruct c;
    Point2D o;
    if (!tac || !IsPlausiblePtr(tac)) return 0;
    c.X = lx; c.Y = ly; c.Z = lz;
    o.X = 0; o.Y = 0;
    FN_TC_C2C(tac, &c, &o);
    if (!SB_ViewToScreen(o.X, o.Y, px, py)) { *px = o.X; *py = o.Y; }
    return 1;
}

static int TC_GridCorner(int gx, int gy, int gz, int* px, int* py)
{
    return TC_ProjLep(gx * 256, gy * 256, gz, px, py);
}

/* 树查找 (类型指针自校准公共实现见 OrderUtil.h) */

static int g_TcTypeOff = -1;   /* 树对象内 Type 指针偏移 pin */
static int g_TcIdOff = -1;     /* 类型内 ID 字符串偏移 pin */

/* id 以 pfx 开头回 1。id 是 24 字节零填充串 (见 TC_FindTrees), 逐字节比到
 * pfx 结束即可, 不会读越界。 */
static int TC_Pfx(const char* id, const char* pfx)
{
    int i;
    for (i = 0; pfx[i]; i++)
        if (id[i] != pfx[i]) return 0;
    return 1;
}

/* 命中 g_TcChopPfx 任一前缀即可砍。 */
static int TC_IsTreeID(const char* id)
{
    unsigned i;
    if (!id || !id[0]) return 0;
    for (i = 0; i < sizeof(g_TcChopPfx) / sizeof(g_TcChopPfx[0]); i++)
        if (TC_Pfx(id, g_TcChopPfx[i])) return 1;
    return 0;
}

#define TC_TREE_CAP 256

static int TC_FindTrees(int cx, int cy, void** trees, int* treeIDs)
{
    int n = 0, dx, dy;
    int lo, hi;
    lo = -(g_TcRange / 2);
    hi = lo + g_TcRange - 1;
    for (dy = lo; dy <= hi; dy++)
    {
        for (dx = lo; dx <= hi; dx++)
        {
            void* cell;
            void* tr;
            void* tp;
            char idb[24];
            char tb[8];
            unsigned ti;
            int k, dup;
            cell = OU_CellPtr(cx + dx, cy + dy);
            if (!cell) continue;
            tr = FN_GETTERRAIN(cell, 0);
            if (!tr || !IsPlausiblePtr(tr)) continue;
            for (dup = 0; dup < n; dup++)
                if (trees[dup] == tr) break;
            if (dup < n) continue;
            tp = OU_ScanTypeOf(tr, &g_TcTypeOff, ADDR_TERRAINTYPES, &g_TcIdOff);
            if (!tp) continue;
            for (k = 0; k < 24; k++) idb[k] = 0;
            if (!OU_TypeID(tp, idb, &g_TcIdOff)) continue;
            if (!TC_IsTreeID(idb)) continue;
            for (ti = 0; ti < 8; ti++) tb[ti] = 0;
            if (!FN_TGT_OBJ(tb, tr)) continue;
            if (n >= TC_TREE_CAP) break;
            trees[n] = tr;
            treeIDs[n] = *(int*)(tb + 0);
            n++;
        }
    }
    return n;
}

/* 待发队列 */

#define TC_UNIT_CAP 1024   /* 快照单位上限 (超限进 OT 队列分帧, 与 OT_QCAP 对齐) */
#define TC_PEND_CAP 1024   /* 待发上限 (同上) */
#define TC_DRAIN_PER_POLL 128

typedef struct { void* unit; int uid; void* tree; int tid; } TCPend;

static TCPend g_TcPend[TC_PEND_CAP];
static int g_TcPendCount = 0;
static int g_TcIdxOk = 0;
static void* g_TcHouse = 0;
static int g_TcHouseIdx = 0;
static int g_TcPrevTrig = 0;

static void TC_ShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_TcPendCount; k++)
    {
        g_TcPend[k].unit = g_TcPend[k + 1].unit;
        g_TcPend[k].uid = g_TcPend[k + 1].uid;
        g_TcPend[k].tree = g_TcPend[k + 1].tree;
        g_TcPend[k].tid = g_TcPend[k + 1].tid;
    }
    g_TcPendCount--;
}

/* 排空队列。house 变化则清空。 */
static void TC_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx;
    int sent = 0;
    if (!cur || cur != g_TcHouse) { g_TcPendCount = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (houseIdx != g_TcHouseIdx) { g_TcPendCount = 0; return; }
    while (g_TcPendCount > 0 && sent < TC_DRAIN_PER_POLL)
    {
        TCPend* e = &g_TcPend[0];
        char srcB[8], tgtB[8];
        unsigned i;
        int srcID, tgtID;
        unsigned char srcR, tgtR;
        for (i = 0; i < 8; i++) srcB[i] = tgtB[i] = 0;
        /* 存活校验不过即丢弃。 */
        if (!OU_PtrAlive(e->unit, e->uid, g_TcIdxOk)) { TC_ShiftDown(); continue; }
        if (!TC_IsChopper(e->unit)) { TC_ShiftDown(); continue; }
        if (!OU_PtrAlive(e->tree, e->tid, g_TcIdxOk)) { TC_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(tgtB, e->tree)) { TC_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { TC_ShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        tgtID = *(int*)(tgtB + 0); tgtR = (unsigned char)tgtB[4];
        if (!TC_FireAttack(houseIdx, srcID, srcR, tgtID, tgtR,
                             e->unit, e->uid, g_TcIdxOk))
            return;
        TC_ShiftDown();
        sent++;
    }
}

/* 触发 */

static void TC_Press(void)
{
    void* cur;
    void** items;
    int selN, i, nu = 0, nt = 0;
    int cx, cy;
    static void* s_units[TC_UNIT_CAP];
    static int s_uids[TC_UNIT_CAP];
    static void* s_trees[TC_TREE_CAP];
    static int s_tids[TC_TREE_CAP];

    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return;
    for (i = 0; i < selN && nu < TC_UNIT_CAP; i++)
    {
        void* o = items[i];
        if (!TC_IsChopper(o)) continue;
        /* 敌我识别: 只派自家单位 (敌人/中立/盟友都不接砍树令)。 */
        if (OU_OrderSide(o) != 1) continue;
        /* 弹头没写 Wood=yes (或压根没武器) 的留在原地, 不派。 */
        if (!TC_UnitCanChop(o)) continue;
        s_units[nu++] = o;
    }
    if (nu <= 0) return;
    if (!OU_MouseCell(&cx, &cy)) return;
    for (i = 0; i < nu; i++)
    {
        char tb[8];
        unsigned ti;
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        s_uids[i] = FN_TGT_OBJ(tb, s_units[i]) ? *(int*)(tb + 0) : 0;
    }
    g_TcIdxOk = 0;
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        if (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 200000 &&
            s_uids[0] > 0 &&
            OU_PtrAliveRaw(s_units[0], s_uids[0]))
            g_TcIdxOk = 1;
    }
    nt = TC_FindTrees(cx, cy, s_trees, s_tids);
    if (nt <= 0) return;
    /* 单位/树按 max 轮转全覆盖 (k % nu / k % nt, IsPlanningEvent=1 排队):
     * 树多 (nt>nu): 覆盖选区内全部树, 每单位规划多棵;
     * 树少 (nu>nt): 全部单位参战, 每棵树被多单位集火。 */
    g_TcPendCount = 0;
    {
        int total = (nt > nu) ? nt : nu;
        for (i = 0; i < total; i++)
        {
            int u = i % nu;
            int t = i % nt;
            if (g_TcPendCount >= TC_PEND_CAP) break;
            g_TcPend[g_TcPendCount].unit = s_units[u];
            g_TcPend[g_TcPendCount].uid = s_uids[u];
            g_TcPend[g_TcPendCount].tree = s_trees[t];
            g_TcPend[g_TcPendCount].tid = s_tids[t];
            g_TcPendCount++;
        }
    }
    g_TcHouse = cur;
    g_TcHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    TC_Drain();
}

void TreeChop_Poll(void)
{
    int trigDown;
    int combo;

    TC_EnsureInit();
    trigDown = OU_KeyDown(g_TcHk.vk);
    combo = (OU_ModsDown(g_TcHk.mods) && OU_ExtraDown(&g_TcHk)) ? 1 : 0;
    if (g_TcPendCount > 0)
    { TC_Drain(); g_TcPrevTrig = trigDown; return; }
    if (!g_TcEnable) { g_TcPrevTrig = trigDown; return; }
    if (!trigDown && g_TcPrevTrig && combo)
        TC_Press();
    g_TcPrevTrig = trigDown;
}

/* 范围框 (按住显示, 与扫描同格)。 */

static unsigned char* s_TcLocked = 0;
static int s_TcW = 0, s_TcH = 0, s_TcPitch = 0;
static OuClip s_TcClip;

/* 3px 白线 / 实心方块 (公共实现: OrderUtil 的 OU_Line / OU_Box)。 */
static void TC_Line(int x0, int y0, int x1, int y1, unsigned a)
{
    OU_Line(s_TcLocked, s_TcW, s_TcH, s_TcPitch, &s_TcClip,
            x0, y0, x1, y1, 255, 255, 255, a, 3);
}

static void TC_Mark(int cx, int cy, int m, unsigned a)
{
    OU_Box(s_TcLocked, s_TcW, s_TcH, s_TcPitch, &s_TcClip, cx, cy, m, 255, 255, 255, a);
}

void TreeChop_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* locked;
    int lo, hi, x0, y0, x1, y1, x2, y2, x3, y3;
    int ccx, ccy, clz, z0, z1, z2, z3;
    int bx0, by0, bx1, by1;
    extern int g_Calibrated;
    if (!g_TcEnable) return;
    if (!OU_KeyDown(g_TcHk.vk)) return;
    if (!OU_ModsDown(g_TcHk.mods) || !OU_ExtraDown(&g_TcHk)) return;
    if (!g_Calibrated) return;
    if (!OU_MouseCellZ(&ccx, &ccy, &clz)) return;
    lo = -(g_TcRange / 2);
    hi = lo + g_TcRange - 1;
    bx0 = ccx + lo; by0 = ccy + lo;
    bx1 = ccx + hi + 1; by1 = ccy + hi + 1;
    TC_CornerZ(bx0, by0, clz, &z0);
    TC_CornerZ(bx1 - 1, by0, clz, &z1);
    TC_CornerZ(bx1 - 1, by1 - 1, clz, &z2);
    TC_CornerZ(bx0, by1 - 1, clz, &z3);
    if (!TC_GridCorner(bx0, by0, z0, &x0, &y0)) return;
    if (!TC_GridCorner(bx1, by0, z1, &x1, &y1)) return;
    if (!TC_GridCorner(bx1, by1, z2, &x2, &y2)) return;
    if (!TC_GridCorner(bx0, by1, z3, &x3, &y3)) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    locked = OU_SurfLock(surf);
    if (!locked) return;
    s_TcLocked = locked; s_TcW = w; s_TcH = h; s_TcPitch = pitch;
    OU_ViewClip(&s_TcClip, w, h);
    TC_Line(x0, y0, x1, y1, 230);
    TC_Line(x1, y1, x2, y2, 230);
    TC_Line(x2, y2, x3, y3, 230);
    TC_Line(x3, y3, x0, y0, 230);
    TC_Mark(x0, y0, 1, 230);
    TC_Mark(x1, y1, 1, 230);
    TC_Mark(x2, y2, 1, 230);
    TC_Mark(x3, y3, 1, 230);
    s_TcLocked = 0;
    OU_SurfUnlock(surf);
}
