/* SingleUnload.cpp - 单个载员下车: 点左下信息面板里的载员图标, 只放那一个。
 *
 * 载具: 引擎原生 UnitClass::Mission_Unload (跳表 0x73E5C0) 只有"全放": 状态 3
 * (入口 0x73D8B7) 每 tick 弹链头一个 (0x73D8CD = RemoveFirstPassenger
 * 0x4DE710), 直到 NumPassengers(0x114) <= NonPassengerCount(0x6E4) 就跳
 * 0x73DCAB 置状态 4 收尾, 中途无法指定对象。
 *
 * 驻军建筑: 载员存在 BuildingClass::Occupants 向量 (0x688/0x694), 不是
 * Passengers 链。Mission::Unload 的虚函数 (BuildingClass::Mission_Unload,
 * 0x44D880, 与 UnitClass 的 0x73D630 同一跳表槽 0x23C) 只在人数 > 0 时调
 * 0x457DE0 全放: 先找一个空位, 再倒序把每个驻军 unlimbo 出去, 最后
 * [0xa8e7ac]-- 并清空向量。
 *
 * 只放某一个的做法: "是哪一个" 必须跟着标准 MegaMission 事件一起走, 各端
 * 才会得出同一个结论。借事件的 Target 槽作自由字段 —— 引擎只在 Target
 * 的 rtti(+0x12) 非 0 时才解析 id (0x4C7225), rtti 留 0 就没人读 id。
 * 于是 id(+0x0E) = SU_MAGIC | 载员序号 (载具 = 链上第几个, 建筑 = 向量第几个)。
 * 事件本身仍是普通 Mission::Unload, 无目标无终点, 引擎照原生流程排队。
 *
 * 钩子 (返回值 0 = 跑原指令, 非 0 = 跳该地址):
 *   0x4C7462 (5B) SU_EventRecord : ESI=事件, EDI=接收对象。认出单卸令就记下
 *                                  (对象,序号); 同对象后到的普通整批卸载令也要
 *                                  清掉旧记录, 否则残留记录会把整批卸载截成
 *                                  只放一个。
 *   0x73D8B7 (6B) SU_StateEntry  : 载具状态 3 入口, ESI=单位。记录里的载员还在
 *                                  链上 → 交原生; 已经不在了 → 跳 0x73DCAB
 *                                  置状态 4, 所以一次任务只放一个。
 *   0x73D8CD (5B) SU_PopOne      : 载具原生弹链头那处 call。有记录就改调
 *                                  FootClass::RemovePassenger (0x4DE670) 指定
 *                                  对象, EAX 回填被卸者后跳 0x73D8D2 (原
 *                                  call 的下一句 mov %eax,%edi)。
 *   0x4580A9 (6B) SU_BldLoop     : 驻军全放循环头, ESI=建筑, EBP=向量下标。
 *                                  有记录就跳过其它下标 (跳 0x45819D 继续),
 *                                  只把记录里那个交原生放。
 *   0x4581A4 (5B) SU_BldTail     : 驻军循环收尾。原生这里 [0xa8e7ac]-- 并清空
 *                                  向量; 我们照减计数, 但只把放走的那个从向量
 *                                  里移掉, 其余驻军留下, 然后跳 0x4581C9 刷新
 *                                  建筑动画。
 *
 * 载具序号 = 从 FirstPassenger(0x118) 沿 NextObject(0x30) 数第几个 (含被面板
 * 跳过的项), 点击侧 (SU_IndexOf) 与钩子侧 (SU_PaxAt) 同一套数法; 建筑序号 =
 * Occupants 向量下标, 点击侧 (SU_BldIndexOf) 与钩子侧同一套。链序/向量序由
 * 模拟决定, 各端一致, 所以序号可以当事件载荷。
 *
 * 记录只由事件建立, 不落本地状态, 因此开启与否不影响联机结论:
 * [Features] SingleUnload 只决定面板点击会不会发这种事件。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderThrottle.h"
#include "OrderUtil.h"
#include "SingleUnload.h"

#define SU_SECTION "Features"

/* MegaMission 事件字段 (与 OFF_PLANFLAG 同一套打包布局)。 */
#define SU_OFF_MISSION   0x0Cu
#define SU_OFF_TGT_ID    0x0Eu
#define SU_OFF_TGT_RTTI  0x12u

#define SU_MAGIC         0x534E0000   /* 'S','N' 前缀; 低 8 位放序号 */
#define SU_MAGIC_MASK    0xFFFFFF00u
#define SU_IDX_MAX       63

#define SU_ADDR_DONE       0x0073DCABu   /* 置状态 4 (收尾) */
#define SU_ADDR_POP_DONE   0x0073D8D2u   /* call RemoveFirstPassenger 的下一句 */
#define SU_ADDR_BLD_SKIP   0x0045819Du   /* 驻军循环: dec ebp; jns 循环头 */
#define SU_ADDR_BLD_TAIL   0x004581C9u   /* 驻军循环: 清向量之后的动画刷新 */

/* 0x457DE0 在放人循环前 +1、循环后 -1 的"卸载进行中"计数 (其它代码据此跳过
 * 重入路径, 例如 0x4D9736)。单卸保留向量, 但这个计数必须照原样平衡。 */
#define ADDR_UNLOAD_BUSY   0xA8E7ACu

#define SU_REC_MAX       8
#define SU_TTL           450u            /* 事件到任务的等待上限 (逻辑帧) */
/* 驻军卸载在同一个任务 tick 内同步跑完, 窗口要短: 窗口过长时, 期间被卖掉/
 * 拆掉的建筑会被截成只放一个。 */
#define SU_BLD_TTL       30u
#define SU_FIRED_TTL     4u              /* 弹完到这个任务再进状态 3 的窗口 */
#define SU_CLICK_MS      350u            /* 同一 (车,载员) 双击去重 */

typedef void __attribute__((thiscall)) (*Fn_SU_RemovePax)(void* self, void* pax);

typedef struct
{
    void* unit;
    int idx;
    int fired;
    unsigned expire;
} SuRec;

static SuRec s_Rec[SU_REC_MAX];
static int s_Enable = 1;
static int s_Init = 0;

static void* s_LastUnit = 0;
static void* s_LastPax = 0;
static unsigned s_LastMs = 0;

/* 本次驻军全放 (0x457DE0) 的裁剪状态。整个函数在一帧内同步跑完, 所以用
 * (建筑, 帧号) 认"是不是同一次调用"; 尾钩一定会清掉。
 * s_BldIdx: >=0 只放这个下标, -1 本 call 一个都不放, -2 不是单卸 (全交原生)。 */
static void* s_BldCall = 0;
static unsigned s_BldFrame = 0;
static int s_BldIdx = -1;
static int s_BldFired = 0;
static void* s_BldPax = 0;

static unsigned SU_Reg(void* regs, unsigned off)
{
    return *(unsigned*)((char*)regs + off);
}

static unsigned SU_Frame(void)
{
    return *(volatile unsigned*)(size_t)ADDR_FRAME;
}

static void SU_LogIdx(const char* head, int idx)
{
    char buf[96];
    unsigned q = 0;

    q = AppendCStr(buf, q, head, sizeof(buf));
    q += UtoA((unsigned)idx, buf + q);
    q = AppendCStr(buf, q, " f=", sizeof(buf));
    q += UtoA(SU_Frame(), buf + q);
    LogLine(buf);
}

/* 面板点击排查: 每次点击只写一行, 带上判定用到的原始指针,
 * 日志里能直接看出卡在哪一步 (不需要用户复述现象)。 */
static void SU_LogClick(const char* why, void* unit, void* pax, void* extra)
{
    char buf[128];
    unsigned q = 0;

    q = AppendCStr(buf, q, "su: ", sizeof(buf));
    q = AppendCStr(buf, q, why, sizeof(buf));
    q = AppendCStr(buf, q, " u=", sizeof(buf));
    q += HtoA((unsigned)(size_t)unit, buf + q);
    q = AppendCStr(buf, q, " p=", sizeof(buf));
    q += HtoA((unsigned)(size_t)pax, buf + q);
    if (extra)
    {
        q = AppendCStr(buf, q, " x=", sizeof(buf));
        q += HtoA((unsigned)(size_t)extra, buf + q);
    }
    LogLine(buf);
}

/* 面板点击排查: 载员对象的关键字段一次写全, 便于对照链序与自述归属。 */
static void SU_LogPax(void* unit, void* pax, int idx)
{
    char buf[200];
    unsigned q = 0;

    q = AppendCStr(buf, q, "su: pax idx=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), idx);
    q = AppendCStr(buf, q, " wai=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), OU_WhatAmI(pax));
    q = AppendCStr(buf, q, " tr=", sizeof(buf));
    q += HtoA(*(unsigned*)((char*)pax + OFF_TECH_CONTAINER), buf + q);
    q = AppendCStr(buf, q, " t11c=", sizeof(buf));
    q += HtoA(*(unsigned*)((char*)pax + OFF_TECH_TRANSPORT), buf + q);
    q = AppendCStr(buf, q, " np=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), *(int*)((char*)unit + OFF_TECH_PASSNUM));
    q = AppendCStr(buf, q, " uf=", sizeof(buf));
    q += HtoA(*(unsigned*)((char*)unit + OFF_TECH_PASSFIRST), buf + q);
    q = AppendCStr(buf, q, " utr=", sizeof(buf));
    q += HtoA(*(unsigned*)((char*)unit + OFF_TECH_TRANSPORT), buf + q);
    LogLine(buf);
}

/* ini 在 BuildPaths 之后才有路径; 没就绪就先按出厂值 (开启) 用。 */
static void SU_EnsureInit(void)
{
    if (s_Init) return;
    if (!g_IniPath[0]) return;
    s_Enable = GetPrivateProfileIntA(SU_SECTION, "SingleUnload", 1, g_IniPath) ? 1 : 0;
    s_Init = 1;
}

/* ---------------- 驻军建筑: Occupants 向量 ---------------- */

static void** SU_BldItems(void* b)
{
    void** it;
    if (!b) return 0;
    it = *(void***)((char*)b + OFF_BLD_OCC_ITEMS);
    if (!it || !IsPlausiblePtr(it)) return 0;
    return it;
}

static int SU_BldCount(void* b)
{
    int c;
    if (!b) return 0;
    c = *(int*)((char*)b + OFF_BLD_OCC_COUNT);
    if (c < 0) return 0;
    if (c > SU_IDX_MAX + 1) c = SU_IDX_MAX + 1;
    return c;
}

static void* SU_BldAt(void* b, int idx)
{
    void** it = SU_BldItems(b);
    if (!it || idx < 0 || idx >= SU_BldCount(b)) return 0;
    return it[idx];
}

static int SU_BldIndexOf(void* b, void* pax)
{
    void** it = SU_BldItems(b);
    int n = SU_BldCount(b), i;

    if (!it || !pax) return -1;
    for (i = 0; i < n; i++)
        if (it[i] == pax) return i;
    return -1;
}

/* 移掉向量第 idx 个 (后面的前移, Count--), 与 DynamicVectorClass::RemoveItem 同序。
 * expect 非空时要求该格仍是它, 防止记录与向量脱节时误删别的驻军。 */
static int SU_BldRemoveAt(void* b, int idx, void* expect)
{
    void** it = SU_BldItems(b);
    int n = SU_BldCount(b), i;

    if (!it || idx < 0 || idx >= n) return 0;
    if (expect && it[idx] != expect) return 0;
    for (i = idx; i + 1 < n; i++) it[i] = it[i + 1];
    *(int*)((char*)b + OFF_BLD_OCC_COUNT) = n - 1;
    return 1;
}

/* 驻军建筑点击排查: 向量下标 / 当前人数 / 容器字段一次写全。 */
static void SU_LogBld(void* b, void* pax, int idx)
{
    char buf[160];
    unsigned q = 0;

    q = AppendCStr(buf, q, "su: bld idx=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), idx);
    q = AppendCStr(buf, q, " n=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), SU_BldCount(b));
    q = AppendCStr(buf, q, " b=", sizeof(buf));
    q += HtoA((unsigned)(size_t)b, buf + q);
    q = AppendCStr(buf, q, " p=", sizeof(buf));
    q += HtoA((unsigned)(size_t)pax, buf + q);
    q = AppendCStr(buf, q, " c=", sizeof(buf));
    q += HtoA(*(unsigned*)((char*)pax + OFF_TECH_CONTAINER), buf + q);
    LogLine(buf);
}

/* 链上第 idx 个载员 (含被面板跳过的项), 越界回 NULL。 */
static void* SU_PaxAt(void* unit, int idx)
{
    void* p;
    int i = 0;

    if (!unit || idx < 0 || idx > SU_IDX_MAX) return 0;
    p = *(void**)((char*)unit + OFF_TECH_PASSFIRST);
    while (p && IsPlausiblePtr(p) && i <= SU_IDX_MAX)
    {
        if (i == idx) return p;
        p = *(void**)((char*)p + OFF_NEXT_OBJ);
        i++;
    }
    return 0;
}

static int SU_IndexOf(void* unit, void* pax)
{
    void* p;
    int i = 0;

    if (!unit || !pax) return -1;
    p = *(void**)((char*)unit + OFF_TECH_PASSFIRST);
    while (p && IsPlausiblePtr(p) && i <= SU_IDX_MAX)
    {
        if (p == pax) return i;
        p = *(void**)((char*)p + OFF_NEXT_OBJ);
        i++;
    }
    return -1;
}

/* 该单位最老的一条记录 (先弹完的那条 expire 最小, 总排在最前), 没有回 -1。 */
static int SU_Find(void* unit)
{
    int i, best = -1;
    unsigned bestExp = 0;

    if (!unit) return -1;
    for (i = 0; i < SU_REC_MAX; i++)
    {
        if (s_Rec[i].unit != unit) continue;
        if (best < 0 || s_Rec[i].expire < bestExp)
        {
            best = i;
            bestExp = s_Rec[i].expire;
        }
    }
    return best;
}

static void SU_Drop(int i)
{
    if (i < 0 || i >= SU_REC_MAX) return;
    s_Rec[i].unit = 0;
    s_Rec[i].idx = 0;
    s_Rec[i].fired = 0;
    s_Rec[i].expire = 0;
}

static void SU_ClearUnit(void* unit)
{
    int i;
    for (i = 0; i < SU_REC_MAX; i++)
        if (s_Rec[i].unit == unit) SU_Drop(i);
}

static void SU_Add(void* unit, int idx, unsigned ttl)
{
    unsigned fr = SU_Frame();
    int i, slot = -1;

    for (i = 0; i < SU_REC_MAX; i++)
    {
        if (s_Rec[i].unit && s_Rec[i].expire && fr > s_Rec[i].expire) SU_Drop(i);
        if (!s_Rec[i].unit && slot < 0) slot = i;
    }
    if (slot < 0)
    {
        /* 满了: 丢最早过期的那条, 让新记录有位置。 */
        slot = 0;
        for (i = 1; i < SU_REC_MAX; i++)
            if (s_Rec[i].expire < s_Rec[slot].expire) slot = i;
    }
    s_Rec[slot].unit = unit;
    s_Rec[slot].idx = idx;
    s_Rec[slot].fired = 0;
    s_Rec[slot].expire = fr + ttl;
}

/* ---------------- 钩子 ---------------- */

/* EventClass 派发: 只读事件字节, 见到 SU_MAGIC 就记一条。 */
extern "C" __declspec(dllexport) unsigned __cdecl SU_EventRecord(void* regs)
{
    unsigned char* ev = (unsigned char*)(size_t)SU_Reg(regs, REG_OFF_ESI);
    void* unit = (void*)(size_t)SU_Reg(regs, REG_OFF_EDI);
    int id;

    if (!ev || !unit) return 0;
    if (ev[SU_OFF_MISSION] != MISSION_UNLOAD) return 0;
    {
        char buf[80];
        unsigned q = 0;
        q = AppendCStr(buf, q, "su: ev16 rtti=", sizeof(buf));
        q = AppendInt(buf, q, sizeof(buf), (int)ev[SU_OFF_TGT_RTTI]);
        q = AppendCStr(buf, q, " id=", sizeof(buf));
        q = AppendInt(buf, q, sizeof(buf), *(int*)(ev + SU_OFF_TGT_ID));
        LogLine(buf);
    }
    if (ev[SU_OFF_TGT_RTTI] != 0)
    {
        /* 带真目标的卸载令: 不是我们的单卸令, 旧记录要清掉, 否则整批
         * 卸载会被截成只放一个。 */
        SU_ClearUnit(unit);
        return 0;
    }
    id = *(int*)(ev + SU_OFF_TGT_ID);
    if (((unsigned)id & SU_MAGIC_MASK) != SU_MAGIC)
    {
        SU_ClearUnit(unit);
        return 0;
    }
    {
        int am = OU_WhatAmI(unit);
        if (am != ABS_UNIT && am != ABS_BUILDING) return 0;
        SU_Add(unit, id & 0xFF, am == ABS_BUILDING ? SU_BLD_TTL : SU_TTL);
    }
    SU_LogIdx("su: rec idx=", id & 0xFF);
    return 0;
}

/* 状态 3 入口: 记录里那个已经下车 (或记录过期) 就收尾, 只放一个。 */
extern "C" __declspec(dllexport) unsigned __cdecl SU_StateEntry(void* regs)
{
    void* unit = (void*)(size_t)SU_Reg(regs, REG_OFF_ESI);
    int i = SU_Find(unit);

    if (i < 0) return 0;
    /* 先看 fired: 弹完那条记录必须一直收尾到状态 3 再进来为止,
     * 中间按帧数过期会把剩下的载员交给原生整批卸载。 */
    if (s_Rec[i].fired)
    {
        SU_LogIdx("su: done idx=", s_Rec[i].idx);
        SU_Drop(i);
        return SU_ADDR_DONE;
    }
    if (s_Rec[i].expire && SU_Frame() > s_Rec[i].expire)
    {
        SU_LogIdx("su: expire idx=", s_Rec[i].idx);
        SU_Drop(i);
        return 0;
    }
    if (!SU_PaxAt(unit, s_Rec[i].idx))
    {
        SU_LogIdx("su: gone idx=", s_Rec[i].idx);
        SU_Drop(i);
        return SU_ADDR_DONE;
    }
    return 0;
}

/* 原生弹链头那处: 有记录就换成指定的那个载员。 */
extern "C" __declspec(dllexport) unsigned __cdecl SU_PopOne(void* regs)
{
    void* unit = (void*)(size_t)SU_Reg(regs, REG_OFF_ESI);
    int i = SU_Find(unit);
    void* pax;

    if (i < 0) return 0;
    if (s_Rec[i].fired)
    {
        SU_Drop(i);
        return SU_ADDR_DONE;
    }
    pax = SU_PaxAt(unit, s_Rec[i].idx);
    if (!pax)
    {
        SU_Drop(i);
        return SU_ADDR_DONE;
    }
    ((Fn_SU_RemovePax)(void*)0x004DE670u)(unit, pax);
    *(unsigned*)((char*)regs + REG_OFF_EAX) = (unsigned)(size_t)pax;
    s_Rec[i].fired = 1;
    s_Rec[i].expire = SU_Frame() + SU_FIRED_TTL;
    SU_LogIdx("su: pop idx=", s_Rec[i].idx);
    return SU_ADDR_POP_DONE;
}

/* 驻军全放循环头 (0x4580A9): 有记录就只让记录里那个进原生放人流程。
 * 其它下标直接跳到循环尾 (dec ebp; jns), 驻军仍留在向量里。 */
extern "C" __declspec(dllexport) unsigned __cdecl SU_BldLoop(void* regs)
{
    void* b = (void*)(size_t)SU_Reg(regs, REG_OFF_ESI);
    int idx = (int)SU_Reg(regs, REG_OFF_EBP);
    unsigned fr = SU_Frame();
    int i;

    if (!b) return 0;
    if (s_BldCall != b || s_BldFrame != fr)
    {
        s_BldCall = b;
        s_BldFrame = fr;
        s_BldIdx = -2;
        s_BldFired = 0;
        s_BldPax = 0;
        i = SU_Find(b);
        if (i < 0) return 0;            /* 不是单卸: 交原生全放 */
        if (s_Rec[i].fired)
        {
            /* 上一次调用没走到收尾 (例如放人途中转去 KillOccupants):
             * 不能再放第二次, 本次一个都不放。 */
            s_BldIdx = -1;
        }
        else if (s_Rec[i].expire && fr > s_Rec[i].expire)
        {
            SU_LogIdx("su: bld expire idx=", s_Rec[i].idx);
            SU_Drop(i);
            s_BldIdx = -1;              /* 认不出要放谁: 本次一个都不放 */
        }
        else
        {
            s_BldIdx = s_Rec[i].idx;
        }
    }
    if (s_BldIdx == -2) return 0;
    if (s_BldFired || idx != s_BldIdx) return SU_ADDR_BLD_SKIP;
    s_BldPax = SU_BldAt(b, idx);
    if (!s_BldPax) return SU_ADDR_BLD_SKIP;
    s_BldFired = 1;
    i = SU_Find(b);
    if (i >= 0) s_Rec[i].fired = 1;
    SU_LogIdx("su: bld pop idx=", idx);
    return 0;                            /* 放这一个 */
}

/* 驻军全放收尾 (0x4581A4): 原生要 [0xa8e7ac]-- 并清空向量。
 * 单卸照减计数, 但只把放走的那个移出向量, 其余驻军留着, 再跳动画刷新。 */
extern "C" __declspec(dllexport) unsigned __cdecl SU_BldTail(void* regs)
{
    void* b = (void*)(size_t)SU_Reg(regs, REG_OFF_ESI);
    int i;

    if (!b || s_BldCall != b || s_BldIdx == -2)
    {
        s_BldCall = 0;
        return 0;
    }
    (*(int*)(size_t)ADDR_UNLOAD_BUSY)--;
    if (s_BldFired && s_BldPax && SU_BldRemoveAt(b, s_BldIdx, s_BldPax))
        SU_LogIdx("su: bld done idx=", s_BldIdx);
    else
        SU_LogIdx("su: bld miss idx=", s_BldIdx);
    i = SU_Find(b);
    if (i >= 0) SU_Drop(i);
    s_BldCall = 0;
    s_BldFrame = 0;
    s_BldIdx = -1;
    s_BldFired = 0;
    s_BldPax = 0;
    return SU_ADDR_BLD_TAIL;
}

/* ---------------- 面板入口 ---------------- */

int SU_ClickPassenger(void* unit, void* pax)
{
    char ev[EVT_SIZE];
    unsigned now;
    void* owner;
    int idx, uid, houseIdx, am;
    unsigned char ur = 0;

    SU_EnsureInit();
    if (!s_Enable) { SU_LogClick("skip off", unit, pax, 0); return 0; }
    if (!IsPlausiblePtr(unit) || !IsPlausiblePtr(pax))
    {
        SU_LogClick("skip ptr", unit, pax, 0);
        return 0;
    }
    am = OU_WhatAmI(unit);
    if (am == ABS_BUILDING)
    {
        idx = SU_BldIndexOf(unit, pax);
        if (idx < 0)
        {
            SU_LogClick("skip bld index", unit, pax, 0);
            return 0;
        }
        /* 下标能查到即证明 pax 在驻军向量里 (面板格子就是按这个向量铺的)。 */
        SU_LogBld(unit, pax, idx);
    }
    else if (am == ABS_UNIT)
    {
        idx = SU_IndexOf(unit, pax);
        if (idx < 0)
        {
            SU_LogClick("skip index", unit, pax, 0);
            return 0;
        }
        /* 序号能查到即证明 pax 在本车的载员链上 (面板格子就是从这个链取的),
         * 所以不再用 Transporter 字段做归属判定。 */
        SU_LogPax(unit, pax, idx);
    }
    else
    {
        SU_LogClick("skip whatami", unit, pax, 0);
        return 0;
    }
    owner = *(void**)((char*)unit + OFF_OWNER);
    if (!owner || owner != *(void**)ADDR_CURPLAYER)
    {
        SU_LogClick("skip owner", unit, pax, owner);
        return 0;
    }

    now = GetTickCount();
    if (unit == s_LastUnit && pax == s_LastPax && (now - s_LastMs) < SU_CLICK_MS)
        return 1;

    uid = OU_ObjTgt(unit, &ur);
    if (uid < 0)
    {
        SU_LogClick("skip noid", unit, pax, 0);
        return 0;
    }
    houseIdx = *(int*)((char*)owner + OFF_HOUSE_INDEX);

    OU_MakeMega(ev, houseIdx, uid, ur, MISSION_UNLOAD, 0, 0, 0, 0, 0, 0, 0);
    *(int*)(ev + SU_OFF_TGT_ID) = (int)(SU_MAGIC | (unsigned)idx);
    ev[SU_OFF_TGT_RTTI] = 0;

    if (!OT_Fire(ev, unit, uid, 0))
    {
        SU_LogClick("skip fire", unit, pax, 0);
        return 0;
    }
    s_LastUnit = unit;
    s_LastPax = pax;
    s_LastMs = now;
    SU_LogIdx("su: click idx=", idx);
    return 1;
}
