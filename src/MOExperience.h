/* MOExperience.h - DLL 协调层 (见 MOExperience.cpp)。
 * 只负责把各功能模块挂到引擎的时序点上:
 *   每循环 prepare (0x4F42F0) 裸钩 -> PowerDrawBoth (所有 *_Poll)
 *   每帧  消息框 Draw (0x5D4A94)   -> MessageDrawHook (所有 *_Draw)
 *   每帧  滚轮 (0x777998) / 侧栏滚动 (0x533F50)
 *   DllMain -> 各模块 *_Install
 */
#pragma once

#define MOE_VERSION "0.2.162a"

/* 一次性初始化: 建日志路径 + 各模块首次载入配置。幂等, 各入口都先调它。 */
void MOE_EnsureInit(void);

/* 帧准备钩入口 (naked): 每循环必跑, 底部电量"始终绘制"靠它。 */
void MOExperience_PrepareEntry(void);
extern void* g_PrepareTramp;
/* 上屏入口钩入口 (naked): 每次条带呈现前先恢复底部电量。 */
void MOExperience_BlitSbEntry(void);
extern void* g_BlitSbTramp;
/* 呈现入口钩入口 (naked): 保留安装 (进出栈平衡), 当前不再绘制。 */
void MOExperience_UpdateEntry(void);
extern void* g_UpdateTramp;

/* 消息框 Draw 内部钩 (0x5D4A94, 7B): 各模块 *_Draw 的唯一出口 (链表非空时才跑)。
 * 依次调 MsgText / SWPanel / 超武侧栏 / 智能中键橡皮线 / 底部电量 /
 * 聊天输入框, 画完返回 0x5D4A9B (跳过原版绘制)。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_MessageDrawHook(void* regs);
/* 消息框 Draw 入口钩 (0x5D49A0, 6B): 每帧必跑; 引擎消息链表为空时
 * (0x5D4A92 的 je 会跳过 0x5D4A94) 代跑整条链路。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_MsgListEntry(void* regs);
/* 滚轮钩 (0x777998, 6B): 超武侧栏优先, 否则战役框历史。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_WheelHook(void* regs);
/* 侧栏滚动钩 (0x533F50, 5B): 悬停超武侧栏/战役框时跳过侧栏滚动。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_SidebarHook(void* regs);
