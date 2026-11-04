/* EnvIni.h - 环境显示相关配置的 ini 节名与键名 (唯一来源)。
 *
 * 设置页"环境显示"组的落盘项全部集中在 [Environment] 一节里: 光照修复总开关、
 * 调色比例、矿石光照、天气画法、云层模式、云影风格, 以及天气的表现参数
 * (雨/雪/火雨/烟/云影的调参)。这些功能分属四个模块 (LightConvFix / LightingPreset /
 * OreLight / Weather), 但配置只有一节 —— 节名与键名只在这里定义一次, 别的
 * 模块不许再写本地副本 (分叉过一次, 参见 notes/MEMORY.md 里 OFF_HOUSE_MONEY)。
 *
 * 键名一律带模块前缀: 合成一节后同名键会互相覆盖 ("Enable" 就有两处)。
 */
#pragma once

/* ini 节名 */
#define ENV_SECTION       "Environment"

/* 光照修复 (src/misc/LightConvFix.cpp): 改光照后逐格重建 LightConvert + 读档后补刷 */
#define ENV_KEY_LIGHTFIX  "LightFixEnable"

/* 光照预设 (src/gui/LightingPreset.cpp): 调色比例 (0..100) */
#define ENV_KEY_LIGHTMIX  "LightMix"

/* 矿石光照 (src/gui/OreLight.cpp): 总开关 + 受环境光照影响的百分比 */
#define ENV_KEY_OREENABLE "OreEnable"
#define ENV_KEY_OREFACTOR "OreFactor"

/* 天气 (src/gui/Weather.cpp): 长期设置三项 (其余调参键留在 Weather.cpp 内) */
#define ENV_KEY_DRAWMODE  "DrawMode"
#define ENV_KEY_CLOUDMODE "CloudMode"
#define ENV_KEY_CLOUDSTYLE "CloudStyle"
