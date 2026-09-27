/* SmartArea.cpp - Shift+中键范围智能下令 (Ctrl+中键模糊攻击的自适应版)。
 *
 * 触发: 按住组合键 (Hotkey, 默认 Shift+Middle) 点按/划线, 先松中键
 * 再松组合键执行 (与模糊同手势)。按住显示生效块菱形框。
 * 点按=单块, 划线=沿轨迹各节点逐块由近及远挑目标 (上限 64 节点)。
 * 松开格直接点中的对象排首位。
 * Shift 处理 (只读, 不接管不写键态): 决策问 MouseOver, Shift 按住时
 * 对部分目标回 ToggleSelect(8, 加选语义), 真光标被盖住。
 * 方法与裸中键同一套 (DecideAction/MouseOver 同函数), 差的是输入。
 * 故 fallback 规则表 (光标无令时, 与左键语义对齐):
 * 本家目标: 步兵对自家载具按进入 (运输机收人, 非运输忽略),
 * 其余跳过 (绝不误击本家)。
 * 非本家目标 (Owner 双单位自校准; 盟友视同模糊, 下手前看清归属):
 * 载具/飞机一律攻击; 步兵对非建筑攻击;
 * 步兵对建筑: 工程师 (ENGINEER/S/Y/F, rulesmo 实查) 按占领
 * (敌建筑占领/中立进驻, 步兵进民房原生即 Mission8),
 * 非工程师留待命, 组合键松开后重判 (真光标, 攻击或进驻都行)。
 * 进驻均分: 步兵对建筑按 Capture/Sabotage 的不管 SquadSize,
 * 按建筑轮转均分 (第0轮)。
 * 规划线 (按住时): 预览=快照+分配 (与终判同一套输入同一套规则,
 * 终判松手重扫一次), 从各单位身上画绿色细线连到分到的目标;
 * 预览截断前 64 单位/前 96 目标 (终判全量), 限 128 线,
 * 换格或 400ms 刷一次, 80ms 节流; 松手即消。
 * 端点坐标走 OU_GetCoords (vtbl+0x48, 范围校验), 投影与方框同系。
 * 松中键当场执行, Shift 可一直按住连点; 待命框保持, 超 300 轮丢弃。
 * 决策: 攻击/进入/进驻/占领才下单, 移动/未知一律跳过 (本功能不下
 * 移动令, 分不到目标的单位原地待命)。选不进同一代表, 混编各按自己光标。
 * 分组 (SquadSize, 默认 4, 与模糊同义): 每目标先分 Squad 个,
 * 剩下的找最近可处理目标加入 (全员有事做, 不添油)。
 * 下令: 标准 0x04 MegaMission (攻击 Target=对象, 进入类 Destination=对象,
 * IsPlanningEvent=0, 与手点同形), OutList 满 drip 跨帧续发。
 * 仲裁: 须无 Ctrl/Z (含 Ctrl 归模糊, 含 Z 归巡逻/砍树); 关或热键坏则
 * 不认领, 组合中键回落裸中键 (AirSpread 问 ClaimsMiddle 决定让路)。
 * 原生 Shift (多选/排路点) 只在左/右键上, 中键无原生功能, 无冲突。
 *
 * 联机: 只发标准事件, 本地只读 + 标准事件入队, 不写模拟内存。
 * 构建见 构建.bat (同一 DLL)。轮询挂 PowerDrawBoth (见 MsgFont.cpp)。
 * 日志: 仅启动行。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "SmartArea.h"

#define SA_SECTION "SmartArea"

/* ---------------- 私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define OFF_NEXT_OBJ 0x30u    /* ObjectClass.NextObject (YRpp 成员顺序) */

typedef void* __attribute__((thiscall)) (*Fn_FindObjType)(void* cell, int abs, int alt);
typedef void* (__fastcall *SA_LockFn)(void*, void*, int, int);
typedef int (__fastcall *SA_UnlockFn)(void*, void*);

#define VT_SA_LOCK   0x5C
#define VT_SA_UNLOCK 0x60

#define FN_FINDOBJTYPE ((Fn_FindObjType)(void*)0x47C4D0u)

/* ---------------- 配置 ---------------- */

static int g_SaInit = 0;
static int g_SaEnable = 1;
static OU_Hotkey g_SaHk;      /* Hotkey=Shift+Middle (含 Ctrl/Z 时被别家抢走) */
static int g_SaRange = 4;       /* 扫描块边长 (格, 2~16) */
static int g_SaSquad = 4;       /* 几打一 (1~16, 同一目标首轮几个, 满分流) */
static int g_SaOwnerOff = -1;   /* Techno 内 Owner 指针偏移 (FuzzyAttack 同式) */

static void SA_EnsureInit(void)
{
    if (g_SaInit) return;
    g_SaInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_SaEnable = GetPrivateProfileIntA(SA_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    OU_ParseHotkey("Shift+Middle", &g_SaHk);           /* 默认 (必成功) */
    OU_ReadHotkey(SA_SECTION, "Hotkey", &g_SaHk);      /* 有则覆盖, 坏串留默认 */
    if (g_SaHk.disabled) g_SaEnable = 0;
    g_SaRange = GetPrivateProfileIntA(SA_SECTION, "Range", 4, g_IniPath);
    g_SaSquad = GetPrivateProfileIntA(SA_SECTION, "SquadSize", 4, g_IniPath);
    if (g_SaRange < 2) g_SaRange = 2;
    if (g_SaRange > 16) g_SaRange = 16;
    if (g_SaSquad < 1) g_SaSquad = 1;
    if (g_SaSquad > 16) g_SaSquad = 16;
    LogLine("smartarea: ON (shift+middle)");
    if (!g_SaEnable) LogLine("smartarea: OFF");
}

/* 认领中键: 开且组合全按住 (含主键), 且无 Ctrl/Z (它俩归别家)。
 * 注意: 松开瞬间主键已抬起, 认领必掉; 松开判定另用 SA_ComboHeld
 * (不含主键, 与模糊同式), 否则松开边永远不执行。 */
int SmartArea_ClaimsMiddle(void)
{
    SA_EnsureInit();
    if (!g_SaEnable) return 0;
    if (!OU_KeyDown(g_SaHk.vk)) return 0;
    if (!OU_ModsDown(g_SaHk.mods) || !OU_ExtraDown(&g_SaHk)) return 0;
    if (OU_KeyDown(VK_CONTROL) || OU_KeyDown('Z')) return 0;
    return 1;
}

/* 组合保持 (松开边用): 开且修饰/附加键按住、无 Ctrl/Z, 不看主键。 */
static int SA_ComboHeld(void)
{
    SA_EnsureInit();
    if (!g_SaEnable) return 0;
    if (!OU_ModsDown(g_SaHk.mods) || !OU_ExtraDown(&g_SaHk)) return 0;
    if (OU_KeyDown(VK_CONTROL) || OU_KeyDown('Z')) return 0;
    return 1;
}

/* ---------------- 决策 ---------------- */

static int SA_IsAttacker(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY || w == ABS_AIRCRAFT) ? 1 : 0;
}

static int SA_IsTargetType(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY ||
            w == ABS_AIRCRAFT || w == ABS_BUILDING) ? 1 : 0;
}

/* ---------------- 类型自校准 (AirSpread 同式, 工程师判定用) ---------------- */

#define SA_TYPARRAY 0xA8EB00u
#define SA_OFF_VEC_ITEMS 4u
#define SA_OFF_VEC_COUNT 16u

static int g_SaTypeOff[3] = { -1, -1, -1 }; /* 0=Unit 1=Aircraft 2=Infantry */
static int g_SaIdOff = -1;

static int SA_ClassSlot(int w)
{
    if (w == ABS_UNIT) return 0;
    if (w == ABS_AIRCRAFT) return 1;
    if (w == ABS_INFANTRY) return 2;
    return -1;
}

static int SA_StrOk(const char* s)
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

static int SA_TypeID(void* p, char* out)
{
    static const unsigned cands[3] = { 0x20u, 0x24u, 0x28u };
    int i;
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (g_SaIdOff >= 0)
    {
        const char* s = (const char*)p + (unsigned)g_SaIdOff;
        int k;
        if (!SA_StrOk(s)) { g_SaIdOff = -1; }
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
        if (!SA_StrOk(s)) continue;
        g_SaIdOff = (int)cands[i];
        for (k = 0; k < 24; k++) out[k] = s[k];
        return 1;
    }
    return 0;
}

static int SA_TypeCount(void*** itemsOut)
{
    void** items;
    int cnt;
    *itemsOut = 0;
    cnt = *(int*)(SA_TYPARRAY + SA_OFF_VEC_COUNT);
    items = *(void***)(SA_TYPARRAY + SA_OFF_VEC_ITEMS);
    if (cnt <= 0 || cnt > 20000) return 0;
    if (!items || !IsPlausiblePtr(items)) return 0;
    *itemsOut = items;
    return cnt;
}

static int SA_InTypeSet(void* p)
{
    void** items = 0;
    int cnt, i;
    cnt = SA_TypeCount(&items);
    if (!cnt) return 0;
    for (i = 0; i < cnt; i++)
        if (items[i] == p) return 1;
    return 0;
}

static void* SA_TypeOf(void* u, int w)
{
    int slot = SA_ClassSlot(w);
    unsigned off;
    char idb[24];
    int k;
    if (slot < 0) return 0;
    if (g_SaTypeOff[slot] >= 0)
    {
        void* p = *(void**)((char*)u + (unsigned)g_SaTypeOff[slot]);
        if (p && IsPlausiblePtr(p) && SA_InTypeSet(p) && SA_TypeID(p, idb))
            return p;
        g_SaTypeOff[slot] = -1;
    }
    for (off = 0x40u; off < 0x900u; off += 4u)
    {
        void* p = *(void**)((char*)u + off);
        if (!p || !IsPlausiblePtr(p)) continue;
        if (!SA_InTypeSet(p)) continue;
        for (k = 0; k < 24; k++) idb[k] = 0;
        if (!SA_TypeID(p, idb)) continue;
        g_SaTypeOff[slot] = (int)off;
        return p;
    }
    return 0;
}

static int SA_Streq(const char* a, const char* b)
{
    while (*a && *b)
    {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* 工程师判定 (rulesmo 4 种, 加新工程师改这里): 占领/进驻 fallback 用。 */
static int SA_IsEngineer(void* u)
{
    static const char* engs[4] = { "ENGINEER", "SENGINEER", "YENGINEER", "FENGINEER" };
    void* tp;
    char id[24];
    int k, e;
    if (OU_WhatAmI(u) != ABS_INFANTRY) return 0;
    tp = SA_TypeOf(u, ABS_INFANTRY);
    if (!tp) return 0;
    for (k = 0; k < 24; k++) id[k] = 0;
    if (!SA_TypeID(tp, id)) return 0;
    for (e = 0; e < 4; e++)
        if (SA_Streq(id, engs[e])) return 1;
    return 0;
}

/* Action(光标) → Mission: 与 AirSpread 同表 (攻击1/进入7/占领8/进驻17);
 * 移动/未知回 -1 (本功能不下移动令)。
 * 选择类 (Select7/ToggleSelect8, 如坦克悬停自家建筑) 无对应订单, 跳过:
 * 左键等价动作是选中, 下令侧只能待命。 */
static int SA_MissionFor(int targetWhat, int action)
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
    return -1;
}

/* 单位 u 对目标 t 的任务: 只读 MouseOver, 非对象令回 -1。
 * actOut 透出原始光标 (fallback 判定用, 可空)。 */
static int SA_UnitMission(void* u, void* t, int* actOut)
{
    int act, ms;
    if (actOut) *actOut = -1;
    if (!SA_IsAttacker(u) || !SA_IsTargetType(t)) return -1;
    act = OU_MouseOverObject(u, t);
    if (actOut) *actOut = act;
    if (act < 0) return -1;
    ms = SA_MissionFor(OU_WhatAmI(t), act);
    if (ms != MISSION_ATTACK && ms != MISSION_ENTER &&
        ms != MISSION_SABOTAGE && ms != MISSION_CAPTURE)
        return -1;
    return ms;
}

/* fallback 规则表 (光标无令时, 与裸中键左键语义对齐, Shift 下直接可用):
 * 本家目标: 步兵对自家载具按进入 (运输机收人; 非运输忽略, 安全),
 * 其余跳过 (绝不误击本家)。
 * 非本家目标 (Owner 双单位自校准, 盟友视同模糊一并处理, 下手前看清):
 * 载具/飞机一律攻击; 步兵对非建筑攻击;
 * 步兵对建筑: 工程师按占领 (敌建筑占领/中立进驻, 步兵进民房原生即此),
 * 非工程师跳过留待命 (等真光标, 防大兵拆错该驻的楼)。
 * 回 mission, 无规则回 -1。 */
static int SA_Fallback(void* u, void* t, int act, void* house)
{
    int uw, tw;
    void* oh;
    if (act != 8 && act >= 0) return -1;
    if (g_SaOwnerOff < 0 || !house) return -1;
    uw = OU_WhatAmI(u);
    tw = OU_WhatAmI(t);
    if (uw != ABS_UNIT && uw != ABS_AIRCRAFT && uw != ABS_INFANTRY)
        return -1;
    if (tw != ABS_UNIT && tw != ABS_AIRCRAFT && tw != ABS_INFANTRY &&
        tw != ABS_BUILDING)
        return -1;
    oh = *(void**)((char*)t + (unsigned)g_SaOwnerOff);
    if (!oh || !IsPlausiblePtr(oh)) return -1;
    if (oh == house)
        return (uw == ABS_INFANTRY && tw == ABS_UNIT) ? MISSION_ENTER : -1;
    if (uw == ABS_UNIT || uw == ABS_AIRCRAFT) return MISSION_ATTACK;
    if (tw != ABS_BUILDING) return MISSION_ATTACK;
    return SA_IsEngineer(u) ? MISSION_CAPTURE : -1;
}

/* ---------------- 下令 (与 AirSpread/FuzzyAttack 同形) ---------------- */

static int SA_FireAttack(int houseIdx, int srcID, unsigned char srcRtti,
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

static int SA_FireDest(int houseIdx, int srcID, unsigned char srcRtti,
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

#define SA_PEND_CAP 512
#define SA_DRAIN_PER_POLL 128

typedef struct { void* unit; int uid; void* tgt; int tid; int mission; } SAPend;

static SAPend g_SaPend[SA_PEND_CAP];
static int g_SaPendCount = 0;
static int g_SaIdxOk = 0;
static void* g_SaHouse = 0;
static int g_SaHouseIdx = 0;
static int g_SaPrevTrig = 0;

static void SA_ShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_SaPendCount; k++)
    {
        g_SaPend[k].unit = g_SaPend[k + 1].unit;
        g_SaPend[k].uid = g_SaPend[k + 1].uid;
        g_SaPend[k].tgt = g_SaPend[k + 1].tgt;
        g_SaPend[k].tid = g_SaPend[k + 1].tid;
        g_SaPend[k].mission = g_SaPend[k + 1].mission;
    }
    g_SaPendCount--;
}

static void SA_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx, sent = 0;
    if (!cur || cur != g_SaHouse) { g_SaPendCount = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (houseIdx != g_SaHouseIdx) { g_SaPendCount = 0; return; }
    while (g_SaPendCount > 0 && sent < SA_DRAIN_PER_POLL)
    {
        SAPend* e = &g_SaPend[0];
        char srcB[8], tgtB[8];
        unsigned i;
        int srcID, tgtID;
        unsigned char srcR, tgtR;
        for (i = 0; i < 8; i++) srcB[i] = tgtB[i] = 0;
        if (!OU_PtrAlive(e->unit, e->uid, g_SaIdxOk)) { SA_ShiftDown(); continue; }
        if (!SA_IsAttacker(e->unit)) { SA_ShiftDown(); continue; }
        if (!OU_PtrAlive(e->tgt, e->tid, g_SaIdxOk)) { SA_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(tgtB, e->tgt)) { SA_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { SA_ShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        tgtID = *(int*)(tgtB + 0); tgtR = (unsigned char)tgtB[4];
        if (e->mission == MISSION_ATTACK)
        {
            if (!SA_FireAttack(houseIdx, srcID, srcR, tgtID, tgtR)) return;
        }
        else
        {
            if (!SA_FireDest(houseIdx, srcID, srcR, tgtID, tgtR, e->mission)) return;
        }
        SA_ShiftDown();
        sent++;
    }
    /* TMP: 发出数 (定完删) */
    if (sent > 0 || g_SaPendCount <= 0)
    {
        char db[48], d1[16];
        unsigned dq = 0;
        dq = AppendCStr(db, dq, "sa: s ", sizeof(db));
        UtoA((unsigned)sent, d1);
        dq = AppendCStr(db, dq, d1, sizeof(db));
        (void)dq;
        LogLine(db);
    }
}

/* ---------------- 找目标 (环扫 + 链走, 不分敌我) ---------------- */

#define SA_TGT_CAP 256

static void* SA_CellPtr(int cx, int cy)
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

/* 链收集: 不分敌我 (决策交给 MouseOver), 其余与模糊同式。 */
static void SA_CollectChain(void* first, void** out, int* tids, int* nio)
{
    void* o = first;
    int steps = 0;
    while (o && steps < 64 && *nio < SA_TGT_CAP)
    {
        int dup;
        char tb[8];
        unsigned ti;
        steps++;
        if (!IsPlausiblePtr(o)) break;
        for (dup = 0; dup < *nio; dup++)
            if (out[dup] == o) break;
        if (dup >= *nio)
        {
            if (!SA_IsTargetType(o)) goto next;
            for (ti = 0; ti < 8; ti++) tb[ti] = 0;
            if (!FN_TGT_OBJ(tb, o)) goto next;
            out[*nio] = o;
            tids[*nio] = *(int*)(tb + 0);
            (*nio)++;
        }
next:
        o = *(void**)((char*)o + OFF_NEXT_OBJ);
    }
}

/* 单块环扫 (d 由小到大=由近及远), 去重并入 out。 */
static void SA_ScanBlock(int cx, int cy, void** out, int* tids, int* nio)
{
    static const int abss[4] = { ABS_UNIT, ABS_INFANTRY, ABS_AIRCRAFT, ABS_BUILDING };
    int d, dx, dy, t;
    int lo = -(g_SaRange / 2);
    int hi = lo + g_SaRange - 1;
    for (d = 0; d <= g_SaRange && *nio < SA_TGT_CAP; d++)
    {
        for (dy = -d; dy <= d && *nio < SA_TGT_CAP; dy++)
        {
            for (dx = -d; dx <= d && *nio < SA_TGT_CAP; dx++)
            {
                int adx = dx >= 0 ? dx : -dx, ady = dy >= 0 ? dy : -dy;
                void* cell;
                if ((adx >= ady ? adx : ady) != d) continue;
                if (dx < lo || dx > hi || dy < lo || dy > hi) continue;
                cell = SA_CellPtr(cx + dx, cy + dy);
                if (!cell) continue;
                for (t = 0; t < 4; t++)
                {
                    void* head = FN_FINDOBJTYPE(cell, abss[t], 0);
                    if (!head || !IsPlausiblePtr(head)) continue;
                    SA_CollectChain(head, out, tids, nio);
                }
            }
        }
    }
}

/* ---------------- 轨迹节点 (划线多块扫描用, 稀疏记录) ---------------- */

#define SA_NODE_CAP 64

static int g_SaNx[SA_NODE_CAP], g_SaNy[SA_NODE_CAP];
static int g_SaNodeCount = 0;

static void SA_NodePush(int cx, int cy)
{
    int dx, dy, adx, ady;
    if (g_SaNodeCount <= 0)
    {
        g_SaNx[0] = cx; g_SaNy[0] = cy;
        g_SaNodeCount = 1;
        return;
    }
    if (g_SaNodeCount >= SA_NODE_CAP) return;
    dx = cx - g_SaNx[g_SaNodeCount - 1]; dy = cy - g_SaNy[g_SaNodeCount - 1];
    adx = dx >= 0 ? dx : -dx; ady = dy >= 0 ? dy : -dy;
    if (adx < 2 && ady < 2) return;
    g_SaNx[g_SaNodeCount] = cx; g_SaNy[g_SaNodeCount] = cy;
    g_SaNodeCount++;
}

/* ---------------- 执行 ---------------- */

#define SA_UNIT_CAP 256

static void SA_BoxReset(void);
static void SA_Drain(void);

/* TMP 诊断 (fallback 验证, 定完删) */
static void SA_Tmp(const char* tag, int v1, int v2, int v3)
{
    char buf[96], b1[16], b2[16], b3[16];
    unsigned q = 0;
    q = AppendCStr(buf, q, tag, sizeof(buf));
    q = AppendCStr(buf, q, " ", sizeof(buf));
    UtoA((unsigned)v1, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " ", sizeof(buf));
    UtoA((unsigned)v2, b2);
    q = AppendCStr(buf, q, b2, sizeof(buf));
    q = AppendCStr(buf, q, " ", sizeof(buf));
    UtoA((unsigned)v3, b3);
    q = AppendCStr(buf, q, b3, sizeof(buf));
    (void)q;
    LogLine(buf);
}

/* 快照: 单位 + 目标存静态 (找目标与修饰键无关, 决策见 DecideFire)。
 * 返回 1=有可处理快照。 */
static void* s_SnapUnits[SA_UNIT_CAP];
static int s_SnapUids[SA_UNIT_CAP];
static int s_SnapBusy[SA_UNIT_CAP];
static int s_SnapNu = 0;
static void* s_SnapTgts[SA_TGT_CAP];
static int s_SnapTids[SA_TGT_CAP];
static int s_SnapNt = 0;

/* 规划线绘制见末尾 SmartArea_Draw (细线, 按住画松手消)。 */
static int g_SaShiftSeen = 0;     /* 本轮按住途中组合成立过 (防裸中键误入) */
static int g_SaHaveSnapshot = 0;  /* 快照待命: 组合键松开即重判下令 (步兵对建筑等) */
static int g_SaSnapAge = 0;       /* 快照年龄 (轮询计, 超 300 丢弃防 stale) */

/* Owner 自校准 (双单位确认, 纯指针比较不解引用; 失败则本轮无 fallback)。 */
static int SA_CalOwner(void* u0, void* u1, void* house)
{
    unsigned off;
    if (!u0 || !house) return 0;
    if (g_SaOwnerOff >= 0)
    {
        if (*(void**)((char*)u0 + (unsigned)g_SaOwnerOff) == house &&
            (!u1 || *(void**)((char*)u1 + (unsigned)g_SaOwnerOff) == house))
            return 1;
        g_SaOwnerOff = -1;
    }
    for (off = 0x40u; off < 0x900u; off += 4u)
    {
        if (*(void**)((char*)u0 + off) != house) continue;
        if (u1 && *(void**)((char*)u1 + off) != house) continue;
        g_SaOwnerOff = (int)off;
        return 1;
    }
    return 0;
}

static int SA_Snapshot(void)
{
    void* cur;
    void** items;
    int selN, i, nu = 0, nt = 0, j;
    int cx, cy;
    void* direct = 0;

    s_SnapNu = 0;
    s_SnapNt = 0;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return 0;
    if (!OU_MouseCellTarget(&cx, &cy, &direct, 0, 0)) return 0;
    /* 收尾节点 (与末节点不同则补记, 尾段不丢)。 */
    if (g_SaNodeCount > 0 &&
        (cx != g_SaNx[g_SaNodeCount - 1] || cy != g_SaNy[g_SaNodeCount - 1]) &&
        g_SaNodeCount < SA_NODE_CAP && OU_CellValid(cx, cy))
        SA_NodePush(cx, cy);
    if (g_SaNodeCount <= 0) return 0;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return 0;
    for (i = 0; i < selN && nu < SA_UNIT_CAP; i++)
    {
        void* o = items[i];
        char tb[8];
        unsigned ti;
        if (!SA_IsAttacker(o)) continue;
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        s_SnapUnits[nu] = o;
        s_SnapUids[nu] = FN_TGT_OBJ(tb, o) ? *(int*)(tb + 0) : 0;
        s_SnapBusy[nu] = 0;
        nu++;
    }
    if (nu <= 0) return 0;
    g_SaIdxOk = 0;
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        if (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 20000 &&
            s_SnapUids[0] > 0 && OU_PtrAliveRaw(s_SnapUnits[0], s_SnapUids[0]))
            g_SaIdxOk = 1;
    }
    /* Owner 自校准 (fallback 用; 失败则本轮无强制攻击)。 */
    if (!SA_CalOwner(s_SnapUnits[0], nu > 1 ? s_SnapUnits[1] : 0, cur))
        g_SaOwnerOff = -1;
    /* 目标: 松开格直接点中的排首, 再沿轨迹各节点逐块环扫 (去重)。 */
    if (direct && IsPlausiblePtr(direct) && SA_IsTargetType(direct))
    {
        char tb[8];
        unsigned ti;
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        if (FN_TGT_OBJ(tb, direct))
        {
            s_SnapTgts[0] = direct;
            s_SnapTids[0] = *(int*)(tb + 0);
            nt = 1;
        }
    }
    for (j = 0; j < g_SaNodeCount && nt < SA_TGT_CAP; j++)
        SA_ScanBlock(g_SaNx[j], g_SaNy[j], s_SnapTgts, s_SnapTids, &nt);
    if (nt <= 0) return 0;
    s_SnapNu = nu;
    s_SnapNt = nt;
    return 1;
}

/* 最终任务 (光标优先, 无令走 fallback 规则表; actOut 透光标, 可空)。 */
static int SA_MissionFinal(void* u, void* t, void* house, int* actOut)
{
    int act = -1, ms, fb;
    ms = SA_UnitMission(u, t, &act);
    if (actOut) *actOut = act;
    if (ms < 0)
    {
        fb = SA_Fallback(u, t, act, house);
        if (fb >= 0) ms = fb;
    }
    return ms;
}

/* 双 sink: 0=发令队列 (终判), 1=预览线对 (预览, 只记线不下令)。 */
static int s_SaSinkPairs = 0;

/* ---------------- 规划线预览基础设施 ---------------- */

#define SA_PREV_CAP 128   /* 预览线上限 (多了看不清, 忠于可读性) */
#define SA_PREV_UNITS 64  /* 预览参与单位上限 (逐对 MouseOver, 控开销) */
#define SA_PREV_TGTS 96   /* 预览参与目标上限 (最近优先, 终判全量) */
static int s_PrevUx[SA_PREV_CAP], s_PrevUy[SA_PREV_CAP];
static int s_PrevTx[SA_PREV_CAP], s_PrevTy[SA_PREV_CAP];
static int s_PrevN = 0;
static int s_PrevRefPx = 0, s_PrevRefPy = 0, s_PrevRefCx = 0, s_PrevRefCy = 0;
static int s_PrevVLogged = 0;   /* 本轮已记 sa: v (TMP) */

/* leptons → 屏像素 (预览参考点换算, 忽略高度, 与方框同系)。 */
static void SA_ProjLep(int lx, int ly, int* px, int* py)
{
    int dx = lx - (s_PrevRefCx * 256 + 128);
    int dy = ly - (s_PrevRefCy * 256 + 128);
    *px = s_PrevRefPx + (dx * 30 - dy * 30) / 256;
    *py = s_PrevRefPy + (dx * 15 + dy * 15) / 256;
}

/* 存一对规划线端点 (双端取坐标, 任一失败则丢弃)。 */
static void SA_PrevStore(void* u, void* t)
{
    CoordStruct a, b;
    int ux, uy, tx, ty;
    if (s_PrevN >= SA_PREV_CAP) return;
    if (!OU_GetCoords(u, &a) || !OU_GetCoords(t, &b)) return;
    SA_ProjLep(a.X, a.Y, &ux, &uy);
    SA_ProjLep(b.X, b.Y, &tx, &ty);
    s_PrevUx[s_PrevN] = ux; s_PrevUy[s_PrevN] = uy;
    s_PrevTx[s_PrevN] = tx; s_PrevTy[s_PrevN] = ty;
    s_PrevN++;
}

/* 装令: 任务就绪则入队 (take 计数可空, fallback 计数可空)。
 * 回 1=装上 (发令或记线)。 */
static int SA_QueueOne(int i, int j, int ms,
                       int* takeOrNull, int* nFbA, int* nFbCE, int isFb)
{
    if (ms < 0) return 0;
    if (s_SaSinkPairs)
    {
        int before = s_PrevN;
        SA_PrevStore(s_SnapUnits[i], s_SnapTgts[j]);
        if (s_PrevN == before) return 0;   /* 坐标失败 */
        s_SnapBusy[i] = 1;
        if (takeOrNull) (*takeOrNull)++;
        return 1;
    }
    if (g_SaPendCount >= SA_PEND_CAP) return 0;
    g_SaPend[g_SaPendCount].unit = s_SnapUnits[i];
    g_SaPend[g_SaPendCount].uid = s_SnapUids[i];
    g_SaPend[g_SaPendCount].tgt = s_SnapTgts[j];
    g_SaPend[g_SaPendCount].tid = s_SnapTids[j];
    g_SaPend[g_SaPendCount].mission = ms;
    g_SaPendCount++;
    s_SnapBusy[i] = 1;
    if (takeOrNull) (*takeOrNull)++;
    if (isFb)
    {
        if (nFbA && ms == MISSION_ATTACK) (*nFbA)++;
        if (nFbCE && ms != MISSION_ATTACK) (*nFbCE)++;
    }
    return 1;
}

/* 装令 (首/次轮通用): 最终任务入队。回 1=装上。 */
static int SA_AssignOne(int i, int j, void* house,
                        int* takeOrNull, int* nFbA, int* nFbCE)
{
    int act = -1, ms, fb;
    ms = SA_UnitMission(s_SnapUnits[i], s_SnapTgts[j], &act);
    fb = (ms < 0);
    if (fb)
    {
        ms = SA_Fallback(s_SnapUnits[i], s_SnapTgts[j], act, house);
        if (ms < 0) return 0;
    }
    return SA_QueueOne(i, j, ms, takeOrNull, nFbA, nFbCE, fb);
}

/* 三轮分配 (终判与预览共用, sink 见 s_SaSinkPairs):
 * 第0轮进驻均分 (步兵×建筑→Capture/Sabotage, 轮转建筑均分, 不管 Squad);
 * 首轮每目标取 Squad 个; 次轮剩下找最近可处理目标加入。
 * nFbA/nFbCE 可空 (预览不计数)。 */
static void SA_Distribute(void* house, int* nFbA, int* nFbCE)
{
    int i, j, nu, nt;
    nu = s_SnapNu;
    nt = s_SnapNt;
    /* 第0轮: 进驻均分 (步兵×建筑→Capture/Sabotage, 轮转建筑均分)。 */
    {
        static int bidx[SA_TGT_CAP];
        int nb = 0, rot = 0;
        for (j = 0; j < nt && nb < SA_TGT_CAP; j++)
            if (OU_WhatAmI(s_SnapTgts[j]) == ABS_BUILDING) bidx[nb++] = j;
        if (nb > 0)
        {
            for (i = 0; i < nu; i++)
            {
                int k, ms;
                if (s_SnapBusy[i]) continue;
                if (OU_WhatAmI(s_SnapUnits[i]) != ABS_INFANTRY) continue;
                if (!OU_PtrAlive(s_SnapUnits[i], s_SnapUids[i], g_SaIdxOk)) continue;
                for (k = 0; k < nb; k++)
                {
                    j = bidx[(rot + k) % nb];
                    ms = SA_MissionFinal(s_SnapUnits[i], s_SnapTgts[j], house, 0);
                    if (ms != MISSION_CAPTURE && ms != MISSION_SABOTAGE) continue;
                    if (SA_QueueOne(i, j, ms, 0, nFbA, nFbCE, 1)) { rot++; break; }
                }
            }
        }
    }
    /* 首轮: 每目标取 Squad 个可处理单位 (满分流下个目标)。 */
    for (j = 0; j < nt; j++)
    {
        int take = 0;
        for (i = 0; i < nu && take < g_SaSquad; i++)
        {
            if (s_SnapBusy[i]) continue;
            if (!OU_PtrAlive(s_SnapUnits[i], s_SnapUids[i], g_SaIdxOk)) continue;
            SA_AssignOne(i, j, house, &take, nFbA, nFbCE);
        }
    }
    /* 次轮: 剩下的找最近可处理目标加入 (无事可做则待命, 不下移动令)。 */
    for (i = 0; i < nu; i++)
    {
        if (s_SnapBusy[i]) continue;
        if (!OU_PtrAlive(s_SnapUnits[i], s_SnapUids[i], g_SaIdxOk)) continue;
        for (j = 0; j < nt; j++)
        {
            if (SA_AssignOne(i, j, house, 0, nFbA, nFbCE)) break;
        }
    }
}

/* 决策下令: 快照单位逐个问 MouseOver, 无令走 fallback 规则表
 * (攻击/占领/进入), 分组发令排空。
 * 组合键还按着且有单位没分到活: 留待命等松手重判; 否则清旗清框。 */
static void SA_DecideFire(void)
{
    void* cur;
    int i, nu, nt, nFbA = 0, nFbCE = 0, leftover = 0;

    nu = s_SnapNu;
    nt = s_SnapNt;
    if (nu <= 0 || nt <= 0)
    { g_SaHaveSnapshot = 0; SA_BoxReset(); return; }
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur))
    { g_SaHaveSnapshot = 0; SA_BoxReset(); return; }
    g_SaPendCount = 0;
    /* TMP: 首目标×首单位最终任务 (shift/act/ms), 定完删 */
    {
        int da = -1;
        int dm = SA_MissionFinal(s_SnapUnits[0], s_SnapTgts[0], cur, &da);
        SA_Tmp("sa: d", OU_KeyDown(VK_SHIFT), da, dm);
    }
    SA_Distribute(cur, &nFbA, &nFbCE);
    for (i = 0; i < nu; i++)
    {
        if (!s_SnapBusy[i] &&
            OU_PtrAlive(s_SnapUnits[i], s_SnapUids[i], g_SaIdxOk))
        { leftover = 1; break; }
    }
    g_SaHouse = cur;
    g_SaHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    SA_Tmp("sa: p", g_SaPendCount, nu, nt);
    SA_Tmp("sa: f", nFbA, nFbCE, leftover);
    SA_Drain();
    if (leftover && SA_ComboHeld())
    {
        /* 组合键还按着: 留待命 (框保持), 松手重判剩下的。 */
        g_SaHaveSnapshot = 1;
        g_SaSnapAge = 0;
    }
    else
    {
        g_SaHaveSnapshot = 0;
        SA_BoxReset();
    }
}

/* 规划线预览基础设施 (静态量/换算/存线) 定义在快照区之前 (QueueOne 先用)。 */

static void SA_Preview(void)
{
    void* cur;
    int cx, cy, rpx, rpy;
    unsigned now;
    static unsigned s_LastMs = 0;
    static int s_LastCx = -9999, s_LastCy = -9999;

    /* 节流: 换格且距上次>80ms, 或超 400ms 刷一次 (目标会动)。 */
    now = GetTickCount();
    if (!OU_MouseCell(&cx, &cy) || !GtMouseGameXY(&rpx, &rpy)) return;
    if (now - s_LastMs < 80) return;
    if (cx == s_LastCx && cy == s_LastCy && now - s_LastMs < 400) return;
    s_LastMs = now;
    s_LastCx = cx; s_LastCy = cy;
    s_PrevRefPx = rpx; s_PrevRefPy = rpy;
    s_PrevRefCx = cx; s_PrevRefCy = cy;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) { s_PrevN = 0; return; }
    /* 整轨迹快照 (与终判同输入, 松手重扫一次, 这里只借来画线)。
     * 预览截断 (控开销: 前 64 单位/前 96 目标, 最近优先; 终判全量)。 */
    SA_Snapshot();
    if (s_SnapNu > SA_PREV_UNITS) s_SnapNu = SA_PREV_UNITS;
    if (s_SnapNt > SA_PREV_TGTS) s_SnapNt = SA_PREV_TGTS;
    s_PrevN = 0;
    s_SaSinkPairs = 1;
    SA_Distribute(cur, 0, 0);
    s_SaSinkPairs = 0;
    /* TMP: 本轮首记对数, 定完删 */
    if (!s_PrevVLogged && s_PrevN > 0)
    {
        s_PrevVLogged = 1;
        SA_Tmp("sa: v", s_PrevN, s_SnapNu, s_SnapNt);
    }
}

/* ---------------- 轮询 (按住采样, 松中键当场快照决策下令) ---------------- */

static void SA_BoxReset(void);
static void SA_BoxSample(void);

void SmartArea_Poll(void)
{
    int trigDown;
    int combo;

    SA_EnsureInit();
    trigDown = OU_KeyDown(g_SaHk.vk);
    combo = SA_ComboHeld() ? 1 : 0;
    if (g_SaPendCount > 0)
    { SA_Drain(); g_SaPrevTrig = trigDown; return; }
    if (!g_SaEnable) { g_SaPrevTrig = trigDown; return; }
    if (trigDown && !g_SaPrevTrig)
    {
        /* 按下: 新一轮 (旧待命/预览作废), 锚点入节点。 */
        int ax, ay;
        g_SaNodeCount = 0;
        g_SaShiftSeen = 0;
        g_SaHaveSnapshot = 0;
        s_SaSinkPairs = 0;
        s_PrevN = 0;
        s_PrevVLogged = 0;
        if (OU_MouseCell(&ax, &ay)) SA_NodePush(ax, ay);
        SA_BoxReset();
    }
    if (trigDown && combo)
    {
        /* 按住: 采样轨迹 + 采样框 + 规划线预览; 记组合成立过。 */
        int nx, ny;
        if (OU_MouseCell(&nx, &ny)) SA_NodePush(nx, ny);
        SA_BoxSample();
        g_SaShiftSeen = 1;
        SA_Preview();
    }
    else if (!trigDown && g_SaPrevTrig && g_SaShiftSeen &&
             !OU_KeyDown(VK_CONTROL) && !OU_KeyDown('Z'))
    {
        /* 松中键: 当场快照+决策+下令 (光标无令走 fallback 规则表;
         * 分不到的留待命, 松手重判); 规划线立刻消失。
         * Ctrl/Z 半路加入则丢弃。 */
        s_PrevN = 0;
        if (SA_Snapshot())
            SA_DecideFire();
    }
    g_SaPrevTrig = trigDown;
    /* 待命且组合键已松 (无 Ctrl/Z): 重判剩下的 (此时真光标)。
     * 超 300 轮还没松则丢弃, 防 stale 惊吓。 */
    if (g_SaHaveSnapshot && !trigDown)
    {
        if (++g_SaSnapAge > 300)
        {
            g_SaHaveSnapshot = 0;
            SA_BoxReset();
        }
        else if (!OU_KeyDown(VK_CONTROL) && !OU_KeyDown('Z') &&
                 !OU_ModsDown(g_SaHk.mods))
            SA_DecideFire();
    }
}

/* ---------------- 渲染期范围框 (与模糊同式, glue 在鼠标上) ---------------- */

static int g_SaBRx = 0, g_SaBRy = 0, g_SaBRcx = 0, g_SaBRcy = 0;
static int g_SaBHas = 0;

/* 命中标记静态量见快照区 (DecideFire 先用, 定义在前)。 */

static void SA_BoxReset(void)
{
    g_SaBHas = 0;
}

static void SA_BoxSample(void)
{
    int mx, my, cx, cy;
    if (!GtMouseGameXY(&mx, &my)) return;
    if (!OU_MouseCell(&cx, &cy)) return;
    g_SaBRx = mx; g_SaBRy = my; g_SaBRcx = cx; g_SaBRcy = cy;
    g_SaBHas = 1;
}

static unsigned char* s_SaLocked = 0;
static int s_SaW = 0, s_SaH = 0, s_SaPitch = 0;

static void SA_Plot(int x, int y, unsigned a)
{
    unsigned short* dstRow;
    if (x < 0 || y < 0 || x >= s_SaW || y >= s_SaH) return;
    dstRow = (unsigned short*)(s_SaLocked + y * s_SaPitch);
    dstRow[x] = Blend16(dstRow[x], 255, 255, 255, a);
}

static void SA_Line(int x0, int y0, int x1, int y1, unsigned a)
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
        SA_Plot(x, y, a);
        SA_Plot(x, y - 1, a);
        SA_Plot(x, y + 1, a);
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

static void SA_Mark(int cx, int cy, int m, unsigned a)
{
    int k, j;
    for (k = -m; k <= m; k++)
        for (j = -m; j <= m; j++)
            SA_Plot(cx + k, cy + j, a);
}

/* 细线 (Bresenham 单像素, 规划线用, 自带裁剪)。 */
static void SA_ThinLine(int x0, int y0, int x1, int y1,
                        unsigned r, unsigned g, unsigned b, unsigned a)
{
    int dx, dy, sx, sy, err, e2, x, y, n;
    unsigned short* dstRow;
    dx = x1 - x0; dy = y1 - y0;
    sx = (dx >= 0) ? 1 : -1;
    sy = (dy >= 0) ? 1 : -1;
    dx = dx >= 0 ? dx : -dx; dy = dy >= 0 ? dy : -dy;
    err = dx - dy;
    x = x0; y = y0;
    for (n = 0; n < 4096; n++)
    {
        if (x >= 0 && y >= 0 && x < s_SaW && y < s_SaH)
        {
            dstRow = (unsigned short*)(s_SaLocked + y * s_SaPitch);
            dstRow[x] = Blend16(dstRow[x], r, g, b, a);
        }
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

static void SA_BoxCorner(int ccx, int ccy, int* px, int* py)
{
    int dx = ccx - g_SaBRcx, dy = ccy - g_SaBRcy;
    *px = g_SaBRx + dx * 30 - dy * 30;
    *py = g_SaBRy + dx * 15 + dy * 15;
}

void SmartArea_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* vt;
    SA_LockFn lockFn;
    SA_UnlockFn unlockFn;
    unsigned char* locked;
    int lo, hi, x0, y0, x1, y1, x2, y2, x3, y3;
    extern int g_Calibrated;
    int showBox, showLines;
    if (!g_SaEnable || (!g_SaBHas && !g_SaHaveSnapshot && s_PrevN <= 0)) return;
    /* 快照待命时框保持 (MMB 已松, 认领必掉, 靠待命旗续画)。
     * 规划线只在按住时画 (松手即消)。 */
    showBox = (g_SaBHas || g_SaHaveSnapshot) &&
              (g_SaHaveSnapshot || SmartArea_ClaimsMiddle());
    showLines = (s_PrevN > 0 && SmartArea_ClaimsMiddle()) ? 1 : 0;
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    lockFn = (SA_LockFn)(*(void**)(vt + VT_SA_LOCK));
    unlockFn = (SA_UnlockFn)(*(void**)(vt + VT_SA_UNLOCK));
    if (!lockFn || !unlockFn) return;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return;
    s_SaLocked = locked; s_SaW = w; s_SaH = h; s_SaPitch = pitch;
    if (showBox)
    {
        lo = -(g_SaRange / 2);
        hi = lo + g_SaRange - 1;
        SA_BoxCorner(g_SaBRcx + lo, g_SaBRcy + lo, &x0, &y0);
        SA_BoxCorner(g_SaBRcx + hi + 1, g_SaBRcy + lo, &x1, &y1);
        SA_BoxCorner(g_SaBRcx + hi + 1, g_SaBRcy + hi + 1, &x2, &y2);
        SA_BoxCorner(g_SaBRcx + lo, g_SaBRcy + hi + 1, &x3, &y3);
        SA_Line(x0, y0, x1, y1, 230);
        SA_Line(x1, y1, x2, y2, 230);
        SA_Line(x2, y2, x3, y3, 230);
        SA_Line(x3, y3, x0, y0, 230);
        SA_Mark(x0, y0, 1, 230);
        SA_Mark(x1, y1, 1, 230);
        SA_Mark(x2, y2, 1, 230);
        SA_Mark(x3, y3, 1, 230);
    }
    /* 规划线 (绿色细线, 单位→分到的目标, 只在按住时存在)。 */
    if (showLines)
    {
        int k;
        for (k = 0; k < s_PrevN; k++)
            SA_ThinLine(s_PrevUx[k], s_PrevUy[k],
                        s_PrevTx[k], s_PrevTy[k], 0, 255, 0, 255);
    }
    s_SaLocked = 0;
    unlockFn(surf, NULL);
}
