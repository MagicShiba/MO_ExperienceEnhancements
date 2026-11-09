/* MCLoss.cpp - 关掉心控单位的"单位损失"提示 (雷达事件 + EVA 语音)。
 *
 * 链路: Ares 3.0 把 FootClass::Destroyed 的 0x4D98C0 整段重写, 对"当前玩家
 * 控制"的阵亡单位依次调 RadarEventClass::Create(UnitLost, cell) 和
 * VoxClass::PlayIndex(EVA.Lost)。心控来的单位 Owner 已被改成我方, 所以被算成
 * 我方损失; 心控流战役里这类死亡远多于自有单位, 雷达圈/左下文本/语音会刷屏。
 *
 * 拦截点: 两个纯表现层引擎函数入口 (地址见 EngineAddr.h)。挡住 type=7 的
 * 雷达事件, 雷达圈与左下"单位遭到损失"行都不再产生; 随后那声 EVA 由紧跟
 * 的一次性标记挡住。抑制时直接跳函数尾 ret 4: 站点就在函数入口, 序言尚未
 * 执行, 栈上只有一个 4 字节参数, 寄存器与栈都保持调用者状态。
 *
 * 单位判据: Ares 的 hook 全程在 EDI 里留着 pThis (两个调用包装都是 jmp thunk,
 * 不改 EDI), 并把同一个值压在调用栈 [esp+8]。两处任一取到有效对象即可;
 * 校验指针/虚表/WhatAmI/Owner 后只读查询 IsMindControlled。取不到就走原版。
 *
 * 联机: 只影响本地雷达事件对象与本地 EVA 播放, 不写模拟状态、不发事件,
 * 各客户端自行决定是否显示, 不会产生不同步。
 * 配置 [Features] MCLossMute (缺失=1 = 静音), ESC 设置页可现读现改。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "MOExperience.h"
#include "MCLoss.h"

#define MC_SECTION "Features"
#define MC_KEY     "MCLossMute"

/* IsMindControlled 只写 AL, 返回类型按 1 字节接。 */
typedef unsigned char __attribute__((thiscall)) (*MC_FnIsMindControlled)(void* self);

static int      g_Init = 0;
static int      g_Enable = 1;
static int      g_SkipVoice = 0;   /* 刚吞掉一次 UnitLost 雷达事件 */
static unsigned g_SkipFrame = 0;   /* 只在同帧内有效 */
static unsigned g_Hits = 0;        /* 抑制日志限频 */

static void MC_EnsureInit(void)
{
    char b[96];
    unsigned q = 0;

    if (g_Init) return;
    g_Init = 1;
    BuildPaths();
    GtEnsureLog();
    g_Enable = GetPrivateProfileIntA(MC_SECTION, MC_KEY, 1, g_IniPath) ? 1 : 0;
    q = AppendCStr(b, q, "mcloss: MCLossMute=", sizeof(b));
    q = AppendCStr(b, q, g_Enable ? "ON" : "OFF", sizeof(b));
    (void)q;
    LogLine(b);
}

/* 启动后前几条抑制记进日志, 之后只累计, 心控战役里不刷屏。 */
static void MC_Log(const char* tag, unsigned frame)
{
    char b[128], n[16];
    unsigned q = 0;

    if (g_Hits++ >= 5) return;
    q = AppendCStr(b, q, "mcloss: ", sizeof(b));
    q = AppendCStr(b, q, tag, sizeof(b));
    q = AppendCStr(b, q, " frame=", sizeof(b));
    UtoA(frame, n);
    q = AppendCStr(b, q, n, sizeof(b));
    (void)q;
    LogLine(b);
}

int MCLoss_Enabled(void)
{
    MC_EnsureInit();
    return g_Enable;
}

void MCLoss_SetEnabled(int on)
{
    MC_EnsureInit();
    g_Enable = on ? 1 : 0;
}

/* 只认"正在阵亡、属于本地玩家、且处于心控状态"的作战单位。 */
static int MC_IsMCLostUnit(void* p)
{
    void** vt;
    int w;

    if (!p || !IsPlausiblePtr(p)) return 0;
    vt = *(void***)p;
    if (!vt || !MOE_InAnyModule((unsigned)(size_t)vt)) return 0;
    w = OU_WhatAmI(p);
    if (w != ABS_UNIT && w != ABS_INFANTRY && w != ABS_AIRCRAFT) return 0;
    if (*(void**)((char*)p + OFF_OWNER) != *(void**)(size_t)ADDR_CURPLAYER)
        return 0;
    return ((MC_FnIsMindControlled)(size_t)FN_TECH_ISMINDCONTROLLED)(p) ? 1 : 0;
}

/* EDI 是 Ares hook 留着的 pThis; [esp+8] 是同一函数压栈保存的副本。 */
static void* MC_LossUnit(void* regs)
{
    unsigned esp = *(unsigned*)((char*)regs + REG_OFF_ESP);
    void* a = (void*)(size_t)*(unsigned*)((char*)regs + REG_OFF_EDI);
    void* b = NULL;

    if (IsPlausiblePtr((void*)(size_t)esp) &&
        IsPlausiblePtr((void*)(size_t)(esp + 8)))
        b = (void*)(size_t)*(unsigned*)(size_t)(esp + 8);
    if (MC_IsMCLostUnit(a)) return a;
    if (MC_IsMCLostUnit(b)) return b;
    return NULL;
}

/* 0x65FA70 入口 (6B): 吞掉心控单位的 UnitLost 雷达事件。
 * 回 0 = 跑原指令; 回 0x65FB47 = 直接 ret 4。 */
extern "C" __declspec(dllexport) unsigned __cdecl MCLoss_CreateHook(void* regs)
{
    void* unit;

    MC_EnsureInit();
    g_SkipVoice = 0;    /* 任何新的雷达事件都让旧标记作废 */
    if (!g_Enable) return 0;
    if (*(unsigned*)((char*)regs + REG_OFF_ECX) != RE_UNIT_LOST) return 0;
    unit = MC_LossUnit(regs);
    if (!unit) return 0;
    g_SkipVoice = 1;
    g_SkipFrame = *(unsigned*)(size_t)ADDR_FRAME;
    MC_Log("radar", g_SkipFrame);
    return ADDR_RE_CREATE_RET;
}

/* 0x752480 入口 (5B): Ares 的 Create 与 PlayIndex 是紧邻两条 call,
 * 所以标记只消费这一次播放, 其它 EVA 一律照常。回 0x752581 = 直接 ret 4。 */
extern "C" __declspec(dllexport) unsigned __cdecl MCLoss_VoiceHook(void* regs)
{
    unsigned frame = *(unsigned*)(size_t)ADDR_FRAME;
    int hit;

    (void)regs;
    MC_EnsureInit();
    hit = g_SkipVoice && g_SkipFrame == frame;
    g_SkipVoice = 0;
    if (!hit || !g_Enable) return 0;
    MC_Log("eva", frame);
    return ADDR_VOX_PLAYINDEX_RET;
}
