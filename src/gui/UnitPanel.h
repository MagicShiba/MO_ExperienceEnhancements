/* UnitPanel.h - 画面左下角的选中信息面板 + 左侧竖排按钮列。
 *
 * 版式: 一块九宫格外框 (border_mental_n9.pcx, 边距 9px), 里面左侧竖排 5 个按钮,
 * 右侧 3x3 个 60x48 的格子放选中单位。
 *   - 单选: 左格图标 (等级 / 弹药), 右格血量 (当前/总计) + 经验。
 *   - 敌方单位: 只画图标 + 血量。
 *   - 同型多选: 每单位一格, 图标左下角画弹药; 空弹优先排在前面。
 *   - 混型: <=9 个单位全展开; >9 按类型分组, 右上角画数量。
 *   - 超过 3 页: 第 27 格换成"更多"。
 * 点击格子 = 选中那个单位 (组则选中整组)。左键点击被点击门吞掉, 不会落到地图上。
 *
 * 绘制落点: Composite (与刻度条同层), 每帧由消息帧钩调用。
 * 只读模拟状态 + 只改本机选中集, 联机安全。
 */
#pragma once

/* 读 [UnitPanel] 配置 (幂等)。 */
void UP_EnsureInit(void);

/* 每循环 (prepare 钩): 快照选中集 / 摆位 / 取点击 / 悬停。 */
void UP_Poll(void);

/* 画进 Composite。返回 1 = 画过。 */
int UP_Draw(void);

/* 滚轮落在面板上则翻页 (dir: 1=下, -1=上)。返回 1 = 已吃掉。 */
int UP_WheelIn(int mx, int my, int dir);

/* 鼠标是否落在面板 (含折叠态的那个按钮) 上。 */
int UP_Over(int mx, int my);

/* 运行时开关 (调试/以后接 ESC 菜单)。 */
void UP_SetEnabled(int enable);
int UP_Enabled(void);

/* 点击门 (Syringe 0x692361, 6B): 落在面板上就吞掉这次地图点击。
 * 只记下待处理点击 (真正的动在下一次 UP_Poll), 门里不做引擎调用。 */
void UP_NoteClick(int mx, int my, int left, int right);
