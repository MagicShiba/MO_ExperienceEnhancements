/* MsgText.h - 消息文本显示 (战役框 + 提示框), 见 MsgText.cpp。
 *
 * 接管引擎 MessageListClass 的消息队列, 自绘 GDI 矢量文本:
 *   战役框 (sp=0, 战役/聊天/系统): 长留历史, 悬停展开, 滚轮翻看。
 *   提示框 (sp=1, 快捷键/路径点): 按引擎超时自然消失。
 * 配置节 [MsgText]。
 *
 * 钩子 (Syringe, 见 dist/MOExperience.dll.inj):
 *   0x5D3BA0 AddMessage 捕获; 0x777998 滚轮翻历史;
 *   0x533F50 悬停时跳过侧栏滚动。
 * 渲染本身挂 0x5D4A94 (引擎链表非空时才跑) 与 0x5D49A0 兜底入口,
 * 两者都在 MOExperience.cpp 的协调层, 见 MOExperience_MsgListEntry。
 * 任何一步失败 (Lock 不上、GDI 未就绪) 都返回 0 走原版绘制, 绝不花屏。
 */
#pragma once

#define MSG_SECTION "MsgText"

/* 一次性初始化 (读配置 + 解析字体); 幂等, 各钩子入口都先调它。 */
void MsgText_EnsureInit(void);

/* 渲染期: 画绿框触发条 + 战役框 + 提示框。
 * 返回 1=画出, 0=无内容, -1=GDI失败, -2=Lock失败。 */
int MsgText_DrawFrame(void* surf, int bitLineH, int screenW,
                      int surfW, int surfH, int surfPitch);

/* 消息捕获钩 (0x5D3BA0, 6B): 只读栈上参数, 分流进两个消息环。 */
extern "C" __declspec(dllexport) DWORD __cdecl MsgText_AddMessageHook(void* regs);

/* 滚轮 (0x777998, 6B): 命中战役框则翻历史并吞掉滚轮, 返回 1=已吞。 */
int MsgText_WheelIn(int mx, int my, int dirUp);
/* 侧栏滚动让位 (0x533F50, 5B): 命中战役框时返回非 0, 调用方跳到让位点。 */
int MsgText_OverCamp(int mx, int my);

/* 聊天输入框锚点 (供 ChatEdit.cpp): 贴战役框下沿, 1=有锚点 (px/py 写出),
 * 0=无 (调用方留在原生位置)。只在底部模式成立。 */
int MsgText_ChatAnchor(int surfW, int surfH, int bitLineH, int* px, int* py);

/* 战役框文字样式 (供 ChatEdit.cpp 的输入框同源):
 * *face=已解析字体名, *px=字高 (<=0 表示跟游戏点阵), *bold=1/0。 */
void MsgText_CampStyle(const wchar_t** face, int* px, int* bold);
