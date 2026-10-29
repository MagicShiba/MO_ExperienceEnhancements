/* NoApproach.cpp - 无法攻击目标的单位不再"冲过去送死"。
 *
 * 现象: 框选攻击单位 + 非攻击单位 (如维修无人机) 后左键点敌方,
 * 不能攻击的单位也会被下令移动到目标, 冲到敌人面前。
 * 根因: 左键派发按各单位的动作调 ObjectClickedAction,
 * 不能攻击的单位动作是 NoMove(2), 其处理器末尾会下发
 * ClickedMission(Move, Target=该对象)。
 *
 * 处理: 在 NoMove 处理器入口 0x4D77BD 挂钩。目标是敌方 Techno
 * (非本家且非盟友) 时直接跳该函数收尾 0x4D7CC0, 当作已处理,
 * 不派发任何 mission, 单位原地不动。其余情况跑原指令。
 * 规划模式 (Z 路径点) 走原版。
 *
 * 联机: 只读查询 + 对自家单位不下令, 不发事件, 不影响联机。
 * 配置 [Features] NoApproach (缺失=1, 改后重启)。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "NoApproach.h"

#define NA_SECTION "Features"

/* ---------------- 只读地址 ---------------- */

#define ADDR_NOMOVE_TRUE    0x4D7CC0u   /* NoMove 处理器收尾 (AL=1, ret 0xc), 跳此即当已处理 */
#define FN_ALLIED           0x4F9A50u   /* HouseClass::IsAlliedWith(HouseClass const*), 只读 */

/* Syringe REGISTERS 相对偏移 (YRpp Syringe.h, 同 BuildQueue.cpp)。 */
#define NA_R_ESI 0x0Cu   /* this (FootClass*) */
#define NA_R_EBX 0x18u   /* pTarget (ObjectClass*) */

/* IsAlliedWith 返回值仅 AL 有效, 函数指针须声明为 unsigned char。 */
typedef unsigned char __attribute__((thiscall)) (*Fn_IsAlliedWith)(void* self, void* other);

/* ---------------- 配置 ---------------- */

static int g_NaInit = 0;
static int g_NaEnable = 1;

static void NA_EnsureInit(void)
{
    char buf[96];
    unsigned q = 0;
    if (g_NaInit) return;
    g_NaInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_NaEnable = GetPrivateProfileIntA(NA_SECTION, "NoApproach", 1, g_IniPath) ? 1 : 0;
    q = AppendCStr(buf, q, "noapproach: ", sizeof(buf));
    q = AppendCStr(buf, q, g_NaEnable ? "ON" : "OFF", sizeof(buf));
    (void)q;
    LogLine(buf);
}

/* 对象指针须再验虚表落 exe 内 (IsPlausiblePtr 只判范围对齐)。 */
static int NA_IsObj(void* p)
{
    if (!p || !IsPlausiblePtr(p)) return 0;
    return OU_InExe((unsigned)(size_t)*(void***)p);
}

/* 0x4D77BD 钩子: 回 0=跑原指令; 回 0x4D7CC0=当已处理 (单位停原地)。 */
extern "C" __declspec(dllexport) unsigned __cdecl NoApproach_Hook(void* regs)
{
    void* pThis;
    void* pTarget;
    void* own;
    void* toh;
    int w;

    NA_EnsureInit();
    if (!g_NaEnable) return 0;
    if (*(unsigned char*)ADDR_PLANMODE) return 0;   /* Z 路径点/规划: 原版 */

    pThis = (void*)(size_t)*(unsigned*)((char*)regs + NA_R_ESI);
    pTarget = (void*)(size_t)*(unsigned*)((char*)regs + NA_R_EBX);
    if (!NA_IsObj(pThis) || !NA_IsObj(pTarget)) return 0;

    /* 目标非 Techno (地形等): 原版。 */
    w = OU_WhatAmI(pTarget);
    if (w != ABS_UNIT && w != ABS_INFANTRY &&
        w != ABS_AIRCRAFT && w != ABS_BUILDING) return 0;

    /* 目标非本家且非盟友才抑制; Owner 不明则保守走原版。 */
    own = *(void**)((char*)pThis + OFF_OWNER);
    if (!own || !IsPlausiblePtr(own)) return 0;
    toh = *(void**)((char*)pTarget + OFF_OWNER);
    if (toh && !IsPlausiblePtr(toh)) return 0;
    if (((Fn_IsAlliedWith)FN_ALLIED)(own, toh)) return 0;
    return ADDR_NOMOVE_TRUE;
}
