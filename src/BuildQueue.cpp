/* BuildQueue.cpp - 建筑排队 (本地队列 + 复刻点击, 不碰模拟)。
 *
 * 设计: 队列状态只存 DLL 本地, 下的全是"点击会下的标准 PRODUCE 事件",
 * 各端模拟天然一致 (混装原版端也只看到普通订单), 构造上不影响联机。
 *   - 左键点别的建筑, 原生先发 ABANDON (0x6AA7E7) +AbandonAll (0x6AA8CD)
 *     切生产 (在产进度直接作废, 即"从头开始"; 实证见文件尾)。
 *     本 mod 要单位式排队, 必须压住这两处 (0.2.94d 起):
 *     H1 (0x6AA7BC, 6 字节 Jcc, 永远跳转): [[esp+0x2c]]==0 即原生不切条件
 *     →跳 0x6AA87C (寄存器不动, 与原生一致); 开关开→跳 0x6AA84B 并置
 *     EDI=0/ESI=[esp+0x10] (装成"刚走完 abandon"状, 与原生逐寄存器一致,
 *     后续 reset 照跑); 开关关且要切→跳 0x6AA7C2 (原生 abandon 路)。
 *     H2 (0x6AA884, 6 字节 mov 无 rel): 开关开且 EBX 非空→跳 0x6AA931
 *     压住 AbandonAll (ECX 首条即重载, 其余不动, 与原生跳过等价);
 *     否则跑原指令。右键到不了 H1 (暂停 intact 实证: flag==0 走原生跳过);
 *     Shift+右键清模拟仍走 0x6ABxxx 原生分支。E8 点一律不碰。
 *   - 点建筑图标时工厂忙 (原版走拒绝音路径 0x6AB689): 吞掉点击记入本地
 *     队列 (house/type/naval/cat), 跳 0x6AB95A (与原版拒绝同尾, 只跳过
 *     播音; 多路到达对账 esp 一致, 跳过字节不碰 EBX/ESI/EDI/EBP, 尾部先
 *     重载 ECX/EDX, 故等价)。Shift+左键一次记 5 个 (槽不够记到满)。
 *     条目不存工厂指针 (0.2.95d 起): 工厂对象会死/换 (日志实证: 开工后
 *     存指针的 R0 全灭队列), 一律按 (house,naval,cat) 每轮现解析槽位,
 *     不存在 stale。cat 取自点击帧原生栈 [esp+0x24] (与原生 GPF 入参同
 *     值, 非 +0xE08 探针), naval 取 [type+0xCCE] (与原生同偏移)。
 *   - 图标数字 (DrawStrip 的 CountTotal 取数点 0x6A9C86, 5 字节恰两条指令
 *     mov ebx,eax + cmp ebx,1): 本地有排队则 EAX/EBX=原版数+本地数后跳
 *     0x6A9CC9 借用原版绘制 (esp 一致, 绘制块只用 EBX/EBP/ESI/ESP);
 *     本地无排队跑原指令 (单位图标零影响)。
 *   - BuildQueue_Poll (每循环, 挂 PowerDrawBoth): 无对账无丢单, 只喂单:
 *     逐条目按 (house,naval,cat) 现解析槽位工厂 (GPF, 槽位即权威, 无 stale);
 *     工厂不存在/非本家→留着等 (卖了重建/具现后自动续上, 永不丢单);
 *     工厂 Object==null (无在产/待放置) 才复刻一次点击下单; 同一工厂每轮
 *     只喂一项 (事件下帧才执行, 当轮再喂会被原版拒绝, 故用 fed 限流,
 *     fed 按本轮解析出的工厂指针区分)。每帧另采样 Shift 进 32 帧闩锁
 *     (左键记数用"当前或近期按下", 防单帧竞态/提前松开)。
 *   - 右键规则 (用户定稿, 0.2.96d 起): 总数 N>1 时每次右键只减数不动在产
 *     (不暂停不取消); N==1 时走原生 (暂停→取消)。
 *     H0 (0x6AB0C1, 6 字节 Jcc, 永远跳转, state 检查): state!=1→跳取消分支
 *     0x6AB18D; state==1 (本要暂停) 且本地有本型→弹掉最后一个本地后跳
 *     原生出口 0x6AB28F (在产继续, 不暂停); 否则跳 0x6AB0C7 (原生暂停)。
 *     到此 EAX=state 地址 (0x6AB0B8 已算好), EDI=图标类型, EBP=abs,
 *     ESP 入口值 (本段无 push, 三个目标原生到达时 ESP 相同); 只读不写,
 *     各目标首用前均自覆写/自测试, 与原生逐寄存器一致。
 *     H3/CancelPeel (取消分支入口 0x6AB197, 8 字节 push/push/mov 无 rel):
 *     state!=1 才到这里: Shift+右键则清空本家本型本地后跑原指令 (原生
 *     AbandonAll/Abandon 清模拟); 本地有本型则弹掉最后一个本地 (FIFO
 *     剩下有序, 在产的留到最后) 后跳 0x6AB28F 原生出口 (与 IsQueued==0
 *     的退出状态逐寄存器一致: EDI=类型, ESP 入口值, 其余覆写后用);
 *     本地无本型跑原指令 (单个走原生取消, Shift+右键清模拟)。
 *     全程不仿造事件构造 (E8 点一律不碰, 0.2.90d 前仿调用致执行侧崩)。
 *     合计: N>1 每次右键减一 (H0/H3 弹本地, 在产/进度永不碰);
 *     N==1 走原生暂停取消; B 全在本地时右键逐个减 (原生 IsQueued==0
 *     本来直接退出, 数字不动, 现由本钩减)。
 *     (0.2.91d 的 R1 镜像清已删: 放置完成与取消在 Object==null 上不可分,
 *     R1 会在放置瞬间清掉整个队列 ("复活"问题由本钩结构性解决——模拟侧
 *     取消只可能在本地无本型时发出, 无需镜像)。
 *   - 建筑页/防御页天然分开: 条目按 house+heap 匹配 (heap=ArrayIndex 类型
 *     唯一, 同型必同类), 喂单按 (naval,cat) 解析各页工厂; 取消只动本家
 *     本型, 不碰别型 (0.2.95d: R0 丢单 path 已删, 跨型号消失只可能是显示
 *     或原生模拟问题, 本地队列永不跨清)。
 *   - Shift 检测走原版键位函数 0x53EC70 (ecx=0x10, AH&0x80 为按下;
 *     与 0x54F25B 处原版 Shift 检测逐字节同用法; GetKeyState 后备)。
 * 待放置时 Object 非空故下一个先等着, 放下后自动开工 (=Phobos 文档行为)。
 * 存档/读档/换 CurrentPlayer 即清空 (未下单=未扣费, 经济一致)。
 * 开关关时钩子全跑原指令、轮询直接返回 (=原版行为)。
 *
 * 诊断 trace (点击级, 无刷屏): bq: qd/qf (左键记入, a=heap b=want c=raw
 * d=used), bq: qn (工厂未具现仍记入, a=heap b=4 c=raw d=used),
 * bq: cp (右键, a=heap b=剩本地 c=shift d=1弹一/2清空/3暂停剥离),
 * bq: sw (压住左键切换清产, a=heap b=depth),
 * bqueue: queued/fire/unqueued (记入/开工/移除, heap+depth),
 * bqueue: base=模块基址。
 *
 * 铁律 (Syringe HandleException 实证: 跳板原样拷贝被偷字节, rel/call
 * 不修正; Phobos 全库也从不在 rel 字节上 return 0):
 *   .inj 钩子凡偷了含 rel 的字节 (E8 call / Jcc), 永远不许 return 0;
 *   条件跳转永不跳回钩子自身地址 (那是补丁, 会自递归)。
 * 本文件钩子皆纯传送字节 (0x6AA7BC Jcc 永跳, 0x6AA884 mov,
 * 0x6AB689 push/or, 0x6AB6CE mov/mov, 0x6AB0C1 Jcc 永跳,
 * 0x6AB197 push/push/mov, 0x6A9C86 mov/cmp): return 0 跑原指令安全,
 * 跳转只跳前向原生目标。
 *
 * 事件复刻 (与点击路径 0x6AB741-0x6AB7C7 逐字节同义):
 *   EventClass ctor 0x4C6970 (thiscall, this=111 字节栈缓冲,
 *     houseIndex/0xE(Produce)/rtti/heap/naval 入栈),
 *   OutList (0xA802C8 计数, 满 0x80 则下帧重试) 111 字节拷入
 *   0xA802D4+next*111, 同款无参回调 call [0x7E1530] (全二进制数十处同调,
 *   必忽略寄存器), 并行数组 0xA83A54[next]=调用后 EAX (全二进制只写不读),
 *   计数器按原版掩码推进。
 * 参数来源 (MSVC+Yrpp 头 offsetof 探针求值, 原版反汇编交叉验证):
 *   rtti=7 (AbstractType::BuildingType, vtable+0x2C 验证);
 *   heap=[type+0xDF8] (BuildingType.ArrayIndex);
 *   naval=[type+0xCCE] (TechnoType.Naval, 原版两次喂给 GetPrimaryFactory);
 *   houseIndex=[house+0x30] (House.ArrayIndex, 原版事件构造同取);
 *   Factory.Object=+0x58, Factory.Owner=+0x6C (SetObject 写入证实);
 *   工厂槽位: GetPrimaryFactory (0x500510, house/abs/naval/cat→槽位工厂,
 *   槽位即权威); 条目不存工厂指针, 每轮现解析, 故不需要 FactoryClass::Array。
 *   CurrentPlayer=*(0xA83D4C)。
 * 点击钩子寄存器 (0x6AB689/0x6AB6CE): EBP=abs (原版 cmp ebp,7 紧邻在前),
 * EDI=图标类型 (0x6AB602 取自 [esp+0x28], 原版 0x6AB6CE 处 [edi+0xCCE]),
 * ESI=工厂 (0x6AB602 处 GetPrimaryFactory 结果, 0x6AB61E mov esi,eax)。
 * 原版右键流 (反汇编实证, 0x6AB0AC-0x6AB0A7, 此处不钩仅记录):
 * state==1 则置 2 发 SUSPEND 后跳 0x6AB28f 退出 (暂停),
 * state!=1 才进 0x6AB18d 取消分支 (IsQueued→0x2E/0x10)。
 *
 * 钩子经 dist/MsgFont.dll.inj 由 Syringe 挂载 (返回 0=跑原指令, 非0=跳往)。
 */

#include <windows.h>
#include "GdiText.h"

#define BQ_SECTION "BuildQueue"

/* Syringe REGISTERS 布局 (YRpp Syringe.h)。 */
#define R_EDI 0x08
#define R_ESI 0x0C
#define R_EBP 0x10
#define R_ESP 0x14
#define R_EBX 0x18
#define R_ECX 0x20
#define R_EAX 0x24

#define ABS_BUILDINGTYPE 7u
#define EVT_PRODUCE 0xEu

#define ADDR_CURPLAYER 0xA83D4Cu
#define OFF_HOUSE_INDEX 0x30u
#define OFF_TT_NAVAL 0xCCEu
#define OFF_BT_ARRAYINDEX 0xDF8u
#define OFF_FAC_OBJECT 0x58u
#define OFF_FAC_OWNER 0x6Cu

#define ADDR_OUT_COUNT 0xA802C8u
#define ADDR_OUT_NEXT 0xA802D0u
#define ADDR_OUT_BUF 0xA802D4u
#define ADDR_OUT_PAR 0xA83A54u
#define EVT_SIZE 111u
#define EVT_CAP 0x80u

#define TO_CLICK_EXIT 0x006AB95Au
#define TO_CANCEL_EXIT 0x006AB28Fu
#define TO_PAUSE_NATIVE 0x006AB0C7u
#define TO_CANCEL_BRANCH 0x006AB18Du
#define TO_DRAWNUM 0x006A9CC9u
#define TO_NOSWITCH_RESET 0x006AA84Bu
#define TO_NOSWITCH_SKIP 0x006AA87Cu
#define TO_SWITCH_ABANDON 0x006AA7C2u
#define TO_NOSWITCH_ALLSKIP 0x006AA931u

#define BQ_CAP 16

/* 条目不存工厂指针: 每轮按 (house,naval,cat) 现解析槽位, 无 stale。
 * buildcat 取自点击帧原生栈 [esp+0x24] (与原生 GPF 入参同值)。 */
typedef struct
{
    void* house;
    void* type;
    unsigned naval;
    unsigned buildcat;
} BQEntry;

static int g_BQInit = 0;
static int g_BQEnable = 0;
static BQEntry g_BQList[BQ_CAP];
static int g_BQCount = 0;
static void* g_BQHouse = 0;
/* Shift 闩锁: Poll 每帧采样 1 位, 32 帧窗口。左键用闩锁 (防单帧竞态/
 * 提前松开), 右键清空只认当前按下 (防 0.5s 内的误删)。 */
static unsigned g_BQShiftHist = 0;

static void* BQ_ModBase(void)
{
    static void* base = 0;
    if (!base)
    {
        HMODULE h = 0;
        GetModuleHandleExA(0x00000004u, (LPCSTR)BQ_ModBase, &h);
        base = h ? (void*)h : (void*)0xFFFFFFFFu;
    }
    return base;
}

static void BQ_EnsureInit(void)
{
    if (g_BQInit) return;
    g_BQInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_BQEnable =
        GetPrivateProfileIntA(BQ_SECTION, "Enable", 0, g_IniPath) ? 1 : 0;
    LogLine(g_BQEnable ? "bqueue: local building queue ON"
                       : "bqueue: local building queue OFF");
    {
        char mb[48], hx[16];
        unsigned q = 0;
        q = AppendCStr(mb, q, "bqueue: modbase=", sizeof(mb));
        HtoA((unsigned)(size_t)BQ_ModBase(), hx);
        q = AppendCStr(mb, q, hx, sizeof(mb));
        (void)q;
        LogLine(mb);
    }
}

static unsigned BQ_Reg(void* regs, unsigned off)
{
    return *(unsigned*)((char*)regs + off);
}

typedef int __attribute__((thiscall)) (*Fn_WhatAmI)(void* self);
typedef void* __attribute__((thiscall)) (*Fn_EventCtor)(void* ev, int houseIdx, int evtType, int rtti, int heap, int naval);
typedef void (__attribute__((cdecl)) *Fn_SndCb)(void);
typedef unsigned short __attribute__((fastcall)) (*Fn_KeyState)(unsigned key);
typedef void* __attribute__((thiscall)) (*Fn_GetPrimaryFactory)(void* house, unsigned abs, unsigned naval, unsigned buildcat);

#define FN_EVENTCTOR ((Fn_EventCtor)(void*)0x4C6970u)
#define FN_GETPRIMARYFACTORY ((Fn_GetPrimaryFactory)(void*)0x500510u)

static int BQ_WhatAmI(void* pType)
{
    void** vt;
    Fn_WhatAmI fn;
    if (!pType) return -1;
    vt = *(void***)pType;
    if (!vt) return -1;
    fn = (Fn_WhatAmI)vt[0x2C / 4];
    if (!fn) return -1;
    return fn(pType);
}

static int BQ_HeapOf(void* pType)
{
    return *(int*)((char*)pType + OFF_BT_ARRAYINDEX);
}

/* 原版键位检测 (0x53EC70, ecx=键码, 返回 AX, AH&0x80 为按下;
 * 与 0x54F25B 处原版 Shift 检测逐字节同用法)。 */
static int BQ_ShiftRaw(void)
{
    unsigned r = ((Fn_KeyState)(void*)0x53EC70u)(0x10u);
    return (r & 0x8000u) ? 1 : 0;
}

/* 点击用 Shift: 当前按下或 32 帧窗口内按过 (Poll 每帧采样)。 */
static int BQ_ShiftDown(void)
{
    if (BQ_ShiftRaw()) return 1;
    return g_BQShiftHist != 0u ? 1 : 0;
}

static void BQ_Trace(const char* head, int a, int b, int c, int d)
{
    char buf[96], b1[16], b2[16], b3[16], b4[16];
    unsigned q = 0;
    q = AppendCStr(buf, q, head, sizeof(buf));
    HtoA((unsigned)a, b1);
    q = AppendCStr(buf, q, " a=", sizeof(buf));
    q = AppendCStr(buf, q, b1, sizeof(buf));
    HtoA((unsigned)b, b2);
    q = AppendCStr(buf, q, " b=", sizeof(buf));
    q = AppendCStr(buf, q, b2, sizeof(buf));
    HtoA((unsigned)c, b3);
    q = AppendCStr(buf, q, " c=", sizeof(buf));
    q = AppendCStr(buf, q, b3, sizeof(buf));
    HtoA((unsigned)d, b4);
    q = AppendCStr(buf, q, " d=", sizeof(buf));
    q = AppendCStr(buf, q, b4, sizeof(buf));
    (void)q;
    LogLine(buf);
}

static void BQ_Log2(const char* head, int heap, int depth);
static void BQ_RemoveAt(int idx);

/* 本家本型本地数 (取消钩用, heap 匹配)。 */
static int BQ_LocalCount(void* house, int heap)
{
    int i, n = 0;
    if (!house) return 0;
    for (i = 0; i < g_BQCount; i++)
    {
        if (g_BQList[i].house == house &&
            BQ_HeapOf(g_BQList[i].type) == heap)
            n++;
    }
    return n;
}

/* 弹掉本家本型最后一个本地 (右键从队尾减, FIFO 剩下有序)。
 * 返回弹出后剩余数。 */
static int BQ_PopLast(void* house, int heap)
{
    int i;
    if (!house) return 0;
    for (i = g_BQCount - 1; i >= 0; i--)
    {
        if (g_BQList[i].house == house &&
            BQ_HeapOf(g_BQList[i].type) == heap)
        {
            BQ_RemoveAt(i);
            break;
        }
    }
    return BQ_LocalCount(house, heap);
}

/* 清空本家本型本地, 返回清掉数 (Shift+右键用)。 */
static int BQ_ClearType(void* house, int heap)
{
    int i, n = 0;
    if (!house) return 0;
    for (i = g_BQCount - 1; i >= 0; i--)
    {
        if (g_BQList[i].house == house &&
            BQ_HeapOf(g_BQList[i].type) == heap)
        {
            BQ_RemoveAt(i);
            n++;
        }
    }
    return n;
}

static void BQ_RemoveAt(int idx)
{
    int k;
    int heap;
    if (idx < 0 || idx >= g_BQCount) return;
    heap = BQ_HeapOf(g_BQList[idx].type);
    for (k = idx; k + 1 < g_BQCount; k++)
        g_BQList[k] = g_BQList[k + 1];
    g_BQCount--;
    BQ_Log2("bqueue: unqueued", heap, g_BQCount);
}

/* 按 (house,naval,cat) 取主工厂 (与原版点击路径同调用; 槽位即权威,
 * 无 stale。工厂未具现/已死返回空, 调用方留着等, 永不丢单)。 */
static void* BQ_Resolve(void* house, unsigned naval, unsigned cat)
{
    if (!house) return 0;
    return FN_GETPRIMARYFACTORY(house, ABS_BUILDINGTYPE, naval, cat);
}

static void BQ_Log2(const char* head, int heap, int depth)
{
    char buf[64], b1[16], b2[16];
    unsigned q;
    q = 0;
    q = AppendCStr(buf, q, head, sizeof(buf));
    HtoA((unsigned)heap, b1);
    q = AppendCStr(buf, q, " heap=", sizeof(buf));
    q = AppendCStr(buf, q, b1, sizeof(buf));
    HtoA((unsigned)depth, b2);
    q = AppendCStr(buf, q, " depth=", sizeof(buf));
    q = AppendCStr(buf, q, b2, sizeof(buf));
    (void)q;
    LogLine(buf);
}

/* 复刻一次点击下单 (PRODUCE 事件手工进 OutList, 见文件头)。
 * OutList 满返回 0 (下帧重试), 否则返回 1。 */
static int BQ_FireProduce(void* house, void* type)
{
    unsigned count;
    unsigned next;
    char ev[EVT_SIZE];
    int houseIdx;
    int heap;
    int naval;
    unsigned i;
    Fn_SndCb snd;
    unsigned eaxAfter;

    count = *(unsigned*)ADDR_OUT_COUNT;
    if (count >= EVT_CAP) return 0;
    next = *(unsigned*)ADDR_OUT_NEXT;

    houseIdx = *(int*)((char*)house + OFF_HOUSE_INDEX);
    heap = BQ_HeapOf(type);
    naval = *(unsigned char*)((char*)type + OFF_TT_NAVAL) ? 1 : 0;
    FN_EVENTCTOR(ev, houseIdx, EVT_PRODUCE, ABS_BUILDINGTYPE, heap, naval);

    for (i = 0; i < EVT_SIZE; i++)
        ((char*)(ADDR_OUT_BUF + next * EVT_SIZE))[i] = ev[i];

    /* 同款无参回调 + 取调用后 EAX 存并行数组 (与原版同序)。 */
    snd = *(Fn_SndCb*)0x7E1530u;
    __asm__ volatile (
        "call *%1\n\t"
        "movl %%eax, %0"
        : "=r" (eaxAfter)
        : "r" (snd)
        : "ecx", "edx", "cc", "memory");
    *(unsigned*)(ADDR_OUT_PAR + next * 4u) = eaxAfter;

    *(unsigned*)ADDR_OUT_NEXT = (next + 1u) & 0x7Fu;
    *(unsigned*)ADDR_OUT_COUNT = count + 1u;
    BQ_Log2("bqueue: fire", heap, g_BQCount);
    return 1;
}

/* 每循环轮询 (挂 PowerDrawBoth)。首行采样 Shift 进闩锁 (开关关/
 * 队列空也采样, 保证闩锁连续); 之后开关关/队列空直接返回。
 * 对账 (R0) 在喂单之前, 倒序扫: 工厂已死/易主→丢弃 (数字不能留给不存
 * 在的工厂)。就绪/待放置 (Object 非空) 只等放置, 不动队列: 放下后
 * Object==null, 下一轮自动开工 (=Phobos 文档行为)。 */
void BuildQueue_Poll(void)
{
    void* cur;
    int i;
    void* fed;

    BQ_EnsureInit();
    g_BQShiftHist = (g_BQShiftHist << 1) | (BQ_ShiftRaw() ? 1u : 0u);
    if (!g_BQEnable || g_BQCount <= 0) return;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || cur != g_BQHouse)
    {
        g_BQCount = 0; /* 菜单/读档/换人: 丢队列 (未下单=未扣费) */
        g_BQHouse = cur;
        return;
    }
    /* 无对账无丢单 (0.2.95d): 条目不存工厂指针, 无 stale 可对;
     * 跨型号消失只可能来自 pop/clear (heap 键, 有 cp trace) 或换 house
     * 清空, 均有日志可查。 */
    if (g_BQCount <= 0) return;
    fed = 0;
    for (i = 0; i < g_BQCount; i++)
    {
        BQEntry* e = &g_BQList[i];
        void* factory;
        int k;
        if (e->house != cur) continue;
        factory = BQ_Resolve(cur, e->naval, e->buildcat);
        if (!factory) continue; /* 工厂不存在: 留着等具现/重建, 不丢单 */
        if (factory == fed) continue; /* 本轮已喂过该工厂 (事件下帧才执行) */
        if (*(void**)((char*)factory + OFF_FAC_OWNER) != cur) continue;
        if (*(void**)((char*)factory + OFF_FAC_OBJECT)) continue;
        if (!BQ_FireProduce(cur, e->type)) continue;
        fed = factory;
        for (k = i; k + 1 < g_BQCount; k++)
            g_BQList[k] = g_BQList[k + 1];
        g_BQCount--;
        i--;
    }
}

/* 0x6AA7BC (6): 左键切换 abandon 门 (je 0x6AA87C, rel, 永远跳转)。
 * EAX=[[esp+0x2c]] (0x6AA7B6 已取)。flag==0 即原生不切→跳原生跳过处;
 * 开关开→跳原生 reset 处并摆 EDI=0/ESI=[esp+0x10] (与走完 abandon 到此
 * 一致, 后续 AbandonAll 由 H2 压住); 开关关→跳原生 abandon 路。
 * 只读栈+本地开关, 不仿造事件, 不碰 E8。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_NoSwitchAbandon(void* regs)
{
    unsigned flagAddr;
    unsigned flag;
    unsigned esp;

    BQ_EnsureInit();
    flagAddr = BQ_Reg(regs, R_EAX);
    flag = flagAddr ? *(unsigned*)flagAddr : 0u;
    if (!flag) return TO_NOSWITCH_SKIP;
    if (!g_BQEnable) return TO_SWITCH_ABANDON;
    esp = BQ_Reg(regs, R_ESP);
    *(unsigned*)((char*)regs + R_EDI) = 0u;
    *(unsigned*)((char*)regs + R_ESI) = *(unsigned*)(esp + 0x10u);
    return TO_NOSWITCH_RESET;
}

/* 0x6AA884 (6): AbandonAll 前 naval 取数 (mov cl,[ebx+0xcce], 无 rel)。
 * regs: EBX=点击类型 (可空)。开关开且 EBX 非空→跳原生跳过处压住切换
 * 清产; 否则跑原指令 (EBX 空时原生同样跳过, 等价)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_NoSwitchAbandonAll(void* regs)
{
    void* clicked;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;
    clicked = (void*)BQ_Reg(regs, R_EBX);
    if (!clicked) return 0;
    BQ_Trace("bq: sw", BQ_HeapOf(clicked), g_BQCount, 0, 1);
    return TO_NOSWITCH_ALLSKIP;
}

/* 0x6AB689 (5): 工厂忙时点建筑图标, 吞掉点击记入本地队列, 跳拒绝同尾。
 * regs: EBP=abs, EDI=图标类型, ESI=工厂。
 * Shift+左键一次记 5 个 (槽不够记到满, 满则走原版拒绝)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_QueueClick(void* regs)
{
    unsigned abs;
    void* pType;
    void* factory;
    void* house;
    int want;
    int free;
    int i;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    factory = (void*)BQ_Reg(regs, R_ESI);
    if (abs != ABS_BUILDINGTYPE || !pType || !factory) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house) return 0;
    want = BQ_ShiftDown() ? 5 : 1;
    BQ_Trace("bq: qd", BQ_HeapOf(pType), want, BQ_ShiftRaw(),
             BQ_ShiftDown());
    free = BQ_CAP - g_BQCount;
    if (free <= 0) return 0; /* 满: 走原版拒绝 */
    if (want > free) want = free;
    if (!g_BQHouse) g_BQHouse = house;
    if (house != g_BQHouse) return 0;
    for (i = 0; i < want; i++)
    {
        g_BQList[g_BQCount].house = house;
        g_BQList[g_BQCount].type = pType;
        g_BQList[g_BQCount].naval =
            *(unsigned char*)((char*)pType + OFF_TT_NAVAL) ? 1u : 0u;
        g_BQList[g_BQCount].buildcat =
            *(unsigned*)(BQ_Reg(regs, R_ESP) + 0x24u);
        g_BQCount++;
    }
    BQ_Log2("bqueue: queued", BQ_HeapOf(pType), g_BQCount);
    return TO_CLICK_EXIT;
}

/* 0x6AB6CE (10): 工厂闲时继续路径 (mov edx,[esp+0x24] + mov al,[edi+0xcce])。
 * 原版只会即时下单 1 个; Shift+左键则顺手记 4 个 (合计 5 个), 跑原指令让
 * 原版下单第 1 个。regs: EBP=abs, EDI=图标类型, ESI=工厂 (可空)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_FreeShift(void* regs)
{
    unsigned abs;
    void* pType;
    void* factory;
    void* house;
    int want;
    int free;
    int i;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    factory = (void*)BQ_Reg(regs, R_ESI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    if (!BQ_ShiftDown())
    {
        BQ_Trace("bq: qf", BQ_HeapOf(pType), 0, BQ_ShiftRaw(), 0);
        return 0;
    }
    house = *(void**)ADDR_CURPLAYER;
    if (!house) return 0;
    if (!factory) /* ESI 可空 (首次下单工厂未具现): 记 trace, 照记不误 */
        BQ_Trace("bq: qn", BQ_HeapOf(pType), 4, BQ_ShiftRaw(),
                 BQ_ShiftDown());
    want = 4;
    free = BQ_CAP - g_BQCount;
    if (free <= 0) return 0;
    if (want > free) want = free;
    if (!g_BQHouse) g_BQHouse = house;
    if (house != g_BQHouse) return 0;
    for (i = 0; i < want; i++)
    {
        g_BQList[g_BQCount].house = house;
        g_BQList[g_BQCount].type = pType;
        g_BQList[g_BQCount].naval =
            *(unsigned char*)((char*)pType + OFF_TT_NAVAL) ? 1u : 0u;
        g_BQList[g_BQCount].buildcat =
            *(unsigned*)(BQ_Reg(regs, R_ESP) + 0x24u);
        g_BQCount++;
    }
    BQ_Trace("bq: qf", BQ_HeapOf(pType), want, BQ_ShiftRaw(), 1);
    BQ_Log2("bqueue: queued", BQ_HeapOf(pType), g_BQCount);
    return 0;
}

/* 0x6AB0C1 (6): 右键 state 检查 (jne 0x6AB18D, rel, 永远跳转)。
 * EAX=state 地址 (0x6AB0B8 已算好), EDI=图标类型, EBP=abs。
 * state!=1→跳取消分支 (原生 jne 去向); state==1 (本要暂停):
 * 开关开且本地有本型→弹掉最后一个本地后跳原生出口 (减数不暂停,
 * 在产继续); 否则跳原生暂停。只读不写, 三目标与原生到达状态一致。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_PausePeel(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    unsigned stateAddr;
    unsigned state;
    int heap;
    int left;

    BQ_EnsureInit();
    stateAddr = BQ_Reg(regs, R_EAX);
    state = stateAddr ? *(unsigned*)stateAddr : 0u;
    if (state != 1u) return TO_CANCEL_BRANCH;
    if (!g_BQEnable) return TO_PAUSE_NATIVE;
    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return TO_PAUSE_NATIVE;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return TO_PAUSE_NATIVE;
    house = *(void**)ADDR_CURPLAYER;
    if (!house || house != g_BQHouse || g_BQCount <= 0)
        return TO_PAUSE_NATIVE;
    heap = BQ_HeapOf(pType);
    left = BQ_LocalCount(house, heap);
    if (left <= 0) return TO_PAUSE_NATIVE;
    left = BQ_PopLast(house, heap);
    BQ_Trace("bq: cp", heap, left, 0, 3);
    return TO_CANCEL_EXIT;
}

/* 0x6AB197 (8): 右键取消分支入口 (push eax + push ecx +
 * mov ecx,[0xa83d4c], 无 rel)。
 * regs: EBP=abs, EDI=图标类型。到此的都是右键 (state!=1 才进取消分支;
 * state==1 的第一次暂停走 SUSPEND 后跳出口, 到不了这里)。
 *   - Shift+右键: 清空本家本型本地, 跑原指令 (原生清模拟)。
 *   - 本地有本型: 弹掉最后一个本地, 跳原生出口 0x6AB28F (在产的留最后)。
 *   - 本地无本型: 跑原指令 (原生暂停后取消/清空, 事件全原生字节)。
 * 全程只读寄存器+本地队列, 不仿造事件, 不碰 E8。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_CancelPeel(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    int heap;
    int left;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house || g_BQCount <= 0) return 0;
    if (house != g_BQHouse) return 0;
    heap = BQ_HeapOf(pType);
    if (BQ_ShiftRaw())
    {
        left = BQ_ClearType(house, heap);
        BQ_Trace("bq: cp", heap, left, 1, 2);
        return 0;
    }
    left = BQ_LocalCount(house, heap);
    if (left <= 0) return 0;
    left = BQ_PopLast(house, heap);
    BQ_Trace("bq: cp", heap, left, 0, 1);
    return TO_CANCEL_EXIT;
}

/* 0x6A9C86 (5): DrawStrip 的 CountTotal 取数点 (mov ebx,eax + cmp ebx,1)。
 * 本地有排队则 EAX/EBX=原版数+本地数后跳绘制块借用原版绘制;
 * 否则跑原指令。[ESP+0x34]=图标类型 (调用前后 esp 无净变化)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_ShowCount(void* regs)
{
    unsigned esp;
    void* pType;
    void* cur;
    unsigned v;
    int n;
    int i;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    esp = BQ_Reg(regs, R_ESP);
    pType = *(void**)(esp + 0x34u);
    if (!pType) return 0;
    cur = *(void**)ADDR_CURPLAYER;
    n = 0;
    if (cur)
    {
        for (i = 0; i < g_BQCount; i++)
        {
            if (g_BQList[i].house == cur && g_BQList[i].type == pType)
                n++;
        }
    }
    if (n <= 0) return 0;
    v = BQ_Reg(regs, R_EAX) + (unsigned)n;
    *(unsigned*)((char*)regs + R_EAX) = v;
    *(unsigned*)((char*)regs + R_EBX) = v;
    return TO_DRAWNUM;
}

/* 取消/切换钩历史: 0.2.90d 前 BQ_AbandonCheck / BQ_ForceQueued /
 * BQ_AbandonBuild / BQ_AbandonAll / BQ_AbandonSingle 钩 E8 事件构造点并
 * 仿调用, 造畸形事件致执行侧崩, 已全删; 0.2.91d 的 R1 镜像清 (hadSim)
 * 会在放置瞬间清掉整个队列 (放置与取消不可分), 已删。
 * 反汇编实证 (0x6AA7B6-0x6AA931): 左键点建筑图标, 原生先按 [[esp+0x2c]]
 * 发 ABANDON (0x6AA7E7, evt=0x10) +AbandonAll (0x6AA8CD, evt=0x2E) 切掉
 * 在产, 再进 0x6ABxxx 下新单——即"点别的建筑从头开始"。现左键队列语义
 * (H1/H2 压住切换, 在产只由右键/放置终结), 右键暂停取消走 0x6ABxxx 原生;
 * 现右键只剩 BQ_CancelPeel, 左键切换只剩 H1/H2, E8 点无钩。 */

