/* ScaleBar.h - 地图缩放。A = 满视图区, R = A * scale 为引擎实际视口
 * (scale<1 放大, scale>1 缩小)。放大把 R 铺回 A; 缩小经 R 大面降采样铺回 A。
 * 实现见 ScaleBar.cpp, 实证见 notes/地图缩放.md。
 */
#pragma once

/* 读 [ScaleBar] 节 (幂等)。 */
void SB_EnsureInit(void);
int  SB_Safe(void);

/* 每循环 (prepare 钩) 调: 摆刻度条矩形 + 处理点击换档 + 准备缩小档大面。 */
void SB_Poll(void);

/* 当前档序号 (0 = 最左 = 最小 scale = 放大最多)。 */
int SB_Level(void);

/* 引擎重建了渲染面 (读档 / 改分辨率) ⇒ 缩放退回 1x 档。
 * 换面会触发侧栏全量重画, 那条路按 ViewBounds 重推 cameo 落点锚点, 必须让
 * ViewBounds 是 A 而不是 R (见实现处的推导)。须在 Fm_ApplyViewWidth 之前调。*/
void SB_ResetZoom1x(void);

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
/* 缩小档: 让回窗口面 (光标擦除 / Render 中段 chrome 用, 见 SB_RenderSuspend)。 */
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
/* 缩小档: 把大面的 R 区域最近邻降采样铺回 Composite 的 A (见 SB_CompositeMagnify)。 */
int SB_CompositeDemagnify(void);

/* CCToolTip 选面/量宽用的矩形影子 = 满视图区 A (见 ScaleBar.cpp)。*/
const unsigned* SB_AViewBlock(void);
void SB_PublishAView(void);

/* 1 = s 是本模块自建的缩小档离屏大面 (两张之一, 指针判定)。 */
int SB_IsBigSurface(void* s);
/* 按宽度认"某张我们认的绘制面", 返回它的高 (0 = 不认)。给手上只有 pitch/宽度
 * 的场合用 (VecText 的 BitFont 对象只带 pitch)。*/
int SB_CanvasH(int w);
/* 每帧由消息帧钩喂一次窗口 Composite 的尺寸 (缩放没开时也要有, 否则
 * SB_CanvasH 认不出窗口面)。传 0/NULL = 忽略。*/
void SB_NoteWindowSurface(void* surf, int w, int h);

/* 当前档要写进 ViewBounds 的视口矩形; 返回 0 = 不缩放 (用 A 本身)。
 * scale 由调用方一次性算好传入 (省函数内重算)。 */
int SB_GetViewRect(RectangleStruct* out, double scale);

/* 三趟之后、UI 之前把 R 放大覆盖回 A, 之后 UI 照旧 1:1 画。返回 1 = 做过放大。*/
int SB_CompositeMagnify(void);

/* 帧入口 (GScreenClass::Draw 0x4F4480, 引擎动 R 之前) 调: 放大档把视口 R 整块
 * 注册成脏区。就地放大每帧把 R 换成放大像素, 而引擎光栅只重画标脏的格子 ⇒
 * 不标脏就会拿放大结果当下一次的源。见实现处说明。*/
void SB_MarkViewDirty(void);

/* 物理鼠标点反变换成视口空间点 (v = (p - A.xy) * R / A)。A 外点原样放行。 */
void SB_MapPoint(Point2D* p, unsigned ret);

/* 视图坐标 (引擎 CoordsToClient 的输出 = 渲染面坐标) → 屏幕坐标。
 * scale=1 时恒等; 供本项目那些按引擎投影画图元的层用。 */
int SB_ViewToScreen(int vx, int vy, int* sx, int* sy);

/* 视口点是否落在 R 里 (缩小档 R 比窗口大, 引擎边界拒不掉底栏带)。1x/放大恒 1。*/
int SB_PtInView(int vx, int vy);

/* 视口比地图大: 居中钳位。原版 min>max 时 TacticalPos 每帧来回跳 ⇒ 抖动。
 * 退化轴直接钉地图正中, 边界读引擎全局自己算。返回值语义与原版一致。 */
int SB_ClampFix(Point2D* c);
/* 1 = 缩放接管中 (该由本模块接管 Clamp_Center)。*/
int SB_ClampNeeded(void);

/* 每帧把"图外"铺成底色 (大面, chrome pass 前后各一次)。增量拷贝搬来的旧像素
 * 没人覆盖, 必须每帧铺。 */
void SB_PaintVoid(void);

/* "在场"探针 (0x6D9B50): 不接管 (表里是建筑, 飞机只走 mode2)。 */

/* 迷雾 (0x6D7840) 越界写护栏: [0x887314] 非我方大面时换 s_CurC 挡写。纯指针比较。 */
void SB_ShroudGuard(void);
void SB_ShroudReport(void);   /* 由 SB_RenderAfterMap 调, 把命中的次数打进日志 */
/* 崩溃处理用: 把四个大面指针 + 存活标志追记进日志 (只读内存里的指针, 不解引用)。 */
void SB_AppendCrashSurfs(char* buf, unsigned* p, unsigned cap);

/* 地图像素增量 → 屏幕增量 (给那些"以鼠标物理像素为基准自己算等距投影"的层用:
 * SmartArea/FuzzyAttack 的路径预览)。scale=1 时恒等。 */
void SB_MapDeltaToScreen(int* dx, int* dy);

/* 刻度条: 兜底画进 Composite (SW_DrawToComposite 由每帧绘制钩调)。 */
int SB_DrawToComposite(void);
