/* B 键信标单机启用与满槽替换。
 * 单机只补放置入口和提示, 跳过组包/广播: 单人局玩家名表可为空。
 * 联机维持每家三槽, 复用引擎按槽覆盖和原生 IPX 广播, 不改变对端协议。
 * 信标仅属本地 UI; 模拟变更不得复用此路径, 必须走标准事件。
 * [Features] BeaconInSP / BeaconAutoReplace 缺省为 1, 重启生效。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "Observer.h"
#include "BeaconSP.h"
#include "BeaconCap.h"

#define BS_SECTION "Features"

typedef void (__attribute__((thiscall)) *Fn_BsLoadArt)(void* self);

/* ---------------- 状态 ---------------- */

static int g_BsInit = 0;
static int g_BsCfg = 1;         /* [Features] BeaconInSP */
static int g_BsSwapCfg = 1;     /* [Features] BeaconAutoReplace */
static int g_BsSpOn = 0;        /* 单机三处 (hotkey/placetail/sendbuild) 就绪 */
static int g_BsNoteOn = 0;      /* 0x430CD4 记序号就绪 */
static int g_BsSwapOn = 0;      /* 两处判满替换就绪且允许 */
static int g_BsLoadTried = 0;
static int g_BsArtLogged = 0;
static int g_BsState = -1;      /* 上次记过的状态 (sp/swap/spec/mp) */
static int g_BsLastSlots = 3;   /* 上次生效的槽数: 单机池化 24 / 联机 3 */

/* 信标槽的放置序号 (0 = 无从得知, 当最旧处理)。原版下标 = 本家 3 格里的槽号;
 * 单机池化后 (BeaconCap) 下标 = 池里的全局槽号, 长度取数组容量 24。
 * 槽被删空后不清: 空槽不参与比较, 再放进来时会被重新记一次序号。 */
#define BS_SLOT_MAX 24
static unsigned g_BcTs[BS_SLOT_MAX];
static unsigned g_BcSeq;

static int Bc_PickOldest(void);
static void Bc_NoteSlot(int house, int slot);

/* 壳的跳转目标 (裸钩里只许用 "m" 约束, 不能用寄存器)。 */
static void* g_BsHotkeyGo   = (void*)ADDR_BC_HOTKEY_GO;
static void* g_BsHotkeyNext = (void*)ADDR_BC_HOTKEY_NEXT;
static void* g_BsTailGo     = (void*)ADDR_BC_PLACETAIL_GO;
static void* g_BsTailNext   = (void*)ADDR_BC_PLACETAIL_NEXT;
static void* g_BsSendSkip   = (void*)ADDR_BC_SENDBUILD_SKIP;
static void* g_BsSendNext   = (void*)ADDR_BC_SENDBUILD_NEXT;
static void* g_BsFullAlloc  = (void*)ADDR_BC_FULLCHK_NEXT;
static void* g_BsFullGiveUp = (void*)ADDR_BC_FULLCHK_GIVEUP;
static void* g_BcPickPtr    = (void*)Bc_PickOldest;
static void* g_BcNotePtr    = (void*)Bc_NoteSlot;

/* ---------------- 五个壳 ----------------
 * 关闭分支重放原指令。辅助函数只破坏落点前会被重载的 caller-saved 寄存器,
 * house、manager 等仍活跃的 callee-saved 寄存器由调用约定保存。 */

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
        : "m" (g_BsSpOn), "m" (g_BsHotkeyGo), "m" (g_BsHotkeyNext)
        : "cc");
}

/* 放置尾部: ebx 为 houseId, [esp+0x1F4] 为刚写入的槽号。
 * 单机和联机都需记录序号, 再进入 EVA/提示段。 */
__attribute__((naked)) void BeaconSP_TailEntry(void)
{
    __asm__ volatile (
        "cmpl $0, %0\n\t"
        "je 1f\n\t"
        "movl 0x1f4(%%esp), %%eax\n\t"
        "pushl %%eax\n\t"
        "pushl %%ebx\n\t"
        "call *%3\n\t"
        "addl $8, %%esp\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        "movl 0xA8B238, %%eax\n\t"
        "jmp *%2\n\t"
        :
        : "m" (g_BsNoteOn), "m" (g_BsTailGo), "m" (g_BsTailNext), "m" (g_BcNotePtr)
        : "cc", "eax", "ecx", "edx");
}

/* 0x430D99 → 0x430E50 (共享收尾, 跳过组包/广播)。单机专用。 */
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
        : "m" (g_BsSpOn), "m" (g_BsSendSkip), "m" (g_BsSendNext)
        : "cc");
}

/* 0x430F4A (CanPlaceBeacon 的"满"返回): 打开时满 3 也返回 1。 */
__attribute__((naked)) void BeaconSP_CanFullEntry(void)
{
    __asm__ volatile (
        "cmpl $0, %0\n\t"
        "je 1f\n\t"
        "movb $1, %%al\n\t"
        "ret $4\n\t"
        "1:\n\t"
        "xorb %%al, %%al\n\t"
        "ret $4\n\t"
        :
        : "m" (g_BsSwapOn)
        : "cc");
}

/* 仅本地未指定槽号 (-1) 时挑最旧槽; 远端指定槽号不得重选。
 * 写回 [esp+0x1F4] 后复用引擎覆盖分支, 广播携带同一槽号。 */
__attribute__((naked)) void BeaconSP_FullEntry(void)
{
    __asm__ volatile (
        "cmpl $0, %0\n\t"
        "je 1f\n\t"
        "cmpl $-1, 0x1f4(%%esp)\n\t"
        "jne 2f\n\t"
        "call *%1\n\t"
        "cmpl $-1, %%eax\n\t"
        "je 3f\n\t"
        "movl %%eax, 0x1f4(%%esp)\n\t"
        "jmp 2f\n\t"
        "1:\n\t"
        "cmpl $-1, 0x1f4(%%esp)\n\t"
        "je 3f\n\t"
        "2:\n\t"
        "jmp *%2\n\t"
        "3:\n\t"
        "jmp *%3\n\t"
        :
        : "m" (g_BsSwapOn), "m" (g_BcPickPtr), "m" (g_BsFullAlloc),
          "m" (g_BsFullGiveUp)
        : "cc", "eax", "ecx", "edx");
}

/* 入口均覆盖完整、无相对寻址的指令。单机组负责入口/跳过广播,
 * 序号组支持单机与联机, 替换组依赖序号组。 */
static const unsigned char kBcMovSess[5] = { 0xA1, 0x38, 0xB2, 0xA8, 0x00 };
static const unsigned char kBcMovPlyr[6] = { 0x8B, 0x0D, 0x78, 0xDA, 0xA8, 0x00 };
static const unsigned char kBcCanFull[5] = { 0x32, 0xC0, 0xC2, 0x04, 0x00 };
static const unsigned char kBcFullChk[8] = { 0x83, 0xBC, 0x24, 0xF4, 0x01, 0x00, 0x00, 0xFF };

#define BS_SCOPE_SP     0
#define BS_SCOPE_NOTE   1
#define BS_SCOPE_SWAP   2

typedef struct
{
    unsigned addr;
    const unsigned char* want;
    int len;
    void* entry;
    const char* tag;
    int scope;                  /* 见上 */
    int state;                  /* 0=没装 1=已装 2=放弃 */
} BS_Site;

static BS_Site g_BsSites[] =
{
    { ADDR_BC_HOTKEY,    kBcMovSess, 5, (void*)BeaconSP_HotkeyEntry,  "hotkey",    BS_SCOPE_SP,   0 },
    { ADDR_BC_PLACETAIL, kBcMovSess, 5, (void*)BeaconSP_TailEntry,    "placetail", BS_SCOPE_NOTE, 0 },
    { ADDR_BC_SENDBUILD, kBcMovPlyr, 6, (void*)BeaconSP_SendEntry,    "sendbuild", BS_SCOPE_SP,   0 },
    { ADDR_BC_CANFULL,   kBcCanFull, 5, (void*)BeaconSP_CanFullEntry, "canfull",   BS_SCOPE_SWAP, 0 },
    { ADDR_BC_FULLCHK,   kBcFullChk, 8, (void*)BeaconSP_FullEntry,    "swapfull",  BS_SCOPE_SWAP, 0 },
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

/* 每次自动替换记一行, 反馈问题时直接看槽号与序号。 */
static void Bc_LogSwap(int house, int slot, unsigned ts)
{
    char buf[80];
    unsigned n = AppendCStr(buf, 0, "beaconsp: swap h=", sizeof(buf));
    n = OU_AppendU(buf, n, (unsigned)house, sizeof(buf));
    n = AppendCStr(buf, n, " slot=", sizeof(buf));
    n = OU_AppendU(buf, n, (unsigned)slot, sizeof(buf));
    n = AppendCStr(buf, n, " seq=", sizeof(buf));
    n = OU_AppendU(buf, n, ts, sizeof(buf));
    LogLine(buf);
}

/* 生效状态变化时记一行 (开局会先 0/0 再切到实际值, 只记变化)。 */
static void BS_LogState(int sp, int swap, int spec, int mp)
{
    char buf[72];
    unsigned n;
    int st = (sp << 3) | (swap << 2) | (spec << 1) | mp;
    if (st == g_BsState) return;
    g_BsState = st;
    n = AppendCStr(buf, 0, "beaconsp: state sp=", sizeof(buf));
    n = OU_AppendU(buf, n, (unsigned)sp, sizeof(buf));
    n = AppendCStr(buf, n, " swap=", sizeof(buf));
    n = OU_AppendU(buf, n, (unsigned)swap, sizeof(buf));
    n = AppendCStr(buf, n, mp ? " mp" : " sp", sizeof(buf));
    if (spec) n = AppendCStr(buf, n, " (spectator, swap off)", sizeof(buf));
    LogLine(buf);
}

static void BS_EnsureInit(void)
{
    char buf[72];
    unsigned n;
    if (g_BsInit) return;
    g_BsInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_BsCfg = GetPrivateProfileIntA(BS_SECTION, "BeaconInSP", 1, g_IniPath) ? 1 : 0;
    g_BsSwapCfg = GetPrivateProfileIntA(BS_SECTION, "BeaconAutoReplace", 1, g_IniPath) ? 1 : 0;
    n = AppendCStr(buf, 0, "beaconsp: BeaconInSP=", sizeof(buf));
    n = OU_AppendU(buf, n, (unsigned)g_BsCfg, sizeof(buf));
    n = AppendCStr(buf, n, " BeaconAutoReplace=", sizeof(buf));
    n = OU_AppendU(buf, n, (unsigned)g_BsSwapCfg, sizeof(buf));
    LogLine(buf);
}

/* 单机不走原生 LoadArt, 启用前必须补齐两个 SHP 和非零雷达周期,
 * 否则绘制会解引用空指针或除零。LoadArt 不释放旧缓冲, 只允许补读一次;
 * 联机由引擎加载, 此处仅检查。 */
static int BS_EnsureArt(int allowLoad)
{
    unsigned art, radar;
    if (*(int*)ADDR_FRAME < 60) return 0;        /* 开局美术还没读 */
    if (allowLoad && !*(unsigned*)ADDR_BC_ART && !*(unsigned*)ADDR_BC_RADARART && !g_BsLoadTried)
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

/* 只读选择本家最小序号的槽, 0 表示未知并优先替换; 无候选返回 -1。
 * 原版按 house*3 定址, 池化按全局槽号及对象 HouseID 过滤。
 * 删除/覆盖由原生放置路径执行。 */
static int Bc_PickOldest(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    void** slotp = (void**)ADDR_BEACON_MGR;
    unsigned best = 0xFFFFFFFFu;
    int house, n, base, cnt, pooled, i, pick = -1;

    if (!cur) return -1;
    house = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (house < 0 || house >= 8) return -1;

    n = BeaconCap_SlotCount();
    if (n > BS_SLOT_MAX) n = BS_SLOT_MAX;
    pooled = (n > 3) ? 1 : 0;
    base = pooled ? 0 : house * 3;      /* slotp 的下标偏移 */
    cnt  = pooled ? n : 3;

    for (i = 0; i < cnt; i++)
    {
        void* b = slotp[base + i];
        if (!b) continue;
        /* 池化后本家与别人的信标同在一个池子, 只能按对象里的 HouseID 认领。 */
        if (pooled && *(int*)((char*)b + OFF_BC_HOUSEID) != house) continue;
        if (g_BcTs[i] < best) { best = g_BcTs[i]; pick = i; }
    }
    if (pick >= 0) Bc_LogSwap(house, pick, best);
    return pick;
}

/* 放置成功后记序号 (0x430CD4 处的 houseId + 槽号)。 */
static void Bc_NoteSlot(int house, int slot)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int n = BeaconCap_SlotCount();
    if (!cur || slot < 0 || slot >= n || slot >= BS_SLOT_MAX) return;
    if (house != *(int*)((char*)cur + OFF_HOUSE_INDEX)) return;
    g_BcTs[slot] = ++g_BcSeq;
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
        /* 仅接管自身跳转, 不覆盖其它补丁。 */
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

/* 装/卸一组, 返回 1 = 该组已全装好。 */
static int BS_Group(int scope, int want)
{
    int i, ok = want;
    for (i = 0; want && i < BS_NSITE; i++)
        if (g_BsSites[i].scope == scope && g_BsSites[i].state == 2) want = ok = 0;
    for (i = 0; i < BS_NSITE; i++)
    {
        if (g_BsSites[i].scope != scope) continue;
        if (want) BS_Install(&g_BsSites[i]);
        else BS_Uninstall(&g_BsSites[i]);
    }
    for (i = 0; ok && i < BS_NSITE; i++)
        if (g_BsSites[i].scope == scope && g_BsSites[i].state != 1) ok = 0;
    if (want && !ok)
        for (i = 0; i < BS_NSITE; i++)
            if (g_BsSites[i].scope == scope) BS_Uninstall(&g_BsSites[i]);
    return ok;
}

void BeaconSP_Poll(void)
{
    int mp, art, spWant, swapWant, noteWant, gm, noteBlocked = 0;
    int sp, note, swap, spec = 0, i, n;
    static int lastFrame = -1;
    int frame = *(int*)ADDR_FRAME;

    BS_EnsureInit();
    mp = OU_IsMP();
    gm = *(int*)0xA8B238u;
    art = (mp || ((gm == 0 || gm == 5) && g_BsCfg)) ? BS_EnsureArt(!mp) : 0;

    /* 场景重开会重置信标, 同槽旧序号也必须失效。 */
    if (frame < lastFrame)
    {
        g_BcSeq = 0;
        for (i = 0; i < BS_SLOT_MAX; i++) g_BcTs[i] = 0;
    }
    lastFrame = frame;

    /* 安装和卸载期间壳都走原版; 只在依赖组实际齐备后发布开关。 */
    g_BsSpOn = g_BsNoteOn = g_BsSwapOn = 0;
    for (i = 0; i < BS_NSITE; i++)
        if (g_BsSites[i].scope == BS_SCOPE_NOTE && g_BsSites[i].state == 2) noteBlocked = 1;
    spWant = ((gm == 0 || gm == 5) && g_BsCfg && art && !noteBlocked) ? 1 : 0;
    sp = BS_Group(BS_SCOPE_SP, spWant);
    swapWant = (g_BsSwapCfg && art && (sp || mp)) ? 1 : 0;
    noteWant = (sp || swapWant) ? 1 : 0;
    note = BS_Group(BS_SCOPE_NOTE, noteWant);
    if (!mp && !note)
    {
        BS_Group(BS_SCOPE_SP, 0);
        sp = 0;
    }
    swap = BS_Group(BS_SCOPE_SWAP, swapWant && note && (mp || sp));

    /* 观战不参与对局: 钩子留着 (免得反复装卸), 只把开关按回原版,
     * 否则观战者满 3 后再点一下就会顶掉被观战玩家的信标并按人家的名义广播。 */
    if (mp && swap) spec = Observer_LocalIsSpectator() ? 1 : 0;

    g_BsSpOn = sp;
    g_BsNoteOn = note;
    g_BsSwapOn = (swap && !spec) ? 1 : 0;

    /* 关掉替换时把序号清干净: 重新打开后已有的信标一律当"无从得知"(最旧)。 */
    if (!g_BsSwapOn && g_BcSeq)
    {
        g_BcSeq = 0;
        for (i = 0; i < BS_SLOT_MAX; i++) g_BcTs[i] = 0;
    }

    /* 槽数一变 (单机池化 24 / 联机 3), 序号的下标语义也跟着变, 必须清空,
     * 否则联机里会拿着单机池的旧序号去挑本家那 3 格。 */
    n = BeaconCap_SlotCount();
    if (n != g_BsLastSlots)
    {
        char buf[48];
        unsigned k = AppendCStr(buf, 0, "beaconsp: slots ", sizeof(buf));
        k = OU_AppendU(buf, k, (unsigned)g_BsLastSlots, sizeof(buf));
        k = AppendCStr(buf, k, " -> ", sizeof(buf));
        OU_AppendU(buf, k, (unsigned)n, sizeof(buf));
        g_BsLastSlots = n;
        g_BcSeq = 0;
        for (i = 0; i < BS_SLOT_MAX; i++) g_BcTs[i] = 0;
        LogLine(buf);
    }

    BS_LogState(g_BsSpOn, g_BsSwapOn, spec, mp);
}
