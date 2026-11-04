/* LightingPreset.h - 游戏内光照预设 (临时设置, 不落盘)。
 *
 * 只改客户端渲染用的 scenario 光照字段 (地图 ini [Lighting] 段读进来的那几处),
 * 不碰任何模拟状态, 不广播, 不写 ini。 */
#ifndef MOE_LIGHTINGPRESET_H
#define MOE_LIGHTINGPRESET_H

/* 参数顺序: 环境 / 等级 / 地面 / 红 / 绿 / 蓝 (与菜单两行参数格同序)。
 * 单位 = 引擎字段单位: 环境与 R/G/B 是 ini 值 ×100 (75 表示 0.75),
 * 等级/地面是 ini 值 ×1000 (32 表示 0.032) —— 这两项只有百分之几的量级,
 * 按 ×100 会丢到个位数。等级可以是负数。 */
#define LIGHT_PARAM_N       6
#define LIGHT_PARAM_MAX     999

enum {
    LIGHT_P_AMBIENT = 0,
    LIGHT_P_LEVEL,
    LIGHT_P_GROUND,
    LIGHT_P_RED,
    LIGHT_P_GREEN,
    LIGHT_P_BLUE
};

/* 预设号: 0 = 用地图自带的光照, 1..5 = 内置预设, -1 = 手动调过。
 * LIGHT_PRESET_COUNT = 6 = 预设号上界+1 (原图 + 标准/早上/下午/黄昏/晚上)。 */
#define LIGHT_PRESET_ORIGINAL  0
#define LIGHT_PRESET_CUSTOM   (-1)
#define LIGHT_PRESET_COUNT     6

/* 调色比例 (等于"用哪一种光照"):
 *   0   = 默认光照: 逐格重建地形 (每格保留点光源色), 单位/建筑不染色;
 *   100 = 触发光照: 引擎触发器那条整池覆写 (点光源色只剩亮度, 单位/建筑一起染);
 *   中间 = 逐格重建 + 按比例给单位/建筑/玩家色上色。
 * 详见 notes/光照预设.md §5 "调色比例"。 */
#define LIGHT_MIX_MAX          100

/* 每循环调用: 场景重开 (新开/读档) 后按引擎现值重算一遍光照。 */
void Lighting_Poll(void);

/* 当前预设号 (LIGHT_PRESET_*)。 */
int  Lighting_Preset(void);
/* 套用预设; LIGHT_PRESET_ORIGINAL = 恢复地图自带的光照。 */
void Lighting_ApplyPreset(int preset);

/* 参数读写 (idx: LIGHT_P_*)。 */
int  Lighting_GetParam(int idx);
void Lighting_SetParam(int idx, int value);

/* 调色比例读写 (0..LIGHT_MIX_MAX, 含义见上)。长期设置: 落盘
 * [Environment] LightMix, 越局保留。 */
int  Lighting_Mix(void);
void Lighting_SetMix(int mix);

/* ESC 菜单"取消"用: 记住 / 还原打开菜单时的光照。 */
void Lighting_SaveState(void);
void Lighting_RestoreState(void);

#endif /* MOE_LIGHTINGPRESET_H */
