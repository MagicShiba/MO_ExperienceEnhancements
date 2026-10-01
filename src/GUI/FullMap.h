/* FullMap.h - 隐藏原版建造/生产侧栏, 地图铺满整个窗口宽度, 小地图保留在原角。
 * 实现见 FullMap.cpp (地址全部来自 gamemd.exe 反汇编实证)。构建见 构建.bat。 */
#pragma once

/* 读 [FullMap] 节 (幂等)。 */
void FM_EnsureInit(void);

/* 装 CreateAll (0x533FD0) 入口钩: 把 Composite/Hidden 的建面矩形加宽, 否则
 * 撑宽视口后地图会画到面外。DllMain 里调 (见 MOExperience.cpp)。 */
void FM_Install(void);

/* 每循环 (prepare 钩 0x4F42F0) 调: 按 [FullMap] 开关撑视口 / 摘侧栏 gadget。 */
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

/* 把输入链里那个"横跨视口"的 gadget 横向收到侧栏分界线。
 * 它的矩形恒等于 ViewBounds; 地图铺满后它越过侧栏分界线, 在
 * ExtractGadgetAt (0x4E15A0) 里把那一列的点击全吃成"点在视口上"。
 * 指针见 FM_NoteGadgetInput。纯命中区状态, 联机安全。 */
void FM_TrimViewGadget(void);
