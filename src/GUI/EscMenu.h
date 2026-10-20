/* EscMenu.h - custom ESC-menu page. See notes/ESC菜单.md. */
#pragma once

/* Reads [EscMenu] Enable; called from MOE_EnsureInit. */
void EscMenu_EnsureInit(void);

/* 语言运行期切换后重载菜单文案 (缺失提示同步切换)。 */
void EscMenu_ReloadLang(void);
