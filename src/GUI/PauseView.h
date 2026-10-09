/* PauseView.h - 单机暂停 + 单步帧进 (热键 + 按钮)。
 * MainLoop 入口门 (0x55D360), 暂停跑迷你循环后跳 0x55DEDB 出本轮。
 * 只读 + 渲染面写入, 仅单机生效。PauseEnable 只定按钮初始显示。
 */
#pragma once

/* 读 [BottomBar] (幂等)。 */
void PV_EnsureInit(void);

/* 每循环调: F5 边沿 + 两按钮点击。 */
void PV_Poll(void);

/* 底栏两按钮画进 Composite (刻度条左侧 II/▶小方块)。返回 1 = 画过。 */
int PV_DrawToComposite(void);
