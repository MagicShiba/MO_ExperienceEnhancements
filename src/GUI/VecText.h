/* VecText.h - 全局文本矢量接管 (见 VecText.cpp)。 */

#pragma once

#define VT_SECTION       "VecText"

/* 一次性载入 [VecText] 配置 (幂等); 协调层 MOE_EnsureInit 调。 */
void VecText_EnsureInit(void);
/* 装运行时补丁 (Blit/DrawText/Print/帧准备/上屏/呈现), 只在 DllMain 调一次。 */
void VecText_Install(void);

/* Frame-prepare hook entry (MOExperience.cpp, naked): always-draw for bottom power. */
void MOExperience_PrepareEntry(void);
extern void* g_PrepareTramp;
/* Sidebar-present hook entry (MOExperience.cpp, naked): restore before present. */
void MOExperience_BlitSbEntry(void);
extern void* g_BlitSbTramp;
/* Update-primary hook entry (MOExperience.cpp, naked): draw on presented surface. */
void MOExperience_UpdateEntry(void);
extern void* g_UpdateTramp;
/* Map-rect capture hook entry (MOExperience.cpp, naked): MapClass::CreateEmptyMap. */
void MOExperience_CreateMapEntry(void);
extern void* g_CreateMapTramp;
/* Gadget-input hook entry (MOExperience.cpp, naked): GadgetClass::Input. */
void MOExperience_GadgetInputEntry(void);
extern void* g_GadgetInputTramp;
/* Sidebar-column fix hook entry (MOExperience.cpp, naked): DisplayClass::InitGUI. */
void MOExperience_InitGuiEntry(void);
extern void* g_InitGuiTramp;

/* Top credits size (CreditPx, >0=user absolute px): bottom power matches it. */
int VecText_CreditPx(void);
/* Credits resolved typeface + bold ([VecText] FontFace/Bold): power uses same. */
const wchar_t* VecText_FontFace(void);
int VecText_Bold(void);
/* Resolved global vector font pixel size ([VecText] FontPx, 0 = auto). */
int VecText_FontPx(void);
int VecText_Bold(void);

/* ---------------- 超武列表整体接管 (Print 入口收集, 消息帧统一重绘) ----------------
 * VecText.cpp 只做高层级隐藏+收集 (不管内容细节, 不逐行绘制);
 * SWPanel.cpp 的超武面板每帧从此表取新鲜行整块重绘 (同一 DLL, 直接共享)。
 * kind: 0=名, 1=计时器。tick=GetTickCount, 400ms 未刷新即过期。 */
#define SWCAP_MAX 32
#define SW_FRESH_MS 150   /* 收集行保鲜 (毫秒): 过期即面板隐藏, 引擎闪烁原样透出 */
typedef struct {
    wchar_t s[64];
    int x, y, kind;
    int fore;          /* 原前景色 (Simple_ a4 实证; -1=未见, 用名青/计时器白) */
    unsigned tick;
} SWCapRow;
extern SWCapRow g_SWCap[SWCAP_MAX];
extern int g_SWCapN;
extern int g_SWCompW, g_SWCompH;   /* 主画布尺寸 (消息帧回填, 收集的坐标系门) */
extern int g_SWPanelOn;            /* 面板总开关 ([SWPanel] Enable, SWPanel 回填) */
