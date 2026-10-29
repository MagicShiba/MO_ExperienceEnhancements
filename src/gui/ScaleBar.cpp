/* ScaleBar.cpp - 地图缩放 (见 ScaleBar.h): 改视口大小 + 呈现时重采样。
 * 纯本地摄像机 + 渲染面写入, 联机安全。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "FullMap.h"
#include "MsgText.h"
#include "VecText.h"
#include "ScaleBar.h"
#include "PiP.h"
#include "MOExperience.h"

#define SB_SECTION "BottomBar"

/* ---------------- 地址 (反汇编实证) ---------------- */
#define ADDR_DSURF_CTOR      0x004BA5A0u   /* DSurface::DSurface(this,w,h,f1,f2) */
#define ADDR_OP_NEW          0x007C8E17u   /* 引擎的 operator new (cdecl, 1 参) */
#define ADDR_OP_DELETE       0x007C8B3Du   /* 引擎的 operator delete (cdecl, 1 参) */
/* ZBuffer/ABuffer 的构造/析构 (对象 0x30 字节，+0x28=宽 +0x2C=高)。
 * 只借这两个函数单独重建缓冲，不走 Set_View_Dimensions 正门
 * (它末尾调 InitGUI，会按 R 重排侧栏/消息区)。 */
#define ADDR_ZBUF_CTOR       0x007BC970u
#define ADDR_ZBUF_DTOR       0x007BCAE0u
#define ADDR_ABUF_CTOR       0x00410CE0u
#define ADDR_ABUF_DTOR       0x00410E50u
#define SB_BUF_OBJ_SIZE      0x30
#define ADDR_WWMOUSE_SB      0x00887640u   /* WWMouseClass::Instance */
#define ADDR_ZBUF_SB         0x00887644u   /* ZBufferClass* (每帧 z-fill/z-test) */
#define ADDR_ABUF_SB         0x0087E8A4u   /* ABufferClass* */
#define ADDR_FRAME_SB        0x00A8ED84u   /* 引擎帧号 */
#define ADDR_TACTICAL_SB     0x00887324u   /* TacticalClass::Instance */
#define ADDR_CUR_SURF_SB     0x00887314u   /* 引擎"当前绘制面" (Render 入口 = Composite) */
/* chrome 脏缓存: [0xB0CD44] 为 0 ⇒ 引擎下帧重画整个 chrome。 */
#define SB_CHROME_DIRTY_N 0xB0CD44u

/* TacticalClass::RegisterDirtyArea (0x6D2790)：矩形推进脏区表
 * ([0xB0CE7C] 起每条 20 字节)。滚动不走这条路 (整幅位移拷贝 + 只画新空条)。 */
typedef void (__fastcall *FnRegDirtyArea)(void*, RectangleStruct, int);

#define SB_LABLEN  10

/* ---------------- 配置 ---------------- */
static int s_Init = 0;
static int s_Enable = 0;
static int s_Level = 4;                 /* = 1x */
static int s_X = 32;                    /* 条右端离"侧栏列左缘"的像素 */
static int s_Y = 4;                     /* 下边距 (控件底边到窗口底边); -1 = 贴带顶 */
static int s_Len = 360;
static int s_FontPx = 12;
static int s_VoidRgb = 0x202028;        /* 图外底色 (深灰), 0xRRGGBB 一个键 */
static int s_LimitFallback = 8192;      /* R 长边上限像素 (0=无限制, <3820 按 3820) */

/* R 长边限制 ([BottomBar] LimitFallback，缺省 8192)：自设安全线。
 * 取 R 的三处须同线，否则 R 与裁剪器错配。0 = 不预检。 */
#define SB_LIMIT_DEF 8192
#define SB_LIMIT_MIN 3820

/* 长边超限? (0=不限) */
static int Sb_OverLimit(int w, int h)
{
    if (s_LimitFallback == 0) return 0;
    return (w > s_LimitFallback || h > s_LimitFallback);
}

/* 超限钳到限制上: 长边=LimitFallback。只钳缩小档 (s>1); 1x/放大档 R<=A 原样走。*/
static double Sb_ClampScale(double s, int aw, int ah)
{
    int longA;
    if (s <= 1.0 || s_LimitFallback == 0) return s;
    longA = aw > ah ? aw : ah;
    if (longA < 1) return s;
    {
        double cap = (double)s_LimitFallback / (double)longA;
        if (s > cap) return cap;
    }
    return s;
}

/* ---------------- 档位表 ----------------
 * 9 档，从最放大排到最小。标尺数字是相对 1x 的倍率 1/scale。
 * scale>1 的档视野比窗口大，走 R 大的离屏面；建不出时退回 A。 */
#define SB_MAXLEV 9
#define SB_LEV_MAX (SB_MAXLEV - 1)     /* 最右那档: 整张地图塞进视图区 */
/* MAX 那档的 scale 由 Sb_CalcMaxScale() 现算, 表里留 0。*/
static const double s_LevAll[SB_MAXLEV] =
    { 0.25, 1.0 / 3.0, 0.5, 1.0 / 1.5, 1.0, 1.25, 1.5, 2.0, 0.0 };
static const double* s_LevScale = s_LevAll;
static int    s_NLev = SB_MAXLEV;
static void Sb_SyncZoomed(void);
static char s_LevLabel[SB_MAXLEV][SB_LABLEN];
/* s_LevAll 里 scale==1.0 那档的下标。 */
static int s_LevOne = 4;
static double Sb_CalcMaxScale(void);   /* MAX 档的倍率, 定义在"档位查询"段 */

/* 刻度条几何 */
static int s_BarX, s_BarY, s_BarW, s_BarH;
static int s_LastX = -1, s_LastY = -1, s_LastW = -1;   /* 上次 geom, 只为少打日志 */
static int s_LbDown = 0;
static int s_BppWarned = 0;
static int s_LockWarned = 0;
static int s_Drew = 0;
static int s_LadderOk = 0;

/* ---------------- 缩小档的离屏大面 (scale>1) ----------------
 * 引擎只会 1:1 画进传给 Render 的面；scale>1 时自建两张 R 大的 DSurface，
 * 只在 Render 期间临时换进全局，完事还原再降采样铺回。 */
static void* s_BigComp = 0;             /* 两张 R 大的离屏面 (引擎双缓冲互换) */
static void* s_BigMap = 0;
static void* s_CurC = 0;                /* 下次 Render 入口要装的 (Composite 槽) */
static void* s_CurM = 0;                /* 下次 Render 入口要装的 (地图槽) */
static void* s_LastFinal = 0;           /* 本帧渲染完的最终画面所在面 (降采样源) */
/* 大面物理尺寸只增不减 (新 R 塞得下就复用)：精确重建会在切侧栏时闪黑。 */
static int   s_BigW = 0, s_BigH = 0;     /* 逻辑 R */
static int   s_BigCapW = 0, s_BigCapH = 0; /* 实际分配尺寸 */
static int   s_SurfActive = 0;
static int   s_SurfWarned = 0;
static int   s_SurfFail = 0;            /* 大面建不出来: 缩小档退回 A (不越界) */
static int   s_SwapSeen = 0;            /* 本帧 Render 入口真的换过大面 (否则不降采样) */
/* 大面已装上但当前让出给窗口面的那一小段。两个让出口 (入口光标擦除串与 chrome
 * pass) 都只见窗口面, 大面被暴露的时间只剩三趟地图。 */
static int   s_Suspended = 0;
/* ZBuffer/ABuffer 当前尺寸 (按 R 重建过一次就记下) */
static int   s_BufW = 0, s_BufH = 0;
/* 新面催引擎重画 (帧号门控)。 */
static int   s_RedrawFrames = 0;
static int   s_RedrawFrameNo = -1;
/* 一次性洗大面：修好窗口面之前烙进的旧像素，增量渲染盖不掉。 */
static int   s_PurgeFrames = 0;
static int   s_PurgeWarned = 0;
/* 渲染期临时换出的原全局 (只在 SB_RenderSwapIn/Out 之间有效) */
static void* s_RenderOldComp = 0;
static void* s_RenderOldMap = 0;
static int   s_RenderSwapped = 0;
/* 记住真正的窗口面 (泄漏自愈用)。 */
static void* s_WinComp = 0;
static void* s_WinMap = 0;
static int   s_LeakWarned = 0;
/* 窗口面的逻辑尺寸。每帧由 SB_NoteWindowSurface 喂。 */
static int   s_WinW = 0, s_WinH = 0;

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
 * 1 = 完全不接管: 消息钩不调放大/刻度条, prepare 不跑 SB_Poll, 查格不折算。*/

void SB_EnsureInit(void)
{
    char b1[16], b2[16], b3[16], buf[192];
    unsigned p;
    if (s_Init) return;
    s_Init = 1;
    GtEnsureLog();
    if (!g_Log) return;
    s_Enable = GetPrivateProfileIntA(SB_SECTION, "ScaleEnable", 0, g_IniPath) ? 1 : 0;
    s_X = GetPrivateProfileIntA(SB_SECTION, "X", 32, g_IniPath);
    if (s_X < 0 || s_X > 4096) s_X = 32;
    s_Y = GetPrivateProfileIntA(SB_SECTION, "Y", 4, g_IniPath);
    if (s_Y > 4096) s_Y = 4096;
    if (s_Y < -1) s_Y = -1;
    s_Len = GetPrivateProfileIntA(SB_SECTION, "Len", 240, g_IniPath);
    s_FontPx = GetPrivateProfileIntA(SB_SECTION, "FontPx", 12, g_IniPath);
    if (s_Len < 80 || s_Len > 2000) s_Len = 240;
    if (s_FontPx < 8) s_FontPx = 8;
    if (s_FontPx > 40) s_FontPx = 40;
    /* 图外底色 (视口比地图大时露出来的那一大片): 一个键, 0xRRGGBB。*/
    s_VoidRgb = GetPrivateProfileIntA(SB_SECTION, "Void", 0x202028, g_IniPath);
    if (s_VoidRgb < 0 || s_VoidRgb > 0xFFFFFF) s_VoidRgb = 0x202028;
    s_NLev = SB_MAXLEV;
    {
        int i;
        for (i = 0; i < (int)s_NLev; i++)
            if (s_LevScale[i] == 1.0) { s_LevOne = i; break; }
    }
    s_Level = GetPrivateProfileIntA(SB_SECTION, "Level", s_LevOne, g_IniPath);
    if (s_Level < 0 || s_Level >= s_NLev) s_Level = s_LevOne;
    s_LimitFallback = GetPrivateProfileIntA(SB_SECTION, "LimitFallback", SB_LIMIT_DEF, g_IniPath);
    if (s_LimitFallback != 0 && s_LimitFallback < SB_LIMIT_MIN) s_LimitFallback = SB_LIMIT_MIN;
    Sb_SyncZoomed();
    if (!s_Enable) return;
    p = 0;
    p = AppendCStr(buf, p, "sbar: on level=", sizeof(buf));
    UtoA((unsigned)s_Level, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " nlev=", sizeof(buf));
    UtoA((unsigned)s_NLev, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    {
        double ms = Sb_CalcMaxScale();
        p = AppendCStr(buf, p, " maxscale=", sizeof(buf));
        p += Sb_Fmt1(buf + p, ms);
        if (ms <= 0.0) p = AppendCStr(buf, p, "(unsupported)", sizeof(buf));
    }
    p = AppendCStr(buf, p, " len=", sizeof(buf));
    UtoA((unsigned)s_Len, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " void=", sizeof(buf));
    UtoA((unsigned)((s_VoidRgb >> 16) & 0xFF), b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, ',', sizeof(buf));
    UtoA((unsigned)((s_VoidRgb >> 8) & 0xFF), b2); p = AppendCStr(buf, p, b2, sizeof(buf));
    p = AppendCh(buf, p, ',', sizeof(buf));
    UtoA((unsigned)(s_VoidRgb & 0xFF), b3); p = AppendCStr(buf, p, b3, sizeof(buf));
    p = AppendCStr(buf, p, " limit=", sizeof(buf));
    UtoA((unsigned)s_LimitFallback, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

int SB_Enabled(void)
{
    SB_EnsureInit();
    return s_Enable;
}

/* ---------------- 档位文字 ("4x" / "2x" / "1x" …) --- */
static void Sb_InitLabels(void)
{
    int i;
    if (s_LadderOk) return;
    s_LadderOk = 1;
    for (i = 0; i < s_NLev; i++)
    {
        char* d = s_LevLabel[i];
        int j = 0;
        if (i == SB_LEV_MAX)
        {
            d[0] = 'm'; d[1] = 'a'; d[2] = 'x'; d[3] = 0;
            continue;
        }
        Sb_Fmt1(d, 1.0 / s_LevScale[i]);
        while (d[j]) j++;
        d[j] = 'x';
        d[j + 1] = 0;
    }
}

/* ---------------- MAX 档：整张地图塞进视图区 ----------------
 * scale = max(图宽/A.W, 图高/A.H)，取更松的那轴两轴才都装得下。
 * 超限钳到 LimitFallback；过小或地图读不到才判不支持。 */
static double s_MaxScale = 0.0;
static int s_MaxOk = 0;
int SB_MapPx(int* l, int* t, int* w, int* h);           /* 定义在文件后部 */
static int Sb_ViewPx(int* ox, int* oy, int* w, int* h);/* 定义在文件后部 */
static void Sb_DropMax(void) { s_MaxOk = 0; s_MaxScale = 0.0; }

static double Sb_CalcMaxScale(void)
{
    RectangleStruct a;
    int ml, mt, mw, mh, rw, rh;
    double sx, sy, sc;
    if (!SB_MapPx(&ml, &mt, &mw, &mh)) return 0.0;
    FM_GetViewRects(&a, 0);
    if (a.Width < 64 || a.Height < 64) return 0.0;
    if (mw < 16 || mh < 16) return 0.0;
    sx = (double)mw / (double)a.Width;
    sy = (double)mh / (double)a.Height;
    sc = sx > sy ? sx : sy;
    if (sc <= 0.02 || sc > 64.0) return 0.0;
    rw = (int)((double)a.Width * sc + 0.5);
    rh = (int)((double)a.Height * sc + 0.5);
    if (rw < 320 || rh < 240) return 0.0;
    if (Sb_OverLimit(rw, rh))
    {
        /* 超限钳到限制上，能装多少装多少。 */
        sc = Sb_ClampScale(sc, a.Width, a.Height);
        if (sc <= 0.02) return 0.0;
        rw = (int)((double)a.Width * sc + 0.5);
        rh = (int)((double)a.Height * sc + 0.5);
        if (rw < 320 || rh < 240) return 0.0;
    }
    return sc;
}

/* 不查缓存的原始倍率。 */
static double Sb_ScaleRaw(void)
{
    if (s_Level != SB_LEV_MAX) return s_LevScale[s_Level];
    if (!s_MaxOk) { s_MaxScale = Sb_CalcMaxScale(); s_MaxOk = (s_MaxScale > 0.0); }
    return s_MaxOk ? s_MaxScale : s_LevScale[s_LevOne];
}

/* ---------------- 档位查询 ----------------
 * SB_Zoomed 走缓存 (省每 blit/每文本的 double 比较)。 */
static int s_ZoomedCache = 0;
static void Sb_SyncZoomed(void)
{
    s_ZoomedCache = (s_Enable && Sb_ScaleRaw() != 1.0) ? 1 : 0;
}
int SB_Level(void)    { return s_Enable ? s_Level : s_NLev - 1; }
double SB_Scale(void) { return s_Enable ? Sb_ScaleRaw() : 1.0; }
int SB_In(void)       { return (s_Enable && Sb_ScaleRaw() < 1.0) ? 1 : 0; }
int SB_Out(void)      { return (s_Enable && Sb_ScaleRaw() > 1.0) ? 1 : 0; }
int SB_Zoomed(void)   { return s_ZoomedCache; }

/* CCToolTip 选面/量宽用的矩形影子 = 满视图区 A。1x 时 A == R。 */
static unsigned g_AView[4] = { 0u, 0u, 1752u, 1048u };

const unsigned* SB_AViewBlock(void) { return g_AView; }

void SB_PublishAView(void)
{
    RectangleStruct a;
    FM_GetViewRects(&a, 0);
    if (a.Width < 16 || a.Height < 16) return;
    g_AView[0] = (unsigned)a.X;
    g_AView[1] = (unsigned)a.Y;
    g_AView[2] = (unsigned)a.Width;
    g_AView[3] = (unsigned)a.Height;
}

/* 缩小档大面 (定义在文件后部)。返回 1 = 可用。 */
static int SB_EnsureBig(int w, int h);
static void SB_SurfaceDestroy(void);
static void Sb_SetLevel(int lv, const char* why);

/* 视口矩形 = 视图区 * scale。ViewBounds 恒为 (0,0,w,h)。
 * 缩小档须先建好大面，建不出返回 0 退回 A。 */
int SB_GetViewRect(RectangleStruct* out, double scale)
{
    RectangleStruct a;
    int w, h;
    double s = scale;
    if (!s_Enable || !out) return 0;
    FM_GetViewRects(&a, 0);
    if (a.Width < 32 || a.Height < 32) return 0;
    /* R 超限钳到限制上。三处取 R 须同线。0 = 不钳制。 */
    s = Sb_ClampScale(s, a.Width, a.Height);
    w = (int)((double)a.Width * s + 0.5);
    h = (int)((double)a.Height * s + 0.5);
    if (w < 32) w = 32;
    if (h < 32) h = 32;
    if (Sb_OverLimit(w, h)) return 0;
    if (s > 1.0 && !SB_EnsureBig(w, h)) return 0;
    out->X = 0;
    out->Y = 0;
    out->Width = w;
    out->Height = h;
    return 1;
}

/* ---------------- 16bpp 像素行的快拷 ----------------
 * 本工程无 CRT (memcpy 是逐字节循环)，每帧数 MB 的搬运走它太贵。
 * 这里按 32 位字搬，输出与 memcpy 逐位相同。 */
typedef unsigned int SbWord __attribute__((may_alias, aligned(1)));

static void Sb_CopyPx(unsigned short* d, const unsigned short* s, int n)
{
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    SbWord a, b, c, e;
    while (n > 0 && (((unsigned long)dp) & 3u) != 0u)
    { *(unsigned short*)dp = *(const unsigned short*)sp; dp += 2; sp += 2; n--; }
    while (n >= 8)
    {
        a = *(const SbWord*)sp;       b = *(const SbWord*)(sp + 4);
        c = *(const SbWord*)(sp + 8); e = *(const SbWord*)(sp + 12);
        *(SbWord*)dp = a;             *(SbWord*)(dp + 4) = b;
        *(SbWord*)(dp + 8) = c;       *(SbWord*)(dp + 12) = e;
        dp += 16; sp += 16; n -= 8;
    }
    while (n >= 4)
    {
        a = *(const SbWord*)sp; b = *(const SbWord*)(sp + 4);
        *(SbWord*)dp = a; *(SbWord*)(dp + 4) = b;
        dp += 8; sp += 8; n -= 4;
    }
    while (n-- > 0)
    { *(unsigned short*)dp = *(const unsigned short*)sp; dp += 2; sp += 2; }
}

/* 最近邻定点重采样。目的矩形须按目标面尺寸再夹一次。 */
static unsigned Sb_Step(int srcW, int dstW)
{
    if (dstW <= 0) return 0;
    return (unsigned)(((unsigned long)srcW << 16) / (unsigned long)dstW);
}
static int Sb_MinI(int a, int b) { return a < b ? a : b; }
static int Sb_MaxI(int a, int b) { return a > b ? a : b; }

/* 目标/源矩形按所在面尺寸裁剪 (改分辨率瞬间可能越界)。 */
static void Sb_ClipA(RectangleStruct* a, int W, int H)
{
    if (a->X < 0) { a->Width += a->X; a->X = 0; }
    if (a->Y < 0) { a->Height += a->Y; a->Y = 0; }
    if (a->X + a->Width > W) a->Width = W - a->X;
    if (a->Y + a->Height > H) a->Height = H - a->Y;
}
static void Sb_ClipR(RectangleStruct* r, int W, int H)
{
    if (r->X < 0) r->X = 0;
    if (r->Y < 0) r->Y = 0;
    if (r->X + r->Width > W) r->Width = W - r->X;
    if (r->Y + r->Height > H) r->Height = H - r->Y;
}

/* 目标列到源列的映射表 (够大就复用)。开不出返回 0，调用方退回逐列计算。 */
#define SB_COLMAP_MAX 16384
static int* s_Col = 0;
static int s_ColN = 0;

static int* Sb_ColMap(int n)
{
    void* p;
    if (n <= 0 || n > SB_COLMAP_MAX) return 0;
    if (n <= s_ColN) return s_Col;
    if (s_Col) VirtualFree(s_Col, 0, MEM_RELEASE);
    s_Col = 0;
    s_ColN = 0;
    p = VirtualAlloc(0, (size_t)n * sizeof(int), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return 0;
    s_Col = (int*)p;
    s_ColN = n;
    return s_Col;
}

static long Sb_Resample(unsigned char* sb, int sp,
                        unsigned char* db, int dp, int iw, int ih,
                        int sx, int sy, int rw, int rh,
                        int dx, int dy, int dw, int dh,
                        int cx0, int cy0, int cx1, int cy1)
{
    unsigned xstep, ystep, acc;
    int y, x0, y0, x1, y1, w, i;
    int* cm;
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
    w = x1 - x0;
    /* 精确 2:1 下采样 (0.5x 档)：步幅恒为 2，直读即可，不要乘法与映射表。
     * 与通用路逐位相同 (xstep 恰为 2<<16)，见 test/scalebar_resample_test.c。 */
    if (rw == dw * 2 && rh == dh * 2)
    {
        int x;
        for (y = y0; y < y1; y++)
        {
            unsigned short* drow = (unsigned short*)(db + (size_t)(dy + y) * dp);
            const unsigned short* srow = (const unsigned short*)
                (sb + (size_t)(sy + y * 2) * sp);
            for (x = x0; x < x1; x++)
                drow[dx + x] = srow[sx + x * 2];
        }
        return (long)w * (long)(y1 - y0);
    }
    /* 列映射每帧算一次，内层只剩两次访存。表按目标绝对列建 (起点 x0)。 */
    cm = Sb_ColMap(w);
    if (cm)
    {
        acc = (unsigned)x0 * xstep;
        for (i = 0; i < w; i++) { cm[i] = (int)(acc >> 16); acc += xstep; }
    }
    for (y = y0; y < y1; y++)
    {
        unsigned short* drow = (unsigned short*)(db + (size_t)(dy + y) * dp);
        const unsigned short* srow = (const unsigned short*)
            (sb + (size_t)(sy + (int)(((unsigned)y * ystep) >> 16)) * sp);
        if (cm)
        {
            unsigned short* p2 = drow + dx + x0;
            const unsigned short* s2 = srow + sx;
            for (i = 0; i < w; i++) p2[i] = s2[cm[i]];
        }
        else
        {
            int x;
            for (x = x0; x < x1; x++)
                drow[dx + x] = srow[sx + (int)(((unsigned)x * xstep) >> 16)];
        }
    }
    return (long)w * (long)(y1 - y0);
}

/* 某关口没通过时的原因码 (限频打一条)。 */
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

/* ---------------- 放大档：把放大结果写进 Composite 的视图区 ----------------
 * 写 Composite (上屏 blit 的源)。源与目标在同一张面，先拷出再原地放大。 */
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

/* ---------------- 帧入口：把干净 R 还给引擎 (放大档) ----------------
 * 放大是就地做的 (R ⊆ A)，呈现后 R 里留着放大像素；引擎光栅是增量的，
 * 视口移动时先整幅位移拷贝 (源就是 R) 再补新空条，不还原就会搬出残影。
 * 还原用呈现前拷在 s_Scratch 里的那份干净 R。静止时引擎不读 R，
 * 只还原即可；移动的帧另整块标脏 (见 SB_ViewPreRender)。
 * 只认 pitch 与 R 的几何，不认面指针 (引擎双缓冲会轮换)。
 * 纯渲染面写入，联机安全。 */
/* 挂起的干净 R 快照 (SB_CompositeMagnify 存，下一帧入口还原)。 */
static struct { int armed, pitch, x, y, w, h, rowB, frame, warned; }
    s_Rst = { 0, 0, 0, 0, 0, 0, 0, -1, 0 };

/* 取消挂起 (离开放大档 / 换档时)。 */
static void Sb_DropRestore(void)
{
    s_Rst.armed = 0;
    s_Rst.rowB = 0;
}

/* 把挂起的干净 R 拷回 Composite 的 R 位置。返回 1 = 还原过。
 * 门不过就静默跳过 (下一帧自愈)。 */
static int Sb_RestoreView(void)
{
    void* comp;
    unsigned char *cb, *kb;
    RectangleStruct* vb;
    RectangleStruct r;
    int cp, cw, ch, y;
    s_Rst.armed = 0;
    if (!s_Scratch || s_Rst.rowB <= 0) return 0;
    comp = *(void**)ADDR_SURF_COMPOSITE;
    if (!comp || !IsPlausiblePtr(comp)) return 0;
    cp = GtSurfGet(comp, GtVT_GET_PITCH);
    cw = GtSurfGet(comp, GtVT_GET_WIDTH);
    ch = GtSurfGet(comp, GtVT_GET_HEIGHT);
    if (cp != s_Rst.pitch || cp <= 0 || cp < cw * 2) return 0;
    vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    if (!vb) return 0;
    r = *vb;
    /* R 须与快照一致 (换档/读档/改分辨率会改它)。 */
    if (r.X != s_Rst.x || r.Y != s_Rst.y || r.Width != s_Rst.w || r.Height != s_Rst.h)
        return 0;
    if (r.Width < 16 || r.Height < 16 || r.X < 0 || r.Y < 0) return 0;
    if (r.X + r.Width > cw || r.Y + r.Height > ch) return 0;
    if (r.Width * 2 != s_Rst.rowB) return 0;
    cb = GtLock(comp);
    if (!cb)
    {
        if (!s_Rst.warned)
        { s_Rst.warned = 1; LogLine("sbar: WARN view restore lock failed"); }
        return 0;
    }
    kb = (unsigned char*)s_Scratch;
    for (y = 0; y < r.Height; y++)
        Sb_CopyPx((unsigned short*)(cb + (size_t)(r.Y + y) * cp + (size_t)r.X * 2),
                  (const unsigned short*)(kb + (size_t)y * s_Rst.rowB), r.Width);
    GtUnlock(comp);
    return 1;
}

/* 整块标脏 (移动那一帧用)。 */
static void Sb_MarkViewDirty(void)
{
    RectangleStruct* vb = (RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    void* tac = *(void**)(size_t)ADDR_TACTICAL_SB;
    FnRegDirtyArea fn;
    RectangleStruct r;
    if (!tac || !IsPlausiblePtr(tac)) return;
    if (!vb) return;
    if (vb->Width < 64 || vb->Width > 8192) return;
    if (vb->Height < 64 || vb->Height > 8192) return;
    r.X = 0;
    r.Y = 0;
    r.Width = vb->Width;
    r.Height = vb->Height;
    fn = (FnRegDirtyArea)(size_t)ADDR_REG_DIRTY_AREA;
    fn(tac, r, 1);
}

/* 换档后连催几帧整块重画，盖住新露出的那圈。 */
static int s_ViewDirtyN = 0;
static int s_ViewDirtyFrame = -1;

/* 上次呈现时的视口原点，判引擎这一趟会不会跑位移拷贝。 */
static int s_LastOX = 0, s_LastOY = 0;
static int s_MoveSeen = 0;

/* 换档时调: R 尺寸变了，快照作废并连催几帧整块重画。 */
static void Sb_LevelChanged(void)
{
    Sb_DropRestore();
    s_ViewDirtyN = 4;
    s_ViewDirtyFrame = -1;
    s_Rst.warned = 0;
    s_MoveSeen = 0;
}

/* 记下刚呈现完的视口原点。 */
static void Sb_NoteOrigin(void)
{
    int ox = 0, oy = 0, vw = 0, vh = 0;
    if (!Sb_ViewPx(&ox, &oy, &vw, &vh)) return;
    s_LastOX = ox;
    s_LastOY = oy;
    s_MoveSeen = 1;
}

/* 视口原点变了就会跑位移拷贝。读不到按会处理。 */
static int Sb_ViewMoved(void)
{
    int ox = 0, oy = 0, vw = 0, vh = 0;
    if (!s_MoveSeen) return 1;
    if (!Sb_ViewPx(&ox, &oy, &vw, &vh)) return 1;
    return (ox != s_LastOX || oy != s_LastOY) ? 1 : 0;
}

void SB_ViewPreRender(void)
{
    int had, ok = 0, moved;
    if (!s_Enable) return;
    /* 缩小档走大面那套；1x 不用还原。 */
    if (!SB_In()) { Sb_DropRestore(); s_ViewDirtyN = 0; s_MoveSeen = 0; return; }

    /* 还原先行 (动不动都做)：位移拷贝真要读 R 时这是最后一道保险。 */
    had = s_Rst.armed;
    if (had) ok = Sb_RestoreView();

    moved = Sb_ViewMoved();
    if (ok && !moved && s_ViewDirtyN <= 0) return;

    if (had && !ok && s_ViewDirtyN <= 0)
    {
        /* 还原被几何门挡掉 = 快照已过期，连催几帧标脏盖住。 */
        s_ViewDirtyN = 4;
        s_ViewDirtyFrame = -1;
        if (!s_Rst.warned)
        { s_Rst.warned = 1; LogLine("sbar: view restore stale -> dirty fallback"); }
    }
    if (s_ViewDirtyN > 0)
    {
        int fr = *(int*)(size_t)ADDR_FRAME_SB;
        if (fr != s_ViewDirtyFrame) { s_ViewDirtyFrame = fr; s_ViewDirtyN--; }
    }
    /* 移动 (或换档后几帧) 才整块标脏。 */
    Sb_MarkViewDirty();
}

/* 整数倍放大的快路: n 倍放大只需把源行横向铺 n 次、再把铺好的行纵向重复
 * n 次，写入量约 1/n。横向按字对写。只在 n 是 2 的幂时调用 (见调用处)。 */
static void Sb_ResampleNx(const unsigned char* sb, int sp, int sw, int sh,
                          unsigned char* db, int dp, int n, int ax, int ay)
{
    int y, x, k, rowPx = sw * n;
    unsigned char* d0 = db + (size_t)ay * dp + (size_t)ax * 2;
    int pairs = n >> 1;
    for (y = 0; y < sh; y++, d0 += (size_t)n * dp)
    {
        const unsigned short* srow =
            (const unsigned short*)(sb + (size_t)y * sp);
        unsigned short* d = (unsigned short*)d0;
        for (x = 0; x < sw; x++)
        {
            SbWord w = (SbWord)((unsigned)srow[x] * 0x00010001u);
            for (k = 0; k < pairs; k++) *(SbWord*)(d + 2 * k) = w;
            d += n;
        }
        for (k = 1; k < n; k++)
            Sb_CopyPx((unsigned short*)(d0 + (size_t)k * dp),
                      (const unsigned short*)d0, rowPx);
    }
}

int SB_CompositeMagnify(void)
{
    RectangleStruct a, r;
    void* comp;
    unsigned char *cb, *sb;
    int cp, cw, ch, need, y, fr;
    if (!s_Enable) return 0;
    if (!SB_In()) return 0;
    /* 同帧第二遍 (兜底消息帧钩) 去重，否则把放大像素当干净 R 存下。
     * 暂停时帧号冻结 (迷你循环反复 Render 同一帧)：还原已把 armed 清零，
     * 此时必须重放，不按帧号去重。 */
    fr = *(int*)(size_t)ADDR_FRAME_SB;
    if (s_Rst.frame == fr && s_Rst.armed) return 1;
    MOE_SetPhase(MP_COMPMAG);
    FM_GetViewRects(&a, &r);
    if (a.Width < 32 || r.Width < 16 || r.Height < 16) return Sb_Skip(1);
    comp = *(void**)ADDR_SURF_COMPOSITE;
    if (!comp || !IsPlausiblePtr(comp)) return Sb_Skip(2);
    cp = GtSurfGet(comp, GtVT_GET_PITCH);
    cw = GtSurfGet(comp, GtVT_GET_WIDTH);
    ch = GtSurfGet(comp, GtVT_GET_HEIGHT);
    if (cp <= 0 || cw < 8 || ch < 8) return Sb_Skip(3);
    /* 下面按 16bpp 算：pitch 不够 2*宽就跳过，否则越过行尾。 */
    if (cp < cw * 2) return Sb_Skip(7);
    Sb_ClipA(&a, cw, ch);
    if (a.Width < 16 || a.Height < 16) return Sb_Skip(1);
    Sb_ClipR(&r, cw, ch);
    if (r.Width < 16 || r.Height < 16) return Sb_Skip(1);
    need = r.Width * 2;
    sb = Sb_Scratch(need * r.Height);
    if (!sb) return Sb_Skip(6);
    cb = GtLock(comp);
    if (!cb) return Sb_Skip(4);
    for (y = 0; y < r.Height; y++)
        Sb_CopyPx((unsigned short*)(sb + (size_t)y * need),
                  (const unsigned short*)(cb + (size_t)(r.Y + y) * cp
                                             + (size_t)r.X * 2), r.Width);
    {
        int n = 0;
        if (a.Width % r.Width == 0) n = a.Width / r.Width;
        /* 只在 n 是 2 的幂时走快路 (通用路 16.16 步长在 n=3 时差一格)。 */
        if (n >= 2 && n <= 8 && (n & (n - 1)) == 0
            && a.Height == r.Height * n
            && a.X >= 0 && a.Y >= 0 && a.X + a.Width <= cw && a.Y + a.Height <= ch)
        {
            Sb_ResampleNx(sb, need, r.Width, r.Height, cb, cp, n, a.X, a.Y);
        }
        else
        {
            Sb_Resample(sb, need, cb, cp, cw, ch,
                        0, 0, r.Width, r.Height,
                        a.X, a.Y, a.Width, a.Height,
                        a.X, a.Y, a.X + a.Width, a.Y + a.Height);
        }
    }
    /* 呈现前不往 R 写 1:1 内容：R ⊆ A，写回去会在左上角露出一块没放大的方块。
     * 快照由下一帧入口的 SB_ViewPreRender 拷回 R。 */
    GtUnlock(comp);
    s_Rst.armed = 1;
    s_Rst.pitch = cp;
    s_Rst.x = r.X;
    s_Rst.y = r.Y;
    s_Rst.w = r.Width;
    s_Rst.h = r.Height;
    s_Rst.rowB = need;
    s_Rst.frame = *(int*)(size_t)ADDR_FRAME_SB;
    Sb_NoteOrigin();
    return 1;
}

/* 缩小档自建 R 大面：只在 Render 期间换进全局，完事还原 + 降采样。 */
typedef void* (__cdecl    *SbAllocFn)(unsigned);
typedef void* (__thiscall *SbDSurfCtorFn)(void*, int, int, int, int);
typedef void  (__thiscall *SbDSurfDtorFn)(void*, int);

static void* Sb_NewSurf(int w, int h)
{
    SbAllocFn al = (SbAllocFn)(size_t)ADDR_OP_NEW;
    void* o;
    if (!al || w < 8 || h < 8) return 0;
    o = al(0x24);
    if (!o || !IsPlausiblePtr(o)) return 0;
    ((SbDSurfCtorFn)(size_t)ADDR_DSURF_CTOR)(o, w, h, 1, 1);
    /* 建面失败 (尺寸对不上) 就销毁别用。 */
    if (GtSurfGet(o, GtVT_GET_WIDTH) != w || GtSurfGet(o, GtVT_GET_HEIGHT) != h)
    {
        void** vt = *(void***)o;
        if (vt && IsPlausiblePtr(vt) && IsPlausiblePtr(vt[0]))
            ((SbDSurfDtorFn)vt[0])(o, 1);
        return 0;
    }
    return o;
}

static void Sb_FreeSurf(void* s)
{
    void** vt;
    if (!s || !IsPlausiblePtr(s)) return;
    vt = *(void***)s;
    if (!vt || !IsPlausiblePtr(vt) || !IsPlausiblePtr(vt[0])) return;
    ((SbDSurfDtorFn)vt[0])(s, 1);
}

/* 新面填图外底色 (深灰)，面缓冲内容未定义，先铺底再画地图。 */
static unsigned short s_VoidPx = 0;
static int s_VoidPxOk = 0;
static unsigned short Sb_VoidPixel(void)
{
    unsigned r, g, b, px;
    if (s_VoidPxOk) return s_VoidPx;
    if (!g_Calibrated) Calibrate();
    r = (unsigned)(((s_VoidRgb >> 16) & 0xFF) >> (8 - g_RBits)) & ((1u << g_RBits) - 1u);
    g = (unsigned)(((s_VoidRgb >> 8) & 0xFF) >> (8 - g_GBits)) & ((1u << g_GBits) - 1u);
    b = (unsigned)((s_VoidRgb & 0xFF) >> (8 - g_BBits)) & ((1u << g_BBits) - 1u);
    px = (r << (g_GBits + g_BBits)) | (g << g_BBits) | b;
    s_VoidPx = (unsigned short)px;
    s_VoidPxOk = 1;
    return (unsigned short)px;
}

static void Sb_FillOpaque(unsigned char* base, int pitch, int W, int H,
                          int x0, int y0, int w, int h, unsigned short px);

static void Sb_ClearSurf(void* s)
{
    unsigned char* b;
    int pitch, w, h;
    if (!s || !IsPlausiblePtr(s)) return;
    pitch = GtSurfGet(s, GtVT_GET_PITCH);
    w = GtSurfGet(s, GtVT_GET_WIDTH);
    h = GtSurfGet(s, GtVT_GET_HEIGHT);
    if (pitch <= 0 || w < 8 || h < 8) return;
    if (pitch < w * 2) return;          /* 不是 16bpp 的面: 逐行会越过行尾 */
    b = GtLock(s);
    if (!b) return;
    Sb_FillOpaque(b, pitch, w, h, 0, 0, w, h, Sb_VoidPixel());
    GtUnlock(s);
}

/* 是不是自建的大面 (换面/绘制前的把关用)。 */
static int Sb_IsBigSurf(void* s)
{
    if (!s || !IsPlausiblePtr(s)) return 0;
    return (s == s_BigComp || s == s_BigMap ||
            s == s_CurC || s == s_CurM || s == s_LastFinal);
}

/* 载入门：进度屏载入中地图半成品，缩小档按 1x 退回。 */
#define ADDR_LOAD_RENDER  0x00A9FA94u

static int s_LoadGate = 0;
static int SB_Loading(void)
{
    int on = (*(volatile int*)(size_t)ADDR_LOAD_PHASE != 0)
          || (*(volatile unsigned char*)(size_t)ADDR_LOAD_RENDER != 0);
    if (on != s_LoadGate)
    {
        s_LoadGate = on;
        LogLine(on ? "sbar: loading gate ON (zoom-out held as 1x)"
                   : "sbar: loading gate OFF");
    }
    return on;
}
/* Render 是否在栈上 (SwapIn 置 1，SwapOut/AfterMap 清 0)。 */
static int s_InRender = 0;
static int s_RefWarned = 0;

void SB_RenderSwapIn(void)
{
    void* c;
    /* 载入中一切重活退回 1x。 */
    if (SB_Loading()) return;
    s_InRender = 1;
    if (!SB_Out() || !s_SurfActive) return;
    if (s_RenderSwapped) return;
    c = *(void**)ADDR_SURF_COMPOSITE;
    s_RenderOldComp = c;
    s_RenderOldMap  = *(void**)ADDR_SURF_MAP;
    /* 入口已是大面 = 上一帧没还原，改用记住的窗口面。 */
    if (Sb_IsBigSurf(c))
    {
        if (!s_LeakWarned)
        {
            s_LeakWarned = 1;
            LogLine("sbar: WARN composite leak from last frame -> re-armed");
        }
        if (s_WinComp) s_RenderOldComp = s_WinComp;
        if (s_WinMap)  s_RenderOldMap = s_WinMap;
    }
    else
    {
        s_WinComp = c;
        s_WinMap = s_RenderOldMap;
    }
    s_RenderSwapped = 1;
    s_SwapSeen = 1;
    s_Suspended = 0;
    if (s_RenderOldComp == s_CurC) return;      /* 已经装好 */
    *(void**)ADDR_SURF_COMPOSITE = s_CurC;
    *(void**)ADDR_SURF_MAP       = s_CurM;
}

/* 让出 (换回窗口面)：Render 里按窗口坐标画的两串不能烙进 R。 */
void SB_RenderSuspend(int forCursor)
{
    (void)forCursor;
    if (!s_RenderSwapped || s_Suspended) return;
    if (!s_RenderOldComp || !IsPlausiblePtr(s_RenderOldComp)) return;
    *(void**)ADDR_SURF_COMPOSITE = s_RenderOldComp;
    *(void**)ADDR_SURF_MAP       = s_RenderOldMap;
    *(void**)ADDR_CUR_SURF_SB    = s_RenderOldComp;
    s_Suspended = 1;
}

/* 装回大面。[0x887314] 须一起装，否则引擎拿错面走虚表崩。 */
void SB_RenderResume(void)
{
    if (!s_Suspended) return;
    s_Suspended = 0;
    if (!s_RenderSwapped) return;
    *(void**)ADDR_SURF_COMPOSITE = s_CurC;
    *(void**)ADDR_SURF_MAP       = s_CurM;
    *(void**)ADDR_CUR_SURF_SB    = s_CurC;
}

void SB_RenderSwapOut(void)
{
    s_InRender = 0;
    if (!s_RenderSwapped) return;
    s_RenderSwapped = 0;
    /* 跟住引擎双缓冲轮换：记下本帧分配，下帧原样装回。让出期间没跑三趟就不能记。 */
    if (!s_Suspended)
    {
        s_CurC = *(void**)ADDR_SURF_COMPOSITE;
        s_CurM = *(void**)ADDR_SURF_MAP;
        s_LastFinal = s_CurC;
    }
    else s_SwapSeen = 0;                 /* 本帧没画就不能拿上一帧的画面降采样 */
    s_Suspended = 0;
    g_PreCurWin = 0;
    g_PreCurSurf = 0;
    /* [0x887314] 是 UI 阶段唯一认的面，不能留成大面。 */
    if (Sb_IsBigSurf(s_RenderOldComp) && s_WinComp && IsPlausiblePtr(s_WinComp))
        s_RenderOldComp = s_WinComp;
    if (Sb_IsBigSurf(s_RenderOldMap) && s_WinMap && IsPlausiblePtr(s_WinMap))
        s_RenderOldMap = s_WinMap;
    if (Sb_IsBigSurf(s_RenderOldComp) && !s_LeakWarned)
    {
        s_LeakWarned = 1;
        LogLine("sbar: WARN window composite unknown, chrome may ghost");
    }
    /* 无条件还原窗口面 (引擎 UI 往它画，不还原就把 UI 烙进大面)。 */
    *(void**)ADDR_SURF_COMPOSITE = s_RenderOldComp;
    *(void**)ADDR_SURF_MAP = s_RenderOldMap;
    *(void**)ADDR_CUR_SURF_SB = s_RenderOldComp;
}

/* 迷雾越界写护栏：ViewBounds=R 而 [0x887314] 是窗口面时写入越界，
 * 临时换 s_CurC 挡。1x 空操作。 */
static int s_ShroudWarn = 0;
static int s_ShroudHit = 0;

void SB_ShroudGuard(void)
{
    void* cur;
    /* PiP 第二摄像机离屏渲染中: [0x887314] 是 PiP 面, 不能按本模块的 s_CurC 改写。 */
    if (PiP_InRender()) return;
    if (!s_Enable) return;
    if (!SB_Out() || !s_SurfActive) return;
    if (!s_CurC || !IsPlausiblePtr(s_CurC)) return;
    cur = *(void**)ADDR_CUR_SURF_SB;
    if (cur == s_CurC) return;
    if (Sb_IsBigSurf(cur)) return;
    /* 不发虚表调用，只做纯指针比较。 */
    s_ShroudHit++;
    *(void**)ADDR_CUR_SURF_SB = s_CurC;
}

void SB_ShroudReport(void)
{
    char b1[16], buf[128];
    unsigned p = 0;
    if (s_ShroudHit <= 0 || s_ShroudWarn) return;
    s_ShroudWarn = 1;
    p = AppendCStr(buf, p, "sbar: shroud guard fired x", sizeof(buf));
    UtoA((unsigned)s_ShroudHit, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

/* 崩溃处理调：追记大面指针 + 存活标志。只读指针，不解引用，崩溃时安全。 */

/* 三趟地图画完立刻调：还原窗口面 + 铺图外 + 降采样。须早于引擎 UI。 */
void SB_RenderAfterMap(void)
{
    /* Render 照原版跑, 但 s_InRender 必须清 (Render 后半仍在栈上)。*/
    if (!s_RenderSwapped) { s_InRender = 0; return; }
    SB_RenderSwapOut();
    SB_ShroudReport();
    /* 载入中不降采样 (三趟跑在 loader 自己的面上)。让出中由 Demagnify 内部跳过。*/
    if (SB_Out() && s_SurfActive && !SB_Loading())
    {
        SB_PaintVoid();
        SB_CompositeDemagnify();
    }
}

/* 边缘卷屏：入口点换回物理鼠标。1x 恒等。 */
#define SB_WWMOUSE_OFF_X  0x1C
#define SB_WWMOUSE_OFF_Y  0x20
void SB_EdgeScrollPointFix(Point2D* pt)
{
    void* m;
    if (!pt || !IsPlausiblePtr(pt)) return;
    if (!s_Enable || !SB_Zoomed()) return;
    m = *(void**)ADDR_WWMOUSE_SB;
    if (!m || !IsPlausiblePtr(m)) return;
    pt->X = *(int*)((char*)m + SB_WWMOUSE_OFF_X);
    pt->Y = *(int*)((char*)m + SB_WWMOUSE_OFF_Y);
}

static void* s_RetComp = 0;   /* 退休槽: 上一代大面, 推迟释放 (见下) */
static void* s_RetMap = 0;
static void SB_SurfaceDestroy(void)
{
    if (!s_SurfActive) return;
    SB_RenderSwapOut();
    /* 诊断: 释放前引擎全局是否还指着大面 (正常恒否, 说明还原漏了)。*/
    {
        void* c = *(void**)ADDR_SURF_COMPOSITE;
        void* m = *(void**)ADDR_SURF_MAP;
        void* d = *(void**)ADDR_CUR_SURF_SB;
        if (c == s_BigComp || c == s_BigMap || m == s_BigComp || m == s_BigMap
            || d == s_BigComp || d == s_BigMap)
            LogLine("sbar: WARN free big surf still referenced by engine");
    }
    /* 推迟一代释放：引擎 Draw 里可能还缓存着这对面指针。 */
    if (s_RetComp) Sb_FreeSurf(s_RetComp);
    if (s_RetMap) Sb_FreeSurf(s_RetMap);
    s_RetComp = s_BigComp; s_RetMap = s_BigMap;
    s_BigComp = s_BigMap = 0;
    s_CurC = s_CurM = s_LastFinal = 0;
    s_BigW = s_BigH = 0;
    s_BigCapW = s_BigCapH = 0;
    s_Suspended = 0;
    s_SurfActive = 0;
}

void SB_AppendCrashSurfs(char* buf, unsigned* p, unsigned cap)
{
    char b1[16];
    unsigned q = *p;
    q = AppendCStr(buf, q, " big=", cap);
    UtoA((unsigned)(size_t)s_BigComp, b1); q = AppendCStr(buf, q, b1, cap);
    q = AppendCh(buf, q, '/', cap);
    UtoA((unsigned)(size_t)s_BigMap, b1); q = AppendCStr(buf, q, b1, cap);
    q = AppendCStr(buf, q, " ret=", cap);
    UtoA((unsigned)(size_t)s_RetComp, b1); q = AppendCStr(buf, q, b1, cap);
    q = AppendCh(buf, q, '/', cap);
    UtoA((unsigned)(size_t)s_RetMap, b1); q = AppendCStr(buf, q, b1, cap);
    q = AppendCStr(buf, q, " act=", cap);
    UtoA((unsigned)s_SurfActive, b1); q = AppendCStr(buf, q, b1, cap);
    *p = q;
}

/* 建/复用 R 大的离屏大面。返回 1 = 可用；0 = 失败，调用方退回 A。 */
static int SB_EnsureBig(int w, int h)
{
    if (s_SurfFail) return 0;
    if (w < 320 || h < 240) return 0;
    /* 引擎不会画进大面时不能做缩小档。 */
    if (!g_RenderTramp)
    {
        if (!s_SurfWarned) { s_SurfWarned = 1;
            LogLine("sbar: zoom-out off (render hook missing)"); }
        return 0;
    }
    /* 引擎每帧互换两槽，窗口面须在没装过大面时抓。 */
    {
        void* wc = *(void**)ADDR_SURF_COMPOSITE;
        void* wm = *(void**)ADDR_SURF_MAP;
        if (wc && IsPlausiblePtr(wc) && !Sb_IsBigSurf(wc))
        {
            if (s_WinComp != wc)
            {
                if (!s_WinComp) s_PurgeFrames = 8;
                s_WinComp = wc;
                s_WinMap = wm;
            }
            s_WinW = GtSurfGet(s_WinComp, GtVT_GET_WIDTH);
            s_WinH = GtSurfGet(s_WinComp, GtVT_GET_HEIGHT);
        }
    }
    /* 尺寸只增不减。 */
    if (s_SurfActive && w <= s_BigCapW && h <= s_BigCapH)
    {
        if (s_BigW != w || s_BigH != h)
        {
            s_BigW = w; s_BigH = h;
            s_RedrawFrames = 4;
        }
        return 1;
    }
    SB_SurfaceDestroy();
    s_BigComp = Sb_NewSurf(w, h);
    if (!s_BigComp)
    {
        s_SurfFail = 1;
        if (!s_SurfWarned) { s_SurfWarned = 1;
            LogLine("sbar: zoom-out surface FAILED -> off"); }
        return 0;
    }
    /* 须两张面 (跟住引擎双缓冲轮换)：第二张建不出就全退。 */
    s_BigMap = Sb_NewSurf(w, h);
    if (!s_BigMap)
    {
        Sb_FreeSurf(s_BigComp); s_BigComp = 0;
        s_SurfFail = 1;
        if (!s_SurfWarned) { s_SurfWarned = 1;
            LogLine("sbar: zoom-out map surface FAILED -> off"); }
        return 0;
    }
    s_CurC = s_BigComp;
    s_CurM = s_BigMap;
    s_LastFinal = 0;
    s_BigW = w; s_BigH = h;
    s_BigCapW = w; s_BigCapH = h;
    s_SurfActive = 1;
    s_RedrawFrames = 4;
    /* 新面缓冲内容未定义，先铺底。 */
    Sb_ClearSurf(s_BigComp);
    Sb_ClearSurf(s_BigMap);
    {
        char b1[16], b2[16], buf[128];
        unsigned p = 0;
        p = AppendCStr(buf, p, "sbar: zoom-out surface ON ", sizeof(buf));
        UtoA((unsigned)w, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, 'x', sizeof(buf));
        UtoA((unsigned)h, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
        LogLine(buf);
    }
    return 1;
}

/* ZBuffer/ABuffer 按 R 重建。先建新再放旧，Render 不留空。 */
typedef void  (__cdecl    *SbFreeFn)(void*);
typedef void  (__thiscall *SbBufCtorFn)(void*, RectangleStruct);
typedef void  (__thiscall *SbBufDtorFn)(void*);

static void* Sb_BuildBuf(int isA, const RectangleStruct* r)
{
    SbAllocFn al = (SbAllocFn)(size_t)ADDR_OP_NEW;
    SbBufCtorFn ct;
    void* p;
    if (!al || !r) return 0;
    p = al(SB_BUF_OBJ_SIZE);
    if (!p || !IsPlausiblePtr(p)) return 0;
    ct = isA ? (SbBufCtorFn)(size_t)ADDR_ABUF_CTOR
             : (SbBufCtorFn)(size_t)ADDR_ZBUF_CTOR;
    ct(p, *r);                          /* 矩形按值传，不能传指针 */
    /* 与 Set_View_Dimensions 第 3 步一致。 */
    if (!isA) *(unsigned*)((unsigned char*)p + 0x24) = 0x8000u;
    return p;
}

static void Sb_DropBuf(int isA, void* p)
{
    if (!p || !IsPlausiblePtr(p)) return;
    ((SbBufDtorFn)(isA ? (size_t)ADDR_ABUF_DTOR : (size_t)ADDR_ZBUF_DTOR))(p);
    ((SbFreeFn)(size_t)ADDR_OP_DELETE)(p);
}

/* PiP 模块复用的离屏基建导出 (复用上面的 DSurface/缓冲构造)。 */
void* SB_NewOffscreen(int w, int h) { return Sb_NewSurf(w, h); }
void  SB_FreeOffscreen(void* s) { Sb_FreeSurf(s); }
void  SB_ClearOffscreen(void* s) { Sb_ClearSurf(s); }

int SB_NewBufPair(void** z, void** a, int w, int h)
{
    RectangleStruct t;
    void* nz;
    void* na;
    if (z) *z = 0;
    if (a) *a = 0;
    if (w < 64 || h < 64) return 0;
    if (Sb_OverLimit(w, h)) return 0;
    t.X = 0; t.Y = 0; t.Width = w; t.Height = h;
    nz = Sb_BuildBuf(0, &t);
    na = Sb_BuildBuf(1, &t);
    if (!nz || !na) { Sb_DropBuf(0, nz); Sb_DropBuf(1, na); return 0; }
    if (z) *z = nz; else Sb_DropBuf(0, nz);
    if (a) *a = na; else Sb_DropBuf(1, na);
    return 1;
}

void SB_FreeBufPair(void* z, void* a) { Sb_DropBuf(0, z); Sb_DropBuf(1, a); }

/* 缓冲真实尺寸直接读对象。 */
static int SB_BufDims(int* w, int* h)
{
    unsigned char* z = *(unsigned char**)ADDR_ZBUF_SB;
    int ww, hh;
    if (!z || !IsPlausiblePtr(z)) return 0;
    ww = *(int*)(z + 0x28);
    hh = *(int*)(z + 0x2C);
    if (ww < 16 || ww > 16384 || hh < 16 || hh > 16384) return 0;
    if (w) *w = ww;
    if (h) *h = hh;
    return 1;
}

/* 按 (bw,bh) 重建两块缓冲。返回 1 = 成功。*/
static int SB_ResizeBufs(int bw, int bh)
{
    RectangleStruct t;
    void *nz, *na, *oz, *oa;
    char b1[16], b2[16], buf[96];
    unsigned p = 0;
    if (bw < 64 || bh < 64) return 0;
    if (Sb_OverLimit(bw, bh)) return 0;
    t.X = 0; t.Y = 0; t.Width = bw; t.Height = bh;
    nz = Sb_BuildBuf(0, &t);
    na = Sb_BuildBuf(1, &t);
    if (!nz || !na)
    {
        Sb_DropBuf(0, nz);
        Sb_DropBuf(1, na);
        return 0;
    }
    oz = *(void**)ADDR_ZBUF_SB;
    oa = *(void**)ADDR_ABUF_SB;
    *(void**)ADDR_ZBUF_SB = nz;
    *(void**)ADDR_ABUF_SB = na;
    Sb_DropBuf(0, oz);
    Sb_DropBuf(1, oa);
    s_BufW = bw;
    s_BufH = bh;
    p = AppendCStr(buf, p, "sbar: zbuf <- ", sizeof(buf));
    UtoA((unsigned)bw, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, 'x', sizeof(buf));
    UtoA((unsigned)bh, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
    LogLine(buf);
    return 1;
}

/* 缓冲是否已经是 (w,h)：以对象里的真实值为准。 */
static int SB_BufIs(int w, int h)
{
    int ww = 0, hh = 0;
    if (s_BufW != w || s_BufH != h) return 0;
    if (!SB_BufDims(&ww, &hh)) return 0;
    return (ww == w && hh == h);
}

/* ZBuffer/ABuffer 尺寸 + 新面催重画。每循环在 FM_Poll 之后调。 */
void SB_PrepareSurface(void)
{
    RectangleStruct a, r;
    if (!s_Enable) return;
    if (SB_Loading()) return;
    /* Render 在栈上时整个推迟。 */
    if (s_InRender)
    {
        if (!s_RefWarned)
        {
            s_RefWarned = 1;
            LogLine("sbar: prepare deferred (render on stack)");
        }
        return;
    }
    MOE_SetPhase(MP_ZBUF);
    /* 上一帧泄漏自愈。 */
    if (s_RenderSwapped) { MOE_SetPhase(MP_SURFSWAP); SB_RenderSwapOut(); }
    FM_GetViewRects(&a, &r);
    if (!SB_Out())
    {
        if (s_SurfActive) SB_SurfaceDestroy();
        s_RedrawFrames = 0;
        s_SurfFail = 0;
        /* 离开缩小档把缓冲缩回 A。以对象真实尺寸为准 (见 SB_BufIs)。 */
        if (a.Width >= 64 && a.Height >= 64 && !SB_BufIs(a.Width, a.Height))
            SB_ResizeBufs(a.Width, a.Height);
        return;
    }
    if (!s_SurfActive || s_BigW != r.Width || s_BigH != r.Height)
    {
        if (!SB_EnsureBig(r.Width, r.Height)) return;   /* 建不出: 保持 A */
    }
    /* 缩小档缓冲须按 R。 */
    if (!SB_BufIs(r.Width, r.Height))
    {
        SB_ResizeBufs(r.Width, r.Height);
        /* 建不出就退回 1x，不能让 Render 野读写。 */
        if (!SB_BufIs(r.Width, r.Height))
        {
            if (!SB_ResizeBufs(a.Width, a.Height))
                s_BufW = s_BufH = 0;
            s_SurfFail = 1;
            SB_SurfaceDestroy();
            if (!s_SurfWarned) { s_SurfWarned = 1;
                LogLine("sbar: zbuf alloc FAILED -> zoom-out off"); }
            /* 必须当帧把档位打回 1x 并重算视口，否则本帧 Render 野读。 */
            Sb_SetLevel(s_LevOne, "bufalloc-fail");
            FM_SyncViewNow();
            return;
        }
    }
    /* 洗大面：催整块 R 重画几帧。 */
    if (s_PurgeFrames > 0)
    {
        s_PurgeFrames--;
        if (s_RedrawFrames < s_PurgeFrames + 2) s_RedrawFrames = s_PurgeFrames + 2;
        if (!s_PurgeWarned)
        {
            s_PurgeWarned = 1;
            LogLine("sbar: purge big surfaces (force full redraw)");
        }
    }
    /* 新面催重画几帧。 */
    if (s_RedrawFrames > 0)
    {
        int fr = *(int*)(size_t)ADDR_FRAME_SB;
        if (fr != s_RedrawFrameNo) { s_RedrawFrameNo = fr; s_RedrawFrames--; }
        {
            void* tac = *(void**)(size_t)ADDR_TACTICAL_SB;
            if (tac && IsPlausiblePtr(tac))
            {
                RectangleStruct dr;
                dr.X = 0; dr.Y = 0; dr.Width = r.Width; dr.Height = r.Height;
                ((FnRegDirtyArea)(size_t)ADDR_REG_DIRTY_AREA)(tac, dr, 1);
            }
        }
    }
}

/* ---------------- 缩小档呈现: 大面的 R 区域 -> Composite 的 A ---------------- */
int SB_CompositeDemagnify(void)
{
    RectangleStruct a, r;
    void* src = s_LastFinal;
    void* dst;
    unsigned char *sb, *db;
    int sp, sw, sh, dp, dww, dhh;
    if (!s_Enable) return 0;
    if (!SB_Out() || !s_SurfActive || !src) return 0;
    /* 本帧入口没换过大面 = 引擎没往大面画，跳过。 */
    if (!s_SwapSeen) return 0;
    s_SwapSeen = 0;
    MOE_SetPhase(MP_COMPMAG);
    FM_GetViewRects(&a, &r);
    if (a.Width < 16 || r.Width < 16 || r.Height < 16) return Sb_Skip(1);
    if (!IsPlausiblePtr(src)) return Sb_Skip(2);
    sp = GtSurfGet(src, GtVT_GET_PITCH);
    sw = GtSurfGet(src, GtVT_GET_WIDTH);
    sh = GtSurfGet(src, GtVT_GET_HEIGHT);
    if (sp <= 0 || sw < 8 || sh < 8) return Sb_Skip(3);
    if (sp < sw * 2) return Sb_Skip(7);
    dst = *(void**)ADDR_SURF_COMPOSITE;
    if (!dst || !IsPlausiblePtr(dst) || dst == src) return Sb_Skip(2);
    /* 目标仍是大面 = 还原漏了，跳过并自愈。 */
    if (Sb_IsBigSurf(dst))
    {
        if (!s_LeakWarned)
        {
            s_LeakWarned = 1;
            LogLine("sbar: WARN demagnify target still big -> skipped");
        }
        return Sb_Skip(8);
    }
    dp = GtSurfGet(dst, GtVT_GET_PITCH);
    dww = GtSurfGet(dst, GtVT_GET_WIDTH);
    dhh = GtSurfGet(dst, GtVT_GET_HEIGHT);
    if (dp <= 0 || dww < 8 || dhh < 8) return Sb_Skip(3);
    if (dp < dww * 2) return Sb_Skip(7);
    Sb_ClipA(&a, dww, dhh);
    if (a.Width < 16 || a.Height < 16) return Sb_Skip(1);
    Sb_ClipR(&r, sw, sh);
    if (r.Width < 16 || r.Height < 16) return Sb_Skip(1);
    sb = GtLock(src);
    if (!sb) return Sb_Skip(4);
    db = GtLock(dst);
    if (!db) { GtUnlock(src); return Sb_Skip(4); }
    Sb_Resample(sb, sp, db, dp, dww, dhh,
                r.X, r.Y, r.Width, r.Height,
                a.X, a.Y, a.Width, a.Height,
                a.X, a.Y, a.X + a.Width, a.Y + a.Height);
    GtUnlock(dst);
    GtUnlock(src);
    /* 每帧作废 chrome 脏缓存：缩小档下底栏带有其它按帧刷新的来源，
     * 只靠刻度条按需刷会留拖影（原因待查），先恢复每帧清。 */
    *(unsigned*)(size_t)SB_CHROME_DIRTY_N = 0;
    return 1;
}

/* 整行/整列是否全为图外底色。 */
static int Sb_RowIs(unsigned char* base, int pitch, int x0, int x1, int y,
                    unsigned short px)
{
    const unsigned short* row = (const unsigned short*)(base + (size_t)y * pitch);
    int i;
    for (i = x0; i < x1; i++) if (row[i] != px) return 0;
    return 1;
}

static int Sb_ColIs(unsigned char* base, int pitch, int x0, int y0, int y1,
                    unsigned short px)
{
    int j;
    for (j = y0; j < y1; j++)
        if (((const unsigned short*)(base + (size_t)j * pitch))[x0] != px) return 0;
    return 1;
}

/* 截图前裁掉图外底色: 视口伸到地图外时 Sb_PaintVoidOn 会按"大面矩形 - 地图矩形"
 * 铺 [ScaleBar] Void, 那是补边不是画面 (MAX 档恒有一边留余量)。先按地图矩形裁,
 * 再削掉引擎地图矩形自带 -5/+135 余量留下的整行/整列底色, 各方向最多削一半。 */
static void Sb_CropVoid(void* s, RectangleStruct* r)
{
    int ml, mt, mw, mh, ox, oy, vw, vh;
    int x0, y0, x1, y1, limx, limy, pitch;
    unsigned char* base;
    unsigned short px;

    if (!SB_MapPx(&ml, &mt, &mw, &mh) || !Sb_ViewPx(&ox, &oy, &vw, &vh)) return;
    x0 = r->X; y0 = r->Y;
    x1 = x0 + r->Width; y1 = y0 + r->Height;
    /* 地图矩形在视口系里的位置。 */
    if (ml - ox > x0) x0 = ml - ox;
    if (mt - oy > y0) y0 = mt - oy;
    if (ml - ox + mw < x1) x1 = ml - ox + mw;
    if (mt - oy + mh < y1) y1 = mt - oy + mh;
    if (x1 - x0 < 64 || y1 - y0 < 64) return;   /* 地图读歪: 保留整幅 */

    pitch = GtSurfGet(s, GtVT_GET_PITCH);
    base = GtLock(s);
    if (base)
    {
        px = Sb_VoidPixel();
        limx = (x1 - x0) / 2;
        limy = (y1 - y0) / 2;
        if (limx < 64) limx = 64;
        if (limy < 64) limy = 64;
        while (y1 - y0 > limy && Sb_RowIs(base, pitch, x0, x1, y0, px)) y0++;
        while (y1 - y0 > limy && Sb_RowIs(base, pitch, x0, x1, y1 - 1, px)) y1--;
        while (x1 - x0 > limx && Sb_ColIs(base, pitch, x0, y0, y1, px)) x0++;
        while (x1 - x0 > limx && Sb_ColIs(base, pitch, x1 - 1, y0, y1, px)) x1--;
        GtUnlock(s);
    }
    r->X = x0; r->Y = y0;
    r->Width = x1 - x0; r->Height = y1 - y0;
}

/* 截图用：本帧未缩放的视口原图在哪 (见 ScaleBar.h)。 */
int SB_RawView(void** surf, int* x, int* y, int* w, int* h)
{
    RectangleStruct a, r;
    void* s;
    int bpp, sw, sh;
    FM_GetViewRects(&a, &r);
    if (r.Width < 16 || r.Height < 16) return 0;
    /* 缩小档: 原图在刚渲染完的大面上 (s_LastFinal, 与降采样同源), 整面 R。
     * 其它档: 原图就在窗口 Composite 里 —— 放大档取放大之前那块 R (R ⊆ A),
     * 1x 时 R == A。取这里而不是 Primary, 是为了拿"未缩放"的像素。 */
    if (SB_Out() && s_SurfActive && s_LastFinal && IsPlausiblePtr(s_LastFinal))
        s = s_LastFinal;
    else
        s = *(void**)ADDR_SURF_COMPOSITE;
    if (!IsPlausiblePtr(s)) return 0;
    bpp = GtSurfGet(s, GtVT_GET_BPP);
    sw = GtSurfGet(s, GtVT_GET_WIDTH);
    sh = GtSurfGet(s, GtVT_GET_HEIGHT);
    if (bpp != 2 || sw < 16 || sh < 16) return 0;
    Sb_ClipR(&r, sw, sh);          /* R 恒从 (0,0) 起, 只需夹右下 */
    if (r.Width < 16 || r.Height < 16) return 0;
    Sb_CropVoid(s, &r);
    *surf = s;
    *x = r.X;
    *y = r.Y;
    *w = r.Width;
    *h = r.Height;
    return 1;
}

/* 主画布判定 (窗口面 + 自建大面)。 */
int SB_IsBigSurface(void* s)
{
    if (!s || !IsPlausiblePtr(s)) return 0;
    if (s == s_BigComp || s == s_BigMap ||
        s == s_CurC || s == s_CurM) return 1;
    return 0;
}

/* 每帧由消息帧钩喂窗口 Composite 尺寸。 */
void SB_NoteWindowSurface(void* surf, int w, int h)
{
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w < 8 || h < 8) return;
    if (w != s_WinW || h != s_WinH)
    {
        s_WinW = w;
        s_WinH = h;
        Sb_DropMax();          /* 视图区跟着窗口变, MAX 的倍率要重算 */
    }
}

/* 只有 pitch 的场合用这个认主画布。认不出返回 0。 */
int SB_CanvasH(int w)
{
    int ww = s_WinW, hh = s_WinH;
    if (w <= 0) return 0;
    /* Composite 恒为窗口大小, 没喂过时问引擎。 */
    if (ww <= 0 || hh <= 0) { ww = GtGameW(); hh = GtGameH(); }
    if (ww > 0 && hh > 0 && w == ww) return hh;
    if (SB_Out() && s_SurfActive && w == s_BigCapW) return s_BigCapH;
    return 0;
}

/* 地图矩形/视口原点：直接读引擎全局。 */
int SB_MapPx(int* l, int* t, int* w, int* h)
{
    int bx, gx, gy, cw, ch;
    if (!l || !t || !w || !h) return 0;
    bx = *(volatile int*)(size_t)0x87F8DCu;
    gx = *(volatile int*)(size_t)0x87F8E4u;
    gy = *(volatile int*)(size_t)0x87F8E8u;
    cw = *(volatile int*)(size_t)0x87F8ECu;
    ch = *(volatile int*)(size_t)0x87F8F0u;
    if (cw <= 0 || ch <= 0 || cw > 8192 || ch > 8192) return 0;
    if (gx < -4096 || gx > 4096 || gy < -4096 || gy > 4096) return 0;
    *l = 30 * (2 * gx - bx);
    *t = 15 * (bx + 2 * gy - 5);
    *w = 60 * cw;
    *h = 30 * ch + 135;
    return 1;
}

/* 视口在大面坐标系里的原点与尺寸 (大面原点恒为 (0,0))。*/
static int Sb_ViewPx(int* ox, int* oy, int* w, int* h)
{
    void* tac;
    if (!ox || !oy || !w || !h) return 0;
    tac = *(void**)0x00887324u;                 /* TacticalClass 实例指针 */
    if (!tac || !IsPlausiblePtr(tac)) return 0;
    *ox = *(volatile int*)((unsigned char*)tac + 0xB0);
    *oy = *(volatile int*)((unsigned char*)tac + 0xB4);
    *w  = *(volatile int*)(size_t)0xB0CE30u;    /* ViewBounds.Width  */
    *h  = *(volatile int*)(size_t)0xB0CE34u;    /* ViewBounds.Height */
    return (*w > 16 && *h > 16 && *ox > -1000000 && *ox < 1000000
                          && *oy > -1000000 && *oy < 1000000);
}

/* 视口比地图大时居中钳位。退化轴钉地图正中。 */
int SB_ClampFix(Point2D* c)
{
    static int s_wasDeg = 2;
    int bl = 0;
    int ml, mt, mw, mh;
    int vw, vh;
    int mnX, mxX, mnY, mxY;
    int cx, cy, midX, midY;
    int degX, degY;
    if (!c || !IsPlausiblePtr(c)) return 0;
    if (!SB_MapPx(&ml, &mt, &mw, &mh)) return 0;
    vw = *(volatile int*)(size_t)0xB0CE30u;     /* ViewBounds.Width  */
    vh = *(volatile int*)(size_t)0xB0CE34u;     /* ViewBounds.Height */
    if (vw <= 16 || vh <= 16) return 0;
    /* 退化 = 地图比视口小。 */
    degX = (mw < vw);
    degY = (mh < vh);
    cx = ml + mw / 2;
    cy = mt + mh / 2;
    mnX = vw / 2 + ml;      mxX = mnX + mw - vw;
    mnY = vh / 2 + mt;      mxY = mnY + mh - vh;
    midX = cx;              midY = cy;
    /* 退化状态翻转时打一条 (周期心跳已删: 值不变时全是重复行)。 */
    {
        int deg = degX || degY;
        if (deg != s_wasDeg)
        {
            char buf[224];
            unsigned q = 0;
            s_wasDeg = deg;
            q = AppendCStr(buf, q, deg ? "sbar: view > map, center-pinned  x "
                                       : "sbar: view <= map, normal clamp  x ",
                            sizeof(buf));
            q = AppendInt(buf, q, sizeof(buf), mnX);
            q = AppendCh(buf, q, ',', sizeof(buf));
            q = AppendInt(buf, q, sizeof(buf), mxX);
            q = AppendCStr(buf, q, "  y ", sizeof(buf));
            q = AppendInt(buf, q, sizeof(buf), mnY);
            q = AppendCh(buf, q, ',', sizeof(buf));
            q = AppendInt(buf, q, sizeof(buf), mxY);
            LogLine(buf);
        }
    }
    /* Y 夹完继续夹 X (与原版同序)。 */
    if (degY) { if (c->Y != midY) { c->Y = midY; bl = 1; } }
    else if (c->Y < mnY) { c->Y = mnY; bl = 1; }
    else if (c->Y > mxY) { c->Y = mxY; bl = 1; }
    /* ---- X ---- */
    if (degX) { if (c->X != midX) { c->X = midX; bl = 1; } }
    else if (c->X < mnX) { c->X = mnX; return 1; }
    else if (c->X > mxX) { c->X = mxX; return 1; }
    return bl;
}

/* 图外底色每帧重铺 (增量拷贝的旧像素没人覆盖)。
 * 首行逐像素写、其余按行拷。 */
static void Sb_FillOpaque(unsigned char* base, int pitch, int W, int H,
                          int x0, int y0, int w, int h, unsigned short px)
{
    int yy, xx;
    unsigned short* row;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > W) w = W - x0;
    if (y0 + h > H) h = H - y0;
    if (w <= 0 || h <= 0) return;
    row = (unsigned short*)(base + (size_t)y0 * pitch) + x0;
    for (xx = 0; xx < w; xx++) row[xx] = px;
    for (yy = 1; yy < h; yy++)
        Sb_CopyPx((unsigned short*)(base + (size_t)(y0 + yy) * pitch) + x0, row, w);
}

/* 在指定的大面上铺图外底色。 */
static void Sb_PaintVoidOn(void* surf)
{
    int ml, mt, mw, mh;
    int ox, oy, vw, vh;
    int rx0, ry0, rx1, ry1;
    int bx0, by0, bx1, by1;
    int pitch, sw, sh;
    unsigned char* base;
    unsigned short px;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (!SB_MapPx(&ml, &mt, &mw, &mh)) return;
    if (!Sb_ViewPx(&ox, &oy, &vw, &vh)) return;
    rx0 = ml - ox;  rx1 = rx0 + mw;
    ry0 = mt - oy;  ry1 = ry0 + mh;
    /* 图外只可能出现在四个边; 地图完全盖住视口时四个矩形都空。*/
    bx0 = Sb_MinI(rx0, vw); if (bx0 < 0) bx0 = 0;      /* 左侧宽 */
    bx1 = Sb_MaxI(rx1, 0); if (bx1 > vw) bx1 = vw;    /* 右侧起点 */
    by0 = Sb_MinI(ry0, vh); if (by0 < 0) by0 = 0;
    by1 = Sb_MaxI(ry1, 0); if (by1 > vh) by1 = vh;
    if (bx0 >= bx1 && by0 >= by1) return;               /* 没有图外 */
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    sw = GtSurfGet(surf, GtVT_GET_WIDTH);
    sh = GtSurfGet(surf, GtVT_GET_HEIGHT);
    if (pitch <= 0 || sw < 8 || sh < 8 || pitch < sw * 2) return;
    /* 面与几何对不上就不铺。 */
    if (sw != vw || sh != vh)
    {
        static int s_warned = 0;
        if (!s_warned) { s_warned = 1; LogLine("sbar: WARN void skip, surface != ViewBounds"); }
        return;
    }
    px = Sb_VoidPixel();
    base = GtLock(surf);
    if (!base) return;
    /* 直接铺大面坐标：左右两条通高，上下只铺中间段。 */
    Sb_FillOpaque(base, pitch, sw, sh, 0, 0, bx0, vh, px);
    Sb_FillOpaque(base, pitch, sw, sh, bx1, 0, vw - bx1, vh, px);
    Sb_FillOpaque(base, pitch, sw, sh, bx0, 0, bx1 - bx0, by0, px);
    Sb_FillOpaque(base, pitch, sw, sh, bx0, by1, bx1 - bx0, vh - by1, px);
    GtUnlock(surf);
}

/* 图外底色：三趟地图后、降采样前铺一次本帧最终面。
 * 第三趟只画地图不碰图外，所以一次就够；另一张面轮到它显示的那帧自会铺。 */
void SB_PaintVoid(void)
{
    if (!s_Enable) return;
    if (!SB_Out() || !s_SurfActive) return;
    if (!s_SwapSeen) return;      /* 本帧没往大面画就不用铺 */
    if (!s_LastFinal || !IsPlausiblePtr(s_LastFinal)) return;
    MOE_SetPhase(MP_PAINTVOID);
    Sb_PaintVoidOn(s_LastFinal);
}

/* 物理鼠标点 -> 视口点。连续且钳到 R 内。出界由 SB_PtInRawView 判。 */
int SB_MapPoint(Point2D* p)
{
    RectangleStruct a, r;
    int vx, vy;
    int inA;
    if (!s_Enable || !p) return 1;
    if (!IsPlausiblePtr(p)) return 1;   /* 入参取错的话 p 会是野指针, 绝不能写 */
    if (!SB_Zoomed()) return 1;
    FM_GetViewRects(&a, &r);
    if (a.Width < 32 || r.Width < 16 || r.Height < 16) return 1;
    inA = !(p->X < a.X || p->Y < a.Y ||
            p->X >= a.X + a.Width || p->Y >= a.Y + a.Height);
    if (!inA)
    {
        /* A 之外：同公式外推再钳住。出界由 SB_PtInRawView 判。 */
        int ww = GtGameW(), wh = GtGameH();
        if (ww <= 0 || wh <= 0) return 1;
        if (p->X < 0 || p->Y < 0 || p->X >= ww || p->Y >= wh) return 1;
        vx = (int)((long)(p->X - a.X) * (long)r.Width / (long)a.Width);
        vy = (int)((long)(p->Y - a.Y) * (long)r.Height / (long)a.Height);
        if (vx < 0) vx = 0;
        if (vy < 0) vy = 0;
        if (vx > r.Width) vx = r.Width;
        if (vy > r.Height) vy = r.Height;
        if (vx > 0x7FFF) vx = 0x7FFF;
        if (vy > 0x7FFF) vy = 0x7FFF;
        p->X = vx;
        p->Y = vy;
        return 0;
    }
    vx = (int)((long)(p->X - a.X) * (long)r.Width / (long)a.Width);
    vy = (int)((long)(p->Y - a.Y) * (long)r.Height / (long)a.Height);
    if (vx < 0) vx = 0;
    if (vy < 0) vy = 0;
    if (vx > r.Width) vx = r.Width;
    if (vy > r.Height) vy = r.Height;
    p->X = vx;
    p->Y = vy;
    return 1;
}

/* 视口点是否落在 R 里。1x/放大恒 1。 */
int SB_PtInView(int vx, int vy)
{
    RectangleStruct a, r;
    if (!s_Enable) return 1;
    if (!SB_Out()) return 1;
    FM_GetViewRects(&a, &r);
    if (r.Width < 16 || r.Height < 16) return 1;
    if (vx < 0 || vy < 0) return 0;
    if (vx > r.Width || vy > r.Height) return 0;
    return 1;
}

/* 物理点是否落在视口 A 里。 */
int SB_PtInRawView(int x, int y)
{
    RectangleStruct a;
    FM_GetViewRects(&a, 0);
    if (a.Width < 32 || a.Height < 16) return 0;
    if (x < a.X || y < a.Y) return 0;
    if (x >= a.X + a.Width || y >= a.Y + a.Height) return 0;
    return 1;
}

/* 视图坐标 -> 屏幕坐标 (本项目按引擎投影画图元的层用) */
int SB_ViewToScreen(int vx, int vy, int* sx, int* sy)
{
    RectangleStruct a, r;
    if (!s_Enable || !SB_Zoomed()) return 0;
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
    if (!s_Enable || !dx || !dy || !SB_Zoomed()) return;
    FM_GetViewRects(&a, &r);
    if (a.Width < 32 || r.Width < 16 || r.Height < 16) return;
    *dx = (int)((long)(*dx) * (long)a.Width / (long)r.Width);
    *dy = (int)((long)(*dy) * (long)a.Height / (long)r.Height);
}

/* ---------------- 刻度条几何 ----------------
 * 控件 = 文字带 (FontPx+2) + 缝 1px + 滑条带 12px + 底留白 2px。
 * 滑条带内 (相对 rowY)：刻度线 2..8，黄色滑块 1..9，贯穿横条 9..11。 */
#define SB_BAND_H    32    /* 底部工具栏带高 (与 FullMap.cpp 的 winH-32 同一条) */
#define SB_SLIDER_H  12    /* 滑条带总高 */
#define SB_LINE_H    3     /* 贯穿横线高, 贴控件底 */
#define SB_TEXT_GAP  1     /* 文字带与滑条带之间的缝 */
#define SB_TICK_H    7     /* 刻度/± 那截 (比滑条带少 2px: 刻度线压低 2) */
#define SB_BOT_PAD   2     /* 横线底下再留 2px: 任何笔画都不许贴控件最后一行 */

/* 黄色滑块：高度不跟刻度线走，下移压住横线首行。 */
#define SB_KNOB_H    9
#define SB_KNOB_DY   1

/* 刻度线下移量 (坐在横线正上方)。 */
#define SB_TICK_DY   2

/* 贯穿横条左端外扩量 (右端不动)。 */
#define SB_LINE_X0   (-2)

/* + / - 画在刻度带里居中 (不骑横线)。 */
#define SB_SIGN_C    (SB_TICK_H / 2)   /* 带内中心那一行 */
#define SB_SIGN_BAR  3                 /* 横笔高 */
#define SB_SIGN_STEM SB_TICK_H         /* 竖笔高 (与刻度带同高, 占满) */
static int s_TextH;     /* 档位文字带高 (按 s_FontPx 估) */
static int s_WidgetH;   /* 文字带 + 缝 + 滑条带 */
static int s_HoverLev = -1;   /* 鼠标悬着的档位, -1 = 不在条上 (SB_Poll 每帧填) */
static int s_Placed = 0;     /* Sb_Place 跑过一遍 (暂停按钮锚点用) */

static int Sb_Gx0(void) { return s_BarX + 14; }
static int Sb_Gx1(void) { return s_BarX + s_BarW - 14; }
static int Sb_DetentX(int i, int a, int b, int n)
{
    return a + (b - a) * i / n;
}
static int Sb_LevelAt(int mx)
{
    int i, best = 0, bd = 0x7FFFFFFF, d;
    int a = Sb_Gx0(), b = Sb_Gx1(), n = s_NLev > 1 ? s_NLev - 1 : 1;
    for (i = 0; i < s_NLev; i++)
    {
        d = mx - Sb_DetentX(i, a, b, n);
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static int Sb_OverBar(int mx, int my)
{
    return mx >= s_BarX - 8 && mx < s_BarX + s_BarW + 8 &&
           my >= s_BarY - 4 && my < s_BarY + s_WidgetH + 4;
}

static void Sb_Place(void)
{
    int gw = GtGameW(), gh = GtGameH();
    int sbw = *(int*)(size_t)ADDR_SIDEBAR_W;
    if (sbw < 64 || sbw > 512) sbw = 168;
    s_TextH = s_FontPx + 2;
    s_BarH = SB_SLIDER_H;
    s_WidgetH = s_TextH + SB_TEXT_GAP + s_BarH + SB_BOT_PAD;
    s_BarW = s_Len;
    /* 右侧基准恒为侧栏列左缘，与 F11 显隐无关。 */
    s_BarX = gw - sbw - s_X - s_BarW;
    if (s_BarX < 0) s_BarX = 0;
    if (s_BarX + s_BarW > gw) s_BarW = gw - s_BarX;
    /* 默认盖底部工具栏带。s_Y 是下边距 (控件底边到窗口底边)，Y=-1 贴带顶。 */
    if (s_Y >= 0) s_BarY = gh - s_Y - s_WidgetH;
    else s_BarY = gh - SB_BAND_H;
    if (s_BarY + s_WidgetH > gh) s_BarY = gh - s_WidgetH;
    if (s_BarY < 0) s_BarY = 0;
    /* 几何变了才打一条。 */
    if (s_BarX != s_LastX || s_BarY != s_LastY || s_BarW != s_LastW)
    {
        char g1[16], buf[128];
        unsigned q = 0;
        s_LastX = s_BarX; s_LastY = s_BarY; s_LastW = s_BarW;
        q = AppendCStr(buf, q, "sbar: geom x=", sizeof(buf));
        UtoA((unsigned)s_BarX, g1); q = AppendCStr(buf, q, g1, sizeof(buf));
        q = AppendCStr(buf, q, " y=", sizeof(buf));
        UtoA((unsigned)s_BarY, g1); q = AppendCStr(buf, q, g1, sizeof(buf));
        q = AppendCStr(buf, q, " w=", sizeof(buf));
        UtoA((unsigned)s_BarW, g1); q = AppendCStr(buf, q, g1, sizeof(buf));
        q = AppendCStr(buf, q, " h=", sizeof(buf));
        UtoA((unsigned)s_WidgetH, g1); q = AppendCStr(buf, q, g1, sizeof(buf));
        q = AppendCStr(buf, q, " win=", sizeof(buf));
        UtoA((unsigned)gw, g1); q = AppendCStr(buf, q, g1, sizeof(buf));
        AppendCh(buf, q, 'x', sizeof(buf));
        UtoA((unsigned)gh, g1); q = AppendCStr(buf, q, g1, sizeof(buf));
        LogLine(buf);
    }
    s_Placed = 1;
}

static void Sb_SetLevel(int lv, const char* why)
{
    char b1[16], buf[160];
    unsigned p = 0;
    if (!s_Enable) return;
    if (lv < 0 || lv >= s_NLev || lv == s_Level) return;
    /* MAX 算不出 ⇒ 退到最高那一档，不静默变 1x。R 超限走钳制，不走这条。 */
    if (lv == SB_LEV_MAX)
    {
        Sb_DropMax();
        if (Sb_CalcMaxScale() <= 0.0)
        {
            LogLine("sbar: max level unsupported -> highest listed level");
            lv = s_NLev - 2;
        }
        else s_MaxOk = 1;
    }
    if (lv != s_Level) Sb_DropMax();
    s_Level = lv;
    Sb_SyncZoomed();
    /* R 随档变，快照作废并连催几帧重画。 */
    Sb_LevelChanged();
    p = AppendCStr(buf, p, "sbar: level=", sizeof(buf));
    UtoA((unsigned)s_Level, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " scale=", sizeof(buf));
    p += Sb_Fmt1(buf + p, SB_Scale());
    p = AppendCStr(buf, p, " by=", sizeof(buf));
    p = AppendCStr(buf, p, why, sizeof(buf));
    p = AppendCStr(buf, p, " drew=", sizeof(buf));
    UtoA((unsigned)s_Drew, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    /* zbuf = 引擎对象里的真实尺寸, zref = 我们记的; 一致只打 zbuf, 不一致才追 zref。 */
    {
        int zw = 0, zh = 0;
        char b2[16], b3[16];
        if (!SB_BufDims(&zw, &zh)) { zw = zh = -1; }
        p = AppendCStr(buf, p, " zbuf=", sizeof(buf));
        p = AppendInt(buf, p, sizeof(buf), zw);
        p = AppendCh(buf, p, 'x', sizeof(buf));
        p = AppendInt(buf, p, sizeof(buf), zh);
        if (zw != s_BufW || zh != s_BufH)
        {
            p = AppendCStr(buf, p, " zref=", sizeof(buf));
            UtoA((unsigned)s_BufW, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
            p = AppendCh(buf, p, 'x', sizeof(buf));
            UtoA((unsigned)s_BufH, b3); p = AppendCStr(buf, p, b3, sizeof(buf));
        }
    }
    LogLine(buf);
}

/* 引擎重建了渲染面 (读档 / 改分辨率) ⇒ 缩放退回 1x。
 * 换面会触发侧栏全量重画，那条路按 ViewBounds 重推锚点，
 * 须让 ViewBounds 先回到 A。须在 Fm_ApplyViewWidth 之前调。 */
void SB_ResetZoom1x(void)
{
    char b1[16], buf[128];
    unsigned p = 0;
    if (!s_Enable) return;
    if (s_Level == s_LevOne) return;
    p = AppendCStr(buf, p, "sbar: level=", sizeof(buf));
    UtoA((unsigned)s_Level, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " -> ", sizeof(buf));
    UtoA((unsigned)s_LevOne, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " (surfaces rebuilt: sidebar redraw must run on A)",
                   sizeof(buf));
    LogLine(buf);
    s_Level = s_LevOne;
    Sb_SyncZoomed();
    Sb_LevelChanged();
}

void SB_SetEnabled(int enable)
{
    SB_EnsureInit();
    enable = enable ? 1 : 0;
    if (enable == s_Enable) return;

    if (!enable)
    {
        if (s_Level != s_LevOne) SB_ResetZoom1x();
        if (s_SurfActive) SB_SurfaceDestroy();
    }

    s_Enable = enable;
    Sb_SyncZoomed();
}

/* 每循环: 摆矩形 + 点击换档。 */
void SB_Poll(void)
{
    int mx, my, down, lv0;
    if (!s_Enable) return;
    Sb_InitLabels();
    /* 大面 + 缓冲须在本帧 Render 之前准备好。 */
    SB_PrepareSurface();
    Sb_Place();
    if (!GtMouseGameXY(&mx, &my)) { s_LbDown = 0; s_HoverLev = -1; return; }
    lv0 = s_Level;
    s_HoverLev = Sb_OverBar(mx, my) ? Sb_LevelAt(mx) : -1;
    if (s_Level == SB_LEV_MAX)
    {
        Sb_DropMax();
        if (Sb_CalcMaxScale() <= 0.0)
            Sb_SetLevel(s_NLev - 2, "max-unsupported");
    }
    down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    if (down && !s_LbDown)
    {
        if (Sb_OverBar(mx, my)) Sb_SetLevel(Sb_LevelAt(mx), "click");
        s_LbDown = 1;
    }
    else if (!down) s_LbDown = 0;
    /* 改档当帧把视口和裁剪器跟上 (prepare 幂等)。 */
    if (s_Level != lv0)
    {
        FM_SyncViewNow();
        SB_PrepareSurface();
    }
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

/* 刻度条只画不擦：换档/悬停切档后旧像素会留在底栏带上。
 * 放大档须在切换时刻刷一次 chrome 脏缓存，让引擎重画底栏带盖掉旧像素。 */
static long s_BarSig = -1;

/* 条的全部像素 (无底板/边框)。ox/oy = 原点偏移。 */
static void Sb_PaintFills(unsigned char* base, int pitch, int W, int H, int ox, int oy)
{
    int bx = s_BarX + ox, by = s_BarY + oy;   /* by = 控件顶 = 文字带顶 */
    int rowY = by + s_TextH + SB_TEXT_GAP;    /* 滑条带顶 */
    int lineY = rowY + SB_SLIDER_H - SB_LINE_H;  /* 贯穿横线, 贴控件底 */
    int dx, i;
    int ga = Sb_Gx0(), gb = Sb_Gx1(), gn = s_NLev > 1 ? s_NLev - 1 : 1;
    int gx0 = Sb_Gx0() + ox, gx1 = Sb_Gx1() + ox;
    long sig;

    sig = s_Level * 2 + (s_HoverLev + 1);
    if (sig != s_BarSig)
    {
        s_BarSig = sig;
        *(unsigned*)(size_t)SB_CHROME_DIRTY_N = 0;
    }

    /* 刻度 + 旋钮坐在横线上方 */
    for (i = 0; i < s_NLev; i++)
    {
        dx = Sb_DetentX(i, ga, gb, gn) + ox;
        Sb_Fill(base, pitch, W, H, dx - 1, rowY + SB_TICK_DY, 2, SB_TICK_H,
                198, 206, 216, 255);
    }
    /* 贯穿横条 */
    Sb_Fill(base, pitch, W, H, gx0 + SB_LINE_X0, lineY,
            gx1 - gx0 - SB_LINE_X0 + 1, SB_LINE_H, 88, 92, 100, 255);
    /* 左 + 右 - */
    dx = bx + 6;
    Sb_Fill(base, pitch, W, H, dx - 4, rowY + SB_SIGN_C - 1, 9, SB_SIGN_BAR, 232, 236, 242, 255);
    Sb_Fill(base, pitch, W, H, dx - 1, rowY + SB_SIGN_C - SB_SIGN_STEM / 2, 3, SB_SIGN_STEM,
            232, 236, 242, 255);
    dx = bx + s_BarW - 7;
    Sb_Fill(base, pitch, W, H, dx - 4, rowY + SB_SIGN_C - 1, 9, SB_SIGN_BAR, 232, 236, 242, 255);
    /* 当前档的黄色滑块 */
    dx = Sb_DetentX(s_Level, ga, gb, gn) + ox;
    Sb_Fill(base, pitch, W, H, dx - 4, rowY + SB_KNOB_DY, 8, SB_KNOB_H, 12, 13, 16, 255);
    Sb_Fill(base, pitch, W, H, dx - 3, rowY + SB_KNOB_DY, 6, SB_KNOB_H, 250, 214, 80, 255);
}

static void Sb_PaintLabels(unsigned char* base, int pitch, int W, int H, int ox, int oy)
{
    int by = s_BarY + oy;
    int i, tw, n, th;
    int ga = Sb_Gx0(), gb = Sb_Gx1(), gn = s_NLev > 1 ? s_NLev - 1 : 1;
    wchar_t buf[10];
    /* 档位文字：只在文字带里。常显 4x / 1x / 0.5x，其余悬停才画。 */
    if (!GdiEnsure(s_FontPx, VecText_Bold(), 7, VecText_FontFace())) return;
    th = g_G->adv;
    if (th > s_TextH - 1) th = s_TextH - 1;
    for (i = 0; i < s_NLev; i++)
    {
        const char* lab = s_LevLabel[i];
        if (i != 0 && i != s_LevOne && i != s_NLev - 2 && i != s_HoverLev) continue;
        n = 0;
        while (lab[n] && n < 9) { buf[n] = (wchar_t)lab[n]; n++; }
        buf[n] = 0;
        if (!n) continue;
        tw = RenderLine(buf, n);
        if (tw <= 0) continue;
        /* 不透明覆盖: 幂等, 每帧照画不叠亮 (末位是透明度百分比, 100=不透明) */
        BlitCoverage(base, pitch, W, H, Sb_DetentX(i, ga, gb, gn) + ox - tw / 2, by,
                     tw + 2, th, 242, 246, 252, 100);
    }
}

static void Sb_Paint(unsigned char* base, int pitch, int W, int H, int ox, int oy)
{
    long long t = Perf_Now();
    Sb_PaintFills(base, pitch, W, H, ox, oy);
    Perf_Add(PF_SB2_FILL, t);
    t = Perf_Now();
    Sb_PaintLabels(base, pitch, W, H, ox, oy);
    Perf_Add(PF_SB2_LBL, t);
}

/* 锁面 + 调 Sb_Paint 的共用壳。bpp 是字节数 (16 位色 = 2)。 */
static int Sb_PaintSurface(void* surf)
{
    unsigned char* base;
    int pitch, w, h, bpp;
    long long t = Perf_Now();
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
    Perf_Add(PF_SB2_SETUP, t);
    Sb_Paint(base, pitch, w, h, 0, 0);
    GtUnlock(surf);
    return 1;
}

int SB_DrawToComposite(void)
{
    void* surf = *(void**)ADDR_SURF_COMPOSITE;
    /* 条画窗口面：画进大面会跟降采样落到画面中部。 */
    if (Sb_IsBigSurf(surf)) return 0;
    if (Sb_PaintSurface(surf)) { s_Drew = 1; return 1; }
    return 0;
}

/* 刻度条矩形 (暂停按钮锚在它左侧用)。 */
int SB_BarGeom(int* x, int* y, int* w, int* h)
{
    if (!s_Enable || !s_Placed) return 0;
    if (s_BarW < 32 || s_WidgetH < 16) return 0;
    if (x) *x = s_BarX;
    if (y) *y = s_BarY;
    if (w) *w = s_BarW;
    if (h) *h = s_WidgetH;
    return 1;
}