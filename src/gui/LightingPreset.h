/* LightingPreset.h - 游戏内光照预设 (临时设置, 不落盘)。
 *
 * 只改客户端渲染用的 scenario 光照字段 (地图 ini [Lighting] 段读进来的那几处),
 * 不碰任何模拟状态, 不广播, 不写 ini。取消菜单/换局后按地图原值重置。 */
#ifndef MOE_LIGHTINGPRESET_H
#define MOE_LIGHTINGPRESET_H

/* 参数个数与顺序: 环境光 / 红 / 绿 / 蓝。
 * 单位 = 地图 ini 的 ×100 (75 表示 0.75, 与引擎字段同单位)。 */
#define LIGHT_PARAM_N       4
#define LIGHT_PARAM_MAX     150

/* 预设号: 0 = 用地图自带的光照, 1..5 = 内置预设, -1 = 手动调过。
 * LIGHT_PRESET_COUNT = 6 = 预设号上界+1 (原图 + 标准/早上/下午/黄昏/晚上)。 */
#define LIGHT_PRESET_ORIGINAL  0
#define LIGHT_PRESET_CUSTOM   (-1)
#define LIGHT_PRESET_COUNT     6

/* 每循环调用: 认出场景重开 (新开/读档) 后把光照恢复成地图原值。 */
void Lighting_Poll(void);

/* 当前预设号 (LIGHT_PRESET_*)。 */
int  Lighting_Preset(void);
/* 套用预设; LIGHT_PRESET_ORIGINAL = 恢复地图自带的光照。 */
void Lighting_ApplyPreset(int preset);

/* 参数读写 (idx: 0=环境光 1=红 2=绿 3=蓝)。 */
int  Lighting_GetParam(int idx);
void Lighting_SetParam(int idx, int value);

/* ESC 菜单"取消"用: 记住 / 还原打开菜单时的光照。 */
void Lighting_SaveState(void);
void Lighting_RestoreState(void);

#endif /* MOE_LIGHTINGPRESET_H */
