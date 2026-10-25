/* PauseView.cpp - 单机暂停 + 单步帧进 (热键 + 按钮)。
 * MainLoop 入口 (0x55D360) 挂门: 暂停跑迷你循环后跳 0x55DEDB 出本轮,
 * 步进帧放行一轮原循环。迷你循环: CallBack → 守卫 → Mark(2) → 输入 →
 * Keyboard → Render → Tactical::Update → MsgWait(15)。做法见 Phobos FrameByFrame。
 * 热键单机恒有效; [BottomBar] PauseEnable 只定按钮初始显示
 * (0=用过一次才显示, 1=常显)。
 * 格网按钮 (暂停键左侧第三块, # 图标): 与暂停按钮同显同隐 (用过一次 F5 才出现),
 * 默认关闭, 点按才画灰色格网。使用率低故不配热键 (G 是原版警戒键, 会冲突)。
 * 截图键 (Shot 模块) 显示时紧贴刻度条, 本模块三键整体左移一格让位。
 * 格网实现见 GridView.cpp (本文件只调 GV_ 接口)。
 * 绘制只落在视图区 A 内 (侧栏/底栏不画)。只读 + 渲染面写入,
 * 联机或未开局恒放行。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "ScaleBar.h"
#include "FullMap.h"
#include "PauseView.h"
#include "GridView.h"
#include "Shot.h"
#include "FrameCap.h"
#include "GameSpeed.h"

#define PV_SECTION "BottomBar"

#define ADDR_SESSION_MODE  0x00A8B238u /* SessionClass 首成员 = GameMode */
#define ADDR_SCENARIO_PTR  0x00A8B230u /* ScenarioClass::Instance (换图看它) */

#define GAME_LAN 3
#define GAME_INTERNET 4

#define VK_F5KEY 0x74

/* 迷你循环出口: MainLoop 尾 ret (Phobos 同值, 见 FrameByFrame.cpp)。 */
#define PV_LOOP_EXIT 0x0055DEDBu

#define ADDR_ISFOCUSED 0x00A8ED80u /* Game::IsFocused (bool) */
#define ADDR_SPECDLG   0x00A8EDA0u /* Game::SpecialDialog (int) */
#define ADDR_MAP       0x0087F7E8u /* MapClass::Instance (=Display, 同对象) */


/* ---------------- 配置 ---------------- */
static int s_Init = 0;
static int s_BtnShowInit = 0;    /* [BottomBar] PauseEnable: 按钮初始显示, 默认隐藏 */
static OU_Hotkey s_Hk;          /* [BottomBar] PauseHotkey (默认 F5) */
static int s_HkOk = 0;

/* ---------------- 状态 ---------------- */
static int s_Paused = 0;
static int s_Step = 0;          /* 待放行帧数 (步进按钮只置 1) */
static int s_AutoShow = 0;      /* 用过一次暂停后本局显示按钮 (默认不绘制) */
static int s_HkPrev = 0;        /* F5 边沿采样 */
static int s_LbPrev = 0;        /* 左键边沿采样 */
static int s_HovG = 0, s_Hov1 = 0, s_Hov2 = 0;  /* 悬停: 格网/暂停/步进 */
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
        s_BtnShowInit = GetPrivateProfileIntA(PV_SECTION, "PauseEnable", 0, g_IniPath) ? 1 : 0;
        OU_ReadHotkey(PV_SECTION, "PauseHotkey", &s_Hk);
        GV_EnsureInit();
        p = 0;
        p = AppendCStr(buf, p, "pv: btn=", sizeof(buf));
        p += UtoA((unsigned)s_BtnShowInit, buf + p);
        p = AppendCStr(buf, p, " hk=", sizeof(buf));
        p += UtoA((unsigned)s_Hk.vk, buf + p);
        p = AppendCStr(buf, p, " cull=", sizeof(buf));
        UtoA((unsigned)GV_CullBack(), buf + p);
        p = AppendCStr(buf, p, " blk=", sizeof(buf));
        UtoA((unsigned)GV_ShowBlocked(), buf + p);
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

/* ---------------- 按钮几何 (刻度条左侧: 截图/步进/暂停/格网) ---------------- */

#define PV_GAPB 6     /* 两块之间 */
#define PV_GAPBAR 10  /* 与刻度条之间 */

/* 截图键占位 = 方块 + 间隔; 它不显示时不占位。 */
static int PV_ShotReserve(void)
{
    int w = Shot_ButtonWidth();
    return w ? w + PV_GAPB : 0;
}

/* 底栏按钮槽: 0=格网 1=暂停 2=步进 3=截图。
 * 截图键 (Shot 模块) 恒占最右一槽 = 紧贴刻度条; 它显示时本模块三键整体左移一格。 */
int PV_SlotGeom(int slot, int* x, int* y)
{
    int bx, by, bw, bh;
    int gw = GtGameW(), gh = GtGameH();
    int sbw, right, gy, px;
    if (gw < 200 || gh < 200) return 0;
    if (SB_BarGeom(&bx, &by, &bw, &bh))
    {
        right = bx - PV_GAPBAR;
        gy = by + bh - PV_BS - 2;
    }
    else
    {
        /* 刻度条没开: 自己锚右下 (口径与 ScaleBar::Sb_Place 同: 侧栏列左缘-32)。 */
        sbw = *(volatile int*)(size_t)ADDR_SIDEBAR_W;
        if (sbw < 64 || sbw > 512) sbw = 168;
        right = gw - sbw - 32;
        gy = gh - 4 - PV_BS;
    }
    if (slot >= 3)
    {
        if (!PV_ShotReserve()) return 0;   /* 截图键没显示 ⇒ 该槽不存在 */
        *x = right - PV_BS;
    }
    else
    {
        px = right - PV_ShotReserve();
        *x = px - PV_BS - (2 - slot) * (PV_BS + PV_GAPB);
    }
    *y = gy;
    if (*x < 0 || gy < 0 || *x + PV_BS > gw || gy + PV_BS > gh) return 0;
    return 1;
}

/* x0/y0=格网键 (最左), x1/y1=暂停键, x2/y2=步进键。 */
static int PV_Geom(int* x0, int* y0, int* x1, int* y1, int* x2, int* y2)
{
    if (!PV_SlotGeom(0, x0, y0)) return 0;
    if (!PV_SlotGeom(1, x1, y1)) return 0;
    if (!PV_SlotGeom(2, x2, y2)) return 0;
    return 1;
}

static int PV_InRect(int mx, int my, int x, int y)
{
    return (mx >= x && mx < x + PV_BS && my >= y && my < y + PV_BS) ? 1 : 0;
}

static int PV_ShowButtons(void)
{
    return (s_BtnShowInit || s_AutoShow) ? 1 : 0;
}

int PV_ButtonsEnabled(void)
{
    PV_EnsureInit();
    return PV_ShowButtons();
}

void PV_SetButtonsEnabled(int enable)
{
    PV_EnsureInit();
    s_BtnShowInit = enable ? 1 : 0;
    if (!s_BtnShowInit)
    {
        s_AutoShow = 0;
        s_Paused = 0;
        s_Step = 0;
    }
    s_LbPrev = 0;
    s_HovG = s_Hov1 = s_Hov2 = 0;
}

/* 格网缓存/绘制已移入 GridView.cpp, 本文件只调 GV_ 接口 (见 GridView.h)。 */

/* 点击 + 悬停 (左键上升沿才动作, 按下点在别处划进来的不算)。 */
static void PV_CheckButtons(void)
{
    int mx, my, down, edge;
    int x0, y0, x1, y1, x2, y2;
    if (!PV_ShowButtons()) { s_LbPrev = 0; s_HovG = s_Hov1 = s_Hov2 = 0; return; }
    if (!GtMouseGameXY(&mx, &my)) { s_LbPrev = 0; return; }
    if (!PV_Geom(&x0, &y0, &x1, &y1, &x2, &y2)) { s_LbPrev = 0; return; }
    s_HovG = PV_InRect(mx, my, x0, y0);
    s_Hov1 = PV_InRect(mx, my, x1, y1);
    s_Hov2 = PV_InRect(mx, my, x2, y2);
    down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    edge = (down && !s_LbPrev) ? 1 : 0;
    s_LbPrev = down;
    if (!edge) return;
    if (s_HovG)
    {
        LogLine(GV_Toggle() ? "pv: grid ON" : "pv: grid OFF");
    }
    else if (s_Hov1) PV_Toggle("btn");
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
        GV_Drop();   /* 新开/读档: 格网缓存作废 */
        LogLine("pv: frame rewound -> resumed");
    }
    s_LastFrame = frame;
    scen = *(void**)(size_t)ADDR_SCENARIO_PTR;
    if (scen != s_LastScen)
    {
        s_LastScen = scen;
        GV_Drop();   /* 换图 key 里也有场景, 这里先行作废 */
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
    GV_PollStable();   /* 格网: 移动帧隐藏, 停稳下一帧重建 */
}

/* ---------------- 主循环入口门 + 迷你循环 ---------------- */

typedef void (__attribute__((stdcall))* FnCallBack)(void);
typedef int (__attribute__((thiscall))* FnGuiU)(void* self, DWORD* pKey, int* pX, int* pY);
typedef void (__attribute__((fastcall))* FnKbProc)(DWORD* pInput);
typedef void (__attribute__((thiscall))* FnRenderMap)(void* self);
typedef void (__attribute__((thiscall))* FnMarkHook)(void* self, int lvl);
typedef void (__attribute__((thiscall))* FnTacUpd)(void* self);

/* 迷你循环的一拍: 15ms, 但一有输入就提前返回。
 * 不能用 Sleep(1): 定时器精度被 FrameCap / GameSpeed(>60) 提到 1ms 后,
 * Sleep(1) 就变成 1000Hz 空转。 */
static void PV_IdleWait(void)
{
    MsgWaitForMultipleObjects(0, NULL, FALSE, 15, QS_ALLINPUT);
}

static void PV_MiniLoop(void)
{
    void* map;
    void* tac;
    void* fnU;
    DWORD key;
    int mx, my;
    if (!*(volatile unsigned char*)(size_t)ADDR_ISACTIVE)
    {
        PV_IdleWait();
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
    PV_IdleWait();
}

/* 主循环门的实体 (Perf 探针把整段和 FrameCap/GameSpeed 分开计时)。 */
static DWORD PV_MainGateBody(void* regs)
{
    long long tSeg;
    if (!PV_CanRun()) return 0;

    /* 暂停中不推进游戏帧: 这时候不能限速(没帧可限)。只同步一下原版速度页,
     * 玩家暂停时改原版滑块也能生效。 */
    if (s_Paused && s_Step <= 0)
    {
        GameSpeed_Poll();
        PV_MiniLoop();
        if (regs) *(DWORD*)((char*)regs + REG_OFF_EAX) =
            *(volatile unsigned char*)(size_t)ADDR_ISACTIVE ? 0 : 1;
        return PV_LOOP_EXIT;
    }

    tSeg = Perf_Now();
    GameSpeed_Frame(); /* >60 的自建毫秒节拍 + 原版速度页同步 (见 GameSpeed.cpp) */
    Perf_Add(PF_GS, tSeg);
    if (!s_Paused) return 0;
    if (s_Step > 0) { s_Step--; return 0; }   /* 步进帧: 走原循环 */
    PV_MiniLoop();
    if (regs) *(DWORD*)((char*)regs + REG_OFF_EAX) =
        *(volatile unsigned char*)(size_t)ADDR_ISACTIVE ? 0 : 1;
    return PV_LOOP_EXIT;
}

extern "C" __declspec(dllexport) DWORD __cdecl PV_MainGate(void* regs)
{
    long long tPf, tSeg;
    DWORD rc;

    PV_EnsureInit();
    tPf = Perf_Now();
    tSeg = tPf;
    FrameCap_Gate();   /* 限帧节拍: 主循环还活着时由这里计时 (见 FrameCap.cpp) */
    Perf_Add(PF_FC, tSeg);
    rc = PV_MainGateBody(regs);
    Perf_Add(PF_GATE, tPf);
    return rc;
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
    /* kind 0=暂停/恢复键, 1=步进键, 2=格网键(#)。暂停中暂停键描黄边,
     * 格网开时格网键描黄边。 */
    int paused = s_Paused ? 1 : 0;
    unsigned char er = 140, eg = 146, eb = 156;
    if (kind == 0 && paused) { er = 250; eg = 214; eb = 80; }
    if (kind == 2 && GV_IsOn()) { er = 250; eg = 214; eb = 80; }
    if ((kind == 0 && s_Hov1) || (kind == 1 && s_Hov2) ||
        (kind == 2 && s_HovG))
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
    else if (kind == 1)
    {
        PV_Triangle(base, pitch, W, H, x + 5, y + 5, PV_BS - 10, 7, 232, 236, 242);
        PV_Fill(base, pitch, W, H, x + 13, y + 5, 3, PV_BS - 10, 232, 236, 242, 255);
    }
    else
    {
        /* # 形: 2 竖 + 2 横, 灰色。 */
        PV_Fill(base, pitch, W, H, x + 7, y + 5, 2, PV_BS - 10, 170, 170, 170, 255);
        PV_Fill(base, pitch, W, H, x + 13, y + 5, 2, PV_BS - 10, 170, 170, 170, 255);
        PV_Fill(base, pitch, W, H, x + 5, y + 7, PV_BS - 10, 2, 170, 170, 170, 255);
        PV_Fill(base, pitch, W, H, x + 5, y + 13, PV_BS - 10, 2, 170, 170, 170, 255);
    }
}

/* 格网绘制见 GridView.cpp (GV_DrawTo, 压底画)。 */

int PV_DrawToComposite(void)
{
    void* surf;
    unsigned char* base;
    int pitch, w, h, bpp;
    int x0, y0, x1, y1, x2, y2;
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
    if (!PV_Geom(&x0, &y0, &x1, &y1, &x2, &y2)) return 0;
    base = GtLock(surf);
    if (!base) return 0;
    /* 格网压底。关着时跳过 (免每帧算裁剪区); 裁剪区 = 满视图区 A
     * (侧栏列/底栏带子都在 A 外, 不落笔)。 */
    if (GV_IsOn())
    {
        RectangleStruct a;
        int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0, clip = 0;
        FM_GetViewRects(&a, 0);
        if (a.Width >= 32 && a.Height >= 32 && a.X >= 0 && a.Y >= 0 &&
            a.X < w && a.Y < h)
        {
            cx0 = a.X; cy0 = a.Y;
            cx1 = a.X + a.Width; cy1 = a.Y + a.Height;
            if (cx1 > w) cx1 = w;
            if (cy1 > h) cy1 = h;
            if (cx1 > cx0 && cy1 > cy0) clip = 1;
        }
        GV_DrawTo(base, pitch, w, h, cx0, cy0, cx1, cy1, clip);
    }
    PV_PaintBtn(base, pitch, w, h, x0, y0, 2);
    PV_PaintBtn(base, pitch, w, h, x1, y1, 0);
    PV_PaintBtn(base, pitch, w, h, x2, y2, 1);
    GtUnlock(surf);
    return 1;
}
