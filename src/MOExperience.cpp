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
#include "MOExperience.h"

/* ---------------- 地址 ---------------- */

#define ADDR_AFTER_MSGLIST_DRAW  0x005D4A9Bu
#define ADDR_SKIP_SCROLL_SIDEBAR 0x00533FC3u
#define ADDR_CURRENT_FRAME       0x00A8ED84u

#define OFF_REG_ECX              0x20

/* ---------------- 初始化 ---------------- */

static int g_InitDone = 0;

void MOE_EnsureInit(void)
{
    char buf[128];
    unsigned p;
    if (g_InitDone) return;
    g_InitDone = 1;
    GtEnsureLog();
    if (!g_Log) return;
    LogLine("==================================================");
    p = AppendCStr(buf, 0, "MOExperience ", sizeof(buf));
    p = AppendCStr(buf, p, MOE_VERSION, sizeof(buf));
    p = AppendCStr(buf, p, " start", sizeof(buf));
    LogLine(buf);
    /* 各模块首次载入 (各自打自己的 ini 节, 幂等) */
    VecText_EnsureInit();
    MsgText_EnsureInit();
    SWPanel_EnsureInit();
    PowerDisplay_EnsureInit();
    FM_EnsureInit();
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
#define ADDR_PREPARE       0x4F42F0u

void* g_PrepareTramp = NULL;

static void PowerDrawBoth(void)
{
    AirWay_Poll();   /* 每循环必跑的只读飞机轮询 (AirWay, 开关关时直接返回) */
    BuildQueue_Poll(); /* 每循环必跑的建筑本地排队 (BuildQueue, 开关关/队列空时直接返回) */
    PatrolCircle_Poll(); /* Z+中键一键巡逻圈 (开关关/无触发时直接返回) */
    SmartOrder_Poll(); /* 裸中键智能下令 (点按自适应/划线散开; Z 让路巡逻, Ctrl 让路模糊, 组合中键让路区域自适应) */
    TreeChop_Poll(); /* Ctrl+Z+中键快速砍树 (开关关/无触发时直接返回) */
    FuzzyAttack_Poll(); /* Ctrl+中键模糊攻击 (开关关/无触发时直接返回) */
    SmartArea_Poll(); /* Shift+中键范围智能下令 (开关关/无触发时直接返回) */
    DeployPreview_Poll(); /* 单基地车裸中键点按: 开过去+到位自动展开 (预览框见渲染) */
    MindSelect_Poll(); /* Shift+P 一步选中被心控单位 (开关关/无组合键时直接返回) */
    SWS_Poll(); /* 超武侧栏切换+扫描 (开关关/隐藏时直接返回, 只读) */
    RepairAll_Poll(); /* 建筑维修简化 (Mode=0 时直接返回; 只读扫描+Repair 事件入队) */
    MOE_EnsureInit();
    /* 撑满视口 + 摘侧栏 gadget (隐藏原版建造栏时必须每循环都跑, 引擎每次
     * 切页/改视口都会把按钮重新挂回去)。 */
    FM_Poll();
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
#define ADDR_BLITSB        0x6A70E0u

void* g_BlitSbTramp = NULL;

/* FullMap 接管时用的出口: 原版尾声等价物 + ret 4 (原 ret 4, 参数栈形一致)。
 * 只做"脏标记 + 返回", 不碰任何寄存器。 */

static void BlitSbEntryDraw(void)
{
    void* tramp;
    MOE_EnsureInit();
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
#define ADDR_GADGET_INPUT   0x4E1640u

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
    int bitLineH, screenW, surfW, surfH, surfPitch;
    int drew = 0, rc;
    static int s_firstDraw = 1;

    (void)regs;
    MOE_EnsureInit();

    tick = GtTickNow();

    surf = *(void**)ADDR_SURF_COMPOSITE;
    if (!surf) { StatTick(tick, 5); return 0; }

    if (!g_Calibrated) Calibrate();

    bitLineH = GtBitLineHeight();
    screenW = GtScreenW();
    surfW = GtSurfGet(surf, GtVT_GET_WIDTH);
    surfH = GtSurfGet(surf, GtVT_GET_HEIGHT);
    surfPitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (surfW <= 0 || surfH <= 0 || surfPitch <= 0) { StatTick(tick, 3); return 0; }
    {   /* 鼠标映射基准取整帧 Primary (见 GtGameW 注释); 读不到就清零,
         * GtMouseGameXY 回退到原始坐标 (恒等, 不强行套 Composite 宽度)。 */
        void* prim = *(void**)ADDR_SURF_PRIMARY;
        int pw = GtSurfGet(prim, GtVT_GET_WIDTH);
        int ph = GtSurfGet(prim, GtVT_GET_HEIGHT);
        if (pw > 100 && ph > 100) GtSetGameSize(pw, ph);
        else GtSetGameSize(0, 0);
    }
    GtRefreshWinSize(tick);

    /* 1 消息文本显示: 战役框 + 提示框 (+ 调试绿框) */
    rc = MsgText_DrawFrame(surf, bitLineH, screenW, surfW, surfH, surfPitch);
    if (rc < 0) { StatTick(tick, rc == -1 ? 2 : 4); return 0; }
    if (rc == 1) drew = 1;

    /* 2 超武面板: 右下整块重绘 (Print 入口已隐藏+收集, 此处统一画)。
     * 主画布尺寸同时回填给收集门: 坐标系一致才收, 否则走原版。
     * g_MapRight = 地图区右边界: 侧栏显示时 1752 / 隐藏时 1920。贴右缘画的那几层
     * (超武面板、超武侧栏) 必须锚到它, 否则侧栏一显示就被压在侧栏底下。*/
    g_SWCompW = surfW; g_SWCompH = surfH;
    g_MapRight = FM_MapRightEdge();
    rc = SWPanel_Draw(surf, bitLineH, surfW, surfH, surfPitch);
    if (rc < 0) { StatTick(tick, rc == -1 ? 2 : 4); return 0; }
    if (rc == 1) drew = 1;

    /* 3 超武侧栏: 右侧 4 列图标 (SWSBar, 开关关/隐藏/无超武时无操作)。 */
    SWS_Draw(surf, surfW, surfH, surfPitch);

    /* 4 橡皮线/范围框 (armed/按住时才画, 自锁面, 不影响其它绘制)。 */
    {
        SmartOrder_Draw(surf, surfW, surfH, surfPitch);
        PatrolCircle_Draw(surf, surfW, surfH, surfPitch);
        FuzzyAttack_Draw(surf, surfW, surfH, surfPitch);
        SmartArea_Draw(surf, surfW, surfH, surfPitch);
        TreeChop_Draw(surf, surfW, surfH, surfPitch);
        DeployPreview_Draw(surf, surfW, surfH, surfPitch);
    }

    /* 5 小地图 (FullMap): 直接画进 Composite。地图铺满后每帧都会盖掉右上角,
     * 而侧栏条带的上屏时机并不保证跟着地图呈现, 所以不靠 BlitSidebar ——
     * 画进 Composite 就成了画面的一部分, 跟着一起被呈现, 不闪。 */
    if (FM_DrawRadarTo(surf)) drew = 1;

    /* 6 底部电量: 只画侧栏面 (总开关 PowerBottom=0 即关)。
     * 随 BlitSidebar 条带上屏, 层级在鼠标之下, 侧栏不画即不显示。 */
    {
        int pwr = PowerDisplay_DrawSb();
        if (pwr < 0) { StatTick(tick, pwr == -1 ? 2 : 4); return 0; }
        if (pwr == 1) drew = 1;
    }

    /* 7 自定义聊天输入框: 盖住原生输入行, 顶层 (未激活即无操作) */
    {
        int ced = ChatEdit_Frame(surf, bitLineH, screenW, surfW, surfH, surfPitch);
        if (ced == 1) drew = 1;
    }

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
 * 目的: 崩了要能直接看到"崩在哪"。之前 v0.2.166/167/168 三次崩溃都只靠日志
 * 尾部推断, 定位不到 —— 这里把故障地址/读写位/寄存器和故障点前后 8 字节
 * 打进日志, 下次崩溃一眼定案。
 *
 * ★ 用 AddVectoredExceptionHandler 而不是 SetUnhandledExceptionFilter:
 *   后者会被后面加载的模块 (YRPP / Ares / 游戏自己) 随手覆盖掉 —— 实测
 *   v0.2.170 装上它之后崩溃依旧没有 CRASH 行。向量处理器在**所有**异常上
 *   最先被调用, 且不会被覆盖。
 *
 * 判定与返回:
 *   只对真正的致命异常动手 (读/写违规、非法指令、整数除零、溢出、栈溢出)。
 *   其余(调试断点等)一律放行, 否则会干扰游戏自身的调试流程。
 *   命中后**照旧返回 EXCEPTION_CONTINUE_SEARCH**, 不改控制流: 该崩还是崩,
 *   我们只是把现场留下来。防重入: 栈溢出时可能连续进来, 用一个忙标志挡住。
 *   日志写不进去 (真·栈溢出) 时退回 CreateFile 单独落一份 crash.txt。 */
static volatile LONG g_CrashBusy = 0;

static LONG CALLBACK Moe_CrashFilter(EXCEPTION_POINTERS* ep)
{
    char b1[16], buf[256];
    unsigned p = 0;
    EXCEPTION_RECORD* r;
    const unsigned char* code;
    HMODULE mod;
    char name[64];
    unsigned i;
    int fatal;

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
        LogLine(buf);

        /* 故障点所在模块 + 该点前后 8 字节: 判断是引擎指令还是野跳转。 */
        name[0] = 0;
        mod = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)r->ExceptionAddress, &mod) && mod)
            GetModuleFileNameA(mod, name, sizeof(name));
        p = 0;
        p = AppendCStr(buf, p, "CRASH: mod=", sizeof(buf));
        p = AppendCStr(buf, p, name, sizeof(buf));
        p = AppendCStr(buf, p, " base=", sizeof(buf));
        UtoA((unsigned)(size_t)mod, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " at", sizeof(buf));
        for (i = 0; i < 8; i++)
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
        AddVectoredExceptionHandler(1, Moe_CrashFilter);
        MOE_EnsureInit();
        VecText_Install();
        AirWay_Install();
        ChatEdit_Install();
        FM_Install();
    }
    return TRUE;
}
