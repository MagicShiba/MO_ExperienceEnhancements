/* MindSelect.h - Shift+P 心控单位加入选中 (实现见 MindSelect.cpp)。 */
#pragma once

void MindSelect_Poll(void);
/* 设置页改完 [Features] MindSelect 后回读 (ini 已写盘; 幂等)。 */
void MindSelect_ReloadHotkey(void);
