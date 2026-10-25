/* PowerDisplay.h - 底部电量指示 (侧栏面), 见 PowerDisplay.cpp。
 * 配置键仍在 [VecText] 里 (PowerBottom/PowerX/PowerPad/PowerMode, 挨着
 * CreditPx/CreditDY), 字体/字号/粗体与资金同源, 见 VecText.h。
 */
#pragma once

void PowerDisplay_EnsureInit(void);
/* 只画侧栏面, 随条带上屏 (层级天然在鼠标之下, 侧栏不呈现即不显示)。
 * 返回 1=画出, 0=关闭/跳过, -1=GDI失败, -2=Lock失败。 */
int PowerDisplay_DrawSb(void);
