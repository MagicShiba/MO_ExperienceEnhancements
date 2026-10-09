/* SideScroll.h - 原版建造/生产侧栏的竖直滚动条 (滑块可拖, 点轨道跳转)。
 * 实现见 SideScroll.cpp; 引擎地址/偏移与推导见 notes/侧栏滚动条.md。
 * 构建见 build-fast.bat。
 *
 * 位置在 cameo 网格右侧的空档 (网格占 colX+22..colX+145, 列宽 168), 不遮图标,
 * 与原版底部那两个翻页按钮并存。内容放得下时不画, 且只在鼠标停进侧栏时绘制。
 * 只控制每个页签自己的 TopRowIndex (本地 UI 状态, 不发事件, 联机安全)。 */
#pragma once

/* 每循环 (prepare 钩 0x4F42F0) 调: 帧门 + 拖动/跳转。 */
void SS_Poll(void);

/* 提示盒记录 (ToolTip_Clamp 每次提示绘制都调): 面+盒+帧。cameo 提示画在侧栏面里
 * 且早于 Blit, 本条画在 Blit 钩里, 不记就会盖住提示字。 */
void SS_NoteTip(void* surf, int x, int y, int w, int h);

/* 侧栏面上屏前调 (BlitSidebar 0x6A70E0 钩): 画滑块。侧栏面是靠 BlitSidebar 带
 * 上屏的, 画在呈现之前 (同 notes/隐藏侧栏.md §4.5)。返回 1 = 画了。 */
int  SS_Draw(void);