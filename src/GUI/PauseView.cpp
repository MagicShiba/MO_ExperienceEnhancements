/* PauseView.cpp - 单机暂停 + 单步帧进 (F5 + 按过一次后常显的两按钮)。
 * MainLoop 入口 (0x55D360) 挂门: 暂停跑迷你循环后跳 0x55DEDB 出本轮,
 * 步进帧放行一轮原循环。迷你循环: CallBack → 守卫 → Mark(2) → 输入 →
 * Keyboard → Render → Tactical::Update → Sleep(1)。做法见 Phobos FrameByFrame。
 * [BottomBar] PauseEnable=0 整个不启用; =1 启用, 按钮按过一次 F5 后常显。
 * 联机或未开局恒放行。只读 + 渲染面写入。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "ScaleBar.h"
#include "PauseView.h"

#define PV_SECTION "BottomBar"

#define ADDR_SESSION_MODE  0x00A8B238u /* SessionClass 首成员 = GameMode */
#define ADDR_SCENARIO_PTR  0x00A8B230u /* ScenarioClass::Instance (换图看它) */
#define ADDR_SCEN_STARTED  0x00A8ED5Cu /* Unsorted::ScenarioStarted */
#define ADDR_FRAME         0x00A8ED84u /* 当前帧号 (倒退=新开/读档) */
#define ADDR_SIDEBAR_W     0x007F5BF8u /* 侧栏列宽 168 (按钮 fallback 锚点用) */

#define GAME_LAN 3
#define GAME_INTERNET 4

#define VK_F5KEY 0x74

/* 迷你循环出口: MainLoop 尾 ret (Phobos 同值, 见 FrameByFrame.cpp)。 */
#define PV_LOOP_EXIT 0x0055DEDBu

#define ADDR_ISACTIVE  0x00A8E9A0u /* Game::IsActive (bool, YRpp Unsorted.h) */
#define ADDR_ISFOCUSED 0x00A8ED80u /* Game::IsFocused (bool) */
#define ADDR_SPECDLG   0x00A8EDA0u /* Game::SpecialDialog (int) */
#define ADDR_MAP       0x0087F7E8u /* MapClass::Instance (=Display, 同对象) */
#define ADDR_TACTICAL  0x00887324u /* TacticalClass::Instance (指针) */

#define REG_OFF_EAX 0x24u   /* YRpp Syringe.h REGISTERS (见 TypeSelect.cpp 注释) */

/* ---------------- 配置 ---------------- */
static int s_Init = 0;
static int s_BtnEnable = 0;      /* [BottomBar] PauseEnable (默认 0 = 不启用) */
static OU_Hotkey s_Hk;          /* [BottomBar] PauseHotkey (默认 F5) */
static int s_HkOk = 0;

/* ---------------- 状态 ---------------- */
static int s_Paused = 0;
static int s_Step = 0;          /* 待放行帧数 (步进按钮只置 1) */
static int s_AutoShow = 0;      /* 用过一次暂停后本局显示按钮 (默认不绘制) */
static int s_HkPrev = 0;        /* F5 边沿采样 */
static int s_LbPrev = 0;        /* 左键边沿采样 */
static int s_Hov1 = 0, s_Hov2 = 0;
static void* s_LastScen = NULL;
static int s_LastFrame = -1;   /* 上次 Poll 见到的帧号 (倒退=新开/读档) */

/* ---------------- 初始化 ---------------- */

void PV_EnsureInit(void)
{
    char buf[128];
    unsigned p;
    if (s_Init) return;
    s_Init = 1;
    BuildPaths();
    GtEnsureLog();
    s_Hk.vk = VK_F5KEY; s_Hk.mods = 0; s_Hk.nExtra = 0; s_Hk.disabled = 0;
    s_HkOk = 1;
    if (g_Log)
    {
        s_BtnEnable = GetPrivateProfileIntA(PV_SECTION, "PauseEnable", 0, g_IniPath) ? 1 : 0;
        OU_ReadHotkey(PV_SECTION, "PauseHotkey", &s_Hk);
        p = 0;
        p = AppendCStr(buf, p, "pv: btn=", sizeof(buf));
        p += UtoA((unsigned)s_BtnEnable, buf + p);
        p = AppendCStr(buf, p, " hk=", sizeof(buf));
        UtoA((unsigned)s_Hk.vk, buf + p);
        LogLine(buf);
    }
}

/* ---------------- 门控 ---------------- */

static int PV_IsMP(void)
{
    int gm;
    if (!IsPlausiblePtr((void*)ADDR_SESSION_MODE)) return 1;
    gm = *(volatile int*)(size_t)ADDR_SESSION_MODE;
    return (gm == GAME_LAN || gm == GAME_INTERNET) ? 1 : 0;
}

static int PV_CanRun(void)
{
    if (!s_Init) return 0;
    if (!s_BtnEnable) return 0;   /* PauseEnable=0: 功能整个不启用 */
    if (PV_IsMP()) return 0;
    if (!*(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED) return 0;
    return 1;
}

/* ---------------- 动作 ---------------- */

static void PV_Toggle(const char* why)
{
    char buf[96];
    char b1[16];
    unsigned p = 0;
    int frame;
    if (!PV_CanRun()) return;
    s_Paused = s_Paused ? 0 : 1;
    s_Step = 0;
    s_AutoShow = 1;   /* 用过一次就显示按钮 (默认不绘制, 见头注释) */
    frame = *(volatile int*)(size_t)ADDR_FRAME;
    p = AppendCStr(buf, p, s_Paused ? "pv: paused by " : "pv: resumed by ", sizeof(buf));
    p = AppendCStr(buf, p, why ? why : "?", sizeof(buf));
    p = AppendCStr(buf, p, " f=", sizeof(buf));
    UtoA((unsigned)(frame < 0 ? 0 : frame), b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

static void PV_StepOnce(void)
{
    if (!PV_CanRun()) return;
    if (!s_Paused) return;   /* 运行中点步进: 忽略 */
    s_Step = 1;
    LogLine("pv: step 1 frame");
}

/* F5 纯边沿 (松开前只触发一次)。 */
static int PV_HotkeyEdge(void)
{
    int down, edge;
    if (!s_HkOk || s_Hk.disabled) return 0;
    if (!GtGameFocused()) { s_HkPrev = 0; return 0; }
    down = OU_HotkeyDown(&s_Hk) ? 1 : 0;
    edge = (down && !s_HkPrev) ? 1 : 0;
    s_HkPrev = down;
    return edge;
}

/* ---------------- 按钮几何 (刻度条左侧两小方块) ---------------- */

#define PV_BS 22      /* 方块边长 */
#define PV_GAPB 6     /* 两块之间 */
#define PV_GAPBAR 10  /* 与刻度条之间 */

static int PV_Geom(int* x1, int* y1, int* x2, int* y2)
{
    int bx, by, bw, bh;
    int gw = GtGameW(), gh = GtGameH();
    int sbw;
    int right;
    if (gw < 200 || gh < 200) return 0;
    if (SB_BarGeom(&bx, &by, &bw, &bh))
    {
        right = bx - PV_GAPBAR;
        *x2 = right - PV_BS;
        *y2 = by + bh - PV_BS - 2;
        *x1 = *x2 - PV_GAPB - PV_BS;
        *y1 = *y2;
    }
    else
    {
        /* 刻度条没开: 自己锚右下 (口径与 ScaleBar::Sb_Place 同: 侧栏列左缘-32)。 */
        sbw = *(volatile int*)(size_t)ADDR_SIDEBAR_W;
        if (sbw < 64 || sbw > 512) sbw = 168;
        right = gw - sbw - 32;
        *x2 = right - PV_BS;
        *y2 = gh - 4 - PV_BS;
        *x1 = *x2 - PV_GAPB - PV_BS;
        *y1 = *y2;
    }
    if (*x1 < 0 || *y1 < 0 || *x2 + PV_BS > gw || *y2 + PV_BS > gh) return 0;
    return 1;
}

static int PV_InRect(int mx, int my, int x, int y)
{
    return (mx >= x && mx < x + PV_BS && my >= y && my < y + PV_BS) ? 1 : 0;
}

static int PV_ShowButtons(void)
{
    return s_AutoShow ? 1 : 0;   /* 默认不显示; 按过一次 F5 暂停后长久显示 */
}

/* 点击 + 悬停 (左键上升沿才动作, 按下点在别处划进来的不算)。 */
static void PV_CheckButtons(void)
{
    int mx, my, down, edge;
    int x1, y1, x2, y2;
    if (!PV_ShowButtons()) { s_LbPrev = 0; s_Hov1 = s_Hov2 = 0; return; }
    if (!GtMouseGameXY(&mx, &my)) { s_LbPrev = 0; return; }
    if (!PV_Geom(&x1, &y1, &x2, &y2)) { s_LbPrev = 0; return; }
    s_Hov1 = PV_InRect(mx, my, x1, y1);
    s_Hov2 = PV_InRect(mx, my, x2, y2);
    down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    edge = (down && !s_LbPrev) ? 1 : 0;
    s_LbPrev = down;
    if (!edge) return;
    if (s_Hov1) PV_Toggle("btn");
    else if (s_Hov2) PV_StepOnce();
}

void PV_Poll(void)
{
    void* scen;
    int frame;
    PV_EnsureInit();
    /* 不可暂停态 (菜单/载入/联机): 强制复位, 暂停态不带出对局。
     * Scenario 对象会被引擎复用, 指针不变, 故帧号倒退与指针变化都判。 */
    if (!PV_CanRun()) { s_Paused = 0; s_Step = 0; return; }
    frame = *(volatile int*)(size_t)ADDR_FRAME;
    if (s_LastFrame >= 0 && frame < s_LastFrame)
    {
        s_Paused = 0;
        s_Step = 0;
        s_LastFrame = frame;
        LogLine("pv: frame rewound -> resumed");
    }
    s_LastFrame = frame;
    scen = *(void**)(size_t)ADDR_SCENARIO_PTR;
    if (scen != s_LastScen)
    {
        s_LastScen = scen;
        if (s_Paused || s_Step)
        {
            s_Paused = 0;
            s_Step = 0;
            LogLine("pv: map changed -> resumed");
        }
    }
    /* Poll 在正常循环 (prepare 钩) 与迷你循环 (Mark 钩) 里都跑。 */
    if (PV_HotkeyEdge()) PV_Toggle("F5");
    PV_CheckButtons();
}

/* ---------------- 主循环入口门 + 迷你循环 ---------------- */

typedef void (__attribute__((stdcall))* FnCallBack)(void);
typedef int (__attribute__((thiscall))* FnGuiU)(void* self, DWORD* pKey, int* pX, int* pY);
typedef void (__attribute__((fastcall))* FnKbProc)(DWORD* pInput);
typedef void (__attribute__((thiscall))* FnRenderMap)(void* self);
typedef void (__attribute__((thiscall))* FnMarkHook)(void* self, int lvl);
typedef void (__attribute__((thiscall))* FnTacUpd)(void* self);

static void PV_MiniLoop(void)
{
    void* map;
    void* tac;
    void* fnU;
    DWORD key;
    int mx, my;
    if (!*(volatile unsigned char*)(size_t)ADDR_ISACTIVE)
    {
        Sleep(1);
        return;
    }
    map = (void*)(size_t)ADDR_MAP;
    ((FnCallBack)(size_t)0x0048D080u)();
    if (*(volatile unsigned char*)(size_t)ADDR_ISFOCUSED &&
        !*(volatile int*)(size_t)ADDR_SPECDLG)
    {
        /* 走钩子入口: 本 DLL 全部 Poll (F5/按钮) 在暂停中照常跑。 */
        ((FnMarkHook)(size_t)0x004F42F0u)(map, 2);
        key = 0; mx = 0; my = 0;
        ((FnGuiU)(size_t)0x004F4320u)(map, &key, &mx, &my);
        if (key)
        {
            ((FnKbProc)(size_t)0x0055DEE0u)(&key);
            if (key == VK_ESCAPE || key == VK_SPACE)
                *(volatile int*)(size_t)ADDR_SPECDLG = 1;
        }
        ((FnRenderMap)(size_t)0x004F4480u)(map);
        tac = *(void**)(size_t)ADDR_TACTICAL;
        if (tac && IsPlausiblePtr(tac))
        {
            fnU = *(void**)((*(void***)tac) + 0x5C / sizeof(void*));
            if (fnU) ((FnTacUpd)fnU)(tac);
        }
    }
    Sleep(1);
}

extern "C" __declspec(dllexport) DWORD __cdecl PV_MainGate(void* regs)
{
    PV_EnsureInit();
    if (!PV_CanRun()) return 0;
    if (!s_Paused) return 0;
    if (s_Step > 0) { s_Step--; return 0; }   /* 步进帧: 走原循环 */
    PV_MiniLoop();
    if (regs) *(DWORD*)((char*)regs + REG_OFF_EAX) =
        *(volatile unsigned char*)(size_t)ADDR_ISACTIVE ? 0 : 1;
    return PV_LOOP_EXIT;
}

/* ---------------- 绘制 ---------------- */

static void PV_Fill(unsigned char* base, int pitch, int W, int H,
                    int x0, int y0, int w, int h,
                    unsigned char r, unsigned char g, unsigned char b, int a)
{
    int yy, xx;
    unsigned short* row;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > W) w = W - x0;
    if (y0 + h > H) h = H - y0;
    if (w <= 0 || h <= 0) return;
    for (yy = 0; yy < h; yy++)
    {
        row = (unsigned short*)(base + (size_t)(y0 + yy) * pitch);
        for (xx = 0; xx < w; xx++)
            row[x0 + xx] = Blend16(row[x0 + xx], r, g, b, a);
    }
}

/* 实心右三角: 高 hh、最大宽 maxw, 左对齐于 cx, 上对齐于 y0。 */
static void PV_Triangle(unsigned char* base, int pitch, int W, int H,
                        int cx, int y0, int hh, int maxw,
                        unsigned char r, unsigned char g, unsigned char b)
{
    int j, w, d;
    if (hh <= 0 || maxw <= 0) return;
    for (j = 0; j < hh; j++)
    {
        d = 2 * j + 1 - hh;
        if (d < 0) d = -d;
        w = maxw - (maxw * d + hh / 2) / hh;
        if (w < 1) w = 1;
        if (w > maxw) w = maxw;
        PV_Fill(base, pitch, W, H, cx, y0 + j, w, 1, r, g, b, 255);
    }
}

static void PV_PaintBtn(unsigned char* base, int pitch, int W, int H,
                       int x, int y, int kind)
{
    /* kind 0=暂停/恢复键, 1=步进键。暂停中暂停键描黄边。 */
    int paused = s_Paused ? 1 : 0;
    unsigned char er = 140, eg = 146, eb = 156;
    if (kind == 0 && paused) { er = 250; eg = 214; eb = 80; }
    if ((kind == 0 && s_Hov1) || (kind == 1 && s_Hov2))
    {
        er = 232; eg = 236; eb = 242;
    }
    PV_Fill(base, pitch, W, H, x, y, PV_BS, PV_BS, 20, 22, 28, 255);
    PV_Fill(base, pitch, W, H, x, y, PV_BS, 1, er, eg, eb, 255);
    PV_Fill(base, pitch, W, H, x, y + PV_BS - 1, PV_BS, 1, er, eg, eb, 255);
    PV_Fill(base, pitch, W, H, x, y, 1, PV_BS, er, eg, eb, 255);
    PV_Fill(base, pitch, W, H, x + PV_BS - 1, y, 1, PV_BS, er, eg, eb, 255);
    if (kind == 0)
    {
        if (paused)
            PV_Triangle(base, pitch, W, H, x + 7, y + 5, PV_BS - 10, 7, 250, 214, 80);
        else
        {
            PV_Fill(base, pitch, W, H, x + 7, y + 5, 3, PV_BS - 10, 232, 236, 242, 255);
            PV_Fill(base, pitch, W, H, x + 12, y + 5, 3, PV_BS - 10, 232, 236, 242, 255);
        }
    }
    else
    {
        PV_Triangle(base, pitch, W, H, x + 5, y + 5, PV_BS - 10, 7, 232, 236, 242);
        PV_Fill(base, pitch, W, H, x + 13, y + 5, 3, PV_BS - 10, 232, 236, 242, 255);
    }
}

int PV_DrawToComposite(void)
{
    void* surf;
    unsigned char* base;
    int pitch, w, h, bpp;
    int x1, y1, x2, y2;
    PV_EnsureInit();
    if (!PV_ShowButtons()) return 0;
    if (PV_IsMP()) return 0;
    surf = *(void**)ADDR_SURF_COMPOSITE;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    if (SB_IsBigSurface(surf)) return 0;   /* 缩小档三趟期间是大面, 别落进去 */
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    w = GtSurfGet(surf, GtVT_GET_WIDTH);
    h = GtSurfGet(surf, GtVT_GET_HEIGHT);
    bpp = GtSurfGet(surf, GtVT_GET_BPP);
    if (bpp != 2 || pitch < w * 2 || w <= 0 || h <= 0) return 0;
    if (!PV_Geom(&x1, &y1, &x2, &y2)) return 0;
    base = GtLock(surf);
    if (!base) return 0;
    PV_PaintBtn(base, pitch, w, h, x1, y1, 0);
    PV_PaintBtn(base, pitch, w, h, x2, y2, 1);
    GtUnlock(surf);
    return 1;
}
