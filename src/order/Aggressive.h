/* Aggressive.h - 侵略模式 (实现在 Aggressive.cpp)。
 * Alt+A 切换选中单位的侵略标记, 已标记单位闲置时自动攻击警戒范围内的敌方
 * 建筑, 并在单位上方画 icon_aggressive 图标。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp):
 * 热键边沿切换标记 + 空闲扫描 + 分批发标准 Attack 事件。
 * 只读查询 + OutList 标准事件入队。 */
void Aggressive_Poll(void);

/* 渲染期画标记图标 (MOE_DrawUILayers 内调): 在每个已标记单位上方 blit。 */
void Aggressive_Draw(void* surf, int w, int h, int pitch);
