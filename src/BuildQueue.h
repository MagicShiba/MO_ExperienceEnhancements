/* BuildQueue.h - 建筑本地排队每帧轮询 (实现见 BuildQueue.cpp)。 */
#pragma once

void BuildQueue_Poll(void);

/* GUI 设置菜单用的运行时开关/查询 ([Features] BuildQueue)。
 * 开启时会一次性写入"就绪"左对齐渲染桩 (幂等)。 */
int BQ_Enabled(void);
void BQ_SetEnabled(int enable);
