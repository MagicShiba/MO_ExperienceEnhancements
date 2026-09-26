/* PatrolCircle.cpp - Z+中键一键巡逻圈 (选中单位直接巡逻)。
 *
 * 触发: 按住 Z (PlanningMode 键) 的同时按鼠标中键 (可配 TriggerVK:
 * 4=中键默认, 5=侧键1, 6=侧键2) 锚定起点, 划线后松开以终点定圈:
 * 起终连线为中线, 四周各外扩 Size/2 (端点沿方向也外扩 Size/2) 成矩环;
 * 点按 (起终两格内) 则以松开格为中心布 4x4 方环 (±2 角) 并开始巡逻。
 * 配置全在 MsgFont.ini [PatrolCircle]。全程不碰光标。
 * 注意: 中键按住拖时游戏可能平移镜头, 起终格按各自时刻镜头映射。
 *
 * 原理 (原生序列实证: 幽灵点击抓到的 bq:ev + 反汇编交叉确认):
 *   新角点击 → MegaMission (Type 4, ctor 0x4C6860, Whom=单位,
 *   Mission=Move, Target=空, Destination=格子, IsPlanningEvent=1),
 *   执行侧 0x638120 建全局节点 + 挂 token。
 *   点已存在节点 (闭环) → UI 直接调 0x639740 (fastcall ECX=单位,
 *   EDX=节点, 置 ClosedLoopNodeCount/StepsToClosedLoop/closed 标志并
 *   刷新渲染, 不发事件不碰全局模式), 同时发 PlanConnect 做同步。
 *   Connect 执行侧 (0x637EFE) 的 vcall 在实践中关不上 (v3/v7 实证
 *   loop 恒 0, 原因应为 token 内分支语义与关闭要求不一致), 故闭环
 *   走与 UI 完全相同的直接调用 (节点取自各单位自己 token[0], 天然
 *   在 token 内, 不怕跨单位重复节点)。
 *   最后 PlanCommit (0x2B) 执行。
 *   序列: 首单位 Mega×4 建节点 → 跟随者 Connect×4/人挂接 → 合环
 *   (全员 Mega(吸收)+全员 Connect, 节点已存在) → 直接关 → 验 loop>0
 *   → Commit。闭环加新点由原生拦截 (消息+丢弃)。事件按序进 OutList,
 *   执行保序; 各阶段就绪即走, 无固定等待。
 *   TargetCell: 0x6E6B20 (ID=X+1000*Y, RTTI=11);
 *   TargetUnit: 0x6E6AB0 (ID=Fetch_ID, RTTI=Abstract=52)。
 *   入队走 OutList (与 BuildQueue 同式, 满则跨帧续发)。
 *   选中表 0xA8ECBC/0xA8ECC8; 本家 CurrentPlayer 0xA83D4C+[0x30];
 *   中心格经 Display::ProcessClickCoords 0x692300; 角校验经
 *   MapCells=[0x87F7E8+0x13C]/MaxCells=[0x87F7E8+0x140]。
 *
 * 约束 (勿踩):
 *   只发 Connect 无节点可链 (全丢弃); 全单位 Mega 跨帧分批会建重复
 *   (人手一套圈); Connect 在空 token 上挂不上, 且执行侧关不上环——
 *   故首单位 Mega 建节点 + 其余 Connect 挂接, 建与挂分阶段发 (同突发
 *   Connect 先跑会被丢弃), 闭环直接调 UI 关闭函数 (与手点同函数,
 *   节点取自各单位自己 token[0]), 最后 Commit。
 * 行为:
 *   不限单位 (OutList 满则 drip 跨帧续发, Mega 恒首单位 4 个);
 *   各阶段就绪即走无固定等待 (帧不动时 poll 数兜底防 busy 卡死);
 *   token 就绪须深度校验 (数组+计数+首节点, 防门控抢跑);
 *   快照跨帧须先经 Fetch_ID 索引验活 (自检失败则 fail-open);
 *   不删路径不拦添加——闭环加新点由原生拦截 (消息+丢弃),
 *   未闭环残留按原生语义延长。
 *
 * 联机: 只发标准事件, 各端同一份原版代码执行, 不影响联机。
 * 本地只读 + 标准事件入队, 不写模拟内存。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MsgFont 同一 DLL)。
 * 轮询挂 PowerDrawBoth (见 MsgFont.cpp), 开关关时直接返回。
 */

#include <windows.h>
#include <math.h>
#include "GdiText.h"

#define PC_SECTION "PatrolCircle"

/* ---------------- 地址 (全只读, 写模拟一律禁止) ---------------- */

#define ADDR_CURPLAYER     0xA83D4Cu
#define OFF_HOUSE_INDEX    0x30u
#define ADDR_SELOBJ_ITEMS  0xA8ECBCu
#define ADDR_SELOBJ_COUNT  0xA8ECC8u

#define ADDR_OUT_COUNT 0xA802C8u
#define ADDR_OUT_NEXT  0xA802D0u
#define ADDR_OUT_BUF   0xA802D4u
#define ADDR_OUT_PAR   0xA83A54u
#define EVT_SIZE 111u
#define EVT_CAP  0x80u

#define ADDR_DISPLAY   0x87F7E8u
#define OFF_MAP_CELLS  0x13Cu
#define OFF_MAP_MAXC   0x140u

#define EVT_MEGAMISSION 0x04u
#define EVT_PLANCONNECT 0x2Au
#define EVT_PLANCOMMIT  0x2Bu
#define OFF_PLANFLAG  0x1Du   /* MegaMission.IsPlanningEvent (ctor 清零, 发送侧置 1) */
#define MISSION_MOVE  2

#define ABS_UNIT 1
#define ABS_AIRCRAFT 2
#define ABS_INFANTRY 15

#define OFF_TECH_TOKEN 0x514u  /* Techno PlanningToken (0x705D20 实证) */
#define OFF_TOK_LOOPN 0x90u   /* PlanningToken ClosedLoopNodeCount */
#define OFF_TOK_NODES 0x8u    /* PlanningToken PlanningNodes 数组 */
#define OFF_TOK_NCOUNT 0x14u  /* PlanningToken 节点数 */

typedef struct { short X, Y; } CellStruct;
typedef struct { int X, Y, Z; } CoordStruct;

typedef void* __attribute__((thiscall)) (*Fn_TargetFromCell)(void* tgt, const CellStruct* cell);
typedef void* __attribute__((thiscall)) (*Fn_TargetFromObj)(void* tgt, void* obj);
/* MegaMission ctor (0x4C6860, ECX=ev111, 栈 0x28):
 * house, srcID, srcRTTI, mission, tgtID, tgtRTTI, dstID, dstRTTI, folID, folRTTI */
typedef void* __attribute__((thiscall)) (*Fn_MegaMissionCtor)(void* ev, int houseIdx,
    int srcID, unsigned srcRttiW, int mission,
    int tgtID, unsigned tgtRttiW, int dstID, unsigned dstRttiW,
    int folID, unsigned folRttiW);
/* PlanConnect ctor (0x4C6780, ECX=ev111, ret 0x18):
 * house, type(0x2A), srcID, srcRTTI(dword, 取低字节),
 * dstID, dstRTTI。布局: Type@0, House@2, Frame@3(取[0xA8ED84]),
 * Target1(单位)@7(ID@7+RTTI@11), Target2(格)@12(ID@12+RTTI@16)。 */
typedef void* __attribute__((thiscall)) (*Fn_PlanConnectCtor)(void* ev, int houseIdx, int evtType,
    int srcID, unsigned srcRttiW, int dstID, unsigned dstRttiW);
typedef void* __attribute__((thiscall)) (*Fn_PlanCommitCtor)(void* ev, int houseIdx, int evtType);
/* UI 关闭函数 0x639740: fastcall ECX=单位, EDX=节点 (本地闭环,
 * 不发事件不碰全局模式, 与手点关闭同函数)。调用前必须保证 token 非空
 * 且节点在其 token 内 (取 token[0] 即满足)。 */
typedef void __attribute__((fastcall)) (*Fn_PlanClose)(void* unit, void* node);
typedef int __attribute__((thiscall)) (*Fn_ClickCoords)(void* disp, Point2D* src, CellStruct* xy,
    CoordStruct* xyz, void** tgt, unsigned char* a5, unsigned char* b6);
typedef int __attribute__((thiscall)) (*Fn_WhatAmI)(void* self);
typedef void (__attribute__((cdecl)) *Fn_SndCb)(void);

#define FN_TGT_CELL   ((Fn_TargetFromCell)(void*)0x6E6B20u)
#define FN_TGT_OBJ    ((Fn_TargetFromObj)(void*)0x6E6AB0u)
#define FN_MEGA       ((Fn_MegaMissionCtor)(void*)0x4C6860u)
#define FN_PLANCONNECT ((Fn_PlanConnectCtor)(void*)0x4C6780u)
#define FN_PLANCOMMIT ((Fn_PlanCommitCtor)(void*)0x4C66C0u)
#define FN_CLICKCOORDS ((Fn_ClickCoords)(void*)0x692300u)
#define FN_PLANCLOSE  ((Fn_PlanClose)(void*)0x639740u)

/* ---------------- 配置 ---------------- */

static int g_PcInit = 0;
static int g_PcEnable = 1;
static int g_PcTriggerVK = 4;   /* 4=中键, 5=侧键1, 6=侧键2 */
static int g_PcRequireZ = 1;
static int g_PcZVK = 90;        /* Z */
static int g_PcSize = 4;        /* 方环边长 (偶数, 角=中心±Size/2) */
static int g_PcCommit = 1;

static void PC_EnsureInit(void)
{
    if (g_PcInit) return;
    g_PcInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_PcEnable = GetPrivateProfileIntA(PC_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    g_PcTriggerVK = GetPrivateProfileIntA(PC_SECTION, "TriggerVK", 4, g_IniPath);
    g_PcRequireZ = GetPrivateProfileIntA(PC_SECTION, "RequireZ", 1, g_IniPath) ? 1 : 0;
    g_PcZVK = GetPrivateProfileIntA(PC_SECTION, "ZVK", 90, g_IniPath);
    g_PcSize = GetPrivateProfileIntA(PC_SECTION, "Size", 4, g_IniPath);
    g_PcCommit = GetPrivateProfileIntA(PC_SECTION, "Commit", 1, g_IniPath) ? 1 : 0;
    if (g_PcTriggerVK < 1 || g_PcTriggerVK > 255) g_PcTriggerVK = 4;
    if (g_PcZVK < 1 || g_PcZVK > 255) g_PcZVK = 90;
    if (g_PcSize < 2) g_PcSize = 2;
    if (g_PcSize > 16) g_PcSize = 16;
    LogLine(g_PcEnable ? "pcircle: event patrol circle ON"
                       : "pcircle: event patrol circle OFF");
}

/* ---------------- 小工具 ---------------- */

static int PC_WhatAmI(void* p)
{
    void** vt;
    Fn_WhatAmI fn;
    if (!p || !IsPlausiblePtr(p)) return -1;
    vt = *(void***)p;
    if (!vt || !IsPlausiblePtr(vt)) return -1;
    fn = (Fn_WhatAmI)vt[0x2C / 4];
    if (!fn) return -1;
    return fn(p);
}

static int PC_IsFoot(void* p)
{
    int w = PC_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_AIRCRAFT || w == ABS_INFANTRY) ? 1 : 0;
}

static void PC_Trace(const char* head, int a, int b, int c)
{
    char buf[96], b1[16], b2[16], b3[16];
    unsigned q = 0;
    q = AppendCStr(buf, q, head, sizeof(buf));
    HtoA((unsigned)a, b1);
    q = AppendCStr(buf, q, " a=", sizeof(buf));
    q = AppendCStr(buf, q, b1, sizeof(buf));
    HtoA((unsigned)b, b2);
    q = AppendCStr(buf, q, " b=", sizeof(buf));
    q = AppendCStr(buf, q, b2, sizeof(buf));
    HtoA((unsigned)c, b3);
    q = AppendCStr(buf, q, " c=", sizeof(buf));
    q = AppendCStr(buf, q, b3, sizeof(buf));
    (void)q;
    LogLine(buf);
}

/* OutList 手工入队 (与 0x6521C0 同义, BuildQueue 同式)。满返回 0。 */
static int PC_Enqueue(char ev[EVT_SIZE])
{
    unsigned count = *(unsigned*)ADDR_OUT_COUNT;
    unsigned next;
    unsigned i;
    Fn_SndCb snd;
    unsigned eaxAfter;
    if (count >= EVT_CAP) return 0;
    next = *(unsigned*)ADDR_OUT_NEXT;
    for (i = 0; i < EVT_SIZE; i++)
        ((char*)(ADDR_OUT_BUF + next * EVT_SIZE))[i] = ev[i];
    snd = *(Fn_SndCb*)0x7E1530u;
    __asm__ volatile (
        "call *%1\n\t"
        "movl %%eax, %0"
        : "=r" (eaxAfter)
        : "r" (snd)
        : "ecx", "edx", "cc", "memory");
    *(unsigned*)(ADDR_OUT_PAR + next * 4u) = eaxAfter;
    *(unsigned*)ADDR_OUT_NEXT = (next + 1u) & 0x7Fu;
    *(unsigned*)ADDR_OUT_COUNT = count + 1u;
    return 1;
}

static int PC_FireMega(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstID, unsigned char dstRtti)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_MOVE,
            0, 0, dstID, (unsigned)dstRtti, 0, 0);
    ev[OFF_PLANFLAG] = 1;
    return PC_Enqueue(ev);
}

static int PC_FireCommit(int houseIdx)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_PLANCOMMIT(ev, houseIdx, EVT_PLANCOMMIT);
    return PC_Enqueue(ev);
}

/* PlanConnect (单位挂已有节点, 不建新节点): src=单位 Whom, dst=角格。 */
static int PC_FireConnect(int houseIdx, int srcID, unsigned char srcRtti,
                          int dstID, unsigned char dstRtti)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_PLANCONNECT(ev, houseIdx, EVT_PLANCONNECT,
                   srcID, (unsigned)srcRtti, dstID, (unsigned)dstRtti);
    return PC_Enqueue(ev);
}

/* 角格合法性: idx=(Y<<9)+X, 0<=idx<MaxCells 且 Cells[idx] 非空。 */
static int PC_CellValid(int cx, int cy)
{
    void** cells;
    int maxc, idx;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return 0;
    cells = *(void***)(ADDR_DISPLAY + OFF_MAP_CELLS);
    maxc = *(int*)(ADDR_DISPLAY + OFF_MAP_MAXC);
    if (!cells || !IsPlausiblePtr(cells) || maxc <= 0 || maxc > 512 * 512) return 0;
    idx = (cy << 9) + cx;
    if (idx < 0 || idx >= maxc) return 0;
    return cells[idx] ? 1 : 0;
}

/* ---------------- 待发队列 (保序 drip) ---------------- */

#define PC_CLOSE_CAP 256   /* 快照上限 (不限单位, 原生逻辑兜底; 静态数组取 256) */
#define PC_PEND_CAP 2048   /* 4+4+4*256, 静态 */
#define PC_DRAIN_PER_POLL 128
/* 触发即突发, OutList (上限 0x80) 满则 drip 跨帧续发。Mega 恒首单位 4 个;
 * Connect 按格找节点, 跨帧不影响挂接。 */

#define ADDR_FRAME 0xA8ED84u       /* 当前帧 (事件 Frame 戳源; 排空后看它推进) */
#define ADDR_IDBASE 0xB0E840u      /* Fetch_ID 索引基址 (纯读存活校验) */
#define ADDR_IDCOUNT 0xB0E844u     /* Fetch_ID 索引计数 */
#define PC_WAIT_FRAMES 30          /* 真失败兜底: 30 逻辑帧还不满才关已有 */
#define PC_WAIT_POLLCAP 3000       /* 冻结防挂: 帧不动时 polls 到此放行 */
#define PC_CLOSEC_FRAMES 6         /* close 落地上限 (之后直接关兜底) */

typedef struct { void* unit; short x, y; char conn; int id; } PCPend;

static PCPend g_PcPend[PC_PEND_CAP];
static int g_PcPendCount = 0;
static int g_PcCommitPend = 0;
static void* g_PcHouse = 0;
static int g_PcHouseIdx = 0;
static int g_PcPrevTrig = 0;
static int g_PcWaitExec = 0;      /* 排空后等 token 落地再闭环 */
static int g_PcWaitPolls = 0;     /* 冻结防挂计数 (帧不动时累积) */
static int g_PcWaitFrames = 0;    /* 已推进逻辑帧数 (主标尺) */
static int g_PcWaitLogFr = 0;     /* wait 进度日志节流 */
static int g_PcPhase = 0;         /* 1=等首单位挂满 2=等全满 3=等合环落地 */
static int g_PcIdxOk = 0;         /* ID 索引机制自检通过 (存活校验总开关) */
static int g_PcCorX[4], g_PcCorY[4]; /* 角格 */
static void* g_PcCloseUnits[PC_CLOSE_CAP]; /* 闭环用单位快照 */
static int g_PcCloseIDs[PC_CLOSE_CAP];     /* 快照 Fetch_ID (存活校验) */
static int g_PcCloseCount = 0;

static void PC_ClearPend(void)
{
    g_PcPendCount = 0;
    g_PcCommitPend = 0;
    g_PcWaitExec = 0;
    g_PcCloseCount = 0;
    g_PcPhase = 0;
}

static void PC_ShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_PcPendCount; k++)
    {
        g_PcPend[k].unit = g_PcPend[k + 1].unit;
        g_PcPend[k].x = g_PcPend[k + 1].x;
        g_PcPend[k].y = g_PcPend[k + 1].y;
        g_PcPend[k].conn = g_PcPend[k + 1].conn;
        g_PcPend[k].id = g_PcPend[k + 1].id;
    }
    g_PcPendCount--;
}

/* 单位存活校验 (纯读, 永不碰对象本身): Fetch_ID 在引擎索引
 * (0xB0E840 基址/0xB0E844 计数, 8 字节 {ID,ptr} 升序) 里二分,
 * 索引指针==快照指针才算活。快照跨帧, 单位战死即悬空, 先验活再碰
 * 对象; g_PcIdxOk=0 (自检未通过) 时 fail-open 全回活 (功能不变)。 */
static int PC_PtrAliveRaw(void* u, int id)
{
    void* base;
    int cnt, lo, hi;
    if (!u || id <= 0) return 0;
    base = *(void**)ADDR_IDBASE;
    cnt = *(int*)ADDR_IDCOUNT;
    if (!base || !IsPlausiblePtr(base) || cnt <= 0 || cnt > 200000) return 0;
    lo = 0; hi = cnt;
    while (lo < hi)
    {
        int mid = lo + ((hi - lo) >> 1);
        char* e = (char*)base + (unsigned)mid * 8u;
        int eid = *(int*)(e + 0);
        if (eid < id) lo = mid + 1;
        else if (eid > id) hi = mid;
        else return (*(void**)(e + 4) == u) ? 1 : 0;
    }
    return 0;
}

static int PC_PtrAlive(void* u, int id)
{
    if (!g_PcIdxOk) return 1;
    return PC_PtrAliveRaw(u, id);
}

/* 快照位存活 (先验活再 IsFoot/解引用, 全链路统一入口)。 */
static int PC_AliveAt(int i)
{
    void* u;
    if (i < 0 || i >= g_PcCloseCount) return 0;
    u = g_PcCloseUnits[i];
    return PC_PtrAlive(u, g_PcCloseIDs[i]);
}

/* token 是否真实可用: 野指针/旧空 token 都会误报就绪, 必须深查
 * (数组有效 + 节点数合法 + 首节点有效), 否则门控抢跑:
 * ready 瞬间满足 → close 在 mega 执行前跑完 → skip → noloop)。
 * 入参为快照下标 (内部先验活, 战死单位一律不就绪)。 */
static int PC_TokenReadyAt(int i)
{
    void* u;
    void* t;
    void** arr;
    int n;
    void* n0;
    if (!PC_AliveAt(i)) return 0;
    u = g_PcCloseUnits[i];
    if (!PC_IsFoot(u)) return 0;
    t = *(void**)((char*)u + OFF_TECH_TOKEN);
    if (!t || !IsPlausiblePtr(t)) return 0;
    arr = *(void***)((char*)t + OFF_TOK_NODES);
    n = *(int*)((char*)t + OFF_TOK_NCOUNT);
    if (!arr || !IsPlausiblePtr(arr) || n < 1 || n > 1000) return 0;
    n0 = arr[0];
    if (!n0 || !IsPlausiblePtr(n0)) return 0;
    return 1;
}

/* 快照内存活单位数 / 真实可用 token 数 (tokens 日志用)。 */
static void PC_TokenStats(int* live, int* ready)
{
    int i, l = 0, r = 0;
    for (i = 0; i < g_PcCloseCount; i++)
    {
        if (!PC_AliveAt(i)) continue;
        if (!PC_IsFoot(g_PcCloseUnits[i])) continue;
        l++;
        if (PC_TokenReadyAt(i)) r++;
    }
    if (live) *live = l;
    if (ready) *ready = r;
}

/* token 节点数 (full 判定: 4 角齐才算挂完, 防执行中途 n=1~3 抢跑)。 */
static int PC_TokenNodeCountAt(int i)
{
    void* u;
    void* t;
    int n;
    if (!PC_TokenReadyAt(i)) return 0;
    u = g_PcCloseUnits[i];
    t = *(void**)((char*)u + OFF_TECH_TOKEN);
    n = *(int*)((char*)t + OFF_TOK_NCOUNT);
    if (n < 0 || n > 1000) return 0;
    return n;
}

static int PC_AllFull(void)
{
    int i;
    for (i = 0; i < g_PcCloseCount; i++)
    {
        if (!PC_AliveAt(i)) continue;
        if (!PC_IsFoot(g_PcCloseUnits[i])) continue;
        if (PC_TokenNodeCountAt(i) < 4) return 0;
    }
    return 1;
}

/* 首单位挂满 (阶段门控): 节点已建好, 可以发 Connect 了。 */
static int PC_LeaderFull(void)
{
    if (g_PcCloseCount <= 0 || !PC_AliveAt(0)) return 0;
    if (!PC_IsFoot(g_PcCloseUnits[0])) return 0;
    return PC_TokenNodeCountAt(0) >= 4 ? 1 : 0;
}

/* 建待发。分阶段: MegaPass (首单位 4 角, 建节点) → ConnPass (其余同角
 * Connect, 挂已有节点) → ClosePass (全员 Mega(吸收)+全员 Connect 合环,
 * 节点早已存在, 同突发无 race; 联机同步闭环状态)。
 * 建节点与挂接分开发 (同突发混发时 Connect 可能先于建节点执行而被丢弃)。
 * conn: 0=Mega(需单位) 1=Connect(需单位)。 */
static void PC_BuildMegaPass(void)
{
    int k;
    g_PcPendCount = 0;
    if (g_PcCloseCount <= 0) return;
    for (k = 0; k < 4; k++)
    {
        if (g_PcPendCount >= PC_PEND_CAP) break;
        g_PcPend[g_PcPendCount].unit = g_PcCloseUnits[0];
        g_PcPend[g_PcPendCount].x = (short)g_PcCorX[k];
        g_PcPend[g_PcPendCount].y = (short)g_PcCorY[k];
        g_PcPend[g_PcPendCount].conn = 0;
        g_PcPend[g_PcPendCount].id = g_PcCloseIDs[0];
        g_PcPendCount++;
    }
}

static void PC_BuildConnPass(void)
{
    int k, u;
    g_PcPendCount = 0;
    for (k = 0; k < 4; k++)
    {
        for (u = 1; u < g_PcCloseCount; u++)
        {
            if (g_PcPendCount >= PC_PEND_CAP) break;
            g_PcPend[g_PcPendCount].unit = g_PcCloseUnits[u];
            g_PcPend[g_PcPendCount].x = (short)g_PcCorX[k];
            g_PcPend[g_PcPendCount].y = (short)g_PcCorY[k];
            g_PcPend[g_PcPendCount].conn = 1;
            g_PcPend[g_PcPendCount].id = g_PcCloseIDs[u];
            g_PcPendCount++;
        }
    }
}

/* 合环 (玩家式: 全员 Mega(首角, 建好即吸收无重复) 紧接全员 Connect,
 * 节点早已存在, 同突发无 race; 联机同步闭环状态)。 */
static void PC_BuildClosePass(void)
{
    int u;
    g_PcPendCount = 0;
    for (u = 0; u < g_PcCloseCount; u++)
    {
        if (g_PcPendCount >= PC_PEND_CAP) break;
        g_PcPend[g_PcPendCount].unit = g_PcCloseUnits[u];
        g_PcPend[g_PcPendCount].x = (short)g_PcCorX[0];
        g_PcPend[g_PcPendCount].y = (short)g_PcCorY[0];
        g_PcPend[g_PcPendCount].conn = 0;
        g_PcPend[g_PcPendCount].id = g_PcCloseIDs[u];
        g_PcPendCount++;
    }
    for (u = 0; u < g_PcCloseCount; u++)
    {
        if (g_PcPendCount >= PC_PEND_CAP) break;
        g_PcPend[g_PcPendCount].unit = g_PcCloseUnits[u];
        g_PcPend[g_PcPendCount].x = (short)g_PcCorX[0];
        g_PcPend[g_PcPendCount].y = (short)g_PcCorY[0];
        g_PcPend[g_PcPendCount].conn = 1;
        g_PcPend[g_PcPendCount].id = g_PcCloseIDs[u];
        g_PcPendCount++;
    }
}
/* 首单位 token 闭环计数 (>0=引擎已闭环, commit 前置条件)。
 * token 非真实可用时一律回 0 (防野指针误报成环)。首单位即下标 0。 */
static int PC_LoopCount(void)
{
    void* u;
    void* t;
    int n;
    if (g_PcCloseCount <= 0 || !PC_AliveAt(0)) return 0;
    u = g_PcCloseUnits[0];
    if (!PC_TokenReadyAt(0)) return 0;
    t = *(void**)((char*)u + OFF_TECH_TOKEN);
    n = *(int*)((char*)t + OFF_TOK_LOOPN);
    if (n < 0 || n > 1000) return 0;
    return n;
}

/* 排空: 发完记帧快照, 下一逻辑帧闭环。
 * 返回 1=全排空, 0=OutList 满 (下轮继续)。中途 house 变化则清空。 */
static int PC_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx;
    int sent = 0;
    if (!cur || cur != g_PcHouse) { PC_ClearPend(); return 1; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (houseIdx != g_PcHouseIdx) { PC_ClearPend(); return 1; }
    while (g_PcPendCount > 0 && sent < PC_DRAIN_PER_POLL)
    {
        PCPend* e = &g_PcPend[0];
        char srcB[8], dstB[8];
        CellStruct cs;
        int srcID, dstID;
        unsigned char srcR, dstR;
        unsigned i;
        for (i = 0; i < 8; i++) srcB[i] = dstB[i] = 0;
        cs.X = e->x; cs.Y = e->y;
        FN_TGT_CELL(dstB, &cs);
        dstID = *(int*)(dstB + 0); dstR = (unsigned char)dstB[4];
        /* 先验活 (纯读索引): 战死即丢弃, 不碰悬空指针 */
        if (!PC_PtrAlive(e->unit, e->id)) { PC_ShiftDown(); continue; }
        if (!PC_IsFoot(e->unit)) { PC_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { PC_ShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        if (e->conn == 1)
        {
            if (!PC_FireConnect(houseIdx, srcID, srcR, dstID, dstR))
                return 0; /* 满: 下轮继续 */
        }
        else if (!PC_FireMega(houseIdx, srcID, srcR, dstID, dstR))
            return 0; /* 满: 下轮继续 */
        PC_ShiftDown();
        sent++;
    }
    if (g_PcPendCount <= 0)
    {
        g_PcWaitExec = 1;
        g_PcWaitPolls = 0;
        g_PcWaitFrames = 0;
        g_PcWaitLogFr = *(int*)ADDR_FRAME;
        PC_Trace("pc: drained", houseIdx, sent, 0);
    }
    return (g_PcPendCount <= 0) ? 1 : 0;
}

/* 直接闭环: 对快照内每个 token 真实可用的单位, 取其 token[0]
 * 调 UI 关闭函数。返回 closed/skipped (d=cnt<<16|skip)。
 * 战死单位跳过 (不同情悬空指针)。 */
static int PC_CloseAll(void)
{
    int i, closed = 0, skipped = 0;
    for (i = 0; i < g_PcCloseCount; i++)
    {
        void* u;
        void* t;
        void** arr;
        void* n0;
        if (!PC_TokenReadyAt(i)) { skipped++; continue; }
        u = g_PcCloseUnits[i];
        t = *(void**)((char*)u + OFF_TECH_TOKEN);
        arr = *(void***)((char*)t + OFF_TOK_NODES);
        n0 = arr[0];
        FN_PLANCLOSE(u, n0);
        closed++;
    }
    return (closed << 16) | (skipped & 0xFFFF);
}

/* 中键划线锚定: 按下记起点格, 松开取终点格定圈。 */
static int g_PcArmed = 0;
static int g_PcAx = 0, g_PcAy = 0;

/* 鼠标屏坐标 → 格 (ProcessClickCoords + 地图校验)。 */
static int PC_MouseCell(int* cx, int* cy)
{
    int mx, my;
    Point2D pt;
    CellStruct center;
    CoordStruct crd;
    void* tgtObj = 0;
    unsigned char b5 = 0, b6 = 0;
    center.X = center.Y = 0;
    crd.X = crd.Y = crd.Z = 0;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    pt.X = mx; pt.Y = my;
    if (!FN_CLICKCOORDS((void*)ADDR_DISPLAY, &pt, &center, &crd,
                        &tgtObj, &b5, &b6))
        return 0;
    *cx = (int)center.X; *cy = (int)center.Y;
    return PC_CellValid(*cx, *cy);
}

static int PC_Rnd(double v)
{
    return (int)(v + (v >= 0 ? 0.5 : -0.5));
}

/* 角计算: 起终 Chebyshev 距<2 视为点按 → 以终点为中心方形 (返回 1);
 * 否则起终连线为中线, 侧向外扩 half, 端点沿方向各延伸 half, 得矩环
 * 4 角绕序 (返回 2)。角出界返回 0 (已记 corner off map)。 */
static int PC_BuildCorners(int ax, int ay, int bx, int by, int half,
                           int corX[4], int corY[4])
{
    int dx = bx - ax, dy = by - ay;
    int adx = dx >= 0 ? dx : -dx, ady = dy >= 0 ? dy : -dy;
    int i;
    if (adx < 2 && ady < 2)
    {
        corX[0] = bx - half; corY[0] = by - half;
        corX[1] = bx + half; corY[1] = by - half;
        corX[2] = bx + half; corY[2] = by + half;
        corX[3] = bx - half; corY[3] = by + half;
        for (i = 0; i < 4; i++)
        {
            if (!PC_CellValid(corX[i], corY[i]))
            {
                PC_Trace("pc: corner off map", corX[i], corY[i], i);
                return 0;
            }
        }
        return 1;
    }
    {
        double L = sqrt((double)dx * dx + (double)dy * dy);
        double ux = dx / L, uy = dy / L;   /* 中线单位方向 */
        double ox = -uy * half, oy = ux * half; /* 侧向外扩 */
        double ex = ux * half, ey = uy * half;  /* 端点延伸 */
        double Ax = ax - ex, Ay = ay - ey;
        double Bx = bx + ex, By = by + ey;
        corX[0] = PC_Rnd(Ax + ox); corY[0] = PC_Rnd(Ay + oy);
        corX[1] = PC_Rnd(Bx + ox); corY[1] = PC_Rnd(By + oy);
        corX[2] = PC_Rnd(Bx - ox); corY[2] = PC_Rnd(By - oy);
        corX[3] = PC_Rnd(Ax - ox); corY[3] = PC_Rnd(Ay - oy);
        for (i = 0; i < 4; i++)
        {
            if (!PC_CellValid(corX[i], corY[i]))
            {
                PC_Trace("pc: corner off map", corX[i], corY[i], i);
                return 0;
            }
        }
        return 2;
    }
}

/* ---------------- 触发 ---------------- */

static void PC_TriggerAt(int ax, int ay, int bx, int by)
{
    void* cur;
    void** items;
    int selN, i, nu = 0;
    int half, cx, cy, mode;
    int corX[4], corY[4];
    int houseIdx;
    static void* s_units[PC_CLOSE_CAP];

    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items))
    { PC_Trace("pc: no selection", 0, 0, 0); return; }
    for (i = 0; i < selN && nu < PC_CLOSE_CAP; i++)
    {
        void* o = items[i];
        if (PC_IsFoot(o)) s_units[nu++] = o;
    }
    if (nu <= 0) { PC_Trace("pc: no foot selected", selN, 0, 0); return; }

    /* 快照当前选择 (单位指针 + Fetch_ID, 跨帧先验活) */
    g_PcCloseCount = 0;
    for (i = 0; i < nu && i < PC_CLOSE_CAP; i++)
    {
        char tb[8];
        unsigned ti;
        g_PcCloseUnits[i] = s_units[i];
        /* 快照 Fetch_ID (存活校验用; 选中单位必活, 此处 vcall 安全) */
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        g_PcCloseIDs[i] = FN_TGT_OBJ(tb, s_units[i]) ? *(int*)(tb + 0) : 0;
    }
    g_PcCloseCount = (nu < PC_CLOSE_CAP) ? nu : PC_CLOSE_CAP;
    /* ID 索引自检: 已知存活的首单位必须能查到, 否则本轮跳过存活校验
     * (fail-open, 功能不变, 记 noidx)。 */
    g_PcIdxOk = 0;
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        if (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 200000 &&
            g_PcCloseIDs[0] > 0 &&
            PC_PtrAliveRaw(g_PcCloseUnits[0], g_PcCloseIDs[0]))
            g_PcIdxOk = 1;
        else
            PC_Trace("pc: noidx", g_PcCloseIDs[0], cnt, 0);
    }

    half = g_PcSize / 2;
    if (half < 1) half = 1;
    mode = PC_BuildCorners(ax, ay, bx, by, half, corX, corY);
    if (!mode) return; /* 角出界已记日志 */
    if (mode == 2)
    {
        PC_Trace("pc: drag", ax, ay, bx);
        cx = (ax + bx) / 2; cy = (ay + by) / 2;
    }
    else { cx = bx; cy = by; }

    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    g_PcHouse = cur;
    g_PcHouseIdx = houseIdx;
    g_PcPendCount = 0;
    g_PcCommitPend = 0;
    g_PcWaitExec = 0;
    for (i = 0; i < 4; i++) { g_PcCorX[i] = corX[i]; g_PcCorY[i] = corY[i]; }
    /* 快照全员直建 (无 detach/删/拦): 新鲜成环; 闭环加新原生吸收提示;
     * 未闭环残留按原生语义延长。闭环不用第 5 Mega (帧戳对不上会建重复),
     * 改玩家式合环 (Mega+Connect) + 直接调关闭函数。 */
    g_PcPhase = 1;
    PC_BuildMegaPass();
    g_PcCommitPend = 1;
    if (mode == 2)
        PC_Trace("pc: fire", g_PcCloseCount, bx, by);
    else
        PC_Trace("pc: fire", g_PcCloseCount, cx, cy);
    PC_Drain();
}

/* 闭环+验证+提交 (一次做完, 同 Poll 内同步完成; 完即放行, FIFO 保序,
 * 无需等 commit 执行)。 */
static void PC_CloseCommit(void)
{
    int cr;
    int loop;
    void* cur;
    int hi;
    cr = PC_CloseAll();
    PC_Trace("pc: closed", (cr >> 16) & 0xFFFF, cr & 0xFFFF, 0);
    loop = PC_LoopCount();
    PC_Trace("pc: loop", loop, 0, 0);
    cur = *(void**)ADDR_CURPLAYER;
    hi = (cur && IsPlausiblePtr(cur)) ?
        *(int*)((char*)cur + OFF_HOUSE_INDEX) : g_PcHouseIdx;
    if (g_PcCommitPend && g_PcCommit && loop > 0)
    {
        if (PC_FireCommit(hi)) PC_Trace("pc: commit", hi, loop, 0);
        else PC_Trace("pc: commit full", hi, 0, 0);
    }
    else if (loop <= 0)
        PC_Trace("pc: noloop", 0, 0, 0);
    g_PcCommitPend = 0;
    PC_ClearPend();
}

/* ---------------- 每循环轮询 (挂 PowerDrawBoth) ---------------- */

void PatrolCircle_Poll(void)
{
    int trigDown;

    PC_EnsureInit();
    trigDown = (GetAsyncKeyState(g_PcTriggerVK) & 0x8000) ? 1 : 0;
    if (g_PcPendCount > 0)
        PC_Drain(); /* drip 续发 */
    else if (g_PcWaitExec)
    {
        /* 三阶段就绪门控 (主标尺=逻辑帧):
         * 阶段 1: 首单位挂满 → 发跟随者 Connect (单选则直接闭环)。
         * 阶段 2: 全员挂满 → 发合环 (全员 Mega+Connect, 多选;
         * 单选直接闭环)。
         * 阶段 3: 首单位 loop 落地 (合环执行到) → 直接关+提交。
         * 闭环加新点由原生拦截 (Mega 执行侧消息+丢弃)。
         * 帧每推进记一行 wait; 各阶段超限 → 兜底 (关已有/跳过);
         * 帧长期不动 → polls 放行防挂。 */
        int live = 0, ready = 0;
        int fr;
        int cap;
        PC_TokenStats(&live, &ready);
        g_PcWaitPolls++;
        fr = *(int*)ADDR_FRAME;
        cap = (g_PcWaitFrames > PC_WAIT_FRAMES || g_PcWaitPolls > PC_WAIT_POLLCAP) ? 1 : 0;
        if (!live)
        {
            PC_Trace("pc: wait abort", 0, 0, 0);
            PC_ClearPend();
        }
        else if (g_PcPhase == 1 && PC_LeaderFull())
        {
            if (g_PcCloseCount <= 1)
            {
                PC_Trace("pc: tokens", ready, live, 0);
                PC_CloseCommit();
            }
            else
            {
                PC_Trace("pc: conn", ready, live, 0);
                g_PcPhase = 2;
                g_PcWaitFrames = 0;
                g_PcWaitPolls = 0;
                PC_BuildConnPass();
                PC_Drain();
            }
        }
        else if (g_PcPhase == 2 && PC_AllFull())
        {
            if (g_PcCloseCount <= 1)
            {
                PC_Trace("pc: tokens", ready, live, 0);
                PC_CloseCommit();
            }
            else
            {
                PC_Trace("pc: cconn", ready, live, 0);
                g_PcPhase = 3;
                g_PcWaitFrames = 0;
                g_PcWaitPolls = 0;
                PC_BuildClosePass();
                PC_Drain();
            }
        }
        else if (g_PcPhase >= 3 && PC_LoopCount() > 0)
        {
            PC_Trace("pc: tokens", ready, live, 0);
            PC_CloseCommit();
        }
        else
        {
            if (fr != g_PcWaitLogFr)
            {
                g_PcWaitLogFr = fr;
                g_PcWaitFrames++;
                PC_Trace((g_PcPhase >= 3) ? "pc: cwait" : "pc: wait",
                         ready, live, 0);
            }
            if ((g_PcPhase >= 3 && g_PcWaitFrames > PC_CLOSEC_FRAMES) || cap)
            {
                PC_Trace("pc: cap", ready, live, 0);
                if (ready > 0)
                    PC_CloseCommit(); /* 有几个算几个 */
                else
                    /* 全空: 直接放行。 */
                    PC_ClearPend();
            }
        }
    }
    if (!g_PcEnable) { g_PcArmed = 0; g_PcPrevTrig = trigDown; return; }
    if (g_PcPendCount > 0 || g_PcWaitExec || g_PcCommitPend)
    { g_PcPrevTrig = trigDown; return; }
    if (trigDown && !g_PcPrevTrig)
    {
        /* 按下: 锚定起点格 (需 Z), 等松开再定圈 */
        if (!g_PcRequireZ || (GetAsyncKeyState(g_PcZVK) & 0x8000))
        {
            int ax, ay;
            if (PC_MouseCell(&ax, &ay))
            {
                g_PcArmed = 1; g_PcAx = ax; g_PcAy = ay;
                PC_Trace("pc: anchor", ax, ay, 0);
            }
            else PC_Trace("pc: off map", 0, 0, 0);
        }
        else
            PC_Trace("pc: need Z", 0, 0, 0);
    }
    else if (!trigDown && g_PcPrevTrig && g_PcArmed)
    {
        /* 松开: 终点=当前格; 起终划线外扩建圈 (松开在图外则按点按处理) */
        int bx, by;
        int ax = g_PcAx, ay = g_PcAy;
        g_PcArmed = 0;
        if (PC_MouseCell(&bx, &by))
            PC_TriggerAt(ax, ay, bx, by);
        else
            PC_TriggerAt(ax, ay, ax, ay);
    }
    g_PcPrevTrig = trigDown;
}
