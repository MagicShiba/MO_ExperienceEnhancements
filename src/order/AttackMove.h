/* AttackMove.h - custom attack-move hotkey (see AttackMove.cpp). */
#pragma once

void AttackMove_Poll(void);
/* 设置页改完 [Features] AttackMove 后回读 (ini 已写盘; 幂等)。 */
void AttackMove_ReloadHotkey(void);
