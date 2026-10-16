/* SmartArea.cpp - 组合中键范围智能下令。
 *
 * 触发: Hotkey (默认 Shift+Middle, 可配单键如 X1), 点按/划线, 松开即执行。
 * 点按=单块, 划线=沿轨迹各节点逐块环扫 (去重, 松开格点中排首, 上限 64 节点)。
 * 决策: 攻击/进入/进驻/占领才下单, 移动/未知跳过 (不下移动令)。
 * 光标查询走 OU_MouseOverObjectNative (ignoreForce=1): 本功能热键含 Shift,
 * 用 0 会被物理 Shift 污染成 ToggleSelect(8) 使主判定全废, 必须取固有光标
 * (与裸中键同语义, 但无视当前修饰键)。仍有待命机制 (见 SA_DirtyHeld):
 * 分不到活的单位留旗, 脏键 (配置修饰/附加键或物理 Shift) 松开后重判。
 * 光标无令走 fallback 规则表 (见 SA_Fallback): 本家只允许步兵进自家运输机,
 * 其余跳过; 非本家载具/飞机/步兵对非建筑强攻; 步兵对建筑仅工程师占领。
 * 分组 (SquadSize, 默认 4): 每目标首轮 Squad 个, 剩下找最近可处理目标加入。
 * 进楼单按归属与类别限数:
 * 自家楼: 工程师修掉血楼只派 1 人 (CAPTURE 进驻即修, 人消耗), 满血跳过;
 *   士兵进驻不限数不查血。
 * 非自家楼: 工程师占无主/中立科技 (TL≠-1) 按楼限 1;
 *   工程师对中立民房 (TL=-1, 不可夺无归属变更) 只做维修 (掉血 1 人/满血跳过);
 *   士兵进驻中立民房轮转均分; 敌楼强占与拆除 Squad。
 * 非自家载具 Enter (占空车) 按目标限 1, 偷车 Capture 不分归属限 1;
 * 进自家运输机装人不限。
 * 工程师判定: InfantryType+0xEC3 (Engineer=yes 本体) 或 GRepair/Repair(29/11)
 * 光标; Capture(9) 不能自证 (士兵对空房同样拿 9, 占领与进驻按单位区分)。
 * 中立判定: House->Type->MultiplayPassive (+0x34/+0x1A6, 只读);
 * 民房判定: TechnoType->TechLevel (+0x634, TL=-1, 只读); 皆有启动自检,
 * 失败回退旧行为。
 * 只下标准 0x04 MegaMission (攻击 Target=对象, 进入类 Destination=对象);
 * 物理 Z 按住 IsPlanningEvent=1 排队追加, 否则直接下令。OutList 满跨帧续发。
 * 预览=快照+分配与终判同一套 (双 sink, 终判松手重扫一次); 预览截断
 * 64 单位/96 目标/128 线, 换格或 400ms 刷, 80ms 节流, 松手即消。
 * 仲裁: 须无 Ctrl (归模糊); Z 不禁。关或热键坏不认领, 回落裸中键。
 * 联机: 只发标准事件, 本地只读不写模拟内存。
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
    /* 报实际生效热键 (vk=5 是 X1 侧键/4 是中键; mods bit0=Ctrl,bit1=Shift,bit2=Alt)。 */
    {
        char buf[80], b1[16];
        unsigned q = 0;
        q = AppendCStr(buf, q, "smartarea: ON hk-vk=", sizeof(buf));
        UtoA((unsigned)g_SaHk.vk, b1);
        q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " mods=", sizeof(buf));
        UtoA((unsigned)g_SaHk.mods, b1);
        q = AppendCStr(buf, q, b1, sizeof(buf));
    (void)q;
    LogLine(buf);
    }
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

/* 工程师判定 (读 InfantryType+0xEC3, 即 Engineer=yes 本体; ID 表保留作交叉)。
 * 类型指针走 SA_TypeOf 步兵槽。 */
static int SA_IsEngUnit(void* u)
{
    void* tp;
    if (!u || !IsPlausiblePtr(u)) return 0;
    if (OU_WhatAmI(u) != ABS_INFANTRY) return 0;
    if (SA_IsEngineer(u)) return 1;
    tp = SA_TypeOf(u, ABS_INFANTRY);
    if (!tp || !IsPlausiblePtr(tp)) return 0;
    return (*(unsigned char*)((char*)tp + 0xEC3u)) ? 1 : 0;
}

/* Action(光标) → Mission: 与 SmartOrder 同表 (攻击1/进入7/占领8/进驻17);
 * 移动/未知回 -1 (本功能不下移动令)。
 * 选择类 (Select7/ToggleSelect8, 如坦克悬停自家建筑) 无对应订单, 跳过:
 * 左键等价动作是选中, 下令侧只能待命。 */
/* 敌意类光标 (左键点上去就是打, 含 Damage28/Eaten10/Detonate55/56):
 * 一律按攻击。例外见 AssignOne 工程师修正 (工程师对非本家建筑转占领)。 */
static int SA_MissionFor(int targetWhat, int action)
{
    if (action == ACTION_ATTACK || action == 62 || action == 63 ||
        action == 64 || action == 71 || action == 53 ||
        action == 10 || action == 28 || action == 55 || action == 56)
        return MISSION_ATTACK;
    if (action == ACTION_ENTER)
        return (targetWhat == ABS_BUILDING) ? MISSION_SABOTAGE : MISSION_ENTER;
    /* Repair(11)/GRepair(29) 点建筑 = 进驻修/占 (与手点进楼同形,
     * 步兵分发转成 Capture 动作再发 Capture(8) 任务)。
     * 碾磨机回收是对载具, 这里只管建筑。 */
    if ((action == 11 || action == 29) && targetWhat == ABS_BUILDING)
        return MISSION_CAPTURE;
    if (action == ACTION_SABOTAGE)
        return MISSION_SABOTAGE;
    if (action == ACTION_GUARDAREA && targetWhat == ABS_BUILDING)
        return MISSION_SABOTAGE;
    if (action == ACTION_CAPTURE)
        return MISSION_CAPTURE;
    return -1;
}

/* 单位 u 对目标 t 的任务: 只读 MouseOver, 非对象令回 -1。
 * actOut 透出原始光标 (fallback 判定用, 可空)。
 * 必须走 Native (ignoreForce=1): 本功能热键含 Shift (默认 Shift+Middle),
 * 用 0 会被物理 Shift 污染成 ToggleSelect(8) (Ctrl 污染成强攻),
 * 真光标失真 → 主判定路径全废, 只剩 fallback, 表现就是"没反应"。
 * 用 1 取单位固有光标 (与裸中键同形), 修饰键不再影响判定。 */
static int SA_UnitMission(void* u, void* t, int* actOut)
{
    int act, ms;
    if (actOut) *actOut = -1;
    if (!SA_IsAttacker(u) || !SA_IsTargetType(t)) return -1;
    act = OU_MouseOverObjectNative(u, t);
    if (actOut) *actOut = act;
    if (act < 0) return -1;
    ms = SA_MissionFor(OU_WhatAmI(t), act);
    if (ms != MISSION_ATTACK && ms != MISSION_ENTER &&
        ms != MISSION_SABOTAGE && ms != MISSION_CAPTURE)
        return -1;
    return ms;
}

/* fallback 规则表 (主判定无令时, act<0 才进; 正常走 Native 光标):
 * 本家目标: 步兵对自家载具按进入 (运输机收人); 其余跳过 (绝不误击本家)。
 * 非本家目标: 载具/飞机一律攻击; 步兵对非建筑攻击;
 * 步兵对建筑: 工程师按占领, 非工程师跳过留待命。
 * 强制攻击须有肯定攻击光标 (OU_CanAttack); 调用失败时不猜, 留待命重判。
 * 回 mission, 无规则回 -1。 */
static int SA_IsEngUnit(void* u);   /* 前置声明 (定义在类型自校准后) */
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
    {
        if (uw == ABS_INFANTRY && tw == ABS_UNIT) return MISSION_ENTER;
        /* 自家建筑: 工程师进驻即修 (人消耗)。Shift 盖住光标时用
         * OU_CanEnter/OU_CanGRepair (ignoreForce=1) 复核。任务发 CAPTURE,
         * 掉血与按楼限 1 人由 AssignOne 把关。 */
        if (tw == ABS_BUILDING && uw == ABS_INFANTRY &&
            (OU_CanEnter(u, t) || OU_CanGRepair(u, t)))
            return MISSION_CAPTURE;
        return -1;
    }
    if (uw == ABS_UNIT || uw == ABS_AIRCRAFT)
        return OU_CanAttack(u, t) ? MISSION_ATTACK : -1;
    if (tw != ABS_BUILDING)
        return OU_CanAttack(u, t) ? MISSION_ATTACK : -1;
    return SA_IsEngUnit(u) ? MISSION_CAPTURE : -1;
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

/* ---------------- 工程师修自家建筑 (进驻即修, 按楼限 1 人) ----------------
 * 只修掉血的 (满血不浪费人)。工程师判定见 SA_IsEngUnit。 */

#define SA_TGT_CAP 256
#define SA_UNIT_CAP 1024   /* 快照单位上限 (超限进 OT 队列分帧, 与 OT_QCAP 对齐) */

/* 快照: 单位 + 目标存静态 (找目标与修饰键无关, 决策见 DecideFire)。
 * 返回 1=有可处理快照。 */
static void* s_SnapUnits[SA_UNIT_CAP];
static int s_SnapUids[SA_UNIT_CAP];
static int s_SnapBusy[SA_UNIT_CAP];
static int s_SnapNu = 0;
static void* s_SnapTgts[SA_TGT_CAP];
static int s_SnapTids[SA_TGT_CAP];
static int s_SnapNt = 0;
static unsigned char s_SnapEnterTaken[SA_TGT_CAP]; /* 占车已派标记 (按目标限1个) */
static unsigned char s_SnapRepairSent[SA_TGT_CAP]; /* 按目标单派标记 (修楼/中立科技/民房预轮) */

#define SA_OFF_HEALTH        0x6Cu   /* ObjectClass::Health */
#define SA_OFF_BLD_TYPE      0x520u  /* BuildingClass::Type */
#define SA_OFF_TYPE_STRENGTH 0xA0u   /* ObjectTypeClass::Strength */

/* 建筑是否掉血 (0<hp<max; 指针/类型有一项读不到按没掉血处理, 绝不浪费工程师)。 */
static int SA_BuildingDamaged(void* b)
{
    void* type;
    int hp, str;
    if (!b || !IsPlausiblePtr(b)) return 0;
    if (OU_WhatAmI(b) != ABS_BUILDING) return 0;
    hp = *(int*)((char*)b + SA_OFF_HEALTH);
    if (hp <= 0) return 0;
    type = *(void**)((char*)b + SA_OFF_BLD_TYPE);
    if (!type || !IsPlausiblePtr(type)) return 0;
    str = *(int*)((char*)type + SA_OFF_TYPE_STRENGTH);
    if (str <= 0 || str > 1000000) return 0;
    return (hp < str) ? 1 : 0;
}

/* ---------------- 找目标 (环扫 + 链走, 不分敌我) ---------------- */

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

static void SA_BoxReset(void);

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

/* 目标是否归本家 (Owner 自校准偏移; 读不到按非本家处理)。 */
static int SA_OwnedByMe(void* t, void* house)
{
    void* oh;
    if (!t || !IsPlausiblePtr(t) || !house || g_SaOwnerOff < 0) return 0;
    oh = *(void**)((char*)t + (unsigned)g_SaOwnerOff);
    if (!oh || !IsPlausiblePtr(oh)) return 0;
    return (oh == house) ? 1 : 0;
}

/* 科技等级 (只读 TechnoType->TechLevel +0x634)。
 * 用途: 区分中立科技 (TL=11) 与平民楼房 (TL=-1), 是规则属性非白名单。
 * 自检: 类型数组里 GAPOWR 读出 1 且 CANEWY08 读出 -1 才启用,
 * 失败则民房规则停用 (中立一律按楼限 1)。全程只读。 */
#define SA_OFF_TL_TECHLEVEL 0x634u
static int g_SaTlOk = 0;
static int g_SaTlBad = 0;
static int SA_TechLevelOf(void* type)
{
    if (!type || !IsPlausiblePtr(type)) return 9999;
    return *(int*)((char*)type + SA_OFF_TL_TECHLEVEL);
}
static void* SA_FindTypeById(const char* want)
{
    void** items = 0;
    int cnt, i;
    char id[24];
    cnt = SA_TypeCount(&items);
    if (cnt <= 0 || !items) return 0;
    for (i = 0; i < cnt; i++)
    {
        int c;
        if (!items[i] || !IsPlausiblePtr(items[i])) continue;
        for (c = 0; c < 24; c++) id[c] = 0;
        if (!SA_TypeID(items[i], id) || !id[0]) continue;
        if (SA_Streq(id, want)) return items[i];
    }
    return 0;
}
static void SA_TechLevelSelfCheck(void)
{
    void* p1;
    void* p2;
    if (g_SaTlOk || g_SaTlBad) return;
    p1 = SA_FindTypeById("GAPOWR");
    p2 = SA_FindTypeById("CANEWY08");
    if (!p1 || !p2)
    {
        /* 数组就绪但找不到则判坏 (防每轮空扫); 否则下次事件再试。 */
        void** items = 0;
        if (SA_TypeCount(&items) > 0 && items) g_SaTlBad = 1;
        return;
    }
    if (SA_TechLevelOf(p1) == 1 && SA_TechLevelOf(p2) == -1) g_SaTlOk = 1;
    else g_SaTlBad = 1;
}
/* 平民楼房 (TL=-1 的建筑类型; 科技类 TL=11)。
 * 实例→类型用 SA_OFF_BLD_TYPE (+0x520)。 */
static int SA_IsCivHouse(void* t)
{
    void* tp;
    if (!g_SaTlOk || g_SaTlBad) return 0;
    if (!t || !IsPlausiblePtr(t)) return 0;
    tp = *(void**)((char*)t + SA_OFF_BLD_TYPE);
    if (!tp || !IsPlausiblePtr(tp)) return 0;
    return (SA_TechLevelOf(tp) == -1) ? 1 : 0;
}

/* 中立阵营判定 (只读 House->Type(+0x34)->MultiplayPassive(+0x1A6))。
 * 自检: 本家不可能是被动中立, 读出 1 则永久停用 (回敌方 Squad 路径)。
 * 无主不算中立; 读不到按非中立。全程只读。 */
static int g_SaNeutOk = 0;
static int g_SaNeutBad = 0;
static int SA_HouseNeutralRaw(void* h)
{
    void* t;
    if (!h || !IsPlausiblePtr(h)) return 0;
    t = *(void**)((char*)h + 0x34u);
    if (!IsPlausiblePtr(t)) return 0;
    return (*(unsigned char*)((char*)t + 0x1A6u)) ? 1 : 0;
}
/* 本家自检一次 (Distribute 入口调, house 即本家)。 */
static void SA_NeutSelfCheck(void* house)
{
    if (g_SaNeutOk || g_SaNeutBad) return;
    if (SA_HouseNeutralRaw(house)) g_SaNeutBad = 1;
    else g_SaNeutOk = 1;
}
/* 目标归中立阵营。 */
static int SA_TargetNeutral(void* t)
{
    void* oh;
    if (!g_SaNeutOk || g_SaNeutBad) return 0;
    if (!t || !IsPlausiblePtr(t) || g_SaOwnerOff < 0) return 0;
    oh = *(void**)((char*)t + (unsigned)g_SaOwnerOff);
    if (!oh || !IsPlausiblePtr(oh)) return 0;
    return SA_HouseNeutralRaw(oh);
}

/* 分配期缓存 (一次 Distribute 内有效): AssignOne 逐对必问 MouseOver,
 * 归属/类别/掉血/工程师标记一次算好, 不逐对重复读。 */
static unsigned char s_SnapUnitEng[SA_UNIT_CAP];
static unsigned char s_SnapTgtOwn[SA_TGT_CAP];
static unsigned char s_SnapTgtNull[SA_TGT_CAP];
static unsigned char s_SnapTgtNeut[SA_TGT_CAP];
static unsigned char s_SnapTgtHouse[SA_TGT_CAP];
static unsigned char s_SnapTgtDmg[SA_TGT_CAP];
static void SA_CachePrep(void* house)
{
    int i, j;
    for (i = 0; i < s_SnapNu; i++)
        s_SnapUnitEng[i] = SA_IsEngUnit(s_SnapUnits[i]) ? 1 : 0;
    for (j = 0; j < s_SnapNt; j++)
    {
        void* t = s_SnapTgts[j];
        void* oh = 0;
        int own = SA_OwnedByMe(t, house);
        if (g_SaOwnerOff >= 0 && t && IsPlausiblePtr(t))
            oh = *(void**)((char*)t + (unsigned)g_SaOwnerOff);
        s_SnapTgtOwn[j] = own ? 1 : 0;
        s_SnapTgtNull[j] = (g_SaOwnerOff >= 0 && oh == 0) ? 1 : 0;
        s_SnapTgtNeut[j] = (!own && SA_TargetNeutral(t)) ? 1 : 0;
        s_SnapTgtHouse[j] = SA_IsCivHouse(t) ? 1 : 0;
        s_SnapTgtDmg[j] = SA_BuildingDamaged(t) ? 1 : 0;
    }
}

/* 按楼限 1: 该目标已派过则跳过, 否则装令并标记。回装令结果。 */
static int SA_RepairCap(int i, int j, int ms, int* takeOrNull)
{
    int r;
    if (s_SnapRepairSent[j]) return 0;
    r = SA_QueueOne(i, j, ms, takeOrNull);
    if (!r) return 0;
    s_SnapRepairSent[j] = 1;
    return 1;
}

/* 装令 (首/次轮通用): 最终任务入队。回 1=装上。
 * 归属/类别/掉血/工程师标记走 Distribute 头缓存, 这里只读缓存
 * (MouseOver 仍逐对问, 是真开销)。 */
static int SA_AssignOne(int i, int j, void* house, int* takeOrNull)
{
    int act = -1, ms;
    int isEng;
    ms = SA_UnitMission(s_SnapUnits[i], s_SnapTgts[j], &act);
    if (ms < 0)
    {
        ms = SA_Fallback(s_SnapUnits[i], s_SnapTgts[j], act, house);
        if (ms < 0) return 0;
    }
    isEng = (act == 29 || act == 11) ? 1 : s_SnapUnitEng[i];
    /* 工程师修正: Damage28 映射是攻击, 但工程师没枪, 非本家转占领。 */
    if (ms == MISSION_ATTACK && act == 28 &&
        OU_WhatAmI(s_SnapTgts[j]) == ABS_BUILDING &&
        house && g_SaOwnerOff >= 0 && isEng &&
        !s_SnapTgtOwn[j] && !s_SnapTgtNull[j])
        ms = MISSION_CAPTURE;
    /* 自家楼分两路: 工程师修掉血楼只派 1 人 (CAPTURE 进驻即修, 人消耗),
     * 满血跳过; 士兵进驻不限数不查血。敌楼占领不管。 */
    if ((ms == MISSION_SABOTAGE || ms == MISSION_ENTER || ms == MISSION_CAPTURE) &&
        OU_WhatAmI(s_SnapTgts[j]) == ABS_BUILDING &&
        s_SnapTgtOwn[j])
    {
        if (isEng)
        {
            ms = MISSION_CAPTURE;
            if (!s_SnapTgtDmg[j]) return 0;
            return SA_RepairCap(i, j, ms, takeOrNull);
        }
        return SA_QueueOne(i, j, ms, takeOrNull);
    }
    /* 非自家建筑进入占领类, 按归属与类别限数:
     * 工程师 + 无主/中立科技 → 按楼限 1;
     * 工程师 + 无主/中立民房 (不可夺无归属变更) → 按修楼限数
     *   (掉血 1 人, 满血跳过);
     * 工程师 + 敌方 → Squad; 士兵进驻 → 均分预轮处理, 这里跳过;
     * 士兵攻击拆除 → 不管。读不到按敌方路径。 */
    if ((ms == MISSION_SABOTAGE || ms == MISSION_ENTER || ms == MISSION_CAPTURE) &&
        OU_WhatAmI(s_SnapTgts[j]) == ABS_BUILDING &&
        !s_SnapTgtOwn[j])
    {
        int isNull = s_SnapTgtNull[j];
        if (isEng && (isNull || s_SnapTgtNeut[j]))
        {
            if (!isNull && s_SnapTgtHouse[j])
            {
                ms = MISSION_CAPTURE;
                if (!s_SnapTgtDmg[j]) return 0;
            }
            return SA_RepairCap(i, j, ms, takeOrNull);
        }
        if (!isEng && !isNull && s_SnapTgtNeut[j] && s_SnapTgtHouse[j] &&
            (ms == MISSION_CAPTURE || ms == MISSION_SABOTAGE))
            return 0;
    }
    /* 占车/偷车按目标限 1 个 (进自家运输机装人不限)。 */
    if (OU_WhatAmI(s_SnapTgts[j]) == ABS_UNIT &&
        (ms == MISSION_CAPTURE ||
         (ms == MISSION_ENTER && !s_SnapTgtOwn[j])))
    {
        int r;
        if (s_SnapEnterTaken[j]) return 0;
        r = SA_QueueOne(i, j, ms, takeOrNull);
        if (!r) return 0;
        s_SnapEnterTaken[j] = 1;
        return 1;
    }
    return SA_QueueOne(i, j, ms, takeOrNull);
}

/* 士兵进驻口径 (民房均分预轮用): 非工程师 + 真进驻光标
 * (Capture/Sabotage), 回 mission; 其它回 -1。归属走缓存。 */
static int SA_SolGarrMs(int i, int j, void* house)
{
    int act = -1, ms;
    if (!house) return -1;
    if (OU_WhatAmI(s_SnapTgts[j]) != ABS_BUILDING) return -1;
    if (s_SnapTgtOwn[j]) return -1;
    ms = SA_UnitMission(s_SnapUnits[i], s_SnapTgts[j], &act);
    if (ms != MISSION_CAPTURE && ms != MISSION_SABOTAGE) return -1;
    if (act == 29 || act == 11 || s_SnapUnitEng[i]) return -1;
    return ms;
}

/* 预轮+两轮分配 (终判与预览共用):
 * 预轮士兵进驻中立民房均分 (轮转, 人用完为止);
 * 首轮每目标取 Squad 个; 次轮剩下找最近可处理目标加入。 */
static void SA_Distribute(void* house)
{
    int i, j, nu, nt;
    nu = s_SnapNu;
    nt = s_SnapNt;
    SA_NeutSelfCheck(house);
    SA_TechLevelSelfCheck();
    SA_CachePrep(house);
    for (j = 0; j < SA_TGT_CAP; j++) s_SnapEnterTaken[j] = 0;
    for (j = 0; j < SA_TGT_CAP; j++) s_SnapRepairSent[j] = 0;
    /* 预轮: 士兵进驻中立民房轮转均分 (人用完为止)。
     * 首轮跳单靠 AssignOne 条件判定 (中立民房进驻类直接跳过), 不另标记。 */
    if (g_SaTlOk && !g_SaTlBad && nu > 0 && nt > 0)
    {
        static int hidx[SA_TGT_CAP];
        int nh = 0, k, ei = 0, progress;
        for (j = 0; j < nt && nh < SA_TGT_CAP; j++)
        {
            if (OU_WhatAmI(s_SnapTgts[j]) != ABS_BUILDING) continue;
            if (s_SnapTgtOwn[j]) continue;
            if (!s_SnapTgtNeut[j]) continue;
            if (!s_SnapTgtHouse[j]) continue;
            hidx[nh++] = j;
        }
        if (nh > 0)
        {
            do {
                int n;
                progress = 0;
                for (k = 0; k < nh; k++)
                {
                    int ms;
                    j = hidx[k];
                    for (n = 0; n < nu; n++)
                    {
                        i = (ei + n) % nu;
                        if (s_SnapBusy[i]) continue;
                        if (!OU_PtrAlive(s_SnapUnits[i], s_SnapUids[i], g_SaIdxOk)) continue;
                        ms = SA_SolGarrMs(i, j, house);
                        if (ms < 0) continue;
                        if (!SA_QueueOne(i, j, ms, 0)) continue;
                        ei = (i + 1) % nu;
                        progress = 1;
                        break;
                    }
                }
            } while (progress);
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
