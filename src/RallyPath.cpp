/* RallyPath.cpp - 工厂聚集点多段路径。
 *
 * 选中工厂后左键点空地设聚集点 (引擎自己画那条线), 按住 Z 继续点则追加尾巴。
 * 折线起点就是玩家设的聚集点格, 尾巴接着引擎那条线续画;
 * 按 S 清掉聚集点时 (引擎 ArchiveTarget 变空) 尾巴一并作废。尾巴用虚线, 与引擎风格一致。
 * 之后出厂的单位先到聚集点再沿尾巴走。左键重设聚集点时旧尾巴作废。
 *
 * 出厂瞬间只登记, 等 DelayFrames 逻辑帧后把整条折线当规划点一次下发, 节点挂上
 * token 后发一次 PlanCommit, 发完即释放追单槽位。
 *
 * 联机: 折线表只在本机, 画线端发节点/提交。PlanCommit 是房屋级事件, 提交前须确认
 * 无别人的待提交路径 (见 RP_ForeignPending)。追单绑定出厂单位 ID, 地址复用不串线。
 * 不存档 (换图/读档直接清)。引擎地址与结论见 notes/聚集点多段路径.md。
 */

#include <windows.h>
#include "GdiText.h"
#include "FullMap.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"
#include "RallyPath.h"

extern int g_Calibrated;

#define RP_SECTION "RallyPath"

/* 钩点由 Syringe 装 (声明见 SyringeHooks.cpp .syhks00 表, 与 .inj 同步),
 * 本文件只提供导出处理函数。 */

#define RP_GATE_SKIP    0x0044384Eu  /* CellClickedAction 函数尾: mov al,1 / ret 0x10 */

/* Syringe REGISTERS 布局见 YRpp Syringe.h, 落点格问 OU_MouseCell (见 RP_Gate)。 */
#define REG_OFF_EDI 0x08u
#define REG_OFF_ESI 0x0Cu

/* ---------------- 字段偏移 (gamemd 实测) ---------------- */

#define OFF_BLD_TYPE      0x520u   /* BuildingClass::Type */
#define OFF_BLD_FACTORY   0x21Cu   /* BuildingClass::Factory (空 = 不产东西) */
#define OFF_TYP_UNDEPLOY  0x16B9u  /* BuildingTypeClass::UndeploysInto */
#define VT_ISUNITFACTORY  0x284u   /* TechnoClass::IsUnitFactory() */
#define OFF_INLIMBO       0x81u    /* ObjectClass::InLimbo */
#define OFF_BLD_ARCHIVE   0x218u   /* TechnoClass::ArchiveTarget: 引擎画聚集点线读的目标
                                    * 指针, 无聚集点 = NULL。只判空, 绝不解引用 (脏值会崩) */

/* 规划点 token (与 PatrolCircle 同): Techno+0x514 = PlanningToken,
 * token+0x08 = 节点数组, token+0x14 = 节点数。 */
#define OFF_TECH_TOKEN 0x514u
#define OFF_TOK_NODES  0x08u
#define OFF_TOK_NCOUNT 0x14u
#define OFF_TOK_FLAG   0x1Cu  /* 提交中标记, 非 0 时提交空转 */
#define OFF_TOK_CURSOR 0x8Cu  /* 规划游标, 提交只推游标 == 0 的 token */
/* PlanningTokenClass::Array (DynamicVectorClass<PlanningTokenClass*> @0xAC4C78)。
 * Items@+0x4 / Capacity@+0x8 / Count@+0x10, 与 token+0x14 节点数同布局。 */
#define ADDR_TOKEN_ARRAY_ITEMS 0xAC4C7Cu
#define ADDR_TOKEN_ARRAY_COUNT 0xAC4C88u   /* Count, 不是 +0x8 的 Capacity */
#define ADDR_PLANNING_MODE    0xAC4CF4u /* PlanningNodeClass::PlanningModeActive */

/* 私有引擎入口 (只读 + 标准事件, 不直接改模拟状态)。 */
typedef void* __attribute__((thiscall)) (*Fn_TargetFromCell)(void* tgt,
                                                             const CellStruct* cell);
typedef int __attribute__((thiscall)) (*Fn_IsUnitFactory)(void* self);

#define FN_TGT_CELL   ((Fn_TargetFromCell)(void*)0x6E6B20u)
typedef void* __attribute__((thiscall)) (*Fn_PlanCommitCtor)(void* ev,
                                                            int houseIdx, int evtType);
#define FN_PLANCOMMIT ((Fn_PlanCommitCtor)(void*)0x4C66C0u)
#define EVT_PLANCOMMIT 0x2Bu   /* EventType::PlanCommit (YRpp GeneralDefinitions.h) */

/* ---------------- 私有常量 ---------------- */

#define ADDR_SCENARIO  0xA8B230u  /* ScenarioClass::Instance (换图代号) */
#define ADDR_FRAME     0xA8ED84u  /* Unsorted::CurrentFrame = 逻辑帧号 */

#define RP_TAB_CAP   128   /* 折线表上限 (静态数组) */
#define RP_NODE_MAX  16    /* 单条折线段数上限 (ini MaxNodes 2~16) */
#define RP_WALK_CAP  32    /* 同时追的单位数 (出厂潮多时后面的不登记) */
/* 出厂后发节点前的等待 (逻辑帧): 游戏逻辑 30 帧/游戏秒, 60 帧 = 2 秒。
 * 读 0xA8ED84 逻辑帧号, 不数轮询次数 (Poll 一帧会被调多次)。 */
#define RP_EXIT_DELAY_FRM 60
static int g_RpDelayFrames = RP_EXIT_DELAY_FRM;
#define RP_LIMBO_MAX 300   /* 连续在厂内多少逻辑帧 (~5s) 才放弃 */
#define RP_COMMIT_WAIT 300 /* 等节点挂上 token 的上限 (~5s), 超了也照发提交 */

/* ---------------- 配置与钩子状态 ---------------- */

static int g_RpInit = 0;
static int g_RpEnable = 1;
static int g_RpMaxNodes = 12;

/* ---------------- 折线表 (本机旁挂, 不存档) ---------------- */

typedef struct
{
    void* bld;                   /* 建筑指针 */
    void* vt;                    /* 抓不到 Fetch_ID 时用它判断建筑还在不在 */
    int   id;                    /* Fetch_ID (跨帧验活, 见 OU_PtrAlive) */
    short x[RP_NODE_MAX];
    short y[RP_NODE_MAX];
    int   n;                     /* 尾巴段数 (0 = 只有起点, 没有尾巴) */
    short sx, sy;                /* 第一段起点 = 玩家左键设的聚集点格, -1 = 没设过 */
    int   had;                   /* 上一帧引擎聚集点是否存在 (由有到无 = 被清, 线作废) */
} RPPath;

static RPPath g_RpTab[RP_TAB_CAP];
static int    g_RpTabCount = 0;

/* 待追单位 (出厂瞬间拷一份折线, 见 RP_WalkPoll)。
 * mode 0=等延迟后发节点 1=等节点挂上 token 再提交 (提交完立即释放槽位)。 */
typedef struct
{
    void* unit;
    void* bld;                        /* 最后一次登记它的出厂建筑 (引擎可能改派, 见 RP_Exit) */
    int   uid;                        /* 出厂瞬间抓的 Fetch_ID (防槽位复用串线) */
    int   idxOk;                      /* 抓到 ID 才严格验活, 否则 fail-open */
    short x[RP_NODE_MAX + 1];       /* 节点 0 = 原生聚集点, 1.. = 我们记的尾巴 */
    short y[RP_NODE_MAX + 1];
    int   n;                        /* 节点总数 */
    int   mode;
    int   sent;                     /* 已发出的节点数 */
    unsigned wait;                  /* 当前阶段已等逻辑帧数 */
    int   fr0;                      /* 出厂那一帧的逻辑帧号 (延迟从它起算) */
    unsigned age;                   /* 出厂后已过的逻辑帧数 */
    unsigned limbo;                 /* 连续在厂内的逻辑帧数 */
} RPWalk;

static RPWalk g_RpWalk[RP_WALK_CAP];
static int    g_RpWalkCount = 0;
static void*  g_RpHouse = NULL; /* 入队瞬间 CurrentPlayer (换房清空) */
static int    g_RpLastFrame = -1; /* 上次处理过的逻辑帧 (一帧只跑一遍) */
static int    g_RpSyncFrame = -1; /* 上次同步聚集点的逻辑帧 (调引擎虚表, 一帧一次) */
static int    g_RpZRecent = 0;    /* Z 宽限剩余逻辑帧数: 画线期间为正, 钩子漏读 Z 也不误清 */
static int    g_RpZLastFrame = -1; /* Z 宽限计时的帧门 (Poll 一帧被调上百次) */

/* ---------------- 小工具 ---------------- */

static int RP_IsFoot(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_AIRCRAFT || w == ABS_INFANTRY) ? 1 : 0;
}

/* ---------------- 建筑类型识别 ----------------
 * 只支持 ini `[RallyPath] Types=` 白名单 (逗号分隔, 留空不限), 默认取自 mod rules
 * 的战车工厂/兵营/船厂主建筑; 结尾 B 的附属版与船厂 *ARDAI 版不列, 基地不参与。
 * ID 在 `BuildingTypeClass + 0x24`, 为内联 char[0x18] (非指针)。 */
#define OFF_TYP_ID 0x24u
#define RP_TYPES_MAX 64

static char g_RpTypes[RP_TYPES_MAX][24];   /* ini Types= 解析结果 */
static int  g_RpTypesCount = 0;

/* 建筑 ID 是不是像样: 3~23 个大写字母/数字/下划线。 */
static int RP_IdOk(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (n < 24)
    {
        char c = s[n];
        if (!c) break;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return 0;
        n++;
    }
    return n >= 3 && n < 24;
}

/* 读 ini Types= (逗号/分号/空格分隔), 存进 g_RpTypes[]。读不到就留空 = 不限类型。 */
static void RP_LoadTypes(void)
{
    char raw[1024];
    int i = 0;
    g_RpTypesCount = 0;
    raw[0] = 0;
    GetPrivateProfileStringA(RP_SECTION, "Types", "", raw, (DWORD)sizeof(raw), g_IniPath);
    while (raw[i] && g_RpTypesCount < RP_TYPES_MAX)
    {
        int k = 0;
        while (raw[i] == ',' || raw[i] == ';' || raw[i] == ' ' || raw[i] == '\t') i++;
        if (!raw[i]) break;
        while (raw[i] && raw[i] != ',' && raw[i] != ';' && raw[i] != ' ' &&
               raw[i] != '\t' && k < 23)
            g_RpTypes[g_RpTypesCount][k++] = raw[i++];
        g_RpTypes[g_RpTypesCount][k] = 0;
        if (k > 0 && RP_IdOk(g_RpTypes[g_RpTypesCount])) g_RpTypesCount++;
    }
}

static int RP_TypeAllowed(const char* id)
{
    int i;
    if (g_RpTypesCount <= 0) return 1;                 /* 没配就不限 */
    if (!id || !RP_IdOk(id)) return 0;
    for (i = 0; i < g_RpTypesCount; i++)
    {
        const char* a = id;
        const char* b = g_RpTypes[i];
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) return 1;
    }
    return 0;
}

/* 兵营/战车工厂/船厂: 是建筑 + 有 Factory + 非反部署建筑
 * (UndeploysInto 的聚集点是反部署落点, 语义不同) + ID 在白名单内。 */
static int RP_IsFactory(void* b)
{
    void** vt;
    unsigned char* type;
    Fn_IsUnitFactory fn;
    if (!b || !IsPlausiblePtr(b)) return 0;
    vt = *(void***)b;
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    if (OU_WhatAmI(b) != ABS_BUILDING) return 0;
    if (!*(void**)((char*)b + OFF_BLD_FACTORY)) return 0;          /* 空 = 不产东西 */
    type = *(unsigned char**)((char*)b + OFF_BLD_TYPE);
    if (!type || !IsPlausiblePtr(type)) return 0;
    if (*(unsigned char*)(type + OFF_TYP_UNDEPLOY)) return 0;      /* 反部署建筑 */
    fn = (Fn_IsUnitFactory)vt[VT_ISUNITFACTORY / 4];
    if (!fn || !IsPlausiblePtr((const void*)fn)) return 0;
    if (!fn(b)) return 0;
    return RP_TypeAllowed((const char*)type + OFF_TYP_ID);
}

/* 引擎当前有没有聚集点: 只读 ArchiveTarget 是否为 NULL, 不解引用。 */
static int RP_HasRally(void* bld)
{
    void* t;
    if (!bld || !IsPlausiblePtr(bld)) return 0;
    if (OU_WhatAmI(bld) != ABS_BUILDING) return 0;
    t = *(void**)((char*)bld + OFF_BLD_ARCHIVE);
    return (t && IsPlausiblePtr(t)) ? 1 : 0;
}

/* ArchiveTarget 失效时可能是脏指针, 只判空、不解引用。聚集点线仍由引擎自己画。 */

/* 建筑还活着吗: 首选 Fetch_ID, 抓不到则比虚表指针。
 * 皆无时当活着 (fail-open), 宁可条目多活也不误删正在画的折线。 */
static int RP_AliveAt(const RPPath* p)
{
    if (!p) return 1;
    if (p->id > 0) return OU_PtrAlive(p->bld, p->id, 1);
    return *(void**)p->bld == p->vt;
}

static RPPath* RP_Find(void* bld)
{
    int i;
    for (i = 0; i < g_RpTabCount; i++)
        if (g_RpTab[i].bld == bld) return &g_RpTab[i];
    return 0;
}

static void RP_ShiftTabDown(int i)
{
    for (; i + 1 < g_RpTabCount; i++) g_RpTab[i] = g_RpTab[i + 1];
    g_RpTabCount--;
}

static void RP_DropPath(void* bld)
{
    RPPath* p = RP_Find(bld);
    if (p) RP_ShiftTabDown((int)(p - g_RpTab));
}

static void RP_ClearAll(void)
{
    g_RpTabCount = 0;
    g_RpWalkCount = 0;
    g_RpHouse = 0;
    g_RpLastFrame = -1;
    g_RpSyncFrame = -1;
}

/* 每轮清掉建筑已没的表项 (卖/摧毁/被占领后地址可能被复用)。 */
static void RP_DropDead(void)
{
    int i = 0;
    while (i < g_RpTabCount)
    {
        if (RP_AliveAt(&g_RpTab[i])) { i++; continue; }
        RP_ShiftTabDown(i);
    }
}

/* 每逻辑帧同步一次引擎聚集点有无 (一帧一次)。
 * 只读 ArchiveTarget 是否为 NULL, 不解引用。
 * 有则记 had=1; 由有变无 (按 S 清聚集点 / 点建筑自身取消) 则折线作废。 */
static void RP_SyncRally(void)
{
    int fr, i = 0;
    if (g_RpTabCount <= 0) return;
    fr = *(int*)ADDR_FRAME;
    if (fr == g_RpSyncFrame) return;
    g_RpSyncFrame = fr;
    while (i < g_RpTabCount)
    {
        RPPath* p = &g_RpTab[i];
        if (!RP_AliveAt(p)) { RP_ShiftTabDown(i); continue; }
        if (RP_HasRally(p->bld))
        {
            p->had = 1;
        }
        else if (p->had)
        {
            RP_ShiftTabDown(i);            /* 聚集点没了 -> 折线清掉 */
            continue;
        }
        i++;
    }
}

static void RP_EnsureInit(void)
{
    if (g_RpInit) return;
    g_RpInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_RpEnable = GetPrivateProfileIntA(RP_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    g_RpMaxNodes = GetPrivateProfileIntA(RP_SECTION, "MaxNodes", 12, g_IniPath);
    if (g_RpMaxNodes < 2) g_RpMaxNodes = 2;
    if (g_RpMaxNodes > RP_NODE_MAX) g_RpMaxNodes = RP_NODE_MAX;
    g_RpDelayFrames = GetPrivateProfileIntA(RP_SECTION, "DelayFrames",
                                          RP_EXIT_DELAY_FRM, g_IniPath);
    if (g_RpDelayFrames < 0) g_RpDelayFrames = 0;
    if (g_RpDelayFrames > 1800) g_RpDelayFrames = 1800;
    RP_LoadTypes();
    OU_LogTag("rallypath: init", g_RpEnable ? "ON (Z + rally polyline)" : "OFF");
}

/* ---------------- 折线记点 (只改本机内存) ---------------- */

/* 取表项, 没有就新建。 */
static RPPath* RP_NewPath(void* bld)
{
    RPPath* p = RP_Find(bld);
    char tb[8] = { 0 };
    if (p) return p;
    RP_EnsureInit();
    if (g_RpTabCount >= RP_TAB_CAP) return 0;
    p = &g_RpTab[g_RpTabCount++];
    p->bld = bld;
    p->vt = *(void**)bld;
    p->id = FN_TGT_OBJ(tb, bld) ? *(int*)(tb + 0) : 0;
    p->n = 0;
    p->sx = -1;
    p->sy = -1;
    p->had = 0;
    return p;
}

/* 只追加尾巴点, x/y 数组只装起点之后的尾巴。 */
static int RP_Push(void* bld, int cx, int cy)
{
    RPPath* p = RP_NewPath(bld);
    if (!p) return 0;
    if (p->n > 0 && p->x[p->n - 1] == (short)cx && p->y[p->n - 1] == (short)cy)
        return 1;                                 /* 与上一段重合, 幂等 */
    if (p->n >= g_RpMaxNodes) return 0;           /* 满了静默丢 */
    p->x[p->n] = (short)cx;
    p->y[p->n] = (short)cy;
    p->n++;
    return 1;
}

/* 玩家左键重设聚集点 → 记该格为第一段起点, 旧尾巴作废。
 * 点回建筑自身 = 原版取消聚集点, 起点尾巴一起清。 */
static void RP_SetRally(void* bld, int cx, int cy)
{
    RPPath* p;
    CoordStruct bc;
    bc.X = bc.Y = bc.Z = 0;
    if (OU_GetCoords(bld, &bc) && cx == bc.X / 256 && cy == bc.Y / 256)
    {
        RP_DropPath(bld);
        return;
    }
    p = RP_NewPath(bld);
    if (!p) return;
    p->n = 0;
    p->sx = (short)cx;
    p->sy = (short)cy;
}

/* ---------------- 钩 1: 聚集点门 (0x44377E) ----------------
 * 回 1 吞掉原生聚集点, 回 0 放行原版。入口 ESI = 被调建筑,
 * 落点格问 OU_MouseCell (与引擎同条 ProcessClickCoords 路径)。 */
static int RP_Selected(void* bld)
{
    void** items = *(void***)ADDR_SELOBJ_ITEMS;
    int selN = *(int*)ADDR_SELOBJ_COUNT;
    int i;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return 0;
    for (i = 0; i < selN; i++)
        if (items[i] == bld) return 1;
    return 0;
}

static int RP_Gate(void* bld)
{
    int cx = -1, cy = -1;
    RP_EnsureInit();
    if (!g_RpEnable) return 0;
    if (!bld || !IsPlausiblePtr(bld)) return 0;
    /* 只认选中的工厂: 单发调用 (AI/触发器等) 传未选中建筑, 不记也不清。 */
    if (!RP_Selected(bld)) return 0;
    /* 普通左键 = 玩家改聚集点, 只记起点并清旧尾巴。
     * 画线期 (g_RpZRecent > 0) 内不清尾巴, 防钩子漏读 Z 误删。 */
    if (!OU_KeyDown('Z'))
    {
        if (g_RpZRecent <= 0 && RP_IsFactory(bld) && OU_MouseCell(&cx, &cy))
            RP_SetRally(bld, cx, cy);
        return 0;
    }
    if (!OU_MouseCell(&cx, &cy)) return 0;
    if (!RP_IsFactory(bld)) return 0;
    if (cx < 0 || cy < 0 || !OU_CellValid(cx, cy)) return 0;
    return RP_Push(bld, cx, cy);
}

/* Syringe 钩: 返回 0 = 走跳板 (原指令), 返回非 0 = 直接跳该地址。 */
extern "C" __declspec(dllexport) unsigned __cdecl RallyPath_GateHook(void* regs)
{
    DWORD* r = (DWORD*)regs;
    if (!r) return 0;
    if (!RP_Gate((void*)r[REG_OFF_ESI / 4u])) return 0;
    return RP_GATE_SKIP;
}

/* ---------------- 取色 ----------------
 * HouseClass::CurrentPlayer + 0x16054 即配色下标, 取
 * ColorScheme::Array[下标].Colors[MainShadeIndex] 即玩家色。
 * Colors @+0x04, MainShadeIndex @+0x330; 取不到回黄。 */
#define ADDR_HOUSE_CUR     0xA83D4Cu  /* HouseClass::CurrentPlayer */
#define OFF_HOUSE_SCHEME   0x16054u   /* HouseClass::ColorSchemeIndex */
#define ADDR_SCHEME_ITEMS  0xB054D4u  /* ColorScheme::Array.items */
#define ADDR_SCHEME_COUNT  0xB054D8u
#define OFF_SCHEME_PAL     0x04u     /* ColorScheme::Colors[256] */
#define OFF_SCHEME_MAIN    0x330u    /* ColorScheme::MainShadeIndex */

static int RP_HouseColor(int* r, int* g, int* b)
{
    void* house = *(void**)ADDR_HOUSE_CUR;
    void** items;
    int cnt, idx, main;
    unsigned char* sc;
    if (!house || !IsPlausiblePtr(house)) return 0;
    idx = *(int*)((char*)house + OFF_HOUSE_SCHEME);
    items = *(void***)ADDR_SCHEME_ITEMS;
    cnt = *(int*)ADDR_SCHEME_COUNT;
    if (!items || !IsPlausiblePtr(items) || cnt <= 0 || idx < 0 || idx >= cnt) return 0;
    sc = (unsigned char*)items[idx];
    if (!sc || !IsPlausiblePtr(sc)) return 0;
    main = *(int*)(sc + OFF_SCHEME_MAIN);
    if (main < 0 || main > 255) return 0;
    *r = sc[OFF_SCHEME_PAL + main * 3 + 0];
    *g = sc[OFF_SCHEME_PAL + main * 3 + 1];
    *b = sc[OFF_SCHEME_PAL + main * 3 + 2];
    return 1;
}

/* ---------------- 输入采集 (Poll 侧) ----------------
 * 按住 Z 时原版左键可被规划门吞掉, 故按下沿自己抓一遍。
 * 与钩子同次点击会走重 RP_Push, 由末段重合幂等去重。 */
static int s_RpPrevLmb = 0;

static void RP_CaptureClick(void)
{
    void** items;
    RectangleStruct viewA;
    int selN, i, cx, cy, px, py;
    FM_GetViewRects(&viewA, 0);
    if (!OU_MouseCellPx(&cx, &cy, &px, &py)) return;
    /* 点须在地图视口内 (侧栏/底栏不记)。 */
    if (viewA.Width > 32 && viewA.Height > 32 &&
        (px < viewA.X || py < viewA.Y ||
         px >= viewA.X + viewA.Width || py >= viewA.Y + viewA.Height))
        return;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return;
    for (i = 0; i < selN; i++)
        if (RP_IsFactory(items[i])) RP_Push(items[i], cx, cy);
}

/* ---------------- 钩 2: 出厂 (0x443C7D) ----------------
 * BuildingClass::ExitObject (0x443C60, 虚表第 64 项) 内 WhatAmI 分派前,
 * ESI = 建筑、EDI = 单位。出口格被占时引擎每帧重试, 登记须按单位去重。 */

static void RP_Exit(void* bld, void* unit)
{
    RPPath* src;
    RPWalk* w;
    char tb[8] = { 0 };
    int i, slot = -1;
    RP_EnsureInit();
    if (!g_RpEnable) return;
    if (!bld || !unit) return;
    if (!RP_IsFoot(unit)) return;
    src = RP_Find(bld);
    if (!src || src->n < 1) return;
    if (!RP_AliveAt(src)) return;
    /* 引擎改派出厂建筑时会递归回调本函数, 真正出厂的是最后一次。
     * 故同一单位以最后一次的 bld 为准, 同建筑重试时保持原记录。 */
    for (i = 0; i < g_RpWalkCount; i++)
        if (g_RpWalk[i].unit == unit) { slot = i; break; }
    if (slot >= 0)
    {
        if (g_RpWalk[slot].bld == bld) return;   /* 同建筑重试, 保持原记录 */
        w = &g_RpWalk[slot];
    }
    else
    {
        if (g_RpWalkCount >= RP_WALK_CAP) return;
        w = &g_RpWalk[g_RpWalkCount++];
        w->mode = 0;                             /* 0 = 待发节点 */
        w->sent = 0;
        w->wait = 0;
        w->fr0 = *(int*)ADDR_FRAME;
        w->age = 0;
        w->limbo = 0;
    }
    w->unit = unit;
    w->bld = bld;
    /* 绑定单位 ID: 槽位被复用时验活失败即丢, 新单位不继承旧折线。
     * 抓不到 ID 则 fail-open。 */
    if (FN_TGT_OBJ(tb, unit)) { w->uid = *(int*)(tb + 0); w->idxOk = 1; }
    else { w->uid = 0; w->idxOk = 0; }
    w->n = 0;
    if (src->sx >= 0 && OU_CellValid(src->sx, src->sy))
    { w->x[0] = src->sx; w->y[0] = src->sy; w->n = 1; }
    for (i = 0; i < src->n && w->n < RP_NODE_MAX + 1; i++)
    {
        if (w->n > 0 && src->x[i] == w->x[w->n - 1] && src->y[i] == w->y[w->n - 1])
            continue;                                    /* 与上一节点重合, 去掉 */
        w->x[w->n] = src->x[i];
        w->y[w->n] = src->y[i];
        w->n++;
    }
    if (w->n < 1) { if (slot < 0) g_RpWalkCount--; return; }
}

/* 被偷字节只读 EDI, 返回后不变, 无须还原。钩点见 notes。 */
extern "C" __declspec(dllexport) unsigned __cdecl RallyPath_ExitHook(void* regs)
{
    DWORD* r = (DWORD*)regs;
    if (!r) return 0;
    RP_Exit((void*)r[REG_OFF_ESI / 4u], (void*)r[REG_OFF_EDI / 4u]);
    return 0;                                    /* 走跳板 = 原指令 */
}

/* ---------------- 出厂追单: 规划点 + 提交 (两阶段) ----------------
 * 事件形状 (抄 PatrolCircle 原生抓包): 每点每单位一条 MegaMission,
 *   Mission=Move(2), Target=空, Destination=格, Follow=同格, IsPlanningEvent=1。
 * 最后补一条 PlanCommit (Type 0x2B)。
 *
 * 须两阶段: 规划点先进队列 (主循环每帧只搬 2 条), PlanCommit 直进 DoList,
 * 同批齐发则提交先落地而被按无节点丢弃。故先发全节点 → 确认挂上 token → 再发提交。
 * 不逐段推进, 发完即交引擎自己走, 随即释放槽位。 */

static int RP_FireNode(int houseIdx, void* unit, int uid, int idxOk, int cx, int cy)
{
    char ev[EVT_SIZE], srcB[8], dstB[8];
    CellStruct cs;
    unsigned i;
    int srcID, dstID;
    unsigned char srcR, dstR;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    for (i = 0; i < 8; i++) srcB[i] = dstB[i] = 0;
    cs.X = (short)cx; cs.Y = (short)cy;
    FN_TGT_CELL(dstB, &cs);
    if (!FN_TGT_OBJ(srcB, unit)) return 0;
    srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
    dstID = *(int*)(dstB + 0); dstR = (unsigned char)dstB[4];
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcR, MISSION_MOVE,
            0, 0, dstID, (unsigned)dstR, dstID, (unsigned)dstR);
    ev[OFF_PLANFLAG] = 1;                 /* 规划点 */
    return OT_Fire(ev, unit, uid, idxOk) ? 1 : 0;
}

static int RP_FireCommit(int houseIdx)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_PLANCOMMIT(ev, houseIdx, EVT_PLANCOMMIT);
    return OU_Enqueue(ev);
}

static void RP_WalkShiftDown(int i)
{
    for (; i + 1 < g_RpWalkCount; i++) g_RpWalk[i] = g_RpWalk[i + 1];
    g_RpWalkCount--;
}

/* 单位 PlanningToken 上的节点数 (只读; token 不存在/野指针回 0)。 */
static int RP_TokenNodes(void* unit)
{
    void* t;
    void** arr;
    int n;
    if (!unit || !IsPlausiblePtr(unit)) return 0;
    t = *(void**)((char*)unit + OFF_TECH_TOKEN);
    if (!t || !IsPlausiblePtr(t)) return 0;
    arr = *(void***)((char*)t + OFF_TOK_NODES);
    n = *(int*)((char*)t + OFF_TOK_NCOUNT);
    if (!arr || !IsPlausiblePtr(arr) || n < 0 || n > 1000) return 0;
    return n;
}

/* ---------------- 别抢玩家的规划 (只读探测) ----------------
 * PlanCommit 是房屋级事件: 我方提交会连带启动玩家画到一半的路径。
 * 故玩家有待提交路径时一直压着不发 (不设超时)。只读扫规划开关与 token 数组。 */

/* token 地址是否属于本模块在跟的单位 (是我们自己刚发的那批)。 */
static int RP_TokenIsOurs(void* tok)
{
    int i;
    for (i = 0; i < g_RpWalkCount; i++)
    {
        void* u = g_RpWalk[i].unit;
        if (!u || !IsPlausiblePtr(u)) continue;
        if (*(void**)((char*)u + OFF_TECH_TOKEN) == tok) return 1;
    }
    return 0;
}

/* 回 1 = 还有别人的待提交路径 (或玩家正在画), 这一帧不发提交。
 * 只把"提交真会触发"的 token 算数: count>0 且游标==0 且提交标记==0。
 * 已在走/已提交过的 token 会被 PlanCommit 跳过, 不构成威胁。 */
static int RP_ForeignPending(void)
{
    void** items;
    int cnt, i;
    if (*(unsigned char*)ADDR_PLANNING_MODE) return 1;   /* 玩家正在画路径 */
    items = *(void***)ADDR_TOKEN_ARRAY_ITEMS;
    cnt = *(int*)ADDR_TOKEN_ARRAY_COUNT;
    if (!items || !IsPlausiblePtr(items) || cnt <= 0 || cnt > 4096) return 0;
    for (i = 0; i < cnt; i++)
    {
        void* tok = items[i];
        int n, cur, flg;
        if (!tok || !IsPlausiblePtr(tok)) continue;
        n = *(int*)((char*)tok + OFF_TOK_NCOUNT);
        if (n <= 0 || n > 1000) continue;
        cur = *(int*)((char*)tok + OFF_TOK_CURSOR);
        flg = *(int*)((char*)tok + OFF_TOK_FLAG);   /* 字节段, 取 int 只为 !=0 判定 */
        if (cur != 0 || flg != 0) continue;         /* 引擎的提交不会推它 */
        if (!RP_TokenIsOurs(tok)) return 1;
    }
    return 0;
}

static void RP_WalkPoll(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx, i, fr;
    if (g_RpWalkCount <= 0) return;
    if (!cur || !IsPlausiblePtr(cur)) return;
    if (g_RpHouse == NULL) g_RpHouse = cur;
    if (cur != g_RpHouse)
    {
        g_RpHouse = cur;
        g_RpWalkCount = 0;
        return;
    }
    /* 一逻辑帧只跑一遍: Poll 一帧可被调多次, 不挡则 age/wait 缩水。 */
    fr = *(int*)ADDR_FRAME;
    if (fr == g_RpLastFrame) return;
    g_RpLastFrame = fr;
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    i = 0;
    while (i < g_RpWalkCount)
    {
        RPWalk* w = &g_RpWalk[i];
        int tn, k;
        /* 指针复用即串线: 只认登记时的单位, ID 对不上就丢 (fail-open 仅限抓不到 ID)。 */
        if (!w->unit || !IsPlausiblePtr(w->unit) || !RP_IsFoot(w->unit) ||
            !OU_PtrAlive(w->unit, w->uid, w->idxOk))
        {
            RP_WalkShiftDown(i);
            continue;
        }
        w->age = (unsigned)(fr - w->fr0);
        if (((unsigned char*)w->unit)[OFF_INLIMBO])
        {
            /* 还在厂里 (载具须慢慢挪出): 等, 不丢。
             * 延迟从真正离厂起算: 在厂期间把计时基准推到当前帧,
             * 否则载具仍压在出口格, 首点不可达会整条作废。 */
            w->limbo++;
            w->fr0 = fr;
            if (w->limbo > RP_LIMBO_MAX) { RP_WalkShiftDown(i); continue; }
            i++;
            continue;
        }
        w->limbo = 0;
        if (w->mode == 0)
        {
            /* 阶段 1: 等满 DelayFrames 逻辑帧。 */
            if (w->age < (unsigned)g_RpDelayFrames)
            {
                i++;
                continue;
            }
            w->mode = 1;
            w->sent = 0;
            for (k = 0; k < w->n; k++)
            {
                if (RP_FireNode(houseIdx, w->unit, w->uid, w->idxOk,
                                w->x[k], w->y[k])) w->sent++;
                else break;
            }
            w->wait = 0;
            i++;
        }
        else
        {
            /* 阶段 2: 等节点挂上 token 再提交。玩家有待提交路径时一直压着不发。 */
            tn = RP_TokenNodes(w->unit);
            if (tn < w->sent && w->wait < RP_COMMIT_WAIT)
            {
                w->wait++;
                i++;
                continue;
            }
            if (RP_ForeignPending()) { w->wait++; i++; continue; }
            RP_FireCommit(houseIdx);
            RP_WalkShiftDown(i);
        }
    }
}

/* ---------------- 每循环轮询 (挂 PowerDrawBoth) ---------------- */

void RallyPath_Poll(void)
{
    static void* s_LastScen = NULL;
    void* scen;
    int zDown, lmb;

    RP_EnsureInit();
    scen = *(void**)ADDR_SCENARIO;
    if (scen != s_LastScen)
    {
        s_LastScen = scen;
        RP_ClearAll();
    }
    if (!g_RpEnable) { s_RpPrevLmb = 0; return; }
    RP_DropDead();
    RP_SyncRally();
    RP_WalkPoll();
    /* Z 宽限: 按住 Z 期间保持 15 逻辑帧, 钩子漏读一次也不误清尾巴。
     * Poll 一帧被调多次, 宽限按逻辑帧递减。 */
    zDown = OU_KeyDown('Z');
    {
        int fr = *(int*)ADDR_FRAME;
        if (fr != g_RpZLastFrame)
        {
            g_RpZLastFrame = fr;
            if (zDown) g_RpZRecent = 15;
            else if (g_RpZRecent > 0) g_RpZRecent--;
        }
    }
    /* Z + 左键按下沿 = 记一段 (原生派发被规划门吞掉时靠这条路)。 */
    lmb = OU_KeyDown(VK_LBUTTON);
    if (zDown && lmb && !s_RpPrevLmb) RP_CaptureClick();
    s_RpPrevLmb = lmb;
}

/* ---------------- 渲染期折线 (只读 + 渲染面写入) ---------------- */

typedef void* (__fastcall *RP_LockFn)(void*, void*, int, int);
typedef int (__fastcall *RP_UnlockFn)(void*, void*);

#define VT_RP_LOCK   0x5C
#define VT_RP_UNLOCK 0x60

/* 格 → 世界坐标 (格心 + 地面高度)。 */
static int RP_CellWorld(int cx, int cy, int* lx, int* ly, int* lz)
{
    int z = 0;
    if (!OU_CellGroundZ(cx, cy, &z)) return 0;
    *lx = cx * 256 + 128;
    *ly = cy * 256 + 128;
    *lz = z;
    return 1;
}

void RallyPath_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* vt;
    RP_LockFn lockFn;
    RP_UnlockFn unlockFn;
    unsigned char* locked;
    void** items;
    OU_PrevLine buf[OU_PREV_CAP]; /* 多选工厂每家一条线, 按全表上限给 */
    int selN, i, k, n;
    int zDown;

    if (!g_RpEnable) return;
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return;
    zDown = OU_KeyDown('Z');

    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    lockFn = (RP_LockFn)(*(void**)(vt + VT_RP_LOCK));
    unlockFn = (RP_UnlockFn)(*(void**)(vt + VT_RP_UNLOCK));
    if (!lockFn || !unlockFn) return;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return;

    n = 0;
    for (i = 0; i < selN; i++)
    {
        RPPath* p;
        CoordStruct bc;
        void* bld = items[i];
        int px = 0, py = 0, pz = 0, ax, ay, haveStart = 0;
        if (!RP_IsFactory(bld)) continue;
        p = RP_Find(bld);
        if (!p || p->n <= 0) continue;
        /* 起点: 设过聚集点就从它起笔 (建筑→聚集点由引擎自己画);
         * 没设过则从建筑本体起笔, 补上"建筑→首段"这一段 —— 否则整条线像从
         * 半空起笔, 多选时看上去就与建筑/其他线断开。 */
        if (p->sx >= 0 && OU_CellValid(p->sx, p->sy) &&
            RP_CellWorld(p->sx, p->sy, &px, &py, &pz))
            haveStart = 1;
        else if (OU_GetCoords(bld, &bc))
        {
            px = bc.X; py = bc.Y; pz = bc.Z;
            haveStart = 1;
        }
        for (k = 0; k < p->n && n < OU_PREV_CAP; k++)
        {
            int lz = 0;
            if (!RP_CellWorld(p->x[k], p->y[k], &ax, &ay, &lz)) break;
            if (!haveStart) { px = ax; py = ay; pz = lz; haveStart = 1; continue; }
            if (ax == px && ay == py) continue;         /* 与上一点重合 */
            OU_PrevPush(buf, &n, px, py, pz, ax, ay, lz);
            px = ax; py = ay; pz = lz;
        }
        if (zDown && haveStart && n < OU_PREV_CAP)
        {
            int mx, my, lx, ly, lz = 0;
            if (OU_MouseCell(&mx, &my) && RP_CellWorld(mx, my, &lx, &ly, &lz))
                OU_PrevPush(buf, &n, px, py, pz, lx, ly, lz);
        }
    }
    if (n > 0)
    {
        int cr, cg, cb;
        if (!RP_HouseColor(&cr, &cg, &cb)) { cr = 255; cg = 255; cb = 0; }
        OU_PrevDrawDash(buf, n, locked, w, h, pitch, cr, cg, cb, 220, 2, 6, 4);
    }
    unlockFn(surf, NULL);
}
