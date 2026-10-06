/* MOExperience.h - DLL 协调层 (见 MOExperience.cpp)。
 * 只负责把各功能模块挂到引擎的时序点上:
 *   每循环 prepare (0x4F42F0) 裸钩 -> PowerDrawBoth (所有 *_Poll)
 *   每帧  消息框 Draw (0x5D4A94)   -> MessageDrawHook (所有 *_Draw)
 *   每帧  滚轮 (0x777998) / 侧栏滚动 (0x533F50)
 *   DllMain -> 各模块 *_Install
 */
#pragma once

#define MOE_VERSION "0.2.208"

/* 一次性初始化: 建日志路径 + 各模块首次载入配置。幂等, 各入口都先调它。 */
void MOE_EnsureInit(void);

/* UI 层链路 (战役框/提示框/超武面板/超武侧栏/橡皮线/小地图/聊天输入框),
 * 在指定面上跑一遍 (坐标是屏幕坐标)。缩放模块"放大呈现"之后要在 Primary 上
 * 重画一遍 —— 地图被重采样铺满视图区, Composite 里那些层已经被盖掉了。
 * 返回 1 = 至少画了一层。 */
int MOE_DrawUILayers(void* surf, int surfW, int surfH, int surfPitch);

/* ---------------- 崩溃定位: 全局阶段标记 ----------------
 * 引擎里到处是裸钩, 崩了只看寄存器常常定位不到 (栈早没了)。各入口把自己
 * 正在做的事写进 g_MoePhase, 崩溃处理程序把阶段名 + 栈上 16 个 dword 一起
 * 打出来, 下一次看日志就知道死在哪一步。 */
enum
{
    MP_NONE = 0,
    MP_POLL,        /* SB_Poll */
    MP_APPLY,       /* Fm_ApplyViewWidth: 写 ViewBounds */
    MP_MSGDRAW,     /* 引擎消息帧 */
    MP_UILAYER,     /* MOE_DrawUILayers */
    MP_COMPMAG,     /* 放大结果写进 Composite */
    MP_BLITSIDEBAR, /* BlitSb */
    MP_TIPPRE       /* 放大前那次提示框 Process (重影修复) */
};
extern volatile unsigned g_MoePhase;
void MOE_SetPhase(unsigned id);
const char* MOE_PhaseName(unsigned id);

/* UpdatePrimarySurface (0x4F4780) 前置跳板 (naked): 只保栈形平衡, 顺便打阶段标记。*/
void MOExperience_UpdateEntry(void);
extern void* g_UpdateTramp;
/* 帧准备钩入口 (naked): 每循环必跑, 底部电量"始终绘制"靠它。 */
void MOExperience_PrepareEntry(void);
extern void* g_PrepareTramp;
/* 上屏入口钩入口 (naked): 每次条带呈现前先恢复底部电量; FullMap 开启时
 * 接管这一次 blit (只呈小地图) 并按原版 ret 4 收尾, 跳过整条侧栏。 */
void MOExperience_BlitSbEntry(void);
extern void* g_BlitSbTramp;
/* 放大前那次提示框绘制钩入口 (naked): 抢 SidebarClass::Draw 里的
 * `ToolTipManager::Draw(1)` (0x6A7089, 7B)。放大档下只跑 Process 不画,
 * 否则那份会被 SB_CompositeMagnify 放大成重影。见 notes/地图缩放.md §5.1。*/
void MOExperience_TipPreDrawEntry(void);
extern void* g_TipPreDrawTramp;
/* 缩放的鼠标折算, 两条数据源都要折, 否则"按下时的点"与"每帧读到的点"差 A/R 倍
 * (放大档下表现为单击被判成拖框、橡皮线起点偏、点选落格偏):
 *   (a) WWMouseClass 虚表 +0x34 (GetMousePosition) —— 每帧轮询, 11 个消费点。
 *       装法见 MOE_InstallMousePosHook。
 *   (b) 窗口过程 0x7775C0 → 0x6930A0 的鼠标**按钮**消息分支, 点击点直接从
 *       Win32 消息的 lParam 取 (左键下/上/双击、右键下/上五个分支), 不过
 *       WWMouseClass。本钩把第4实参那个 lParam **指针**换成折算好的副本 ——
 *       那个槽在 0x6930A0 自己的栈帧上, ret 0x10 会弹掉 ⇒ 窗口过程那边一个字节
 *       都不动 (那个值后面还要交给 0x54F790 更新 WWMouseClass 的位置)。*/
void MOE_MousePosEntry(void);
void MOE_InstallMousePosHook(void);
int  MOE_MousePosPending(void);   /* 1 = 还没装上 (prepare 每循环重试) */
void MOExperience_WndMouseEntry(void);
extern void* g_WndMouseTramp;

/* 消息框 Draw 内部钩 (0x5D4A94, 7B): 各模块 *_Draw 的唯一出口 (链表非空时才跑)。
 * 依次调 MsgText / SWPanel / 超武侧栏 / 智能中键橡皮线 / 底部电量 /
 * 聊天输入框, 画完返回 0x5D4A9B (跳过原版绘制)。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_MessageDrawHook(void* regs);

/* 呈现后处理: 挂在 UpdatePrimarySurface 的两个帧尾调用点上, blit 一完就做放大
 * 呈现 + UI 重画 + 刻度条。⚠ 必须是裸壳 (栈要逐字节对, 见 MOExperience.cpp)。
 * 装在 VecText_Install 里。 */
/* 消息框 Draw 入口钩 (0x5D49A0, 6B): 每帧必跑; 引擎消息链表为空时
 * (0x5D4A92 的 je 会跳过 0x5D4A94) 代跑整条链路。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_MsgListEntry(void* regs);
/* 滚轮钩 (0x777998, 6B): 超武侧栏优先, 否则战役框历史。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_WheelHook(void* regs);
/* 侧栏滚动钩 (0x533F50, 5B): 悬停超武侧栏/战役框时跳过侧栏滚动。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_SidebarHook(void* regs);
