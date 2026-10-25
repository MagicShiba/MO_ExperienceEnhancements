/* GameSpeed.cpp - 自定义游戏速度 (5~120 FPS, 121 = 原版不限速)
 *
 * 引擎的 GameSpeed (0xA8EB60) 是 0~6 的档位, 主循环 0x55D79E 把它当"本帧等几个
 * tick (1/60 秒)"写进帧定时器 0x887350。引擎是帧锁定的, 所以帧率就是游戏速度。
 *
 * 两个钩子:
 *   0x55D79E (mov esi,[0xA8EB60]) 换成自己算的 tick 数, 跳过原指令。
 *     5..60: Bresenham 把 60/FPS 摊到相邻 tick (35 -> 1,2,2,1,2,2,2)。
 *     >60  : 一个 tick 装不下, 让引擎 TimeLeft=0 全速, 再由 GameSpeed_Frame
 *            用 QPC 绝对时刻限速 (需要 timeBeginPeriod(1))。
 *     121  : 完全交还引擎。
 *   0x55E3B5 (mov [0xABCD44], eax) 修正引擎帧率计数器的 960ms 口径, 顺带让
 *   Ares 的 OSD 与设定值一致。
 *
 * 与原版速度页共存: 只读 0xA8EB60, 变了就把档位换算成 FPS 抄进 s_Fps, 所以
 * "最后拖哪个以哪个为准"。绝不回写它 —— 那是动画除数表 0x832CEC 的行索引。
 * 联机 (GameMode 3/4) 一律不接管。
 *
 * 配置: MOExperience.ini [GameSpeed] Enable / FPS / FixEngineFps
 */
#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "GameSpeed.h"

#define ADDR_GAMESPEED   0x00A8EB60u  /* GameOptionsClass::Instance.GameSpeed */

/* YRpp Syringe.h REGISTERS 里的寄存器偏移 */

#define GS_INI_SECTION   "GameSpeed"

static int      s_Init = 0;
static int      s_Enable = 1;
static int      s_Fps = 60;      /* 5..120, 或 ESC_SPEED_TOP */
static int      s_LastGS = -1;   /* 上次看到的 GameSpeed, -1 = 还没建立基线 */
static unsigned s_Acc = 0;       /* Bresenham 累加器 (单位: tick*FPS) */

static long long s_Qpf = 0;      /* QPC 频率, 0 = 不可用 */
static long long s_Next = 0;     /* 下一帧的 QPC 时刻, 0 = 未定 */
static long long s_SettleT = 0;  /* 上一次引擎结算的 QPC 时刻 */
static int       s_FixEngFps = 0;
static int       s_TimerHi = 0;

static UINT (WINAPI *s_pBeginPeriod)(UINT) = NULL;

/* ---------------- 初始化 ---------------- */

void GameSpeed_EnsureInit(void)
{
    LARGE_INTEGER f;
    HMODULE h;
    int v;

    if (s_Init) return;
    s_Init = 1;

    if (!g_IniPath[0]) BuildPaths();
    s_Enable = GetPrivateProfileIntA(GS_INI_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    s_FixEngFps = GetPrivateProfileIntA(GS_INI_SECTION, "FixEngineFps", 1, g_IniPath) ? 1 : 0;

    v = GetPrivateProfileIntA(GS_INI_SECTION, "FPS", 60, g_IniPath);
    s_Fps = (v < ESC_SPEED_MIN) ? ESC_SPEED_MIN
          : (v > ESC_SPEED_TOP) ? ESC_SPEED_TOP : v;

    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) s_Qpf = f.QuadPart;

    h = LoadLibraryA("winmm.dll");
    if (h) s_pBeginPeriod = (UINT (WINAPI*)(UINT))(void*)
        GetProcAddress(h, "timeBeginPeriod");

    if (s_Enable) LogLine("gamespeed: on");
}

int GameSpeed_Enabled(void)
{
    GameSpeed_EnsureInit();
    return s_Enable;
}

void GameSpeed_SetEnabled(int on)
{
    GameSpeed_EnsureInit();
    s_Enable = on ? 1 : 0;
    s_Acc = 0;
    s_Next = 0;
}

int GameSpeed_GetFps(void)
{
    GameSpeed_EnsureInit();
    return s_Fps;
}

void GameSpeed_SetFps(int fps)
{
    GameSpeed_EnsureInit();
    if (fps < ESC_SPEED_MIN) fps = ESC_SPEED_MIN;
    if (fps > ESC_SPEED_TOP) fps = ESC_SPEED_TOP;
    if (fps == s_Fps) return;
    s_Fps = fps;
    s_Acc = 0;      /* 换档重排抖动相位 */
    s_Next = 0;
}

/* ---------------- 原版速度页同步 (只读 0xA8EB60) ---------------- */

/* 引擎档位 -> FPS: 0 = 不限速, 1..6 = 60/30/20/15/12/10 */
static int GS_FpsFromMode(int gs)
{
    static const unsigned char kFps[7] = { 0, 60, 30, 20, 15, 12, 10 };

    if (gs < 0 || gs > 6) return s_Fps;
    if (gs == 0) return ESC_SPEED_TOP;
    return kFps[gs];
}

/* 开局时引擎会从 spawn.ini 写一次 GameSpeed, 那不是玩家操作, 所以第一次只建立
 * 基线, 否则 ini 里的自定义值一进游戏就被冲掉。 */
static void GS_SyncFromMode(void)
{
    int gs = *(volatile int*)(size_t)ADDR_GAMESPEED;

    if (gs < 0 || gs > 6) return;
    if (s_LastGS < 0) { s_LastGS = gs; return; }
    if (gs == s_LastGS) return;
    s_LastGS = gs;
    s_Fps = GS_FpsFromMode(gs);
    s_Acc = 0;
    s_Next = 0;
}

/* ---------------- 系统定时器精度 ---------------- */

/* 单机路径引擎不调 timeBeginPeriod (0x5D5870 只有联机分支用), 系统粒度 15.6ms,
 * >60 档的帧根本睡不出来。只开不关, 免得切档时反复试探精度。 */
static void GS_TimerHi(void)
{
    if (s_TimerHi) return;
    s_TimerHi = 1;
    if (s_pBeginPeriod) s_pBeginPeriod(1u);
}

/* ---------------- tick 计算 (0x55D79E 钩子) ---------------- */

/* 返回本帧该等的 tick 数; -1 = 不接管 (跑原指令)。 */
int GameSpeed_Ticks(void)
{
    int f, gm, t;

    GameSpeed_EnsureInit();
    if (!s_Enable) return -1;

    gm = *(volatile int*)(size_t)ADDR_GAMEMODE;
    if (gm == 3 || gm == 4) return -1;          /* 联机不碰 */

    f = s_Fps;
    if (f > 60) return 0;                       /* >60 与 MAX: 引擎不限速 */

    /* 每帧应得 60/FPS 个 tick, 整数除法的余数摊到后续帧: 60/35 -> 1,2,2,1,2,2,2 */
    s_Acc += 60u;
    t = (int)(s_Acc / (unsigned)f);
    s_Acc -= (unsigned)t * (unsigned)f;
    return t;
}

/* ---------------- >60 的毫秒节拍 ---------------- */

/* 目标时刻按 period 累加, 偶发的 Sleep 过冲由后续帧少等补回来, 平均帧率才等于
 * 设定值; 偏离超过 200ms (读盘/切窗口/系统睡眠) 才重新对时, 免得不眠不休追帧。 */
static void GS_Wait(void)
{
    LARGE_INTEGER now;
    long long period, left;

    if (!s_Qpf) return;
    period = s_Qpf / (long long)s_Fps;
    if (period <= 0) return;

    QueryPerformanceCounter(&now);
    if (!s_Next || now.QuadPart - s_Next > s_Qpf / 5 ||
        s_Next - now.QuadPart > s_Qpf / 5)
        s_Next = now.QuadPart;

    for (;;)
    {
        QueryPerformanceCounter(&now);
        left = (s_Next - now.QuadPart) * 1000000 / s_Qpf;   /* 剩余微秒 */
        if (left <= 0) break;
        /* 剩不到 2ms 就自旋: Sleep 会过冲, 8ms 的帧过冲 2ms 就掉到 100fps 了。
         * 也不能让毫秒数算成 0 还调 Sleep(0) —— 那是让出时间片, 可能等满 15.6ms。 */
        if (left > 2000) Sleep((DWORD)((left - 1000) / 1000));
    }

    s_Next += period;
}

/* 每帧一次 (PV_MainGate 里调): 同步原版滑块 + >60 的限速。 */
void GameSpeed_Frame(void)
{
    int gm;

    GameSpeed_EnsureInit();
    GS_SyncFromMode();

    gm = *(volatile int*)(size_t)ADDR_GAMEMODE;
    if (!s_Enable || gm == 3 || gm == 4 || s_Fps <= 60 || s_Fps > 120)
    {
        s_Next = 0;
        return;
    }

    GS_TimerHi();
    GS_Wait();
}

void GameSpeed_Poll(void)
{
    GameSpeed_EnsureInit();
    GS_SyncFromMode();
}

/* ---------------- 引擎帧率计数器: 960ms 口径修正 ----------------
 * 结算 0x55E372 的条件是 (0x6C8C40() - 快照) >= 60, 而 0x6C8C40 = timeGetTime()>>4
 * (单位 16ms), 所以引擎的"一秒"是 960ms: 显示值 = 墙钟帧率 * 0.96 (设 100 显示 96)。
 * 原版 60 档每帧 1 tick, 960ms 里正好 60 帧, 看不出偏差; 我们按墙钟设定就暴露了。
 *
 * Ares 的 OSD 只是打印 0xABCD44, 自己不统计, 所以改这里就等于"和 Ares 对齐"。
 * 0xABCD44 还是低帧率降效判定的输入 (0x55AF60), 不能每帧临时改再改回, 所以在
 * 写入点用 QPC 实测窗口墙钟换算后写回 EAX, 返回 0 让引擎自己执行那条 mov。
 * TotalFrames (0xABCD48) 在挂点之前已累加, Ares 的 Avg 仍是引擎口径, 不改。 */
extern "C" __declspec(dllexport) DWORD __cdecl GameSpeed_EngSettleHook(void* regs)
{
    LARGE_INTEGER now;
    long long dt;
    unsigned nf;
    int fps;

    if (!regs || !s_Qpf || !s_Enable || !s_FixEngFps) return 0;

    nf = *(DWORD*)((char*)regs + REG_OFF_EAX);   /* 引擎刚数完的本窗口帧数 */

    QueryPerformanceCounter(&now);
    if (!s_SettleT) { s_SettleT = now.QuadPart; return 0; }
    dt = now.QuadPart - s_SettleT;
    s_SettleT = now.QuadPart;

    /* 正常窗口 960ms; 超出说明中途暂停/读盘/失焦, 这一轮不接管 */
    if (dt < s_Qpf * 85 / 100 || dt > s_Qpf * 3 / 2) return 0;

    fps = (int)((double)nf * (double)s_Qpf / (double)dt + 0.5);
    if (fps < 1) fps = 1;

    *(DWORD*)((char*)regs + REG_OFF_EAX) = (DWORD)fps;
    return 0;       /* 重放原指令: 引擎自己 mov [0xABCD44], eax */
}

/* ---------------- 钩子 ---------------- */

/* Syringe 钩子返回 0 = 重放被覆盖的原指令; 非 0 = 绝对地址跳转, 不是相对偏移。
 * 跳过原指令就返回 0x55D79E + 6 = 0x55D7A4 (那条 lea ecx,[esp+0x14])。 */
#define GS_RESUME 0x0055D7A4u

extern "C" __declspec(dllexport) DWORD __cdecl GameSpeed_TickHook(void* regs)
{
    int t = GameSpeed_Ticks();

    if (t < 0 || !regs) return 0;               /* 跑原指令, 保留引擎档位 */
    *(DWORD*)((char*)regs + REG_OFF_ESI) = (DWORD)t;
    return (DWORD)GS_RESUME;
}
