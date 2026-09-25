/* TypeSelect.cpp - T键批量选中建筑 (Phobos BuildingTypeSelectable 移植)。
 *
 * 原版限制: 按T同类选中几乎只对 1x1 且有 UndeploysInto 的建筑有效。
 * 门控在 BuildingTypeClass::IsVehicle (0x465D40): 先查 UndeploysInto (+0x408),
 * 再查地基 1x1 (查表 0x8192B8/0x819310); 经 BuildingClass::CanBeSelectedNow
 * (0x459C00) 被同类匹配 IDMatches (0x732C30) 调用, 而 IDMatches 被
 * TypeSelectExecute 的两段匹配循环调用。
 * 移植做法 (与 Phobos 同原理, 见 references/Phobos/src/Misc/Selection.cpp):
 *   两段循环头 (0x732A85/0x732B28) 置位, 函数唯一出口 (0x732C20) 清零,
 *   窗口内把 0x465D40 直接判真 (EAX=1 后跳到 0x465D6A ret)。
 * Ares 共存 (已查 Ares.dll 二进制): Ares 只钩了 0x732C30 与 0x465D4A
 * (IsVehicle 函数内部), 与本文件四个钩点均无重叠; 置位窗口内跳过 Ares 的
 * 内钩与 Phobos 行为一致 (Phobos 同样跳到 0x465D6A)。
 *
 * 钩子经 dist/MsgFont.dll.inj 由 Syringe 挂载 (寄存器结构见 YRpp Syringe.h,
 * EAX 在 regs+0x24; 返回 0=跑原指令, 非0=跳往该地址):
 *   0x732A85 = TypeSelect_SetCtx, 7 (mov eax,[ebp+4] + mov edx,[esp+14])
 *   0x732B28 = TypeSelect_SetCtx, 6 (mov edx,[ebx+4] + mov esi,[edx+edi*4])
 *   0x465D40 = TypeSelect_BuildingGate, 6 (mov eax,[ecx+408])
 *   0x732C20 = TypeSelect_Clear, 8 (pop edi + pop ebx + add esp,208, 唯一出口)
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MsgFont 同一 DLL)。
 */

#include <windows.h>
#include "GdiText.h"

#define TS_SECTION "TypeSelect"

/* Syringe REGISTERS 布局 (YRpp Syringe.h, MsgFont.cpp 的
 * OFF_REG_ESP=0x14 / OFF_REG_ECX=0x20 可互证, EAX 紧跟 ECX 之后)。 */
#define REG_OFF_EAX 0x24

#define ADDR_ISVEHICLE_SKIP 0x00465D6Au

static int g_TSInit = 0;
static int g_TSEnable = 1;
static volatile LONG g_TSActive = 0;

static void TypeSelect_EnsureInit(void)
{
    if (g_TSInit) return;
    g_TSInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_TSEnable = GetPrivateProfileIntA(TS_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    LogLine(g_TSEnable ? "tselect: building type-select ON"
                       : "tselect: building type-select OFF");
}

/* 两段匹配循环头共用: 置位后跑原指令 (返回 0)。 */
extern "C" __declspec(dllexport) DWORD __cdecl TypeSelect_SetCtx(void* regs)
{
    (void)regs;
    TypeSelect_EnsureInit();
    if (g_TSEnable) g_TSActive = 1;
    return 0;
}

/* TypeSelectExecute 唯一出口 (单 ret 0x732C28 的前置 pop 段): 清零跑原指令。 */
extern "C" __declspec(dllexport) DWORD __cdecl TypeSelect_Clear(void* regs)
{
    (void)regs;
    g_TSActive = 0;
    return 0;
}

/* IsVehicle 门: 窗口外/开关关跑原指令; 窗口内直接判真 (跳过原函数体)。 */
extern "C" __declspec(dllexport) DWORD __cdecl TypeSelect_BuildingGate(void* regs)
{
    TypeSelect_EnsureInit();
    if (!g_TSEnable || !g_TSActive) return 0;
    *(DWORD*)((char*)regs + REG_OFF_EAX) = 1;
    return ADDR_ISVEHICLE_SKIP;
}
