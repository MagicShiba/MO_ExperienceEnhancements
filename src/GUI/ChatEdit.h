/* ChatEdit.h - 聊天输入框 (见 ChatEdit.cpp)。贴在战役框下沿, 与它同属一块 UI,
 * 配置并进 [MsgText]:
 *   ChatEdit     自绘输入框 (0=原生输入行照常显示)
 *   ChatBoxSP    单人模式也建原生聊天框 (Phobos AllowChatBoxInSinglePlayer 同原理)
 *   ChatNameSP   单人消息也显示 "名字: " 前缀 (联机恒显示)
 * 文字样式跟战役框 ([MsgText] FontFace/FontHeight/Bold), 不另设字体键。
 */
#pragma once

#include <windows.h>

void ChatEdit_Install(void);
/* 每帧调用 (MOExperience 消息帧尾部): 0=未激活, 1=激活中 (绘制在 DrawTail 做) */
int ChatEdit_Frame(void* surf, int bitLineH, int screenW,
                   int surfW, int surfH, int surfPitch);
/* 发送指纹命中? (MsgText_AddMessageHook 侧判"自己的话", 单机回声无名就靠它) */
int ChatEdit_MatchSent(const wchar_t* msg);
/* 单人消息是否显示 "名字: " 前缀 (MsgText 排版用) */
int ChatEdit_ShowNameSP(void);

/* 单人聊天框: 对话框函数按 [SessionClass 0xA8B238]==0/5 判单人并跳过聊天框
 * 四处逻辑。ChatBoxSP=1 时跳过 session 检查 (保留其余旗标检查),
 * 返回 0 走原版。 */
extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_ChatSp1(void* regs);
extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_ChatSp2(void* regs);
extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_ChatSp3(void* regs);
extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_ChatSp4(void* regs);