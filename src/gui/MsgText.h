/* MsgText.h - 消息文本显示 (战役框 + 操作与战斗提示), 见 MsgText.cpp。
 *
 * 接管引擎 MessageListClass 的消息队列, 自绘 GDI 矢量文本:
 *   战役框 (sp=0, 战役/聊天/系统): 长留历史, 悬停展开, 滚轮翻看。
 *   操作与战斗提示 (sp=1): 操作提示 (快捷键/路径点) + 雷达事件,
 *     按引擎超时自然消失。雷达事件行可点击, 点一下视口跳到事件发生地
 *     (走引擎自己的 SetTacticalPosition, 只动本地摄像机)。
 * 配置节 [MsgText]。
 *
 * 钩子 (Syringe, 见 dist/MOExperience.dll.inj):
 *   0x5D3BA0 AddMessage 捕获; 0x777998 滚轮翻历史;
 *   0x533F50 悬停时跳过侧栏滚动; 0x69234B 雷达事件行点击门。
 * 渲染本身挂 0x5D4A94 (引擎链表非空时才跑) 与 0x5D49A0 兜底入口,
 * 两者都在 MOExperience.cpp 的协调层, 见 MOExperience_MsgListEntry。
 * 任何一步失败 (Lock 不上、GDI 未就绪) 都返回 0 走原版绘制, 绝不花屏。
 */
#pragma once

#define MSG_SECTION "MsgText"

/* 一次性初始化 (读配置 + 解析字体); 幂等, 各钩子入口都先调它。 */
void MsgText_EnsureInit(void);

/* 运行期切换语言后重载缓存的雷达文案。 */
void MsgText_ReloadLang(void);

/* 是否接管 (与 MsgText_DrawFrame 同一开关, 协调层无条件抑制原版用)。 */
int MsgText_Enabled(void);

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

/* 雷达事件行点击门 (0x69234B, 5B): 落在操作与战斗提示的雷达事件行上就
 * 完成视口跳转并吞掉这次地图点击 (0x6925FC = 原函数"未处理"出口)。
 * 返回 0 = 不属于雷达事件行, 原版自己处理。
 * 联机安全: 只动本地摄像机 (引擎空格跳转同款), 不写模拟对象、不发事件。 */
extern "C" __declspec(dllexport) unsigned __cdecl MsgText_RadarClickGate(void* regs);

/* 最新一条仍可见的雷达事件 (操作与战斗提示里的雷达行): 返回 1 = 有, 写出格与序号。
 * seq 每次收到新雷达事件就 +1 (单调递增, 不随清历史归零), 供 PiP 判断"来了新事件"。
 * 只读, 不改消息环/模拟状态。 */
int  MsgText_LatestRadar(short* cx, short* cy, unsigned* seq);

/* 格子的地形高度 (GetCellCoords 的 Z; 只读查询)。无效格返回 0。 */
int  MsgText_CellZ(short cx, short cy);

/* 引擎的 Z -> 屏幕 Y 偏移 (0x6D20E0; 纯换算, 只读)。 */
int  MsgText_ZToScreen(int z);

/* 聊天输入框锚点 (供 ChatEdit.cpp): 贴战役框下沿, 1=有锚点 (px/py 写出),
 * 0=无 (调用方留在原生位置)。只在底部模式成立。 */
int MsgText_ChatAnchor(int surfW, int surfH, int bitLineH, int* px, int* py);

/* 战役框文字样式 (供 ChatEdit.cpp 的输入框同源):
 * *face=已解析字体名, *px=字高 (<=0 表示跟游戏点阵), *bold=1/0。 */
void MsgText_CampStyle(const wchar_t** face, int* px, int* bold);

/* ESC 菜单运行期读取/修改的显示选项 (只改内存, 不写 INI)。 */
enum {
    MSG_OPT_CAMP_ENABLE = 0,
    MSG_OPT_CAMP_LINES,
    MSG_OPT_CAMP_OPACITY,
    MSG_OPT_CAMP_BOTTOM,
    MSG_OPT_CAMP_CENTERED,
    MSG_OPT_CAMP_WIDTH,
    MSG_OPT_CAMP_ANIM,
    MSG_OPT_CAMP_BAR,
    MSG_OPT_HINT_SHOW,
    MSG_OPT_HINT_COLOR,
    MSG_OPT_HINT_LINES,
    MSG_OPT_HINT_WIDTH,
    MSG_OPT_HINT_BAR,
    MSG_OPT_HINT_RADAR,
    MSG_OPT_COUNT
};

int  MsgText_GetOption(int id);
void MsgText_SetOption(int id, int value);

/* ESC 设置页取消: 整份运行期配置快照/恢复 (不碰 INI)。 */
void MsgText_SaveOptions(void);
void MsgText_RestoreOptions(void);
