/* AirSpread.cpp - 中键智能下令 (点按自适应/划线散开, 原散开升级)。
 *
 * 决策 (只读, 联机安全): 终点有对象时调 DecideAction 0x692610
 * (与左键下单前同一函数, 全范围无事件构造/OutList 引用),
 * 逐单位用 MouseOverObject (vtbl+0x74) 做能力过滤;
 * 不调 ObjectClickedAction/ClickedMission (写本地状态)。
 * 终点是空地时不决策, 直接散开。
 * 映射 (0x4D74E0 跳转表): 攻击→Mission1, 进载具→Enter7,
 * 进驻/占领→Capture8 (步兵进民房原生即此); 未知动作无操作。
 * 护驻军: 建筑可进驻但全局判攻击时, 进驻单位照进驻、攻击改移动。
 * 落点转换: 散开落点压到建筑上时按直接下令 (进驻/攻击), 空军除外;
 * 护航不转回攻击; 移动散开跳过建筑中心格。
 * 散开: 点按螺旋配额 (步兵 3/格, 飞机/车辆 1/格);
 * 划线按轨迹弧长重采样, 空军/地面分组, 弧形怎么画就怎么排。
 * 下令: 标准 0x04 MegaMission (IsPlanningEvent=0, 与手点同形),
 * 目标显式, OutList 满 drip 跨帧续发; 发前验活, house 变化清空。
 * 落点无效则半径 5 环搜, 找不到丢弃该单位。
 * 分类: Aircraft 恒飞; 步兵/车辆 MZ==Fly(9), ST==Winged(4) 兜底。
 * 左键下令原样不动。
 *
 * 联机: 只发标准事件, 各端同一份原版代码执行。本地只读 + 标准事件入队,
 * 不写模拟内存。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MsgFont 同一 DLL)。
 * 轮询挂 PowerDrawBoth, 橡皮线挂 MsgFont_MessageDrawHook (见 MsgFont.cpp)。
 * 日志: 仅启动行；诊断临时加行，用完即删。
 */

#include <windows.h>
#include <math.h>
#include "GdiText.h"
#include "AirSpread.h"
#include "OrderUtil.h"
#include "SmartArea.h"

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
static OU_Hotkey g_AsHk;      /* Hotkey=Middle (裸中键, Z/Ctrl/组合中键按住时让路) */
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
    OU_ParseHotkey("Middle", &g_AsHk);            /* 默认 (必成功) */
    OU_ReadHotkey(AS_SECTION, "Hotkey", &g_AsHk); /* 有则覆盖, 坏串留默认 */
    if (g_AsHk.disabled) g_AsEnable = 0;
    g_AsNodeStep = GetPrivateProfileIntA(AS_SECTION, "NodeStep", 2, g_IniPath);
    if (g_AsNodeStep < 1) g_AsNodeStep = 1;
    if (g_AsNodeStep > 8) g_AsNodeStep = 8;
    LogLine("airspread: ON (middle smart)");
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

typedef struct { void* unit; int id; int w; int fly; } ASUnit;

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

static void AS_Press(void)
{
    void** items;
    int selN, i, nu = 0;
    int ax, ay, mx, my;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return;
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
    if (nu <= 0) return;
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
        int mz, st;
        void* tp = 0;
        u->fly = 0;
        if (u->w == ABS_AIRCRAFT) u->fly = 1;
        else
        {
            tp = AS_TypeOf(u->unit, u->w);
            if (tp)
            {
                mz = *(int*)((char*)tp + AS_OFF_MZ);
                st = *(int*)((char*)tp + AS_OFF_ST);
                if (mz == AS_MZ_FLY || st == AS_ST_WINGED) u->fly = 1;
            }
        }
    }
    if (!OU_MouseCellPx(&ax, &ay, &mx, &my)) return;
    g_AsArmed = 1;
    g_AsAx = ax; g_AsAy = ay;
    g_AsPx = mx; g_AsPy = my;
    g_AsCount = nu;
    g_AsNodeCount = 0;
    AS_NodePush(ax, ay, mx, my); /* 首节点=锚点 (格+实测像素) */
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

/* ---------------- 智能对象令 (攻击/进入/进驻/占领, 与原生点击同形) ---
 * 攻击: Mission=Attack, Target=敌对象, Dst=空 (FuzzyAttack 同形)。
 * 进入/进驻/占领: Mission=Enter7/Capture8/Eaten9/Sabotage17,
 * Target=空, Destination=目标对象 (AutoLoad §11 真实点击布局)。
 * 全部 IsPlanningEvent=0 直接下令。 */

typedef struct { void* unit; int uid; void* tgt; int tid; int mission; } ASObj;

static ASObj g_AsObj[AS_PEND_CAP];
static int g_AsObjCount = 0;

static int AS_FireAttack(int houseIdx, int srcID, unsigned char srcRtti,
                         int tgtID, unsigned char tgtRtti)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_ATTACK,
            tgtID, (unsigned)tgtRtti, 0, 0, 0, 0);
    ev[OFF_PLANFLAG] = 0;
    return OU_Enqueue(ev);
}

static int AS_FireDest(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstID, unsigned char dstRtti, int mission)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, mission,
            0, 0, dstID, (unsigned)dstRtti, 0, 0);
    ev[OFF_PLANFLAG] = 0;
    return OU_Enqueue(ev);
}

/* Action(光标) → Mission(订单): gamemd 0x4D74E0 跳转表实证
 * (Enter3→7 载具, Capture9→8, Sabotage16→17;
 * 建筑上的 Enter/GuardArea 一律按进驻 17, 与原生驻军同形)。
 * 未知/不可转发 (Select/Repair/Harvest/超武等) 回 -1 (调用方按无操作处理,
 * 绝不猜着下单); Move 回 MISSION_MOVE。 */
static int AS_MissionFor(int targetWhat, int action)
{
    if (action == ACTION_ATTACK || action == 62 || action == 63 ||
        action == 64 || action == 71 || action == 53)
        return MISSION_ATTACK;
    if (action == ACTION_ENTER)
        return (targetWhat == ABS_BUILDING) ? MISSION_SABOTAGE : MISSION_ENTER;
    if (action == ACTION_SABOTAGE)
        return MISSION_SABOTAGE;
    if (action == ACTION_GUARDAREA && targetWhat == ABS_BUILDING)
        return MISSION_SABOTAGE;
    if (action == ACTION_CAPTURE)
        return MISSION_CAPTURE;
    if (action == ACTION_MOVE)
        return MISSION_MOVE;
    return -1;
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

static void AS_ObjShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_AsObjCount; k++)
    {
        g_AsObj[k].unit = g_AsObj[k + 1].unit;
        g_AsObj[k].uid = g_AsObj[k + 1].uid;
        g_AsObj[k].tgt = g_AsObj[k + 1].tgt;
        g_AsObj[k].tid = g_AsObj[k + 1].tid;
        g_AsObj[k].mission = g_AsObj[k + 1].mission;
    }
    g_AsObjCount--;
}

/* 排空: 每事件先验活再发; house 变化则清空 (读档/换边防串令)。
 * 先排对象令 (攻击/进入/进驻), 再排移动令。 */
static void AS_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx, sent = 0;
    if (!cur || cur != g_AsHouse) { g_AsPendCount = 0; g_AsObjCount = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (houseIdx != g_AsHouseIdx) { g_AsPendCount = 0; g_AsObjCount = 0; return; }
    while (g_AsObjCount > 0 && sent < AS_DRAIN_PER_POLL)
    {
        ASObj* e = &g_AsObj[0];
        char srcB[8], tgtB[8];
        unsigned i;
        int srcID, tgtID;
        unsigned char srcR, tgtR;
        for (i = 0; i < 8; i++) srcB[i] = tgtB[i] = 0;
        if (!OU_PtrAlive(e->unit, e->uid, g_AsIdxOk)) { AS_ObjShiftDown(); continue; }
        if (OU_WhatAmI(e->unit) != ABS_UNIT &&
            OU_WhatAmI(e->unit) != ABS_AIRCRAFT &&
            OU_WhatAmI(e->unit) != ABS_INFANTRY) { AS_ObjShiftDown(); continue; }
        if (!OU_PtrAlive(e->tgt, e->tid, g_AsIdxOk)) { AS_ObjShiftDown(); continue; }
        if (!FN_TGT_OBJ(tgtB, e->tgt)) { AS_ObjShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { AS_ObjShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        tgtID = *(int*)(tgtB + 0); tgtR = (unsigned char)tgtB[4];
        if (e->mission == MISSION_ATTACK)
        {
            if (!AS_FireAttack(houseIdx, srcID, srcR, tgtID, tgtR))
                return;
        }
        else
        {
            if (!AS_FireDest(houseIdx, srcID, srcR, tgtID, tgtR, e->mission))
                return;
        }
        AS_ObjShiftDown();
        sent++;
    }
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

/* 落点格上的建筑 (只读 CellClass::FindObjectOfType 0x47C4D0,
 * FuzzyAttack 同入口; 取链头即够, 建筑一格通常唯一)。 */
typedef void* __attribute__((thiscall)) (*Fn_FindObjType)(void* cell, int abs, int alt);
#define FN_FINDOBJTYPE ((Fn_FindObjType)(void*)0x47C4D0u)

static void* AS_CellBuilding(int cx, int cy)
{
    void** cells;
    int maxc, idx;
    void* cell;
    void* head;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return 0;
    cells = *(void***)(ADDR_DISPLAY + OFF_MAP_CELLS);
    maxc = *(int*)(ADDR_DISPLAY + OFF_MAP_MAXC);
    if (!cells || !IsPlausiblePtr(cells) || maxc <= 0 || maxc > 512 * 512) return 0;
    idx = (cy << 9) + cx;
    if (idx < 0 || idx >= maxc) return 0;
    cell = cells[idx];
    if (!cell || !IsPlausiblePtr(cell)) return 0;
    head = FN_FINDOBJTYPE(cell, ABS_BUILDING, 0);
    if (!head || !IsPlausiblePtr(head)) return 0;
    if (OU_WhatAmI(head) != ABS_BUILDING) return 0;
    return head;
}

/* 护航目标 (SmartPoint 移动子集散开时=原目标, 其余散开=NULL):
 * 落点与之是同一建筑且判定为攻击时保持移动 (不把护驻军打回来)。 */
static void* g_AsProtTgt = 0;

static void AS_Queue(int ui, int cx, int cy)
{
    int ox, oy;
    ASUnit* u;
    void* slotObj;
    if (ui < 0 || ui >= g_AsCount) return;
    u = &g_AsUnits[ui];
    if (!AS_NearValid(cx, cy, &ox, &oy)) return;
    /* 落点转换: 地面单位落点格上有建筑时, 按直接下令处理
     * (进驻/攻击/占领), 而不是走过去再转攻击。
     * 空军不管地面建筑; 护航目标同建筑的攻击保持移动。 */
    if (u->fly == 0 && g_AsHouse)
    {
        slotObj = AS_CellBuilding(ox, oy);
        if (slotObj)
        {
            char tb[8];
            unsigned ti;
            int act, ms, tid;
            for (ti = 0; ti < 8; ti++) tb[ti] = 0;
            if (FN_TGT_OBJ(tb, slotObj))
            {
                tid = *(int*)(tb + 0);
                act = OU_MouseOverObject(u->unit, slotObj);
                ms = (act < 0) ? -1 : AS_MissionFor(ABS_BUILDING, act);
                if ((ms == MISSION_ATTACK || ms == MISSION_ENTER ||
                     ms == MISSION_SABOTAGE || ms == MISSION_CAPTURE) &&
                    !(slotObj == g_AsProtTgt && ms == MISSION_ATTACK) &&
                    g_AsObjCount < AS_PEND_CAP)
                {
                    g_AsObj[g_AsObjCount].unit = u->unit;
                    g_AsObj[g_AsObjCount].uid = u->id;
                    g_AsObj[g_AsObjCount].tgt = slotObj;
                    g_AsObj[g_AsObjCount].tid = tid;
                    g_AsObj[g_AsObjCount].mission = ms;
                    g_AsObjCount++;
                    return;
                }
            }
        }
    }
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

/* 智能下单: 终点有点中对象时调游戏自己的决策下单。
 * 全局决策调 DecideAction (与左键下单前同一函数, 只读不下单):
 * 同选中同目标时回的值与左键光标一致 (代表制: 混编按代表单位定)。
 * 执行走标准 MegaMission 事件 (与手点同形), 联机安全;
 * 不调 ObjectClickedAction/ClickedMission (它们写本地单位状态, 联机不安全)。
 * 护驻军: 建筑可进驻但全局判攻击时 (代表是坦克), 进驻单位照进驻、
 * 攻击改移动 (不摧毁建筑; 纯坦克则照常攻击, 与左键一致)。
 * 返回 0=全移动请调用方散开, 1=对象令已下, 2=无操作 (禁散开,
 * 终点有对象时绝不往对象格散开, 防走到脸上再自动开火)。 */
static int AS_SmartPoint(int bx, int by, void* tgt)
{
    int i, tgtWhat, tgtTid = 0;
    static int missions[AS_CAP];
    int nAtk = 0, nEnt = 0, nGar = 0, nCap = 0, nMov = 0;
    int gAct = -1, gMiss = -1;
    int fbMode = 0; /* 全局不可用时的逐单位回落 */
    char tb[8];
    unsigned ti;
    if (!tgt || !IsPlausiblePtr(tgt)) return 0;
    tgtWhat = OU_WhatAmI(tgt);
    if (tgtWhat != ABS_UNIT && tgtWhat != ABS_INFANTRY &&
        tgtWhat != ABS_AIRCRAFT && tgtWhat != ABS_BUILDING)
        return 0;
    for (ti = 0; ti < 8; ti++) tb[ti] = 0;
    if (!FN_TGT_OBJ(tb, tgt)) return 0;
    tgtTid = *(int*)(tb + 0);
    /* 全局决策: 游戏自己定 (只读 DecideAction)。 */
    gAct = OU_DecideAction(bx, by, tgt);
    if (gAct < 0) return 0;
    gMiss = AS_MissionFor(tgtWhat, gAct);
    if (gMiss == MISSION_MOVE) return 0; /* 移动: 调用方散开 */
    if (gMiss < 0)
    {
        /* 全局 NoMove/不可转发: 退回逐单位游戏判定 (与悬停光标同源)。
         * 有单位能攻击/进入/进驻/占领就下单 (防全局误判吞令);
         * 否则无操作 (绝不往对象上散开)。 */
        fbMode = 1;
        for (i = 0; i < g_AsCount; i++)
        {
            ASUnit* u = &g_AsUnits[i];
            int act, ms;
            missions[i] = -1;
            if (u->fly != 1 && u->fly != 0) continue;
            if (u->w != ABS_UNIT && u->w != ABS_AIRCRAFT && u->w != ABS_INFANTRY)
                continue;
            if (!OU_PtrAlive(u->unit, u->id, g_AsIdxOk)) continue;
            act = OU_MouseOverObject(u->unit, tgt);
            if (act < 0) ms = -1;
            else ms = AS_MissionFor(tgtWhat, act);
            if (ms == MISSION_ATTACK || ms == MISSION_ENTER ||
                ms == MISSION_SABOTAGE || ms == MISSION_CAPTURE)
                missions[i] = ms;
            else if (ms == MISSION_MOVE) missions[i] = MISSION_MOVE;
            else missions[i] = -1;
        }
    }
    else
    {
    /* 逐单位能力过滤 (只读 MouseOver): 与全局同任务才下单,
     * 本会移动的跟随移动, 其它分歧按无操作 (与原生 fail 分支同效果)。 */
    for (i = 0; i < g_AsCount; i++)
    {
        ASUnit* u = &g_AsUnits[i];
        int act, ms;
        missions[i] = -1;
        if (u->fly != 1 && u->fly != 0) continue;
        if (u->w != ABS_UNIT && u->w != ABS_AIRCRAFT && u->w != ABS_INFANTRY)
            continue;
        if (!OU_PtrAlive(u->unit, u->id, g_AsIdxOk)) continue;
        act = OU_MouseOverObject(u->unit, tgt);
        if (act < 0) ms = -1;
        else ms = AS_MissionFor(tgtWhat, act);
        if (ms == gMiss) missions[i] = ms;
        else if (ms == MISSION_MOVE) missions[i] = MISSION_MOVE;
        else missions[i] = -1;
    }
    }
    /* 护驻军 (两路通用): 建筑有单位能进驻/占领/进入时, 攻击改移动。 */
    if (tgtWhat == ABS_BUILDING)
    {
        int hasGar = 0;
        for (i = 0; i < g_AsCount; i++)
        {
            if (missions[i] == MISSION_SABOTAGE || missions[i] == MISSION_CAPTURE ||
                missions[i] == MISSION_ENTER) { hasGar = 1; break; }
        }
        if (hasGar)
        {
            if (gMiss == MISSION_ATTACK) gMiss = MISSION_SABOTAGE;
            for (i = 0; i < g_AsCount; i++)
            {
                if (missions[i] == MISSION_ATTACK)
                    missions[i] = MISSION_MOVE;
            }
        }
    }
    for (i = 0; i < g_AsCount; i++)
    {
        if (g_AsUnits[i].fly != 1 && g_AsUnits[i].fly != 0) continue;
        if (missions[i] == MISSION_ATTACK) nAtk++;
        else if (missions[i] == MISSION_ENTER) nEnt++;
        else if (missions[i] == MISSION_SABOTAGE) nGar++;
        else if (missions[i] == MISSION_CAPTURE) nCap++;
        else if (missions[i] == MISSION_MOVE) nMov++;
    }
    if (!fbMode && nAtk + nEnt + nGar + nCap <= 0 && nMov > 0)
        return 0; /* 全局即移动: 回落原散开 (保持配额/螺旋逻辑一致) */
    if (nAtk + nEnt + nGar + nCap <= 0)
        return 2; /* 无对象令: 无操作, 禁往对象散开 */
    /* 对象令入队 (攻击 Target=敌, 进入类 Destination=目标)。 */
    for (i = 0; i < g_AsCount; i++)
    {
        int ms = missions[i];
        if (ms == MISSION_MOVE || ms < 0) continue;
        if (g_AsUnits[i].fly != 1 && g_AsUnits[i].fly != 0) continue;
        if (g_AsObjCount >= AS_PEND_CAP) break;
        g_AsObj[g_AsObjCount].unit = g_AsUnits[i].unit;
        g_AsObj[g_AsObjCount].uid = g_AsUnits[i].id;
        g_AsObj[g_AsObjCount].tgt = tgt;
        g_AsObj[g_AsObjCount].tid = tgtTid;
        g_AsObj[g_AsObjCount].mission = ms;
        g_AsObjCount++;
    }
    /* 移动子集绕目标散开 (配额与原点按一致: 步兵3/格, 飞机/车辆1/格)。
     * 建筑目标跳过中心格 (本格极可能是建筑本体, 不往上派移动令,
     * 防引擎转移动为攻击); 环从 r=1 起。
     * 子集落点转换时护航目标不转回攻击 (见 g_AsProtTgt)。 */
    if (nMov > 0)
    {
        g_AsProtTgt = tgt;
        static int qcx[512], qcy[512];
        static int quA[512], quI[512], quV[512];
        int qn = 0, r, k;
        if (tgtWhat == ABS_BUILDING) qn = 0;
        else
        {
            qcx[0] = bx; qcy[0] = by;
            quA[0] = quI[0] = quV[0] = 0;
            qn = OU_CellValid(bx, by) ? 1 : 0;
        }
        for (r = 1; r <= 40 && qn < 512; r++)
        {
            int dx, dy;
            for (dx = -r; dx <= r && qn < 512; dx++)
                if (OU_CellValid(bx + dx, by - r))
                { qcx[qn] = bx + dx; qcy[qn] = by - r; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
            for (dy = -r + 1; dy <= r && qn < 512; dy++)
                if (OU_CellValid(bx + r, by + dy))
                { qcx[qn] = bx + r; qcy[qn] = by + dy; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
            for (dx = r - 1; dx >= -r && qn < 512; dx--)
                if (OU_CellValid(bx + dx, by + r))
                { qcx[qn] = bx + dx; qcy[qn] = by + r; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
            for (dy = r - 1; dy >= -r + 1 && qn < 512; dy--)
                if (OU_CellValid(bx - r, by + dy))
                { qcx[qn] = bx - r; qcy[qn] = by + dy; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
        }
        for (k = 0; k < g_AsCount; k++)
        {
            ASUnit* u = &g_AsUnits[k];
            int cap, c;
            if (missions[k] != MISSION_MOVE) continue;
            if (u->fly != 1 && u->fly != 0) continue;
            if (u->w != ABS_UNIT && u->w != ABS_AIRCRAFT && u->w != ABS_INFANTRY) continue;
            for (c = 0; c < qn; c++)
            {
                if (u->w == ABS_INFANTRY) { cap = 3; if (quI[c] >= cap) continue; quI[c]++; }
                else if (u->w == ABS_AIRCRAFT) { cap = 1; if (quA[c] >= cap) continue; quA[c]++; }
                else { cap = 1; if (quV[c] >= cap) continue; quV[c]++; }
                AS_Queue(k, qcx[c], qcy[c]);
                break;
            }
            (void)cap;
        }
    }
    AS_Drain();
    return 1;
}

static void AS_Release(void)
{
    int bx, by, mx, my;
    int i;
    static int airList[AS_CAP], gndList[AS_CAP];
    int airN = 0, gndN = 0;
    void* tgt = 0;
    g_AsArmed = 0;
    if (OU_MouseCellTarget(&bx, &by, &tgt, &mx, &my))
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
    if (airN + gndN <= 0) return;
    {
        void* cur = *(void**)ADDR_CURPLAYER;
        if (!cur || !IsPlausiblePtr(cur)) return;
        g_AsHouse = cur;
        g_AsHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    }
    g_AsPendCount = 0;
    g_AsObjCount = 0;
    g_AsProtTgt = 0;
    if (tgt)
    {
        /* 终点点中对象: 先走智能自适应 (攻击/进入/进驻/占领)。
         * r==2 无操作: 直接返回。r==1 已下单: 直接返回
         * (SmartPoint 内部已排空, 绝不能以队列余量判断,
         * 否则已发的单会被弧形移动覆盖)。r==0 全移动:
         * 点按走点散开, 划线走弧形 (保阵型)。 */
        int r = AS_SmartPoint(bx, by, tgt);
        if (r) return;
    }
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
            (void)cap;
        }
        AS_Drain();
        return;
    }
    /* 划线: 空军/地面分组, 各按轨迹弧长重采样为本组槽位数 (弧形怎么画就怎么排)。 */
    AS_ArcGroup(airList, airN);
    AS_ArcGroup(gndList, gndN);
    AS_Drain();
}

void AirSpread_Poll(void)
{
    int trigDown;
    AS_EnsureInit();
    trigDown = OU_KeyDown(g_AsHk.vk);
    if (!g_AsEnable) { g_AsArmed = 0; g_AsPrevTrig = trigDown; return; }
    if (g_AsPendCount > 0 || g_AsObjCount > 0) { AS_Drain(); g_AsPrevTrig = trigDown; return; }
    if (trigDown && g_AsArmed)
    {
        /* 按住途中加了 Hotkey 附加键/修饰键、Z/Ctrl 或组合中键即改主意了:
         * 散开取消待命 (松开不执行), 执行权交巡逻/砍树/模糊/区域自适应
         * (它们按松开瞬间组合键判定; 区域自适应关则不认领, 照旧散开)。 */
        if (!OU_ModsDown(g_AsHk.mods) || !OU_ExtraDown(&g_AsHk) ||
            OU_KeyDown('Z') || OU_KeyDown(VK_CONTROL) ||
            SmartArea_ClaimsMiddle())
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
         * 组合中键按住时归 SmartArea, 这里只认 Hotkey 裸键 (互斥见上面)。 */
        if (OU_ModsDown(g_AsHk.mods) && OU_ExtraDown(&g_AsHk) &&
            !OU_KeyDown('Z') && !OU_KeyDown(VK_CONTROL) &&
            !SmartArea_ClaimsMiddle())
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
