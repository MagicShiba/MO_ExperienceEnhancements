/* BeaconCap.cpp - 单机把原有 24 个信标槽作为共享池, 保留对象的真实 HouseID。
 * 原版线性遍历已经覆盖全部 24 槽, 只修改按 house 分段的查询与定址。
 * 配置 BeaconLimit=3..24, 3 保留原版; 仅 Campaign/Skirmish 且 BeaconInSP 开启时安装。
 * 切换模式前清空池, 避免全局槽号被原版重新解释成别家的槽。 */

#include <windows.h>
#include <string.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "BeaconCap.h"

#define CAP_SECTION "Features"
#define CAP_MAX     24          /* Beacons[8][3] = 24 格, 96 字节 */
#define CAP_VANILLA 3
#define CAP_FRAME_MIN 60        /* 与 BeaconSP 的装钩门控一致 */

typedef struct
{
    unsigned addr;
    unsigned char want[8];
    unsigned char make[8];
    int len;
    int lim_off;                /* >=0: 写 BeaconLimit; -1: make 原样; -2: 生成入口 E9 */
} CapByte;

static int g_CapOn = 0;
static void __attribute__((thiscall)) CapDeleteAll(void* self, int house);
static void* g_CapDeleteAllPtr = (void*)CapDeleteAll;
static void* g_CapDeleteAllNext = (void*)ADDR_BC_CAP_DELETEALL_NEXT;

/* 入口偷取 6B 无相对指令; 禁用时完整重放序言, 启用时尾跳到 thiscall 包装。
 * 包装只调用 DeleteBeacon, 不调用 DeleteAllBeacons 本身。 */
__attribute__((naked)) void BeaconCap_DeleteAllEntry(void)
{
    __asm__ volatile (
        "cmpl $0, %0\n\t"
        "jne 1f\n\t"
        "pushl %%ebp\n\t"
        "movl %%ecx, %%ebp\n\t"
        "movl 0x60(%%ebp), %%eax\n\t"
        "jmp *%1\n\t"
        "1: jmp *%2\n\t"
        : : "m" (g_CapOn), "m" (g_CapDeleteAllNext), "m" (g_CapDeleteAllPtr)
        : "cc");
}

static const CapByte g_CapTab[] =
{
    /* ---- Z: house*3 → 0 ---- */
    { ADDR_BC_CAP_Z1,  { 0x8D,0x04,0x5B }, { 0x31,0xC0,0x90 }, 3, -1 }, /* xor eax,eax */
    { ADDR_BC_CAP_Z2,  { 0x8D,0x0C,0x58 }, { 0x8B,0xC8,0x90 }, 3, -1 }, /* mov ecx,eax (槽号留下) */
    { ADDR_BC_CAP_Z3,  { 0x8B,0xD3 },       { 0x31,0xD2 },      2, -1 },
    { ADDR_BC_CAP_Z4,  { 0x8D,0x04,0x58 }, { 0x90,0x90,0x90 }, 3, -1 }, /* eax 本来就是 id */
    { ADDR_BC_CAP_Z5,  { 0x8B,0xCB },       { 0x31,0xC9 },      2, -1 },
    { ADDR_BC_CAP_Z6,  { 0x8D,0x14,0x52 }, { 0x31,0xD2,0x90 }, 3, -1 },
    { ADDR_BC_CAP_Z9,  { 0x8D,0x0C,0x41 }, { 0x31,0xC0,0x90 }, 3, -1 },
    { ADDR_BC_CAP_Z10, { 0x8D,0x14,0x49 }, { 0x31,0xD2,0x90 }, 3, -1 },
    { ADDR_BC_CAP_Z11, { 0x8D,0x04,0x4B,0x8B,0xD1,0x03,0xD0 },
                        { 0x8B,0xD3,0x90,0x90,0x90,0x90,0x90 }, 7, -1 },
    { ADDR_BC_CAP_Z13, { 0x8D,0x14,0x42 }, { 0x31,0xC0,0x90 }, 3, -1 },
    { ADDR_BC_CAP_Z14, { 0x8D,0x0C,0x49 }, { 0x31,0xC9,0x90 }, 3, -1 },

    /* ---- L1..L3: 判满 / 扫槽, 跟配置走 ---- */
    { ADDR_BC_CAP_L1,  { 0x83,0xF9,0x03 }, { 0x83,0xF9,0x03 }, 3, 2 },
    { ADDR_BC_CAP_L2,  { 0x83,0xF8,0x03 }, { 0x83,0xF8,0x03 }, 3, 2 },
    { ADDR_BC_CAP_L3,  { 0x83,0xF8,0x03 }, { 0x83,0xF8,0x03 }, 3, 2 },

    /* ---- L4..L12: 遍历一律按容量 24 (多扫空槽无害, 少了会漏画/漏删) ---- */
    { ADDR_BC_CAP_L4,  { 0x83,0xFE,0x03 }, { 0x83,0xFE,0x18 }, 3, -1 },
    { ADDR_BC_CAP_L5,  { 0x83,0xFE,0x03 }, { 0x83,0xFE,0x18 }, 3, -1 },
    { ADDR_BC_CAP_L6,  { 0x83,0xFA,0x03 }, { 0x83,0xFA,0x18 }, 3, -1 },
    { ADDR_BC_CAP_L7,  { 0x83,0xFA,0x03 }, { 0x83,0xFA,0x18 }, 3, -1 },
    { ADDR_BC_CAP_L8,  { 0x83,0xFB,0x03 }, { 0x83,0xFB,0x18 }, 3, -1 },
    { ADDR_BC_CAP_L10, { 0x83,0xFA,0x03 }, { 0x83,0xFA,0x18 }, 3, -1 },
    { ADDR_BC_CAP_L11, { 0x83,0xF8,0x03 }, { 0x83,0xF8,0x18 }, 3, -1 },
    { ADDR_BC_CAP_L12, { 0x83,0xFD,0x03 }, { 0x83,0xFD,0x18 }, 3, -1 },

    /* ---- H: house 循环界 8 → 1 ---- */
    { ADDR_BC_CAP_H4,  { 0x83,0xF8,0x08 },                     { 0x83,0xF8,0x01 },                     3, -1 },
    { ADDR_BC_CAP_H6,  { 0x83,0xF8,0x08 },                     { 0x83,0xF8,0x01 },                     3, -1 },
    { ADDR_BC_CAP_H7,  { 0x83,0xFD,0x08 },                     { 0x83,0xFD,0x01 },                     3, -1 },
    { ADDR_BC_CAP_H8,  { 0x83,0xFF,0x08 },                     { 0x83,0xFF,0x01 },                     3, -1 },
    { ADDR_BC_CAP_H9,  { 0x83,0xFF,0x08 },                     { 0x83,0xFF,0x01 },                     3, -1 },
    { ADDR_BC_CAP_H11, { 0xC7,0x44,0x24,0x14,0x08,0x00,0x00,0x00 }, { 0xC7,0x44,0x24,0x14,0x01,0x00,0x00,0x00 }, 8, -1 },
    /* 入口 E9 与其余寻址补丁在同一事务安装/恢复。 */
    { ADDR_BC_CAP_DELETEALL, { 0x55,0x8B,0xE9,0x8B,0x45,0x60 }, { 0 }, 6, -2 },
};
#define CAP_N ((int)(sizeof(g_CapTab) / sizeof(g_CapTab[0])))

static int g_CapInit = 0;
static int g_CapCfg = CAP_MAX;      /* [Features] BeaconLimit */
static int g_CapSpCfg = 1;          /* BeaconInSP */
static int g_CapDead = 0;           /* 校验没过, 本次运行不再尝试 */
static int g_CapState = -1;         /* 0=关闭, 1=池化, 2=等待恢复; 恢复失败只记一次 */

static void CapLog(const char* msg)
{
    char buf[72];
    unsigned n = AppendCStr(buf, 0, "beaconcap: ", sizeof(buf));
    AppendCStr(buf, n, msg ? msg : "?", sizeof(buf));
    LogLine(buf);
}

static void CapEnsureInit(void)
{
    char buf[72];
    unsigned n;
    if (g_CapInit) return;
    g_CapInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_CapCfg = GetPrivateProfileIntA(CAP_SECTION, "BeaconLimit", CAP_MAX, g_IniPath);
    if (g_CapCfg < CAP_VANILLA) g_CapCfg = CAP_VANILLA;
    g_CapSpCfg = GetPrivateProfileIntA(CAP_SECTION, "BeaconInSP", 1, g_IniPath) ? 1 : 0;
    if (g_CapCfg > CAP_MAX) g_CapCfg = CAP_MAX;
    n = AppendCStr(buf, 0, "BeaconLimit=", sizeof(buf));
    n = OU_AppendU(buf, n, (unsigned)g_CapCfg, sizeof(buf));
    AppendCStr(buf, n, (g_CapCfg > CAP_VANILLA) ? " (sp only)" : " (vanilla, no patch)", sizeof(buf));
    CapLog(buf);
}

/* 只在池化布局有效时按对象归属删除, 不把物理槽当作玩家编号。 */
static void __attribute__((thiscall)) CapDeleteAll(void* self, int house)
{
    typedef void (__attribute__((thiscall)) *DeleteFn)(void*, int, int);
    int gm = *(int*)0xA8B238u;
    if (gm != 0 && gm != 5) return;
    void** slots = (void**)self;
    for (int i = 0; i < CAP_MAX; ++i)
    {
        void* b = slots[i];
        if (b && *(int*)((char*)b + OFF_BC_HOUSEID) == house)
            ((DeleteFn)(void*)ADDR_BC_CAP_DELETE)(self, house, i);
    }
}

static void CapFill(const CapByte* b, unsigned char* out)
{
    memcpy(out, b->make, (size_t)b->len);
    if (b->lim_off >= 0) out[b->lim_off] = (unsigned char)g_CapCfg;
    if (b->lim_off == -2)
    {
        out[0] = 0xE9;
        int rel = (int)(unsigned)BeaconCap_DeleteAllEntry - (int)(b->addr + 5);
        memcpy(out + 1, &rel, 4);
        out[5] = 0x90;
    }
}

/* 同页多次解锁会记录到 RWX, 必须逆序恢复。失败的保护恢复保留到下轮重试,
 * 不把尚未恢复的 old 值丢掉, 也不在保护事务未结束时开启另一次写入。 */
static DWORD g_CapOld[CAP_N];
static int g_CapUnlocked = 0;
static int g_CapProtLogged = 0;
static int CapRelock(void)
{
    DWORD ignored;
    while (g_CapUnlocked)
    {
        int i = g_CapUnlocked - 1;
        if (!VirtualProtect((void*)g_CapTab[i].addr, (SIZE_T)g_CapTab[i].len,
                            g_CapOld[i], &ignored))
        {
            if (!g_CapProtLogged) { CapLog("protection restore failed - retrying"); g_CapProtLogged = 1; }
            return 0;
        }
        --g_CapUnlocked;
    }
    g_CapProtLogged = 0;
    return 1;
}

static int CapEmpty(void)
{
    void** slots = (void**)ADDR_BEACON_MGR;
    for (int i = 0; i < CAP_MAX; ++i) if (slots[i]) return 0;
    return 1;
}

/* 校验完整目标状态、全部解锁后才改字节; 失败不产生半安装/半恢复。
 * 返回 1 表示字节事务完成; 代码页保护恢复失败由 CapRelock 独立重试。 */
static int CapChange(int enable)
{
    unsigned char buf[8];
    if (!CapRelock()) return 0;
    for (int i = 0; i < CAP_N; ++i)
    {
        const CapByte* b = &g_CapTab[i];
        if (!OU_InExe(b->addr)) return 0;
        CapFill(b, buf);
        const unsigned char* actual = (const unsigned char*)b->addr;
        const unsigned char* expected = enable ? b->want : buf;
        for (int j = 0; j < b->len; ++j) if (actual[j] != expected[j]) return 0;
    }
    for (int i = 0; i < CAP_N; ++i)
    {
        const CapByte* b = &g_CapTab[i];
        if (!VirtualProtect((void*)b->addr, (SIZE_T)b->len,
                            PAGE_EXECUTE_READWRITE, &g_CapOld[i]))
        {
            CapRelock();
            return 0;
        }
        ++g_CapUnlocked;
    }
    /* Reset 原生线性清 24 格, 不调用 DeleteAllBeacons, 不广播且不改单位模拟。
     * 模式切换时池槽不能继续按 house*3 解释, 所以恢复字节前必须清空本地信标 UI。 */
    if (!enable && !CapEmpty())
    {
        typedef void (__attribute__((thiscall)) *ResetFn)(void*);
        ((ResetFn)(void*)ADDR_BC_CAP_RESET)((void*)ADDR_BEACON_MGR);
    }
    for (int i = 0; i < CAP_N; ++i)
    {
        const CapByte* b = &g_CapTab[i];
        CapFill(b, buf);
        memcpy((void*)b->addr, enable ? buf : b->want, (size_t)b->len);
    }
    g_CapOn = enable;
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    CapRelock();
    return 1;
}

int BeaconCap_SlotCount(void)
{
    return g_CapOn ? g_CapCfg : CAP_VANILLA;
}

void BeaconCap_Poll(void)
{
    CapEnsureInit();
    if (!CapRelock()) return;
    int gm = *(int*)0xA8B238u;
    int want = ((gm == 0 || gm == 5) && g_CapSpCfg && !g_CapDead && g_CapCfg > CAP_VANILLA);
    if (want && !g_CapOn)
    {
        /* 旧场景/读档的原版 house 槽不可直接当全局池使用; 等原生 Reset 清空。 */
        if (*(int*)ADDR_FRAME < CAP_FRAME_MIN || !CapEmpty()) return;
        if (!CapChange(1))
        {
            g_CapDead = 1;
            CapLog("install failed - OFF (vanilla limit)");
            return;
        }
        char buf[48];
        unsigned n = AppendCStr(buf, 0, "ON limit=", sizeof(buf));
        n = OU_AppendU(buf, n, (unsigned)g_CapCfg, sizeof(buf));
        AppendCStr(buf, n, " (pooled, sp)", sizeof(buf));
        CapLog(buf);
    }
    else if (!want && g_CapOn)
    {
        if (!CapChange(0))
        {
            if (g_CapState != 2) CapLog("restore failed - pooled state retained, retrying");
            g_CapState = 2;
            return;
        }
        CapLog("restored (vanilla 3/house)");
    }
    g_CapState = g_CapOn;
}
