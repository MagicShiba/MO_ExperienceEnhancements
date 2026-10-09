/* FullMap.h - 隐藏原版建造/生产侧栏, 地图铺满整个窗口宽度, 小地图保留在原角。
 * 实现见 FullMap.cpp (地址全部来自 gamemd.exe 反汇编实证), 根因/不变量见
 * notes/隐藏侧栏.md。构建见 build-fast.bat。 */
#pragma once

#include "GdiText.h"

/* 读 [FullMap] 节 (幂等)。 */
void FM_EnsureInit(void);

/* 装字节补丁 + CreateAll 两个调用点重定向: 让**渲染面永远全宽** (态0 要把视口
 * 收回 clientW-168, 面不能跟着缩)。DllMain 里调 (见 MOExperience.cpp)。 */
void FM_Install(void);

/* 每循环 (prepare 钩 0x4F42F0) 调: 采 F11 三态切换、撑视口、纠正侧栏列、摘
 * 侧栏 gadget。三态 (ini [FullMap] ToggleVk, 默认 F11):
 *   0 完全显示  视口收回 clientW-168, 整条侧栏面上屏, gadget 可用, 点击让位引擎
 *   1 只显示小地图和顶部两个按钮 (视口全宽)
 *   2 全屏      侧栏一点不显示, 整窗都是地图, 点击全给地图
 * ⚠️ 态0 不能只 blit 整条侧栏而视口仍全宽: 凡是拿视口跟侧栏列做比较的地方
 * 全错 168, 页签/cameo/小地图一个都点不动。 */
void FM_Poll(void);

/* 缩放改档后立刻同步一次视口 (ScaleBar 的 SB_Poll 里调)。
 * 背景: SB_Poll 先跑 PrepareSurface(旧档) 再处理点击改档, 而引擎裁剪对象
 * ([0x887644]/[0x87E8A4], setDims 按 ViewBounds 重建) 要等下一帧 FM_Poll 才更新。
 * 中间的 Render/sidebar blit 就会用旧 R 裁剪往窗口小面里写 ⇒ 0x494BF9 越界崩。
 * 这里直接调一次 Fm_ApplyViewWidth (幂等, setDims 有帧门控), 让改档当帧就一致。*/
void FM_SyncViewNow(void);

/* 条带上屏钩 (SidebarClass::BlitSidebar 0x6A70E0) 调: 只 blit 保留的那一小块,
 * 补齐原版尾声的脏标记, 然后返回"直接 ret 4 的出口"指针 —— 调用方把
 * g_BlitSbTramp 指向它即等于跳过原版整条 blit。返回 0 = 不接管。 */
unsigned long FM_BlitSb(void);

/* 把保留区 blit 进指定面 (通常是 Composite, 每帧呈现前的绘制点)。走这条路而不是
 * 靠 BlitSidebar: 地图一铺满就每帧盖掉右上角, 而侧栏上屏时机并不保证跟着地图
 * 呈现, 挂在每帧必跑的呈现前绘制点上就没有闪烁。返回 1=画了。 */
int FM_DrawRadarTo(void* surf);

/* GadgetClass::Input (0x4E1640) 钩: 输入链上唯一能拿到真实 gadget 指针 (this)
 * 的地方, 也是原函数调 ExtractGadgetAt 之前唯一的插手时机 —— 所以
 * FM_TrimViewGadget 挂在这一层。只在 gadget 横向跨过侧栏分界线时记下它。 */
void FM_NoteGadgetInput(void* gadget);

/* 把那个"横跨视口"的 gadget 的命中区收到"视口减去侧栏列"(与原版几何一致)。
 * 它的矩形恒等于 ViewBounds; 视口全宽时它连侧栏列一起吞掉, 那一列的点击全被判
 * 成"点在视口上"。纯命中区状态, 联机安全。 */
void FM_TrimViewGadget(void);

/* 本循环实际用的两个视口矩形 (缩放模块 ScaleBar 要):
 *   a = 满视图区 (侧栏模式决定宽度, 引擎"本该"用的视口)
 *   r = 引擎实际拿到的视口 = a 居中的 1/m 子矩形 (m = ScaleBar 的放大倍数,
 *       m=1 时与 a 相同)。r 会被写进 ViewBounds。
 * 两个指针都可传 0 表示不要那个。FM_Poll 每循环刷新。 */
void FM_GetViewRects(RectangleStruct* a, RectangleStruct* r);

/* DisplayClass::ProcessClickCoords (0x692300) 落点 0x692406 的点击门 (Syringe 钩,
 * 见 MOExperience.dll.inj): 点击落在屏幕上那块小地图矩形里时, 自己把视口对准
 * 那一格 (RadarClass::ScrollTo 0x653F70), 再返回 0x6925FC (原函数的"未处理"出口)
 * 把这次点击吞掉, 不让它穿透去框单位/打靶。返回 0 = 交回原版。
 * 格范围由 Fm_ScanTacticalMapRect 只读扫 TacticalClass 得到, 没扫到就只吞不跳。
 * 纯本地摄像机, 联机安全。 */
unsigned FM_RadarClickGate(void* regs);






/* ProcessClickCoords 拒点门要不要为"侧栏顶部那一块"(资金条/小地图/顶部两按钮)
 * 放行。= 点击门武装好了 且物理鼠标点落在那一块 ⇒ 与点击门必然同进同退 (两处判定
 * 同源, 不会出现"放行了却没人接")。缩放档下那一块在引擎眼里已不在视口里
 * (ViewBounds=R), 不放行就等于小地图点不动。纯几何判定, 不写内存。*/
int  FM_ShouldDeferClick(void);

/* 把 SidebarBounds.X 纠正成"侧栏列在屏幕上的起点"(视口右边界 - 列宽)。引擎自己
 * 算的是 viewRect.X+viewRect.W, 视口铺满后会差 168。FM_Poll 每循环调一次 (全屏
 * 态改推成"恒不成立"的鼠标路由阈值), DisplayClass::InitGUI 入口钩也调 ——
 * 那批一次性侧栏布局写入只发生一次, 必须在那之前就纠正好。
 * ★ InitGUI 那条路**永远**按"侧栏存在"的几何纠正, 不看当前模式。 */
void FM_SyncSidebarColumn(void);

/* 侧栏列此刻是否**整条**呈现 (态0, 或本模块没接管)。态1 只留顶部一小块、态2 完全
 * 不呈现 —— 那两种情况下侧栏列的屏幕区域归**地图**所有: 画在侧栏面上的东西不会上屏,
 * 而落在那里的点击是**地图点击**(框选/打靶), 绝不能被当成侧栏交互吞掉。返回 1/0。 */
int  FM_SidebarVisible(void);

/* **地图区的右边界** (= ViewBounds.X + ViewBounds.Width): 侧栏显示时 1752,
 * 隐藏时 1920。MOE 那些贴右下角画的层 (超武侧栏 SWSBar、超武倒计时列表
 * SWPanel) 必须锚到这里, 否则侧栏一显示就被压在侧栏底下 / 一隐藏就偏出屏幕
 * 右缘。返回 0 = 本模块没接管, 调用方退回用面宽。 */
int  FM_MapRightEdge(void);