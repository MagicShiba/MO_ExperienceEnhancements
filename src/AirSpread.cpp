/* AirSpread.cpp - 中键散开 v3。
 *
 * 点按 (无拖线): 像左键点移动一样到达目的地, 只是按配额分开:
 * 步兵 3/格, 飞机/车辆 1/格 (以目标格为中心螺旋向外找格)。
 * 划线: 拖动轨迹采样为稀疏节点 (NodeStep 格一步, 上限 64),
 * 空军组/地面组各按弧长重采样为本组槽位数, 每单位一道标准移动令。
 * 弧形怎么画, 单位就怎么排。左键下令原样不动。
 * 下令: 标准 0x04 MegaMission 普通移动 (IsPlanningEvent=0, 与手点右键同形),
 * 每单位目标格显式, OutList 满则 drip 跨帧续发; 发前验活, house 变化则清空。
 * 落点: 插值格无效则半径 5 环搜有效格, 找不到则丢弃该单位 (记 dst -1)。
 * 分类: Aircraft 恒飞; 步兵/车辆 MZ==Fly(9), ST==Winged(4) 兜底。
 *
 * 联机: 只发标准事件, 各端同一份原版代码执行。本地只读 + 标准事件入队,
 * 不写模拟内存。下令端本地算、广播显式结果 (RACommands 同路线)。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MsgFont 同一 DLL)。
 * 轮询挂 PowerDrawBoth, 橡皮线挂 MsgFont_MessageDrawHook (见 MsgFont.cpp)。
 */

#include <windows.h>
#include <math.h>
#include "GdiText.h"
#include "AirSpread.h"
#include "OrderUtil.h"

#define AS_SECTION "AirSpread"

/* ---------------- 散开私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define ADDR_TYPARRAY 0xA8EB00u
#define OFF_VEC_ITEMS 4u
#define OFF_VEC_COUNT 16u
#define AS_CAP 256
#define AS_PEND_CAP 512
#define AS_DRAIN_PER_POLL 128
#define FN_TGT_CELL ((Fn_TargetFromCell)(void*)0x6E6B20u)

typedef void* __attribute__((thiscall)) (*Fn_TargetFromCell)(void* tgt, const CellStruct* cell);
typedef void* (__fastcall *AS_LockFn)(void*, void*, int, int);
typedef int (__fastcall *AS_UnlockFn)(void*, void*);

#define VT_AS_LOCK   0x5C
#define VT_AS_UNLOCK 0x60

/* ---------------- 配置 ---------------- */

static int g_AsInit = 0;
static int g_AsEnable = 1;
static int g_AsTriggerVK = 4;   /* 4=中键 */
static int g_AsNodeStep = 2;    /* 拖线采样步长 (格, 切比雪夫距离) */

/* 分类判据 (参考 RACommands, MO rulesmo 实测 + 内存 dump 双确认):
 * Aircraft 恒飞; 步兵/车辆 MZ==Fly(9), ST==Winged(4) 兜底。
 * MO 的飞行步兵/悬停直升机 MZ=Fly、ST=Hover(3), 故判据以 MZ==9 为主。 */
#define AS_OFF_MZ 1460   /* dump 实证 (JUMPJET=9=Fly, MAMM=2=Destroyer) */
#define AS_OFF_ST 1596   /* 未定死: 当前读垃圾, 命中概率极低, 仅作无害兜底 */
#define AS_MZ_FLY 9
#define AS_ST_WINGED 4

static void AS_EnsureInit(void)
{
    if (g_AsInit) return;
    g_AsInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_AsEnable = GetPrivateProfileIntA(AS_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    g_AsTriggerVK = GetPrivateProfileIntA(AS_SECTION, "TriggerVK", 4, g_IniPath);
    g_AsNodeStep = GetPrivateProfileIntA(AS_SECTION, "NodeStep", 2, g_IniPath);
    if (g_AsNodeStep < 1) g_AsNodeStep = 1;
    if (g_AsNodeStep > 8) g_AsNodeStep = 8;
    if (g_AsTriggerVK < 1 || g_AsTriggerVK > 255) g_AsTriggerVK = 4;
    LogLine("airspread: ON (middle spread)");
    if (!g_AsEnable) LogLine("airspread: OFF");
}

/* ---------------- 小工具 (通用部分见 OrderUtil.h) ---------------- */

/* ---------------- 类型自校准 ---------------- */

static int g_AsTypeOff[3] = { -1, -1, -1 }; /* 按类别 pin: 0=Unit 1=Aircraft 2=Infantry */
static int g_AsIdOff = -1;                  /* 类型内 ID 字符串偏移 pin */

static int AS_ClassSlot(int w)
{
    if (w == ABS_UNIT) return 0;
    if (w == ABS_AIRCRAFT) return 1;
    if (w == ABS_INFANTRY) return 2;
    return -1;
}

static int AS_StrOk(const char* s)
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

/* 类型指针 p 的 ID 是否可读; 可读则拷入 out (24 字节) 并 pin 偏移。 */
static int AS_TypeID(void* p, char* out)
{
    static const unsigned cands[3] = { 0x20u, 0x24u, 0x28u };
    int i;
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (g_AsIdOff >= 0)
    {
        const char* s = (const char*)p + (unsigned)g_AsIdOff;
        int k;
        if (!AS_StrOk(s)) { g_AsIdOff = -1; }
        else
        {
            for (k = 0; k < 24; k++) out[k] = s[k];
            return 1;
        }
    }
    for (i = 0; i < 3; i++)
    {
        const char* s = (const char*)p + cands[i];
        int k;
        if (!AS_StrOk(s)) continue;
        g_AsIdOff = (int)cands[i];
        for (k = 0; k < 24; k++) out[k] = s[k];
        return 1;
    }
    return 0;
}

/* 类型数组元素数 (守卫后返回, 否则 0)。 */
static int AS_TypeCount(void*** itemsOut)
{
    void** items;
    int cnt;
    *itemsOut = 0;
    cnt = *(int*)(ADDR_TYPARRAY + OFF_VEC_COUNT);
    items = *(void***)(ADDR_TYPARRAY + OFF_VEC_ITEMS);
    if (cnt <= 0 || cnt > 20000) return 0;
    if (!items || !IsPlausiblePtr(items)) return 0;
    *itemsOut = items;
    return cnt;
}

static int AS_InTypeSet(void* p)
{
    void** items = 0;
    int cnt, i;
    cnt = AS_TypeCount(&items);
    if (!cnt) return 0;
    for (i = 0; i < cnt; i++)
        if (items[i] == p) return 1;
    return 0;
}

/* 单位取 Type 指针 (pin 命中则快路, 否则全扫; 跨单位一致即自证)。 */
static void* AS_TypeOf(void* u, int w)
{
    int slot = AS_ClassSlot(w);
    unsigned off;
    char idb[24];
    int k;
    if (slot < 0) return 0;
    if (g_AsTypeOff[slot] >= 0)
    {
        void* p = *(void**)((char*)u + (unsigned)g_AsTypeOff[slot]);
        if (p && IsPlausiblePtr(p) && AS_InTypeSet(p) && AS_TypeID(p, idb))
            return p;
        g_AsTypeOff[slot] = -1;
    }
    for (off = 0x40u; off < 0x900u; off += 4u)
    {
        void* p = *(void**)((char*)u + off);
        if (!p || !IsPlausiblePtr(p)) continue;
        if (!AS_InTypeSet(p)) continue;
        for (k = 0; k < 24; k++) idb[k] = 0;
        if (!AS_TypeID(p, idb)) continue;
        g_AsTypeOff[slot] = (int)off;
        return p;
    }
    return 0;
}

/* ---------------- 快照与触发 ---------------- */

typedef struct { void* unit; int id; int w; int fly; char type[24]; } ASUnit;

static ASUnit g_AsUnits[AS_CAP];
static int g_AsCount = 0;
static int g_AsIdxOk = 0;
static int g_AsArmed = 0;
static int g_AsAx = 0, g_AsAy = 0;
static int g_AsPx = 0, g_AsPy = 0;
static int g_AsPrevTrig = 0;

/* 拖线采样节点 (弧形槽位源, 上限 64, 稀疏记录)。
 * 格用于落点计算; 屏坐标用于画线 (采样时刻实测像素, 不估算——
 * 等距视角下格差到像素没有各轴独立系数, 估算必错)。 */
#define AS_NODE_CAP 64
static int g_AsNx[AS_NODE_CAP], g_AsNy[AS_NODE_CAP];
static int g_AsNPx[AS_NODE_CAP], g_AsNPy[AS_NODE_CAP];
static int g_AsNodeCount = 0;

static void AS_NodePush(int cx, int cy, int px, int py)
{
    int lx, ly, dx, dy, adx, ady;
    if (g_AsNodeCount <= 0)
    {
        g_AsNx[0] = cx; g_AsNy[0] = cy;
        g_AsNPx[0] = px; g_AsNPy[0] = py;
        g_AsNodeCount = 1;
        return;
    }
    if (g_AsNodeCount >= AS_NODE_CAP) return;
    lx = g_AsNx[g_AsNodeCount - 1]; ly = g_AsNy[g_AsNodeCount - 1];
    dx = cx - lx; dy = cy - ly;
    adx = dx >= 0 ? dx : -dx; ady = dy >= 0 ? dy : -dy;
    if (adx < g_AsNodeStep && ady < g_AsNodeStep) return; /* 太密不记 */
    g_AsNx[g_AsNodeCount] = cx; g_AsNy[g_AsNodeCount] = cy;
    g_AsNPx[g_AsNodeCount] = px; g_AsNPy[g_AsNodeCount] = py;
    g_AsNodeCount++;
}

static void AS_LogU(int i)
{
    char buf[128], b1[16], b2[16], b3[16];
    unsigned q = 0;
    int k;
    ASUnit* u = &g_AsUnits[i];
    q = AppendCStr(buf, q, "as: u w=", sizeof(buf));
    UtoA((unsigned)u->w, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " fid=", sizeof(buf));
    UtoA((unsigned)u->id, b2);
    q = AppendCStr(buf, q, b2, sizeof(buf));
    q = AppendCStr(buf, q, " type=\"", sizeof(buf));
    for (k = 0; k < 24 && u->type[k]; k++)
        q = AppendCh(buf, q, u->type[k], sizeof(buf));
    q = AppendCStr(buf, q, "\" fly=", sizeof(buf));
    UtoA((unsigned)u->fly, b3);
    q = AppendCStr(buf, q, b3, sizeof(buf));
    (void)q;
    LogLine(buf);
}

static void AS_Press(void)
{
    void** items;
    int selN, i, nu = 0, fly = 0;
    int ax, ay, mx, my;
    char b1[16], b2[16], b3[16], buf[128];
    unsigned q;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items))
    { LogLine("as: no selection"); return; }
    for (i = 0; i < selN && nu < AS_CAP; i++)
    {
        void* o = items[i];
        char tb[8];
        unsigned ti;
        int w = OU_WhatAmI(o);
        if (w != ABS_UNIT && w != ABS_AIRCRAFT && w != ABS_INFANTRY) continue;
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        g_AsUnits[nu].unit = o;
        g_AsUnits[nu].id = FN_TGT_OBJ(tb, o) ? *(int*)(tb + 0) : 0;
        g_AsUnits[nu].w = w;
        nu++;
    }
    if (nu <= 0) { LogLine("as: no foot"); return; }
    /* ID 索引自检 (PatrolCircle 同式, 失败则 fail-open)。 */
    g_AsIdxOk = 0;
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        if (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 20000 &&
            g_AsUnits[0].id > 0 &&
            OU_PtrAliveRaw(g_AsUnits[0].unit, g_AsUnits[0].id))
            g_AsIdxOk = 1;
    }
    /* 分类: Aircraft 恒飞; 步兵/车辆按运动类型 (MZ==Fly, ST==Winged 兜底)。 */
    for (i = 0; i < nu; i++)
    {
        ASUnit* u = &g_AsUnits[i];
        int k, mz, st;
        void* tp = 0;
        for (k = 0; k < 24; k++) u->type[k] = 0;
        u->fly = 0;
        if (u->w == ABS_AIRCRAFT) u->fly = 1;
        else
        {
            tp = AS_TypeOf(u->unit, u->w);
            if (tp)
            {
                AS_TypeID(tp, u->type);
                mz = *(int*)((char*)tp + AS_OFF_MZ);
                st = *(int*)((char*)tp + AS_OFF_ST);
                if (mz == AS_MZ_FLY || st == AS_ST_WINGED) u->fly = 1;
            }
        }
        if (u->fly) fly++;
        AS_LogU(i);
    }
    if (!OU_MouseCellPx(&ax, &ay, &mx, &my)) { LogLine("as: off map"); return; }
    g_AsArmed = 1;
    g_AsAx = ax; g_AsAy = ay;
    g_AsPx = mx; g_AsPy = my;
    g_AsCount = nu;
    g_AsNodeCount = 0;
    AS_NodePush(ax, ay, mx, my); /* 首节点=锚点 (格+实测像素) */
    q = 0;
    q = AppendCStr(buf, q, "as: anchor ", sizeof(buf));
    UtoA((unsigned)ax, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCh(buf, q, ',', sizeof(buf));
    UtoA((unsigned)ay, b2);
    q = AppendCStr(buf, q, b2, sizeof(buf));
    q = AppendCStr(buf, q, " n=", sizeof(buf));
    UtoA((unsigned)nu, b3);
    q = AppendCStr(buf, q, b3, sizeof(buf));
    q = AppendCStr(buf, q, " fly=", sizeof(buf));
    UtoA((unsigned)fly, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    (void)q;
    LogLine(buf);
}

static int AS_Rnd(double v)
{
    return (int)(v + (v >= 0 ? 0.5 : -0.5));
}

/* 落点校验: 首选格无效则方形环由近及远找有效格 (半径 5), 找不到则丢弃。 */
static int AS_NearValid(int x, int y, int* ox, int* oy)
{
    int r, dx, dy;
    if (OU_CellValid(x, y)) { *ox = x; *oy = y; return 1; }
    for (r = 1; r <= 5; r++)
    {
        for (dy = -r; dy <= r; dy++)
        {
            for (dx = -r; dx <= r; dx++)
            {
                int cx, cy;
                if (dx != r && dx != -r && dy != r && dy != -r) continue;
                cx = x + dx; cy = y + dy;
                if (OU_CellValid(cx, cy)) { *ox = cx; *oy = cy; return 1; }
            }
        }
    }
    return 0;
}

/* ---------------- 标准移动事件 (普通 MegaMission, flag=0) ---------------- */

typedef struct { void* unit; int id; short x, y; } ASPend;

static ASPend g_AsPend[AS_PEND_CAP];
static int g_AsPendCount = 0;
static void* g_AsHouse = 0;
static int g_AsHouseIdx = 0;

/* 普通移动令: Mission=Move, Target=空, Destination=格, IsPlanningEvent=0
 * (与手点右键移动同形, 非巡逻)。 */
static int AS_FireMove(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstID, unsigned char dstRtti)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_MOVE,
            0, 0, dstID, (unsigned)dstRtti, 0, 0);
    ev[OFF_PLANFLAG] = 0;
    return OU_Enqueue(ev);
}

static void AS_ShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_AsPendCount; k++)
    {
        g_AsPend[k].unit = g_AsPend[k + 1].unit;
        g_AsPend[k].x = g_AsPend[k + 1].x;
        g_AsPend[k].y = g_AsPend[k + 1].y;
        g_AsPend[k].id = g_AsPend[k + 1].id;
    }
    g_AsPendCount--;
}

/* 排空: 每事件先验活再发; house 变化则清空 (读档/换边防串令)。 */
static void AS_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx, sent = 0;
    if (!cur || cur != g_AsHouse) { g_AsPendCount = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (houseIdx != g_AsHouseIdx) { g_AsPendCount = 0; return; }
    while (g_AsPendCount > 0 && sent < AS_DRAIN_PER_POLL)
    {
        ASPend* e = &g_AsPend[0];
        char srcB[8], dstB[8];
        CellStruct cs;
        int srcID, dstID;
        unsigned char srcR, dstR;
        unsigned i;
        for (i = 0; i < 8; i++) srcB[i] = dstB[i] = 0;
        cs.X = e->x; cs.Y = e->y;
        FN_TGT_CELL(dstB, &cs);
        dstID = *(int*)(dstB + 0); dstR = (unsigned char)dstB[4];
        if (!OU_PtrAlive(e->unit, e->id, g_AsIdxOk)) { AS_ShiftDown(); continue; }
        if (OU_WhatAmI(e->unit) != ABS_UNIT &&
            OU_WhatAmI(e->unit) != ABS_AIRCRAFT &&
            OU_WhatAmI(e->unit) != ABS_INFANTRY) { AS_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { AS_ShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        if (!AS_FireMove(houseIdx, srcID, srcR, dstID, dstR))
            return; /* OutList 满: 下轮继续 */
        AS_ShiftDown();
        sent++;
    }
}

static void AS_LogDst(int i, int cx, int cy)
{
    char buf[96], b1[16], b2[16], b3[16];
    unsigned q = 0;
    q = AppendCStr(buf, q, "as: dst i=", sizeof(buf));
    UtoA((unsigned)i, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " cell=", sizeof(buf));
    UtoA((unsigned)(cx < 0 ? 0 : cx), b2);
    q = AppendCStr(buf, q, b2, sizeof(buf));
    q = AppendCh(buf, q, ',', sizeof(buf));
    UtoA((unsigned)(cy < 0 ? 0 : cy), b3);
    q = AppendCStr(buf, q, b3, sizeof(buf));
    (void)q;
    LogLine(buf);
}

static void AS_Queue(int ui, int cx, int cy)
{
    int ox, oy;
    ASUnit* u;
    if (ui < 0 || ui >= g_AsCount) return;
    u = &g_AsUnits[ui];
    if (!AS_NearValid(cx, cy, &ox, &oy))
    {
        AS_LogDst(ui, -1, -1);
        return;
    }
    AS_LogDst(ui, ox, oy);
    if (g_AsPendCount >= AS_PEND_CAP) return;
    g_AsPend[g_AsPendCount].unit = u->unit;
    g_AsPend[g_AsPendCount].id = u->id;
    g_AsPend[g_AsPendCount].x = (short)ox;
    g_AsPend[g_AsPendCount].y = (short)oy;
    g_AsPendCount++;
}

/* 一组按轨迹弧长重采样 (sqrt 是 gcc 内建, 无需 libm, PatrolCircle 同式)。 */
static void AS_ArcGroup(int* idx, int n)
{
    static double scum[AS_NODE_CAP];
    int nc = g_AsNodeCount;
    int j, k, seg;
    double L;
    if (n <= 0) return;
    if (nc > AS_NODE_CAP) nc = AS_NODE_CAP;
    if (nc < 2) { for (k = 0; k < n; k++) AS_Queue(idx[k], g_AsNx[0], g_AsNy[0]); return; }
    scum[0] = 0.0;
    for (j = 1; j < nc; j++)
    {
        double dx = (double)(g_AsNx[j] - g_AsNx[j - 1]);
        double dy = (double)(g_AsNy[j] - g_AsNy[j - 1]);
        scum[j] = scum[j - 1] + sqrt(dx * dx + dy * dy);
    }
    L = scum[nc - 1];
    if (L < 0.5) { for (k = 0; k < n; k++) AS_Queue(idx[k], g_AsNx[0], g_AsNy[0]); return; }
    seg = 0;
    for (k = 0; k < n; k++)
    {
        double s = ((double)k + 0.5) * L / (double)n;
        double fx, fy, t, segLen;
        while (seg + 1 < nc - 1 && scum[seg + 1] < s) seg++;
        segLen = scum[seg + 1] - scum[seg];
        t = (segLen > 0.0001) ? (s - scum[seg]) / segLen : 0.0;
        fx = (double)g_AsNx[seg] + (double)(g_AsNx[seg + 1] - g_AsNx[seg]) * t;
        fy = (double)g_AsNy[seg] + (double)(g_AsNy[seg + 1] - g_AsNy[seg]) * t;
        AS_Queue(idx[k], AS_Rnd(fx), AS_Rnd(fy));
    }
}

static void AS_Release(void)
{
    int bx, by, mx, my;
    int i;
    static int airList[AS_CAP], gndList[AS_CAP];
    int airN = 0, gndN = 0;
    char buf[128], b1[16], b2[16], b3[16];
    unsigned q;
    g_AsArmed = 0;
    if (OU_MouseCellPx(&bx, &by, &mx, &my))
    {
        /* 松开格收尾 (与末节点不同则补记, 尾段不丢)。 */
        if (g_AsNodeCount > 0 &&
            (bx != g_AsNx[g_AsNodeCount - 1] || by != g_AsNy[g_AsNodeCount - 1]) &&
            g_AsNodeCount < AS_NODE_CAP && OU_CellValid(bx, by))
        {
            g_AsNx[g_AsNodeCount] = bx; g_AsNy[g_AsNodeCount] = by;
            g_AsNPx[g_AsNodeCount] = mx; g_AsNPy[g_AsNodeCount] = my;
            g_AsNodeCount++;
        }
    }
    else if (g_AsNodeCount > 0) { bx = g_AsNx[g_AsNodeCount - 1]; by = g_AsNy[g_AsNodeCount - 1]; }
    else return;
    /* 存活过滤: 淘汰死单位 (fly=-1); 分组: fly==1 空军, fly==0 地面。 */
    for (i = 0; i < g_AsCount; i++)
    {
        ASUnit* u = &g_AsUnits[i];
        if (!OU_PtrAlive(u->unit, u->id, g_AsIdxOk)) { u->fly = -1; continue; }
        if (u->fly == 1) airList[airN++] = i;
        else if (u->fly == 0) gndList[gndN++] = i;
    }
    q = 0;
    q = AppendCStr(buf, q, "as: go nodes=", sizeof(buf));
    UtoA((unsigned)g_AsNodeCount, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " air=", sizeof(buf));
    UtoA((unsigned)airN, b2);
    q = AppendCStr(buf, q, b2, sizeof(buf));
    q = AppendCStr(buf, q, " gnd=", sizeof(buf));
    UtoA((unsigned)gndN, b3);
    q = AppendCStr(buf, q, b3, sizeof(buf));
    (void)q;
    LogLine(buf);
    if (airN + gndN <= 0) return;
    {
        void* cur = *(void**)ADDR_CURPLAYER;
        if (!cur || !IsPlausiblePtr(cur)) return;
        g_AsHouse = cur;
        g_AsHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    }
    g_AsPendCount = 0;
    if (g_AsNodeCount <= 1)
    {
        /* 点按: 像左键点移动一样到目的地, 按配额分开。
         * 步兵 3/格, 飞机 1/格, 车辆 1/格 (三类计数独立)。
         * 以松开格为中心螺旋向外 (只收有效格, 上限 512 格)。 */
        static int qcx[512], qcy[512];
        static int quA[512], quI[512], quV[512];
        int qn = 0, r;
        qcx[0] = bx; qcy[0] = by;
        quA[0] = quI[0] = quV[0] = 0;
        qn = OU_CellValid(bx, by) ? 1 : 0;
        for (r = 1; r <= 40 && qn < 512; r++)
        {
            int dx, dy;
            for (dx = -r; dx <= r && qn < 512; dx++)
            {
                if (OU_CellValid(bx + dx, by - r))
                { qcx[qn] = bx + dx; qcy[qn] = by - r; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
            }
            for (dy = -r + 1; dy <= r && qn < 512; dy++)
            {
                if (OU_CellValid(bx + r, by + dy))
                { qcx[qn] = bx + r; qcy[qn] = by + dy; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
            }
            for (dx = r - 1; dx >= -r && qn < 512; dx--)
            {
                if (OU_CellValid(bx + dx, by + r))
                { qcx[qn] = bx + dx; qcy[qn] = by + r; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
            }
            for (dy = r - 1; dy >= -r + 1 && qn < 512; dy--)
            {
                if (OU_CellValid(bx - r, by + dy))
                { qcx[qn] = bx - r; qcy[qn] = by + dy; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
            }
        }
        for (i = 0; i < g_AsCount; i++)
        {
            ASUnit* u = &g_AsUnits[i];
            int cap, c;
            if (u->fly != 1 && u->fly != 0) continue;
            if (u->w != ABS_UNIT && u->w != ABS_AIRCRAFT && u->w != ABS_INFANTRY) continue;
            for (c = 0; c < qn; c++)
            {
                if (u->w == ABS_INFANTRY) { cap = 3; if (quI[c] >= cap) continue; quI[c]++; }
                else if (u->w == ABS_AIRCRAFT) { cap = 1; if (quA[c] >= cap) continue; quA[c]++; }
                else { cap = 1; if (quV[c] >= cap) continue; quV[c]++; }
                AS_Queue(i, qcx[c], qcy[c]);
                break;
            }
            if (c >= qn) AS_LogDst(i, -1, -1); /* 格不够 (几乎不可能) */
            (void)cap;
        }
        {
            char fbuf[96], f1[16];
            unsigned fq = 0;
            fq = AppendCStr(fbuf, fq, "as: fire queued=", sizeof(fbuf));
            UtoA((unsigned)g_AsPendCount, f1);
            fq = AppendCStr(fbuf, fq, f1, sizeof(fbuf));
            (void)fq;
            LogLine(fbuf);
        }
        AS_Drain();
        {
            char fbuf[96], f1[16];
            unsigned fq = 0;
            fq = AppendCStr(fbuf, fq, "as: left pend=", sizeof(fbuf));
            UtoA((unsigned)g_AsPendCount, f1);
            fq = AppendCStr(fbuf, fq, f1, sizeof(fbuf));
            (void)fq;
            LogLine(fbuf);
        }
        return;
    }
    /* 划线: 空军/地面分组, 各按轨迹弧长重采样为本组槽位数 (弧形怎么画就怎么排)。 */
    AS_ArcGroup(airList, airN);
    AS_ArcGroup(gndList, gndN);
    {
        char fbuf[96], f1[16];
        unsigned fq = 0;
        fq = AppendCStr(fbuf, fq, "as: fire queued=", sizeof(fbuf));
        UtoA((unsigned)g_AsPendCount, f1);
        fq = AppendCStr(fbuf, fq, f1, sizeof(fbuf));
        (void)fq;
        LogLine(fbuf);
    }
    AS_Drain();
    {
        char fbuf[96], f1[16];
        unsigned fq = 0;
        fq = AppendCStr(fbuf, fq, "as: left pend=", sizeof(fbuf));
        UtoA((unsigned)g_AsPendCount, f1);
        fq = AppendCStr(fbuf, fq, f1, sizeof(fbuf));
        (void)fq;
        LogLine(fbuf);
    }
}

void AirSpread_Poll(void)
{
    int trigDown;
    AS_EnsureInit();
    trigDown = (GetAsyncKeyState(g_AsTriggerVK) & 0x8000) ? 1 : 0;
    if (!g_AsEnable) { g_AsArmed = 0; g_AsPrevTrig = trigDown; return; }
    if (g_AsPendCount > 0) { AS_Drain(); g_AsPrevTrig = trigDown; return; }
    if (trigDown && g_AsArmed)
    {
        /* 按住途中加了 Z/Ctrl 即改主意了: 散开取消待命 (松开不执行),
         * 执行权交给巡逻/砍树/模糊 (它们按松开瞬间组合键判定)。 */
        if ((GetAsyncKeyState(90) & 0x8000) ||
            (GetAsyncKeyState(0x11) & 0x8000))
            g_AsArmed = 0;
    }
    if (trigDown && g_AsArmed)
    {
        /* 按住拖动: 采样轨迹节点 (格+实测像素, 稀疏记录, 弧形槽位源)。 */
        int cx, cy, px, py;
        if (OU_MouseCellPx(&cx, &cy, &px, &py))
            AS_NodePush(cx, cy, px, py);
    }
    if (trigDown && !g_AsPrevTrig)
    {
        /* 按下: Z 按住时归 PatrolCircle, Ctrl 按住时归 FuzzyAttack,
         * 这里只认裸中键 (三方互斥)。 */
        if (!(GetAsyncKeyState(90) & 0x8000) &&
            !(GetAsyncKeyState(0x11) & 0x8000))
            AS_Press();
    }
    else if (!trigDown && g_AsPrevTrig && g_AsArmed)
        AS_Release();
    g_AsPrevTrig = trigDown;
}

/* ---------------- 渲染期橡皮线 (3px 高亮) ---------------- */

static unsigned char* s_AsLocked = 0;
static int s_AsW = 0, s_AsH = 0, s_AsPitch = 0;

static void AS_Plot(int x, int y, unsigned a)
{
    unsigned short* dstRow;
    if (x < 0 || y < 0 || x >= s_AsW || y >= s_AsH) return;
    dstRow = (unsigned short*)(s_AsLocked + y * s_AsPitch);
    dstRow[x] = Blend16(dstRow[x], 255, 255, 255, a);
}

/* 3px 线 (Bresenham + 上下各 1px, 零依赖)。 */
static void AS_Line(int x0, int y0, int x1, int y1, unsigned a)
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
        AS_Plot(x, y, a);
        AS_Plot(x, y - 1, a);
        AS_Plot(x, y + 1, a);
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

/* 实心方块标记 (半边 m)。 */
static void AS_Mark(int cx, int cy, int m, unsigned a)
{
    int k, j;
    for (k = -m; k <= m; k++)
        for (j = -m; j <= m; j++)
            AS_Plot(cx + k, cy + j, a);
}

void AirSpread_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* vt;
    AS_LockFn lockFn;
    AS_UnlockFn unlockFn;
    unsigned char* locked;
    int x0, y0, x1, y1;
    extern int g_Calibrated;
    if (!g_AsEnable || !g_AsArmed) return;
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    if (!GtMouseGameXY(&x1, &y1)) return;
    x0 = g_AsPx; y0 = g_AsPy;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    lockFn = (AS_LockFn)(*(void**)(vt + VT_AS_LOCK));
    unlockFn = (AS_UnlockFn)(*(void**)(vt + VT_AS_UNLOCK));
    if (!lockFn || !unlockFn) return;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return;
    s_AsLocked = locked; s_AsW = w; s_AsH = h; s_AsPitch = pitch;
    /* 节点折线 (全用采样时刻实测像素, 弧形保真) + 橡皮尾巴 (末节点→当前鼠标)。
     * 注: 中键拖动会平移镜头, 前后节点像素不在同一镜头帧, 大位移拖动线会
     * 整体漂, 这是引擎行为, 与 PatrolCircle 同 caveat。 */
    {
        int i, ex, ey;
        int nc = g_AsNodeCount;
        if (nc > AS_NODE_CAP) nc = AS_NODE_CAP;
        ex = x0; ey = y0;
        for (i = 0; i < nc; i++)
        {
            int nx = g_AsNPx[i], ny = g_AsNPy[i];
            if (i > 0) AS_Line(ex, ey, nx, ny, 230);
            AS_Mark(nx, ny, 1, 230);
            ex = nx; ey = ny;
        }
        AS_Line(ex, ey, x1, y1, 230);
        AS_Mark(x0, y0, 3, 255); /* 锚点 7x7 */
        AS_Mark(x1, y1, 2, 255); /* 当前 5x5 */
    }
    s_AsLocked = 0;
    unlockFn(surf, NULL);
}
