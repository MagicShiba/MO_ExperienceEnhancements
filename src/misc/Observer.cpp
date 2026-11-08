/* Observer.cpp - 多观战修复 + 观战面板排序 + 读档后身份重建, 详见 notes/观战.md。
 * 多观战：Observer(0xAC1198)是全局单例，只留最后一个观战房；
 * 把本机 CurrentPlayer 对齐过去即可命中原版观战分支。
 * 读档：房数组整表重建, Observer 不会跟着恢复(变空/悬垂), 单机按本机房重设。
 * 面板排序：重填面板名单为人类在前、AI 在后，不含任何观战槽。
 * 联机安全：只写本机 CurrentPlayer 与纯 UI 面板名单; Observer 只在单机写,
 * 联机保持只读(各端选举结果一致, 改了会分叉)。
 */

#include "GdiText.h"
#include "EngineAddr.h"
#include "OrderUtil.h"
#include "Observer.h"

#define OB_SECTION "Features"

/* ---------------- 引擎地址 ---------------- */

#define ADDR_CURRENT      0xA83D4Cu  /* HouseClass::CurrentPlayer，本机量，可写 */
#define ADDR_OBSERVER     0xAC1198u  /* HouseClass::Observer，只读 */
#define ADDR_NODES_ITEMS  0xA8DA78u  /* 联机节点表 items */
#define ADDR_NODES_COUNT  0xA8DA84u  /* 联机节点表 count */
#define ADDR_PANEL_TABLE  0x884B94u  /* 面板名单 HouseClass* 表 */
#define ADDR_PANEL_COUNT  0x884CF8u  /* 面板名单计数 */
#define PANEL_MAX_SLOTS   64u        /* 房数上限，防越界 */

#define OFF_HOUSEINDICES  0x1180u    /* Scenario::HouseIndices[16] */
#define OFF_NODE_SPECT    0x6Bu      /* NodeNameType::SpectatorFlag，-1 为观战 */
#define OFF_NODE_HOUSEIDX 0x6Fu      /* NodeNameType::HouseIndex */

#define NODE_MAX 16

/* ---------------- 配置 ---------------- */

static int g_ObInit = 0;
static int g_ObEnable = 1;

static void OB_EnsureInit(void)
{
    if (g_ObInit) return;
    g_ObInit = 1;
    BuildPaths();
    g_ObEnable = GetPrivateProfileIntA(OB_SECTION, "Observer", 1, g_IniPath) ? 1 : 0;
}

/* 设置页运行期读写。 */
int Observer_Enabled(void)
{
    if (!g_ObInit) OB_EnsureInit();
    return g_ObEnable;
}

void Observer_SetEnabled(int on)
{
    if (!g_ObInit) OB_EnsureInit();
    g_ObEnable = on ? 1 : 0;
}

/* ---------------- 房属性判据 ---------------- */

/* 节点表可用时才用节点判据。 */
static int OB_NodesReady(void)
{
    return IsPlausiblePtr(*(void***)ADDR_NODES_ITEMS) &&
           *(int*)ADDR_NODES_COUNT > 0 && *(int*)ADDR_NODES_COUNT <= NODE_MAX;
}

/* 节点表中有观战节点指向本房。 */
static int OB_NodeClaimsHouse(void* house)
{
    void** items = *(void***)ADDR_NODES_ITEMS;
    int count = *(int*)ADDR_NODES_COUNT;
    int arrayIndex = *(int*)((const char*)house + OFF_HOUSE_INDEX);
    int i;

    if (!OB_NodesReady()) return 0;
    for (i = 0; i < count; i++)
    {
        void* n = items[i];
        if (!IsPlausiblePtr(n)) continue;
        if (*(int*)((const char*)n + OFF_NODE_SPECT) != -1) continue;
        if (*(int*)((const char*)n + OFF_NODE_HOUSEIDX) == arrayIndex) return 1;
    }
    return 0;
}

/* 本房无出生点（ArrayIndex 不在 Scenario::HouseIndices 内）。
 * 整表全 -1 = 未初始化（这张表是选房阶段的数据，读档不恢复），不能当"无出生点"的证据：
 * 否则读档进图后本机被判成观战者，所有"观战全可见 / 观战不参与对局"的分支一起误判。 */
static int OB_HouseHasNoSpawn(void* house)
{
    const int* idx;
    int arrayIndex, i, valid = 0;

    idx = (const int*)(*(const char**)ADDR_SCENARIO + OFF_HOUSEINDICES);
    if (!IsPlausiblePtr(idx)) return 0;
    arrayIndex = *(int*)((const char*)house + OFF_HOUSE_INDEX);
    if (arrayIndex < 0 || arrayIndex > 1024) return 0;
    for (i = 0; i < 16; i++)
    {
        if (idx[i] >= 0 && idx[i] <= 1024) valid++;
        if (idx[i] == arrayIndex) return 0;
    }
    return valid > 0 ? 1 : 0;
}

/* 是否任一观战槽（不只是全局真观战房）。 */
static int OB_IsAnyObserverSlot(void* house)
{
    return OB_NodeClaimsHouse(house) || OB_HouseHasNoSpawn(house);
}

/* 本机房是否为观战者。 */
static int OB_LocalHouseIsObserver(void* house)
{
    if (!*(unsigned char*)((const char*)house + OFF_IS_HUMAN)) return 0;
    return OB_IsAnyObserverSlot(house);
}

static int OB_IsNeutral(void* house)
{
    unsigned char* ht = *(unsigned char**)((const char*)house + OFF_HOUSE_TYPE);
    if (!IsPlausiblePtr(ht)) return 1;
    return *(unsigned char*)((const char*)ht + OFF_HTYPE_PASSIVE) == 1;
}

/* 房是否还在 HouseClass::Array 里。
 * 读档会整表重建房对象, 而 CurrentPlayer / Observer 是全局静态, 引擎不跟着改:
 * 读档后它们可能为空, 也可能指向已释放的旧房 (地址合法但内容已不属于本局)。
 * 那种指针一旦写回 CurrentPlayer, 引擎后面拿它取 PlanningPaths 之类就崩,
 * 所以凡是"写 CurrentPlayer / 认 Observer"之前都必须先过这一关。 */
static int OB_HouseInArray(void* house)
{
    void** items = *(void***)ADDR_HOUSES_ITEMS;
    int count = *(int*)ADDR_HOUSES_COUNT;
    int i;

    if (!IsPlausiblePtr(house)) return 0;
    if (!IsPlausiblePtr(items) || count <= 0 || count > 64) return 0;
    for (i = 0; i < count; i++)
        if (items[i] == house) return 1;
    return 0;
}

/* ---------------- 观战面板名单 ----------------
 * 排除中立 / 任何观战槽 / 野指针；先人类后 AI，组内保持数组原序。
 * 引擎每帧重算，本函数每帧重填，幂等。
 */
static void OB_FillPanelList(void)
{
    void** hitems = *(void***)ADDR_HOUSES_ITEMS;
    int hcount = *(int*)ADDR_HOUSES_COUNT;
    void** tbl = (void**)ADDR_PANEL_TABLE;
    int i, n = 0, pass;

    if (!IsPlausiblePtr(hitems) || hcount <= 0 || hcount > 64) return;

    for (pass = 0; pass < 2; pass++)
    {
        int wantHuman = (pass == 0);
        for (i = 0; i < hcount && n < (int)PANEL_MAX_SLOTS; i++)
        {
            void* h = hitems[i];
            if (!IsPlausiblePtr(h)) continue;
            if (*(unsigned char*)((const char*)h + OFF_IS_HUMAN) != wantHuman) continue;
            if (OB_IsNeutral(h)) continue;
            if (OB_IsAnyObserverSlot(h)) continue;
            tbl[n++] = h;
        }
    }
    *(int*)ADDR_PANEL_COUNT = n;
}

/* ---------------- 每循环 ---------------- */

void Observer_Poll(void)
{
    void *house, *observer;

    if (!g_ObInit) OB_EnsureInit();
    if (!g_ObEnable) return;

    house = *(void**)ADDR_CURRENT;
    if (!OB_HouseInArray(house)) return;      /* 读档瞬间 CurrentPlayer 被清 0 */
    observer = *(void**)ADDR_OBSERVER;

    /* 单机: 观战者只有本机, 观战房就是本机房 —— 读档后房数组重建, Observer 会
     * 变空或指向已释放的旧房, 这里一律按本机房重建, 三种状态一次覆盖。
     * 联机不碰: Observer 是各端跑同一套选举得到的全局值, 改它会跟其它端分叉;
     * 联机读档后 Observer 失效属于原版缺陷, 本模块不救。 */
    if (!OU_IsMP() && observer != house && OB_LocalHouseIsObserver(house))
    {
        *(void**)ADDR_OBSERVER = house;
        observer = house;
    }

    if (observer == house)   /* house 已确认在房表里 */
    {
        OB_FillPanelList();
        return;
    }

    /* 本机是观战者但没拿到真观战房: 对齐到全局房。observer 必须先确认还在
     * 房表里, 否则等于把 CurrentPlayer 写成野指针 (读档后必崩)。 */
    if (!OB_HouseInArray(observer)) return;
    if (!OB_LocalHouseIsObserver(house)) return;
    *(void**)ADDR_CURRENT = observer;
}

/* ---------------- 战败评估 (钩子 0x4FC3E6) ----------------
 * AcceptDefeat(0x4FC0B0) 末尾内联着 MPlayer_Defeated: 数一遍存活房, 再按
 * CurrentPlayer->Defeated(+0x1F5) 落 Flag_To_Lose / Flag_To_Win。
 * 观战房不参赛, Defeated 从开局就是 1, 于是任何 AI 出局都会给观战房落败旗,
 * 90 帧后整局收场; 正常局靠 IsGameOver(+0x1F6) 已置位早退躲过, 读档后这口
 * 状态不恢复, 早退失效 —— 这才是"读档后一有 AI 被击败就结束"的成因。
 * 站点 6 字节 (xor edi,edi / xor ebx,ebx / test eax,eax) 之后就是评估段,
 * 观战者直接跳到函数尾声 0x4FC6BC (pop edi/esi/ebp/ebx; add esp,0xB0; ret),
 * 不落任何胜负旗; 之前的战败房清理与"has been defeated"提示照常跑。
 * 细节与落旗链条见 notes/观战.md §8。 */
static int OB_DefeatGuardArmed(void)
{
    void *cp = *(void**)ADDR_CURRENT;
    void *ob = *(void**)ADDR_OBSERVER;

    if (OU_IsMP()) return 0;                  /* 联机各端必须一致收场, 不插手 */
    if (cp != ob) return 0;                   /* 引擎自己认我们是观战房 */
    return OB_HouseInArray(cp) && OB_LocalHouseIsObserver(cp);
}

/* 同一个战败房只记一次: 观战局被吞掉的结束判定要能在日志里看到。 */
static unsigned s_DefLogged = 0u;

static void OB_LogDefeatSkip(void)
{
    void* cp = *(void**)ADDR_CURRENT;
    unsigned sig = (unsigned)(size_t)cp;
    char buf[64];
    unsigned p;

    if (sig == s_DefLogged) return;
    s_DefLogged = sig;

    p = AppendCStr(buf, 0, "obs def skip: house=", sizeof(buf));
    p += HtoA((unsigned)(size_t)cp, buf + p);
    LogLine(buf);
}

extern "C" __declspec(dllexport) DWORD __cdecl Observer_DefeatEvalHook(void* regs)
{
    (void)regs;
    if (!OB_DefeatGuardArmed()) return 0;    /* 重放偷走的 6 字节 */
    OB_LogDefeatSkip();
    return 0x4FC6BCu;                        /* 函数尾声, 不落任何胜负旗 */
}

/* ---------------- 只读查询 ----------------
 * 关掉观战修复不等于本机变成玩家，所以不看 g_ObEnable。 */
int Observer_LocalIsSpectator(void)
{
    void *house = *(void**)ADDR_CURRENT;
    void *observer = *(void**)ADDR_OBSERVER;

    if (!IsPlausiblePtr(house)) return 0;
    if (IsPlausiblePtr(observer) && house == observer) return 1;
    return OB_LocalHouseIsObserver(house);
}
