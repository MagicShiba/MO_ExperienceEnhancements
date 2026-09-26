/* PatrolCircle.cpp - Z+中键一键巡逻圈 (选中单位直接巡逻)。
 *
 * 触发: 按住 Z (PlanningMode 键) 的同时点鼠标中键 (可配 TriggerVK:
 * 4=中键默认, 5=侧键1, 6=侧键2), 当前选中单位以点击格为中心布 4x4
 * 方环 (±2 角) 并开始巡逻。配置全在 MsgFont.ini [PatrolCircle]。
 * 全程不碰光标 (幽灵点击已退役)。
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
 *   序列: 删旧圈 NodeDelete×4 (上次角, 有上次才发) → 首单位 Mega×4
 *   建节点 → 跟随者 Connect×4/人挂接 → 全员→首角 close-Connect 合环
 *   (联机同步) → 直接关 → 验 loop>0 → Commit。事件按序进 OutList,
 *   执行保序; 各阶段就绪即走, 无固定等待。
 *   TargetCell: 0x6E6B20 (ID=X+1000*Y, RTTI=11);
 *   TargetUnit: 0x6E6AB0 (ID=Fetch_ID, RTTI=Abstract=52)。
 *   入队走 OutList (与 BuildQueue 同式, 满则跨帧续发)。
 *   选中表 0xA8ECBC/0xA8ECC8; 本家 CurrentPlayer 0xA83D4C+[0x30];
 *   中心格经 Display::ProcessClickCoords 0x692300; 角校验经
 *   MapCells=[0x87F7E8+0x13C]/MaxCells=[0x87F7E8+0x140]。
 *
 * 教训:
 *   v1 只发 Connect: 无节点可链, 全丢弃。v2 只发 Mega×5: 建节点但
 *   第 5 Mega 因子事件帧戳对不上而建重复, 不闭环。v3 Mega(首单位)+
 *   Connect(全单位): Connect 在空 token 上挂不上, 退化单单位。
 *   v4/v5 幽灵点击: 链路正确但第 5 点像素未对齐中心则不闭环, 且
 *   commit 抢跑会消费空路径 (后加 loop 门控修好, 环已能成)。
 *   v6 观测式幽灵点击: hover 门控 + loop 门控, 环稳定建成, 但要动光标。
 *   v7 纯事件复刻 (Mega 全单位 + Connect 全单位 + 门控 commit):
 *   编码与原生逐字节一致但 loop 恒 0——Connect 执行关不上环。
 *   v8 = Mega 建节点 + 直接调 UI 关闭函数 + Commit (无光标动作)。
 *   v9: 待发一次突发 (同逻辑帧同帧戳), 多单位挂同一套共享节点
 *   (跨帧分批会建重复=每个单位一套圈); 节点增长数记日志验证 (=4)。
 *   v10: 日志证实第一遍只建节点不挂 token (新格), 第二遍同位置才
 *   挂 token——改两遍发送 (自适应: 第一遍后 token 齐则跳过第二遍)。
 *   v11: 野指针/旧空 token 误报就绪致门控抢跑 (mega 执行前 close 跑完)
 *   ——token 深度校验 (数组+计数+首节点) 后才算就绪/闭环。
 *   v12: 跨触发 token 残留致新事件往旧圈追加 (越试圈越多)——触发时
 *   解绑选中单位旧 token (fresh start, 只清单位侧回指)。
 *   v13: 多选只建一套共享圈——之前全单位 Mega, 执行侧 0x638CE0 对无
 *   token 单位必回 -1 (自建新节点), 故人手一套圈 (nodes=单位数×4)。
 *   原生协议 (反汇编实证): 首单位 Mega 建节点 + 其余单位 PlanConnect
 *   (0x2A, 构造 0x4C6780, 单位→格) 挂到已存在节点上; 执行侧 0x637EFE
 *   按格找节点 → 无 token 则建 token → 挂 token → 调关闭函数, 全程不
 *   建新节点。改后: 角 k 发 Mega(首单位) + Connect(其余, 同角),
 *   节点增长恒=4。单选路径与之前逐字节一致, 零回归。
 *   v14: 去延时去上限——排空后等一个逻辑帧即闭环提交 (无 ExecMs 超时/
 *   无第二遍; 帧计不动时 poll 数兜底防 busy 卡死); 去 MaxUnits, 选中
 *   多少跟多少 (OutList 满则 drip 跨帧续发, Connect 按格找节点跨帧可挂;
 *   Mega 恒首单位 4 个)。
 *   v15: v14 等一帧是错的——日志实证排空后 1 帧时 nodes=0/tokens=0
 *   (引擎延迟数帧才执行, close 空跑致 noloop; 事件随后执行故画面有线
 *   但环未闭)。改就绪门控: 全员 token 挂满 (n>=4) 当即闭环提交, 不等
 *   固定时间; 仅真失败时 poll 上限兜底关已有。conev 编码已对
 *   (与 megev 同格同帧戳, 日志 hex 核对过), 不动。
 *   v16: v15 的 poll 计数是错的——PowerDrawBoth 一逻辑帧调几十次,
 *   45 polls 在同一帧内跑完 (日志: cap 前连一行 wait 都没有),
 *   引擎执行 (数逻辑帧延迟) 永远等不到。改按逻辑帧计数:
 *   全满即闭环; 30 逻辑帧还不满才兜底; polls 只做冻结防挂
 *   (暂停/菜单帧不动时放行不清 busy)。
 *   v17: Connect 与 Mega 同突发时挂不上 (执行顺序: Connect 先跑则格
 *   上无节点直接丢弃, 永不重试; 日志: nodes=4 但 followers 30 帧
 *   ready=0)。改分阶段: 先发首单位 Mega → 首单位挂满 (节点已存在)
 *   再发跟随者 Connect → 全满闭环提交。各阶段就绪即走, 无固定等待。
 *   v18: 旧圈清不掉根因——commit 不删节点 (总数 4→40 实锤), 节点靠
 *   token 引用渲染; v12 的 fresh-detach 把单位侧砍断, 孤儿 token 抱旧
 *   节点永久渲染。修: 保留 detach (防跨触发追加/ stale token), 触发时
 *   先发 PlanNodeDelete (0x2C, 构造 0x4C65E0) 删上次 4 角 (执行侧从
 *   token 解链再删节点, 原生机制), 删完 (节点数回落) 再建新圈。
 *   闭环改玩家式: 全满后发 close-Connect (全员→首角, 原生手动即 8 连
 *   Connect 后 Commit), 等首单位 loop 落地再直接关+提交 (联机同步闭环
 *   状态, 单选仍走老路)。全标准事件, 联机安全。
 *   v19: 已有闭环圈时连续按崩溃。两处 hardening (全标准事件, 不碰模拟):
 *   (1) 快照跨帧防野指针——选中单位在 op 期间战死则快照悬空, 下轮询
 *   vcall 即崩。用引擎 ID 索引 (0xB0E840 基址/0xB0E844 计数, 8 字节
 *   {ID,ptr}, 升序) 纯读二分验证 (指针==索引指针才算活), 先验活再碰
 *   对象; 索引机制自检失败则 fail-open (跳过校验, 功能不变)。
 *   (2) commit 消费确认——CloseCommit 只把 commit 发进 OutList, 执行
 *   在数帧后; 此时立即接受下一次触发, 新删旧事件与未执行完的旧事件
 *   交错 (原生对此直接拦截+提示)。改 phase4: 忙到首单位 token 指针
 *   变化 (被消费) 或 15 帧, 再放行。
 *   v20: v19 仍崩, 日志定位——4×NodeDelete 进队后执行时崩。根因:
 *   NodeDelete 执行逐成员调 0x705D20 取 token 且无空检查, 而触发时已
 *   detach (成员 token 全空) → 空解引用。v18 删旧能活 purely 因为上次
 *   角无节点 (搜索无命中直接返回)。修: 删→detach→建 (删时成员必须链
 *   接, 与原生右键删节点配置一致); delok/无旧圈时才 detach; 删阶段不
 *   读 token (防删执行中途读半截状态); 掉 delcur (全空 abort 时成员已
 *   断, 删即崩, 残留交下次触发链式清理); 掉 phase4 (postcap 实证 token
 *   常驻, 15 帧纯开销且 FIFO 已保序, 回到执行完即放行)。
 *
 * 联机: 只发标准事件, 各端同一份原版代码执行, 不影响联机。
 * 本地只读 + 标准事件入队, 不写模拟内存。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MsgFont 同一 DLL)。
 * 轮询挂 PowerDrawBoth (见 MsgFont.cpp), 开关关时直接返回。
 */

#include <windows.h>
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
#define EVT_PLANNODEDELETE 0x2Cu
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
/* PlanNodeDelete ctor (0x4C65E0, ECX=ev111, ret 0x10):
 * house, type(0x2C), tgtID, tgtRTTI(dword 取低字节)。
 * 布局: Type@0, House@2, Frame@3, Target(格)@7(ID@7+RTTI@11)。 */
typedef void* __attribute__((thiscall)) (*Fn_PlanNodeDeleteCtor)(void* ev, int houseIdx, int evtType,
    int tgtID, unsigned tgtRttiW);
typedef void* __attribute__((thiscall)) (*Fn_PlanCommitCtor)(void* ev, int houseIdx, int evtType);
/* UI 关闭函数 0x639740: fastcall ECX=单位, EDX=节点 (无栈参, 本地闭环,
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
#define FN_PLANNODEDEL ((Fn_PlanNodeDeleteCtor)(void*)0x4C65E0u)
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
    /* v14: MaxUnits/GhostExecMs 已删除 (不限单位, 不等超时), ini 残留键忽略。 */
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

/* 首 30 字节 hex (编码核对, 与原生 bq:ev 交叉验证用)。 */
static void PC_DumpEv(const char* tag, const char* ev)
{
    char buf[128], hx[4];
    unsigned q = 0, i;
    q = AppendCStr(buf, q, tag, sizeof(buf));
    for (i = 0; i < 30; i++)
    {
        unsigned char b = (unsigned char)ev[i];
        hx[0] = "0123456789ABCDEF"[b >> 4];
        hx[1] = "0123456789ABCDEF"[b & 15];
        hx[2] = 0;
        q = AppendCStr(buf, q, hx, sizeof(buf));
    }
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

static int g_PcDumpMega = 0;
static int g_PcDumpConn = 0;
static int g_PcDumpDel = 0;

static int PC_FireMega(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstID, unsigned char dstRtti)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_MOVE,
            0, 0, dstID, (unsigned)dstRtti, 0, 0);
    ev[OFF_PLANFLAG] = 1;
    if (!g_PcDumpMega) { g_PcDumpMega = 1; PC_DumpEv("pc: megev ", ev); }
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

/* PlanNodeDelete (删角格节点, 执行侧从 token 解链再删, 无则忽略)。 */
static int PC_FireNodeDelete(int houseIdx, int dstID, unsigned char dstRtti)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_PLANNODEDEL(ev, houseIdx, EVT_PLANNODEDELETE, dstID, (unsigned)dstRtti);
    if (!g_PcDumpDel) { g_PcDumpDel = 1; PC_DumpEv("pc: delev ", ev); }
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
    if (!g_PcDumpConn) { g_PcDumpConn = 1; PC_DumpEv("pc: conev ", ev); }
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
 * Connect 按格找节点, 跨帧不影响挂接; NodeDelete 无则忽略。 */

#define ADDR_NODES_COUNT 0xAC4B40u /* 规划节点数 (执行侧搜索上界, 只读验证) */
#define ADDR_FRAME 0xA8ED84u       /* 当前帧 (事件 Frame 戳源; 排空后看它推进) */
#define ADDR_IDBASE 0xB0E840u      /* Fetch_ID 索引基址 (纯读存活校验) */
#define ADDR_IDCOUNT 0xB0E844u     /* Fetch_ID 索引计数 */
#define PC_WAIT_FRAMES 30          /* 真失败兜底: 30 逻辑帧还不满才关已有 */
#define PC_WAIT_POLLCAP 3000       /* 冻结防挂: 帧不动时 polls 到此放行 */
#define PC_DEL_FRAMES 4            /* 删旧圈等待回落上限 (正常 1~2 帧) */
#define PC_CLOSEC_FRAMES 6         /* close-Connect 落地上限 (之后直接关兜底) */

typedef struct { void* unit; short x, y; char conn; int id; } PCPend;

static PCPend g_PcPend[PC_PEND_CAP];
static int g_PcPendCount = 0;
static int g_PcCommitPend = 0;
static void* g_PcHouse = 0;
static int g_PcHouseIdx = 0;
static int g_PcPrevTrig = 0;
static int g_PcWaitExec = 0;      /* 排空后等 token 落地再闭环 */
static int g_PcWaitFrame = 0;     /* 排空时帧快照 */
static int g_PcWaitPolls = 0;     /* 冻结防挂计数 (帧不动时累积) */
static int g_PcWaitFrames = 0;    /* 已推进逻辑帧数 (主标尺) */
static int g_PcWaitLogFr = 0;     /* wait 进度日志节流 */
static int g_PcPhase = 0;         /* 0=删旧圈 1=等首单位挂满 2=等全满 3=等合环落地 */
static int g_PcIdxOk = 0;         /* ID 索引机制自检通过 (存活校验总开关) */
static int g_PcPrevCorX[4], g_PcPrevCorY[4]; /* 上次 4 角 (删旧圈用) */
static void* g_PcPrevUnits[PC_CLOSE_CAP];   /* 上次成员 (删前存活 gate) */
static int g_PcPrevIDs[PC_CLOSE_CAP];
static int g_PcPrevCount = 0;
static int g_PcPrevValid = 0;
static int g_PcNodes0 = -1;      /* 触发时节点数快照 (验证只建一套圈) */
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
 * 索引指针==快照指针才算活。快照跨帧, 单位战死即悬空, 先验活再碰。
 * g_PcIdxOk=0 (自检未通过) 时 fail-open 全回活 (功能不变)。 */
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
 * (数组有效 + 节点数合法 + 首节点有效), 否则门控抢跑 (v10 血案:
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

/* 建待发。分阶段 (v17/v18): 删旧(上次 4 角 NodeDelete) → MegaPass
 * (首单位 4 角, 建节点) → ConnPass (其余同角 Connect, 挂已有节点) →
 * CloseConnPass (全员→首角 Connect, 玩家式合环, 联机同步)。
 * 同突发混发时 Connect 可能先于建节点执行而被丢弃, 故必须分开。
 * conn: 0=Mega(需单位) 1=Connect(需单位) 2=NodeDelete(不需单位)。 */
static void PC_BuildDelPass(void)
{
    int k;
    g_PcPendCount = 0;
    for (k = 0; k < 4; k++)
    {
        if (g_PcPendCount >= PC_PEND_CAP) break;
        g_PcPend[g_PcPendCount].unit = 0;
        g_PcPend[g_PcPendCount].x = (short)g_PcPrevCorX[k];
        g_PcPend[g_PcPendCount].y = (short)g_PcPrevCorY[k];
        g_PcPend[g_PcPendCount].conn = 2;
        g_PcPend[g_PcPendCount].id = 0;
        g_PcPendCount++;
    }
}
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

/* 全员→首角 Connect (玩家式合环, 联机同步闭环状态)。 */
static void PC_BuildCloseConnPass(void)
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
        if (e->conn == 2)
        {
            /* 删旧圈: 不需单位 */
            if (!PC_FireNodeDelete(houseIdx, dstID, dstR))
                return 0; /* 满: 下轮继续 */
        }
        else
        {
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
        }
        PC_ShiftDown();
        sent++;
    }
    if (g_PcPendCount <= 0)
    {
        g_PcWaitExec = 1;
        g_PcWaitFrame = *(int*)ADDR_FRAME;
        g_PcWaitPolls = 0;
        g_PcWaitFrames = 0;
        g_PcWaitLogFr = g_PcWaitFrame;
        PC_Trace("pc: drained", houseIdx, sent, g_PcWaitFrame);
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

/* 新规划顶掉旧绑定: 解绑选中单位残留 token。只清单位侧回指, token
 * 对象本身不动 (遍历代码仍见有效 Owner, 不引入空悬挂), 交由引擎管理。
 * 调用时机铁律: 只能在无 NodeDelete 在途时调 (删执行逐成员取 token 无
 * 空检查, 断后删即崩, v20 血案)。即: 无旧圈触发时立即调; 有旧圈时等
 * delok 后调。不影响联机。 */
static void PC_DetachAll(void)
{
    int i, detached = 0;
    for (i = 0; i < g_PcCloseCount; i++)
    {
        void* u = g_PcCloseUnits[i];
        void* t;
        if (!PC_AliveAt(i)) continue;
        if (!PC_IsFoot(u)) continue;
        t = *(void**)((char*)u + OFF_TECH_TOKEN);
        if (t && IsPlausiblePtr(t))
        {
            *(void**)((char*)u + OFF_TECH_TOKEN) = 0;
            detached++;
        }
    }
    if (detached) PC_Trace("pc: fresh", detached, 0, 0);
}

/* ---------------- 触发 ---------------- */

static void PC_Trigger(void)
{
    void* cur;
    void** items;
    int selN, i, nu = 0;
    int mx, my;
    Point2D pt;
    CellStruct center;
    CoordStruct crd;
    void* tgtObj = 0;
    unsigned char b5 = 0, b6 = 0;
    int ok;
    int half, cx, cy;
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

    if (!GtMouseGameXY(&mx, &my)) { PC_Trace("pc: no mouse", 0, 0, 0); return; }
    pt.X = mx; pt.Y = my;
    center.X = center.Y = 0;
    crd.X = crd.Y = crd.Z = 0;
    ok = FN_CLICKCOORDS((void*)ADDR_DISPLAY, &pt, &center, &crd, &tgtObj, &b5, &b6);
    if (!ok) { PC_Trace("pc: off map", mx, my, 0); return; }
    cx = (int)center.X; cy = (int)center.Y;
    if (!PC_CellValid(cx, cy)) { PC_Trace("pc: bad center", cx, cy, 0); return; }

    half = g_PcSize / 2;
    if (half < 1) half = 1;
    corX[0] = cx - half; corY[0] = cy - half;
    corX[1] = cx + half; corY[1] = cy - half;
    corX[2] = cx + half; corY[2] = cy + half;
    corX[3] = cx - half; corY[3] = cy + half;
    for (i = 0; i < 4; i++)
    {
        if (!PC_CellValid(corX[i], corY[i]))
        {
            PC_Trace("pc: corner off map", corX[i], corY[i], i);
            return;
        }
    }

    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    g_PcHouse = cur;
    g_PcHouseIdx = houseIdx;
    g_PcPendCount = 0;
    g_PcCommitPend = 0;
    g_PcWaitExec = 0;
    g_PcDumpMega = 0;
    g_PcDumpConn = 0;
    g_PcDumpDel = 0;
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
    {
        int nc = *(int*)ADDR_NODES_COUNT;
        g_PcNodes0 = (nc >= 0 && nc < 200000) ? nc : -1;
    }
    for (i = 0; i < 4; i++) { g_PcCorX[i] = corX[i]; g_PcCorY[i] = corY[i]; }
    /* 阶段: 有旧圈 → 先删 (成员链接态, 删执行安全), delok 后 detach 再建;
     * 无旧圈 → 立即 detach (无删在途, 安全) 后建新圈。闭环不用第 5 Mega
     * (帧戳对不上会建重复), 改玩家式 close-Connect + 直接调关闭函数。
     * 删前 gate: 上次成员必须全活 (战死成员悬空, 删执行同样会崩;
     * 有死亡 → 跳过删, 宁漏删 (下次人齐再触发会清) 不崩溃)。 */
    if (g_PcPrevValid)
    {
        int k, allAlive = 1;
        for (k = 0; k < g_PcPrevCount; k++)
        {
            if (!PC_PtrAliveRaw(g_PcPrevUnits[k], g_PcPrevIDs[k]))
            { allAlive = 0; break; }
        }
        if (allAlive)
        {
            g_PcPhase = 0;
            PC_BuildDelPass();
            PC_Trace("pc: del", g_PcNodes0, 0, 0);
        }
        else
        {
            PC_Trace("pc: delskip_dead", g_PcPrevCount, 0, 0);
            PC_DetachAll();
            g_PcPhase = 1;
            PC_BuildMegaPass();
        }
    }
    else
    {
        PC_DetachAll();
        g_PcPhase = 1;
        PC_BuildMegaPass();
    }
    for (i = 0; i < 4; i++)
    { g_PcPrevCorX[i] = corX[i]; g_PcPrevCorY[i] = corY[i]; }
    g_PcPrevValid = 1;
    g_PcPrevCount = 0;
    for (i = 0; i < g_PcCloseCount; i++)
    {
        g_PcPrevUnits[i] = g_PcCloseUnits[i];
        g_PcPrevIDs[i] = g_PcCloseIDs[i];
    }
    g_PcPrevCount = g_PcCloseCount;
    g_PcCommitPend = 1;
    PC_Trace("pc: fire", nu, cx, cy);
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
    {
        int nc = *(int*)ADDR_NODES_COUNT;
        int grew = (g_PcNodes0 >= 0 && nc >= 0 && nc < 200000) ?
            nc - g_PcNodes0 : -999;
        PC_Trace("pc: nodes", grew, nc, 0);
    }
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
        /* 五阶段就绪门控 (主标尺=逻辑帧):
         * 阶段 0 (删旧圈): 节点数回落 → detach → 发 Mega 建新圈
         * (4 帧无回落也走)。删阶段不读 token (删执行中途状态半截)。
         * 阶段 1: 首单位挂满 → 发跟随者 Connect (单选则直接闭环)。
         * 阶段 2: 全员挂满 → 发 close-Connect (多选; 单选直接闭环)。
         * 阶段 3: 首单位 loop 落地 (合环执行到) → 直接关+提交。
         * 帧每推进记一行 wait; 各阶段超限 → 兜底 (关已有/跳过);
         * 帧长期不动 → polls 放行防挂。 */
        int live = 0, ready = 0;
        int nc, grew, fr;
        int cap;
        if (g_PcPhase > 0)
            PC_TokenStats(&live, &ready);
        nc = *(int*)ADDR_NODES_COUNT;
        grew = (g_PcNodes0 >= 0 && nc >= 0 && nc < 200000) ? nc - g_PcNodes0 : -999;
        g_PcWaitPolls++;
        fr = *(int*)ADDR_FRAME;
        cap = (g_PcWaitFrames > PC_WAIT_FRAMES || g_PcWaitPolls > PC_WAIT_POLLCAP) ? 1 : 0;
        if (g_PcPhase > 0 && !live)
        {
            PC_Trace("pc: wait abort", 0, 0, 0);
            PC_ClearPend();
        }
        else if (g_PcPhase <= 0 &&
                 ((g_PcNodes0 >= 0 && nc < g_PcNodes0) || g_PcWaitFrames >= PC_DEL_FRAMES))
        {
            /* 删旧圈落地 (或无旧可删) → 先 detach (删已执行完, 安全),
             * 再建新圈。detach 后的 token 读一律回空, 门控安全。 */
            PC_Trace("pc: delok", grew, nc, 0);
            PC_DetachAll();
            g_PcPhase = 1;
            g_PcWaitFrames = 0;
            g_PcWaitPolls = 0;
            PC_BuildMegaPass();
            PC_Drain();
        }
        else if (g_PcPhase == 1 && PC_LeaderFull())
        {
            if (g_PcCloseCount <= 1)
            {
                PC_Trace("pc: tokens", ready, live, grew);
                PC_CloseCommit();
            }
            else
            {
                PC_Trace("pc: conn", ready, live, grew);
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
                PC_Trace("pc: tokens", ready, live, grew);
                PC_CloseCommit();
            }
            else
            {
                PC_Trace("pc: cconn", ready, live, grew);
                g_PcPhase = 3;
                g_PcWaitFrames = 0;
                g_PcWaitPolls = 0;
                PC_BuildCloseConnPass();
                PC_Drain();
            }
        }
        else if (g_PcPhase >= 3 && PC_LoopCount() > 0)
        {
            PC_Trace("pc: tokens", ready, live, grew);
            PC_CloseCommit();
        }
        else
        {
            if (fr != g_PcWaitLogFr)
            {
                g_PcWaitLogFr = fr;
                g_PcWaitFrames++;
                PC_Trace((g_PcPhase >= 3) ? "pc: cwait" : "pc: wait",
                         ready, live, grew);
            }
            if ((g_PcPhase >= 3 && g_PcWaitFrames > PC_CLOSEC_FRAMES) || cap)
            {
                PC_Trace("pc: cap", ready, live, grew);
                if (ready > 0)
                    PC_CloseCommit(); /* 有几个算几个 */
                else
                    /* 全空: 不删 (成员已断, 删执行无空检查会崩;
                     * 残留交下次触发链式清理, 那时成员链接态安全)。 */
                    PC_ClearPend();
            }
        }
    }
    if (!g_PcEnable) { g_PcPrevTrig = trigDown; return; }
    if (g_PcPendCount > 0 || g_PcWaitExec || g_PcCommitPend)
    { g_PcPrevTrig = trigDown; return; }
    if (trigDown && !g_PcPrevTrig)
    {
        if (!g_PcRequireZ || (GetAsyncKeyState(g_PcZVK) & 0x8000))
            PC_Trigger();
        else
            PC_Trace("pc: need Z", 0, 0, 0);
    }
    g_PcPrevTrig = trigDown;
}
