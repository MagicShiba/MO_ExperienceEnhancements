/* SmartOrder.cpp - 中键智能下令 (点按自适应/划线散开, 原散开升级)。
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
 * 预览: 按住时绿线 (单位中心→预估落点/目标中心, 含高度, 限 128 线,
 * 换格/节点变或 400ms 刷一次), 与松手分配同规则 (终判松手重算一次)。
 * 下令: 标准 0x04 MegaMission (IsPlanningEvent=0, 与手点同形),
 * 目标显式, OutList 满 drip 跨帧续发; 发前验活, house 变化清空。
 * 落点无效则半径 5 环搜, 找不到丢弃该单位。
 * 分类: Aircraft 恒飞; 步兵/车辆 MZ==Fly(9), ST==Winged(4) 兜底。
 * 左键下令原样不动。
 *
 * 联机: 只发标准事件, 各端同一份原版代码执行。本地只读 + 标准事件入队,
 * 不写模拟内存。
 *
 * 构建见 build-fast.bat (32 位 mingw, 无 CRT, 与 MOExperience 同一 DLL)。
 * 轮询挂 PowerDrawBoth, 橡皮线挂 MOExperience_MessageDrawHook (见 MOExperience.cpp)。
 * 日志: 仅启动行。
 */

#include <windows.h>
#include "EngineAddr.h"
#include <math.h>
#include "GdiText.h"
#include "SmartOrder.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"
#include "SmartArea.h"
#include "DeployPreview.h"

#define SO_SECTION "SmartOrder"

/* ---------------- 散开私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define ADDR_TYPARRAY 0xA8EB00u
#define SO_CAP 1024         /* 快照单位上限 (超限进 OT 队列分帧, 与 OT_QCAP 对齐) */
#define SO_PEND_CAP 1024    /* 待发上限 (同上) */
#define SO_DRAIN_PER_POLL 128
#define FN_TGT_CELL ((Fn_TargetFromCell)(void*)0x6E6B20u)

typedef void* __attribute__((thiscall)) (*Fn_TargetFromCell)(void* tgt, const CellStruct* cell);
/* ---------------- 配置 ---------------- */

static int g_SoInit = 0;
static int g_SoEnable = 1;
static OU_Hotkey g_SoHk;      /* Hotkey=Middle (裸中键, Z/Ctrl/组合中键按住时让路) */
static int g_SoNodeStep = 2;    /* 拖线采样步长 (格, 切比雪夫距离) */

/* 分类判据 (参考 RACommands, MO rulesmo 实测 + 内存 dump 双确认):
 * Aircraft 恒飞; 步兵/车辆 MZ==Fly(9), ST==Winged(4) 兜底。
 * MO 的飞行步兵/悬停直升机 MZ=Fly、ST=Hover(3), 故判据以 MZ==9 为主。 */
#define SO_OFF_MZ 1460   /* dump 实证 (JUMPJET=9=Fly, MAMM=2=Destroyer) */
#define SO_OFF_ST 1596   /* 未定死: 当前读垃圾, 命中概率极低, 仅作无害兜底 */
#define SO_MZ_FLY 9
#define SO_ST_WINGED 4

static void SO_EnsureInit(void)
{
    if (g_SoInit) return;
    g_SoInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_SoEnable = GetPrivateProfileIntA(SO_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    OU_ParseHotkey("Middle", &g_SoHk);            /* 默认 (必成功) */
    OU_ReadHotkey(SO_SECTION, "Hotkey", &g_SoHk); /* 有则覆盖, 坏串留默认 */
    if (g_SoHk.disabled) g_SoEnable = 0;
    g_SoNodeStep = GetPrivateProfileIntA(SO_SECTION, "NodeStep", 2, g_IniPath);
    if (g_SoNodeStep < 1) g_SoNodeStep = 1;
    if (g_SoNodeStep > 8) g_SoNodeStep = 8;
    LogLine("smartorder: ON (middle smart)");
    if (!g_SoEnable) LogLine("smartorder: OFF");
}

/* ---------------- 小工具 (通用部分见 OrderUtil.h) ---------------- */

/* ---------------- 类型自校准 ---------------- */

static int g_SoTypeOff[3] = { -1, -1, -1 }; /* 按类别 pin: 0=Unit 1=Aircraft 2=Infantry */
static int g_SoIdOff = -1;                  /* 类型内 ID 字符串偏移 pin */

static int SO_ClassSlot(int w)
{
    if (w == ABS_UNIT) return 0;
    if (w == ABS_AIRCRAFT) return 1;
    if (w == ABS_INFANTRY) return 2;
    return -1;
}

static int SO_StrOk(const char* s)
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
static int SO_TypeID(void* p, char* out)
{
    static const unsigned cands[3] = { 0x20u, 0x24u, 0x28u };
    int i;
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (g_SoIdOff >= 0)
    {
        const char* s = (const char*)p + (unsigned)g_SoIdOff;
        int k;
        if (!SO_StrOk(s)) { g_SoIdOff = -1; }
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
        if (!SO_StrOk(s)) continue;
        g_SoIdOff = (int)cands[i];
        for (k = 0; k < 24; k++) out[k] = s[k];
        return 1;
    }
    return 0;
}

/* 类型数组元素数 (守卫后返回, 否则 0)。 */
static int SO_TypeCount(void*** itemsOut)
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

static int SO_InTypeSet(void* p)
{
    void** items = 0;
    int cnt, i;
    cnt = SO_TypeCount(&items);
    if (!cnt) return 0;
    for (i = 0; i < cnt; i++)
        if (items[i] == p) return 1;
    return 0;
}

/* 单位取 Type 指针 (pin 命中则快路, 否则全扫; 跨单位一致即自证)。 */
static void* SO_TypeOf(void* u, int w)
{
    int slot = SO_ClassSlot(w);
    unsigned off;
    char idb[24];
    int k;
    if (slot < 0) return 0;
    if (g_SoTypeOff[slot] >= 0)
    {
        void* p = *(void**)((char*)u + (unsigned)g_SoTypeOff[slot]);
        if (p && IsPlausiblePtr(p) && SO_InTypeSet(p) && SO_TypeID(p, idb))
            return p;
        g_SoTypeOff[slot] = -1;
    }
    for (off = 0x40u; off < 0x900u; off += 4u)
    {
        void* p = *(void**)((char*)u + off);
        if (!p || !IsPlausiblePtr(p)) continue;
        if (!SO_InTypeSet(p)) continue;
        for (k = 0; k < 24; k++) idb[k] = 0;
        if (!SO_TypeID(p, idb)) continue;
        g_SoTypeOff[slot] = (int)off;
        return p;
    }
    return 0;
}

/* ---------------- 快照与触发 ---------------- */

typedef struct { void* unit; int id; int w; int fly; } SOUnit;

static SOUnit g_SoUnits[SO_CAP];
/* 空军/地面下标表 (预览与松手共用, 单线程轮询内串行使用)。 */
static int s_SoAir[SO_CAP], s_SoGnd[SO_CAP];
static int g_SoCount = 0;
static int g_SoIdxOk = 0;
static int g_SoArmed = 0;
static int g_SoAx = 0, g_SoAy = 0;
static int g_SoPx = 0, g_SoPy = 0;
static int g_SoPrevTrig = 0;

/* 拖线采样节点 (弧形槽位源, 上限 64, 稀疏记录)。
 * 格用于落点计算; 屏坐标用于画线 (采样时刻实测像素, 不估算——
 * 等距视角下格差到像素没有各轴独立系数, 估算必错)。 */
#define SO_NODE_CAP 64
static int g_SoNx[SO_NODE_CAP], g_SoNy[SO_NODE_CAP];
static int g_SoNPx[SO_NODE_CAP], g_SoNPy[SO_NODE_CAP];
static int g_SoNodeCount = 0;

/* 绿线预览缓冲 (端点 leptons 世界坐标, 见 OrderUtil.h OU_PrevLine)。
 * 分配走 SO_Queue/SO_SmartPoint 同一入口 (s_SoPrevSink=1 时只记线不下令),
 * 预览=终判同一套数学, 终判松手重算一次。 */
static OU_PrevLine s_SoPrevBuf[OU_PREV_CAP];
static int s_SoPrevN = 0;
static int s_SoPrevSink = 0;      /* 1=预览只记线 (不碰 Pend/Obj 队列) */

static void SO_NodePush(int cx, int cy, int px, int py)
{
    int lx, ly, dx, dy, adx, ady;
    if (g_SoNodeCount <= 0)
    {
        g_SoNx[0] = cx; g_SoNy[0] = cy;
        g_SoNPx[0] = px; g_SoNPy[0] = py;
        g_SoNodeCount = 1;
        return;
    }
    if (g_SoNodeCount >= SO_NODE_CAP) return;
    lx = g_SoNx[g_SoNodeCount - 1]; ly = g_SoNy[g_SoNodeCount - 1];
    dx = cx - lx; dy = cy - ly;
    adx = dx >= 0 ? dx : -dx; ady = dy >= 0 ? dy : -dy;
    if (adx < g_SoNodeStep && ady < g_SoNodeStep) return; /* 太密不记 */
    g_SoNx[g_SoNodeCount] = cx; g_SoNy[g_SoNodeCount] = cy;
    g_SoNPx[g_SoNodeCount] = px; g_SoNPy[g_SoNodeCount] = py;
    g_SoNodeCount++;
}

static void SO_Press(void)
{
    void** items;
    int selN, i, nu = 0;
    int ax, ay, mx, my;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return;
    for (i = 0; i < selN && nu < SO_CAP; i++)
    {
        void* o = items[i];
        char tb[8];
        unsigned ti;
        int w = OU_WhatAmI(o);
        if (w != ABS_UNIT && w != ABS_AIRCRAFT && w != ABS_INFANTRY) continue;
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        g_SoUnits[nu].unit = o;
        g_SoUnits[nu].id = FN_TGT_OBJ(tb, o) ? *(int*)(tb + 0) : 0;
        g_SoUnits[nu].w = w;
        nu++;
    }
    if (nu <= 0) return;
    /* ID 索引自检 (PatrolCircle 同式, 失败则 fail-open)。 */
    g_SoIdxOk = 0;
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        if (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 20000 &&
            g_SoUnits[0].id > 0 &&
            OU_PtrAliveRaw(g_SoUnits[0].unit, g_SoUnits[0].id))
            g_SoIdxOk = 1;
    }
    /* 分类: Aircraft 恒飞; 步兵/车辆按运动类型 (MZ==Fly, ST==Winged 兜底)。 */
    for (i = 0; i < nu; i++)
    {
        SOUnit* u = &g_SoUnits[i];
        int mz, st;
        void* tp = 0;
        u->fly = 0;
        if (u->w == ABS_AIRCRAFT) u->fly = 1;
        else
        {
            tp = SO_TypeOf(u->unit, u->w);
            if (tp)
            {
                mz = *(int*)((char*)tp + SO_OFF_MZ);
                st = *(int*)((char*)tp + SO_OFF_ST);
                if (mz == SO_MZ_FLY || st == SO_ST_WINGED) u->fly = 1;
            }
        }
    }
    if (!OU_MouseCellPx(&ax, &ay, &mx, &my)) return;
    g_SoArmed = 1;
    g_SoAx = ax; g_SoAy = ay;
    g_SoPx = mx; g_SoPy = my;
    g_SoCount = nu;
    g_SoNodeCount = 0;
    s_SoPrevN = 0;
    s_SoPrevSink = 0;
    SO_NodePush(ax, ay, mx, my); /* 首节点=锚点 (格+实测像素) */
}

static int SO_Rnd(double v)
{
    return (int)(v + (v >= 0 ? 0.5 : -0.5));
}

/* 落点校验: 首选格无效则方形环由近及远找有效格 (半径 5), 找不到则丢弃。 */
static int SO_NearValid(int x, int y, int* ox, int* oy)
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

typedef struct { void* unit; int id; short x, y; } SOPend;

static SOPend g_SoPend[SO_PEND_CAP];
static int g_SoPendCount = 0;
static void* g_SoHouse = 0;
static int g_SoHouseIdx = 0;

/* 普通移动令: Mission=Move, Target=空, Destination=格, IsPlanningEvent=0
 * (与手点右键移动同形, 非巡逻)。 */
static int SO_FireMove(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstID, unsigned char dstRtti,
                       void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    OU_EvClear(ev);
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_MOVE,
            0, 0, dstID, (unsigned)dstRtti, 0, 0);
    ev[OFF_PLANFLAG] = 0;
    return OT_Fire(ev, unit, uid, idxOk);
}

/* ---------------- 智能对象令 (攻击/进入/进驻/占领, 与原生点击同形) ---
 * 攻击: Mission=Attack, Target=敌对象, Dst=空 (FuzzyAttack 同形)。
 * 进入/进驻/占领: Mission=Enter7/Capture8/Eaten9/Sabotage17,
 * Target=空, Destination=目标对象 (AutoLoad §11 真实点击布局)。
 * 全部 IsPlanningEvent=0 直接下令。 */

typedef struct { void* unit; int uid; void* tgt; int tid; int mission; } SOObj;

static SOObj g_SoObj[SO_PEND_CAP];
static int g_SoObjCount = 0;

static int SO_FireAttack(int houseIdx, int srcID, unsigned char srcRtti,
                         int tgtID, unsigned char tgtRtti,
                         void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    OU_EvClear(ev);
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_ATTACK,
            tgtID, (unsigned)tgtRtti, 0, 0, 0, 0);
    ev[OFF_PLANFLAG] = 0;
    return OT_Fire(ev, unit, uid, idxOk);
}

static int SO_FireDest(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstID, unsigned char dstRtti, int mission,
                       void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    OU_EvClear(ev);
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, mission,
            0, 0, dstID, (unsigned)dstRtti, 0, 0);
    ev[OFF_PLANFLAG] = 0;
    return OT_Fire(ev, unit, uid, idxOk);
}

/* Action(光标) → Mission(订单): gamemd 0x4D74E0 跳转表实证
 * (Enter3→7 载具, Capture9→8, Sabotage16→17;
 * 建筑上的 Enter/GuardArea 一律按进驻 17, 与原生驻军同形)。
 * actWhat = OU_WhatAmI(下令单位): 先过类内重映射 (OU_RemapAction, 复刻
 * InfantryClass 0x51F190 / UnitClass 0x738890) —— 工程师对血厚敌楼的
 * Damage(28)→Capture(9), 不再误判成攻击 (否则会误发主武器攻击)。
 * 未知/不可转发 (Select/Repair/超武等) 回 -1 (调用方按无操作处理,
 * 绝不猜着下单); Move 回 MISSION_MOVE。actWhat=0 表示不重映射。 */
static int SO_MissionFor(int actWhat, int targetWhat, int action)
{
    action = OU_RemapAction(actWhat, action);
    if (action == ACTION_ATTACK || action == 53 || action == 63 ||
        action == 64 || action == 71)
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

/* 全局决策 (OU_DecideAction) 的代表单位类: 选中单位里固有光标 == gAct 的第一个的类,
 * 供类内重映射用 (代表制: 混编按代表单位定)。找不到回 0 (不重映射),
 * 此时工程师等少数动作会落到逐单位回落分支, 结果仍正确。 */
static int SO_RepWhat(int gAct, void* tgt)
{
    int i;
    for (i = 0; i < g_SoCount; i++)
    {
        SOUnit* u = &g_SoUnits[i];
        if (u->fly != 1 && u->fly != 0) continue;
        if (u->w != ABS_UNIT && u->w != ABS_AIRCRAFT && u->w != ABS_INFANTRY)
            continue;
        if (!OU_PtrAlive(u->unit, u->id, g_SoIdxOk)) continue;
        if (OU_MouseOverObject(u->unit, tgt) == gAct) return u->w;
    }
    return 0;
}

static void SO_ShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_SoPendCount; k++)
    {
        g_SoPend[k].unit = g_SoPend[k + 1].unit;
        g_SoPend[k].x = g_SoPend[k + 1].x;
        g_SoPend[k].y = g_SoPend[k + 1].y;
        g_SoPend[k].id = g_SoPend[k + 1].id;
    }
    g_SoPendCount--;
}

static void SO_ObjShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_SoObjCount; k++)
    {
        g_SoObj[k].unit = g_SoObj[k + 1].unit;
        g_SoObj[k].uid = g_SoObj[k + 1].uid;
        g_SoObj[k].tgt = g_SoObj[k + 1].tgt;
        g_SoObj[k].tid = g_SoObj[k + 1].tid;
        g_SoObj[k].mission = g_SoObj[k + 1].mission;
    }
    g_SoObjCount--;
}

/* 排空: 每事件先验活再发; house 变化则清空 (读档/换边防串令)。
 * 先排对象令 (攻击/进入/进驻), 再排移动令。 */
static void SO_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx, sent = 0;
    if (!cur || cur != g_SoHouse) { g_SoPendCount = 0; g_SoObjCount = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (houseIdx != g_SoHouseIdx) { g_SoPendCount = 0; g_SoObjCount = 0; return; }
    while (g_SoObjCount > 0 && sent < SO_DRAIN_PER_POLL)
    {
        SOObj* e = &g_SoObj[0];
        char srcB[8], tgtB[8];
        unsigned i;
        int srcID, tgtID;
        unsigned char srcR, tgtR;
        for (i = 0; i < 8; i++) srcB[i] = tgtB[i] = 0;
        if (!OU_PtrAlive(e->unit, e->uid, g_SoIdxOk)) { SO_ObjShiftDown(); continue; }
        if (OU_WhatAmI(e->unit) != ABS_UNIT &&
            OU_WhatAmI(e->unit) != ABS_AIRCRAFT &&
            OU_WhatAmI(e->unit) != ABS_INFANTRY) { SO_ObjShiftDown(); continue; }
        if (!OU_PtrAlive(e->tgt, e->tid, g_SoIdxOk)) { SO_ObjShiftDown(); continue; }
        if (!FN_TGT_OBJ(tgtB, e->tgt)) { SO_ObjShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { SO_ObjShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        tgtID = *(int*)(tgtB + 0); tgtR = (unsigned char)tgtB[4];
        if (e->mission == MISSION_ATTACK)
        {
            if (!SO_FireAttack(houseIdx, srcID, srcR, tgtID, tgtR,
                               e->unit, e->uid, g_SoIdxOk))
                return;
        }
        else
        {
            if (!SO_FireDest(houseIdx, srcID, srcR, tgtID, tgtR, e->mission,
                              e->unit, e->uid, g_SoIdxOk))
                return;
        }
        SO_ObjShiftDown();
        sent++;
    }
    while (g_SoPendCount > 0 && sent < SO_DRAIN_PER_POLL)
    {
        SOPend* e = &g_SoPend[0];
        char srcB[8], dstB[8];
        CellStruct cs;
        int srcID, dstID;
        unsigned char srcR, dstR;
        unsigned i;
        for (i = 0; i < 8; i++) srcB[i] = dstB[i] = 0;
        cs.X = e->x; cs.Y = e->y;
        FN_TGT_CELL(dstB, &cs);
        dstID = *(int*)(dstB + 0); dstR = (unsigned char)dstB[4];
        if (!OU_PtrAlive(e->unit, e->id, g_SoIdxOk)) { SO_ShiftDown(); continue; }
        if (OU_WhatAmI(e->unit) != ABS_UNIT &&
            OU_WhatAmI(e->unit) != ABS_AIRCRAFT &&
            OU_WhatAmI(e->unit) != ABS_INFANTRY) { SO_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { SO_ShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        if (!SO_FireMove(houseIdx, srcID, srcR, dstID, dstR,
                          e->unit, e->id, g_SoIdxOk))
            return; /* OutList 满: 下轮继续 */
        SO_ShiftDown();
        sent++;
    }
}

/* 落点格上的建筑 (只读 CellClass::FindObjectOfType 0x47C4D0,
 * FuzzyAttack 同入口; 取链头即够, 建筑一格通常唯一)。 */
typedef void* __attribute__((thiscall)) (*Fn_FindObjType)(void* cell, int abs, int alt);
#define FN_FINDOBJTYPE ((Fn_FindObjType)(void*)0x47C4D0u)

static void* SO_CellBuilding(int cx, int cy)
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
static void* g_SoProtTgt = 0;

/* ---------------- 移动分配 (松手下令 / 按住预览共用入口) ---------------- */

static void SO_Queue(int ui, int cx, int cy)
{
    int ox, oy;
    SOUnit* u;
    if (ui < 0 || ui >= g_SoCount) return;
    u = &g_SoUnits[ui];
    if (!SO_NearValid(cx, cy, &ox, &oy)) return;
    /* 落点转换: 地面单位落点格上有建筑时, 按直接下令处理
     * (进驻/攻击/占领), 而不是走过去再转攻击。
     * 空军不管地面建筑; 护航目标同建筑的攻击保持移动。
     * 预览时同规则只记线 (预览期 g_SoHouse 已暂存为当前家)。 */
    if (u->fly == 0 && g_SoHouse)
    {
        void* slotObj = SO_CellBuilding(ox, oy);
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
                ms = (act < 0) ? -1 : SO_MissionFor(u->w, ABS_BUILDING, act);
                if ((ms == MISSION_ATTACK || ms == MISSION_ENTER ||
                      ms == MISSION_SABOTAGE || ms == MISSION_CAPTURE) &&
                    !(slotObj == g_SoProtTgt && ms == MISSION_ATTACK))
                {
                    if (s_SoPrevSink)
                    {
                        if (OU_PtrAlive(u->unit, u->id, g_SoIdxOk))
                            OU_PrevPushObjs(s_SoPrevBuf, &s_SoPrevN, u->unit, slotObj);
                        return;
                    }
                    if (g_SoObjCount < SO_PEND_CAP)
                    {
                        g_SoObj[g_SoObjCount].unit = u->unit;
                        g_SoObj[g_SoObjCount].uid = u->id;
                        g_SoObj[g_SoObjCount].tgt = slotObj;
                        g_SoObj[g_SoObjCount].tid = tid;
                        g_SoObj[g_SoObjCount].mission = ms;
                        g_SoObjCount++;
                        return;
                    }
                }
            }
        }
    }
    if (s_SoPrevSink)
    {
        if (OU_PtrAlive(u->unit, u->id, g_SoIdxOk))
            OU_PrevPushCell(s_SoPrevBuf, &s_SoPrevN, u->unit, ox, oy);
        return;
    }
    if (g_SoPendCount >= SO_PEND_CAP) return;
    g_SoPend[g_SoPendCount].unit = u->unit;
    g_SoPend[g_SoPendCount].id = u->id;
    g_SoPend[g_SoPendCount].x = (short)ox;
    g_SoPend[g_SoPendCount].y = (short)oy;
    g_SoPendCount++;
}

/* 螺旋格表 (点按散开 / SmartPoint 移动子集 / 预览三处共用):
 * 以 (bx,by) 为中心向外, 只收有效格 (上限 512);
 * skipCenter=1 跳过中心格 (建筑目标移动子集用, 防往建筑本体派移动令)。
 * 配额计数一并清零, 回格数。 */
static int SO_SpiralCells(int bx, int by, int skipCenter,
                          int* qcx, int* qcy, int* quA, int* quI, int* quV)
{
    int qn = 0, r;
    if (!skipCenter)
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
    return qn;
}

/* 螺旋配额分配 (三处共用, 经 SO_Queue: 预览 sink 下只记线)。
 * 步兵 3/格, 飞机 1/格, 车辆 1/格 (三类计数独立)。
 * missions==0 全员; 否则只分 missions[k]==MISSION_MOVE 的单位。 */
static void SO_SpiralAssign(int bx, int by, int skipCenter, int* missions)
{
    static int qcx[512], qcy[512];
    static int quA[512], quI[512], quV[512];
    int qn, k;
    qn = SO_SpiralCells(bx, by, skipCenter, qcx, qcy, quA, quI, quV);
    for (k = 0; k < g_SoCount; k++)
    {
        SOUnit* u = &g_SoUnits[k];
        int c;
        if (missions && missions[k] != MISSION_MOVE) continue;
        if (u->fly != 1 && u->fly != 0) continue;
        if (u->w != ABS_UNIT && u->w != ABS_AIRCRAFT && u->w != ABS_INFANTRY) continue;
        if (!OU_PtrAlive(u->unit, u->id, g_SoIdxOk)) continue;
        if (s_SoPrevSink && s_SoPrevN >= OU_PREV_CAP) break;
        for (c = 0; c < qn; c++)
        {
            if (u->w == ABS_INFANTRY) { if (quI[c] >= 3) continue; quI[c]++; }
            else if (u->w == ABS_AIRCRAFT) { if (quA[c] >= 1) continue; quA[c]++; }
            else { if (quV[c] >= 1) continue; quV[c]++; }
            SO_Queue(k, qcx[c], qcy[c]);
            break;
        }
    }
}

/* 一组按轨迹弧长重采样 (sqrt 是 gcc 内建, 无需 libm, PatrolCircle 同式)。 */
static void SO_ArcGroup(int* idx, int n)
{
    static double scum[SO_NODE_CAP];
    int nc = g_SoNodeCount;
    int j, k, seg;
    double L;
    if (n <= 0) return;
    if (nc > SO_NODE_CAP) nc = SO_NODE_CAP;
    if (nc < 2) { for (k = 0; k < n; k++) SO_Queue(idx[k], g_SoNx[0], g_SoNy[0]); return; }
    scum[0] = 0.0;
    for (j = 1; j < nc; j++)
    {
        double dx = (double)(g_SoNx[j] - g_SoNx[j - 1]);
        double dy = (double)(g_SoNy[j] - g_SoNy[j - 1]);
        scum[j] = scum[j - 1] + sqrt(dx * dx + dy * dy);
    }
    L = scum[nc - 1];
    if (L < 0.5) { for (k = 0; k < n; k++) SO_Queue(idx[k], g_SoNx[0], g_SoNy[0]); return; }
    seg = 0;
    for (k = 0; k < n; k++)
    {
        double s = ((double)k + 0.5) * L / (double)n;
        double fx, fy, t, segLen;
        while (seg + 1 < nc - 1 && scum[seg + 1] < s) seg++;
        segLen = scum[seg + 1] - scum[seg];
        t = (segLen > 0.0001) ? (s - scum[seg]) / segLen : 0.0;
        fx = (double)g_SoNx[seg] + (double)(g_SoNx[seg + 1] - g_SoNx[seg]) * t;
        fy = (double)g_SoNy[seg] + (double)(g_SoNy[seg + 1] - g_SoNy[seg]) * t;
        SO_Queue(idx[k], SO_Rnd(fx), SO_Rnd(fy));
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
static int SO_SmartPoint(int bx, int by, void* tgt)
{
    int i, tgtWhat, tgtTid = 0;
    static int missions[SO_CAP];
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
    gMiss = SO_MissionFor(SO_RepWhat(gAct, tgt), tgtWhat, gAct);
    if (gMiss == MISSION_MOVE) return 0; /* 移动: 调用方散开 */
    if (gMiss < 0)
    {
        /* 全局 NoMove/不可转发: 退回逐单位游戏判定 (与悬停光标同源)。
         * 有单位能攻击/进入/进驻/占领就下单 (防全局误判吞令);
         * 否则无操作 (绝不往对象上散开)。 */
        fbMode = 1;
        for (i = 0; i < g_SoCount; i++)
        {
            SOUnit* u = &g_SoUnits[i];
            int act, ms;
            missions[i] = -1;
            if (u->fly != 1 && u->fly != 0) continue;
            if (u->w != ABS_UNIT && u->w != ABS_AIRCRAFT && u->w != ABS_INFANTRY)
                continue;
            if (!OU_PtrAlive(u->unit, u->id, g_SoIdxOk)) continue;
            act = OU_MouseOverObject(u->unit, tgt);
            if (act < 0) ms = -1;
            else ms = SO_MissionFor(u->w, tgtWhat, act);
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
    for (i = 0; i < g_SoCount; i++)
    {
        SOUnit* u = &g_SoUnits[i];
        int act, ms;
        missions[i] = -1;
        if (u->fly != 1 && u->fly != 0) continue;
        if (u->w != ABS_UNIT && u->w != ABS_AIRCRAFT && u->w != ABS_INFANTRY)
            continue;
        if (!OU_PtrAlive(u->unit, u->id, g_SoIdxOk)) continue;
        act = OU_MouseOverObject(u->unit, tgt);
        if (act < 0) ms = -1;
        else ms = SO_MissionFor(u->w, tgtWhat, act);
        if (ms == gMiss) missions[i] = ms;
        else if (ms == MISSION_MOVE) missions[i] = MISSION_MOVE;
        else missions[i] = -1;
    }
    }
    /* 护驻军 (两路通用): 建筑有单位能进驻/占领/进入时, 攻击改移动。 */
    if (tgtWhat == ABS_BUILDING)
    {
        int hasGar = 0;
        for (i = 0; i < g_SoCount; i++)
        {
            if (missions[i] == MISSION_SABOTAGE || missions[i] == MISSION_CAPTURE ||
                missions[i] == MISSION_ENTER) { hasGar = 1; break; }
        }
        if (hasGar)
        {
            if (gMiss == MISSION_ATTACK) gMiss = MISSION_SABOTAGE;
            for (i = 0; i < g_SoCount; i++)
            {
                if (missions[i] == MISSION_ATTACK)
                    missions[i] = MISSION_MOVE;
            }
        }
    }
    for (i = 0; i < g_SoCount; i++)
    {
        if (g_SoUnits[i].fly != 1 && g_SoUnits[i].fly != 0) continue;
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
    /* 对象令入队 (攻击 Target=敌, 进入类 Destination=目标)。
     * 预览时同规则只记线。 */
    for (i = 0; i < g_SoCount; i++)
    {
        int ms = missions[i];
        if (ms == MISSION_MOVE || ms < 0) continue;
        if (g_SoUnits[i].fly != 1 && g_SoUnits[i].fly != 0) continue;
        if (s_SoPrevSink)
        {
            if (s_SoPrevN >= OU_PREV_CAP) break;
            if (OU_PtrAlive(g_SoUnits[i].unit, g_SoUnits[i].id, g_SoIdxOk))
                OU_PrevPushObjs(s_SoPrevBuf, &s_SoPrevN, g_SoUnits[i].unit, tgt);
            continue;
        }
        if (g_SoObjCount >= SO_PEND_CAP) break;
        g_SoObj[g_SoObjCount].unit = g_SoUnits[i].unit;
        g_SoObj[g_SoObjCount].uid = g_SoUnits[i].id;
        g_SoObj[g_SoObjCount].tgt = tgt;
        g_SoObj[g_SoObjCount].tid = tgtTid;
        g_SoObj[g_SoObjCount].mission = ms;
        g_SoObjCount++;
    }
    /* 移动子集绕目标散开 (配额与原点按一致: 步兵3/格, 飞机/车辆1/格)。
     * 建筑目标跳过中心格 (本格极可能是建筑本体, 不往上派移动令,
     * 防引擎转移动为攻击); 环从 r=1 起。
     * 子集落点转换时护航目标不转回攻击 (见 g_SoProtTgt)。 */
    if (nMov > 0)
    {
        g_SoProtTgt = tgt;
        SO_SpiralAssign(bx, by, tgtWhat == ABS_BUILDING, missions);
    }
    if (s_SoPrevSink) return 1;   /* 预览: 只记线, 不排空下令 */
    SO_Drain();
    return 1;
}

/* 绿线预览: 当前鼠标格/目标 + 现有轨迹节点, 与 SO_Release 同规则分配,
 * s_SoPrevSink=1 下只记线不下令。节流: 换格/节点数变且距上次>80ms,
 * 或超 400ms 刷一次 (单位会动)。 */
static void SO_Preview(void)
{
    void* cur;
    int bx, by, i;
    void* tgt = 0;
    static unsigned s_LastMs = 0;
    static int s_LastBx = -9999, s_LastBy = -9999, s_LastNc = -1;
    int airN = 0, gndN = 0;
    void* saveHouse;
    int saveIdx;
    void* saveProt;
    int done = 0;

    if (!OU_MouseCellTarget(&bx, &by, &tgt, 0, 0)) { s_SoPrevN = 0; return; }
    if (!OU_PrevThrottle(&s_LastMs, &s_LastBx, &s_LastBy, &s_LastNc,
                         bx, by, g_SoNodeCount, 80, 400)) return;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) { s_SoPrevN = 0; return; }
    if (g_SoCount <= 0) { s_SoPrevN = 0; return; }
    /* 存活分组 (Release 同式, 不写 fly, 只分组): */
    airN = 0; gndN = 0;
    for (i = 0; i < g_SoCount; i++)
    {
        SOUnit* u = &g_SoUnits[i];
        if (!OU_PtrAlive(u->unit, u->id, g_SoIdxOk)) continue;
        if (u->fly == 1) { if (airN < SO_CAP) s_SoAir[airN++] = i; }
        else if (u->fly == 0) { if (gndN < SO_CAP) s_SoGnd[gndN++] = i; }
    }
    if (airN + gndN <= 0) { s_SoPrevN = 0; return; }
    s_SoPrevN = 0;
    s_SoPrevSink = 1;
    saveHouse = g_SoHouse; saveIdx = g_SoHouseIdx; saveProt = g_SoProtTgt;
    g_SoHouse = cur;
    g_SoHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    g_SoProtTgt = 0;
    if (tgt && IsPlausiblePtr(tgt))
        done = SO_SmartPoint(bx, by, tgt);
    if (!done)
    {
        if (g_SoNodeCount <= 1)
        {
            /* 点按: 松开格为中心螺旋配额 (Release 同式)。 */
            SO_SpiralAssign(bx, by, 0, 0);
        }
        else
        {
            /* 划线: 空军/地面分组弧长重采样 (Release 同式, 经 SO_Queue 记线)。 */
            SO_ArcGroup(s_SoAir, airN);
            SO_ArcGroup(s_SoGnd, gndN);
        }
    }
    g_SoHouse = saveHouse; g_SoHouseIdx = saveIdx; g_SoProtTgt = saveProt;
    s_SoPrevSink = 0;
}

static void SO_Release(void)
{
    int bx, by, mx, my;
    int i;
    int airN = 0, gndN = 0;
    void* tgt = 0;
    g_SoArmed = 0;
    s_SoPrevN = 0;
    if (OU_MouseCellTarget(&bx, &by, &tgt, &mx, &my))
    {
        /* 松开格收尾 (与末节点不同则补记, 尾段不丢)。 */
        if (g_SoNodeCount > 0 &&
            (bx != g_SoNx[g_SoNodeCount - 1] || by != g_SoNy[g_SoNodeCount - 1]) &&
            g_SoNodeCount < SO_NODE_CAP && OU_CellValid(bx, by))
        {
            g_SoNx[g_SoNodeCount] = bx; g_SoNy[g_SoNodeCount] = by;
            g_SoNPx[g_SoNodeCount] = mx; g_SoNPy[g_SoNodeCount] = my;
            g_SoNodeCount++;
        }
    }
    else if (g_SoNodeCount > 0) { bx = g_SoNx[g_SoNodeCount - 1]; by = g_SoNy[g_SoNodeCount - 1]; }
    else return;
    /* 存活过滤: 淘汰死单位 (fly=-1); 分组: fly==1 空军, fly==0 地面。 */
    for (i = 0; i < g_SoCount; i++)
    {
        SOUnit* u = &g_SoUnits[i];
        if (!OU_PtrAlive(u->unit, u->id, g_SoIdxOk)) { u->fly = -1; continue; }
        if (u->fly == 1) s_SoAir[airN++] = i;
        else if (u->fly == 0) s_SoGnd[gndN++] = i;
    }
    if (airN + gndN <= 0) return;
    {
        void* cur = *(void**)ADDR_CURPLAYER;
        if (!cur || !IsPlausiblePtr(cur)) return;
        g_SoHouse = cur;
        g_SoHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    }
    g_SoPendCount = 0;
    g_SoObjCount = 0;
    g_SoProtTgt = 0;
    if (tgt)
    {
        /* 终点点中对象: 先走智能自适应 (攻击/进入/进驻/占领)。
         * r==2 无操作: 直接返回。r==1 已下单: 直接返回
         * (SmartPoint 内部已排空, 绝不能以队列余量判断,
         * 否则已发的单会被弧形移动覆盖)。r==0 全移动:
         * 点按走点散开, 划线走弧形 (保阵型)。 */
        int r = SO_SmartPoint(bx, by, tgt);
        if (r) return;
    }
    if (g_SoNodeCount <= 1)
    {
        /* 点按: 像左键点移动一样到目的地, 按配额分开。
         * 步兵 3/格, 飞机 1/格, 车辆 1/格 (三类计数独立)。
         * 以松开格为中心螺旋向外 (只收有效格, 上限 512 格)。 */
        SO_SpiralAssign(bx, by, 0, 0);
        SO_Drain();
        return;
    }
    /* 划线: 空军/地面分组, 各按轨迹弧长重采样为本组槽位数 (弧形怎么画就怎么排)。 */
    SO_ArcGroup(s_SoAir, airN);
    SO_ArcGroup(s_SoGnd, gndN);
    SO_Drain();
}

void SmartOrder_Poll(void)
{
    int trigDown;
    SO_EnsureInit();
    trigDown = OU_KeyDown(g_SoHk.vk);
    if (!g_SoEnable) { g_SoArmed = 0; s_SoPrevN = 0; g_SoPrevTrig = trigDown; return; }
    if (g_SoPendCount > 0 || g_SoObjCount > 0) { SO_Drain(); g_SoPrevTrig = trigDown; return; }
    if (trigDown && g_SoArmed)
    {
        /* 按住途中加了 Hotkey 附加键/修饰键、Z/Ctrl 或组合中键即改主意了:
         * 散开取消待命 (松开不执行), 执行权交巡逻/砍树/模糊/区域自适应
         * (它们按松开瞬间组合键判定; 区域自适应关则不认领, 照旧散开)。
         * 单基地车中途被认领 (DeployPreview) 同样让路。 */
        if (!OU_ModsDown(g_SoHk.mods) || !OU_ExtraDown(&g_SoHk) ||
            OU_KeyDown('Z') || OU_KeyDown(VK_CONTROL) ||
            SmartArea_ClaimsMiddle() || DeployPreview_ClaimsMiddle())
        {
            g_SoArmed = 0;
            s_SoPrevN = 0;
        }
    }
    if (trigDown && g_SoArmed)
    {
        /* 按住拖动: 采样轨迹节点 (格+实测像素, 稀疏记录, 弧形槽位源)。
         * 同步刷绿线预览 (单位→预估落点, 与松手分配同规则)。 */
        int cx, cy, px, py;
        if (OU_MouseCellPx(&cx, &cy, &px, &py))
            SO_NodePush(cx, cy, px, py);
        SO_Preview();
    }
    if (trigDown && !g_SoPrevTrig)
    {
        /* 按下: Z 按住时归 PatrolCircle, Ctrl 按住时归 FuzzyAttack,
         * 组合中键按住时归 SmartArea, 单基地车裸中键归 DeployPreview,
         * 这里只认 Hotkey 裸键 (互斥见上面)。 */
        if (OU_ModsDown(g_SoHk.mods) && OU_ExtraDown(&g_SoHk) &&
            !OU_KeyDown('Z') && !OU_KeyDown(VK_CONTROL) &&
            !SmartArea_ClaimsMiddle() && !DeployPreview_ClaimsMiddle())
            SO_Press();
    }
    else if (!trigDown && g_SoPrevTrig && g_SoArmed)
        SO_Release();
    g_SoPrevTrig = trigDown;
}

/* ---------------- 渲染期橡皮线 (3px 高亮) ---------------- */

static unsigned char* s_AsLocked = 0;
static int s_AsW = 0, s_AsH = 0, s_AsPitch = 0;

/* 3px 白线 / 实心方块 (公共实现: OrderUtil 的 OU_Line / OU_Box)。 */
static void SO_Line(int x0, int y0, int x1, int y1, unsigned a)
{
    OU_Line(s_AsLocked, s_AsW, s_AsH, s_AsPitch, 0,
            x0, y0, x1, y1, 255, 255, 255, a, 3);
}

static void SO_Mark(int cx, int cy, int m, unsigned a)
{
    OU_Box(s_AsLocked, s_AsW, s_AsH, s_AsPitch, 0,
           cx, cy, m, 255, 255, 255, a);
}

void SmartOrder_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* locked;
    int x0, y0, x1, y1;
    extern int g_Calibrated;
    if (!g_SoEnable || !g_SoArmed) return;
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    if (!GtMouseGameXY(&x1, &y1)) return;
    x0 = g_SoPx; y0 = g_SoPy;
    locked = OU_SurfLock(surf);
    if (!locked) return;
    s_AsLocked = locked; s_AsW = w; s_AsH = h; s_AsPitch = pitch;
    /* 节点折线 (全用采样时刻实测像素, 弧形保真) + 橡皮尾巴 (末节点→当前鼠标)。
     * 注: 中键拖动会平移镜头, 前后节点像素不在同一镜头帧, 大位移拖动线会
     * 整体漂, 这是引擎行为, 与 PatrolCircle 同 caveat。 */
    {
        int i, ex, ey;
        int nc = g_SoNodeCount;
        if (nc > SO_NODE_CAP) nc = SO_NODE_CAP;
        ex = x0; ey = y0;
        for (i = 0; i < nc; i++)
        {
            int nx = g_SoNPx[i], ny = g_SoNPy[i];
            if (i > 0) SO_Line(ex, ey, nx, ny, 230);
            SO_Mark(nx, ny, 1, 230);
            ex = nx; ey = ny;
        }
        SO_Line(ex, ey, x1, y1, 230);
        SO_Mark(x0, y0, 3, 255); /* 锚点 7x7 */
        SO_Mark(x1, y1, 2, 255); /* 当前 5x5 */
    }
    /* 绿线预览 (单位中心→预估落点/目标中心, 含高度, 只在按住时存在)。 */
    OU_PrevDraw(s_SoPrevBuf, s_SoPrevN, locked, w, h, pitch, 0, 255, 0, 255, 1);
    s_AsLocked = 0;
    OU_SurfUnlock(surf);
}
