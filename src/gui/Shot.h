/* Shot.h - 视口原图截图 (底栏相机按钮)。
 * 截"未缩放的视口原图"且不含任何 UI (原生截图 0x537BC0 截的是 Primary 整屏)。
 * 默认不显示: [BottomBar] ShotEnable=1 或 ESC 菜单 GUI 页开关才出现,
 * 位置紧贴缩放刻度条左侧 (PauseView 三键整体左移一格让位)。
 * 纯读渲染面 + 写本地文件, 联机安全。
 */
#pragma once

/* 读 [BottomBar] (幂等)。 */
void Shot_EnsureInit(void);

/* 按钮显示开关 (= ESC 菜单 GUI 页那一行, 默认 0 = 不显示)。 */
int Shot_Visible(void);
void Shot_SetVisible(int enable);

/* 相机键边长; 0 = 不显示 (PauseView 据此决定三键是否左移)。 */
int Shot_ButtonWidth(void);

/* 每循环 (prepare 钩) 调: 按钮悬停 + 点击 (点击只排队, 不在这里截)。 */
void Shot_Poll(void);

/* 消息帧钩里、SB_RenderSwapOut 之后立刻调: 做掉排队的一次截图。
 * 此刻三种档位的原图都齐了, 且还没有任何 UI 被画上去。 */
void Shot_CapturePending(void);

/* 相机按钮画进 Composite (须在刻度条与 PauseView 三键之后画)。 */
int Shot_DrawToComposite(void);
