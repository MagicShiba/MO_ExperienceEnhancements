/* OrderThrottle.cpp - 大量单位下令: 单机同步直写 DoList, 联机分帧排队。
 *
 * 原版 EventClass::OutList 一帧只装 128 条, 一次下令每单位各发一条,
 * 第 129 条起静默丢弃。钩两处上游: ClickedMission @ 0x6FFBE0 (右键移动/
 * 攻击、G 警戒) 与槽 0x374 @ 0x6FFE00 (S 停止、E 进入系); 另钩三处满分支
 * (Mega @ 0x646EF5、374 槽 @ 0x6FFE55、正统入队 @ 0x6521C9, Z 路径点走这里)。
 *
 * 单机永不分帧: 单位令一律直写 DoList (原版转运手续, 执行帧 cur+1),
 * 上限即 DoList 的 16384。联机保持原版语义, OrderThrottle=2 时普通令
 * 才跨帧排队, 规划令永不排队。补发前验存活, 换图/超时丢弃。
 *
 * 联机完全绕过 (实时判定, 不缓存): 默认 mode<2 时联机一个钩不留
 * (不装新钩, 已装的恢复原版字节并释放跳板); mode=2 联机只留两处上游钩,
 * 溢出钩 (SP 直写用) 照样不装。
 *
 * 配置 [Features] OrderThrottle (缺失=1, 改后重启):
 *   0=禁用 (完全旁路), 1=单机直写+联机原版 (默认), 2=联机普通令也分帧。
 * 分帧不影响同步 (见 notes/选中与下令上限.md §5)。
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

/* 溢出钩 (SP 直写 DoList, 同波不分队; MP 走原版失败路, 行为不变):
 * MegaMission 内联追加 @ 0x646EF5, 槽 0x374 内联追加 @ 0x6FFE55,
 * 正统入队 @ 0x6521C9 (Z 路径点经 0x637DD0 调 0x6521C0 走这里, 另 4 处调用方同覆盖)。 */
#define ADDR_MEGAFULL  0x00646EF5u
#define ADDR_MEGANORM  0x00646F01u
#define ADDR_MEGASUCC  0x00646F4Eu   /* mov $1,al + 原版尾 (直写成功走这里, 返 1) */
#define ADDR_MEGAFAIL  0x00646F58u   /* xor al,al + 原版尾 */
#define ADDR_STOPFULL  0x006FFE55u
#define ADDR_STOPNORM  0x006FFE61u
#define ADDR_STOPTAIL  0x006FFEADu   /* 成功/失败同落 (都返 1, 与原版静默丢同形) */
#define ADDR_ENQFULL  0x006521C9u
#define ADDR_ENQNORM  0x006521D5u
#define ADDR_ENQSUCC  0x00652225u   /* mov al,1 + ret (直写成功走这里, 返 1) */
#define ADDR_ENQFAIL  0x0065222Au

/* 正统入队 (0x6521C0) 的满分支 (`mov OutCount,%eax + cmp $0x80,%eax` 10B + jge):
 * Z 规划 (0x637DD0 及另 4 处调用方) 全走这里进 OutList, 满静默丢。
 * 事件取 entry-esp+0x4; eax 原样保留 (追加体 `mov %eax,%esi` 当源用)。
 * r==0→追加体, r==1→成功尾 (事件已落 DoList), r==2→失败尾 (原版)。
 * 三尾皆 reg-safe, esp 进出平衡。 */

#define ADDR_SCENARIO 0xA8B230u   /* ScenarioClass::Instance (换图代号) */
#define ADDR_FRAME    0xA8ED84u   /* Unsorted::CurrentFrame */
#define ADDR_MOVE_FB  0x822CF2u   /* Unsorted::MoveFeedback */
#define ADDR_PLANMODE 0xAC4CF4u   /* PlanningModeActive (ClickedMission 内 0x637AA0 读它定规划令) */

#define OFF_INLIMBO 0x81u
#define OFF_ISALIVE 0x90u

#define OT_QCAP     1024    /* 待发队列上限 (2 的幂, & 掩码做环形) */
#define OT_QMASK    (OT_QCAP - 1)
#define OT_MAXWAIT  600     /* 单条最多等 600 逻辑帧 (~10s), 超时丢弃 */
#define OT_STATFRAMES 1200  /* 统计日志间隔 (~20 秒) */

/* ---------------- 配置 ---------------- */

static int g_OtInit = 0;
static int g_OtMode = 1;        /* [Features] OrderThrottle: 0=禁用,1=仅单机,2=联机也启用 */
static int g_OtMpBypassLogged = 0;   /* 联机绕过日志只打一次 (回单机清零) */
static int g_OtFirstLog = 0;
static void OT_MarkFirstDefer(void);

static unsigned long g_OtStatSent = 0, g_OtStatDrop = 0;
static int g_OtStatFrame = 0;
/* 诊断: 放行数 + 按 kind 入队数 + Attack 入队数。 */
static unsigned long s_DecPass = 0, s_DecQ0 = 0, s_DecQ1 = 0, s_DecQ2 = 0;
static unsigned long s_DecAtk = 0;
/* 诊断: 单机直写成功/失败累计 (+上次已上报失败快照, 变才打日志)。 */
static unsigned long s_SpillOk = 0, s_SpillFail = 0;
static unsigned long s_SpillRepFail = 0;

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
    unsigned char plan; /* 入队瞬间的 PlanningModeActive (补发时还原, 防规划令变直发) */
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
static int   g_OtHookStateMega = 0;
static int   g_OtHookStateStop = 0;
static volatile int g_OtRet = 0;    /* 裸壳跨 popal 传返回值 */

/* 对象指针须再验虚表落 exe 内 (IsPlausiblePtr 只判范围对齐)。 */
static int OT_IsObj(void* p)
{
    if (!p || !IsPlausiblePtr(p)) return 0;
    return OU_InExe((unsigned)(size_t)*(void***)p);
}

static int OT_ObjLive(void* p)
{
    if (!OT_IsObj(p)) return 0;
    if (!((unsigned char*)p)[OFF_ISALIVE]) return 0;
    if (((unsigned char*)p)[OFF_INLIMBO]) return 0;
    return 1;
}

/* 联机判定见 OU_IsMP (OrderUtil.h, 订单系统共用)。 */

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
    if (OU_IsMP() && g_OtMode < 2) return 0;
    return 1;
}

static void OT_Flush(const char* why)
{
    char buf[96];
    unsigned q = 0;
    if (s_QCount <= 0) return;
    q = AppendCStr(buf, q, "othrottle: drop queued=", sizeof(buf));
    q = OU_AppendU(buf, q, (unsigned)s_QCount, sizeof(buf));
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
    int mp;
    OT_EnsureInit();
    if (g_OtMode <= 0) return 0;
    if (*(unsigned char*)ADDR_PLANMODE) { s_DecPass++; return 0; }   /* 规划令永不排队 */
    mp = OU_IsMP();
    if (mp && g_OtMode < 2) return 0;
    if (!mp) { s_DecPass++; return 0; }   /* SP: 永不排队, 同步直行 */
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
    e->plan = *(unsigned char*)ADDR_PLANMODE;
    s_QCount++;
    if (kind == 0) s_DecQ0++; else s_DecQ1++;
    if (kind == 0 && mission == 1) s_DecAtk++;
    OT_MarkFirstDefer();
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

/* OutList 满处置 (溢出钩用): MP 走原版 (0=有空位追加, 2=满失败);
 * SP 一律直写 DoList (1=成功, 2=DoList 满), 上限 16384。 */
static int __cdecl OT_SpillDecide(const char* ev)
{
    if (OU_IsMP())
        return (*(unsigned*)ADDR_OUT_COUNT < EVT_CAP) ? 0 : 2;
    if (OU_DirectToDoList(ev)) { s_SpillOk++; return 1; }
    s_SpillFail++;
    return 2;
}

static void* g_CallSpillDecide = (void*)OT_SpillDecide;

static void* g_MegaNormal = (void*)ADDR_MEGANORM;
static void* g_MegaSucc = (void*)ADDR_MEGASUCC;
static void* g_MegaFail = (void*)ADDR_MEGAFAIL;
static void* g_StopNormal = (void*)ADDR_STOPNORM;
static void* g_StopTail = (void*)ADDR_STOPTAIL;

/* 0x646E90 满分支壳: 事件指针取 eax (原版追加体 `mov %eax,%esi` 的同一源,
 * 即 0x646EF0 处 call 的返回值, 与调用者栈形无关)。壳原样保留 eax
 * (判定结果走 ecx 中转); ecx/edx 在三目标处都重建, 可借用。 */
__attribute__((naked)) void OrderThrottle_MegaSpillEntry(void)
{
    __asm__ volatile (
        "pushl %%eax\n\t"            /* 存原 eax (=真事件, 追加体也要它) */
        "pushl %%eax\n\t"            /* dec 参数: 直接传 eax 值, 不算 esp 偏移 */
        "call *%0\n\t"
        "addl $4, %%esp\n\t"
        "movl %%eax, %%ecx\n\t"     /* dec 结果转 ecx */
        "popl %%eax\n\t"            /* 恢复原 eax, esp 回到入口值 */
        "cmpl $1, %%ecx\n\t"
        "je 1f\n\t"
        "cmpl $2, %%ecx\n\t"
        "je 2f\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        "jmp *%2\n\t"
        "2:\n\t"
        "jmp *%3\n\t"
        :
        : "m" (g_CallSpillDecide), "m" (g_MegaNormal),
          "m" (g_MegaSucc), "m" (g_MegaFail)
        : "cc", "memory");
}

/* 0x6FFE00 满分支壳: 事件在 esp+0x10。r==0 走原追加体,
 * r==1/2 同落原版尾 (都返 1, 直写成功与原版静默丢同形)。
 * 追加体 (0x6FFE61) 首句即重建 eax/esi, 不依赖入口寄存器, 故无需保留。 */
__attribute__((naked)) void OrderThrottle_StopSpillEntry(void)
{
    __asm__ volatile (
        "leal 0x10(%%esp), %%eax\n\t"
        "pushl %%eax\n\t"
        "call *%0\n\t"
        "addl $4, %%esp\n\t"
        "testl %%eax, %%eax\n\t"
        "jnz 1f\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        "jmp *%2\n\t"
        :
        : "m" (g_CallSpillDecide), "m" (g_StopNormal), "m" (g_StopTail)
        : "cc", "memory");
}

static void* g_EnqNormal = (void*)ADDR_ENQNORM;
static void* g_EnqSucc = (void*)ADDR_ENQSUCC;
static void* g_EnqFail = (void*)ADDR_ENQFAIL;
static int   g_OtHookStateEnq = 0;

/* 正统入队溢出壳 (见 ADDR_ENQ* 注释): ev=入口 esp+0x4, eax 原样保留。 */
__attribute__((naked)) void OrderThrottle_EnqSpillEntry(void)
{
    __asm__ volatile (
        "pushl %%eax\n\t"            /* 存原 eax (=OutCount, 追加体要它) */
        "leal 0x08(%%esp), %%eax\n\t" /* 事件 (入口 esp+0x4) */
        "pushl %%eax\n\t"
        "call *%0\n\t"
        "addl $4, %%esp\n\t"
        "movl %%eax, %%ecx\n\t"
        "popl %%eax\n\t"            /* 恢复, esp 回到入口值 */
        "cmpl $1, %%ecx\n\t"
        "je 1f\n\t"
        "cmpl $2, %%ecx\n\t"
        "je 2f\n\t"
        "jmp *%1\n\t"
        "1:\n\t"
        "jmp *%2\n\t"
        "2:\n\t"
        "jmp *%3\n\t"
        :
        : "m" (g_CallSpillDecide), "m" (g_EnqNormal),
          "m" (g_EnqSucc), "m" (g_EnqFail)
        : "cc", "memory");
}

/* 中键托管: 预制好的标准 MegaMission 事件 (111B) 交 OT 队列。
 * 单机永不接管 (回 0, 调用方 OU_Enqueue 单机直写 DoList);
 * 联机仅 OrderThrottle=2 时接管 (回 1, 跨帧补发), 否则回 0 走原 drip。
 * 存与发的都是标准事件; 联机补发走同一 OutList 通道, 不影响同步。 */
int OT_SubmitRaw(const char* ev, void* whom, int whomID, int idxOk)
{
    OtOrder* e;
    int mp;
    OT_EnsureInit();
    if (g_OtMode <= 0) return 0;
    mp = OU_IsMP();
    if (mp && g_OtMode < 2) return 0;
    if (!mp) return 0;   /* SP: 调用方 OU_Enqueue 直发/直注, 不进 OT 队 */
    if (s_QCount >= OT_QCAP) return 0;   /* 队满: 调用方回落自家 drip, 绝不丢 */
    e = s_QTail();
    OU_CopyEvt(e->ev, ev);
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
    s_QCount++;
    s_DecQ2++;
    OT_MarkFirstDefer();
    return 1;
}

int OT_Fire(char* ev, void* whom, int whomID, int idxOk)
{
    if (OT_SubmitRaw(ev, whom, whomID, idxOk)) return 1;
    return OU_Enqueue(ev);
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
    q = AppendCStr(buf, q, tag ? tag : "?", sizeof(buf));
    OU_LogTag(buf, msg);
}

static void OT_MarkFirstDefer(void)
{
    if (!g_OtFirstLog)
    {
        g_OtFirstLog = 1;
        LogLine("othrottle: first defer (queueing orders)");
    }
}

/* 钩点原版字节 (安装与卸载共用)。溢出钩只比对前 5B, 后 5B 原样不动。 */
static const unsigned char kOtPrologue[OT_STOLEN] =
    { 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00, 0x53, 0x55, 0x56, 0x57 };
static const unsigned char kOtPrologue374[OT374_STOLEN] =
    { 0x83, 0xEC, 0x78, 0x56, 0x57 };
static const unsigned char kOtOutFull[10] =
    { 0x81, 0x3D, 0xC8, 0x02, 0xA8, 0x00, 0x80, 0x00, 0x00, 0x00 };
static const unsigned char kOtEnqFull[10] =
    { 0xA1, 0xC8, 0x02, 0xA8, 0x00, 0x3D, 0x80, 0x00, 0x00, 0x00 };

/* 钩点只需落在 gamemd .text 内, 不必 4 对齐。 */

static void OT_InstallHook(unsigned addr, unsigned stolen,
                             const unsigned char* want, void* entry,
                             void** trampOut, int* stateOut, const char* tag)
{
    unsigned char* site;
    unsigned char* st = NULL;
    DWORD old = 0;
    unsigned i;
    if (*stateOut) return;
    if (!OU_InExe(addr)) return;
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
    /* 溢出钩 (trampOut 空) 不需跳板: 壳自行分流到原成功/失败尾。 */
    if (trampOut)
    {
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
    }
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old))
    {
        if (st) VirtualFree(st, 0, MEM_RELEASE);
        *stateOut = 2;
        OT_HookLog(tag, " VirtualProtect fail - OFF");
        return;
    }
    site[0] = 0xE9;
    *(int*)(site + 1) = (int)(long)entry - (int)(long)(site + 5);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    if (trampOut) *trampOut = st;
    *stateOut = 1;
    OT_HookLog(tag, " hook ON");
}

/* 联机绕过卸载: 自家钩还在就恢复原版, 状态清 0 (下次单机重装)。
 * 跳板钩有跳板就从跳板恢复, 无跳板则从原版字节恢复; 溢出钩恢复前 5B。
 * 已不是自家钩则只清状态。 */
static void OT_Uninstall(unsigned addr, unsigned stolen, const unsigned char* want,
                         void* entry, void** trampOut, int* stateOut, const char* tag)
{
    unsigned char* site;
    unsigned char* st;
    DWORD old = 0;
    unsigned i;
    unsigned n;
    if (*stateOut != 1) return;
    if (!OU_InExe(addr)) return;
    site = (unsigned char*)addr;
    st = trampOut ? (unsigned char*)*trampOut : NULL;
    if (site[0] != 0xE9 ||
        (void*)(site + 5 + *(int*)(site + 1)) != entry)
    {
        if (st) { VirtualFree(st, 0, MEM_RELEASE); *trampOut = NULL; }
        *stateOut = 0;
        return;
    }
    n = trampOut ? stolen : 5;
    if (!VirtualProtect(site, n, PAGE_EXECUTE_READWRITE, &old)) return;
    for (i = 0; i < n; i++) site[i] = st ? st[i] : want[i];
    VirtualProtect(site, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, n);
    if (st) { VirtualFree(st, 0, MEM_RELEASE); *trampOut = NULL; }
    *stateOut = 0;
    OT_HookLog(tag, " MP bypass, hook removed");
}

static void OT_EnsureHook(void)
{
    int mp;
    OT_EnsureInit();
    mp = OU_IsMP();
    if (mp && g_OtMode < 2)
    {
        /* 联机默认 (mode<2): 整模块绕过, 一个钩不留。Decide 本就实时放行, 队列恒空。 */
        OT_Uninstall(ADDR_CLICKEDMISSION, OT_STOLEN, kOtPrologue,
                     (void*)OrderThrottle_Entry, &g_OtTramp, &g_OtHookState,
                     "ClickedMission");
        OT_Uninstall(ADDR_CLICKED374, OT374_STOLEN, kOtPrologue374,
                     (void*)OrderThrottle_Entry374, &g_OtTramp374,
                     &g_OtHookState374, "Clicked374");
        OT_Uninstall(ADDR_MEGAFULL, 10u, kOtOutFull,
                     (void*)OrderThrottle_MegaSpillEntry, NULL,
                     &g_OtHookStateMega, "MegaSpill");
        OT_Uninstall(ADDR_STOPFULL, 10u, kOtOutFull,
                     (void*)OrderThrottle_StopSpillEntry, NULL,
                     &g_OtHookStateStop, "StopSpill");
        OT_Uninstall(ADDR_ENQFULL, 10u, kOtEnqFull,
                     (void*)OrderThrottle_EnqSpillEntry, NULL,
                     &g_OtHookStateEnq, "EnqSpill");
        if (!g_OtMpBypassLogged)
        {
            g_OtMpBypassLogged = 1;
            LogLine("othrottle: MP bypass (vanilla, no hooks)");
        }
        return;
    }
    g_OtMpBypassLogged = 0;
    OT_InstallHook(ADDR_CLICKEDMISSION, OT_STOLEN, kOtPrologue,
                   (void*)OrderThrottle_Entry, &g_OtTramp, &g_OtHookState,
                   "ClickedMission");
    OT_InstallHook(ADDR_CLICKED374, OT374_STOLEN, kOtPrologue374,
                   (void*)OrderThrottle_Entry374, &g_OtTramp374,
                   &g_OtHookState374, "Clicked374");
    /* 三处溢出钩只在单机装 (mode=2 联机也不装)。 */
    if (mp) return;
    OT_InstallHook(ADDR_MEGAFULL, 10u, kOtOutFull,
                   (void*)OrderThrottle_MegaSpillEntry, NULL,
                   &g_OtHookStateMega, "MegaSpill");
    OT_InstallHook(ADDR_STOPFULL, 10u, kOtOutFull,
                   (void*)OrderThrottle_StopSpillEntry, NULL,
                   &g_OtHookStateStop, "StopSpill");
    /* 正统入队满分支 (`mov OutCount,%eax + cmp $0x80,%eax`)。
     * Z 路径点经 0x637DD0 调 0x6521C0 全走这里, 一钩覆盖 5 处调用方。 */
    OT_InstallHook(ADDR_ENQFULL, 10u, kOtEnqFull,
                   (void*)OrderThrottle_EnqSpillEntry, NULL,
                   &g_OtHookStateEnq, "EnqSpill");
}

/* ---------------- 补发 ---------------- */

typedef int __attribute__((thiscall)) (*Fn_ClickedMission)(void* self, int mission,
    void* target, void* cell, void* nearCell);
typedef int __attribute__((thiscall)) (*Fn_Clicked374)(void* self, int arg);

/* 走跳板 (原函数头部, 不递归)。kind=2 直接 OU_Enqueue, 满回 0 留下轮。
 * 回 1=已处理, 0=跳板未装 (留下轮重试)。 */
static int OT_Replay(OtOrder* e)
{
    unsigned char mf, pm;
    int ok = 0;
    if (e->kind == 2) return OU_Enqueue(e->ev);
    /* 原函数读 live 的 MoveFeedback 与 PlanningModeActive, 还原成入队瞬间再调。 */
    mf = *(unsigned char*)ADDR_MOVE_FB;
    pm = *(unsigned char*)ADDR_PLANMODE;
    *(unsigned char*)ADDR_MOVE_FB = e->mf;
    *(unsigned char*)ADDR_PLANMODE = e->plan;
    if (e->kind == 1)
    {
        Fn_Clicked374 fn = (Fn_Clicked374)g_OtTramp374;
        if (fn) { fn(e->self, e->mission); ok = 1; }
    }
    else
    {
        Fn_ClickedMission fn = (Fn_ClickedMission)g_OtTramp;
        if (fn) { fn(e->self, e->mission, e->target, e->cell, e->nearCell); ok = 1; }
    }
    *(unsigned char*)ADDR_MOVE_FB = mf;
    *(unsigned char*)ADDR_PLANMODE = pm;
    return ok;
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

    OT_EnsureInit();
    if (g_OtMode <= 0) { OT_Flush("disabled"); return; }
    OT_EnsureHook();
    /* 换图/读档: 队列里的对象与格指针全部作废。 */
    scen = *(void**)ADDR_SCENARIO;
    if (scen != s_LastScen)
    {
        s_LastScen = scen;
        OT_Flush("map changed");
    }
    if (!OT_On()) { OT_Flush("inactive"); return; }

    /* 统计 (~20 秒一结): 正常时静默, 只在丢单/直写失败时记一行。
     * q0/q1/q2=三路入队, atk=Attack 入队, pass=直发放行,
     * spill=单机直写成功/失败, do=DoList 当前数。 */
    if (*(int*)ADDR_FRAME - g_OtStatFrame >= OT_STATFRAMES)
    {
        char buf[192];
        unsigned q;
        int bad = (g_OtStatDrop || s_SpillFail != s_SpillRepFail) ? 1 : 0;
        g_OtStatFrame = *(int*)ADDR_FRAME;
        if (bad)
        {
            q = AppendCStr(buf, 0, "othrottle: sent=", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)g_OtStatSent, sizeof(buf));
            q = AppendCStr(buf, q, " drop=", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)g_OtStatDrop, sizeof(buf));
            q = AppendCStr(buf, q, " pending=", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)s_QCount, sizeof(buf));
            q = AppendCStr(buf, q, " q0=", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)s_DecQ0, sizeof(buf));
            q = AppendCStr(buf, q, " q1=", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)s_DecQ1, sizeof(buf));
            q = AppendCStr(buf, q, " q2=", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)s_DecQ2, sizeof(buf));
            q = AppendCStr(buf, q, " atk=", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)s_DecAtk, sizeof(buf));
            q = AppendCStr(buf, q, " pass=", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)s_DecPass, sizeof(buf));
            q = AppendCStr(buf, q, " spill=", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)s_SpillOk, sizeof(buf));
            q = AppendCStr(buf, q, "/", sizeof(buf));
            q = OU_AppendU(buf, q, (unsigned)s_SpillFail, sizeof(buf));
            q = AppendCStr(buf, q, " do=", sizeof(buf));
            q = OU_AppendU(buf, q, *(unsigned*)ADDR_DO_COUNT, sizeof(buf));
            LogLine(buf);
        }
        g_OtStatSent = 0;
        g_OtStatDrop = 0;
        s_SpillRepFail = s_SpillFail;
    }
    if (s_QCount <= 0) return;   /* SP 永不排队, 到此恒为 0; 下为联机模式 2 专用 */

    /* cnt 是 OutList 数的上界 (一条令推进 1, 中途拒收只会高估=少发方向)。 */
    cnt = *(unsigned*)ADDR_OUT_COUNT;
    if (cnt >= EVT_CAP) return;
    while (s_QCount > 0 && cnt < EVT_CAP)
    {
        OtOrder* e = s_QFront();
        e->wait++;
        if (e->wait >= OT_MAXWAIT) { s_QPop(); g_OtStatDrop++; continue; }
        if (!OT_EntryOk(e)) { s_QPop(); g_OtStatDrop++; continue; }
        if (!OT_Replay(e)) break;   /* 满或跳板未装: 留下轮 */
        cnt++;
        g_OtStatSent++;
        s_QPop();
    }
}
