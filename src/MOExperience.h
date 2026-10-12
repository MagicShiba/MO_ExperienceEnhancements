/* MOExperience.h - DLL 协调层: 把各功能模块挂到引擎时序点上 (见 .cpp)。 */
#pragma once

#define MOE_VERSION "0.2.228"

/* 一次性初始化: 建日志路径 + 各模块首次载入配置。幂等, 各入口都先调它。 */
void MOE_EnsureInit(void);

/* UI 层链路: 放大呈现后在 Primary 上重画一遍 (Composite 里那份已被盖掉)。
 * 返回 1 = 至少画了一层。 */
int MOE_DrawUILayers(void* surf, int surfW, int surfH, int surfPitch,
                       int* bitLineH, int* screenW);

/* 崩溃定位: 各入口把阶段 id 写进 g_MoePhase, 崩溃处理打阶段名 + 栈快照。 */
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
/* GScreenClass::Render 入口钩: 缩小档把两槽换成 R 大面, 三趟后还原+降采样。 */
void MOExperience_RenderEntry(void);
extern void* g_RenderTramp;
/* Render 里 "[0x887314] = 渲染面"那条的钩: 缩小档让回窗口面 (光标擦除串)。 */
void MOExperience_PreCurEntry(void);
extern void* g_PreCurTramp;
/* chrome pass 调用点的钩: 缩小档让回窗口面 (底栏/侧栏按窗口坐标画)。 */
void MOExperience_ChromePreEntry(void);
extern void* g_ChromePreTramp;
/* chrome pass 之后把大面装回 (SB_RenderResume)。 */
void MOExperience_ChromePostEntry(void);
extern void* g_ChromePostTramp;
/* 由缩放模块维护: 非 0 时上面那条钩还把**入口 eax** 换成 g_PreCurSurf
 * (0x4F44A2 的光标擦除直接吃 eax, 不读全局)。*/
extern int   g_PreCurWin;
extern void* g_PreCurSurf;
/* 三趟地图前那条的钩: 缩小档把大面装回 (SB_RenderResume)。 */
void MOExperience_MapEntry(void);
extern void* g_MapTramp;
/* 三趟地图画完的钩: 缩小档还原窗口面 + 降采样。 */
void MOExperience_AfterMapEntry(void);
extern void* g_AfterMapTramp;
/* 边缘卷屏 0x692B60 入口钩 (naked): 缩放档把传入点换回物理鼠标 (屏幕空间死区)。 */
void MOExperience_EdgeFixEntry(void);
extern void* g_EdgeFixTramp;
/* 通用拉伸 blit 0x6BA580 入口钩 (naked): 缩放档下面指针野了就跳过 (防崩)。 */
void MOExperience_BlitGuardEntry(void);
extern void* g_BlitGuardTramp;
/* ProcessClickCoords 入口钩: 缩小档把视口外的点明确拒掉 (映射值看 SB_PtInView,
 * 物理坐标看 SB_PtInRawView, 任一出界即拒)。 */
void MOExperience_ClickCoordsEntry(void);
extern void* g_ClickCoordsTramp;
/* Clamp_Center 入口钩: 视口比地图大时钉地图正中 (SB_ClampFix)。 */
void MOExperience_ClampCenterEntry(void);
/* DrawPixelEffects (迷雾) 入口钩: 先把 [0x887314] 换成大面挡越界写。 */
void MOExperience_ShroudEntry(void);
extern void* g_ShroudTramp;
/* 帧准备钩入口 (naked): 每循环必跑, 底部电量"始终绘制"靠它。 */
void MOExperience_PrepareEntry(void);
extern void* g_PrepareTramp;
/* 上屏入口钩入口 (naked): 每次条带呈现前先恢复底部电量; FullMap 开启时
 * 接管这一次 blit (只呈小地图) 并按原版 ret 4 收尾, 跳过整条侧栏。 */
void MOExperience_BlitSbEntry(void);
extern void* g_BlitSbTramp;
/* 提示框绘制钩: 放大档下抢掉 SidebarClass::Draw 里那次 Draw(1) 防重影。 */
void MOExperience_TipPreDrawEntry(void);
extern void* g_TipPreDrawTramp;
/* 缩放的鼠标折算 (WWMouse +0x34 与窗口过程按钮消息两条源, 见 .cpp)。 */
void MOE_MousePosEntry(void);
void MOE_InstallMousePosHook(void);
int  MOE_MousePosPending(void);   /* 1 = 还没装上 (prepare 每循环重试) */
void MOExperience_WndMouseEntry(void);
extern void* g_WndMouseTramp;

/* 消息框 Draw 内部钩: 各模块 *_Draw 唯一出口, 画完跳过原版绘制。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_MessageDrawHook(void* regs);

/* 消息框 Draw 入口钩 (0x5D49A0, 6B): 每帧必跑; 引擎消息链表为空时
 * (0x5D4A92 的 je 会跳过 0x5D4A94) 代跑整条链路。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_MsgListEntry(void* regs);
/* 滚轮钩 (0x777998, 6B): 超武侧栏优先, 否则战役框历史。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_WheelHook(void* regs);
/* 侧栏滚动钩 (0x533F50, 5B): 悬停超武侧栏/战役框时跳过侧栏滚动。 */
extern "C" __declspec(dllexport) DWORD __cdecl MOExperience_SidebarHook(void* regs);
