/* Icon.h - 提示栏自定义图标 (建造提示第三行的 ▲▼○ 改走 PNG 位图)。
 *
 * 做法: ToolTip.cpp 拼串时用 PUA 码 (U+E000~E002, 与正常文本零碰撞),
 * VecText 的 Blit 入口与 GetDim 度量见到 PUA 即走此处, 不走 GDI/原版点阵
 * (雅黑缺 ⚡⌚ 等符号字形, 文本方式必 tofu; 见 ToolTip.cpp 文件头)。
 * 位图来自 icons/icon_*.png (黑=墨, 白=忽略, 与底部闪电点阵同约定),
 * 由 icons/mkicons.py 在改图后手动转成 icons/icons.inc 编进 DLL, 无外部文件。
 * 步进统一 (=归一化宽度), 天然对齐; 高度≤32, 行内垂直居中。
 * 实现见 Icon.cpp。构建见 build-fast.bat。 */

#pragma once

/* PUA 码 (ToolTip 拼串用, 与 Icon 槽位一一对应) */
#define ICON_PWR_UP 0xE000u
#define ICON_PWR_DOWN 0xE001u
#define ICON_TIME 0xE002u
#define ICON_COST 0xE003u

/* wc 是否图标码 (是则槽位有效) */
int Icon_Is(unsigned wc);
/* 图标步进像素 (全图标统一宽度; 未知回 0) */
int Icon_Advance(unsigned wc);
/* 图标位图高度像素 (未知回 0, 行步进计算用) */
int Icon_Height(unsigned wc);
/* 行高: BitFont+0x1C (DrawText 换行步进, 提示钩临时抬高后即新行高),
 * 非法回退 19 (只读, 不调引擎) */
int Icon_LineH(void* t);
/* 画图标: 墨色取 colorW (16 位显示色), 覆盖率=255-亮度, 行内居中,
 * 按 Tp/B 与面界裁剪。0=画出, <0=调用方走原版。 */
int Icon_Blit(void* t, unsigned wc, int x, int y, int colorW, int h);
