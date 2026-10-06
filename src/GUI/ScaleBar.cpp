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
 * 缩小 (scale>1): R 比窗口大, 引擎必须画进一张 R 大的离屏面 —— 本模块自建两张,
 *   只在 GScreenClass::Render (0x4F4480) 期间换进 [0x88731C]/[0x8872FC],
 *   三趟之后 (0x4F451B 钩) 立刻还原窗口面并把大面降采样铺回 A。
 *   ⇒ 引擎从头到尾只见过窗口尺寸的面, 窗口坐标的绘制永远落在它该在的地方。
 *
 * 鼠标: 不改引擎**实例里存**的鼠标坐标 (光标留在物理位置), 只改"引擎读到的
 *   那个副本": v = (p - A.xy) * R.size / A.size。两条数据源都要折 ——
 *   WWMouseClass 虚表 +0x34 (每帧轮询) 与窗口过程那条 lParam (按下/松开),
 *   见 MOExperience.cpp。1x 时折算恒等。
 *
 * ⚠ 凡是"按 ViewBounds 定位"的引擎代码都会跟着视口缩小的量挪位置 —— 除了地图光栅化,
 *   还有超武列表 (PowerBarClass::Draw 0x6DA9D0)、边缘卷屏 (Scroll_Map)、框选橡皮线
 *   (0x6D5030)、视口 gadget (0x4E1640)、悬停提示三处 (见下面 g_AView 段)。反过来,
 *   读 **WindowBounds** 的代码**不跟**缩放走 —— 1x 下两者相等, 缩放时才是分水岭, 也正是
 *   "底栏带"(y ∈ [A.Height, winH)) 在 R 里变成地图、却又被窗口坐标画到的原因。
 *   新增缩放功能时按这两条核一遍。
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
#define ADDR_SURF_MAP        0x008872FCu   /* 渲染时与 Composite 互换的持久地图面 */
#define ADDR_DSURF_CTOR      0x004BA5A0u   /* DSurface::DSurface(this,w,h,f1,f2) */
#define ADDR_OP_NEW          0x007C8E17u   /* 引擎的 operator new (cdecl, 1 参) */
#define ADDR_OP_DELETE       0x007C8B3Du   /* 引擎的 operator delete (cdecl, 1 参) */
/* ZBuffer/ABuffer 的构造/析构 (尺寸 = 0x30 字节, 布局 +0x28=宽 +0x2C=高)。
 * ★ 只借这两个函数**单独**重建缓冲, 不走 Set_View_Dimensions —— 那个正门除重建
 *   缓冲外还会 (a) 改写 ViewBounds 与 [0xB0CE2x] 缓存 (b) 重建 WWMouse 视口
 *   (c) **末尾调 InitGUI (0x6ABD30) 按传进去的矩形重排整条侧栏** (d) 按
 *   [0x886FA8] 重设 MessageList 区。缩小档传 R (> 窗口) 进去 ⇒ (c) 把 cameo/
 *   页签/雷达按 R 的几何摆 ⇒ 侧栏面 (168 宽) 里往 x=R.Width-30 处拉伸
 *   ⇒ 0x6BA643 的 `mov [ecx],dx` 越界写崩 (实测 phase=applyview, 崩在
 *   Set_View_Dimensions→InitGUI 链上)。(d) 同理把消息区摆到屏幕外。*/
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
#define ADDR_REG_DIRTY_SB    0x006D2790u   /* TacticalClass::RegisterDirtyArea */
#define ADDR_CUR_SURF_SB     0x00887314u   /* 引擎"当前绘制面" (Render 入口 = Composite) */
#define ADDR_HIDDEN_SB       0x0088730Cu   /* 地图增量面 (诊断用) */
#define ADDR_VIEWBOUNDS_SB   0x00886FA0u   /* ViewBounds */
#define ADDR_SYNC_VIEW_SB    0x006D5F60u   /* TacticalClass::SetTacticalViewBounds */
#define ADDR_CACHE_W_SB      0x00B0CE30u   /* view_bound 缓存宽 */
#define ADDR_CACHE_H_SB      0x00B0CE34u   /* view_bound 缓存高 */

#define SB_LABLEN  10

/* ---------------- 配置 ---------------- */
static int s_Init = 0;
static int s_Enable = 0;
static int s_Level = 2;
static int s_X = -1;                    /* <0 = 靠视图区右缘 */
static int s_Y = -1;                    /* <0 = 贴视图区下沿上方 */
static int s_Len = 240;
static int s_FontPx = 13;
static int s_GrowZBuf = 0;              /* [ScaleBar] GrowZBuf: 1=按 R 放大 ZBuffer/ABuffer */

/* ---------------- 档位表 ----------------
 * 6 档 (0 = 最左 = 放大最多):
 *   0.25 = 4 倍放大 / 0.5 = 2 倍 / 0.769 = 1.3 倍 / 1 = 原样
 *   1.4286 = 0.7 倍视野 / 2 = 半数视野 (缩小档, 需要 R 大的离屏面)
 * 缩小档的面建不出来时自动退回 A (等于 1x), 不会越界。*/
#define SB_MAXLEV 6
static const double s_LevAll[SB_MAXLEV] =
    { 0.25, 0.5, 1.0 / 1.3, 1.0, 1.0 / 0.7, 2.0 };
static const double* s_LevScale = s_LevAll;
static int    s_NLev = SB_MAXLEV;
static char s_LevLabel[SB_MAXLEV][SB_LABLEN];

/* 刻度条几何 */
static int s_BarX, s_BarY, s_BarW, s_BarH;
static int s_Safe = 0;      /* [Debug] Safe=1: 完全不接管 */
static int s_LbDown = 0;
static int s_BppWarned = 0;
static int s_LockWarned = 0;
static int s_Drew = 0;
static int s_LadderOk = 0;

/* ---------------- 缩小档的离屏大面 (scale>1) ----------------
 * 引擎只会 1:1 逐格画进传给 Render 的面; scale>1 时 R 比窗口大, 所以自建两张
 * R 大小的 DSurface (Composite/地图面), 只在 GScreenClass::Render 期间临时换进
 * 全局 0x88731C/0x8872FC (TacticalClass::Render 内部会把这两个互换, 尺寸必须相同),
 * 渲染完立刻还原, 再把大面的 R 区域降采样铺回真 Composite 的 A。 */
static void* s_BigComp = 0;             /* 两张 R 大的离屏面 (引擎双缓冲互换) */
static void* s_BigMap = 0;
static void* s_CurC = 0;                /* 下次 Render 入口要装的 (Composite 槽) */
static void* s_CurM = 0;                /* 下次 Render 入口要装的 (地图槽) */
static void* s_LastFinal = 0;           /* 本帧渲染完的最终画面所在面 (降采样源) */
/* ★ 大面的**物理**尺寸 (>= 逻辑 R)。侧栏模式(F11)与缩放档都会改 R; 若按 R 精确
 *   重建, 连按 F11 就是"每帧建两张新面 + 每帧清零", 而新建面在引擎重画之前是黑的
 *   ⇒ 连按侧栏键时整屏黑 (实测图2)。所以尺寸**只增不减**: 新 R 只要塞得下就复用,
 *   引擎只往 (0,0,R) 画、降采样也只读 (0,0,R), 多出来的边角永远没人碰。*/
static int   s_BigW = 0, s_BigH = 0;     /* 逻辑 R */
static int   s_BigCapW = 0, s_BigCapH = 0; /* 实际分配尺寸 */
static int   s_SurfActive = 0;
static int   s_SurfWarned = 0;
static int   s_SurfFail = 0;            /* 大面建不出来: 缩小档退回 A (不越界) */
static int   s_SwapSeen = 0;            /* 本帧 Render 入口真的换过大面 (否则不降采样) */
/* 大面已装上但**当前让出**给窗口面的那一小段 (见 SB_RenderSuspend/Resume)。
 * 两个让出窗口, 夹起来大面被暴露的时间只剩"三趟地图":
 *   (a) 0x4F4492..0x4F44CA —— `[0x887314]=eax` + WWMouse::vt+0x40(面,0) 光标
 *       擦除 + `[0xB0B518]/[0xB0B519]` 那套 + mode 记账 + call 0x53BAE0;
 *   (b) 0x4F44FD..0x4F4504 —— `call [edx+0x40]` = **chrome pass 0x6D0A20**
 *       (底栏矩形 [0xB0FC58] + 侧栏图标 + SidebarClass::Draw 里的
 *       ToolTipManager::Draw(1) 悬停提示盒), 全按**窗口坐标**画。
 * ⚠ (b) 是"画面中部底栏残影"与"悬停提示显示两次"的**唯一**来源, 之前只挡了 (a)。
 *   一律只见窗口面 ⇒ 不可能再把底栏/提示盒之类画进 R。*/
static int   s_Suspended = 0;
/* ZBuffer/ABuffer 当前尺寸 (按 R 重建过一次就记下) */
static int   s_BufW = 0, s_BufH = 0;
/* 新面催引擎重画 (帧号门控, 连续几帧推整块脏区) */
static int   s_RedrawFrames = 0;
static int   s_RedrawFrameNo = -1;
/* 一次性洗大面: 修好 s_WinComp 那一刻之前的帧可能已经把窗口坐标的东西烙进大面,
 * 引擎增量渲染不会再覆盖它们 ⇒ 必须自己抹一次黑并强制整块重画。*/
static int   s_PurgeFrames = 0;
static int   s_PurgeWarned = 0;
/* 渲染期临时换出的原全局 (只在 SB_RenderSwapIn/Out 之间有效) */
static void* s_RenderOldComp = 0;
static void* s_RenderOldMap = 0;
static int   s_RenderSwapped = 0;
/* 记住"真正的窗口面"。上一帧若换出去没换回来 (泄漏), SwapIn 会看到
 * [0x88731C] 已经是我们的面 —— 那时用它当还原目标, 否则泄漏会一直传染,
 * 后面所有按窗口坐标的绘制 (刻度条/UI 层) 全落进大面 ⇒ 降采样后残影。*/
static void* s_WinComp = 0;
static void* s_WinMap = 0;
static int   s_LeakWarned = 0;
/* 窗口面的逻辑尺寸 (SB_CanvasH 用)。缩小档下 [0x88731C] 绝大多数时间是 R 大的
 * 大面, 所以"这块面是不是主画布"不能问当前全局, 只能靠记住的窗口面尺寸。
 * 每帧由消息帧钩的 SB_NoteWindowSurface 喂 (见那里), SB_EnsureBig 里另有一份。*/
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
    s_GrowZBuf = GetPrivateProfileIntA(SB_SECTION, "GrowZBuf", 0, g_IniPath) ? 1 : 0;
    s_NLev = SB_MAXLEV;
    s_Level = GetPrivateProfileIntA(SB_SECTION, "Level", 3, g_IniPath);
    if (s_Level < 0 || s_Level >= s_NLev) s_Level = 3;
    if (!s_Enable) return;
    p = 0;
    p = AppendCStr(buf, p, "sbar: on level=", sizeof(buf));
    UtoA((unsigned)s_Level, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " nlev=", sizeof(buf));
    UtoA((unsigned)s_NLev, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " len=", sizeof(buf));
    UtoA((unsigned)s_Len, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " growzbuf=", sizeof(buf));
    UtoA((unsigned)s_GrowZBuf, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
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
double SB_Scale(void) { return SB_Zoomed() ? s_LevScale[s_Level] : 1.0; }
int SB_In(void)       { return (s_Enable && s_LevScale[s_Level] < 1.0) ? 1 : 0; }
int SB_Out(void)      { return (s_Enable && !s_Safe && s_LevScale[s_Level] > 1.0) ? 1 : 0; }
int SB_Zoomed(void)   { return (s_Enable && !s_Safe && s_LevScale[s_Level] != 1.0) ? 1 : 0; }

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

/* 缩小档大面 (定义在文件后部)。返回 1 = 可用; 0 = 建不出来, 调用方必须退回 A。*/
static int SB_EnsureBig(int w, int h);
static void SB_SurfaceDestroy(void);

/* 引擎实际要画的视口矩形 = 视图区 * scale。
 * ★ ViewBounds 恒为 (0,0,w,h): 引擎的渲染落点是 ViewBounds.xy + (mapPos -
 *   TacticalPos), 而它自己的两条查格路 (CoordsToCell / ClientToCoords) 只有在
 *   X/Y = 0 时才自洽, 非 0 会让下单击与 hover 差 2·X。
 * ★ 缩小档 (scale>1) 必须先确保大离屏面建好, 否则引擎会按 R 往窗口面里画 =
 *   越界。建不出来就返回 0, FullMap 于是用 A (行为等于 1x)。*/
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
    if (s > 1.0 && !SB_EnsureBig(w, h)) return 0;   /* 大面缺失: 退回 A */
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

/* ---------------- 缩小档 (scale>1): 自建 R 大小的离屏大面 ----------------
 * 引擎只会把视口 1:1 画进传给 Render 的面。A 是窗口大小, R=A*scale 比窗口大,
 * 所以另建两张 R 大的 DSurface, 只在 GScreenClass::Render (0x4F4480) 期间把它们
 * 换进全局:
 *   [0x88731C] Composite  <- 大面 (GScreenClass::Render 三趟都从它取面)
 *   [0x8872FC] 地图面      <- 大面 (TacticalClass::Render 内部会把这两个互换,
 *                                   尺寸必须一致, 否则 CopyFromPart 跨面越界)
 * 渲染结束 (消息帧钩) 立刻还原成窗口面, 再把大面的 R 区域降采样铺回真 Composite。
 * ZBuffer/ABuffer 是每像素 2 字节、按视口尺寸分配并被 Render 整块清零/写, R 大于
 * 窗口时必须经 DisplayClass::Set_View_Dimensions (0x4A8960) 按 R 重建, 否则越界。
 *
 * ⚠ 大面是我们自己的, 引擎的 CreateAll 只会释放全局里的面 (正常时是窗口面),
 *   所以能安全自管生命周期; 若发现全局被外部换过就只放弃不释放, 避免二次释放。*/
typedef void* (__cdecl    *SbAllocFn)(unsigned);
typedef void* (__thiscall *SbDSurfCtorFn)(void*, int, int, int, int);
typedef void  (__thiscall *SbDSurfDtorFn)(void*, int);
typedef void  (__fastcall *SbRegDirtyFn)(void*, RectangleStruct, int);

static void* Sb_NewSurf(int w, int h)
{
    SbAllocFn al = (SbAllocFn)(size_t)ADDR_OP_NEW;
    void* o;
    if (!al || w < 8 || h < 8) return 0;
    o = al(0x24);                       /* DSurface 对象大小 (CreateAll 实测) */
    if (!o || !IsPlausiblePtr(o)) return 0;
    ((SbDSurfCtorFn)(size_t)ADDR_DSURF_CTOR)(o, w, h, 1, 1);
    /* 建面失败时虚表可能仍可读但尺寸不对, 用它当判据; 失败就销毁别用。*/
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
    ((SbDSurfDtorFn)vt[0])(s, 1);       /* vtable[0](this, 1) = 引擎释放面对象 */
}

/* 整面填零 (无 CRT: 自己写循环)。宽高/pitch 先按**面自己的**尺寸夹一遍。
 * 只用于刚建好大面时: DSurface 的像素缓冲是 operator new 出来的, 内容
 * 未定义; 地图光栅化只覆盖它认为该覆盖的格子, 图外/半格区会把堆里
 * 上一块内存的残留直接当成画面。清成黑 = 引擎自己的图外底色, 视觉上
 * 也对。
 * ⚠ 不要想着"抹掉底栏带"来藏残影 (已试过, 图1): R 里那条带是**地图**,
 * 地图是增量画的, 抹了不一定回来 ⇒ 留一条黑带。正解是别让窗口坐标
 * 的绘制碰到大面 (SB_RenderSuspend/Resume)。 */
static void Sb_ClearSurf(void* s)
{
    unsigned char* b;
    int pitch, w, h, x, y;
    if (!s || !IsPlausiblePtr(s)) return;
    pitch = GtSurfGet(s, GtVT_GET_PITCH);
    w = GtSurfGet(s, GtVT_GET_WIDTH);
    h = GtSurfGet(s, GtVT_GET_HEIGHT);
    if (pitch <= 0 || w < 8 || h < 8) return;
    if (pitch < w * 2) return;          /* 不是 16bpp 的面: 逐行会越过行尾 */
    b = GtLock(s);
    if (!b) return;
    for (y = 0; y < h; y++)
    {
        unsigned short* row = (unsigned short*)(b + (size_t)y * pitch);
        for (x = 0; x < w; x++) row[x] = 0;
    }
    GtUnlock(s);
}

/* 是不是我们自己造的大面 (换面/绘制前的把关用)。*/
static int Sb_IsBigSurf(void* s)
{
    if (!s || !IsPlausiblePtr(s)) return 0;
    return (s == s_BigComp || s == s_BigMap ||
            s == s_CurC || s == s_CurM || s == s_LastFinal);
}

void SB_RenderSwapIn(void)
{
    void* c;
    if (!SB_Out() || !s_SurfActive) return;
    if (s_RenderSwapped) return;
    c = *(void**)ADDR_SURF_COMPOSITE;
    s_RenderOldComp = c;
    s_RenderOldMap  = *(void**)ADDR_SURF_MAP;
    /* ★ 换面泄漏自愈: 入口看到 [0x88731C] 已经是我们的面 ⇒ 上一帧没还原。
     *   此时若拿它当"还原目标"，泄漏会永远传下去 (之后按窗口坐标画的刻度条/
     *   UI 层全落进大面 ⇒ 降采样后就在地图中间闪出底栏残影)。改用记住的窗口面。 */
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
    if (s_RenderOldComp == s_CurC) return;      /* 已经装好 (泄漏自愈后重复) */
    *(void**)ADDR_SURF_COMPOSITE = s_CurC;
    *(void**)ADDR_SURF_MAP       = s_CurM;
}

/* 让出: 把三个全局换回窗口面, 但**不动** s_CurC/s_CurM/s_LastFinal/s_RenderSwapped
 *   (那些是"本帧结束时要记下来的分配", 在让出期间还没变)。
 *
 *   挂两个点, 都是 Render 里"按**窗口坐标**画"的那一串:
 *     a) 入口 `mov [0x887314], eax` (0x4F4492) —— 之后是 WWMouse::vt+0x40(面,0)
 *        (光标擦除)、`[0xB0B518]/[0xB0B519]` 那套、mode 记账 + `call 0x53BAE0`。
 *     b) **chrome pass 调用点 (0x4F44FD)** —— `call [esi+0x40]` (虚表 +0x40 =
 *        0x6D0A20, "底栏/侧栏 chrome" 那一趟)。它按 `[0xB0FC58]` 那个底栏矩形
 *        (1x = (0, H-32, W-168, 32)) 往 `[0x887314]` blit, 再调 SidebarClass::Draw
 *        (0x6A6C30, 内含 ToolTipManager::Draw(1))。**这是画面中部那条底栏残影与
 *        缩小的悬停提示的唯一来源** —— 之前只挡了 a), b) 漏了, 底栏与提示盒就按
 *        窗口坐标烙进 R, 降采样后落在 y = H*A.H/R.H ≈ 733 (实测图1/图2)。
 *        0x4F4505 (`mov ecx,[0x88731C]`, 第三趟地图之前) 再装回大面。
 *
 *   ⇒ **大面被暴露的窗口只剩 "三趟地图" 这一段**, 窗口坐标的绘制在结构上不可能
 *     再碰到它。不用再去"抹掉底栏带"——那块在 R 里是**地图**, 抹了会留黑带
 *     (实测图1), 而且引擎的地图是**增量**画的, 不保证每帧重覆。*/
void SB_RenderSuspend(int forCursor)
{
    if (!s_RenderSwapped || s_Suspended) return;
    if (!s_RenderOldComp || !IsPlausiblePtr(s_RenderOldComp)) return;
    *(void**)ADDR_SURF_COMPOSITE = s_RenderOldComp;
    *(void**)ADDR_SURF_MAP       = s_RenderOldMap;
    *(void**)ADDR_CUR_SURF_SB    = s_RenderOldComp;
    s_Suspended = 1;
    /* 0x4F44A2 那笔光标擦除**直接吃 eax** (不是读全局), 所以 (a) 还要让
     * MOExperience_PreCurEntry 把入口 eax 一并换成窗口面。chrome pass 那条
     * (b) 走虚表、实参只有一个 flags, 不吃渲染面 ⇒ 不碰 g_PreCur*。*/
    if (forCursor)
    {
        g_PreCurWin = 1;
        g_PreCurSurf = s_RenderOldComp;
    }
}

/* 三趟地图之前 (0x4F44CB 的 `mov eax,[0x88731C]`) 与 chrome pass 之后
 *   (0x4F4505 的 `mov ecx,[0x88731C]`) 各装回一次大面。那之后的代码自己
 *   会重读全局, 所以不用改任何寄存器。*/
void SB_RenderResume(void)
{
    if (!s_Suspended) return;
    s_Suspended = 0;
    if (!s_RenderSwapped) return;
    *(void**)ADDR_SURF_COMPOSITE = s_CurC;
    *(void**)ADDR_SURF_MAP       = s_CurM;
}

void SB_RenderSwapOut(void)
{
    if (!s_RenderSwapped) return;
    s_RenderSwapped = 0;
    /* ★ 记录渲染后的分配, 下次入口原样装回去 —— 引擎的双缓冲靠 TacticalClass::
     * Render (0x6D4063, 仅 eMode 0/3) 把 [0x88731C]/[0x8872FC]**永久互换**: 下一帧
     * 它就在"换过之后"的分配上继续"净面覆盖脏面"。我们每帧在入口按固定顺序重装会
     * 打断这个轮换 ⇒ 把带单位的那张当净面复制 ⇒ 残影/拖影。所以必须把本帧结束时的
     * 分配记下来下帧原样装回。最终画面在结束时的 [0x88731C], 留给降采样。
     * ⚠ 让出期间 ([0x88731C] 是窗口面, 三趟地图压根没跑) **不能**记 —— 否则会把
     *   窗口面记成"下帧要装的大面", 下一帧整个渲染落到窗口面里 = 越界。*/
    if (!s_Suspended)
    {
        s_CurC = *(void**)ADDR_SURF_COMPOSITE;
        s_CurM = *(void**)ADDR_SURF_MAP;
        s_LastFinal = s_CurC;
    }
    else s_SwapSeen = 0;                 /* 本帧没画 ⇒ 也不能拿上一帧的画面降采样 */
    s_Suspended = 0;
    g_PreCurWin = 0;
    g_PreCurSurf = 0;
    /* ⚠ [0x887314] ("当前绘制面") 是 UI 阶段唯一认的面: 侧栏 0x4F4547、chrome pass
     *   0x6D0A20 (底栏 blit 进它)、光标 0x4F4593 全都读它。**绝不能**留成大面 ——
     *   底栏会按窗口坐标画进 R 大的面, 再被降采样搬到画面中部 (实测 y=524)。
     *   s_RenderOldComp 万一还是大面 (自愈没抓到窗口面) 时, 用 s_WinComp 兜底。*/
    if (Sb_IsBigSurf(s_RenderOldComp) && s_WinComp && IsPlausiblePtr(s_WinComp))
        s_RenderOldComp = s_WinComp;
    if (Sb_IsBigSurf(s_RenderOldMap) && s_WinMap && IsPlausiblePtr(s_WinMap))
        s_RenderOldMap = s_WinMap;
    if (Sb_IsBigSurf(s_RenderOldComp) && !s_LeakWarned)
    {
        s_LeakWarned = 1;
        LogLine("sbar: WARN window composite unknown, chrome may ghost");
    }
    /* 无条件还原窗口面 (不按"是否等于我们的面"判, 互换后判据会漏)。
     * ★ 还要把"当前绘制面" [0x887314] 一起还原: Render 入口 (0x4F4492) 与
     *   TacticalClass::Render (0x6D4076) 都把它写成那个(被换过的)Composite 槽,
     *   所以地图画完后它还指着大面。引擎紧随其后的 UI (侧栏/cameo, 0x4F4547 那条)
     *   是往 [0x887314] 画的 —— 不还原就把 UI 画进 R 大的大面, 坐标按窗口算 ⇒
     *   cameo 越界写坏堆 (实测: 切侧栏模式时 0x6BA643 的 cameo 透明拉伸写飞)。*/
    *(void**)ADDR_SURF_COMPOSITE = s_RenderOldComp;
    *(void**)ADDR_SURF_MAP = s_RenderOldMap;
    *(void**)ADDR_CUR_SURF_SB = s_RenderOldComp;
}

/* 三趟地图画完 (0x4F451B) 立刻调: 还原窗口面 + 把大面 R 区域降采样铺回窗口
 * Composite 的 A。**必须早于引擎的 UI** (侧栏 0x4F4547 / 消息框 0x4F455D), 否则
 * 引擎 UI 会被画进大面、再被我们一起降采样 ⇒ 底栏残影闪在画面中部、cameo 越界。
 * 1x/放大档恒空操作 (没换过大面)。*/
/* ---------------- 诊断: 引擎底栏的矩形与脏缓存 ----------------
 * 已验证的引擎事实 (gamemd 反汇编, 别再猜):
 *   GScreenClass 虚表 +0x44 = 0x6D0A20, 由 Render 的 0x4F459A 调用, 它是
 *   "底栏/侧栏 chrome" 的绘制 pass:
 *     - 0x6D0BAC  BlitImage([0x887300], [0xB0FB08], ...)      侧栏面
 *     - 0x6D0C5F  BlitImage([0x887314], [0xB0FA3C], ...)      ★当前绘制面
 *     - 0x6D0CC4  BlitImage([0x887314], [0xB0FA90], ...)      ★当前绘制面
 *     - 0x6D0DC8  BlitImage([0x887314], [0xB0FABC], ...)      ★当前绘制面
 *     - 0x6D0E38  SidebarClass::Draw
 *   0x6D0BD1-0x6D0C22: 这三笔的目标矩形 **直接取自 [0xB0FC58] 的 X/Y**,
 *   再和目标面的 vt+0x78 自矩形求交 (0x421B60)。
 *   [0xB0FC58] 是 0x72FEB8 在初始化时 new 出来的 RectangleStruct:
 *     X=0, Y=(底边-0x20), Width=[0xA8EB84]-0xA8(168 侧栏), Height=0x20
 *   ⇒ 它就是**底栏矩形**, 1x 时应为 (0, H-32, W-168, 32)。
 *   脏缓存: [0xB0CC00..(n)] 存"已经画过 chrome 的面指针", n=[0xB0CD44];
 *   0x6D0B3C-0x6D0B57 若 [0x887314] 在表里 ⇒ bl=0 ⇒ **整段 chrome 跳过**。
 *   1x 下成立是因为地图每帧只覆写 ViewBounds=A(0,0,*,H-32), 底栏带在 A 之外,
 *   像素跨帧保留。缩小档我们每帧覆写 A, 且 [0x887314]=Composite ⇒ 必须核对
 *   [0xB0FC58] 的 Y 是否仍是 H-32。*/
/* ⚠ 踩过的坑 (实测崩在这里): 别写"收 p 回 q"的日志小助手。AppendCStr 的越界写在
 *   `buf[pos]=0` 那一行 (objdump: `movb $0x0,(%esi,%eax,1)`, AppendCStr+0x46) ——
 *   pos 一旦 >= size-1, 拷贝循环整个不执行, 但收尾那行照样写 ⇒ 当场踩栈。
 *   所以诊断一律平铺写, 只用 224 字节的 buf。*/
#define SB_CHROME_BAR     0xB0FC58u  /* 指向底栏 RectangleStruct 的指针 */
#define SB_CHROME_DIRTY_N 0xB0CD44u
#define SB_CHROME_DIRTY_0 0xB0CC00u
#define SB_SCREENW        0xA8EB84u  /* 底栏宽度的来源: [0xA8EB84] - 168 */

void SB_RenderAfterMap(void)
{
    if (!s_RenderSwapped) return;
    /* 诊断 (缩小档下每 ~2 秒一次): 各渲染面的指针/尺寸, 用来定位"底栏残影"到底
     * 被画进了哪张面。可随时删。*/
    {
        static int s_diag = 0;
        if (++s_diag >= 120)
        {
            void* c = *(void**)ADDR_SURF_COMPOSITE;
            void* u = *(void**)ADDR_CUR_SURF_SB;
            void* h = *(void**)ADDR_HIDDEN_SB;
            void* sb2 = *(void**)ADDR_WWMOUSE_SB;
            char b1[16], b2[16], buf[224];
            unsigned p = 0;
            s_diag = 0;
            p = AppendCStr(buf, p, "sbar: diag comp=", sizeof(buf));
            UtoA((unsigned)(size_t)c, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            if (c && IsPlausiblePtr(c))
            { int w = GtSurfGet(c, GtVT_GET_WIDTH), hh = GtSurfGet(c, GtVT_GET_HEIGHT);
              p = AppendCh(buf, p, '(', sizeof(buf));
              UtoA((unsigned)w, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
              p = AppendCh(buf, p, 'x', sizeof(buf));
              UtoA((unsigned)hh, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
              p = AppendCh(buf, p, ')', sizeof(buf)); }
            p = AppendCStr(buf, p, " cur=", sizeof(buf));
            UtoA((unsigned)(size_t)u, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCStr(buf, p, " hidden=", sizeof(buf));
            UtoA((unsigned)(size_t)h, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            if (h && IsPlausiblePtr(h))
            { int w = GtSurfGet(h, GtVT_GET_WIDTH), hh = GtSurfGet(h, GtVT_GET_HEIGHT);
              p = AppendCh(buf, p, '(', sizeof(buf));
              UtoA((unsigned)w, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
              p = AppendCh(buf, p, 'x', sizeof(buf));
              UtoA((unsigned)hh, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
              p = AppendCh(buf, p, ')', sizeof(buf)); }
            p = AppendCStr(buf, p, " final=", sizeof(buf));
            UtoA((unsigned)(size_t)s_LastFinal, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCStr(buf, p, " bigC=", sizeof(buf));
            UtoA((unsigned)(size_t)s_BigComp, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCStr(buf, p, " win=", sizeof(buf));
            UtoA((unsigned)(GtGameW()), b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCh(buf, p, 'x', sizeof(buf));
            UtoA((unsigned)(GtGameH()), b2); p = AppendCStr(buf, p, b2, sizeof(buf));
            (void)sb2;
            LogLine(buf);
            /* 底栏 chrome 的诊断: 引擎底栏矩形 + 脏缓存。
             * 1x 下 bar 应当是 (0, H-32, W-168, 32)。若缩小档下这个 Y
             * 变了 (或不等于 H-32), 底栏就被画进了 A 里。
             * 平铺写, 只用 buf[224]。*/
            {
                unsigned bp = 0;
                unsigned bar = *(const unsigned*)(size_t)SB_CHROME_BAR;
                RectangleStruct a2, r2;
                FM_GetViewRects(&a2, &r2);
                bp = AppendCStr(buf, bp, "sbar: bar ptr=", sizeof(buf));
                UtoA(bar, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCStr(buf, bp, " r=", sizeof(buf));
                if (IsPlausiblePtr((void*)(size_t)bar))
                {
                    UtoA(((const int*)(size_t)bar)[0], b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                    bp = AppendCh(buf, bp, ',', sizeof(buf));
                    UtoA(((const int*)(size_t)bar)[1], b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                    bp = AppendCh(buf, bp, ',', sizeof(buf));
                    UtoA(((const int*)(size_t)bar)[2], b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                    bp = AppendCh(buf, bp, ',', sizeof(buf));
                    UtoA(((const int*)(size_t)bar)[3], b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                }
                else bp = AppendCStr(buf, bp, "?", sizeof(buf));
                bp = AppendCStr(buf, bp, " A=", sizeof(buf));
                UtoA((unsigned)a2.X, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCh(buf, bp, ',', sizeof(buf));
                UtoA((unsigned)a2.Y, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCh(buf, bp, ',', sizeof(buf));
                UtoA((unsigned)a2.Width, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCh(buf, bp, ',', sizeof(buf));
                UtoA((unsigned)a2.Height, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCStr(buf, bp, " R=", sizeof(buf));
                UtoA((unsigned)r2.Width, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCh(buf, bp, ',', sizeof(buf));
                UtoA((unsigned)r2.Height, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCStr(buf, bp, " scrw=", sizeof(buf));
                UtoA(*(const unsigned*)(size_t)SB_SCREENW, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCStr(buf, bp, " n=", sizeof(buf));
                UtoA(*(const unsigned*)(size_t)SB_CHROME_DIRTY_N, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCStr(buf, bp, " d0=", sizeof(buf));
                UtoA(*(const unsigned*)(size_t)SB_CHROME_DIRTY_0, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                bp = AppendCStr(buf, bp, " cur=", sizeof(buf));
                UtoA((unsigned)(size_t)u, b1); bp = AppendCStr(buf, bp, b1, sizeof(buf));
                LogLine(buf);
            }
        }
    }
    SB_RenderSwapOut();
    if (SB_Out() && s_SurfActive) SB_CompositeDemagnify();
    /* 诊断: 还原之后 [0x887314] 必须是**窗口面**。chrome pass (0x4F459A) 紧接着
     * 就读它, 留成大面 ⇒ 底栏 blit 进大面 ⇒ 降采样后落在 y=1048*1048/2096=524。*/
    {
        static int s_post = 0;
        if (++s_post >= 120)
        {
            void* d = *(void**)ADDR_CUR_SURF_SB;
            char b1[16], buf[128];
            unsigned p = 0;
            s_post = 0;
            p = AppendCStr(buf, p, "sbar: post cur=", sizeof(buf));
            UtoA((unsigned)(size_t)d, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            if (d && IsPlausiblePtr(d))
            { p = AppendCh(buf, p, '(', sizeof(buf));
              UtoA((unsigned)GtSurfGet(d, GtVT_GET_WIDTH), b1);
              p = AppendCStr(buf, p, b1, sizeof(buf));
              p = AppendCh(buf, p, 'x', sizeof(buf));
              UtoA((unsigned)GtSurfGet(d, GtVT_GET_HEIGHT), b1);
              p = AppendCStr(buf, p, b1, sizeof(buf));
              p = AppendCh(buf, p, ')', sizeof(buf)); }
            p = AppendCStr(buf, p, " winC=", sizeof(buf));
            UtoA((unsigned)(size_t)s_WinComp, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCStr(buf, p, " isBig=", sizeof(buf));
            UtoA((unsigned)Sb_IsBigSurf(d), b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            LogLine(buf);
        }
    }
}

/* ---------------- 边缘卷屏: 把引擎拿到的点按**屏幕**空间给回 ----------------
 * DisplayClass 的边缘自动卷屏是 `0x00692B60` (每帧由 `0x00692F30` 驱动, 唯一
 * 调用点 0x69304E)。它把传入点当成"视口坐标" (`pt + ViewBounds.xy`), 再拿结果去
 * 和**面的宽高** (vt+0x7C/0x80) 比, 算死区/方向 —— 这是**屏幕**空间的判据。
 *
 * 缩放后 WWMouse 虚表 +0x34 返回的是**视口**点 (物理 * R/A), 直接喂进去会让
 * 死区跟着缩放: 缩小档下 (R>A) 光标还没到屏幕边就出死区 ⇒ "鼠标在画面中部就能
 * 卷动", 而左右/上下仍按物理边判 ⇒ "判定还在左上角"。
 *
 * 修法: 在 0x692B60 入口把那个点换回**物理**鼠标 (实例 +0x1C/+0x20)。1x 时本来就
 * 是物理点 ⇒ 恒等; 只在缩放档 (SB_Zoomed) 动手。0x692B60 只有这一个调用点, 且它
 * 是整条驱动里**最后**一个用这个点的地方 (ProcessClickCoords 在那之前已用完),
 * 所以不会影响 hover/点击。只读 + 改本地临时点, 联机安全。*/
#define SB_WWMOUSE_OFF_X  0x1C
#define SB_WWMOUSE_OFF_Y  0x20
void SB_EdgeScrollPointFix(Point2D* pt)
{
    void* m;
    if (!pt || !IsPlausiblePtr(pt)) return;
    if (!s_Enable || s_Safe || !SB_Zoomed()) return;
    m = *(void**)ADDR_WWMOUSE_SB;
    if (!m || !IsPlausiblePtr(m)) return;
    pt->X = *(int*)((char*)m + SB_WWMOUSE_OFF_X);
    pt->Y = *(int*)((char*)m + SB_WWMOUSE_OFF_Y);
}

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
    Sb_FreeSurf(s_BigComp);
    Sb_FreeSurf(s_BigMap);
    s_BigComp = s_BigMap = 0;
    s_CurC = s_CurM = s_LastFinal = 0;
    s_BigW = s_BigH = 0;
    s_BigCapW = s_BigCapH = 0;
    s_Suspended = 0;
    s_SurfActive = 0;
}

/* 建/复用 R 大的离屏大面。返回 1 = 可用; 0 = 失败 (置 s_SurfFail, 调用方退回 A)。*/
static int SB_EnsureBig(int w, int h)
{
    if (s_SurfFail) return 0;
    if (w < 320 || h < 240) return 0;
    /* Render 入口钩没装上就不能做缩小档: 引擎不会画进大面, 只会按 R 往窗口面里
     * 画 ⇒ 越界。返回 0 让 FullMap 用 A (行为等于 1x)。*/
    if (!g_RenderTramp)
    {
        if (!s_SurfWarned) { s_SurfWarned = 1;
            LogLine("sbar: zoom-out off (render hook missing)"); }
        return 0;
    }
    /* ★★ 记住"窗口面" —— 这一条是底栏残影的根因, 别删。
     *   引擎每帧**永久互换** [0x88731C]/[0x8872FC] (TacticalClass::Render
     *   0x6D4063/0x6D4076, 仅 eMode 0/3), 所以本帧 SwapOut 还原成窗口面之后,
     *   下一帧 Render 入口 SwapIn 读到的 [0x88731C] **必然已经是上一帧留下的大面**
     *   => 走"泄漏自愈"分支 => `if (s_WinComp) s_RenderOldComp = s_WinComp;`。
     *   而 s_WinComp 原本只在"没走自愈分支"的 else 里赋值 —— 首帧就已经是自愈分支,
     *   于是它**恒为 0**, 自愈成了空操作, 大面被当成窗口面一路带进 UI 阶段:
     *   `SB_RenderSwapOut` 把 [0x887314] 还原成大面 ⇒ chrome pass (0x6D0A20,
     *   0x4F459A) 把底栏 blit 进 (0,1048,1752,32) 的大面 ⇒ 降采样后落在
     *   y = 1048 * 1048/2096 = **524**、宽 876 —— 正是画面中部那条底栏残影。
     *   同一原因下 [0xB0CC00] 也被记成大面 => chrome 脏缓存恒命中 => 真正的
     *   底栏带永远不重画 => 鼠标划过去时 WWMouse 还原旧背景 => 底部重影。
     *   这里在**还没有装过大面**的时机把窗口面抓下来。*/
    {
        void* wc = *(void**)ADDR_SURF_COMPOSITE;
        void* wm = *(void**)ADDR_SURF_MAP;
        if (wc && IsPlausiblePtr(wc) && !Sb_IsBigSurf(wc))
        {
            if (s_WinComp != wc)
            {
                if (!s_WinComp) s_PurgeFrames = 8;   /* 首次抓到 ⇒ 之前可能烙过印记 */
                s_WinComp = wc;
                s_WinMap = wm;
            }
            /* 记住窗口面的尺寸: 缩小档下 [0x88731C] 绝大多数时间是 R 大的大面,
             * "这块面是不是主画布"只能靠这个判断 (SB_CanvasH)。*/
            s_WinW = GtSurfGet(s_WinComp, GtVT_GET_WIDTH);
            s_WinH = GtSurfGet(s_WinComp, GtVT_GET_HEIGHT);
        }
    }
    /* ★ 尺寸只增不减: 侧栏模式(F11)与缩放档都会改 R; 若按 R 精确重建, 连按 F11 就是
     *   "每帧建两张新面 + 每帧清零", 而新建面在引擎重画之前是黑的
     *   ⇒ 连按侧栏键时整屏黑 (实测图2)。新 R 只要塞得下就复用:
     *   引擎只往 (0,0,R) 画、降采样也只读 (0,0,R), 多出来的边角永远没人碰。 */
    if (s_SurfActive && w <= s_BigCapW && h <= s_BigCapH)
    {
        if (s_BigW != w || s_BigH != h)
        {
            s_BigW = w; s_BigH = h;
            s_RedrawFrames = 4;            /* R 变了: 催引擎把新露出的那圈重画 */
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
    /* ★ 必须**两张**面: 引擎双缓冲 (0x6D3FCF 把净面 CopyFromPart 覆盖脏面, 清掉上一帧
     * 单位)。入口装 s_CurC/s_CurM (初始 A/B), 渲染后 SB_RenderSwapOut 把引擎换完的
     * 分配记回 s_CurC/s_CurM, 下帧原样装回 ⇒ 轮换不被打断、无残影。*/
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
    s_RedrawFrames = 4;                 /* 新面全空: 逼引擎把整块重画几帧 */
    /* ★ 新面必须**清零**: DSurface 内部缓冲是 operator new 出来的, 内容未定义。
     *   地图光栅化只覆盖它认为该覆盖的格子, 没覆盖到的地方 (图外/半格) 会把堆里
     *   上一块内存的残留直接当成画面 —— 这就是"底栏残影闪在画面中部"的一个来源。
     *   (清成黑 = 引擎自己的图外底色, 视觉上也对。) */
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

/* ---------------- ZBuffer/ABuffer 按 R 重建 ----------------
 * 引擎 Set_View_Dimensions (0x4A8960) 的第 3/5 步就是"按 ViewBounds 释放+重建这两块
 * 缓冲", 但那个正门**顺带**做了四件在缩小档下全是坏事的事:
 *   (a) 覆写 ViewBounds 与 [0xB0CE2x] 缓存  → 缩放自己的不变量被它改掉
 *   (b) WWMouse 虚表 +0x0C/+0x10 重建鼠标视口 → 鼠标视口变成 R, 光标/死区判据全错
 *   (c) **末尾 InitGUI (0x6ABD30)**: 侧栏页签/cameo 行/小地图/底部工具栏的**一次性**
 *       布局写入全按传进去的矩形算。传 R (3504 宽) 进去, cameo 就被摆到 168 宽的
 *       侧栏面 x=3474 处 ⇒ 拉伸越界写坏堆 ⇒ 下一次侧栏重画崩
 *       (实测 `MOE: phase=applyview`, eip=0x6BA643 `mov [ecx],dx`, ecx=0x0FFD911E;
 *        栈上 0x6A9A43 在 0x6B600D 之下 ⇒ InitGUI→侧栏图标→拉伸 blit 的链)。
 *       同一个根因也让原版侧栏按钮的命中区被摆歪 ⇒ "侧栏点击失效"。
 *   (d) MessageList 区按 [0x886FA8]=R.Width 重设 → 消息框被摆到屏幕外。
 * 所以这里**只借它的构造/析构** (0x7BC970 / 0x7BCAE0 / 0x410CE0 / 0x410E50,
 * 对象 0x30 字节, 布局 +0x28=宽 +0x2C=高) 单独重建两块缓冲: 不碰 ViewBounds、
 * 不碰缓存、不碰 WWMouse、不碰侧栏布局。纯本地缓冲重建, 联机安全。
 *
 * 顺序上**先建新的再放旧的**: 分配失败时引擎手里仍是完好的旧缓冲 (宁可尺寸不匹配,
 * 也不能出现空指针 —— Render 每帧无条件解引用 [0x887644]/[0x87E8A4])。
 *
 * ⚠ **调用约定按 `ret N` 核实, 不要照"像是指针"去猜**:
 *   - `0x7C8E17` operator new / `0x7C8B3D` operator delete: 结尾裸 `ret` = cdecl,
 *     调用方清 1 个参数;
 *   - `0x7BCAE0` / `0x410E50` 析构: 结尾裸 `ret` = thiscall 无栈参数;
 *   - `0x7BC970` / `0x410CE0` 构造: 结尾 **`ret 0x10`** (0x7BCA46 / 0x410DB3),
 *     入口按 `[esp+4]/[+8]/[+0xC]/[+0x10]` 读 ⇒ 那个矩形是**按值传**的 16 字节,
 *     不是一个指针。第一版图省事写成 `(void*, const RectangleStruct*)` 传了 `&t`:
 *     矩形四个 dword 恰好被当值读走 (所以缓冲尺寸还是对的), 但被 callee 多弹掉
 *     12 字节 ⇒ **栈被砸**, 表现是 `eip=0xB`、`eax=1`、栈上出现半截日志文本
 *     (`MOE: phase=applyview`, 上一条日志正好是 `sbar: zbuf <- 2503x1497`)。*/
typedef void* (__cdecl    *SbAllocFn)(unsigned);
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
    ct(p, *r);                          /* ★ 按值传 (ret 0x10), 不能传指针 */
    /* 与 Set_View_Dimensions 第 3 步一致 (ctor 内部其实也写了同一个字段)。*/
    if (!isA) *(unsigned*)((unsigned char*)p + 0x24) = 0x8000u;
    return p;
}

static void Sb_DropBuf(int isA, void* p)
{
    if (!p || !IsPlausiblePtr(p)) return;
    ((SbBufDtorFn)(isA ? (size_t)ADDR_ABUF_DTOR : (size_t)ADDR_ZBUF_DTOR))(p);
    ((SbFreeFn)(size_t)ADDR_OP_DELETE)(p);
}

/* 缓冲当前真实尺寸 (直接读对象, 不信 s_BufW —— 引擎自己的 Set_View_Dimensions
 * 也会重建它们, 我们记的尺寸会与实际脱节, 脱节一次就是一次越界)。*/
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
    if (bw < 64 || bh < 64 || bw > 8192 || bh > 8192) return 0;
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

/* 缓冲是否已经是 (w,h)。s_BufW/s_BufH 只是我们的记账, 必须与对象里的真实值一致
 * 才算"已经是" —— 否则引擎那边刚用 A 重建过, 我们还以为自己是 R 大小。*/
static int SB_BufIs(int w, int h)
{
    int ww = 0, hh = 0;
    if (s_BufW != w || s_BufH != h) return 0;
    if (!SB_BufDims(&ww, &hh)) return 0;
    return (ww == w && hh == h);
}

/* ZBuffer/ABuffer 尺寸 + 新面催重画。每循环在 FM_Poll 之后调 (SB_Poll)。*/
void SB_PrepareSurface(void)
{
    RectangleStruct a, r;
    if (!s_Enable || s_Safe) return;
    /* 换面泄漏自愈: 上一帧 Render 入口换了大面却没走到还原点 (钩子被抢/异常),
     * 引擎全局就一直指着大面 ⇒ 后面按窗口坐标画的东西全进大面 ⇒ 残影。*/
    if (s_RenderSwapped) SB_RenderSwapOut();
    g_PreCurWin = 0;
    g_PreCurSurf = 0;
    FM_GetViewRects(&a, &r);
    if (!SB_Out())
    {
        if (s_SurfActive) SB_SurfaceDestroy();
        s_RedrawFrames = 0;
        s_SurfFail = 0;                 /* 离开缩小档: 下次可重试建面 */
        /* ★ 离开缩小档必须把 ZBuffer/ABuffer **缩回 A**。停在大尺寸 (R) 而渲染面
         * 已回到窗口 Composite 时, 光栅化的行步进/裁剪与目标面不一致 ⇒ `[edi]`
         * 走出界 (实测切回 1x/放大档时 0x494BF9 的 `mov [edi],ax` 崩)。*/
        if (s_GrowZBuf && r.Width >= 64 && r.Height >= 64)
        {
            if (!SB_BufIs(r.Width, r.Height)) SB_ResizeBufs(r.Width, r.Height);
        }
        else if (s_BufW > 0 && !SB_BufIs(a.Width, a.Height))
            SB_ResizeBufs(a.Width, a.Height);
        return;
    }
    if (!s_SurfActive || s_BigW != r.Width || s_BigH != r.Height)
    {
        if (!SB_EnsureBig(r.Width, r.Height)) return;   /* 建不出: 保持 A */
    }
    /* ★ ZBuffer/ABuffer 在缩小档**必须**按 R 重建。TacticalClass::Render 每帧用
     * ViewBounds(R) 的坐标 z-fill/z-test 这两块缓冲, 而引擎自己的 Set_View_Dimensions
     * 只按窗口 A 建 (FullMap 换宽度时就是这么调的) ⇒ 停在 A 大小时, 用 R 坐标寻址会
     * 越过行尾/缓冲末尾, 读/写到野地址 (实测 0x49741E 的 `mov dx,[eax]` 崩在垃圾堆指针)。
     * GrowZBuf 只作为"1x/放大档也按视口重建"的额外开关, 缩小档恒开。*/
    if (!SB_BufIs(r.Width, r.Height))
    {
        SB_ResizeBufs(r.Width, r.Height);
        /* 两块缓冲建不出来 = 内存不够。宁可退回 1x (R=A) 也不能让 Render 崩。*/
        if (!SB_BufIs(r.Width, r.Height))
        {
            if (!SB_ResizeBufs(a.Width, a.Height))
                s_BufW = s_BufH = 0;
            s_SurfFail = 1;
            SB_SurfaceDestroy();
            if (!s_SurfWarned) { s_SurfWarned = 1;
                LogLine("sbar: zbuf alloc FAILED -> zoom-out off"); }
            return;
        }
    }
    /* ★★ 一次性"洗大面": 把历史烙进大面的窗口坐标印记催掉。
     *   引擎的地图是**增量**画的 (只重画标脏的格子), 所以某一帧被按窗口坐标画进
     *   大面的东西 (最早是底栏, 见 SB_EnsureBig 里 s_WinComp 那段注释), 之后**永远
     *   不会被覆盖** —— 而我们每帧降采样都照抄整块 R, 于是那条印记就一直挂在画面
     *   中部。这解释了"根因修好后它不闪了却还在": 已经烙进去的像素没人负责清。
     *   触发点: s_WinComp 首次抓到窗口面 (修复生效的那一刻) —— 之前的帧都可能烙过。
     *
     * ⚠ **不要自己抹大面的像素**。踩过: 第一版这里调 `Sb_ClearSurf(s_CurC/s_CurM)`,
     *   结果在世界面还没换回来的时刻动了引擎正在用的面, 引擎的 ZBuffer 与实际像素
     *   脱节, 下一帧光栅器 `mov [edi],ax` (gamemd 0x494A36) 直接把像素写进我们
     *   自己的 DLL 里崩掉。催引擎自己重画整块 R 就够了 —— 它会把 R 里每个格子
     *   都重画一遍, 印记必被覆盖。*/
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
    /* 新面催重画 (换视口宽度本身不标脏, 见 Fm_DirtyRedraw)。*/
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
                ((SbRegDirtyFn)(size_t)ADDR_REG_DIRTY_SB)(tac, dr, 1);
            }
        }
    }
}

/* ---------------- 缩小档呈现: 大面的 R 区域 -> Composite 的 A ----------------
 * 与 SB_CompositeMagnify 方向相反 (缩小), 但源 (大面) 与目标 (窗口面) 是**两张**
 * 不同的面, 不存在采样时读到刚写进去的像素的问题, 可以直接读源写目标, 无需暂存。*/
int SB_CompositeDemagnify(void)
{
    RectangleStruct a, r;
    void* src = s_LastFinal;         /* 本帧渲染完的最终画面 (SB_RenderSwapOut 记录) */
    void* dst;
    unsigned char *sb, *db;
    int sp, sw, sh, dp, dww, dhh;
    if (!s_Enable || s_Safe) return 0;
    if (!SB_Out() || !s_SurfActive || !src) return 0;
    /* 本帧 Render 入口没换过大面 = 引擎没往大面画, 降采样会铺一片空白, 跳过。*/
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
    dst = *(void**)ADDR_SURF_COMPOSITE;      /* 已被 SB_RenderSwapOut 还原成窗口面 */
    if (!dst || !IsPlausiblePtr(dst) || dst == src) return Sb_Skip(2);
    /* 目标还是我们的大面 = 上一帧还原漏了 (换面泄漏)。此时降采样是"大面→大面",
     * 越采越糊, 而且按窗口坐标的 UI 会一直往里堆。跳过并自愈。*/
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
    if (a.X < 0) { a.Width += a.X; a.X = 0; }
    if (a.Y < 0) { a.Height += a.Y; a.Y = 0; }
    if (a.X + a.Width > dww) a.Width = dww - a.X;
    if (a.Y + a.Height > dhh) a.Height = dhh - a.Y;
    if (a.Width < 16 || a.Height < 16) return Sb_Skip(1);
    if (r.X < 0) r.X = 0;
    if (r.Y < 0) r.Y = 0;
    if (r.X + r.Width > sw) r.Width = sw - r.X;
    if (r.Y + r.Height > sh) r.Height = sh - r.Y;
    if (r.Width < 16 || r.Height < 16) return Sb_Skip(1);
    sb = GtLock(src);
    if (!sb) return Sb_Skip(4);
    db = GtLock(dst);
    if (!db) { GtUnlock(src); return Sb_Skip(4); }
    Sb_Resample(sb, sp, sw, sh, db, dp, dww, dhh,
                r.X, r.Y, r.Width, r.Height,
                a.X, a.Y, a.Width, a.Height,
                a.X, a.Y, a.X + a.Width, a.Y + a.Height);
    GtUnlock(dst);
    GtUnlock(src);
    /* ★ 作废引擎的 chrome 脏缓存。0x6D0A20 用 [0xB0CC00..(n)] (n=[0xB0CD44])
     *   记"哪些面已经画过底栏", 0x6D0B3C-0x6D0B57 命中就 bl=0 ⇒ **整段 chrome 跳过**。
     *   它的前提是"同一张面的像素没被动过"; 1x 成立是因为地图只覆写 ViewBounds=A,
     *   底栏带在 A 之外。缩小档我们每帧覆写 Composite, 前提被打破 ⇒ 缓存恒命中,
     *   底栏带永远不重画, 鼠标划过去时 WWMouse::vt+0x40 还原旧背景 => 底部重影。
     *   清 n= 让 0x6D0B3E 的 `jle 0x6D0B59` 命中 ⇒ bl=1 ⇒ 下帧必重画。只写一个
     *   计数器, 不改任何绘制状态。*/
    *(unsigned*)(size_t)SB_CHROME_DIRTY_N = 0;
    return 1;
}

/* ---------------- "这块面是不是我们该拦的那块画布" ----------------
 * 引擎的文本 blit (Simple_Text_Print / Print) 把**渲染面**当实参传下去, 而
 * VecText 的超武列表收表/拦截就是靠"面是不是主画布"这一句把原版列表摘下来的
 * (见 VecText.cpp 的 VecSimpleEntry)。
 *
 * 缩小档下三趟地图期间渲染面 = R 大的离屏大面 (2503x1497), 只认窗口面
 * (1920x1080) ⇒ 原版名/计时器/底色条整批漏出拦截带被放行, 画进大面, 降采样后
 * 就挤在画面右缘 (实测图4: 自定义面板没了, 变成引擎原本那串压扁的列表)。
 *
 * ⇒ 主画布 = 窗口面 (VecText 自己每帧知道它的尺寸), 再加上本模块自建的大面。
 *   1x/放大档下大面不存在 ⇒ 行为与原版逐位相同。*/
int SB_IsBigSurface(void* s)
{
    if (!s || !IsPlausiblePtr(s)) return 0;
    if (s == s_BigComp || s == s_BigMap ||
        s == s_CurC || s == s_CurM) return 1;
    return 0;
}

/* 每帧由消息帧钩喂窗口 Composite 的尺寸 (那一刻刚还原完, surf 必是窗口面)。
 * 不依赖 [ScaleBar] Enable —— 缩小档没开过时 s_WinW/s_WinH 也得有值。*/
void SB_NoteWindowSurface(void* surf, int w, int h)
{
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w < 8 || h < 8) return;
    if (w != s_WinW || h != s_WinH)
    {
        s_WinW = w;
        s_WinH = h;
    }
}

/* 只有 pitch 的场合 (VecText 的 BitFont 对象) 用这个。认不出的场合返回 0,
 * 调用方据此退回原版 1bpp 光栅 —— 只是外观退化, 不会出错。*/
int SB_CanvasH(int w)
{
    int ww = s_WinW, hh = s_WinH;
    if (w <= 0) return 0;
    /* 还没喂过窗口面 (消息帧钩还没跑): 问窗口尺寸。两者恒等 —— 引擎的
     * Composite 就是窗口大小 (诊断里 comp=1920x1080 / win=1920x1080)。*/
    if (ww <= 0 || hh <= 0) { ww = GtGameW(); hh = GtGameH(); }
    if (ww > 0 && hh > 0 && w == ww) return hh;
    if (SB_Out() && s_SurfActive && w == s_BigCapW) return s_BigCapH;
    return 0;
}

/* ---------------- 物理鼠标点 -> 视口点 (两条折算共用) -------------------
 * 引擎把地图画在渲染面的 (mapPos - TacticalPos), 屏幕点 p 落在的视口坐标 =
 * (p - A.xy) * R.size / A.size。两个方向共用这一个式子。
 *
* ⚠ **A 之外的点: 1x / 放大档原样放行, 不推**。调用点里不乏 UI/卷屏/侧栏那类本来
 * 就是屏幕空间的判据, 把 A 外的点乱推成 R 之外等于替引擎改它自己的数据。而这两种档
 * 下"原样放行"恰好也是对的: R ≤ A, 物理 y=1060 原样传下去仍然落在 R 之外, 引擎自己
 * 会拒:
 *   - ClientToCoords 只比 ViewBounds 的宽高 (0xB0CE30/34, 由
 *     TacticalClass::Set_View_Dimensions 0x6D5F60 从 ViewBounds 抄来);
 *   - 框选橡皮线 (0x6D5030) 与 Scroll_Map 都是同一套 ViewBounds 边界判定;
 *   - gadget 命中链 (GadgetClass::Input) 不经过这里 (它用虚表 +0x2C/+0x30, 且
 *     0x4E16AA/0x4E16BF 证明那里读的是 WWMouse 实例的**物理**坐标)。
 *
 * ⚠⚠ **缩小档 (R > A) 不能原样放行** —— 那正是"点底部底栏被映射到画面中部"的根因:
 *   R 比窗口大, 底栏带的物理 y (1048..1080) 落在 R (0..1497) **之内**, 于是引擎把
 *   "底栏"当成"地图上 y=1060 那一带", 点选/框选/点一下底栏都变成对那个地图像素
 *   下令, 而那个像素呈现在屏幕上的位置是 1060*A.H/R.H ≈ 742 ⇒ **画面正中**。
 *   所以缩小档下 A 外的窗口内点也走同一个线性变换, 折完必然落在 R 之外
 *   (物理 ≥ a.Y+a.Height ⇒ 视口 ≥ r.Height), "不是地图上的点"这件事在视口坐标里
 *   也成立; 再由 SB_PtInView 把它明确拒掉。
 *   屏幕空间的卷屏判据不受影响: SB_EdgeScrollPointFix 入口会把那个点换回物理鼠标。*/
void SB_MapPoint(Point2D* p, unsigned ret)
{
    RectangleStruct a, r;
    int vx, vy;
    if (!s_Enable || s_Safe || !p) return;
    if (!IsPlausiblePtr(p)) return;   /* 入参取错的话 p 会是野指针, 绝不能写 */
    if (!SB_Zoomed()) return;
    if (ret >= 0x00653700u && ret < 0x00653800u) return;   /* 雷达: 雷达面内坐标 */
    FM_GetViewRects(&a, &r);
    if (a.Width < 32 || r.Width < 16 || r.Height < 16) return;
    if (p->X < a.X || p->Y < a.Y ||
        p->X >= a.X + a.Width || p->Y >= a.Y + a.Height)
    {
        /* A 之外。缩小档 + 点还在窗口里 ⇒ 按同一变换折到 R 之外。窗口外的点
         * (拖出窗口) 维持原样: 那里两个坐标系的原点一样, 折不折无差别。*/
        int ww = GtGameW(), wh = GtGameH();
        if (!SB_Out() || ww <= 0 || wh <= 0) return;
        if (p->X < 0 || p->Y < 0 || p->X >= ww || p->Y >= wh) return;
        vx = (int)((long)(p->X - a.X) * (long)r.Width / (long)a.Width);
        vy = (int)((long)(p->Y - a.Y) * (long)r.Height / (long)a.Height);
        if (vx <= r.Width) vx = r.Width + 1;   /* 保证真的出界 */
        if (vy <= r.Height) vy = r.Height + 1;
        if (vx > 0x7FFF) vx = 0x7FFF;
        if (vy > 0x7FFF) vy = 0x7FFF;
        p->X = vx;
        p->Y = vy;
        return;
    }
    vx = (int)((long)(p->X - a.X) * (long)r.Width / (long)a.Width);
    vy = (int)((long)(p->Y - a.Y) * (long)r.Height / (long)a.Height);
    if (vx < 0) vx = 0;
    if (vy < 0) vy = 0;
    if (vx > r.Width) vx = r.Width;
    if (vy > r.Height) vy = r.Height;
    p->X = vx;
    p->Y = vy;
}

/* 这个**视口点**算不算"落在地图视口里"。
 * 消费方: MOExperience_ClickCoordsEntry (DisplayClass::ProcessClickCoords 0x692300
 * 的入口钩) —— 那个函数**只**拒 X<0 / Y<0, 没有上界判定 (反汇编 0x69234B/0x692356),
 * 所以缩小档下必须由我们把"视口外"的点明确拒掉, 否则底栏点击会变成对地图下令。
 * 1x/放大档恒 1 (R ≤ A, 原样放行的点本来就落在 R 外, 由引擎自己的边界逻辑管)。*/
int SB_PtInView(int vx, int vy)
{
    RectangleStruct a, r;
    if (!s_Enable || s_Safe) return 1;
    if (!SB_Out()) return 1;
    FM_GetViewRects(&a, &r);
    if (r.Width < 16 || r.Height < 16) return 1;
    if (vx < 0 || vy < 0) return 0;
    if (vx > r.Width || vy > r.Height) return 0;
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
    /* 缩小档的离屏大面 + ZBuffer/ABuffer 尺寸必须在本帧 Render 之前准备好。 */
    SB_PrepareSurface();
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
    void* surf = *(void**)ADDR_SURF_COMPOSITE;
    if (s_Safe) return 0;
    /* 绝不能把条画进缩小档的大面: 那些像素会跟着降采样出现在画面中部, 看起来
     * 就是"底栏残影在地图里闪"。窗口面才画。*/
    if (Sb_IsBigSurf(surf)) return 0;
    if (Sb_PaintSurface(surf)) { s_Drew = 1; return 1; }
    return 0;
}