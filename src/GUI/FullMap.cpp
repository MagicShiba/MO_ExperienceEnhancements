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
#define ADDR_ISNOD_SIDEBAR    0x00A8EB7Cu   /* byte: 1=侧栏在右(苏联), 0=在左(盟军) */
#define ADDR_RADAR_RECT       0x008809F4u   /* RadarClass+0x120C: 雷达绘制矩形(侧栏面内) */
#define ADDR_RADAR_GADGET    0x00B04A10u   /* 雷达 gadget 本体 (虚表 0x7F02BC, Action=+0x7C) */
#define ADDR_RADAR_VTABLE    0x007F02BCu
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
 * DSurface::SidebarBounds.X —— 我们每循环都把它维护成侧栏列在屏幕上的起点
 * (苏联=视口宽-168, 盟军=0), 正好就是小地图/按钮所在那一列的起点。所以只改
 * imm32, 不加指令、不动寄存器。
 *
 * ★ 这个偏移量在引擎里有 5 处同样的读法, 少改一处就有一处把点击判定落到屏外:
 *  a) 0x478BD7 `mov eax,[0x886FA8]`  MouseClass 鼠标钳制路由 (视口铺满后
 *     1920 永远大于任何鼠标X -> 整列点击被判成地图点击)。
 *  b) 0x65386B `mov edx,[0x886FA8]`  顶部两个小按钮的光标形状区命中判定。
 *  c) 0x654332 `mov ecx,[0x886FA8]`  ★ RadarClass 摆放三个 gadget 的位置:
 *     0xB04978 / 0xB04910 (顶部两个按钮) 与 0xB04A10 (雷达 gadget 本体) 的
 *     X 都是 `局部X + 这个偏移`。铺满后偏移变成 1920 -> 三个 gadget 全部被摆到
 *     屏幕外 (实测 1936,49), ExtractGadgetAt 永远命中不到, 于是小地图点击既
 *     进不了 RadarClass::Action 也进不了顶上一簇按钮的命中逻辑。
 *     ★ 这三者的位置只在 DisplayClass::InitGUI(0x6ABD30 -> 0x6403A0) 里算一次,
 *       所以"运行时自己改 gadget 矩形"撑不过下一帧/换分辨率, 必须改这处 imm32。
 *  d) 0x653A3F `mov ebx,[0x886FA8]`  RadarClass::Action 内部把鼠标 X 换算成
 *     侧栏面内坐标的偏移。不改则 esi = 鼠标X - 1920 恒为负, 命中判定恒假。
 *  e) 0x6D06C7 `mov edi,[0x886FA8]`  侧栏顶部一簇的光标形状判定 (纯外观)。
 *
 * 未改的同类读法 (刻意): 0x653D92 (`ViewBounds.Width/9` = 视口能显示多少格,
 * 原版 1752/9=194 已经远大于任何地图宽度, 改成 1752 与不改等价)。 */
#define ADDR_SIDEBARBR_X    0x00478BD7u   /* A1 A8 6F 88 00 = mov eax,[imm32], imm32 @+1 */
#define ADDR_RADARHIT_X     0x0065386Bu   /* 8B 15 ..      = mov edx,[imm32], imm32 @+2 */
#define ADDR_RADARPOS_X     0x00654332u   /* 8B 0D ..      = mov ecx,[imm32], imm32 @+2 */
#define ADDR_RADARACT_X     0x00653A3Fu   /* 8B 1D ..      = mov ebx,[imm32], imm32 @+2 */
#define ADDR_TOPCURSOR_X    0x006D06C7u   /* 8B 3D ..      = mov edi,[imm32], imm32 @+2 */

/* 小地图点击吞没门 (DisplayClass::ProcessClickCoords 0x692300 内)。
 *  落点 0x692406 处正好两条指令 (51 B9 E8 F7 87 00 = push ecx / mov ecx,0x87F7E8),
 *  Syringe 钩 6 字节 (见 MOExperience.dll.inj); 跳回 0x69240C。返回 0 走原版,
 *  返回 0x6925FC 即原函数的"未处理"出口
 *  (pop edi/esi/ebp / xor al,al / pop ebx / add esp,0x18 / ret 0x18),
 *  栈形在该落点上完全对齐。做法与 SWSBar 的 SWS_ClickGate 同一套路。 */
#define ADDR_CLICKGATE_SKIP  0x006925FCu

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
static int s_RadarClick = 1;
static int s_GadgetLogged = 0;
static int s_JumpLogged = 0;
/* 左键"本次按下已处理"标记 —— 只由点击门维护 (读 GetAsyncKeyState, 松开时清) */
static int s_LbPress = 0;
static int s_Warned = 0;
static int s_SurfWarned = 0;
static int s_ClickLogged = 0;
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
    s_RadarClick =
        GetPrivateProfileIntA(FM_SECTION, "RadarClick", 1, g_IniPath) ? 1 : 0;
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
    p = AppendCStr(buf, p, " rclk=", sizeof(buf));
    UtoA((unsigned)s_RadarClick, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
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
 * ★★★ 但光改 0x72AD20 不够 —— 0x533FD0 (GScreenClass::CreateAll) 只有三个调用点,
 *   两个用 0x72AD20 的输出建面, **第三个不用**:
 *     0x56100E  窗口尺寸变化/初建  -> arg1 = 0x72AD20 的输出  (面宽跟着补丁变全宽)
 *     0x67E7EA  同上, 同一个形状
 *     0x6BDE14  **新开一局走这条** -> 自己算, 完全不碰 0x72AD20:
 *         0x6BDDAC: mov edx,[0x7F5BF8]      ; edx = 168
 *         0x6BDDC4: mov eax,[0x886FB8]      ; eax = WindowBounds.Width
 *         0x6BDDC8: sub eax,edx             ; eax = Width-168  <== 面的宽度来源
 *         0x6BDDD7: mov [ebp-0x7A4],eax     ; CreateAll 的 arg1.Width
 *         (同一段还把 {0,0,168,H} 交给 arg2 -> 另建一个 168 宽的面)
 *   ⇒ 新开一局 Composite 一直是 `Width-168 = 1752`, 于是视口被迫夹到 1752,
 *     右边 168px 没人画 = 黑底; 而**读档不重跑这条**, 所以读档看着正常。
 *   修法: 把 `sub eax,edx` 换成等长的 `sub eax,eax` (2B C2 -> 2B C0),
 *     W 就变成完整的 WindowBounds.Width。edx 在这条指令之后已无用途,
 *     arg2 的侧栏矩形在 0x6BDDB2 就已经写好了, 不受影响。
 *
 * ★ 之前试过钩 CreateAll(0x533FD0) 改建面矩形, 两次都崩 (0xC0000005 / EIP=0,
 *   崩在 CreateAll 里那几处 `mov edx,[eax]; call [edx+0x18]` 未判空)。直连
 *   DirectDraw 建面这条路不通, 别再碰 —— 改调用点的字节就行, 不用钩。 */

#define ADDR_VIEWRECT       0x0072AD20u
#define ADDR_PATCH_SUBW     0x0072AD69u   /* imm32 of `sub edx,0xA8` */
#define ADDR_PATCH_GDIX     0x0072AD60u   /* `mov ecx,0xA8` */
#define ADDR_PATCH_SURFW    0x006BDDC8u   /* `sub eax,edx` in new-scenario CreateAll */

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

/* 某处 imm32 核对失败时的日志 (静态缓冲, 只在启动时调用)。
 * 注意: 打印的是指令首地址处的前 4 字节, 顺序按内存从小到大。 */
static const char* Fm_SiteBad(const char* name, unsigned at)
{
    static char buf[96];
    unsigned q = 0;
    unsigned char* p = (unsigned char*)(size_t)at;
    char b1[16];
    int i;
    q = AppendCStr(buf, q, "fullmap: hit-site bytes mismatch ", sizeof(buf));
    q = AppendCStr(buf, q, name, sizeof(buf));
    q = AppendCStr(buf, q, " @", sizeof(buf));
    UtoA(at, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " =", sizeof(buf));
    for (i = 0; i < 4; i++)
    {
        b1[0] = "0123456789ABCDEF"[(p[i] >> 4) & 15];
        b1[1] = "0123456789ABCDEF"[p[i] & 15];
        b1[2] = 0;
        q = AppendCStr(buf, q, b1, sizeof(buf));
    }
    return buf;
}

void FM_Install(void)
{
    static const unsigned char kWantSub[3] = { 0xA8, 0x00, 0x00 };  /* imm32 */
    static const unsigned char kWantGdi[5] = { 0xB9, 0xA8, 0x00, 0x00, 0x00 };
    static const unsigned char kWantSur[2] = { 0x2B, 0xC2 };          /* sub eax,edx */
    /* ★ 必须是 nop nop, 不能写 `sub eax,eax` (2B C0)!
     *   `2B C0` 让 eax 归零 -> W 变 0 -> CreateAll 里两处 `test W; jle`
     *   全部跳过 -> [0x88731C]/[0x8872FC] 保持 NULL ->
     *   引擎 0x6D410D `mov edx,[0x8872FC]; mov edx,[edx]` 空指针崩。
     *   要的是"不减", 也就是什么都不做。 */
    static const unsigned char kToSur[2]   = { 0x90, 0x90 };          /* nop nop */
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
    /* 新开一局那条 CreateAll 不经过 0x72AD20, 自己用 [0x7F5BF8] 扣 168。
     * 不改这里 Composite 就是 Width-168 宽 => 右侧 168px 黑底 (读档不复现)。 */
    if (Fm_BytesSame((unsigned char*)(size_t)ADDR_PATCH_SURFW, kWantSur, 2))
    {
        LogLine(Fm_PatchBytes(ADDR_PATCH_SURFW, kToSur, 2)
            ? "fullmap: surface-width patch ON (new scenario, 0x6BDDC8)"
            : "fullmap: surface-width patch fail");
    }
    else
    {
        LogLine(Fm_SiteBad("newwidth", ADDR_PATCH_SURFW));
    }
    /* 五处"侧栏横向偏移"取数改指 SidebarBounds.X: 让小地图/顶上一簇按钮重新
     * 既画在屏幕内、又能收点击。逐条核对原字节, 全部一致才一起改。 */
    {
        static const unsigned char kWant[4] = { 0xA8, 0x6F, 0x88, 0x00 };
        static const unsigned char kToSbX[4] = { 0x90, 0x6F, 0x88, 0x00 };
        static const struct { unsigned at; int off; const char* name; } kSites[5] = {
            { ADDR_SIDEBARBR_X, 1, "route" },
            { ADDR_RADARHIT_X, 2, "topcursor" },
            { ADDR_RADARPOS_X, 2, "radarpos" },
            { ADDR_RADARACT_X, 2, "radaract" },
            { ADDR_TOPCURSOR_X, 2, "stripcursor" },
        };
        int i, ok = 1, done = 0;
        char b1[16], buf[192];
        unsigned q;
        for (i = 0; i < 5; i++)
        {
            if (!Fm_BytesSame((unsigned char*)(size_t)(kSites[i].at + kSites[i].off), kWant, 4))
            {
                ok = 0;
                LogLine(Fm_SiteBad(kSites[i].name, kSites[i].at));
            }
        }
        if (ok)
        {
            for (i = 0; i < 5; i++)
                if (Fm_PatchBytes(kSites[i].at + kSites[i].off, kToSbX, 4)) done++;
            q = 0;
            q = AppendCStr(buf, q, "fullmap: sidebar-hit patch ON ", sizeof(buf));
            UtoA((unsigned)done, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, "/5 sites", sizeof(buf));
            LogLine(buf);
        }
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
int FM_IniOn(const char* key, int def)
{
    return GetPrivateProfileIntA(FM_SECTION, key, def, g_IniPath) ? 1 : 0;
}

/* 侧栏列起点: 引擎判定"鼠标在不在侧栏上"和小地图/按钮摆放全用它。
 *
 * ★ 引擎自己算出来的值是 `SidebarBounds.X = viewRect.X + viewRect.W`
 *   (0x6A51C0, viewRect 来自 0x72AD90)。原版视口扣掉侧栏 168, 所以那个和正好
 *   等于"侧栏列在屏幕上的起点"(NOD 1920 屏 = 0+1752 = 1752)。
 *   视口铺满后 viewRect.W 变成 1920 -> 引擎算出的 1752 变成 **1920**,
 *   于是 (a) 三个 radar gadget 被摆到屏外 1936 (0x654320 用它当横向偏移),
 *   (b) InitGUI 里 `mov [0xB0B3E4], -SidebarBounds.X` 之类一次性写下的
 *       cameo/页签滚动偏移也全错 168 -> **侧栏布局显示混乱**。
 *   所以每循环要把它按"视口右边界 - 列宽"改回来 (苏联=1752 / 盟军=视口左边界),
 *   并且必须在 DisplayClass::InitGUI (0x6ABD30) **之前**就改好 —— 那一堆一次性
 *   写入只发生一次, 下一帧再纠正过来已经晚了 (MOExperience.cpp 挂了入口裸钩)。 */
typedef void (__thiscall *FnSyncViewBounds)(void* tactical, void* rect);
#define ADDR_SYNC_VIEW       0x006D5F60u   /* TacticalClass 视口同步 (Set_View_Dimensions 内部第二步) */

void FM_SyncSidebarColumn(void)
{
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    RectangleStruct* vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    int sbW = *(int*)(size_t)ADDR_SIDEBAR_W;
    int colX;
    if (!s_Enable) return;
    if (sbW < 64 || sbW > 512) return;
    if (vb->Width < 64 || vb->Width > 8192) return;
    if (*(unsigned char*)(size_t)ADDR_ISNOD_SIDEBAR)
        colX = vb->X + vb->Width - sbW;
    else
        colX = vb->X;
    if (colX < 0) colX = 0;
    if (sb->X != colX) sb->X = colX;
}

void FM_Poll(void)
{
    RectangleStruct* sb;
    RectangleStruct radar;
    RectangleStruct* vb;
    int* wb;
    int winW, sbW, colX, topH;

    if (!s_Enable) return;
    if (!g_Log) return;
    /* ★ 这里**不要**碰 s_LbPress: 那是点击门自己维护的"本次按下已处理"标记。
     * 门直接读 GetAsyncKeyState, 不依赖 prepare 与点击事件的先后; 上一版在
     * 这里采"按下沿"写进同一个变量, 结果门每次进来都以为"已处理"而永不触发。*/
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
            /* ★ 不能只 "skip": 视口比渲染面宽时引擎照旧往面外画, 那就是
             * "新开一局花屏"的直接原因 (Composite 偶尔是按旧宽度 1752 建的)。
             * 这里把视口收到面宽以内, 并同步 TacticalClass 的 view_bound ——
             * 照抄引擎 DisplayClass::Set_View_Dimensions (0x4A8960) 里那两步:
             * 写 ViewBounds, 然后 push &ViewBounds / call 0x6D5F60 (ecx=Tactical)。*/
            if (!s_SurfWarned)
            {
                char b1[16], b2[16], buf[192];
                unsigned q = 0;
                s_SurfWarned = 1;
                q = AppendCStr(buf, q, "fullmap: view wider than surface ", sizeof(buf));
                UtoA((unsigned)vb->Width, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
                q = AppendCStr(buf, q, " > ", sizeof(buf));
                UtoA((unsigned)cw, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
                q = AppendCStr(buf, q, " -> clamp", sizeof(buf));
                LogLine(buf);
            }
            vb->Width = cw;
            {
                void* tac = *(void**)(size_t)0x00887324u;
                FnSyncViewBounds sync = (FnSyncViewBounds)(size_t)ADDR_SYNC_VIEW;
                if (tac && IsPlausiblePtr(tac) && sync)
                    sync(tac, (void*)(size_t)ADDR_VIEWBOUNDS);
            }
        }
    }

    /* 侧栏列起点按"视口右边界 - 列宽"纠正 (理由见 FM_SyncSidebarColumn 注释) */
    FM_SyncSidebarColumn();
    colX = sb->X;
    topH = sb->Y;
    if (topH < 32 || topH > 512) topH = 158;

if (s_DisableButtons)
        Fm_DisarmSidebarGadgets(colX, topH);

    /* 一次性自检: 三个 radar gadget 的矩形是不是真的落在那一列里。
     * 位置只在 DisplayClass::InitGUI (0x6ABD30 -> 0x6403A0 -> 0x654320) 算一次,
     * 偏移取自被我们改过 imm32 的 0x654332, 所以这里能直接看出补丁是否生效:
     * 应该在 colX+11..colX+156 / colX+83..colX+155 之间。 */
    if (!s_GadgetLogged)
    {
        static const unsigned kG[3] = { 0x00B04978u, 0x00B04910u, 0x00B04A10u };
        static const char* kN[3] = { "btnA", "btnB", "radar" };
        char b1[16], b2[16], buf[256];
        int i;
        unsigned q = 0;
        s_GadgetLogged = 1;
        for (i = 0; i < 3; i++)
        {
            unsigned char* g = (unsigned char*)(size_t)kG[i];
            int gx, gy, gw, gh;
            if (!IsPlausiblePtr(g) || !*(void**)g) continue;
            /* GadgetClass: +0x0C X / +0x10 Y / +0x14 W / +0x18 H */
            gx = *(int*)(g + 0x0Cu);
            gy = *(int*)(g + 0x10u);
            gw = *(int*)(g + 0x14u);
            gh = *(int*)(g + 0x18u);
            q = AppendCStr(buf, q, i ? " " : "fullmap: gadget ", sizeof(buf));
            q = AppendCStr(buf, q, kN[i], sizeof(buf));
            q = AppendCStr(buf, q, "=", sizeof(buf));
            UtoA((unsigned)gx, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)gy, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)gw, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, "x", sizeof(buf));
            UtoA((unsigned)gh, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
        }
        q = AppendCStr(buf, q, " colX=", sizeof(buf));
        UtoA((unsigned)colX, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        LogLine(buf);
    }
}
/* ★ 不要强制面板面重画。
 * 曾经为了治"内容随鼠标漂"去置 [this+0x53A6]/[0x53A7], 结果 SidebarClass::Draw
 * 每帧走全量重画分支 (0x6A6CFD 起的 CC_Draw_Shape 会把侧栏面整块重铺), 把先前
 * TabClass::Draw (0x6D0BBE -> 0x4A2370) 画进去的小地图擦掉; 小地图要等
 * RadarClass 自己判定视口变了才重画 —— 于是每帧擦一次、隔几帧补一次, 就是闪烁。
 * 面板面里小地图那块由 RadarClass 维护, 本来就是持久的, 不需要外力催它重画。
 * "内容随鼠标漂"那个症状的真正原因是当初 blit 用了错矩形 (0xB04A00 那个 72x18
 * 的小控件), 换成 RadarClass+0x120C 之后已经没有了。*/

/* ---------------- 目标矩形必须整个落在目标面内 ----------------
 * ★ 这是"新开一局花屏"的直接原因: Composite 偶尔是按**旧宽度** (1920-168=1752)
 *   建的 (建面时机早于 0x72AD20 的字节补丁, 或者走了别的重建路径)。
 *   保留区在右边 (dst.X = 1752+11 = 1763, 宽 168), 往 1752 宽的面上 blit
 *   会越过行尾 116 像素 -> 直接踩坏后面的行/内存 -> 画面混乱。
 *   所以任何一笔 blit 之前都先按目标面实际尺寸夹一刀。返回 0 = 不画。 */
static int Fm_FitRectToSurf(void* surf, RectangleStruct* dst, RectangleStruct* src)
{
    int w = GtSurfGet(surf, GtVT_GET_WIDTH);
    int h = GtSurfGet(surf, GtVT_GET_HEIGHT);
    RectangleStruct s2 = *src;
    if (w <= 0 || h <= 0) return 0;
    if (dst->X < 0 || dst->Y < 0) return 0;
    if (dst->X >= w || dst->Y >= h) return 0;
    if (s2.X < 0 || s2.Y < 0) return 0;
    /* 右边/下边超出就裁掉超出部分 (源与目标同步裁, 保持 1:1) */
    if (dst->X + s2.Width > w) s2.Width = w - dst->X;
    if (dst->Y + s2.Height > h) s2.Height = h - dst->Y;
    if (s2.Width <= 0 || s2.Height <= 0) return 0;
    if (s2.X + s2.Width > w) return 0;
    if (s2.Y + s2.Height > h) return 0;
    *src = s2;
    return 1;
}

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
        /* 原版 blit 用 `src.X + (isNOD?ViewW:0)`, 视口铺满后会把小地图顶到屏外。
         * 改成用 SidebarBounds.X (FM_Poll 每循环维护成侧栏列在屏幕上的起点):
         * 侧栏列没撑开时这个式子与引擎原版公式逐像素等价, 盟军侧也落在左角。*/
        sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
        if (sbw < 64 || sbw > 512) sbw = 168;
        Fm_ClientOrigin(&ox, &oy);
        winW = ((int*)(size_t)ADDR_WINDOWBOUNDS)[2];
        if (winW < 640 || winW > 8192) winW = src.X + sbw;
        dst.X = ox + ((RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS)->X + src.X;
        dst.Y = oy + src.Y;
        dst.Width = src.Width;
        dst.Height = src.Height;
        if (!Fm_FitRectToSurf(prim, &dst, &src))
            return (unsigned long)(size_t)Fm_Ret4;
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
 * 坐标与呈到 Primary 时一致: SidebarBounds.X + 面内X (见 FM_BlitSb)。 */
int FM_DrawRadarTo(void* surf)
{
    RectangleStruct src, dst;
    void* strip;
    void** vt;
    FnCopyFromPart fn;
    int sbw, winW, colX;

    if (!s_Enable || !s_KeepRadar) return 0;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    if (!strip || !IsPlausiblePtr(strip)) return 0;
    if (!Fm_KeepRect(&src)) return 0;

    sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
    if (sbw < 64 || sbw > 512) sbw = 168;
    winW = ((int*)(size_t)ADDR_WINDOWBOUNDS)[2];
    if (winW < 640 || winW > 8192) winW = 1920;
    colX = ((RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS)->X;
    if (colX < 0 || colX > winW) colX = winW - sbw;
    if (colX < 0) colX = 0;

    dst.X = colX + src.X;
    dst.Y = src.Y;
    dst.Width = src.Width;
    dst.Height = src.Height;
    if (!Fm_FitRectToSurf(surf, &dst, &src)) return 0;

    vt = *(void***)(size_t)surf;
    fn = (FnCopyFromPart)vt[VT_COPY_FROM_PART >> 2];
    if (!fn) return 0;
Fm_ClearStripExceptRadar(&src);
    fn(surf, &dst, strip, &src, 0, 1);
    return 1;
}

/* ---------------- 视口 gadget 收窄 ----------------
 * GadgetClass::Input (0x4E1640) 里的命中测试是 ExtractGadgetAt (0x4E15A0):
 *   X=+0x0C Y=+0x10 W=+0x14 H=+0x18 直接当屏幕坐标比, 沿 GetPrev(+0x04) 走;
 *   命中里取**面积最小**的那个 (0x4E15FB `imul` 比 W*H, 大了就不取)。
 * 输入链上只有一个 gadget, 横跨整个视口 (实测 {0,0,1920x1048}, 矩形恒等于
 * ViewBounds)。原版它恰好等于"视口减去侧栏列", 地图铺满后变成整宽, 就把侧栏
 * 那一列整个吞进命中区, 那一列的点击全被判成"点在视口上"。
 * 指针取自 Input 的 this —— 是真 gadget 指针, 不是 ID。
 * 收窄成"视口减去侧栏列"即与原版几何完全一致:
 *   苏联 (列在右) -> 左边界不动, 右边界收到列起点
 *   盟军 (列在左) -> 左边界推到列尾, 右边界不动 */
#define GAD_LOG_X       0x0Cu
#define GAD_LOG_W       0x14u

static void* s_ViewGadget = NULL;
static unsigned s_TrimOldW = 0;
static int s_TrimLogged = 0;

/* 视口扣掉侧栏列之后的矩形。读不到 (列/视口不合理) 返回 0。 */
static int Fm_ViewNoSidebar(RectangleStruct* out)
{
    RectangleStruct* vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
    int right;
    if (sbw < 64 || sbw > 512) return 0;
    if (vb->Width <= sbw || vb->Height <= 64) return 0;
    right = vb->X + vb->Width;
    if (*(unsigned char*)(size_t)ADDR_ISNOD_SIDEBAR)
    {
        out->X = vb->X;
        out->Width = sb->X - vb->X;
    }
    else
    {
        out->X = sb->X + sbw;
        out->Width = right - out->X;
    }
    out->Y = vb->Y;
    out->Height = vb->Height;
    return out->Width > 16 && out->Height > 16;
}

void FM_TrimViewGadget(void)
{
    unsigned char* g;
    RectangleStruct keep;
    int gx, gw, wasX, wasW;

    if (!s_Enable || !s_TrimViewGadget) return;
    g = (unsigned char*)s_ViewGadget;
    if (!g || !IsPlausiblePtr(g)) return;
    if ((unsigned)(size_t)g < 0x00400000u
        || (unsigned)(size_t)g > 0x008A0000u) { s_ViewGadget = NULL; return; }
    if (!Fm_ViewNoSidebar(&keep)) return;
    gx = *(int*)(g + GAD_LOG_X);
    gw = *(int*)(g + GAD_LOG_W);
    if (gx < 0 || gw <= 0) return;
    /* 只动"横向宽度 == ViewBounds 宽"那一个 (实测 {0,0,1920x1048}); 别的
     * gadget 不归我们管。引擎每帧从 ViewBounds 重设, 所以每次命中都要重做。*/
    if (gx == keep.X && gw == keep.Width) return;
    wasX = gx; wasW = gw;
    *(int*)(g + GAD_LOG_X) = keep.X;
    *(int*)(g + GAD_LOG_W) = keep.Width;
    g[0x20] = 1;                         /* NeedsRedraw */
    if (s_TrimOldW != (unsigned)wasW || s_TrimLogged == 0)
    {
        char t1[16], t2[16], t3[16], buf[192];
        unsigned q = 0;
        s_TrimOldW = (unsigned)wasW;
        if (!s_TrimLogged)
        {
            s_TrimLogged = 1;
            q = AppendCStr(buf, q, "fullmap: view gadget ", sizeof(buf));
            UtoA((unsigned)wasX, t1); q = AppendCStr(buf, q, t1, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)wasW, t2); q = AppendCStr(buf, q, t2, sizeof(buf));
            q = AppendCStr(buf, q, " -> ", sizeof(buf));
            UtoA((unsigned)keep.X, t1); q = AppendCStr(buf, q, t1, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)keep.Width, t2); q = AppendCStr(buf, q, t2, sizeof(buf));
            q = AppendCStr(buf, q, " sidebar=", sizeof(buf));
            UtoA((unsigned)(((RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS)->X), t3);
            q = AppendCStr(buf, q, t3, sizeof(buf));
            LogLine(buf);
        }
    }
}

void FM_NoteGadgetInput(void* gadget)
{
    unsigned char* g;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    RectangleStruct* vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    void** vt;
    int gx, gw, sbw;

    if (!gadget || !IsPlausiblePtr(gadget)) return;
    g = (unsigned char*)gadget;
    /* ★ 内存安全: 只认 gamemd.exe 自己映像 (.text .data .bss) 里的 gadget。
     * 这里要往对象里写 X/Width, 万一 this 是别的模块 (Ares 之类) 造出来的
     * gadget 或野指针, 写进去就是内存破坏。 */
    if ((unsigned)(size_t)gadget < 0x00400000u
        || (unsigned)(size_t)gadget > 0x008A0000u) return;
    vt = *(void***)g;
    if (!vt || !IsPlausiblePtr(vt)) return;
    if ((unsigned)(size_t)vt < 0x00400000u
        || (unsigned)(size_t)vt > 0x008A0000u) return;
    sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
    if (sbw < 64 || sbw > 512) return;
    gx = *(int*)(g + GAD_LOG_X);
    gw = *(int*)(g + GAD_LOG_W);
    /* 认它 = 矩形恒等于 ViewBounds (实测 {0,0,1920x1048}); 别的 gadget 不归我们管 */
    if (gw <= 0) return;
    if (gw != vb->Width) return;
    if (gx != vb->X) return;
    /* 且必须横向盖住侧栏列, 否则收窄它没有意义 */
    if (gx + gw <= sb->X) return;
    if (gx >= sb->X + sbw) return;
    s_ViewGadget = gadget;
    /* 在命中判定之前收窄: 钩子跑在原函数体之前, 而原函数紧接着就调
     * ExtractGadgetAt, 所以此刻改宽度能立刻影响这一次命中。
     * (FM_Poll 里做无效 —— 那里排在 Input 之后, 引擎每帧从 ViewBounds 重设。) */
    FM_TrimViewGadget();
}

/* ---------------- 地图格范围 (格) + 原生视口滚动 ----------------
 * 小地图点击要把"雷达像素"换算成"地图格", 再让引擎把视口对准那一格。
 * 格范围用 Fm_ScanTacticalMapRect 只读扫描 TacticalClass 得到 (见其注释)。
 *
 * 滚动: RadarClass::ScrollTo (0x653F70) —— 就三条指令的包装:
 *       mov eax,[esp+4] / mov ecx,[0x887324] / push eax / call 0x6D6070 / ret 4
 *    0x6D6070 = TacticalClass::SetTacticalPosition(CoordStruct*):
 *    取 coord 的格, 减去半个视口, 越界时退回夹好的值, 写进视口并置 Redrawing。
 *
 * ★ 引擎自带的 RadarClass::Action (0x6539D0) 里就有"点雷达跳视口"这一段,
 *   但它是死代码: 入口要求 [0x880C98]==1 && [0x880C94]==1, 而这两个全局
 *   全二进制**只有读没有写** (gamemd/Ares 都搜过), .data 初值 0 -> 恒 bail。
 *   所以这条链在原版里也不通, 只能自己接。
 *
 * 联机安全: 只动本地摄像机 (TacticalClass 视口原点), 不写任何模拟对象、
 * 不发事件。逐客户端各自决定看哪里。 */
#define ADDR_SCROLL_TO       0x00653F70u   /* RadarClass::ScrollTo(CoordStruct*) */
#define ADDR_VISIBLE_MIN_X   0x00880C84u   /* 单例+0x149C 可见区最小格 X */
#define ADDR_VISIBLE_MIN_Y   0x00880C88u   /* 单例+0x14A0 可见区最小格 Y */
#define ADDR_VISIBLE_MAX_X   0x00880C8Cu   /* 单例+0x14A4 可见区最大格 X */
#define ADDR_VISIBLE_MAX_Y   0x00880C90u   /* 单例+0x14A8 可见区最大格 Y */

typedef struct { int X, Y, Z; } CoordStruct;

typedef void (__thiscall *FnScrollTo)(void* self, const CoordStruct* c);

static RectangleStruct s_MapRect;
static int s_MapRectOk = 0;
static int s_MapLogged = 0;

/* ---------------- 地图格矩形: 从 TacticalClass 只读扫描 ----------------
 * ★ 不要钩 MapClass::CreateEmptyMap (0x653F50) 拿入参: 试过两次都在
 *   "我们的代码跑完、跳板返回之后" 崩 (0xC0000005), 逐条核对过落点、跳板、
 *   栈收支都对, 定位不到就弃用了。
 *   为一个矩形去钩引擎内部函数, 收益/风险比太差 —— 优先只读路径。
 *
 * 改用无钩扫描: MapRect / MaxWidth / MaxHeight 都在 TacticalClass 的
 * 0x0E0..0x200 这一片, 但确切偏移没核实过 (YRpp 头文件那段是占位名), 所以
 * **不写死偏移**, 逐 4 字节试, 用两条判据筛:
 *   1) 宽高像地图尺寸: 16..512 格, X/Y 在 -64..512;
 *   2) 可见区格坐标 `[0x880C84..90]` 必须落在里面。两种语义都试:
 *      (a) {minX,minY,maxX,maxY} —— 四个都在 [X,X+W) x [Y,Y+H)
 *      (b) {X,Y,W,H}          —— X..X+W / Y..Y+H 落在地图内
 * 命中即锁定并停止扫描。区间已判过 IsPlausiblePtr, 只读, 联机安全。 */
#define ADDR_TACTICAL        0x00887324u

static int Fm_VisFits(const RectangleStruct* r)
{
    int a = *(int*)(size_t)ADDR_VISIBLE_MIN_X;
    int b = *(int*)(size_t)ADDR_VISIBLE_MIN_Y;
    int c = *(int*)(size_t)ADDR_VISIBLE_MAX_X;
    int d = *(int*)(size_t)ADDR_VISIBLE_MAX_Y;
    int x0 = r->X, y0 = r->Y, x1 = r->X + r->Width, y1 = r->Y + r->Height;
    /* 语义 (a): {minX,minY,maxX,maxY} 四个都必须在地图内 */
    if (a >= x0 && a < x1 && b >= y0 && b < y1
        && c >= x0 && c <= x1 && d >= y0 && d <= y1) return 1;
    /* 语义 (b): {X,Y,W,H} 的可见区也必须在地图内 */
    if (a >= x0 && a + c <= x1 && b >= y0 && b + d <= y1) return 1;
    return 0;
}

static int Fm_ScanTacticalMapRect(void)
{
    void* tc = *(void**)(size_t)ADDR_TACTICAL;
    char* base;
    RectangleStruct r;
    int off;
    if (!tc || !IsPlausiblePtr(tc)) return 0;
    base = (char*)tc;
    for (off = 0x0E0; off <= 0x200; off += 4)
    {
        r = *(RectangleStruct*)(base + off);
        if (r.Width < 16 || r.Width > 512) continue;
        if (r.Height < 16 || r.Height > 512) continue;
        if (r.X < -64 || r.X > 512) continue;
        if (r.Y < -64 || r.Y > 512) continue;
        if (!Fm_VisFits(&r)) continue;
        s_MapRect = r;
        s_MapRectOk = 1;
        if (!s_MapLogged)
        {
            char b1[16], b2[16], buf[192];
            unsigned q = 0;
            s_MapLogged = 1;
            q = AppendCStr(buf, q, "fullmap: map rect scan off=", sizeof(buf));
            UtoA((unsigned)off, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " ", sizeof(buf));
            UtoA((unsigned)r.X, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)r.Y, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)r.Width, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, "x", sizeof(buf));
            UtoA((unsigned)r.Height, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            LogLine(buf);
        }
        return 1;
    }
    return 0;
}

/* 雷达矩形内的相对像素 -> 地图格。整块雷达图就是整张地图的等比缩略图,
 * 所以按比例线性映射; 结果夹回地图范围内。 */
static int Fm_RadarPixelToCell(int px, int py, const RectangleStruct* radar,
    int* outX, int* outY)
{
    int lx, ly, cx, cy;
    if (!s_MapRectOk && !Fm_ScanTacticalMapRect()) return 0;
    if (radar->Width < 8 || radar->Height < 8) return 0;
    lx = px - radar->X;
    ly = py - radar->Y;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx > radar->Width - 1) lx = radar->Width - 1;
    if (ly > radar->Height - 1) ly = radar->Height - 1;
    cx = s_MapRect.X + (lx * s_MapRect.Width) / radar->Width;
    cy = s_MapRect.Y + (ly * s_MapRect.Height) / radar->Height;
    if (cx < s_MapRect.X) cx = s_MapRect.X;
    if (cy < s_MapRect.Y) cy = s_MapRect.Y;
    if (cx > s_MapRect.X + s_MapRect.Width - 1) cx = s_MapRect.X + s_MapRect.Width - 1;
    if (cy > s_MapRect.Y + s_MapRect.Height - 1) cy = s_MapRect.Y + s_MapRect.Height - 1;
    *outX = cx;
    *outY = cy;
    return 1;
}

/* ---------------- 小地图点击: 自己接手 + 吞掉穿透 ----------------
 * 引擎把这一块当普通地图点击 (框单位/打靶/下单), 所以在
 * DisplayClass::ProcessClickCoords 里挂一道门: 落点在那块小地图矩形内就自己
 * 完成视口跳转, 再跳 0x6925FC (原函数的"未处理"出口, pop edi/esi/ebp /
 * xor al,al / pop ebx / add esp,0x18 / ret 0x18, 栈形在该落点完全对齐)
 * 把这次点击吞掉。做法与 SWSBar 的 SWS_ClickGate 同一套路。
 *
 * 左键才处理: 右键是取消/强制移动。
 * "一次按下只处理一次" 不再用 FM_Poll 采边沿 —— 点击事件与 prepare
 * (0x4F42F0 一帧能被调好几次) 的先后关系不可控, 边沿可能在 press 事件
 * **之后**才被采到, 于是门永远错过按下那一帧。改成门里直接读按键实时状态
 * + 一个"本次按下已处理"标记, 松开时清标记。
 *
 * 命中范围是**整块保留区** (资金条 + 小地图 + 那两个按钮画在一起的那一块):
 * 这一块是我们画上去的 UI, 点击不该再落到地图上 (框单位/打靶/下单)。
 * 与 SWSBar 的 SWS_ClickGate 同一路子 —— 那里吞没是因为"侧栏盖住的地图那块
 * 点不动", 这里吞没是因为反过来: 这一块是面板, 点它不该动地图。
 * 吞没只发生在 ProcessClickCoords 里, 侧栏自己的 widget 分派 (0x6D0680,
 * 在 0x6922E0 里排在 0x692F30 之后、无条件执行) 不受影响, 所以顶部两个
 * 按钮的原生响应照旧。
 *
 * 联机安全: 只动本地摄像机 (TacticalClass 视口原点), 不写任何模拟对象、
 * 不发事件。 */
unsigned FM_RadarClickGate(void* regs)
{
    RectangleStruct radar, keep;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int winW, colX, mx, my, kx, ky, cx, cy, lb;

    (void)regs;
    if (!s_Enable || !s_KeepRadar || !s_RadarClick) return 0;
    if (!g_Log) return 0;
    lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    if (!lb) { s_LbPress = 0; return 0; }  /* 松开: 下一次按下重新允许处理 */
    if (s_LbPress) return 0;              /* 本次按下已处理过 (按住拖动不重复) */
    s_LbPress = 1;
    if (!Fm_RadarRect(&radar)) return 0;
    if (!Fm_KeepRect(&keep)) return 0;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    winW = ((int*)(size_t)ADDR_WINDOWBOUNDS)[2];
    if (winW < 640 || winW > 8192) return 0;
    colX = sb->X;
    if (colX < 0 || colX > winW) return 0;

    /* 保留区整块吞掉: 这一块是面板, 点击不该穿透到地图 */
    kx = colX + keep.X;
    ky = keep.Y;
    if (mx < kx || mx >= kx + keep.Width) return 0;
    if (my < ky || my >= ky + keep.Height) return 0;

    /* 小地图本体才做视口跳转; 点那两个按钮 / 资金条只吞不跳 */
    if (mx < colX + radar.X || mx >= colX + radar.X + radar.Width)
        return (unsigned)ADDR_CLICKGATE_SKIP;
    if (my < radar.Y || my >= radar.Y + radar.Height)
        return (unsigned)ADDR_CLICKGATE_SKIP;

    if (!Fm_RadarPixelToCell(mx - colX, my, &radar, &cx, &cy))
    {
        if (!s_ClickLogged)
        {
            s_ClickLogged = 1;
            LogLine("fullmap: radar-click ignored (no map rect yet)");
        }
        return (unsigned)ADDR_CLICKGATE_SKIP;
    }

    {
        CoordStruct c;
        FnScrollTo fn = (FnScrollTo)(size_t)ADDR_SCROLL_TO;
        c.X = cx * 256 + 0x80;
        c.Y = cy * 256 + 0x80;
        c.Z = 0;
        if (fn) fn(NULL, &c);
        if (!s_JumpLogged)
        {
            char b1[16], b2[16], buf[192];
            unsigned q = 0;
            s_JumpLogged = 1;
            q = AppendCStr(buf, q, "fullmap: radar-click px=", sizeof(buf));
            UtoA((unsigned)mx, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)my, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " rect=", sizeof(buf));
            UtoA((unsigned)(colX + radar.X), b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)radar.Y, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)radar.Width, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, "x", sizeof(buf));
            UtoA((unsigned)radar.Height, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, " -> cell=", sizeof(buf));
            UtoA((unsigned)cx, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, ",", sizeof(buf));
            UtoA((unsigned)cy, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, " colX=", sizeof(buf));
            UtoA((unsigned)colX, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            LogLine(buf);
        }
    }
    return (unsigned)ADDR_CLICKGATE_SKIP;
}

/* Syringe 入口 (cdecl, regs 未用; 返回值 = 继续执行的地址, 0=走原版) */
extern "C" __declspec(dllexport) unsigned __cdecl FullMap_RadarClickGate(void* regs)
{
    return FM_RadarClickGate(regs);
}
