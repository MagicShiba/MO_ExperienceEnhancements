/* SmartArea.h - Shift+中键范围智能下令 (实现见 SmartArea.cpp)。 */
#pragma once

/* 组合键当前是否认领中键 (SmartOrder 仲裁用: 认领时裸中键让路;
 * 关则不认领, 组合中键回落裸中键行为, 与 ini 注释一致)。 */
int SmartArea_ClaimsMiddle(void);

/* 当前组合键是否仅为物理 Shift+中键 (无附加键/其它修饰)。
 * DeployPreview 的 Shift 锁轴排直线要用同一组合, 有可展开单位时优先。 */
int SmartArea_ShiftMiddleOnly(void);

void SmartArea_Poll(void);

void SmartArea_Draw(void* surf, int w, int h, int pitch);
