/* TreeChop.cpp - Ctrl+Z+中键快速砍树 (选中地面单位强制攻击树木)。
 *
 * 触发: 按住 Ctrl + Z 的同时按住鼠标中键 (TriggerVK, 默认 4)
 * 显示生效格块菱形框, 松开才在当前格执行: 以鼠标格为基准扫
 * Range x Range (默认 4x4, 可配 2~16), 找树木地形, 选中单位
 * 按轮询均分强制攻击 (MegaMission Mission=Attack, Target=树对象)。
 * Z 按住即路径点语义, 事件带 IsPlanningEvent=1 (排进路径点队列)。
 * 与巡逻圈互斥: Ctrl 按住时巡逻圈让路 (见 PatrolCircle.cpp)。
 *
 * 找树 (YRpp 交叉确认):
 *   CellClass::GetTerrain (thiscall 0x47C550, ECX=格, 栈 bool) 直取
 *   TerrainClass*; 其 Type 指针偏移自校准 (Terrain 对象内扫指针,
 *   命中 TerrainType 数组 0xA8E318 成员即 pin); 类型 ID 读法与单位
 *   类型同基类 (候选 0x20/0x24/0x28, pin 后复用)。
 *   树判定: ID 以 "TREE" 开头 (TREE01~46/JP/N/B/H 全覆盖) 或以
 *   "TIBT" 开头 (TIBTRE01~03 纤维树); VEINTREE/岩石/箱子等排除
 *   (rulesmo.ini [TerrainTypes] 实测名单)。
 * 任务值: Mission::Attack=5 (YRpp GeneralDefinitions.h)。
 * 目标编码: TargetFromObj 0x6E6AB0 对树对象同样产出 ID+RTTI 描述,
 * 直接透传, 不硬编码 RTTI。
 *
 * 联机: 只发标准事件, 各端同一份原版代码执行, 不影响联机。
 * 本地只读 + 标准事件入队, 不写模拟内存。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MsgFont 同一 DLL)。
 * 轮询挂 PowerDrawBoth (见 MsgFont.cpp), 开关关时直接返回。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"

#define TC_SECTION "TreeChop"

/* ---------------- 砍树私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define ADDR_TERRAINTYPES 0xA8E318u  /* TerrainType 数组 (DynamicVector) */
#define OFF_VEC_ITEMS 4u
#define OFF_VEC_COUNT 16u

/* CellClass::GetTerrain (0x47C550, ECX=格, 栈 bool alt): 返回树对象或空。 */
typedef void* __attribute__((thiscall)) (*Fn_GetTerrain)(void* cell, int alt);
typedef void* (__fastcall *TC_LockFn)(void*, void*, int, int);
typedef int (__fastcall *TC_UnlockFn)(void*, void*);

#define VT_TC_LOCK   0x5C
#define VT_TC_UNLOCK 0x60

#define FN_GETTERRAIN ((Fn_GetTerrain)(void*)0x47C550u)

/* ---------------- 配置 ---------------- */

static int g_TcInit = 0;
static int g_TcEnable = 1;
static int g_TcTriggerVK = 4;   /* 4=中键 */
static int g_TcZVK = 90;        /* Z (路径点) */
static int g_TcRange = 4;       /* 扫描块边长 (格, 鼠标在块内 [Range/2][Range/2]) */
#define TC_CTRLVK 0x11          /* Ctrl (强制攻击) */

static void TC_EnsureInit(void)
{
    char buf[96], b1[16];
    unsigned q = 0;
    if (g_TcInit) return;
    g_TcInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_TcEnable = GetPrivateProfileIntA(TC_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    g_TcTriggerVK = GetPrivateProfileIntA(TC_SECTION, "TriggerVK", 4, g_IniPath);
    g_TcZVK = GetPrivateProfileIntA(TC_SECTION, "ZVK", 90, g_IniPath);
    g_TcRange = GetPrivateProfileIntA(TC_SECTION, "Range", 4, g_IniPath);
    if (g_TcTriggerVK < 1 || g_TcTriggerVK > 255) g_TcTriggerVK = 4;
    if (g_TcZVK < 1 || g_TcZVK > 255) g_TcZVK = 90;
    if (g_TcRange < 2) g_TcRange = 2;
    if (g_TcRange > 16) g_TcRange = 16;
    q = AppendCStr(buf, q, "treechop: ON (ctrl+Z+middle) range=", sizeof(buf));
    UtoA((unsigned)g_TcRange, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    (void)q;
    LogLine(buf);
    if (!g_TcEnable) LogLine("treechop: OFF");
}

/* ---------------- 小工具 (通用部分见 OrderUtil.h) ---------------- */

/* 只要地面 (飞机砍不了树, 不点)。 */
static int TC_IsChopper(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY) ? 1 : 0;
}

/* 强制攻击树: MegaMission(Attack, Target=树对象, Dst=空, 路径点=1)。 */
static int TC_FireAttack(int houseIdx, int srcID, unsigned char srcRtti,
                         int tgtID, unsigned char tgtRtti)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_ATTACK,
            tgtID, (unsigned)tgtRtti, 0, 0, 0, 0);
    ev[OFF_PLANFLAG] = 1;
    return OU_Enqueue(ev);
}

static void* TC_CellPtr(int cx, int cy)
{
    void** cells;
    int maxc, idx;
    if (!OU_CellValid(cx, cy)) return 0;
    cells = *(void***)(ADDR_DISPLAY + OFF_MAP_CELLS);
    maxc = *(int*)(ADDR_DISPLAY + OFF_MAP_MAXC);
    idx = (cy << 9) + cx;
    if (idx < 0 || idx >= maxc) return 0;
    return cells[idx];
}

/* ---------------- 树查找 (地形自校准 + ID 前缀) ---------------- */

static int g_TcTypeOff = -1;  /* TerrainClass 内 Type 指针偏移 pin */
static int g_TcIdOff = -1;    /* TerrainType 内 ID 字符串偏移 pin */

static int TC_StrOk(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (n < 24)
    {
        char c = s[n];
        if (!c) return (n > 0) ? 1 : 0;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-')) return 0;
        n++;
    }
    return 0;
}

/* 类型 ID 可读即拷出 (24 字节) 并 pin 偏移。 */
static int TC_TypeID(void* p, char* out)
{
    static const unsigned cands[3] = { 0x20u, 0x24u, 0x28u };
    int i, k;
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (g_TcIdOff >= 0)
    {
        const char* s = (const char*)p + (unsigned)g_TcIdOff;
        if (!TC_StrOk(s)) { g_TcIdOff = -1; }
        else
        {
            for (k = 0; k < 24; k++) out[k] = s[k];
            return 1;
        }
    }
    for (i = 0; i < 3; i++)
    {
        const char* s = (const char*)p + cands[i];
        if (!TC_StrOk(s)) continue;
        g_TcIdOff = (int)cands[i];
        for (k = 0; k < 24; k++) out[k] = s[k];
        return 1;
    }
    return 0;
}

/* 地形类型集合成员判定 (数组越界/坏读全回 0)。 */
static int TC_InTerrainSet(void* p)
{
    void** items;
    int cnt, i;
    if (!p) return 0;
    cnt = *(int*)(ADDR_TERRAINTYPES + OFF_VEC_COUNT);
    items = *(void***)(ADDR_TERRAINTYPES + OFF_VEC_ITEMS);
    if (cnt <= 0 || cnt > 20000) return 0;
    if (!items || !IsPlausiblePtr(items)) return 0;
    for (i = 0; i < cnt; i++)
        if (items[i] == p) return 1;
    return 0;
}

/* 树对象取 Type 指针 (pin 命中直取, 否则全扫; 带类型集合验证)。 */
static void* TC_TerrainTypeOf(void* t)
{
    unsigned off;
    char idb[24];
    int k;
    if (g_TcTypeOff >= 0)
    {
        void* p = *(void**)((char*)t + (unsigned)g_TcTypeOff);
        if (p && IsPlausiblePtr(p) && TC_InTerrainSet(p) && TC_TypeID(p, idb))
            return p;
        g_TcTypeOff = -1;
    }
    for (off = 0x40u; off < 0x900u; off += 4u)
    {
        void* p = *(void**)((char*)t + off);
        if (!p || !IsPlausiblePtr(p)) continue;
        if (!TC_InTerrainSet(p)) continue;
        for (k = 0; k < 24; k++) idb[k] = 0;
        if (!TC_TypeID(p, idb)) continue;
        g_TcTypeOff = (int)off;
        return p;
    }
    return 0;
}

/* 树判定: TREE* / TIBTRE* (VEINTREE/岩石/箱子等排除)。 */
static int TC_IsTreeID(const char* id)
{
    if (!id || !id[0]) return 0;
    if (id[0] == 'T' && id[1] == 'R' && id[2] == 'E' && id[3] == 'E')
        return 1;
    if (id[0] == 'T' && id[1] == 'I' && id[2] == 'B' && id[3] == 'T')
        return 1;
    return 0;
}

#define TC_TREE_CAP 256  /* Range 上限 16 → 16x16 格, 一格一树 */

static int TC_FindTrees(int cx, int cy, void** trees, int* treeIDs)
{
    int n = 0, dx, dy;
    int lo, hi;
    /* Range x Range: 鼠标在块内 [Range/2][Range/2] (Range=4 → 偏移 -2..+1)。 */
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
            cell = TC_CellPtr(cx + dx, cy + dy);
            if (!cell) continue;
            tr = FN_GETTERRAIN(cell, 0);
            if (!tr || !IsPlausiblePtr(tr)) continue;
            for (dup = 0; dup < n; dup++)
                if (trees[dup] == tr) break;
            if (dup < n) continue;
            tp = TC_TerrainTypeOf(tr);
            if (!tp) continue;
            for (k = 0; k < 24; k++) idb[k] = 0;
            if (!TC_TypeID(tp, idb)) continue;
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

/* ---------------- 待发队列 (保序 drip) ---------------- */

#define TC_UNIT_CAP 256
#define TC_PEND_CAP 512
#define TC_DRAIN_PER_POLL 128

typedef struct { void* unit; int uid; void* tree; int tid; } TCPend;

static TCPend g_TcPend[TC_PEND_CAP];
static int g_TcPendCount = 0;
static int g_TcIdxOk = 0;         /* ID 索引机制自检通过 (存活校验总开关) */
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

/* 排空: 发完即止 (攻击无多阶段门控)。中途 house 变化则清空。 */
static void TC_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx;
    int sent = 0;
    char buf[64], b1[16];
    unsigned q;
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
        /* 先验活 (纯读索引): 单位战死/树已倒即丢弃, 不碰悬空指针 */
        if (!OU_PtrAlive(e->unit, e->uid, g_TcIdxOk)) { TC_ShiftDown(); continue; }
        if (!TC_IsChopper(e->unit)) { TC_ShiftDown(); continue; }
        if (!OU_PtrAlive(e->tree, e->tid, g_TcIdxOk)) { TC_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(tgtB, e->tree)) { TC_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { TC_ShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        tgtID = *(int*)(tgtB + 0); tgtR = (unsigned char)tgtB[4];
        if (!TC_FireAttack(houseIdx, srcID, srcR, tgtID, tgtR))
            return; /* 满: 下轮继续 */
        TC_ShiftDown();
        sent++;
    }
    if (g_TcPendCount <= 0)
    {
        q = 0;
        q = AppendCStr(buf, q, "tc: drained sent=", sizeof(buf));
        UtoA((unsigned)sent, b1);
        q = AppendCStr(buf, q, b1, sizeof(buf));
        (void)q;
        LogLine(buf);
    }
}

/* ---------------- 触发 ---------------- */

static void TC_Press(void)
{
    void* cur;
    void** items;
    int selN, i, nu = 0, nt = 0;
    int cx, cy;
    char buf[96], b1[16], b2[16];
    unsigned q;
    static void* s_units[TC_UNIT_CAP];
    static int s_uids[TC_UNIT_CAP];
    static void* s_trees[TC_TREE_CAP];
    static int s_tids[TC_TREE_CAP];

    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items))
    { LogLine("tc: no selection"); return; }
    for (i = 0; i < selN && nu < TC_UNIT_CAP; i++)
    {
        void* o = items[i];
        if (TC_IsChopper(o)) s_units[nu++] = o;
    }
    if (nu <= 0) { LogLine("tc: no choppers"); return; }
    if (!OU_MouseCell(&cx, &cy)) { LogLine("tc: off map"); return; }
    /* 快照单位 Fetch_ID (存活校验用; 选中单位必活, 此处 vcall 安全) */
    for (i = 0; i < nu; i++)
    {
        char tb[8];
        unsigned ti;
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        s_uids[i] = FN_TGT_OBJ(tb, s_units[i]) ? *(int*)(tb + 0) : 0;
    }
    /* ID 索引自检 (fail-open): 首单位查不到则本轮跳过存活校验 */
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
    if (nt <= 0)
    {
        /* 附地形数组计数: 0=数组未加载/地址不对, >0=这块真没树。 */
        char nb[64], n1[16];
        unsigned nq = 0;
        int tcnt = *(int*)(ADDR_TERRAINTYPES + OFF_VEC_COUNT);
        nq = AppendCStr(nb, nq, "tc: no trees types=", sizeof(nb));
        UtoA((unsigned)(tcnt > 0 ? tcnt : 0), n1);
        nq = AppendCStr(nb, nq, n1, sizeof(nb));
        (void)nq;
        LogLine(nb);
        return;
    }
    q = 0;
    q = AppendCStr(buf, q, "tc: chop trees=", sizeof(buf));
    UtoA((unsigned)nt, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " units=", sizeof(buf));
    UtoA((unsigned)nu, b2);
    q = AppendCStr(buf, q, b2, sizeof(buf));
    (void)q;
    LogLine(buf);
    /* 建队: 单位按轮询均分到树 (unit i -> tree[i % nt]) */
    g_TcPendCount = 0;
    for (i = 0; i < nu; i++)
    {
        int t = i % nt;
        if (g_TcPendCount >= TC_PEND_CAP) break;
        g_TcPend[g_TcPendCount].unit = s_units[i];
        g_TcPend[g_TcPendCount].uid = s_uids[i];
        g_TcPend[g_TcPendCount].tree = s_trees[t];
        g_TcPend[g_TcPendCount].tid = s_tids[t];
        g_TcPendCount++;
    }
    g_TcHouse = cur;
    g_TcHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    TC_Drain();
}

/* ---------------- 每循环轮询 (挂 PowerDrawBoth) ---------------- */

static void TC_BoxReset(void);
static void TC_BoxSample(void);

void TreeChop_Poll(void)
{
    int trigDown;
    int ctrl, z, combo;

    TC_EnsureInit();
    trigDown = (GetAsyncKeyState(g_TcTriggerVK) & 0x8000) ? 1 : 0;
    ctrl = (GetAsyncKeyState(TC_CTRLVK) & 0x8000) ? 1 : 0;
    z = (GetAsyncKeyState(g_TcZVK) & 0x8000) ? 1 : 0;
    combo = (ctrl && z) ? 1 : 0;
    if (g_TcPendCount > 0)
    { TC_Drain(); g_TcPrevTrig = trigDown; return; } /* drip 续发 */
    if (!g_TcEnable) { g_TcPrevTrig = trigDown; return; }
    if (trigDown && !g_TcPrevTrig)
        TC_BoxReset();            /* 按下: 采样器清零 (无动作, 松开才执行) */
    if (trigDown && combo)
        TC_BoxSample();           /* 按住: 采样画框 (+像素质校准) */
    else if (!trigDown && g_TcPrevTrig && combo)
        TC_Press();               /* 松开: 当前格执行 */
    g_TcPrevTrig = trigDown;
}

/* ---------------- 渲染期范围框 (按住显示, 松开执行) ---------------- */
/* 格→像素固定等距映射: X+1 格 = (+30,+15)px, Y+1 格 = (-30,+15)px
 * (RA2 格 60x30, 无缩放)。故意不用采样解算: 中键按住平移镜头,
 * 前后采样不在同一镜头帧, 解出的矩阵是垃圾 (框乱 skew 的根因)。
 * 参考点按轮刷新 (像素+格同轮, 恒一致), 框 glue 在鼠标上。 */

static int g_TcBRx = 0, g_TcBRy = 0, g_TcBRcx = 0, g_TcBRcy = 0;
static int g_TcBHas = 0;

static void TC_BoxReset(void)
{
    g_TcBHas = 0;
}

/* 按住期间每轮跟踪最新 (像素+格) 参考点。 */
static void TC_BoxSample(void)
{
    int mx, my, cx, cy;
    if (!GtMouseGameXY(&mx, &my)) return;
    if (!OU_MouseCell(&cx, &cy)) return;
    g_TcBRx = mx; g_TcBRy = my; g_TcBRcx = cx; g_TcBRcy = cy;
    g_TcBHas = 1;
}

static unsigned char* s_TcLocked = 0;
static int s_TcW = 0, s_TcH = 0, s_TcPitch = 0;

static void TC_Plot(int x, int y, unsigned a)
{
    unsigned short* dstRow;
    if (x < 0 || y < 0 || x >= s_TcW || y >= s_TcH) return;
    dstRow = (unsigned short*)(s_TcLocked + y * s_TcPitch);
    dstRow[x] = Blend16(dstRow[x], 255, 255, 255, a);
}

/* 3px 线 (Bresenham + 上下各 1px, 零依赖)。 */
static void TC_Line(int x0, int y0, int x1, int y1, unsigned a)
{
    int dx, dy, sx, sy, err, e2, x, y, n;
    dx = x1 - x0; dy = y1 - y0;
    sx = (dx >= 0) ? 1 : -1;
    sy = (dy >= 0) ? 1 : -1;
    dx = dx >= 0 ? dx : -dx; dy = dy >= 0 ? dy : -dy;
    err = dx - dy;
    x = x0; y = y0;
    for (n = 0; n < 4096; n++)
    {
        TC_Plot(x, y, a);
        TC_Plot(x, y - 1, a);
        TC_Plot(x, y + 1, a);
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

/* 实心方块标记 (半边 m)。 */
static void TC_Mark(int cx, int cy, int m, unsigned a)
{
    int k, j;
    for (k = -m; k <= m; k++)
        for (j = -m; j <= m; j++)
            TC_Plot(cx + k, cy + j, a);
}

/* 格块角 → 像素 (参考点 + 固定等距差)。 */
static void TC_BoxCorner(int ccx, int ccy, int* px, int* py)
{
    int dx = ccx - g_TcBRcx, dy = ccy - g_TcBRcy;
    *px = g_TcBRx + dx * 30 - dy * 30;
    *py = g_TcBRy + dx * 15 + dy * 15;
}

void TreeChop_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* vt;
    TC_LockFn lockFn;
    TC_UnlockFn unlockFn;
    unsigned char* locked;
    int lo, hi, x0, y0, x1, y1, x2, y2, x3, y3;
    extern int g_Calibrated;
    if (!g_TcEnable || !g_TcBHas) return;
    if (!(GetAsyncKeyState(g_TcTriggerVK) & 0x8000)) return;  /* 按住才画 */
    if (!(GetAsyncKeyState(TC_CTRLVK) & 0x8000)) return;
    if (!(GetAsyncKeyState(g_TcZVK) & 0x8000)) return;        /* 缺 Z 归别家 */
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    lockFn = (TC_LockFn)(*(void**)(vt + VT_TC_LOCK));
    unlockFn = (TC_UnlockFn)(*(void**)(vt + VT_TC_UNLOCK));
    if (!lockFn || !unlockFn) return;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return;
    s_TcLocked = locked; s_TcW = w; s_TcH = h; s_TcPitch = pitch;
    /* 生效格块 (参考格为块中心锚点, 与 Find 一致) 菱形框 + 四角标记。
     * 注: 中键拖动平移镜头时参考点按轮更新, 帧间小漂, 与各家线同 caveat。 */
    lo = -(g_TcRange / 2);
    hi = lo + g_TcRange - 1;
    TC_BoxCorner(g_TcBRcx + lo, g_TcBRcy + lo, &x0, &y0);
    TC_BoxCorner(g_TcBRcx + hi + 1, g_TcBRcy + lo, &x1, &y1);
    TC_BoxCorner(g_TcBRcx + hi + 1, g_TcBRcy + hi + 1, &x2, &y2);
    TC_BoxCorner(g_TcBRcx + lo, g_TcBRcy + hi + 1, &x3, &y3);
    TC_Line(x0, y0, x1, y1, 230);
    TC_Line(x1, y1, x2, y2, 230);
    TC_Line(x2, y2, x3, y3, 230);
    TC_Line(x3, y3, x0, y0, 230);
    TC_Mark(x0, y0, 1, 230);
    TC_Mark(x1, y1, 1, 230);
    TC_Mark(x2, y2, 1, 230);
    TC_Mark(x3, y3, 1, 230);
    s_TcLocked = 0;
    unlockFn(surf, NULL);
}
