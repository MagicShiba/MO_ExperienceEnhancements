/* RallyPath.h - 兵营/重工聚集点多段路径 (Z 折线, 实现见 RallyPath.cpp)。
 * 每循环轮询 (挂 PowerDrawBoth, 见 MOExperience.cpp): 装两处钩 + 清表 + 追单排空。
 * 折线绘制挂 MOE_DrawUILayers (只读 + 渲染面写入, 联机安全)。 */
#pragma once

void RallyPath_Poll(void);
void RallyPath_Draw(void* surf, int w, int h, int pitch);