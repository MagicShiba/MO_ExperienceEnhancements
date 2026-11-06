/* 单机信标共享池: BeaconLimit=3..24, 3 保留每家三槽。
 * 仅 Campaign/Skirmish 且 BeaconInSP 开启时安装。 */
#ifndef BEACONCAP_H
#define BEACONCAP_H

void BeaconCap_Poll(void);

/* 当前布局的槽数: 池化时为 BeaconLimit, 否则为每家 3。 */
int BeaconCap_SlotCount(void);

#endif
