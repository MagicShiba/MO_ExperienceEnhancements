/* TreeChop.h - Ctrl+Z+中键快速砍树 (实现见 TreeChop.cpp)。 */
#pragma once

void TreeChop_Poll(void);
/* 设置页改完 [TreeChop] Hotkey 后回读 (ini 已写盘; 幂等)。 */
void TreeChop_ReloadHotkey(void);
/* 渲染期范围框 (MOExperience_MessageDrawHook 内调用, 按住才画)。 */
void TreeChop_Draw(void* surf, int w, int h, int pitch);
