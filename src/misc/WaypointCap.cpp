/* WaypointCap.cpp - 同路线分支 16 上限 + 下令前检查 128 上限, 单机解除。
 * 单机专用, 联机完全绕过 (实时判定: 不装新钩, 已装的恢复原版字节)。
 *
 * 原版两处拒收:
 *  - 0x6384DC `cmp $0x10,%eax / jne`: 同路线第 17 个单位起拒收。
 *  - 0x6391A7 `cmp $0x80,%ebp / jle`: 规划模式下分支数+新单位数 >128 整单拒收。
 * 将满提示 0x6383B4 一并搬到上限-1。
 *
 * 单机同手势永不分帧 (见 OrderThrottle.cpp): 本模块只放行上限检查,
 * 溢出由派发侧直写 DoList。
 *
 * 配置 [Features] WaypointCap (缺失=0, 改后重启):
 *   0=单机不限制 (默认), N>0=单机上限 N (最大 4096)。联机恒 16/128。
 *
 * 引擎事实见 notes/路径点上限.md。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "WaypointCap.h"

#define WP_SECTION "Features"

#define ADDR_WP_CHECK  0x006384DCu
#define ADDR_WP_NORMAL 0x00638477u   /* 原 jne 目标: 正常建分支 */
#define ADDR_WP_REFUSE 0x006384E1u   /* 原相等落点: 播音效拒收返 0 */

#define ADDR_WP_WARN   0x006383B4u
#define ADDR_WP_WARNGO 0x006383BAu   /* 原相等落点: 存状态 2 (UI 播上限音) */
#define ADDR_WP_WARNNO 0x006383C4u   /* 原 jne 目标: 继续 */

/* 0x639130 尾 (下令前检查): ebp=本家分支数+新单位数。单机恒过,
 * 联机恒 128。壳内照原样先 pop 再跳, 两目标栈与原版一致。 */
#define ADDR_WP_ORDER   0x006391A7u
#define ADDR_WP_ORDEROK 0x00639203u
#define ADDR_WP_ORDERNO 0x006391B1u
#define WP_ORDER_SP 0x7FFFFFFF   /* 单机下令前检查恒过 (派发侧同步兜底) */
#define WP_ORDER_MP 0x80


#define WP_VANILLA 16
#define WP_INF     0x7FFFFFFF
#define WP_MAXCFG  4096

static int   g_WpInit = 0;
static int   g_WpCfg = 0;            /* [Features] WaypointCap */
static int   g_WpHookState = 0;      /* 0=没试 1=已装 2=放弃 */
static int   g_WpHookState2 = 0;     /* 将满提示钩, 状态机同上 */
static int   g_WpHookState3 = 0;     /* 下令前检查钩, 状态机同上 */
static int   g_WpCap = WP_VANILLA;   /* 生效上限 (Poll 按实时单机/联机刷新) */
static int   g_WpWarnAt = WP_VANILLA - 1;  /* 将满提示位 (上限-1) */
static int   g_WpOrderCap = WP_ORDER_MP;   /* 下令前检查上限 (单机恒过/联机 128) */
static int   g_WpMpLogged = 0;       /* 联机绕过日志只打一次 */
static void* g_WpNormal = (void*)ADDR_WP_NORMAL;
static void* g_WpRefuse = (void*)ADDR_WP_REFUSE;
static void* g_WpWarnGo = (void*)ADDR_WP_WARNGO;
static void* g_WpWarnNo = (void*)ADDR_WP_WARNNO;
static void* g_WpOrderOk = (void*)ADDR_WP_ORDEROK;
static void* g_WpOrderNo = (void*)ADDR_WP_ORDERNO;

/* 联机判定见 OU_IsMP (OrderUtil.h, 订单系统共用)。 */

/* 拒收取证 (查 hetero 分队用, 常开, 仅拒收时一行): 0=分支墙, 1=下令前检查墙。 */
static void __cdecl WP_LogRefuse(int kind, unsigned cnt, int cap)
{
    char buf[96];
    unsigned q = 0;
    q = AppendCStr(buf, q, "waypointcap: refuse kind=", sizeof(buf));
    q = OU_AppendU(buf, q, (unsigned)kind, sizeof(buf));
    q = AppendCStr(buf, q, " cnt=", sizeof(buf));
    q = OU_AppendU(buf, q, cnt, sizeof(buf));
    q = AppendCStr(buf, q, " cap=", sizeof(buf));
    q = OU_AppendU(buf, q, (unsigned)cap, sizeof(buf));
    LogLine(buf);
}

static void* g_CallWpRefuse = (void*)WP_LogRefuse;

/* 壳: eax=当前分支数。count < cap 走正常建点, 否则记一行走原拒收。
 * 裸钩操作数只许 "m" 禁止 "r"。 */
__attribute__((naked)) void WaypointCap_Entry(void)
{
    __asm__ volatile (
        "cmpl %0, %%eax\n\t"
        "jb 1f\n\t"
        "pushal\n\t"
        "pushfl\n\t"
        "pushl %0\n\t"
        "pushl %%eax\n\t"
        "pushl $0\n\t"
        "call *%3\n\t"
        "addl $12, %%esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%2\n\t"
        "1:\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_WpCap),
          "m" (g_WpNormal), "m" (g_WpRefuse),
          "m" (g_CallWpRefuse)
        : "cc", "memory");
}

/* 将满提示壳: ebp=token, count == 上限-1 走原存状态 2, 否则继续。 */
__attribute__((naked)) void WaypointCap_WarnEntry(void)
{
    __asm__ volatile (
        "movl 0x14(%%ebp), %%eax\n\t"
        "cmpl %0, %%eax\n\t"
        "je 1f\n\t"
        "jmp *%2\n\t"
        "1:\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_WpWarnAt), "m" (g_WpWarnGo), "m" (g_WpWarnNo)
        : "cc");
}

/* 下令前检查壳: ebp=计数和。超限记一行走原拒收, 否则走原通过。
 * 单机恒过, 联机恒 128。 */
__attribute__((naked)) void WaypointCap_OrderEntry(void)
{
    __asm__ volatile (
        "cmpl %0, %%ebp\n\t"
        "jle 1f\n\t"
        "pushal\n\t"
        "pushfl\n\t"
        "pushl %0\n\t"
        "pushl %%ebp\n\t"
        "pushl $1\n\t"
        "call *%3\n\t"
        "addl $12, %%esp\n\t"
        "popfl\n\t"
        "popal\n\t"
        "popl %%ebp\n\t"
        "popl %%ebx\n\t"
        "jmp *%2\n\t"
        "1:\n\t"
        "popl %%ebp\n\t"
        "popl %%ebx\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_WpOrderCap), "m" (g_WpOrderOk), "m" (g_WpOrderNo),
          "m" (g_CallWpRefuse)
        : "cc", "memory");
}

static void WP_Log2(const char* tag, const char* msg)
{
    char buf[64];
    unsigned q = 0;
    q = AppendCStr(buf, q, "waypointcap: ", sizeof(buf));
    q = AppendCStr(buf, q, tag ? tag : "?", sizeof(buf));
    OU_LogTag(buf, msg);
}

/* 装一处 jmp 钩 (wantLen=5 直接盖, =6 尾补 1 个 NOP)。
 * 目标点被占/字节对不上则放弃 (幂等)。 */
static void WP_InstallSite(unsigned addr, const unsigned char* want, int wantLen,
                           void* entry, int* stateOut, const char* tag)
{
    unsigned char* site;
    DWORD old = 0;
    int i;
    if (!OU_InExe(addr)) return;
    if (*(int*)ADDR_FRAME < 60) return;      /* 等 exe 解包稳定 */
    site = (unsigned char*)addr;
    if (*stateOut == 1)
    {
        /* 自愈: 钩被后来者覆盖就重装。 */
        if (site[0] == 0xE9 &&
            (void*)(site + 5 + *(int*)(site + 1)) == entry)
            return;
        *stateOut = 0;
        WP_Log2(tag, " hook lost, reinstalling");
    }
    if (*stateOut) return;
    if (site[0] == 0xE9)
    {
        void* tgt = (void*)(site + 5 + *(int*)(site + 1));
        if (tgt == entry) { *stateOut = 1; return; }
        *stateOut = 2;
        WP_Log2(tag, " taken by another patch - OFF");
        return;
    }
    for (i = 0; i < wantLen; i++)
    {
        if (site[i] == want[i]) continue;
        *stateOut = 2;
        WP_Log2(tag, " prologue mismatch - OFF");
        return;
    }
    if (!VirtualProtect(site, wantLen, PAGE_EXECUTE_READWRITE, &old))
    {
        *stateOut = 2;
        WP_Log2(tag, " VirtualProtect fail - OFF");
        return;
    }
    site[0] = 0xE9;
    *(int*)(site + 1) = (int)(long)entry - (int)(long)(site + 5);
    for (i = 5; i < wantLen; i++) site[i] = 0x90;
    VirtualProtect(site, wantLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, wantLen);
    *stateOut = 1;
    WP_Log2(tag, " hook ON");
}

/* 三处原版字节 (安装与卸载共用)。 */
static const unsigned char kWpWantBranch[5] =
    { 0x83, 0xF8, 0x10, 0x75, 0x96 };              /* 0x6384DC: cmp $0x10,%eax / jne */
static const unsigned char kWpWantWarn[6] =
    { 0x83, 0x7D, 0x14, 0x0F, 0x75, 0x0A };        /* 0x6383B4: cmpl $0xF,0x14(%ebp) / jne */
static const unsigned char kWpWantOrder[6] =
    { 0x81, 0xFD, 0x80, 0x00, 0x00, 0x00 };        /* 0x6391A7: cmp $0x80,%ebp */

/* 联机绕过卸载: 自家钩还在就恢复原版字节, 状态清 0。 */
static void WP_UninstallSite(unsigned addr, const unsigned char* want, int wantLen,
                             void* entry, int* stateOut, const char* tag)
{
    unsigned char* site;
    DWORD old = 0;
    int i;
    if (*stateOut != 1) return;
    if (!OU_InExe(addr)) return;
    site = (unsigned char*)addr;
    if (site[0] != 0xE9 ||
        (void*)(site + 5 + *(int*)(site + 1)) != entry)
    {
        *stateOut = 0;
        return;
    }
    if (!VirtualProtect(site, wantLen, PAGE_EXECUTE_READWRITE, &old)) return;
    for (i = 0; i < wantLen; i++) site[i] = want[i];
    VirtualProtect(site, wantLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, wantLen);
    *stateOut = 0;
    WP_Log2(tag, " MP bypass, hook removed");
}

static void WP_EnsureHook(void)
{
    /* 联机绕过: 不装新钩, 已装的恢复原版。单机/联机按实时判定, 不缓存。 */
    if (OU_IsMP())
    {
        WP_UninstallSite(ADDR_WP_CHECK, kWpWantBranch, 5,
                         (void*)WaypointCap_Entry, &g_WpHookState, "wpbranch");
        WP_UninstallSite(ADDR_WP_WARN, kWpWantWarn, 6,
                         (void*)WaypointCap_WarnEntry, &g_WpHookState2, "wpwarn");
        WP_UninstallSite(ADDR_WP_ORDER, kWpWantOrder, 6,
                         (void*)WaypointCap_OrderEntry, &g_WpHookState3, "wporder");
        if (!g_WpMpLogged)
        {
            g_WpMpLogged = 1;
            LogLine("waypointcap: MP bypass (vanilla 16/128)");
        }
        return;
    }
    g_WpMpLogged = 0;
    WP_InstallSite(ADDR_WP_CHECK, kWpWantBranch, 5,
                   (void*)WaypointCap_Entry, &g_WpHookState, "wpbranch");
    /* 不限制时将满提示永不触发, 不装 (省一处补丁)。 */
    if (g_WpCap != WP_INF)
        WP_InstallSite(ADDR_WP_WARN, kWpWantWarn, 6,
                       (void*)WaypointCap_WarnEntry, &g_WpHookState2, "wpwarn");
    WP_InstallSite(ADDR_WP_ORDER, kWpWantOrder, 6,
                   (void*)WaypointCap_OrderEntry, &g_WpHookState3, "wporder");
}

void WpCap_Poll(void)
{
    int mp;
    if (!g_WpInit)
    {
        char buf[96];
        unsigned q = 0;
        g_WpInit = 1;
        BuildPaths();
        GtEnsureLog();
        g_WpCfg = GetPrivateProfileIntA(WP_SECTION, "WaypointCap", 0, g_IniPath);
        if (g_WpCfg < 0) g_WpCfg = 0;
        if (g_WpCfg > WP_MAXCFG) g_WpCfg = WP_MAXCFG;
        q = AppendCStr(buf, q, "waypointcap: cap=", sizeof(buf));
        q = OU_AppendU(buf, q, (unsigned)g_WpCfg, sizeof(buf));
        q = AppendCStr(buf, q, " (0=unlimited, SP only)", sizeof(buf));
        LogLine(buf);
    }
    /* 生效上限每轮按实时模式刷新: 联机恒原版 16/128, 单机按配置。 */
    mp = OU_IsMP();
    if (mp)
    {
        g_WpCap = WP_VANILLA;
        g_WpWarnAt = WP_VANILLA - 1;
        g_WpOrderCap = WP_ORDER_MP;
    }
    else
    {
        g_WpCap = (g_WpCfg > 0) ? g_WpCfg : WP_INF;
        g_WpWarnAt = (g_WpCfg > 0) ? (g_WpCfg - 1) : WP_INF;
        g_WpOrderCap = WP_ORDER_SP;
    }
    WP_EnsureHook();
}
