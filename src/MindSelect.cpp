/* MindSelect.cpp - Shift+P 结尾只剩心控单位 (收敛式过滤)。
 *
 * 背景: 原生 Shift+P 也会走全选 (P 处理无视 Shift, 与 CursorCheat 无关),
 * 所以单次加法/单次过滤都会被原生全选盖掉, 必须多轮收敛。
 * 用法: 点按 Shift+P (别长按), 结尾选中只剩心控单位。
 * 作用域继承当前选中 (P 一次屏内/两次全图, 原生先全选, 本模块再收)。
 *
 * 每轮操作 (Shift+P 上升沿跑一次: 原生全选在消息泵同步落地,
 * 渲染钩子看到边沿时已生效, 无需连打):
 *   1. 屏内 selectables 扫心控 → 原生 Select() 加入 (加法, 非心控零触碰)。
 *   2. 当前选中里非心控 → 清 IsSelected 标记并原地压缩 (只摘杂, 心控不动)。
 *   3. 两处都无心控 → 碰都不碰 (不清空原选中, 只记日志)。
 *
 * 数据源 (与 P/框选同源, 见 Phobos Misc/Selection.cpp):
 *   TacticalSelectables @0xB0CEC8 (500 项定长, 逐项跳过空/野指针,
 *   不依赖 SelectableCount 偏移); 条目 {Object*, X, Y} (YRpp TacticalClass.h:13)。
 *   CurrentObjects @0xA8ECB8 (YRpp ObjectClass.h:38; +4 Items/+8 Cap/+16 Count)。
 *
 * 引擎调用 (二进制实证过, v3/v6 已进游戏验证可用):
 *   IsMindControlled = 0x7105E0 (机码 8B 81 C0 02 00 00 / 85 C0 ...,
 *     return [ecx+0x2C0] || byte[ecx+0x2C4], 纯读)。
 *   Select() = vtable+0x14C (UnselectAll=0x48DC90 机码逐个 call [vtable+0x150]
 *     即 Deselect, Select 为前一槽; YRpp 声明顺序一致)。
 *   IsSelectable() = vtable+0x64 (实现 0x6F32D0, 纯读; 槽位锚点: 同表
 *     +0x7C=IsSurfaced/+0xAC=GetRenderCoords/+0xB8=GetYSort/+0xE8=
 *     SpawnParachuted 均与 YRpp 记的函数地址对上, +0x48=GetCoords)。
 *   MindControlledByAUnit = +0x2C4 (bool 字节; 0x53B2A2 永久心控处
 *     mov byte [esi+0x2C4],1, 0x6F8372 索敌处同查 +0x2C0/+0x2C4)。
 * 除此之外零引擎调用: 不扫三军数组, 不校准 Owner, 不碰模拟内存。
 * 选中是本机 UI 状态, 无模拟事件, 联机安全。
 *
 * 判定口径对齐 Phobos Select Captured Units (Commands/SelectCaptured.cpp:50):
 *   IsMindControlled() && IsSelectable() && !MindControlledByAUnit
 * → 排掉永久心控 (MCV/永久心灵支配者) 与引擎判不可选的 (工程兵/医疗兵等
 *   IsSelectableCombatant=no, 建筑的 WhatAmI 判定也在引擎内自带)。
 *
 * 地址出处 (全只读, 写模拟一律禁止):
 *   IsAlive +0x90 / InLimbo +0x81 / IsSelected +0x83 / Health +0x6C
 *   (YRpp ObjectClass.h Westwood 偏移命名)。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"

#define MS_SECTION "MindSelect"

/* ---------------- 私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define ADDR_SELECTABLES 0xB0CEC8u   /* Unsorted::TacticalSelectables (YRpp Unsorted.h:652) */
#define SEL_CAP          500u        /* DEFINE_ARRAY_REFERENCE 上限 */
#define OFF_SEL_NEXT     12u         /* {Object*, X, Y} (YRpp TacticalClass.h:13) */

#define ADDR_CUROBJ    0xA8ECB8u     /* ObjectClass::CurrentObjects (当前选中) */
#define OFF_VEC_ITEMS  4u
#define OFF_VEC_CAP    8u
#define OFF_VEC_COUNT  16u

#define OFF_ISALIVE    0x90u
#define OFF_INLIMBO    0x81u
#define OFF_ISSELECTED 0x83u
#define OFF_HEALTH     0x6Cu
#define OFF_MCBYAUNIT  0x2C4u    /* TechnoClass::MindControlledByAUnit (bool 字节) */

#define VT_SELECT       (0x14Cu / 4u)   /* 见文件头推导 */
#define VT_ISSELECTABLE (0x64u / 4u)    /* ObjectClass::IsSelectable */

typedef int __attribute__((thiscall)) (*Fn_IsMindControlled)(void* self);
typedef int __attribute__((thiscall)) (*Fn_IsSelectable)(void* self);
typedef int __attribute__((thiscall)) (*Fn_Select)(void* self);

#define FN_ISMC ((Fn_IsMindControlled)(void*)0x7105E0u)

#define MS_MC_CAP 256   /* 单轮心控收集上限 (超限截断) */

/* ---------------- 配置 ---------------- */

static int g_MsInit = 0;
static int g_MsEnable = 1;
static OU_Hotkey g_MsHk;      /* Hotkey=Shift+P */
static int g_MsPrevCombo = 0;

static void MS_EnsureInit(void)
{
    if (g_MsInit) return;
    g_MsInit = 1;
    BuildPaths();
    g_MsEnable = GetPrivateProfileIntA(MS_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    OU_ParseHotkey("Shift+P", &g_MsHk);           /* 默认 (必成功) */
    OU_ReadHotkey(MS_SECTION, "Hotkey", &g_MsHk); /* 有则覆盖, 坏串留默认 */
    if (g_MsHk.disabled) g_MsEnable = 0;
}

/* ---------------- 判定 + 单步 ---------------- */

/* 引擎可选性 (纯读调用, 对应 Phobos SelectCaptured 里的 pTechno->IsSelectable()):
 * Health>0 && !InLimbo && WhatAmI!=Building && Type->IsSelectableCombatant
 * && Owner->IsControlledByCurrentPlayer() (0x50B6F0: 联机比 CurrentPlayer
 * 0xA83D4C, 单机读 House+0x1EC/+0x1ED)。心控时 Owner 已被改成控制方,
 * 故心控单位能过这条。 */
static int MS_IsSelectable(void* u)
{
    void** vt;
    Fn_IsSelectable fn;
    vt = *(void***)u;                     /* 调用方已验 u 合理 */
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    fn = (Fn_IsSelectable)vt[VT_ISSELECTABLE];
    /* 槽坏时按"可选"放行 (OU_WhatAmI 已成功走过该槽); MS_SelectOne 会自校验。 */
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
    if (!((unsigned char*)u)[OFF_ISALIVE]) return 0;
    if (((unsigned char*)u)[OFF_INLIMBO]) return 0;
    if (*(int*)((char*)u + OFF_HEALTH) <= 0) return 0;
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

    /* 1. 屏内心控收集 (加法源, 非心控零触碰)。 */
    for (i = 0; i < (int)SEL_CAP && nS < MS_MC_CAP; i++)
    {
        void* u = *(void**)(ADDR_SELECTABLES + (unsigned)i * OFF_SEL_NEXT);
        if (MS_IsMCd(u)) scrMC[nS++] = u;
    }
    /* 2. 当前选中清点 (只读, 不动)。 */
    items = *(void***)(ADDR_CUROBJ + OFF_VEC_ITEMS);
    cap = *(int*)(ADDR_CUROBJ + OFF_VEC_CAP);
    count = *(int*)(ADDR_CUROBJ + OFF_VEC_COUNT);
    if (!items || !IsPlausiblePtr(items)) return;
    if (cap <= 0 || cap > 8192 || count < 0 || count > cap) return;
    for (i = 0; i < count; i++)
        if (MS_IsMCd(items[i])) selMC++;
    /* 3. 两处都无心控 → 碰都不碰 (不清空原选中)。 */
    if (nS == 0 && selMC == 0) return;
    /* 4. 选中去杂 (心控前移保留, 非心控摘标记; 先收后加, 条目数只增不乱)。 */
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
    *(int*)(ADDR_CUROBJ + OFF_VEC_COUNT) = kept;
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
