/*
 * MOExperience.cpp - DLL 协调层: 只把各功能模块挂到引擎的时序点上。
 *
 *   时序  prepare 0x4F42F0 (每循环) -> PowerDrawBoth: 依次调各模块 *_Poll
 *         Draw    0x5D4A94 (每帧)   -> MessageDrawHook: 依次调各模块 *_Draw
 *   交互  0x777998 滚轮 / 0x533F50 侧栏滚动
 *   入口  DllMain 装 VecText / AirWay / ChatEdit 的运行时补丁
 *
 * 各功能在 src\GUI\ (文本/面板) 与 src\ (中键/订单) 下各自成模块,
 * 各有自己的 ini 节。
 *
 * 裸钩铁律: 操作数只许 "m" (内存直接寻址), 禁止 "r"!
 * "r" 让编译器在 pushal 之前用 ecx/edx 传操作数地址, 恰好盖掉入口活寄存器
 * (prepare/blitsb 的 ecx=this, update 的 edx=surface) -> 跳板/原函数拿到
 * 垃圾 -> 启动即崩。"m" 不占寄存器, 绝对安全。
 * ★ **函数体中间的钩再加一条**: 那里的 eax/ecx/... 是活的寄存器, 栈上是**外层函数
 *   自己的帧槽**, 不能按"入口钩读栈参数"的方式取。入口 eax 只能用 pushal 之后的
 *   EAX 槽 [esp+0x1C] (见 MOExperience_PreCurEntry)。
 *
 * 构建见 build-fast.bat (32 位 mingw, 无 CRT)。
 */

#include <windows.h>
#include "GdiText.h"
#include "FullMap.h"
#include "OrderUtil.h"
#include "VecText.h"
#include "AirWay.h"
#include "BuildQueue.h"
#include "ChatEdit.h"
#include "DeployPreview.h"
#include "FuzzyAttack.h"
#include "AttackMove.h"
#include "MindSelect.h"
#include "MsgText.h"
#include "PatrolCircle.h"
#include "PowerDisplay.h"
#include "RepairAll.h"
#include "SmartArea.h"
#include "SmartOrder.h"
#include "SWSBar.h"
#include "SWPanel.h"
#include "TreeChop.h"
#include "WeaponRange.h"
#include "ScaleBar.h"
#include "MOExperience.h"

/* ---------------- 地址 ---------------- */

#define ADDR_AFTER_MSGLIST_DRAW  0x005D4A9Bu
#define ADDR_SKIP_SCROLL_SIDEBAR 0x00533FC3u

#define OFF_REG_ECX              0x20

/* ---------------- 初始化 ---------------- */

static int g_InitDone = 0;

/* ---------------- 宿主守卫 ----------------
 * DLL 与游戏同目录, 日志按 exe 所在目录落盘, 非游戏宿主 (启动器等)
 * 被加载时会往同一个 MOExperience.log 里追加, 看着像“执行了两次”。
 * 且 0x5D4510 等引擎地址在别的进程里是别的数据, 直接读即 AV
 * (本次 CRASH: eip 在本 DLL +0x27DBF, cmp byte [0x5D4510],0x53,
 * tgt=0x5D4510, 第一程 blit 读到 "atan2" 即非游戏镜像)。
 * 非 gamemd.exe 一律不装钩, 只留一行 host 供区分进程。无 CRT,
 * 后缀大小写不敏感匹配。 */
static int g_HostChecked = 0;
static int g_HostOk = 0;

static int MOE_HostIsGame(void)
{
    char pb[260];
    DWORD n, i, base, blen, k;
    static const char kWant[] = "gamemd.exe";
    if (g_HostChecked) return g_HostOk;
    g_HostChecked = 1;
    g_HostOk = 0;
    n = GetModuleFileNameA(NULL, pb, (DWORD)sizeof(pb));
    if (n == 0 || n >= (DWORD)sizeof(pb)) return 0;
    pb[n] = 0;
    i = n;
    while (i > 0 && pb[i - 1] != '\\' && pb[i - 1] != '/' && pb[i - 1] != ':') i--;
    base = i;
    blen = n - base;
    if (blen != 10) return 0;
    for (k = 0; k < 10; k++)
    {
        char a = pb[base + k], b = kWant[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (a != b) return 0;
    }
    g_HostOk = 1;
    return 1;
}

/* host 一行 (start 之前打, 双进程各留一行, 下次直接按 pid 区分)。 */
static void MOE_LogHost(void)
{
    char pb[260], buf[512], b1[16];
    unsigned p = 0;
    DWORD n = GetModuleFileNameA(NULL, pb, (DWORD)sizeof(pb));
    if (n == 0 || n >= (DWORD)sizeof(pb)) pb[0] = 0;
    else pb[n] = 0;
    p = AppendCStr(buf, p, "moe: host ", sizeof(buf));
    p = AppendCStr(buf, p, pb, sizeof(buf));
    p = AppendCStr(buf, p, " pid=", sizeof(buf));
    UtoA((unsigned)GetCurrentProcessId(), b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

/* ---------------- 崩溃定位: 全局阶段标记 ----------------
 * 引擎里到处是钩子, 崩了只看寄存器常常定位不到 (栈已经没了)。各入口把正在做的
 * 事写进 g_MoePhase, 崩溃处理程序把阶段名 + 栈上 16 个 dword 一起打出来。
 * 栈里有没有本 DLL 的地址能立刻区分"我们自己的 bug"和"我们把引擎的数据搞坏了"。*/
volatile unsigned g_MoePhase = MP_NONE;

static const char* const k_PhaseName[] =
{
    "-", "poll", "applyview", "msgdraw", "uilayers", "comp-magnify", "blitsidebar",
    "tip-pre", "paintvoid", "zbuf", "surfaceswap"
};
#define MP_COUNT ((int)(sizeof(k_PhaseName) / sizeof(k_PhaseName[0])))

const char* MOE_PhaseName(unsigned id)
{
    if ((int)id < 0 || (int)id >= MP_COUNT) return "?";
    return k_PhaseName[id];
}

void MOE_SetPhase(unsigned id) { g_MoePhase = id; }



void MOE_EnsureInit(void)
{
    char buf[128], b1[16];
    unsigned p;
    if (g_InitDone) return;
    g_InitDone = 1;
    GtEnsureLog();
    if (!g_Log) return;
    if (!MOE_HostIsGame()) return;   /* 非游戏宿主: DllMain 已留 host 行, 这里不再初始化各模块 */
    LogLine("==================================================");
    p = AppendCStr(buf, 0, "MOExperience ", sizeof(buf));
    p = AppendCStr(buf, p, MOE_VERSION, sizeof(buf));
    p = AppendCStr(buf, p, " start pid=", sizeof(buf));
    UtoA((unsigned)GetCurrentProcessId(), b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
    /* 各模块首次载入 (各自打自己的 ini 节, 幂等) */
    VecText_EnsureInit();
    MsgText_EnsureInit();
    SWPanel_EnsureInit();
    PowerDisplay_EnsureInit();
    FM_EnsureInit();
    SB_EnsureInit();   /* 缩放刻度条: 读 [ScaleBar] (档位/几何); Enable=0 时全部空转 */
}

/* ---------------- 渲染回退原因统计 ----------------
 * kind 0=矢量成功 1=无可见行 2=GDI失败 3=尺寸异常 4=Lock失败 5=无画布 */
static unsigned g_StVec = 0, g_StEmpty = 0, g_StGdi = 0;
static unsigned g_StDims = 0, g_StLock = 0, g_StSurf = 0;
static unsigned g_StLastFrame = 0;

static void StatTick(unsigned frame, int kind)
{
    char b1[16], b2[16], b3[16], b4[16], b5[16], buf[256];
    unsigned p = 0;
    if (kind == 0) g_StVec++;
    else if (kind == 1) g_StEmpty++;
    else if (kind == 2) g_StGdi++;
    else if (kind == 3) g_StDims++;
    else if (kind == 4) g_StLock++;
    else g_StSurf++;
    if (frame - g_StLastFrame < 1800) return;
    g_StLastFrame = frame;
    p = AppendCStr(buf, p, "stat: vec=", sizeof(buf));
    p += UtoA(g_StVec, buf + p);
    p = AppendCStr(buf, p, " empty=", sizeof(buf));
    UtoA(g_StEmpty, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " gdi=", sizeof(buf));
    UtoA(g_StGdi, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
    p = AppendCStr(buf, p, " dims=", sizeof(buf));
    UtoA(g_StDims, b3); p = AppendCStr(buf, p, b3, sizeof(buf));
    p = AppendCStr(buf, p, " lock=", sizeof(buf));
    UtoA(g_StLock, b4); p = AppendCStr(buf, p, b4, sizeof(buf));
    p = AppendCStr(buf, p, " surf=", sizeof(buf));
    UtoA(g_StSurf, b5); p = AppendCStr(buf, p, b5, sizeof(buf));
    LogLine(buf);
    g_StVec = g_StEmpty = g_StGdi = g_StDims = g_StLock = g_StSurf = 0;
}

/* ---------------- 每循环 prepare (0x4F42F0 裸钩) ----------------
 * 0x4F42F0 (prepare, 每循环必跑, render 条件跳过时它也跑):
 * 首 5B 恰一条指令 (mov eax,[Tactical=0x887324]), 跳板拷 5B + 跳回
 * 0x4F42F5, 入口 PatchAt 跳到 MOExperience_PrepareEntry (安装见 VecText)。
 * Entry 为 naked: pushal/pushfd -> 调 PowerDrawBoth -> popfd/popal ->
 * 尾跳跳板 (非 call! call 会多压一层返回地址, 原函数体 [esp+4] 取 arg
 * 即错位)。栈形全程 [ret][arg], 原 ret 4 收尾, 与引擎一致。
 * 循环体内事件 (重画盖住/上屏) 跑在 prepare 之前, 故每迭代先恢复;
 * render 跳过 (静置/滚完) 时照画——之前所有"帧内"修法缺的就是这一块。 */

void* g_PrepareTramp = NULL;

static void PowerDrawBoth(void)
{
    AirWay_Poll();   /* 每循环必跑的只读飞机轮询 (AirWay, 开关关时直接返回) */
    BuildQueue_Poll(); /* 每循环必跑的建筑本地排队 (BuildQueue, 开关关/队列空时直接返回) */
    PatrolCircle_Poll(); /* Z+中键一键巡逻圈 (开关关/无触发时直接返回) */
    SmartOrder_Poll(); /* 裸中键智能下令 (点按自适应/划线散开; Z 让路巡逻, Ctrl 让路模糊, 组合中键让路区域自适应) */
    TreeChop_Poll(); /* Ctrl+Z+中键快速砍树 (开关关/无触发时直接返回) */
    FuzzyAttack_Poll(); /* Ctrl+中键模糊攻击 (开关关/无触发时直接返回) */
    AttackMove_Poll(); /* 自定义移动攻击 (默认 X2, Mission=29, 直接/队列与 Z 同义) */
    SmartArea_Poll(); /* Shift+中键范围智能下令 (开关关/无触发时直接返回) */
    DeployPreview_Poll(); /* 单基地车裸中键点按: 开过去+到位自动展开 (预览框见渲染) */
    MindSelect_Poll(); /* Shift+P 一步选中被心控单位 (开关关/无组合键时直接返回) */
    SWS_Poll(); /* 超武侧栏切换+扫描 (开关关/隐藏时直接返回, 只读) */
    RepairAll_Poll(); /* 建筑维修简化 (Mode=0 时直接返回; 只读扫描+Repair 事件入队) */
    MOE_EnsureInit();
    /* 撑满视口 + 摘侧栏 gadget (隐藏原版建造栏时必须每循环都跑, 引擎每次
     * 切页/改视口都会把按钮重新挂回去)。 */
    FM_Poll();
    /* 缩放刻度条: 探整图 / 建档位表 / 摆条 / 点选滚轮换档。必须在 FM_Poll 之后
     * —— 落位要用 FM_Poll 刚刷新出来的视口矩形。 */
    SB_Poll();
    /* CCToolTip 选面/量宽用的矩形影子 = 满视图区 A (见 ScaleBar.cpp)。*/
    SB_PublishAView();
    /* 鼠标源头折算 (缩放): 注入可能早于 WWMouse 构造, 没装上就在这里每循环重试。*/
    if (MOE_MousePosPending()) MOE_InstallMousePosHook();
    /* 只画侧栏面: 随条带上屏, 层级天然在鼠标之下;
     * 侧栏不呈现时 BlitSidebar 不跑, 字不会冒出来。 */
    PowerDisplay_DrawSb();
}

static void* g_CallPrepare = (void*)PowerDrawBoth;
__attribute__((naked)) void MOExperience_PrepareEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushfl\n\t"
        "call *%0\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallPrepare), "m" (g_PrepareTramp)
        : "memory", "cc");
}

/* ---------------- 底部电量最后期合成: 上屏入口钩 ----------------
 * SidebarClass::BlitSidebar=0x6A70E0 (全二进制唯一的条带上屏点):
 * 首 6B 恰三条指令 (sub esp,0x38/push ebx/push ebp), 跳板拷 6B + 跳回
 * 0x6A70E6, 入口 PatchAt 跳到 MOExperience_BlitSbEntry (安装见 VecText)。
 * 引擎事件 (重画盖住+上屏) 会把屏上条带整体盖掉 (含屏直画部分),
 * 与层/时机无关。故在呈现入口先恢复: 每次上屏前字已在侧栏面里,
 * 呈出的每一帧都带字, 结构上不再有"被盖帧"。
 * Entry 同 prepare 钩形 (naked + 尾跳跳板, 栈形 [ret][force], 原 ret 4)。
 * 不碰光标/脏旗, 无拖影。
 *
 * FullMap 开启时多一条路: FM_BlitSb 自己只把小地图那一小块 blit 上去, 并把
 * 跳板指针换成本 DLL 里一个只含 `ret 4` 的小函数 -> 原版那条"整条侧栏"blit
 * 完全不跑, 面板连同电力条一起消失。接管时补上原版尾声的三个脏标记
 * (0xB0B519=1 / 0xB0B518=0 / this+0x53A8=0)。
 * ★ 裸钩本身一个字都不改: 换的是 g_BlitSbTramp 的指向, 汇编层与改动前
 *   逐字节相同 (上一版动过这层, 结果默认路径直接崩)。 */

void* g_BlitSbTramp = NULL;

/* FullMap 接管时用的出口: 原版尾声等价物 + ret 4 (原 ret 4, 参数栈形一致)。
 * 只做"脏标记 + 返回", 不碰任何寄存器。 */

static void BlitSbEntryDraw(void)
{
    void* tramp;
    MOE_EnsureInit();
    MOE_SetPhase(MP_BLITSIDEBAR);
    /* ★ 顺序要紧: **先画字, 再上屏**。`FM_BlitSb()` 就是把侧栏面 blit 到 Primary
     * 的那一步, 原来排在 PowerDisplay_DrawSb 前面 —— 字画在条带已经呈现之后,
     * 晚一帧。而 prepare 里画的那份会被引擎 `SidebarClass::Draw` 的重画擦掉,
     * 于是引擎每次重画面板的那一帧字就不在 = 时有时无的闪烁 (不是透明度问题,
     * PwrBlitCache 的 alpha=100 本来就是覆盖写)。*/
    PowerDisplay_DrawSb();
    tramp = (void*)(size_t)FM_BlitSb();   /* 0 = 不接管, 返回引擎跳板 */
    if (tramp) g_BlitSbTramp = tramp;
}

static void* g_CallBlitSb = (void*)BlitSbEntryDraw;
__attribute__((naked)) void MOExperience_BlitSbEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushfl\n\t"
        "call *%0\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallBlitSb), "m" (g_BlitSbTramp)
        : "memory", "cc");
}

/* ---------------- 呈现入口钩 (空操作, 只保栈形平衡) ----------------
 * GScreenClass::UpdatePrimarySurface=0x4F4780。FullMap 的小地图改成画进
 * Composite 之后这条钩已无用途, 保留安装只是不改变进出栈形。
 * 首7B = 83 EC 5C 8B 44 24 60, 跳板抄7B 跳回 0x4F4787。 */
void* g_UpdateTramp = NULL;

static void DrawPresentArg(void* surf)
{
    (void)surf;
}
static void* g_CallUpdate = (void*)DrawPresentArg;
__attribute__((naked)) void MOExperience_UpdateEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushfl\n\t"
        "push %%edx\n\t"
        "call *%0\n\t"
        "add $4, %%esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallUpdate), "m" (g_UpdateTramp)
        : "memory", "cc");
}

/* ---------------- 缩小档: 渲染期把 Composite/地图面换成大离屏面 ----------------
 * GScreenClass::Render (0x4F4480, 首8B = 83 EC 08 A1 1C 73 88 00 = sub esp,8 /
 * mov eax,[0x88731C])。缩小档 (scale>1) 时 R 比窗口大, 引擎必须画进一张 R 大的面,
 * 所以在这条入口把 [0x88731C]/[0x8872FC] 临时换成 SB 自建的大面 (TacticalClass::
 * Render 内部会把这两个互换, 尺寸必须相同); 渲染期间三趟与引擎原生 UI 都落在大面上。
 * 渲染完在消息帧钩 (MOExperience_MessageDrawHook) 立刻 SB_RenderSwapOut 还原, 再做
 * 降采样 + UI。这样大面只在一次 Render 内出现, 窗口期的其它代码看到的仍是窗口面。
 * 首8B 恰好两条完整指令 (sub esp,8 / mov eax,imm32), 跳回 0x4F4488。
 * 入口 naked: pushal/pushfl 平衡, C 函数不碰栈; 跳板照跑被抄的 8B。 */
void* g_RenderTramp = NULL;

static void RenderEntrySwap(void)
{
    SB_RenderSwapIn();
}
static void* g_CallRender = (void*)RenderEntrySwap;
__attribute__((naked)) void MOExperience_RenderEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushfl\n\t"
        "call *%0\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallRender), "m" (g_RenderTramp)
        : "memory", "cc");
}

/* ---------------- 缩小档: 把"大面被暴露的窗口"压到只剩三趟地图 ----------------
 * GScreenClass::Render 入口 (0x4F4480) 我们把 [0x88731C]/[0x8872FC] 换成了 R 大的
 * 离屏面。但从 0x4F4492 到 0x4F44CB 之间还有一串**按窗口坐标**画的指令:
 *   0x4F4492  [0x887314] = eax
 *   0x4F44A2  WWMouse::vt+0x40(面, 0)      ← 光标擦除/重绘
 *   0x4F44B7..0x4F44C2  mode 记账 + `call 0x53BAE0`
 *   0x4F453E / 0x4F4593  (三趟之后, 我们已经还原了)
 * 那些像素落进 R 大的大面, 降采样后就在**画面中部**闪出一条底栏/刻度条残影 (实测图1)。
 * 之前试过"每帧抹掉 R 里 y∈[A.Height,winH) 那条带"来盖住它 —— 错: 那一带在 R 里是
 * **地图**, 而地图是增量画的, 抹完不一定回来 ⇒ 留一条黑带 (实测图1 的黑带就是它)。
 * 正解是**别让窗口坐标的绘制碰到大面**: 0x4F4492 让回窗口面, 0x4F44CB 装回去。
 *
 * ⚠ 这两条都是**函数体中间**的钩: 此刻 eax/ecx/... 是活的寄存器, 栈上是 Render 自己
 *   的帧槽, 不能按"入口钩读栈参数"取。这里一条寄存器都不改, 所以不用碰 eax:
 *   入口壳只 pushal/popal + 调 C。
 *   (第一版在这里写 `movl 4(%esp),%eax` —— 那是"`ret` 后第一个栈参数"的偏移, 在函数体
 *    中间取到的是 Render 的局部, 写进 [0x887314] 后有人拿它虚表派发 ⇒ eip=0x400007。) */
void* g_PreCurTramp = NULL;
void* g_MapTramp = NULL;
/* SB_RenderSuspend 置 1 时把 eax 换成 g_PreCurSurf (光标那几笔直接吃 eax, 不看全局)。*/
int   g_PreCurWin = 0;
void* g_PreCurSurf = NULL;

/* ⚠ 0x4F4492 在函数体中间: eax 是活的寄存器, 栈上是 Render 自己的帧槽, **不能**按
 *   "入口钩读栈参数"取 (那偏移是 `ret` 后第一个参数)。要拿/换 eax 只能用 pushal 帧里
 *   的 EAX 槽 `[esp+0x1C]` (pushal 压入次序 EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX)。
 *   踩过的坑: 误用 `movl 4(%esp),%eax` 拿到 Render 的局部写进 [0x887314] ⇒
 *   `call [eax+0x2c]` 把像素当虚表 → eip=0x400007。 */
__attribute__((naked)) void MOExperience_PreCurEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "cmpl $0, %0\n\t"             /* SB_RenderSuspend 让出了没有 */
        "je 2f\n\t"
        "movl %1, %%ecx\n\t"
        "movl %%ecx, 28(%%esp)\n\t"   /* 改 pushal 帧的 EAX 槽 → popal 还原回去 */
        "2:\n\t"
        "popal\n\t"
        "jmp *%2\n\t"
        :
        : "m" (g_PreCurWin), "m" (g_PreCurSurf), "m" (g_PreCurTramp)
        : "memory", "cc");
}

static void MapC(void)
{
    SB_RenderResume();
}
static void* g_CallMap = (void*)MapC;
__attribute__((naked)) void MOExperience_MapEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "call *%0\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallMap), "m" (g_MapTramp)
        : "memory", "cc");
}

/* ---------------- 缩小档: chrome pass 那一趟也要拿到窗口面 ----------------
 * Render (0x4F4480) 在**第二趟与第三趟地图之间**还有一笔
 *   0x4F44F9 mov eax,[esp+0x10] / 0x4F44FD mov edx,[esi] / 0x4F44FF push eax /
 *   0x4F4500 mov ecx,esi        / 0x4F4502 call [edx+0x40]
 * 虚表 +0x40 = **0x6D0A20** (实测 ChromeC 打出来的 t40 邻位; 之前笔记把 +0x44
 * 认成 0x6D0A20 是错的, +0x44 是 0x4F45B0 的呈现那一趟)。那一趟是"底栏/侧栏
 * chrome":
 *   0x6D0AC2/0x6D0BF8/0x6D0CDC  取 [0xB0FC58] = 底栏矩形 (1x: 0,H-32,W-168,32)
 *   0x6D0C3B/0x6D0D29/0x6D0DC8  往 [0x887314] blit (侧栏图标/底栏条)
 *   0x6D0D7A/0x6D0E38 一带       SidebarClass::Draw (0x6A6C30, 内含
 *                                ToolTipManager::Draw(1) = 悬停提示盒)
 * 全部按**窗口坐标**。此刻大面还装着 (0x4F44CB 才刚装回去), 于是:
 *   - 底栏被烙进 R 的 y∈[A.Height,winH) 那条带 ⇒ 降采样后落在画面正中
 *     (y = H*A.H/R.H, 1.43 倍档 = 733, 实测图1), 而窗口面上那条底栏因为 chrome
 *     脏缓存 ([0xB0CC00]) 记的是大面而**永不重画** ⇒ 选中单位时按钮反馈全落在
 *     中部那条"错误底栏"上 (实测图1), 底下那条是 1x 留下的死像素;
 *   - 悬停提示盒同样被烙进大面并降采样 (缩小 1/scale 倍、位置上移), 之后再由
 *     0x4F4564 那次正确绘制一遍 ⇒ **同一个提示显示两次** (实测图2)。
 * 于是让出窗口面 (0x4F44FD) / 装回大面 (0x4F4505)。这仍是纯渲染面切换 + 只读
 * 查询, 联机安全。
 *
 * ⚠ 两条都是函数体中间/入口之间的钩: 一条寄存器都不改 (eax 是给 chrome pass 的
 *   flags 实参, ecx/edx 由原指令重读), 所以壳只 pushal/popal。*/
void* g_ChromePreTramp = NULL;
void* g_ChromePostTramp = NULL;

static void ChromePreC(void)
{
    SB_RenderSuspend(0);
    /* 让出窗口面之后立刻把"图外"铺在窗口面上。此刻窗口面上已经是这一帧的
     * 地图 (0x4F44DF / 0x4F44F4 两趟 0x6D3D10 刚画完), 而图外那块永远是
     * 增量拷贝搬过来的旧像素 —— 见 ScaleBar.cpp 里 SB_PaintVoid 的注释。*/
    SB_PaintVoid();
}
static void* g_CallChromePre = (void*)ChromePreC;
__attribute__((naked)) void MOExperience_ChromePreEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "call *%0\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallChromePre), "m" (g_ChromePreTramp)
        : "memory", "cc");
}

static void ChromePostC(void)
{
    SB_RenderResume();
    /* 装回大面之后、第三趟地图 (0x4F4515) 之前, 把图外也铺到大面上:
     * 增量拷贝的残影正是留在这张面上, 在源头清掉最干净。*/
    SB_PaintVoid();
}
static void* g_CallChromePost = (void*)ChromePostC;
__attribute__((naked)) void MOExperience_ChromePostEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "call *%0\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallChromePost), "m" (g_ChromePostTramp)
        : "memory", "cc");
}

/* ---------------- 缩小档: 三趟地图画完、引擎 UI 之前还原窗口面 ----------------
 * GScreenClass::Render 0x4F4480 的三趟地图 (0x4F44DF/0x4F44F4/0x4F4515) 之后,
 * 紧接着 0x4F451B 就是引擎原生 UI: 0x4F4547 (侧栏/cameo, 往"当前绘制面"画) 与
 * 0x4F455D MessageList::Draw。缩小档地图画在大面上的时候, 这些 UI 也会落进大面
 * (坐标按窗口算) ⇒ (1) 底栏残影被一起降采样到画面中部; (2) 侧栏 cameo 往尺寸
 * 不符的大面里拉伸 ⇒ 越界写坏堆 (切侧栏模式时崩)。
 * 所以在这里 (0x4F451B) 就把 [0x88731C]/[0x8872FC]/[0x887314] 还原成窗口面,
 * 并做降采样 —— 之后的引擎 UI 与本项目 *_Draw 全部 1:1 落在窗口 Composite 上。
 * 首 5B = `A0 19 B5 B0 00` (mov al,[0xB0B519]), 跳回 0x4F4520。 */
void* g_AfterMapTramp = NULL;

static void AfterMapC(void)
{
    MOE_EnsureInit();
    SB_RenderAfterMap();
}
static void* g_CallAfterMap = (void*)AfterMapC;
__attribute__((naked)) void MOExperience_AfterMapEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushfl\n\t"
        "call *%0\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallAfterMap), "m" (g_AfterMapTramp)
        : "memory", "cc");
}

/* ---------------- chrome pass 前的探针 (只诊断) ----------------
 * GScreenClass::Render 0x4F4596: `8B 16` (mov edx,[esi]) / 0x4F4598: `8B CE`
 * (mov ecx,esi) / 0x4F459A: `FF 52 44` (call [edx+0x44]) —— 这一笔是引擎在
 * 三趟地图与原生 UI 之后唯一剩下的"呈现"调用, 底栏就在它里面画。
 * ⚠ 之前笔记里"虚表 +0x44 = 0x6D0A20"是**错的**: 0x6D0A20 在整个 gamemd 里零引用,
 *   不可能是任何虚表项。这里直接把 this 与虚表项打出来。
 * in-body 钩取 this 用 **esi**: 0x4F4490 `mov esi,ecx` 之后 esi 一直就是 this,
 * 而且 esi 是 callee-saved ⇒ pushal 帧的 ESI 槽固定在 [esp+0x04]。
 * ⚠ 别用"Render 栈帧 [esp+0x14]"那种偏移: 中途 0x4F44D6 还有 push edi,
 *   栈深不是常量, 算错了 self 就是野指针 (实测探针一声不吭)。*/
void* g_ChromeTramp = NULL;

static unsigned ChromeSurfStr(char* buf, unsigned p, unsigned sz, const char* tag, void* s)
{
    char b1[16];
    p = AppendCStr(buf, p, tag, sz);
    UtoA((unsigned)(size_t)s, b1); p = AppendCStr(buf, p, b1, sz);
    if (s && IsPlausiblePtr(s))
    {
        p = AppendCh(buf, p, '(', sz);
        UtoA((unsigned)GtSurfGet(s, GtVT_GET_WIDTH), b1); p = AppendCStr(buf, p, b1, sz);
        p = AppendCh(buf, p, 'x', sz);
        UtoA((unsigned)GtSurfGet(s, GtVT_GET_HEIGHT), b1); p = AppendCStr(buf, p, b1, sz);
        p = AppendCh(buf, p, ')', sz);
    }
    p = AppendCh(buf, p, ' ', sz);
    return p;
}

static void ChromeC(void* self)
{
    static int s_n = 0;
    void** vt = 0;
    char b1[16], buf[224];
    unsigned p = 0;
    if (++s_n < 5) return;                  /* 前几帧不刷屏 */
    s_n = 0;
    if (self && IsPlausiblePtr(self)) vt = *(void***)self;
    p = AppendCStr(buf, p, "moe: chrome self=", sizeof(buf));
    UtoA((unsigned)(size_t)self, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " t44=", sizeof(buf));
    UtoA((unsigned)(size_t)(vt ? vt[0x44 / 4] : 0), b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, ' ', sizeof(buf));
    p = ChromeSurfStr(buf, p, sizeof(buf), "cur=", *(void**)0x00887314u);
    p = ChromeSurfStr(buf, p, sizeof(buf), "comp=", *(void**)0x0088731Cu);
    p = ChromeSurfStr(buf, p, sizeof(buf), "map=", *(void**)0x008872FCu);
    p = ChromeSurfStr(buf, p, sizeof(buf), "sb=", *(void**)0x00887300u);
    LogLine(buf);
}
static void* g_CallChrome = (void*)ChromeC;
__attribute__((naked)) void MOExperience_ChromeEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "movl 0x04(%%esp), %%eax\n\t"   /* esi == this (0x4F4490 mov esi,ecx) */
        "pushl %%eax\n\t"
        "call *%0\n\t"
        "addl $4, %%esp\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallChrome), "m" (g_ChromeTramp)
        : "memory", "cc");
}

/* ---------------- BlitImage (0x4AED70) 目标面探针 (只诊断) ----------------
 * 底栏鬼影 = 底栏按**窗口坐标**被 blit 进 R 大的面。0x4F4596 那个 chrome 调用点已验证
 * [0x887314] 是窗口面 (t44=0x4F4780 UpdatePrimarySurface), 所以画它的是三趟地图里的
 * 某个 overlay。0x4AED70 是全二进制唯一的图像 blit 入口, 目标面在 ecx。命中我们的大面
 * 时把返回地址 + 前 6 个栈参数打出来, 一次就能定位调用方与几何。同一返回地址只打一次。
 * 首 6B = `83 EC 94 53 56 57` (sub esp,0x94 / push ebx/esi/edi), 跳回 0x4AED76。*/
void* g_BlitTramp = NULL;

static void BlitDiagC(unsigned* frame, void* dest, void* ra)
{
    /* ★ 日志限流: 同一 ra 只打一次, 表满之后不再打 (之前只有 12 个槽,
     *   实测有 55 个不同的 ra, 满了之后每次都打 ⇒ 单文件 700MB+ 的灾难)。
     *   这个探针只用来一次性认调用方, 64 个足够; 真要重新认, 重进游戏即可。*/
    static unsigned s_seen[64];
    static int s_nseen;
    const unsigned* a = frame + 9;         /* pushal 帧 0x24 起 = 原函数栈参数 */
    unsigned k, i;
    char b1[16], buf[224];
    unsigned p = 0;
    /* 只关心"比窗口大的面" = 我们自己造的大面 (R = A*scale, scale>1)。
     * 不必和 ScaleBar 内部状态耦合, 也不必新开导出。*/
    if (!dest || !IsPlausiblePtr(dest)) return;
    if (GtSurfGet(dest, GtVT_GET_WIDTH) <= GtGameW()
        || GtSurfGet(dest, GtVT_GET_HEIGHT) <= GtGameH()) return;
    for (i = 0; i < (unsigned)s_nseen; i++) if (s_seen[i] == (unsigned)(size_t)ra) return;
    if (s_nseen >= 64) return;             /* 表满: 停, 不再打 */
    s_seen[s_nseen++] = (unsigned)(size_t)ra;
    p = AppendCStr(buf, p, "moe: blitbig ra=", sizeof(buf));
    UtoA((unsigned)(size_t)ra, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " dst=", sizeof(buf));
    UtoA((unsigned)(size_t)dest, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, '(', sizeof(buf));
    if (dest && IsPlausiblePtr(dest))
    {
        UtoA((unsigned)GtSurfGet(dest, GtVT_GET_WIDTH), b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, 'x', sizeof(buf));
        UtoA((unsigned)GtSurfGet(dest, GtVT_GET_HEIGHT), b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
    }
    p = AppendCh(buf, p, ')', sizeof(buf));
    for (k = 0; k < 6; k++)
    {
        p = AppendCh(buf, p, ' ', sizeof(buf));
        UtoA(a[k], b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    }
    LogLine(buf);
}
static void* g_CallBlit = (void*)BlitDiagC;
__attribute__((naked)) void MOExperience_BlitDiagEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "movl 0x18(%%esp), %%eax\n\t"   /* pushal 帧 ECX 槽 = 目标面 */
        "movl 0x20(%%esp), %%edx\n\t"   /* 返回地址 */
        "pushl %%edx\n\t"
        "pushl %%eax\n\t"
        "pushl %%esp\n\t"            /* pushal 帧基址 */
        "call *%0\n\t"
        "addl $12, %%esp\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallBlit), "m" (g_BlitTramp)
        : "memory", "cc");
}

/* ---------------- 缩放: 边缘卷屏的死区按屏幕空间算 ----------------
 * DisplayClass 的边缘自动卷屏 0x00692B60 (每帧由 0x00692F30 驱动, 唯一调用点
 * 0x69304E) 把传入点当视口坐标, 再拿 `pt + ViewBounds.xy` 去和面的宽高比。
 * 缩放后 +0x34 给的是视口点 (物理*R/A) ⇒ 死区跟着缩放, 缩小档下"中部就能卷屏、
 * 判定还留在左上角"。在入口把点换回物理鼠标 (SB_EdgeScrollPointFix)。
 * 首 7B = `83 EC 18 53 55 56 57` (sub esp,0x18 / push ebx/ebp/esi/edi),
 * 跳回 0x692B67。arg1 (Point2D*) = 入口 [esp+4], pushal 之后在 [esp+36]。 */
void* g_EdgeFixTramp = NULL;

static void EdgeFixC(Point2D* pt)
{
    MOE_EnsureInit();
    SB_EdgeScrollPointFix(pt);
}
static void* g_CallEdgeFix = (void*)EdgeFixC;
__attribute__((naked)) void MOExperience_EdgeFixEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "movl 36(%%esp), %%eax\n\t"
        "pushl %%eax\n\t"
        "call *%0\n\t"
        "addl $4, %%esp\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallEdgeFix), "m" (g_EdgeFixTramp)
        : "memory", "cc");
}

/* ---------------- 缩小档: 视口外的点不许变成地图点击 ----------------
 * `DisplayClass::ProcessClickCoords` (0x692300, thiscall + 6 栈参 = `ret 0x18`)
 * 是**所有**地图点击路径的总闸: 点选下单击 (窗口过程 0x693126/0x6931F9/0x6932A4)、
 * 框选 (<900px 那条 0x4AE538)、ClientToCoords 那两条 (0x4AAC6D/0x4FB405)、边缘
 * 卷屏驱动 (0x692FFC)。它的判据只有两条:
 *   0x69234B  mov eax,[ebp] ; test eax,eax ; jl 0x6925FC   → X < 0 拒
 *   0x692356  mov ebx,[ebp+4] ; test ebx,ebx ; jl 0x6925FC → Y < 0 拒
 * **没有上界判定** ⇒ "点在视口之外"这件事引擎自己是不管的。
 *
 * 1x / 放大档下 R ≤ A, 落在窗口里但不在视图区 A 的点 (底栏带 y∈[1048,1080)、
 * 侧栏列 x ≥ 1752) 经 SB_MapPoint 原样放行后仍然在 R 之外, 而那两条 ViewBounds
 * 边界判定 (ClientToCoords / 橡皮线 / Scroll_Map) 会把它拒掉 —— 一直是对的。
 *
 * 缩小档 (R = A*scale > 窗口) 下就全错了: 底栏的物理 y=1060 落在 R(0..1497) **之内**
 * ⇒ 引擎把"底栏"当成"地图上 y=1060 那一带", 于是**选中单位点底部底栏 = 对画面中部
 * 的那个地图像素下令**(实测图: 点底栏 → 单位朝画面正中跑)。
 * 修法两半:
 *   (a) ScaleBar.cpp 的 SB_MapPoint: 缩小档下 A 外的**窗口内**点也走同一个线性变换,
 *       折完必然落在 R 之外 ⇒ "不是地图上的点"在视口坐标里也成立;
 *   (b) 这里在入口把视口外的点明确拒掉 (SB_PtInView), 让 ProcessClickCoords 返回 0
 *       —— 0x201/0x203 两条分支本来就 `test al,al; je 0x693408` 直接跳出,
 *       `LeftMouseButtonClick` (0x4AC310) 也就不会记框选锚点, DecideAction 无从下手。
 * 只在 SB_Out() 时动手 ⇒ 1x/放大档逐位不变。纯本地摄像机 + 只读判定, 联机安全。
 *
 * 首 7B = `8B 44 24 10 83 EC 18` (mov eax,[esp+0x10] / sub esp,0x18), 跳板跳回
 * 0x692307。入口栈: [esp]=ret, [esp+4]=arg1 = Point2D* (0x692325 `mov ebp,[esp+0x28]`
 * 折回原栈偏移 0x4 证实); pushal 之后它在 [esp+0x24]。拒掉时自己 `ret 0x18`。*/
void* g_ClickCoordsTramp = NULL;

static int ClickCoordsReject(void* pt)
{
    static int s_tick = 0;
    if (!pt || !IsPlausiblePtr(pt)) return 0;
    if (SB_PtInView(((const int*)pt)[0], ((const int*)pt)[1])) return 0;
    /* 只在真拒掉的时候打一条 (2 秒一条): 用来确认这条钩确实在命中
     * "点在视口外" 的那类调用。若一直打不出来, 说明底栏点击根本没走
     * ProcessClickCoords, 那就得往 gadget 命中链那边查。*/
    if (--s_tick <= 0)
    {
        char b1[16], b2[16], buf[128];
        unsigned q = 0;
        unsigned now = GetTickCount();
        s_tick = 120;
        q = AppendCStr(buf, q, "sbar: click outside view ", sizeof(buf));
        UtoA((unsigned)((const int*)pt)[0], b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCh(buf, q, ',', sizeof(buf));
        UtoA((unsigned)((const int*)pt)[1], b2); q = AppendCStr(buf, q, b2, sizeof(buf));
        q = AppendCStr(buf, q, " tick=", sizeof(buf));
        UtoA(now, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        LogLine(buf);
    }
    return 1;
}
static void* g_CallClickCoords = (void*)ClickCoordsReject;
__attribute__((naked)) void MOExperience_ClickCoordsEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "movl 0x24(%%esp), %%eax\n\t"     /* pushal 帧之上 = 原栈 arg1 = Point2D* */
        "pushl %%eax\n\t"
        "call *%0\n\t"
        "addl $4, %%esp\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        "popal\n\t"
        "xorl %%eax, %%eax\n\t"
        "ret $0x18\n\t"
        :
        : "m" (g_CallClickCoords), "m" (g_ClickCoordsTramp)
        : "memory", "cc");
}

/* ---------------- 视口比地图大: 接管 TacticalClass::Clamp_Center ----------------
 * 抖动 + 一侧残影的根因 (完整推导见 ScaleBar.cpp 的 SB_ClampFix 注释):
 * Clamp_Center 的 min/max 由地图尺寸与 ViewBounds 现算, 视口大于地图时 min > max,
 * 夹位自相矛盾 ⇒ TacticalPos 每帧在 min/max 之间跳; 而地图光栅化是**增量**的
 * (按位移做 CopyFromPart), 位移反复变号 ⇒ 来回搬运同一块像素 = 抖动 + 一侧残影。
 *
 * 做法: 边界**不再靠回调原版探出来**, 改成直接读引擎全局自己算 ——
 * `[0x87F8DC..0x87F8F0]` 就是 MapClass 全局对象 (0x87F7E8) 的
 * +0xF4/+0xFC/+0x100/+0x104/+0x108, 语义已逐条反汇编确认 (地图左上角格坐标与
 * 宽高格数, px 换算 60/30 一格)。推导过程写在 ScaleBar.cpp 的 Sb_MapPx 注释里。
 *
 * ⚠⚠ **踩过的坑, 别再走回头路: 不要用跳板回调原版来探边界。**
 *   原版结尾是裸 `ret 4` (被调方清栈)。当时把函数指针写成了 `__cdecl`
 *   (调用方清栈) ⇒ callee 弹一次、GCC 又以为该自己弹而不弹 ⇒ **每次探针净丢
 *   4 字节栈**, 一次钩两次探针丢 8 字节。这个函数是被 `Set_View_Dimensions`
 *   → `SetTacticalViewBounds` (0x6D5FA8) 在开局切视图那一瞬调用的, 丢掉的栈把
 *   返回地址改成垃圾 ⇒ 实测崩在 `eip=0x09C8A708` (Ares 的 .data, 字节
 *   `FF FF FF FF 20 33 2E 33` = " 3.3"), `MOE: phase=applyview`, 栈上
 *   `006D5FAD ← 004A89C2`。
 *   (改成 `__stdcall` 能跑, 但既然全局已经读通, 干脆不跨边界调引擎函数了。)
 *   ★ 项目口径: 其余 30 多处引擎函数指针都是"被调方清栈"
 *     (`__attribute__((thiscall))`)。**要这个语义就别写 `__cdecl`**;
 *     也别写只有 1 个参数的 `thiscall` —— GCC 会把它当 this 塞进 ecx 不压栈,
 *     实测直接崩。要"被调方清栈"就用 `__stdcall`。
 * 首 5B = `A1 E4 F8 87 00` (mov eax,[0x87F8E4]), 跳板跳回 0x6D8645。
 * Entry 不用 pushal (要原样把 al 与栈交给原版的 `ret 4`), 只在调 C 前后保住
 * ecx/实参指针。*/
static int SbClampCenter(void* self, Point2D* c)
{
    (void)self;
    if (!c || !IsPlausiblePtr(c)) return 0;
    if (!SB_ClampNeeded()) return 0;
    return SB_ClampFix(c);
}
static void* g_CallClamp = (void*)SbClampCenter;

__attribute__((naked)) void MOExperience_ClampCenterEntry(void)
{
    __asm__ volatile (
        "movl 4(%%esp), %%eax\n\t"        /* arg1 = Point2D* (原版只收这一个栈参) */
        "pushl %%ecx\n\t"
        "pushl %%eax\n\t"                  /* C 的第2实参 self (原版不用 ecx) */
        "pushl %%eax\n\t"                  /* C 的第1实参 c */
        "call *%0\n\t"
        "addl $8, %%esp\n\t"
        "popl %%ecx\n\t"
        "ret $4\n\t"                     /* ★ 原版就是 ret 4: 弹 retaddr + 实参 */
        :
        : "m" (g_CallClamp)
        : "memory", "cc");
}

/* ---------------- 通用拉伸 blit (0x6BA580) 的野面守卫 ----------------
 * 引擎的 cameo/图标拉伸在缩放档下会拿到野面指针 (实测 eip=0x6BA643 的
 * `mov [ecx],dx`, ecx=0xFFxxxxxx 野指针; SWS 隐藏→AddCameo→重画、F11 切态、
 * 刚进缩小档都会触发)。1x 时这里恒走原函数。
 * 守卫只在面指针明显非法时跳过 (返回 0 = 绘制失败, 调用方本来就有失败分支),
 * 合法时原样跑跳板 ⇒ 正常帧零行为变化。只在 SB_Zoomed() 时检查, 1x 零开销。
 * 首 5B = `83 EC 14 55 56` (sub esp,0x14 / push ebp/esi), 跳回 0x6BA585。
 * arg2 (面) = 入口 [esp+8], arg3 (面) = 入口 [esp+0xC]; pushal 之后在
 * [esp+40]/[esp+44]。原函数 __thiscall + ret 0x10 (4 实参), 跳过时同样 ret 0x10。 */
void* g_BlitGuardTramp = NULL;

static int BlitGuardC(void* r, void* s1, void* s2)
{
    void** vt;
    int* ri;
    int i;
    if (!SB_Zoomed()) return 1;
    /* 面指针野了直接跳过。*/
    if (!s1 || !IsPlausiblePtr(s1) || !s2 || !IsPlausiblePtr(s2))
    {
        static int l = 0;
        if (!l) { l = 1; LogLine("blit: skip wild surf"); }
        return 0;
    }
    vt = *(void***)s1;
    if (!vt || !IsPlausiblePtr(vt))
    {
        static int l = 0;
        if (!l) { l = 1; LogLine("blit: skip wild surf vt"); }
        return 0;
    }
    vt = *(void***)s2;
    if (!vt || !IsPlausiblePtr(vt))
    {
        static int l = 0;
        if (!l) { l = 1; LogLine("blit: skip wild surf vt2"); }
        return 0;
    }
    /* 面尺寸离谱 (被释放后复用的野对象) 也跳过。用 GtSurfGet 读, 虚表已验过。
     * ★ 还要验 pitch: 拉伸循环用 pitch/2 步进 (0x6BA608/0x6BA61B), pitch 野了
     * 行指针直接飞 (实测 ecx=0xFFxxxxxx, 而宽高看着都正常)。16bpp 面 pitch
     * 恒为 width*2 按 4 对齐, 超出即野。*/
    {
        int w1 = GtSurfGet(s1, GtVT_GET_WIDTH), h1 = GtSurfGet(s1, GtVT_GET_HEIGHT);
        int w2 = GtSurfGet(s2, GtVT_GET_WIDTH), h2 = GtSurfGet(s2, GtVT_GET_HEIGHT);
        int p1 = GtSurfGet(s1, GtVT_GET_PITCH), p2 = GtSurfGet(s2, GtVT_GET_PITCH);
        if (w1 < 1 || w1 > 8192 || h1 < 1 || h1 > 8192
            || w2 < 1 || w2 > 8192 || h2 < 1 || h2 > 8192
            || p1 < w1 * 2 || p1 > w1 * 2 + 1024
            || p2 < w2 * 2 || p2 > w2 * 2 + 1024)
        {
            static int l = 0;
            if (!l) { l = 1; LogLine("blit: skip insane surf dims/pitch"); }
            return 0;
        }
    }
    /* 矩形参数野了 (R 坐标算飞/野指针) 也跳过。arg1 是调用方栈上 16 字节,
     * 按 4 个 int 读, 任何一个超出屏幕量级就判野 (R 最大 3504, 留足余量)。*/
    if (!r || !IsPlausiblePtr(r))
    {
        static int l = 0;
        if (!l) { l = 1; LogLine("blit: skip wild rect ptr"); }
        return 0;
    }
    ri = (int*)r;
    for (i = 0; i < 4; i++)
    {
        int v = ri[i];
        if (v < -32768 || v > 32767)
        {
            static int l = 0;
            if (!l)
            {
                char b1[16], buf[128];
                unsigned q = 0;
                l = 1;
                q = AppendCStr(buf, q, "blit: skip wild rect v=", sizeof(buf));
                UtoA((unsigned)(v < 0 ? -v : v), b1);
                q = AppendCStr(buf, q, b1, sizeof(buf));
                LogLine(buf);
            }
            return 0;
        }
    }
    return 1;
}
static void* g_CallBlitGuard = (void*)BlitGuardC;
__attribute__((naked)) void MOExperience_BlitGuardEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "movl 36(%%esp), %%eax\n\t"
        "movl 40(%%esp), %%ecx\n\t"
        "movl 44(%%esp), %%edx\n\t"
        "pushl %%edx\n\t"
        "pushl %%ecx\n\t"
        "pushl %%eax\n\t"
        "call *%0\n\t"
        "addl $12, %%esp\n\t"
        "testl %%eax, %%eax\n\t"
        "popal\n\t"
        "jnz 1f\n\t"
        "xorl %%eax, %%eax\n\t"
        "ret $0x10\n\t"
        "1:\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallBlitGuard), "m" (g_BlitGuardTramp)
        : "memory", "cc");
}


/* ---------------- 缩放: 放大前那次提示框绘制要跳过 ----------------
 * 放大档下悬停提示显示两遍: 一份 1:1 正确, 一份字号大一倍、位置偏到 2 倍远。
 * 字号来自全局字体 [0x89C4D0], 永不随缩放变 ⇒ 大那份是 SB_CompositeMagnify 的
 * 重采样产物; 地图每帧重画整个视口 ⇒ 不是残影 ⇒ 只能是放大前那次绘制。
 * `ToolTipManager::Draw` 只有两个调用点: 0x4F4564 (Render, 放大后) 与
 * 0x6A7081 (SidebarClass::Draw, 放大前)。这里在放大档挡掉后者。
 * ⚠ 必须自己跑 Process (0x724AD0): Draw(1) 第一件事就是它, 刷 Dimension 与
 *   LifeTime/Sticky 计时; 整个跳掉之后放大后那次会画空框。
 * ⚠ 只跳绘制, 0x6A7090 之后的 BlitSidebar 判据一字不改。
 * 门控与 SB_CompositeMagnify 一致 ⇒ 1x 与 Safe 档零行为变化。联机安全。 */
#define ADDR_TIPMANAGER      0x00887368u  /* CCToolTip::Instance */
#define ADDR_TIP_PRO_DRAW    0x006A7089u  /* mov edx,[ecx]/push 1/call [edx+0x0C] */
#define ADDR_TIP_PRO_NEXT    0x006A7090u  /* mov dl,[0xB0B518] —— 跳过点 */
#define ADDR_TIP_PROCESS     0x00724AD0u  /* ToolTipManager::Process (thiscall, 无参) */

void* g_TipPreDrawTramp = NULL;
static void* g_TipPreDrawDest = NULL;

/* bool Process() { JMP_THIS(0x724AD0); } —— 无参无栈清理 (0x724B7C 裸 ret)。 */
typedef void (__fastcall *TipProcessFn)(void* self);

static void* TipPreDrawSkip(void)
{
    void* self;
    static int s_logged = 0;
    MOE_EnsureInit();
    g_TipPreDrawDest = (SB_In() && !SB_Safe())
                       ? (void*)(size_t)ADDR_TIP_PRO_NEXT : g_TipPreDrawTramp;
    if (!g_TipPreDrawDest) g_TipPreDrawDest = (void*)(size_t)ADDR_TIP_PRO_NEXT;
    if (g_TipPreDrawDest == g_TipPreDrawTramp) return g_TipPreDrawTramp;
    MOE_SetPhase(MP_TIPPRE);
    self = *(void**)(size_t)ADDR_TIPMANAGER;
    if (self && IsPlausiblePtr(self))
        ((TipProcessFn)(size_t)ADDR_TIP_PROCESS)(self);
    if (!s_logged)
    {
        s_logged = 1;
        LogLine("tip: pre-magnify draw skipped");
    }
    return g_TipPreDrawDest;
}

static void* g_CallTipPre = (void*)TipPreDrawSkip;
__attribute__((naked)) void MOExperience_TipPreDrawEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushfl\n\t"
        "call *%0\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallTipPre), "m" (g_TipPreDrawDest)
        : "memory", "cc");
}

/* ---------------- 缩放: 引擎看到的鼠标位置 (在源头折算) ----------------
 * 视口 R = A*scale。放大档下引擎自己拿鼠标点算出来的格子/矩形全都错一档,
 * 因为**判据那一边是视口空间**: `DisplayClass::ProcessClickCoords` 的
 * `TacticalClass::Pick(点)`、框选的 900px 上限、`Scroll_Map` 的
 * `ViewBounds` 右/下界、以及 `TacticalClass::Render` 里画橡皮线那个函数
 * (0x6D5030, 由第三趟 0x4F4515 调用) 的边界判定 —— 全按视口算。
 *
 * 这些点一共有 11 个来源, 全是 `WWMouseClass` 虚表 **+0x34**
 * (GetMousePosition) 的调用点 (全二进制扫 `mov ecx,[0x887640]` 附近
 * `call [reg+0x34]` 得到的全集):
 *   0x4A56CE 单位矩形命中   0x4A91EF/0x4A9232/0x4A9284 DisplayClass::Scroll_Map
 *   0x4AAC6D ClientToCoords 0x4AE538 框选 ProcessClickCoords
 *   0x4B8FAA 地图像素区判定 0x4FB405 ClientToCoords -> ProcessClickCoords
 *   0x692F5E 点选下单击     0x6D0ABA 地形判定   0x6D5068 框选橡皮线
 *
 * ★ 折算必须挑**数据源**而不是消费者: 挂在 `ClientToCoords` (0x6D2280) /
 * `CoordsToCell` (0x6D6590) 上按调用点白名单折是不可行的 —— 它们的入参绝大
 * 多数是引擎自己的地图像素, 折算等于替引擎改数据 (一折就崩); 而只挑"看着像
 * 鼠标"的少数调用点又必然漏 (0x6D5068 橡皮线、0x692F5E 点选都漏过)。
 * 源头只有一处: 虚表 +0x34 换成我们自己的入口, 由它直接读实例里的 +0x1C/+0x20
 * (与 GtMouseGameXY 同源) 再折成视口点, 11 个消费点一次覆盖。
 * 另有一条数据源: 鼠标**按钮**消息走窗口过程那条 lParam, 见下面的
 * MOExperience_WndMouseEntry —— 不折它就只剩一半口径, 单击会被判成拖框。
 *
 * 为什么这样安全:
 *  - gadget 命中链 (`GadgetClass::Input` 0x4E1640) 用的是虚表 **+0x2C/+0x30**
 *    (X/Y 取值), 不是 +0x34 ⇒ 侧栏/工具栏按钮命中一点不变;
 *  - 光标绘制走 +0x3C/+0x40 (DrawMouseTo), 读实例里的原始坐标 ⇒ 光标仍
 *    停在物理位置 (与 notes/地图缩放.md §8 "放大档光标看不见" 一致);
 *  - 雷达 `RadarClass` 是 gadget, 同样走 +0x2C/+0x30, 不受影响;
 *  - **1x 时 A == R, 折算恒等** ⇒ 非放大档零行为变化;
 *  - 只读实例内存 + 只算坐标, 不碰任何模拟状态, 联机安全。 */
#define ADDR_WWMOUSE        0x00887640u
#define WWMOUSE_VT_GETPOS   0x34u
#define WWMOUSE_OFF_X       0x1C
#define WWMOUSE_OFF_Y       0x20

static Point2D s_MouseVp;
static void*   g_MousePosRet = 0;    /* 裸壳 popal 之后要把返回值塞回 EAX */
static void*   g_MouseVtSlot = 0;
static void*   g_MouseVtOld  = 0;
static int     g_MouseHookDone = 0;  /* 装上 (或本来就装过) 才置 1 */

static void MOE_MousePos(void* out)
{
    void* m = *(void**)ADDR_WWMOUSE;
    int mx = 0, my = 0;
    if (!m || !IsPlausiblePtr(m))
    {
        g_MousePosRet = (Point2D*)out;
        return;
    }
    mx = *(int*)((char*)m + WWMOUSE_OFF_X);
    my = *(int*)((char*)m + WWMOUSE_OFF_Y);
    s_MouseVp.X = mx;
    s_MouseVp.Y = my;
    if (SB_Zoomed()) SB_MapPoint(&s_MouseVp, 0);
    /* 调用点全都用返回值 (11 处都只 `mov reg,[eax]` / `mov reg,[eax+4]`),
     * 传进来的那个出参没人读 —— 但它本来就是调用者的栈上临时量, 顺手填上
     * 与原版同口径, 读不出参的老调用点也不会拿到脏值。*/
    if (out && IsPlausiblePtr(out))
    {
        ((Point2D*)out)->X = s_MouseVp.X;
        ((Point2D*)out)->Y = s_MouseVp.Y;
        g_MousePosRet = (Point2D*)out;
    }
    else g_MousePosRet = &s_MouseVp;
}

/* 虚表槽 __thiscall, 1 个栈参数 ⇒ callee 自己弹栈 (ret 4)。
 * 栈形: 入点 [esp]=返回地址 [esp+4]=出参; pushal 之后出参在 [esp+36]。*/
static void* g_CallMousePos = (void*)MOE_MousePos;
__attribute__((naked)) void MOE_MousePosEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "movl 36(%%esp), %%eax\n\t"   /* 出参 */
        "pushl %%eax\n\t"
        "call *%0\n\t"
        "addl $4, %%esp\n\t"
        "popal\n\t"
        "movl %1, %%eax\n\t"          /* popal 把 EAX 还原了, 重新塞返回值 */
        "ret $4\n\t"
        :
        : "m" (g_CallMousePos), "m" (g_MousePosRet)
        : "memory", "cc");
}

void MOE_InstallMousePosHook(void)
{
    void* m = *(void**)ADDR_WWMOUSE;
    void** vt;
    unsigned char* slot;
    DWORD old = 0;
    char b1[16], buf[128];
    unsigned p = 0;
    if (g_MouseHookDone) return;
    if (!m || !IsPlausiblePtr(m))
    {
        /* 注入时机可能早于 WWMouse 构造 ⇒ 不置 done, 下一个循环再试。
         * 每循环都试, 但只打一次日志 (否则能把日志刷爆)。*/
        static int s_waited = 0;
        if (!s_waited)
        {
            s_waited = 1;
            LogLine("moe: mouse hook wait (no WWMouse::Instance)");
        }
        return;
    }
    vt = *(void***)(void*)m;   /* 虚表地址 = 实例首 dword */
    if (!vt || !IsPlausiblePtr(vt))
    {
        LogLine("moe: mouse hook OFF (no vtable)");
        return;
    }
    g_MouseVtSlot = (void*)(vt + WWMOUSE_VT_GETPOS / sizeof(void*));
    if (!IsPlausiblePtr(g_MouseVtSlot))
    {
        LogLine("moe: mouse hook OFF (slot implausible)");
        g_MouseVtSlot = 0;
        return;
    }
    g_MouseVtOld = *(void**)g_MouseVtSlot;
    if (g_MouseVtOld == (void*)MOE_MousePosEntry)
    {
        g_MouseHookDone = 1;
        LogLine("moe: mouse hook already ON");
        return;
    }
    slot = (unsigned char*)g_MouseVtSlot;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
    {
        LogLine("moe: mouse hook OFF (VirtualProtect)");
        return;
    }
    *(void**)slot = (void*)MOE_MousePosEntry;
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_MouseHookDone = 1;
    p = AppendCStr(buf, p, "moe: mouse hook ON vt=", sizeof(buf));
    UtoA((unsigned)(size_t)g_MouseVtSlot, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " old=", sizeof(buf));
    UtoA((unsigned)(size_t)g_MouseVtOld, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

/* 0 = 还没装上 (prepare 每循环重试; 见 PowerDrawBoth)。 */
int MOE_MousePosPending(void) { return g_MouseHookDone ? 0 : 1; }

/* ---------------- 缩放: 鼠标**按钮**消息那条路 (0x6930A0) ------------------
 * 上面那个虚表 +0x34 的钩覆盖的是"**每帧读鼠标现在在哪**"(11 个调用点全是
 * 这一类)。引擎处理**鼠标按钮消息**走的是另一条路, 完全不过 WWMouseClass:
 *
 *   窗口过程 0x7775C0 (0x777C79 用 RegisterClassEx 注册的那一个)
 *     ├ 0x777627  Msg 既不是 0x475 也不是网络包 → 进"鼠标消息分支"
 *     └ 0x777640  call 0x6930A0 (this=MouseClass::Instance=0x87F7E8)
 *            第4实参 = **&lParam** (窗口过程自己栈上那个槽)
 *
 * 0x6930A0 用 `eax = Msg-0x201` 查一张 6 项跳转表 (0x693410), 落地的只有五个
 * 鼠标按钮, 开头是同一段 (0x693126 为例):
 *   mov edx,[esp+0x34]      ; 第4实参 = &lParam
 *   mov eax,[edx]           ; LOWORD(lParam) → x
 *   movsx ecx,word [esp+36] ; HIWORD(lParam) → y
 *   sub  eax,[0x886FA0]     ; 减 ViewBounds.xy (缩放时恒 0)
 * 这五个分支分别做: 点选下令 + 记框选锚点 (0x201, `LeftMouseButtonClick`
 * 0x4AC310 写 DisplayClass+0x11DC/+0x11E0)、滚屏与点选收尾 (0x202/0x203,
 * `0x63A8E0` + ProcessClickCoords + `DecideAction` 0x692610 + `0x4AB9B0`)、
 * 边缘卷屏开关 (0x204, `0x63AB00`)、左键还按着才收尾 (0x205)。
 *
 * 不折的后果 (放大档 2x): "按下时的点"是**没折算的物理点**, "当前点"是已折算的
 * 视口点, 两者差 A/R = 2 倍。`DisplayClass::0x4AC380` (每帧跑的框选状态机) 拿
 * 这两个比, 不等就进框选 ⇒ **单击必然被判成拖框**, 且橡皮线 (0x6D5030) 起点落在
 * 2 倍远、终点正好在光标下。点选落格与边缘卷屏判据同样偏一档。
 *
 * ★ **只能换掉那个"指针", 不能改 lParam 的值**: 同一个 lParam 槽在 0x777645 之后
 * 还要**原样**交给 `0x778030` 和 0x7779B9 → `0x54F790` —— 后者靠 lParam 更新
 * `WWMouseClass` 的位置 (虚表 +0x38/+0x44)。改了值不还原 ⇒ 光标跑偏、gadget 命中
 * (+0x2C/+0x30) 全歪、本项目自己的 `GtMouseGameXY` 还会**再折一次**。
 * 而第4实参那个槽在 0x6930A0 **自己**的栈帧上 (窗口过程 call 时压的), 它的
 * `ret 0x10` 会把它弹掉 ⇒ 只换指针, 窗口过程那边一个字节都不受影响。
 *
 * ★ 副本里必须放**打包好的 lParam** (`(y<<16)|x`), **不能放 Point2D**:
 * 引擎先把整个 dword 抄进自己的栈槽, 再 `movsx eax,ax` 取低 16 位 (x)、
 * `movsx ecx,word [esp+36]` 取那个 dword 的**高 16 位** (= y)。放 Point2D 的话
 * y 恒等于 X 的高 16 位 (0) ⇒ 每次点击都变成"点地图最上一行"。
 *
 * 抄 8B = `A0 78 E3 A8 00` (mov al,[0xA8E378]) + `83 EC 1C` (sub esp,0x1C),
 * 跳板跳回 0x6930A8。Entry **尾跳**跳板 (不 call): 原函数 `ret 0x10` 直接回窗口
 * 过程 0x777645, 我们没有"返回点" ⇒ 汇编层栈必须逐字节平衡。
 * ★ 入口 ecx = this, 跳板里 `mov esi,ecx` 把它当 DisplayClass 用 ⇒ 中间那个
 * cdecl 调用的前后必须手动 push/pop ecx (见 Entry 里的说明)。
 * 1x / [Debug] Safe=1 时只做恒等抄写 ⇒ 零行为变化。 */

void* g_WndMouseTramp = NULL;

/* 替引擎读的那份 lParam。0x6930A0 全程单线程、不泵消息, 一份就够。 */
static unsigned s_WndLp;                 /* 低 16 位 x / 高 16 位 y, 与 lParam 同编码 */
static unsigned* const s_WndLpP = &s_WndLp;   /* 裸钩操作数只许 "m", 传指针 */

/* 把 lParam 里的 (x,y) 折成视口点, 按 lParam 的编码塞回 s_WndLp。折算与
 * WWMouseClass 虚表 +0x34 那条**同一个式子** (SB_MapPoint), 这样"按下时的点"
 * 与"每帧读到的点"才同一坐标系 —— 框选状态机 (0x4AC380) 正是拿这两个比。 */
static void MOE_WndFixLParam(unsigned slot)
{
    unsigned lp, out;
    Point2D pt;
    int x, y;
    if (!slot) return;
    lp = *(unsigned*)(size_t)slot;
    x = (int)(short)(lp & 0xFFFFu);
    y = (int)(short)((lp >> 16) & 0xFFFFu);
    if (SB_Zoomed())
    {
        pt.X = x;
        pt.Y = y;
        SB_MapPoint(&pt, 0);
        x = pt.X;
        y = pt.Y;
    }
    if (x < -32768) x = -32768;   /* 引擎按 16 位符号扩展读, 别溢出 */
    if (x >  32767) x =  32767;
    if (y < -32768) y = -32768;
    if (y >  32767) y =  32767;
    out = ((unsigned)(y & 0xFFFF) << 16) | (unsigned)(x & 0xFFFF);
    s_WndLp = out;
}

static void* g_CallWndFix = (void*)MOE_WndFixLParam;

__attribute__((naked)) void MOExperience_WndMouseEntry(void)
{
    /* 裸钩铁律 (两条, 见本文件头):
     *   1) 操作数只许 "m" —— "r" 会让编译器在 pushal 之前用 ecx/edx 传操作数
     *      地址, 恰好盖掉入口活寄存器。
     *   2) ★ 这里连**调 C 函数**都会盖掉 ecx: 入口 ecx = this = 0x87F7E8,
     *      跳板里 0x6930AD `mov esi,ecx` 直接把它当 DisplayClass 用。
     *      cdecl 辅助函数返回时 ecx 是它内部用的值 ⇒ 后面第一个
     *      `cmp byte [esi+0x555A],bl` (0x693194 / 0x693232 / 0x69338F, 三条
     *      鼠标按钮分支各一处) 就读野指针崩 ⇒ 表现是"崩点飘忽、看着无关"。
     *      所以 call 前后手动 push/pop ecx。ebx/esi/edi/ebp 是 callee-saved,
     *      C 函数自己会还原, 不用管。
     * 栈必须逐字节平衡 (尾跳跳板, 原函数自己 ret 0x10 回窗口过程)。*/
    __asm__ volatile (
        "movl 0x10(%%esp), %%eax\n\t"    /* 第4实参 = &lParam */
        "pushl %%ecx\n\t"                /* ★ 保住 this */
        "pushl %%eax\n\t"
        "call *%0\n\t"                   /* 折算进 s_WndLp (cdecl, 1 个实参) */
        "addl $4, %%esp\n\t"
        "popl %%ecx\n\t"                 /* ★ 还原 this */
        "movl %1, %%eax\n\t"             /* eax = &s_WndLp */
        "movl %%eax, 0x10(%%esp)\n\t"    /* ★ 第4实参换成我们的副本 */
        "jmp *%2\n\t"                    /* 尾跳跳板 → 0x6930A8 */
        :
        : "m" (g_CallWndFix), "m" (s_WndLpP), "m" (g_WndMouseTramp)
        : "memory", "cc");
}

/* ---------------- DisplayClass::InitGUI 入口钩 (0x6ABD30) ----------------
 * 这个函数里有**一次性**的侧栏布局写入, 全都用 SidebarBounds.X:
 *   0x6ABDB2 SetPosition(0xB07E58, ...) / 0x6ABE37 mov [0xB0B3E4], -SidebarBounds.X
 *   0x6ABE67 mov [0xB07E3C], -SidebarBounds.X / 0x6ABE9E mov [esi-0x1C], -SidebarBounds.X
 *   0x6ABEDF / 0x6ABF0F 同类 / 0x6ABD39 call 0x6403A0 -> 0x654320 摆三个 radar gadget
 * 而 SidebarBounds.X 是引擎在别处按 `viewRect.X + viewRect.W` 算出来的 —— 视口
 * 铺满后那个和从 1752 变成 1920, 于是上面这些**一次性**写入全错 168:
 *   - 三个 radar gadget 被摆到屏外 (1936)
 *   - cameo/页签滚动偏移差 168 -> 侧栏布局显示混乱
 * FM_Poll 每循环纠正一次来不及 (这些只发生一次), 所以在 InitGUI **入口**先纠正。
 * 首6B 恰三条指令 (83 EC 24 / 53 / 55), 跳板抄6B + 跳回 0x6ABD36。
 * Entry 不带参数 (pushal/pushfl 平衡, 原栈形 [ret] 不变)。 */
void* g_InitGuiTramp = NULL;

static void InitGuiEntryNote(void)
{
    FM_SyncSidebarColumn();
}
static void* g_CallInitGui = (void*)InitGuiEntryNote;

__attribute__((naked)) void MOExperience_InitGuiEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushfl\n\t"
        "call *%0\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallInitGui), "m" (g_InitGuiTramp)
        : "memory", "cc");
}

/* ---------------- 视口 gadget 输入钩 (GadgetClass::Input 0x4E1640) ----------------
 * 这是整条 gadget 命中链的入口: Input(this=根gadget) 里紧接着就调
 * ExtractGadgetAt (0x4E15A0) 找出鼠标下的那个 gadget 并对它调 Clicked。
 * 引擎把横跨整个视口的容器 gadget 摆在 ViewBounds 上; 地图铺满后它连侧栏列
 * 一起吞掉, 那一列的点击全被判成"点在视口上"。所以必须在 ExtractGadgetAt
 * 之前把它的宽度收成"视口减去侧栏列"。
 * 首6B 恰三条指令 (83 EC 0C / 53 / 55), 跳板抄6B + 跳回 0x4E1646。
 * 入口栈形 [ret][arg0..]; this 在 ECX, pushal 不动 ECX, 直接 push 过去。 */

void* g_GadgetInputTramp = NULL;

static void GadgetInputEntryNote(void* self)
{
    FM_NoteGadgetInput(self);
}
static void* g_CallGadgetInput = (void*)GadgetInputEntryNote;

__attribute__((naked)) void MOExperience_GadgetInputEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushfl\n\t"
        "push %%ecx\n\t"
        "call *%0\n\t"
        "add $4, %%esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallGadgetInput), "m" (g_GadgetInputTramp)
        : "memory", "cc");
}

/* ---------------- UI 层链路 (在 Composite 上跑一遍) ----------------
 * 放大档先由缩放模块把视口放大覆盖整个视图区 (SB_CompositeMagnify), **之后**才跑
 * 这条链, 于是这些层仍然 1:1 落在放大后的地图上 (位置不变、不被放大)。侧栏那几层
 * (超武侧栏/小地图/底部电量) 本来就画在视图区之外, 完全不受放大影响。
 * 绘制顺序 (后者压前者):
 *   1 MsgText    战役框 + 提示框 + 触发条绿框
 *   2 SWPanel    超武面板 (右下, 锚地图区右边界)
 *   3 SWSBar     超武侧栏 (右侧 4 列图标)
 *   4 智能中键    橡皮线/范围框 (armed/按住时才画)
 *   5 小地图     FullMap: 直接画进 Composite
 * (底部电量只写侧栏面, 由 prepare 钩每帧画; 刻度条画在视图区之外的工具栏带)
 * 返回 1 = 至少画了一层; 失败一律静默 (宁可少画也不要花屏)。*/
int MOE_DrawUILayers(void* surf, int surfW, int surfH, int surfPitch)
{
    int drew = 0, rc, bitLineH, screenW;
    RectangleStruct viewA;
    MOE_SetPhase(MP_UILAYER);
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    if (surfW <= 0 || surfH <= 0 || surfPitch <= 0) return 0;
    bitLineH = GtBitLineHeight();
    /* UI 层是 1:1 画在**放大后**的地图上的, 所以布局基准是视图区 A (1752), 不是
     * 视口 R。原来这里给的是 GtScreenW() = ViewBounds.Width, 而 FullMap 把
     * ViewBounds 写成了 R ⇒ 放大档下 screenW 变成 438/876, 战役框/提示框/触发条
     * 全被按"半屏=R 的一半"算宽度、按"(R-宽)/2"算居中 ⇒ 整块跑到地图左边。*/
    FM_GetViewRects(&viewA, 0);
    screenW = (viewA.Width > 200) ? viewA.Width : GtScreenW();
    /* 这两个要**先**给: VecText 的 inband 门 (藏掉原版超武/雷达名单) 靠它们判
     * "这块文字是不是我们自己那块布上的"。以前放在 MsgText 后面, 而 MsgText 一旦
     * 返回 <0 就整条链提前 return ⇒ 门没开 ⇒ 原版名单原样画出来。*/
    g_SWCompW = surfW; g_SWCompH = surfH;
    g_MapRight = FM_MapRightEdge();
    rc = MsgText_DrawFrame(surf, bitLineH, screenW, surfW, surfH, surfPitch);
    if (rc < 0) return drew;
    if (rc == 1) drew = 1;
    rc = SWPanel_Draw(surf, bitLineH, surfW, surfH, surfPitch);
    if (rc < 0) return drew;
    if (rc == 1) drew = 1;
    SWS_Draw(surf, surfW, surfH, surfPitch);
    SmartOrder_Draw(surf, surfW, surfH, surfPitch);
    PatrolCircle_Draw(surf, surfW, surfH, surfPitch);
    FuzzyAttack_Draw(surf, surfW, surfH, surfPitch);
    SmartArea_Draw(surf, surfW, surfH, surfPitch);
    TreeChop_Draw(surf, surfW, surfH, surfPitch);
    DeployPreview_Draw(surf, surfW, surfH, surfPitch);
    WeaponRange_Draw(surf, surfW, surfH, surfPitch);
    if (FM_DrawRadarTo(surf)) drew = 1;
    return drew;
}

/* ---------------- 每帧消息渲染 (0x5D4A94) ----------------
 * 唯一的消息帧出口: 各模块按层级从下往上叠画, 任一步失败都回退原版。
 * 绘制顺序 (后者压前者):
 *   1 MsgText    战役框 + 提示框 + 触发条绿框 (Composite)
 *   2 SWPanel    超武面板 (右下)
 *   3 SWSBar     超武侧栏 (右侧 4 列图标)
 *   4 智能中键    橡皮线/范围框 (armed/按住时才画)
 *   5 PowerDisplay 底部电量 (只写侧栏面, 层级在鼠标之下)
 *   6 ChatEdit   自定义聊天输入框 (顶层, 压住原生输入行) */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_MessageDrawHook(void* regs)
{
    void* surf;
    unsigned tick;
    int surfW, surfH, surfPitch;
    int drew = 0;
    static int s_firstDraw = 1;

    (void)regs;
    MOE_EnsureInit();
    MOE_SetPhase(MP_MSGDRAW);
    /* 缩小档: 渲染期 Composite/地图面被换成了 R 大的离屏面, 这里必须**先**还原成
     * 窗口面, 后面取到的 surf 才是 UI 该画的那张 (窗口面)。1x/放大档恒空操作。*/
    SB_RenderSwapOut();

    tick = GtTickNow();

    surf = *(void**)ADDR_SURF_COMPOSITE;
    if (!surf) { StatTick(tick, 5); return 0; }

    if (!g_Calibrated) Calibrate();

    surfW = GtSurfGet(surf, GtVT_GET_WIDTH);
    surfH = GtSurfGet(surf, GtVT_GET_HEIGHT);
    surfPitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (surfW <= 0 || surfH <= 0 || surfPitch <= 0) { StatTick(tick, 3); return 0; }
    /* 喂给缩放模块: 窗口 Composite 的尺寸。VecText 的"这块面是不是主画布"判据
     * (超武列表收表/拦截) 要用它, 且必须在**缩放没开**时也有值。*/
    SB_NoteWindowSurface(surf, surfW, surfH);
    {   /* 鼠标映射基准取整帧 Primary (见 GtGameW 注释); 读不到就清零,
         * GtMouseGameXY 回退到原始坐标 (恒等, 不强行套 Composite 宽度)。 */
        void* prim = *(void**)ADDR_SURF_PRIMARY;
        int pw = GtSurfGet(prim, GtVT_GET_WIDTH);
        int ph = GtSurfGet(prim, GtVT_GET_HEIGHT);
        if (pw > 100 && ph > 100) GtSetGameSize(pw, ph);
        else GtSetGameSize(0, 0);
    }
    GtRefreshWinSize(tick);

    /* 缩放呈现 (在 UI 链之前):
     *   放大档 (视口比视图区小): 把 Composite 里视口 R 的像素最近邻放大铺回 A;
     *   缩小档 (视口比视图区大): 渲染期 Composite/地图面被临时换成 R 大的离屏面,
     *     这里先还原全局, 再把大面的 R 降采样铺回真 Composite 的 A。
     * 之后 UI 链仍 1:1 落在缩放后的地图上。
     * ⚠ 必须改 **Composite** 而不是 Primary: UpdatePrimarySurface (把地图面 blit
     * 到 Primary) 一帧要跑很多次, 写屏上的东西只会闪一瞬 (洋红判定框验证过)。
     * Composite 是这些 blit 的源, 改它 ⇒ 后面每次 blit 都把结果搬上屏。*/
    if (SB_In()) SB_CompositeMagnify();
    /* 缩小档的还原+降采样已提前到 0x4F451B (MOExperience_AfterMapEntry), 这样
     * 引擎 UI 不会落进大面。钩子没装上时才回退到这里的旧路径。 */
    else if (SB_Out() && !g_AfterMapTramp) SB_CompositeDemagnify();

    drew = MOE_DrawUILayers(surf, surfW, surfH, surfPitch);

    /* 聊天输入框 (顶层, 压住原生输入行)。这一层以前只写在注释里、实际没接:
     * 自绘盒子(底板+边框+光标)整层不画 ⇒ 屏上只剩引擎原生输入行的字, 看着就是
     * "输入框显示坏了"; 而且 ce_Active / CE_NoteSent 只由它驱动, 于是单机模式下
     * "自己的话"永远匹配不上 ⇒ 文本回落成 SchemeColor(0) 的青色, 玩家颜色丢失。*/
    {
        RectangleStruct a2;
        int ce;
        FM_GetViewRects(&a2, 0);
        ce = ChatEdit_Frame(surf, GtBitLineHeight(),
                            (a2.Width > 200) ? a2.Width : GtScreenW(),
                            surfW, surfH, surfPitch);
        if (ce == 1) drew = 1;
    }

    /* 底部电量: 只画侧栏面 (总开关 PowerBottom=0 即关)。 */
    {
        int pwr = PowerDisplay_DrawSb();
        if (pwr < 0) { StatTick(tick, pwr == -1 ? 2 : 4); return 0; }
        if (pwr == 1) drew = 1;
    }

    /* 刻度条: 画在视图区之外的底部工具栏带子上 (A 之外 ⇒ 引擎的 blit 不会把它
     * 缩放, 也跟着一起上屏)。 */
    if (SB_DrawToComposite()) drew = 1;

    if (!drew) { StatTick(tick, 1); return 0; }
    StatTick(tick, 0);    if (s_firstDraw)
    {
        char b1[16], b2[16], b3[16], buf[256];
        unsigned p = 0;
        s_firstDraw = 0;
        p = AppendCStr(buf, p, "draw: ok dual surf=", sizeof(buf));
        UtoA((unsigned)surfW, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, 'x', sizeof(buf));
        UtoA((unsigned)surfH, b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " pitch=", sizeof(buf));
        UtoA((unsigned)surfPitch, b3);
        p = AppendCStr(buf, p, b3, sizeof(buf));
        p = AppendCStr(buf, p, " game=", sizeof(buf));
        UtoA((unsigned)(GtGameW() < 0 ? 0 : GtGameW()), b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, 'x', sizeof(buf));
        UtoA((unsigned)(GtGameH() < 0 ? 0 : GtGameH()), b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        LogLine(buf);
    }
    return ADDR_AFTER_MSGLIST_DRAW;
}

/* ---------------- 消息帧兜底入口 (0x5D49A0, 6B) ----------------
 * 0x5D4A94 那道钩不是"每帧都跑", 引擎有条件跳过它 (反汇编):
 *   0x5D49B9  je  0x5D4A8D      ; [this+0x19] IsEdit 为 0 → 跳过原生行段
 *   0x5D4A8D  mov ecx,[esi]     ; 消息链表头
 *   0x5D4A92  je  0x5D4A9B      ; ★ 链表为空 → 直接返回, 0x5D4A94 走不到
 * 引擎 Manage 会摘掉过期项, 所以没有活消息时 MessageDrawHook 一次都不跑,
 * 上面 6 个 *_Draw 全部落空 (症状: 超武侧栏/超武信息只在冒出战役框或提示框
 * 的那几帧出现)。本钩挂在 Draw 入口, 每帧必跑, 链表空时代跑整条链路。
 * 绿框触发条不再单独画: MsgText_DrawFrame 内已有一份, 原来两处重复。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_MsgListEntry(void* regs)
{
    void* self;
    /* this 在 Draw 入口即 ECX (thiscall), 从 regs+OFF_REG_ECX 取;
     * 取不到/不可信时按"非空"处理 (保守留给 0x5D4A94, 绝不崩)。 */
    self = regs ? *(void**)((char*)regs + OFF_REG_ECX) : NULL;
    if (self && IsPlausiblePtr(self) && *(void**)self) return 0;
    {
        static int s_logged = 0;
        if (!s_logged) { s_logged = 1; LogLine("green: engine list empty, render from entry"); }
    }
    MOExperience_MessageDrawHook(regs);
    return 0;
}

/* ---------------- 滚轮 (0x777998, 6B) ----------------
 * 进入时 ECX = 原始 WParam (shr 尚未执行), bit31=1 为下滚 (回最新),
 * 否则上滚 (翻历史)。超武侧栏优先: 鼠标在超武栏上就滚超武栏, 不碰战役框。
 * 必须在战役框判定之前 —— 战役框无效时超武栏照样要能滚。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_WheelHook(void* regs)
{
    int mx, my;
    unsigned wparam;

    MOE_EnsureInit();
    if (!GtMouseGameXY(&mx, &my)) return 0;

    wparam = *(unsigned*)((char*)regs + OFF_REG_ECX);
/* Windows 约定: wparam 高位 delta 为**负**(bit31=1) = 轮子朝用户 = **下滚**;
     * bit31=0 = 上滚。
     *
     * ⚠️ 两个框的"上/下"语义天生相反, 所以 dir 要**反向**传, 这不是 bug:
     *   - 战役框 (MsgText): 最新在**下**底 (rowEnd = totalAll - skip, skip=0 看最下面)。
     *       下滚 = 看更新的 = skip 减小 => dirUp=1。
     *   - 超武栏 (SWSBar): 第 0 项在**上**面 (r = rowIndex - s_Scroll)。
     *       下滚 = 看更靠后的 = s_Scroll 增大 => dir=+1。
     * 于是 bit31=1(下滚): SWS 传 +1, 战役框传 dirUp=1 —— 两个都是 1,
     * 但含义相反, 因为底层变量方向相反。改任何一边都要同时验证另一边。*/
    if (SWS_WheelIn(mx, my, (wparam & 0x80000000u) ? 1 : -1)) return 0;
    MsgText_WheelIn(mx, my, (wparam & 0x80000000u) ? 1 : 0);
    return 0;
}

/* ---------------- 侧栏滚动 (0x533F50, 5B) ----------------
 * 悬停超武侧栏或战役框时跳过, 那一滚已经分别给了超武栏 (见 WheelHook)
 * 和战役框历史。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_SidebarHook(void* regs)
{
    int mx, my;
    (void)regs;
    MOE_EnsureInit();
    if (!GtMouseGameXY(&mx, &my)) return 0;
    if (SWS_OverBar(mx, my)) return ADDR_SKIP_SCROLL_SIDEBAR;
    if (MsgText_OverCamp(mx, my)) return ADDR_SKIP_SCROLL_SIDEBAR;
    return 0;
}

/* ---------------- 崩溃现场: 异常捕获 ----------------
 * 目的: 崩了要能直接看到"崩在哪"。之前三次崩溃都只靠日志
 * 尾部推断, 定位不到 —— 这里把故障地址/读写位/寄存器和故障点前后 8 字节
 * 打进日志, 下次崩溃一眼定案。
 *
 * ★ 用 AddVectoredExceptionHandler 而不是 SetUnhandledExceptionFilter:
 *   后者会被后面加载的模块 (YRPP / Ares / 游戏自己) 随手覆盖掉 —— 实测
 *   装上它之后崩溃依旧没有 CRASH 行。向量处理器在**所有**异常上
 *   最先被调用, 且不会被覆盖。
 *
 * 判定与返回:
 *   只对真正的致命异常动手 (读/写违规、非法指令、整数除零、溢出、栈溢出)。
 *   其余(调试断点等)一律放行, 否则会干扰游戏自身的调试流程。
 *   命中后**照旧返回 EXCEPTION_CONTINUE_SEARCH**, 不改控制流: 该崩还是崩,
 *   我们只是把现场留下来。防重入: 栈溢出时可能连续进来, 用一个忙标志挡住。
 *   日志写不进去 (真·栈溢出) 时退回 CreateFile 单独落一份 crash.txt。 */
static volatile LONG g_CrashBusy = 0;

/* 把 base 起的 n 个 dword 按 8 位十六进制接在 buf 后 (读不了就留空)。 */
static unsigned MOE_DumpWords(char* buf, unsigned p, unsigned base, int n,
                            unsigned cap)
{
    char b1[16];
    int i, ok;
    /* 不用探测: 崩溃过滤器就跑在这条线程自己的栈上, [esp, esp+n*4) 必然可读。
     * 之前加探测反而把整行输出吞成空 (VirtualQuery 判定不可信)。*/
    ok = (base > 0x10000u && base < 0x7FF00000u && (base & 3u) == 0
          && IsPlausiblePtr((void*)(size_t)base)) ? 1 : 0;
    for (i = 0; i < n; i++)
    {
        if (ok)
        {
            unsigned w = *(const unsigned*)((const unsigned char*)(size_t)base + i * 4);
            b1[0] = "0123456789ABCDEF"[(w >> 28) & 15];
            b1[1] = "0123456789ABCDEF"[(w >> 24) & 15];
            b1[2] = "0123456789ABCDEF"[(w >> 20) & 15];
            b1[3] = "0123456789ABCDEF"[(w >> 16) & 15];
            b1[4] = "0123456789ABCDEF"[(w >> 12) & 15];
            b1[5] = "0123456789ABCDEF"[(w >> 8) & 15];
            b1[6] = "0123456789ABCDEF"[(w >> 4) & 15];
            b1[7] = "0123456789ABCDEF"[w & 15];
            b1[8] = 0;
        }
        else b1[0] = 0;
        /* cap 必须是**调用方缓冲区的真实大小**。之前这里传的是 sizeof(b1)=16,
         * 于是第 15 个字符之后 AppendCStr 全部静默丢弃, 而 AppendCh 用的还是
         * 写死的 512 ⇒ 日志里只剩一串空格, 一个字都看不到。*/
        p = AppendCh(buf, p, ' ', cap);
        p = AppendCStr(buf, p, b1, cap);
        if (i == 7) p = AppendCh(buf, p, '|', cap);
    }
    return p;
}

static LONG CALLBACK Moe_CrashFilter(EXCEPTION_POINTERS* ep)
{
    char b1[16], buf[512];
    unsigned p = 0;
    EXCEPTION_RECORD* r;
    const unsigned char* code;
    HMODULE mod;
    char name[64];
    unsigned i;
    int fatal, readable;

    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    r = ep->ExceptionRecord;
    fatal = (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
         || (r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION)
         || (r->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO)
         || (r->ExceptionCode == EXCEPTION_INT_OVERFLOW)
         || (r->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
         || (r->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION);
    if (!fatal) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_CrashBusy, 1, 0) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    GtEnsureLog();
    code = (const unsigned char*)r->ExceptionAddress;
    if (g_Log)
    {
        p = AppendCStr(buf, p, "CRASH: code=", sizeof(buf));
        UtoA((unsigned)r->ExceptionCode, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " addr=", sizeof(buf));
        UtoA((unsigned)(size_t)r->ExceptionAddress, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, (r->NumberParameters >= 2) ? " (read)" : " (write)", sizeof(buf));
        p = AppendCStr(buf, p, " fault=", sizeof(buf));
        UtoA(r->NumberParameters ? (unsigned)(size_t)r->ExceptionInformation[0] : 0u, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        /* 出错的数据地址 (读/写目标, fault= 只是读写标志别再误读)。 */
        if (r->NumberParameters >= 2)
        {
            p = AppendCStr(buf, p, " tgt=", sizeof(buf));
            UtoA((unsigned)(size_t)r->ExceptionInformation[1], b1);
            p = AppendCStr(buf, p, b1, sizeof(buf));
        }
        LogLine(buf);

        p = 0;
        p = AppendCStr(buf, p, "CRASH: eax=", sizeof(buf));
        UtoA((unsigned)(size_t)ep->ContextRecord->Eax, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " ecx=", sizeof(buf));
        UtoA((unsigned)(size_t)ep->ContextRecord->Ecx, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " edx=", sizeof(buf));
        UtoA((unsigned)(size_t)ep->ContextRecord->Edx, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " esp=", sizeof(buf));
        UtoA((unsigned)(size_t)ep->ContextRecord->Esp, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " ebp=", sizeof(buf));
        UtoA((unsigned)(size_t)ep->ContextRecord->Ebp, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " eip=", sizeof(buf));
        UtoA((unsigned)(size_t)ep->ContextRecord->Eip, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        LogLine(buf);

        /* 阶段名 + 两个栈窗口 (esp / ebp)。esp 可能本身是坏的, 先 IsBadReadPtr
         * 探一下。ebp 那个窗口才是帧函数返回地址所在 (它用 ebp 相对寻址), 有了它
         * 就能直接定位到 gamemd 里是哪一句在调坏指针。*/
        p = 0;
        p = AppendCStr(buf, p, "MOE: phase=", sizeof(buf));
        p = AppendCStr(buf, p, MOE_PhaseName(g_MoePhase), sizeof(buf));
        p = AppendCStr(buf, p, " stk=", sizeof(buf));
        p = MOE_DumpWords(buf, p, (unsigned)(size_t)ep->ContextRecord->Esp, 24,
                             sizeof(buf));
        LogLine(buf);
        p = 0;
        p = AppendCStr(buf, p, "MOE: bfp=", sizeof(buf));
        p = MOE_DumpWords(buf, p, (unsigned)(size_t)ep->ContextRecord->Ebp, 24,
                             sizeof(buf));
        LogLine(buf);

        /* 故障点所在模块 + 该点前后 8 字节: 判断是引擎指令还是野跳转。 */
        /* 故障地址本身可能不可读 (野跳转), 那时读字节会二次异常, 后面什么都留不下。*/
        name[0] = 0;
        mod = NULL;
        {
            unsigned ea = (unsigned)(size_t)r->ExceptionAddress;
            readable = (ea >= 0x10000u && ea < 0x7FF00000u) ? 1 : 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)(size_t)ea, &mod) && mod)
                GetModuleFileNameA(mod, name, sizeof(name));
        }
        p = 0;
        p = AppendCStr(buf, p, "CRASH: mod=", sizeof(buf));
        p = AppendCStr(buf, p, name, sizeof(buf));
        p = AppendCStr(buf, p, " base=", sizeof(buf));
        UtoA((unsigned)(size_t)mod, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " at", sizeof(buf));
        for (i = 0; readable && i < 8; i++)
        {
            b1[0] = "0123456789ABCDEF"[(code[i] >> 4) & 15];
            b1[1] = "0123456789ABCDEF"[code[i] & 15];
            b1[2] = 0;
            p = AppendCStr(buf, p, " ", sizeof(buf));
            p = AppendCStr(buf, p, b1, sizeof(buf));
        }
        LogLine(buf);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" __declspec(dllexport) BOOL __stdcall DllMain(HINSTANCE h, DWORD reason, LPVOID res)
{
    (void)h; (void)res;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
        GtEnsureLog();
        MOE_LogHost();
        if (!MOE_HostIsGame())
        {
            LogLine("moe: host not gamemd, hooks skipped");
            return TRUE;
        }
        AddVectoredExceptionHandler(1, Moe_CrashFilter);
        MOE_EnsureInit();
        VecText_Install();
        AirWay_Install();
        ChatEdit_Install();
        FM_Install();
    }
    return TRUE;
}
