/* PowerDisplay.h - 底部电量指示 (侧栏面), 见 PowerDisplay.cpp。
 * 配置键仍在 [VecText] 里 (PowerBottom/PowerX/PowerPad/PowerMode, 挨着
 * CreditPx/CreditDY), 字体/字号/粗体与资金同源, 见 VecText.h。
 */
#pragma once

void PowerDisplay_EnsureInit(void);
/* 只画侧栏面, 随条带上屏 (层级天然在鼠标之下, 侧栏不呈现即不显示)。
 * 返回 1=画出, 0=关闭/跳过, -1=GDI失败, -2=Lock失败。 */
int PowerDisplay_DrawSb(void);

/* ESC 设置页运行期读写 (只改内存; 键仍属 [VecText])。
 * 模式: 0=负载/电力, 1=余量; 显示开关就是 Bottom。 */
enum {
    PWO_ENABLE = 0,   /* PowerBottom */
    PWO_MODE,         /* PowerMode */
    PWO_X,            /* PowerX (0=侧栏内居中) */
    PWO_PAD           /* PowerPad 底边距 */
};
int  PowerDisplay_GetOption(int id);
void PowerDisplay_SetOption(int id, int value);
void PowerDisplay_SaveOptions(void);
void PowerDisplay_RestoreOptions(void);
/* 换字体后必须调: 缓存的覆盖位图是按旧字面生成的, 键里查不出字面变化。 */
void PowerDisplay_Invalidate(void);
