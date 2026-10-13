/* MOExperience.cpp - DLL 协调层: 把各模块挂到引擎时序点上。
 *   prepare 0x4F42F0 (每循环) -> 各模块 *_Poll
 *   Draw    0x5D4A94 (每帧)   -> 各模块 *_Draw
 * 裸钩铁律: 操作数只许 "m" 禁止 "r" ("r" 会在 pushal 前占 ecx/edx 盖活寄存器)。
 * 函数体中间的钩不能按入口钩读栈参 (栈上是外层帧槽); 入口 eax 用 [esp+0x1C] 取。
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
#include "Observer.h"
#include "OrderThrottle.h"
#include "PatrolCircle.h"
#include "PlaceScatter.h"
#include "PowerDisplay.h"
#include "RepairAll.h"
#include "SideScroll.h"
#include "SmartArea.h"
#include "SmartOrder.h"
#include "SWSBar.h"
#include "SWPanel.h"
#include "TreeChop.h"
#include "WaypointCap.h"
#include "MegaListCap.h"
#include "WeaponRange.h"
#include "ScaleBar.h"
#include "PauseView.h"
#include "MOExperience.h"

/* ---------------- 地址 ---------------- */

#define ADDR_AFTER_MSGLIST_DRAW  0x005D4A9Bu
#define ADDR_SKIP_SCROLL_SIDEBAR 0x00533FC3u

#define OFF_REG_ECX              0x20

/* ---------------- 初始化 ---------------- */

static int g_InitDone = 0;
static int g_MouseHookDone = 0;   /* 鼠标虚表钩装上置 1 (只 0->1) */

/* 宿主守卫: 非 gamemd.exe 不装钩 (引擎地址在别进程是别的数据)。 */
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

/* host 一行 (start 之前打, 双进程各留一行, 下次直接按 pid 区分):
 * moe: host Syringe.exe pid=2668 not gamemd, skipped
 * moe: host gamemd.exe pid=12976 (只记文件名, 路径太长不记)。 */
static void MOE_LogHost(void)
{
    char pb[260], buf[512], b1[16];
    unsigned p = 0, i;
    DWORD n = GetModuleFileNameA(NULL, pb, (DWORD)sizeof(pb));
    if (n == 0 || n >= (DWORD)sizeof(pb)) pb[0] = 0;
    else pb[n] = 0;
    i = n;
    while (i > 0 && pb[i - 1] != '\\' && pb[i - 1] != '/' && pb[i - 1] != ':') i--;
    p = AppendCStr(buf, p, "moe: host ", sizeof(buf));
    p = AppendCStr(buf, p, pb + i, sizeof(buf));
    p = AppendCStr(buf, p, " pid=", sizeof(buf));
    UtoA((unsigned)GetCurrentProcessId(), b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    if (!MOE_HostIsGame())
        p = AppendCStr(buf, p, " not gamemd, skipped", sizeof(buf));
    LogLine(buf);
}

/* 日志打开 + 启动轮转 ([Log] MaxKB, 默认 500, <=0 不限)。
 * 幂等, 每进程只执行一次。超限先截断 (占着也能清), 不行再删, 都不行就续写。 */
#define MOE_LOG_MAXKB_DEF 500
#define MOE_LOG_MAXKB_MAX 1048576u

void GtEnsureLog(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int maxKB, rotated = 0;
    unsigned limit;
    if (g_Log) return;
    BuildPaths();
    maxKB = GetPrivateProfileIntA("Log", "MaxKB", MOE_LOG_MAXKB_DEF, g_IniPath);
    if (maxKB <= 0) limit = 0;
    else
    {
        if ((unsigned)maxKB > MOE_LOG_MAXKB_MAX) maxKB = (int)MOE_LOG_MAXKB_MAX;
        limit = (unsigned)maxKB * 1024u;
    }
    if (limit > 0 &&
        GetFileAttributesExA(g_LogPath, GetFileExInfoStandard, &fa) &&
        !(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        (fa.nFileSizeHigh > 0 || fa.nFileSizeLow > limit))
    {
        HANDLE h = CreateFileA(g_LogPath, GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); rotated = 1; }
        else if (DeleteFileA(g_LogPath)) rotated = 1;
    }
    g_Log = CreateFileA(g_LogPath, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_Log == INVALID_HANDLE_VALUE) g_Log = NULL;
    if (rotated && g_Log)
    {
        char rb[64], n1[16];
        unsigned q = AppendCStr(rb, 0, "log: cleared (exceeded ", sizeof(rb));
        UtoA((unsigned)maxKB, n1);
        q = AppendCStr(rb, q, n1, sizeof(rb));
        AppendCStr(rb, q, "KB)", sizeof(rb));
        LogLine(rb);
    }
}

/* 崩溃定位: 各入口写阶段 id, 崩溃处理打阶段名 + 栈快照 (见 .h)。 */
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
    if (!MOE_HostIsGame()) return;   /* 非游戏宿主: MOE_LogHost 已留带 skipped 的行, 这里不再初始化各模块 */
    LogLine("=====");
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
    SB_EnsureInit();   /* 底栏: 读 [BottomBar] (缩放档位/几何); ScaleEnable=0 时缩放空转 */
    PV_EnsureInit();   /* 底栏: 读 [BottomBar] (暂停按钮 PauseEnable/PauseHotkey) */
}

/* ---------------- 渲染回退原因统计 ----------------
 * kind 0=矢量成功 1=无可见行 2=GDI失败 3=尺寸异常 4=Lock失败 5=无画布 */
static unsigned g_StVec = 0, g_StEmpty = 0, g_StGdi = 0;
static unsigned g_StDims = 0, g_StLock = 0, g_StSurf = 0;
static unsigned g_StLastFrame = 0;

static void StatTick(unsigned frame, int kind)
{
    if (kind == 0) g_StVec++;
    else if (kind == 1) g_StEmpty++;
    else if (kind == 2) g_StGdi++;
    else if (kind == 3) g_StDims++;
    else if (kind == 4) g_StLock++;
    else g_StSurf++;
    if (frame - g_StLastFrame < 1800) return;
    {
        char b1[16], b2[16], b3[16], b4[16], b5[16], buf[256];
        unsigned p = 0;
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
}

/* 每循环 prepare 裸钩 (首 5B 一条指令, 跳板拷 5B 跳回 0x4F42F5)。
 * Entry 尾跳跳板 (非 call, 栈形全程 [ret][arg], 原 ret 4)。 */

void* g_PrepareTramp = NULL;

static void PowerDrawBoth(void)
{
    AirWay_Poll();
    BuildQueue_Poll();
    PatrolCircle_Poll();
    SmartOrder_Poll();
    TreeChop_Poll();
    FuzzyAttack_Poll();
    AttackMove_Poll();
    SmartArea_Poll();
    DeployPreview_Poll();
    MindSelect_Poll();
    OrderThrottle_Poll();
    WpCap_Poll();
    MlCap_Poll();
    SWS_Poll();
    RepairAll_Poll();
    /* 侧栏放建筑时驱散地基上的自家单位 (只读+标准 Move 事件)。 */
    PlaceScatter_Poll();
    /* 多观战修复 + 观战面板排序 (联机安全, 见 Observer.cpp)。 */
    Observer_Poll();
    if (!g_InitDone) MOE_EnsureInit();
    /* 撑满视口 + 摘侧栏 gadget (隐藏原版建造栏时必须每循环都跑, 引擎每次
     * 切页/改视口都会把按钮重新挂回去)。 */
    FM_Poll();
    /* 原版侧栏滚动条 (拖滑块定滚动位置)。必须在 FM_Poll 之后 —— 它靠
     * FM_SidebarVisible() 判断侧栏列此刻归谁 (态1/2 那一列是地图的)。 */
    SS_Poll();
    /* 缩放刻度条: 探整图 / 建档位表 / 摆条 / 点选滚轮换档。必须在 FM_Poll 之后
     * —— 落位要用 FM_Poll 刚刷新出来的视口矩形。 */
    SB_Poll();
    /* 单机暂停/步进: F5 边沿 + 底栏两按钮点击 (暂停中经迷你循环的 Mark 钩
     * 照样跑到这里)。 */
    PV_Poll();
    /* CCToolTip 选面/量宽用的矩形影子 = 满视图区 A (见 ScaleBar.cpp)。*/
    SB_PublishAView();
    /* 鼠标源头折算 (缩放): 注入可能早于 WWMouse 构造, 没装上就在这里每循环重试。*/
    if (!g_MouseHookDone) MOE_InstallMousePosHook();
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

/* 上屏入口钩: 每次条带呈现前先恢复底部电量 (引擎重画/上屏会盖掉)。
 * FullMap 接管时只 blit 小地图块并跳过原版整条侧栏 blit (脏标记照补)。 */

void* g_BlitSbTramp = NULL;

/* FullMap 接管时用的出口: 原版尾声等价物 + ret 4 (原 ret 4, 参数栈形一致)。
 * 只做"脏标记 + 返回", 不碰任何寄存器。 */

static void BlitSbEntryDraw(void)
{
    void* tramp;
    MOE_EnsureInit();
    MOE_SetPhase(MP_BLITSIDEBAR);
    /* 侧栏滚动条要在**呈现之前**画进侧栏面 (侧栏面靠 BlitSidebar 带上屏);
     * 排在 PowerDisplay 之后 = 层级最上, 不被底电那行字盖住。 */
    SS_Draw();
    PowerDisplay_DrawSb();   /* 先画字再上屏, 否则晚一帧闪烁。 */
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

/* 呈现入口钩 (空操作占位, 只保栈形平衡)。 */
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
 * GScreenClass::Render (0x4F4480) 入口把 [0x88731C]/[0x8872FC] 换成 R 大面
 * (两槽尺寸必须一致), 渲染完还原并降采样。首 8B 两条完整指令, 跳回 0x4F4488。 */
void* g_RenderTramp = NULL;

static void RenderEntrySwap(void)
{
    /* 必须在引擎动 R 之前 (SwapIn 那一步会把 [0x88731C] 换成大面)。*/
    SB_ViewPreRender();
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

/* 缩小档: 大面暴露窗口压到只剩三趟地图 (0x4F4492 让出, 0x4F44CB 装回)。
 * 函数体中间的钩, 壳里不改寄存器。 */
void* g_PreCurTramp = NULL;
void* g_MapTramp = NULL;
/* SB_RenderSuspend 置 1 时把 eax 换成 g_PreCurSurf (光标那几笔直接吃 eax, 不看全局)。*/
int   g_PreCurWin = 0;
void* g_PreCurSurf = NULL;

/* 0x4F4492 在函数体中间: 换 eax 只能改 pushal 帧的 EAX 槽 [esp+0x1C]。 */
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

/* chrome pass (底栏/侧栏按窗口坐标画): 0x4F44FD 让出, 0x4F4505 装回。 */
void* g_ChromePreTramp = NULL;
void* g_ChromePostTramp = NULL;

static void ChromePreC(void)
{
    SB_RenderSuspend(0);
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

/* 三趟地图画完、引擎 UI 之前还原窗口面 + 降采样 (首 5B, 跳回 0x4F4520)。 */
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


/* 边缘卷屏死区按屏幕空间算: 入口把点换回物理鼠标 (首 7B, 跳回 0x692B67)。 */
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

/* ProcessClickCoords (所有地图点击的总闸, 只判 X<0/Y<0、无上界): 缩小档下
 * 视口外的点入口明确拒掉 (映射值 SB_PtInView + 物理坐标 SB_PtInRawView,
 * 任一出界即拒), 否则底栏点击变地图下令。只 SB_Out 动手。
 * 拒掉时自己 ret 0x18。首 7B, 跳板跳回 0x692307。
 *
 * ⚠️ 例外: 侧栏顶部那一块走小地图点击门 (0x692406), 判据只能是**物理**鼠标
 * 位置 (映射值已钳进 R, 看不出界; 见 FM_ShouldDeferClick)。 */
void* g_ClickCoordsTramp = NULL;

static int ClickCoordsReject(void* pt)
{
    static int s_tick = 0;
    if (!IsPlausiblePtr(pt)) return 0;
    if (SB_PtInView(((const int*)pt)[0], ((const int*)pt)[1])) return 0;
    /* 侧栏顶部那一块交给后面的点击门吞, 别在这里拒掉 (放行而没人接手 = 点成对
     * 地图某点的下令, 比点不动更糟, 所以判据里含"门武装好了没有")。*/
    if (SB_Zoomed() && FM_ShouldDeferClick()) return 0;
    /* 映射值已全钳制 (框选不断崖), 出界只能看物理: 栏带点击物理必在 A 外。
     * Gt 失败则沿用旧路 (映射判), 不多事。 */
    if (SB_Zoomed())
    {
        int rx = 0, ry = 0;
        if (GtMouseGameXY(&rx, &ry) && !SB_PtInRawView(rx, ry)) return 1;
    }
    /* 真拒掉才打一条 (2 秒一条): 确认底栏点击走到了这条总闸。 */
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

/* Clamp_Center 接管: 视口比地图大时钉正中。整条替换原版、不回调,
 * 只在缩放启用时装钩 (见 VecText.cpp)。 */
static int SbClampCenter(void* self, Point2D* c)
{
    (void)self;
    if (!IsPlausiblePtr(c)) return 0;
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
        "ret $4\n\t"                     /* 与原版 epilogue 一致 */
        :
        : "m" (g_CallClamp)
        : "memory", "cc");
}


/* 迷雾 DrawPixelEffects 入口钩: ViewBounds=R 而面是窗口面即越界, 先换大面挡。
 * 挂函数入口 (跳回 0x6D7845), 不挂 E8 调用点。引擎自己还原。 */
void* g_ShroudTramp = NULL;

static void ShroudC(void) { SB_ShroudGuard(); }
static void* g_CallShroud = (void*)ShroudC;

__attribute__((naked)) void MOExperience_ShroudEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "call *%0\n\t"
        "popal\n\t"
        "jmp *%1\n\t"                     /* 跳板 = 原 5B + jmp 0x6D7845 */
        :
        : "m" (g_CallShroud), "m" (g_ShroudTramp)
        : "memory", "cc");
}

/* 通用拉伸 blit (0x6BA580) 守卫: 面/矩形非法或矩形两张面都不沾即跳过 (防崩)。
 * 只 SB_Zoomed 检查, 1x 原样跑跳板。跳过时同样 ret 0x10。 */
void* g_BlitGuardTramp = NULL;

static int BlitGuardC(void* r, void* s1, void* s2)
{
    void** vt;
    int* ri;
    int i;
    if (!SB_Zoomed()) return 1;
    /* 面指针野了直接跳过。*/
    if (!IsPlausiblePtr(s1) || !IsPlausiblePtr(s2))
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
    /* 矩形先验 (纯算术): 野矩形不必再付虚表调用。 */
    if (!IsPlausiblePtr(r))
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
    /* 尺寸/pitch 离谱也跳过 (16bpp pitch = width*2 对齐, 野了行指针飞)。 */
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
        /* ---------------- 0x6BA580 的矩形坐标系 (判据必须照它写) ----------------
         * `BlitImage(rect, src)` 一个矩形给两张面共用, 但两边用法完全不同:
         *   源: `Lock(0,0)` (0x6BA5B8) 之后线性走, 每行 `src += 2*w` (0x6BA637)
         *       ⇒ 源的 x/y 根本没用, 只要 w<=w2 且 h<=h2 就安全。
         *   目标: `Lock(0,0) + (x+y)*2` (0x6BA628), 行步进 = 目标 pitch
         *       (0x6BA653) ⇒ 目标必须 x,y,w,h 整个落在 s1 内 (§25 那起 0x6BA643)。
         * 两边各按各的判; 目标不合就把 x/y 夹进 s1 照画; 两张都沾不上才跳过。 */
        {
            int okSrc = (ri[2] > 0 && ri[3] > 0 && ri[2] <= w2 && ri[3] <= h2);
            int okDst = (ri[0] >= 0 && ri[1] >= 0 && ri[2] > 0 && ri[3] > 0
                         && ri[0] <= w1 - ri[2] && ri[1] <= h1 - ri[3]);
            if (!okSrc && !okDst)
            {
                static int l = 0;
                if (!l)
                {
                    char b1[16], buf[160];
                    unsigned q = 0;
                    l = 1;
                    q = AppendCStr(buf, q, "blit: skip unclipped s1=", sizeof(buf));
                    UtoA((unsigned)w1, b1);
                    q = AppendCStr(buf, q, b1, sizeof(buf));
                    q = AppendCh(buf, q, 'x', sizeof(buf));
                    UtoA((unsigned)h1, b1);
                    q = AppendCStr(buf, q, b1, sizeof(buf));
                    q = AppendCStr(buf, q, " s2=", sizeof(buf));
                    UtoA((unsigned)w2, b1);
                    q = AppendCStr(buf, q, b1, sizeof(buf));
                    q = AppendCh(buf, q, 'x', sizeof(buf));
                    UtoA((unsigned)h2, b1);
                    q = AppendCStr(buf, q, b1, sizeof(buf));
                    q = AppendCStr(buf, q, " rect=", sizeof(buf));
                    UtoA((unsigned)ri[0], b1);
                    q = AppendCStr(buf, q, b1, sizeof(buf));
                    q = AppendCh(buf, q, ',', sizeof(buf));
                    UtoA((unsigned)ri[1], b1);
                    q = AppendCStr(buf, q, b1, sizeof(buf));
                    q = AppendCh(buf, q, ',', sizeof(buf));
                    UtoA((unsigned)ri[2], b1);
                    q = AppendCStr(buf, q, b1, sizeof(buf));
                    q = AppendCh(buf, q, ',', sizeof(buf));
                    UtoA((unsigned)ri[3], b1);
                    q = AppendCStr(buf, q, b1, sizeof(buf));
                    LogLine(buf);
                }
                return 0;
            }
        /* 源容得下但 x/y 出目标面 (侧栏图标那类): 把 x/y 夹进 s1 再照画。宁可位置略有
         * 偏差, 也别整块不画, 而且绝不越界。 */
        if (!okDst && okSrc)
        {
            if (ri[2] > w1) ri[2] = w1;
            if (ri[3] > h1) ri[3] = h1;
            if (ri[2] <= 0 || ri[3] <= 0) return 0;
            if (ri[0] < 0) ri[0] = 0;
            if (ri[1] < 0) ri[1] = 0;
            if (ri[0] > w1 - ri[2]) ri[0] = w1 - ri[2];
            if (ri[1] > h1 - ri[3]) ri[1] = h1 - ri[3];
        }
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


/* 放大前那次提示框绘制跳过: 放大档只跑 Process 不画, 否则重影。
 * 须自己跑 Process (刷 Dimension/计时), 只跳绘制。 */
#define ADDR_TIPMANAGER      0x00887368u  /* CCToolTip::Instance */
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
    g_TipPreDrawDest = SB_In()
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

/* 引擎看到的鼠标位置 (源头折算): 折虚表 +0x34 (11 个消费点一次覆盖),
 * gadget/光标/雷达走别的槽不受影响。1x 恒等。 */
#define ADDR_WWMOUSE        0x00887640u
#define WWMOUSE_VT_GETPOS   0x34u
#define WWMOUSE_OFF_X       0x1C
#define WWMOUSE_OFF_Y       0x20

static Point2D s_MouseVp;
static void*   g_MousePosRet = 0;    /* 裸壳 popal 之后要把返回值塞回 EAX */
static void*   g_MouseVtSlot = 0;
static void*   g_MouseVtOld  = 0;

static void MOE_MousePos(void* out)
{
    void* m = *(void**)ADDR_WWMOUSE;
    int mx = 0, my = 0;
    if (!IsPlausiblePtr(m))
    {
        g_MousePosRet = (Point2D*)out;
        return;
    }
    mx = *(int*)((char*)m + WWMOUSE_OFF_X);
    my = *(int*)((char*)m + WWMOUSE_OFF_Y);
    s_MouseVp.X = mx;
    s_MouseVp.Y = my;
    if (SB_Zoomed()) SB_MapPoint(&s_MouseVp);
    /* 出参没人读也顺手填上 (调用者栈上临时量, 同口径防脏值)。 */
    if (out && IsPlausiblePtr(out))
    {
        ((Point2D*)out)->X = s_MouseVp.X;
        ((Point2D*)out)->Y = s_MouseVp.Y;
        g_MousePosRet = (Point2D*)out;
    }
    else g_MousePosRet = &s_MouseVp;
}

/* 虚表槽 __thiscall, 1 栈参 ⇒ ret 4。入点 [esp+4]=出参, pushal 后在 [esp+36]。 */
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
    if (!IsPlausiblePtr(m))
    {
        /* 注入早于 WWMouse 构造: 下循环重试 (日志只打一次)。 */
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

/* 鼠标按钮消息路 (0x6930A0): 点击点从 Win32 lParam 取, 不过 WWMouse。
 * 只换第 4 实参那个指针为折算副本 (槽在 0x6930A0 自己栈帧上, ret 弹掉不影响
 * 窗口过程); 副本须是打包好的 lParam ((y<<16)|x), 不能放 Point2D。
 * 不折则"按下点"(物理)与"当前点"(视口)差 A/R 倍 ⇒ 单击判拖框。
 * call 前后手动 push/pop ecx (入口 ecx=this, 跳板拿它当 DisplayClass)。
 * 栈逐字节平衡 (尾跳跳板, 原 ret 0x10)。 */

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
        SB_MapPoint(&pt);
        x = pt.X;
        y = pt.Y;
    }
    /* lParam 按 (y<<16)|x 打包, masking 收尾 (值域早被映射钳住, 不会 wrap)。 */
    out = ((unsigned)(y & 0xFFFF) << 16) | (unsigned)(x & 0xFFFF);
    s_WndLp = out;
}

static void* g_CallWndFix = (void*)MOE_WndFixLParam;

__attribute__((naked)) void MOExperience_WndMouseEntry(void)
{
    /* 裸钩铁律见文件头: 操作数只许 "m"; call 前后手动保 ecx (入口 this);
     * 栈逐字节平衡 (尾跳跳板)。 */
    __asm__ volatile (
        "movl 0x10(%%esp), %%eax\n\t"    /* 第4实参 = &lParam */
        "pushl %%ecx\n\t"                /* 保住 this */
        "pushl %%eax\n\t"
        "call *%0\n\t"                   /* 折算进 s_WndLp (cdecl, 1 个实参) */
        "addl $4, %%esp\n\t"
        "popl %%ecx\n\t"                 /* 还原 this */
        "movl %1, %%eax\n\t"             /* eax = &s_WndLp */
        "movl %%eax, 0x10(%%esp)\n\t"    /* 第4实参换成我们的副本 */
        "jmp *%2\n\t"                    /* 尾跳跳板 → 0x6930A8 */
        :
        : "m" (g_CallWndFix), "m" (s_WndLpP), "m" (g_WndMouseTramp)
        : "memory", "cc");
}

/* InitGUI 入口钩: 先纠 SidebarBounds.X (视口铺满后一次性布局全错 168)。 */
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

/* gadget 输入钩: 先把横跨视口的容器宽度收成"视口减侧栏列" (FM_NoteGadgetInput)。 */

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

/* UI 层链路 (放大呈现之后跑, 各层仍 1:1。顺序见函数内, 后者压前者)。 */
int MOE_DrawUILayers(void* surf, int surfW, int surfH, int surfPitch,
                     int* bitLineH, int* screenW, int* pMsgDrew)
{
    int drew = 0, rc, lh, sw;
    RectangleStruct viewA;
    MOE_SetPhase(MP_UILAYER);
    if (pMsgDrew) *pMsgDrew = 0;
    if (!IsPlausiblePtr(surf)) return 0;
    if (surfW <= 0 || surfH <= 0 || surfPitch <= 0) return 0;
    lh = GtBitLineHeight();
    /* 布局基准取视图区 A (1752), 不是视口 R (放大档 R 才几百, 居中全偏左)。 */
    FM_GetViewRects(&viewA, 0);
    sw = (viewA.Width > 200) ? viewA.Width : GtScreenW();
    if (bitLineH) *bitLineH = lh;
    if (screenW) *screenW = sw;
    /* inband 门判据先给: MsgText 提前 return 会跳过, 原版名单就盖不住。 */
    g_SWCompW = surfW; g_SWCompH = surfH;
    g_MapRight = FM_MapRightEdge();
    rc = MsgText_DrawFrame(surf, lh, sw, surfW, surfH, surfPitch);
    if (rc < 0) return drew;
    if (rc == 1) { drew = 1; if (pMsgDrew) *pMsgDrew = 1; }
    rc = SWPanel_Draw(surf, lh, surfW, surfH, surfPitch);
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
    int bitLineH = 0, screenW = 0;
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
         * GtMouseGameXY 回退到原始坐标 (恒等, 不强行套 Composite 宽度)。
         * 分辨率不变不重设 (每帧 2 次虚表调用省掉)。 */
        static int s_PrimW = -1, s_PrimH = -1;
        void* prim = *(void**)ADDR_SURF_PRIMARY;
        int pw = GtSurfGet(prim, GtVT_GET_WIDTH);
        int ph = GtSurfGet(prim, GtVT_GET_HEIGHT);
        int cw = (pw > 100 && ph > 100) ? pw : 0;
        int ch = (pw > 100 && ph > 100) ? ph : 0;
        if (cw != s_PrimW || ch != s_PrimH)
        {
            s_PrimW = cw; s_PrimH = ch;
            GtSetGameSize(cw, ch);
        }
    }
    GtRefreshWinSize(tick);

    /* 缩放呈现 (UI 链之前): 改 Composite (它是上屏 blit 的源, 改 Primary 只闪一瞬)。
     * 缩小档的还原+铺图外+降采样在 AfterMap (0x4F451B) 做, 这样引擎 UI 不会落进大面;
     * 该钩没装上时才回退到这里跑。 */
    if (SB_In()) SB_CompositeMagnify();
    else if (SB_Out() && !g_AfterMapTramp) { SB_PaintVoid(); SB_CompositeDemagnify(); }

    drew = MOE_DrawUILayers(surf, surfW, surfH, surfPitch, &bitLineH, &screenW, NULL);

    /* 聊天输入框 (顶层)。ce_Active/CE_NoteSent 只由它驱动, 不接则颜色丢失。 */
    {
        int ce;
        ce = ChatEdit_Frame(surf, bitLineH, screenW,
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
    /* 暂停/步进/格网三按钮 (刻度条左侧, 启用后按过一次 F5 才画; 格网另带灰线)。 */
    if (PV_DrawToComposite()) drew = 1;

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
    /* 接管开即恒跳过原版, 不看本帧画没画出 (否则空帧原版闪一帧)。
     * 未接管才放行原版。 */
    if (MsgText_Enabled()) return ADDR_AFTER_MSGLIST_DRAW;
    return 0;
}

/* 消息帧兜底入口: 链表空时 0x5D4A94 走不到, 这里代跑整条链路。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_MsgListEntry(void* regs)
{
    void* self;
    /* 取不到 this 按"非空"保守处理 (留给 0x5D4A94, 绝不崩)。 */
    self = regs ? *(void**)((char*)regs + OFF_REG_ECX) : NULL;
    if (self && IsPlausiblePtr(self) && *(void**)self) return 0;
    {
        static int s_logged = 0;
        if (!s_logged) { s_logged = 1; LogLine("green: engine list empty, render from entry"); }
    }
    MOExperience_MessageDrawHook(regs);
    return 0;
}

/* 滚轮: 超武侧栏优先 (悬停其上时), 否则战役框历史。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_WheelHook(void* regs)
{
    int mx, my;
    unsigned wparam;

    MOE_EnsureInit();
    if (!GtMouseGameXY(&mx, &my)) return 0;

    wparam = *(unsigned*)((char*)regs + OFF_REG_ECX);
/* wparam 高位 (bit31) 为负 = 下滚。两框"上/下"语义相反, dir 反向传 (非 bug):
 * 战役框最新在下 (下滚 skip 减), 超武栏 0 项在上 (下滚 scroll 加)。 */
    if (SWS_WheelIn(mx, my, (wparam & 0x80000000u) ? 1 : -1)) return 0;
    MsgText_WheelIn(mx, my, (wparam & 0x80000000u) ? 1 : 0);
    return 0;
}

/* 侧栏滚动: 悬停超武侧栏/战役框时跳过 (那一滚已分别给出去)。 */
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

/* 崩溃现场捕获: 向量处理器 (不被覆盖), 只记致命异常, 照旧崩 (不改控制流)。 */
static volatile LONG g_CrashBusy = 0;

/* 把 base 起 n 个 dword 接在 buf 后 (读不了留空, 不做探测)。 */
static unsigned MOE_DumpWords(char* buf, unsigned p, unsigned base, int n,
                            unsigned cap)
{
    char b1[16];
    int i, ok;
    /* 跑在崩溃线程自己的栈上, [esp, esp+n*4) 必然可读, 无需探测。 */
    ok = ((base & 3u) == 0 && IsPlausiblePtr((void*)(size_t)base)) ? 1 : 0;
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
        /* cap 传调用方缓冲区的真实大小 (传小了会被静默截断)。 */
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
        p = AppendCStr(buf, p, " tid=", sizeof(buf));
        UtoA((unsigned)GetCurrentThreadId(), b1); p = AppendCStr(buf, p, b1, sizeof(buf));
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

        /* 阶段名 + 两个栈窗口 (esp / ebp, ebp 窗是返回地址所在)。 */
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

        /* 面快照 (只读 5 个引擎全局 + 我们 4 个面的指针整数, 零解引用):
         * 判断崩溃现场的面是不是我们的 (泄漏残留/退休面/野面)。 */
        p = 0;
        p = AppendCStr(buf, p, "MOE: surf sb=", sizeof(buf));
        UtoA(*(const unsigned*)(size_t)0x00887300u, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " alt=", sizeof(buf));
        UtoA(*(const unsigned*)(size_t)0x00887310u, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " cur=", sizeof(buf));
        UtoA(*(const unsigned*)(size_t)0x00887314u, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " comp=", sizeof(buf));
        UtoA(*(const unsigned*)(size_t)0x0088731Cu, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " map=", sizeof(buf));
        UtoA(*(const unsigned*)(size_t)0x008872FCu, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        SB_AppendCrashSurfs(buf, &p, sizeof(buf));
        LogLine(buf);

        /* 故障点所在模块 + 前后 8 字节 (野跳转时读字节会二次异常, 先判可读)。 */
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
        if (!MOE_HostIsGame()) return TRUE;
        AddVectoredExceptionHandler(1, Moe_CrashFilter);
        MOE_EnsureInit();
        VecText_Install();
        AirWay_Install();
        ChatEdit_Install();
        FM_Install();
    }
    return TRUE;
}
