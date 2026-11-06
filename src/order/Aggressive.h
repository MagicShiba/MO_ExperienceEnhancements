/* Alt+A 切换本地侵略标记; 空闲单位自动向范围内目标发标准事件。 */
#pragma once

/* 只读查询与标准事件入队, 联机不得直接改变模拟状态。 */
void Aggressive_Poll(void);
/* 设置页改完 [Aggressive] Hotkey 后回读 (ini 已写盘; 幂等)。 */
void Aggressive_ReloadHotkey(void);
/* Target: 1 = 建筑及作战单位, 0 = 仅建筑。 */
int  Aggressive_AllTargets(void);
void Aggressive_SetAllTargets(int all);

/* 渲染期画标记图标 (MOE_DrawUILayers 内调): 在每个已标记单位上方 blit。 */
void Aggressive_Draw(void* surf, int w, int h, int pitch);
