/* Alt+A 切换本地侵略标记; 空闲单位自动向范围内目标发标准事件。
 * 无敌目标 (Immune=yes / Armor=special) 与有价值的科技建筑默认不当目标。 */
#pragma once

/* 只读查询与标准事件入队, 联机不得直接改变模拟状态。 */
void Aggressive_Poll(void);
/* 设置页改完 [Aggressive] Hotkey 后回读 (ini 已写盘; 幂等)。 */
void Aggressive_ReloadHotkey(void);
/* Target: 1 = 建筑及作战单位, 0 = 仅建筑。 */
int  Aggressive_AllTargets(void);
void Aggressive_SetAllTargets(int all);
/* SkipTechBuildings: 1 = 有价值的科技建筑不当目标 (留着占领), 0 = 照打。 */
int  Aggressive_SkipTechBuildings(void);
void Aggressive_SetSkipTechBuildings(int on);

/* 渲染期画标记图标 (MOE_DrawUILayers 内调): 在每个已标记单位上方贴图标,
 * 只写战场视口内的像素 (底栏/侧栏不每帧重画, 写进去会留残影)。 */
void Aggressive_Draw(void* surf, int w, int h, int pitch);
