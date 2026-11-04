/* PatrolCircle.h - Z+中键一键巡逻圈 (实现见 PatrolCircle.cpp)。 */
#pragma once

void PatrolCircle_Poll(void);
/* 设置页改完 [PatrolCircle] Hotkey 后回读 (ini 已写盘; 幂等)。 */
void PatrolCircle_ReloadHotkey(void);
/* 渲染期橡皮线 (MOExperience_MessageDrawHook 内调用, armed 时才画)。 */
void PatrolCircle_Draw(void* surf, int w, int h, int pitch);
