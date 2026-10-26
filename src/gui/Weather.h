/* Weather.h - 全屏天气覆盖层 (图像 / 粒子两种画法), 实现见 Weather.cpp。
 *
 * 图像档: art.ini 的 RAIN/SNOW.SHP 整屏帧, 引擎 blit 平铺进视图区。
 * 粒子档: 自绘雨丝/雪花/火星, 逐帧推进, 连续平滑 (三种天气共用一套粒子, 只是参数
 * 不同)。火雨只有粒子形态, 与画法无关。云影是独立的一层 (云图压暗), 与降水无关。
 *
 * 雨、雪各自独立开关 (同时开就是雨夹雪), 只在本次运行有效;
 * **云影开关**与**画法**是长期设置, 落盘到 [Weather] 的 CloudEnable / DrawMode。
 */
#pragma once

/* 画法 */
#define WEATHER_DRAW_IMAGE     0
#define WEATHER_DRAW_PARTICLE  1
#define WEATHER_DRAW_N         2      /* 菜单档位数 */

int  Weather_DrawMode(void);
void Weather_SetDrawMode(int mode);

/* 降水开关 (1=开)。 */
int  Weather_Rain(void);
void Weather_SetRain(int on);
int  Weather_Snow(void);
void Weather_SetSnow(int on);
int  Weather_Fire(void);
void Weather_SetFire(int on);

/* 云影 (云图压暗)。独立于雨/雪, 可单独开; 长期生效 (菜单里改会落盘)。 */
int  Weather_Cloud(void);
void Weather_SetCloud(int on);

/* 云影风格预设 (cumulus / big / small / sheet / overcast / patchy / streak)。
 * 每种是一套独立的噪声语汇 (各自的置换表 + y 轴压缩比), 不是同一张图调浓淡。
 * 菜单里选会落盘到 [Weather] CloudStyle。 */
#define WEATHER_CLOUD_STYLE_N  7      /* 与 Weather.cpp 的 kCldStyle 行数一致 */
int  Weather_CloudStyle(void);
int  Weather_CloudStyleCount(void);
void Weather_SetCloudStyle(int idx);

/* 每帧绘制: 只画视图区 (不碰侧栏/底栏)。返回 1=画了。 */
int Weather_Draw(void* surf, int surfW, int surfH, int surfPitch);
