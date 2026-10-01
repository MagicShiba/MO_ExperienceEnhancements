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
 * 调一次, 并且 DisplayClass::InitGUI 入口钩 (MOExperience_InitGuiEntry) 也调
 * —— 那一批一次性侧栏布局写入只发生一次, 必须在那之前就纠正好。 */
void FM_SyncSidebarColumn(void);

int  FM_IniOn(const char* key, int def);
