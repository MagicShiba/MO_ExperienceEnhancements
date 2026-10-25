/*
 * FullMap.cpp - 隐藏原版建造/生产侧栏 (页签+cameos+电力条), 地图铺满窗口宽度,
 * 小地图 (RadarClass) 仍留在原来那一角 (NOD=右上 / GDI=左上)。
 * F11 三态: 0=完全显示 1=只显示小地图和顶部按钮 2=全屏 (侧栏一点不显示)。
 * 地址表/根因/不变量见 notes/隐藏侧栏.md。
 *
 * 三条结构前提 (反汇编实证):
 *   - 视口宽度纯函数 0x72AD20 输出 {0,0,客户宽-168,客户高-32}; Composite 的建面
 *     矩形与 Set_View_Dimensions 的入参都是它的输出, 所以改这一个函数, 面和视口
 *     一起变全宽。
 *   - 面板与小地图在同一块离屏面 Sidebar (0x887300) 上, 每帧最后由
 *     BlitSidebar (0x6A70E0) 整条 blit 到 Primary ⇒ "不画面板"保不住小地图,
 *     必须接管那次 blit。
 *   - 地图是**增量**画进地图面 (0x8872FC) 的, 只有标脏的格子才重画; 换视口宽度
 *     不会标脏 ⇒ 必须自己催 (Fm_DirtyRedraw)。
 *
 * 联机安全: 只写视口矩形/渲染面/gadget 标志, 全是本地渲染与输入状态, 每客户端
 * 按自己窗口尺寸算, 不碰任何模拟内存, 不发事件。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "FullMap.h"
#include "ScaleBar.h"
#include "MOExperience.h"

#define FM_SECTION "Sidebar"

/* ---------------- 地址 (反汇编实证) ---------------- */

#define ADDR_SIDEBARBOUNDS    0x00886F90u
#define ADDR_WINDOWBOUNDS     0x00886FB0u
#define ADDR_ISNOD_SIDEBAR    0x00A8EB7Cu   /* byte: 1=侧栏在右(苏联), 0=在左(盟军) */
#define ADDR_ARMAGEDDON       0x00A8ED6Bu   /* byte: 末日模式 (视口 X 偏移也不加 168) */
#define ADDR_RADAR_RECT       0x008809F4u   /* RadarClass+0x120C: 雷达绘制矩形(侧栏面内) */
#define ADDR_SIDEBAR_DIRTY    0x00B0B518u   /* byte: TabClass::Draw 每帧置 1 */
#define ADDR_SIDEBAR_TOUCH    0x00B0B519u   /* byte */
#define ADDR_SIDEBAR_REDRAW   0x00884B90u   /* byte (this+0x53A8) */
#define ADDR_SIDEBAR_NEEDRD   0x00884B8Eu   /* byte (单例+0x53A6) */
#define ADDR_SIDEBAR_BGNEEDRD 0x00884B8Fu   /* byte (this+0x53A7) */
#define ADDR_GAME_HWND        0x00B73550u
/* Set_View_Dimensions 按 ViewBounds 重建的两个"可见区裁剪器"。两者结构相同:
 * +0..+0x0F 就是那个矩形, +0x28/+0x2C 是 W/H。鼠标→格换算与 Render 的
 * Clip() 都走它们 —— 与 ViewBounds 不同源就会判错。*/
#define ADDR_VIEWCLIP_A       0x00887644u
#define ADDR_VIEWCLIP_B       0x0087E8A4u

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

/* 把五处"取 ViewBounds.Width 当侧栏横向偏移"的指令操作数改指向
 * DSurface::SidebarBounds.X —— 我们每循环把它维护成侧栏列在屏幕上的起点
 * (苏联=视口右边界-168, 盟军=0), 正好是小地图/按钮所在那一列的起点。
 * 只改 imm32, 不加指令、不动寄存器。少改一处就有一处把判定落到屏外:
 *  a) 0x478BD7 MouseClass 鼠标钳制路由
 *  b) 0x65386B 顶部两个小按钮的光标形状区命中
 *  c) 0x654332 RadarClass 摆三个 gadget 的 X (0xB04978/0xB04910/0xB04A10)。
 *     位置只在 DisplayClass::InitGUI 里算一次 ⇒ 运行时改 gadget 矩形撑不过
 *     下一帧/换分辨率, 必须改这处 imm32。
 *  d) 0x653A3F RadarClass::Action 把鼠标 X 换算成侧栏面内坐标
 *  e) 0x6D06C7 侧栏顶部一簇的光标形状判定 (纯外观)
 * 刻意不改: 0x653D92 (ViewW/9 = 视口能显示多少格, 原版 1752/9=194 已远大于
 * 任何地图宽度)。 */
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

#define VT_COPY_FROM_PART 0x08   /* DSurface::CopyFromPart */
#define VT_FILL_RECT      0x14   /* DSurface::FillRect */
#define WW_VT_FIX_CURSOR  0x3C   /* WWMouseClass: 把光标精灵烙到指定面 (第二实参 = isNOD/miniMap 标志) */

/* ---------------- 引擎函数原型 ---------------- */

typedef int (__thiscall *FnCopyFromPart)(void* self,
    RectangleStruct* pDstRect, void* pSrcSurface,
    RectangleStruct* pSrcRect, int bUnk1, int bUnk2);
typedef void (__thiscall *FnMouseFixSurface)(void* mouse, void* surface, int isNodMiniMap);
typedef int (__thiscall *FnFillRect)(void* self, RectangleStruct* rect, unsigned color);

/* ---------------- 显示模式 (F11 循环切换) ----------------
 * 三态循环, 只改"我们接管多少" + 视口宽度:
 *   FULL  (0, 初始态) 完全显示: 视口收回 clientW-168, 整条侧栏面上屏,
 *                             gadget 保持可用, 点击让位给引擎 widget 分派
 *   RADAR (1) 只显示小地图和顶部两个按钮 (视口全宽, 侧栏列归地图)
 *   BARE  (2) 全屏: 侧栏一点不显示 (小地图也不画), 点击全给地图
 * 字节补丁不反向撤 —— 撤了还得重算视口并同步 view_bound, 风险远大于收益。
 * "完全显示"就是把视口收回原版宽度 + 把整条侧栏面 blit 出来, 随时能切回去。 */
#define FM_MODE_FULL   0
#define FM_MODE_BARE   2

#define ADDR_FRAME_COUNT        0x00A8ED84u

/* ---------------- 配置 ---------------- */

static int s_Init = 0;
static int s_Mode = FM_MODE_FULL;
static OU_Hotkey s_ToggleHk;   /* [Sidebar] ToggleKey (默认 F11) */
static int s_Enable = 0;
static int s_LockedFull = 0;   /* HideEnable=0 但缩放开着: 锁定完全显示, 不许 F11 切换 */
/* 菜单选中"禁用" (HideEnable=0)。模块内部仍按完全显示跑 (缩放开着时视口补偿不能停),
 * 只是不响应切换键; 下次启动按 ini 真正关掉。 */
static int s_MenuOff = 0;
/* 启动即禁用时菜单选了启用: 全宽视口补丁没装, 运行期无法安全接管 ⇒ 记下, 重启生效。 */
static int s_WantState = -1;
static int s_PatchesInstalled = 0;
static int s_ExtraH = 5;
/* 左键"本次按下已处理"标记 —— 只由点击门维护 (读 GetAsyncKeyState, 松开时清) */
static int s_LbPress = 0;
static int s_Warned = 0;
static int s_SurfWarned = 0;
/* 缩放 (ScaleBar): A = 满视图区, R = 引擎实际视口 (A 居中的 1/m)。每循环刷新。 */
static RectangleStruct s_ViewA = { 0, 0, 640, 448 };
static RectangleStruct s_ViewR = { 0, 0, 640, 448 };
static int s_ZoomLast = 1;
static int s_ZoomChanged = 0;
/* 换视口宽度后还要催地图重画几帧 (见 Fm_DirtyRedraw 的注释) */
static int s_DirtyFrames = 0;


void FM_EnsureInit(void)
{
    char b1[16], buf[128];
    unsigned p;
    if (s_Init) return;
    s_Init = 1;
    GtEnsureLog();
    if (!g_Log) return;
    s_Enable = GetPrivateProfileIntA(FM_SECTION, "HideEnable", 1, g_IniPath) ? 1 : 0;
    s_MenuOff = !s_Enable;
    s_ExtraH = GetPrivateProfileIntA(FM_SECTION, "ExtraH", 5, g_IniPath);
    if (s_ExtraH < 0) s_ExtraH = 0;
    if (s_ExtraH > 64) s_ExtraH = 64;
    OU_ParseHotkey("F11", &s_ToggleHk);
    OU_ReadHotkey(FM_SECTION, "ToggleKey", &s_ToggleHk);
    s_Mode = GetPrivateProfileIntA(FM_SECTION, "Mode", FM_MODE_FULL, g_IniPath);
    if (s_Mode < FM_MODE_FULL || s_Mode > FM_MODE_BARE) s_Mode = FM_MODE_FULL;
    /* 缩放开着时锁定完全显示跑全套补偿, 不许 F11 切走。
     * 否则缩放改了 ViewBounds 而侧栏仍走引擎原生, 整条侧栏错位闪烁。 */
    if (!s_Enable
        && GetPrivateProfileIntA("BottomBar", "ScaleEnable", 0, g_IniPath))
    {
        s_Enable = 1;
        s_LockedFull = 1;
        s_Mode = FM_MODE_FULL;
    }
    if (!s_Enable) return;
    p = 0;
    p = AppendCStr(buf, p, "fullmap: en=", sizeof(buf));
    UtoA((unsigned)s_Enable, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " mode=", sizeof(buf));
    UtoA((unsigned)s_Mode, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    if (s_LockedFull) p = AppendCStr(buf, p, " locked-full", sizeof(buf));
    LogLine(buf);
}

/* 三态的固定行为 (不再有细化开关):
 *   态0 FULL  一概不接管, 只把视口宽度改回原版值 (Fm_ApplyViewWidth), 于是引擎
 *               自己的几何判定/整条侧栏 blit/侧栏列起点/视口 gadget 全部回到原版。
 *               只 blit 整条侧栏面但视口仍全宽是不够的: 那一列几何会全错 168。
 *   态1 RADAR 保留小地图+顶部两按钮; 摘页签/cameo; 收窄视口 gadget; 小地图可点。
 *   态2 BARE  侧栏一点不显示, 点击全给地图。 */
static int Fm_ModeFull(void)   { return s_Mode == FM_MODE_FULL; }
static int Fm_ModeBare(void)   { return s_Mode == FM_MODE_BARE; }
/* 我们要不要呈现侧栏面的一块 (态0 = 整条, 态1 = 顶部块, 态2 = 一块都不) */
static int Fm_ModeKeep(void)
{
    return !Fm_ModeBare();            /* FULL = 整条, RADAR = 顶部块 */
}
/* 侧栏整列在屏幕上算不算"不存在": 不存在时它要被推出视口, 视口命中区恢复整宽,
 * 鼠标路由阈值也要推到恒不成立 (见 Fm_SyncColumn)。 */
static int Fm_SidebarGone(void)  { return Fm_ModeBare(); }
/* 页签/cameo 之类的侧栏 gadget 要不要摘掉 (摘掉后点击落到地图上) */
static int Fm_ModeDisarm(void)
{
    return !Fm_ModeFull();            /* 侧栏不呈现时必须摘, 否则隐形按钮吃地图点击 */
}
/* 小地图点击门 (自己跳视口 + 吞穿透)。态0 让位给引擎 widget 分派, 态2 没有面板
 * 可点, 只有态1 开这道门。**但缩放档例外**: 缩放时 ViewBounds=R, 小地图那块在
 * 引擎眼里已经"不在视口里", 原生 RadarClass::Action 那条 gadget 链也不可靠
 * ⇒ 缩放档一律自己接, 不看模式。1x 且非缩小档时逐位沿用原版 (Fm_ModeGate 为 0)。 */
static int Fm_ModeGate(void)
{
    if (Fm_ModeBare()) return 0;
    if (Fm_ModeFull()) return SB_Zoomed() ? 1 : 0;
    return 1;
}
/* 视口 gadget 要不要横向收窄成"视口减去侧栏列" */
static int Fm_ModeTrim(void)
{
    /* 态0 视口本来就不含侧栏列; 态2 整窗都是地图, 命中区就该是整宽 */
    return Fm_ModeFull() ? 0 : !Fm_ModeBare();
}

/* ---------------- 要保留的那一块: 侧栏面顶部整块 ----------------
 * SidebarBounds.Y (实测 158) 正好是"顶上一簇"与"面板"的分界:
 *   y <  158 : 资金条 + 小地图 + 它的边框 + 右边两个按钮   <- 保留
 *   y >= 158 : 页签 + cameo + 底部电力条                   <- 丢掉
 * 所以直接 blit {0,0,SidebarBounds.Width,SidebarBounds.Y}, 分界线是引擎自己
 * 定的, 不用猜。FM_DrawRadarTo (画进 Composite) 用的是同一个矩形。 */
static int Fm_KeepRect(RectangleStruct* out, int* swOut, int* shOut)
{
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    void* strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    int sw = 168, sh = 1080, k;
    if (strip) {
        int w = GtSurfGet(strip, GtVT_GET_WIDTH);
        int h = GtSurfGet(strip, GtVT_GET_HEIGHT);
        if (w > 0 && h > 0) { sw = w; sh = h; }
    }
    /* 态2 一块都不保留 (小地图也不画); 态0 是整条侧栏面 */
    if (!Fm_ModeKeep()) return 0;
    if (Fm_ModeFull())
    {
        if (sw < 32 || sh < 32) return 0;
        out->X = 0; out->Y = 0; out->Width = sw; out->Height = sh;
        if (swOut) *swOut = sw;
        if (shOut) *shOut = sh;
        return 1;
    }
    k = sb->Y;
    if (k < 32 || k > 512) k = 158;
    k += s_ExtraH;                        /* 用户要求: 再多露一点 */
    if (k > sh) k = sh;
    if (k < 32) return 0;
    out->X = 0; out->Y = 0; out->Width = sw; out->Height = k;
    if (swOut) *swOut = sw;
    if (shOut) *shOut = sh;
    return 1;
}
/* ---------------- 侧栏"顶部那一块" (资金条 + 小地图 + 顶部两个按钮) -------------
 * Fm_KeepRect 在态0 返回的是**整条**侧栏面 (那是"呈现哪一块"的问题)。但
 * "点击该不该落到地图上"问的是另一件事: 只有顶部那一块点了没意义 (纯 UI),
 * 页签/cameo 点了是要下单的, 必须放行给引擎 gadget 链。所以单独一个函数,
 * 三态一致 = {0,0,列宽,SidebarBounds.Y} (侧栏面内坐标)。
 * ⚠️ **不加 s_ExtraH**: 那是"呈现上再多露几像素"的偏好, 不是点击语义。
 * 加上去会把页签那一行最上面 5px 也吞掉, 页签就难点了。
 * 态2 (全屏) 侧栏一块都不显示 ⇒ 返回 0。*/
static int Fm_KeepTopRect(RectangleStruct* out)
{
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    void* strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    int sw = 168, sh = 1080, k;
    if (!out) return 0;
    if (Fm_ModeBare()) return 0;
    if (strip && IsPlausiblePtr(strip))
    {
        int w = GtSurfGet(strip, GtVT_GET_WIDTH);
        int h = GtSurfGet(strip, GtVT_GET_HEIGHT);
        if (w > 0 && h > 0) { sw = w; sh = h; }
    }
    if (sw < 32 || sh < 32) return 0;
    k = sb->Y;
    if (k < 32 || k > 512) k = 158;
    if (k > sh) k = sh;
    if (k < 32) return 0;
    out->X = 0; out->Y = 0; out->Width = sw; out->Height = k;
    return 1;
}

static int Fm_RadarRect(RectangleStruct* out);   /* 定义在下面 §小地图矩形 */
static int Fm_ColumnX(int bare);                   /* 定义在下面 §侧栏列起点 */

/* 屏幕物理坐标是否落在侧栏"顶部那一块"(资金条 + 小地图 + 顶部两个按钮)。
 * 缩放档下这一块在引擎眼里不在视口里 (ViewBounds=R), 所以 ScaleBar 的
 * ProcessClickCoords 拒点门要拿它**放行**: 那一小块是我们画的 UI, 点击由
 * FM_RadarClickGate 吞掉。判据用物理坐标 (映射值已钳进 R, 看不出界)。
 * 列起点用 `Fm_ColumnX(0)` 现算, 不读 SidebarBounds.X (那个全局缩小档下会被
 * 引擎按 R 重算, 瞬时值不可信)。*/
static int FM_PtInKeepRegion(int px, int py)
{
    RectangleStruct radar, top;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int colX;
    if (!s_Enable) return 0;
    /* ★ 武装判据与 FM_RadarClickGate 逐条同源 —— 两边任何一条不一致, 就会出现
     * "拒点门放行了但点击门不接" (点成对某个地图点的下令, 比点不动更糟)。*/
    if (!Fm_ModeKeep() || !Fm_ModeGate()) return 0;
    if (!Fm_RadarRect(&radar)) return 0;      /* 顺带验雷达矩形可用 */
    if (!Fm_KeepTopRect(&top)) return 0;
    colX = Fm_ColumnX(0);
    if (colX < 0) colX = sb->X;
    if (colX < 0 || colX > 8192) return 0;
    if (px < colX + top.X || px >= colX + top.X + top.Width) return 0;
    if (py < top.Y || py >= top.Y + top.Height) return 0;
    return 1;
}

/* 侧栏"顶部那一块"(资金条 + 小地图 + 顶部两个按钮) 在**屏幕**上的矩形。
 * 与 FM_PtInKeepRegion / FM_RadarClickGate 同源 (Fm_KeepTopRect + Fm_ColumnX)。
 * 小窗 (PiP) 用它避让: 态 2 (只留顶部块) 时视口已铺到屏幕右缘, 这一块却仍压在
 * 画面上, 不躲就会盖住小地图。态 3 (全隐藏) 返回 0 —— 那时整块都没有, 右上角
 * 是干净的地图区, 小窗可以随便放。 */
int FM_KeepTopRectScreen(RectangleStruct* out)
{
    RectangleStruct top;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int colX;
    if (!Fm_KeepTopRect(&top)) return 0;
    colX = Fm_ColumnX(0);
    if (colX < 0) colX = sb->X;
    if (colX < 0 || colX > 8192) return 0;
    out->X = colX + top.X;
    out->Y = top.Y;
    out->Width = top.Width;
    out->Height = top.Height;
    return (out->Width > 0 && out->Height > 0);
}

/* 视口拒点门要放行吗 (ScaleBar 的 ClickCoordsReject 调)。判据是**物理**鼠标位置:
 * 传进来的 pt 是折算出来的, 已被移到 R 之外, 反算回去才无歧义。*/
int FM_ShouldDeferClick(void)
{
    int mx = 0, my = 0;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    return FM_PtInKeepRegion(mx, my);
}
/* ---------------- 把侧栏面里除保留区以外的部分抹掉 ----------------
 * 面板是**被画进 Composite** 的 (Render 之后 DSurface::Temp 就是 Composite),
 * 而 BlitSidebar 的两个调用点都在补丁内, 追不出是谁画的 ⇒ 从源头堵: 每次接管
 * blit 时把侧栏面里保留区以外的部分全抹黑, 无论哪条路去呈这块面都最多呈出
 * 保留的那一块。态0 不抹 (会把引擎刚画好的面板擦掉); 态2 连小地图也不要,
 * keep 传 NULL = 整块抹。 */
static void Fm_ClearStripExceptRadar(const RectangleStruct* keep, int sw, int sh)
{
    void* strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    void** vt;
    FnFillRect fn;
    RectangleStruct r;
    int top, bot;
    /* 完全显示: 不抹任何东西, 否则会把引擎刚画好的面板擦掉 */
    if (Fm_ModeFull()) return;
    if (!strip || !IsPlausiblePtr(strip)) return;
    if (sw <= 0 || sh <= 0) { sw = 168; sh = 1080; }
    vt = *(void***)(size_t)strip;
    fn = (FnFillRect)vt[VT_FILL_RECT >> 2];
    if (!fn) return;
    if (!keep) {                          /* 全屏: 整块抹黑 */
        r.X = 0; r.Y = 0; r.Width = sw; r.Height = sh;
        fn(strip, &r, 0);
        return;
    }
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

/* 每帧只抹一次。同一帧里两个调用点 (条带上屏 / 小地图自绘) 算出的 keep 完全
 * 相同, 而两次之间没有任何代码写条带 (小地图自绘之后才是 PowerDisplay_DrawSb),
 * 所以第二次是纯浪费 —— 态1 下那是每帧最多 4 次整幅 FillRect。 */
static int s_ClearFrame = -1;
static void Fm_ClearStripOnce(const RectangleStruct* keep, int sw, int sh)
{
    int fr = *(int*)(size_t)ADDR_FRAME_COUNT;
    if (fr == s_ClearFrame) return;
    s_ClearFrame = fr;
    {
        long long t = Perf_Now();
        Fm_ClearStripExceptRadar(keep, sw, sh);
        Perf_Add(PF_H_CLAER, t);
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
 * 0x72AD20 是纯函数, 输出
 *   { X:(非NOD且非末日)?168:0, Y:0, W:WindowBounds.Width-168, H:Height-32 }
 * Composite 的建面矩形与 Set_View_Dimensions 的入参**都是它的输出**, 所以改这
 * 一个函数, 面和视口一起变全宽。两处字节补丁 (等长改写):
 *   0x72AD69 `sub edx,0xA8` 的 imm32 -> 0   : W 不再扣 168
 *   0x72AD60 `mov ecx,0xA8`     -> 5x nop   : 盟军侧栏那 168 偏移也去掉
 * 光改 0x72AD20 不够 —— `GScreenClass::CreateAll` (0x533FD0) 第三个调用点
 *   (**新开一局** 0x6BDE14) 自己算宽, 0x6BDDC8 的 `sub eax,edx` 要 nop 掉。
 *   只能 nop 不能换 `sub eax,eax` (2B C0): 后者让宽恒 0, CreateAll 里两处
 *   `test W; jle` 会被跳过 ⇒ [0x88731C]/[0x8872FC] 保持 NULL ⇒ 新游戏必崩。
 *   也不要去钩 CreateAll 改建面矩形 (那条路未判空)。*/

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

/* 把 `call 0x72AD20` (E8 + rel32, 5 字节) 改指向我们自己的函数。等长改写,
 * 调用点的寄存器收支完全不变 (只用 EAX 拿返回值, ECX 传入参)。 */
static int Fm_PatchCallTo(unsigned at, void* target)
{
    unsigned char* p = (unsigned char*)(size_t)at;
    unsigned char buf[5];
    DWORD old;
    int rel, i;
    if (p[0] != 0xE8) return 0;
    rel = *(int*)(void*)(p + 1);
    if ((unsigned)(at + 5 + rel) != ADDR_VIEWRECT) return 0;   /* 落点必须是它 */
    rel = (int)((unsigned)(size_t)target - (at + 5));
    buf[0] = 0xE8;
    buf[1] = (unsigned char)(rel & 0xFF);
    buf[2] = (unsigned char)((rel >> 8) & 0xFF);
    buf[3] = (unsigned char)((rel >> 16) & 0xFF);
    buf[4] = (unsigned char)((rel >> 24) & 0xFF);
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (i = 0; i < 5; i++) p[i] = buf[i];
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
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

/* 前置声明: 视口宽度的动态切换在文件后面实现, 但 FM_Install 就要用 */
static int Fm_ViewPatchSet(int wide);
static int Fm_ViewPatchDetect(void);
static int s_ViewPatched = -1;   /* -1=未探测, -2=字节不符不碰, 0=原版宽, 1=全宽 */
static RectangleStruct* __fastcall Fm_ViewRectStub(RectangleStruct* out);

/* 宿主映像在不在 (DllMain 调用时 gamemd 可能还没映射, 那就一个字节都不写)。
 * 判据: 0x400000 已提交且是 PE 头, 且模块文件名是
 * gamemd.exe / gamemd-spawn.exe / ra2md.exe。 */
static int Fm_HostImageOk(void)
{
    HMODULE h = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    char path[MAX_PATH];
    char* base;
    DWORD n;
    int ok;
    if (!VirtualQuery((void*)(size_t)0x00400000u, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect == PAGE_NOACCESS || mbi.Protect == PAGE_GUARD) return 0;
    if (*(const unsigned short*)(size_t)0x00400000u != 0x5A4Du) return 0;   /* 'MZ' */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)(size_t)0x00400000u, &h) || !h) return 0;
    if ((unsigned)(size_t)h != 0x00400000u) return 0;
    n = GetModuleFileNameA(h, path, sizeof(path));
    if (n == 0 || n >= sizeof(path)) return 0;
    base = path;
    for (n = 0; path[n]; n++) if (path[n] == '\\' || path[n] == '/') base = path + n + 1;
    ok = (lstrcmpiA(base, "gamemd.exe") == 0) ||
         (lstrcmpiA(base, "gamemd-spawn.exe") == 0) ||
         (lstrcmpiA(base, "ra2md.exe") == 0);
    return ok;
}

/* "n/N sites" 形式的日志 */
static const char* Fm_SiteBad2(const char* name, int n, int total)
{
    static char buf[96];
    char b1[16], b2[16];
    unsigned q = 0;
    q = AppendCStr(buf, q, "fullmap: ", sizeof(buf));
    q = AppendCStr(buf, q, name, sizeof(buf));
    q = AppendCStr(buf, q, " ON ", sizeof(buf));
    UtoA((unsigned)n, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, "/", sizeof(buf));
    UtoA((unsigned)total, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
    return buf;
}

void FM_Install(void)
{
    static const unsigned char kWantSur[2] = { 0x2B, 0xC2 };          /* sub eax,edx */
    static const unsigned char kToSur[2]   = { 0x90, 0x90 };          /* nop nop (不能 2B C0, 见上) */
    FM_EnsureInit();
    if (!g_Log) return;
    if (!s_Enable)
    {
        LogLine("fullmap: view-rect patch OFF (Enable=0)");
        return;
    }
    if (!Fm_HostImageOk())
    {
        s_ViewPatched = -2;
        LogLine("fullmap: gamemd image not mapped yet, patch skipped");
        return;
    }
    /* 从当前字节判态, 不许假定自己装过 (初始态本来就是原版态)。 */
    s_ViewPatched = Fm_ViewPatchDetect();
    if (s_ViewPatched == -2)
        LogLine("fullmap: viewrect bytes are neither vanilla nor wide, patch OFF");
    else Fm_ViewPatchSet(!Fm_ModeFull());
    /* 建面永远全宽: CreateAll 的两个 viewRect 调用点改指向 Fm_ViewRectStub。
     * 这样态0 (视口回原版宽度) 期间的建面/改分辨率也不会把渲染面缩窄。 */
    {
        int n = 0;
        if (Fm_PatchCallTo(0x00560F9Cu, (void*)Fm_ViewRectStub)) n++;
        if (Fm_PatchCallTo(0x0067E776u, (void*)Fm_ViewRectStub)) n++;
        LogLine(Fm_SiteBad2("createall-rect", n, 2));
    }
    /* 新开局那条 CreateAll 自己扣 168, 不改则右侧 168px 黑底。 */
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
    /* 五处"侧栏横向偏移"取数改指 SidebarBounds.X: 让小地图/顶上一簇按钮既画在
     * 屏幕内、又能收点击。原生 `RadarClass::Action` (0x6539D0) 全靠这个偏移把
     * 鼠标X 换成侧栏面内坐标, 它不生效小地图点击就完全没反应。
     * ★ 逐处安装: 某一处字节对不上只该坏它自己那一处, 不能连坐。
     * ★ 已经是 kDone 的算打过 (幂等)。装完逐处复核并记进日志。*/
    {
        static const unsigned char kWant[4]  = { 0xA8, 0x6F, 0x88, 0x00 };
        static const unsigned char kDone[4]  = { 0x90, 0x6F, 0x88, 0x00 };
        static const struct { unsigned at; int off; const char* name; } kSites[5] = {
            { ADDR_SIDEBARBR_X, 1, "route" },
            { ADDR_RADARHIT_X, 2, "topcursor" },
            { ADDR_RADARPOS_X, 2, "radarpos" },
            { ADDR_RADARACT_X, 2, "radaract" },
            { ADDR_TOPCURSOR_X, 2, "stripcursor" },
        };
        int i, done = 0;
        for (i = 0; i < 5; i++)
        {
            unsigned at = kSites[i].at + kSites[i].off;
            if (Fm_BytesSame((unsigned char*)(size_t)at, kDone, 4)) { done++; continue; }
            if (!Fm_BytesSame((unsigned char*)(size_t)at, kWant, 4))
            {
                LogLine(Fm_SiteBad(kSites[i].name, kSites[i].at));
                continue;
            }
            if (Fm_PatchBytes(at, kDone, 4)) done++;
            else LogLine(Fm_SiteBad(kSites[i].name, kSites[i].at));
        }
        {
            char b1[16], buf[128];
            unsigned q = 0;
            q = AppendCStr(buf, q, "fullmap: sidebar-hit patch ON ", sizeof(buf));
            UtoA((unsigned)done, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, "/5 sites", sizeof(buf));
            LogLine(buf);
        }
    }    /* ★ 复核 (逐处): 每处 imm32 都必须真的变成 0x00886F90 (SidebarBounds.X)。
     * 单独点名 radaract —— 原生小地图命中判定就靠它, 它不行其余都白搭。*/
    {
        static const unsigned char kDone[4] = { 0x90, 0x6F, 0x88, 0x00 };
        static const struct { unsigned at; int off; const char* name; } kChk[5] = {
            { ADDR_SIDEBARBR_X, 1, "route" },
            { ADDR_RADARHIT_X, 2, "topcursor" },
            { ADDR_RADARPOS_X, 2, "radarpos" },
            { ADDR_RADARACT_X, 2, "radaract" },
            { ADDR_TOPCURSOR_X, 2, "stripcursor" },
        };
        int i;
        for (i = 0; i < 5; i++)
        {
            char b1[16], buf[160];
            unsigned q = 0;
            int ok = Fm_BytesSame((unsigned char*)(size_t)(kChk[i].at + kChk[i].off),
                kDone, 4);
            q = AppendCStr(buf, q, "fullmap: verify ", sizeof(buf));
            q = AppendCStr(buf, q, kChk[i].name, sizeof(buf));
            q = AppendCStr(buf, q, ok ? " ok" : " BAD", sizeof(buf));
            if (!ok)
            {
                q = AppendCStr(buf, q, " bytes=", sizeof(buf));
                UtoA(kChk[i].at, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
                q = AppendCh(buf, q, ':', sizeof(buf));
                {
                    unsigned char* p = (unsigned char*)(size_t)kChk[i].at;
                    int j2;
                    for (j2 = 0; j2 < 6; j2++)
                    {
                        b1[0] = "0123456789ABCDEF"[(p[j2] >> 4) & 15];
                        b1[1] = "0123456789ABCDEF"[p[j2] & 15];
                        b1[2] = 0;
                        q = AppendCStr(buf, q, b1, sizeof(buf));
                    }
                }
            }
            LogLine(buf);
        }
    }
    /* 边缘卷屏右缘 (0x692B60): 引擎按 面宽+侧栏宽 判定, 面恒全宽后判据跑到屏外、
     * 右缘永不触发。改取面右缘, 等长只写 ebp
     * (lea ebp,[eax+ebp-1] -> lea ebp,[eax-1]; nop)。 */
    {
        static const unsigned char kWantEdge[4] = { 0x8D, 0x6C, 0x28, 0xFF };
        static const unsigned char kEdge[4]     = { 0x8D, 0x68, 0xFF, 0x90 };
        if (Fm_BytesSame((unsigned char*)0x00692BB4u, kWantEdge, 4))
            LogLine(Fm_PatchBytes(0x00692BB4u, kEdge, 4)
                ? "fullmap: edge-scroll zone ON (composite right edge)"
                : "fullmap: edge-scroll zone patch fail");
        else if (Fm_BytesSame((unsigned char*)0x00692BB4u, kEdge, 4))
            LogLine("fullmap: edge-scroll zone ON (already patched)");
        else LogLine(Fm_SiteBad("edgezone", 0x00692BB4u));
    }
    s_PatchesInstalled = 1;
    LogLine("fullmap: viewrect hook ON (surface full width / view follows mode)");
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

static int s_DisarmFrame = -1;
static void Fm_DisarmSidebarGadgets(int colX, int topH)
{
    int i, fr = *(int*)(size_t)ADDR_FRAME_COUNT;
    /* 帧门: 同帧重复 poll 写同值, 每帧摘一次即可 (引擎帧内 Enable 回去下帧重摘)。 */
    if (fr == s_DisarmFrame) return;
    s_DisarmFrame = fr;
    for (i = 0; i < FM_CAMEO_N; i++)
        Fm_DisarmAt(ADDR_CAMEO_ARRAY + (unsigned)i * FM_CAMEO_STRIDE, colX, topH);
    for (i = 0; i < FM_TAB_N; i++)
        Fm_DisarmAt(ADDR_TAB_ARRAY + (unsigned)i * FM_TAB_STRIDE, colX, topH);
}

/* "完全显示"模式: 把我们摘掉的 gadget 恢复回可用 (清 Disabled 标志)。
 * 只碰同一个数组里同样通过校验的那些, 不越界写。 */
static void Fm_RearmAt(unsigned g, int colX, int topH)
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
    if (y < topH) return;
    if (x < colX - 8) return;
    if (x > colX + 256) return;
    p[GAD_DISAB] = 0;
    p[GAD_REDRAW] = 1;
}

static void Fm_RearmSidebarGadgets(int colX, int topH)
{
    int i;
    for (i = 0; i < FM_CAMEO_N; i++)
        Fm_RearmAt(ADDR_CAMEO_ARRAY + (unsigned)i * FM_CAMEO_STRIDE, colX, topH);
    for (i = 0; i < FM_TAB_N; i++)
        Fm_RearmAt(ADDR_TAB_ARRAY + (unsigned)i * FM_TAB_STRIDE, colX, topH);
}

/* 侧栏列起点 (cameo/页签/雷达摆放与鼠标判定的基准)。视口铺满后引擎算出来错 168,
 * 每循环按"视口右边界 - 列宽"改回, 且须在 InitGUI 之前改好 (一次性写入)。
 * 不碰 0x72AD90; 跟实际视口宽度走, 不看模式。 */
typedef void (__thiscall *FnSyncViewBounds)(void* tactical, void* rect);
#define ADDR_SYNC_VIEW       0x006D5F60u   /* TacticalClass 视口同步 (Set_View_Dimensions 内部第二步) */

/* [0xB0CE28/2C/30/34] = ViewBounds 缓存副本 (0x6D5F60 抄)。缓存须恒等于 R,
 * 否则 TacticalPos/ClientToCoords 按旧值算出恒定偏移。详见 notes §2.1。 */
#define ADDR_VIEW_CACHE_W    0x00B0CE30u
#define ADDR_VIEW_CACHE_H    0x00B0CE34u

/* 让缓存回到 R。幂等, 代价是两次 int 比较 —— 所以"几何没变"的早退之前也照跑。*/
static void Fm_SyncViewCache(void* tac, int w, int h)
{
    FnSyncViewBounds sync = (FnSyncViewBounds)(size_t)ADDR_SYNC_VIEW;
    if (!sync || !tac || !IsPlausiblePtr(tac)) return;
    if (*(int*)(size_t)ADDR_VIEW_CACHE_W == w
     && *(int*)(size_t)ADDR_VIEW_CACHE_H == h) return;
    sync(tac, (void*)(size_t)ADDR_VIEWBOUNDS);
}

/* Set_View_Dimensions (视口尺寸变化的引擎正门): 附带重建裁剪器/鼠标视口/InitGUI。 */
typedef void (__thiscall *FnSetViewDims)(void* self, const void* rect);
typedef void (__thiscall *FnInitGui)(void* self);
#define ADDR_SET_VIEW_DIMS   0x004A8960u
#define ADDR_INIT_GUI        0x006ABD30u   /* DisplayClass::InitGUI (thiscall, 无栈参) */
#define ADDR_DISPLAY_INSTANCE 0x0087F7E8u
/* 重入保护: 它末尾会调 InitGUI, 侧栏布局里可能又回到本模块 */
static int s_InSetViewDims = 0;
static int s_InInitGui = 0;
/* 一帧只允许重建一次裁剪器 (prepare 一帧能跑上百次) */
static int s_SyncViewFrame = -1;

/* 侧栏一次性布局需要按 **A** 重算的标记 (几何一变就置, FM_Poll 消费)。 */
static int s_LayoutDirty = 0;

/* ---------------- 侧栏一次性布局: 单独跑一遍 InitGUI(A) ----------------
 * InitGUI 里那批**一次性**写入 (页签/cameo 位置与滚动偏移 `-SidebarBounds.X`、
 * 三个 radar gadget 的位置、底栏矩形 `[0xB0FC58..]`) 全都按**当时的 ViewBounds**
 * 算, 而且只在跑的时候算一次 ⇒ 视口几何一变就必须拿正确的矩形重跑, 否则侧栏
 * 图标就摆在上一次那个宽度算出来的位置上 (读档最明显: CreateAll 走我们改指的全宽
 * 矩形 1920x1080, 侧栏布局按 1920 算完就一直留着)。
 *
 * 为什么不用 Set_View_Dimensions (0x4A8960) 重跑: 它顺带按传入矩形重建
 * ZBuffer/ABuffer (缩小档下 R>A, 重建出来的缓冲比 ScaleBar 的小 ⇒ 野写)、
 * 重建 WWMouse 视口 (死区/光标判据全错)、还动消息区矩形。而这里要的**只是布局**。
 * 做法: 临时把 ViewBounds 摆成 A → 调 InitGUI → 摆回 R。列起点由 InitGUI 入口
 * 钩 (FM_SyncSidebarColumn) 纠, 完事再兜一层。
 * ⚠️ 只在真正需要时调 (缩小档 / 引擎重建了面), 否则每帧跑一遍纯属浪费。 */
static int Fm_InitGuiForA(void)
{
    RectangleStruct* vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    void* strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    void* self = (void*)(size_t)ADDR_DISPLAY_INSTANCE;
    FnInitGui fn = (FnInitGui)(size_t)ADDR_INIT_GUI;
    RectangleStruct keep;
    if (!fn || s_InInitGui || s_InSetViewDims) return 0;
    /* InitGUI 会解引用侧栏面 (0x887300) 调虚表, 面还没建好时不能进。 */
    if (!strip || !IsPlausiblePtr(strip)) return 0;
    if (s_ViewA.Width < 64 || s_ViewA.Width > 8192) return 0;
    if (s_ViewA.Height < 64 || s_ViewA.Height > 8192) return 0;
    keep = *vb;
    vb->X = s_ViewA.X; vb->Y = s_ViewA.Y;
    vb->Width = s_ViewA.Width; vb->Height = s_ViewA.Height;
    s_InInitGui = 1;
    fn(self);
    s_InInitGui = 0;
    *vb = keep;
    FM_SyncSidebarColumn();
    LogLine("fullmap: sidebar layout refreshed for A");
    return 1;
}

/* 脏标记的消费口: 真跑成了才清 (跑不成就留着, 下一趟再试)。*/
static void Fm_ConsumeLayoutDirty(void)
{
    if (!s_LayoutDirty) return;
    if (Fm_InitGuiForA()) s_LayoutDirty = 0;
}

/* 当前视口是不是"全宽"那套。屏幕布局量一律按 A 算 (跟 R 走会多出第二条侧栏)。 */static int Fm_ViewWide(void)
{
    const RectangleStruct* vb = &s_ViewA;
    RectangleStruct* wb = (RectangleStruct*)(size_t)ADDR_WINDOWBOUNDS;
    if (wb->Width < 64 || wb->Width > 8192) return 1;
    return (vb->Width >= wb->Width - 8) ? 1 : 0;
}

/* 侧栏列起点 (bare=1 是全屏路由阈值)。全宽时减 168 落回屏内; 原版视口不减。
 * bare=1 顺带让面板全量重画判据恒不成立 (免每次点击强刷)。 */
static int Fm_ColumnX(int bare)
{
    const RectangleStruct* vb = &s_ViewA;   /* 满视图区, 不跟缩放走 (见 Fm_ViewWide) */
    int sbW = *(int*)(size_t)ADDR_SIDEBAR_W;
    int colX;
    if (sbW < 64 || sbW > 512) return -1;
    if (vb->Width < 64 || vb->Width > 8192) return -1;
    if (*(unsigned char*)(size_t)ADDR_ISNOD_SIDEBAR)
    {
        if (bare || !Fm_ViewWide()) colX = vb->X + vb->Width;
        else colX = vb->X + vb->Width - sbW;
    }
    else
    {
        if (bare) colX = -(sbW + 8);
        else colX = 0;
    }
    if (!bare && colX < 0) colX = 0;
    return colX;
}

/* 侧栏列在屏幕上的起点。条带上屏 (FM_BlitSb) 与小地图自绘 (FM_DrawRadarTo)
 * 必须算出同一个值, 所以只留这一处; 判据仍从视口算, 不读 SidebarBounds.X
 * (那个全局在"全屏"态会被推成路由阈值)。取值不成时退回"贴右缘"。 */
static int Fm_ColumnPlace(void)
{
    int sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
    int winW = ((int*)(size_t)ADDR_WINDOWBOUNDS)[2];
    int colX;
    if (sbw < 64 || sbw > 512) sbw = 168;
    if (winW < 640 || winW > 8192) winW = 1920;
    colX = Fm_ColumnX(0);
    if (colX < 0 || colX > winW) colX = winW - sbw;
    if (colX < 0) colX = 0;
    return colX;
}

/* 把侧栏列起点钉在 1x 的值上。⚠️ `SidebarBounds.X` 同时是那五处 imm32 补丁读的
 * 值 (含 0x653A3F = `RadarClass::Action`), 它一抖小地图原生点击链就断, 所以
 * 这里必须稳定 —— **不要**补调 `SidebarClass::ComputeLayout` (0x6A5130) 去"按纠正
 * 后的值重推派生锚点", 那函数在 `flag!=0` 时会照 `ViewBounds`(= R) 重算它。
 * 详见 notes/隐藏侧栏.md §4.3b。*/
static void Fm_SyncColumn(int bare)
{
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int colX;
    if (!s_Enable) return;
    colX = Fm_ColumnX(bare);
    if (colX < 0) return;
    if (sb->X != colX) sb->X = colX;
}

/* InitGUI 入口钩专用: 永远按"侧栏存在"的几何纠正 (全屏态开局偏移不错 168)。 */
void FM_SyncSidebarColumn(void)
{
    Fm_SyncColumn(0);
}

/* ---------------- 地图区右边界 (给 MOE 那些贴右下角画的层当锚点) ---------------- */
int FM_MapRightEdge(void)
{
    /* 满视图区右边界 (缩放不改变"地图区"这个概念, 见 Fm_ViewWide 的说明)。*/
    const RectangleStruct* vb = &s_ViewA;
    int r;
    if (!s_Enable) return 0;
    if (vb->Width < 64 || vb->Width > 8192) return 0;
    /* 视口右边界 (态0=1752, 态1/2=1920): 各自贴边, 自适应侧栏显隐。 */
    r = vb->X + vb->Width;
    if (r < 64 || r > 8192) return 0;
    return r;
}

/* 视口宽度动态切 (三条腿: 字节窗口翻态 + 每循环纠 ViewBounds + 建面恒全宽)。
 * 安全阀: 视口不许比面宽 (否则往面外画即花屏), 只夹窄不 skip。 */

/* 两个字节窗口的"已知两态"字节 (供探测用):
 *   原版态: 0x72AD69 imm32 = A8 00 00 / 0x72AD60 = B9 A8 00 00 00
 *   全宽态: 0x72AD69 imm32 = 00 00 00 / 0x72AD60 = 90 x5            */
static const unsigned char kVanSub[3] = { 0xA8, 0x00, 0x00 };
static const unsigned char kVanGdi[5] = { 0xB9, 0xA8, 0x00, 0x00, 0x00 };
static const unsigned char kWideSub[3] = { 0x00, 0x00, 0x00 };
static const unsigned char kWideGdi[5] = { 0x90, 0x90, 0x90, 0x90, 0x90 };

/* 视口字节窗口现在处在哪一态: 0=原版宽, 1=全宽, -2=两种都不像 (外部改过)。 */
static int Fm_ViewPatchDetect(void)
{
    const unsigned char* pSub = (const unsigned char*)(size_t)ADDR_PATCH_SUBW;
    const unsigned char* pGdi = (const unsigned char*)(size_t)ADDR_PATCH_GDIX;
    if (Fm_BytesSame(pSub, kVanSub, 3) && Fm_BytesSame(pGdi, kVanGdi, 5)) return 0;
    if (Fm_BytesSame(pSub, kWideSub, 3) && Fm_BytesSame(pGdi, kWideGdi, 5)) return 1;
    return -2;
}

static int Fm_ViewPatchSet(int wide)
{
    const unsigned char* wSub = wide ? kWideSub : kVanSub;
    const unsigned char* wGdi = wide ? kWideGdi : kVanGdi;
    if (s_ViewPatched == -1) s_ViewPatched = Fm_ViewPatchDetect();
    if (s_ViewPatched == -2) return 0;
    if (wide == s_ViewPatched) return 1;
    /* 当前字节必须正好是"另一个已知态"才动手 —— 只认两种已知态, 别的当外部
     * 补丁停手 (别拿"相反态"当唯一预期, 见 FM_Install 注释)。*/
    {
        int cur = Fm_ViewPatchDetect();
        if (cur == -2 || cur == wide)
        {
            LogLine(wide ? "fullmap: viewrect bytes mismatch (to wide)"
                         : "fullmap: viewrect bytes mismatch (to vanilla)");
            s_ViewPatched = -2;
            return 0;
        }
        s_ViewPatched = cur;
    }
    if (!Fm_PatchBytes(ADDR_PATCH_SUBW, wSub, 3)
        || !Fm_PatchBytes(ADDR_PATCH_GDIX, wGdi, 5))
    {
        LogLine("fullmap: viewrect patch fail");
        s_ViewPatched = -2;
        return 0;
    }
    s_ViewPatched = wide;
    LogLine(wide ? "fullmap: viewrect -> full width (view = client)"
                 : "fullmap: viewrect -> vanilla (view = client-168)");
    return 1;
}

/* 建面矩形恒全宽全高 (高不减, 免读档残影带)。
 * ViewBounds.Height 事后由 Fm_ApplyViewWidth 纠回。 */
static RectangleStruct* __fastcall Fm_ViewRectStub(RectangleStruct* out)
{
    RectangleStruct* wb = (RectangleStruct*)(size_t)ADDR_WINDOWBOUNDS;
    if (out && wb->Width >= 64 && wb->Width <= 8192
        && wb->Height >= 96 && wb->Height <= 8192)
    {
        out->X = 0;
        out->Y = 0;
        out->Width = wb->Width;
        out->Height = wb->Height;
    }
    return out;
}

/* 换几何后催地图重画: 增量面只重画标脏格, 新露出的边必须连推几帧整窗脏区。 */
#define ADDR_REG_DIRTY_AREA 0x006D2790u
typedef void (__fastcall *FnRegDirtyArea)(void*, RectangleStruct, int);

static void Fm_DirtyRedraw(void)
{
    RectangleStruct* vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    RectangleStruct* wb = (RectangleStruct*)(size_t)ADDR_WINDOWBOUNDS;
    void* tac = *(void**)(size_t)0x00887324u;
    FnRegDirtyArea fn;
    RectangleStruct r;
    if (s_DirtyFrames <= 0) return;
    if (!tac || !IsPlausiblePtr(tac)) return;
    if (vb->Width < 64 || vb->Width > 8192) return;
    if (vb->Height < 64 || vb->Height > 8192) return;
    r.X = 0;
    r.Y = 0;
    r.Width = (wb->Width >= 64 && wb->Width <= 8192) ? wb->Width : vb->Width;
    r.Height = (wb->Height >= 64 && wb->Height <= 8192) ? wb->Height : vb->Height;
    if (r.Width < vb->Width) r.Width = vb->Width;
    if (r.Height < vb->Height) r.Height = vb->Height;
    fn = (FnRegDirtyArea)(size_t)ADDR_REG_DIRTY_AREA;
    fn(tac, r, 1);
    s_DirtyFrames--;
}

/* 本循环用的两个视口矩形 (给 ScaleBar 的拉伸与鼠标反变换)。
 * 关闭时报引擎当前视口 (s_ViewA/R 此时停在初值, 不可用)。 */
void FM_GetViewRects(RectangleStruct* a, RectangleStruct* r)
{
    const RectangleStruct* vb = (const RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    if (!s_Enable)
    {
        if (a) *a = *vb;
        if (r) *r = *vb;
        return;
    }
    if (a) *a = s_ViewA;
    if (r) *r = s_ViewR;
}

static void Fm_ApplyViewWidth(void)
{
    RectangleStruct* vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    RectangleStruct* wb = (RectangleStruct*)(size_t)ADDR_WINDOWBOUNDS;
    void* tac;
    void* comp;
    int sbW = *(int*)(size_t)ADDR_SIDEBAR_W;
    int winW, winH, x, y = 0, w, h, cw, oldW, oldH, d;

    if (!s_Enable) return;
    MOE_SetPhase(MP_APPLY);
    if (sbW < 64 || sbW > 512) return;
    if (wb->Width < 640 || wb->Width > 8192) return;
    if (wb->Height < 240 || wb->Height > 8192) return;
    if (vb->Height < 64 || vb->Height > 8192) return;

    winW = wb->Width;
    winH = wb->Height;
    h = winH - 32;                                  /* 0x7F5BFC */
    if (h < 64) return;
    if (Fm_ModeFull())
    {
        x = (*(unsigned char*)(size_t)ADDR_ISNOD_SIDEBAR
             || *(unsigned char*)(size_t)ADDR_ARMAGEDDON) ? 0 : sbW;
        w = winW - sbW;
    }
    else
    {
        x = 0;
        w = winW;
    }
    if (w < 64) return;

    comp = *(void**)(size_t)ADDR_SURF_COMPOSITE;
    cw = comp ? GtSurfGet(comp, GtVT_GET_WIDTH) : -1;
    if (cw > 0 && w > cw)
    {
        if (!s_SurfWarned)
        {
            char b1[16], b2[16], buf[192];
            unsigned q = 0;
            s_SurfWarned = 1;
            q = AppendCStr(buf, q, "fullmap: view wider than surface ", sizeof(buf));
            UtoA((unsigned)w, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " > ", sizeof(buf));
            UtoA((unsigned)cw, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, " -> clamp", sizeof(buf));
            LogLine(buf);
        }
        w = cw;
        if (x > w - 16) x = 0;
    }
    /* ---------------- 缩放 (ScaleBar): 视口改成 A*scale ----------------
     * 引擎只会 1:1 逐格画, 所以缩放只能靠"视口大小 + 呈现时重采样":
     *   A = 满视图区 (上面算出来的), R = A * scale (ScaleBar 算, 含落点)。
     *   scale<1 视口收小 -> 引擎画得少, 呈现时把 R 放大铺回 A;
     *   scale>1 视口放大 -> 引擎照旧 1:1 画进 ScaleBar 的大面, 三趟之后
     *   把 R 降采样进 Composite 的 A (引擎的 ZBuffer/ABuffer 也跟着按 R 重建)。
     * 本模块只负责把 R 写进 ViewBounds, 缩放的几何全在 ScaleBar 里。 */
    {
        RectangleStruct r;
        int sig;
        double sc = SB_Scale();   /* 一次算好, GetViewRect 与 sig 门共用。 */
        /* A 永远是"满屏本该有的视口矩形", 与缩放无关。 */
        s_ViewA.X = x; s_ViewA.Y = 0; s_ViewA.Width = w; s_ViewA.Height = h;
        if (SB_GetViewRect(&r, sc))
        {
            x = r.X;
            y = r.Y;
            w = r.Width;
            h = r.Height;
        }
        else { y = 0; }
        sig = (int)(sc * 256.0);
        if (s_ZoomLast != sig)
        {
            /* 换挡时视口变了, 但**不要**走下面"左边界钉住"那套: 缩放要的是中心
             * 不动 (缩放原地), 那套是给侧栏模式切换用的 (左边界固定)。*/
            s_ZoomChanged = 1;
            s_ZoomLast = sig;
        }
        s_ViewR.X = x; s_ViewR.Y = y; s_ViewR.Width = w; s_ViewR.Height = h;
    }
    tac = *(void**)(size_t)0x00887324u;
    /* "拿 A 调 setDims"分支会把缓存留成 A, 早退前先自愈。 */
    if (vb->X == x && vb->Y == y && vb->Width == w && vb->Height == h)
    {
        Fm_SyncViewCache(tac, w, h);
        return;
    }
    /* 几何变了 ⇒ 侧栏那批一次性布局写入也得按 A 重跑。缩小档不走 setDims,
     * 所以那条路完全没人替我们刷新它 ⇒ 这里无条件置脏 (是否真跑见 FM_Poll)。*/
    s_LayoutDirty = 1;

    oldW = vb->Width;
    oldH = vb->Height;
    vb->X = x;
    vb->Y = y;
    vb->Width = w;
    vb->Height = h;
    /* 几何变了催整块重画 (判几何不判宽度, 高/起点也可能变)。连推几帧盖住重建窗口。 */
    {
        static int s_LastDirtyW = -1, s_LastDirtyH = -1, s_LastDirtyX = -1;
        if (w != s_LastDirtyW || h != s_LastDirtyH || x != s_LastDirtyX)
        {
            s_LastDirtyW = w; s_LastDirtyH = h; s_LastDirtyX = x;
            s_DirtyFrames = 3;
            {
                char b1[16], b2[16], b3[16], buf[160];
                unsigned q = 0;
                q = AppendCStr(buf, q, "fullmap: view ", sizeof(buf));
                UtoA((unsigned)oldW, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
                q = AppendCh(buf, q, 'x', sizeof(buf));
                UtoA((unsigned)oldH, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
                q = AppendCStr(buf, q, "->", sizeof(buf));
                UtoA((unsigned)w, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
                q = AppendCh(buf, q, 'x', sizeof(buf));
                UtoA((unsigned)h, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
                q = AppendCh(buf, q, '@', sizeof(buf));
                UtoA((unsigned)x, b3); q = AppendCStr(buf, q, b3, sizeof(buf));
                q = AppendCStr(buf, q, " redraw", sizeof(buf));
                LogLine(buf);
            }
        }
    }
    tac = *(void**)(size_t)0x00887324u;
    if (tac && IsPlausiblePtr(tac))
    {
        /* 换宽度钉左边界: 中心按 ±ΔW/2 挪回 (引擎按中心-W/2 算左上)。只挪中心。 */
        d = (w - oldW) / 2;
        if (d && s_ZoomChanged)
            d = 0;              /* 只是换了缩放挡: 中心保持不动 */
        s_ZoomChanged = 0;
        if (d)
        {
            *(int*)((char*)tac + 0xD64) += d;
            *(int*)((char*)tac + 0xD74) += d;
        }
        /* 便宜路径: 只同步矩形给 TacticalClass (幂等, 缓存对上才真调)。 */
        Fm_SyncViewCache(tac, w, h);
        /* 宽度真变了走一次正门重建裁剪器/鼠标视口 (只调它俩不会重建)。
         * 缩小档禁调 (它按 R 重建会顶掉 ScaleBar 的缓冲, 且末尾 InitGUI)。 */
        if (w != oldW && !s_InSetViewDims && !SB_Out())
        {
            void* mouse = *(void**)ADDR_WWMOUSE;
            FnSetViewDims setDims = (FnSetViewDims)(size_t)ADDR_SET_VIEW_DIMS;
            int fr = *(int*)(size_t)ADDR_FRAME_COUNT;
            if (mouse && IsPlausiblePtr(mouse) && fr != s_SyncViewFrame)
            {
                s_SyncViewFrame = fr;
                s_InSetViewDims = 1;
                /* 用 A 重建 (InitGUI 一次性布局按它算, 传 R 侧栏全错), 完事写回 R。
                 * 但 setDims 落到 Set_Viewport (0x6D5F60) 会把相机中心 (+0xD74)
                 * 按**传入的 A** 夹位: 中心停在地图边缘时会被 A 的界拽回 (A 界比
                 * R 界窄) ⇒ 换档瞬间视野跳回 1x 边缘。存下中心, setDims 后写回,
                 * 让随后 Fm_SyncViewCache 的 Set_Viewport(R) 按 R 重新夹位。 */
                {
                    RectangleStruct keep = *vb;      /* = R */
                    int c0 = *(int*)((char*)tac + 0xD64);
                    int c1 = *(int*)((char*)tac + 0xD68);
                    int c2 = *(int*)((char*)tac + 0xD74);
                    int c3 = *(int*)((char*)tac + 0xD78);
                    vb->X = s_ViewA.X; vb->Y = s_ViewA.Y;
                    vb->Width = s_ViewA.Width; vb->Height = s_ViewA.Height;
                    setDims((void*)(size_t)ADDR_DISPLAY_INSTANCE,
                            (const void*)(size_t)ADDR_VIEWBOUNDS);
                    *vb = keep;                       /* 还原成 R */
                    *(int*)((char*)tac + 0xD64) = c0;
                    *(int*)((char*)tac + 0xD68) = c1;
                    *(int*)((char*)tac + 0xD74) = c2;
                    *(int*)((char*)tac + 0xD78) = c3;
                }
                s_InSetViewDims = 0;
                /* 重建裁剪器顺带改了侧栏的一次性布局 —— 立刻纠正列起点。*/
                FM_SyncSidebarColumn();
                /* setDims 把缓存留成 A, 纠回 R (否则视野恒偏 (A-R)/2)。 */
                Fm_SyncViewCache(tac, w, h);
                /* setDims 末尾已经按 A 跑过一次 InitGUI ⇒ 布局这块它包了。 */
                s_LayoutDirty = 0;
            }
        }
        /* 缩小档 (以及宽度没变 / 帧门挡住的那几次) 没人跑 InitGUI ⇒ 补上。
         * 这是"其它缩放挡位下侧栏图标不按位摆"的那一半: 一次性布局只有
         * setDims 那条路会重算, 而那条路恰好在缩小档被禁掉了。 */
        Fm_ConsumeLayoutDirty();
    }
}

/* 缩放改档后立刻同步视口 (幂等, setDims 有帧门控)。 */
void FM_SyncViewNow(void)
{
    if (!s_Enable) return;
    Fm_ApplyViewWidth();
    Fm_DirtyRedraw();
}

/* ---------------- F11 边沿检测 + 三态切换 ----------------
 * prepare 钩 (0x4F42F0) 一帧能跑上百次, 必须自门控帧号。
 * 只在游戏窗口有焦点时响应, 免得切窗口时误按。 */
static int s_TogLastFrame = -1;
static int s_TogKeyDown = 0;
static int s_GadgetsArmed = 0;   /* 1 = 页签/cameo 已经恢复可用 (不必每帧再复) */
static int s_ForceRedraw = 0;    /* 还剩几帧强制面板全量重画 (进态0 用) */
static int s_ForceRedrawFrame = -1;
static int s_LastLoggedMode = -1;   /* 只用于"模式变了就打一行几何" */
static int s_LastLoggedZoom = -1;

static void Fm_PollToggle(void)
{
    static const char* kName[3] = { "full", "radar", "bare" };
    char buf[96];
    unsigned p = 0;
    int fr, down;
    if (!s_Enable || s_LockedFull || s_MenuOff || s_ToggleHk.disabled) return;
    fr = *(int*)(size_t)ADDR_FRAME_COUNT;
    if (fr == s_TogLastFrame) return;
    s_TogLastFrame = fr;
    if (!GtGameFocused()) { s_TogKeyDown = 0; return; }
    down = OU_HotkeyDown(&s_ToggleHk) ? 1 : 0;
    if (down && !s_TogKeyDown)
    {
        s_Mode = (s_Mode + 1) % 3;
        /* 换模式后侧栏面要重画: 完整显示需要面板在, 收窄/全屏状态需要抹干净 */
        *(unsigned char*)(size_t)ADDR_SIDEBAR_REDRAW = 1;
        *(unsigned char*)(size_t)ADDR_SIDEBAR_NEEDRD = 1;
        *(unsigned char*)(size_t)ADDR_SIDEBAR_BGNEEDRD = 1;
        s_RadarLockOk = 0;
        s_LbPress = 0;
        s_GadgetsArmed = 0;
        /* 态1/2 期间侧栏面除保留区外被抹黑过; 切回态0 必须让引擎把面板真的
         * 重画一遍, 否则呈上去的是抹黑后的残留。0x53A7=1 走 SidebarClass::Draw
         * 的全量重画分支 (0x6A6CFD), 0x53A6/0x53A8 一并置上。
         * 只压几帧 —— 每帧都压会把刚画好的小地图又擦掉 (见文件末尾的警告)。 */
        s_ForceRedraw = Fm_ModeFull() ? 8 : 0;
        s_ForceRedrawFrame = -1;
        p = AppendCStr(buf, p, "fullmap: mode -> ", sizeof(buf));
        p = AppendCStr(buf, p, kName[s_Mode], sizeof(buf));
        LogLine(buf);
    }
    s_TogKeyDown = down;
}

/* 引擎重建了渲染面 (读档 CreateAll / 改分辨率) ⇒ 侧栏面是**空白的新面**, 而
 * 那批一次性布局写入 (页签/cameo 位置、三个 radar gadget 位置、底栏矩形) 在上
 * 一次几何下就写完了, 引擎不会自己重算 ⇒ 不补一遍就是"读档后侧栏图标不按位摆"。
 * 判据用侧栏面指针换人 (比尺寸硬: 尺寸可能碰巧一样)。
 * 顺带把缩放退回 1x: `s_ForceRedraw` 会逼侧栏走全量重画, 那条路里的
 * `ComputeLayout` 照当时的 ViewBounds 重推 cameo 锚点, ViewBounds 是 R 时全错。
 * 必须在 Fm_ApplyViewWidth 之前调 (本函数已是 FM_Poll 的第一个动作)。*/
static void Fm_NoteSurfaceRebuild(void)
{
    static void* s_LastStrip = NULL;
    void* strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    if (!s_Enable) return;
    if (!strip || !IsPlausiblePtr(strip)) { s_LastStrip = NULL; return; }
    if (strip == s_LastStrip) return;
    if (s_LastStrip)
    {
        char b1[16], b2[16], buf[128];
        unsigned q = 0;
        q = AppendCStr(buf, q, "fullmap: surfaces rebuilt ", sizeof(buf));
        UtoA((unsigned)(size_t)s_LastStrip, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " -> ", sizeof(buf));
        UtoA((unsigned)(size_t)strip, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
        LogLine(buf);
        /* 新面尺寸可能不同 ⇒ 小地图矩形的锁定值作废 (Fm_RadarRect 自己会重锁)。*/
        s_RadarLockOk = 0;
        /* 缩放退 1x (ViewBounds 因此是 A, 见上)。1x 时空转。*/
        SB_ResetZoom1x();
        /* 一次性布局按 A 重跑 + 面板全量重画几帧 (新面是空的)。*/
        s_LayoutDirty = 1;
        s_ForceRedraw = 8;
        s_ForceRedrawFrame = -1;
        s_DirtyFrames = 3;
    }
    s_LastStrip = strip;
}

void FM_Poll(void)
{
    RectangleStruct* sb;
    RectangleStruct radar;
    RectangleStruct* vb;
    int* wb;
    int winW, sbW, colX, topH, colXcalc;
    int radarOk = 0;

    if (!s_Enable) return;
    if (!g_Log) return;
    /* 引擎重建了面? (读档/改分辨率) —— 要在 Apply 之前知道, 好让它这一趟就把
     * 布局按 A 重跑。 */
    Fm_NoteSurfaceRebuild();
    /* 不碰 s_LbPress (点击门自己的标记, 门直接读按键状态)。 */
    Fm_PollToggle();
    /* 视口宽度跟模式走 (幂等, 须在侧栏列纠正之前)。 */
    Fm_ViewPatchSet(!Fm_ModeFull());
    /* ViewBounds 引擎不重算, 每循环纠正。 */
    Fm_ApplyViewWidth();
    /* 几何没变但面换过人 (读档) 时, 上面的 Apply 会早退, 布局脏标记还在这儿。*/
    Fm_ConsumeLayoutDirty();
    /* 催重画 (须在 Apply 之后, 那儿才知道几何变没变)。 */
    Fm_DirtyRedraw();
    sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    wb = (int*)(size_t)ADDR_WINDOWBOUNDS;
    sbW = *(int*)(size_t)ADDR_SIDEBAR_W;
    if (sbW < 64 || sbW > 512) return;
    winW = wb[2];
    if (winW < 640 || winW > 8192) return;
    /* 只有态1/2 才需要小地图矩形 (态0 整条交给引擎) */
    radarOk = Fm_RadarRect(&radar);
    if (!radarOk && Fm_ModeKeep()) return;

    /* 侧栏列起点纠正 (态0 也调, 否则切回来路由阈值留 1920)。 */
    Fm_SyncColumn(Fm_SidebarGone());
    /* 诊断以模式为触发 (拿宽度比永远相等, 等于没诊断)。 */
    if (s_Mode != s_LastLoggedMode || SB_Level() != s_LastLoggedZoom)
    {
        char b1[16], b2[16], buf[256];
        RectangleStruct rr;
        int okR;
        unsigned q = 0;
        s_LastLoggedMode = s_Mode;
        s_LastLoggedZoom = SB_Level();
        q = AppendCStr(buf, q, "fullmap: geom mode=", sizeof(buf));
        UtoA((unsigned)s_Mode, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " zoom=", sizeof(buf));
        {
            /* scale 是小数 (0.25 = 4 倍放大 ... >1 = 缩小), 打三位定点 */
            double sc = SB_Scale();
            int iv = (int)sc;
            int fr = (int)((sc - (double)iv) * 1000.0 + 0.5);
            UtoA((unsigned)iv, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCh(buf, q, '.', sizeof(buf));
            UtoA((unsigned)fr, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        }
        q = AppendCStr(buf, q, " view=", sizeof(buf));
        UtoA((unsigned)vb->Width, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCh(buf, q, 'x', sizeof(buf));
        UtoA((unsigned)vb->Height, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, "/", sizeof(buf));
        UtoA((unsigned)vb->X, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCh(buf, q, ',', sizeof(buf));
        UtoA((unsigned)vb->Y, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " col=", sizeof(buf));
        UtoA((unsigned)sb->X, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        colXcalc = Fm_ColumnX(0);   /* 诊断分支内现算 (平时每 poll 省一次)。
                                     * 与 col 一致不记, 不一致才追 colCalc=。 */
        if (colXcalc != sb->X)
        {
            q = AppendCStr(buf, q, " colCalc=", sizeof(buf));
            UtoA((unsigned)colXcalc, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        }
        q = AppendCStr(buf, q, " wide=", sizeof(buf));
        UtoA((unsigned)Fm_ViewWide(), b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " keep=", sizeof(buf));
        UtoA((unsigned)Fm_ModeKeep(), b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        okR = radarOk; rr = radar;   /* 1077 已取 (副作用幂等, 值相同)。 */
        q = AppendCStr(buf, q, " radar=", sizeof(buf));
        if (okR)
        {
            UtoA((unsigned)rr.Width, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, "x", sizeof(buf));
            UtoA((unsigned)rr.Height, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, "@", sizeof(buf));
            UtoA((unsigned)rr.X, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        }
        else q = AppendCStr(buf, q, "none", sizeof(buf));
        q = AppendCStr(buf, q, " anchor=", sizeof(buf));
        UtoA((unsigned)FM_MapRightEdge(), b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        LogLine(buf);
        /* 第二行: 引擎内部状态与视口是否同源。侧栏标题栏那条"未探索的区域"
         * 就是这一组不一致造成的 (视口窄、裁剪器宽 ⇒ 侧栏列上的鼠标被当成
         * "还在地图里"去查格, 那一格在地图外)。这三个矩形必须每行都能对。*/
        {
            void* pComp = *(void**)ADDR_SURF_COMPOSITE;
            void* pMap = *(void**)ADDR_SURF_MAP;
            void* pStrip = *(void**)ADDR_SURF_SIDEBAR;
            int cwid = pComp ? GtSurfGet(pComp, GtVT_GET_WIDTH) : -1;
            int chgt = pComp ? GtSurfGet(pComp, GtVT_GET_HEIGHT) : -1;
            int mwid = pMap ? GtSurfGet(pMap, GtVT_GET_WIDTH) : -1;
            int mhgt = pMap ? GtSurfGet(pMap, GtVT_GET_HEIGHT) : -1;
            int swid = pStrip ? GtSurfGet(pStrip, GtVT_GET_WIDTH) : -1;
            int shgt = pStrip ? GtSurfGet(pStrip, GtVT_GET_HEIGHT) : -1;
            RectangleStruct* cb = (RectangleStruct*)(size_t)ADDR_VIEWCLIP_A;
            RectangleStruct* cb2 = (RectangleStruct*)(size_t)ADDR_VIEWCLIP_B;
            q = 0;
            q = AppendCStr(buf, q, "fullmap: surf comp=", sizeof(buf));
            UtoA((unsigned)cwid, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCh(buf, q, 'x', sizeof(buf));
            UtoA((unsigned)chgt, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            if (mwid != cwid || mhgt != chgt)   /* map 与 comp 一致不记 */
            {
                q = AppendCStr(buf, q, " map=", sizeof(buf));
                UtoA((unsigned)mwid, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
                q = AppendCh(buf, q, 'x', sizeof(buf));
                UtoA((unsigned)mhgt, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            }
            q = AppendCStr(buf, q, " strip=", sizeof(buf));
            UtoA((unsigned)swid, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCh(buf, q, 'x', sizeof(buf));
            UtoA((unsigned)shgt, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, " clipA=", sizeof(buf));
            UtoA((unsigned)cb->Width, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCh(buf, q, '@', sizeof(buf));
            UtoA((unsigned)cb->X, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, " clipB=", sizeof(buf));
            UtoA((unsigned)cb2->Width, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCh(buf, q, '@', sizeof(buf));
            UtoA((unsigned)cb2->X, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            if (s_ViewPatched)   /* 0 是常态, 不记; 非 0 才追 */
            {
                q = AppendCStr(buf, q, " vpatch=", sizeof(buf));
                UtoA((unsigned)s_ViewPatched, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            }
            LogLine(buf);
        }
    }
    topH = sb->Y;
    if (topH < 32 || topH > 512) topH = 158;
    /* 摘/复 gadget 一律按"侧栏列真实起点"筛选: 全屏态 sb->X 已经被推成路由阈值
     * (1920 / -176), 拿它当筛选条件会让整列 gadget 全部漏掉, 隐形按钮就又活了。*/
    colX = Fm_ColumnPlace();
    if (Fm_ModeDisarm())
    {
        Fm_DisarmSidebarGadgets(colX, topH);
        s_GadgetsArmed = 0;
    }
    else if (!s_GadgetsArmed)
    {
        /* 恢复可用只需做一次: 引擎不会自己把 Disabled 置回去 (它只 Enable 自己
         * 摘的那些), 所以进 FULL 态时补一遍就够, 之后每帧不再碰这 244 个对象。*/
        Fm_RearmSidebarGadgets(colX, topH);
        s_GadgetsArmed = 1;
    }
    /* 视口命中区: 态1 收窄成"视口减去侧栏列", 态0/态2 是整宽 (态0 视口本来就是
     * 原版 1752, 本来就不含侧栏列)。每循环都做一遍 (幂等) —— FM_NoteGadgetInput
     * 那条路在态2 会被"gadget 是否跨过侧栏列"的判据挡掉, 光靠它恢复不了。*/
    FM_TrimViewGadget();
    /* 进态0 压几帧强制面板全量重画 (引擎帧号门控, 跨帧不耗光)。 */
    if (s_ForceRedraw > 0)
    {
        int fr = *(int*)(size_t)ADDR_FRAME_COUNT;
        if (fr != s_ForceRedrawFrame)
        {
            s_ForceRedrawFrame = fr;
            s_ForceRedraw--;
        }
        *(unsigned char*)(size_t)ADDR_SIDEBAR_REDRAW = 1;
        *(unsigned char*)(size_t)ADDR_SIDEBAR_NEEDRD = 1;
        *(unsigned char*)(size_t)ADDR_SIDEBAR_BGNEEDRD = 1;
    }
}
/* 不要每帧强制面板重画 (会擦掉小地图致闪烁; 面板面本就持久)。 */

/* 目标矩形须整个落在目标面内 (面偶按旧宽度建, 越行尾即花屏/踩内存)。 */
static int Fm_FitRectToSurf(RectangleStruct* dst, RectangleStruct* src, int w, int h)
{
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

/* 侧栏列 blit 够不到的尾巴抹黑 (只态0: 态1/2 那列归地图, 抹了变黑带)。 */
static void Fm_CoverColumnTail(void* surf, const RectangleStruct* dst, int w, int h)
{
    void** vt;
    FnFillRect fn;
    RectangleStruct r;
    int y0, x0, cw;
    if (!Fm_ModeFull()) return;         /* 侧栏列归地图, 不许碰 */
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0) return;
    y0 = dst->Y + dst->Height;
    if (y0 >= h) return;                       /* 整列都盖到了 */
    x0 = dst->X;
    cw = dst->Width;
    if (x0 < 0 || cw <= 0 || x0 >= w) return;
    if (x0 + cw > w) cw = w - x0;
    r.X = x0;
    r.Y = y0;
    r.Width = cw;
    r.Height = h - y0;
    if (r.Height <= 0) return;
    vt = *(void***)(size_t)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    fn = (FnFillRect)vt[VT_FILL_RECT >> 2];
    if (fn) fn(surf, &r, 0);
}

/* 接管时的出口: 原版尾声 (0x6A7481) 等价物。原函数栈形是 [ret][bool] 且
 * 结尾 ret 4, 这里只做脏标记 + ret 4, 正好替掉整条 blit。 */
__attribute__((naked)) static void Fm_Ret4(void)
{
    __asm__ volatile ("ret $4\n\t");
}

/* 把光标精灵烙进侧栏条带, 调用期间借用 ViewBounds.Width 让它落在 blit 真正会去的位置。
 * 不变量: 接管呈现路径时, 引擎在这条路径内部用来推导坐标的全局, 呈现目标与精灵
 * 定位必须继续读同一个 —— 原版 BlitSidebar (0x6A70E0) 用 ViewBounds.Width 同时做
 * 两件事 (烙精灵 0x6A71B7 / 算 blit 落点 0x6A71D0), 而我们把侧栏列钉在 A 的右缘,
 * 缩放档下 A≠R ⇒ 精灵落错位置, 偏差随换挡/滚动在 0..差值间跳 ⇒ 忽隐忽现。
 * 借完立刻还原; flag=1 分支只读这一个字段, 中间没有引擎代码。
 * 盟军侧栏 (isNOD=0) 两边本来就都是 0, 不用动。*/
static void Fm_CursorStampStrip(void* mouse, void* strip, int colX)
{
    RectangleStruct* vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    void** vt;
    FnMouseFixSurface fn;
    int save, want;
    if (!mouse || !IsPlausiblePtr(mouse)) return;
    if (!strip || !IsPlausiblePtr(strip)) return;
    vt = *(void***)(size_t)mouse;
    if (!vt || !IsPlausiblePtr(vt)) return;
    fn = (FnMouseFixSurface)vt[WW_VT_FIX_CURSOR >> 2];
    if (!fn) return;
    want = (*(unsigned char*)(size_t)ADDR_ISNOD_SIDEBAR) ? colX : 0;
    if (!vb) { fn(mouse, strip, 1); return; }
    save = vb->Width;
    if (want > 0 && want != save) vb->Width = want;
    fn(mouse, strip, 1);
    if (want > 0 && want != save) vb->Width = save;
}

int FM_SidebarVisible(void)
{
    if (!s_Enable) return 1;
    return Fm_ModeFull() ? 1 : 0;
}

unsigned long FM_BlitSb(void)
{
    void* strip;
    void* prim;
    void* mouse;
    RectangleStruct src, dst;
    int ox, oy, colX;
    int stripW = 0, stripH = 0, primW = 0, primH = 0;

    if (!s_Enable) return 0;

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
    stripW = GtSurfGet(strip, GtVT_GET_WIDTH);
    stripH = GtSurfGet(strip, GtVT_GET_HEIGHT);
    primW = GtSurfGet(prim, GtVT_GET_WIDTH);
    primH = GtSurfGet(prim, GtVT_GET_HEIGHT);

    /* 全屏: 侧栏面整块抹黑, 一笔都不上屏 (原版整条 blit 已被换掉, 所以屏幕上
     * 什么都没有 —— 地图本来就已经铺满整宽了)。 */
    if (!Fm_ModeKeep())
    {
        Fm_ClearStripOnce(NULL, stripW, stripH);
        return (unsigned long)(size_t)Fm_Ret4;
    }

    if (Fm_KeepRect(&src, &stripW, &stripH))
    {
        void** vt;
        FnCopyFromPart fn;
        /* 原版 blit 用 `src.X + (isNOD?ViewW:0)`, 视口铺满后会把小地图顶到屏外。
         * 改成用侧栏列真实起点 —— 但**从视口算** (Fm_ColumnX), 不读
         * SidebarBounds.X: 那个全局在"全屏"态会被推成 1920 的路由阈值, 拿它当
         * 呈现位置就会把整条侧栏画到屏外/地图中间 (切换瞬间的残留)。*/
        colX = Fm_ColumnPlace();
        Fm_ClientOrigin(&ox, &oy);
        dst.X = ox + colX + src.X;
        dst.Y = oy + src.Y;
        dst.Width = src.Width;
        dst.Height = src.Height;
        if (!Fm_FitRectToSurf(&dst, &src, primW, primH))
            return (unsigned long)(size_t)Fm_Ret4;
        vt = *(void***)(size_t)prim;
        fn = (FnCopyFromPart)vt[VT_COPY_FROM_PART >> 2];
        Fm_ClearStripOnce(&src, stripW, stripH);
        Fm_CoverColumnTail(prim, &dst, primW, primH);
        /* 原版 blit 前那笔光标烙印: 横坐标基准必须与上面的 blit 目标同源,
         * 所以只能排在 colX 算出来之后 (原版本来也在 blit 之前)。*/
        Fm_CursorStampStrip(mouse, strip, colX);
        /* 小地图上屏**不再在这里做**: 同一块矩形每帧已经由 FM_DrawRadarTo 画进
         * Composite (消息帧链路里), 而 present 会把 Composite 整幅送上屏 —— 这里
         * 再 blit 一次到 Primary 是纯重复 (实测 32~40us/帧), 且两面相同时更是
         * 逐像素重写。脏标记/抹黑/光标烙印这些副作用照做 (它们和呈现语义绑定),
         * 只省掉 CopyFromPart 那一趟。回退: 把下面这行 fn(...) 的注释去掉。 */
        (void)fn;
    }
    else if (!s_Warned && !Fm_ModeBare())
    {
        s_Warned = 1;
        LogLine("fullmap: radar rect invalid, strip not shown");
    }
    return (unsigned long)(size_t)Fm_Ret4;
}
/* 小地图画进 Composite (不再依赖侧栏上屏时机, 构造上不闪)。 */
static int s_RadarDupLogged = 0;
int FM_DrawRadarTo(void* surf)
{
    RectangleStruct src, dst;
    void* strip;
    void** vt;
    FnCopyFromPart fn;
    int colX;
    int stripW = 0, stripH = 0, surfW = 0, surfH = 0;

    if (!s_Enable || !Fm_ModeKeep()) return 0;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    /* 同一块小地图每帧被 blit 两次: 一次进 Composite (这里), 一次进 Primary
     * (FM_BlitSb 的侧栏呈现)。实测 32us/帧, 占整帧 1%。
     * 两面相同 (全屏 1:1) 时是逐像素重复, 直接省掉;
     * 两面不同时 present 会把 Composite 整幅送上屏, 侧栏那一次的落点被覆盖,
     * 所以这里画的才是留下的那一份 —— 两种情形都只需保留本函数。 */
    if (!s_RadarDupLogged)
    {
        s_RadarDupLogged = 1;
        if (surf == *(void**)(size_t)ADDR_SURF_PRIMARY)
            LogLine("fullmap: composite==primary, radar blit in blitsb is redundant");
        else
            LogLine("fullmap: composite!=primary, radar blit in blitsb is overwritten by present");
    }
    strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    if (!strip || !IsPlausiblePtr(strip)) return 0;
    if (!Fm_KeepRect(&src, &stripW, &stripH)) return 0;
    surfW = GtSurfGet(surf, GtVT_GET_WIDTH);
    surfH = GtSurfGet(surf, GtVT_GET_HEIGHT);

    colX = Fm_ColumnPlace();       /* 与 FM_BlitSb 同一个值, 见 Fm_ColumnPlace */

    dst.X = colX + src.X;
    dst.Y = src.Y;
    dst.Width = src.Width;
    dst.Height = src.Height;
    if (!Fm_FitRectToSurf(&dst, &src, surfW, surfH)) return 0;

    vt = *(void***)(size_t)surf;
    fn = (FnCopyFromPart)vt[VT_COPY_FROM_PART >> 2];
    if (!fn) return 0;
    Fm_ClearStripOnce(&src, stripW, stripH);
    /* 目标面比条带矮时, blit 够不到的那几条先抹黑 (见 Fm_CoverColumnTail) */
    Fm_CoverColumnTail(surf, &dst, surfW, surfH);
    fn(surf, &dst, strip, &src, 0, 1);
    return 1;
}

/* 视口 gadget 收窄: 容器摆在 ViewBounds 上会吞掉侧栏列, 在命中前收成"视口减侧栏"。
 * XYWH 也要收 (缩小档 R 高超窗口)。命中区按 A 算。 */
#define GAD_LOG_X       0x0Cu
#define GAD_LOG_Y       0x10u
#define GAD_LOG_W       0x14u
#define GAD_LOG_H       0x18u


static void* s_ViewGadget = NULL;
static unsigned s_TrimOldW = 0;
static int s_TrimLogged = 0;

/* 视口扣掉侧栏列之后的矩形。读不到 (列/视口不合理) 返回 0。 */
static int Fm_ViewNoSidebar(RectangleStruct* out)
{
    /* 用 A (满视图区): 放大后整个 A 都是地图, 命中区要盖满 A, 不能只盖 R。*/
    const RectangleStruct* vb = &s_ViewA;
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
    int gx, gy, gw, gh;

    if (!s_Enable) return;
    g = (unsigned char*)s_ViewGadget;
    if (!g || !IsPlausiblePtr(g)) return;
    if ((unsigned)(size_t)g < 0x00400000u
        || (unsigned)(size_t)g > 0x008A0000u) { s_ViewGadget = NULL; return; }
    gx = *(int*)(g + GAD_LOG_X);
    gy = *(int*)(g + GAD_LOG_Y);
    gw = *(int*)(g + GAD_LOG_W);
    gh = *(int*)(g + GAD_LOG_H);
    if (gx < 0 || gw <= 0 || gy < 0 || gh <= 0) return;
    if (!Fm_ModeTrim())
    {
        /* 不收窄时恢复成整块 A (引擎每帧从 ViewBounds 重设, 只改切模式当帧)。 */
        const RectangleStruct* vb = &s_ViewA;
        if (vb->Width < 64 || vb->Width > 8192) return;
        if (gx == vb->X && gy == vb->Y && gw == vb->Width && gh == vb->Height)
            return;
        *(int*)(g + GAD_LOG_X) = vb->X;
        *(int*)(g + GAD_LOG_Y) = vb->Y;
        *(int*)(g + GAD_LOG_W) = vb->Width;
        *(int*)(g + GAD_LOG_H) = vb->Height;
        g[0x20] = 1;
        return;
    }
    if (!Fm_ViewNoSidebar(&keep)) return;
    /* 只动"横向跨过侧栏分界线"那一个; 别的 gadget 不归我们管。引擎每帧从
     * ViewBounds 重设, 所以每次命中都要重做。*/
    if (gx == keep.X && gy == keep.Y && gw == keep.Width && gh == keep.Height)
        return;
    *(int*)(g + GAD_LOG_X) = keep.X;
    *(int*)(g + GAD_LOG_Y) = keep.Y;
    *(int*)(g + GAD_LOG_W) = keep.Width;
    *(int*)(g + GAD_LOG_H) = keep.Height;
    g[0x20] = 1;                         /* NeedsRedraw */
    /* 日志只在形状真的变了时打一条 (缩放档下 Y/Height 一直跟 R 变, 否则刷屏)。*/
    {
        unsigned sig = (unsigned)(((gw & 0xFFFFu) << 16) | (gh & 0xFFFFu));
        if (s_TrimOldW != sig || s_TrimLogged == 0)
        {
            char t1[16], t2[16], buf[160];
            unsigned q = 0;
            s_TrimOldW = sig;
            s_TrimLogged = 1;
            q = AppendCStr(buf, q, "fullmap: view gadget ", sizeof(buf));
            UtoA((unsigned)gx, t1); q = AppendCStr(buf, q, t1, sizeof(buf));
            q = AppendCh(buf, q, ',', sizeof(buf));
            UtoA((unsigned)gy, t2); q = AppendCStr(buf, q, t2, sizeof(buf));
            q = AppendCh(buf, q, ' ', sizeof(buf));
            UtoA((unsigned)gw, t1); q = AppendCStr(buf, q, t1, sizeof(buf));
            q = AppendCh(buf, q, 'x', sizeof(buf));
            UtoA((unsigned)gh, t2); q = AppendCStr(buf, q, t2, sizeof(buf));
            q = AppendCStr(buf, q, " -> ", sizeof(buf));
            UtoA((unsigned)keep.X, t1); q = AppendCStr(buf, q, t1, sizeof(buf));
            q = AppendCh(buf, q, ',', sizeof(buf));
            UtoA((unsigned)keep.Y, t2); q = AppendCStr(buf, q, t2, sizeof(buf));
            q = AppendCh(buf, q, ' ', sizeof(buf));
            UtoA((unsigned)keep.Width, t1); q = AppendCStr(buf, q, t1, sizeof(buf));
            q = AppendCh(buf, q, 'x', sizeof(buf));
            UtoA((unsigned)keep.Height, t2); q = AppendCStr(buf, q, t2, sizeof(buf));
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
    /* 只认 gamemd 自己映像里的 gadget (往别处写即内存破坏)。 */
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
    /* 认它 = 矩形恒等于 ViewBounds (判据只比 X/Width, Y/H 随 R 变认不准)。 */
    if (gw <= 0) return;
    if (gw != vb->Width) return;
    if (gx != vb->X) return;
    /* 且必须横向盖住侧栏列, 否则收窄它没有意义 */
    if (gx + gw <= sb->X) return;
    if (gx >= sb->X + sbw) return;
    s_ViewGadget = gadget;
    /* 此刻改矩形能立刻影响这次命中 (钩子跑在 ExtractGadgetAt 之前)。 */
    FM_TrimViewGadget();
}

/* ---------------- 小地图点击门: 吞掉穿透, 跳转交给原生 ----------------
 * 引擎把侧栏顶部这一块当普通地图点击 (框单位/打靶/下单), 所以在
 * `DisplayClass::ProcessClickCoords` 里挂一道门: 落点在那块矩形内就跳 0x6925FC
 * (原函数的"未处理"出口, 栈形在该落点对齐) 吞掉。
 * ★ 视口跳转交给**引擎原生** `RadarClass::Action` (0x6539D0), 这里一个字都不算 ——
 * 靠 `FM_Install` 里 0x653A3F 那处补丁把"鼠标X -> 侧栏面内X"的偏移改成读
 * SidebarBounds.X, 它不生效小地图点击就完全没反应。别在这里自己换算坐标, 也别调
 * 0x656750/0x578080 顶替原生链 (会重复跳/跳错)。
 * 只吞左键 (右键是取消/强制移动)。"一次按下只处理一次"由门里直接读
 * GetAsyncKeyState + 已处理标记实现 —— 不能靠 FM_Poll 采边沿: 点击事件与 prepare
 * (一帧能被调好几次) 的先后不可控, 边沿可能在 press 之后才采到。
 * 命中范围是侧栏顶部那一块 (Fm_KeepTopRect), **不能**用 Fm_KeepRect: 态0 它是
 * 整条侧栏面, 会把页签/cameo 一起吞掉 (那两样点了是要下单的)。吞没只发生在
 * ProcessClickCoords 里, 侧栏自己的 widget 分派 (0x6D0680) 不受影响。
 * 联机安全: 只动本地摄像机, 不写模拟对象、不发事件。*/

/* ---------------- 小地图点击: 自己接手 + 吞掉穿透 ----------------
 * 引擎把这一块当普通地图点击 (框单位/打靶/下单), 所以在
 * DisplayClass::ProcessClickCoords 里挂一道门: 落点在那块小地图矩形内就自己
 * 完成视口跳转, 再跳 0x6925FC (原函数的"未处理"出口, 栈形在该落点对齐) 吞掉。
 * 只吞左键 (右键是取消/强制移动); "一次按下只处理一次"由门里直接读
 * GetAsyncKeyState + "已处理"标记实现 —— 不能靠 FM_Poll 采边沿, 点击事件与
 * prepare (一帧能被调好几次) 的先后不可控, 边沿可能在 press **之后**才采到。
 * 命中范围是**侧栏顶部那一块** (资金条 + 小地图 + 那两个按钮, Fm_KeepTopRect):
 * 这一块是我们画的 UI, 点击不该再落到地图上。⚠️ **不能**用 Fm_KeepRect ——
 * 态0 它是整条侧栏面, 缩放档开门后会把页签/cameo 一起吞掉 (那两样点了是要
 * 下单的)。吞没只发生在 ProcessClickCoords 里, 侧栏自己的 widget 分派
 * (0x6D0680) 不受影响, 顶部两个按钮的原生响应照旧。
 * 联机安全: 只动本地摄像机, 不写模拟对象、不发事件。 */
unsigned FM_RadarClickGate(void* regs)
{
    RectangleStruct radar, keep;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int colX, mx, my, kx, ky, lb;

    (void)regs;
    if (!g_Log) return 0;
    lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    if (!lb) { s_LbPress = 0; return 0; }  /* 松开: 下一次按下重新允许处理 */
    if (s_LbPress) return 0;              /* 本次按下已处理过 (按住拖动不重复) */
    s_LbPress = 1;
    if (!s_Enable || !Fm_ModeKeep() || !Fm_ModeGate()) return 0;
    if (!Fm_RadarRect(&radar)) return 0;
    if (!Fm_KeepTopRect(&keep)) return 0;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    /* 列起点与呈现位置同源 (Fm_ColumnX), 不读 SidebarBounds.X: 那个全局引擎
     * 会按当前 ViewBounds (= R) 重算, 缩小档下瞬时值不是列起点。与
     * FM_PtInKeepRegion 用同一个函数, 两边判定才不可能打架。 */
    colX = Fm_ColumnX(0);
    if (colX < 0) colX = sb->X;
    if (colX < 0 || colX > 8192) return 0;

    /* 顶部那一块整块吞掉: 这一块是面板, 点击不该穿透到地图 */
    kx = colX + keep.X;
    ky = keep.Y;
    if (mx < kx || mx >= kx + keep.Width) return 0;
    if (my < ky || my >= ky + keep.Height) return 0;

    /* 落在小地图本体之外的 (那两个按钮 / 资金条) 也吞掉, 只是不跳 */
    return (unsigned)ADDR_CLICKGATE_SKIP;
}

/* 物理游戏坐标是否落在小地图矩形内。PiP 用它区分"点小地图 = 给小窗定位"与
 * "点别处 = 失焦"。列起点必须与 FM_RadarClickGate 同源 (Fm_ColumnX)。 */
int FM_RadarHit(int gx, int gy)
{
    RectangleStruct radar;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int colX;

    if (!Fm_RadarRect(&radar)) return 0;
    colX = Fm_ColumnX(0);
    if (colX < 0) colX = sb->X;
    if (colX < 0 || colX > 8192) return 0;
    gx -= colX + radar.X;
    gy -= radar.Y;
    return (gx >= 0 && gx < radar.Width && gy >= 0 && gy < radar.Height) ? 1 : 0;
}

/* 侧栏列在屏幕 (合成面) 上的起点 X; 拿不到返回 -1。 */
int FM_SidebarColumnX(void)
{
    int colX = Fm_ColumnPlace();   /* 无效时自己退回"贴右缘" */
    if (colX < 0 || colX > 8192) return -1;
    return colX;
}

/* Syringe 入口 (cdecl, regs 未用; 返回值 = 继续执行的地址, 0=走原版) */
extern "C" __declspec(dllexport) unsigned __cdecl FullMap_RadarClickGate(void* regs)
{
    return FM_RadarClickGate(regs);
}

/* ---------------- GUI 设置菜单接口 ---------------- */

int FM_HideState(void)
{
    FM_EnsureInit();
    if (s_WantState >= 0) return s_WantState;
    if (s_MenuOff || !s_Enable) return 0;
    if (s_LockedFull) return 1;
    return 1 + s_Mode;
}

void FM_SetHideState(int state)
{
    static const char* const kState[4] = { "off", "full", "radar", "bare" };
    char buf[64];
    unsigned p = 0;

    if (state < 0) state = 0;
    if (state > 3) state = 3;
    FM_EnsureInit();

    if (state != 0 && !s_Enable && !s_PatchesInstalled)
    {
        /* 启动即禁用 ⇒ 全宽视口补丁没装, 运行期接管会把侧栏画到面外。
         * 记下选择, 让 ini 在下次启动时按正常路径生效。 */
        s_WantState = state;
        LogLine("fullmap: hide state saved (restart required)");
        return;
    }
    s_WantState = -1;

    if (state == 0)
    {
        /* 菜单"禁用": 内部回完全显示 (原版视口 + 整条侧栏), 只是不响应切换键。
         * 下次启动按 HideEnable=0 真正关掉 (缩放开着时仍锁完全显示补偿视口)。*/
        s_MenuOff = 1;
        s_Mode = FM_MODE_FULL;
        s_LockedFull = 0;
    }
    else
    {
        s_MenuOff = 0;
        s_Mode = state - 1;
        s_LockedFull = 0;
        if (!s_Enable)
        {
            s_Enable = 1;
            Fm_ViewPatchSet(s_Mode != FM_MODE_FULL);
        }
    }

    /* 与切换键同一条收尾: 侧栏强制全量重画 + 清输入/悬停态。 */
    *(unsigned char*)(size_t)ADDR_SIDEBAR_REDRAW = 1;
    *(unsigned char*)(size_t)ADDR_SIDEBAR_NEEDRD = 1;
    *(unsigned char*)(size_t)ADDR_SIDEBAR_BGNEEDRD = 1;
    s_RadarLockOk = 0;
    s_LbPress = 0;
    s_GadgetsArmed = 0;
    s_ForceRedraw = Fm_ModeFull() ? 8 : 0;
    s_ForceRedrawFrame = -1;

    p = AppendCStr(buf, 0, "fullmap: menu state -> ", sizeof(buf));
    p = AppendCStr(buf, p, kState[state], sizeof(buf));
    LogLine(buf);
}
