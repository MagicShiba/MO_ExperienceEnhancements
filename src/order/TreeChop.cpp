/* TreeChop.cpp - Ctrl+Z+中键快速砍树。
 * 按住显示 Range x Range 菱形框, 松开在当前鼠标格执行。
 * 框内树用地形扫描找 (TREE / TIBTRE 前缀, 见 TC_IsTreeID),
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

/* 配置 */
static int g_TcInit = 0;
static int g_TcEnable = 1;
static OU_Hotkey g_TcHk;
static int g_TcRange = 4;

static void TC_EnsureInit(void)
{
    char buf[96], b1[16];
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
    q = AppendCStr(buf, q, "treechop: ON (ctrl+Z+middle) range=", sizeof(buf));
    UtoA((unsigned)g_TcRange, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    (void)q;
    LogLine(buf);
    if (!g_TcEnable) LogLine("treechop: OFF");
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

/* 树查找 */

static int g_TcTypeOff = -1;
static int g_TcIdOff = -1;

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

/* ID 字符串有效即拷出并记住偏移。 */
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

/* 指针是否在 TerrainType 数组内。 */
static int TC_InTerrainSet(void* p) { return OU_InVec(ADDR_TERRAINTYPES, p); }

/* 树对象取 Type 指针 (命中直取, 否则扫描)。 */
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

/* TREE* / TIBTRE* 为树, 其余排除。 */
static int TC_IsTreeID(const char* id)
{
    if (!id || !id[0]) return 0;
    if (id[0] == 'T' && id[1] == 'R' && id[2] == 'E' && id[3] == 'E')
        return 1;
    if (id[0] == 'T' && id[1] == 'I' && id[2] == 'B' && id[3] == 'T')
        return 1;
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

/* 触发 */

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
    if (nt <= 0)
    {
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

/* 3px 白线 / 实心方块 (公共实现: OrderUtil 的 OU_Line / OU_Box)。 */
static void TC_Line(int x0, int y0, int x1, int y1, unsigned a)
{
    OU_Line(s_TcLocked, s_TcW, s_TcH, s_TcPitch, 0,
            x0, y0, x1, y1, 255, 255, 255, a, 3);
}

static void TC_Mark(int cx, int cy, int m, unsigned a)
{
    OU_Box(s_TcLocked, s_TcW, s_TcH, s_TcPitch, 0, cx, cy, m, 255, 255, 255, a);
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
