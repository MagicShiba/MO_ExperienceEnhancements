/* MOExperience.h - DLL 协调层 (见 MOExperience.cpp)。
 * 只负责把各功能模块挂到引擎的时序点上:
 *   每循环 prepare (0x4F42F0) 裸钩 -> PowerDrawBoth (所有 *_Poll)
 *   每帧  消息框 Draw (0x5D4A94)   -> MessageDrawHook (所有 *_Draw)
 *   每帧  滚轮 (0x777998) / 侧栏滚动 (0x533F50)
 *   DllMain -> 各模块 *_Install
 */
#pragma once

#define MOE_VERSION "0.2.218"

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
    MP_TIPPRE,      /* 放大前那次提示框 Process (重影修复) */
    MP_PAINTVOID,   /* SB_PaintVoid: 铺图外底色 */
    MP_ZBUF,        /* SB_PrepareSurface: ZBuffer/大面尺寸 */
    MP_SURFSWAP     /* 换面 (SwapIn/SwapOut/Suspend/Resume) */
};
extern volatile unsigned g_MoePhase;
void MOE_SetPhase(unsigned id);
const char* MOE_PhaseName(unsigned id);

/* UpdatePrimarySurface (0x4F4780) 前置跳板 (naked): 只保栈形平衡, 顺便打阶段标记。*/
void MOExperience_UpdateEntry(void);
extern void* g_UpdateTramp;
/* GScreenClass::Render (0x4F4480) 入口钩 (naked): 缩小档 (scale>1) 时把
 * [0x88731C]/[0x8872FC] 临时换成 SB 的 R 大离屏面, 让引擎 1:1 画进大面;
 * 三趟地图之后在 0x4F451B 还原 + 降采样。1x/放大档/未启用时恒为空操作。*/
void MOExperience_RenderEntry(void);
extern void* g_RenderTramp;
/* GScreenClass::Render 里"[0x887314] = 渲染面"那条 (0x4F4492, 5B) 的钩 (naked):
 * 缩小档下把三个渲染面全局**让回窗口面** (SB_RenderSuspend)。那之后到三趟地图之间
 * 引擎还有一串按**窗口坐标**画的指令 (WWMouse::vt+0x40 光标擦除、
 * `[0xB0B518]/[0xB0B519]`、`GScreenClass::vt+0x40`); 它们若拿到 R 大的离屏大面,
 * 那些像素就混进地图, 降采样后在画面中部闪出一条底栏/刻度条残影。
 * 0x4F44CB 那条把大面装回去 (SB_RenderResume), 于是大面被暴露的窗口只剩三趟地图。*/
void MOExperience_PreCurEntry(void);
extern void* g_PreCurTramp;
/* GScreenClass::Render 里 chrome pass 调用点 (0x4F44FD, 5B = `8B 16 50 8B CE`,
 * 下一条正是 `call [edx+0x40]`) 的钩 (naked): 缩小档把三个渲染面全局**让回窗口面**
 * (SB_RenderSuspend(0))。虚表 +0x40 = 0x6D0A20 那趟"底栏/侧栏 chrome"全按**窗口
 * 坐标**画 (底栏矩形 [0xB0FC58]、SidebarClass::Draw、ToolTipManager::Draw(1)),
 * 拿到 R 大的大面就会把底栏与提示盒烙进地图, 降采样后落在画面中部 (实测图1/图2)。
 * 0x4F44FD 之前挡不住它 —— 之前只挡了入口那串。*/
void MOExperience_ChromePreEntry(void);
extern void* g_ChromePreTramp;
/* chrome pass 之后 (0x4F4505, 6B = `8B 0D 1C 73 88 00`, 第三趟地图之前) 把大面
 * 装回全局 (SB_RenderResume), 跳回 0x4F450B 让原指令照跑重读全局。*/
void MOExperience_ChromePostEntry(void);
extern void* g_ChromePostTramp;
/* 由缩放模块维护: 非 0 时上面那条钩还把**入口 eax** 换成 g_PreCurSurf
 * (0x4F44A2 的光标擦除直接吃 eax, 不读全局)。*/
extern int   g_PreCurWin;
extern void* g_PreCurSurf;
/* GScreenClass::Render 里三趟地图前那条 (0x4F44CB, 5B = `A1 1C 73 88 00`) 的钩
 * (naked): 缩小档把大面装回全局 (SB_RenderResume), 跳回 0x4F44D0 让原指令照跑。*/
void MOExperience_MapEntry(void);
extern void* g_MapTramp;
/* GScreenClass::Render 三趟地图画完、引擎 UI 之前 (0x4F451B) 的钩 (naked):
 * 缩小档还原窗口面 + 降采样; 1x/放大档恒空操作。 */
void MOExperience_AfterMapEntry(void);
extern void* g_AfterMapTramp;
void MOExperience_ChromeEntry(void);
extern void* g_ChromeTramp;
void MOExperience_BlitDiagEntry(void);
extern void* g_BlitTramp;
/* 边缘卷屏 0x692B60 入口钩 (naked): 缩放档把传入点换回物理鼠标 (屏幕空间死区)。 */
void MOExperience_EdgeFixEntry(void);
extern void* g_EdgeFixTramp;
/* 通用拉伸 blit 0x6BA580 入口钩 (naked): 缩放档下面指针野了就跳过 (防崩)。 */
void MOExperience_BlitGuardEntry(void);
extern void* g_BlitGuardTramp;
/* DisplayClass::ProcessClickCoords (0x692300) 入口钩 (naked): 缩小档下把**视口外**的
 * 点明确拒掉 (该函数只判 X<0 / Y<0, 没有上界判定)。ViewBounds = R 比窗口大时,
 * 底栏带 (y∈[A.Height,winH)) 的物理坐标落在 R 之内 ⇒ 引擎把底栏当成地图上那一带,
 * 选中单位点底栏 = 对画面中部的地图像素下令 (SB_PtInView)。1x/放大档恒空操作。 */
void MOExperience_ClickCoordsEntry(void);
extern void* g_ClickCoordsTramp;
/* TacticalClass::Clamp_Center (0x6D8640) 入口钩: 视口比地图大时把该轴钉在地图正中,
 * 消掉 "TacticalPos 每帧在 min/max 之间跳 ⇒ 增量光栅化来回搬运像素" 造成的抖动与
 * 一侧残影 (推导见 ScaleBar.cpp 的 SB_ClampFix)。首 5B = `A1 E4 F8 87 00`。 */
void MOExperience_ClampCenterEntry(void);
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
