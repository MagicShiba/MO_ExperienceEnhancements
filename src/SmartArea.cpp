/* SmartArea.cpp - 组合中键范围智能下令 (Ctrl+中键模糊攻击的自适应版)。
 *
 * 触发: Hotkey (默认 Shift+Middle, 主键须是 Middle, 可配单键如 X1)
 * 点按/划线, 松开即执行 (与模糊同手势)。按住显示生效块菱形框。
 * 点按=单块, 划线=沿轨迹各节点逐块由近及远挑目标 (上限 64 节点),
 * 松开格直接点中的对象排首位。
 * 决策口径与裸中键同一套 (DecideAction/MouseOver 同函数), 差的是输入:
 * Shift 按住时部分目标回 ToggleSelect(8, 加选语义), 真光标被盖住 —— 不管
 * Hotkey 配没配 Shift, 物理 Shift 按住都盖光标, 故待命看物理脏键
 * (SA_DirtyHeld: 配置修饰/附加键, 或物理 Shift, 任一按住即脏)。
 * 无令时走 fallback 规则表 (细节见 SA_Fallback): 本家目标只允许步兵进自家
 * 运输机, 其余跳过 (绝不误击本家); 非本家 (盟友视同模糊) 载具/飞机一律攻击,
 * 步兵对非建筑攻击, 对建筑只由工程师 (ENGINEER/S/Y/F) 占领, 非工程师留待命,
 * 脏键松开后按真光标重判 (无修饰单键不脏即不等)。
 * 分组 (SquadSize, 默认 4, 与模糊同义): 每目标先分 Squad 个, 剩下的找最近
 * 可处理目标加入 (全员有事做); 进驻不进这轮, 按建筑轮转均分 (不管 SquadSize)。
 * 只下 攻击/进入/进驻/占领 四类令 (本功能不下移动令, 分不到目标的原地待命)。
 * 下令走标准 0x04 MegaMission (攻击 Target=对象, 进入类 Destination=对象,
 * 与手点同形; 物理 Z 按住时 IsPlanningEvent=1 排队, 按序追加路径点不覆盖 ——
 * 点 B 再点 A 再点 C 得 B→A→C; 不按 Z 则直接下令 IsPlanningEvent=0)。
 * OutList 满 drip 跨帧续发。
 * 规划线 (按住时): 预览=快照+分配, 与终判同一套输入同一套规则 (终判松手
 * 重扫一次); 预览截断前 64 单位/前 96 目标 (终判全量), 限 128 线, 换格、
 * 节点数变或 400ms 刷一次, 80ms 节流, 松手即消。端点存 leptons 世界坐标
 * (单位中心含高度, 见 OrderUtil.h OU_PrevLine), 绘制期经引擎投影。
 * 仲裁: 须无 Ctrl (含 Ctrl 归模糊); Z 不禁 (Z+裸中键仍归巡逻圈)。
 * 原生 Shift (多选/排路点) 只在左/右键上, 中键无原生功能, 无冲突。
 * 关或热键坏则不认领, 组合中键回落裸中键; 待命框超 300 轮丢弃。
 *
 * 联机: 只发标准事件, 本地只读 + 标准事件入队, 不写模拟内存。
 * 构建见 build-fast.bat (同一 DLL)。轮询挂 PowerDrawBoth (见 MOExperience.cpp)。
 * 日志: 仅启动行。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"
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

/* 认领中键: 开且组合全按住 (含主键), 且无 Ctrl (归模糊);
 * Z 不禁 (用户习惯按住 Z, 巡逻圈见认领会自己让路)。
 * 注意: 松开瞬间主键已抬起, 认领必掉; 松开判定另用 SA_ComboHeld
 * (不含主键, 与模糊同式), 否则松开边永远不执行。 */
int SmartArea_ClaimsMiddle(void)
{
    SA_EnsureInit();
    if (!g_SaEnable) return 0;
    if (!OU_KeyDown(g_SaHk.vk)) return 0;
    if (!OU_ModsDown(g_SaHk.mods) || !OU_ExtraDown(&g_SaHk)) return 0;
    if (OU_KeyDown(VK_CONTROL)) return 0;
    return 1;
}

/* 组合保持 (松开边用): 开且修饰/附加键按住、无 Ctrl, 不看主键 (Z 不禁)。 */
static int SA_ComboHeld(void)
{
    SA_EnsureInit();
    if (!g_SaEnable) return 0;
    if (!OU_ModsDown(g_SaHk.mods) || !OU_ExtraDown(&g_SaHk)) return 0;
    if (OU_KeyDown(VK_CONTROL)) return 0;
    return 1;
}

/* 脏键保持 (待命用): 配置的修饰/附加键真被按住, 或物理 Shift 按住。
 * 注意与 ComboHeld 的区别: ModsDown(0)/ExtraDown(空) 恒真 (空真),
 * 快捷键无修饰 (如单 X1) 时 ComboHeld 恒成立, 不能拿它判断"松手";
 * 物理 Shift 污染光标 (ToggleSelect), 不管配没配置都必须等它松开。
 * Ctrl/Z 不在这里 (仲裁键, 出现即整单丢弃, 不待命)。 */
static int SA_DirtyHeld(void)
{
    SA_EnsureInit();
    if (!g_SaEnable) return 0;
    if (g_SaHk.mods && OU_ModsDown(g_SaHk.mods)) return 1;
    if (g_SaHk.nExtra > 0 && OU_ExtraDown(&g_SaHk)) return 1;
    if (OU_KeyDown(VK_SHIFT)) return 1;
    return 0;
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

/* ---------------- 类型自校准 (SmartOrder 同式, 工程师判定用) ---------------- */

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

/* Action(光标) → Mission: 与 SmartOrder 同表 (攻击1/进入7/占领8/进驻17);
 * 移动/未知回 -1 (本功能不下移动令)。
 * 选择类 (Select7/ToggleSelect8, 如坦克悬停自家建筑) 无对应订单, 跳过:
 * 左键等价动作是选中, 下令侧只能待命。 */
/* 敌意类光标 (左键点上去就是打, 含 Damage28/Eaten10/Detonate55/56):
 * 一律按攻击 (Damage28 实测: 悬停某类建筑回 28, 不映射则跳过)。
 * 例外见 AssignOne 工程师修正 (工程师对非本家建筑转占领, 没枪佯攻无用)。 */
static int SA_MissionFor(int targetWhat, int action)
{
    if (action == ACTION_ATTACK || action == 62 || action == 63 ||
        action == 64 || action == 71 || action == 53 ||
        action == 10 || action == 28 || action == 55 || action == 56)
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

/* ---------------- 下令 (MegaMission 形状同 SmartOrder/FuzzyAttack; 唯一区别是排队) ---------------- */

static int SA_FireAttack(int houseIdx, int srcID, unsigned char srcRtti,
                         int tgtID, unsigned char tgtRtti,
                         void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_ATTACK,
            tgtID, (unsigned)tgtRtti, 0, 0, 0, 0);
    /* 按住 Z = 路径点(排队, 按序追加不覆盖); 不按 Z 直接下令 (与手点同形)。 */
    ev[OFF_PLANFLAG] = OU_KeyDown('Z') ? 1 : 0;
    return OT_Fire(ev, unit, uid, idxOk);
}

static int SA_FireDest(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstID, unsigned char dstRtti, int mission,
                       void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, mission,
            0, 0, dstID, (unsigned)dstRtti, 0, 0);
    /* 按住 Z = 路径点(排队, 按序追加不覆盖); 不按 Z 直接下令 (与手点同形)。 */
    ev[OFF_PLANFLAG] = OU_KeyDown('Z') ? 1 : 0;
    return OT_Fire(ev, unit, uid, idxOk);
}

#define SA_PEND_CAP 1024   /* 待发上限 (超限进 OT 队列分帧, 与 OT_QCAP 对齐) */
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
            if (!SA_FireAttack(houseIdx, srcID, srcR, tgtID, tgtR,
                                 e->unit, e->uid, g_SaIdxOk)) return;
        }
        else
        {
            if (!SA_FireDest(houseIdx, srcID, srcR, tgtID, tgtR, e->mission,
                              e->unit, e->uid, g_SaIdxOk)) return;
        }
        SA_ShiftDown();
        sent++;
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

#define SA_UNIT_CAP 1024   /* 快照单位上限 (超限进 OT 队列分帧, 与 OT_QCAP 对齐) */

static void SA_BoxReset(void);

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

/* 规划线缓冲 (端点 leptons 世界坐标, 见 OrderUtil.h OU_PrevLine)。
 * 预览线上限 128 (多了看不清, 忠于可读性)。 */
#define SA_PREV_UNITS 64  /* 预览参与单位上限 (逐对 MouseOver, 控开销) */
#define SA_PREV_TGTS 96   /* 预览参与目标上限 (最近优先, 终判全量) */
static OU_PrevLine s_PrevBuf[OU_PREV_CAP];
static int s_PrevN = 0;

/* 装令: 任务就绪则入队 (发令) 或记线 (预览), take 计数可空。
 * 回 1=装上。 */
static int SA_QueueOne(int i, int j, int ms, int* takeOrNull)
{
    if (ms < 0) return 0;
    if (s_SaSinkPairs)
    {
        if (!OU_PrevPushObjs(s_PrevBuf, &s_PrevN, s_SnapUnits[i], s_SnapTgts[j]))
            return 0;   /* 满或坐标失败 */
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
    return 1;
}

/* 装令 (首/次轮通用): 最终任务入队。回 1=装上。 */
static int SA_AssignOne(int i, int j, void* house, int* takeOrNull)
{
    int act = -1, ms;
    ms = SA_UnitMission(s_SnapUnits[i], s_SnapTgts[j], &act);
    if (ms < 0)
    {
        ms = SA_Fallback(s_SnapUnits[i], s_SnapTgts[j], act, house);
        if (ms < 0) return 0;
    }
    /* 工程师修正: Damage28 光标映射是攻击, 但工程师没枪, 佯攻等于哑火;
     * 工程师对非本家建筑应占领。其它单位/目标不动。 */
    if (ms == MISSION_ATTACK && act == 28 &&
        OU_WhatAmI(s_SnapUnits[i]) == ABS_INFANTRY &&
        OU_WhatAmI(s_SnapTgts[j]) == ABS_BUILDING &&
        house && g_SaOwnerOff >= 0 && SA_IsEngineer(s_SnapUnits[i]))
    {
        void* oh = *(void**)((char*)s_SnapTgts[j] + (unsigned)g_SaOwnerOff);
        if (oh && IsPlausiblePtr(oh) && oh != house)
            ms = MISSION_CAPTURE;
    }
    return SA_QueueOne(i, j, ms, takeOrNull);
}

/* 三轮分配 (终判与预览共用, sink 见 s_SaSinkPairs):
 * 第0轮进驻均分 (步兵×建筑→Capture/Sabotage, 轮转建筑均分, 不管 Squad);
 * 首轮每目标取 Squad 个; 次轮剩下找最近可处理目标加入。 */
static void SA_Distribute(void* house)
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
                    if (SA_QueueOne(i, j, ms, 0)) { rot++; break; }
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
            SA_AssignOne(i, j, house, &take);
        }
    }
    /* 次轮: 剩下的找最近可处理目标加入 (无事可做则待命, 不下移动令)。 */
    for (i = 0; i < nu; i++)
    {
        if (s_SnapBusy[i]) continue;
        if (!OU_PtrAlive(s_SnapUnits[i], s_SnapUids[i], g_SaIdxOk)) continue;
        for (j = 0; j < nt; j++)
        {
            if (SA_AssignOne(i, j, house, 0)) break;
        }
    }
}

/* 决策下令: 快照单位逐个问 MouseOver, 无令走 fallback 规则表
 * (攻击/占领/进入), 分组发令排空。
 * 组合键还按着且有单位没分到活: 留待命等松手重判; 否则清旗清框。 */
static void SA_DecideFire(void)
{
    void* cur;
    int i, nu, nt, leftover = 0;

    nu = s_SnapNu;
    nt = s_SnapNt;
    if (nu <= 0 || nt <= 0)
    { g_SaHaveSnapshot = 0; SA_BoxReset(); return; }
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur))
    { g_SaHaveSnapshot = 0; SA_BoxReset(); return; }
    g_SaPendCount = 0;
    SA_Distribute(cur);
    for (i = 0; i < nu; i++)
    {
        if (!s_SnapBusy[i] &&
            OU_PtrAlive(s_SnapUnits[i], s_SnapUids[i], g_SaIdxOk))
        { leftover = 1; break; }
    }
    g_SaHouse = cur;
    g_SaHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    SA_Drain();
    if (leftover && SA_DirtyHeld())
    {
        /* 脏键还按着 (配置修饰/附加键, 或物理 Shift): 留待命 (框保持),
         * 等干净再重判剩下的。无修饰单键 (如 X1) 且 Shift 没按:
         * 脏恒假, 直接清 (重判结果不变, 不空转)。 */
        g_SaHaveSnapshot = 1;
        g_SaSnapAge = 0;
    }
    else
    {
        g_SaHaveSnapshot = 0;
        SA_BoxReset();
    }
}

static void SA_Preview(void)
{
    void* cur;
    int cx, cy;
    static unsigned s_LastMs = 0;
    static int s_LastCx = -9999, s_LastCy = -9999, s_LastNc = -1;

    /* 节流: 换格/节点数变且距上次>80ms, 或超 400ms 刷一次 (目标会动)。 */
    if (!OU_MouseCell(&cx, &cy)) return;
    if (!OU_PrevThrottle(&s_LastMs, &s_LastCx, &s_LastCy, &s_LastNc,
                         cx, cy, g_SaNodeCount, 80, 400)) return;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) { s_PrevN = 0; return; }
    /* 整轨迹快照 (与终判同输入, 松手重扫一次, 这里只借来画线)。
     * 预览截断 (控开销: 前 64 单位/前 96 目标, 最近优先; 终判全量)。 */
    SA_Snapshot();
    if (s_SnapNu > SA_PREV_UNITS) s_SnapNu = SA_PREV_UNITS;
    if (s_SnapNt > SA_PREV_TGTS) s_SnapNt = SA_PREV_TGTS;
    s_PrevN = 0;
    s_SaSinkPairs = 1;
    SA_Distribute(cur);
    s_SaSinkPairs = 0;
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
             !OU_KeyDown(VK_CONTROL))
    {
        /* 松开: 当场快照+决策+下令 (光标无令走 fallback 规则表;
         * 分不到的留待命, 松手重判); 规划线立刻消失。
         * Ctrl 半路加入则丢弃 (归模糊); Z 不禁。 */
        s_PrevN = 0;
        if (SA_Snapshot())
            SA_DecideFire();
    }
    g_SaPrevTrig = trigDown;
    /* 待命且脏键已松 (配置修饰/附加键与物理 Shift 全抬起, 无 Ctrl; Z 不禁):
     * 重判剩下的 (此时真光标)。无修饰单键 (如 X1) 不脏即不等。
     * 超 300 轮仍不松则丢弃, 防 stale 惊吓。 */
    if (g_SaHaveSnapshot && !trigDown)
    {
        if (++g_SaSnapAge > 300)
        {
            g_SaHaveSnapshot = 0;
            SA_BoxReset();
        }
        else if (!OU_KeyDown(VK_CONTROL) && !SA_DirtyHeld())
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
    /* 规划线 (绿色细线, 单位中心→分到的目标中心, 含高度, 只在按住时存在)。 */
    if (showLines)
        OU_PrevDraw(s_PrevBuf, s_PrevN, locked, w, h, pitch, 0, 255, 0, 255, 1);
    s_SaLocked = 0;
    unlockFn(surf, NULL);
}
