/* ToolTip.h - 建造列表提示追加耗时/耗电 ([Features] ToolTips), 见 ToolTip.cpp。
 * 钩子入口 ToolTip_Extend/Clamp/AdvFix 由 .inj 直接按名字挂, 不在这个头里。 */
#pragma once

/* 设置页开关 (落盘 [Features] ToolTips, 默认开)。运行期现读, 关掉下一帧就回原版。 */
int  ToolTip_Enabled(void);
void ToolTip_SetEnabled(int on);
