/* 侵略模式: 热键切换本机标记, 空闲单位定期寻找警戒范围内可下令目标。
 * 订单沿用引擎光标判定和 Action→Mission 映射, 攻击使用 Target,
 * 进入/占领/破坏使用 Destination。忽略被动中立, Target 可限为建筑。
 * 无敌目标 (Immune=yes / Armor=special) 与有价值的科技建筑默认不当目标。
 * 联机只读模拟状态, 所有命令经 OU_FireMega / OT_Fire 标准事件广播;
 * 不得直接调用改变模拟状态的引擎函数。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "PcxImage.h"
#include "Aggressive.h"

#define AG_SECTION "Aggressive"

/* ---------------- 只读地址 ---------------- */

#define VT_GETMISSION (0x184u / 4u)

/* MISSION_ATTACK 已在 OrderUtil.h 定义; 这里只补闲置判定用到的三个。 */
#define MISSION_GUARD  5
#define MISSION_AREAGUARD 11
#define MISSION_STOP   13

typedef int __attribute__((thiscall)) (*Fn_GetMission)(void* self);
typedef int __attribute__((thiscall)) (*Fn_GetGuardRange)(void* self, int control);
typedef int __attribute__((thiscall)) (*Fn_GetWeaponRange)(void* self, int idxWeapon);
typedef int __attribute__((thiscall)) (*Fn_IniKeyCount)(void* self, const char* section);
typedef const char* __attribute__((thiscall)) (*Fn_IniKeyName)(void* self, const char* section, int idx);

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
static int g_AgAllTargets = 1;   /* 0=只打建筑 (Target=building) / 1=所有敌方单位 (默认) */
static int g_AgSkipTech = 1;     /* 1=有价值的科技建筑不当目标 (SkipTechBuildings, 默认) */

/* 有价值的科技建筑 (按 rules 节名 = 类型 ID 认): 占领后给电力/资金/维修/视野*/
#define AG_TECH_MAX 48

static const char* const AG_TECH_DEFAULT =
    "CAPOWR,CASLAB,CAAIRP,CAOILD,CAOILDOM,CAFHOSP,CAHOSP";

static char g_AgTechRaw[1024];              /* ini 原文 (OU_ReadList 就地切分) */
static const char* g_AgTechBld[AG_TECH_MAX];
static int g_AgTechN = 0;

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
    {
        char buf[32];
        OU_ReadString(AG_SECTION, "Target", "all", buf, sizeof(buf));
        g_AgAllTargets = (buf[0] == 'b' || buf[0] == 'B') ? 0 : 1;
    }
    g_AgSkipTech = GetPrivateProfileIntA(AG_SECTION, "SkipTechBuildings", 1, g_IniPath) ? 1 : 0;
    g_AgTechN = OU_ReadList(AG_SECTION, "SkipTechList", AG_TECH_DEFAULT,
                            g_AgTechRaw, sizeof(g_AgTechRaw),
                            g_AgTechBld, AG_TECH_MAX);
    LogLine(g_AgEnable ? "aggressive: ON" : "aggressive: OFF");
    LogLine(g_AgSkipTech ? "aggressive: skip tech buildings" : "aggressive: hit tech buildings");
}

/* 设置页改完热键后回读 (ini 已写盘)。 */
void Aggressive_ReloadHotkey(void)
{
    AG_EnsureInit();
    g_AgEnable = OU_ReadHotkeySingle(AG_SECTION, "Hotkey", "Alt+A", &g_AgHk);
}

/* 目标范围 (设置页分段): 1 = 所有敌方单位, 0 = 只打建筑。 */
int Aggressive_AllTargets(void)
{
    AG_EnsureInit();
    return g_AgAllTargets;
}

void Aggressive_SetAllTargets(int all)
{
    AG_EnsureInit();
    g_AgAllTargets = all ? 1 : 0;
}

/* 有价值的科技建筑 (设置页开关): 1 = 不当目标, 0 = 照打。 */
int Aggressive_SkipTechBuildings(void)
{
    AG_EnsureInit();
    return g_AgSkipTech;
}

void Aggressive_SetSkipTechBuildings(int on)
{
    AG_EnsureInit();
    g_AgSkipTech = on ? 1 : 0;
}

/* ---------------- 小工具 ---------------- */

/* 对象 Fetch_ID (身份; 失败回 0)。rttiOut 透出 RTTI 字节 (可空)。 */
static int AG_FetchId(void* p, unsigned char* rttiOut)
{
    char tb[8] = {0};
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

/* 目标搜索半径 (轻子): GuardRange 与主/副武器射程取最大 —— 不主动索敌的支援/
 * 攻城单位 GuardRange 比真射程小, 只按它会够不到本来能打的目标。不能用
 * GetGuardRange(1): 它会把射程加倍后按 4096 轻子 (16 格) 封顶。
 * 最后乘 RangePercent%。非法回 0。 */
static int AG_GuardRange(void* u)
{
    int r, a, b;
    if (!u || !IsPlausiblePtr(u)) return 0;
    r = FN_GETGUARDRANGE(u, 0);
    a = AG_WeaponRange(u, 0);
    b = AG_WeaponRange(u, 1);
    if (a > r) r = a;
    if (b > r) r = b;
    if (r <= 0) return 0;
    if (g_AgRangePct != 100) r = r * g_AgRangePct / 100;
    if (r < 128 || r > 65536) return 0;
    return r;
}

/* 使用共享的只读光标与类内重映射, 排除移动令。 */
static int AG_UnitMission(void* u, void* t)
{
    int ms = OU_NativeMission(u, t);
    return (ms == MISSION_MOVE) ? -1 : ms;
}

/* 类型指针: ObjectClass::GetType 虚表槽 0x84 (引擎 0x69256B 同入口)。 */
typedef void* __attribute__((thiscall)) (*Fn_GetType)(void* self);
typedef void* __attribute__((thiscall)) (*Fn_GetCellAtCoords)(void* disp, const CoordStruct* crd);
typedef unsigned char __attribute__((thiscall)) (*Fn_CellSensors)(void* cell, int houseIdx);

#define VT_GETTYPE (0x84u / 4u)

/* "打不烂"的类型只有两种: `Immune=yes` 或 `Armor=special` —— 弹头打上去 0 伤害,
 * 攻击令只让单位白跑一趟。判据读 ObjectTypeClass 的两个字段:
 *   +0x233 Immune / +0x09C Armor (护甲序号, 见 AG_SpecialArmor)。 */
#define AG_ARMOR_SPECIAL_FALLBACK 236   /* 拿不到 rules ini 时的兜底 */

/* 类型指针 (GetType 虚表槽 0x84): 取不到返回 0。 */
static void* AG_TypeOf(void* p)
{
    void** vt;
    Fn_GetType getType;
    if (!p || !IsPlausiblePtr(p)) return 0;
    vt = *(void***)p;
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    getType = (Fn_GetType)vt[VT_GETTYPE];
    if (!getType || !IsPlausiblePtr((const void*)getType)) return 0;
    return getType(p);
}

/* 一行日志 "aggressive: ArmorTypes=N special=M", 对账用。 */
static void AG_LogArmorIdx(int count, int idx)
{
    char buf[64];
    unsigned p = AppendCStr(buf, 0, "aggressive: ArmorTypes=", sizeof(buf));
    p = AppendInt(buf, p, sizeof(buf), count);
    p = AppendCStr(buf, p, " special=", sizeof(buf));
    AppendInt(buf, p, sizeof(buf), idx);
    LogLine(buf);
}

/* 类型 ID (AbstractTypeClass::ID, char[0x18]) 是否等于 name: 大小写不敏感,
 * 按字段长度截断比较, 不假设调用方给的是 NUL 结尾的串。 */
static int AG_IdIs(const char* id, const char* name)
{
    int i;
    if (!id || !name) return 0;
    for (i = 0; i < 0x17; i++)
    {
        char a = id[i], b = name[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
        if (!a) return 1;
    }
    return 0;
}

/* `special` 护甲的序号: 问游戏已加载的 rules ini (0x887048 的 CCINIClass*)。
 * GetKeyCount/GetKeyName 的遍历顺序就是 Ares 给护甲编号的顺序 (它的
 * Enumerable<ArmorType>::LoadFromINIList 用同一对接口), 第 i 个键名即 `Armor`
 * 字段里会出现的值 i。缓存 "ini 指针 -> 序号" (换图会换指针); 拿不到 ini 用兜底
 * 常量, 有 ini 但没这个键则返回 -1 (不按 special 过滤)。 */
static int AG_SpecialArmor(void)
{
    static void* s_ini;
    static int s_idx = AG_ARMOR_SPECIAL_FALLBACK;
    void* ini = *(void**)ADDR_INI_RULES;
    int n, i;
    if (ini == s_ini) return s_idx;
    s_ini = ini;
    s_idx = AG_ARMOR_SPECIAL_FALLBACK;
    if (!ini || !IsPlausiblePtr(ini)) return s_idx;
    n = ((Fn_IniKeyCount)(void*)FN_INI_GETKEYCOUNT)(ini, "ArmorTypes");
    s_idx = -1;
    for (i = 0; i < n; i++)
    {
        const char* k = ((Fn_IniKeyName)(void*)FN_INI_GETKEYNAME)(ini, "ArmorTypes", i);
        if (k && AG_IdIs(k, "special")) { s_idx = i; break; }
    }
    AG_LogArmorIdx(n, s_idx);
    return s_idx;
}

/* 无敌对象: 打不动, 攻击令只会让单位白跑一趟。 */
static int AG_Invincible(void* p)
{
    void* type = AG_TypeOf(p);
    if (!type) return 0;
    if (*(unsigned char*)((char*)type + OFF_TYPE_IMMUNE)) return 1;
    return *(int*)((char*)type + OFF_TYPE_ARMOR) == AG_SpecialArmor();
}

/* 建筑是否属于上面那张表。建筑类型直接读 BuildingClass::Type (+0x520),
 * 不走 GetType 虚调用。 */
static int AG_ValuableBld(void* p)
{
    void* type;
    const char* id;
    unsigned i;
    if (!p || !IsPlausiblePtr(p)) return 0;
    type = *(void**)((char*)p + OFF_BLD_TYPE);
    if (!type || !IsPlausiblePtr(type)) return 0;
    id = (const char*)((char*)type + OFF_TYPE_ID);
    for (i = 0; i < (unsigned)g_AgTechN; i++)
        if (AG_IdIs(id, g_AgTechBld[i])) return 1;
    return 0;
}

/* 一条标记管两种过滤: 目标打不烂或值得留着时只挡攻击令 —— 占领/进驻这类
 * "拿走"的订单照发。 */
static int AG_NoAttack(void* p, int isBld)
{
    if (AG_Invincible(p)) return 1;
    return (isBld && g_AgSkipTech && AG_ValuableBld(p)) ? 1 : 0;
}

/* 这个目标"鼠标点得到"吗 —— 复刻引擎命中判定 (0x6DA380 像素命中 /
 * 0x692300 点击后处理): +0x41A 置位放行; 隐形 (Cloaked) 要求本家探测器覆盖
 * 其所在格; InvisibleInGame 类型一律点不到。点不到的目标光标落不上去, 攻击令
 * 也会被引擎清掉 (0x414D79), 单位只会发呆。 */
static int AG_Clickable(void* p, void* cur, const CoordStruct* c)
{
    void* type;
    void* cell;
    int idx;
    if (!p || !c) return 0;
    if (*(unsigned char*)((char*)p + OFF_TECH_HITEXEMPT)) return 1;
    if (*(int*)((char*)p + OFF_TECH_CLOAKSTATE) == CLOAKSTATE_CLOAKED)
    {
        if (!cur || !IsPlausiblePtr(cur)) return 1;   /* 查不到本家就退回原判定 */
        idx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
        if (idx < 0 || idx >= CELL_SENSORS_MAXHOUSE) return 1;   /* 索引异常不误杀 */
        cell = ((Fn_GetCellAtCoords)(void*)FN_GETCELLATCOORDS)((void*)ADDR_DISPLAY, c);
        if (!cell || !IsPlausiblePtr(cell)) return 1;
        if (!((Fn_CellSensors)(void*)FN_CELL_SENSORS)(cell, idx)) return 0;
    }
    type = AG_TypeOf(p);
    if (type && IsPlausiblePtr(type) &&
        *(unsigned char*)((char*)type + OFF_TYPE_INVISIBLE)) return 0;
    return 1;
}

/* ---------------- 标记集合 (本机 UI 状态) ---------------- */

#define AG_TRK_CAP 1024

typedef struct { void* p; int uid; int lastFire; } AGTrk;
static AGTrk s_AgTrk[AG_TRK_CAP];
static int s_AgTrkN = 0;
static int s_AgIdxOk = 0;   /* Fetch_ID 索引自检通过 (OU_PtrAlive 才严格) */

/* 还活着且可显示 (存活三连 + 索引校验 fail-open)。 */
static int AG_Alive(void* p, int uid)
{
    if (!OU_TechAliveRaw(p)) return 0;
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
        if (!OU_IsUnit(o)) continue;
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

/* ---------------- 敌方目标候选 (每轮建一次, 摊薄 GetCoords) ---------------- */

#define AG_TGT_CAP 4096     /* 候选表容量 (建筑优先收, 满了就不再收单位) */
#define AG_SCAN_CAP 16384   /* 单个类型数组的合法 count 上限, 超出按脏数据跳过 */

typedef struct { void* p; int id; int x, y; int noAtk; } AGTgt;
static AGTgt s_AgTgt[AG_TGT_CAP];
static int s_AgTgtN = 0;

/* 中立阵营判定 (OU_HouseNeutral: House->Type->MultiplayPassive, SmartArea 同式)。
 * Mode: 0=未定, 1=启用过滤, 2=停用 (回"非本家即目标")。 */
static int g_AgNeutMode = 0;

/* 四个类型数组各自的两轮稳定门: 引擎在改数组时可能读到半初始化的 items/count
 * (解引用会 fault), 只有连续两帧没变才当可用。0 号是建筑数组。 */
#define AG_ARR_N 4
static const unsigned s_AgArrBase[AG_ARR_N] = {
    ADDR_BLD_ARR, ADDR_UNIT_ARR, ADDR_INF_ARR, ADDR_AIRCRAFT_ARR
};
static void* s_AgArrItems[AG_ARR_N];
static int s_AgArrCount[AG_ARR_N];
static int s_AgArrStable[AG_ARR_N];

static void AG_Observe(void)
{
    int k;
    for (k = 0; k < AG_ARR_N; k++)
    {
        void** items = ARR_ITEMS(s_AgArrBase[k]);
        int count = ARR_COUNT(s_AgArrBase[k]);
        if (!items || !IsPlausiblePtr(items) || count <= 0 || count > AG_SCAN_CAP)
        {
            s_AgArrItems[k] = 0; s_AgArrCount[k] = 0; s_AgArrStable[k] = 0;
            continue;
        }
        if (items == s_AgArrItems[k] && count == s_AgArrCount[k])
        { if (s_AgArrStable[k] < 2) s_AgArrStable[k]++; continue; }
        s_AgArrItems[k] = items; s_AgArrCount[k] = count; s_AgArrStable[k] = 1;
    }
}

/* 收集存活、有坐标的非本家候选; 引擎光标最终判定可否下令。
 * items/count 切换时可能不匹配, 只遍历可读前缀。 */
static void AG_CollectArr(int k, void* cur)
{
    void** items = ARR_ITEMS(s_AgArrBase[k]);
    int count = ARR_COUNT(s_AgArrBase[k]);
    unsigned able;
    int i;
    if (s_AgArrStable[k] < 2) return;
    if (!cur || !items || !IsPlausiblePtr(items)) return;
    if (count <= 0 || count > AG_SCAN_CAP) return;
    able = MemReadableBytes(items, (unsigned)count * 4u) / 4u;
    if ((unsigned)count > able) count = (int)able;
    if (count <= 0) return;
    for (i = 0; i < count && s_AgTgtN < AG_TGT_CAP; i++)
    {
        void* p = items[i];
        void* own;
        CoordStruct c;
        if (!p || !IsPlausiblePtr(p)) continue;
        if (!OU_TechAliveRaw(p)) continue;
        if (k == 0 ? (OU_WhatAmI(p) != ABS_BUILDING) : !OU_IsUnit(p)) continue;
        own = *(void**)((char*)p + OFF_OWNER);
        if (!own || !IsPlausiblePtr(own) || own == cur) continue;   /* 敌/中立 */
        if (g_AgNeutMode == 1 && OU_HouseNeutral(own)) continue;   /* 忽略中立 */
        if (!OU_GetCoords(p, &c)) continue;
        if (!AG_Clickable(p, cur, &c)) continue;   /* 光标点不到的不当候选 */
        s_AgTgt[s_AgTgtN].p = p;
        s_AgTgt[s_AgTgtN].id = AG_FetchId(p, 0);
        s_AgTgt[s_AgTgtN].x = c.X;
        s_AgTgt[s_AgTgtN].y = c.Y;
        /* 打不烂/值得留的照收, 由 AG_FindTarget 按订单丢 (见 AG_NoAttack)。 */
        s_AgTgt[s_AgTgtN].noAtk = AG_NoAttack(p, k == 0);
        s_AgTgtN++;
    }
}

static void AG_BuildCands(void* cur)
{
    s_AgTgtN = 0;
    if (!cur) return;
    /* 本家自检一次 (本家不可能是被动中立)。 */
    if (g_AgNeutMode == 0)
        g_AgNeutMode = OU_HouseNeutral(cur) ? 2 : 1;
    AG_CollectArr(0, cur);              /* 建筑: 两种模式都要 */
    if (!g_AgAllTargets) return;
    AG_CollectArr(1, cur);              /* 载具 */
    AG_CollectArr(2, cur);              /* 步兵 */
    AG_CollectArr(3, cur);              /* 飞机 */
}

/* 警戒范围内最近的、本单位真能对其下令 (鼠标对应订单) 的敌方目标
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
    for (i = 0; i < s_AgTgtN; i++)
    {
        long long dx, dy, d2;
        int ms;
        dx = (long long)s_AgTgt[i].x - (long long)cu.X;
        dy = (long long)s_AgTgt[i].y - (long long)cu.Y;
        d2 = dx * dx + dy * dy;
        if (d2 > rr) continue;
        if (best && d2 >= bestD) continue;
        ms = AG_UnitMission(u, s_AgTgt[i].p);   /* 只读光标 = 与左键同判 */
        if (ms < 0) continue;
        /* 打不烂/值得留的目标: 只跳过攻击令 (工程师的占领/进驻照发)。 */
        if (ms == MISSION_ATTACK && s_AgTgt[i].noAtk) continue;
        best = s_AgTgt[i].p;
        bestD = d2;
        bestId = s_AgTgt[i].id;
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
        if (!OU_IsUnit(e->unit)) { AG_ShiftDown(); continue; }
        if (!OU_PtrAlive(e->foe, e->fid, s_AgIdxOk)) { AG_ShiftDown(); continue; }
        /* 挑中后隐形/探测器没了: 光标点不到, 引擎会清目标。 */
        {
            CoordStruct fc;
            if (!OU_GetCoords(e->foe, &fc) || !AG_Clickable(e->foe, cur, &fc))
            { AG_ShiftDown(); continue; }
        }
        /* 发令前重算订单 (只读光标复核), 状态变了就丢; 挑中后才变成打不烂的,
         * 攻击令同样作废 (占领令照发)。 */
        {
            int ms = AG_UnitMission(e->unit, e->foe);
            if (ms != e->mission) { AG_ShiftDown(); continue; }
            if (ms == MISSION_ATTACK && AG_Invincible(e->foe))
            { AG_ShiftDown(); continue; }
        }
        srcID = AG_FetchId(e->unit, &srcR);
        tgtID = AG_FetchId(e->foe, &tgtR);
        if (srcID <= 0 || tgtID <= 0) { AG_ShiftDown(); continue; }
        /* 攻击放 Target 槽; 进驻/占领/破坏放 Destination 槽 (与鼠标下令同形)。 */
        atk = (e->mission == MISSION_ATTACK);
        if (!OU_FireMega(houseIdx, srcID, srcR, e->mission,
                         atk ? tgtID : 0, atk ? tgtR : 0,
                         atk ? 0 : tgtID, atk ? 0 : tgtR, 0, 0, 0,
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
    if (s_AgTrkN <= 0) return;
    if (!cur || !IsPlausiblePtr(cur)) return;
    AG_BuildCands(cur);
    if (s_AgTgtN <= 0) return;
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
    unsigned char* base;
    OuClip clip;
    PcxImage img;
    int ox, oy, i;
    AG_EnsureInit();
    if (!g_AgEnable || s_AgTrkN <= 0 || !g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch < w * 2) return;
    /* 每帧重取: 引擎重载后 BSurface 指针会变。 */
    if (!PcxImage_Load(&img, "icon_aggressive.pcx", 2)) return;
    OU_ViewClip(&clip, w, h);
    base = GtLock(surf);
    if (!base) return;
    ox = g_AgOffX - img.width / 2;
    oy = g_AgOffY - img.height / 2;
    for (i = 0; i < s_AgTrkN; i++)
    {
        CoordStruct c;
        int px, py;
        if (!AG_Alive(s_AgTrk[i].p, s_AgTrk[i].uid)) continue;
        if (!OU_GetCoords(s_AgTrk[i].p, &c)) continue;
        if (!OU_ProjWorld(c.X, c.Y, c.Z, &px, &py)) continue;
        PcxImage_DrawClip(&img, base, pitch, px + ox, py + oy,
                          clip.x0, clip.y0, clip.x1, clip.y1);
    }
    GtUnlock(surf);
}
