/* MegaListCap.h - MegaMissionList 256 上限提到 4096 (实现见 MegaListCap.cpp)。
 * 单机多单位一次下令时, 执行侧每帧只从 DoList 搬 256 条进 MegaMissionList,
 * 满即丢弃。本模块把 Array/Timings 搬到堆上 4096 份, 38 处同长补丁。
 * 只在单机装; 联机走不到 256 以上, 行为与原版一致。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp): 懒装 17 处补丁。 */
void MlCap_Poll(void);
