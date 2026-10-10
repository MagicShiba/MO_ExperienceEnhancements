/* PlaceScatter.h - 侧栏放建筑时驱散地基上的自家单位 (实现见 PlaceScatter.cpp)。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp)。
 * 纯只读查询 + OutList 标准 Move 事件入队, 不写模拟内存。 */
void PlaceScatter_Poll(void);
