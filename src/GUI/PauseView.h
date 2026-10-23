/* PauseView.h - 单机暂停 + 单步帧进 (热键 + 按钮)。
 * MainLoop 入口门 (0x55D360), 暂停跑迷你循环后跳 0x55DEDB 出本轮。
 * 只读 + 渲染面写入, 仅单机生效。PauseEnable 只定按钮初始显示。
 */
#pragma once

/* 底栏小方块边长 (PauseView 三键与 Shot 相机键共用)。 */
#define PV_BS 22

/* 读 [BottomBar] (幂等)。 */
void PV_EnsureInit(void);

/* 每循环调: F5 边沿 + 两按钮点击。 */
void PV_Poll(void);

/* 暂停/步进按钮显示开关; 关闭时同时清除暂停态。 */
int PV_ButtonsEnabled(void);
void PV_SetButtonsEnabled(int enable);

/* 底栏按钮槽几何: 0=格网 1=暂停 2=步进 3=截图 (恒为最右, 紧贴刻度条;
 * 截图键未显示时该槽不存在)。返回 1 = 摆得下, (x,y) 是方块左上角;
 * 0 = 没开/没摆好 (不画也不响应点击)。截图键由 Shot 模块用槽 3 自绘。 */
int PV_SlotGeom(int slot, int* x, int* y);

/* 底栏三按钮画进 Composite (刻度条左侧 #/II/▶小方块, # 与暂停按钮同显同隐)。
 * 格网实现见 GridView.h (本文件只调 GV_ 接口), 格网开且缓存有效时压底画灰色格网。
 * 返回 1 = 画过。 */
int PV_DrawToComposite(void);
