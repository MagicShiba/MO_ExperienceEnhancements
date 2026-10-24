/* FrameCap.cpp - ESC 菜单/设置页限帧 (固定 60)。
 *
 * 引擎主循环 (0x55D360) 和模态对话框循环都没有帧率节流, 窗口化下 cnc-ddraw 的
 * Blt 也不阻塞游戏线程; 菜单打开后逻辑暂停、每帧极轻, 循环能跑到上千 Hz 把单核
 * 打满。这里挂一个 60 节拍器, 到点才放行。
 *
 * 两条挂点, 每个模式只允许一个节拍器 (多用会让一轮迭代推后多个周期):
 *   LOOP  (菜单开着 + 帧号还在动) -> 只有主循环门 0x55D360 节拍
 *   MODAL (帧号冻结 = 模态循环)  -> 只有"队列空"的那次 PeekMessage 节拍
 *
 * 计时用 QPC; 等待用 MsgWaitForMultipleObjects(QS_ALLINPUT), 有输入立刻醒。
 * 只在队列空时等: 拿到消息必须马上交回, 否则消息吞吐被钉在 60 条/秒。
 * 只在 ESC 菜单/设置页打开时限速, 联机与载入屏一律放行。
 *
 * 配置: [FrameCap] Enable=1。设计取舍与实测见 notes/帧率限制.md。
 */
#include <windows.h>
#include "GdiText.h"
#include "FrameCap.h"

#define ADDR_FRAME_NO    0x00A8ED84u /* 主循环帧号 (每帧 inc) */
#define ADDR_LOAD_PHASE  0x00A9FAB0u /* 进度屏相位 (1/2/3 = 载入中) */
#define ADDR_IAT_PEEK    0x007E1484u /* 导入表里 PeekMessageA 的 thunk */
#define ADDR_GAMEMODE    0x00A8B238u /* SessionClass::Instance 首成员 = GameMode */

#define FC_FPS           60
#define FC_MENU_FREEZE   120u        /* 菜单开着但帧号这么久没动 => 已进模态循环 */
#define FC_SUB_FREEZE    300u        /* 没注册对话框 (子页面) => 帧号冻结这么久才算 */
#define FC_PACED_MS      15          /* 帧周期 >= 15ms => 引擎自己在按 GameSpeed 压帧 */

enum { FC_MODE_NONE = 0, FC_MODE_LOOP = 1, FC_MODE_MODAL = 2 };

static int s_Init = 0;
static int s_Enable = 1;
static DWORD s_MainTid = 0;
static void* s_Dlg = NULL;           /* 当前 ESC 对话框 */

static long long s_Qpf = 0;
static long long s_Until = 0;        /* 下一节拍时刻 (QPC), 0 = 未定 */
static UINT (WINAPI *s_pBeginPeriod)(UINT) = NULL;
static int s_Boosted = 0;

static unsigned s_FrNo = 0, s_FrAt = 0;   /* 帧号 + 上次变化时刻 */
static unsigned s_ChkAt = 0;
static int s_ChkMode = FC_MODE_NONE;

static long long s_GateAt = 0;            /* 上次门的时刻 */
static long long s_FrameEst = 0;          /* 帧周期估计 (只采样我们没等的帧) */
static int s_EnginePaced = 0;             /* 上一帧引擎自己已在节流 */

static int s_Cnc = -1;               /* ddraw 是否 cnc-ddraw: -1=未判定 0=其它 1=是 */
static void* s_SysPeek = NULL;       /* user32!PeekMessageA 本体 (未被动过的槽) */
static void HookPeek(void);

static long long FC_Now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

/* ---------------- 何时限速 ---------------- */

static int MenuOpen(void)
{
    if (!s_Dlg) return 0;
    if (!IsWindow((HWND)s_Dlg)) { s_Dlg = NULL; return 0; }
    return IsWindowVisible((HWND)s_Dlg) ? 1 : 0;
}

/* 返回模式; FC_MODE_NONE = 此刻不该限速。
 * 子页面 (游戏控制/载入/储存/删除/放弃) 不走注册对话框那条路, 打开时首页还会被
 * 隐藏, 所以只能靠"帧号不推进"判定; 读盘时帧号同样不推进, 用进度屏相位排掉。
 * 结果缓存 20ms: 模态空转时每秒会问上百万次。 */
static int FC_Mode(void)
{
    unsigned now = GetTickCount();
    unsigned f, ph;
    int menu, frozen, gm;

    if (s_ChkAt && (unsigned)(now - s_ChkAt) < 20) return s_ChkMode;
    s_ChkAt = now ? now : 1;
    s_ChkMode = FC_MODE_NONE;

    f = *(volatile unsigned*)(size_t)ADDR_FRAME_NO;
    /* 首次 (或帧号恰为 0 的开局前) s_FrAt 还没建立基线, 直接取 now,
     * 否则 frozen = now - 0 会是个天文数字, 没开菜单也判成模态。 */
    if (f != s_FrNo || !s_FrAt) { s_FrNo = f; s_FrAt = now; }
    ph = *(volatile unsigned*)(size_t)ADDR_LOAD_PHASE;
    if (ph >= 1 && ph <= 3) return FC_MODE_NONE;      /* 载入屏: 别压 */

    gm = *(volatile int*)(size_t)ADDR_GAMEMODE;
    if (gm == 3 || gm == 4) return FC_MODE_NONE;      /* 联机: 帧率归主机管 */

    menu = MenuOpen();
    frozen = (int)(now - s_FrAt);
    if (!menu)
    {
        if (frozen >= (int)FC_SUB_FREEZE) s_ChkMode = FC_MODE_MODAL;
        return s_ChkMode;
    }
    s_ChkMode = (frozen >= (int)FC_MENU_FREEZE) ? FC_MODE_MODAL : FC_MODE_LOOP;
    return s_ChkMode;
}

/* ---------------- 等待 ---------------- */

/* 默认 15.6ms 粒度下 Sleep/MsgWait 都会向上取整, 17ms 的节拍凑不出来。
 * 只开不关 (GameSpeed 的 >60 档也要 1ms, 反复开关会让切档后掉回粗粒度)。 */
static void FC_TimerBoost(void)
{
    if (s_Boosted) return;
    s_Boosted = 1;
    if (s_pBeginPeriod) s_pBeginPeriod(1u);
}

/* 等到 until; 期间只要有输入/投递消息就立刻返回 —— 点击不用等满一个周期。 */
static void FC_WaitUntil(long long until)
{
    for (;;)
    {
        long long left = until - FC_Now();
        DWORD ms;
        if (left <= 0) return;
        ms = (DWORD)((left * 1000 + s_Qpf - 1) / s_Qpf);   /* 向上取整 */
        if (ms == 0) ms = 1;
        if (ms > 16) ms = 16;
        if (MsgWaitForMultipleObjects(0, NULL, FALSE, ms, QS_ALLINPUT) != WAIT_TIMEOUT)
            return;
    }
}

/* 一个节拍: 到点前等, 到点后把 deadline 推一个周期 (落后整周期以上则重排, 不追帧) */
static void FC_Pace(void)
{
    long long now, period;

    if (!s_Qpf) return;
    period = s_Qpf / FC_FPS;
    now = FC_Now();
    if (!s_Until || now - s_Until > s_Qpf / 5 || s_Until - now > s_Qpf / 5) s_Until = now;
    if (now < s_Until)
    {
        FC_TimerBoost();
        FC_WaitUntil(s_Until);
        now = FC_Now();
    }
    if (now - s_Until > period) s_Until = now + period;
    else s_Until += period;
}

/* LOOP 模式专用: 引擎自己的帧定时器 (0x55E160) 可能已经按 GameSpeed 在压帧
 * (档位 1 = 60), 我们再加一层就成了 30。只采样我们没等的帧间隔, 平滑后
 * >= 15ms 就认为引擎已在节流, 本帧不再等。 */
static int FC_EngineAlreadyPaced(void)
{
    long long now = FC_Now();
    long long dt = s_GateAt ? now - s_GateAt : 0;

    if (dt > 0 && !s_EnginePaced)
    {
        if (dt > s_Qpf / 5) s_FrameEst = 0;                 /* 停顿/切场景: 重估 */
        else s_FrameEst = s_FrameEst ? s_FrameEst + (dt - s_FrameEst) / 4 : dt;
    }
    s_GateAt = now;
    s_EnginePaced = (s_FrameEst >= s_Qpf * FC_PACED_MS / 1000) ? 1 : 0;
    if (s_EnginePaced) s_Until = 0;
    return s_EnginePaced;
}

/* ---------------- 挂点 ----------------
 * isGate: 1 = 主循环门 (每帧一次), 0 = 队列空的 PeekMessage */
static void FC_Cap(int isGate)
{
    int mode;

    if (!s_Init) FrameCap_EnsureInit();
    if (s_MainTid && GetCurrentThreadId() != s_MainTid) return;
    if (s_Cnc != 0) HookPeek();      /* 被 cnc-ddraw 抢走就挂回来 (只是指针比较) */
    if (!s_Enable) { s_Until = 0; return; }

    mode = FC_Mode();
    if (mode == FC_MODE_NONE) { s_Until = 0; return; }

    /* 每个模式只有一个节拍器, 别的挂点直接放行, 否则一轮迭代要等多次。 */
    if (mode == FC_MODE_LOOP ? !isGate : isGate) return;

    if (mode == FC_MODE_LOOP && FC_EngineAlreadyPaced()) return;
    FC_Pace();
}

void FrameCap_Gate(void)
{
    FC_Cap(1);
}

/* ---------------- PeekMessageA 导入表钩 ----------------
 * 只改 IAT 指针不动代码字节。cnc-ddraw 建 DirectDraw 时会把这一槽换成它的
 * fake_PeekMessageA (real 指向系统函数), 钩子会被整个绕过, 所以每帧比一次,
 * 不是我们就挂回去, 链路是 fc → cnc-ddraw fake → 系统函数。
 * 别家 ddraw 包装器不碰这一槽, 且重挂可能互相递归, 所以只认 cnc-ddraw:
 * 7.x 导出了 DDGetProcAddress / DDIsWindowed / DDEnableZoom。 */
typedef BOOL (WINAPI *PeekMsgFn)(LPMSG, HWND, UINT, UINT, UINT);

static BOOL WINAPI FC_PeekMessage(LPMSG, HWND, UINT, UINT, UINT);

static PeekMsgFn s_OrigPeek = NULL;

static int IsCncDdraw(void)
{
    HMODULE h;

    if (s_Cnc >= 0) return s_Cnc;
    h = GetModuleHandleA("ddraw.dll");
    if (!h) return 0;                /* 还没加载, 下次再看 */
    s_Cnc = (GetProcAddress(h, "DDGetProcAddress") ||
             GetProcAddress(h, "DDIsWindowed") ||
             GetProcAddress(h, "DDEnableZoom")) ? 1 : 0;
    return s_Cnc;
}

static void HookPeek(void)
{
    PeekMsgFn* slot = (PeekMsgFn*)(size_t)ADDR_IAT_PEEK;
    PeekMsgFn cur = *slot;
    DWORD old;

    if (!cur || cur == &FC_PeekMessage) return;
    /* 只碰两种槽: 还没被动过的系统函数, 和 cnc-ddraw 的 fake。
     * 别家 ddraw 包装器可能把"当时槽位的值" (= 我们的函数) 当 real 缓存,
     * 挂上去就成了 fc → 它 → fc 递归爆栈。 */
    if ((void*)cur != s_SysPeek && !IsCncDdraw()) return;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &old)) return;
    s_OrigPeek = cur;
    *slot = &FC_PeekMessage;
    VirtualProtect(slot, sizeof(*slot), old, &old);
}

static BOOL WINAPI FC_PeekMessage(LPMSG msg, HWND hwnd, UINT mn, UINT mx, UINT rm)
{
    BOOL r = s_OrigPeek ? s_OrigPeek(msg, hwnd, mn, mx, rm) : FALSE;

    /* 只有"队列空"的那次才参与节拍: 拿到消息必须马上交回去处理。 */
    if (!r) FC_Cap(0);
    return r;
}

/* ---------------- 对外 ---------------- */

void FrameCap_EnsureInit(void)
{
    if (s_Init) return;
    s_Init = 1;
    s_MainTid = GetCurrentThreadId();
    BuildPaths();
    s_Enable = GetPrivateProfileIntA("FrameCap", "Enable", 1, g_IniPath) ? 1 : 0;

    {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) s_Qpf = f.QuadPart;
    }
    {
        HMODULE h = LoadLibraryA("winmm.dll");
        if (h) s_pBeginPeriod = (UINT (WINAPI*)(UINT))(void*)
            GetProcAddress(h, "timeBeginPeriod");
    }
    {
        HMODULE u = GetModuleHandleA("user32.dll");
        if (u) s_SysPeek = (void*)(size_t)GetProcAddress(u, "PeekMessageA");
    }
    HookPeek();
    if (s_Enable) LogLine("framecap: on");
}

int FrameCap_Enabled(void)
{
    if (!s_Init) FrameCap_EnsureInit();
    return s_Enable;
}

void FrameCap_SetEnabled(int on)
{
    s_Enable = on ? 1 : 0;
    s_Until = 0;
    s_FrameEst = 0;      /* 重开时别用旧估计判"引擎在节流" */
    s_EnginePaced = 0;
}

void FrameCap_SetActiveDlg(void* hwnd)
{
    s_Dlg = hwnd;
    s_Until = 0;
    s_ChkAt = 0;
}

void* FrameCap_ActiveDlg(void)
{
    return s_Dlg;
}
