/* SmartArea.h - Shift+中键范围智能下令 (实现见 SmartArea.cpp)。 */
#pragma once

/* 组合键当前是否认领中键 (SmartOrder 仲裁用: 认领时裸中键让路;
 * 关则不认领, 组合中键回落裸中键行为, 与 ini 注释一致)。 */
int SmartArea_ClaimsMiddle(void);

void SmartArea_Poll(void);

void SmartArea_Draw(void* surf, int w, int h, int pitch);
