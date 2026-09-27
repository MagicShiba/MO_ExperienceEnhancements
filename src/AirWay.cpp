/* AirWay.cpp - 飞行器路径点开关 (Phobos ExtendedAircraftMissions 移植其一)。
 *
 * 原版限制: AircraftTypeClass::CanUseWaypoint (虚表槽 0x7E2908) 恒返 false,
 * 路径点模式键只对地面单位开放。本模块只把这一道 UI 门打开:
 * DllMain 经 AirWay_Install 在虚表槽写入 AirWay_CanUseWaypoint (无参,
 * ECX=this 照收忽略, 恒返 1, 与 thiscall 兼容); 开关关时不碰虚表。
 * 排队/规划/返航等行为全走原版公共代码, 到站返航摆头属原版行为, 不动。
 *
 * 联机: 门只管发令侧 UI, 发出的是原版事件, 各端用同一份原版代码执行,
 * 不影响联机 (对方只是自己发不出这种命令)。
 * Ares 共存: 若 Ares 也改写同槽, 日志 old 值会暴露 (last writer wins)。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MOExperience 同一 DLL)。
 */

#include <windows.h>
#include "GdiText.h"
#include "AirWay.h"

#define AW_SECTION "AirWay"
#define AW_VTABLE_SLOT ((void**)0x007E2908u)

static int g_AirWayEnable = 0;
static int g_AirWayPatched = 0;

/* 路径点门: 恒返 1。
 * 被叫时 ECX=this, 栈上无参数, ret 直接兼容 thiscall。 */
extern "C" int __cdecl AirWay_CanUseWaypoint(void)
{
    return 1;
}

void AirWay_Install(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    BuildPaths();
    GtEnsureLog();
    g_AirWayEnable =
        GetPrivateProfileIntA(AW_SECTION, "Enable", 0, g_IniPath) ? 1 : 0;
    if (!g_AirWayEnable)
    {
        LogLine("airway: aircraft waypoints OFF");
        return;
    }
    LogLine("airway: aircraft waypoints ON (patch lazy)");
}

/* 延迟补丁 (首个 Poll 时, 每循环必跑, exe 已解包稳定)。
 * 只在成功后标记, 失败下帧重试 (SPAWN 进程早期内存尚不可写)。 */
static void AirWay_EnsurePatched(void)
{
    char buf[96], b1[16];
    unsigned q;
    void* old;
    DWORD prot;
    if (g_AirWayPatched) return;
    old = *AW_VTABLE_SLOT;
    if (old == (void*)AirWay_CanUseWaypoint)
    {
        g_AirWayPatched = 1;
        LogLine("airway: vtable already patched");
        return;
    }
    if (!IsPlausiblePtr(old) && old != 0)
    {
        LogLine("airway: vtable unreadable, retry later");
        return;
    }
    if (!VirtualProtect((void*)AW_VTABLE_SLOT, 4, PAGE_EXECUTE_READWRITE, &prot))
    {
        LogLine("airway: vtable protect failed, retry later");
        return;
    }
    *AW_VTABLE_SLOT = (void*)AirWay_CanUseWaypoint;
    VirtualProtect((void*)AW_VTABLE_SLOT, 4, prot, &prot);
    FlushInstructionCache(GetCurrentProcess(), (void*)AW_VTABLE_SLOT, 4);
    g_AirWayPatched = 1;
    q = 0;
    q = AppendCStr(buf, q, "airway: vtable patched old=", sizeof(buf));
    HtoA((unsigned)(size_t)old, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    LogLine(buf);
}

void AirWay_Poll(void)
{
    if (!g_AirWayEnable) return;
    AirWay_EnsurePatched();
}
