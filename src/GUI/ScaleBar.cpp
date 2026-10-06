/*
 * ScaleBar.cpp - 地图缩放刻度条, 见 ScaleBar.h。
 *
 * 引擎侧的三条硬事实 (gamemd.exe 反汇编实证):
 *  1) 地图是**每帧逐格 1:1 软件光栅化**进"渲染面"的:
 *     TacticalClass::Render (0x6D3D10) 的格循环 (0x6D6D10 起) 每行步进 30px、
 *     每列 60px, 格子恒 60x30; 画哪一格由 ViewBounds (0x886FA0) + TacticalPos
 *     (+0xB0) 决定。**没有任何缩放/采样路径** —— 所以缩放只能自己把视口改小/
 *     改大, 呈现时再把画好的像素重采样。
 *  2) GScreenClass::Render (0x4F4480) 里地图画三趟, 渲染面是参数 (三个调用点
 *     0x4F44DF/0x4F44F4/0x4F4515 都 push [0x88731C] = Composite); 紧接着
 *     0x4F455D 才是 MessageList::Draw (引擎 UI) 与我们的 *_Draw 钩。
 *     ⇒ 在那之前把 Composite 的视口 R 放大覆盖到视图区 A, **UI 就全部照旧画在
 *       Composite 上, 天然 1:1 不受缩放影响** (战役框/提示框/聊天框/橡皮线/
 *       光标都不用管)。
 *  3) ClientToCoords (0x6D2280) 就是 `mapPos = (client + TacticalPos) * Iso`,
 *     且只用 view_bound 的宽高做裁剪 —— 屏幕点 → 视口点 是一条可逆的线性变换,
 *     所以两个方向共用同一个反变换 (SB_MapPoint)。
 *
 * 两个矩形:
 *   A = 满视图区 (侧栏模式决定宽, 高 = winH-32), 由 FullMap 每循环写死;
 *   R = 引擎实际视口 = A * scale (scale<1 视口收小 = 放大)。R 由本模块算,
 *   FullMap 只负责写进 ViewBounds, 并且必须同步 [0xB0CE28/2C/30/34] 那份缓存
 *   (见 notes §2.1)。
 *
 * 放大: 引擎照旧画进 Composite 的 (0,0,R); 每帧在 UI 之前把 R 最近邻放大覆盖到
 *   A (SB_CompositeMagnify)。写 Composite 而不是 Primary, 因为它是后面每次
 *   上屏 blit 的**源**, 在它上面改, 多少次 blit 都把放大结果原样搬上屏。
 *
 * 鼠标: 不改引擎**实例里存**的鼠标坐标 (光标留在物理位置), 只改"引擎读到的
 *   那个副本": v = (p - A.xy) * R.size / A.size。两条数据源都要折 ——
 *   WWMouseClass 虚表 +0x34 (每帧轮询) 与窗口过程那条 lParam (按下/松开),
 *   见 MOExperience.cpp。1x 时折算恒等。
 *
 * ⚠ 凡是"按 ViewBounds 定位"的引擎代码都会跟着视口缩小的量挪位置 —— 除了
 *   地图光栅化, 还有超武列表 (PowerBarClass::Draw 0x6DA9D0)、边缘卷屏
 *   (Scroll_Map)、框选橡皮线 (0x6D5030)、悬停提示三处 (见下面 g_AView 段)。
 *   新增缩放功能时按这条核一遍。
 *
 * 纯本地摄像机 + 只读查询 + 渲染面写入, 联机安全。
 */

#include <windows.h>
#include "GdiText.h"
#include "FullMap.h"
#include "MsgText.h"
#include "VecText.h"
#include "ScaleBar.h"
#include "MOExperience.h"

#define SB_SECTION "ScaleBar"

/* ---------------- 地址 (反汇编实证) ---------------- */
#define ADDR_SURF_COMPOSITE  0x0088731Cu

#define SB_LABLEN  10

/* ---------------- 配置 ---------------- */
static int s_Init = 0;
static int s_Enable = 0;
static int s_Level = 2;
static int s_X = -1;                    /* <0 = 靠视图区右缘 */
static int s_Y = -1;                    /* <0 = 贴视图区下沿上方 */
static int s_Len = 240;
static int s_FontPx = 13;

/* ---------------- 档位表 ----------------
 * 只有放大档: 0.25 = 4 倍 / 0.5 = 2 倍 / 1 = 原样 (0 = 最左 = 放大最多)。
 * 缩小档 (scale>1) 停用: 它需要一张"整张图那么大"的 DirectDraw 面 (7800x4035
 * 的图 ⇒ 66MB), ddraw 内部扛不住。*/
static const double s_LevScale[3] = { 0.25, 0.5, 1.0 };
static const int    s_NLev = 3;
static char s_LevLabel[s_NLev][SB_LABLEN];

/* 刻度条几何 */
static int s_BarX, s_BarY, s_BarW, s_BarH;
static int s_Safe = 0;      /* [Debug] Safe=1: 完全不接管 */
static int s_LbDown = 0;
static int s_BppWarned = 0;
static int s_LockWarned = 0;
static int s_Drew = 0;
static int s_LadderOk = 0;

static unsigned Sb_Fmt1(char* p, double v)
{
    int neg = 0;
    int ip, fr;
    char tmp[10];
    int n = 0, m = 0;
    if (v < 0.0) { neg = 1; v = -v; }
    ip = (int)v;
    fr = (int)((v - (double)ip) * 10.0 + 0.5);
    if (fr >= 10) { fr = 0; ip++; }
    if (ip == 0) { tmp[n++] = '0'; }
    else { while (ip) { tmp[n++] = (char)('0' + (ip % 10)); ip /= 10; } }
    while (n > 0) p[m++] = tmp[--n];
    if (fr) { p[m++] = '.'; p[m++] = (char)('0' + fr); }
    if (neg) p[m++] = '-';
    p[m] = 0;
    return (unsigned)m;
}

/* ---------------- 初始化 ----------------
 * 1 = 完全不接管: 消息钩不调放大/刻度条, prepare 不跑 SB_Poll, 查格不折算。
 * 用来二分"崩溃到底跟缩放有没有关系" (Enable=0 只是不画, 钩子还在跑)。*/
int  SB_Safe(void) { return s_Safe; }

void SB_EnsureInit(void)
{
    char b1[16], buf[192];
    unsigned p;
    if (s_Init) return;
    s_Init = 1;
    GtEnsureLog();
    if (!g_Log) return;
    s_Enable = GetPrivateProfileIntA(SB_SECTION, "Enable", 0, g_IniPath) ? 1 : 0;
    s_X = GetPrivateProfileIntA(SB_SECTION, "X", -1, g_IniPath);
    s_Y = GetPrivateProfileIntA(SB_SECTION, "Y", -1, g_IniPath);
    s_Len = GetPrivateProfileIntA(SB_SECTION, "Len", 240, g_IniPath);
    s_FontPx = GetPrivateProfileIntA(SB_SECTION, "FontPx", 13, g_IniPath);
    if (s_Len < 80 || s_Len > 2000) s_Len = 240;
    if (s_FontPx < 8) s_FontPx = 8;
    if (s_FontPx > 40) s_FontPx = 40;
    s_Safe = GetPrivateProfileIntA("Debug", "Safe", 0, g_IniPath) ? 1 : 0;
    s_Level = GetPrivateProfileIntA(SB_SECTION, "Level", 2, g_IniPath);
    if (s_Level < 0 || s_Level >= s_NLev) s_Level = 2;
    if (!s_Enable) return;
    p = 0;
    p = AppendCStr(buf, p, "sbar: on level=", sizeof(buf));
    UtoA((unsigned)s_Level, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " len=", sizeof(buf));
    UtoA((unsigned)s_Len, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

/* ---------------- 档位文字 (只有放大档, 所以永远是 "4x" / "2x" / "1x") --- */
static void Sb_InitLabels(void)
{
    int i;
    if (s_LadderOk) return;
    s_LadderOk = 1;
    for (i = 0; i < s_NLev; i++)
    {
        char* d = s_LevLabel[i];
        int j;
        Sb_Fmt1(d, 1.0 / s_LevScale[i]);
        j = 0;
        while (d[j]) j++;
        d[j] = 'x';
        d[j + 1] = 0;
    }
}

/* ---------------- 档位查询 ---------------- */
int SB_Level(void)    { return s_Enable ? s_Level : s_NLev - 1; }
double SB_Scale(void) { return SB_In() ? s_LevScale[s_Level] : 1.0; }
int SB_In(void)       { return (s_Enable && s_LevScale[s_Level] < 1.0) ? 1 : 0; }

/* ---------------- CCToolTip 的"选面/量宽"矩形必须是 A ----------------
 * `ToolTipManager::Update` (0x478BA0) 与 `CCToolTip::DrawText` (0x478E30)
 * 把 ViewBounds 的绝对地址写死在指令里, 拿它当右界与 maxWidth。放大档下
 * ViewBounds 已是视口 R, 拿它当界 ⇒ 误判"提示框在侧栏上"、文本提前折行。
 * 消费方: VecText.cpp 的 rep[] (改 imm32 改指) 与 ToolTip.cpp 的
 * ToolTip_Clamp (自己算, 按 SB_In 取)。1x 时 A == R ⇒ 与原版一致。 */
static unsigned g_AView[4] = { 0u, 0u, 1752u, 1048u };

const unsigned* SB_AViewBlock(void) { return g_AView; }

void SB_PublishAView(void)
{
    RectangleStruct a;
    FM_GetViewRects(&a, 0);
    if (a.Width < 16 || a.Height < 16) return;   /* 还没布局好: 保留上一份 */
    g_AView[0] = (unsigned)a.X;
    g_AView[1] = (unsigned)a.Y;
    g_AView[2] = (unsigned)a.Width;
    g_AView[3] = (unsigned)a.Height;
}

/* 引擎实际要画的视口矩形 = 视图区 * scale。
 * ★ ViewBounds 恒为 (0,0,w,h): 引擎的渲染落点是 ViewBounds.xy + (mapPos -
 *   TacticalPos), 而它自己的两条查格路 (CoordsToCell / ClientToCoords) 只有在
 *   X/Y = 0 时才自洽, 非 0 会让下单击与 hover 差 2·X。*/
int SB_GetViewRect(RectangleStruct* out)
{
    RectangleStruct a;
    int w, h;
    double s;
    if (!s_Enable || s_Safe || !out) return 0;
    FM_GetViewRects(&a, 0);
    if (a.Width < 32 || a.Height < 32) return 0;
    s = SB_Scale();
    w = (int)((double)a.Width * s + 0.5);
    h = (int)((double)a.Height * s + 0.5);
    if (w < 32) w = 32;
    if (h < 32) h = 32;
    out->X = 0;
    out->Y = 0;
    out->Width = w;
    out->Height = h;
    return 1;
}

/* ---------------- 最近邻定点重采样 ----------------
 * 定点步长: 目标第 i 像素取源 (i * step) >> 16, 无浮点。
 * ⚠ 目的矩形必须按**目标面尺寸** (iw/ih) 再夹一次: 只按传入的裁剪矩形夹的话,
 * 调用方给的矩形一旦超出目标面 (视图区比 Composite 大 —— 启动/改分辨率的瞬间),
 * 就会整片写出界 ⇒ 堆被写坏 ⇒ 之后某个虚表/函数指针变野, 跳到堆地址崩。 */
static unsigned Sb_Step(int srcW, int dstW)
{
    if (dstW <= 0) return 0;
    return (unsigned)(((unsigned long)srcW << 16) / (unsigned long)dstW);
}
static int Sb_MinI(int a, int b) { return a < b ? a : b; }
static int Sb_MaxI(int a, int b) { return a > b ? a : b; }

static long Sb_Resample(unsigned char* sb, int sp, int sw, int sh,
                        unsigned char* db, int dp, int iw, int ih,
                        int sx, int sy, int rw, int rh,
                        int dx, int dy, int dw, int dh,
                        int cx0, int cy0, int cx1, int cy1)
{
    unsigned xstep, ystep;
    int x, y, x0, y0, x1, y1;
    (void)sw; (void)sh;
    if (iw <= 0 || ih <= 0) return 0;
    if (dx < 0) { dw += dx; dx = 0; }
    if (dy < 0) { dh += dy; dy = 0; }
    if (dx + dw > iw) dw = iw - dx;
    if (dy + dh > ih) dh = ih - dy;
    if (dw <= 0 || dh <= 0 || rw <= 0 || rh <= 0) return 0;
    x0 = Sb_MaxI(cx0 - dx, 0);
    x1 = Sb_MinI(cx1 - dx, dw);
    y0 = Sb_MaxI(cy0 - dy, 0);
    y1 = Sb_MinI(cy1 - dy, dh);
    if (x1 <= x0 || y1 <= y0) return 0;
    xstep = Sb_Step(rw, dw);
    ystep = Sb_Step(rh, dh);
    for (y = y0; y < y1; y++)
    {
        unsigned short* drow = (unsigned short*)(db + (size_t)(dy + y) * dp);
        const unsigned short* srow = (const unsigned short*)
            (sb + (size_t)(sy + (int)((unsigned)(y * (int)ystep) >> 16)) * sp);
        for (x = x0; x < x1; x++)
            drow[dx + x] = srow[sx + (int)((unsigned)(x * (int)xstep) >> 16)];
    }
    return (long)(x1 - x0) * (long)(y1 - y0);
}

/* 某个关口没通过时的原因码 (只在真出问题时打一条, 正常帧零开销) */
static int Sb_Skip(int why)
{
    char buf[64];
    unsigned q = 0;
    static int s_tick = 0;
    if (s_tick > 0) { s_tick--; return 0; }
    s_tick = 60;
    q = AppendCStr(buf, q, "sbar: skip w", sizeof(buf));
    buf[q++] = (char)('0' + (why >= 0 && why < 10 ? why : 9));
    LogLine(buf);
    return 0;
}

/* ---------------- 放大档: 把放大结果写进 Composite 的视图区 ----------------
 * 为什么写 Composite 而不是 Primary: `UpdatePrimarySurface` (把地图面 blit 到
 * Primary) **一帧要跑很多次**, 呈现后处理只能落在其中一次之后 ⇒ 写进 Primary 的
 * 放大结果会被随后的 blit 覆盖 (洋红判定框实测只闪一瞬)。Composite 是这些 blit
 * 的**源**, 在它上面改, 后面每一次 blit 都会把放大结果原样搬上屏, 次数再多也
 * 一样 —— 而且完全不需要"blit 之后"的钩子。
 *
 * 一帧时序: 引擎 Render (1:1 地图进 Composite 的 (0,0,R)) → 本函数 (把 R 放大
 * 覆盖视图区 A) → 我们自己的 UI 层 (1:1 落在放大后的地图上) → 引擎 blit 上屏。
 *
 * ⚠ 源和目标在同一张面 (A 包含 R), 必须先把 R 拷出来再原地放大, 否则最近邻
 * 采样会读到刚写进去的放大像素。*/
static void* s_Scratch = 0;
static int s_ScratchBytes = 0;

static unsigned char* Sb_Scratch(int need)
{
    if (s_ScratchBytes >= need) return (unsigned char*)s_Scratch;
    if (s_Scratch) VirtualFree(s_Scratch, 0, MEM_RELEASE);
    s_ScratchBytes = need + (need >> 1);
    s_Scratch = VirtualAlloc(0, (size_t)s_ScratchBytes, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
    return (unsigned char*)s_Scratch;
}

int SB_CompositeMagnify(void)
{
    RectangleStruct a, r;
    void* comp;
    unsigned char *cb, *sb;
    int cp, cw, ch, need, y;
    if (!s_Enable || s_Safe) return 0;
    if (!SB_In()) return 0;
    MOE_SetPhase(MP_COMPMAG);
    FM_GetViewRects(&a, &r);
    if (a.Width < 32 || r.Width < 16 || r.Height < 16) return Sb_Skip(1);
    comp = *(void**)(size_t)ADDR_SURF_COMPOSITE;
    if (!comp || !IsPlausiblePtr(comp)) return Sb_Skip(2);
    cp = GtSurfGet(comp, GtVT_GET_PITCH);
    cw = GtSurfGet(comp, GtVT_GET_WIDTH);
    ch = GtSurfGet(comp, GtVT_GET_HEIGHT);
    if (cp <= 0 || cw < 8 || ch < 8) return Sb_Skip(3);
    /* 下面所有读写都按 16bpp (unsigned short) 算。pitch 不够 2*width 说明这不是
     * 16bpp 的面, 继续走就会每行越过行尾、最后几行直接冲出缓冲。*/
    if (cp < cw * 2) return Sb_Skip(7);
    if (a.X < 0) { a.Width += a.X; a.X = 0; }
    if (a.Y < 0) { a.Height += a.Y; a.Y = 0; }
    if (a.X + a.Width > cw) a.Width = cw - a.X;
    if (a.Y + a.Height > ch) a.Height = ch - a.Y;
    if (a.Width < 16 || a.Height < 16) return Sb_Skip(1);
    if (r.X < 0) r.X = 0;
    if (r.Y < 0) r.Y = 0;
    if (r.X + r.Width > cw) r.Width = cw - r.X;
    if (r.Y + r.Height > ch) r.Height = ch - r.Y;
    if (r.Width < 16 || r.Height < 16) return Sb_Skip(1);
    need = r.Width * 2;
    sb = Sb_Scratch(need * r.Height);
    if (!sb) return Sb_Skip(6);
    cb = GtLock(comp);
    if (!cb) return Sb_Skip(4);
    for (y = 0; y < r.Height; y++)
        memcpy(sb + (size_t)y * need,
               cb + (size_t)(r.Y + y) * cp + (size_t)r.X * 2, (size_t)need);
    Sb_Resample(sb, need, r.Width, r.Height, cb, cp, cw, ch,
                0, 0, r.Width, r.Height,
                a.X, a.Y, a.Width, a.Height,
                a.X, a.Y, a.X + a.Width, a.Y + a.Height);
    GtUnlock(comp);
    return 1;
}

/* ---------------- 物理鼠标点 -> 视口点 (两条折算共用) -------------------
 * 引擎把地图画在渲染面的 (mapPos - TacticalPos), 屏幕点 p 落在的视口坐标 =
 * (p - A.xy) * R.size / A.size。两个方向共用这一个式子。
 *
 * ⚠ **A 之外的点原样放行, 不推**。调用点里不乏 UI/卷屏/侧栏那类本来就是屏幕
 * 空间的判据, 把 A 外的点乱推成 R 之外等于替引擎改它自己的数据。引擎自己会拒:
 *   - ClientToCoords 只比 ViewBounds 的宽高 (0xB0CE30/34, 由
 *     TacticalClass::Set_View_Dimensions 0x6D5F60 从 ViewBounds 抄来);
 *   - 框选橡皮线 (0x6D5030) 与 Scroll_Map 都是同一套 ViewBounds 边界判定;
 *   - gadget 命中链 (GadgetClass::Input) 不经过这里 (它用虚表 +0x2C/+0x30)。*/
void SB_MapPoint(Point2D* p, unsigned ret)
{
    RectangleStruct a, r;
    int vx, vy;
    if (!s_Enable || s_Safe || !p) return;
    if (!IsPlausiblePtr(p)) return;   /* 入参取错的话 p 会是野指针, 绝不能写 */
    if (!SB_In()) return;
    if (ret >= 0x00653700u && ret < 0x00653800u) return;   /* 雷达: 雷达面内坐标 */
    FM_GetViewRects(&a, &r);
    if (a.Width < 32 || r.Width < 16 || r.Height < 16) return;
    if (p->X < a.X || p->Y < a.Y ||
        p->X >= a.X + a.Width || p->Y >= a.Y + a.Height) return;
    vx = (int)((long)(p->X - a.X) * (long)r.Width / (long)a.Width);
    vy = (int)((long)(p->Y - a.Y) * (long)r.Height / (long)a.Height);
    if (vx < 0) vx = 0;
    if (vy < 0) vy = 0;
    if (vx > r.Width) vx = r.Width;
    if (vy > r.Height) vy = r.Height;
    p->X = vx;
    p->Y = vy;
}

/* 视图坐标 -> 屏幕坐标 (本项目按引擎投影画图元的层用) */
int SB_ViewToScreen(int vx, int vy, int* sx, int* sy)
{
    RectangleStruct a, r;
    if (!s_Enable || !SB_In()) return 0;
    FM_GetViewRects(&a, &r);
    if (a.Width < 32 || r.Width < 16 || r.Height < 16) return 0;
    if (sx) *sx = a.X + (int)((long)vx * (long)a.Width / (long)r.Width);
    if (sy) *sy = a.Y + (int)((long)vy * (long)a.Height / (long)r.Height);
    return 1;
}

/* 地图像素增量 -> 屏幕增量 */
void SB_MapDeltaToScreen(int* dx, int* dy)
{
    RectangleStruct a, r;
    if (!s_Enable || !dx || !dy || !SB_In()) return;
    FM_GetViewRects(&a, &r);
    if (a.Width < 32 || r.Width < 16 || r.Height < 16) return;
    *dx = (int)((long)(*dx) * (long)a.Width / (long)r.Width);
    *dy = (int)((long)(*dy) * (long)a.Height / (long)r.Height);
}

/* ---------------- 刻度条几何 ---------------- */
static int Sb_Gx0(void) { return s_BarX + 14; }
static int Sb_Gx1(void) { return s_BarX + s_BarW - 14; }
static int Sb_DetentX(int i)
{
    int a = Sb_Gx0(), b = Sb_Gx1(), n = s_NLev > 1 ? s_NLev - 1 : 1;
    return a + (b - a) * i / n;
}
static int Sb_LevelAt(int mx)
{
    int i, best = 0, bd = 0x7FFFFFFF, d;
    for (i = 0; i < s_NLev; i++)
    {
        d = mx - Sb_DetentX(i);
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static int Sb_OverBar(int mx, int my)
{
    return mx >= s_BarX - 8 && mx < s_BarX + s_BarW + 8 &&
           my >= s_BarY - 4 && my < s_BarY + s_BarH + 4;
}

static void Sb_Place(void)
{
    RectangleStruct a;
    FM_GetViewRects(&a, 0);
    /* 条的高度: 字高 + 上下留白 (至少 14px, 否则点击区太矮)。*/
    s_BarH = s_FontPx + 10;
    if (s_BarH < 14) s_BarH = 14;
    s_BarW = s_Len;
    if (s_X >= 0) s_BarX = s_X;
    else s_BarX = a.X + a.Width - s_BarW - 12;
    /* 默认盖在**底部工具栏**那条带子上 (视图高 = winH-32, 工具栏就是视图下面
     * 那 32px)。放这里缩放的取样/放大都碰不到它, 放大时工具栏和条都保持 1:1。*/
    if (s_Y >= 0) s_BarY = s_Y;
    else s_BarY = GtGameH() - s_BarH;   /* 贴窗口底边, 整条都落在工具栏带子里 */
    if (s_BarY + s_BarH > GtGameH()) s_BarY = GtGameH() - s_BarH;
    if (s_BarX < a.X) s_BarX = a.X;
    if (s_BarY < a.Y) s_BarY = a.Y;
}

static void Sb_SetLevel(int lv, const char* why)
{
    char b1[16], buf[160];
    unsigned p = 0;
    if (!s_Enable) return;
    if (lv < 0 || lv >= s_NLev || lv == s_Level) return;
    s_Level = lv;
    p = AppendCStr(buf, p, "sbar: level=", sizeof(buf));
    UtoA((unsigned)s_Level, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " scale=", sizeof(buf));
    p += Sb_Fmt1(buf + p, SB_Scale());
    p = AppendCStr(buf, p, " by=", sizeof(buf));
    p = AppendCStr(buf, p, why, sizeof(buf));
    p = AppendCStr(buf, p, " drew=", sizeof(buf));
    UtoA((unsigned)s_Drew, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

/* 每循环: 摆矩形 + 点击换档。 */
void SB_Poll(void)
{
    int mx, my, down;
    if (!s_Enable) return;
    Sb_InitLabels();
    Sb_Place();
    if (!GtMouseGameXY(&mx, &my)) { s_LbDown = 0; return; }
    down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    if (down && !s_LbDown)
    {
        if (Sb_OverBar(mx, my)) Sb_SetLevel(Sb_LevelAt(mx), "click");
        s_LbDown = 1;
    }
    else if (!down) s_LbDown = 0;
}

/* ---------------- 刻度条绘制 (已 Lock 的 16 位面里直接写像素) ---------------- */
static void Sb_Fill(unsigned char* base, int pitch, int W, int H,
                    int x0, int y0, int w, int h,
                    unsigned char r, unsigned char g, unsigned char b, int a)
{
    int yy, xx;
    unsigned short* row;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > W) w = W - x0;
    if (y0 + h > H) h = H - y0;
    for (yy = 0; yy < h; yy++)
    {
        row = (unsigned short*)(base + (size_t)(y0 + yy) * pitch);
        for (xx = 0; xx < w; xx++)
            row[x0 + xx] = Blend16(row[x0 + xx], r, g, b, a);
    }
}

/* 条的全部像素。不画底板/边框 (半透明底板叠在会变的底栏上每帧重新混色 = 闪烁);
 * 全部 alpha=255 直写, 像素不随底色变化。ox/oy = 坐标原点偏移。*/
static void Sb_Paint(unsigned char* base, int pitch, int W, int H, int ox, int oy)
{
    int bx = s_BarX + ox, by = s_BarY + oy;
    int mid = by + s_BarH / 2;
    int dx, i, tw, n;
    wchar_t buf[10];
    int gx0 = Sb_Gx0() + ox, gx1 = Sb_Gx1() + ox;
    if (s_NLev < 1) return;
    /* 槽 */
    Sb_Fill(base, pitch, W, H, gx0, mid - 1, gx1 - gx0 + 1, 3, 88, 92, 100, 255);
    /* 左"+" 右"-" */
    dx = bx + 6;
    Sb_Fill(base, pitch, W, H, dx - 3, mid - 1, 7, 3, 232, 236, 242, 255);
    Sb_Fill(base, pitch, W, H, dx - 1, mid - 4, 3, 9, 232, 236, 242, 255);
    dx = bx + s_BarW - 7;
    Sb_Fill(base, pitch, W, H, dx - 4, mid - 1, 9, 3, 232, 236, 242, 255);
    /* 刻度 */
    for (i = 0; i < s_NLev; i++)
    {
        dx = Sb_DetentX(i) + ox;
        Sb_Fill(base, pitch, W, H, dx - 1, mid - 7, 2, 15, 198, 206, 216, 255);
    }
    /* 旋钮 (当前档): 深色描边 + 亮芯 */
    dx = Sb_DetentX(s_Level) + ox;
    Sb_Fill(base, pitch, W, H, dx - 4, mid - 10, 8, 21, 12, 13, 16, 255);
    Sb_Fill(base, pitch, W, H, dx - 2, mid - 8, 4, 17, 250, 214, 80, 255);
    /* 档位文字 */
    if (!GdiEnsure(s_FontPx, VecText_Bold(), 7, VecText_FontFace())) return;
    for (i = 0; i < s_NLev; i++)
    {
        const char* lab = s_LevLabel[i];
        n = 0;
        while (lab[n] && n < 9) { buf[n] = (wchar_t)lab[n]; n++; }
        buf[n] = 0;
        if (!n) continue;
        tw = RenderLine(buf, n);
        if (tw <= 0) continue;
        BlitCoverage(base, pitch, W, H, Sb_DetentX(i) + ox - tw / 2, by - 1,
                     tw + 2, g_G->adv, 242, 246, 252, 100);
    }
}

/* 锁面 + 调 Sb_Paint 的共用壳。bpp 槽返回的是**字节**数 (16 位色=2)。*/
static int Sb_PaintSurface(void* surf)
{
    unsigned char* base;
    int pitch, w, h, bpp;
    if (!s_Enable || !surf || !IsPlausiblePtr(surf)) return 0;
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    w = GtSurfGet(surf, GtVT_GET_WIDTH);
    h = GtSurfGet(surf, GtVT_GET_HEIGHT);
    bpp = GtSurfGet(surf, GtVT_GET_BPP);
    if (bpp != 2 || pitch <= 0 || w <= 0 || h <= 0)
    {
        if (!s_BppWarned)
        {
            char b1[16], buf[112];
            unsigned q = 0;
            s_BppWarned = 1;
            q = AppendCStr(buf, q, "sbar: surface bpp=", sizeof(buf));
            UtoA((unsigned)bpp, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " (need 2) -> bar off", sizeof(buf));
            LogLine(buf);
        }
        return 0;
    }
    Sb_Place();
    base = GtLock(surf);
    if (!base)
    {
        if (!s_LockWarned)
        {
            s_LockWarned = 1;
            LogLine("sbar: surface lock failed -> bar not drawn");
        }
        return 0;
    }
    Sb_Paint(base, pitch, w, h, 0, 0);
    GtUnlock(surf);
    return 1;
}

int SB_DrawToComposite(void)
{
    if (s_Safe) return 0;
    if (Sb_PaintSurface(*(void**)ADDR_SURF_COMPOSITE)) { s_Drew = 1; return 1; }
    return 0;
}