/* ChatEdit.h - 自定义聊天输入框 (实现见 ChatEdit.cpp)。 */
#pragma once

void ChatEdit_Install(void);
/* 每帧调用 (MessageDrawHook 尾部): 0=未激活, 1=激活中 (绘制在 DrawTail 做) */
int ChatEdit_Frame(void* surf, int bitLineH, int screenW,
                   int surfW, int surfH, int surfPitch);
/* 发送指纹命中? (AddMessage 侧判"自己的话", 单机回声无名就靠它) */
int ChatEdit_MatchSent(const wchar_t* msg);
/* 聊天输入框锚点 (实现见 MOExperience.cpp): 贴战役框下沿, 1=有锚点 */
int ChatAnchor_CampBelow(int surfW, int surfH, int bitLineH, int* px, int* py);
