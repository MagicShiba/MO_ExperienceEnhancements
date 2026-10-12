/* OrderThrottle.cpp - 大量单位分帧下令。
 *
 * 原版 EventClass::OutList 一帧只装 128 条, 而一次下令给选区每个单位各发一条,
 * 于是第 129 条起被静默丢弃。钩两处: ClickedMission @ 0x6FFBE0 (右键移动/
 * 攻击、G 警戒, 槽 0x378) 与槽 0x374 @ 0x6FFE00 (S 停止传 6、E 进入系传 7,
 * 体内直塞 OutList, 同样一单位一条)。OutList 放得下且无排队则放行,
 * 否则入队; Poll 每帧经跳板补发, 判据与原版 Add 同一句。
 * 补发前验存活, 换图/超时丢弃。
 *
 * 配置 [Features] OrderThrottle (单键三态, 缺失=1, 改后重启):
 *   0=禁用 (完全旁路, 与原版逐位相同), 1=仅单机启用 (默认),
 *   2=联机也启用 (LAN/Internet 照常分帧)。
 * 2 纯粹保守: 分帧本身不影响同步 (见 notes/选中与下令上限.md §5), 挡的是
 * 下一层 Winsock 出站缓冲 —— 每帧命令量被顶到上限时的表现没验证过。
 *
 * 引擎事实与推导见 notes/选中与下令上限.md。 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"

#define OT_SECTION "Features"

/* ---------------- 地址 ---------------- */

#define ADDR_CLICKEDMISSION 0x006FFBE0u
#define OT_STOLEN   10u

/* 槽 0x374: S 停止 (传 6) / E 进入系 (传 7) 组命令的派发目标, 体内直塞 OutList。 */
#define ADDR_CLICKED374 0x006FFE00u
#define OT374_STOLEN 5u                       /* sub esp,0x78 + push esi + push edi */

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
static int g_OtMode = 1;        /* [Features] OrderThrottle: 0=禁用,1=仅单机,2=联机也启用 */
static int g_OtMpWarned = 0;
static int g_OtFirstLog = 0;

static unsigned long g_OtStatSent = 0, g_OtStatDrop = 0;
static int g_OtStatFrame = 0;
/* 诊断: 放行数 + 按 kind 入队数 + Attack 入队数。 */
static unsigned long s_DecPass = 0, s_DecQ0 = 0, s_DecQ1 = 0, s_DecQ2 = 0;
static unsigned long s_DecAtk = 0;

/* ---------------- 待发队列 (环形, 不另存 tail) ---------------- */

typedef struct
{
    void* self;        /* 被下令的对象 (TechnoClass*) */
    void* target;      /* pTarget, 可空 (kind=1/2 时无用) */
    void* cell;        /* TargetCell, 可空 (kind=1/2 时无用) */
    void* nearCell;   /* NearestTargetCellICanEnter, 可空 (kind=1/2 时无用) */
    int   mission;
    int   kind;        /* 0=ClickedMission(0x6FFBE0), 1=槽0x374(0x6FFE00, 单 int 参),
                        * 2=中键托管 raw 事件 (自带 111B, 补发=OU_Enqueue) */
    int   wait;        /* 已等逻辑帧 */
    unsigned char mf;  /* 入队瞬间的 MoveFeedback (补发时还原, 语音不变) */
    /* kind=2 专用 */
    char  ev[EVT_SIZE];
    void* whom;        /* 事件 Whom 单位 (存活校验用) */
    int   whomID;      /* Whom 的 Fetch_ID */
    int   idxOk;       /* 索引自检标志 (fail-open 用) */
    void* house;       /* 入队瞬间 CurrentPlayer (读档/换边防串令) */
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
static void* g_OtTramp374 = NULL;
static int   g_OtHookState374 = 0; /* 槽 0x374 钩, 状态机同上 */
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
    g_OtMode = GetPrivateProfileIntA(OT_SECTION, "OrderThrottle", 1, g_IniPath);
    if (g_OtMode < 0) g_OtMode = 0;
    if (g_OtMode > 2) g_OtMode = 2;
    q = AppendCStr(buf, q, "othrottle: mode=", sizeof(buf));
    q += UtoA((unsigned)g_OtMode, buf + q);
    LogLine(buf);
}

/* 0=禁用; 1=仅单机 (联机整模块空转, 默认配置); 2=联机也启用。 */
static int OT_On(void)
{
    OT_EnsureInit();
    if (g_OtMode <= 0) return 0;
    if (OT_IsMP() && g_OtMode < 2) return 0;
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

/* 入口栈形 (ClickedMission): [esp]=ret, +4=Mission, +8=pTarget, +0xC=TargetCell,
 * +0x10=NearestCell, ECX=this。pushal 后原栈在 esp+32。 */
static int __cdecl OT_DecideKind(void* self, int mission, void* target,
                                 void* cell, void* nearCell, int kind)
{
    OtOrder* e;
    unsigned cnt;
    OT_EnsureInit();
    if (g_OtMode <= 0) return 0;
    if (OT_IsMP() && g_OtMode < 2) return 0;
    /* 有排队保序; 无排队且原版放得下 (与 Add 同一句) 则放行。 */
    if (s_QCount <= 0)
    {
        cnt = *(unsigned*)ADDR_OUT_COUNT;
        if (cnt < EVT_CAP) { s_DecPass++; return 0; }
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
    e->kind = kind;
    e->wait = 0;
    e->mf = *(unsigned char*)ADDR_MOVE_FB;
    s_QCount++;
    if (kind == 0) s_DecQ0++; else s_DecQ1++;
    if (kind == 0 && mission == 1) s_DecAtk++;
    if (!g_OtFirstLog)
    {
        g_OtFirstLog = 1;
        LogLine("othrottle: first defer (queueing orders)");
    }
    return 1;
}

static int __cdecl OT_Decide(void* self, int mission, void* target,
                             void* cell, void* nearCell)
{
    return OT_DecideKind(self, mission, target, cell, nearCell, 0);
}

/* 槽 0x374 入口栈形: [esp]=ret, +4=int 参数 (S 传 6, E 系传 7), ECX=this。
 * pushal 后原栈在 esp+32。 */
static int __cdecl OT_Decide374(void* self, int arg)
{
    return OT_DecideKind(self, arg, NULL, NULL, NULL, 1);
}

static void* g_CallOtDecide = (void*)OT_Decide;
static void* g_CallOtDecide374 = (void*)OT_Decide374;

/* 中键托管: 预制好的标准 MegaMission 事件 (111B) 交 OT 跨帧补发。
 * 回 1=已接管 (调用方直接当成功); 回 0=没接管 (OT 关/联机未启用/队满),
 * 调用方走原 drip。存与发的都是标准事件、走同一 OutList 通道, 不影响同步;
 * 默认配置下联机回 0, 各家中键保持原 drip。 */
int OT_SubmitRaw(const char* ev, void* whom, int whomID, int idxOk)
{
    OtOrder* e;
    unsigned i;
    OT_EnsureInit();
    if (g_OtMode <= 0) return 0;
    if (OT_IsMP() && g_OtMode < 2) return 0;
    if (s_QCount >= OT_QCAP) return 0;   /* 队满: 调用方回落自家 drip, 绝不丢 */
    e = s_QTail();
    for (i = 0; i < EVT_SIZE; i++) e->ev[i] = ev[i];
    e->self = whom;
    e->whom = whom;
    e->whomID = whomID;
    e->idxOk = idxOk;
    e->house = *(void**)ADDR_CURPLAYER;
    e->target = NULL;
    e->cell = NULL;
    e->nearCell = NULL;
    e->mission = -1;
    e->kind = 2;
    e->wait = 0;
    e->mf = *(unsigned char*)ADDR_MOVE_FB;
    s_QCount++;
    s_DecQ2++;
    if (!g_OtFirstLog)
    {
        g_OtFirstLog = 1;
        LogLine("othrottle: first defer (queueing orders)");
    }
    return 1;
}

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

/* 槽 0x374 壳: 同铁律 (操作数只许 "m"), 返回 0=放行走跳板,
 * 非 0=已接管 (AL=返回值, ret 0x4)。原函数 ret $0x4, 这里同形。 */
__attribute__((naked)) void OrderThrottle_Entry374(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "movl 36(%%esp), %%ebx\n\t"      /* 原 [esp+4] = int 参数 */
        "movl 24(%%esp), %%esi\n\t"      /* pushal 帧 ECX 槽 = this */
        "pushl %%ebx\n\t"
        "pushl %%esi\n\t"
        "call *%0\n\t"
        "addl $8, %%esp\n\t"
        "movl %%eax, %1\n\t"             /* 跨 popal 传返回值 (只保证 AL) */
        "testl %%eax, %%eax\n\t"
        "jz 1f\n\t"
        "popal\n\t"
        "movl %1, %%eax\n\t"
        "ret $0x4\n\t"
        "1:\n\t"
        "popal\n\t"
        "jmp *%2\n\t"
        :
        : "m" (g_CallOtDecide374), "m" (g_OtRet), "m" (g_OtTramp374)
        : "memory", "cc");
}

/* 偷入口 stolen 字节 jmp 到壳, 跳板存原 stolen 字节 + 跳回 resume。
 * 被偷的字节须不含任何直接相对跳转, 跳板原样重放才安全。
 * 目标点已被别的补丁占了, 或字节对不上, 就整个放弃 (幂等, 不会反复试)。 */
static void OT_HookLog(const char* tag, const char* msg)
{
    char buf[64];
    unsigned q = 0;
    q = AppendCStr(buf, q, "othrottle: ", sizeof(buf));
    q = AppendCStr(buf, q, tag, sizeof(buf));
    q = AppendCStr(buf, q, msg, sizeof(buf));
    LogLine(buf);
}

static void OT_InstallHook(unsigned addr, unsigned stolen,
                             const unsigned char* want, void* entry,
                             void** trampOut, int* stateOut, const char* tag)
{
    unsigned char* site;
    unsigned char* st;
    DWORD old = 0;
    unsigned i;
    if (*stateOut) return;
    if (!IsPlausiblePtr((void*)addr)) return;
    if (*(int*)ADDR_FRAME < 60) return;      /* 等 exe 解包稳定 */
    site = (unsigned char*)addr;
    if (site[0] == 0xE9)
    {
        void* tgt = (void*)(site + 5 + *(int*)(site + 1));
        if (tgt == entry) { *stateOut = 1; return; }
        *stateOut = 2;
        OT_HookLog(tag, " taken by another patch - OFF");
        return;
    }
    for (i = 0; i < stolen; i++)
    {
        if (site[i] == want[i]) continue;
        *stateOut = 2;
        OT_HookLog(tag, " prologue mismatch - OFF");
        return;
    }
    st = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!st)
    {
        *stateOut = 2;
        OT_HookLog(tag, " VirtualAlloc fail - OFF");
        return;
    }
    for (i = 0; i < stolen; i++) st[i] = site[i];
    st[stolen] = 0xE9;
    *(int*)(st + stolen + 1) =
        (int)(long)(site + stolen) - (int)(long)(st + stolen + 5);
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
    {
        VirtualFree(st, 0, MEM_RELEASE);
        *stateOut = 2;
        OT_HookLog(tag, " VirtualProtect fail - OFF");
        return;
    }
    site[0] = 0xE9;
    *(int*)(site + 1) = (int)(long)entry - (int)(long)(site + 5);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    *trampOut = st;
    *stateOut = 1;
    OT_HookLog(tag, " hook ON");
}

static void OT_EnsureHook(void)
{
    /* 0x6FFBE0: `sub esp,0x88` + 四个 push, 偷 10B (跳板重放, 回跳 0x6FFBEA)。 */
    static const unsigned char kPrologue[OT_STOLEN] =
        { 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x53, 0x55, 0x56, 0x57 };
    /* 0x6FFE00: `sub esp,0x78` + push esi + push edi, 偷 5B (回跳 0x6FFE05)。 */
    static const unsigned char kPrologue374[OT374_STOLEN] =
        { 0x83, 0xEC, 0x78, 0x56, 0x57 };
    OT_InstallHook(ADDR_CLICKEDMISSION, OT_STOLEN, kPrologue,
                   (void*)OrderThrottle_Entry, &g_OtTramp, &g_OtHookState,
                   "ClickedMission");
    OT_InstallHook(ADDR_CLICKED374, OT374_STOLEN, kPrologue374,
                   (void*)OrderThrottle_Entry374, &g_OtTramp374,
                   &g_OtHookState374, "Clicked374");
}

/* ---------------- 补发 ---------------- */

typedef int __attribute__((thiscall)) (*Fn_ClickedMission)(void* self, int mission,
    void* target, void* cell, void* nearCell);
typedef int __attribute__((thiscall)) (*Fn_Clicked374)(void* self, int arg);

/* 走跳板 (原函数头部字节, 不是被补丁的入口, 不递归)。kind=1 的原函数
 * (0x6FFE00) 内部自带 128 检查 (满静默丢但返 1), Poll 已保证 cnt<128 才调。
 * kind=2 直接 OU_Enqueue, 满回 0 (调用方 Poll 见 0 则 break 留下轮)。
 * 回 1=已处理 (pop), 0=OutList 真满 (break, 不 pop)。 */
static int OT_Replay(OtOrder* e)
{
    unsigned char mf;
    if (e->kind == 2) return OU_Enqueue(e->ev);
    if (e->kind == 1)
    {
        Fn_Clicked374 fn = (Fn_Clicked374)g_OtTramp374;
        if (!fn) return 1;
        mf = *(unsigned char*)ADDR_MOVE_FB;
        *(unsigned char*)ADDR_MOVE_FB = e->mf;
        fn(e->self, e->mission);
        *(unsigned char*)ADDR_MOVE_FB = mf;
        return 1;
    }
    {
        Fn_ClickedMission fn = (Fn_ClickedMission)g_OtTramp;
        if (!fn) return 1;
        mf = *(unsigned char*)ADDR_MOVE_FB;
        *(unsigned char*)ADDR_MOVE_FB = e->mf;
        fn(e->self, e->mission, e->target, e->cell, e->nearCell);
        *(unsigned char*)ADDR_MOVE_FB = mf;
        return 1;
    }
}

/* self 必须活; kind=0 另验 target/cell;
 * kind=1 只有 self; kind=2 用 Fetch_ID 二分验 Whom + house 未变。 */
static int OT_EntryOk(OtOrder* e)
{
    void* cur;
    if (!OT_ObjLive(e->self)) return 0;
    if (e->kind == 1) return 1;
    if (e->kind == 2)
    {
        cur = *(void**)ADDR_CURPLAYER;
        if (!cur || cur != e->house) return 0;
        return OU_PtrAlive(e->whom, e->whomID, e->idxOk);
    }
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
    char buf[192], b1[16], b2[16];
    unsigned q;

    OT_EnsureInit();
    if (g_OtMode <= 0) { OT_Flush("disabled"); return; }
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
        if (!OT_EntryOk(e)) { s_QPop(); g_OtStatDrop++; continue; }
        if (!OT_Replay(e)) break;   /* OutList 真满 (仅 kind=2 回 0): 留到下轮 */
        cnt++;
        g_OtStatSent++;
        s_QPop();
    }

    /* 统计 (~20 秒一行, 顺带报 pending, 确认节流在动)。
     * q0/q1/q2=三路入队累计 (0=右键系, 1=S/E 组命令, 2=中键托管),
     * atk=Attack 入队累计, pass=水位放行累计。 */
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
            q = AppendCStr(buf, q, " q0=", sizeof(buf));
            q += UtoA((unsigned)s_DecQ0, b2);
            q = AppendCStr(buf, q, " q1=", sizeof(buf));
            q += UtoA((unsigned)s_DecQ1, b1);
            q = AppendCStr(buf, q, " q2=", sizeof(buf));
            q += UtoA((unsigned)s_DecQ2, b2);
            q = AppendCStr(buf, q, " atk=", sizeof(buf));
            q += UtoA((unsigned)s_DecAtk, b1);
            q = AppendCStr(buf, q, " pass=", sizeof(buf));
            q += UtoA((unsigned)s_DecPass, b2);
            LogLine(buf);
            g_OtStatSent = 0;
            g_OtStatDrop = 0;
        }
    }
}
