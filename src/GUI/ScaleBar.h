/* ScaleBar.h - 地图缩放 (放大档 0.25 = 4 倍 / 0.5 = 2 倍; 缩小档 1.5 = 1.5 倍 /
 * 2 = 2 倍视野, 需 [ScaleBar] ZoomOut=1; 1 = 原样)。
 * 实现见 ScaleBar.cpp, 引擎侧实证见 notes/地图缩放.md。
 *
 * 模型: 两个矩形
 *   A = 满视图区 (引擎本该用的视口, 侧栏模式决定宽高)
 *   R = 引擎实际视口 = A * scale (scale<1 放大, scale>1 缩小)
 * 引擎只会 1:1 逐格光栅化 (格子恒 60x30), 没有缩放路径:
 *   放大 (scale<1): 视口收小 -> 引擎画得少 -> 呈现时把 R 最近邻放大铺回 A;
 *   缩小 (scale>1): 引擎画进一张 R 那么大的离屏面 -> 三趟之后把 R 最近邻降采样
 *     铺回 A。大面由本模块自建并只在渲染期间临时换进 [0x88731C]/[0x8872FC],
 *     ZBuffer/ABuffer 按 R 经 Set_View_Dimensions 重建 (见 SB_PrepareSurface)。
 * 呈现写的是 Composite 而不是 Primary: 它是后面每次上屏 blit 的**源**。
 */
#pragma once

/* 读 [ScaleBar] 节 (幂等)。 */
void SB_EnsureInit(void);
int  SB_Safe(void);

/* 每循环 (prepare 钩) 调: 摆刻度条矩形 + 处理点击换档 + 准备缩小档大面。 */
void SB_Poll(void);

/* 当前档序号 (0 = 最左 = 最小 scale = 放大最多)。 */
int SB_Level(void);

/* 当前档的视口缩放: R = A * SB_Scale()。0.25 = 4 倍放大, 1 = 原样, 2 = 2 倍视野。 */
double SB_Scale(void);

/* 1 = 放大档 (视口比视图区小)。 */
int SB_In(void);

/* 1 = 缩小档 (视口比视图区大, scale>1)。 */
int SB_Out(void);

/* 1 = 当前档位不是 1x (放大或缩小, 且未 Safe)。 */
int SB_Zoomed(void);

/* 缩小档: 每循环 (prepare) 准备离屏大面 + 按 R 重建 ZBuffer/ABuffer。 */
void SB_PrepareSurface(void);
/* 缩小档: 让回窗口面。forCursor != 0 = 供 0x4F4492 那条光标擦除钩用 (顺带把
 * 入口 eax 换成窗口面, g_PreCurSurf); forCursor == 0 = 供"按窗口坐标画的引擎
 * UI"用 (Render 中段的 chrome pass, 见下), 不碰 g_PreCur*。
 * 两个 Resume 成对: 0x4F44CB 装回大面, 0x4F4505 (chrome pass 之后) 装回大面。
 * ⇒ 大面被暴露的窗口只剩"三趟地图"这一段。*/
void SB_RenderSuspend(int forCursor);
void SB_RenderResume(void);
/* 缩小档: GScreenClass::Render (0x4F4480) 入口把 Composite/地图面临时换成大面。 */
void SB_RenderSwapIn(void);
/* 缩小档: 消息帧钩里还原 Composite/地图面 (之后才做降采样+UI)。 */
void SB_RenderSwapOut(void);
/* 缩小档: 三趟地图画完 (0x4F451B) 立刻还原窗口面 + 降采样。必须早于引擎 UI,
 * 否则侧栏/cameo 被画进大面 (底栏残影 + 越界写坏堆)。1x/放大档恒空操作。 */
void SB_RenderAfterMap(void);
/* 边缘卷屏 (0x692B60) 入口修点: 缩放档把传入点换回物理鼠标 (屏幕空间死区)。 */
void SB_EdgeScrollPointFix(Point2D* pt);
/* 缩小档: 把大面的 R 区域最近邻降采样铺回 Composite 的 A (见 SB_CompositeMagnify)。 */
int SB_CompositeDemagnify(void);

/* CCToolTip 选面/量宽用的矩形影子 = 满视图区 A (见 ScaleBar.cpp)。*/
const unsigned* SB_AViewBlock(void);
void SB_PublishAView(void);

/* 1 = s 是本模块自建的缩小档离屏大面 (两张 DSurface 之一)。
 * 引擎的文本 blit 会把**渲染面**当实参传下去, 而缩小档下渲染面在三趟地图期间
 * 就是这张大面 (2503x1497)。VecText 把原版超武列表摘下来靠的是"面是不是主画布"
 * 这一句 —— 只认窗口面 (1920x1080) 时缩小档下那批行整批漏出拦截带, 被降采样挤
 * 到画面右缘, 自定义面板反而整块消失 (实测图4)。用**指针**判最准 (大小相同的
 * 面在本项目里只有这两类, 但指针判定不受将来窗口改尺寸影响)。*/
int SB_IsBigSurface(void* s);
/* 按宽度认"某张我们认的绘制面", 返回它的高 (0 = 不认)。给手上只有 pitch/宽度
 * 的场合用 (VecText 的 BitFont 对象只带 pitch)。*/
int SB_CanvasH(int w);
/* 每帧由消息帧钩喂一次窗口 Composite 的尺寸 (缩放没开时也要有, 否则
 * SB_CanvasH 认不出窗口面)。传 0/NULL = 忽略。*/
void SB_NoteWindowSurface(void* surf, int w, int h);

/* 当前档要写进 ViewBounds 的视口矩形; 返回 0 = 不缩放 (用 A 本身)。 */
int SB_GetViewRect(RectangleStruct* out);

/* 三趟 Render 之后、UI 之前调: 把 Composite 的 R 最近邻放大覆盖到视图区 A,
 * 于是之后的 UI (引擎 MessageList + 本项目各 *_Draw) 全部照旧 1:1 画在
 * Composite 上, 位置不变、不被放大。返回 1 = 做了放大。*/
int SB_CompositeMagnify(void);

/* WWMouseClass 虚表 +0x34 (GetMousePosition) 与窗口过程那条 lParam 折算共用:
 * 把**物理**鼠标点反变换成视口空间的点
 * (v = (p - A.xy) * R.size / A.size; ViewBounds.X/Y 缩放时恒为 0)。
 * 视图区 A 之外的点原样放行 —— 引擎自己那套 ViewBounds 边界判定会拒掉它们。
 * ret = 调用点返回地址 (保留给将来需要按调用点区别处理时用, 现在恒传 0)。 */
void SB_MapPoint(Point2D* p, unsigned ret);

/* 视图坐标 (引擎 CoordsToClient 的输出 = 渲染面坐标) → 屏幕坐标。
 * scale=1 时恒等; 供本项目那些按引擎投影画图元的层用。 */
int SB_ViewToScreen(int vx, int vy, int* sx, int* sy);

/* 这个**视口点**是否落在地图视口 R 里。缩小档下 R 比窗口大, 引擎自己的边界逻辑
 * 拒不掉"点在窗口内但在视图区外"(底栏带)的那种点, 由本函数 + ProcessClickCoords
 * 入口钩明确拒掉 (见 ScaleBar.cpp 的 SB_PtInView 与 MOExperience_ClickCoordsEntry)。
 * 1x/放大档恒 1。*/
int SB_PtInView(int vx, int vy);

/* 地图像素增量 → 屏幕增量 (给那些"以鼠标物理像素为基准自己算等距投影"的层用:
 * SmartArea/FuzzyAttack 的路径预览)。scale=1 时恒等。 */
void SB_MapDeltaToScreen(int* dx, int* dy);

/* 刻度条: 兜底画进 Composite (SW_DrawToComposite 由每帧绘制钩调)。 */
int SB_DrawToComposite(void);
