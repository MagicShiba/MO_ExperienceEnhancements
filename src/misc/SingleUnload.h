/* SingleUnload.h - 只卸一个载员: 左下信息面板点载员图标 → 该载员下车
 * (实现与引擎事实见 SingleUnload.cpp, 界面入口在 UnitPanel.cpp)。 */
#pragma once

/* unit = 载具, pax = 面板上被点的那个载员。
 * 回 1 = 已发出单卸事件 (调用方不要再走选中逻辑); 0 = 不归我们管。 */
int SU_ClickPassenger(void* unit, void* pax);
