/* SWSBar.h - 超武侧栏每循环轮询 + 渲染期绘制 (实现见 SWSBar.cpp)。 */

#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp):
 * 切换键边沿 + 超武扫描 (只读, 联机安全)。开关关/隐藏时直接返回。 */
void SWS_Poll(void);

/* 渲染期绘制 (挂 MOExperience_MessageDrawHook, Composite 面):
 * 右侧 4 列图标 (盟/苏/厄/焚风, 空列隐藏)。只读 + 自有绘制, 联机安全。 */
void SWS_Draw(void* surf, int w, int h, int pitch);

/* 点击吞没门 (0x692419, 7B, 见 dist/MOExperience.dll.inj):
 * 鼠标落在侧栏区内时跳往 0x6925FC (Phobos 同式), 否则返回 0 跑原版。 */
extern "C" __declspec(dllexport) unsigned __cdecl SWS_ClickGate(void* regs);

/* 合并规则快照 (0x6CEA20, 6B, 见 dist/MOExperience.dll.inj):
 * 挂在 SuperWeaponTypeClass::LoadFromINI 入口, 取 this(ECX) 与栈上的 pINI,
 * 当场读 SidebarPCX / SW.ShowCameo / SW.ManualFire / SW.UseAITargeting /
 * Action / Type / UseChargeDrain 存进自有表。地图/战役内嵌 ini 新增或改写的
 * 超武只有这一条路读得到 (CCINIClass::INI_Rules 只有基础规则那份)。
 * **不留 ini 指针** (地图那份是栈上临时对象), 恒返回 0 = 跑原指令。 */
extern "C" __declspec(dllexport) unsigned __cdecl SWS_SwTypeIni(void* regs);

/* 滚轮接管 (MOExperience_WheelHook @0x777998 里调用):
 * 鼠标落在侧栏区内时接管上/下滚动, 通吃 s_Cols 表; 状态在最近一次 SWS_Draw 里
 * 发布, 无需开关时自问向上随滚动条回卷。 */
extern "C" __declspec(dllexport) int __cdecl SWS_WheelIn(int mx, int my, int dir);
extern "C" __declspec(dllexport) int __cdecl SWS_OverBar(int mx, int my);
