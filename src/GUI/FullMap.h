/* FullMap.h - 隐藏原版建造/生产侧栏, 地图铺满整个窗口宽度, 小地图保留在原角。
 * 实现见 FullMap.cpp (地址全部来自 gamemd.exe 反汇编实证)。构建见 build-fast.bat。 */
#pragma once

/* 读 [FullMap] 节 (幂等)。 */
void FM_EnsureInit(void);

/* 装字节补丁 + CreateAll 两个调用点重定向: 让**渲染面永远全宽** (态0 要把视口收回
 * 1752, 面不能跟着缩)。DllMain 里调 (见 MOExperience.cpp)。 */
void FM_Install(void);

/* 每循环 (prepare 钩 0x4F42F0) 调: 采 F11 三态切换 (见下), 撑视口 / 纠正侧栏列 /
 * 摘侧栏 gadget。F11 三态 (ini [FullMap] ToggleVk, 默认 F11):
 *   0 完全显示  整条侧栏面上屏, gadget 可用, 点击让位引擎 —— 与原版一致
 *   1 只显示小地图和顶部两个按钮
 *   2 全屏      侧栏一点不显示, 整窗都是地图, 点击全给地图
 * 态0 会把视口宽度**收回** clientW-168 并恢复 0x72AD20 的原版字节 —— 只 blit 整条
 * 侧栏但视口仍全宽时, 页签/cameo/小地图一个都点不动 (凡是拿视口跟侧栏列做比较
 * 的地方全错 168)。详见 FullMap.cpp 的 Fm_ApplyViewWidth 与 notes/隐藏侧栏.md。 */
void FM_Poll(void);

/* 条带上屏钩 (SidebarClass::BlitSidebar 0x6A70E0) 调:
 * 只 blit 小地图那一小块, 补齐原版尾声的脏标记, 然后返回"直接 ret 4 的出口"
 * 指针 —— 调用方把 g_BlitSbTramp 指向它即等于跳过原版整条 blit。
 * 返回 0 = 不接管 (调用方继续用引擎跳板, 原版照跑)。 */
unsigned long FM_BlitSb(void);
/* 把小地图 blit 进指定面 (通常是 DSurface::Composite, 每帧呈现前的绘制点)。
 * 走这条路而不是靠 BlitSidebar: 地图一铺满就每帧盖掉右上角, 而侧栏上屏的时机并
 * 不保证跟着地图呈现, 挂在每帧必跑的呈现前绘制点上就没有闪烁。返回 1=画了。 */
int FM_DrawRadarTo(void* surf);

/* GadgetClass::Input (0x4E1640) 钩: 这是输入链上唯一能拿到真实 gadget 指针的地方
 * (this), 也是原函数调 ExtractGadgetAt 之前唯一的插手时机 —— 所以
 * FM_TrimViewGadget 挂在这一层。只在 gadget 横向跨过侧栏分界线时记下它。 */
void FM_NoteGadgetInput(void* gadget);

/* 把输入链里那个"横跨视口"的 gadget 的命中区收到"视口减去侧栏列"
 * (与原版几何一致)。它的矩形恒等于 ViewBounds; 地图铺满后它连侧栏列一起吞掉,
 * 那一列的点击全被判成"点在视口上"。指针见 FM_NoteGadgetInput。
 * 纯命中区状态, 联机安全。 */
void FM_TrimViewGadget(void);

/* DisplayClass::ProcessClickCoords (0x692300) 落点 0x692406 的点击门 (Syringe
 * 钩, 见 MOExperience.dll.inj): 点击落在屏幕上那块小地图矩形里时, 自己把视口
 * 对准那一格 (RadarClass::ScrollTo 0x653F70), 再返回 0x6925FC (原函数的
 * "未处理"出口) 把这次点击吞掉, 不让它穿透去框单位/打靶。
 * 返回 0 = 交回原版。纯本地摄像机, 联机安全。
 * 格范围由 FullMap.cpp 里的 Fm_ScanTacticalMapRect 只读扫 TacticalClass 得到;
 * 没扫到就只吞点击不跳转。 */
unsigned FM_RadarClickGate(void* regs);

/* 把 SidebarBounds.X 纠正成"侧栏列在屏幕上的起点"(视口右边界 - 列宽)。
 * 引擎自己算的是 viewRect.X+viewRect.W, 视口铺满后会差 168。FM_Poll 每循环
 * 调一次 (全屏态改推成"恒不成立"的鼠标路由阈值, 见 FullMap.cpp 的 Fm_SyncColumn),
 * 并且 DisplayClass::InitGUI 入口钩 (MOExperience_InitGuiEntry) 也调 ——
 * 那一批一次性侧栏布局写入只发生一次, 必须在那之前就纠正好。
 * ★ InitGUI 那条路**永远**按"侧栏存在"的几何纠正, 不看当前模式。 */
void FM_SyncSidebarColumn(void);

int  FM_IniOn(const char* key, int def);

/* **地图区的右边界** (= ViewBounds.X + ViewBounds.Width): 态0 侧栏显示时是 1752,
 * 态1/2 侧栏隐藏时是 1920。MOE 那些贴右下角画的层 (超武侧栏 SWSBar、超武倒计时
 * 列表 SWPanel) 必须锚到这里, 否则侧栏一显示就被压在侧栏底下 / 侧栏一隐藏就偏出
 * 屏幕右缘。返回 0 = 本模块没接管, 调用方退回用面宽。 */
int  FM_MapRightEdge(void);
