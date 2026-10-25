/* Icon - 提示栏自定义图标。
 *
 * ToolTip 用 PUA 码 (U+E000~E003) 占位，VecText 见到后按 PCX 覆盖率实画。
 * 位图源 `icons/icon_*.pcx`，构建时嵌入 DLL；首次使用时缓存索引。 */

#pragma once

/* PUA 码，与 Icon 槽位一一对应 */
#define ICON_PWR_UP 0xE000u
#define ICON_PWR_DOWN 0xE001u
#define ICON_TIME 0xE002u
#define ICON_COST 0xE003u

/* 首次 UI 绘制时预取全部图标。 */
void Icon_Preload(void);
/* wc 是否图标码 */
int Icon_Is(unsigned wc);
/* 图标步进像素（=PCX 宽度，失败回 0） */
int Icon_Advance(unsigned wc);
/* 图标位图高度（失败回 0） */
int Icon_Height(unsigned wc);
/* 行高：BitFont+0x1C，非法回退 19 */
int Icon_LineH(void* t);
/* 画图标：colorW 为 16 位显示色，按 Tp/B 与面界裁剪。
 * 0=画出，<0=加载失败/参数非法。 */
int Icon_Blit(void* t, unsigned wc, int x, int y, int colorW, int h);
