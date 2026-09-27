/* TreeChop.h - Ctrl+Z+中键快速砍树 (实现见 TreeChop.cpp)。 */
#pragma once

void TreeChop_Poll(void);
/* 渲染期范围框 (MsgFont_MessageDrawHook 内调用, 按住才画)。 */
void TreeChop_Draw(void* surf, int w, int h, int pitch);
