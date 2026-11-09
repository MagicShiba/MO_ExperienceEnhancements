/* Shift+P 在原生 P 全选落地后, 仅保留当前选中的临时心控单位并补入屏内候选。
 * 判定为 IsMindControlled && IsSelectable && !MindControlledByAUnit。
 * 仅修改本地选择列表和 IsSelected, 不改模拟状态、不发模拟事件。
 * 没有心控候选时保持原选择。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "SelPoolCap.h"

#define MS_SECTION "Features"

/* ---------------- 私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define OFF_SEL_NEXT     12u         /* {Object*, X, Y} (YRpp TacticalClass.h:13) 基址见 SelPool_Base() */

#define OFF_VEC_CAP    8u

#define OFF_ISSELECTED 0x83u
#define OFF_MCBYAUNIT  0x2C4u    /* TechnoClass::MindControlledByAUnit (bool 字节) */

#define VT_SELECT       (0x14Cu / 4u)   /* ObjectClass::Select */
#define VT_ISSELECTABLE (0x64u / 4u)    /* ObjectClass::IsSelectable */

typedef unsigned char __attribute__((thiscall)) (*Fn_IsMindControlled)(void* self);
typedef int __attribute__((thiscall)) (*Fn_IsSelectable)(void* self);
typedef int __attribute__((thiscall)) (*Fn_Select)(void* self);

#define FN_ISMC ((Fn_IsMindControlled)(void*)(size_t)FN_TECH_ISMINDCONTROLLED)

#define MS_MC_CAP 256   /* 单轮心控收集上限 (超限截断) */

/* ---------------- 配置 ---------------- */

static int g_MsInit = 0;
static int g_MsEnable = 1;
static OU_Hotkey g_MsHk;      /* [Features] MindSelect (默认 Shift+P) */
static int g_MsPrevCombo = 0;

static void MS_EnsureInit(void)
{
    if (g_MsInit) return;
    g_MsInit = 1;
    BuildPaths();
    g_MsEnable = OU_ReadHotkeySingle(MS_SECTION, "MindSelect", "Shift+P", &g_MsHk);
}

/* 设置页写盘后回读热键。 */
void MindSelect_ReloadHotkey(void)
{
    MS_EnsureInit();
    g_MsEnable = OU_ReadHotkeySingle(MS_SECTION, "MindSelect", "Shift+P", &g_MsHk);
}

/* ---------------- 判定 + 单步 ---------------- */

/* 只读引擎可选性: 含所有权、存活和 IsSelectableCombatant 判定。 */
static int MS_IsSelectable(void* u)
{
    void** vt;
    Fn_IsSelectable fn;
    vt = *(void***)u;                     /* 调用方已验 u 合理 */
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    fn = (Fn_IsSelectable)vt[VT_ISSELECTABLE];
    /* 无有效查询函数时交给原生 Select 最终判定。 */
    if (!fn || !IsPlausiblePtr((const void*)fn)) return 1;
    return fn(u) ? 1 : 0;
}

/* 心控判定前置: 作战单位 + 存活 + 进图 + 有血 + 非永久心控 + 引擎可选。
 * 非作战单位 (如建筑) 直接判非心控 (不进引擎调用, 省一次调用也避开
 * 建筑 +0x2C0 野值误判)。 */
static int MS_IsMCd(void* u)
{
    int w;
    if (!u || !IsPlausiblePtr(u)) return 0;
    w = OU_WhatAmI(u);
    if (w != ABS_UNIT && w != ABS_INFANTRY && w != ABS_AIRCRAFT) return 0;
    if (!OU_TechAliveRaw(u)) return 0;                 /* 存活三连 */
    if (((unsigned char*)u)[OFF_MCBYAUNIT]) return 0;   /* 永久心控不算 */
    if (!MS_IsSelectable(u)) return 0;                 /* 引擎可选性 */
    return FN_ISMC(u) ? 1 : 0;             /* 心控判定 (只读调用) */
}

static int MS_SelectOne(void* u)
{
    void** vt;
    Fn_Select fn;
    vt = *(void***)u;                     /* 调用方已验 u 合理 */
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    fn = (Fn_Select)vt[VT_SELECT];
    if (!fn || !IsPlausiblePtr((const void*)fn)) return 0;
    return fn(u) ? 1 : 0;                 /* 原生 Select (同框选/P) */
}

/* ---------------- 收敛轮 (幂等) ---------------- */

static void MS_Round(void)
{
    void* scrMC[MS_MC_CAP];
    int nS = 0, i;
    void** items;
    int cap, count, selMC = 0, kept = 0;
    unsigned lim = SelPool_Capacity();
    unsigned char* base = SelPool_Base();

    /* 扫描上界: 候选池容量 (SelPoolCap 扩容后 4096, 没装还是 500) 与
     * 本帧实际表项数取小 —— 只扫有效前缀, 不碰表尾之外残留的旧指针。 */
    {
        void* tac = *(void**)(size_t)ADDR_TACTICAL;
        if (tac && IsPlausiblePtr(tac))
        {
            int c = *(int*)((char*)tac + OFF_TAC_SELCNT);
            if (c <= 0) lim = 0;
            else if ((unsigned)c < lim) lim = (unsigned)c;
        }
    }

    /* 1. 屏内心控收集 (加法源, 非心控零触碰)。 */
    for (i = 0; i < (int)lim && nS < MS_MC_CAP; i++)
    {
        void* u = *(void**)(base + (unsigned)i * OFF_SEL_NEXT);
        if (MS_IsMCd(u)) scrMC[nS++] = u;
    }
    /* 2. 当前选中清点 (只读, 不动)。 */
    items = *(void***)(ADDR_CUR_OBJECTS + OFF_VEC_ITEMS);
    cap = *(int*)(ADDR_CUR_OBJECTS + OFF_VEC_CAP);
    count = *(int*)(ADDR_CUR_OBJECTS + OFF_VEC_COUNT);
    if (!items || !IsPlausiblePtr(items)) return;
    if (cap <= 0 || cap > 8192 || count < 0 || count > cap) return;
    for (i = 0; i < count; i++)
        if (MS_IsMCd(items[i])) selMC++;
    /* 3. 两处都无心控 → 碰都不碰 (不清空原选中)。 */
    if (nS == 0 && selMC == 0) return;
    /* 4. 原地保留心控条目, 仅清除非心控对象的本地选中标记。 */
    for (i = 0; i < count; i++)
    {
        void* u = items[i];
        if (MS_IsMCd(u))
        {
            items[kept++] = u;
        }
        else if (u && IsPlausiblePtr(u))
        {
            ((unsigned char*)u)[OFF_ISSELECTED] = 0;
        }
    }
    *(int*)(ADDR_CUR_OBJECTS + OFF_VEC_COUNT) = kept;
    /* 5. 屏内心控原生加入 (已在选中里挂 IsSelected=1 的会被引擎去重, 不双收)。 */
    for (i = 0; i < nS; i++) MS_SelectOne(scrMC[i]);
}

/* ---------------- 每循环轮询 (挂 PowerDrawBoth) ---------------- */

void MindSelect_Poll(void)
{
    int combo;

    MS_EnsureInit();
    combo = OU_HotkeyDown(&g_MsHk) ? 1 : 0;
    if (!g_MsEnable) { g_MsPrevCombo = combo; return; }
    if (combo && !g_MsPrevCombo)
        MS_Round();                       /* Hotkey 上升沿: 跑一次收敛 */
    g_MsPrevCombo = combo;
}
