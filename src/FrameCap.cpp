/* FrameCap.cpp - ESC 菜单/设置页限帧 (固定 60)。
 *
 * 引擎主循环 (0x55D360) 和模态对话框循环都没有帧率节流, 窗口化下 cnc-ddraw 的
 * Blt 也不阻塞游戏线程; 菜单打开后逻辑暂停、每帧极轻, 循环能跑到上千 Hz 把单核
 * 打满。这里在每帧必经过的三处挂同一个等待预算, 到点才放行, 未到点 Sleep:
 *   - MainLoop 入口 0x55D360            (PauseView 调)
 *   - 渲染尾 0x4F4589                   (VecText 调)
 *   - PeekMessageA 导入表钩 (0x7E1484)  (模态循环里唯一还在跑的东西)
 *
 * 只在 ESC 菜单/设置页打开时限速, 关掉立刻完全放行 —— 对局与联机不受影响。
 * 配置: [FrameCap] Enable=1
 */
#include <windows.h>
#include "GdiText.h"
#include "FrameCap.h"

#define ADDR_FRAME_NO    0x00A8ED84u /* 主循环帧号 (每帧 inc) */
#define ADDR_LOAD_PHASE  0x00A9FAB0u /* 进度屏相位 (1/2/3 = 载入中) */
#define ADDR_IAT_PEEK    0x007E1484u /* 导入表里 PeekMessageA 的 thunk */

#define FC_PERIOD 17u                /* 1000/60 */

static int s_Init = 0;
static int s_Enable = 1;
static unsigned s_Next = 0;          /* 下一帧的 GetTickCount 时刻, 0=未定 */
static DWORD s_MainTid = 0;

static void* s_Dlg = NULL;           /* 当前 ESC 对话框 */

/* ---------------- 何时限速 ---------------- */

static int MenuOpen(void)
{
    if (!s_Dlg) return 0;
    if (!IsWindow((HWND)s_Dlg)) { s_Dlg = NULL; return 0; }
    return IsWindowVisible((HWND)s_Dlg) ? 1 : 0;
}

/* 子页面打开时首页会被隐藏, 注册的对话框未必可见, 此时靠帧号判定: 一旦进入模态
 * 循环主循环就停了, 帧号不再推进。载入进度屏同样冻结帧号, 用相位排掉。 */
static unsigned s_FrzNo = 0;
static unsigned s_FrzT = 0;

static int ModalSpin(unsigned now)
{
    unsigned f = *(volatile unsigned*)(size_t)ADDR_FRAME_NO;
    unsigned ph = *(volatile unsigned*)(size_t)ADDR_LOAD_PHASE;

    if (ph >= 1 && ph <= 3) { s_FrzNo = f; s_FrzT = now; return 0; }
    if (f != s_FrzNo) { s_FrzNo = f; s_FrzT = now; return 0; }
    if (!s_FrzT) { s_FrzT = now; return 0; }
    return (int)(now - s_FrzT) >= 300;
}

/* 空转时每秒调用上百万次, 结果缓存 20ms */
static unsigned s_ChkT = 0;
static int s_ChkVal = 0;

static int Capping(void)
{
    unsigned now = GetTickCount();

    if (s_ChkT && (unsigned)(now - s_ChkT) < 20) return s_ChkVal;
    s_ChkT = now ? now : 1;
    s_ChkVal = MenuOpen() || ModalSpin(now);
    return s_ChkVal;
}

/* ---------------- PeekMessageA 导入表钩 ----------------
 * 只改 IAT 指针不动代码字节。cnc-ddraw 建 DirectDraw 时会把这一槽换成它的
 * fake_PeekMessageA (real 指向系统函数), 钩子会被整个绕过, 所以每帧比一次,
 * 不是我们就挂回去, 链路是 fc → cnc-ddraw fake → 系统函数。
 * 别家 ddraw 不碰这一槽, 且重挂可能互相递归, 所以只认 cnc-ddraw:
 * 7.x 导出了 DDGetProcAddress / DDIsWindowed / DDEnableZoom。 */
typedef BOOL (WINAPI *PeekMsgFn)(LPMSG, HWND, UINT, UINT, UINT);

static BOOL WINAPI FC_PeekMessage(LPMSG, HWND, UINT, UINT, UINT);

static PeekMsgFn s_OrigPeek = NULL;
static int s_Cnc = -1;               /* -1=未判定 0=其它 1=cnc-ddraw */

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
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE, &old)) return;
    s_OrigPeek = cur;
    *slot = &FC_PeekMessage;
    VirtualProtect(slot, sizeof(*slot), old, &old);
}

static BOOL WINAPI FC_PeekMessage(LPMSG msg, HWND hwnd, UINT mn, UINT mx, UINT rm)
{
    BOOL r = s_OrigPeek ? s_OrigPeek(msg, hwnd, mn, mx, rm) : FALSE;

    FrameCap_Frame();
    return r;
}

/* ---------------- 限速 ---------------- */

void FrameCap_Frame(void)
{
    unsigned now;
    int wait;

    if (!s_Init) FrameCap_EnsureInit();
    if (s_MainTid && GetCurrentThreadId() != s_MainTid) return;
    if (IsCncDdraw()) HookPeek();    /* 被抢走就挂回来 (只是指针比较) */
    if (!s_Enable) return;

    /* 菜单没开: 完全不干预 */
    if (!Capping()) { s_Next = 0; return; }

    now = GetTickCount();
    if (!s_Next || (int)(now - s_Next) > 200) s_Next = now;  /* 首帧/长时间停顿后重排 */

    wait = (int)(s_Next - now);
    if (wait > 0)
    {
        /* 没到帧时刻必须 Sleep 让出 CPU —— 直接 return 会让模态循环全速空转。 */
        Sleep((DWORD)(wait > 2 ? wait - 1 : 1));
        return;
    }

    s_Next += FC_PERIOD;
    now = GetTickCount();
    if ((int)(now - s_Next) > (int)FC_PERIOD) s_Next = now;  /* 落后超过一帧就不再累积 */
}

/* ---------------- 对外 ---------------- */

void FrameCap_EnsureInit(void)
{
    if (s_Init) return;
    s_Init = 1;
    s_MainTid = GetCurrentThreadId();
    BuildPaths();
    s_Enable = GetPrivateProfileIntA("FrameCap", "Enable", 1, g_IniPath) ? 1 : 0;
    HookPeek();
}

int FrameCap_Enabled(void)
{
    if (!s_Init) FrameCap_EnsureInit();
    return s_Enable;
}

void FrameCap_SetEnabled(int on)
{
    s_Enable = on ? 1 : 0;
    s_Next = 0;
}

void FrameCap_SetActiveDlg(void* hwnd)
{
    s_Dlg = hwnd;
    s_Next = 0;
}

void* FrameCap_ActiveDlg(void)
{
    return s_Dlg;
}
