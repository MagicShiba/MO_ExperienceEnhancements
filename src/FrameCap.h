/* FrameCap.h - ESC 菜单/设置页限帧 (固定 60)。说明见 FrameCap.cpp 顶部。 */
#pragma once

void FrameCap_EnsureInit(void);
int  FrameCap_Enabled(void);
void FrameCap_SetEnabled(int on);

/* 每帧限速点, 三处共用一次等待预算:
 *   MainLoop 入口 0x55D360 / 渲染尾 0x4F4589 / PeekMessageA 导入表钩 */
void FrameCap_Frame(void);

/* ESC 菜单对话框 (首页 + 各级子页): 非 0 且可见 = 菜单开着 = 限速。 */
void FrameCap_SetActiveDlg(void* hwnd);
void* FrameCap_ActiveDlg(void);
