/* OrderThrottle.cpp - 大量单位分帧下令。
 *
 * 原版 EventClass::OutList 一帧只装 128 条, 而一次下令给选区每个单位各发一条,
 * 于是第 129 条起被静默丢弃。钩 ClickedMission @ 0x6FFBE0 上游: OutList 放得下
 * 且无排队则放行, 否则入队; Poll 每帧经跳板补发, 判据与原版 Add 同一句。
 * 补发前验存活, 换图/超时丢弃。
 *
 * MPEnable 默认 0: 分帧本身不影响同步 (见 notes/选中与下令上限.md §5), 这个开关
 * 只挡下一层 Winsock 出站缓冲 —— 每帧命令量被顶到上限时的表现没验证过。
 *
 * 引擎事实与推导见 notes/选中与下令上限.md。 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"

#define OT_SECTION "OrderThrottle"

/* ---------------- 地址 ---------------- */

#define ADDR_CLICKEDMISSION 0x006FFBE0u
#define OT_STOLEN   10u
#define OT_RESUME   (ADDR_CLICKEDMISSION + OT_STOLEN)  /* 0x6FFBEA */

#define ADDR_GAMEMODE 0xA8B238u   /* SessionClass::Instance->GameMode (首个 int) */
#define ADDR_SCENARIO 0xA8B230u   /* ScenarioClass::Instance (换图代号) */
#define ADDR_FRAME    0xA8ED84u   /* Unsorted::CurrentFrame */
#define ADDR_MOVE_FB  0x822CF2u   /* Unsorted::MoveFeedback */

#define GAME_LAN      3
#define GAME_INTERNET 4

/* gamemd 映像范围 (同 RepairAll): 虚表与引擎函数都落里面, 验对象用。 */
#define OT_EXE_LO 0x401000u
#define OT_EXE_HI 0x8A0000u

#define OFF_INLIMBO 0x81u
#define OFF_ISALIVE 0x90u

#define OT_QCAP     1024    /* 待发队列上限 (2 的幂, & 掩码做环形) */
#define OT_QMASK    (OT_QCAP - 1)
#define OT_MAXWAIT  600     /* 单条最多等 600 逻辑帧 (~10s), 超时丢弃 */
#define OT_STATFRAMES 1200  /* 统计日志间隔 (~20 秒) */

/* ---------------- 配置 ---------------- */

static int g_OtInit = 0;
static int g_OtEnable = 1;      /* [OrderThrottle] Enable */
static int g_OtMpEnable = 0;    /* [OrderThrottle] MPEnable (默认关!) */
static int g_OtMpWarned = 0;
static int g_OtFirstLog = 0;

static unsigned long g_OtStatSent = 0, g_OtStatDrop = 0;
static int g_OtStatFrame = 0;

/* ---------------- 待发队列 (环形, 不另存 tail) ---------------- */

typedef struct
{
    void* self;        /* 被下令的对象 (TechnoClass*) */
    void* target;      /* pTarget, 可空 */
    void* cell;        /* TargetCell, 可空 */
    void* nearCell;   /* NearestTargetCellICanEnter, 可空 */
    int   mission;
    int   wait;        /* 已等逻辑帧 */
    unsigned char mf;  /* 入队瞬间的 MoveFeedback (补发时还原, 语音不变) */
} OtOrder;

static OtOrder s_Q[OT_QCAP];
static int s_QHead = 0;
static int s_QCount = 0;

static OtOrder* s_QTail(void) { return &s_Q[(s_QHead + s_QCount) & OT_QMASK]; }
static OtOrder* s_QFront(void) { return &s_Q[s_QHead]; }
static void s_QPop(void) { s_QHead = (s_QHead + 1u) & OT_QMASK; s_QCount--; }
static void s_QClear(void) { s_QHead = 0; s_QCount = 0; }

/* ---------------- 钩子 / 跳板 ---------------- */

static void* g_OtTramp = NULL;
static int   g_OtHookState = 0;    /* 0=没试 1=已装 2=放弃 */
static volatile int g_OtRet = 0;    /* 裸壳跨 popal 传返回值 */

static int OT_InExe(unsigned v) { return (v >= OT_EXE_LO && v < OT_EXE_HI) ? 1 : 0; }

/* IsPlausiblePtr 只判 [64K,2G)+4 对齐, 堆/栈数字也能过, 须再验虚表落 exe 内。 */
static int OT_IsObj(void* p)
{
    if (!p || !IsPlausiblePtr(p)) return 0;
    return OT_InExe((unsigned)(size_t)*(void***)p);
}

static int OT_ObjLive(void* p)
{
    if (!OT_IsObj(p)) return 0;
    if (!((unsigned char*)p)[OFF_ISALIVE]) return 0;
    if (((unsigned char*)p)[OFF_INLIMBO]) return 0;
    return 1;
}

static int OT_IsMP(void)
{
    int gm = *(int*)ADDR_GAMEMODE;
    return (gm == GAME_LAN || gm == GAME_INTERNET) ? 1 : 0;
}

static void OT_EnsureInit(void)
{
    char buf[128];
    unsigned q = 0;
    if (g_OtInit) return;
    g_OtInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_OtEnable = GetPrivateProfileIntA(OT_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    g_OtMpEnable = GetPrivateProfileIntA(OT_SECTION, "MPEnable", 0, g_IniPath) ? 1 : 0;
    q = AppendCStr(buf, q, "othrottle: enable=", sizeof(buf));
    q += UtoA((unsigned)g_OtEnable, buf + q);
    q = AppendCStr(buf, q, " mp=", sizeof(buf));
    q += UtoA((unsigned)g_OtMpEnable, buf + q);
    LogLine(buf);
}

/* 联机且 MPEnable=0 时整模块空转 (默认配置)。 */
static int OT_On(void)
{
    OT_EnsureInit();
    if (!g_OtEnable) return 0;
    if (OT_IsMP() && !g_OtMpEnable) return 0;
    if (OT_IsMP() && !g_OtMpWarned)
    {
        g_OtMpWarned = 1;
        LogLine("othrottle: MP ON - refilling OutList to 128 every catch-up frame");
    }
    return 1;
}

static void OT_Flush(const char* why)
{
    char buf[96], b1[16];
    unsigned q = 0;
    if (s_QCount <= 0) return;
    q = AppendCStr(buf, q, "othrottle: drop queued=", sizeof(buf));
    q += UtoA((unsigned)s_QCount, b1);
    q = AppendCStr(buf, q, " (", sizeof(buf));
    q = AppendCStr(buf, q, why ? why : "?", sizeof(buf));
    q = AppendCStr(buf, q, ")", sizeof(buf));
    LogLine(buf);
    s_QClear();
}

/* 入口栈形: [esp]=ret, +4=Mission, +8=pTarget, +0xC=TargetCell,
 * +0x10=NearestCell, ECX=this。pushal 后原栈在 esp+32。 */
static int __cdecl OT_Decide(void* self, int mission, void* target,
                             void* cell, void* nearCell)
{
    OtOrder* e;
    unsigned cnt;
    OT_EnsureInit();
    if (!g_OtEnable) return 0;
    if (OT_IsMP() && !g_OtMpEnable) return 0;
    /* 有排队保序; 无排队且原版放得下 (与 Add 同一句) 则放行。 */
    if (s_QCount <= 0)
    {
        cnt = *(unsigned*)ADDR_OUT_COUNT;
        if (cnt < EVT_CAP) return 0;
    }
    if (s_QCount >= OT_QCAP)
    {
        g_OtStatDrop++;
        return 2;                                   /* 队列满: 与原版同形地失败 */
    }
    e = s_QTail();
    e->self = self;
    e->target = target;
    e->cell = cell;
    e->nearCell = nearCell;
    e->mission = mission;
    e->wait = 0;
    e->mf = *(unsigned char*)ADDR_MOVE_FB;
    s_QCount++;
    if (!g_OtFirstLog)
    {
        g_OtFirstLog = 1;
        LogLine("othrottle: first defer (queueing orders)");
    }
    return 1;
}

static void* g_CallOtDecide = (void*)OT_Decide;

/* 裸钩铁律见 MOExperience.cpp 文件头: 操作数只许 "m" 禁止 "r"。
 * 返回 0 = 放行走跳板, 非 0 = 已接管 (AL=返回值, ret 0x10)。 */
__attribute__((naked)) void OrderThrottle_Entry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "movl 48(%%esp), %%eax\n\t"      /* NearestCell */
        "movl 44(%%esp), %%ecx\n\t"      /* TargetCell   */
        "movl 40(%%esp), %%edx\n\t"      /* pTarget      */
        "movl 36(%%esp), %%ebx\n\t"      /* Mission      */
        "movl 24(%%esp), %%esi\n\t"      /* pushal 帧 ECX 槽 = this */
        "pushl %%eax\n\t"
        "pushl %%ecx\n\t"
        "pushl %%edx\n\t"
        "pushl %%ebx\n\t"
        "pushl %%esi\n\t"
        "call *%0\n\t"
        "addl $20, %%esp\n\t"
        "movl %%eax, %1\n\t"             /* 跨 popal 传返回值 (只保证 AL) */
        "testl %%eax, %%eax\n\t"
        "jz 1f\n\t"
        "popal\n\t"
        "movl %1, %%eax\n\t"
        "ret $0x10\n\t"
        "1:\n\t"
        "popal\n\t"
        "jmp *%2\n\t"
        :
        : "m" (g_CallOtDecide), "m" (g_OtRet), "m" (g_OtTramp)
        : "memory", "cc");
}

/* 偷 0x6FFBE0 起 5 字节 jmp 到壳, 跳板存原 10 字节 + 跳回。被偷的字节是
 * `sub esp,0x88` + 四个 push, 不含任何直接相对跳转, 跳板原样重放安全。
 * 目标点已被别的补丁占了, 或字节对不上, 就整个放弃 (幂等, 不会反复试)。 */
static void OT_EnsureHook(void)
{
    unsigned char* site;
    unsigned char* st;
    DWORD old = 0;
    int i;
    static const unsigned char kPrologue[OT_STOLEN] =
        { 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x53, 0x55, 0x56, 0x57 };
    if (g_OtHookState) return;
    if (!IsPlausiblePtr((void*)ADDR_CLICKEDMISSION)) return;
    if (*(int*)ADDR_FRAME < 60) return;      /* 等 exe 解包稳定 */
    site = (unsigned char*)ADDR_CLICKEDMISSION;
    if (site[0] == 0xE9)
    {
        void* tgt = (void*)(site + 5 + *(int*)(site + 1));
        if (tgt == (void*)OrderThrottle_Entry) { g_OtHookState = 1; return; }
        g_OtHookState = 2;
        LogLine("othrottle: hook site taken by another patch - OFF");
        return;
    }
    for (i = 0; i < (int)OT_STOLEN; i++)
    {
        if (site[i] == kPrologue[i]) continue;
        g_OtHookState = 2;
        LogLine("othrottle: prologue mismatch - OFF");
        return;
    }
    st = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!st) { g_OtHookState = 2; LogLine("othrottle: VirtualAlloc fail - OFF"); return; }
    for (i = 0; i < (int)OT_STOLEN; i++) st[i] = site[i];
    st[OT_STOLEN] = 0xE9;
    *(int*)(st + OT_STOLEN + 1) =
        (int)(long)OT_RESUME - (int)(long)(st + OT_STOLEN + 5);
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
    {
        VirtualFree(st, 0, MEM_RELEASE);
        g_OtHookState = 2;
        LogLine("othrottle: VirtualProtect fail - OFF");
        return;
    }
    site[0] = 0xE9;
    *(int*)(site + 1) = (int)(long)OrderThrottle_Entry - (int)(long)(site + 5);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    g_OtTramp = st;
    g_OtHookState = 1;
    LogLine("othrottle: ClickedMission hook ON");
}

/* ---------------- 补发 ---------------- */

typedef int __attribute__((thiscall)) (*Fn_ClickedMission)(void* self, int mission,
    void* target, void* cell, void* nearCell);

/* 走跳板 (原函数前 10B), 不是被补丁的入口, 不递归。 */
static void OT_Replay(OtOrder* e)
{
    Fn_ClickedMission fn = (Fn_ClickedMission)g_OtTramp;
    unsigned char mf;
    if (!fn) return;
    mf = *(unsigned char*)ADDR_MOVE_FB;
    *(unsigned char*)ADDR_MOVE_FB = e->mf;
    fn(e->self, e->mission, e->target, e->cell, e->nearCell);
    *(unsigned char*)ADDR_MOVE_FB = mf;
}

/* self 必须活; target 空或活; cell 空或指针合理。 */
static int OT_EntryOk(OtOrder* e)
{
    if (!OT_ObjLive(e->self)) return 0;
    if (e->target && !OT_ObjLive(e->target)) return 0;
    if (e->cell && !IsPlausiblePtr(e->cell)) return 0;
    if (e->nearCell && !IsPlausiblePtr(e->nearCell)) return 0;
    return 1;
}

void OrderThrottle_Poll(void)
{
    static void* s_LastScen = NULL;
    void* scen;
    unsigned cnt;
    char buf[128], b1[16], b2[16];
    unsigned q;

    OT_EnsureInit();
    if (!g_OtEnable) { OT_Flush("disabled"); return; }
    OT_EnsureHook();
    /* 换图/读档: 队列里的对象与格指针全部作废。 */
    scen = *(void**)ADDR_SCENARIO;
    if (scen != s_LastScen) { s_LastScen = scen; OT_Flush("map changed"); }
    if (!OT_On()) { OT_Flush("inactive"); return; }
    if (s_QCount <= 0) return;

    /* 不发超: cnt 是 OutList 当前数的上界 —— 一条令恰好推进 1, 原版中途拒收只会
     * 让 cnt 高估, 高估是安全方向 (少发而非多发)。`cnt < EVT_CAP` 保证不溢出。 */
    cnt = *(unsigned*)ADDR_OUT_COUNT;
    if (cnt >= EVT_CAP) return;
    while (s_QCount > 0 && cnt < EVT_CAP)
    {
        OtOrder* e = s_QFront();
        e->wait++;
        if (e->wait >= OT_MAXWAIT) { s_QPop(); g_OtStatDrop++; continue; }
        if (OT_EntryOk(e)) { OT_Replay(e); cnt++; g_OtStatSent++; }
        else g_OtStatDrop++;
        s_QPop();
    }

    /* 统计 (~20 秒一行, 顺带报 pending, 确认节流在动)。 */
    if (*(int*)ADDR_FRAME - g_OtStatFrame >= OT_STATFRAMES)
    {
        g_OtStatFrame = *(int*)ADDR_FRAME;
        if (g_OtStatSent || g_OtStatDrop)
        {
            q = AppendCStr(buf, 0, "othrottle: sent=", sizeof(buf));
            q += UtoA((unsigned)g_OtStatSent, b1);
            q = AppendCStr(buf, q, " drop=", sizeof(buf));
            q += UtoA((unsigned)g_OtStatDrop, b2);
            q = AppendCStr(buf, q, " pending=", sizeof(buf));
            q += UtoA((unsigned)s_QCount, b1);
            LogLine(buf);
            g_OtStatSent = 0;
            g_OtStatDrop = 0;
        }
    }
}
