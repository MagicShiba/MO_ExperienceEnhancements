/* SyringeHooks.cpp - Syringe 内嵌钩子声明 (免 .dll.inj, 参考 AutoLoadDev 做法)。
 *
 * 原理 (见 AutoLoadDev/notes/HANDBOOK.md §1 与 old_NOTES.md §13):
 * PE 段名必须恰好是 ".syhks00" (8 字节精确匹配), 16 字节一条
 *   { hookAddr, hookSize, 导出函数名字符串指针, 0 }。
 * Syringe 枚举段内条目, 按名字 GetProcAddress 后挂钩, 不再需要
 * dist/MOExperience.dll.inj 与游戏目录的 MOExperience.dll.inj。
 * 名称指针是绝对 VA, 需要重定位项 (GCC 默认自带 .reloc 覆盖)。
 *
 * 双源同步铁律: 本表与 dist/MOExperience.dll.inj 内容必须一致
 * (地址/长度/导出名三者逐行对应, 顺序也保持一致)。
 * 改任何钩子时两处一起改, 改完跑 tools/hook-rel-audit.py
 * (它现在同时校验本表与 .inj 是否一致, 见该脚本尾部)。
 * 游戏目录只放 MOExperience.dll, 不得再放 MOExperience.dll.inj,
 * 否则同一钩子被声明两次 (AutoLoadDev 同款教训)。
 *
 * SyringeHandshake 导出: 仅做结构自校验 + 回填提示串, 不做 exeCRC
 * 把关 (重打包变体 CRC 不同但映像一致, 见 AutoLoadDev 注释)。
 * 它在 Syringe.exe 进程里被调用 (DLL 被 LoadLibrary 进 Syringe 做握手),
 * 所以函数体内禁止碰任何引擎地址, 只填消息即返回。
 */

#include <windows.h>
#include "MOEVersion.h"   /* MOE_VERSION (握手提示串用) */

/* ---------------- 内嵌钩子表 ---------------- */

struct SyHookDecl
{
    unsigned int addr;
    unsigned int size;
    const char* name;
    unsigned int pad;
};

#define SYHOOK(a, s, n) { (a), (s), (n), 0u }

/* 与 dist/MOExperience.dll.inj 逐行对应 (40 项, 顺序一致)。 */
static SyHookDecl g_syHooks[] __attribute__((section(".syhks00"), used, aligned(16))) =
{
    SYHOOK(0x5D3BA0u, 6u, "MsgText_AddMessageHook"),
    SYHOOK(0x5D49A0u, 6u, "MOExperience_MsgListEntry"),
    SYHOOK(0x5D49B4u, 3u, "ChatEdit_HideNative"),
    SYHOOK(0x5D4A94u, 7u, "MOExperience_MessageDrawHook"),
    SYHOOK(0x777998u, 6u, "MOExperience_WheelHook"),
    SYHOOK(0x533F50u, 5u, "MOExperience_SidebarHook"),
    SYHOOK(0x55E477u, 5u, "ChatEdit_ChatSp1"),
    SYHOOK(0x55E62Fu, 6u, "ChatEdit_ChatSp2"),
    SYHOOK(0x55E693u, 6u, "ChatEdit_ChatSp3"),
    SYHOOK(0x55E746u, 5u, "ChatEdit_ChatSp4"),
    SYHOOK(0x732A85u, 7u, "TypeSelect_SetCtx"),
    SYHOOK(0x732B28u, 6u, "TypeSelect_SetCtx"),
    SYHOOK(0x465D40u, 6u, "TypeSelect_BuildingGate"),
    SYHOOK(0x732C20u, 8u, "TypeSelect_Clear"),
    SYHOOK(0x6AB689u, 5u, "BQ_QueueClick"),
    SYHOOK(0x6AB0C1u, 6u, "BQ_PausePeel"),
    SYHOOK(0x6AB197u, 8u, "BQ_CancelPeel"),
    SYHOOK(0x6AAE44u, 6u, "BQ_DarkDispatchPeel"),
    SYHOOK(0x6AAFE1u, 5u, "BQ_DarkPauseSeen"),
    SYHOOK(0x6AB6CEu, 10u, "BQ_FreeShift"),
    SYHOOK(0x6A9C86u, 5u, "BQ_ShowCount"),
    SYHOOK(0x6A9C6Cu, 6u, "BQ_DrawNullFac"),
    SYHOOK(0x6A93D9u, 5u, "ToolTip_Extend"),
    SYHOOK(0x478FDCu, 5u, "ToolTip_Clamp"),
    SYHOOK(0x4346C0u, 5u, "ToolTip_AdvFix"),
    SYHOOK(0x692419u, 7u, "SWS_ClickGate"),
    SYHOOK(0x692406u, 6u, "FullMap_RadarClickGate"),
    SYHOOK(0x536FA0u, 7u, "RepairAll_HotKey"),
    SYHOOK(0x6A78D9u, 6u, "RepairAll_Button"),
    SYHOOK(0x55D360u, 5u, "PV_MainGate"),
    SYHOOK(0x69234Bu, 5u, "MsgText_RadarClickGate"),
    SYHOOK(0x44377Eu, 6u, "RallyPath_GateHook"),
    SYHOOK(0x443C81u, 7u, "RallyPath_ExitHook"),
    SYHOOK(0x4D77BDu, 6u, "NoApproach_Hook"),
    SYHOOK(0x4AC310u, 6u, "PiP_LeftDownGate"),
    SYHOOK(0x4AB9B0u, 6u, "PiP_LeftUpGate"),
    SYHOOK(0x6DA180u, 6u, "PiP_BandGate"),
    SYHOOK(0x4F1140u, 5u, "EscMenu_OnShow"),
    SYHOOK(0x622396u, 6u, "EscMenu_OnPaintSurface"),
    SYHOOK(0x55DDA0u, 5u, "PlayerColor_TickHook"),
};

/* ---------------- Syringe 握手 ---------------- */

struct SyringeHandshakeInfo
{
    int cbSize;
    int num_hooks;
    unsigned int checksum;
    DWORD exeFilesize;
    DWORD exeTimestamp;
    unsigned int exeCRC;
    int cchMessage;
    char* Message;
};

extern "C" __declspec(dllexport) HRESULT __cdecl SyringeHandshake(SyringeHandshakeInfo* pInfo)
{
    /* "MOExperience " + 版本 + " (embedded hooks)" 手工拼接 (无 CRT)。 */
    static const char kHead[] = "MOExperience ";
    static const char kTail[] = " (embedded hooks, no .inj needed)";
    const char* ver = MOE_VERSION;
    int i = 0, k;
    if (!pInfo || pInfo->cbSize < (int)sizeof(SyringeHandshakeInfo))
        return E_FAIL;
    if (pInfo->Message && pInfo->cchMessage > 0)
    {
        for (k = 0; kHead[k] && i < pInfo->cchMessage - 1; k++)
            pInfo->Message[i++] = kHead[k];
        for (k = 0; ver[k] && i < pInfo->cchMessage - 1; k++)
            pInfo->Message[i++] = ver[k];
        for (k = 0; kTail[k] && i < pInfo->cchMessage - 1; k++)
            pInfo->Message[i++] = kTail[k];
        pInfo->Message[i] = '\0';
    }
    return S_OK;
}
