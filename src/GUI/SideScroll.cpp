/* SideScroll.cpp - 原版建造/生产侧栏的竖直滚动条。
 *
 * 原版只有两个"整页"翻页按钮和滚轮 (滚轮是合成点击那两个按钮), 都是整行粗调,
 * 没法停在第 3 行。本模块在 cameo 网格右侧的空档里给一条能拖的竖条, 一像素对一行。
 * 只有滑块一样东西: 无底色/无描边/不半透明, 鼠标停进侧栏时才出现。
 *
 * 引擎事实 (gamemd 反汇编实证; 推导见 notes/侧栏滚动条.md):
 *
 *   侧栏对象            0x87F7E8  地址就是对象本身 (Mouse/Sidebar/Display 同一静态
 *                                对象)。按 *(void**)0x87F7E8 读会拿到虚表指针。
 *   Tabs[0]             this+0x1544, 步长 0xF94      ActiveTab this+0x539C
 *   StripClass          +0x44 TopRowIndex (行号)     +0x54 CameoCount
 *                      +0x48/+0x4C/+0x3E/+0x3F 滚动动画态
 *   SidebarBounds       0x886F90 {X=列起点, Y=0x9E, W=168, H}
 *   cameo 区            [0xB0B4F8] 上缘 Y, [0xB0B504] 高 = 行数*50, 行高 50
 *   cameo 列起点        [0xB0B4F4] = colX+0x16, 列距 [0xB0B4FC]=63, 盒 60x48
 *   可见 cameo 数       GetVisibleCameoCount 0x6AC430 —— 无参、不读 ecx、结尾 ret
 *
 *   ⇒ 网格占 colX+22 .. colX+145, 列宽 168, 右侧约 21px 空档 = 本条的位置。
 *
 * 联机安全: 只写 Tabs[tab]+0x44 与动画态字段, 全是本地 UI 状态。不发事件, 不碰模拟。
 *
 * 构建 build-fast.bat; 测试 test/sidescroll_geom_test.c。
 */

#include <windows.h>
#include "GdiText.h"
#include "FullMap.h"
#include "SideScroll.h"

#define SS_SECTION "Sidebar"

/* ---------------- 引擎地址 ---------------- */
#define ADDR_SB_OBJ        0x0087F7E8u  /* 侧栏对象本身 */
#define OFF_ACTIVE_TAB     0x539Cu
#define OFF_TABS           0x1544u
#define OFF_TAB_STRIDE     0x0F94u
#define OFF_STRIP_TOPROW   0x44u
#define OFF_STRIP_TARGET   0x48u
#define OFF_STRIP_PIXOFF   0x4Cu
#define OFF_STRIP_DIR      0x3Eu
#define OFF_STRIP_ANIM     0x3Fu
#define OFF_STRIP_CAMCOUNT 0x54u

#define ADDR_SB_X          0x00886F90u  /* SidebarBounds */
#define ADDR_SB_Y          0x00886F94u
#define ADDR_SB_W          0x00886F98u
#define ADDR_SB_H          0x00886F9Cu
#define ADDR_CAM_TOP       0x00B0B4F8u  /* cameo 区上缘 Y */
#define ADDR_CAM_H         0x00B0B504u  /* cameo 区高 */
#define ADDR_SB_REDRAW     0x00B0B518u  /* 侧栏脏标记 */
#define ADDR_SB_NEEDBG     0x00884B8Fu  /* this+0x53A7 页签底图重画 */

#define ADDR_SCENARIO      0x00A8B230u
#define ADDR_STARTED       0x00A8ED5Cu
#define ADDR_ISACTIVE      0x00A8E9A0u
#define ADDR_FRAME         0x00A8ED84u

/* 可见 cameo 数: 无参、不读 ecx、结尾 ret 不清栈 ⇒ __cdecl。签名猜错 = 栈被清错 =
 * 崩溃而不是返回错值。返回的是 **cameo 数** (行数*2), 不是行数。 */
typedef int (__cdecl *Fn_VisCameos)(void);
#define FN_VISCAMEOS ((Fn_VisCameos)(void*)0x006AC430u)

/* 侧栏面宽 (实测 168); 对不上就不画 —— 面被换掉/半初始化时宁可整条不画。 */
#define SS_SURF_W 168

/* ---------------- 配置 ---------------- */
static int s_Init = 0;
static int s_Enable = 1;
static int s_HitW = 14;         /* 生效宽度 6~20: 点击/拖动判定 */
static int s_ShowW = 6;         /* 显示宽度 2~20: 滑块绘制, 右对齐在生效区内 */
static int s_RightMargin = 3;   /* 离侧栏右缘 0~12 */
static int s_ThumbRgb = 0xA5D3FF;   /* 滑块颜色 0xRRGGBB */

/* ---------------- 状态 ---------------- */
static int s_LastFrame = -1;
static int s_Drag = 0;           /* 1 = 正在拖滑块 (含"刚跳完还在按住") */
static int s_Grab = 0;           /* 指针 Y - 滑块上沿; 跳转后重设成新的抓取点 */
static unsigned s_DragStrip = 0; /* 开拖那一刻锁定的 Tabs[tab] (换页签不串台) */
static int s_LbPrev = 0;
static int s_Logged = 0;

static void SS_ClampCfg(void)
{
    if (s_HitW < 6 || s_HitW > 20) s_HitW = 14;
    if (s_ShowW < 2 || s_ShowW > 20) s_ShowW = 6;
    if (s_ShowW > s_HitW) s_ShowW = s_HitW;   /* 显示条不出生效区 */
    if (s_RightMargin < 0 || s_RightMargin > 12) s_RightMargin = 3;
    if (s_ThumbRgb < 0 || s_ThumbRgb > 0xFFFFFF) s_ThumbRgb = 0xA5D3FF;
}

void SS_EnsureInit(void)
{
    char buf[128];
    unsigned p;
    if (s_Init) return;
    s_Init = 1;
    BuildPaths();
    GtEnsureLog();
    if (g_Log)
    {
        /* GetPrivateProfileIntA 直接吃 0xRRGGBB (与 [ScaleBar] Void 同格式)。 */
        s_Enable      = GetPrivateProfileIntA(SS_SECTION, "ScrollEnable", 1, g_IniPath) ? 1 : 0;
        s_HitW        = GetPrivateProfileIntA(SS_SECTION, "HitWidth", 14, g_IniPath);
        s_ShowW       = GetPrivateProfileIntA(SS_SECTION, "ShowWidth", 6, g_IniPath);
        s_RightMargin = GetPrivateProfileIntA(SS_SECTION, "RightMargin", 3, g_IniPath);
        s_ThumbRgb    = GetPrivateProfileIntA(SS_SECTION, "ThumbColor", 0xA5D3FF, g_IniPath);
        SS_ClampCfg();
        p = 0;
        p = AppendCStr(buf, p, "sidescroll: en=", sizeof(buf));
        p += UtoA((unsigned)s_Enable, buf + p);
        p = AppendCStr(buf, p, " hit=", sizeof(buf));
        p += UtoA((unsigned)s_HitW, buf + p);
        p = AppendCStr(buf, p, " show=", sizeof(buf));
        p += UtoA((unsigned)s_ShowW, buf + p);
        p = AppendCStr(buf, p, " rm=", sizeof(buf));
        p += UtoA((unsigned)s_RightMargin, buf + p);
        LogLine(buf);
    }
}

/* ---------------- 几何 ---------------- */

typedef struct {
    int colX;              /* 侧栏列起点 (屏幕 x); 侧栏面的 x 原点就是它 */
    int bx, by, bw, bh;    /* 生效矩形 (点击/拖动判定), 屏幕坐标 */
    int sx, sw;            /* 显示矩形 x/宽: 右对齐在生效区内, 高与生效区同 (by/bh) */
    int top, maxTop;       /* 当前行号 / 最大行号 */
    int thumbY, thumbH;
    unsigned strip;        /* Tabs[ActiveTab] */
} SsGeo;

/* fail-closed: 几何读不出来就不画, 绝不在野值上算东西。
 * maxTop = ceil(cnt/2) - 可见行数, 与引擎自己的翻页判据同解 (见 notes §1.3),
 * 两条控件给出的最大行号必须一致, 否则会互相打脸。 */
static int SS_Geo(SsGeo* g)
{
    int colW, camTop, camH, vis, cnt, total, maxTop, cur;
    int thumbH, span, thumbY;
    unsigned tab, strip;

    if (!IsPlausiblePtr((void*)ADDR_SB_OBJ)) return 0;
    if (!IsPlausiblePtr((void*)ADDR_SCENARIO)) return 0;
    if (!*(volatile unsigned char*)(size_t)ADDR_STARTED) return 0;
    if (!*(volatile unsigned char*)(size_t)ADDR_ISACTIVE) return 0;

    colW   = *(volatile int*)(size_t)ADDR_SB_W;
    camTop = *(volatile int*)(size_t)ADDR_CAM_TOP;
    camH   = *(volatile int*)(size_t)ADDR_CAM_H;
    if (colW < 100 || colW > 512) return 0;
    if (camTop < 0 || camTop > 4096) return 0;
    if (camH < 50 || camH > 4096) return 0;

    tab = *(volatile unsigned*)(size_t)(ADDR_SB_OBJ + OFF_ACTIVE_TAB);
    if (tab > 3) return 0;
    strip = ADDR_SB_OBJ + OFF_TABS + tab * OFF_TAB_STRIDE;

    cnt = *(volatile int*)(size_t)(strip + OFF_STRIP_CAMCOUNT);
    if (cnt < 0 || cnt > 4096) return 0;
    vis = FN_VISCAMEOS();
    if (vis < 0 || vis > 128) return 0;
    if (vis < 2) vis = 2;               /* 窗口太矮时引擎给 0, 兜底一行 */

    total = (cnt + 1) / 2;
    maxTop = total - (vis / 2);
    if (maxTop <= 0) return 0;          /* 放得下 -> 不显示 */

    cur = *(volatile int*)(size_t)(strip + OFF_STRIP_TOPROW);
    if (cur < 0) cur = 0;
    if (cur > maxTop) cur = maxTop;     /* 只夹本地副本, 不改引擎 */

    thumbH = camH * (vis / 2) / total;  /* 滑块高 = 可见比例, 无下限钳制 */
    span = camH - thumbH;
    if (span <= 0) return 0;   /* maxTop>0 时恒有 thumbH<camH, 到这就是野值 */

    thumbY = camTop + (span * cur + maxTop / 2) / maxTop;

    g->colX = *(volatile int*)(size_t)ADDR_SB_X;
    g->bx = g->colX + colW - s_RightMargin - s_HitW;
    g->by = camTop;
    g->bw = s_HitW;
    g->bh = camH;
    g->sx = g->colX + colW - s_RightMargin - s_ShowW;
    g->sw = s_ShowW;
    g->top = cur;
    g->maxTop = maxTop;
    g->thumbY = thumbY;
    g->thumbH = thumbH;
    g->strip = strip;
    return 1;
}

/* 写滚动位置。四个动画态字段必须一起清零 —— 只改 +0x44 的话引擎会拿 [0x48] 里
 * "还剩几行"的旧值继续滑, 表现为松手后又自己挪回去。
 * 重画标志与引擎滚完自己置的一致 (0x6A8EA7 + 0x6A7143); 0x53A7 的重画矩形从
 * [0xB0B4F8] 起, 在小地图下方, 所以每帧压它不会擦掉小地图。 */
static void SS_SetTopRow(unsigned strip, int nt)
{
    *(volatile int*)(size_t)(strip + OFF_STRIP_TOPROW) = nt;
    *(volatile int*)(size_t)(strip + OFF_STRIP_TARGET) = 0;
    *(volatile int*)(size_t)(strip + OFF_STRIP_PIXOFF) = 0;
    *(volatile unsigned char*)(size_t)(strip + OFF_STRIP_DIR)  = 0;
    *(volatile unsigned char*)(size_t)(strip + OFF_STRIP_ANIM) = 0;
    *(volatile unsigned char*)(size_t)ADDR_SB_NEEDBG = 1;
    *(volatile unsigned char*)(size_t)ADDR_SB_REDRAW  = 1;
}

/* 行 <-> 像素 的两个方向都**四舍五入**, 才能互为反函数 (截断版会差一行, 见
 * notes §2.1)。跳转与拖动必须共用这一对, 否则"跳过去"再"接着拖"会落到不同的行。 */
static void SS_DragTo(SsGeo* g, int my)
{
    int th, span, nt;
    span = g->bh - g->thumbH;
    if (span <= 0) return;
    th = my - s_Grab;
    if (th < g->by) th = g->by;
    if (th > g->by + span) th = g->by + span;
    nt = ((th - g->by) * g->maxTop + span / 2) / span;
    if (nt != g->top) SS_SetTopRow(s_DragStrip, nt);
}

/* 点轨道空白处: 把滑块整个挪到该比例 (不是翻页), 并顺手开拖 —— 一次点击完成
 * "跳过去 + 继续拖着微调"。 */
static int SS_JumpTo(SsGeo* g, int my)
{
    int span, th, nt, before = g->top;
    span = g->bh - g->thumbH;
    if (span <= 0) return 0;
    th = my - g->thumbH / 2;          /* 让滑块中心落在指针上 */
    if (th < g->by) th = g->by;
    if (th > g->by + span) th = g->by + span;
    nt = ((th - g->by) * g->maxTop + span / 2) / span;
    s_Grab = my - th;                 /* 之后拖动不再跳 */
    if (nt != before) SS_SetTopRow(g->strip, nt);
    return 1;
}

/* 鼠标在侧栏列里吗 (滑块只在这种情况下绘制)。 */
static int SS_MouseOnSidebar(int mx, int my)
{
    int x = *(volatile int*)(size_t)ADDR_SB_X;
    int y = *(volatile int*)(size_t)ADDR_SB_Y;
    int w = *(volatile int*)(size_t)ADDR_SB_W;
    int h = *(volatile int*)(size_t)ADDR_SB_H;
    if (w < 64 || w > 512 || h < 64) return 0;
    return (mx >= x && mx < x + w && my >= y && my < y + h);
}

/* ---------------- 提示避让 ----------------
 * cameo 提示盒画在侧栏面里 (CCToolTip::Draw2 侧栏分支, 早于 Blit), 本条画在
 * BlitSidebar 钩里 (呈现前最后一步) ⇒ 天然盖住提示字。ToolTip_Clamp 每次提示
 * 绘制都经 SS_NoteTip 记下面+盒+帧; 这里横向相交才断开, 纵向在盒上下分两段画
 * (各外扩 1px 不蹭边框)。只认画在本面(sb)且帧差 ≤1 的记录: composite 面的提示
 * 由引擎盖在本条之上, 无需避让; 跨帧即失效, 提示消失不留 stale 缺口。 */
static void* s_TipSurf = NULL;
static int s_TipX = 0, s_TipY = 0, s_TipW = 0, s_TipH = 0;
static int s_TipFrame = -1;

void SS_NoteTip(void* surf, int x, int y, int w, int h)
{
    int fr;
    if (!surf || w <= 0 || h <= 0) return;
    if (w > 2048 || h > 2048) return;
    fr = *(volatile int*)(size_t)ADDR_FRAME;
    s_TipSurf = surf;
    s_TipX = x; s_TipY = y; s_TipW = w; s_TipH = h;
    s_TipFrame = fr;
}

/* ---------------- 每循环 ---------------- */

void SS_Poll(void)
{
    SsGeo g;
    int mx, my, down, fr;
    char buf[128], t1[16], t2[16];
    unsigned q;

    SS_EnsureInit();
    if (!s_Enable) { s_Drag = 0; s_LbPrev = 0; return; }
    /* 帧门: 0x4F42F0 一帧能被调 142 次 (见 notes/超武侧栏.md §9)。 */
    fr = *(volatile int*)(size_t)ADDR_FRAME;
    if (fr == s_LastFrame) return;
    s_LastFrame = fr;

    /* 态1/2 下侧栏列的屏幕区域归地图: 画了也不会上屏, 而那里的点击是地图点击
     * (框选/打靶), 吞掉就是 bug。 */
    if (!FM_SidebarVisible()) { s_Drag = 0; s_LbPrev = 0; return; }
    if (!GtMouseGameXY(&mx, &my) || !GtGameFocused())
    { s_Drag = 0; s_LbPrev = 0; return; }
    down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;

    if (!SS_Geo(&g)) { s_Drag = 0; s_LbPrev = down; return; }

    if (s_Drag)
    {
        if (down) SS_DragTo(&g, my);
        else s_Drag = 0;
        s_LbPrev = down;
        return;
    }

    /* 整条都吃按下沿: 点滑块上直接开拖 (保留抓取点不跳); 点轨道空白处先跳到该
     * 比例再开拖。那块区域不属于引擎任何 gadget, 吃下不抢原版任何点击。
     * 注意这条与"鼠标不在侧栏时不绘制"是两件事 —— 滑块不可见时照样能点。 */
    if (down && !s_LbPrev &&
        mx >= g.bx && mx < g.bx + g.bw && my >= g.by && my < g.by + g.bh)
    {
        if (my >= g.thumbY && my < g.thumbY + g.thumbH)
        {
            s_Drag = 1;
            s_Grab = my - g.thumbY;
        }
        else if (SS_JumpTo(&g, my))
        {
            s_Drag = 1;
        }
        s_DragStrip = g.strip;       /* 开拖时锁定页签, 拖动中换页签不串台 */
    }
    s_LbPrev = down;

    /* 旁路诊断: fail-closed 的原因只靠"屏上没条"查不出来 (见 notes §7)。 */
    if (!s_Logged && g_Log)
    {
        s_Logged = 1;
        q = 0;
        q = AppendCStr(buf, q, "sidescroll: bar=", sizeof(buf));
        q += UtoA((unsigned)g.bx, buf + q);
        q = AppendCStr(buf, q, ",", sizeof(buf));
        q += UtoA((unsigned)g.by, buf + q);
        q = AppendCStr(buf, q, " ", sizeof(buf));
        q += UtoA((unsigned)g.bw, t1);
        q = AppendCStr(buf, q, "x", sizeof(buf));
        UtoA((unsigned)g.bh, t2);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCh(buf, q, 'x', sizeof(buf));
        q = AppendCStr(buf, q, t2, sizeof(buf));
        q = AppendCStr(buf, q, " top=", sizeof(buf));
        q += UtoA((unsigned)g.top, buf + q);
        q = AppendCStr(buf, q, "/", sizeof(buf));
        q += UtoA((unsigned)g.maxTop, buf + q);
        LogLine(buf);
    }
}

/* ---------------- 绘制 (侧栏面上屏前) ---------------- */

int SS_Draw(void)
{
    SsGeo g;
    void* sb;
    int mx, my, x0;
    RectangleStruct r;
    ColorStruct c;

    if (!s_Init || !s_Enable) return 0;
    if (!FM_SidebarVisible()) return 0;
    if (!SS_Geo(&g)) return 0;
    /* 鼠标不在侧栏列里就不画 —— 少一条常驻的竖线。 */
    if (!GtMouseGameXY(&mx, &my) || !SS_MouseOnSidebar(mx, my)) return 0;

    sb = *(void**)ADDR_SURF_SIDEBAR;
    if (!sb || !IsPlausiblePtr(sb)) return 0;
    /* 侧栏面实测 168x1080: x 原点 = 侧栏列起点, y 原点 = 屏幕上沿。所以屏幕 x
     * 要减 colX, y 直接用。 */
    if (GtSurfGet(sb, GtVT_GET_WIDTH) != SS_SURF_W) return 0;
    if (GtSurfGet(sb, GtVT_GET_HEIGHT) < 400) return 0;

    x0 = g.sx - g.colX;
    if (x0 < 0 || x0 + g.sw > SS_SURF_W) return 0;
    if (g.by < 0 || g.thumbY - g.by < 0) return 0;
    if (g.thumbY + g.thumbH > g.by + g.bh) return 0;

    c.R = (unsigned char)((s_ThumbRgb >> 16) & 0xFF);
    c.G = (unsigned char)((s_ThumbRgb >> 8) & 0xFF);
    c.B = (unsigned char)(s_ThumbRgb & 0xFF);
    r.X = x0;
    r.Width = g.sw;

    /* 提示盒处断开: 横向不交或纵向不交都整段画, 只有真压住字才分两段。 */
    {
        int fr = *(volatile int*)(size_t)ADDR_FRAME;
        int t0 = g.thumbY, t1 = g.thumbY + g.thumbH;
        int cx0, cx1, cy0, cy1, s0, s1;
        if (s_TipSurf && s_TipSurf == sb && s_TipFrame >= 0 &&
            fr >= s_TipFrame && fr - s_TipFrame <= 1)
        {
            cx0 = s_TipX - 1;
            cx1 = s_TipX + s_TipW + 1;
            cy0 = s_TipY - 1;
            cy1 = s_TipY + s_TipH + 1;
            if (cx1 > x0 && cx0 < x0 + g.sw && cy1 > t0 && cy0 < t1)
            {
                int drew = 0;
                s0 = t0;
                s1 = (cy0 < t1) ? cy0 : t1;   /* 盒上段 */
                if (s1 > s0)
                {
                    r.Y = s0;
                    r.Height = s1 - s0;
                    GtFillRectTrans(sb, &r, &c, 100);
                    drew = 1;
                }
                s0 = (cy1 > t0) ? cy1 : t0;   /* 盒下段 */
                s1 = t1;
                if (s1 > s0)
                {
                    r.Y = s0;
                    r.Height = s1 - s0;
                    GtFillRectTrans(sb, &r, &c, 100);
                    drew = 1;
                }
                return drew;
            }
        }
    }

    r.Y = g.thumbY;
    r.Height = g.thumbH;
    GtFillRectTrans(sb, &r, &c, 100);
    return 1;
}