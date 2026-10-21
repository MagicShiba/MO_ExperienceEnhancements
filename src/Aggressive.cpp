/* Aggressive.cpp - 侵略模式。
 *
 * Alt+A (Hotkey 可改) 把当前选中的作战单位 (Unit/Infantry/Aircraft) 加入/移出
 * "侵略标记"集合 (全体已在集合内则整体移出, 否则整体加入)。已标记的单位处于
 * 闲置 (Mission=Guard/Area_Guard/Stop) 时, 每隔 Interval 帧扫一次: 对警戒半径内的
 * 敌方建筑发一条与鼠标左键同形的标准事件 (经 OT_Fire / OutList 广播), 并在其上方
 * 画 icon_aggressive 图像 (内嵌 PCX, 原版 16 位 blit)。
 *
 * 目标/订单取"鼠标对应的事件" (与左键点同一目标同判):
 *   - 目标只认**敌方**建筑, 忽略中立 (House->Type->MultiplayPassive, 与 SmartArea 同式)。
 *   - 订单 = 复刻引擎左键点击的真实派发链 (见下 AG_UnitMission): 固有光标
 *     (OU_MouseOverObjectNative, ignoreForce=1) → 类内重映射 → FootClass 的
 *     Action→Mission 表。不做单位类型特判 (工程师占领、劫持等一律按实际左键),
 *     所以工程师自动占领、作战单位自动攻击, 打不了的目标不下单。
 *
 * 联机 (AGENTS.md 铁律): 不写任何模拟内存, 不直调改模拟的引擎函数。
 *   - 标记集合是本机 UI 状态 (同 MindSelect 的选中), 各客户端各自维护。
 *   - 唯一的模拟变更 = 往 OutList 塞标准攻击/占领事件; 各端执行同一份原版代码。
 *   - 其余全只读 (GetGuardRange / GetCoords / MouseOver 判定都是纯读)。
 *
 * 只读偏移 (反汇编实证, 同 RepairAll/FuzzyAttack):
 *   ObjectClass::IsAlive 0x90 / InLimbo 0x81 / Health 0x6C
 *   TechnoClass::Owner 0x21C
 *   BuildingClass::Array 0xA8EB40 (DynamicVectorClass: Items@+4 / Count@+16)
 *   HouseClass::ArrayIndex (CurrentPlayer+0x30)
 *   Mission 0xAC (虚表槽 0x184)      Frame 0xA8ED84
 *
 * 引擎调用:
 *   TechnoClass::GetGuardRange(control=1) @ 0x707E60 (thiscall, 纯读): 返回
 *     area-guard 半径 = (GuardRange, 未设则最大武器射程) x2; 本模块 /2 得真射程
 *     (入口必须取 0x707E60, 原因见 notes/侵略模式.md)。
 *   TargetClass 构造器 FN_TGT_OBJ、MegaMission FN_MEGA 见 OrderUtil.h。
 *
 * 构建见 build-fast.bat (32 位 mingw, 无 CRT, 与 MOExperience 同一 DLL)。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"
#include "PcxImage.h"
#include "Aggressive.h"

#define AG_SECTION "Aggressive"

/* ---------------- 只读地址 ---------------- */

#define ADDR_BLD_ARR   0xA8EB40u
#define ADDR_FRAME     0xA8ED84u
#define ARR_ITEMS(base) (*(void***)((base) + 4u))
#define ARR_COUNT(base) (*(int*)((base) + 16u))

#define OFF_ISALIVE 0x90u
#define OFF_INLIMBO 0x81u
#define OFF_HEALTH  0x6Cu
#define OFF_OWNER   0x21Cu

#define VT_GETMISSION (0x184u / 4u)

/* MISSION_ATTACK 已在 OrderUtil.h 定义; 这里只补闲置判定用到的三个。 */
#define MISSION_GUARD  5
#define MISSION_AREAGUARD 11
#define MISSION_STOP   13

typedef int __attribute__((thiscall)) (*Fn_GetMission)(void* self);
typedef int __attribute__((thiscall)) (*Fn_GetGuardRange)(void* self, int control);
typedef int __attribute__((thiscall)) (*Fn_GetWeaponRange)(void* self, int idxWeapon);

#define FN_GETGUARDRANGE ((Fn_GetGuardRange)(void*)0x707E60u)

/* ObjectClass::GetWeaponRange(int idxWeapon) 虚表槽 0x168 (0=主武器, 1=副武器)。
 * 全部 TechnoClass 系 vtable 该槽均指向 0x7012C0, 无派生覆盖。 */
#define VT_GETWEAPONRANGE (0x168u / 4u)

/* ---------------- 配置 ---------------- */

static int g_AgInit = 0;
static int g_AgEnable = 1;
static OU_Hotkey g_AgHk;         /* Hotkey, 默认 Alt+A */
static int g_AgOffX = 0;         /* 图标相对单位中心的像素偏移 */
static int g_AgOffY = -20;
static int g_AgInterval = 15;    /* 扫描间隔 (逻辑帧) */
static int g_AgCooldown = 45;    /* 同一单位两次发令最小间隔 (逻辑帧) */
static int g_AgRangePct = 100;   /* 攻击半径 = 真射程 x RangePercent% */

static int g_AgPrev = 0;
static void* g_AgHouse = 0;
static int s_AgLastFrame = -1;
static int s_AgLastScan = -100000;

static void AG_EnsureInit(void)
{
    if (g_AgInit) return;
    g_AgInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_AgEnable = OU_ReadHotkeySingle(AG_SECTION, "Hotkey", "Alt+A", &g_AgHk);
    g_AgOffX = GetPrivateProfileIntA(AG_SECTION, "IconOffsetX", 0, g_IniPath);
    g_AgOffY = GetPrivateProfileIntA(AG_SECTION, "IconOffsetY", -20, g_IniPath);
    if (g_AgOffX < -400) g_AgOffX = -400;
    if (g_AgOffX > 400) g_AgOffX = 400;
    if (g_AgOffY < -400) g_AgOffY = -400;
    if (g_AgOffY > 400) g_AgOffY = 400;
    g_AgInterval = GetPrivateProfileIntA(AG_SECTION, "Interval", 15, g_IniPath);
    if (g_AgInterval < 1) g_AgInterval = 1;
    if (g_AgInterval > 300) g_AgInterval = 300;
    g_AgCooldown = GetPrivateProfileIntA(AG_SECTION, "Cooldown", 45, g_IniPath);
    if (g_AgCooldown < 0) g_AgCooldown = 0;
    if (g_AgCooldown > 1800) g_AgCooldown = 1800;
    g_AgRangePct = GetPrivateProfileIntA(AG_SECTION, "RangePercent", 100, g_IniPath);
    if (g_AgRangePct < 10) g_AgRangePct = 10;
    if (g_AgRangePct > 400) g_AgRangePct = 400;
    if (!g_AgEnable) LogLine("aggressive: OFF");
    else LogLine("aggressive: ON (Alt+A; enemy building in guard range; mouse-mapped order)");
}

/* ---------------- 小工具 ---------------- */

static int AG_IsAttacker(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY || w == ABS_AIRCRAFT) ? 1 : 0;
}

/* 对象 Fetch_ID (身份; 失败回 0)。rttiOut 透出 RTTI 字节 (可空)。 */
static int AG_FetchId(void* p, unsigned char* rttiOut)
{
    char tb[8];
    unsigned i;
    for (i = 0; i < 8; i++) tb[i] = 0;
    if (!p || !FN_TGT_OBJ(tb, p)) return 0;
    if (rttiOut) *rttiOut = (unsigned char)tb[4];
    return *(int*)(tb + 0);
}

/* 当前任务 (虚表槽 0x184; 失败回 -1)。 */
static int AG_Mission(void* u)
{
    void** vt;
    Fn_GetMission fn;
    if (!u || !IsPlausiblePtr(u)) return -1;
    vt = *(void***)u;
    if (!vt || !IsPlausiblePtr(vt)) return -1;
    fn = (Fn_GetMission)vt[VT_GETMISSION];
    if (!fn || !IsPlausiblePtr((const void*)fn)) return -1;
    return fn(u);
}

/* 主/副武器射程 (轻子; idx 0=主武器 1=副武器)。虚表槽 0x168; 失败回 0。 */
static int AG_WeaponRange(void* u, int idx)
{
    void** vt;
    Fn_GetWeaponRange fn;
    if (!u || !IsPlausiblePtr(u)) return 0;
    vt = *(void***)u;
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    fn = (Fn_GetWeaponRange)vt[VT_GETWEAPONRANGE];
    if (!fn || !IsPlausiblePtr((const void*)fn)) return 0;
    return fn(u, idx);
}

/* 目标搜索半径 (轻子): 取类型的 GuardRange 原值 (GetGuardRange(0)，不可用时回 0)，
 * 未设则回退到主/副武器射程较大者。不能用 GetGuardRange(1)：它会把射程加倍后
 * 以 4096 轻子(16 格)封顶，无 GuardRange 的远射程单位会被严重截断。
 * 最后乘 RangePercent% (默认 100)。非法回 0。 */
static int AG_GuardRange(void* u)
{
    int r;
    if (!u || !IsPlausiblePtr(u)) return 0;
    r = FN_GETGUARDRANGE(u, 0);
    if (r <= 0)
    {
        int a = AG_WeaponRange(u, 0);
        int b = AG_WeaponRange(u, 1);
        r = (a > b) ? a : b;
    }
    if (r <= 0) return 0;
    if (g_AgRangePct != 100) r = r * g_AgRangePct / 100;
    if (r < 128 || r > 65536) return 0;
    return r;
}

/* ---------------- 鼠标对应的事件: 单位固有光标 → 订单 ----------------
 *
 * 复刻 gamemd 左键点击的真实派发链 (反汇编实证), 不做兵种特判:
 *
 *   ① 取光标 OU_MouseOverObjectNative = vtbl+0x74 = Class::GetActionOnObject
 *      (InfantryClass 0x51E3B0 / UnitClass 0x73FD50 ...), 正是引擎点同一目标时调的那个。
 *
 *   ② 类内重映射 (InfantryClass::ObjectClickedAction 0x51F190 / UnitClass 0x738890):
 *      工程师对血厚敌楼的固有光标是 Damage(28), 引擎在这一层翻成 Capture(9)
 *      再派发 —— 所以必须照做, 否则会误发主武器攻击 (Mission=Attack)。
 *
 *   ③ FootClass::ObjectClickedAction (0x4D74E0) Action→Mission 表。
 *
 * 三者都封在 OrderUtil 的 OU_RemapAction / OU_NativeMission 里 (SmartOrder/
 * FuzzyAttack/AttackMove 共用同一份, 避免逐模块维护)。详见 notes/侵略模式.md。
 * 事件布局: Attack 放 Target=目标, 进驻/占领/破坏 放 Destination=目标。 */

/* 单位 u 对建筑 t 的订单 = 实际左键会下的 Mission (ignoreForce=1, 无视修饰键)。
 * 本模块不下移动令 (光标为 Move 时不作为目标)。 */
static int AG_UnitMission(void* u, void* t)
{
    int ms = OU_NativeMission(u, t);
    return (ms == MISSION_MOVE) ? -1 : ms;
}

/* ---------------- 标记集合 (本机 UI 状态) ---------------- */

#define AG_TRK_CAP 1024

typedef struct { void* p; int uid; int lastFire; } AGTrk;
static AGTrk s_AgTrk[AG_TRK_CAP];
static int s_AgTrkN = 0;
static int s_AgIdxOk = 0;   /* Fetch_ID 索引自检通过 (OU_PtrAlive 才严格) */

/* 只读字节判据 (IsAlive/InLimbo/Health; 不碰 Fetch_ID 索引)。 */
static int AG_AliveRaw(void* p)
{
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (!((unsigned char*)p)[OFF_ISALIVE]) return 0;
    if (((unsigned char*)p)[OFF_INLIMBO]) return 0;
    if (*(int*)((char*)p + OFF_HEALTH) <= 0) return 0;
    return 1;
}

/* 还活着且可显示 (只读字节判据 + 索引校验 fail-open)。 */
static int AG_Alive(void* p, int uid)
{
    if (!AG_AliveRaw(p)) return 0;
    if (!OU_PtrAlive(p, uid, s_AgIdxOk)) return 0;
    return 1;
}

static int AG_FindIdx(int uid)
{
    int i;
    if (uid <= 0) return -1;
    for (i = 0; i < s_AgTrkN; i++)
        if (s_AgTrk[i].uid == uid) return i;
    return -1;
}

static void AG_Add(void* p, int uid)
{
    if (uid <= 0) return;
    if (AG_FindIdx(uid) >= 0) return;
    if (s_AgTrkN >= AG_TRK_CAP) return;
    s_AgTrk[s_AgTrkN].p = p;
    s_AgTrk[s_AgTrkN].uid = uid;
    s_AgTrk[s_AgTrkN].lastFire = -100000;
    s_AgTrkN++;
}

static void AG_Remove(int uid)
{
    int i = AG_FindIdx(uid);
    if (i < 0) return;
    for (; i + 1 < s_AgTrkN; i++) s_AgTrk[i] = s_AgTrk[i + 1];
    s_AgTrkN--;
}

static void AG_Prune(void)
{
    int i, k = 0;
    for (i = 0; i < s_AgTrkN; i++)
    {
        if (!AG_Alive(s_AgTrk[i].p, s_AgTrk[i].uid)) continue;
        if (k != i) s_AgTrk[k] = s_AgTrk[i];
        k++;
    }
    s_AgTrkN = k;
}

/* 热键边沿: 全体已在集合内 -> 整体移出; 否则把能加的加进来。 */
static void AG_ToggleSelect(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    void** items = *(void***)ADDR_SELOBJ_ITEMS;
    int selN = *(int*)ADDR_SELOBJ_COUNT;
    static void* sel[AG_TRK_CAP];
    static int sid[AG_TRK_CAP];
    int n = 0, i, all = 1;
    if (!cur || !IsPlausiblePtr(cur)) return;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return;
    for (i = 0; i < selN && n < AG_TRK_CAP; i++)
    {
        void* o = items[i];
        int uid;
        if (!o || !IsPlausiblePtr(o)) continue;
        if (!AG_IsAttacker(o)) continue;
        if (*(void**)((char*)o + OFF_OWNER) != cur) continue;   /* 只认本家 */
        uid = AG_FetchId(o, 0);
        if (uid <= 0) continue;
        sel[n] = o; sid[n] = uid; n++;
    }
    if (n <= 0) return;
    for (i = 0; i < n; i++)
        if (AG_FindIdx(sid[i]) < 0) { all = 0; break; }
    if (all) { for (i = 0; i < n; i++) AG_Remove(sid[i]); }
    else { for (i = 0; i < n; i++) AG_Add(sel[i], sid[i]); }
}

/* ---------------- 敌方建筑候选 (每轮建一次, 摊薄 GetCoords) ---------------- */

#define AG_BLD_CAP 4096

typedef struct { void* p; int id; int x, y; } AGBld;
static AGBld s_AgBld[AG_BLD_CAP];
static int s_AgBldN = 0;

/* 中立阵营判定 (只读 House->Type(+0x34)->MultiplayPassive(+0x1A6); SmartArea 同式)。
 * 自检: 本家不可能是被动中立。Mode: 0=未定, 1=启用过滤, 2=停用 (回"非本家即目标")。
 * 读不到一律按非中立 (不误伤候选取舍)。全程只读。 */
static int g_AgNeutMode = 0;
static int AG_HouseNeutralRaw(void* h)
{
    void* t;
    if (!h || !IsPlausiblePtr(h)) return 0;
    t = *(void**)((char*)h + 0x34u);
    if (!IsPlausiblePtr(t)) return 0;
    return (*(unsigned char*)((char*)t + 0x1A6u)) ? 1 : 0;
}

static void* s_AgPrevItems = 0;
static int s_AgPrevCount = 0;
static int s_AgStable = 0;

static void AG_Observe(void)
{
    void** items = ARR_ITEMS(ADDR_BLD_ARR);
    int count = ARR_COUNT(ADDR_BLD_ARR);
    if (!items || !IsPlausiblePtr(items) || count <= 0 || count > AG_BLD_CAP)
    { s_AgPrevItems = 0; s_AgPrevCount = 0; s_AgStable = 0; return; }
    if (items == s_AgPrevItems && count == s_AgPrevCount)
    { if (s_AgStable < 2) s_AgStable++; return; }
    s_AgPrevItems = items; s_AgPrevCount = count; s_AgStable = 1;
}

/* 收集存活、敌方 (忽略中立) 的建筑坐标 (数组半初始化垃圾会致 fault, 见稳定门)。 */
static void AG_BuildCands(void* cur)
{
    void** items = ARR_ITEMS(ADDR_BLD_ARR);
    int count = ARR_COUNT(ADDR_BLD_ARR);
    int i;
    s_AgBldN = 0;
    if (!cur || !items || !IsPlausiblePtr(items)) return;
    if (count <= 0 || count > AG_BLD_CAP) return;
    /* 本家自检一次 (本家不可能是被动中立)。 */
    if (g_AgNeutMode == 0)
        g_AgNeutMode = AG_HouseNeutralRaw(cur) ? 2 : 1;
    for (i = 0; i < count && s_AgBldN < AG_BLD_CAP; i++)
    {
        void* b = items[i];
        void* own;
        CoordStruct cb;
        if (!b || !IsPlausiblePtr(b)) continue;
        if (!AG_AliveRaw(b)) continue;
        if (OU_WhatAmI(b) != ABS_BUILDING) continue;
        own = *(void**)((char*)b + OFF_OWNER);
        if (!own || !IsPlausiblePtr(own) || own == cur) continue;   /* 敌/中立 */
        if (g_AgNeutMode == 1 && AG_HouseNeutralRaw(own)) continue;  /* 忽略中立 */
        if (!OU_GetCoords(b, &cb)) continue;
        s_AgBld[s_AgBldN].p = b;
        s_AgBld[s_AgBldN].id = AG_FetchId(b, 0);
        s_AgBld[s_AgBldN].x = cb.X;
        s_AgBld[s_AgBldN].y = cb.Y;
        s_AgBldN++;
    }
}

/* 警戒范围内最近的、本单位真能对其下令 (鼠标对应订单) 的敌方建筑
 * (圆心距离, 只算水平面)。outMs = 该订单 (攻击/占领/...)。 */
static void* AG_FindTarget(void* u, int range, int* outId, int* outMs)
{
    CoordStruct cu;
    long long rr, bestD = 0;
    void* best = 0;
    int bestId = 0, bestMs = -1, i;
    *outId = 0;
    *outMs = -1;
    if (!OU_GetCoords(u, &cu)) return 0;
    rr = (long long)range * (long long)range;
    for (i = 0; i < s_AgBldN; i++)
    {
        long long dx, dy, d2;
        int ms;
        dx = (long long)s_AgBld[i].x - (long long)cu.X;
        dy = (long long)s_AgBld[i].y - (long long)cu.Y;
        d2 = dx * dx + dy * dy;
        if (d2 > rr) continue;
        if (best && d2 >= bestD) continue;
        ms = AG_UnitMission(u, s_AgBld[i].p);   /* 只读光标 = 与左键同判 */
        if (ms < 0) continue;
        best = s_AgBld[i].p;
        bestD = d2;
        bestId = s_AgBld[i].id;
        bestMs = ms;
    }
    if (best) { *outId = bestId; *outMs = bestMs; }
    return best;
}

/* ---------------- 待发队列 (保序滴发) ---------------- */

#define AG_PEND_CAP 1024
#define AG_DRAIN_PER_POLL 128

typedef struct { void* unit; int uid; void* foe; int fid; int mission; } AGPend;
static AGPend g_AgPend[AG_PEND_CAP];
static int g_AgPendN = 0;

/* 发一条对象令事件 (MegaMission, 直接下令 flag=0)。
 * 攻击: Target=敌对象, Destination=空; 进驻/占领/破坏: Target=空, Destination=敌对象
 * (与鼠标点建筑的真实事件布局同形, 见 SmartOrder/SmartArea)。 */
static int AG_Fire(int houseIdx, int srcID, unsigned char srcRtti, int mission,
                   int tgtID, unsigned char tgtRtti,
                   int dstID, unsigned char dstRtti,
                   void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, mission,
            tgtID, (unsigned)tgtRtti, dstID, (unsigned)dstRtti, 0, 0);
    ev[OFF_PLANFLAG] = 0;
    return OT_Fire(ev, unit, uid, idxOk);
}

static void AG_ShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_AgPendN; k++) g_AgPend[k] = g_AgPend[k + 1];
    g_AgPendN--;
}

static void AG_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx, sent = 0;
    if (!cur || cur != g_AgHouse) { g_AgPendN = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    while (g_AgPendN > 0 && sent < AG_DRAIN_PER_POLL)
    {
        AGPend* e = &g_AgPend[0];
        int srcID, tgtID, atk;
        unsigned char srcR = 0, tgtR = 0;
        if (!AG_Alive(e->unit, e->uid)) { AG_ShiftDown(); continue; }
        if (!AG_IsAttacker(e->unit)) { AG_ShiftDown(); continue; }
        if (!OU_PtrAlive(e->foe, e->fid, s_AgIdxOk)) { AG_ShiftDown(); continue; }
        /* 发令前重算订单 (只读光标复核): 状态变了 (打不着/改占领) 就丢, 不猜着下单。 */
        if (AG_UnitMission(e->unit, e->foe) != e->mission)
        { AG_ShiftDown(); continue; }
        srcID = AG_FetchId(e->unit, &srcR);
        tgtID = AG_FetchId(e->foe, &tgtR);
        if (srcID <= 0 || tgtID <= 0) { AG_ShiftDown(); continue; }
        /* 攻击放 Target 槽; 进驻/占领/破坏放 Destination 槽 (与引擎同形)。 */
        atk = (e->mission == MISSION_ATTACK);
        if (!AG_Fire(houseIdx, srcID, srcR, e->mission,
                     atk ? tgtID : 0, atk ? tgtR : 0,
                     atk ? 0 : tgtID, atk ? 0 : tgtR,
                     e->unit, e->uid, s_AgIdxOk))
            return;   /* OutList 满: 下轮继续 */
        AG_ShiftDown();
        sent++;
    }
}

/* ---------------- 扫描 ---------------- */

static void AG_Scan(int fr)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int i;
    if (s_AgTrkN <= 0 || s_AgStable < 2) return;
    if (!cur || !IsPlausiblePtr(cur)) return;
    AG_BuildCands(cur);
    if (s_AgBldN <= 0) return;
    for (i = 0; i < s_AgTrkN && g_AgPendN < AG_PEND_CAP; i++)
    {
        void* u = s_AgTrk[i].p;
        int uid = s_AgTrk[i].uid;
        int m, range, tid = 0, tms = -1;
        void* tgt;
        if (!AG_Alive(u, uid)) continue;
        m = AG_Mission(u);
        if (m != MISSION_GUARD && m != MISSION_AREAGUARD && m != MISSION_STOP)
            continue;   /* 闲置 (守卫/区域守卫/停止) 才管 */
        if (fr - s_AgTrk[i].lastFire < g_AgCooldown) continue;
        range = AG_GuardRange(u);
        if (range <= 0) continue;
        tgt = AG_FindTarget(u, range, &tid, &tms);
        if (!tgt || tid <= 0) continue;   /* 有目标即有订单 (bestMs 恒 >= 0) */
        g_AgPend[g_AgPendN].unit = u;
        g_AgPend[g_AgPendN].uid = uid;
        g_AgPend[g_AgPendN].foe = tgt;
        g_AgPend[g_AgPendN].fid = tid;
        g_AgPend[g_AgPendN].mission = tms;
        g_AgPendN++;
        s_AgTrk[i].lastFire = fr;
    }
    /* Fetch_ID 索引自检: 用队首实测一遍, 失败则 OU_PtrAlive 全 fail-open。 */
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        s_AgIdxOk = 0;
        if (g_AgPendN > 0 && base && IsPlausiblePtr(base) &&
            cnt > 0 && cnt <= 200000 &&
            OU_PtrAliveRaw(g_AgPend[0].unit, g_AgPend[0].uid))
            s_AgIdxOk = 1;
    }
    AG_Drain();
}

/* ---------------- 每循环轮询 (挂 PowerDrawBoth) ---------------- */

void Aggressive_Poll(void)
{
    int fr, combo;
    AG_EnsureInit();
    if (!g_AgEnable) { g_AgPrev = 0; return; }
    if (!GtGameFocused()) { g_AgPrev = 0; return; }
    fr = *(int*)ADDR_FRAME;
    if (fr == s_AgLastFrame) return;   /* 0x4F42F0 一帧跑多次, 自门控 */
    s_AgLastFrame = fr;
    /* 换家 (观战切位/换图): 清标记。 */
    {
        void* cur = *(void**)ADDR_CURPLAYER;
        if (cur != g_AgHouse)
        {
            s_AgTrkN = 0; g_AgPendN = 0; g_AgHouse = cur;
            s_AgLastScan = -100000;
        }
    }
    combo = OU_HotkeyDown(&g_AgHk) ? 1 : 0;
    if (combo && !g_AgPrev) AG_ToggleSelect();
    g_AgPrev = combo;
    AG_Observe();
    if (g_AgPendN > 0) { AG_Drain(); return; }
    AG_Prune();
    if (fr - s_AgLastScan >= g_AgInterval)   /* 首轮 -100000, 立即扫 */
    {
        s_AgLastScan = fr;
        AG_Scan(fr);
    }
}

/* ---------------- 渲染期图标 (MOE_DrawUILayers 内调) ---------------- */

void Aggressive_Draw(void* surf, int w, int h, int pitch)
{
    PcxImage img;
    int ox, oy, i;
    AG_EnsureInit();
    if (!g_AgEnable || s_AgTrkN <= 0) return;
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    /* 首帧真正加载; 之后 PcxGet 命中引擎面表 (不为每帧读盘/重建路径)。
     * 每帧现取是因为引擎缓存重载后 BSurface 指针会变, 一张图多个标记共用。 */
    if (!PcxImage_Load(&img, "icon_aggressive.pcx", 2)) return;
    ox = g_AgOffX - img.width / 2;
    oy = g_AgOffY - img.height / 2;
    for (i = 0; i < s_AgTrkN; i++)
    {
        CoordStruct c;
        int px, py;
        if (!AG_Alive(s_AgTrk[i].p, s_AgTrk[i].uid)) continue;
        if (!OU_GetCoords(s_AgTrk[i].p, &c)) continue;
        if (!OU_ProjWorld(c.X, c.Y, c.Z, &px, &py)) continue;
        /* 完全在面外不画: PcxImage_Draw 会把越界坐标夹进面内, 屏外单位
         * 否则会以图标贴在屏幕边缘。 */
        px += ox;
        py += oy;
        if (px + img.width <= 0 || py + img.height <= 0 ||
            px >= w || py >= h) continue;
        PcxImage_Draw(&img, surf, px, py);
    }
}
