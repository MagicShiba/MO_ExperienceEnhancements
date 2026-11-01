/* OreLight.h - 矿石/宝石受环境光照影响系数, 实现见 OreLight.cpp。
 *
 * 矿石覆盖物的绘制把 Brightness 硬编码成 0x3E8 (满亮度), 夜里仍全亮。
 * 钩点把栈上的 Brightness 改写成
 *   1000 + (Intensity_Normal - 1000) × Factor/100;
 * Factor=0 保持原版 (矿不受光照), 100 完全遵循环境光 (与普通覆盖物一致)。
 * Factor 是长期设置, 落盘到 [Environment] 的 OreFactor。
 */
#pragma once

#define ORELIGHT_FACTOR_MAX  100          /* 菜单上限 */

/* 当前比例 (0..ORELIGHT_FACTOR_MAX)。 */
int  OreLight_Factor(void);
void OreLight_SetFactor(int factor);
