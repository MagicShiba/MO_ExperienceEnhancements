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
 * 构建见 构建.bat (32 位 mingw, 无 CRT)。
 */

#include <windows.h>
#include "GdiText.h"
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
 * 不碰光标/脏旗, 无拖影。 */
#define ADDR_BLITSB        0x6A70E0u

void* g_BlitSbTramp = NULL;

static void BlitSbEntryDraw(void)
{
    MOE_EnsureInit();
    PowerDisplay_DrawSb();
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

/* ---------------- 呈现入口钩 (已退役为空操作) ----------------
 * GScreenClass::UpdatePrimarySurface=0x4F4780 钩子保留安装 (进出栈平衡),
 * 但不再绘制: 往呈现面直画层级盖过鼠标、侧栏不呈现时仍冒字,
 * 故只画侧栏面、随条带上屏。 */
void* g_UpdateTramp = NULL;

static void DrawPresentArg(void* surf)
{
    (void)surf;
    /* 退役: 不再往呈现面直画 (盖鼠标 + 侧栏隐藏仍冒字), 保持空操作。
     * 钩子保留安装 (进出栈平衡), 但不再绘制。 */
    MOE_EnsureInit();
    return;
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
     * 主画布尺寸同时回填给收集门: 坐标系一致才收, 否则走原版。 */
    g_SWCompW = surfW; g_SWCompH = surfH;
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

    /* 5 底部电量: 只画侧栏面 (总开关 PowerBottom=0 即关)。
     * 随 BlitSidebar 条带上屏, 层级在鼠标之下, 侧栏不画即不显示。 */
    {
        int pwr = PowerDisplay_DrawSb();
        if (pwr < 0) { StatTick(tick, pwr == -1 ? 2 : 4); return 0; }
        if (pwr == 1) drew = 1;
    }

    /* 6 自定义聊天输入框: 盖住原生输入行, 顶层 (未激活即无操作) */
    {
        int ced = ChatEdit_Frame(surf, bitLineH, screenW, surfW, surfH, surfPitch);
        if (ced == 1) drew = 1;
    }

    if (!drew) { StatTick(tick, 1); return 0; }
    StatTick(tick, 0);
    if (s_firstDraw)
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
    /* 战役框: wparam 高位=上滚 -> g_Skip 减小 (看更新的)。
     * 超武栏 s_Scroll 是"第一条可见的条目下标", 上滚应看更靠前=减小。
     * 实测这个方向是反的, 所以这里传反的 dir, 手感才和战役框一致。 */
    if (SWS_WheelIn(mx, my, (wparam & 0x80000000u) ? 1 : -1)) return 0;
    MsgText_WheelIn(mx, my, (wparam & 0x80000000u) ? 0 : 1);
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

extern "C" __declspec(dllexport) BOOL __stdcall DllMain(HINSTANCE h, DWORD reason, LPVOID res)
{
    (void)h; (void)res;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
        MOE_EnsureInit();
        VecText_Install();
        AirWay_Install();
        ChatEdit_Install();
    }
    return TRUE;
}
