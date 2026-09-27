/* FuzzyAttack.h - Ctrl+中键模糊下令 (实现见 FuzzyAttack.cpp)。 */
#pragma once

void FuzzyAttack_Poll(void);
/* 渲染期范围框 (MsgFont_MessageDrawHook 内调用, 按住才画)。 */
void FuzzyAttack_Draw(void* surf, int w, int h, int pitch);
