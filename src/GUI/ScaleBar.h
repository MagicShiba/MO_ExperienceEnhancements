/* ScaleBar.h - 地图缩放。A = 满视图区，R = A * scale 为引擎实际视口
 * (scale<1 放大，scale>1 缩小)。放大把 R 铺回 A；缩小经 R 大面降采样铺回 A。
 */
#pragma once

/* 读 [BottomBar] 节 (幂等)。 */
void SB_EnsureInit(void);

/* 每循环 (prepare 钩) 调: 摆刻度条矩形 + 处理点击换档 + 准备缩小档大面。 */
void SB_Poll(void);

/* 当前档序号 (0 = 最左 = 最小 scale = 放大最多)。 */
int SB_Level(void);

/* 引擎重建了渲染面 (读档 / 改分辨率) ⇒ 缩放退回 1x 档。
 * 须在 Fm_ApplyViewWidth 之前调。 */
void SB_ResetZoom1x(void);

/* 当前档的视口缩放: R = A * SB_Scale()。0.25 = 4 倍放大, 1 = 原样, 2 = 2 倍视野。
 * R 超 [BottomBar] LimitFallback 时钳到限制上 (长边=限制值, 见 SB_GetViewRect)。 */
double SB_Scale(void);

/* 1 = 放大档 (视口比视图区小)。 */
int SB_In(void);

/* 1 = 缩小档 (视口比视图区大, scale>1)。 */
int SB_Out(void);

/* 1 = 当前档位不是 1x (放大或缩小, 且未 Safe)。 */
int SB_Zoomed(void);

/* 缩小档: 每循环 (prepare) 准备离屏大面 + 按 R 重建 ZBuffer/ABuffer。 */
void SB_PrepareSurface(void);
/* 缩小档: 让回窗口面 (光标擦除 / Render 中段 chrome 用)。 */
void SB_RenderSuspend(int forCursor);
void SB_RenderResume(void);
/* 缩小档: GScreenClass::Render (0x4F4480) 入口把 Composite/地图面临时换成大面。 */
void SB_RenderSwapIn(void);
/* 缩小档: 消息帧钩里还原 Composite/地图面 (之后才做降采样+UI)。 */
void SB_RenderSwapOut(void);
/* 缩小档: 三趟地图画完立刻还原窗口面 + 降采样 (须早于引擎 UI)。 */
void SB_RenderAfterMap(void);
/* 边缘卷屏 (0x692B60) 入口修点: 缩放档把传入点换回物理鼠标 (屏幕空间死区)。 */
void SB_EdgeScrollPointFix(Point2D* pt);
/* 缩小档: 把大面的 R 区域降采样铺回 Composite 的 A。 */
int SB_CompositeDemagnify(void);

/* CCToolTip 选面/量宽用的矩形影子 = 满视图区 A。 */
const unsigned* SB_AViewBlock(void);
void SB_PublishAView(void);

/* 1 = s 是本模块自建的缩小档离屏大面 (指针判定)。 */
int SB_IsBigSurface(void* s);
/* 按宽度认某张绘制面，返回它的高 (0 = 不认)。 */
int SB_CanvasH(int w);
/* 每帧由消息帧钩喂一次窗口 Composite 的尺寸。传 0/NULL = 忽略。 */
void SB_NoteWindowSurface(void* surf, int w, int h);

/* 当前档要写进 ViewBounds 的视口矩形；返回 0 = 不缩放 (用 A 本身)。 */
int SB_GetViewRect(RectangleStruct* out, double scale);

/* 三趟之后、UI 之前把 R 放大覆盖回 A。返回 1 = 做过放大。 */
int SB_CompositeMagnify(void);

/* 帧入口 (引擎动 R 之前) 调：放大档把上一帧留在 R 里的放大像素还原，
 * 移动的帧另整块标脏 (见 notes §5.1)。 */
void SB_ViewPreRender(void);

/* 物理鼠标点反变换成视口空间点。连续且钳到 R 内。
 * 返回 1 = 可按原逻辑继续用 p；返回 0 = p 在视口外 (别拿去查格/下令)。 */
int SB_MapPoint(Point2D* p);

/* 视图坐标 (引擎 CoordsToClient 的输出 = 渲染面坐标) → 屏幕坐标。
 * scale=1 时恒等; 供本项目那些按引擎投影画图元的层用。 */
int SB_ViewToScreen(int vx, int vy, int* sx, int* sy);

/* 视口点是否落在 R 里。1x/放大恒 1。 */
int SB_PtInView(int vx, int vy);

/* 物理点是否落在视口 A 里 (拒掉栏带点击用)。 */
int SB_PtInRawView(int x, int y);

/* 视口比地图大：居中钳位。退化轴钉地图正中。 */
int SB_ClampFix(Point2D* c);
/* 1 = 缩放接管中。 */
int SB_ClampNeeded(void);

/* 三趟地图后、降采样前铺一次本帧最终面的图外。 */
void SB_PaintVoid(void);

/* "在场"探针：不接管。 */

/* 迷雾越界写护栏：[0x887314] 非我方大面时换 s_CurC 挡写。 */
void SB_ShroudGuard(void);
void SB_ShroudReport(void);
/* 崩溃处理用：追记大面指针 + 存活标志 (只读，不解引用)。 */
void SB_AppendCrashSurfs(char* buf, unsigned* p, unsigned cap);

/* 地图像素增量 → 屏幕增量。scale=1 时恒等。 */
void SB_MapDeltaToScreen(int* dx, int* dy);

/* 刻度条：画进 Composite。 */
int SB_DrawToComposite(void);

/* 刻度条矩形 (暂停按钮锚在它左侧用)。返回 0 = 没开/没摆好。 */
int SB_BarGeom(int* x, int* y, int* w, int* h);
