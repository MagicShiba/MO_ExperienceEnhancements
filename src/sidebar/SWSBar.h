/* SWSBar.h - 超武侧栏每循环轮询 + 渲染期绘制 (实现见 SWSBar.cpp)。 */

#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp):
 * 切换键边沿 + 超武扫描 (只读, 联机安全)。开关关/隐藏时直接返回。 */
void SWS_Poll(void);

/* 渲染期绘制 (挂 MOExperience_MessageDrawHook, Composite 面):
 * 右侧最多 4 列图标 (默认每阵营一列, 空列隐藏; FillMode=1 则不分阵营、平衡填充)。
 * 只读 + 自有绘制, 联机安全。 */
void SWS_Draw(void* surf, int w, int h, int pitch);

/* 点击吞没门 (0x692419, 7B, 见 dist/MOExperience.dll.inj):
 * 鼠标落在侧栏区内时跳往 0x6925FC (Phobos 同式), 否则返回 0 跑原版。 */
extern "C" __declspec(dllexport) unsigned __cdecl SWS_ClickGate(void* regs);

/* 滚轮接管 (MOExperience_WheelHook @0x777998 里调用):
 * 鼠标落在侧栏区内时接管上/下滚动, 通吃 s_Cols 表; 状态在最近一次 SWS_Draw 里
 * 发布, 无需开关时自问向上随滚动条回卷。 */
extern "C" __declspec(dllexport) int __cdecl SWS_WheelIn(int mx, int my, int dir);
extern "C" __declspec(dllexport) int __cdecl SWS_OverBar(int mx, int my);

/* 崩溃现场用: 数据地址归属 (侧栏自有缓冲 / 不是我们的)。
 * 只比区间不解引用, 可安全地在异常处理器里调。 */
extern "C" __declspec(dllexport) const char* __cdecl SWS_WhoOwns(unsigned addr);

/* 自动化回归用 (SWSTest.cpp): 动作号 2~5 = 收起/展开/关去重/开去重,
 * 与按钮/热键改的是同一批状态。0/1 (总开关) 直接用 SWS_SetEnabled。 */
void SWS_TestAction(int a);

/* GUI 设置菜单用的运行时开关/查询。 */
int SWS_Enabled(void);
void SWS_SetEnabled(int enable);
int SWS_FillMode(void);
void SWS_SetFillMode(int fill);
int SWS_MaxRows(void);
void SWS_SetMaxRows(int rows);
int SWS_SideRight(void);
void SWS_SetSideRight(int right);
