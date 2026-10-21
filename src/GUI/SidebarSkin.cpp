/* SidebarSkin.cpp - 强制指定阵营的侧栏/UI 皮肤 (纯本地视觉, 联机不应用)。
 *
 * 引擎事实:
 *   0x534FA0  bool __fastcall Game::LoadUI(int sideIndex)  (ECX=side)
 *             - 按 side 加载 SIDEC%02d*.MIX, 调色板, tooltip 颜色;
 *             - 0x535347 调 0x6D02B0, 后者调 0x6A5840 重载侧栏 SHP。
 *   0xA8B230  ScenarioClass::Instance, +0x34B8 = PlayerSideIndex。
 *   Ares 的 YuriFileNames 钩子读 Scenario->PlayerSideIndex, 不是 LoadUI 的 ESI,
 *   所以加载时必须临时把 +0x34B8 改成目标值, 加载完立刻恢复。
 *
 * 4 个 0x534FA0 调用点 (0x67E761 / 0x68781F / 0x687833 / 0x6878C5) 的 call
 * 目标改到本文件的 __fastcall 包装: 首次加载时直接把 side 换成目标阵营, 只
 * 加载一次, 不在 InitGUI/setDims 内二次 LoadUI。Ares 的文件名钩子读
 * Scenario->PlayerSideIndex, 所以包装内临时改 +0x34B8, 调用后立刻恢复。
 * 不新增 Syringe hook。 */

#include <windows.h>
#include "GdiText.h"
#include "FullMap.h"
#include "OrderUtil.h"
#include "SidebarSkin.h"

#define SK_SECTION          "Sidebar"
#define SK_KEY              "SkinSide"

#define ADDR_SCENARIO       0x00A8B230u
#define OFF_PLAYER_SIDE     0x34B8u
#define ADDR_SB_REDRAW      0x00B0B518u
#define ADDR_SB_NEEDBG      0x00884B8Fu
#define ADDR_SIDEBAR        0x0087F7E8u
#define ADDR_UI_LIVE_FLAG   0x00A8ED6Bu
#define ADDR_PAL_FREE_JZ    0x006A586Du
#define ADDR_PAL_FREE_JZ2   0x006A5A65u

typedef char (__fastcall *Fn_LoadUI)(int sideIndex);
#define FN_LOAD_UI          ((Fn_LoadUI)(void*)0x00534FA0u)
typedef void (__fastcall *Fn_SidebarReinit)(void* self);
#define FN_SIDEBAR_REINIT   ((Fn_SidebarReinit)(void*)0x006D03A0u)
typedef void (__fastcall *Fn_InitGui)(void* self);
#define FN_INIT_GUI         ((Fn_InitGui)(void*)0x006ABD30u)
typedef void (__fastcall *Fn_InvalidateFile)(const char* name);
#define FN_INVALIDATE_FILE  ((Fn_InvalidateFile)(void*)0x005B4270u)

/* 0x6D04F0(this, 1): this=ECX, 参数在栈上, ret 4。 */
static char Sk_SidebarUpdate(void* self, int arg) __attribute__((naked));
static char Sk_SidebarUpdate(void* self, int arg)
{
    (void)self; (void)arg;
    __asm__ volatile (
        "mov 8(%esp), %eax\n\t"
        "mov 4(%esp), %ecx\n\t"
        "push %eax\n\t"
        "mov $0x006D04F0, %eax\n\t"
        "call *%eax\n\t"
        "ret\n\t");
}

/* 0x6A6C30(this, force): this=ECX, force 在栈上, ret 4。
 * 只在 A8ED6B=0 时才会真正重画背景 SIDE1/2/3; 切换后必须补画一次,
 * 否则引擎继续显示旧阵营的缓存侧栏背景。 */
static void Sk_SidebarDraw(void* self, int force) __attribute__((naked));
static void Sk_SidebarDraw(void* self, int force)
{
    (void)self; (void)force;
    __asm__ volatile (
        "mov 8(%esp), %eax\n\t"
        "mov 4(%esp), %ecx\n\t"
        "push %eax\n\t"
        "mov $0x006A6C30, %eax\n\t"
        "call *%eax\n\t"
        "ret\n\t");
}

static const unsigned k_LoadUiCalls[] =
{
    0x0067E761u, 0x0068781Fu, 0x00687833u, 0x006878C5u
};

/* 侧栏绘制/命中区里 0x1a/0x12 的布局偏移、滚动区、按钮矩形都按
 * Scenario->PlayerSideIndex 选。强制皮肤时须按目标阵营, 否则布局与已加载的
 * SHP 不一致, 侧栏会花屏/点错按钮。这里把侧栏代码里所有
 * `mov 0x34b8(reg), dst` 统一改成 `mov [s_SkinSide], dst`。 */
static int s_SkinSide = 0;       /* 当前 UI 用的阵营 (绘制面读取) */

static const unsigned k_SideReads[] =
{
    0x006A5095u, 0x006A51EFu, 0x006A51FFu, 0x006A6629u, 0x006A66F0u,
    0x006A6954u, 0x006A6A60u, 0x006A6B29u, 0x006A6D5Bu, 0x006A74E5u,
    0x006A763Du, 0x006A79C4u, 0x006A7B57u, 0x006A7C96u, 0x006A7F12u,
    0x006A822Du, 0x006A8338u, 0x006A888Bu, 0x006A8948u, 0x006A89ECu,
    0x006A8B60u, 0x006A9400u, 0x006A95F3u, 0x006AA62Eu, 0x006ABF15u,
    0x006AC350u, 0x006AC3E5u, 0x006AC435u, 0x006D035Cu,
    0x00652EB1u, 0x0063FB5Du, 0x006403DFu, 0x006403FFu
};

/* `cmp [Scenario+0x34b8], 2` = 83 B8 B8 34 00 00 02 (7 字节)。
 * 这是 UI 资源/调色板选择 Yuri 分支的唯一开关:
 *   0x72F27A/0x72F370  0x72F270/0x72F350  LoadUI 资源选集
 *   0x72FA1A           0x72FA10            RADAR/BKGD 的 Yuri 变体
 *   0x72FBC5           0x72FBC0            UIBKGDY/RADARYURI 调色板
 *   0x73067A           0x730670            同上 (另一入口)
 * 改成 `cmp [s_SkinSide], 2` = 83 3D imm32 02, 长度不变。
 * 不 patch 的话: 实际阵营=Yuri 而主题=盟军时, 会加载 Yuri 皮肤 + Yuri 调色板,
 * 但布局按主题算, 必然花屏。 */
static const unsigned k_SideCmps[] =
{
    0x0072F27Au, 0x0072F370u, 0x0072FA1Au, 0x0072FBC5u, 0x0073067Au
};

static int   s_Init = 0;
static int   s_Force = 0;          /* 0=默认, 1..4=强制阵营 (持久设置/ESC) */
static int   s_ActionForce = -1;   /* 触发器临时覆盖: -1=无, 0..4=Action 900 参数 */
static int   s_SidePatch = 0;
static int   s_CmpPatch = 0;
static int   s_Applying = 0;
static int   s_Pending = 0;
static int   s_ActionLocal = 0;    /* pending switch came from Action 900 (allowed in MP) */

static int Sk_EffectiveForce(void)
{
    return (s_ActionForce >= 0) ? s_ActionForce : s_Force;
}

/* 0x5B40B0 的文件缓存按文件名 hash 命中后直接返回旧 buffer。
 * 0x534FA0 里的 0x72F350 会先 0x730100/0x730400 释放上一阵营的 UI SHP/PAL,
 * 再 0x72FA10/0x72ADE0 从磁盘重载。若只清 SIDE1/2/3, 其余同名资源仍命中
 * 缓存, 拿到的是已 free 的旧 buffer -> 侧栏/雷达/顶部栏花屏。
 * 0x5B4270(name) 只清缓存项, 不释放数据。
 * 0x72FA10: RADAR/TOP/CREDITS/BKGD.../SIDE.../ADDON/SIDEBTTN/... (见 kNames)
 * 0x72F350: SIDEBAR/UIBKGD/UIBKGDY/RADARYURI/OBSERVER/YRII.PAL
 * 0x6A5840: GCLOCK2/SELL/REPAIR/R-UP/R-DN/TAB00-03/SIDE1-3/ADDON/OBSALLI
 * 0x6D02B0: Button00-24.SHP
 * 0x63F7C0: POWERP.SHP */
static void Sk_InvalidateSideSHP(void)
{
    static const char* const kNames[] =
    {
        "POWERP.SHP",
        "RADAR.SHP", "RADARY.SHP", "TOP.SHP", "CREDITS.SHP",
        "BKGDLG.SHP", "BKGDLGY.SHP", "BKGDMD.SHP", "BKGDMDY.SHP",
        "BKGDSM.SHP", "BKGDSMY.SHP", "SIDEBTTN.SHP", "SIDE2B.SHP",
        "LSPACER.SHP", "LENDCAP.SHP", "RENDCAP.SHP", "BTTNBKGD.SHP",
        "FSALG.SHP", "DIPLOBTN.SHP", "OPTBTN.SHP",
        "SDBTM.SHP", "LWSCRNS.SHP", "LWSCRNL.SHP",
        "GCLOCK2.SHP", "SELL.SHP", "REPAIR.SHP", "R-UP.SHP", "R-DN.SHP",
        "TAB00.SHP", "TAB01.SHP", "TAB02.SHP", "TAB03.SHP",
        "SIDE1.SHP", "SIDE2.SHP", "SIDE3.SHP", "ADDON.SHP", "OBSALLI.SHP",
        "SIDEGDI1.SHP", "SIDEGDI2.SHP", "SIDEGDI3.SHP",
        "SIDEBAR.PAL", "UIBKGD.PAL", "UIBKGDY.PAL", "RADARYURI.PAL",
        "OBSERVER.PAL", "YRII.PAL", "SDBTNANM.PAL", "SHELL2.PAL", "FSSCRN.PAL"
    };
    char name[16];
    unsigned i;
    int n;

    for (i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        FN_INVALIDATE_FILE(kNames[i]);

    for (n = 0; n < 25; ++n)
    {
        name[0] = 'B'; name[1] = 'u'; name[2] = 't'; name[3] = 't';
        name[4] = 'o'; name[5] = 'n';
        name[6] = (char)('0' + n / 10);
        name[7] = (char)('0' + n % 10);
        name[8] = '.'; name[9] = 'S'; name[10] = 'H'; name[11] = 'P';
        name[12] = 0;
        FN_INVALIDATE_FILE(name);
    }
}

static void Sk_Reload(int target, int actual, void* scen)
{
    int ok;

    if (s_Applying) return;
    s_Applying = 1;

    s_SkinSide = target;

    *(int*)((char*)scen + OFF_PLAYER_SIDE) = target;
    Sk_InvalidateSideSHP();
    ok = FN_LOAD_UI(target) ? 1 : 0;

    if (ok)
    {
        unsigned char* live = (unsigned char*)(size_t)ADDR_UI_LIVE_FLAG;
        unsigned char oldLive = *live;

        *live = 0;
        FN_SIDEBAR_REINIT((void*)(size_t)ADDR_SIDEBAR);
        Sk_SidebarUpdate((void*)(size_t)ADDR_SIDEBAR, 1);
        FN_INIT_GUI((void*)(size_t)ADDR_SIDEBAR);
        Sk_SidebarDraw((void*)(size_t)ADDR_SIDEBAR, 1);
        *live = oldLive;

        *(volatile int*)(size_t)ADDR_SB_REDRAW = 1;
        *(volatile unsigned char*)(size_t)ADDR_SB_NEEDBG = 1;
        FM_SyncSidebarColumn();
    }
    else
    {
        LogLine("skin: load failed");
    }

    if (!ok) s_SkinSide = actual;
    *(int*)((char*)scen + OFF_PLAYER_SIDE) = actual;
    s_Applying = 0;
}

/* 引擎首次加载 UI 时直接换 side。相比在 InitGUI 里二次 LoadUI:
 *   - 只加载一次, 不会把 setDims 正在用的侧栏 SHP 换掉;
 *   - 新局/读档的布局仍由引擎按正常顺序算。 */
static char __fastcall Sk_LoadUISide(int side) __attribute__((noinline));
static char __fastcall Sk_LoadUISide(int side)
{
    void* scen;
    int actual, target;
    char ok;

    SidebarSkin_EnsureInit();

    /* Action 900 只改变当前这局的显示, 不是持久设置。新局/读档重新走
     * LoadUI 时丢弃临时覆盖, 回到 INI/ESC 的 SkinSide。 */
    s_ActionForce = -1;
    s_ActionLocal = 0;
    s_Pending = 0;

    if (!s_Force)
    {
        s_SkinSide = side;
        return FN_LOAD_UI(side);
    }
    if (OU_IsMP())
    {
        s_SkinSide = side;
        return FN_LOAD_UI(side);
    }

    scen = *(void**)ADDR_SCENARIO;
    if (!scen || !IsPlausiblePtr(scen))
    {
        s_SkinSide = side;
        return FN_LOAD_UI(side);
    }

    actual = *(int*)((char*)scen + OFF_PLAYER_SIDE);
    if (actual < 0 || actual > 3) actual = 0;
    target = s_Force - 1;
    s_SkinSide = target;

    *(int*)((char*)scen + OFF_PLAYER_SIDE) = target;
    Sk_InvalidateSideSHP();
    ok = FN_LOAD_UI(target) ? 1 : 0;
    *(int*)((char*)scen + OFF_PLAYER_SIDE) = actual;
    if (!ok) s_SkinSide = actual;

    return ok;
}

static void Sk_PatchCall(unsigned addr, void* fn)
{
    unsigned char* p = (unsigned char*)(size_t)addr;
    unsigned char* cur;
    DWORD old;
    int rel;

    if (p[0] != 0xE8) return;
    cur = p + 5 + *(int*)(p + 1);
    if (cur != (unsigned char*)FN_LOAD_UI) return;

    rel = (int)((char*)fn - (char*)(addr + 5));
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = 0xE8;
    *(int*)(p + 1) = rel;
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
}

/* `mov 0x34b8(reg), dst` = 8B modrm B8 34 00 00 (6 字节)。
 * 改成 `mov [s_SkinSide], dst` = 8B (05|dst<<3) imm32, 长度不变, 不用 NOP。 */
static void Sk_PatchSideRead(unsigned addr)
{
    unsigned char* p = (unsigned char*)(size_t)addr;
    DWORD old;
    unsigned dst;

    if (p[0] != 0x8B) return;
    if ((p[1] & 0xC0) != 0x80) return;      /* mod=10: disp32 */
    if ((p[1] & 0x07) == 0x04) return;      /* SIB, 不符合原指令 */
    if (p[2] != 0xB8 || p[3] != 0x34 || p[4] != 0x00 || p[5] != 0x00) return;

    dst = (unsigned)((p[1] >> 3) & 0x07);
    if (!VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &old)) return;
    p[1] = (unsigned char)(0x05 | (dst << 3));
    *(unsigned*)(p + 2) = (unsigned)(size_t)&s_SkinSide;
    VirtualProtect(p, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 6);
    s_SidePatch++;
}

static void Sk_PatchSideCmp(unsigned addr)
{
    unsigned char* p = (unsigned char*)(size_t)addr;
    DWORD old;

    if (p[0] != 0x83 || p[1] != 0xB8) return;
    if (p[2] != 0xB8 || p[3] != 0x34 || p[4] != 0x00 || p[5] != 0x00) return;
    if (p[6] != 0x02) return;

    if (!VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old)) return;
    p[1] = 0x3D;
    *(unsigned*)(p + 2) = (unsigned)(size_t)&s_SkinSide;
    VirtualProtect(p, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 7);
    s_CmpPatch++;
}

static void Sk_SkipPaletteFree(unsigned addr)
{
    unsigned char* p = (unsigned char*)(size_t)addr;
    DWORD old;

    if (p[0] != 0x74 || p[1] != 0x0C) return;
    if (!VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = 0xEB;
    VirtualProtect(p, 2, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 2);
}

static void Sk_KeepOldPalette(void)
{
    Sk_SkipPaletteFree(ADDR_PAL_FREE_JZ);
    Sk_SkipPaletteFree(ADDR_PAL_FREE_JZ2);
}

static void Sk_InstallLoadPatches(void)
{
    unsigned i;

    for (i = 0; i < sizeof(k_LoadUiCalls) / sizeof(k_LoadUiCalls[0]); i++)
        Sk_PatchCall(k_LoadUiCalls[i], (void*)&Sk_LoadUISide);
    for (i = 0; i < sizeof(k_SideReads) / sizeof(k_SideReads[0]); i++)
        Sk_PatchSideRead(k_SideReads[i]);
    for (i = 0; i < sizeof(k_SideCmps) / sizeof(k_SideCmps[0]); i++)
        Sk_PatchSideCmp(k_SideCmps[i]);
    Sk_KeepOldPalette();
}

void SidebarSkin_EnsureInit(void)
{
    if (s_Init) return;
    BuildPaths();
    s_Force = GetPrivateProfileIntA(SK_SECTION, SK_KEY, 0, g_IniPath);
    if (s_Force < 0 || s_Force > 4) s_Force = 0;
    Sk_InstallLoadPatches();
    if (s_Force &&
        (s_SidePatch != sizeof(k_SideReads) / sizeof(k_SideReads[0]) ||
         s_CmpPatch != sizeof(k_SideCmps) / sizeof(k_SideCmps[0])))
    {
        LogLine("skin: patch miss, feature disabled");
        s_Force = 0;
    }
    s_Init = 1;
}

void SidebarSkin_Poll(void)
{
    void* scen;
    int actual, target, force;

    SidebarSkin_EnsureInit();
    force = Sk_EffectiveForce();
    if (!force)
    {
        void* sc = *(void**)ADDR_SCENARIO;
        if (sc && IsPlausiblePtr(sc))
        {
            int a = *(int*)((char*)sc + OFF_PLAYER_SIDE);
            if (a >= 0 && a <= 3) s_SkinSide = a;
        }
    }
    if (!s_Pending || s_Applying) return;

    if (OU_IsMP() && !s_ActionLocal)
    {
        s_Pending = 0;
        s_ActionLocal = 0;
        return;
    }

    scen = *(void**)ADDR_SCENARIO;
    if (!scen || !IsPlausiblePtr(scen)) return;

    actual = *(int*)((char*)scen + OFF_PLAYER_SIDE);
    if (actual < 0 || actual > 3) actual = 0;
    target = force ? force - 1 : actual;

    Sk_Reload(target, actual, scen);
    s_Pending = 0;
    s_ActionLocal = 0;
}

/* 返回持久设置 (INI/ESC), 不返回 Action 900 的临时覆盖。 */
int SidebarSkin_Get(void)
{
    SidebarSkin_EnsureInit();
    return s_Force;
}

void SidebarSkin_Set(int value)
{
    SidebarSkin_EnsureInit();
    if (value < 0 || value > 4) value = 0;
    s_Force = value;
    s_ActionForce = -1;
    s_ActionLocal = 0;

    if (OU_IsMP())
    {
        s_Pending = 0;
        return;
    }

    /* 默认延迟到主循环稳定点应用; ESC 设置页会立即 Poll 以便预览。 */
    s_Pending = 1;
}

/* Action 900 switch: local-only visual, so it is allowed in multiplayer.
 * 只覆盖当前场景的显示, 不写 s_Force/INI; 新局或读档时由 Sk_LoadUISide 丢弃。 */
void SidebarSkin_SetAction(int value)
{
    SidebarSkin_EnsureInit();
    if (value < 0 || value > 4) return;
    s_ActionForce = value;
    s_ActionLocal = 1;
    s_Pending = 1;
}

/* Syringe hook at TActionClass::Execute (0x6DD8B0, 6 bytes).
 * Unknown actions are safely ignored by the engine, so we only need to read
 * the action and let the original prologue/epilogue run. Return 0 = original. */
extern "C" __declspec(dllexport) DWORD __cdecl TAction_UISkin(void* regs)
{
    unsigned char* r = (unsigned char*)regs;
    void* self;
    int value;

    if (!r) return 0;
    self = *(void**)(r + 0x20); /* ECX = TActionClass* */
    if (!self || !IsPlausiblePtr(self)) return 0;
    if (*(int*)((char*)self + 0x2C) != 900) return 0;

    value = *(int*)((char*)self + 0x90);
    if (value < 0 || value > 4) return 0;

    if (Sk_EffectiveForce() != value || (value != 0 && s_SkinSide != value - 1))
        SidebarSkin_SetAction(value);

    return 0;
}
