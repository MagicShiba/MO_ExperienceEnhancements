/* BeaconSP.h - 通信信标 (B 键放置) 单机启用 (实现见 BeaconSP.cpp)。
 * 单机专用: 联机不装钩, 已装的恢复原版字节 (实时判定, 不缓存)。 */
#pragma once

/* 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp): 按单机/联机懒装/卸钩。 */
void BeaconSP_Poll(void);
