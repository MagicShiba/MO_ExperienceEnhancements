/* RepairAll.cpp - 建筑维修操作量简化。
 *
 * 背景: 原版修建筑要点维修图标 (或按维修快捷键) 进维修光标, 再逐个点带血的
 *       建筑。密集挨打时操作量爆炸。
 * 用法: [RepairAll] Mode 三档
 *       0 = 保持原样 (本模块完全不介入, 两个钩子直接返回 0);
 *       1 = 半自动, 手动触发;
 *       2 = 自动修, 定时扫, 掉了血且未在修的建筑自动进维修流程。
 * 手动触发 (Mode=1/2 都行, DoubleClick=0 可关掉后两条):
 *       Ctrl + 点维修图标 / Ctrl + 维修快捷键 / 双击维修图标 / 连按两下快捷键。
 * 触发点就是原版那两处, 不新增键位; 快捷键 VK 从 KeyboardMD.ini 的
 * [Hotkey] ToggleRepair 读, 玩家改键自动跟随。
 *
 * 联机 (AGENTS.md 铁律): **不写任何模拟内存, 不直调改模拟的引擎函数**。
 *   唯一的模拟变更 = 往 OutList 塞标准事件 EventType::Repair (0x15), 构造
 *   方式与玩家手点维修图标**逐字节同形** (照抄引擎 0x4AC0BE 那一段, 连 house
 *   都取 [0xA83D4C]+0x30)。真去改血的是引擎自己的 BuildingClass::Update,
 *   各端拿同一条事件得到同一结果。钱照扣, 与手点维修一致。
 *   其余全是只读。
 *
 * 只读偏移 (全部反汇编实证, 不靠类型头推算 —— 类型类布局会偏 0xA00):
 *   ObjectClass::Health 0x6C / InLimbo 0x81 / IsAlive 0x90
 *   TechnoClass::Owner 0x21C     TechnoClass 恰好排到 0x520, 故
 *   BuildingClass::Type 0x520     0x450630/0x4FF5BD 也把它当类型指针用
 *   BuildingClass::IsBeingRepaired 0x6E8   (0x4506C6 读 / 0x4509B4 清 0)
 *   ObjectTypeClass::Strength 0xA0         (引擎 0x5F5C42 自己的 fild [eax+0xA0])
 *   Mission 0xAC (BuildingClass 虚表槽 0x184 = 0x5B3040 = mov eax,[ecx+0xAC])
 *   BuildingClass::Array 0xA8EB40 / HouseClass::Array 0xA80228
 *     (DynamicVectorClass: Items@+4 / Count@+16, 传对象基址, 同 SWSBar)
 *   Frame 0xA8ED84
 *
 * 三条引擎/构建铁律 (改本文件前先读):
 *
 * 1) **钩子返回 0 时 Syringe 把被偷的字节搬到跳板里原样重放**, 里面若有直接
 *    相对跳转 (E8/E9/0F 8x/短 jcc), 基址变了而 rel32 没跟着变 -> 跳野地址。
 *    所以钩点必须挑**无 rel** 的指令边界: 维修图标钩点在 0x6A78D9 偷 6B
 *    (`mov ecx,[0x8871E0]`), 不用 0x6A78F6 偷 9B (第三条是 `call 0x4AC8C0`);
 *    热键钩点 0x536FA0 偷的 `mov ecx,imm32` 里那个 E8 只是 imm32 的一个字节。
 *    改任何 .inj 钩子后先跑 tools/hook-rel-audit.py。
 *
 * 2) **bool 返回值只保证在 AL**。引擎实现里出现 `xor al,al` / `mov al,x` 的,
 *    函数指针返回类型必须声明成 unsigned char —— 声明成 int 会读到 EAX 高 24 位
 *    的垃圾, 把"false"读成 true (RA_CanRepair 必须这么声明)。对照
 *    WhatAmI(0x459EC0) 是 `mov eax,6; ret` 写满 EAX, 那个声明 int 才安全。
 *
 * 3) **IsPlausiblePtr 不能当前置解引用或调虚表** —— 它只判 [64K,2G)+4 对齐,
 *    堆/栈里随便一个数都能过。判"这真是个对象"看虚表指针是否落在 exe 映像内
 *    (RA_IsObj); 判"这真是个函数"看目标是否落在 exe 内 (RA_InExe)。
 *
 * 另: 每帧路径上不允许 GetPrivateProfileIntA (同 SWSBar.cpp), 配置全部在
 *   RA_EnsureInit 读一次; 0x4F42F0 一帧跑 ~142 次, 故 Poll 内自门控帧号;
 *   钩子里只读键态/置标志, 全图扫描一律挪到 RepairAll_Poll 的正常帧上下文。
 *
 * 构建见 build-fast.bat / build-full.bat (32 位 mingw, 无 CRT)。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "RepairAll.h"

#define RA_SECTION "RepairAll"

/* ---------------- 只读地址 ---------------- */

#define ADDR_BLD_ARR   0xA8EB40u   /* BuildingClass::Array */
#define ADDR_HOUSE_ARR 0xA80228u   /* HouseClass::Array */
#define ADDR_FRAME     0xA8ED84u
#define ARR_ITEMS(base) (*(void***)((base) + 4u))
#define ARR_COUNT(base) (*(int*)((base) + 16u))

#define OFF_HEALTH        0x6Cu
#define OFF_INLIMBO       0x81u
#define OFF_ISALIVE       0x90u
#define OFF_OWNER         0x21Cu   /* TechnoClass::Owner -> HouseClass* */
#define OFF_HOUSE_IDX     0x30u    /* HouseClass::ArrayIndex */
#define OFF_HOUSE_MONEY   0x2F0u   /* HouseClass::Money (Owner 值校验用) */
#define OFF_BLD_TYPE      0x520u   /* BuildingClass::Type */
#define OFF_BLD_REPAIRING 0x6E8u   /* BuildingClass::IsBeingRepaired */
#define OFF_TYPE_STRENGTH 0xA0u    /* ObjectTypeClass::Strength */

/* gamemd.exe 映像范围, 用于 RA_InExe。BuildingClass 构造链
 * 0x43B740->0x6F2B40->0x65A750 往对象头写 0x7E180C (.rdata), 槽里是
 * 0x40xxxx 代码指针 —— 真对象的虚表与引擎函数都落在这个范围内。 */
#define RA_EXE_LO 0x401000u
#define RA_EXE_HI 0x8A0000u

#define EVT_REPAIR        0x15    /* EventType::Repair */
#define ADDR_SKIP_HOTKEY  0x00536FACu  /* 0x536FA0 偷 7B 后的落点 (ret 4) */
#define ADDR_SKIP_BUTTON  0x006A78FFu  /* 0x6A78D9 偷 6B 后的落点 (分支公共尾) */

/* 标准事件构造, 与 SmartOrder/DeployPreview 手点同形;
 * TargetClass 构造器 FN_TGT_OBJ 复用 OrderUtil.h 的 (同一函数 0x6E6AB0)。 */
typedef void* __attribute__((thiscall)) (*Fn_RaTgtEventCtor)(void* ev, int houseIdx,
    int evtType, int id, unsigned rttiW);
#define FN_TGT_EV ((Fn_RaTgtEventCtor)(void*)0x4C65E0u)

typedef int __attribute__((thiscall)) (*Fn_WhatAmI)(void* self);
typedef unsigned char __attribute__((thiscall)) (*Fn_CanRepair)(void* self);
typedef int __attribute__((thiscall)) (*Fn_GetMission)(void* self);

#define MISSION_CONSTRUCTION 0x12
#define MISSION_SELLING      0x13

#define RA_QCAP    512u  /* 待发队列上限 */
#define RA_BATCH   24    /* 每逻辑帧最多入队事件数 (OutList 容量 128 要留余量) */
#define RA_SCAN_CAP 256  /* Owner 值校验抽样上限 */
#define RA_DBL_SLOP 12   /* 双击两点容差 (游戏像素) */

/* ---------------- 配置 ---------------- */

static int g_RaInit = 0;
static int g_RaMode = 0;          /* [RepairAll] Mode, 0~2 */
static int g_RaInterval = 30;     /* Mode=2 扫描间隔 (逻辑帧, 30≈1秒) */
static int g_RaBatch = RA_BATCH;  /* [RepairAll] Batch */
static int g_RaDbl = 1;           /* [RepairAll] DoubleClick */
static int g_RaDblFrames = 12;    /* [RepairAll] DblFrames */
static int g_RaRepairVk = 0x4B;   /* 维修快捷键 VK, 读 KeyboardMD.ini */

/* 目录部分从 g_IniPath 截出 (同 BuildPaths: 在最后一个分隔符处截断再拼名)。 */
static void RA_LoadRepairKey(void)
{
    char dir[MAX_PATH_LEN];
    char path[MAX_PATH_LEN];
    unsigned i, k;
    int vk;
    for (i = 0; i + 1 < (unsigned)MAX_PATH_LEN && g_IniPath[i]; i++)
        dir[i] = g_IniPath[i];
    dir[i] = 0;
    while (i > 0 && dir[i - 1] != '\\' && dir[i - 1] != '/') i--;
    if (i == 0) return;                      /* 取不到目录, 保持默认 'K' */
    dir[i - 1] = 0;
    k = AppendCStr(path, 0, dir, MAX_PATH_LEN);
    if (k == 0 || k + 18 >= (unsigned)MAX_PATH_LEN) return;
    k = AppendCStr(path, k, "\\KeyboardMD.ini", MAX_PATH_LEN);
    vk = GetPrivateProfileIntA("Hotkey", "ToggleRepair", 0x4B, path);
    if (vk > 0 && vk < 256) g_RaRepairVk = vk;
}

static void RA_EnsureInit(void)
{
    char buf[96];
    unsigned p = 0;
    if (g_RaInit) return;
    g_RaInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_RaMode = GetPrivateProfileIntA(RA_SECTION, "Mode", 0, g_IniPath);
    if (g_RaMode < 0) g_RaMode = 0;
    if (g_RaMode > 2) g_RaMode = 2;
    g_RaInterval = GetPrivateProfileIntA(RA_SECTION, "Interval", 30, g_IniPath);
    if (g_RaInterval < 5) g_RaInterval = 5;
    if (g_RaInterval > 600) g_RaInterval = 600;
    g_RaBatch = GetPrivateProfileIntA(RA_SECTION, "Batch", RA_BATCH, g_IniPath);
    if (g_RaBatch < 1) g_RaBatch = 1;
    if (g_RaBatch > 96) g_RaBatch = 96;
    g_RaDbl = GetPrivateProfileIntA(RA_SECTION, "DoubleClick", 1, g_IniPath) ? 1 : 0;
    g_RaDblFrames = GetPrivateProfileIntA(RA_SECTION, "DblFrames", 12, g_IniPath);
    if (g_RaDblFrames < 3) g_RaDblFrames = 3;
    if (g_RaDblFrames > 60) g_RaDblFrames = 60;
    RA_LoadRepairKey();
    p = AppendCStr(buf, p, "repairall: mode=", sizeof(buf));
    p += UtoA((unsigned)g_RaMode, buf + p);
    p = AppendCStr(buf, p, " interval=", sizeof(buf));
    p += UtoA((unsigned)g_RaInterval, buf + p);
    p = AppendCStr(buf, p, " batch=", sizeof(buf));
    p += UtoA((unsigned)g_RaBatch, buf + p);
    p = AppendCStr(buf, p, " dbl=", sizeof(buf));
    p += UtoA((unsigned)g_RaDbl, buf + p);
    p = AppendCStr(buf, p, "/", sizeof(buf));
    p += UtoA((unsigned)g_RaDblFrames, buf + p);
    p = AppendCStr(buf, p, " vk=", sizeof(buf));
    p += HtoA((unsigned)g_RaRepairVk, buf + p);
    LogLine(buf);
}

static int RA_On(void)
{
    RA_EnsureInit();
    return g_RaMode != 0;
}

static int RA_InExe(unsigned v)
{
    return (v >= RA_EXE_LO && v < RA_EXE_HI) ? 1 : 0;
}

static int RA_IsObj(void* p)
{
    if (!p || !IsPlausiblePtr(p)) return 0;
    return RA_InExe((unsigned)(size_t)*(void***)p);
}

static int RA_WhatAmI(void* b)
{
    void** vt;
    Fn_WhatAmI fn;
    if (!RA_IsObj(b)) return -1;
    vt = *(void***)b;
    fn = (Fn_WhatAmI)vt[0x2C / 4];
    if (!RA_InExe((unsigned)(size_t)fn)) return -1;
    return fn(b);
}

/* "这栋楼能不能点 wrench 修", 虚表槽 0x94 = 0x452630, 与原版扳手光标同一句
 * 判断。**必须问引擎, 不能自己抄那串关卡**: 资格判定全在 0x450630, 而那里
 * 0x4506CE 一看到 IsBeingRepaired 就短路去修理 —— 原版不让你修不可修的东西,
 * 靠的是光标阶段根本不生成 Repair 事件。只抄事件构造 = 绕过限制。
 * 返回类型是 unsigned char, 见文件头坑 2。 */
static int RA_CanRepair(void* b)
{
    void** vt;
    Fn_CanRepair fn;
    unsigned char r;
    if (!RA_IsObj(b)) return 0;
    vt = *(void***)b;
    fn = (Fn_CanRepair)vt[0x94 / 4];
    if (!RA_InExe((unsigned)(size_t)fn)) return 0;
    r = fn(b);
    return r ? 1 : 0;
}

/* 正在建造/出售中的楼不插队。引擎自己的开始修理判定也这么排
 * (0x450663 cmp 0x12 / 0x450676 cmp 0x13)。 */
static int RA_MissionBusy(void* b)
{
    void** vt;
    Fn_GetMission fn;
    int m;
    if (!RA_IsObj(b)) return 1;
    vt = *(void***)b;
    fn = (Fn_GetMission)vt[0x184 / 4];
    if (!RA_InExe((unsigned)(size_t)fn)) return 1;
    m = fn(b);
    return (m == MISSION_CONSTRUCTION || m == MISSION_SELLING) ? 1 : 0;
}

/* ---------------- Owner 偏移值校验 (只比值, 不解引用候选字段) ---------------- */

/* 指针是否落在 HouseClass::Array 内, 且 Money 像话。 */
static int RA_IsHouse(void* p)
{
    void** items;
    int n, i, money;
    if (!p) return 1;                     /* 空 Owner 放过 (未放置的中立) */
    if (!IsPlausiblePtr(p)) return 0;
    items = ARR_ITEMS(ADDR_HOUSE_ARR);
    n = ARR_COUNT(ADDR_HOUSE_ARR);
    if (!items || !IsPlausiblePtr(items) || n <= 0 || n > 64) return 0;
    for (i = 0; i < n; i++)
    {
        if (items[i] != p) continue;
        money = *(int*)((char*)p + OFF_HOUSE_MONEY);
        return (money >= 0 && money <= 10000000) ? 1 : 0;
    }
    return 0;
}

/* TechnoClass::Owner 必须是 HouseClass::Array 的成员, 且本家自己的建筑能命中。
 * 偏移一旦不对必然有建筑取不到 HouseClass, 整轮直接放弃 —— 宁可不修, 也绝不
 * 修到别人家。 */
static int RA_OwnerOk(void** items, int count, void* cur)
{
    int i, n, mine = 0;
    n = (count > RA_SCAN_CAP) ? RA_SCAN_CAP : count;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++)
    {
        void* b = items[i];
        if (!RA_IsObj(b)) return 0;
        if (!RA_IsHouse(*(void**)((char*)b + OFF_OWNER))) return 0;
        if (*(void**)((char*)b + OFF_OWNER) == cur) mine = 1;
    }
    return mine;
}

/* ---------------- 待发队列 (环形, 逻辑帧滴发) ---------------- */

static void* s_RaQPtr[RA_QCAP];
static int   s_RaQId[RA_QCAP];
static int   s_RaQHead = 0;
static int   s_RaQN = 0;
static int   s_RaIdxOk = 0;     /* Fetch_ID 索引自检通过 (OU_PtrAlive 才严格) */

static void RA_Push(void* b, int id)
{
    int w = s_RaQHead + s_RaQN;
    if (s_RaQN >= (int)RA_QCAP) return;
    if (w >= (int)RA_QCAP) w -= (int)RA_QCAP;
    s_RaQPtr[w] = b;
    s_RaQId[w] = id;
    s_RaQN++;
}

static void RA_Pop(void)
{
    s_RaQHead++;
    if (s_RaQHead >= (int)RA_QCAP) s_RaQHead = 0;
    s_RaQN--;
}

/* ---------------- 建筑数组稳定门 (同 SWSBar: 启动期半初始化垃圾会致 fault) --- */

static void* s_RaPrevItems = NULL;
static int   s_RaPrevCount = 0;
static int   s_RaStable = 0;

static void RA_Observe(void)
{
    void** items = ARR_ITEMS(ADDR_BLD_ARR);
    int count = ARR_COUNT(ADDR_BLD_ARR);
    if (!items || !IsPlausiblePtr(items) || count <= 0 || count > 4096)
    {
        s_RaPrevItems = NULL;
        s_RaPrevCount = 0;
        s_RaStable = 0;
        return;
    }
    if (items == s_RaPrevItems && count == s_RaPrevCount)
    {
        if (s_RaStable < 2) s_RaStable++;
        return;
    }
    s_RaPrevItems = items;
    s_RaPrevCount = count;
    s_RaStable = 1;
}

/* ---------------- 扫描 + 发事件 ---------------- */

/* 该修就入队。便宜的字节判据排在虚表调用前面。
 * 不用"已报过"表: Repair 事件处理 (0x4C6EFD) 只查 IsAlive 就
 * SetRepairState(-1), 标志一定被置上, IsBeingRepaired 本身就是可靠的
 * "已经报过"标记, 下一轮自然跳过; 事件万一被丢, 下一轮重发才是想要的。 */
static int RA_TryQueue(void* b, void* cur)
{
    char tb[8];
    void* type;
    unsigned i;
    int hp, str;
    if (!RA_IsObj(b)) return 0;
    if (!((unsigned char*)b)[OFF_ISALIVE]) return 0;
    if (((unsigned char*)b)[OFF_INLIMBO]) return 0;
    if (((unsigned char*)b)[OFF_BLD_REPAIRING]) return 0;   /* 已经在修 */
    if (*(void**)((char*)b + OFF_OWNER) != cur) return 0;   /* 不是自己的 */
    hp = *(int*)((char*)b + OFF_HEALTH);
    if (hp <= 0) return 0;                                  /* 报废/在拆 */
    if (RA_WhatAmI(b) != ABS_BUILDING) return 0;
    if (!RA_CanRepair(b)) return 0;                         /* 引擎说不能修就不修 */
    if (RA_MissionBusy(b)) return 0;                        /* 正在建造/出售中 */
    type = *(void**)((char*)b + OFF_BLD_TYPE);
    if (!RA_IsObj(type)) return 0;
    str = *(int*)((char*)type + OFF_TYPE_STRENGTH);
    if (str <= hp) return 0;                                /* 没掉血 */
    for (i = 0; i < 8; i++) tb[i] = 0;
    if (!FN_TGT_OBJ(tb, b)) return 0;
    if (*(int*)(tb + 0) <= 0) return 0;
    RA_Push(b, *(int*)(tb + 0));
    return 1;
}

/* 扫全图建筑。数组没稳定 / Owner 校验没过 / 队列堵了就什么都不做。 */
static void RA_Scan(void)
{
    void** items;
    void* cur;
    int count, i, added = 0;
    if (s_RaStable < 2 || s_RaQN >= (int)RA_QCAP) return;
    items = ARR_ITEMS(ADDR_BLD_ARR);
    count = ARR_COUNT(ADDR_BLD_ARR);
    if (items != s_RaPrevItems || count != s_RaPrevCount) return;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return;
    if (!RA_OwnerOk(items, count, cur)) return;
    for (i = 0; i < count; i++)
    {
        if (s_RaQN >= (int)RA_QCAP) break;
        if (RA_TryQueue(items[i], cur)) added++;
    }
    if (added > 0)
    {
        /* Fetch_ID 索引自检: 用队首实测一遍, 失败则 OU_PtrAlive 全 fail-open。 */
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        s_RaIdxOk = 0;
        if (s_RaQN > 0 && base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 200000 &&
            OU_PtrAliveRaw(s_RaQPtr[s_RaQHead], s_RaQId[s_RaQHead]))
            s_RaIdxOk = 1;
    }
}

static int RA_Fire(int houseIdx, void* b)
{
    char ev[EVT_SIZE];
    char tb[8];
    unsigned i;
    for (i = 0; i < 8; i++) tb[i] = 0;
    if (!FN_TGT_OBJ(tb, b)) return 0;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_TGT_EV(ev, houseIdx, EVT_REPAIR, *(int*)(tb + 0),
              (unsigned)(unsigned char)tb[4]);
    return OU_Enqueue(ev);
}

/* 逻辑帧滴发: 队列里逐个复核存活/归属/可修性/血量后入 OutList。队满留到下轮。 */
static void RA_Drain(void)
{
    void* cur;
    int houseIdx, sent = 0;
    if (s_RaQN <= 0) return;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) { s_RaQN = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_IDX);
    while (s_RaQN > 0 && sent < g_RaBatch)
    {
        void* b = s_RaQPtr[s_RaQHead];
        int id = s_RaQId[s_RaQHead];
        if (!RA_IsObj(b) ||
            !((unsigned char*)b)[OFF_ISALIVE] ||
            ((unsigned char*)b)[OFF_INLIMBO] ||
            ((unsigned char*)b)[OFF_BLD_REPAIRING] ||
            *(void**)((char*)b + OFF_OWNER) != cur ||
            *(int*)((char*)b + OFF_HEALTH) <= 0) { RA_Pop(); continue; }
        if (!OU_PtrAlive(b, id, s_RaIdxOk)) { RA_Pop(); continue; }
        if (RA_WhatAmI(b) != ABS_BUILDING) { RA_Pop(); continue; }
        if (!RA_CanRepair(b)) { RA_Pop(); continue; }
        if (RA_MissionBusy(b)) { RA_Pop(); continue; }
        if (!RA_Fire(houseIdx, b)) return;   /* OutList 满: 保留到下轮 */
        RA_Pop();
        sent++;
    }
}

/* ---------------- 两个触发钩子 (见 dist/MOExperience.dll.inj) ---------------- */

static int s_RaPending = 0;     /* 钩子只置这个标志, 扫描挪到 Poll 里做 */
static int s_RaKeyFresh = 0;    /* 维修快捷键在上次"按下"之后被松开过 */

static int s_RaDblFrame = -1;
static int s_RaDblX = 0;
static int s_RaDblY = 0;

/* 双击判定。帧号用引擎的 0xA8ED84; 图标那边再比点位 (GtMouseGameXY, 侧栏
 * 区域同样有效), 快捷键那边靠"两下之间松开过"来排除按住不放的自动重复。
 * 命中后清状态, 免得第三下又算一次; 超窗或点位差太远就当两次独立单击 ——
 * 顺带挡掉"维修->卖->维修"这种快速三连的误判。 */
static int RA_DblHit(int checkPos)
{
    int fr = *(int*)ADDR_FRAME;
    int mx = 0, my = 0, dx, dy, d;
    if (checkPos && !GtMouseGameXY(&mx, &my)) { s_RaDblFrame = -1; return 0; }
    if (s_RaDblFrame >= 0)
    {
        d = fr - s_RaDblFrame;
        dx = mx - s_RaDblX;
        dy = my - s_RaDblY;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (d >= 1 && d <= g_RaDblFrames &&
            (!checkPos || (dx <= RA_DBL_SLOP && dy <= RA_DBL_SLOP)))
        {
            s_RaDblFrame = -1;
            return 1;
        }
    }
    s_RaDblFrame = fr;
    s_RaDblX = mx;
    s_RaDblY = my;
    return 0;
}

/* 触发判定。fromButton=1 只在侧栏维修图标那个钩子里为真。
 * 双击为什么只吞第二下: 吞第一下就没法在"其实是单击"时补一个维修光标了
 * —— 补它就得自己直调 MapClass::SetRepairMode 改模式状态, 铁律不许。于是双击
 * 的净效果 = 单击的维修光标切换(由第一下完成) + 一键全修。Ctrl+点击则是两下
 * 都吞 (否则会进维修光标又马上退掉, 还白白 UnselectAll 一次)。
 * 没按 Ctrl 又不是双击 -> 返回 0, 原版行为一点不动。 */
static unsigned RA_Trigger(int fromButton)
{
    int dbl = 0;
    if (!RA_On()) return 0;
    if (g_RaDbl)
    {
        if (fromButton)
            dbl = RA_DblHit(1);
        else if (s_RaKeyFresh)
        {
            s_RaKeyFresh = 0;         /* 这次按下算数, 往下只看间隔 */
            dbl = RA_DblHit(0);
        }
    }
    if (OU_KeyDown(VK_CONTROL)) { s_RaPending = 1; return 1; }
    if (dbl) { s_RaPending = 1; return 1; }
    return 0;
}

extern "C" __declspec(dllexport) unsigned __cdecl RepairAll_HotKey(void* regs)
{
    (void)regs;
    if (!RA_Trigger(0)) return 0;
    return ADDR_SKIP_HOTKEY;      /* 0x536FAC = ret 4, 跳过 SetRepairMode */
}

extern "C" __declspec(dllexport) unsigned __cdecl RepairAll_Button(void* regs)
{
    (void)regs;
    if (!RA_Trigger(1)) return 0;
    return ADDR_SKIP_BUTTON;      /* 0x6A78FF = 分支公共尾 jmp */
}

/* ---------------- 每循环轮询 (挂 PowerDrawBoth) ---------------- */

static int s_RaLastFrame = -1;
static int s_RaLastScan = -100000;

void RepairAll_Poll(void)
{
    int fr;
    if (!RA_On()) return;
    /* 0x4F42F0 一帧跑 ~142 次 (SWSBar 实测), 必须自门控帧号。 */
    fr = *(int*)ADDR_FRAME;
    if (fr == s_RaLastFrame) return;
    s_RaLastFrame = fr;
    /* 记维修快捷键是否松开过: 把"按住不放的自动重复"从双击里剔掉。 */
    if (!OU_KeyDown((unsigned)g_RaRepairVk)) s_RaKeyFresh = 1;
    RA_Observe();
    RA_Drain();
    if (s_RaPending)
    {
        s_RaPending = 0;
        RA_Scan();
        s_RaLastScan = fr;          /* 紧接着的自动扫描让这一帧 */
    }
    else if (g_RaMode == 2 &&
        (s_RaLastScan < 0 || fr - s_RaLastScan >= g_RaInterval))
    {
        s_RaLastScan = fr;
        RA_Scan();
    }
}
