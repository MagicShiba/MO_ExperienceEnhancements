/* BuildQueue.cpp - 建筑排队 (本地队列 + 复刻点击, 不碰模拟)。
 *
 * 设计: 队列状态只存 DLL 本地, 下的全是"点击会下的标准 PRODUCE 事件",
 * 各端模拟天然一致 (混装原版端也只看到普通订单), 构造上不影响联机。
 *   - 建筑页/防御页天然分开: 条目按 house+heap 匹配 (heap=BuildingType.ArrayIndex,
 *     唯一), 喂单按 (naval,cat) 解析各页工厂; 取消只动本家本型。
 *   - 条目**不存工厂指针**: 工厂对象会死/换, 一律按 (house,naval,cat) 每轮现解析
 *     槽位 (GetPrimaryFactory 0x500510, 槽位即权威), 故不存在 stale。
 *   - 左键跨型号点击: 忙时走原版拒绝路径 0x6AB689, 吞掉记入本地; 闲时原生下单
 *     第 1 个并顺手记 4 个 (Shift 一次记 5 个, 槽不够记到满)。
* 切换清产的原生 abandon 路 (0x6AA7BC/0x6AA884) 零触发, 不钩 (见文件末说明)。
 *   - 图标数字 (DrawStrip 的 CountTotal 取数点 0x6A9C86): 本地有排队则
 *     EAX/EBX=原版数+本地数后跳 0x6A9CC9 借用原版绘制; 无排队跑原指令。
 *   - 就绪文本左对齐 (DrawStrip Ready/状态分支, 纯渲染补丁, 不碰模拟):
 *     原版同一行 (y 同为 ebp+1) 有三套排版 (esi 为图标 X):
 *     数字 (x=esi+0x3C, flag 0x242 右对齐);
 *     有数字时的状态字 (x=esi+2, flag 0x42 左对齐, 如"等待");
 *     无数字时的状态字/就绪 (x=esi+0x1E, flag 0x142 居中)。
 *     flag 高字节位 0=居中/位 1=右对齐 (0x4A59E0 布局与 0x4A5EB0 绘制
 *     同口径实证: 0x242 右/0x142 中/0x42 左)。
 *     工厂完工且有排队时, 状态块画居中的"就绪" (IsDone→0xD53 经
 *     0x6A9DAB/0x6A9E17) 与右侧数字同行重叠 (CJK 比原版 1bpp 字宽)。
 *     开关开时给状态块加条件排版 (与 0x6A9EE4 处 Ready 块同判据):
 *     本图标画过数字 ([esp+0x13]==1) 则走左分支几何 (x=esi+2,
 *     flag 0x42), 否则走原版居中 (x=esi+0x1E, flag 0x142)。
 *     实现为两处 JMP 桩 (开关开时一次性写入, 指令等长覆盖, 无 rel):
 *       布局点 0x6A9DAB (8B: lea+push) → BQ_StateLay,
 *       绘制点 0x6A9E17 (5B: push) → BQ_StateDraw;
 *     桩内读 [esp+0x13] (布局点前有 push 1/push 2 用 +0x1B,
 *     绘制点前两次调用已平衡用 +0x13, 逐条数 push 实证),
 *     原字节照抄执行后跳回原生后继 (0x6A9DB3/0x6A9E1C)。
 *     无数字的就绪/等待保持原版居中; 有数字时"就绪"居左,
 *     右侧数字底框独占右侧不再被挡。
 *     数字底框本身由 VecText 度量 (maxW<=0 不限宽同样给 GDI 真宽)
 *     保证与落墨同宽、正确包裹。字节不符 (版本/Ares 差异) 则静默跳过
 *     (fail-close 回原版)。
 *   - BuildQueue_Poll (每循环, 挂 PowerDrawBoth): 无对账无丢单, 只喂单 ——
 *     逐条目按 (house,naval,cat) 现解析工厂; 工厂不存在/非本家→留着等
 *     (卖了重建/具现后自动续上); 工厂 Object==null (无在产/待放置) 才复刻一次
 *     点击下单; 同一工厂每轮只喂一项 (事件下帧才执行, 当轮再喂会被原版拒绝,
 *     故用 fed 限流)。每帧另采样 Shift 进 32 帧闩锁 (左键记数用"当前或近期
 *     按下", 防单帧竞态/提前松开)。
  *   - 限造 (rules.ini BuildLimit, 本 mod 大量建筑限 1/2/3): 原版达上限拒收,
  *     本地队列须按同口径算余量 BQ_RoomLeft, 记入时只记还造得出的条数,
  *     每轮喂单前排不进的直接丢 (=原版"点不下"语义, 队列自愈)。
 *   - 右键规则 (Shift=清全部): 点一次暂停, 再点取消; 取消时优先减数 (N>1 弹尾,
 *     在产/暂停不动), 最后剩在产单个走原生真取消。Shift+右键=取消全部: 清空本地
 *     后跑原生 0x2E (在产亦清, 显示归零); 未暂停的按住 Shift 点只先暂停
 *     (随后暂停态再 Shift 点清零), 天然两步防误删。
 *   - 右键真分发 (反汇编定案): [esp+0xB0]&0x10 为右键; [esp+0x18]==null→
 *     0x6AB0AC 明路 (空闲态才到); 在产未暂停→0x6AAFE1 暗暂停 (暂停保持原生);
 *     已暂停/空闲→按 shift 进 0x6AAE4A (0x2E, Shift 清全) 或 0x6AAFBD (0x10,
 *     经 jmp 0x6AB1FF 共享尾构造)。剥离在明取消分支 (0x6AB197, d=1/5/0/8) 与暗
 *     分发点 (0x6AAE44, d=6/7/8) 两处; 明暂停检查 state==1 一律原生暂停。
  *     全程不仿造事件构造 (E8 点一律不碰)。
 *   - Shift 检测走原版键位函数 0x53EC70 (ecx=0x10, AH&0x80 为按下; 与 0x54F25B
 *     处原版 Shift 检测逐字节同用法; GetKeyState 后备)。
 * 待放置时 Object 非空故下一个先等着, 放下后自动开工 (=Phobos 文档行为)。
 * 存档/读档/换 CurrentPlayer 即清空 (未下单=未扣费, 经济一致)。
  * 开关关时钩子全跑原指令、轮询直接返回 (=原版行为)。
  * 日志只剩启动两行 (bqueue: ON + modbase); 逐事件 trace 已全删。
  *
 * 铁律 (Syringe HandleException 实证: 跳板原样拷贝被偷字节, rel/call 不修正;
 * Phobos 全库也从不在 rel 字节上 return 0):
 *   .inj 钩子凡偷了含 rel 的字节 (E8 call / Jcc), 永远不许 return 0;
 *   条件跳转永不跳回钩子自身地址 (那是补丁, 会自递归)。
 *   本文件钩子皆纯传送字节 (0x6AB689 push/or, 0x6AB6CE mov/mov, 0x6AB0C1 Jcc 永跳,
 *   0x6AB197 push/push/mov, 0x6AAE44 Jcc 永跳, 0x6AAFE1 mov, 0x6A9C86 mov/cmp,
 *   0x6A9C6C mov/mov): return 0 跑原指令安全, 跳转只跳前向原生目标。
 *   9/10 字节钩避免非零返回 (疑触 Syringe 短桩问题), 大块改用 Jcc 分发点。
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
 *   CurrentPlayer=*(0xA83D4C)。
 * 限造字段: BuildLimit=[type+0x3B8]; OwnedBuildingTypes=[house+0x5500],
 *   FactoryProducedBuildingTypes=[house+0x55A0] (CounterClass 取值走 0x49FAE0,
 *   参数=类型 ArrayIndex); 在产判定=Factory.Object→vtable+0x84。
 * 点击钩子寄存器 (0x6AB689/0x6AB6CE): EBP=abs (原版 cmp ebp,7 紧邻在前),
 * EDI=图标类型 (0x6AB602 取自 [esp+0x28], 原版 0x6AB6CE 处 [edi+0xCCE]),
 * ESI=工厂 (0x6AB602 处 GetPrimaryFactory 结果, 0x6AB61E mov esi,eax)。
 * 原版右键流 (反汇编实证, 0x6AB0AC-0x6AB0A7, 此处不钩仅记录):
 * state==1 则置 2 发 SUSPEND 后跳 0x6AB28f 退出 (暂停),
 * state!=1 才进 0x6AB18d 取消分支 (IsQueued→0x2E/0x10)。
 *
 * 钩子经 dist/MOExperience.dll.inj 由 Syringe 挂载 (返回 0=跑原指令, 非0=跳往)。
 */

#include <windows.h>
#include "GdiText.h"

#define BQ_SECTION "Features"

/* Syringe REGISTERS 布局 (YRpp Syringe.h)。 */
#define R_EDI 0x08
#define R_ESI 0x0C
#define R_EBP 0x10
#define R_ESP 0x14
#define R_EBX 0x18
#define R_EAX 0x24

#define ABS_BUILDINGTYPE 7u
#define EVT_PRODUCE 0xEu

#define ADDR_CURPLAYER 0xA83D4Cu
#define OFF_HOUSE_INDEX 0x30u
#define OFF_TT_NAVAL 0xCCEu
#define OFF_TT_BUILDLIMIT 0x3B8u
#define OFF_BT_ARRAYINDEX 0xDF8u
#define OFF_FAC_OBJECT 0x58u
#define OFF_FAC_OWNER 0x6Cu
/* HouseClass 限造计数 (CounterClass, 取值走 0x49FAE0)。
 * 0x5500=OwnedBuildingTypes (现拥有, 含在产, 正限判它),
 * 0x55A0=FactoryProducedBuildingTypes (历史造出, 负限判它)。 */
#define OFF_HOUSE_OWNEDBLD 0x5500u
#define OFF_HOUSE_PRODBLD 0x55A0u

#define ADDR_OUT_COUNT 0xA802C8u
#define ADDR_OUT_NEXT 0xA802D0u
#define ADDR_OUT_BUF 0xA802D4u
#define ADDR_OUT_PAR 0xA83A54u
#define EVT_SIZE 111u
#define EVT_CAP 0x80u

#define ADDR_FACARR_ITEMS 0xA83E34u
#define ADDR_FACARR_COUNT 0xA83E40u
/* FactoryClass::Array 布局经构造自注册反汇编实证 (inc-store Count,
 * indexed-store Items, CountTotal 同地址遍历): Items=[0xA83E34],
 * Count=[0xA83E40]。只用于显示求和 (不做存活判断, 无 stale 问题)。 */

#define TO_CLICK_EXIT 0x006AB95Au
#define TO_CANCEL_EXIT 0x006AB28Fu
#define TO_DARK_CANCEL_NATIVE 0x006AAFBDu
#define TO_DARK_ALL_NATIVE 0x006AAE4Au
#define TO_PAUSE_NATIVE 0x006AB0C7u
#define TO_CANCEL_BRANCH 0x006AB18Du
#define TO_DRAWNUM 0x006A9CC9u
#define TO_DRAWSKIP 0x006A9D88u

#define BQ_CAP 50

/* 待发事件窗守卫: Poll 挂渲染 (可快于逻辑帧), 同一工厂连续两轮都可能
 * 见到 Object==null (上轮发的 PRODUCE 还没执行), 连发两单则第二单在
 * 执行侧被 BeginProduction 拒绝 (0x4FA520 起首即 busy+building 返 3),
 * 静默丢单。pend 记住本轮发过单的线, N 轮内不再发 (执行后 Object 非空,
 * 本来就会停; 哑单则 N 轮后重试)。键为 (house,naval,cat), 与喂单同键。 */
#define BQ_PEND_N 45
#define BQ_PEND_SLOTS 8
typedef struct
{
    void* house;
    unsigned naval;
    unsigned buildcat;
    int age;
} BQPend;

static BQPend g_BQPend[BQ_PEND_SLOTS];

static void BQ_PendAge(void)
{
    int s;
    for (s = 0; s < BQ_PEND_SLOTS; s++)
    {
        if (g_BQPend[s].house && g_BQPend[s].age < BQ_PEND_N)
            g_BQPend[s].age++;
    }
}

static void BQ_PendClear(void)
{
    int s;
    for (s = 0; s < BQ_PEND_SLOTS; s++)
        g_BQPend[s].house = 0;
}

static int BQ_Pending(void* house, unsigned naval, unsigned cat)
{
    int s;
    for (s = 0; s < BQ_PEND_SLOTS; s++)
    {
        if (g_BQPend[s].house == house && g_BQPend[s].naval == naval &&
            g_BQPend[s].buildcat == cat && g_BQPend[s].age < BQ_PEND_N)
            return 1;
    }
    return 0;
}

static void BQ_MarkPending(void* house, unsigned naval, unsigned cat)
{
    int s;
    int free = -1;
    for (s = 0; s < BQ_PEND_SLOTS; s++)
    {
        if (g_BQPend[s].house == house && g_BQPend[s].naval == naval &&
            g_BQPend[s].buildcat == cat)
        {
            g_BQPend[s].age = 0;
            return;
        }
        if (!g_BQPend[s].house || g_BQPend[s].age >= BQ_PEND_N)
            free = s;
    }
    if (free < 0) return; /* 槽满: 不记 (fail-open, 等价于无守卫) */
    g_BQPend[free].house = house;
    g_BQPend[free].naval = naval;
    g_BQPend[free].buildcat = cat;
    g_BQPend[free].age = 0;
}

/* 条目不存工厂指针: 每轮按 (house,naval,cat) 现解析槽位, 无 stale。
 * buildcat 取自点击帧原生栈 [esp+0x24] (与原生 GPF 入参同值)。 */
typedef struct
{
    void* house;
    void* type;
    unsigned naval;
    unsigned buildcat;
    int flying; /* 已发单待模拟认领 (见喂单确认制) */
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

/* 状态块条件排版桩 (纯渲染, 见文件头): [esp+0x13] 为 1 (本图标画过
 * 数字) 走左分支, 否则走原版居中。返回地址跳回原生后继。
 * 注意: 裸函数, 不得读写除 EAX 外的寄存器 (调用点后续要用 ECX/EBX),
 * EFLAGS 在桩内外均无存活依赖。 */
static DWORD g_BQBackLay = 0x006A9DB3u;
static DWORD g_BQBackDraw = 0x006A9E1Cu;

__attribute__((naked)) static void BQ_StateLay(void)
{
    __asm__ volatile (
        "cmpb $1, 0x1B(%%esp)\n\t"
        "je 1f\n\t"
        "lea 0x1E(%%esi), %%eax\n\t"
        "push $0x142\n\t"
        "jmp *%0\n\t"
        "1:\n\t"
        "lea 0x2(%%esi), %%eax\n\t"
        "push $0x42\n\t"
        "jmp *%0\n\t"
        : : "m" (g_BQBackLay));
}

__attribute__((naked)) static void BQ_StateDraw(void)
{
    __asm__ volatile (
        "cmpb $1, 0x13(%%esp)\n\t"
        "je 1f\n\t"
        "push $0x142\n\t"
        "jmp *%0\n\t"
        "1:\n\t"
        "push $0x42\n\t"
        "jmp *%0\n\t"
        : : "m" (g_BQBackDraw));
}

/* 写 JMP rel32 桩 (len>=5, 多余字节补 NOP)。调用方先验字节。 */
static void BQ_WriteJmp(void* site, void* stub, unsigned len)
{
    unsigned char* p = (unsigned char*)site;
    DWORD old = 0;
    int rel = (int)((char*)stub - ((char*)site + 5));
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = 0xE9;
    *(int*)(p + 1) = rel;
    {
        unsigned k;
        for (k = 5; k < len; k++) p[k] = 0x90;
    }
    VirtualProtect(p, len, old, &old);
}

static void BQ_ReadyLeftPatch(void)
{
    unsigned char* s1 = (unsigned char*)(size_t)0x6A9DAB;
    unsigned char* s2 = (unsigned char*)(size_t)0x6A9E17;
    /* 8D 46 1E | 68 42 01 00 00 */
    if (s1[0] == 0x8D && s1[1] == 0x46 && s1[2] == 0x1E && s1[3] == 0x68 &&
        s1[4] == 0x42 && s1[5] == 0x01 && s1[6] == 0x00 && s1[7] == 0x00)
        BQ_WriteJmp(s1, (void*)BQ_StateLay, 8);
    /* 68 42 01 00 00 */
    if (s2[0] == 0x68 && s2[1] == 0x42 && s2[2] == 0x01 && s2[3] == 0x00 &&
        s2[4] == 0x00)
        BQ_WriteJmp(s2, (void*)BQ_StateDraw, 5);
}

static void BQ_EnsureInit(void)
{
    if (g_BQInit) return;
    g_BQInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_BQEnable =
        GetPrivateProfileIntA(BQ_SECTION, "BuildQueue", 0, g_IniPath) ? 1 : 0;
    LogLine(g_BQEnable ? "bqueue: local building queue ON"
                       : "bqueue: local building queue OFF");
    if (g_BQEnable) BQ_ReadyLeftPatch();
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
/* 工厂内该型的模拟数 (在产+在排, 原版绘制同调法): thiscall ECX=工厂,
 * 栈参=类型指针。0x4CA670 经原生绘制调用点实证。 */
typedef int __attribute__((thiscall)) (*Fn_CountTotal)(void* factory, void* type);
#define FN_COUNTTOTAL ((Fn_CountTotal)(void*)0x4CA670u)
/* VectorClass::GetItemCount (0x49FAE0): 取限造计数器, 只读。 */
typedef int __attribute__((thiscall)) (*Fn_CounterGet)(void* counter, int index);
#define FN_COUNTERGET ((Fn_CounterGet)(void*)0x49FAE0u)
/* ObjectClass::GetType (vtable+0x84): 判某工厂是否正在造本型。 */
typedef void* __attribute__((thiscall)) (*Fn_ObjGetType)(void* obj);
#define OFF_VT_GETTYPE (0x84 / 4)

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

static void BQ_RemoveAt(int idx);
static int BQ_ClearType(void* house, int heap);
static int BQ_SimCount(void* house, void* type);
static int BQ_LimitOf(void* type);
static int BQ_RoomLeft(void* house, void* type, int others);

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

static void BQ_RemoveAt(int idx)
{
    int k;
    if (idx < 0 || idx >= g_BQCount) return;
    for (k = idx; k + 1 < g_BQCount; k++)
        g_BQList[k] = g_BQList[k + 1];
    g_BQCount--;
}

/* 按 (house,naval,cat) 取主工厂 (与原版点击路径同调用; 槽位即权威,
 * 无 stale。工厂未具现/已死返回空, 调用方留着等, 永不丢单)。 */
static void* BQ_Resolve(void* house, unsigned naval, unsigned cat)
{
    if (!house) return 0;
    return FN_GETPRIMARYFACTORY(house, ABS_BUILDINGTYPE, naval, cat);
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
    /* OutList_Next 是引擎的字段, 值必须是 0..EVT_CAP-1 才配当写下标:
     * 脏值直接算 next*EVT_SIZE 就是一次任意地址写, 故先 mask。*/
    next = *(unsigned*)ADDR_OUT_NEXT & 0x7Fu;
    if (next >= EVT_CAP) return 0;

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
        /* 必须列出 ebx/esi/edi/ebp: 引擎这个回调是真实函数, 会动 callee-saved
         * 寄存器, 而编译器以为它不会 —— 于是下面用 next/count 写 OutList_Count /
         * OutList_Next 时可能拿到垃圾, 引擎同帧按这份坏账本去派发事件就会跳进堆
         * (0xC0000005, EIP 落在非代码区)。*/
        : "ecx", "edx", "ebx", "esi", "edi", "ebp", "cc", "memory");
    *(unsigned*)(ADDR_OUT_PAR + next * 4u) = eaxAfter;

    *(unsigned*)ADDR_OUT_NEXT = (next + 1u) & 0x7Fu;
    *(unsigned*)ADDR_OUT_COUNT = count + 1u;
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

    BQ_EnsureInit();
    g_BQShiftHist = (g_BQShiftHist << 1) | (BQ_ShiftRaw() ? 1u : 0u);
    if (!g_BQEnable) return;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || (g_BQHouse && cur != g_BQHouse))
    {
        g_BQCount = 0; /* 菜单/读档/换人: 丢队列 (未下单=未扣费) */
        g_BQHouse = cur;
        BQ_PendClear();
        return;
    }
    if (!g_BQHouse) g_BQHouse = cur;
    BQ_PendAge();
    if (g_BQCount <= 0) return;
    /* 无对账无丢单: 条目不存工厂指针, 无 stale 可对;
     * 确认制: 发单只 mark flying 不移除, 下轮见模拟认领
     * (CountTotal>0) 才移除; 执行拒收 (忙+建筑直接返 3) 则条目留着、
     * 数字留着, pend 窗后重发。发而不删, 永不哑单丢数 (认领前图标短暂
     * 双计一次, 1~2 轮即消)。 */
    if (g_BQCount <= 0) return;
    for (i = 0; i < g_BQCount; i++)
    {
        BQEntry* e = &g_BQList[i];
        void* factory;
        if (e->house != cur) continue;
        /* 限造余量不足的直接丢 (=原版"点不下"语义, 队列自愈)。 */
        if (BQ_LimitOf(e->type) &&
            BQ_RoomLeft(cur, e->type,
                        BQ_LocalCount(cur, BQ_HeapOf(e->type)) - 1) <= 0)
        {
            BQ_RemoveAt(i);
            i--;
            continue;
        }
        factory = BQ_Resolve(cur, e->naval, e->buildcat);
        if (!factory) continue; /* 工厂不存在: 留着等具现/重建, 不丢单 */
        if (e->flying)
        {
            if (BQ_SimCount(cur, e->type) > 0)
            {
                /* 模拟已认领则删条目并继续 (删后下标顶上, 不可再用旧工厂)。 */
                BQ_RemoveAt(i);
                i--;
                continue;
            }
            if (BQ_Pending(cur, e->naval, e->buildcat))
                continue; /* 等执行 */
            /* 否则 pend 过期未认领: 下沉重发 (经 owner/object 门) */
        }
        else
        {
            if (BQ_Pending(cur, e->naval, e->buildcat)) continue;
        }
        if (*(void**)((char*)factory + OFF_FAC_OWNER) != cur) continue;
        if (*(void**)((char*)factory + OFF_FAC_OBJECT)) continue;
        BQ_MarkPending(cur, e->naval, e->buildcat);
        if (!BQ_FireProduce(cur, e->type)) continue;
        e->flying = 1;
    }
}

/* 记入 N 条本地 (槽位复用须清 flying, 否则残留 1 会被误认领)。 */
static void BQ_PushN(void* house, void* type, unsigned naval, unsigned cat,
                     int n)
{
    int i;
    for (i = 0; i < n; i++)
    {
        g_BQList[g_BQCount].house = house;
        g_BQList[g_BQCount].type = type;
        g_BQList[g_BQCount].naval = naval;
        g_BQList[g_BQCount].buildcat = cat;
        g_BQList[g_BQCount].flying = 0;
        g_BQCount++;
    }
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
    free = BQ_CAP - g_BQCount;
    if (free <= 0) return 0; /* 满: 走原版拒绝 */
    if (want > free) want = free;
    if (!g_BQHouse) g_BQHouse = house;
    if (house != g_BQHouse) return 0;
    /* 限造: 只记余量内的条数 (余量 0 则不记, 交回原版拒绝路径)。 */
    {
        int room = BQ_RoomLeft(house, pType, 0);
        if (want > room) want = room;
    }
    if (want <= 0) return 0;
    BQ_PushN(house, pType,
             *(unsigned char*)((char*)pType + OFF_TT_NAVAL) ? 1u : 0u,
             *(unsigned*)(BQ_Reg(regs, R_ESP) + 0x24u), want);
    return TO_CLICK_EXIT;
}

/* 0x6AB6CE (10): 工厂闲时继续路径 (mov edx,[esp+0x24] + mov al,[edi+0xcce])。
 * 原版只会即时下单 1 个; Shift+左键则顺手记 4 个 (合计 5 个), 跑原指令让
 * 原版下单第 1 个。regs: EBP=abs, EDI=图标类型, ESI=工厂 (可空)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_FreeShift(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    int want;
    int free;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    if (!BQ_ShiftDown()) return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house) return 0;
    /* ESI=工厂可空 (首次下单工厂未具现): 照记不误 */
    want = 4;
    free = BQ_CAP - g_BQCount;
    if (free <= 0) return 0;
    if (want > free) want = free;
    if (!g_BQHouse) g_BQHouse = house;
    if (house != g_BQHouse) return 0;
    /* 限造: 给原生即将下的 1 个留位, 余下只记余量内的条数。 */
    {
        int room = BQ_RoomLeft(house, pType, 1);
        if (want > room) want = room;
    }
    BQ_PushN(house, pType,
             *(unsigned char*)((char*)pType + OFF_TT_NAVAL) ? 1u : 0u,
             *(unsigned*)(BQ_Reg(regs, R_ESP) + 0x24u), want);
    return 0;
}

/* 0x6AB0C1 (6): 右键 state 检查 (jne 0x6AB18D, rel, 永远跳转)。
 * EAX=state 地址 (0x6AB0B8 已算好), EDI=图标类型, EBP=abs。
 * state!=1→跳取消分支 (原生 jne 去向); state==1 (本要暂停):
 * Shift+右键且本地有本型→清空后转取消分支 (原生下 0x2E);
 * 否则一律原生暂停 (d=3 与暂停优先模型一致)。
 * 只读不写。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_PausePeel(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    unsigned stateAddr;
    unsigned state;
    int heap;

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
    {
        unsigned esp = BQ_Reg(regs, R_ESP);
        int bit = (*(unsigned char*)(esp + 0x13u)) ? 1 : 0;
        if (bit && BQ_LocalCount(house, heap) > 0)
        {
            BQ_ClearType(house, heap);
            return TO_CANCEL_BRANCH;
        }
    }
    return TO_PAUSE_NATIVE;
}

/* 0x6AB197 (8): 右键取消分支入口 (push eax + push ecx +
 * mov ecx,[0xa83d4c], 无 rel)。
 * regs: EBP=abs, EDI=图标类型。到此的都是右键 (state!=1 才进取消分支;
 * state==1 的第一次暂停走 SUSPEND 后跳出口, 到不了这里)。
 *   - Shift+右键: 清空本家本型本地, 跑原指令 (原生清模拟)。
 *   - 本地有本型: 弹掉最后一个本地, 跳原生出口 0x6AB28F (在产的留最后)。
 *   - 本地无本型: 看模拟总数: 模拟>1 (本地已 drain, 在产+
 *     在排全在模拟, 此时原生 IsQueued 为真, Shift 常驻更是 AbandonAll)
 *     →拦下直接退, 在产继续; 模拟<=1 (仅剩在产单个或全空) 才跑原指令
 *     (原生暂停后取消/清空, 事件全原生字节)。
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
    {
        /* Shift+右键到明取消分支: 清空本地后跑原生 (原生凭同
         * 字节下 0x2E 清模拟; 空闲态右键才到这里)。c 记原生决策位。 */
        unsigned esp = BQ_Reg(regs, R_ESP);
        int bit = (*(unsigned char*)(esp + 0x13u)) ? 1 : 0;
        if (bit)
        {
            BQ_ClearType(house, heap);
            return 0;
        }
    }
    left = BQ_LocalCount(house, heap);
    if (left <= 0)
    {
        /* 本地无本型: 按总数 (本地+模拟) 决策。本地已 drain、模拟还抱着在产/在排
         * 时放行原生=下发 ABANDON (IsQueued 为真则 0x10, Shift 常驻更是 0x2E
         * AbandonAll), 在产进度作废。故总数>1 时直接退 (在产继续), 总数<=1
         * (仅剩在产单个或全空) 才走原生。 */
        int sim = BQ_SimCount(house, pType);
        if (sim > 1)
        {
            return TO_CANCEL_EXIT;
        }
        return 0;
    }
    left = BQ_PopLast(house, heap);
    return TO_CANCEL_EXIT;
}

/* 真分发 (反汇编定案, 右键点建筑图标):
 *   0x6AADB5: [esp+0xb0]&0x10 为右键 (否则跳原生出口);
 *   0x6AADC3: [esp+0x18]==null→0x6AB0AC 明路 (H0 state 检查, 只在空闲/过渡态到);
 *   0x6AAE0D: [[esp+0x18]+0x38]!=0(在产忙) 且 [[esp+0x18]+0x70]==0(未暂停)
 *     →0x6AAFE1 暗暂停 (SUSPEND#1, 经 0x6AB02A 构造, ec 调用者 0x6AB02F);
 *   否则 (空闲或已暂停) →音效→0x6AAE44 看 [esp+0x13]shift:
 *     无 shift→0x6AAFBD 暗取消 (0x10), 有 shift→0x6AAE4A 暗清全部 (0x2E),
 *     两者经 jmp 0x6AB1FF 共享尾构造。
 * 在产时右键永远走暗路, 明路钩子够不着 —— 剥离必须挂在真分发点上。 */

/* 0x6AAFE1 (5): 暗暂停入口 (mov eax,[0x8871e0], 无 rel)。
 * regs: EDI=图标类型 (存疑时 WhatAmI 守卫), EBP=abs。
 * 纯哨兵: 永远跑原指令 (暂停保持原生)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_DarkPauseSeen(void* regs)
{
    (void)regs;
    BQ_EnsureInit();
    if (!g_BQEnable) return 0;
    return 0;
}

/* 清空本家本型全部本地 (shift+右键清全部用)。返回清空前数量。 */
static int BQ_ClearType(void* house, int heap)
{
    int i;
    int n = 0;
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

/* 暗路剥离: 本地有本型→弹尾跳原生出口; 本地空→模拟>1 拦下; 否则走原生。Shift 方向由分发点处理。 */
static DWORD BQ_DarkPeel(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    int heap;
    int left;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house || g_BQCount <= 0) return 0;
    if (house != g_BQHouse) return 0;
    heap = BQ_HeapOf(pType);
    left = BQ_LocalCount(house, heap);
    if (left <= 0)
    {
        int sim = BQ_SimCount(house, pType);
        if (sim > 1)
        {
            return TO_CANCEL_EXIT;
        }
        return 0;
    }
    left = BQ_PopLast(house, heap);
    return TO_CANCEL_EXIT;
}

/* 0x6AAE44 (6): 右键暗分发 (je 0x6AAFBD, rel, 永远跳转)。
 * 到此=右键+非空闲态 (在产未暂停已由上游 0x6AAE0D 分走暗暂停;
 * 此处为已暂停/空闲)。EDI=图标类型, EBP=abs,
 * [esp+0x13]=shift (0→暗取消 0x6AAFBD, 1→暗清全 0x6AAE4A)。
 * Jcc 永跳 (铁律: 偷 rel 永不 return 0; 9/10 字节钩非零返回疑触 Syringe
 * 短桩问题, 故一律用 6 字节 Jcc 模式)。去向: 剥离跳 0x6AB28F 原生出口;
 * 放行跳回原生去向 (保持栈/寄存器与原生到达一致, 跳过仅为 je 本身)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_DarkDispatchPeel(
    void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    unsigned esp;
    int shift;
    int heap;

    BQ_EnsureInit();
    esp = BQ_Reg(regs, R_ESP);
    shift = (*(unsigned char*)(esp + 0x13u)) ? 1 : 0;
    if (!g_BQEnable)
        return (DWORD)(shift ? TO_DARK_ALL_NATIVE : TO_DARK_CANCEL_NATIVE);
    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType)
        return (DWORD)(shift ? TO_DARK_ALL_NATIVE : TO_DARK_CANCEL_NATIVE);
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE)
        return (DWORD)(shift ? TO_DARK_ALL_NATIVE : TO_DARK_CANCEL_NATIVE);
    house = *(void**)ADDR_CURPLAYER;
    if (!house || house != g_BQHouse)
        return (DWORD)(shift ? TO_DARK_ALL_NATIVE : TO_DARK_CANCEL_NATIVE);
    heap = BQ_HeapOf(pType);
    if (shift)
    {
        /* Shift+右键=取消全部 (用户定稿): 清空本家本型本地后跑
         * 原生 (0x2E 清模拟, 在产亦清, 显示归零, 如 5→0/10→0)。
         * 未暂停的到不了这里 (上游进了暗暂停块先暂停), 天然两步防误删:
         * 松开 shift 点=减一, 按住 shift 点=清零。 */
        BQ_ClearType(house, heap);
        return TO_DARK_ALL_NATIVE;
    }
    {
        DWORD r = BQ_DarkPeel(regs);
        if (r) return r;
    }
    return TO_DARK_CANCEL_NATIVE;
}

/* 0x6A9C6C (6): CountTotal 前工厂空检查 (test eax,eax +
 * mov [esp+0x18],eax, 无 rel)。EAX=GPF 工厂 (可空), [ESP+0x34]=图标类型。
 * 工厂非空→跑原指令; 工厂空 (建筑页没下过单槽位无工厂/绘制按 buildcat=0
 * 取槽而防御页在它槽, 连带被跳过) 且本地有本型→EAX/EBX=模拟全厂和+本地数
 * 后跳绘制块 (只画本地数会漏计在产的那 1 个, 防御页点 5 个
 * 显示 4); 工厂空且本地无→置 [esp+0x18]=0 后跳原生跳过处。
 * [esp+0x18] 在两目标之后无人读, 绘制跳跃无需处理它。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_DrawNullFac(void* regs)
{
    unsigned esp;
    void* pType;
    void* cur;
    unsigned v;
    int n;
    int i;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;
    if (BQ_Reg(regs, R_EAX)) return 0;

    esp = BQ_Reg(regs, R_ESP);
    pType = *(void**)(esp + 0x34u);
    cur = *(void**)ADDR_CURPLAYER;
    n = 0;
    if (pType && cur)
    {
        for (i = 0; i < g_BQCount; i++)
        {
            if (g_BQList[i].house == cur && g_BQList[i].type == pType)
                n++;
        }
    }
    if (n <= 0)
    {
        *(unsigned*)(esp + 0x18u) = 0u;
        return TO_DRAWSKIP;
    }
    /* 工厂空不代表没在产 (槽位不对而已): 全厂求和补上, 与 ShowCount 同口径。 */
    v = (unsigned)BQ_SimCount(cur, pType) + (unsigned)n;
    if (!v) return 0;
    *(unsigned*)((char*)regs + R_EAX) = v;
    *(unsigned*)((char*)regs + R_EBX) = v;
    return TO_DRAWNUM;
}

/* 本家各工厂中该型的模拟数之和。绘制只认单槽工厂会跨页漏计, 全扫才对。 */
static void BQ_CountSimDup(void* house, void* type, int* pSim, int* pDup)
{
    void** items;
    int count;
    int i;
    int sim = 0;
    int dup = 0;
    if (pSim) *pSim = 0;
    if (pDup) *pDup = 0;
    if (!house || !type) return;
    items = *(void***)ADDR_FACARR_ITEMS;
    count = *(int*)ADDR_FACARR_COUNT;
    if (!items || count <= 0) return;
    for (i = 0; i < count; i++)
    {
        void* fac = items[i];
        void* obj;
        void** vt;
        if (!fac) continue;
        if (*(void**)((char*)fac + OFF_FAC_OWNER) != house) continue;
        sim += FN_COUNTTOTAL(fac, type);
        obj = *(void**)((char*)fac + OFF_FAC_OBJECT);
        if (!obj) continue;
        vt = *(void***)obj;
        if (!vt || !vt[OFF_VT_GETTYPE]) continue;
        if (((Fn_ObjGetType)vt[OFF_VT_GETTYPE])(obj) == type) dup++;
    }
    if (dup > sim) dup = sim;
    if (pSim) *pSim = sim;
    if (pDup) *pDup = dup;
}

static int BQ_SimCount(void* house, void* type)
{
    int sim = 0;
    BQ_CountSimDup(house, type, &sim, 0);
    return sim;
}

/* 本型的 rules.ini BuildLimit (0=不限, 正=总数上限, 负=历史上限)。 */
static int BQ_LimitOf(void* type)
{
    if (!type) return 0;
    return *(int*)((char*)type + OFF_TT_BUILDLIMIT);
}

/* 本家本型还能再排几条 (无限制返 BQ_CAP, 不足截 0)。
 * others=同型其余本地条目数 (含已发单未认领的)。正限扣 Owned, 负限扣历史;
 * 在排=各厂 CountTotal 之和再扣掉正在造本型的厂数 (在产已计入计数器)。
 * 全只读, 不碰模拟。 */
static int BQ_RoomLeft(void* house, void* type, int others)
{
    int limit = BQ_LimitOf(type);
    int idx;
    int room;
    int sim = 0;
    int dup = 0;

    if (!limit || !house || !type) return BQ_CAP;
    idx = BQ_HeapOf(type);
    room = (limit > 0) ? limit - FN_COUNTERGET((char*)house + OFF_HOUSE_OWNEDBLD,
                                               idx)
                       : -limit - FN_COUNTERGET((char*)house + OFF_HOUSE_PRODBLD,
                                                idx);
    BQ_CountSimDup(house, type, &sim, &dup);
    room -= sim - dup + others;
    return room > 0 ? room : 0;
}

/* 0x6A9C86 (5): DrawStrip 的 CountTotal 取数点 (mov ebx,eax + cmp ebx,1)。
 * EAX 原生数只认单槽工厂, 跨页漏计, 故本地有排队或原生数为 0 时改用全厂
 * 求和 (sim+n) 后跳绘制块借用原版绘制; 其余 (单位/建筑页单个在产原生数>0)
 * 跑原指令零影响。[ESP+0x34]=图标类型 (调用前后 esp 无净变化)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_ShowCount(void* regs)
{
    unsigned esp;
    void* pType;
    void* cur;
    unsigned v;
    int n;
    int i;
    int sim;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    esp = BQ_Reg(regs, R_ESP);
    pType = *(void**)(esp + 0x34u);
    if (!pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur) return 0;
    n = 0;
    for (i = 0; i < g_BQCount; i++)
    {
        if (g_BQList[i].house == cur && g_BQList[i].type == pType)
            n++;
    }
    if (n <= 0 && BQ_Reg(regs, R_EAX)) return 0;
    sim = BQ_SimCount(cur, pType);
    v = (unsigned)sim + (unsigned)n;
    if (!v) return 0;
    *(unsigned*)((char*)regs + R_EAX) = v;
    *(unsigned*)((char*)regs + R_EBX) = v;
    return TO_DRAWNUM;
}

/* 跨型号点击不切在产 (反汇编实证 0x6AA7B6-0x6AA931 + 日志双证): 它实际永远走
 * "忙拒绝 + 本地记入", 在产从未被切掉。故不钩切换清产, 也不镜像取消/对账工厂
 * 指针 —— 前者会在放置瞬间误清队列 (放置与取消不可分), 后者会在开工换指针时
 * 丢单。 */

