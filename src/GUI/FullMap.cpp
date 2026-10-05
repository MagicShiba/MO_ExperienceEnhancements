/*
 * FullMap.cpp - 隐藏原版建造/生产侧栏 (页签+cameos+电力条), 地图铺满窗口宽度,
 * 小地图 (RadarClass) 仍留在原来那一角 (NOD=右上 / GDI=左上)。
 * F11 三态: 0=完全显示 1=只显示小地图和顶部按钮 2=全屏 (侧栏一点不显示)。
 * 详细地址表/根因/不变量见 notes/隐藏侧栏.md。
 *
 * 引擎结构要点 (gamemd.exe 反汇编实证):
 *   MouseClass/TabClass/SidebarClass/PowerClass/RadarClass/DisplayClass 共用
 *   单例槽 0x87F7E8。
 *   视口宽度纯函数 0x72AD20 输出 {0,0,客户宽-168,客户高-32}; Composite 的建面
 *   矩形与 Set_View_Dimensions 的入参都是它的输出, 所以改这一个函数, 面和视口
 *   一起变全宽。
 *   每帧 (TabClass::Draw 0x6D0A20): 侧栏背景 -> 小地图 -> SidebarClass::Draw
 *   (面板) -> BlitSidebar (0x6A70E0) 整条 blit 到 Primary。面板与小地图在同一块
 *   离屏面 Sidebar (0x887300) 上, 所以"不画面板"不保小地图, 必须接管那次 blit。
 *   地图是**增量**画进地图面 (0x8872FC) 的, 只有标脏的格子才重画; 换视口宽度
 *   不会标脏 ⇒ 必须自己催 (Fm_DirtyRedraw)。
 *
 * 联机安全: 只写视口矩形/渲染面/gadget 标志, 全是本地渲染与输入状态, 每客户端
 * 按自己窗口尺寸算, 不碰任何模拟内存, 不发事件。
 */

#include <windows.h>
#include "GdiText.h"
#include "FullMap.h"
#include "ScaleBar.h"
#include "MOExperience.h"

#define FM_SECTION "FullMap"

/* ---------------- 地址 (反汇编实证) ---------------- */

#define ADDR_VIEWBOUNDS       0x00886FA0u
#define ADDR_SIDEBARBOUNDS    0x00886F90u
#define ADDR_WINDOWBOUNDS     0x00886FB0u
#define ADDR_SIDEBAR_W        0x007F5BF8u   /* 168 */
#define ADDR_ISNOD_SIDEBAR    0x00A8EB7Cu   /* byte: 1=侧栏在右(苏联), 0=在左(盟军) */
#define ADDR_ARMAGEDDON       0x00A8ED6Bu   /* byte: 末日模式 (视口 X 偏移也不加 168) */
#define ADDR_RADAR_RECT       0x008809F4u   /* RadarClass+0x120C: 雷达绘制矩形(侧栏面内) */
#define ADDR_SIDEBAR_DIRTY    0x00B0B518u   /* byte: TabClass::Draw 每帧置 1 */
#define ADDR_SIDEBAR_TOUCH    0x00B0B519u   /* byte */
#define ADDR_SIDEBAR_REDRAW   0x00884B90u   /* byte (this+0x53A8) */
#define ADDR_SIDEBAR_NEEDRD   0x00884B8Eu   /* byte (单例+0x53A6) */
#define ADDR_SIDEBAR_BGNEEDRD 0x00884B8Fu   /* byte (this+0x53A7) */
#define ADDR_GAME_HWND        0x00B73550u
#define ADDR_WWMOUSE          0x00887640u
#define ADDR_SURF_SIDEBAR     0x00887300u
#define ADDR_SURF_PRIMARY     0x00887308u
#define ADDR_SURF_MAP         0x008872FCu   /* 地图面 (Render 拿它当 CopyFromPart 的源) */
#define ADDR_SURF_COMPOSITE   0x0088731Cu
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
 *  c) 0x654332 ★ RadarClass 摆三个 gadget 的 X (0xB04978/0xB04910/0xB04A10)。
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
static int s_ToggleVk = 0x7A;   /* VK_F11 */
static int s_Enable = 0;
static int s_ExtraH = 5;
static int s_JumpLogged = 0;
/* 左键"本次按下已处理"标记 —— 只由点击门维护 (读 GetAsyncKeyState, 松开时清) */
static int s_LbPress = 0;
static int s_Warned = 0;
static int s_SurfWarned = 0;
static int s_ClickLogged = 0;
static unsigned s_nPoll = 0;
static unsigned s_nBlit = 0;
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
    s_Enable = GetPrivateProfileIntA(FM_SECTION, "Enable", 0, g_IniPath) ? 1 : 0;
    s_ExtraH = GetPrivateProfileIntA(FM_SECTION, "ExtraH", 5, g_IniPath);
    if (s_ExtraH < 0) s_ExtraH = 0;
    if (s_ExtraH > 64) s_ExtraH = 64;
    s_ToggleVk = GetPrivateProfileIntA(FM_SECTION, "ToggleVk", 0x7A, g_IniPath);
    if (s_ToggleVk <= 0 || s_ToggleVk > 255) s_ToggleVk = 0x7A;
    s_Mode = GetPrivateProfileIntA(FM_SECTION, "Mode", FM_MODE_FULL, g_IniPath);
    if (s_Mode < FM_MODE_FULL || s_Mode > FM_MODE_BARE) s_Mode = FM_MODE_FULL;
    if (!s_Enable) return;
    p = 0;
    p = AppendCStr(buf, p, "fullmap: en=", sizeof(buf));
    UtoA((unsigned)s_Enable, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " mode=", sizeof(buf));
    UtoA((unsigned)s_Mode, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " vk=", sizeof(buf));
    UtoA((unsigned)s_ToggleVk, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
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
 * 可点, 只有态1 开这道门。 */
static int Fm_ModeGate(void)     { return Fm_ModeFull() ? 0 : !Fm_ModeBare(); }
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
    /* 态2 一块都不保留 (小地图也不画); 态0 是整条侧栏面 */
    if (!Fm_ModeKeep()) return 0;
    if (Fm_ModeFull())
    {
        if (sw < 32 || sh < 32) return 0;
        out->X = 0; out->Y = 0; out->Width = sw; out->Height = sh;
        return 1;
    }
    k = sb->Y;
    if (k < 32 || k > 512) k = 158;
    k += s_ExtraH;                        /* 用户要求: 再多露一点 */
    if (k > sh) k = sh;
    if (k < 32) return 0;
    out->X = 0; out->Y = 0; out->Width = sw; out->Height = k;
    return 1;
}
/* ---------------- 把侧栏面里除保留区以外的部分抹掉 ----------------
 * 面板是**被画进 Composite** 的 (Render 之后 DSurface::Temp 就是 Composite),
 * 而 BlitSidebar 的两个调用点都在补丁内, 追不出是谁画的 ⇒ 从源头堵: 每次接管
 * blit 时把侧栏面里保留区以外的部分全抹黑, 无论哪条路去呈这块面都最多呈出
 * 保留的那一块。态0 不抹 (会把引擎刚画好的面板擦掉); 态2 连小地图也不要,
 * keep 传 NULL = 整块抹。 */
static void Fm_ClearStripExceptRadar(const RectangleStruct* keep)
{
    void* strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    void** vt;
    FnFillRect fn;
    RectangleStruct r;
    int sw = 168, sh = 1080;
    int w, h, top, bot;
    /* 完全显示: 不抹任何东西, 否则会把引擎刚画好的面板擦掉 */
    if (Fm_ModeFull()) return;
    if (!strip || !IsPlausiblePtr(strip)) return;
    w = GtSurfGet(strip, GtVT_GET_WIDTH);
    h = GtSurfGet(strip, GtVT_GET_HEIGHT);
    if (w <= 0 || h <= 0) { sw = 168; sh = 1080; }
    else { sw = w; sh = h; }
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
 * ★ 光改 0x72AD20 不够 —— `GScreenClass::CreateAll` (0x533FD0) 有三个调用点,
 *   两个用它的输出, 第三个 (**新开一局** 0x6BDE14) 自己算:
 *     0x6BDDC8 `sub eax,edx` -> `nop nop` : 让它不减 168。
 *   ⚠️ **绝不能写成 `sub eax,eax` (2B C0)** —— 那是"结果恒 0"而不是"不减 168",
 *     CreateAll 里两处 `test W; jle` 被跳过 ⇒ [0x88731C]/[0x8872FC] 保持 NULL
 *     ⇒ 0x6D410D 解引用空指针, **新游戏必崩**。"让算术不生效"只能 nop。
 * ★ 不要钩 CreateAll 改建面矩形: 崩 (函数里有未判空的
 *   `mov edx,[eax]; call [edx+0x18]`)。改调用点的字节就行。 */

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

/* ★ 宿主映像在不在。本模块由 DllMain 调用, 而 Syringe/CnCNet 链路上它可能在
 * gamemd 映像还没落到 0x400000 时就被装进某个进程 —— 那时读 0x72AD69 会
 * 0xC0000005。所有落点都是 gamemd 里的**绝对地址**, 宿主不在就一个字节都
 * 不写 (原版几何, 侧栏照旧), 比半装状态好得多。
 * 判据: 0x400000 已提交且是 PE 头, 且该模块文件名是 gamemd.exe / ra2md.exe。
 * ⚠️ 别只查 GetModuleHandleA("gamemd.exe") —— 换个壳启动就查不到, 功能会
 *   静默失效。 */
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
    ok = (lstrcmpiA(base, "gamemd.exe") == 0) || (lstrcmpiA(base, "ra2md.exe") == 0);
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
    /* ★ 从**当前字节**判态, 不许"假定自己装过"。别拿"相反态"当预期: 初始态
     * (Mode=0) 磁盘上本来就是原版态, 拿全宽态当预期必然 mismatch, 于是
     * s_ViewPatched 被打死成 -2, **之后 0x72AD20 永远不会被打开** ——
     * 态1/2 下另外三十个调用点仍拿 1752 宽矩形去配 1920 的 ViewBounds = 花屏。*/
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

static void Fm_DisarmSidebarGadgets(int colX, int topH)
{
    int i;
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

/* 侧栏列起点: 引擎判定"鼠标在不在侧栏上"、小地图/按钮摆放、cameo/页签滚动
 * 偏移全用它。引擎自己算的是 `SidebarBounds.X = viewRect.X + viewRect.W`
 * (0x6A51C0, viewRect 来自 0x72AD90) —— 视口铺满后那个和从 1752 变成 1920,
 * 于是 radar gadget 被摆到屏外、InitGUI 里 `mov [0xB0B3E4], -SidebarBounds.X`
 * 之类一次性写入也全错 168 ⇒ 侧栏布局混乱。
 * 所以每循环按"视口右边界 - 列宽"改回来, 并且必须在 InitGUI (0x6ABD30)
 * **之前**就改好 —— 那批一次性写入只发生一次, 下一帧再纠正已晚 (MOExperience.cpp
 * 在 InitGUI 入口挂了裸钩)。
 * ⚠️ 不要补 0x72AD90: W 变 1920 会让 SidebarBounds.X 更错。
 * ⚠️ 必须跟着**实际视口宽度**走, 不看模式 (切换当帧两者会短暂不一致)。 */
typedef void (__thiscall *FnSyncViewBounds)(void* tactical, void* rect);
#define ADDR_SYNC_VIEW       0x006D5F60u   /* TacticalClass 视口同步 (Set_View_Dimensions 内部第二步) */
/* ★ `[0xB0CE28/2C/30/34]` = ViewBounds 的**缓存副本**, 由 0x6D5F60 抄。引擎有两处
 * 读的是这份缓存而不是 ViewBounds:
 *   `Update_Tactical_Position` (0x6D8B30): TacticalPos = 中心 - [0xB0CE30/34]/2
 *   `ClientToCoords`           (0x6D2280): 用它判点在不在视口内
 * 所以缓存必须**始终等于 R**。本模块为修侧栏布局有一处故意拿 **A** 调
 * `DisplayClass::Set_View_Dimensions`, 顺手就把缓存留成了 A ⇒ TacticalPos 按 A 算
 * 而渲染按 R ⇒ 恒定偏移 (A-R)/2, 放大档下 = 视野整体偏左上 (左上出界/右下少一圈)
 * + 引擎悬浮提示错位(与我们自己那份重影)。详见 notes §2.1。*/
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

/* DisplayClass::Set_View_Dimensions (0x4A8960): 视口尺寸变化的**引擎正门**。
 * 比 SetTacticalViewBounds 多做三件我们必须一起做的事:
 *   (a) 用 ViewBounds 重建两个可见区裁剪器 [0x887644] / [0x87E8A4]
 *   (b) 重建 WWMouseClass 的视口 (vtable+0x0C)
 *   (c) 末尾 call InitGUI 重排侧栏
 * 建面走 Fm_ViewRectStub (恒全宽) 时 (a)(b) 会按全宽建好; 我们随后把视口收回
 * clientW-168, 不同步 ⇒ 引擎内部长期"视口 1752 / 裁剪 1920", 鼠标落在侧栏列上
 * 会被当成"还在地图里"去查格, 那一格在地图外 ⇒ 侧栏标题栏报"未探索的区域"。
 * this = 0x87F7E8 (与 MouseClass/TabClass/... 共用槽)。*/
typedef void (__thiscall *FnSetViewDims)(void* self, const void* rect);
#define ADDR_SET_VIEW_DIMS   0x004A8960u
#define ADDR_DISPLAY_INSTANCE 0x0087F7E8u
/* 重入保护: 它末尾会调 InitGUI, 侧栏布局里可能又回到本模块 */
static int s_InSetViewDims = 0;
/* 一帧只允许重建一次裁剪器 (prepare 一帧能跑上百次) */
static int s_SyncViewFrame = -1;

/* 当前视口是不是"全宽"那套 (态1/2)。按 A 与客户宽比, 而不是按模式 ——
 * 切换的当帧两者会短暂不一致, 而侧栏列起点必须跟着**实际视口**走。
 *
 * ★ 缩放后引擎拿到的是 R (居中那一小块), 但**屏幕布局**相关的量一律按 A 算:
 * 侧栏列起点 / 地图右边界 / 视口 gadget 命中区都跟着 R 走的话, 放大时侧栏会被
 * 摆到屏幕中间 (R 右边界 = 1095/1314), 凭空多出第二条侧栏。 */static int Fm_ViewWide(void)
{
    const RectangleStruct* vb = &s_ViewA;
    RectangleStruct* wb = (RectangleStruct*)(size_t)ADDR_WINDOWBOUNDS;
    if (wb->Width < 64 || wb->Width > 8192) return 1;
    return (vb->Width >= wb->Width - 8) ? 1 : 0;
}

/* 侧栏列起点算出来 (bare=1 时是"全屏"那套路由阈值)。返回 -1 = 数据不可信。
 *
 * bare=0 是"侧栏列真的存在"的几何: 紧贴视口右边界 (苏联) 或左边界 (盟军)。
 * 全宽视口时视口多出那 168, 起点要减掉它才落在屏幕内 (1920-168=1752);
 * 原版视口 (1752) 时不能减 —— 减了会变成 1584, 而 InitGUI 用它摆三个 radar
 * gadget 且摆完不再重算, 小地图/顶部按钮会永远偏 168、点了没反应。
 *
 * bare=1 动 SidebarBounds.X 是因为引擎有两处拿它当**鼠标路由**阈值:
 *   苏联 0x478BD7 (已改读 SidebarBounds.X): `cmp mouseX,(ViewBounds.X+
 *     SidebarBounds.X)` ⇒ 全屏态要让它 = 视口右边界。
 *   盟军 0x478BF4: `cmp mouseX,(SidebarBounds.X+SidebarBounds.Width)`
 *     ⇒ 全屏态要让它 = -(168+8)。
 * 两者都让任何鼠标 X 判成地图。0x478DBE/0x478DD6 (鼠标在不在侧栏列 ⇒ 强制
 * 面板全量重画) 用同一组阈值, 取值后恒不成立, 顺带避免每次点击都强刷面板。 */
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

static void Fm_SyncColumn(int bare)
{
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int colX;
    if (!s_Enable) return;
    colX = Fm_ColumnX(bare);
    if (colX < 0) return;
    if (sb->X != colX) sb->X = colX;
}

/* InitGUI 入口钩专用: 永远按"侧栏存在"的几何纠正。
 * 那一批一次性写入 (cameo/页签滚动偏移 = -SidebarBounds.X) 必须用这个值, 否则
 * 全屏态下开局的话偏移会按 1920 算, 之后切回完全显示/只显示小地图时侧栏布局错 168。 */
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
    /* 视口右边界: 态0 = 1752 (侧栏在它右边), 态1/2 = 1920 (侧栏没了)。跟着它走
     * ⇒ 两种形态各自贴边, 就是"适应侧栏显示/隐藏"。
     * ⚠️ 别改回 Fm_ColumnX(0) (三态恒 1752): 隐藏侧栏后列表会缩在 1752。*/
    r = vb->X + vb->Width;
    if (r < 64 || r > 8192) return 0;
    return r;
}

/* ---------------- 视口宽度: 原版 (clientW-168) <-> 全宽, 动态切 ----------------
 * ★ 三条腿, 缺一条就出花屏/残影:
 *   (1) **字节窗口按模式翻**。0x72AD20 全仓有 32 个调用点 (CreateAll 两处 +
 *       Set_View_Dimensions + 0x63xxxx 那一片格/视图求交 + 0x77830C 的缩放
 *       中心); 只改 ViewBounds 不够, 那些地方会继续拿到全宽矩形。态0 把两个
 *       窗口恢复原字节, 引擎所有几何判定于是自动正确。
 *   (2) `Fm_ApplyViewWidth` 每循环纠正 ViewBounds 并同步 view_bound —— 引擎
 *       自己不会重算它 (只有建面那条路才会, 而建面矩形我们在 (3) 里始终给
 *       全宽), 所以这一步必须每循环做。
 *   (3) 建面永远全宽: CreateAll 两个调用点改指 `Fm_ViewRectStub`, 新开局那条
 *       (0x6BDE14) 走 nop。态0 期间改分辨率也不会把渲染面缩窄。
 * 安全阀: 视口不许比 Composite 实际宽度还大 (面按旧宽度建时会有), 夹窄而不
 * skip —— 视口比面宽时引擎照旧往面外画, 那就是花屏的直接原因。 */

/* 两个字节窗口的"已知两态"字节 (供探测用):
 *   原版态: 0x72AD69 imm32 = A8 00 00 / 0x72AD60 = B9 A8 00 00 00
 *   全宽态: 0x72AD69 imm32 = 00 00 00 / 0x72AD60 = 90 x5            */
static const unsigned char kVanSub[3] = { 0xA8, 0x00, 0x00 };
static const unsigned char kVanGdi[5] = { 0xB9, 0xA8, 0x00, 0x00, 0x00 };
static const unsigned char kWideSub[4] = { 0x00, 0x00, 0x00 };
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

/* 建面用的"永远全宽"矩形。签名照 0x72AD20: ecx = 出参, 返回出参指针。
 * ★ 高度**不减**那 32 的上下栏高, 必须与"新开局"那条 (0x6BDE14) 逐字段一致:
 *   CreateAll 用这个矩形建 Composite (0x88731C) 与地图面 (0x8872FC, 高度另取
 *   WindowBounds.Height), 紧接着 Set_View_Dimensions 又拿它去重建两个裁剪器。
 *   原版函数给 H-32 ⇒ **读档**这条路 Composite 只有 1920x1048 (新开局是 1080),
 *   侧栏列底下 32 行既没被条带 blit 到 (条带 168x1080 放不下) 又不会被地图重画
 *   (视口高 1048) = 永远留着旧像素的残影带。
 *   ViewBounds.Height 由 Fm_ApplyViewWidth 立刻纠正回 clientH-32, 这里给满是安全的。 */
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

/* ---------------- 换几何后催地图重画 (残影的真正修法) ----------------
 *
 * ★ 残影的根因 (不是"呈现只搬脏区", 那个方向是错的): 地图是**增量**画进地图面
 *   (0x8872FC) 的, 只有标脏的格子才重画; 而视口宽度的所有改法都**只改矩形,
 *   一个格子都不标脏**。态0 (窄) -> 态1/2 (宽) 时右边新露出的 168px 从没进过
 *   脏队列, 留的还是上一模式的像素; 它跟着地图一起动 (都在同一块面里), 所以看着
 *   像"贴在地图上"。反方向 (缩窄) 同样会留。
 *
 * 修法: 几何变了之后连着几帧把**整窗**推进脏区队列, 让引擎把地图重画一遍。
 *   `TacticalClass::RegisterDirtyArea` = 0x6D2790, 坐标**客户区相对**
 *   (Phobos/Ares 的调用点都先减 TacticalPos, 因为它们传绝对矩形);
 *   **不碰 TacticalPos**, 视口不许跳。只推渲染脏区, 联机安全。
 * ★ 标**整窗**而不是标视口: 视口宽度一变, 视口外那条 (侧栏列所在) 从来没被
 *   引擎当视口的一部分标过脏, 留的还是上一模式的像素; 只标视口治不好那条带。 */
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
    if (fn) fn(tac, r, 1);
    s_DirtyFrames--;
}

/* 本循环用的两个视口矩形 (给 ScaleBar 的拉伸与鼠标反变换)。 */
void FM_GetViewRects(RectangleStruct* a, RectangleStruct* r)
{
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
    if (x < 0) x = 0;
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
        /* A 永远记"引擎本该用的满视图区", 与缩放无关。 */
        s_ViewA.X = x; s_ViewA.Y = 0; s_ViewA.Width = w; s_ViewA.Height = h;
        if (SB_GetViewRect(&r))
        {
            x = r.X;
            y = r.Y;
            w = r.Width;
            h = r.Height;
        }
        else { y = 0; }
        sig = (int)(SB_Scale() * 256.0);
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
    /* 早退前也自愈一次: 下面那个"拿 A 调 Set_View_Dimensions"的分支会把缓存
     * 留在 A 上 (见 Fm_SyncViewCache), 而几何没变时这里直接 return, 救不回来。*/
    if (vb->X == x && vb->Y == y && vb->Width == w && vb->Height == h)
    {
        Fm_SyncViewCache(tac, w, h);
        return;
    }

    oldW = vb->Width;
    oldH = vb->Height;
    vb->X = x;
    vb->Y = y;
    vb->Width = w;
    vb->Height = h;
    /* 几何变了 => 催引擎把整块重画一遍, 否则视口外那条留残影。判"几何变了"
     * 而不是"宽度变了": 态0 <-> 态1/2 的高度/起点也可能不同。
     * 连续几帧都推, 覆盖"切模式那一帧 + 之后重建面/改分辨率"的窗口。 */
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
                q = AppendCStr(buf, q, " -> ", sizeof(buf));
                UtoA((unsigned)w, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
                q = AppendCh(buf, q, 'x', sizeof(buf));
                UtoA((unsigned)h, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
                q = AppendCh(buf, q, '@', sizeof(buf));
                UtoA((unsigned)x, b3); q = AppendCStr(buf, q, b3, sizeof(buf));
                q = AppendCStr(buf, q, " (dirty redraw queued)", sizeof(buf));
                LogLine(buf);
            }
        }
    }
    tac = *(void**)(size_t)0x00887324u;
    if (tac && IsPlausiblePtr(tac))
    {
        /* ★ 换宽度时把**左边界**钉住 (用户要求"左侧固定, 右侧收缩或扩展")。引擎存的是
         * 视口中心格 (+0xD64, +0xD74 是它的目标值), 而 `0x6D8B30` 用
         * `中心 - ViewBounds.Width/2` 算出左上角 (TacticalPos) 供 cell->client
         * 换算用 ⇒ 宽度一变左边界就跑半个差值 (1920↔1752 = 84px)。把中心按
         * ±ΔW/2 挪回去左边界就不动; 只挪中心, 让引擎照旧钳位 (地图边缘该夹还夹)。 */
        d = (w - oldW) / 2;
        if (d && s_ZoomChanged)
            d = 0;              /* 只是换了缩放挡: 中心保持不动 */
        s_ZoomChanged = 0;
        if (d)
        {
            *(int*)((char*)tac + 0xD64) += d;
            *(int*)((char*)tac + 0xD74) += d;
        }
        /* 便宜的每循环路径: 只把矩形同步给 TacticalClass (照抄引擎
         * Set_View_Dimensions 的第二步)。幂等; 缓存对不上时才真调。*/
        Fm_SyncViewCache(tac, w, h);
        /* ★ 宽度真的变了还要走一次引擎自己的 Set_View_Dimensions ——
         * 只调 0x6D5F60 不会重建那两个裁剪器/鼠标视口, 引擎内部就长期停在
         * "视口 1752 / 裁剪 1920" ⇒ 鼠标落在侧栏列上被当成"还在地图里"去查格,
         * 那一格在地图外 ⇒ 侧栏标题栏报"未探索的区域"。**读档必现** (CreateAll
         * 之后紧跟一次按 1920 建裁剪器, 之后没人重建)。
         * 安全阀: 只在宽度真变时调; 引擎帧号门控一帧一次 (prepare 一帧上百次);
         * WWMouseClass 必须在 (0x4A89C2 不判空就解引用); s_InSetViewDims 防它
         * 末尾 InitGUI 重入。*/
        if (w != oldW && !s_InSetViewDims)
        {
            void* mouse = *(void**)ADDR_WWMOUSE;
            FnSetViewDims setDims = (FnSetViewDims)(size_t)ADDR_SET_VIEW_DIMS;
            int fr = *(int*)(size_t)ADDR_FRAME_COUNT;
            if (setDims && mouse && IsPlausiblePtr(mouse) && fr != s_SyncViewFrame)
            {
                s_SyncViewFrame = fr;
                s_InSetViewDims = 1;
                /* ★ 用 **A** 重建, 完事把 ViewBounds 写回 R。
                 * Set_View_Dimensions 末尾会 InitGUI, 侧栏页签/cameo 行/底部工具栏
                 * 那些"一次性写入"的偏移全按传进去的矩形算。传 R 的话引擎是按
                 * 438x262 摆的 ⇒ 侧栏下半截空掉、底部工具栏落到侧栏中段、鼠标命中
                 * 也跟着错。隐藏侧栏那套之所以没事, 正是因为它传进去的就是 A。
                 * 渲染那边仍然读 ViewBounds, 所以这里结束前必须还原成 R ——
                 * 视口小正是缩放的实现手段, 渲染期必须是 R。*/
                {
                    RectangleStruct keep = *vb;      /* = R */
                    vb->X = s_ViewA.X; vb->Y = s_ViewA.Y;
                    vb->Width = s_ViewA.Width; vb->Height = s_ViewA.Height;
                    setDims((void*)(size_t)ADDR_DISPLAY_INSTANCE,
                            (const void*)(size_t)ADDR_VIEWBOUNDS);
                    *vb = keep;                       /* 还原成 R */
                }
                s_InSetViewDims = 0;
                /* 重建裁剪器顺带改了侧栏的一次性布局 —— 立刻纠正列起点。*/
                FM_SyncSidebarColumn();
                /* 上面那次 setDims 顺手把 [0xB0CE30/34] 留成了 A ⇒ 每帧
                 * TacticalPos 按 A 算而渲染按 R, 视野恒偏 (A-R)/2。纠正回 R。*/
                Fm_SyncViewCache(tac, w, h);
                Fm_SyncViewCache(tac, w, h);
            }
        }
    }
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
    if (!s_Enable || !s_ToggleVk) return;
    fr = *(int*)(size_t)ADDR_FRAME_COUNT;
    if (fr == s_TogLastFrame) return;
    s_TogLastFrame = fr;
    if (!GtGameFocused()) { s_TogKeyDown = 0; return; }
    down = (GetAsyncKeyState(s_ToggleVk) & 0x8000) ? 1 : 0;
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

void FM_Poll(void)
{
    RectangleStruct* sb;
    RectangleStruct radar;
    RectangleStruct* vb;
    int* wb;
    int winW, sbW, colX, topH, colXcalc;

    if (!s_Enable) return;
    if (!g_Log) return;
    /* ★ 这里**不要**碰 s_LbPress: 那是点击门自己维护的"本次按下已处理"标记。
     * 门直接读 GetAsyncKeyState, 不依赖 prepare 与点击事件的先后。*/
    Fm_PollToggle();
    /* 视口宽度跟着模式走 (内部幂等)。必须在纠正侧栏列之前 —— 后者要用视口
     * 右边界。*/
    Fm_ViewPatchSet(!Fm_ModeFull());
    /* ViewBounds 引擎自己不会重算, 每循环按当前模式纠正 + 同步 view_bound。*/
    Fm_ApplyViewWidth();
    /* 催地图重画 (换几何后新露出的那条没人标脏 => 残影)。必须在
     * ApplyViewWidth 之后: 那儿才知道几何变了没有。*/
    Fm_DirtyRedraw();
    s_nPoll++;
    sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    wb = (int*)(size_t)ADDR_WINDOWBOUNDS;
    sbW = *(int*)(size_t)ADDR_SIDEBAR_W;
    if (sbW < 64 || sbW > 512) return;
    colXcalc = Fm_ColumnX(0);
    winW = wb[2];
    if (winW < 640 || winW > 8192) return;
    /* 只有态1/2 才需要小地图矩形 (态0 整条交给引擎) */
    if (!Fm_RadarRect(&radar) && Fm_ModeKeep()) return;

    /* 侧栏列起点纠正。态0 也要调: 否则从"全屏"态切回来时 SidebarBounds.X
     * 还留着 1920 的路由阈值, 整条侧栏会 blit 到屏外。*/
    Fm_SyncColumn(Fm_SidebarGone());
    /* 诊断以**模式**为触发条件 (拿 vb->Width != before 当条件永远相等, 等于
     * 没有诊断)。一次打两行: 几何 + 引擎内部与视口是否同源。*/
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
        q = AppendCStr(buf, q, " colCalc=", sizeof(buf));
        UtoA((unsigned)colXcalc, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " wide=", sizeof(buf));
        UtoA((unsigned)Fm_ViewWide(), b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " keep=", sizeof(buf));
        UtoA((unsigned)Fm_ModeKeep(), b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        okR = Fm_RadarRect(&rr);
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
            q = AppendCStr(buf, q, " map=", sizeof(buf));
            UtoA((unsigned)mwid, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCh(buf, q, 'x', sizeof(buf));
            UtoA((unsigned)mhgt, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
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
            q = AppendCStr(buf, q, " vpatch=", sizeof(buf));
            UtoA((unsigned)s_ViewPatched, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            LogLine(buf);
        }
    }
    topH = sb->Y;
    if (topH < 32 || topH > 512) topH = 158;
    /* 摘/复 gadget 一律按"侧栏列真实起点"筛选: 全屏态 sb->X 已经被推成路由阈值
     * (1920 / -176), 拿它当筛选条件会让整列 gadget 全部漏掉, 隐形按钮就又活了。*/
    colX = Fm_ColumnX(0);
    if (colX < 0)
        colX = *(unsigned char*)(size_t)ADDR_ISNOD_SIDEBAR
             ? vb->X + vb->Width - sbW : 0;
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
    /* 进态0 后压几帧强制面板全量重画 (见 Fm_PollToggle 的说明)。prepare 一帧能
     * 跑上百次, 所以用引擎帧号门控, 保证真的跨了几帧而不是一帧内耗光。放在最后:
     * 置位之后同一帧的 TabClass::Draw 就会走全量重画, FM_BlitSb 尾声再清 0x53A8。*/
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
/* ★ 不要在每帧强制面板面重画。
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

/* ---------------- 侧栏列"没被 blit 覆盖到"的那几条抹黑 ----------------
 * 态0 视口是 clientW-168, 侧栏列不在视口里 ⇒ 地图永远不会画它; 条带若比目标面
 * 矮, blit 够不到的那几条就永远留着上一帧/上一模式的像素 = 残影带。抹掉。
 * ⚠️ **态1/2 绝对不能抹**: 那两态视口整宽, 侧栏列归**地图**所有, 抹黑等于把
 *   地图擦成一条黑带 (地图是增量画的, 擦完不会自己回来)。症状: "小地图模式
 *   下侧栏列下面一片黑"。判断依据是"这一列归谁"(视口覆不覆盖它), 不是
 *   "条带有没有盖全"。
 * dst 是**已被 Fm_FitRectToSurf 夹过**的矩形。 */
static void Fm_CoverColumnTail(void* surf, const RectangleStruct* dst)
{
    void** vt;
    FnFillRect fn;
    RectangleStruct r;
    int w, h, y0, x0, cw;
    if (!Fm_ModeFull()) return;         /* 侧栏列归地图, 不许碰 */
    if (!surf || !IsPlausiblePtr(surf)) return;
    w = GtSurfGet(surf, GtVT_GET_WIDTH);
    h = GtSurfGet(surf, GtVT_GET_HEIGHT);
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

unsigned long FM_BlitSb(void)
{
    void* strip;
    void* prim;
    void* mouse;
    RectangleStruct src, dst;
    int ox, oy, sbw, winW, colX;

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

    /* 全屏: 侧栏面整块抹黑, 一笔都不上屏 (原版整条 blit 已被换掉, 所以屏幕上
     * 什么都没有 —— 地图本来就已经铺满整宽了)。 */
    if (!Fm_ModeKeep())
    {
        Fm_ClearStripExceptRadar(NULL);
        return (unsigned long)(size_t)Fm_Ret4;
    }

    /* 原版 blit 前先把光标区域从侧栏面上还原, 跟着做一次, 否则小地图上留洞 */
    if (mouse && IsPlausiblePtr(mouse))
    {
        void** vt = *(void***)(size_t)mouse;
        FnMouseFixSurface fn = (FnMouseFixSurface)vt[WW_VT_FIX_CURSOR >> 2];
        if (fn) fn(mouse, strip, 1);
    }

    if (Fm_KeepRect(&src))
    {
        void** vt;
        FnCopyFromPart fn;
        /* 原版 blit 用 `src.X + (isNOD?ViewW:0)`, 视口铺满后会把小地图顶到屏外。
         * 改成用侧栏列真实起点 —— 但**从视口算** (Fm_ColumnX), 不读
         * SidebarBounds.X: 那个全局在"全屏"态会被推成 1920 的路由阈值, 拿它当
         * 呈现位置就会把整条侧栏画到屏外/地图中间 (切换瞬间的残留)。*/
        sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
        if (sbw < 64 || sbw > 512) sbw = 168;
        winW = ((int*)(size_t)ADDR_WINDOWBOUNDS)[2];
        if (winW < 640 || winW > 8192) winW = src.X + sbw;
        colX = Fm_ColumnX(0);
        if (colX < 0) colX = winW - sbw;      /* 兜底也贴右缘, 绝不画到屏幕中间 */
        if (colX < 0) colX = 0;
        Fm_ClientOrigin(&ox, &oy);
        dst.X = ox + colX + src.X;
        dst.Y = oy + src.Y;
        dst.Width = src.Width;
        dst.Height = src.Height;
        if (!Fm_FitRectToSurf(prim, &dst, &src))
            return (unsigned long)(size_t)Fm_Ret4;
        vt = *(void***)(size_t)prim;
        fn = (FnCopyFromPart)vt[VT_COPY_FROM_PART >> 2];
        Fm_ClearStripExceptRadar(&src);
        Fm_CoverColumnTail(prim, &dst);
        if (fn) fn(prim, &dst, strip, &src, 0, 1);
    }
    else if (!s_Warned && !Fm_ModeBare())
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

    if (!s_Enable || !Fm_ModeKeep()) return 0;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    strip = *(void**)(size_t)ADDR_SURF_SIDEBAR;
    if (!strip || !IsPlausiblePtr(strip)) return 0;
    if (!Fm_KeepRect(&src)) return 0;

    sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
    if (sbw < 64 || sbw > 512) sbw = 168;
    winW = ((int*)(size_t)ADDR_WINDOWBOUNDS)[2];
    if (winW < 640 || winW > 8192) winW = 1920;
    colX = Fm_ColumnX(0);          /* 同 FM_BlitSb: 从视口算, 不读 SidebarBounds.X */
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
    /* 目标面比条带矮时, blit 够不到的那几条先抹黑 (见 Fm_CoverColumnTail) */
    Fm_CoverColumnTail(surf, &dst);
    fn(surf, &dst, strip, &src, 0, 1);
    return 1;
}

/* ---------------- 视口 gadget 收窄 ----------------
 * GadgetClass::Input (0x4E1640) 的命中测试是 ExtractGadgetAt (0x4E15A0):
 *   X/W/H 直接当屏幕坐标比, 沿 GetPrev 走; 命中里取**面积最小**的那个。
 * 输入链上只有一个 gadget, 矩形恒等于 ViewBounds (实测 {0,0,1920x1048})。
 * 态1 视口全宽 ⇒ 它把侧栏列整个吞进命中区, 那一列的点击全被判成"点在视口上";
 * 收窄成"视口减去侧栏列"即与原版几何完全一致 (苏联列在右则右边界收到列起点,
 * 盟军列在左则左边界推到列尾)。指针取自 Input 的 this —— 是真 gadget 指针。 */
#define GAD_LOG_X       0x0Cu
#define GAD_LOG_W       0x14u

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
    int gx, gw, wasX, wasW;

    if (!s_Enable) return;
    g = (unsigned char*)s_ViewGadget;
    if (!g || !IsPlausiblePtr(g)) return;
    if ((unsigned)(size_t)g < 0x00400000u
        || (unsigned)(size_t)g > 0x008A0000u) { s_ViewGadget = NULL; return; }
    gx = *(int*)(g + GAD_LOG_X);
    gw = *(int*)(g + GAD_LOG_W);
    if (gx < 0 || gw <= 0) return;
    if (!Fm_ModeTrim())
    {
        /* 不收窄 (全屏态): 命中区必须是整宽 (按 A, 缩放时整块 A 都是地图)。
         * 引擎每帧从 ViewBounds 重设, 这里只是在切模式的当帧把上一次收窄的结果
         * 主动改回去, 不依赖那套重设。*/
        const RectangleStruct* vb = &s_ViewA;
        if (vb->Width < 64 || vb->Width > 8192) return;
        if (gx == vb->X && gw == vb->Width) return;
        *(int*)(g + GAD_LOG_X) = vb->X;
        *(int*)(g + GAD_LOG_W) = vb->Width;
        g[0x20] = 1;
        return;
    }
    if (!Fm_ViewNoSidebar(&keep)) return;
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

/* ---------------- 地图格范围 + 原生视口滚动 ----------------
 * 小地图点击要把"雷达像素"换算成"地图格", 再让引擎把视口对准那一格。
 * 格范围用 Fm_ScanTacticalMapRect 只读扫描 TacticalClass 得到 (见其注释)。
 * 滚动: RadarClass::ScrollTo (0x653F70), 它是
 *   TacticalClass::SetTacticalPosition (0x6D6070) 的包装。
 * ★ 引擎自带的 RadarClass::Action (0x6539D0) 里有"点雷达跳视口"但那是死代码:
 *   入口要求 [0x880C98]==1 && [0x880C94]==1, 这两个全局全二进制只有读没有写,
 *   .data 初值 0 ⇒ 恒 bail。这条链在原版里也不通, 只能自己接。
 * 联机安全: 只动本地摄像机, 逐客户端各自决定看哪里。 */
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
 * ⚠️ 不要钩 MapClass::CreateEmptyMap (0x653F50) 拿入参: 崩 (0xC0000005,
 *   发生在跳板返回之后)。为一个矩形去钩引擎内部函数收益/风险比太差。
 * 无钩扫描: MapRect / MaxWidth / MaxHeight 都在 TacticalClass 的 0x0E0..0x200,
 * 确切偏移没核实过 (YRpp 头文件那段是占位名), 所以**不写死偏移**, 逐 4 字节
 * 试, 用两条判据筛:
 *   1) 宽高像地图尺寸: 16..512 格, X/Y 在 -64..512;
 *   2) 可见区格坐标 `[0x880C84..90]` 必须落在里面。两种语义都试:
 *      (a) {minX,minY,maxX,maxY}  (b) {X,Y,W,H}
 * 命中即锁定。区间已判过 IsPlausiblePtr, 只读, 联机安全。 */
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
 * 完成视口跳转, 再跳 0x6925FC (原函数的"未处理"出口, 栈形在该落点对齐) 吞掉。
 * 只吞左键 (右键是取消/强制移动); "一次按下只处理一次"由门里直接读
 * GetAsyncKeyState + "已处理"标记实现 —— 不能靠 FM_Poll 采边沿, 点击事件与
 * prepare (一帧能被调好几次) 的先后不可控, 边沿可能在 press **之后**才采到。
 * 命中范围是**整块保留区** (资金条 + 小地图 + 那两个按钮): 这一块是我们画的
 * UI, 点击不该再落到地图上。吞没只发生在 ProcessClickCoords 里, 侧栏自己的
 * widget 分派 (0x6D0680) 不受影响, 顶部两个按钮的原生响应照旧。
 * 联机安全: 只动本地摄像机, 不写模拟对象、不发事件。 */
unsigned FM_RadarClickGate(void* regs)
{
    RectangleStruct radar, keep;
    RectangleStruct* sb = (RectangleStruct*)(size_t)ADDR_SIDEBARBOUNDS;
    int winW, colX, mx, my, kx, ky, cx, cy, lb;

    (void)regs;
    if (!s_Enable || !Fm_ModeKeep() || !Fm_ModeGate()) return 0;
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
