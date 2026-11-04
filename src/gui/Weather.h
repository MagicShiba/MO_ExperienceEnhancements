/* Weather.h - 全屏天气覆盖层 (图像 / 粒子两种画法), 实现见 Weather.cpp。
 *
 * 图像档: art.ini 的 RAIN/SNOW.SHP 整屏帧, 引擎 blit 平铺进视图区。
 * 粒子档: 自绘雨丝/雪花/火星, 逐帧推进, 连续平滑 (三种天气共用一套粒子, 只是参数
 * 不同)。火雨只有粒子形态, 与画法无关。云层是独立的一层 (云图压暗 = 云影,
 * 同一张图混向雾色 = 雾气, 雾色跟雨雪一样受环境光), 与降水无关。
 *
 * 雨、雪各自独立开关 (同时开就是雨夹雪), 只在本次运行有效; 云层模式与降水
 * 画法是长期设置, 落盘到 [Environment] 的 CloudMode / DrawMode。
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

/* 雷电 (闪电 + 打雷): 独立开关, 不下雨也能打; 与画法无关, 只在本次运行有效
 * (不落盘)。落点可以在视图外, 声像跟着落点走。雨声 (RainLoop) 跟着"下雨"
 * 本身走, 不受这个开关影响。 */
int  Weather_Thunder(void);
void Weather_SetThunder(int on);

/* 云层模式: 关 / 云影 (压暗) / 雾气 (混向雾色)。一档就是一个值, 独立于雨/雪,
 * 长期生效 (菜单里改会落盘 [Environment] CloudMode)。 */
#define WEATHER_CLOUD_OFF     0
#define WEATHER_CLOUD_SHADOW  1
#define WEATHER_CLOUD_FOG     2
#define WEATHER_CLOUD_MODE_N  3
int  Weather_CloudMode(void);
void Weather_SetCloudMode(int mode);

/* 云层风格预设 (cumulus / big / small / sheet / overcast / patchy / streak /
 * cirrus)。每种是一套独立的噪声语汇 (各自的置换表 + y 轴压缩比), 不是同一张图
 * 调浓淡。菜单里选会落盘到 [Environment] CloudStyle。 */
#define WEATHER_CLOUD_STYLE_N  8      /* 与 Weather.cpp 的 kCldStyle 行数一致 */
int  Weather_CloudStyle(void);
int  Weather_CloudStyleCount(void);
void Weather_SetCloudStyle(int idx);

/* 每帧绘制 (AfterMap 钩调, 三趟地图画完之后、UI 与缩放呈现之前):
 * 画进本帧"未缩放视口原图面" (SB_RawView 同源) —— 天气是战场画面的一部分,
 * 跟着原图走缩放呈现, 也因此无UI截图与存档缩略图天然包含天气。
 * 返回 1=画了。 */
int Weather_Draw(void);
