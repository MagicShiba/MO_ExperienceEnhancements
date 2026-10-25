/* WeaponRange.h - 单选按住 Ctrl 显示单位攻击范围 (实现见 WeaponRange.cpp)。 */
#pragma once

/* 渲染期范围圈 (MOExperience_MessageDrawHook 内调用, 单选+Ctrl 才画)。 */
void WeaponRange_Draw(void* surf, int w, int h, int pitch);
