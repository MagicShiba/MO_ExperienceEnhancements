/* SingleUnload.cpp - 面板载员格的两件事: 单个下车, 以及按住 Z 给载员规划路径点。
 * 细节见 notes/单个载员下车.md 与 notes/载具内成员路径点.md。
 *
 * 单卸: 引擎原生卸载只有"全放" (载具 0x73D8B7 每 tick 弹链头, 建筑 0x457DE0 倒序
 * unlimbo 全部), 参数里没有"哪一个"。所以仍发标准 Mission::Unload, 把序号塞进事件的
 * Target.id (rtti 留 0 时引擎不解析 id), 再由五个钩子在派发/入口/弹人/循环处按记录
 * 裁掉其它人。序号 = 载具 Passengers 链下标 / 建筑 Occupants 向量下标, 链序由模拟
 * 决定、各端一致, 所以能当事件载荷; 记录只由事件建立, 不落本地状态。
 *
 * 联机 (GameMode 3/4) 一律不启用单卸: 正确性依赖每个端都按同一套规则解释这条令,
 * 对手缺这套钩子就会当场分叉, 引擎也没有可广播的替代做法。
 *
 * 路径点只发标准 MegaMission (IsPlanningEvent=1): 引擎建节点那条路
 * (0x637E00 → 0x638120) 不看 InLimbo, 所以舱内载员也能挂节点。
 *
 * 钩子返回值约定: 0 = 跑原指令, 非 0 = 跳该地址。rel 站点要自己跑原指令并返回
 * 绝对地址, 见 notes/崩溃定位.md。
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
/* 原生"弹链头" 0x4DE710: 只有 ECX=this, 无栈参, 返回被卸者。
 * 站点 0x73D8CD 那句 call 被 Syringe 抄进桩里时 rel32 不会修正, 所以
 * "跑原指令" 在这里等于跳到别的代码池 (见 notes/崩溃定位.md)。 */
typedef void* __attribute__((thiscall)) (*Fn_SU_PopHead)(void* self);

typedef struct
{
    void* unit;
    int idx;
    int fired;
    unsigned expire;
} SuRec;

static SuRec s_Rec[SU_REC_MAX];
static int s_Enable = 1;
static int s_PlanEnable = 1;   /* [Features] PaxWaypoint */
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
    s_PlanEnable = GetPrivateProfileIntA(SU_SECTION, "PaxWaypoint", 1, g_IniPath) ? 1 : 0;
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
    /* 联机不建记录: 钩子仍在, 没有记录就全部落回原生整批卸载。 */
    if (OU_IsMP()) return 0;
    id = *(int*)(ev + SU_OFF_TGT_ID);
    if (ev[SU_OFF_TGT_RTTI] != 0 || ((unsigned)id & SU_MAGIC_MASK) != SU_MAGIC)
    {
        /* 带真目标的卸载令 / 无 SU_MAGIC 前缀: 都不是我们的单卸令,
         * 旧记录要清掉, 否则整批卸载会被截成只放一个。 */
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

/* 原生弹链头那处: 有记录就换成指定的那个载员。
 * 没记录时也必须自己跑原 call (0x4DE710) 再跳下一句: 桩尾副本里那句
 * call 的 rel32 指向原站点附近, 副本换到代码池后目标跟着平移, 落到别的
 * 代码池零填充上 → add [eax],al, eax=0 → 写地址 0 崩溃 (见 notes/崩溃定位.md)。 */
extern "C" __declspec(dllexport) unsigned __cdecl SU_PopOne(void* regs)
{
    void* unit = (void*)(size_t)SU_Reg(regs, REG_OFF_ESI);
    int i = SU_Find(unit);
    void* pax;

    if (i < 0)
    {
        /* GCC 的 thiscall = this 放 ECX、不压栈, 与 0x4DE710 的约定一致。
         * 返回值直接是 EAX, 下一句 0x73D8D2 就是 mov edi,eax。 */
        *(unsigned*)((char*)regs + REG_OFF_EAX) =
            (unsigned)(size_t)((Fn_SU_PopHead)(void*)0x004DE710u)(unit);
        return SU_ADDR_POP_DONE;
    }
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

/* ---------------- 载员路径点 (Z + 点载员格 / 点本车格=全部载员) ---------------- */

/* 点击即发令: 一次点击 = 一条 IsPlanningEvent=1 的 MegaMission, 引擎在 0x638120
 * 把节点挂到该载员的 token 上, 松开 Z 由引擎自己提交 (0x637A10)。可以一次给多个
 * 载员规划, 每个人的令各自判类型、各自发。
 *
 * 三条硬约束:
 *  1. 提交会吃掉队首节点 (0x6385C0 把 token[0] 转成真令派下去), 载员在舱里跑不了
 *     就废了 —— 所以单载员容器认人时要先垫一个"走到原地"的节点去挨这一下。
 *  2. 取格函数自身被点击门钩着, 必须用 s_PlanQuery 自锁, 否则取格恒失败。
 *  3. 取消选中 (右键) 必须能结束规划, 否则地图点击仍被吞掉去给载员加点。
 *
 * 手势、垫点判据、任务类型映射见 notes/载具内成员路径点.md。 */

typedef struct
{
    void* pax;
    int   id, idxOk;   /* 绑单位 ID, 防指针复用串线 */
    int   padded;      /* 这条计划已经垫过 */
    int   seen;        /* 见过 token 挂上节点 (计划真建起来了) */
} SuPad;

static SuPad s_Pad[SU_PAX_MAX];
static void* s_PadScen = 0;
static int   s_PadFrame = -1;

/* 本次规划的对象 (Z 按住期间有效) */
static void* s_PlanPax[SU_PAX_MAX];
static int   s_PlanPaxN = 0;
static void* s_PlanHost = 0;
static int   s_PlanHostBld = 0;
static int   s_PlanWant = 0;      /* 有一次点击要加点 (格在 Poll 里取) */
static int   s_PlanLb = 0;        /* 左键按下沿: 一次按下只加一个点 */
static int   s_PlanQuery = 0;     /* 正在自己算格: 门里不许再吞 (会递归) */
static int   s_PlanFired = 0;     /* 本次规划已发几轮节点 (面板据此决定收不收名单) */

/* PlanningModeActive: 按住 Z 期间为 1 (各端一致, 只读)。 */
static int SU_PlanMode(void)
{
    return *(volatile unsigned char*)(size_t)ADDR_PLANMODE ? 1 : 0;
}

/* 容器还在当前选区里吗。右键取消选中 = 移除, 这时就不能再给他车里的载员下令了。 */
static int SU_HostSelected(void)
{
    void** items = *(void***)(size_t)(ADDR_CUR_OBJECTS + OFF_VEC_ITEMS);
    int n = *(int*)(size_t)(ADDR_CUR_OBJECTS + OFF_VEC_COUNT);
    int i;

    if (!s_PlanHost) return 0;
    if (!items || !IsPlausiblePtr(items)) return 0;
    if (n <= 0 || n > 256) return 0;
    for (i = 0; i < n; i++)
        if (items[i] == s_PlanHost) return 1;
    return 0;
}

/* p 现在确实在这个容器的载员名单里吗。
 * 面板快照会过期 (载员下车/换车/指针被复用), 每个都要现查, 不能只信名单。 */
static int SU_PaxInHost(void* host, int am, void* p)
{
    if (!host || !p || !IsPlausiblePtr(p)) return 0;
    if (am == ABS_BUILDING) return SU_BldIndexOf(host, p) >= 0;
    return SU_IndexOf(host, p) >= 0;
}

static int SU_PaxInside(void* pax)
{
    if (!IsPlausiblePtr(s_PlanHost)) return 0;
    return SU_PaxInHost(s_PlanHost, s_PlanHostBld ? ABS_BUILDING : ABS_UNIT, pax);
}

/* 还有没有可以下令的载员: 顺便把已经不在车里的剔掉。 */
static int SU_PlanStillInside(void)
{
    int i, j = 0;

    if (s_PlanPaxN <= 0) return 0;
    if (!SU_HostSelected()) return 0;
    for (i = 0; i < s_PlanPaxN; i++)
        if (SU_PaxInside(s_PlanPax[i])) s_PlanPax[j++] = s_PlanPax[i];
    s_PlanPaxN = j;
    return j > 0;
}

static void SU_PlanDisarm(void)
{
    s_PlanHost = 0;
    s_PlanHostBld = 0;
    s_PlanPaxN = 0;
    s_PlanWant = 0;
    s_PlanLb = 0;
    s_PlanFired = 0;
}

/* ---------------- 垫点状态表 ---------------- */

static void SU_PadClear(void)
{
    int i;
    for (i = 0; i < SU_PAX_MAX; i++) s_Pad[i].pax = 0;
}

/* 取这个载员的垫点状态; 没有就占一格 (满了丢 0 号, 再老的没有活动可依)。 */
static SuPad* SU_PadSlot(void* pax)
{
    SuPad* e;
    int i, uid;
    unsigned char r = 0;

    for (i = 0; i < SU_PAX_MAX; i++)
        if (s_Pad[i].pax == pax) return &s_Pad[i];
    for (i = 0; i < SU_PAX_MAX; i++)
        if (!s_Pad[i].pax) break;
    if (i >= SU_PAX_MAX)
    {
        SU_LogClick("plan pad drop", s_Pad[0].pax, pax, 0);
        for (i = 0; i + 1 < SU_PAX_MAX; i++) s_Pad[i] = s_Pad[i + 1];
        i = SU_PAX_MAX - 1;
    }
    e = &s_Pad[i];
    e->pax = pax;
    uid = OU_ObjTgt(pax, &r);
    e->id = (uid > 0) ? uid : 0;
    e->idxOk = (uid > 0) ? 1 : 0;      /* 抓不到 ID 就 fail-open, 别把记录判死 */
    e->padded = 0;
    e->seen = 0;
    return e;
}

/* 一逻辑帧扫一次: 见到 token 上有节点就记 seen, 之后又清空 = 计划跑完,
 * 下一条路径重新垫点。 */
static void SU_PadPoll(void)
{
    void* scen = *(void**)ADDR_SCENARIO;
    int fr, i, n;

    if (scen != s_PadScen)
    {
        s_PadScen = scen;
        SU_PadClear();
        return;
    }
    fr = *(int*)ADDR_FRAME;
    if (fr == s_PadFrame) return;
    s_PadFrame = fr;
    for (i = 0; i < SU_PAX_MAX; i++)
    {
        SuPad* e = &s_Pad[i];
        if (!e->pax) continue;
        if (!IsPlausiblePtr(e->pax) || !OU_PtrAlive(e->pax, e->id, e->idxOk))
        {
            e->pax = 0;
            continue;
        }
        n = OU_TokenNodes(e->pax);
        if (n > 0) e->seen = 1;                /* 节点确实挂上了 token */
        else if (e->seen) { e->padded = 0; e->seen = 0; }   /* 计划跑完: 下次进车重新垫 */
    }
}

/* 发一条规划令 (IsPlanningEvent=1)。形状与手点同源: 攻击填 Target,
 * 进入/占领/进驻破坏填 Destination, 移动填 Destination+Follow=格。 */
static int SU_FirePlanNode(int houseIdx, void* pax, int uid, unsigned char ur,
                           int ms, int tid, unsigned char tr, int cx, int cy)
{
    unsigned char cr = 0;
    int cid;

    if (ms == MISSION_ATTACK && tid >= 0)
        return OU_FireMega(houseIdx, uid, ur, MISSION_ATTACK,
                           tid, tr, 0, 0, 0, 0, 1, pax, uid, 1);
    if (tid >= 0)
        return OU_FireMega(houseIdx, uid, ur, ms,
                           0, 0, tid, tr, 0, 0, 1, pax, uid, 1);
    cid = OU_CellTgt(cx, cy, &cr);
    return OU_FireMega(houseIdx, uid, ur, MISSION_MOVE,
                       0, 0, cid, cr, cid, cr, 1, pax, uid, 1);
}

/* 容器里现在有几个载员 (载具走 0x118 链, 建筑走 Occupants 向量)。 */
static int SU_HostPaxCount(void* host, int bld)
{
    int n = 0;

    if (!host || !IsPlausiblePtr(host)) return 0;
    if (bld)
    {
        void** items = *(void***)((char*)host + OFF_BLD_OCC_ITEMS);
        int cnt = *(int*)((char*)host + OFF_BLD_OCC_COUNT);
        if (!items || !IsPlausiblePtr(items) || cnt <= 0) return 0;
        return (cnt > SU_PAX_MAX) ? SU_PAX_MAX : cnt;
    }
    {
        void* p = *(void**)((char*)host + OFF_TECH_PASSFIRST);
        int guard = 0;
        while (p && IsPlausiblePtr(p) && guard < 64)
        {
            n++;
            p = *(void**)((char*)p + OFF_NEXT_OBJ);
            guard++;
        }
    }
    return n;
}

/* 给一个载员垫点: 一个移动节点, 落点取他自己所在格。
 *
 * 必须在**认人那一刻**发, 不能等第一次点击: 提交发生在松开 Z 的瞬间 (0x637A10),
 * 而单机事件是直写 DoList、由主循环下一拍才派发, 同帧发就会撞上"事件还没进 token
 * 就提交"。落点用自己那格: 提交后等于"走到原地", 出舱瞬间完成, 也不与第一个真
 * 节点同格同形被引擎合并。 */
static void SU_PlanPadOne(void* pax)
{
    SuPad* pad;
    CoordStruct c;
    void* owner;
    int houseIdx, uid, cx, cy;
    unsigned char ur = 0;
    char buf[64];
    unsigned q = 0;

    if (!pax || !IsPlausiblePtr(pax)) return;
    pad = SU_PadSlot(pax);
    if (pad->padded) return;
    /* 已经有计划留在 token 上: 往队尾垫一个只会多走一段。 */
    if (OU_TokenNodes(pax) > 0) { pad->padded = 1; return; }
    owner = *(void**)((char*)pax + OFF_OWNER);
    if (!owner) { SU_LogClick("plan pad owner", s_PlanHost, pax, 0); return; }
    houseIdx = *(int*)((char*)owner + OFF_HOUSE_INDEX);
    uid = OU_ObjTgt(pax, &ur);
    if (uid <= 0) { SU_LogClick("plan pad uid", s_PlanHost, pax, 0); return; }
    if (!OU_GetCoords(pax, &c) &&
        !(s_PlanHost && OU_GetCoords(s_PlanHost, &c)))
    {
        SU_LogClick("plan pad pos", s_PlanHost, pax, 0);
        return;
    }
    cx = c.X / 256;
    cy = c.Y / 256;
    if (!OU_CellValid(cx, cy)) { SU_LogClick("plan pad cell", s_PlanHost, pax, 0); return; }
    if (!SU_FirePlanNode(houseIdx, pax, uid, ur, MISSION_MOVE, -1, 0, cx, cy))
    {
        SU_LogClick("plan pad fire", s_PlanHost, pax, 0);
        return;
    }
    pad->padded = 1;
    q = AppendCStr(buf, q, "su: plan pad x=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), cx);
    q = AppendCStr(buf, q, " y=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), cy);
    LogLine(buf);
}

/* ---------------- 面板入口 ---------------- */

/* s_PlanPax[0..n-1] 已经填好之后: 立状态 + 给需要的人垫点。
 * lb = 1 表示这次认人用的就是刚按下的那一下 (面板点击), 要吞掉;
 * lb = 0 表示认人是顺手做的 (按 Z 自动认名单), 紧接着的地图点击要当第一个节点。 */
static int SU_PlanBegin(void* host, int am, int n, int lb)
{
    int i, pc;

    if (n <= 0) { SU_LogClick("plan arm empty", host, 0, 0); return 0; }
    s_PlanHost = host;
    s_PlanHostBld = (am == ABS_BUILDING) ? 1 : 0;
    s_PlanPaxN = n;
    s_PlanWant = 0;
    s_PlanLb = lb;
    s_PlanFired = 0;
    SU_LogClick("plan arm", host, s_PlanPax[0], (void*)(size_t)n);
    /* 只有容器里就他一个人时, 提交才会吃掉队首节点; 多载员不垫。
     * pc==0 视为容器读失败, 按"单载员"兜底 (垫多一段比丢第一个点轻)。 */
    pc = SU_HostPaxCount(host, s_PlanHostBld);
    if ((pc == 1 || pc == 0) && n == 1)
    {
        for (i = 0; i < n; i++) SU_PlanPadOne(s_PlanPax[i]);   /* 必须此刻垫, 等点击就晚了 */
    }
    else
    {
        char buf[48];
        unsigned q = 0;
        q = AppendCStr(buf, q, "su: plan nopad pc=", sizeof(buf));
        q = AppendInt(buf, q, sizeof(buf), pc);
        LogLine(buf);
    }
    return 1;
}

/* 容器是不是本家的、能不能承载载员; 能就回 am, 否则回 -1。 */
static int SU_PlanHostAm(void* host)
{
    void* owner;
    int am;

    if (!IsPlausiblePtr(host)) return -1;
    am = OU_WhatAmI(host);
    if (am != ABS_BUILDING && am != ABS_UNIT) return -1;
    owner = *(void**)((char*)host + OFF_OWNER);
    if (!owner || owner != *(void**)ADDR_CURPLAYER) return -1;
    return am;
}

/* 认人: pax 非空 = 只给这一个; pax == 0 = 给容器里所有载员。
 * 只认本家的容器 (别家的令发不出去), 认下后由 SU_PlanBegin 决定要不要垫点。 */
static int SU_PlanArm(void* host, void* pax)
{
    int am, n = 0, i;

    if (!s_PlanEnable) return 0;
    am = SU_PlanHostAm(host);
    if (am < 0) return 0;

    if (pax)
    {
        if (!SU_PaxInHost(host, am, pax)) return 0;
        s_PlanPax[n++] = pax;
    }
    else if (am == ABS_BUILDING)
    {
        void** items = *(void***)((char*)host + OFF_BLD_OCC_ITEMS);
        int cnt = *(int*)((char*)host + OFF_BLD_OCC_COUNT);
        if (items && IsPlausiblePtr(items) && cnt > 0)
        {
            if (cnt > SU_PAX_MAX) cnt = SU_PAX_MAX;
            for (i = 0; i < cnt; i++)
                if (items[i] && IsPlausiblePtr(items[i])) s_PlanPax[n++] = items[i];
        }
    }
    else
    {
        void* p = *(void**)((char*)host + OFF_TECH_PASSFIRST);
        int guard = 0;
        while (p && IsPlausiblePtr(p) && guard < 64 && n < SU_PAX_MAX)
        {
            s_PlanPax[n++] = p;
            p = *(void**)((char*)p + OFF_NEXT_OBJ);
            guard++;
        }
    }
    return SU_PlanBegin(host, am, n, 1);
}

/* 名单里的载员收进 s_PlanPax[], 回收下几个。 */
static int SU_PlanCollect(void* host, int am, void** list, int n)
{
    int i, m = 0;

    for (i = 0; i < n && m < SU_PAX_MAX; i++)
        if (SU_PaxInHost(host, am, list[i])) s_PlanPax[m++] = list[i];
    return m;
}

/* 用面板名单认人: 只收名单里确实还在这个容器里的人。
 * lb 见 SU_PlanBegin (面板点击来的要吞掉那一下, 按 Z 自动认的不能吞)。 */
static int SU_PlanArmFromList(void* host, void** list, int n, int lb)
{
    int am, m;

    if (!s_PlanEnable) return 0;
    if (!SU_PlanMode()) return 0;          /* 没按 Z 就不是规划, 交回普通点击 */
    if (!list || n <= 0) return 0;
    am = SU_PlanHostAm(host);
    if (am < 0) return 0;
    m = SU_PlanCollect(host, am, list, n);
    if (m <= 0) return 0;
    return SU_PlanBegin(host, am, m, lb);
}

/* 面板 shift 多选之后: 只给名单里的这几个载员规划 (名单外的载员不参与)。 */
int SU_PlanArmList(void* host, void** list, int n)
{
    return SU_PlanArmFromList(host, list, n, 1);
}

/* 按 Z 那一刻自动认下 shift 名单: 有名单就直接在地图上点, 不必先点面板一次。
 * 不吞当前按下 —— 这一次按下是地图点击, 要当第一个节点。 */
int SU_PlanAutoArm(void* host, void** list, int n)
{
    if (s_PlanPaxN > 0) return 0;          /* 已经认过人, 不重复 */
    return SU_PlanArmFromList(host, list, n, 0);
}

int SU_PlanActive(void)
{
    return s_PlanPaxN > 0;
}

int SU_PlanFiredCount(void)
{
    return s_PlanFired;
}

/* 这个载员在不在本次规划的参与集合里。
 * 面板画高亮框要用: "Z+点一个人开规划, 再 shift 补选" 时第一个人不在 shift 名单里,
 * 只看名单他会一个框都没有 —— 但他是实实在在参与了这次规划的。 */
int SU_PlanHasPax(void* pax)
{
    int i;

    if (!pax) return 0;
    for (i = 0; i < s_PlanPaxN; i++) if (s_PlanPax[i] == pax) return 1;
    return 0;
}

/* 规划已认下之后 shift 补选: 新出现的载员加进集合。
 *
 * **只增不减**: 节点是"点击即发"的, 已经发出去的收不回来 (单机没有可用的删节点
 * 事件), 把一个人移出集合只会留下挂在他 token 上的孤儿节点。所以 shift 取消选中
 * 对已在规划中的人不起作用 —— 要重来就松 Z 重新认人。 */
int SU_PlanResync(void* host, void** list, int n)
{
    int am, i, k, add = 0;

    if (!s_PlanEnable) return 0;
    if (s_PlanPaxN <= 0 || !s_PlanHost || host != s_PlanHost) return 0;
    if (!list || n <= 0) return 0;
    am = SU_PlanHostAm(host);
    if (am < 0) return 0;
    for (i = 0; i < n && s_PlanPaxN < SU_PAX_MAX; i++)
    {
        void* p = list[i];
        if (!SU_PaxInHost(host, am, p)) continue;
        for (k = 0; k < s_PlanPaxN; k++) if (s_PlanPax[k] == p) break;
        if (k < s_PlanPaxN) continue;          /* 已经在集合里 */
        s_PlanPax[s_PlanPaxN++] = p;
        add++;
    }
    return add ? 1 : 0;
}

/* 记一个路径点 (立即发事件), 对所有已认下的载员各发一条。
 *
 * 任务不能恒为 Move: 路径点上的命令与裸左键同判 (工程师点建筑=占领、
 * 步兵点敌兵=攻击、点自家载具=进入)。判定直接问引擎 OU_NativeMission
 * (vtbl+0x74 光标 → 类内重映射 → 0x4D74E0 表), 与手点一致;
 * 回 -1 (该光标下引擎也无对应 Mission) 才落回移动。
 * 每个载员各自判: 同一格对工程师是占领、对步兵是移动, 不能共用一次判定。 */
static void SU_PlanAdd(int cx, int cy, void* tgt)
{
    char buf[96];
    unsigned q = 0;
    void* owner;
    int houseIdx, k, ok = 0, ms0 = MISSION_MOVE;

    if (!OU_CellValid(cx, cy)) { SU_LogClick("plan cell bad", s_PlanHost, s_PlanPax[0], 0); return; }
    owner = *(void**)((char*)s_PlanHost + OFF_OWNER);
    if (!owner) { SU_LogClick("plan owner fail", s_PlanHost, s_PlanPax[0], 0); return; }
    houseIdx = *(int*)((char*)owner + OFF_HOUSE_INDEX);

    for (k = 0; k < s_PlanPaxN; k++)
    {
        void* pax = s_PlanPax[k];
        int ms = MISSION_MOVE, tid = -1, uid;
        unsigned char tr = 0, ur = 0;

        uid = OU_ObjTgt(pax, &ur);
        if (uid <= 0) { SU_LogClick("plan uid fail", s_PlanHost, pax, 0); continue; }
        /* 点中自己所在的容器 / 自己: 不当作对象目标 (Enter 回本车无意义), 落回移动。 */
        if (tgt && IsPlausiblePtr(tgt) && tgt != s_PlanHost && tgt != pax)
        {
            int m = OU_NativeMission(pax, tgt);
            if (m == MISSION_ATTACK || m == MISSION_ENTER ||
                m == MISSION_CAPTURE || m == MISSION_SABOTAGE)
            {
                int id = OU_ObjTgt(tgt, &tr);
                if (id >= 0) { ms = m; tid = id; }   /* 解不出 ID 就落回移动 */
            }
        }
        if (!SU_FirePlanNode(houseIdx, pax, uid, ur, ms, tid, tr, cx, cy))
        {
            SU_LogClick("plan fire fail", s_PlanHost, pax, 0);
            continue;
        }
        if (!ok) ms0 = ms;
        ok++;
    }
    if (!ok) return;
    s_PlanFired++;

    q = AppendCStr(buf, q, "su: plan node m=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), ms0);
    q = AppendCStr(buf, q, " x=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), cx);
    q = AppendCStr(buf, q, " y=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), cy);
    q = AppendCStr(buf, q, " n=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), s_PlanFired);
    q = AppendCStr(buf, q, " k=", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), ok);
    q = AppendCStr(buf, q, "/", sizeof(buf));
    q = AppendInt(buf, q, sizeof(buf), s_PlanPaxN);
    LogLine(buf);
}

/* 面板点了本车/本建筑那一格 (第 0/1 格) 且 Z 按住 = 给全部载员规划。
 * 回 1 = 已接管这次点击。不对容器本身下令: 建筑载员时它本来就不能动,
 * 载具不靠面板也能直接在地图上给它下点。 */
int SU_PlanArmAll(void* host)
{
    SU_EnsureInit();
    if (!s_Enable) return 0;
    if (!SU_PlanMode()) return 0;
    return SU_PlanArm(host, 0);
}

/* ---------------- 点击门侧 ---------------- */

int SU_PlanClick(int mx, int my)
{
    (void)mx;
    (void)my;
    /* SU_PlanPoll 取格走的正是这个被钩的函数, 那时鼠标同样按着、s_PlanLb 同样为 1,
     * 不挡住就会把自家这次坐标计算也吞掉 -> 格永远算不出来。 */
    if (s_PlanQuery) return 0;
    if (s_PlanPaxN <= 0) return 0;
    if (!SU_PlanMode() || !SU_PlanStillInside())
    {
        SU_PlanDisarm();
        return 0;
    }
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return 0;
    if (s_PlanLb) return 1;       /* 同一次按下的后续帧: 吞掉, 不重复加点 */
    s_PlanLb = 1;
    s_PlanWant = 1;
    return 1;
}

void SU_PlanPoll(void)
{
    int cx, cy, ok;
    void* tgt = 0;

    if (s_PlanPaxN > 0)
    {
        /* 松开 Z / 载员不在了 / 容器取消选中: 结束这次规划,
         * 提交由引擎自己在 0x637A10 里做。 */
        if (!SU_PlanMode() || !SU_PlanStillInside())
        {
            SU_LogClick(SU_PlanMode() ? "plan end gone" : "plan end z", s_PlanHost,
                        s_PlanPax[0], (void*)(size_t)s_PlanFired);
            SU_PlanDisarm();
        }
        else
        {
            if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) s_PlanLb = 0;
            if (s_PlanWant)
            {
                s_PlanWant = 0;
                /* 格与点中对象只能在 ProcessClickCoords 之外取 (它自己就是被钩的函数)。 */
                s_PlanQuery = 1;
                ok = OU_MouseCellTarget(&cx, &cy, &tgt, 0, 0);
                s_PlanQuery = 0;
                if (!ok) SU_LogClick("plan cell fail", s_PlanHost, s_PlanPax[0], 0);
                else SU_PlanAdd(cx, cy, tgt);
            }
        }
    }
    SU_PadPoll();
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
    if (!s_Enable) return 0;
    /* Z 按住点载员格 = 只给这一个载员开规划 (联机也能用: 只发标准事件)。
     * 必须重建集合: 往现有集合里补会变成整组下点, 就切不回单人。
     * 按住 shift 补人走 UP_Click 的 shift 分支 (SU_PlanResync), 到不了这里。 */
    if (SU_PlanMode())
    {
        if (SU_PlanArm(unit, pax)) return 1;
    }
    if (OU_IsMP()) { SU_LogClick("skip mp", unit, pax, 0); return 0; }
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
        /* 序号能查到即证明 pax 在本车的载员链上 (面板格子就取自这条链),
         * 不必再看 Transporter 字段。 */
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
