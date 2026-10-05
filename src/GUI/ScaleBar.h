/* ScaleBar.h - 地图缩放 (只有放大档: 0.25 = 4 倍 / 0.5 = 2 倍 / 1 = 原样)。
 * 实现见 ScaleBar.cpp, 引擎侧实证见 notes/地图缩放.md。
 *
 * 模型: 两个矩形
 *   A = 满视图区 (引擎本该用的视口, 侧栏模式决定宽高)
 *   R = 引擎实际视口 = A * scale (scale<1 放大)
 * 引擎只会 1:1 逐格光栅化 (格子恒 60x30), 没有缩放路径, 所以放大只能是
 * "视口收小 -> 引擎画得少 -> 呈现时把 R 最近邻放大铺回 A"。呈现那步写的是
 * Composite 而不是 Primary: 它是后面每次上屏 blit 的**源**, 在它上面改, 后面
 * 多少次 blit 都把放大结果原样搬上屏 (见 SB_CompositeMagnify)。
 * 缩小档 (scale>1) 需要一张"整张图那么大"的 DirectDraw 面, 实测 ddraw 内部
 * 扛不住, 整块停用 (档位表里只生成 <1 的档)。
 */
#pragma once

/* 读 [ScaleBar] 节 (幂等)。 */
void SB_EnsureInit(void);
int  SB_Safe(void);

/* 每循环 (prepare 钩) 调: 摆刻度条矩形 + 处理点击换档。 */
void SB_Poll(void);

/* 当前档序号 (0 = 最左 = 最小 scale = 放大最多)。 */
int SB_Level(void);

/* 当前档的视口缩放: R = A * SB_Scale()。0.25 = 4 倍放大, 1 = 原样。 */
double SB_Scale(void);

/* 1 = 放大档 (视口比视图区小)。 */
int SB_In(void);

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

/* 地图像素增量 → 屏幕增量 (给那些"以鼠标物理像素为基准自己算等距投影"的层用:
 * SmartArea/FuzzyAttack 的路径预览)。scale=1 时恒等。 */
void SB_MapDeltaToScreen(int* dx, int* dy);

/* 刻度条: 兜底画进 Composite (SW_DrawToComposite 由每帧绘制钩调)。 */
int SB_DrawToComposite(void);
