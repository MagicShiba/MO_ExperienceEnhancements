/* BeaconSP.cpp - 通信信标 (B 键放置) 单机启用。
 *
 * 原版只在联机可用: 引擎在四处 `GameMode==0 || GameMode==5` 早退,
 * 单机只需补前两处 (另两处早退之前本地状态已改完, 之后只剩发包):
 *   0x5370A0  BeaconPlacementCommandClass::Execute 入口 → B 键什么都不做
 *   0x430CD4  BeaconManagerClass::PlaceBeacon 尾 → 无 EVA、无两条操作提示
 * 信标本体 (建对象/写槽/绘制/点选/存文字) 都在早退之前, 单机本来就有。
 *
 * 0x430D99 起组 IPX 包并逐玩家广播, 先解引用玩家名表首元素 (单人局可能为空),
 * 且单机没有对端, 所以在 0x430D99 再拦一刀跳共享收尾 0x430E50:
 * EVA 与提示照拿, 组包与广播整段不跑。
 *
 * 上限保持每玩家 3 个不动 (数组第二维即上限, 改它要动 18 处循环界 + 7 处
 * house*3 步长, 且包里 houseBeaconId 只有一字节, 单侧加格会让对端越界):
 * 单机联机完全一致, 零协议改动。详见 notes/通信信标.md。
 *
 * 联机: 三处都不装钩, 已装的恢复原版字节, 联机字节与原版逐字节一致。
 * 配置 [Features] BeaconInSP (缺失=1, 改后重启)。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "BeaconSP.h"

#define BS_SECTION "Features"

typedef void (__attribute__((thiscall)) *Fn_BsLoadArt)(void* self);

/* ---------------- 状态 ---------------- */

static int g_BsInit = 0;
static int g_BsCfg = 1;         /* [Features] BeaconInSP */
static int g_BsOn = 0;          /* 壳内只读这一个 int */
static int g_BsLoadTried = 0;
static int g_BsArtLogged = 0;

/* 壳的跳转目标 (裸钩里只许用 "m" 约束, 不能用寄存器)。 */
static void* g_BsHotkeyGo   = (void*)ADDR_BC_HOTKEY_GO;
static void* g_BsHotkeyNext = (void*)ADDR_BC_HOTKEY_NEXT;
static void* g_BsTailGo     = (void*)ADDR_BC_PLACETAIL_GO;
static void* g_BsTailNext   = (void*)ADDR_BC_PLACETAIL_NEXT;
static void* g_BsSendSkip   = (void*)ADDR_BC_SENDBUILD_SKIP;
static void* g_BsSendNext   = (void*)ADDR_BC_SENDBUILD_NEXT;

/* ---------------- 三个壳 ----------------
 * 全是"判开关再跳", 不碰任何寄存器的值; 关闭分支照原指令跑一遍再回原流程,
 * 两条路径都与原版等价。 */

/* 0x5370A0 → 0x5370AE (call 0x731A30 开/关布署模式)。 */
__attribute__((naked)) void BeaconSP_HotkeyEntry(void)
{
    __asm__ volatile (
        "cmpl $0, %0\n\t"
        "je 1f\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        "movl 0xA8B238, %%eax\n\t"
        "jmp *%2\n\t"
        :
        : "m" (g_BsOn), "m" (g_BsHotkeyGo), "m" (g_BsHotkeyNext)
        : "cc");
}

/* 0x430CD4 → 0x430CEA (EVA + 两条操作提示段)。 */
__attribute__((naked)) void BeaconSP_TailEntry(void)
{
    __asm__ volatile (
        "cmpl $0, %0\n\t"
        "je 1f\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        "movl 0xA8B238, %%eax\n\t"
        "jmp *%2\n\t"
        :
        : "m" (g_BsOn), "m" (g_BsTailGo), "m" (g_BsTailNext)
        : "cc");
}

/* 0x430D99 → 0x430E50 (共享收尾, 跳过组包/广播)。 */
__attribute__((naked)) void BeaconSP_SendEntry(void)
{
    __asm__ volatile (
        "cmpl $0, %0\n\t"
        "je 1f\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        "movl 0xA8DA78, %%ecx\n\t"
        "jmp *%2\n\t"
        :
        : "m" (g_BsOn), "m" (g_BsSendSkip), "m" (g_BsSendNext)
        : "cc");
}

/* ---------------- 站点表 ----------------
 * 两处偷同一条 `mov eax,[0xA8B238]` (GameMode); 0x430D99 偷
 * `mov ecx,[0xA8DA78]` (玩家名表 Items)。都是无 rel 的指令边界。 */
static const unsigned char kBcMovSess[5] = { 0xA1, 0x38, 0xB2, 0xA8, 0x00 };
static const unsigned char kBcMovPlyr[6] = { 0x8B, 0x0D, 0x78, 0xDA, 0xA8, 0x00 };

typedef struct
{
    unsigned addr;
    const unsigned char* want;
    int len;
    void* entry;
    const char* tag;
    int state;                  /* 0=没装 1=已装 2=放弃 */
} BS_Site;

static BS_Site g_BsSites[] =
{
    { ADDR_BC_HOTKEY,    kBcMovSess, 5, (void*)BeaconSP_HotkeyEntry, "hotkey",    0 },
    { ADDR_BC_PLACETAIL, kBcMovSess, 5, (void*)BeaconSP_TailEntry,   "placetail", 0 },
    { ADDR_BC_SENDBUILD, kBcMovPlyr, 6, (void*)BeaconSP_SendEntry,   "sendbuild", 0 },
};
#define BS_NSITE ((int)(sizeof(g_BsSites) / sizeof(g_BsSites[0])))

/* ---------------- 日志 ---------------- */

static void BS_Log(const char* tag, const char* msg)
{
    char buf[64];
    unsigned n = AppendCStr(buf, 0, "beaconsp: ", sizeof(buf));
    AppendCStr(buf, n, tag ? tag : "?", sizeof(buf));
    OU_LogTag(buf, msg);
}

static void BS_EnsureInit(void)
{
    char buf[48];
    unsigned n;
    if (g_BsInit) return;
    g_BsInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_BsCfg = GetPrivateProfileIntA(BS_SECTION, "BeaconInSP", 1, g_IniPath) ? 1 : 0;
    n = AppendCStr(buf, 0, "beaconsp: ", sizeof(buf));
    AppendCStr(buf, n, g_BsCfg ? "ON (SP only)" : "OFF", sizeof(buf));
    LogLine(buf);
}

/* ---------------- 信标美术 ----------------
 * 两个 SHP 头指针和雷达动画周期 (+0x7C) 只由 LoadArt 0x4309D0 填, 而单机开局
 * 不走它 (0x687BB2 属联机开局流程), 两者都停在静态构造留下的 0。
 * 三处 `idiv` 拿 +0x7C 当除数 (0x430659 / 0x431718 / 0x4318B6), 且全在
 * AllocatedCount != 0 之后 —— 单机一放信标就除零崩溃。所以先补读一次,
 * 读不进来就不启用 (空 SHP 指针进 0x430250 同样是崩)。 */
static int BS_EnsureArt(void)
{
    unsigned art, radar;
    if (*(int*)ADDR_FRAME < 60) return 0;        /* 开局美术还没读 */
    if (!*(unsigned*)ADDR_BC_ART && !*(unsigned*)ADDR_BC_RADARART && !g_BsLoadTried)
    {
        g_BsLoadTried = 1;                       /* 只补一次: LoadArt 不释放旧缓冲 */
        ((Fn_BsLoadArt)(void*)ADDR_BC_LOADART)((void*)ADDR_BEACON_MGR);
    }
    art = *(unsigned*)ADDR_BC_ART;
    radar = *(unsigned*)ADDR_BC_RADARART;
    if (art && radar && *(unsigned*)(ADDR_BEACON_MGR + OFF_BC_PERIOD)) return 1;
    if (!g_BsArtLogged)
    {
        char buf[64];
        unsigned n;
        g_BsArtLogged = 1;
        n = AppendCStr(buf, 0, "beaconsp: no beacon art p=", sizeof(buf));
        n = OU_AppendU(buf, n, art, sizeof(buf));
        n = AppendCStr(buf, n, " r=", sizeof(buf));
        n = OU_AppendU(buf, n, radar, sizeof(buf));
        AppendCStr(buf, n, " - OFF", sizeof(buf));
        LogLine(buf);
    }
    return 0;
}

/* ---------------- 装/卸 (幂等) ---------------- */

static void BS_Install(BS_Site* s)
{
    unsigned char* p;
    DWORD old = 0;
    int i;
    if (!OU_InExe(s->addr) || *(int*)ADDR_FRAME < 60) return;
    p = (unsigned char*)s->addr;
    if (p[0] == 0xE9)
    {
        /* 已经是我们的钩 (含被后来者覆盖后重装) → 完成; 是别人的 → 让路。 */
        if ((void*)(p + 5 + *(int*)(p + 1)) == s->entry) { s->state = 1; return; }
        if (s->state != 2)
        {
            s->state = 2;
            BS_Log(s->tag, " taken by another patch - OFF");
        }
        return;
    }
    if (s->state == 2) return;                   /* 已放弃: 不与别的补丁抢同一个站点 */
    for (i = 0; i < s->len; i++) if (p[i] != s->want[i]) break;
    if (i < s->len)
    {
        s->state = 2;
        BS_Log(s->tag, " prologue mismatch - OFF");
        return;
    }
    if (!VirtualProtect(p, s->len, PAGE_EXECUTE_READWRITE, &old))
    {
        s->state = 2;
        BS_Log(s->tag, " VirtualProtect fail - OFF");
        return;
    }
    p[0] = 0xE9;
    *(int*)(p + 1) = (int)(long)s->entry - (int)(long)(p + 5);
    for (i = 5; i < s->len; i++) p[i] = 0x90;
    VirtualProtect(p, s->len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, s->len);
    s->state = 1;
    BS_Log(s->tag, " hook ON");
}

static void BS_Uninstall(BS_Site* s)
{
    unsigned char* p;
    DWORD old = 0;
    int i;
    if (s->state != 1 || !OU_InExe(s->addr)) return;
    p = (unsigned char*)s->addr;
    if (p[0] != 0xE9 || (void*)(p + 5 + *(int*)(p + 1)) != s->entry)
    {
        s->state = 0;                            /* 已被别人改回, 不再管 */
        return;
    }
    if (!VirtualProtect(p, s->len, PAGE_EXECUTE_READWRITE, &old)) return;
    for (i = 0; i < s->len; i++) p[i] = s->want[i];
    VirtualProtect(p, s->len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, s->len);
    s->state = 0;
    BS_Log(s->tag, " restored (vanilla)");
}

void BeaconSP_Poll(void)
{
    int i, on;
    BS_EnsureInit();
    /* 联机一律原版; 单机选项关或美术不全也不启用。 */
    on = (g_BsCfg && !OU_IsMP() && BS_EnsureArt()) ? 1 : 0;
    for (i = 0; i < BS_NSITE; i++)
    {
        if (on) BS_Install(&g_BsSites[i]);
        else BS_Uninstall(&g_BsSites[i]);
    }
    /* 三处必须全装上才放行: 少了 sendbuild 那处单机会跑进组包/广播段。 */
    for (i = 0; on && i < BS_NSITE; i++) if (g_BsSites[i].state != 1) on = 0;
    g_BsOn = on;
}
