/* CheerSelect.cpp - C 键只让选中单位欢呼 (原版是发 ALLCHEER, 本家全体)。
 * 推导与引擎细节见 notes/欢呼.md, 这里只列改代码必须知道的约束:
 *   钩 0x730F30 (AllToCheerCommandClass::Execute 入口, 8 字节可整条重放);
 *   处理完跳 0x730FDB 那条裸 ret —— 本模块没有分配 0x70 栈帧, 不能跳 0x730FD8。
 *   只调 TechnoClass::Cheer(0) (虚表 0x388): 只有步兵重写了它, 载具/飞机/建筑
 *   走基类空实现, 与原版一致; Force=0 时引擎自己判"得是 Guard/Area_Guard 且
 *   当前序列空闲", 移动中的步兵不欢呼也不被打断。
 *   联机与无选中一律回原版 (发 ALLCHEER 事件); 绝不读写 [0x84555C] 那个全局
 *   冷却戳, 否则"局部欢呼过一次, 之后就按不出全局"。局部欢呼本身无冷却、无状态。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "CheerSelect.h"

#define CS_SECTION "Features"

/* TechnoClass::Cheer(bool Force), thiscall, 收尾 ret 4。 */
typedef void (__attribute__((thiscall)) *Fn_Cheer)(void* self, int force);
/* VocClass::PlayGlobal: fastcall(ecx=下标, edx=pan), 栈=音量/控制器, ret 8。 */
typedef void (__fastcall *Fn_PlayGlobal)(int idx, int pan, float vol, void* ctrl);

static int g_CsInit = 0;
static int g_CsEnable = 1;

static void CS_EnsureInit(void)
{
    if (g_CsInit) return;
    g_CsInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_CsEnable = GetPrivateProfileIntA(CS_SECTION, "CheerSelected", 1, g_IniPath) ? 1 : 0;
    LogLine(g_CsEnable ? "cheer: selected-only ON" : "cheer: selected-only OFF");
}

/* 0x730F30 钩子: 回 0=跑原指令; 回 0x730FDB=裸 ret (本模块已处理)。 */
extern "C" __declspec(dllexport) unsigned __cdecl CheerSelect_Hook(void* regs)
{
    void** items;
    void* rules;
    int cnt;
    int i;
    int n;
    int snd;

    (void)regs;
    CS_EnsureInit();
    if (!g_CsEnable) return 0;
    if (OU_IsMP()) return 0;
    /* 原版在这条指令后就返回; 走自己的路要自己判, 否则静音时反而能欢呼。 */
    if (*(volatile int*)(size_t)ADDR_MUTE_SWLAUNCHES) return 0;

    items = ARR_ITEMS(ADDR_CUR_OBJECTS);
    cnt = ARR_COUNT(ADDR_CUR_OBJECTS);
    if (!items || cnt <= 0) return 0;   /* 没选中: 原版全体欢呼 */

    n = 0;
    for (i = 0; i < cnt; i++)
    {
        void* obj = items[i];
        void** vt;
        Fn_Cheer fn;

        if (!obj || !IsPlausiblePtr(obj)) continue;
        if (OU_WhatAmI(obj) != ABS_INFANTRY) continue;
        /* 选中敌方单位是允许的 (看情报用), 但欢呼只给自己家, 同原版。 */
        if (*(void**)((char*)obj + OFF_OWNER) != *(void**)(size_t)ADDR_CURPLAYER) continue;
        if (!*(unsigned char*)((char*)obj + OFF_ISALIVE)) continue;
        if (*(unsigned char*)((char*)obj + OFF_INLIMBO)) continue;

        vt = *(void***)obj;
        if (!vt || !IsPlausiblePtr(vt)) continue;
        fn = (Fn_Cheer)vt[OFF_VT_CHEER / 4];
        if (!fn || !IsPlausiblePtr((const void*)fn)) continue;
        fn(obj, 0);
        n++;
    }

    /* 照 HouseClass::Cheer 的收尾: 有人真的欢呼才播一次 CheerSound。 */
    if (n)
    {
        rules = *(void**)(size_t)ADDR_RULES;
        snd = (rules && IsPlausiblePtr(rules))
            ? *(int*)((char*)rules + OFF_RULES_CHEERSOUND) : -1;
        if (snd >= 0)
            ((Fn_PlayGlobal)(size_t)FN_VOC_PLAYGLOBAL)(snd, 0x2000, 1.0f, NULL);
    }
    /* 一个能欢呼的都没有也消费掉这次按键, 否则"选中即范围"就不成立。 */
    return ADDR_CMD_CHEER_RET;
}
