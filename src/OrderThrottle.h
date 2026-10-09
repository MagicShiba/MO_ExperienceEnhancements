/* OrderThrottle.h - 大量单位下令分帧派发 (原版 OutList 一帧 128 条, 多的静默丢)。
 * 实现见 OrderThrottle.cpp; 引擎事实与推导见 notes/选中与下令上限.md。
 * 构建见 build-fast.bat。
 *
 * 钩子是 .text 直接补丁 (不进 Syringe .inj), 幂等且冲突时自动放弃。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp): 懒装 ClickedMission 钩 +
 * 按 OutList 水位补发排队指令 + 换图清队列。 */
void OrderThrottle_Poll(void);