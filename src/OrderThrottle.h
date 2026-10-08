/* OrderThrottle.h - 大量单位下令分帧派发 (打破 OutList 一帧 128 条上限)。
 * 实现见 OrderThrottle.cpp。构建见 build-fast.bat (同一 DLL)。
 *
 * 钩子铁律: 本模块是 .text 直接补丁 (不进 Syringe .inj), 被偷的 10 字节
 * (`sub esp,0x88` + 四个 push) 里没有任何直接相对跳转, 跳板原样重放安全。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp):
 * 懒装 ClickedMission 钩 + 按 OutList 水位补发排队指令 + 换图清队列。 */
void OrderThrottle_Poll(void);