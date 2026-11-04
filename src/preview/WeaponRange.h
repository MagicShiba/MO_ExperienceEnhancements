/* WeaponRange.h - 单选按住 Ctrl 显示单位攻击范围 (实现见 WeaponRange.cpp)。 */
#pragma once

/* 渲染期范围圈 (MOExperience_MessageDrawHook 内调用, 单选+Ctrl 才画)。 */
void WeaponRange_Draw(void* surf, int w, int h, int pitch);

/* 设置页运行期读写:
 *   Enabled  落盘 [Features] WeaponRange (默认开), 关掉后下一帧就不再画;
 *   Height   落盘 [Features] WeaponRangeAAZ (0..8192), 对空圈抬高的 lepton 值。 */
int  WeaponRange_Enabled(void);
void WeaponRange_SetEnabled(int on);
int  WeaponRange_Height(void);
void WeaponRange_SetHeight(int v);
