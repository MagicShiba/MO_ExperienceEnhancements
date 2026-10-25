/* FrameCap.h - ESC 菜单/设置页限帧 (固定 60)。说明见 FrameCap.cpp 顶部。 */
#pragma once

void FrameCap_EnsureInit(void);
int  FrameCap_Enabled(void);
void FrameCap_SetEnabled(int on);

/* 主循环节拍点: 由主循环门 PV_MainGate (0x55D360) 每帧调一次。
 * 主循环还活着时只有它节拍; 模态循环里由队列空的 PeekMessage 节拍。 */
void FrameCap_Gate(void);

/* ESC 菜单对话框 (首页 + 各级子页): 非 0 且可见 = 菜单开着 = 限速。 */
void FrameCap_SetActiveDlg(void* hwnd);
void* FrameCap_ActiveDlg(void);
