/* MegaListCap.cpp - EventClass::MegaMissionList 256 → 4096。
 *
 * 执行侧 (0x64C739) 每帧只从 DoList 搬 256 条进 MegaMissionList,
 * 满即在 DoList 原槽置 IsExecuted 位丢弃。
 * MegaMissionList = QueueClass<Event,256> @ 0xA83ED0
 * (Count+0, Head+4, Tail+8, Array[256] +12 每项 111B, Timings[256] @ 0xA8ADDC),
 * 原地无扩容空间, 故 Array/Timings 整体搬家到堆上 4096 份, 头不动:
 * 初始化基址+个数、入队上限、入队数组/Timings 基址、Tail 环绕掩码、
 * 出队数组基址、Head 前进掩码 (漏了它头尾脱节)、Head/下标环绕掩码 10 处、
 * 出队侧散读基址 20 处, 共 38 处 imm32/disp32 同长改写, 无跳板。
 *
 * 联机安全: 联机每帧待执行 MegaMission ≤ OutList 128 < 256,
 * 新旧基址掩码映射一致; 直写 DoList 只在单机发生。
 *
 * 引擎事实见 notes/路径点上限.md §MegaMissionList。 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "MegaListCap.h"

#define ML_SECTION "Features"

#define ML_N 0x1000u           /* 4096 (掩码 0xFFF, 与 WaypointCap 上限对齐) */
#define ML_MASK 0xFFFu
#define ADDR_FRAME 0xA8ED84u   /* Unsorted::CurrentFrame */

static int g_MlInit = 0;
static int g_MlDone = 0;
static unsigned char* g_MlBuf = NULL;   /* Array [4096] */
static unsigned char* g_MlTim = NULL;   /* Timings [4096] */

/* 一处补丁: 验形 (want) 后把偏移 immOff 起 4 字节写成 immNew (同长)。
 * 回 1=已是目标/本次写上, 0=形状不符。 */
static int ML_Patch(unsigned addr, const unsigned char* want, int len,
                    int immOff, unsigned immNew)
{
    unsigned char* site;
    DWORD old = 0;
    int i;
    unsigned cur;
    if (!addr) return 0;
    if (*(int*)ADDR_FRAME < 60) return 0;      /* 等 exe 解包稳定 */
    site = (unsigned char*)addr;
    for (i = 0; i < len; i++)
    {
        if (want[i] == 0xAA) continue;   /* 0xAA=通配 */
        if (site[i] != want[i]) return 0;
    }
    cur = *(unsigned*)(site + immOff);
    if (cur == immNew) return 1;
    if (!VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(unsigned*)(site + immOff) = immNew;
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    return 1;
}

static void ML_Log(const char* msg)
{
    OU_LogTag("megalist: ", msg);
}

/* 出队侧散读基址重定向: 20 处 `[idx*111 + 旧基址+off]` (off<111),
 * 验 disp 与寻址形态后同长改写 disp32=新基址+off。 */
static int ML_Rebase(unsigned dispAddr, unsigned off)
{
    unsigned char* d;
    unsigned oldImm = 0xA83EDCu + off;
    unsigned newImm = (unsigned)(size_t)g_MlBuf + off;
    unsigned char mrm;
    if (!dispAddr || off >= EVT_SIZE) return 0;
    if (*(int*)ADDR_FRAME < 60) return 0;
    d = (unsigned char*)dispAddr;
    if (*(unsigned*)d != oldImm && *(unsigned*)d != newImm) return 0;
    if (*(unsigned*)d == newImm) return 1;
    if (d[-1] != 0x40) return 0;
    mrm = d[-2];
    if (mrm != 0x9C && mrm != 0x94 && mrm != 0x84 && mrm != 0x8C) return 0;
    {
        DWORD old = 0;
        if (!VirtualProtect(d, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
        *(unsigned*)d = newImm;
        VirtualProtect(d, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), d, 4);
    }
    return 1;
}

/* 补丁形状表 (文件域, Poll 内只做改写)。 */
static const unsigned char kMlInitBase[5] = { 0xB8, 0xAA, 0xAA, 0xAA, 0xAA };
static const unsigned char kMlInitCnt[5] = { 0xB9, 0x00, 0x01, 0x00, 0x00 };
static const unsigned char kMlEnqCap[10] =
    { 0x81, 0x3D, 0xD0, 0x3E, 0xA8, 0x00, 0x00, 0x01, 0x00, 0x00 };
static const unsigned char kMlEnqArr[7] =
    { 0x8D, 0xBC, 0x40, 0xAA, 0xAA, 0xAA, 0xAA };
static const unsigned char kMlEnqTim[7] =
    { 0x89, 0x04, 0x8D, 0xAA, 0xAA, 0xAA, 0xAA };
static const unsigned char kMlTailMask[6] =
    { 0x81, 0xE2, 0xFF, 0x00, 0x00, 0x00 };
static const unsigned char kMlHeadAdvMask[6] =
    { 0x81, 0xE7, 0xFF, 0x00, 0x00, 0x00 };
static const unsigned char kMlDeqArr[7] =
    { 0x8D, 0x8C, 0x40, 0xAA, 0xAA, 0xAA, 0xAA };
static const unsigned char kMlHeadMask[5] =
    { 0x25, 0xFF, 0x00, 0x00, 0x00 };
static const unsigned kMlHeadSites[] =
{
    0x0064CB14u, 0x0064CB65u, 0x0064CD4Cu, 0x0064CD73u, 0x0064CDB8u,
    0x0064CE60u, 0x0064D6E9u, 0x0064D722u, 0x0064D81Fu, 0x0064D858u
};
static const unsigned kMlRebaseAddr[] =
{
    0x0064CD63u, 0x0064CE6Eu, 0x0064D707u, 0x0064D83Du,
    0x0064CD5Au, 0x0064D6F7u, 0x0064D82Du,
    0x0064CB22u, 0x0064CB76u,
    0x0064CB3Au, 0x0064CB7Du,
    0x0064CB2Du,
    0x0064CB58u, 0x0064CB84u, 0x0064CD81u, 0x0064CDE5u,
    0x0064D737u, 0x0064D86Du,
    0x0064CB49u, 0x0064CDCDu
};
static const unsigned char kMlRebaseOff[] =
{
    7, 7, 7, 7,
    11, 11, 11,
    12, 12,
    14, 14,
    18,
    19, 19, 19, 19, 19, 19,
    23, 23
};

void MlCap_Poll(void)
{
    unsigned i;
    int ok = 1;
    static int s_MlMpLogged = 0;
    if (g_MlDone) return;
    /* 只在单机生效: 联机一个补丁都不装 (完全绕过)。
     * (联机走不到 256 以上，见文件头)。 */
    if (OU_IsMP())
    {
        if (!s_MlMpLogged) { s_MlMpLogged = 1; ML_Log("MP bypass (SP only)"); }
        return;
    }
    if (!g_MlInit)
    {
        g_MlInit = 1;
        BuildPaths();
        GtEnsureLog();
        g_MlBuf = (unsigned char*)VirtualAlloc(NULL, ML_N * EVT_SIZE + ML_N * 4u,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_MlBuf)
        {
            ML_Log("VirtualAlloc fail - OFF");
            g_MlDone = 1;
            return;
        }
        g_MlTim = g_MlBuf + ML_N * EVT_SIZE;
    }
    if (*(int*)ADDR_FRAME < 60) return;
    if (!ML_Patch(0x004E7F00u, kMlInitBase, 5, 1, (unsigned)(size_t)g_MlBuf))
        ok = 0;
    if (!ML_Patch(0x004E7F05u, kMlInitCnt, 5, 1, ML_N))
        ok = 0;
    if (!ML_Patch(0x0064C739u, kMlEnqCap, 10, 6, ML_N))
        ok = 0;
    if (!ML_Patch(0x0064C774u, kMlEnqArr, 7, 3, (unsigned)(size_t)g_MlBuf))
        ok = 0;
    if (!ML_Patch(0x0064C78Cu, kMlEnqTim, 7, 3, (unsigned)(size_t)g_MlTim))
        ok = 0;
    if (!ML_Patch(0x0064C79Fu, kMlTailMask, 6, 2, ML_MASK))
        ok = 0;
    if (!ML_Patch(0x0064CC21u, kMlHeadAdvMask, 6, 2, ML_MASK))
        ok = 0;
    if (!ML_Patch(0x0064CC03u, kMlDeqArr, 7, 3, (unsigned)(size_t)g_MlBuf))
        ok = 0;
    for (i = 0; i < sizeof(kMlHeadSites) / sizeof(kMlHeadSites[0]); i++)
    {
        if (!ML_Patch(kMlHeadSites[i], kMlHeadMask, 5, 1, ML_MASK))
            ok = 0;
    }
    for (i = 0; i < sizeof(kMlRebaseAddr) / sizeof(kMlRebaseAddr[0]); i++)
    {
        if (!ML_Rebase(kMlRebaseAddr[i], kMlRebaseOff[i]))
            ok = 0;
    }
    if (ok && !g_MlDone)
    {
        g_MlDone = 1;
        ML_Log("cap 256->4096 ON (38 sites)");
    }
}
