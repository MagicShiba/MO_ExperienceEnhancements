/* RepairAll.cpp - 建筑维修操作量简化。
 *
 * 原版修建筑要点维修图标 (或按维修快捷键) 进维修光标, 再逐个点带血的建筑, 密集
 * 挨打时操作量爆炸。实现与偏移出处见 notes/建筑一键维修.md。
 *
 * 用法 [RepairAll] Mode: 0 = 保持原样 (两个钩子直接返回 0);
 *   1 = 半自动, 手动触发; 2 = 自动修, 定时扫, 掉了血且未在修的建筑自动进维修流程。
 * 手动触发 (Mode=1/2 都行, DoubleClick=0 可关掉后两条): Ctrl+维修图标 /
 *   Ctrl+维修快捷键 / 双击维修图标 / 连按两下快捷键。触发点就是原版那两处,
 *   不新增键位; 快捷键 VK 从 KeyboardMD.ini [Hotkey] ToggleRepair 读。
 *
 * Ctrl+维修快捷键走不了引擎命令派发: 按住 Ctrl 时 0x55DEE0 只按精确的 "Ctrl+K"
 * 查命令表 (KeyboardMD 把 ToggleRepair 绑在裸 K 上), 而
 * ToggleRepairModeCommandClass::PreventCombinationOverride (0x535BD0) 恒返回
 * false, 不会退回裸 K 命令, 于是 0x536FA0 的 Execute 不被调用。这条路由
 * RepairAll_Poll 自己轮询键态补上。
 *
 * 联机: **不写任何模拟内存, 不直调改模拟的引擎函数**。唯一的模拟变更 = 往
 * OutList 塞标准事件 EventType::Repair (0x15), 构造与玩家手点维修图标逐字节
 * 同形 (照抄引擎 0x4AC0BE 那一段, house 取 [0xA83D4C]+0x30)。真去改血的是引擎
 * 自己的 BuildingClass::Update, 各端拿同一条事件得到同一结果。其余全是只读。
 * 只读偏移全部走 EngineAddr.h —— 类型类布局会偏 0xA00, 不能靠类型头推算。
 *
 * 三条通用铁律 (详见 notes/崩溃定位.md):
 *   1) 钩点必须挑**无 rel** 的指令边界: 维修图标钩点 0x6A78D9 偷 6B
 *      (mov ecx,[0x8871E0]), 不用 0x6A78F6 偷 9B (第三条是 call 0x4AC8C0);
 *      热键钩点 0x536FA0 偷的 mov ecx,imm32 里那个 E8 只是立即数的一个字节。
 *   2) bool 返回值只保证在 AL: 引擎里出现 xor al,al / mov al,x 的函数, 指针类型
 *      必须是 unsigned char (RA_CanRepair 如此), 声明 int 会读到高 24 位垃圾。
 *   3) IsPlausiblePtr 只判 [64K,2G)+4 对齐, 不能当前置解引用或调虚表: 判对象看
 *      虚表是否在 exe 映像内 (RA_IsObj), 判函数看目标是否在 exe 内 (OU_InExe)。
 *
 * 每帧路径上不许 GetPrivateProfileIntA: 配置在 RA_EnsureInit 读一次; 0x4F42F0
 * 一帧跑上百次, 故 Poll 内自门控帧号; 钩子里只读键态/置标志, 全图扫描挪到 Poll。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "RepairAll.h"

#define RA_SECTION "RepairAll"

/* DynamicVectorClass 容器: 传对象基址, items/count 是它的首两成员。 */

#define EVT_REPAIR        0x15    /* EventType::Repair */
#define ADDR_SKIP_HOTKEY  0x00536FACu  /* 0x536FA0 偷 7B 后的落点 (ret 4) */
#define ADDR_SKIP_BUTTON  0x006A78FFu  /* 0x6A78D9 偷 6B 后的落点 (分支公共尾) */

/* 标准事件构造, 与 SmartOrder/DeployPreview 手点同形;
 * TargetClass 构造器 FN_TGT_OBJ 复用 OrderUtil.h 的 (同一函数 0x6E6AB0)。 */
typedef void* __attribute__((thiscall)) (*Fn_RaTgtEventCtor)(void* ev, int houseIdx,
    int evtType, int id, unsigned rttiW);
#define FN_TGT_EV ((Fn_RaTgtEventCtor)(void*)0x4C65E0u)

typedef int __attribute__((thiscall)) (*Fn_WhatAmI)(void* self);
typedef unsigned char __attribute__((thiscall)) (*Fn_CanRepair)(void* self);
typedef int __attribute__((thiscall)) (*Fn_GetMission)(void* self);

#define MISSION_CONSTRUCTION 0x12
#define MISSION_SELLING      0x13

#define RA_QCAP    512u  /* 待发队列上限 */
#define RA_BATCH   24    /* 每逻辑帧最多入队事件数 (OutList 容量 128 要留余量) */
#define RA_SCAN_CAP 256  /* Owner 值校验抽样上限 */
#define RA_DBL_SLOP 12   /* 双击两点容差 (游戏像素) */

/* ---------------- 配置 ---------------- */

static int g_RaInit = 0;
static int g_RaMode = 0;          /* [RepairAll] Mode, 0~2 */
static int g_RaInterval = 30;     /* Mode=2 扫描间隔 (逻辑帧, 30≈1秒) */
static int g_RaBatch = RA_BATCH;  /* [RepairAll] Batch */
static int g_RaDbl = 1;           /* [RepairAll] DoubleClick */
static int g_RaDblMs = 400;       /* [RepairAll] DblMs: 两下最大间隔 (毫秒) */
static int g_RaRepairVk = 0x4B;   /* 维修快捷键 VK, 读 KeyboardMD.ini */

#define RA_INI(key, def) GetPrivateProfileIntA(RA_SECTION, key, def, g_IniPath)

static int RA_Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 游戏根目录 (= g_IniPath 所在目录) 下拼 KeyboardMD.ini 读 ToggleRepair。 */
static void RA_LoadRepairKey(void)
{
    char path[MAX_PATH_LEN];
    const char* dir = GtGameDir();
    unsigned n;
    int vk;
    if (!dir[0]) return;                            /* 取不到目录: 保持默认 'K' */
    n = AppendCStr(path, 0, dir, MAX_PATH_LEN);
    AppendCStr(path, n, "\\KeyboardMD.ini", MAX_PATH_LEN);
    vk = GetPrivateProfileIntA("Hotkey", "ToggleRepair", 0x4B, path);
    if (vk > 0 && vk < 256) g_RaRepairVk = vk;
}

static void RA_EnsureInit(void)
{
    char buf[96];
    unsigned p = 0;
    if (g_RaInit) return;
    g_RaInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_RaMode     = RA_Clamp(RA_INI("Mode", 0), 0, 2);
    g_RaInterval = RA_Clamp(RA_INI("Interval", 30), 5, 600);
    g_RaBatch    = RA_Clamp(RA_INI("Batch", RA_BATCH), 1, 96);
    g_RaDbl      = RA_INI("DoubleClick", 1) ? 1 : 0;
    /* 用**墙钟毫秒**而不是逻辑帧: 逻辑帧 = 游戏速度, GameSpeed=0 (不限速) 或高
     * 帧率时"12 帧"只有几十毫秒, 人的双击落不进去。 */
    g_RaDblMs    = RA_Clamp(RA_INI("DblMs", 400), 100, 1500);
    RA_LoadRepairKey();
    p = AppendCStr(buf, p, "repairall: mode=", sizeof(buf));
    p = AppendInt(buf, p, sizeof(buf), g_RaMode);
    p = AppendCStr(buf, p, " interval=", sizeof(buf));
    p = AppendInt(buf, p, sizeof(buf), g_RaInterval);
    p = AppendCStr(buf, p, " batch=", sizeof(buf));
    p = AppendInt(buf, p, sizeof(buf), g_RaBatch);
    p = AppendCStr(buf, p, " dbl=", sizeof(buf));
    p = AppendInt(buf, p, sizeof(buf), g_RaDbl);
    p = AppendCStr(buf, p, "/", sizeof(buf));
    p = AppendInt(buf, p, sizeof(buf), g_RaDblMs);
    p = AppendCStr(buf, p, "ms vk=", sizeof(buf));
    p += HtoA((unsigned)g_RaRepairVk, buf + p);
    LogLine(buf);
}

static int RA_On(void)
{
    RA_EnsureInit();
    return g_RaMode != 0;
}

static int RA_IsObj(void* p)
{
    return (p && IsPlausiblePtr(p) && OU_InExe((unsigned)(size_t)*(void***)p)) ? 1 : 0;
}

/* 取虚表 slot 处指向 exe 内的函数; 不是对象 / 槽值不像函数 -> 0。
 * 三个引擎询问 (WhatAmI / CanRepair / Mission) 共用这一道把关。 */
static void* RA_VFn(void* b, unsigned slot)
{
    void* fn;
    if (!RA_IsObj(b)) return 0;
    fn = (*(void***)b)[slot / 4];
    return OU_InExe((unsigned)(size_t)fn) ? fn : 0;
}

static int RA_WhatAmI(void* b)
{
    Fn_WhatAmI fn = (Fn_WhatAmI)RA_VFn(b, 0x2C);
    return fn ? fn(b) : -1;
}

/* "这栋楼能不能点 wrench 修", 虚表槽 0x94 = 0x452630, 与原版扳手光标同一句
 * 判断。**必须问引擎, 不能自己抄那串关卡**: 资格判定全在 0x450630, 而那里
 * 0x4506CE 一看到 IsBeingRepaired 就短路去修理 —— 原版不让你修不可修的东西,
 * 靠的是光标阶段根本不生成 Repair 事件。只抄事件构造 = 绕过限制。
 * 返回类型是 unsigned char, 见文件头坑 2。 */
static int RA_CanRepair(void* b)
{
    Fn_CanRepair fn = (Fn_CanRepair)RA_VFn(b, 0x94);
    if (!fn) return 0;
    return fn(b) ? 1 : 0;
}

/* 正在建造/出售中的楼不插队。引擎自己的开始修理判定也这么排
 * (0x450663 cmp 0x12 / 0x450676 cmp 0x13)。 */
static int RA_MissionBusy(void* b)
{
    Fn_GetMission fn = (Fn_GetMission)RA_VFn(b, 0x184);
    int m;
    if (!fn) return 1;
    m = fn(b);
    return (m == MISSION_CONSTRUCTION || m == MISSION_SELLING) ? 1 : 0;
}

/* ---------------- Owner 偏移值校验 (只比值, 不解引用候选字段) ---------------- */

/* 指针是否落在 HouseClass::Array 内, 且 Money 像话。 */
static int RA_IsHouse(void* p)
{
    void** items;
    int n, i, money;
    if (!p) return 1;                     /* 空 Owner 放过 (未放置的中立) */
    if (!IsPlausiblePtr(p)) return 0;
    items = ARR_ITEMS(ADDR_HOUSE_ARR);
    n = ARR_COUNT(ADDR_HOUSE_ARR);
    if (!items || !IsPlausiblePtr(items) || n <= 0 || n > 64) return 0;
    for (i = 0; i < n; i++)
    {
        if (items[i] != p) continue;
        money = *(int*)((char*)p + OFF_HOUSE_MONEY);
        return (money >= 0 && money <= 10000000) ? 1 : 0;
    }
    return 0;
}

/* TechnoClass::Owner 必须是 HouseClass::Array 的成员, 且本家自己的建筑能命中。
 * 偏移一旦不对必然有建筑取不到 HouseClass, 整轮直接放弃 —— 宁可不修, 也绝不
 * 修到别人家。 */
static int RA_OwnerOk(void** items, int count, void* cur)
{
    int i, n, mine = 0;
    n = (count > RA_SCAN_CAP) ? RA_SCAN_CAP : count;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++)
    {
        void* b = items[i];
        void* own;
        if (!RA_IsObj(b)) return 0;
        own = *(void**)((char*)b + OFF_OWNER);
        if (!RA_IsHouse(own)) return 0;
        if (own == cur) mine = 1;
    }
    return mine;
}

/* ---------------- 待发队列 (环形, 逻辑帧滴发) ---------------- */

static void* s_RaQPtr[RA_QCAP];
static int   s_RaQId[RA_QCAP];
static int   s_RaQHead = 0;
static int   s_RaQN = 0;
static int   s_RaIdxOk = 0;     /* Fetch_ID 索引自检通过 (OU_PtrAlive 才严格) */

static void RA_Push(void* b, int id)
{
    int w;
    if (s_RaQN >= (int)RA_QCAP) return;
    w = s_RaQHead + s_RaQN;
    if (w >= (int)RA_QCAP) w -= (int)RA_QCAP;
    s_RaQPtr[w] = b;
    s_RaQId[w] = id;
    s_RaQN++;
}

static void RA_Pop(void)
{
    if (++s_RaQHead >= (int)RA_QCAP) s_RaQHead = 0;
    s_RaQN--;
}

/* ---------------- 建筑数组稳定门 (同 SWSBar: 启动期半初始化垃圾会致 fault) --- */

static void* s_RaPrevItems = NULL;
static int   s_RaPrevCount = 0;
static int   s_RaStable = 0;

static void RA_Observe(void)
{
    void** items = ARR_ITEMS(ADDR_BLD_ARR);
    int count = ARR_COUNT(ADDR_BLD_ARR);
    if (!items || !IsPlausiblePtr(items) || count <= 0 || count > 4096)
    {
        s_RaPrevItems = NULL;
        s_RaStable = s_RaPrevCount = 0;
        return;
    }
    if (items == s_RaPrevItems && count == s_RaPrevCount)
    {
        if (s_RaStable < 2) s_RaStable++;
        return;
    }
    s_RaPrevItems = items;
    s_RaPrevCount = count;
    s_RaStable = 1;
}

/* ---------------- 扫描 + 发事件 ---------------- */

/* 还值不值得修: 存活/在场/未在修/自己的/有血/是楼/引擎说能修/没在建卖。
 * 便宜的字节判据排在虚表调用前面。扫描与滴发共用 (中间隔了若干帧, 对象状态
 * 可能已变, 所以滴发必须再问一遍)。 */
static int RA_StillWorth(void* b, void* cur)
{
    if (!RA_IsObj(b)) return 0;
    if (!((unsigned char*)b)[OFF_ISALIVE]) return 0;
    if (((unsigned char*)b)[OFF_INLIMBO]) return 0;
    if (((unsigned char*)b)[OFF_BLD_REPAIRING]) return 0;   /* 已经在修 */
    if (*(void**)((char*)b + OFF_OWNER) != cur) return 0;   /* 不是自己的 */
    if (*(int*)((char*)b + OFF_HEALTH) <= 0) return 0;      /* 报废/在拆 */
    if (RA_WhatAmI(b) != ABS_BUILDING) return 0;
    if (!RA_CanRepair(b)) return 0;                         /* 引擎说不能修就不修 */
    return RA_MissionBusy(b) ? 0 : 1;                       /* 正在建造/出售中 */
}

/* 该修就入队。不用"已报过"表: Repair 事件处理 (0x4C6EFD) 只查 IsAlive 就
 * SetRepairState(-1), 标志一定被置上, IsBeingRepaired 本身就是可靠的
 * "已经报过"标记, 下一轮自然跳过; 事件万一被丢, 下一轮重发才是想要的。 */
static int RA_TryQueue(void* b, void* cur)
{
    void* type;
    int id, str;
    if (!RA_StillWorth(b, cur)) return 0;
    type = *(void**)((char*)b + OFF_BLD_TYPE);
    if (!RA_IsObj(type)) return 0;
    str = *(int*)((char*)type + OFF_TYPE_STRENGTH);
    if (str <= *(int*)((char*)b + OFF_HEALTH)) return 0;    /* 没掉血 */
    id = OU_ObjTgt(b, 0);
    if (id <= 0) return 0;
    RA_Push(b, id);
    return 1;
}

/* 扫全图建筑。数组没稳定 / Owner 校验没过 / 队列堵了就什么都不做。 */
static void RA_Scan(void)
{
    void** items;
    void* cur;
    int count, i, added = 0;
    if (s_RaStable < 2 || s_RaQN >= (int)RA_QCAP) return;
    items = ARR_ITEMS(ADDR_BLD_ARR);
    count = ARR_COUNT(ADDR_BLD_ARR);
    if (items != s_RaPrevItems || count != s_RaPrevCount) return;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return;
    if (!RA_OwnerOk(items, count, cur)) return;
    for (i = 0; i < count; i++)
    {
        if (s_RaQN >= (int)RA_QCAP) break;
        if (RA_TryQueue(items[i], cur)) added++;
    }
    /* Fetch_ID 索引自检: 用队首实测一遍, 失败则 OU_PtrAlive 全 fail-open。 */
    if (added > 0)
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        s_RaIdxOk = (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 200000 &&
                     OU_PtrAliveRaw(s_RaQPtr[s_RaQHead], s_RaQId[s_RaQHead])) ? 1 : 0;
    }
}

static int RA_Fire(int houseIdx, void* b)
{
    char ev[EVT_SIZE];
    unsigned char rtti = 0;
    int id = OU_ObjTgt(b, &rtti);
    if (id < 0) return 0;
    OU_EvClear(ev);
    FN_TGT_EV(ev, houseIdx, EVT_REPAIR, id, (unsigned)rtti);
    return OU_Enqueue(ev);
}

/* 逻辑帧滴发: 队列里逐个复核存活/归属/可修性/血量后入 OutList。队满留到下轮。 */
static void RA_Drain(void)
{
    void* cur;
    int houseIdx, sent = 0;
    if (s_RaQN <= 0) return;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) { s_RaQN = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    while (s_RaQN > 0 && sent < g_RaBatch)
    {
        void* b = s_RaQPtr[s_RaQHead];
        /* 先过 Ptralive 再问虚表: 队列里的指针可能已经被回收复用。 */
        if (!OU_PtrAlive(b, s_RaQId[s_RaQHead], s_RaIdxOk) ||
            !RA_StillWorth(b, cur)) { RA_Pop(); continue; }
        if (!RA_Fire(houseIdx, b)) return;   /* OutList 满: 保留到下轮 */
        RA_Pop();
        sent++;
    }
}

/* ---------------- 两个触发钩子 (见 dist/MOExperience.dll.inj) ---------------- */

static int s_RaPending = 0;     /* 钩子/Poll 只置这个标志, 扫描挪到 Poll 里做 */
static int s_RaKeyFresh = 0;    /* 维修快捷键在上次"按下"之后被松开过 */
static int s_RaCtrlPrev = 0;    /* Ctrl+维修键边沿 (引擎不派发, 由 Poll 轮询) */

static unsigned s_RaDblMs = 0;  /* 上一次按下的 GetTickCount, 配合 s_RaDblSet */
static int s_RaDblSet = 0;
static int s_RaDblX = 0;
static int s_RaDblY = 0;

/* 双击判定, 用**墙钟毫秒** (GetTickCount) 而不是引擎逻辑帧。
 * 逻辑帧就是游戏速度: GameSpeed=0 不限速时"12 帧"只有几十毫秒, 手点追不上。
 * 图标那边再比点位 (GtMouseGameXY, 侧栏区域同样有效), 快捷键那边靠"两下之间
 * 松开过"来排除按住不放的自动重复。命中后清状态, 免得第三下又算一次;
 * 超窗或点位差太远就当两次独立单击 —— 顺带挡掉"维修->卖->维修"这种快速三连。 */
static int RA_DblHit(int checkPos)
{
    unsigned now = GetTickCount(), d;
    int mx = 0, my = 0, dx, dy;
    if (checkPos && !GtMouseGameXY(&mx, &my)) { s_RaDblSet = 0; return 0; }
    if (s_RaDblSet)
    {
        d = now - s_RaDblMs;              /* 无符号相减, 回绕安全 */
        dx = mx - s_RaDblX;
        dy = my - s_RaDblY;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (d >= 1 && d <= (unsigned)g_RaDblMs &&
            (!checkPos || (dx <= RA_DBL_SLOP && dy <= RA_DBL_SLOP)))
        {
            s_RaDblSet = 0;
            return 1;
        }
    }
    s_RaDblMs = now;
    s_RaDblSet = 1;
    s_RaDblX = mx;
    s_RaDblY = my;
    return 0;
}

/* 触发判定。fromButton=1 只在侧栏维修图标那个钩子里为真。
 * 双击为什么只吞第二下: 吞第一下就没法在"其实是单击"时补一个维修光标了
 * —— 补它就得自己直调 MapClass::SetRepairMode 改模式状态, 铁律不许。于是双击
 * 的净效果 = 单击的维修光标切换(由第一下完成) + 一键全修。Ctrl+点击则是两下
 * 都吞 (否则会进维修光标又马上退掉, 还白白 UnselectAll 一次)。
 * 没按 Ctrl 又不是双击 -> 返回 0, 原版行为一点不动。 */
static unsigned RA_Trigger(int fromButton)
{
    int dbl = 0;
    if (!RA_On()) return 0;
    if (g_RaDbl)
    {
        if (fromButton) dbl = RA_DblHit(1);
        else if (s_RaKeyFresh) { s_RaKeyFresh = 0; dbl = RA_DblHit(0); }
    }
    if (!dbl && OU_KeyDown(VK_CONTROL)) dbl = 1;
    if (dbl) s_RaPending = 1;
    return (unsigned)dbl;
}

extern "C" __declspec(dllexport) unsigned __cdecl RepairAll_HotKey(void* regs)
{
    (void)regs;
    return RA_Trigger(0) ? ADDR_SKIP_HOTKEY : 0;   /* 0x536FAC = ret 4 */
}

extern "C" __declspec(dllexport) unsigned __cdecl RepairAll_Button(void* regs)
{
    (void)regs;
    return RA_Trigger(1) ? ADDR_SKIP_BUTTON : 0;   /* 0x6A78FF = 公共尾 jmp */
}

/* ---------------- 每循环轮询 (挂 PowerDrawBoth) ---------------- */

static int s_RaLastFrame = -1;
static int s_RaLastScan = -100000;

/* 聊天输入中不响应: 引擎此时不派发命令热键, 而 Ctrl+维修键是 Poll 自己补的,
 * 不加这道门会在聊天框里打 Ctrl+K 就触发全修。MessageListClass::IsEdit=+0x19。 */
static int RA_ChatTyping(void)
{
    return *(volatile unsigned char*)(size_t)(ADDR_MSGLIST + OFF_MSGLIST_EDIT) ? 1 : 0;
}

void RepairAll_Poll(void)
{
    int fr, scan, repairDown, ctrl;
    if (!RA_On()) return;
    /* 键态每循环都采: 不能放在下面帧门之后 —— 卡帧时一帧只采一次, 快按会漏。 */
    repairDown = OU_KeyDown((unsigned)g_RaRepairVk);
    if (!repairDown) s_RaKeyFresh = 1;
    /* Ctrl+维修键: 引擎不派发 (见文件头), 只能这里轮询补, 边沿触发一次。
     * 先查键态再查焦点/聊天框: 每帧约 142 次调用, 常态下省掉窗口查询。 */
    ctrl = (repairDown && OU_KeyDown(VK_CONTROL) &&
            GtGameFocused() && !RA_ChatTyping()) ? 1 : 0;
    if (ctrl && !s_RaCtrlPrev) s_RaPending = 1;
    s_RaCtrlPrev = ctrl;
    /* 0x4F42F0 一帧跑 ~142 次 (SWSBar 实测), 必须自门控帧号。 */
    fr = *(int*)ADDR_FRAME;
    if (fr == s_RaLastFrame) return;
    s_RaLastFrame = fr;
    RA_Observe();
    RA_Drain();
    /* 手动触发这一帧必须有结果, 别等下一个 Interval。 */
    scan = s_RaPending || (g_RaMode == 2 && (s_RaLastScan < 0 || fr - s_RaLastScan >= g_RaInterval));
    s_RaPending = 0;
    if (scan) { s_RaLastScan = fr; RA_Scan(); }
}
