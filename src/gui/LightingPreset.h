/* LightingPreset.h - 游戏内光照预设 (临时设置, 不落盘)。
 *
 * 只改客户端渲染用的 scenario 光照字段 (地图 ini [Lighting] 段读进来的那几处),
 * 不碰任何模拟状态, 不广播, 不写 ini。 */
#ifndef MOE_LIGHTINGPRESET_H
#define MOE_LIGHTINGPRESET_H

/* 参数顺序: 环境 / 等级 / 地面 / 红 / 绿 / 蓝 (与菜单两行参数格同序)。
 * 单位 = 引擎字段单位: 环境与 R/G/B 是 ini 值 ×100 (75 表示 0.75),
 * 等级/地面是 ini 值 ×1000 (32 表示 0.032) —— 这两项只有百分之几的量级,
 * 按 ×100 会丢到个位数。 */
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

/* 调色方式 (只决定"改完之后怎么上色", 不改任何字段值):
 *   PERCEL  = 逐格重建 convert, 每格自己算色 —— 与地图自带光照逐像素一致 (默认);
 *   UNIFORM = 全池统一改色, 与引擎触发器 SetTintRed 同一套 —— 整片一个颜色, 偏亮。
 * 详见 notes/光照预设.md 3.1 / 3.2。 */
#define LIGHT_MODE_PERCEL      0
#define LIGHT_MODE_UNIFORM     1
#define LIGHT_MODE_COUNT       2

/* 每循环调用: 认出场景重开 (新开/读档) 后把光照恢复成地图原值。 */
void Lighting_Poll(void);

/* 当前预设号 (LIGHT_PRESET_*)。 */
int  Lighting_Preset(void);
/* 套用预设; LIGHT_PRESET_ORIGINAL = 恢复地图自带的光照。 */
void Lighting_ApplyPreset(int preset);

/* 参数读写 (idx: LIGHT_P_*)。 */
int  Lighting_GetParam(int idx);
void Lighting_SetParam(int idx, int value);

/* 调色方式读写 (LIGHT_MODE_*); 改方式会立刻按当前字段重算一遍。 */
int  Lighting_Mode(void);
void Lighting_SetMode(int mode);

/* ESC 菜单"取消"用: 记住 / 还原打开菜单时的光照。 */
void Lighting_SaveState(void);
void Lighting_RestoreState(void);

#endif /* MOE_LIGHTINGPRESET_H */
