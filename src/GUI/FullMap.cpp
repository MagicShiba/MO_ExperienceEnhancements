/*
 * FullMap.cpp - 隐藏原版建造/生产侧栏 (页签+cameos+电力条), 地图铺满窗口宽度,
 * 小地图 (RadarClass) 仍然留在原来那一角 (NOD=右上 / GDI=左上)。
 *
 * ---------------------------------------------------------------------------
 * 引擎结构 (gamemd.exe 反汇编实证, 全是视口/渲染面, 不写任何模拟内存)
 * ---------------------------------------------------------------------------
 * MouseClass/TabClass/SidebarClass/PowerClass/RadarClass/DisplayClass 是同一个
 * 单例对象 0x87F7E8 (vtable 0x7E1964)。相关全局:
 *
 *   DSurface::WindowBounds  0x886FB0  = 窗口客户区矩形 {X,Y,W,H}
 *   DSurface::SidebarBounds 0x886F90  = 侧栏列矩形 (鼠标"在不在侧栏上"判定用)
 *   DSurface::ViewBounds    0x886FA0  = 地图视口 (渲染/摄像机/点击换算全读它)
 *   0x7F5BF8 = 168 侧栏列宽,  0x7F5BFC = 32 上下栏高
 *   0xA8EB7C isNODSidebar: 1=侧栏在右(苏联), 0=侧栏在左(盟军)
 *   0xB04A00/04/08/0C 侧栏顶部一个 72x18 小控件, **不是**小地图, 别用
 *   RadarClass+0x120C = 0x8809F4 才是小地图在侧栏面内的矩形
 *   0xB0B518/19 侧栏脏标记, 0x884B90 = 侧栏 NeedsRedraw (this+0x53A8)
 *   0x884B8E/8F = this+0x53A6/53A7 (面板面重绘门控)
 *   0xB73550 游戏窗口 HWND, 0x887640 WWMouseClass::Instance
 *
 * 每帧顺序 (TabClass::Draw 0x6D0A20): 侧栏背景 -> 0x4A2370 小地图 ->
 * 0x6A6C30 SidebarClass::Draw (面板) -> 0x6A70E0 BlitSidebar 整条 blit 到
 * DSurface::Primary (0x887308)。面板和小地图在同一块离屏面
 * DSurface::Sidebar (0x887300) 上, 所以"不画面板"并不会顺带保留小地图,
 * 必须自己接管那一次 blit。
 *
 * 视口宽度: 0x72AD20 这个纯函数算出 {0,0,客户宽-168,客户高-32}, 而 Composite
 * 的建面矩形与 Set_View_Dimensions 的入参**都是它的输出**, 所以改这一个函数,
 * 面和视口自动一起变全宽 (见下面 FM_Install)。顺带实证: 1920x1080 下 Composite
 * 本来只有 1752x1080 (= 1920-168), 右侧那 168px 地图从来没画过。
 *
 * 联机安全: 只写视口矩形/渲染面/gadget 标志, 全是本地渲染与输入状态, 每客户端
 * 按自己窗口尺寸算, 不碰任何模拟内存, 不发事件。
 */

#include <windows.h>
#include "GdiText.h"
#include "FullMap.h"

#define FM_SECTION "FullMap"

/* ---------------- 地址 (反汇编实证) ---------------- */

#define ADDR_VIEWBOUNDS       0x00886FA0u
#define ADDR_SIDEBARBOUNDS    0x00886F90u
#define ADDR_WINDOWBOUNDS     0x00886FB0u
#define ADDR_SIDEBAR_W        0x007F5BF8u   /* 168 */
#define ADDR_RADAR_RECT       0x008809F4u   /* RadarClass+0x120C */
#define ADDR_SIDEBAR_DIRTY    0x00B0B518u   /* byte: TabClass::Draw 每帧置 1 */
#define ADDR_SIDEBAR_TOUCH    0x00B0B519u   /* byte */
#define ADDR_SIDEBAR_REDRAW   0x00884B90u   /* byte (this+0x53A8) */
#define ADDR_SIDEBAR_NEEDRD   0x00884B8Eu   /* byte (单例+0x53A6) */
#define ADDR_SIDEBAR_BGNEEDRD 0x00884B8Fu   /* byte (this+0x53A7) */
#define ADDR_GAME_HWND        0x00B73550u
#define ADDR_WWMOUSE          0x00887640u
#define ADDR_SURF_SIDEBAR     0x00887300u
#define ADDR_SURF_PRIMARY     0x00887308u
#define ADDR_SURF_COMPOSITE   0x0088731Cu

/* 侧栏 gadget: 两个地址确定的内联数组 (都是引擎自己的侧栏对象) */
#define ADDR_CAMEO_ARRAY      0x00B07E80u   /* SelectClass[240], 步长 0x38 */
#define FM_CAMEO_N            240
#define FM_CAMEO_STRIDE       0x38
#define ADDR_TAB_ARRAY        0x00B07C48u   /* ShapeButtonClass[4], 步长 0x60 */
#define FM_TAB_N              4
#define FM_TAB_STRIDE         0x60

#define GAD_X       0x0Cu
#define GAD_Y       0x10u
#define GAD_W       0x14u
#define GAD_H       0x18u
#define GAD_REDRAW  0x1Cu
#define GAD_DISAB   0x1Eu

/* 把两处"取 ViewBounds.Width 当侧栏横向偏移"的指令, 操作数改指向
 * DSurface::SidebarBounds.X —— 我们每循环都把它维护成 (视口宽 - 168) = 1752,
 * 正好就是小地图/按钮所在那一列的起点。所以只改 imm32, 不加指令、不动寄存器。
 *
 *  a) MouseClass::ProcessClick 0x478BD7  `mov eax,[0x886FA8]`
 *     视口撑满后 1920 永远大于任何鼠标X -> 整列点击被判成地图点击, 侧栏里的
 *     gadget (小地图 / 顶上一簇那两个按钮) 一个都收不到。改读 1752 之后
 *     鼠标X > 1752 就走侧栏分支。
 *  b) RadarClass 命中判定 0x65386B  `mov edx,[0x886FA8]`
 *     它 `and ecx,edx` 取的也是这个横向偏移 (NOD 侧栏在右), 需要 1752;
 *     改读 SidebarBounds.X 后 屏上X = 面内X(11) + 1752 = 1763, 对上了。
 * 两条指令的该操作数在函数内都只用于这一次比较/偏移, 不影响别的用途。 */
#define ADDR_SIDEBARBR_X    0x00478BD7u   /* mov eax,[imm32], imm32 @+1 */
#define ADDR_RADARHIT_X     0x0065386Bu   /* mov edx,[imm32], imm32 @+2 */

#define VT_COPY_FROM_PART 0x08   /* DSurface::CopyFromPart */
#define VT_FILL_RECT      0x14   /* DSurface::FillRect */
#define WW_VT_FIX_CURSOR  0x3C   /* WWMouseClass: 按面恢复光标 */

/* ---------------- 引擎函数原型 ---------------- */

typedef int (__thiscall *FnCopyFromPart)(void* self,
    RectangleStruct* pDstRect, void* pSrcSurface,
    RectangleStruct* pSrcRect, int bUnk1, int bUnk2);
typedef void (__thiscall *FnMouseFixSurface)(void* mouse, void* surface, int flag);
typedef int (__thiscall *FnFillRect)(void* self, RectangleStruct* rect, unsigned color);

/* ---------------- 配置 ---------------- */

static int s_Init = 0;
static int s_Enable = 0;
static int s_WidenView = 1;
static int s_WidenSurface = 0;
static int s_KeepRadar = 1;
static int s_TrimViewGadget = 1;
static int s_ExtraH = 5;
static int s_DisableButtons = 1;
static int s_Warned = 0;
static int s_SurfWarned = 0;
static unsigned s_nPoll = 0;
static unsigned s_nBlit = 0;


void FM_EnsureInit(void)
{
    char b1[16], buf[224];
    unsigned p;
    if (s_Init) return;
    s_Init = 1;
    GtEnsureLog();
    if (!g_Log) return;
    s_Enable = GetPrivateProfileIntA(FM_SECTION, "Enable", 0, g_IniPath) ? 1 : 0;
    s_WidenView = GetPrivateProfileIntA(FM_SECTION, "WidenView", 1, g_IniPath) ? 1 : 0;
    s_WidenSurface = GetPrivateProfileIntA(FM_SECTION, "WidenSurface", 0, g_IniPath) ? 1 : 0;
    s_KeepRadar = GetPrivateProfileIntA(FM_SECTION, "KeepRadar", 1, g_IniPath) ? 1 : 0;
    s_TrimViewGadget = GetPrivateProfileIntA(FM_SECTION, "TrimViewGadget", 1, g_IniPath) ? 1 : 0;
    s_ExtraH = GetPrivateProfileIntA(FM_SECTION, "ExtraH", 5, g_IniPath);
    if (s_ExtraH < 0) s_ExtraH = 0;
    if (s_ExtraH > 64) s_ExtraH = 64;
    s_DisableButtons =
        GetPrivateProfileIntA(FM_SECTION, "DisableSidebarButtons", 1, g_IniPath) ? 1 : 0;
    if (!s_Enable) return;
    p = 0;
    p = AppendCStr(buf, p, "fullmap: en=", sizeof(buf));
    UtoA((unsigned)s_Enable, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " view=", sizeof(buf));
    UtoA((unsigned)s_WidenView, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " surf=", sizeof(buf));
    UtoA((unsigned)s_WidenSurface, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " radar=", sizeof(buf));
    UtoA((unsigned)s_KeepRadar, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " btn=", sizeof(buf));
    UtoA((unsigned)s_DisableButtons, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

/* ---------------- 要保留的那一块: 侧栏面顶部整块 ----------------
 * SidebarBounds.Y (实测 158) 正好是"顶上一簇"与"面板"的分界:
 *   y <  158 : 资金条 + 小地图 + 它的边框 + 右边两个按钮   <- 保留
 *   y >= 158 : 页签 + cameo + 底部电力条                   <- 丢掉
 * 所以直接 blit {0,0,SidebarBounds.Width,SidebarBounds.Y}, 分界线是引擎自己
 * 定的, 不用猜。FM_DrawRadarTo (画进 Composite) 用的是同一个矩形。 */
static int Fm_KeepRect(RectangleStruct* out)
{
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    void* strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    int sw = 168, sh = 1080, k;
    if (strip) {
        int w = GtSurfGet(strip, GtVT_GET_WIDTH);
        int h = GtSurfGet(strip, GtVT_GET_HEIGHT);
        if (w > 0 && h > 0) { sw = w; sh = h; }
    }
k = sb->Y;
    if (k < 32 || k > 512) k = 158;
    k += s_ExtraH;                        /* 用户要求: 再多露一点 */
    if (k > sh) k = sh;
    if (k < 32) return 0;
    out->X = 0; out->Y = 0; out->Width = sw; out->Height = k;
    return 1;
}
/* ---------------- 把侧栏面里除小地图以外的部分抹掉 ----------------
 * 追了很久"整条侧栏是谁顶上屏的": upd: 插桩证明呈现的只有 Composite, 且
 * BlitSidebar 全二进制只有 0x6A70AE / 0x777723 两个调用点、都在补丁内 ——
 * 但面板就是会出现。说明它是**被画进了 Composite** (Render 之后
 * DSurface::Temp 就是 Composite, 而 Composite 是要上屏的)。
 * 既然追不出具体是谁画的, 就从源头堵: 每次接管 blit 时把侧栏面里小地图以外
 * 的部分全抹成黑。这样无论哪条路去呈这块面, 最多只能呈出小地图。 */
static void Fm_ClearStripExceptRadar(const RectangleStruct* keep)
{
    void* strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    void** vt;
    FnFillRect fn;
    RectangleStruct r;
    int sw = 168, sh = 1080;
    int w, h, top, bot;
    if (!strip || !IsPlausiblePtr(strip)) return;
    w = GtSurfGet(strip, GtVT_GET_WIDTH);
    h = GtSurfGet(strip, GtVT_GET_HEIGHT);
    if (w <= 0 || h <= 0) { sw = 168; sh = 1080; }
    else { sw = w; sh = h; }
    vt = *(void***)(size_t)strip;
    fn = (FnFillRect)vt[VT_FILL_RECT >> 2];
    if (!fn) return;
    top = keep->Y;
    bot = keep->Y + keep->Height;
    if (top < 0) top = 0;
    if (bot > sh) bot = sh;
    if (top > 0) {                       /* 小地图上方 */
        r.X = 0; r.Y = 0; r.Width = sw; r.Height = top;
        fn(strip, &r, 0);
    }
    if (bot < sh) {                      /* 小地图下方 */
        r.X = 0; r.Y = bot; r.Width = sw; r.Height = sh - bot;
        fn(strip, &r, 0);
    }
    if (keep->X > 0) {                   /* 左窄边 */
        r.X = 0; r.Y = top; r.Width = keep->X; r.Height = bot - top;
        fn(strip, &r, 0);
    }
    if (keep->X + keep->Width < sw) {    /* 右窄边 */
        r.X = keep->X + keep->Width; r.Y = top;
        r.Width = sw - (keep->X + keep->Width); r.Height = bot - top;
        fn(strip, &r, 0);
    }
}

/* 客户区屏幕原点: 与原版 BlitSidebar 完全相同的算法 (GetClientRect +
 * ClientToScreen), 原版就是拿它当 blit 目标基准的。取不到就退回 (0,0),
 * 全屏下正是 (0,0)。 */
static void Fm_ClientOrigin(int* ox, int* oy)
{
    HWND h = (HWND)(size_t)*(int*)(size_t)ADDR_GAME_HWND;
    RECT rc;
    POINT pt;
    *ox = 0;
    *oy = 0;
    if (!h) return;
    if (!IsWindow(h)) return;
    if (!GetClientRect(h, &rc)) return;
    if (rc.right < 100 || rc.bottom < 100) return;
    pt.x = rc.left;
    pt.y = rc.top;
    if (!ClientToScreen(h, &pt)) return;
    *ox = pt.x;
    *oy = pt.y;
}
/* ---------------- 视口矩形计算: 去掉侧栏那 168 ----------------
 * 0x72AD20 是纯函数 (只有 mov/xor/sub, 无副作用), 输出:
 *     out = { X: (非NOD且非末日模式) ? 168 : 0, Y: 0,
 *             W: WindowBounds.Width - 168, H: WindowBounds.Height - 32 }
 * 而 DSurface::Composite 的建面矩形与 DisplayClass::Set_View_Dimensions 的入参
 * **都是它的输出** (0x560F9C / 0x67E776 两处 call), 所以改这一个函数, 面和视口
 * 会自动一起变全宽 —— 不需要碰建面代码、不需要新分配、不需要跳板。
 *
 * 两处字节补丁 (等长改写, 不动任何指令边界):
 *   0x72AD69 `sub edx,0xA8` 的 imm32 -> 0      : W 不再扣 168
 *   0x72AD60 `mov ecx,0xA8`     -> 5x nop    : 盟军侧栏那 168 偏移也去掉
 *                                          (ecx 在 0x72AD44 已经是 0)
 *
 * ★ 之前试过钩 CreateAll(0x533FD0) 改建面矩形, 两次都崩 (0xC0000005 / EIP=0,
 *   崩在 CreateAll 里那几处 `mov edx,[eax]; call [edx+0x18]` 未判空)。直连
 *   DirectDraw 建面这条路不通, 别再碰。 */

#define ADDR_VIEWRECT       0x0072AD20u
#define ADDR_PATCH_SUBW     0x0072AD69u   /* imm32 of `sub edx,0xA8` */
#define ADDR_PATCH_GDIX     0x0072AD60u   /* `mov ecx,0xA8` */

static int Fm_BytesSame(const unsigned char* a, const unsigned char* b, int n)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int Fm_PatchBytes(unsigned at, const unsigned char* bytes, int n)
{
    unsigned char* p = (unsigned char*)(size_t)at;
    DWORD old;
    int i;
    if (!VirtualProtect(p, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (i = 0; i < n; i++) p[i] = bytes[i];
    VirtualProtect(p, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, (SIZE_T)n);
    return 1;
}

void FM_Install(void)
{
    static const unsigned char kWantSub[3] = { 0xA8, 0x00, 0x00 };  /* imm32 */
    static const unsigned char kWantGdi[5] = { 0xB9, 0xA8, 0x00, 0x00, 0x00 };
    static const unsigned char kZero4[4] = { 0x00, 0x00, 0x00, 0x00 };
    static const unsigned char kNop5[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
    FM_EnsureInit();
    if (!g_Log) return;
    if (!s_Enable || !s_WidenSurface)
    {
        LogLine("fullmap: view-rect patch OFF (sidebar stays 168)");
        return;
    }
    if (!Fm_BytesSame((unsigned char*)(size_t)ADDR_PATCH_SUBW, kWantSub, 3)
        || !Fm_BytesSame((unsigned char*)(size_t)ADDR_PATCH_GDIX, kWantGdi, 5))
    {
        LogLine("fullmap: viewrect bytes mismatch, patch OFF");
        return;
    }
    if (!Fm_PatchBytes(ADDR_PATCH_SUBW, kZero4, 3)
        || !Fm_PatchBytes(ADDR_PATCH_GDIX, kNop5, 5))
    {
        LogLine("fullmap: viewrect patch fail");
        return;
    }
    /* 两处"侧栏横向偏移"取数改指 SidebarBounds.X: 让小地图/那两个按钮重新能点 */
{
        static const unsigned char kWantA[4] = { 0xA8, 0x6F, 0x88, 0x00 };
        static const unsigned char kWantB[4] = { 0xA8, 0x6F, 0x88, 0x00 };
        static const unsigned char kToSbX[4] = { 0x90, 0x6F, 0x88, 0x00 };
        if (Fm_BytesSame((unsigned char*)(size_t)(ADDR_SIDEBARBR_X + 1), kWantA, 4)
            && Fm_BytesSame((unsigned char*)(size_t)(ADDR_RADARHIT_X + 2), kWantB, 4))
        {
            if (Fm_PatchBytes(ADDR_SIDEBARBR_X + 1, kToSbX, 4)
                && Fm_PatchBytes(ADDR_RADARHIT_X + 2, kToSbX, 4))
                LogLine("fullmap: viewrect + sidebar-hit patch ON (2 sites)");
            else
                LogLine("fullmap: sidebar-hit patch fail");
        }
        else LogLine("fullmap: sidebar-hit bytes mismatch");
    }
    LogLine("fullmap: viewrect hook ON (surface+view full width)");
}
/* ---------------- 小地图矩形 (侧栏面内坐标) ----------------
 * 正确来源是 RadarClass+0x120C = 0x8809F4 这一个 RectangleStruct, 不是
 * 0xB04A00 那一组 —— 那组是侧栏顶部另一个 72x18 小控件 (实测 radar=11,20,72x18,
 * blit 出来正是用户看到的"简报按钮")。
 * 0x8809F4 每帧由 SidebarClass::Draw (0x6A6C34~0x6A6C6F) 从
 * [0xB07DC8/D0/D4/D8] 刷新; 引擎自己"只呈小地图"的那条分支 (0x6A7433) 用的
 * 也正是它, dst 公式与下面 Fm_BlitSb 里的一致。 */

/* 侧栏面里那张雷达图是固定尺寸的, 但 0x8809F4 (= [0xB07DC8/D0/D4/D8] 每帧刷新)
 * 的 **高度实测会在 103~137 之间跳** (X/Y/宽一直稳在 11,20,145)。
 * 逐帧照抄就等于让 blit 矩形一闪一闪 —— 实测就是闪烁的来源。
 * 所以锁住它, 并且只增不减: 头几帧从小往大长到真实尺寸后就稳住了。
 * 侧栏面自身尺寸变了 (改分辨率) 就重新锁。 */
static RectangleStruct s_RadarLock;
static int s_RadarLockOk = 0;
static int s_RadarLockSW = 0;
static int s_RadarLockSH = 0;

static int Fm_RadarRect(RectangleStruct* out)
{
    RectangleStruct* r = (RectangleStruct*)(size_t)ADDR_RADAR_RECT;
    int sw = 168, sh = 1080;
    void* strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    if (strip) {
        int w = GtSurfGet(strip, GtVT_GET_WIDTH);
        int h = GtSurfGet(strip, GtVT_GET_HEIGHT);
        if (w > 0 && h > 0) { sw = w; sh = h; }
    }
    if (s_RadarLockOk && sw == s_RadarLockSW && sh == s_RadarLockSH)
    {
        if (r->Width > s_RadarLock.Width) s_RadarLock.Width = r->Width;
        if (r->Height > s_RadarLock.Height) s_RadarLock.Height = r->Height;
        if (s_RadarLock.X + s_RadarLock.Width > sw)
            s_RadarLock.Width = sw - s_RadarLock.X;
        if (s_RadarLock.Y + s_RadarLock.Height > sh)
            s_RadarLock.Height = sh - s_RadarLock.Y;
        *out = s_RadarLock;
        return 1;
    }
    if (r->Width <= 16 || r->Width > sw) return 0;
    if (r->Height <= 16 || r->Height > sh) return 0;
    /* 必须整体落在侧栏面内, 且不能是顶部那一小条控件 */
    if (r->X < 0 || r->X + r->Width > sw) return 0;
    if (r->Y < 0 || r->Y + r->Height > sh) return 0;
    if (r->Width < 64 || r->Height < 64) return 0;
    s_RadarLock = *r;
    s_RadarLockOk = 1;
    s_RadarLockSW = sw;
    s_RadarLockSH = sh;
    *out = *r;
    return 1;
}

/* ---------------- 摘侧栏 gadget ----------------
 * 只碰两个地址确定的内联数组 (cameo / 页签), 落在静态 .data 段, 是引擎自己的
 * 侧栏对象。Disabled 写 GadgetClass+0x1E —— 引擎 GadgetClass::Disable (0x4E1460)
 * 写的同一个字节, 命中测试与输入都会跳过, 点击于是落到地图上。 */

static void Fm_DisarmAt(unsigned g, int colX, int topH)
{
    unsigned char* p = (unsigned char*)(size_t)g;
    int x, y, w, h;
    x = *(int*)(p + GAD_X);
    y = *(int*)(p + GAD_Y);
    w = *(int*)(p + GAD_W);
    h = *(int*)(p + GAD_H);
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;
    if (x < -512 || x > 8192) return;
    if (y < -512 || y > 8192) return;
    if (y < topH) return;              /* 顶上一簇 (资金条/那两个按钮) 要留 */
    if (x < colX - 8) return;          /* 不在侧栏列里, 不归我们管 */
    if (x > colX + 256) return;
    p[GAD_DISAB] = 1;
    p[GAD_REDRAW] = 1;
}

static void Fm_DisarmSidebarGadgets(int colX, int topH)
{
    int i;
    for (i = 0; i < FM_CAMEO_N; i++)
        Fm_DisarmAt(ADDR_CAMEO_ARRAY + (unsigned)i * FM_CAMEO_STRIDE, colX, topH);
    for (i = 0; i < FM_TAB_N; i++)
        Fm_DisarmAt(ADDR_TAB_ARRAY + (unsigned)i * FM_TAB_STRIDE, colX, topH);
}

/* ---------------- 侧栏列同步 + 摘按钮 ----------------
 * 视口宽度已经由 0x72AD20 的字节补丁变成全宽 (Composite 也是同一个矩形建的),
 * 所以这里不再自己改 ViewBounds —— 引擎自己在 Set_View_Dimensions 里写好并同步
 * view_bound (0x6D5F60), 比我手写安全得多 (上一版就是在这里崩的)。
 * 只做两件收尾: 把侧栏列推到屏幕外, 并摘掉侧栏自带的按钮。 */

void FM_Poll(void)
{
    RectangleStruct* sb;
    RectangleStruct radar;
    RectangleStruct* vb;
    int* wb;
    int winW, sbW, colX, topH;

    if (!s_Enable) return;
    if (!g_Log) return;
    if (!s_WidenView || !s_WidenSurface) return;
    s_nPoll++;
    sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    wb = (int*)(size_t)ADDR_WINDOWBOUNDS;
    sbW = *(int*)(size_t)ADDR_SIDEBAR_W;
    if (sbW < 64 || sbW > 512) return;
    winW = wb[2];
    if (winW < 640 || winW > 8192) return;
    if (!Fm_RadarRect(&radar)) return;

    /* 安全阀: 视口不许比渲染面还宽, 否则地图会画到面外踩内存。
     * 补丁没生效时这里直接停用, 行为退回"只隐藏面板"。*/
    {
        void* comp = *(void**)(size_t)ADDR_SURF_COMPOSITE;
        int cw = comp ? GtSurfGet(comp, GtVT_GET_WIDTH) : -1;
        if (cw > 0 && vb->Width > cw)
        {
            if (!s_SurfWarned)
            {
                char b1[16], b2[16], buf[192];
                unsigned q = 0;
                s_SurfWarned = 1;
                q = AppendCStr(buf, q, "fullmap: view wider than surface ", sizeof(buf));
                UtoA((unsigned)vb->Width, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
                q = AppendCStr(buf, q, " > ", sizeof(buf));
                UtoA((unsigned)cw, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
                q = AppendCStr(buf, q, " -> skip", sizeof(buf));
                LogLine(buf);
            }
            return;
        }
    }

/* 侧栏列起点必须始终等于 (视口宽 - 列宽): MouseClass::ProcessClick 与
     * RadarClass 命中判定都靠它来识别那一列 (见上面两处操作数补丁)。*/
    colX = vb->X + vb->Width - sbW;
    if (sb->X != colX) sb->X = colX;
    topH = sb->Y;
    if (topH < 32 || topH > 512) topH = 158;

    if (s_DisableButtons)
        Fm_DisarmSidebarGadgets(colX, topH);
}
/* ★ 不要强制面板面重画。
 * 曾经为了治"内容随鼠标漂"去置 [this+0x53A6]/[0x53A7], 结果 SidebarClass::Draw
 * 每帧走全量重画分支 (0x6A6CFD 起的 CC_Draw_Shape 会把侧栏面整块重铺), 把先前
 * TabClass::Draw (0x6D0BBE -> 0x4A2370) 画进去的小地图擦掉; 小地图要等
 * RadarClass 自己判定视口变了才重画 —— 于是每帧擦一次、隔几帧补一次, 就是闪烁。
 * 面板面里小地图那块由 RadarClass 维护, 本来就是持久的, 不需要外力催它重画。
 * "内容随鼠标漂"那个症状的真正原因是当初 blit 用了错矩形 (0xB04A00 那个 72x18
 * 的小控件), 换成 RadarClass+0x120C 之后已经没有了。*/

/* 接管时的出口: 原版尾声 (0x6A7481) 等价物。原函数栈形是 [ret][bool] 且
 * 结尾 ret 4, 这里只做脏标记 + ret 4, 正好替掉整条 blit。 */
__attribute__((naked)) static void Fm_Ret4(void)
{
    __asm__ volatile ("ret $4\n\t");
}

unsigned long FM_BlitSb(void)
{
    void* strip;
    void* prim;
    void* mouse;
    RectangleStruct src, dst;
    int ox, oy, sbw, winW;

    if (!s_Enable) return 0;
    s_nBlit++;

    /* 与原版尾声 (0x6A7481/0x6A748A) 一致: 标脏标记给下帧 Render 用, 清脏旗。
     * 只在真正接管时做 —— 原版提前返回的那几条路并不会置 0xB0B519。 */
    *(unsigned char*)(size_t)ADDR_SIDEBAR_TOUCH = 1;
    *(unsigned char*)(size_t)ADDR_SIDEBAR_DIRTY = 0;
    *(unsigned char*)(size_t)ADDR_SIDEBAR_REDRAW = 0;

    strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    prim = *(void**)(size_t)ADDR_SURF_PRIMARY;
    mouse = *(void**)(size_t)ADDR_WWMOUSE;
    if (!strip || !prim || !IsPlausiblePtr(strip) || !IsPlausiblePtr(prim))
        return (unsigned long)(size_t)Fm_Ret4;

    /* 原版 blit 前先把光标区域从侧栏面上还原, 跟着做一次, 否则小地图上留洞 */
    if (mouse && IsPlausiblePtr(mouse))
    {
        void** vt = *(void***)(size_t)mouse;
        FnMouseFixSurface fn = (FnMouseFixSurface)vt[WW_VT_FIX_CURSOR >> 2];
        if (fn) fn(mouse, strip, 1);
    }

    if (s_KeepRadar && Fm_KeepRect(&src))
    {
        void** vt;
        FnCopyFromPart fn;
        /* 视口已经是全宽, 原来那套 `src.X + (isNOD?ViewW:0)` 会把小地图顶到屏幕外。
         * 改成右对齐: 客户宽 - 侧栏列宽 + 面内X。侧栏列没撑开时 (WidenSurface=0)
         * 这个式子正好退化成引擎原版的 dst.X, 一个像素都不差。*/
        sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
        if (sbw < 64 || sbw > 512) sbw = 168;
        Fm_ClientOrigin(&ox, &oy);
        winW = ((int*)(size_t)ADDR_WINDOWBOUNDS)[2];
        if (winW < 640 || winW > 8192) winW = src.X + sbw;
        dst.X = ox + winW - sbw + src.X;
        dst.Y = oy + src.Y;
        dst.Width = src.Width;
        dst.Height = src.Height;
        vt = *(void***)(size_t)prim;
        fn = (FnCopyFromPart)vt[VT_COPY_FROM_PART >> 2];
        Fm_ClearStripExceptRadar(&src);
        if (fn) fn(prim, &dst, strip, &src, 0, 1);
    }
    else if (!s_Warned)
    {
        s_Warned = 1;
        LogLine("fullmap: radar rect invalid, strip not shown");
    }
    return (unsigned long)(size_t)Fm_Ret4;
}
/* ---------------- 把小地图画进指定面 (每帧必跑的呈现前) ----------------
 * Composite 每帧都被地图重画并呈现, 所以把小地图直接画进 Composite, 它就是
 * 画面的一部分 —— 不再依赖侧栏何时上屏, 闪烁在构造上消失。
 * 坐标与呈到 Primary 时一致 (右对齐): 客户宽 - 侧栏列宽 + 面内X。 */
int FM_DrawRadarTo(void* surf)
{
    RectangleStruct src, dst;
    void* strip;
    void** vt;
    FnCopyFromPart fn;
    int sbw, winW;

    if (!s_Enable || !s_KeepRadar) return 0;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    if (!strip || !IsPlausiblePtr(strip)) return 0;
    if (!Fm_KeepRect(&src)) return 0;

    sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
    if (sbw < 64 || sbw > 512) sbw = 168;
    winW = ((int*)(size_t)ADDR_WINDOWBOUNDS)[2];
    if (winW < 640 || winW > 8192) winW = 1920;

    dst.X = winW - sbw + src.X;
    dst.Y = src.Y;
    dst.Width = src.Width;
    dst.Height = src.Height;

    vt = *(void***)(size_t)surf;
    fn = (FnCopyFromPart)vt[VT_COPY_FROM_PART >> 2];
    if (!fn) return 0;
Fm_ClearStripExceptRadar(&src);
    fn(surf, &dst, strip, &src, 0, 1);
    return 1;
}

/* ---------------- 视口 gadget 收窄 ----------------
 * GadgetClass::Input (0x4E1640) 里的命中测试是 ExtractGadgetAt (0x4E15A0):
 *   X=+0x0C Y=+0x10 W=+0x14 H=+0x18 直接当屏幕坐标比, 沿 GetPrev(+0x04) 走,
 *   每个命中都覆盖结果, 最终返回沿 GetPrev 最靠前的那个。
 * 输入链上只有一个 gadget, 横跨整个视口 (实测 {0,0,1920x1048}, 矩形恒等于
 * ViewBounds)。原版它宽 1752, 正好够不着侧栏那一列; 地图铺满后变 1920,
 * 就把右上角整列吞进命中区, 那一列的点击全被判成"点在视口上"。
 * 指针取自 Input 的 this —— 是真 gadget 指针, 不是 ID。 */
#define GAD_LOG_X       0x0Cu
#define GAD_LOG_W       0x14u

static void* s_ViewGadget = NULL;
static unsigned s_TrimOldW = 0;
static int s_TrimLogged = 0;

void FM_TrimViewGadget(void)
{
    unsigned char* g;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int gx, gw, colX;

    if (!s_Enable || !s_TrimViewGadget) return;
    g = (unsigned char*)s_ViewGadget;
    if (!g || !IsPlausiblePtr(g)) return;
    colX = sb->X;
    if (colX < 640 || colX > 8192) return;
    gx = *(int*)(g + GAD_LOG_X);
    gw = *(int*)(g + GAD_LOG_W);
    if (gx < 0 || gw <= 0) return;
    if (gx + gw <= colX) return;          /* 本来就够不着侧栏, 不用动 */
    if (gx >= colX) return;              /* 整块都在侧栏右侧, 不是它 */
    *(int*)(g + GAD_LOG_W) = colX - gx;
    g[0x20] = 1;                         /* NeedsRedraw */
    if (s_TrimOldW != (unsigned)gw)
    {
        char t1[16], t2[16], buf[160];
        unsigned q = 0;
        s_TrimOldW = (unsigned)gw;
        if (!s_TrimLogged)
        {
            s_TrimLogged = 1;
            q = AppendCStr(buf, q, "fullmap: view gadget trimmed w=", sizeof(buf));
            UtoA((unsigned)gw, t1); q = AppendCStr(buf, q, t1, sizeof(buf));
            q = AppendCStr(buf, q, "->", sizeof(buf));
            UtoA((unsigned)(colX - gx), t2); q = AppendCStr(buf, q, t2, sizeof(buf));
            LogLine(buf);
        }
    }
}

void FM_NoteGadgetInput(void* gadget)
{
    unsigned char* g;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int gx, gw, colX;

    if (!gadget || !IsPlausiblePtr(gadget)) return;
    g = (unsigned char*)gadget;
    colX = sb->X;
    if (colX < 640 || colX > 8192) return;
    gx = *(int*)(g + GAD_LOG_X);
    gw = *(int*)(g + GAD_LOG_W);
    if (gx < 0 || gw <= 0) return;
    /* 只认横向跨过侧栏分界线的那个 (实测 {0,0,1920x1048}); 它就是该收窄的目标 */
    if (gx >= colX || gx + gw <= colX) return;
    s_ViewGadget = gadget;
    /* 在命中判定之前收窄: 钩子跑在原函数体之前, 而原函数紧接着就调
     * ExtractGadgetAt, 所以此刻改宽度能立刻影响这一次命中。
     * (FM_Poll 里做无效 —— 那里排在 Input 之后, 引擎每帧从 ViewBounds 重设。) */
    FM_TrimViewGadget();
}
