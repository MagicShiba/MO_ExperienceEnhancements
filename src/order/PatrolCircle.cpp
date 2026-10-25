/* PatrolCircle.cpp - Z+中键一键巡逻圈 (选中单位直接巡逻)。
 *
 * 触发: 按住 Z (PlanningMode 键) 的同时按住鼠标中键 (Hotkey, 默认 Z+Middle),
 * 划线后松开在终点定圈: 起终连线为中线, 四周各外扩 Size/2 (端点沿方向也
 * 外扩) 成矩环; 点按 (起终两格内) 则以松开格为中心布 4x4 方环。拖线期间按
 * NodeStep 采样轨迹节点; 松开时起终距离 < LoopClose (且节点>=4) 则走曲线闭环:
 * 节点按 LoopStep 稀疏抽路点, 全员 Mega 建点 + 合环 Connect + Commit。
 * 配置全在 MOExperience.ini [PatrolCircle]。全程不碰光标, 不限单位。
 * 与砍树互斥: Ctrl 按住时让路 TreeChop (见 TreeChop.cpp)。拖动期间渲染期画
 * 白橡皮线 (锚点→当前鼠标, 与 SmartOrder 同风格), 挂
 * MOExperience_MessageDrawHook (见 MOExperience.cpp)。中键按住拖时游戏可能
 * 平移镜头, 起终格按各自时刻镜头映射。
 *
 * 原生序列 (OutList 抓包 pev + YRpp EventClass.h 交叉确认):
 *   每点每单位各一 MegaMission (Type 4, ctor 0x4C6860, Whom=单位,
 *   Mission=Move, Target=空, Destination=格子, Follow=格子(同 Dst),
 *   IsPlanningEvent=1), 执行侧 0x638120 建节点 + 挂各单位独立 token。
 *   关环 = 全员 Connect 首节点 (Type 0x2A, Connect-only 无 Mega),
 *   最后 PlanCommit (0x2B, 空载荷)。事件布局 (TargetClass 5 字节:
 *   ID4+RTTI1): Whom@7, Mission@12, Target@14, Dst@19, Follow@24, Flag@29。
 *   TargetCell 0x6E6B20 (ID=X+1000*Y, RTTI=11); TargetUnit 0x6E6AB0
 *   (ID=Fetch_ID, RTTI=Abstract=52)。入队走 OutList, 满则跨帧续发。
 *   选中表 0xA8ECBC/0xA8ECC8; CurrentPlayer 0xA83D4C+[0x30];
 *   中心格经 Display::ProcessClickCoords 0x692300; 角校验经
 *   MapCells=[0x87F7E8+0x13C]/MaxCells=[0x87F7E8+0x140]。
 *
 * 约束 (勿踩):
 *   只发 Connect 无节点可链 (全丢弃) —— 故先全员 Mega 建点, 挂满才发合环
 *   Connect (同突发混发时 Connect 可能先于建节点执行而被丢弃);
 *   关环绝不能带 Mega (已存在点再 Mega 建重复), 也不能本地直调 0x639740
 *   (只改本机, 联机分叉); Follow 必须 =Dst (空则关环语义缺失)。
 *   各阶段就绪即走无固定等待 (帧不动时 poll 数兜底防 busy 卡死);
 *   token 就绪须深度校验 (数组+计数+首节点, 防门控抢跑);
 *   快照跨帧须先经 Fetch_ID 索引验活 (自检失败则 fail-open);
 *   不删路径不拦添加 —— 闭环加新点由原生拦截 (消息+丢弃), 未闭环残留按
 *   原生语义延长。
 *
 * 联机: 只发标准事件, 各端同一份原版代码执行, 不影响联机。
 * 本地只读 + 标准事件入队, 不写模拟内存。
 * 构建见 build-fast.bat (32 位 mingw, 无 CRT, 与 MOExperience 同一 DLL)。
 * 轮询挂 PowerDrawBoth (见 MOExperience.cpp), 开关关时直接返回。
 */

#include <windows.h>
#include "EngineAddr.h"
#include <math.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"
#include "SmartArea.h"

#define PC_SECTION "PatrolCircle"

/* ---------------- 巡逻圈私有地址/事件 (共享部分见 OrderUtil.h) ---------------- */

#define EVT_PLANCONNECT 0x2Au

#define OFF_TOK_LOOPN 0x90u   /* PlanningToken ClosedLoopNodeCount */

typedef void* __attribute__((thiscall)) (*Fn_TargetFromCell)(void* tgt, const CellStruct* cell);
/* MegaMission ctor (0x4C6860, ECX=ev111, 栈 0x28):
 * house, srcID, srcRTTI, mission, tgtID, tgtRtti, dstID, dstRtti, folID, folRtti */
/* PlanConnect ctor (0x4C6780, ECX=ev111, ret 0x18):
 * house, type(0x2A), srcID, srcRTTI(dword, 取低字节),
 * dstID, dstRTTI。布局: Type@0, House@2, Frame@3(取[0xA8ED84]),
 * Target1(单位)@7(ID@7+RTTI@11), Target2(格)@12(ID@12+RTTI@16)。 */
typedef void* __attribute__((thiscall)) (*Fn_PlanConnectCtor)(void* ev, int houseIdx, int evtType,
    int srcID, unsigned srcRttiW, int dstID, unsigned dstRttiW);
typedef void* __attribute__((thiscall)) (*Fn_PlanCommitCtor)(void* ev, int houseIdx, int evtType);
/* UI 关闭函数 0x639740 (fastcall ECX=单位, EDX=节点):
 * 故意不用 (只改本机模拟状态, 不广播, 联机必不同步)。
 * 关环一律走合环 Connect 事件 (原生对照, pev 实证)。 */
#define FN_TGT_CELL   ((Fn_TargetFromCell)(void*)0x6E6B20u)
#define FN_PLANCONNECT ((Fn_PlanConnectCtor)(void*)0x4C6780u)
#define FN_PLANCOMMIT ((Fn_PlanCommitCtor)(void*)0x4C66C0u)

/* ---------------- 配置 ---------------- */

static int g_PcInit = 0;
static int g_PcEnable = 1;
static OU_Hotkey g_PcHk;      /* Hotkey=Z+Middle (主键中键 + 必须按住 Z) */
static int g_PcSize = 4;        /* 方环边长 (偶数, 角=中心±Size/2) */
static int g_PcCommit = 1;
static int g_PcNodeStep = 2;    /* 拖线采样步长 (格, 切比雪夫距离) */
static int g_PcLoopClose = 3;   /* 起终切比雪夫距离 < 此值判闭环 (曲线巡逻) */
static int g_PcLoopStep = 3;    /* 闭环路点最小间距 (格, 比散开配额稀疏) */

static void PC_EnsureInit(void)
{
    if (g_PcInit) return;
    g_PcInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_PcEnable = GetPrivateProfileIntA(PC_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    OU_ParseHotkey("Z+Middle", &g_PcHk);          /* 默认 (必成功) */
    OU_ReadHotkey(PC_SECTION, "Hotkey", &g_PcHk); /* 有则覆盖, 坏串留默认 */
    if (g_PcHk.disabled) g_PcEnable = 0;
    g_PcSize = GetPrivateProfileIntA(PC_SECTION, "Size", 4, g_IniPath);
    g_PcCommit = GetPrivateProfileIntA(PC_SECTION, "Commit", 1, g_IniPath) ? 1 : 0;
    if (g_PcSize < 2) g_PcSize = 2;
    if (g_PcSize > 16) g_PcSize = 16;
    g_PcNodeStep = GetPrivateProfileIntA(PC_SECTION, "NodeStep", 2, g_IniPath);
    if (g_PcNodeStep < 1) g_PcNodeStep = 1;
    if (g_PcNodeStep > 8) g_PcNodeStep = 8;
    g_PcLoopClose = GetPrivateProfileIntA(PC_SECTION, "LoopClose", 3, g_IniPath);
    if (g_PcLoopClose < 2) g_PcLoopClose = 2;
    if (g_PcLoopClose > 16) g_PcLoopClose = 16;
    g_PcLoopStep = GetPrivateProfileIntA(PC_SECTION, "LoopStep", 3, g_IniPath);
    if (g_PcLoopStep < 2) g_PcLoopStep = 2;
    if (g_PcLoopStep > 16) g_PcLoopStep = 16;
    LogLine(g_PcEnable ? "pcircle: event patrol circle ON"
                       : "pcircle: event patrol circle OFF");
}

/* ---------------- 小工具 (通用部分见 OrderUtil.h) ---------------- */

static int PC_IsFoot(void* p)
{
    int w = OU_WhatAmI(p);
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

static int PC_FireMega(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstID, unsigned char dstRtti,
                       void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    OU_EvClear(ev);
    /* 原生对照 (pev 抓包): 每单位每节点各一 Mega, Follow=Dst 格
     * (Follow 空会导致关环语义缺失, 联机不同步的根因之一)。 */
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_MOVE,
            0, 0, dstID, (unsigned)dstRtti, dstID, (unsigned)dstRtti);
    ev[OFF_PLANFLAG] = 1;
    return OT_Fire(ev, unit, uid, idxOk);
}

static int PC_FireCommit(int houseIdx)
{
    return OU_FirePlanCommit(houseIdx);
}

/* PlanConnect (单位挂已有节点, 不建新节点): src=单位 Whom, dst=角格。 */
static int PC_FireConnect(int houseIdx, int srcID, unsigned char srcRtti,
                          int dstID, unsigned char dstRtti,
                          void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    OU_EvClear(ev);
    FN_PLANCONNECT(ev, houseIdx, EVT_PLANCONNECT,
                   srcID, (unsigned)srcRtti, dstID, (unsigned)dstRtti);
    return OT_Fire(ev, unit, uid, idxOk);
}

/* ---------------- 待发队列 (保序 drip) ---------------- */

#define PC_CLOSE_CAP 1024   /* 快照上限 (超限进 OT 队列分帧, 与 OT_QCAP 对齐) */
#define PC_WP_CAP 16       /* 单圈路点上限 (引擎 token 最多 16 节点, 矩环 4/曲线均分 16) */
#define PC_PEND_CAP 17408  /* Mega 16*1024 + 合环 1024, 静态 */
#define PC_DRAIN_PER_POLL 128
/* 触发即突发, OutList (上限 0x80) 满则 drip 跨帧续发; Mega 逐点全员,
 * Connect 合环, 跨帧不影响。 */

#define PC_WAIT_FRAMES 30          /* 真失败兜底: 30 逻辑帧还不满才关已有 */
#define PC_WAIT_POLLCAP 3000       /* 冻结防挂: 帧不动时 polls 到此放行 */
#define PC_CLOSEC_FRAMES 6         /* close 落地上限 (之后提交, 开环也提交) */

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
static int g_PcPhase = 0;         /* 1=等全员挂满 2=等合环落地 */
static int g_PcIdxOk = 0;         /* ID 索引机制自检通过 (存活校验总开关) */
static int g_PcCorX[PC_WP_CAP], g_PcCorY[PC_WP_CAP]; /* 路点格 (矩环 4/曲线 N) */
static int g_PcCorCount = 0;     /* 本圈路点数 */
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

/* 快照位存活 (先验活再 IsFoot/解引用, 全链路统一入口)。 */
static int PC_AliveAt(int i)
{
    void* u;
    if (i < 0 || i >= g_PcCloseCount) return 0;
    u = g_PcCloseUnits[i];
    return OU_PtrAlive(u, g_PcCloseIDs[i], g_PcIdxOk);
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

/* token 节点数 (full 判定: 路点齐才算挂完, 防执行中途抢跑)。 */
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
        if (PC_TokenNodeCountAt(i) < g_PcCorCount) return 0;
    }
    return 1;
}

/* 建待发。原生对照 (pev 抓包, 单机 3 单位手动圈):
 * 每点每单位各一 Mega (全员独立 token, 非共享节点),
 * 逐点全员 (与原生逐点击全员同序) → 关环 Connect-only
 * (全员连首节点, 无 Mega, 原生关环即此) → Commit。
 * 注意: 关环绝不能带 Mega (已存在点再 Mega 会建重复节点),
 * 更不能本地直调关环函数 (只改本机, 联机分叉)。 */
static void PC_BuildMegaPass(void)
{
    int k, u;
    g_PcPendCount = 0;
    if (g_PcCloseCount <= 0) return;
    for (k = 0; k < g_PcCorCount; k++)
    {
        for (u = 0; u < g_PcCloseCount; u++)
        {
            if (g_PcPendCount >= PC_PEND_CAP) break;
            g_PcPend[g_PcPendCount].unit = g_PcCloseUnits[u];
            g_PcPend[g_PcPendCount].x = (short)g_PcCorX[k];
            g_PcPend[g_PcPendCount].y = (short)g_PcCorY[k];
            g_PcPend[g_PcPendCount].conn = 0;
            g_PcPend[g_PcPendCount].id = g_PcCloseIDs[u];
            g_PcPendCount++;
        }
    }
}

/* 合环: 全员 Connect 首路点 (Connect-only, 无 Mega, 与原生关环一致)。
 * 各单位 token 早已独立建好 (全员 Mega), 连首节点即闭环, 联机同步。 */
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
        if (!OU_PtrAlive(e->unit, e->id, g_PcIdxOk)) { PC_ShiftDown(); continue; }
        if (!PC_IsFoot(e->unit)) { PC_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { PC_ShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        if (e->conn == 1)
        {
            if (!PC_FireConnect(houseIdx, srcID, srcR, dstID, dstR,
                                 e->unit, e->id, g_PcIdxOk))
                return 0; /* 满: 下轮继续 */
        }
        else if (!PC_FireMega(houseIdx, srcID, srcR, dstID, dstR,
                              e->unit, e->id, g_PcIdxOk))
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

/* 中键划线锚定: 按下记起点格 (+实测像素, 橡皮线用), 松开取终点格定圈。 */
static int g_PcArmed = 0;
static int g_PcAx = 0, g_PcAy = 0;
static int g_PcPx = 0, g_PcPy = 0;

/* ---------------- 拖线轨迹节点 (格+采样时刻像素) ---------------- */
#define PC_NODE_CAP 64
static int g_PcNx[PC_NODE_CAP], g_PcNy[PC_NODE_CAP];
static int g_PcNPx[PC_NODE_CAP], g_PcNPy[PC_NODE_CAP];
static int g_PcNodeCount = 0;

static int PC_Cheb(int ax, int ay, int bx, int by)
{
    int dx = ax >= bx ? ax - bx : bx - ax;
    int dy = ay >= by ? ay - by : by - ay;
    return dx >= dy ? dx : dy;
}

/* 采样: 与末节点切比雪夫距离 >= NodeStep 才记;
 * 满 64 后滑尾 (起点保留, 末端保持新鲜)。 */
static void PC_NodePush(int cx, int cy, int px, int py)
{
    if (g_PcNodeCount <= 0)
    {
        g_PcNx[0] = cx; g_PcNy[0] = cy;
        g_PcNPx[0] = px; g_PcNPy[0] = py;
        g_PcNodeCount = 1;
        return;
    }
    if (PC_Cheb(cx, cy, g_PcNx[g_PcNodeCount - 1],
                g_PcNy[g_PcNodeCount - 1]) < g_PcNodeStep)
        return;
    if (g_PcNodeCount >= PC_NODE_CAP)
    {
        g_PcNx[PC_NODE_CAP - 1] = cx; g_PcNy[PC_NODE_CAP - 1] = cy;
        g_PcNPx[PC_NODE_CAP - 1] = px; g_PcNPy[PC_NODE_CAP - 1] = py;
        return;
    }
    g_PcNx[g_PcNodeCount] = cx; g_PcNy[g_PcNodeCount] = cy;
    g_PcNPx[g_PcNodeCount] = px; g_PcNPy[g_PcNodeCount] = py;
    g_PcNodeCount++;
}

/* 按住期间每轮采样当前格 (出图则跳过, 下轮继续)。 */
static void PC_Sample(void)
{
    int cx, cy, mx, my;
    if (!OU_MouseCell(&cx, &cy)) return;
    if (!GtMouseGameXY(&mx, &my)) return;
    PC_NodePush(cx, cy, mx, my);
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
            if (!OU_CellValid(corX[i], corY[i]))
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
            if (!OU_CellValid(corX[i], corY[i]))
            {
                PC_Trace("pc: corner off map", corX[i], corY[i], i);
                return 0;
            }
        }
        return 2;
    }
}

/* 曲线闭环: 轨迹节点按 LoopStep 稀疏抽路点, 去尾 (与起点 < LoopClose
 * 的收尾点由合环走路点[0] 自然闭合); 超过 PC_WP_CAP (16) 则沿整条轨迹
 * 均匀均分取 16 (不能截断, 否则全挤在开头一段)。
 * 成功填 g_PcCor/g_PcCorCount 返回 3, 否则返回 0 (调用方退回矩形)。
 * 稀疏度远高于散开配额 (散开步兵 3/格, 这里路点至少 LoopStep 格)。 */
static int PC_BuildCurve(void)
{
    int px[PC_NODE_CAP], py[PC_NODE_CAP];
    int n = 0, i;
    if (g_PcNodeCount < 4) return 0;
    px[0] = g_PcNx[0]; py[0] = g_PcNy[0]; n = 1;
    for (i = 1; i < g_PcNodeCount; i++)
    {
        if (PC_Cheb(g_PcNx[i], g_PcNy[i], px[n - 1], py[n - 1]) >= g_PcLoopStep)
        { px[n] = g_PcNx[i]; py[n] = g_PcNy[i]; n++; }
    }
    while (n > 1 && PC_Cheb(px[n - 1], py[n - 1], px[0], py[0]) < g_PcLoopClose)
        n--;
    if (n < 3) return 0;
    if (n > PC_WP_CAP)
    {
        /* 均匀均分: 在 n 个抽点里等距取 PC_WP_CAP 个, 首尾覆盖全程。 */
        int qx[PC_WP_CAP], qy[PC_WP_CAP];
        for (i = 0; i < PC_WP_CAP; i++)
        {
            int idx = (i * n) / PC_WP_CAP;
            if (idx >= n) idx = n - 1;
            qx[i] = px[idx]; qy[i] = py[idx];
        }
        for (i = 0; i < PC_WP_CAP; i++) { px[i] = qx[i]; py[i] = qy[i]; }
        n = PC_WP_CAP;
    }
    for (i = 0; i < n; i++)
    {
        if (!OU_CellValid(px[i], py[i])) return 0; /* 有点出界退回矩形 */
        g_PcCorX[i] = px[i]; g_PcCorY[i] = py[i];
    }
    g_PcCorCount = n;
    return 3;
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
            OU_PtrAliveRaw(g_PcCloseUnits[0], g_PcCloseIDs[0]))
            g_PcIdxOk = 1;
        else
            PC_Trace("pc: noidx", g_PcCloseIDs[0], cnt, 0);
    }

    half = g_PcSize / 2;
    if (half < 1) half = 1;
    /* 近闭环优先: 节点>=4 且起终 < LoopClose → 曲线巡逻圈;
     * 否则原行为 (矩环/点按方环)。 */
    mode = 0;
    if (g_PcNodeCount >= 4 && PC_Cheb(ax, ay, bx, by) < g_PcLoopClose)
        mode = PC_BuildCurve();
    if (!mode)
        mode = PC_BuildCorners(ax, ay, bx, by, half, corX, corY);
    if (!mode) return; /* 角出界已记日志 */
    if (mode == 3)
    {
        PC_Trace("pc: curve", g_PcCorCount, nu, 0);
        cx = g_PcCorX[0]; cy = g_PcCorY[0];
    }
    else if (mode == 2)
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
    if (mode != 3)
    {
        for (i = 0; i < 4; i++) { g_PcCorX[i] = corX[i]; g_PcCorY[i] = corY[i]; }
        g_PcCorCount = 4;
    }
    /* 快照全员逐点 Mega (无 detach/删/拦, 与原生逐点击全员同构):
     * 新鲜成环; 闭环加新原生吸收提示; 未闭环残留按原生语义延长。
     * 合环 Connect-only (无 Mega, 防重复节点), 纯事件关环。 */
    g_PcPhase = 1;
    PC_BuildMegaPass();
    g_PcCommitPend = 1;
    if (mode == 2)
        PC_Trace("pc: fire", g_PcCloseCount, bx, by);
    else
        PC_Trace("pc: fire", g_PcCloseCount, cx, cy);
    PC_Drain();
}

/* 验证+提交 (同 Poll 内一次做完; 完即放行, FIFO 保序, 无需等 commit 执行)。
 * 纯事件关环 (合环 Connect 已执行), loop>0 才 Commit; 关不上则提交开环
 * 路径 (两端一致, 只降级不崩)。绝不能本地直调关环函数 0x639740: 只改本机
 * 不广播, 联机两端 token 分叉不同步。 */
static void PC_CloseCommit(void)
{
    int loop;
    void* cur;
    int hi;
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
    trigDown = OU_KeyDown(g_PcHk.vk);
    if (g_PcPendCount > 0)
        PC_Drain(); /* drip 续发 */
    else if (g_PcWaitExec)
    {
        /* 二阶段就绪门控 (主标尺=逻辑帧):
         * 阶段 1: 全员路点挂满 → 发合环 (全员 Connect 首路点, Connect-only)。
         * 阶段 2: 首单位 loop 落地 → 提交。
         * 帧每推进记一行 wait; 各阶段超限 → 兜底提交 (开环也提交, 两端一致);
         * 帧长期不动 → polls 放行防挂。全程纯事件, 无本地直调, 联机安全。 */
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
        else if (g_PcPhase == 1 && PC_AllFull())
        {
            PC_Trace("pc: cconn", ready, live, 0);
            g_PcPhase = 2;
            g_PcWaitFrames = 0;
            g_PcWaitPolls = 0;
            PC_BuildClosePass();
            PC_Drain();
        }
        else if (g_PcPhase >= 2 && PC_LoopCount() > 0)
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
                PC_Trace((g_PcPhase >= 2) ? "pc: cwait" : "pc: wait",
                         ready, live, 0);
            }
            if ((g_PcPhase >= 2 && g_PcWaitFrames > PC_CLOSEC_FRAMES) || cap)
            {
                PC_Trace("pc: cap", ready, live, 0);
                PC_CloseCommit(); /* 超限提交 (有环是环, 无环是路, 两端一致) */
            }
        }
    }
    if (!g_PcEnable) { g_PcArmed = 0; g_PcPrevTrig = trigDown; return; }
    if (g_PcPendCount > 0 || g_PcWaitExec || g_PcCommitPend)
    { g_PcPrevTrig = trigDown; return; }
    if (trigDown && !g_PcPrevTrig)
    {
        /* 按下: 锚定起点格 (Hotkey 附加键如 Z 必须按住, 且不能按 Ctrl——
         * Ctrl+Z+中键归砍树; SmartArea 组合认领时也让路, Z 照常用),
         * 节点[0]=锚点, 等松开再定圈 */
        if (OU_ModsDown(g_PcHk.mods) && OU_ExtraDown(&g_PcHk) &&
            !OU_KeyDown(VK_CONTROL) && !SmartArea_ClaimsMiddle())
        {
            int ax, ay;
            if (OU_MouseCell(&ax, &ay))
            {
                int mx = 0, my = 0;
                g_PcArmed = 1; g_PcAx = ax; g_PcAy = ay;
                g_PcNodeCount = 0;
                if (GtMouseGameXY(&mx, &my))
                { g_PcPx = mx; g_PcPy = my; PC_NodePush(ax, ay, mx, my); }
                PC_Trace("pc: anchor", ax, ay, 0);
            }
            else PC_Trace("pc: off map", 0, 0, 0);
        }
        else
            PC_Trace("pc: need Z", 0, 0, 0);
    }
    else if (!trigDown && g_PcPrevTrig && g_PcArmed)
    {
        /* 松开: 终点=当前格; 近闭环走曲线, 否则起终划线外扩建圈
         * (松开在图外则按点按处理, 节点仍在可走曲线)。
         * 松开瞬间按住 Ctrl 即改主意砍树了: 巡逻取消, 执行权交 TreeChop。 */
        int bx, by;
        int ax = g_PcAx, ay = g_PcAy;
        int mx = 0, my = 0;
        g_PcArmed = 0;
        if (OU_KeyDown(VK_CONTROL))
        {
            g_PcPrevTrig = trigDown;
            return;
        }
        if (OU_MouseCell(&bx, &by))
        {
            if (GtMouseGameXY(&mx, &my)) PC_NodePush(bx, by, mx, my);
            PC_TriggerAt(ax, ay, bx, by);
        }
        else
            PC_TriggerAt(ax, ay, ax, ay);
    }
    if (g_PcArmed && trigDown) PC_Sample(); /* 按住期间采样轨迹 */
    g_PcPrevTrig = trigDown;
}

/* ---------------- 渲染期橡皮线 (3px 白线, 与 SmartOrder 同风格) ---------------- */

static unsigned char* s_PcLocked = 0;
static int s_PcW = 0, s_PcH = 0, s_PcPitch = 0;

/* 3px 白线 / 实心方块 (公共实现: OrderUtil 的 OU_Line / OU_Box)。 */
static void PC_Line(int x0, int y0, int x1, int y1, unsigned a)
{
    OU_Line(s_PcLocked, s_PcW, s_PcH, s_PcPitch, 0,
            x0, y0, x1, y1, 255, 255, 255, a, 3);
}

static void PC_Mark(int cx, int cy, int m, unsigned a)
{
    OU_Box(s_PcLocked, s_PcW, s_PcH, s_PcPitch, 0,
           cx, cy, m, 255, 255, 255, a);
}

void PatrolCircle_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* locked;
    int x0, y0, x1, y1;
    extern int g_Calibrated;
    if (!g_PcEnable || !g_PcArmed) return;
    if (OU_KeyDown(VK_CONTROL)) return; /* 按住 Ctrl 显示砍树框, 巡逻线让路 */
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    if (!GtMouseGameXY(&x1, &y1)) return;
    x0 = g_PcPx; y0 = g_PcPy;
    locked = OU_SurfLock(surf);
    if (!locked) return;
    s_PcLocked = locked; s_PcW = w; s_PcH = h; s_PcPitch = pitch;
    /* 节点折线 (采样时刻实测像素) + 橡皮尾巴 (末节点→当前鼠标)。
     * 与 SmartOrder 同 caveat: 中键拖动平移镜头, 节点像素跨镜头帧会漂;
     * 落点格按松开时刻重算, 不受影响。 */
    {
        int i, ex, ey;
        int nc = g_PcNodeCount;
        if (nc > PC_NODE_CAP) nc = PC_NODE_CAP;
        ex = (nc > 0) ? g_PcNPx[0] : x0;
        ey = (nc > 0) ? g_PcNPy[0] : y0;
        for (i = 0; i < nc; i++)
        {
            int nx = g_PcNPx[i], ny = g_PcNPy[i];
            if (i > 0) PC_Line(ex, ey, nx, ny, 230);
            PC_Mark(nx, ny, 1, 230);
            ex = nx; ey = ny;
        }
        PC_Line(ex, ey, x1, y1, 230);
        PC_Mark(x0, y0, 3, 255); /* 锚点 7x7 */
        PC_Mark(x1, y1, 2, 255); /* 当前 5x5 */
    }
    s_PcLocked = 0;
    OU_SurfUnlock(surf);
}
