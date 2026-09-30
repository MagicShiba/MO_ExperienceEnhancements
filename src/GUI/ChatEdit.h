/* ChatEdit.h - 自定义聊天输入框 (实现见 ChatEdit.cpp)。配置节 [ChatEdit]。 */
#pragma once

#include <windows.h>

void ChatEdit_Install(void);
/* 每帧调用 (MOExperience 消息帧尾部): 0=未激活, 1=激活中 (绘制在 DrawTail 做) */
int ChatEdit_Frame(void* surf, int bitLineH, int screenW,
                   int surfW, int surfH, int surfPitch);
/* 发送指纹命中? (MsgText_AddMessageHook 侧判"自己的话", 单机回声无名就靠它) */
int ChatEdit_MatchSent(const wchar_t* msg);
