/* ScaleBar.h - 地图缩放刻度条 (放大 0.25 = 4 倍 ... 缩小到"看到全图")。
 * 实现见 ScaleBar.cpp, 引擎侧实证见 notes/地图缩放.md。
 *
 * 模型: 两个矩形
 *   A = 满视图区 (引擎本该用的视口, 侧栏模式决定宽高)
 *   R = 引擎实际视口 = A 的 scale 倍 (scale<1 放大 / scale>1 缩小)
 * 引擎只会 1:1 逐格光栅化 (格子恒 60x30), 没有缩放路径, 所以:
 *   - 放大 (scale<1): 视口收小, 引擎画得少, 呈现时把 R 最近邻放大铺回 A;
 *   - 缩小 (scale>1): 视口放大到装得下 R, 引擎照旧 1:1 画 —— 但画到**我们自己的
 *     一张整图大小的面**上 (SB_RenderSurface), 三趟 Render 之后把 R 降采样进
 *     Composite 的 A (SB_PostMapRender)。UI 全程画在 Composite 上, 天然 1:1 不受影响。
 * 档位表在运行时按地图尺寸算出来 (SB_LevelCount/SB_LevelLabel), 不是写死的 3 档。
 */
#pragma once

/* 读 [ScaleBar] 节 (幂等)。 */
void SB_EnsureInit(void);
int  SB_Safe(void);

/* 每循环 (prepare 钩) 调: 探地图尺寸/重建档位表/摆刻度条矩形/处理点击与滚轮。 */
void SB_Poll(void);

/* 当前档序号 (0 = 最左 = 最小 scale = 放大最多)。 */
int SB_Level(void);

/* 当前档的视口缩放: R = A * SB_Scale()。0.25 = 4 倍放大, 1 = 原样, >1 = 缩小。 */
double SB_Scale(void);

/* 当前档的放大倍数 (1/scale 的整数形式, 只用于日志/文字)。 */
int SB_Magnify(void);

/* 档位表 (运行时按地图尺寸与预算生成)。 */
int SB_LevelCount(void);
const char* SB_LevelLabel(int i);
double SB_LevelScale(int i);

/* 1 = 缩小档 (视口比视图区大, 要走"自建面 + 降采样"那条路)。 */
int SB_Out(void);
/* 1 = 放大档 (视口比视图区小, 呈现时放大铺回)。 */
int SB_In(void);
int SB_CanZoomOut(void);

/* 当前档要写进 ViewBounds 的视口矩形; 返回 0 = 不缩放 (用 A 本身)。 */
int SB_GetViewRect(RectangleStruct* out);

/* TacticalClass::Render (0x6D3D10) 入口钩: 缩小档把渲染面换成我们的大面,
 * 原面传回 0 时用引擎自己的 Composite。返回非 0 = 替换。 */

/* 三趟 Render 之后、UI 之前 (钩 0x4F451B): 把大面里的 R 降采样进 Composite 的 A。
 * 只重画脏矩形 (RegisterDirtyArea 汇总的), 顺带做周期性全量校正。 */

/* 呈现之后 (DoBlit 0x4F4780 的后置 trampoline) 调: 放大档把 Composite 的 R 放大
 * 铺到 Primary 的 A, 再把 A\R 四条带拷回 (UI 在那几条带子里)。返回 1 = 做了放大。 */
int SB_CompositeMagnify(void);

/* WWMouseClass 虚表 +0x34 (GetMousePosition) 的钩共用: 把**物理**鼠标点反变换成
 * 视口空间的点 (v = (p - A.xy) * R.size / A.size; ViewBounds.X/Y 缩放时恒为 0)。
 * 视图区 A 之外的点原样放行 —— 引擎自己那套 ViewBounds 边界判定会拒掉它们。
 * ret = 调用点返回地址 (保留给将来需要按调用点区别处理时用, 现在恒传 0)。 */
void SB_MapPoint(Point2D* p, unsigned ret);

/* 视图坐标 (引擎 CoordsToClient 的输出 = 渲染面坐标) → 屏幕坐标。
 * scale=1 时恒等; 供本项目那些按引擎投影画图元的层用。 */
int SB_ViewToScreen(int vx, int vy, int* sx, int* sy);

/* 地图像素增量 → 屏幕增量 (给那些"以鼠标物理像素为基准自己算等距投影"的层用:
 * SmartArea/FuzzyAttack 的路径预览)。scale=1 时恒等。 */
void SB_MapDeltaToScreen(int* dx, int* dy);

/* 帧尾 (BlitSidebar 钩) 把刻度条画到 Primary: 底部工具栏那条 32px 带子不在
 * Composite 的呈现范围内, 画进 Composite 会被盖掉。返回 1 = 画了。 */
int SB_DrawToPrimary(void);

/* 兜底: 画进 Composite。 */
int SB_DrawToComposite(void);

/* 滚轮 (MOExperience_WheelHook @0x777998) 调: 悬在条上换一档, dirUp=1 是上滚
 * (往放大那边)。返回 1 = 吃掉了这一滚。 */
int SB_WheelIn(int mx, int my, int dirUp);

/* RegisterDirtyArea (0x6D2790) 钩: 缩小档下把引擎标脏的矩形折算成降采样目的
 * 区的脏矩形 (纯渲染状态, 不碰模拟)。 */

/* 缩小档且视口很大时跳过 ZBuffer/ABuffer 的每帧清零 (视口按 ViewBounds 重建,
 * 缩小档下它们是几千万像素, 每帧 memset 光清零就十几毫秒)。纯渲染缓冲,
 * 代价是深度/alpha 值不归零 (光束/子弹/粒子的混合在缩小档下可能略有偏差)。 */
int SB_SkipZAFill(void);