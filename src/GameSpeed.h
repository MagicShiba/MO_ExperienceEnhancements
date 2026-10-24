/* GameSpeed.h - 自定义游戏速度 (5~120 FPS, 121 = 原版不限速)。说明见 GameSpeed.cpp。 */
#pragma once

/* 取值域: 5..120 是目标 FPS, 121 是 MAX(不限速, 显示成 MAX)。
 * 拖动条按 ESC_SPEED_STEP 吸附; 手输数值不受此限。 */
#define ESC_SPEED_MIN   5
#define ESC_SPEED_MAX   120
#define ESC_SPEED_TOP   121
#define ESC_SPEED_STEP  5

void GameSpeed_EnsureInit(void);
int  GameSpeed_Enabled(void);
void GameSpeed_SetEnabled(int on);

int  GameSpeed_GetFps(void);
void GameSpeed_SetFps(int fps);

/* 每帧一次 (PV_MainGate 调): 同步原版速度页 + >60 的毫秒节拍。 */
void GameSpeed_Frame(void);

/* 只做"读 0xA8EB60 同步自己": 菜单打开时主循环停摆, Frame 不会被调, 绘制前补一次。 */
void GameSpeed_Poll(void);
