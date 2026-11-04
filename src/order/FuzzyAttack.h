/* FuzzyAttack.h - Ctrl+中键模糊下令 (实现见 FuzzyAttack.cpp)。 */
#pragma once

void FuzzyAttack_Poll(void);
/* 设置页改完 [FuzzyAttack] Hotkey 后回读 (ini 已写盘; 幂等)。 */
void FuzzyAttack_ReloadHotkey(void);
/* 渲染期范围框 (MOExperience_MessageDrawHook 内调用, 按住才画)。 */
void FuzzyAttack_Draw(void* surf, int w, int h, int pitch);
