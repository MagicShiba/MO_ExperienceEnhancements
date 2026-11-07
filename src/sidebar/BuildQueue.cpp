/* BuildQueue.cpp - 建筑排队 (本地队列 + 复刻点击, 不碰模拟)。
 *
 * 队列状态只存 DLL 本地, 下的全是"点击会下的标准 PRODUCE 事件", 各端模拟天然一致
 * (混装原版端也只看到普通订单)。地址表、事件布局、限造口径见
 * notes/BuildQueue建造队列.md; 这里只留改代码要守的约束。
 *
 *   - 条目按 house+heap 匹配 (heap=BuildingType.ArrayIndex, 唯一), 建筑页/防御页
 *     天然分开; 喂单按 (naval,cat) 每轮现解析各页工厂。
 *   - 条目**不存工厂指针**: 工厂对象会死/换, 一律现解析 (GetPrimaryFactory
 *     0x500510 只读 house 的缓存槽, 槽空照发 —— 引擎拒单会清槽)。认领扫
 *     FactoryClass::Array, 判工厂 Object 的类型 = 本条类型, 不看槽。
 *   - 左键跨型号点击: 忙时走原版拒绝路径 0x6AB689 并记入本地; 闲时原生下单第 1 个
 *     并顺手记 4 个 (Shift 一次记 5 个, 槽不够记到满)。
 *   - 限造 (BuildLimit): 记入时只记还造得出的条数, 喂单前排不进的直接丢 (=原版
 *     "点不下", 队列自愈)。权限复查用 HouseClass::CanBuild (0x4F7870): ==0 丢,
 *     >0 (含 -1 临时灰化) 不动。
 *   - 右键: 点一次暂停, 再点取消; 取消优先减数 (在产/暂停不动), 最后剩在产单个走
 *     原生真取消。Shift+右键=清全部。真实分发见下方"真分发"说明。
 *   - 图标数字 (0x6A9C86) 与就绪文本左对齐 (0x6A9DAB/0x6A9E17 两处 JMP 桩) 是纯
 *     渲染补丁, 不碰模拟。
 *   - 开关关时钩子全跑原指令、轮询直接返回 (=原版行为)。
 *
 * 铁律: 钩子凡偷了含 rel 的字节 (E8 call / Jcc), 永远不许 return 0 —— Syringe 把
 *   副本搬进桩池且不重定位 rel32, 跑副本会跳野地址; 条件跳转永不跳回钩子自身。
 *   本文件钩点皆无 rel (0x6AB689/0x6AB6CE/0x6AB197/0x6A9C86/0x6A9C6C),
 *   两个 Jcc 点 (0x6AB0C1/0x6AAE44) 永远跳转。
 *
 * 事件复刻 (与 0x6AB741-0x6AB7C7 逐字节同义): EventClass ctor 0x4C6970 (thiscall,
 *   111 字节栈缓冲), OutList 计数 0xA802C8 / 缓冲 0xA802D4 / 回调 [0x7E1530];
 *   rtti=7, heap=[type+0xDF8], naval=[type+0xCCE], houseIndex=[house+0x30]。
 *   点击钩子寄存器: EBP=abs, EDI=图标类型, ESI=工厂。
 *
 * 钩子经 dist/MOExperience.dll.inj 由 Syringe 挂载 (返回 0=跑原指令, 非0=跳往)。
 */

#include <windows.h>
#include "EngineAddr.h"
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

#define ADDR_FACARR_ITEMS 0xA83E34u
#define ADDR_FACARR_COUNT 0xA83E40u
/* FactoryClass::Array: Items=[0xA83E34], Count=[0xA83E40]。
 * 显示求和与认领扫描共用 (只读, 不做存活判断)。 */

#define TO_CLICK_EXIT 0x006AB95Au
#define TO_CANCEL_EXIT 0x006AB28Fu
#define TO_DARK_CANCEL_NATIVE 0x006AAFBDu
#define TO_DARK_ALL_NATIVE 0x006AAE4Au
#define TO_PAUSE_NATIVE 0x006AB0C7u
#define TO_CANCEL_BRANCH 0x006AB18Du
#define TO_DRAWNUM 0x006A9CC9u
#define TO_DRAWSKIP 0x006A9D88u

#define BQ_CAP 50

/* 重发节流: 同一条产线 (house,naval,cat) 发单后至少隔 2 个逻辑帧。
 * 联机事件还会经过 DoList 等待目标执行帧, 是否仍在途必须读标准事件队列,
 * 不能用这两帧当作执行确认。Poll 挂在渲染 prepare 上, 节流只按逻辑帧。 */
#define BQ_PEND_FRAMES 2
/* 主工厂缓存槽为空时的重发间隔: 引擎可能拒单并清空槽, 每 2 帧照发会一直
 * 刷事件。单被接受后条目立刻被认领, 这个间隔不拖慢正常队列。 */
#define BQ_BLIND_FRAMES 15
#define BQ_PEND_SLOTS 8
typedef struct
{
    void* house;
    unsigned naval;
    unsigned buildcat;
    int frame; /* 上次发单的逻辑帧 (ADDR_FRAME); 帧号倒退=读档/新局, 视为过期 */
} BQPend;

static BQPend g_BQPend[BQ_PEND_SLOTS];

static int BQ_Frame(void)
{
    return *(int*)ADDR_FRAME;
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
    int fr = BQ_Frame();
    for (s = 0; s < BQ_PEND_SLOTS; s++)
    {
        int age;
        if (g_BQPend[s].house != house || g_BQPend[s].naval != naval ||
            g_BQPend[s].buildcat != cat)
            continue;
        age = fr - g_BQPend[s].frame;
        if (age < 0) return 0;
        return (age < BQ_PEND_FRAMES) ? 1 : 0;
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
            g_BQPend[s].frame = BQ_Frame();
            return;
        }
        if (!g_BQPend[s].house && free < 0) free = s;
    }
    if (free < 0) return; /* 槽满: 不记 (fail-open, 等价于无守卫) */
    g_BQPend[free].house = house;
    g_BQPend[free].naval = naval;
    g_BQPend[free].buildcat = cat;
    g_BQPend[free].frame = BQ_Frame();
}

/* 条目按 (house,type,naval,cat) 定位产线; buildcat 取自点击帧原生栈
 * [esp+0x24] (与原生 GPF 入参同值)。 */
typedef struct
{
    void* house;
    void* type;
    unsigned naval;
    unsigned buildcat;
    int flying; /* 已发单, 等待该产线的 CountTotal 增长 */
    int fired;  /* 最近发送帧 (缓存槽空时限重发) */
    int baseline; /* 最近一次发送前该类型的模拟总数 */
} BQEntry;

static int g_BQInit = 0;
static int g_BQEnable = 0;
static BQEntry g_BQList[BQ_CAP];
static int g_BQCount = 0;
static void* g_BQHouse = 0;
/* Shift 近按窗: Poll 记下最近采样到按下的逻辑帧, 窗口 2 帧。
 * 左键用"当前按下或刚松开" (防单帧竞态/提前松开), 右键清空只认当前按下。
 * 窗口必须按逻辑帧: 按轮询次数算会随轮询频率变成几百毫秒, 那段时间每个普通
 * 点击都被当成 Shift+点击 (一次记 5 个)。 */
#define BQ_SHIFT_FRAMES 2
static int g_BQShiftFrame = 0; /* 0=本局还没按过 */

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
/* HouseClass::CanBuild (0x4F7870): thiscall ECX=house。
 * 返回 >0 可造 (1 可造 / -1 临时灰化), ==0 永久不可造。只读, 联机安全。 */
typedef int __attribute__((thiscall)) (*Fn_CanBuild)(void* house, void* type, int buildLimitOnly, int allowIfInProduction);
/* 工厂内该型的模拟数 (在产+在排, 原版绘制同调法): thiscall ECX=工厂。 */
typedef int __attribute__((thiscall)) (*Fn_CountTotal)(void* factory, void* type);
#define FN_COUNTTOTAL ((Fn_CountTotal)(void*)0x4CA670u)
/* VectorClass::GetItemCount (0x49FAE0): 取限造计数器, 只读。 */
typedef int __attribute__((thiscall)) (*Fn_CounterGet)(void* counter, int index);
#define FN_COUNTERGET ((Fn_CounterGet)(void*)0x49FAE0u)
/* ObjectClass::GetType (vtable+0x84): 判某工厂是否正在造本型。 */
typedef void* __attribute__((thiscall)) (*Fn_ObjGetType)(void* obj);
#define OFF_VT_GETTYPE (0x84 / 4)

/* 对象类型 (只读; 取不到返 0)。 */
static void* BQ_ObjType(void* obj)
{
    void** vt;
    if (!obj) return 0;
    vt = *(void***)obj;
    if (!vt || !vt[OFF_VT_GETTYPE]) return 0;
    return ((Fn_ObjGetType)vt[OFF_VT_GETTYPE])(obj);
}

#define FN_EVENTCTOR ((Fn_EventCtor)(void*)0x4C6970u)
#define FN_GETPRIMARYFACTORY ((Fn_GetPrimaryFactory)(void*)0x500510u)
#define FN_CANBUILD ((Fn_CanBuild)(void*)0x4F7870u)

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

/* 原版键位检测 (0x53EC70, ecx=键码, 返回 AX, AH&0x80 为按下)。 */
static int BQ_ShiftRaw(void)
{
    unsigned r = ((Fn_KeyState)(void*)0x53EC70u)(0x10u);
    return (r & 0x8000u) ? 1 : 0;
}

/* 点击用 Shift: 当前按下或最近 2 个逻辑帧内按过 (Poll 采样)。 */
static int BQ_ShiftDown(void)
{
    int age;
    if (BQ_ShiftRaw()) return 1;
    if (!g_BQShiftFrame) return 0;
    age = BQ_Frame() - g_BQShiftFrame;
    return (age >= 0 && age <= BQ_SHIFT_FRAMES) ? 1 : 0;
}

static void BQ_RemoveAt(int idx);
static int BQ_ClearType(void* house, int heap);
static int BQ_SimCount(void* house, void* type);
static int BQ_VisibleCount(void* house, void* type);
static int BQ_LimitOf(void* type);
static int BQ_LocalTypeCount(void* house, void* type);
static DWORD BQ_PeelOne(void* house, void* type);
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

/* 弹掉本家本型最后一个本地 (右键从队尾减, FIFO 剩下有序)。 */
static void BQ_PopLast(void* house, int heap)
{
    int i;
    if (!house) return;
    for (i = g_BQCount - 1; i >= 0; i--)
    {
        if (g_BQList[i].house == house &&
            BQ_HeapOf(g_BQList[i].type) == heap)
        {
            BQ_RemoveAt(i);
            return;
        }
    }
}

static void BQ_RemoveAt(int idx)
{
    int k;
    if (idx < 0 || idx >= g_BQCount) return;
    for (k = idx; k + 1 < g_BQCount; k++)
        g_BQList[k] = g_BQList[k + 1];
    g_BQCount--;
}

/* 按 (house,naval,cat) 取主工厂。返回的是 house 的缓存槽, 不是"找工厂":
 * 引擎 BeginProduction 槽空时新建工厂写回, 拒单时清空 (0x4FA7EC)。 */
static void* BQ_Resolve(void* house, unsigned naval, unsigned cat)
{
    if (!house) return 0;
    return FN_GETPRIMARYFACTORY(house, ABS_BUILDINGTYPE, naval, cat);
}

/* 本家任一工厂正在造本型 (Object 类型对上)。只读。 */
static int BQ_FacOnTypeAny(void* house, void* type)
{
    void** items;
    int count;
    int i;
    if (!house || !type) return 0;
    items = *(void***)ADDR_FACARR_ITEMS;
    count = *(int*)ADDR_FACARR_COUNT;
    if (!items || count <= 0 || !IsPlausiblePtr(items)) return 0;
    if (count > 4096) count = 4096;
    for (i = 0; i < count; i++)
    {
        void* fac = items[i];
        void* obj;
        if (!fac) continue;
        if (*(void**)((char*)fac + OFF_FAC_OWNER) != house) continue;
        obj = *(void**)((char*)fac + OFF_FAC_OBJECT);
        if (!obj) continue;
        if (BQ_ObjType(obj) == type) return 1;
    }
    return 0;
}

/* YRpp EventClass / QueueClass: Count+Head+Tail+EventClass[capacity]。
 * EventClass 是 111 字节紧凑布局: Type/IsExecuted/HouseIndex/Frame/DataBuffer。
 * 只读检查本家建筑 PRODUCE (含原生点击首单), 让网络延迟决定等待时长。
 * 两页共用此守卫是保守串行: 在途单落地后各自工厂仍可并行建造。 */
static int BQ_QueuedProduce(unsigned address, unsigned capacity, void* house)
{
    unsigned char* queue = (unsigned char*)(size_t)address;
    int count = *(int*)queue;
    unsigned head = *(unsigned*)(queue + 4);
    int houseIdx = *(int*)((char*)house + OFF_HOUSE_INDEX);
    int i;
    if (count < 0 || (unsigned)count > capacity || head >= capacity) return 1;
    for (i = 0; i < count; i++)
    {
        unsigned slot = (head + (unsigned)i) & (capacity - 1u);
        const unsigned char* ev = queue + 12u + slot * EVT_SIZE;
        if (ev[0] == EVT_PRODUCE && !(ev[1] & 1u) && (int)(signed char)ev[2] == houseIdx &&
            *(const int*)(ev + 7) == (int)ABS_BUILDINGTYPE)
            return 1;
    }
    return 0;
}

static int BQ_ProduceInTransit(void* house)
{
    /* EventClass::OutList 128 / DoList 128*128 (YRpp EventClass.h)。 */
    return BQ_QueuedProduce(ADDR_OUT_COUNT, EVT_CAP, house) ||
           BQ_QueuedProduce(0x008B41F8u, 128u * 128u, house);
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

/* 每循环轮询 (挂 PowerDrawBoth)。首轮采样 Shift (开关关/队列空也采样,
 * 保证闩锁连续); 之后开关关/队列空直接返回。
 * 条目不存工厂指针, 每轮按 (house,naval,cat) 现解析, 无 stale。
 * 就绪/待放置 (Object 非空) 只等放置: 放下后 Object==null, 下一轮自动开工。 */
void BuildQueue_Poll(void)
{
    void* cur;
    int i;
    int fr;

    BQ_EnsureInit();
    if (BQ_ShiftRaw()) g_BQShiftFrame = BQ_Frame();
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
    if (g_BQCount <= 0) return;
    fr = BQ_Frame();
    if (BQ_ProduceInTransit(cur)) return;
    /* 确认制: 发单只 mark flying 不移除, 见模拟认领才移除; 执行拒收则条目
     * 留着, 重发窗口过后自动重试。 */
    for (i = 0; i < g_BQCount; i++)
    {
        BQEntry* e = &g_BQList[i];
        void* factory;
        int baseline;
        if (e->house != cur) continue;
        /* CountTotal 增长才确认当前在途单; 生产对象本身不是订单确认信号。 */
        if (e->flying && BQ_SimCount(cur, e->type) > e->baseline)
        {
            BQ_RemoveAt(i);
            break;
        }
        /* 限造余量不足的直接丢 (=原版"点不下"语义, 队列自愈)。 */
        if (BQ_LimitOf(e->type) &&
            BQ_RoomLeft(cur, e->type,
                        BQ_LocalCount(cur, BQ_HeapOf(e->type)) - 1) <= 0)
        {
            BQ_RemoveAt(i);
            i--;
            continue;
        }
        /* 权限复查: 失去建造资格的条目永不认领, 还会压住整条产线。
         * CanBuild==0 直接丢, >0 (含 -1 临时灰化) 不动。只读, 联机安全。 */
        if (FN_CANBUILD(cur, e->type, 0, 1) == 0)
        {
            BQ_RemoveAt(i);
            i--;
            continue;
        }
        if (BQ_Pending(cur, e->naval, e->buildcat)) continue;
        /* 缓存槽可被引擎拒单清空, 但实际在产/待放置对象仍然存在。
         * 同型在产时不发第二单, 防止用同一个对象认领后续订单。 */
        if (BQ_FacOnTypeAny(cur, e->type)) continue;
        factory = BQ_Resolve(cur, e->naval, e->buildcat);
        if (factory)
        {
            if (*(void**)((char*)factory + OFF_FAC_OWNER) != cur) continue;
            if (*(void**)((char*)factory + OFF_FAC_OBJECT)) continue;
        }
        else if (e->flying && e->fired && fr - e->fired >= 0 &&
                 fr - e->fired < BQ_BLIND_FRAMES)
        {
            continue; /* 缓存槽空: 照发, 但引擎可能每单都拒, 别每轮都发 */
        }
        /* 槽空也照发: 引擎 BeginProduction 会按需建厂并写回槽, 拦着就
         * 再也续不上。 */
        baseline = BQ_SimCount(cur, e->type);
        BQ_MarkPending(cur, e->naval, e->buildcat);
        if (!BQ_FireProduce(cur, e->type)) continue;
        e->flying = 1;
        e->fired = fr;
        e->baseline = baseline;
        break; /* 每轮只发一单, 下一轮由标准事件队列确认后再继续。 */
    }
}

/* 记入 N 条本地 (槽位复用须清 flying/fired, 否则残留会被误认领 /
 * 让新条目的重发窗沿用旧时间)。 */
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
        g_BQList[g_BQCount].fired = 0;
        g_BQList[g_BQCount].baseline = 0;
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
    void* house;
    int want;
    int free;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
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
 * 首个 PRODUCE 尚在网络队列时, 普通/Shift 点击暂存本地并跳过重复原生下单；
 * 无在途订单时仅 Shift 追加 4 个, 原生首单仍照常执行。regs: EBP=abs,
 * EDI=图标类型, ESI=工厂 (可空)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_FreeShift(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    int want;
    int free;
    int pendingOrder;
    int shift;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house) return 0;
    shift = BQ_ShiftDown();
    pendingOrder = BQ_ProduceInTransit(house);
    if (!shift && !pendingOrder) return 0;
    free = BQ_CAP - g_BQCount;
    if (free <= 0) return 0;
    if (!g_BQHouse) g_BQHouse = house;
    if (house != g_BQHouse) return 0;
    /* 在途首单尚未更新工厂状态时，普通点击也必须保存在本地队列。 */
    if (pendingOrder)
        want = shift ? 5 : 1;
    else
    {
        if (!shift) return 0;
        want = 4; /* 原生首单另下, 本地只记剩余四个 */
    }
    if (want > free) want = free;
    /* 已有在途首单时它已占建造上限; 否则给原生即将下的首单留位。 */
    {
        int room = BQ_RoomLeft(house, pType, pendingOrder ? 0 : 1);
        if (want > room) want = room;
    }
    if (want <= 0) return pendingOrder ? TO_CLICK_EXIT : 0;
    BQ_PushN(house, pType,
             *(unsigned char*)((char*)pType + OFF_TT_NAVAL) ? 1u : 0u,
             *(unsigned*)(BQ_Reg(regs, R_ESP) + 0x24u), want);
    return pendingOrder ? TO_CLICK_EXIT : 0;
}

/* 0x6AB0C1 (6): 右键 state 检查 (jne 0x6AB18D, rel, 永远跳转)。
 * EAX=state 地址 (0x6AB0B8 已算好), EDI=图标类型, EBP=abs。
 * state!=1→跳取消分支 (原生 jne 去向); state==1 (本要暂停):
 * Shift+右键且本地有本型→清空后转取消分支 (原生下 0x2E);
 * 否则一律原生暂停 (暂停优先)。
 * 只读不写。
 * 注意: 副本里是那 6 字节 jne rel32, Syringe 抄到桩池时不修正 rel32,
 * 所以这里每个分支都必须返回绝对地址; 一旦 return 0 就会跑去执行副本,
 * 跳到代码池的零填充上崩 (见 notes/崩溃定位.md)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_PausePeel(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    unsigned stateAddr;
    unsigned state;
    unsigned esp;

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
    esp = BQ_Reg(regs, R_ESP);
    if (*(unsigned char*)(esp + 0x13u) &&
        BQ_LocalCount(house, BQ_HeapOf(pType)) > 0)
    {
        BQ_ClearType(house, BQ_HeapOf(pType));
        return TO_CANCEL_BRANCH;
    }
    return TO_PAUSE_NATIVE;
}

static DWORD BQ_PeelOne(void* house, void* type)
{
    int heap = BQ_HeapOf(type);
    if (BQ_LocalCount(house, heap) > 0)
    {
        BQ_PopLast(house, heap);
        return TO_CANCEL_EXIT;
    }
    return BQ_SimCount(house, type) > 1 ? TO_CANCEL_EXIT : 0;
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
    unsigned esp;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house || !g_BQCount || house != g_BQHouse) return 0;
    esp = BQ_Reg(regs, R_ESP);
    if (*(unsigned char*)(esp + 0x13u))
    {
        BQ_ClearType(house, BQ_HeapOf(pType));
        return 0;
    }
    return BQ_PeelOne(house, pType);
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

/* 0x6AAE44 (6): 右键暗分发 (je 0x6AAFBD, rel, 永远跳转)。
 * 到此=右键+非空闲态 (在产未暂停已由上游 0x6AAE0D 分走暗暂停;
 * 此处为已暂停/空闲)。EDI=图标类型, EBP=abs,
 * [esp+0x13]=shift (0→暗取消 0x6AAFBD, 1→暗清全 0x6AAE4A)。
 * 去向: 剥离跳 0x6AB28F 原生出口; 放行跳回原生去向。
 * 同 BQ_PausePeel: 副本是 je rel32, 每个分支都必须返回绝对地址, 不能 return 0。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_DarkDispatchPeel(
    void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    unsigned esp;
    int shift;
    DWORD peel;

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
    if (shift)
    {
        /* 原生清空模拟订单, 本地同型条目需同步清除。 */
        BQ_ClearType(house, BQ_HeapOf(pType));
        return TO_DARK_ALL_NATIVE;
    }
    peel = BQ_PeelOne(house, pType);
    return peel ? peel : TO_DARK_CANCEL_NATIVE;
}

/* 0x6A9C6C (6): CountTotal 前工厂空检查 (test eax,eax +
 * mov [esp+0x18],eax, 无 rel)。EAX=GPF 工厂 (可空), [ESP+0x34]=图标类型。
 * 工厂非空→跑原指令; 工厂空时用全厂模拟数+本地待建数。唯一在产订单不画数,
 * 唯一本地待建订单显示 1; 无计数时跳过原生绘制。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_DrawNullFac(void* regs)
{
    unsigned esp;
    void* pType;
    void* cur;
    unsigned v;
    int n;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;
    if (BQ_Reg(regs, R_EAX)) return 0;

    esp = BQ_Reg(regs, R_ESP);
    pType = *(void**)(esp + 0x34u);
    cur = *(void**)ADDR_CURPLAYER;
    n = BQ_LocalTypeCount(cur, pType);
    if (n <= 0)
    {
        *(unsigned*)(esp + 0x18u) = 0u;
        return TO_DRAWSKIP;
    }
    /* 工厂空不代表没在产 (槽位不对而已): 用统一口径补全订单数。 */
    v = (unsigned)BQ_VisibleCount(cur, pType);
    if (v == 1u && BQ_FacOnTypeAny(cur, pType))
    {
        *(unsigned*)(esp + 0x18u) = 0u;
        return TO_DRAWSKIP;
    }
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

static int BQ_LocalTypeCount(void* house, void* type)
{
    int i;
    int count = 0;
    if (!house || !type) return 0;
    for (i = 0; i < g_BQCount; i++)
    {
        if (g_BQList[i].house == house && g_BQList[i].type == type)
            count++;
    }
    return count;
}

static int BQ_VisibleCount(void* house, void* type)
{
    int i;
    int local;
    int sim;
    int reflected = 0;
    if (!house || !type) return 0;
    local = BQ_LocalTypeCount(house, type);
    sim = BQ_SimCount(house, type);
    for (i = 0; i < g_BQCount; i++)
    {
        BQEntry* e = &g_BQList[i];
        if (e->house == house && e->type == type && e->flying &&
            sim > e->baseline)
        {
            reflected++;
            break;
        }
    }
    return sim + local - reflected;
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
 * 求和 (sim+n)。唯一在产订单不画数, 唯一本地待建订单显示 1。
 * [ESP+0x34]=图标类型 (调用前后 esp 无净变化)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_ShowCount(void* regs)
{
    unsigned esp;
    void* pType;
    void* cur;
    unsigned v;
    int n;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    esp = BQ_Reg(regs, R_ESP);
    pType = *(void**)(esp + 0x34u);
    if (!pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur) return 0;
    n = BQ_LocalTypeCount(cur, pType);
    if (n <= 0 && BQ_Reg(regs, R_EAX)) return 0;
    v = (unsigned)BQ_VisibleCount(cur, pType);
    if (!v) return 0;
    if (v == 1u && BQ_FacOnTypeAny(cur, pType)) return 0;
    *(unsigned*)((char*)regs + R_EAX) = v;
    *(unsigned*)((char*)regs + R_EBX) = v;
    return TO_DRAWNUM;
}

/* 跨型号点击不切在产: 永远走"忙拒绝 + 本地记入", 在产不被切掉。
 * 故不钩切换清产, 也不镜像取消/对账工厂指针: 前者会在放置瞬间误清队列,
 * 后者会在开工换指针时丢单。 */

/* GUI 设置菜单用的运行时开关/查询 (ESC 菜单 "建筑队列" 开关)。 */
int BQ_Enabled(void)
{
    BQ_EnsureInit();
    return g_BQEnable;
}

void BQ_SetEnabled(int enable)
{
    BQ_EnsureInit();
    g_BQEnable = enable ? 1 : 0;
    if (g_BQEnable) BQ_ReadyLeftPatch(); /* 幂等; 菜单开时补挂就绪左对齐桩 */
    LogLine(g_BQEnable ? "bqueue: enabled (menu)"
                       : "bqueue: disabled (menu)");
}

