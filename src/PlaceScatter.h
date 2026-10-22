/* PlaceScatter.h - 侧栏放建筑时驱散地基上的自家单位 (实现见 PlaceScatter.cpp)。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp)。
 * 纯只读查询 + OutList 标准 Move 事件入队, 不写模拟内存。 */
void PlaceScatter_Poll(void);

/* GUI 设置菜单用的运行时模式/查询 ([PlaceScatter] Mode)。
 * 0=关闭, 1=放下时驱离, 2=自动驱离。 */
int PS_Mode(void);
void PS_SetMode(int mode);
