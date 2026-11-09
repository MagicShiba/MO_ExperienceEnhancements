/* MCLoss.h - 心控单位损失提示开关 (实现与推导见 MCLoss.cpp)。 */
#pragma once

/* 0x65FA70 RadarEventClass::Create / 0x752480 VoxClass::PlayIndex 入口钩。
 * 回 0 = 跑原指令; 回函数尾 ret 4 = 吞掉这一次本地表现。 */
extern "C" __declspec(dllexport) unsigned __cdecl MCLoss_CreateHook(void* regs);
extern "C" __declspec(dllexport) unsigned __cdecl MCLoss_VoiceHook(void* regs);

/* 设置页运行期读写 ([Features] MCLossMute), 改完下一帧生效。 */
int  MCLoss_Enabled(void);
void MCLoss_SetEnabled(int on);
