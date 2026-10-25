/* Observer.cpp - 多观战修复 + 观战面板排序，详见 notes/观战.md。
 * 多观战：Observer(0xAC1198)是全局单例，只留最后一个观战房；
 * 把本机 CurrentPlayer 对齐过去即可命中原版观战分支。
 * 面板排序：重填面板名单为人类在前、AI 在后，不含任何观战槽。
 * 联机安全：只写本机 CurrentPlayer 与纯 UI 面板名单，Observer 只读。
 */

#include "GdiText.h"
#include "EngineAddr.h"
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

/* 本房无出生点（ArrayIndex 不在 Scenario::HouseIndices 内）。 */
static int OB_HouseHasNoSpawn(void* house)
{
    const int* idx;
    int arrayIndex, i;

    idx = (const int*)(*(const char**)ADDR_SCENARIO + OFF_HOUSEINDICES);
    if (!IsPlausiblePtr(idx)) return 0;
    arrayIndex = *(int*)((const char*)house + OFF_HOUSE_INDEX);
    if (arrayIndex < 0 || arrayIndex > 1024) return 0;
    for (i = 0; i < 16; i++)
        if (idx[i] == arrayIndex) return 0;
    return 1;
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
    if (!IsPlausiblePtr(house)) return;
    observer = *(void**)ADDR_OBSERVER;

    if (IsPlausiblePtr(observer) && house == observer)
    {
        OB_FillPanelList();
        return;
    }

    /* 本机是观战者但没拿到真观战房：对齐到全局房。 */
    if (!IsPlausiblePtr(observer)) return;
    if (!OB_LocalHouseIsObserver(house)) return;
    *(void**)ADDR_CURRENT = observer;
}
