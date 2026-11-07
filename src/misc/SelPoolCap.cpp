/* 框选候选池扩为 4096 项, 14 处同长补丁统一迁移基址、扫描终点和容量。
 * 条目为 {Object*, X, Y}, 每帧 Render 重建; 原表无原地扩容空间。
 * 该表仅供本地渲染与选择 UI 使用, 不参与模拟或联机事件。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "MOExperience.h"
#include "SelPoolCap.h"

#define SC_N      4096u    /* 新容量 (原版 500) */
#define SC_ENT    12u      /* {Object*, X, Y} */
#define SC_SITES  (sizeof(kScSites) / sizeof(kScSites[0]))

/* fld 的两个特殊值: 直接写常量 / 写新表尾 */
#define SC_F_CAP  0xFFFFFFF0u
#define SC_F_END  0xFFFFFFF1u

typedef struct
{
    unsigned addr;          /* 指令地址 */
    unsigned char len;      /* 指令长度 (VirtualProtect / Flush 用) */
    unsigned char immOff;   /* 立即数在指令内的偏移 */
    unsigned char op0, op1, op2;   /* 形状校验 (0 = 该位不校验) */
    unsigned fld;           /* 0/4/8 = 表内字段偏移; SC_F_CAP / SC_F_END */
} ScSite;

static const ScSite kScSites[] =
{
    /* 批量登记 0x6D9CE0 (视口内单位表) */
    { 0x006D9E8Eu, 7, 3, 0x89, 0x0C, 0x85, 0u },
    { 0x006D9EAFu, 7, 3, 0x89, 0x14, 0x85, 4u },
    { 0x006D9EB6u, 7, 3, 0x89, 0x0C, 0x85, 8u },
    /* 单体登记 AddSelectable 0x6D9EF0 */
    { 0x006D9F32u, 7, 3, 0x89, 0x14, 0x85, 0u },
    { 0x006D9F53u, 7, 3, 0x89, 0x34, 0x85, 4u },
    { 0x006D9F5Bu, 7, 3, 0x89, 0x14, 0x85, 8u },
    /* 框内有无物件 0x6DA080 / 单击拾取 0x6DA380 */
    { 0x006DA0DFu, 5, 1, 0xBE, 0, 0, 0u },
    { 0x006DA3CAu, 5, 1, 0xBF, 0, 0, 4u },
    /* 移除登记 0x6DA560 起始基址 */
    { 0x006DA564u, 5, 1, 0xB8, 0, 0, 0u },
    /* 框选遍历 0x6DA5C0 */
    { 0x006DA5FEu, 5, 1, 0xBF, 0, 0, 0u },
    /* GetSelectable(index) 0x6DC430 */
    { 0x006DC437u, 7, 3, 0x8B, 0x04, 0x85, 0u },
    /* 容量检查: AddSelectable / 批量登记 */
    { 0x006D9EF7u, 5, 1, 0x3D, 0, 0, SC_F_CAP },
    { 0x006D9E5Cu, 5, 1, 0x3D, 0, 0, SC_F_CAP },
    /* 移除登记的全表扫描终点 */
    { 0x006DA576u, 5, 1, 0x3D, 0, 0, SC_F_END },
};

static int g_ScInit = 0;
static int g_ScDone = 0;
static int g_ScOn = 0;      /* 补丁确实装上 (失败/放弃时保持 0, 容量仍按 500 算) */

/* 新表放本模块 .bss: 不占文件体积, 地址在 DLL 映像内 (< 0x80000000),
 * 0x6DA576 那条移除登记用的是**有符号** `jl` 比表尾, 地址必须保持低位。 */
static unsigned char s_ScBuf[SC_N * SC_ENT];
static DWORD s_ScOld[SC_SITES];
static unsigned s_ScUnlocked;
static int s_ScProtLogged;

/* 同页重复解锁会记录到 RWX, 必须逆序恢复; 失败时保留原保护值供下轮重试。 */
static int SC_Relock(void)
{
    DWORD ignored;
    while (s_ScUnlocked)
    {
        unsigned i = s_ScUnlocked - 1;
        if (!VirtualProtect((void*)kScSites[i].addr, kScSites[i].len, s_ScOld[i], &ignored))
        {
            if (!s_ScProtLogged) OU_LogTag("selpool: ", "protection restore failed - retrying");
            s_ScProtLogged = 1;
            return 0;
        }
        --s_ScUnlocked;
    }
    s_ScProtLogged = 0;
    return 1;
}

/* 日志形如 "selpool: opcode mismatch at 0x006D9E8E" (消息截到 24, 后面要留地址)。 */
static void SC_LogAddr(const char* msg, unsigned addr)
{
    char buf[48];
    unsigned p = AppendCStr(buf, 0, msg, 24);
    p = AppendCh(buf, p, ' ', sizeof(buf));
    HtoA(addr, buf + p);            /* 自带 "0x" + 八位 hex + NUL */
    OU_LogTag("selpool: ", buf);
}

/* 该 site 现在的立即数应该是多少 (旧值 / 新值)。 */
static void SC_Imm(const ScSite* s, unsigned* want, unsigned* immNew)
{
    if (s->fld == SC_F_CAP)
    {
        *want = 0x1F4u;
        *immNew = SC_N;
    }
    else if (s->fld == SC_F_END)
    {
        *want = ADDR_TACTICAL_SELARR + 500u * SC_ENT;
        *immNew = (unsigned)(size_t)s_ScBuf + SC_N * SC_ENT;
    }
    else
    {
        *want = ADDR_TACTICAL_SELARR + s->fld;
        *immNew = (unsigned)(size_t)s_ScBuf + s->fld;
    }
}

void SelPool_Poll(void)
{
    unsigned i, want, immNew, cur;
    unsigned char* site;
    int ok = 1;

    if (!SC_Relock() || g_ScDone) return;

    if (!g_ScInit)
    {
        g_ScInit = 1;
        BuildPaths();
        GtEnsureLog();
    }

    /* Phobos 的 ExtSelection 把 Unsorted::TacticalSelectables (0xB0CEC8) 的
     * 基址写死在代码里, 不经过引擎的登记点; 本模块把表搬到 s_ScBuf 后, Phobos
     * 仍按旧表 + 新计数遍历, 读到的是已失效的旧条目 → 虚调用野指针崩溃。
     * 两者只能留一个: 有 Phobos 时保持原表不搬 (容量 500)。 */
    if (MOE_PhobosLoaded())
    {
        g_ScDone = 1;
        OU_LogTag("selpool: ", "OFF (Phobos loaded: hardcoded TacticalSelectables)");
        return;
    }

    if (*(int*)ADDR_FRAME < 60) return;   /* 等 exe 解包稳定 */

    /* 第一遍: 只验, 不写。任一处既不是原版形态也不是已改形态就整体放弃。 */
    for (i = 0; i < SC_SITES; i++)
    {
        site = (unsigned char*)kScSites[i].addr;
        if (site[0] != kScSites[i].op0
            || (kScSites[i].op1 && site[1] != kScSites[i].op1)
            || (kScSites[i].op2 && site[2] != kScSites[i].op2))
        {
            SC_LogAddr("opcode mismatch at", kScSites[i].addr);
            ok = 0;
            break;
        }
        SC_Imm(&kScSites[i], &want, &immNew);
        cur = *(unsigned*)(site + kScSites[i].immOff);
        if (cur != want && cur != immNew)
        {
            SC_LogAddr("imm mismatch at", kScSites[i].addr);
            ok = 0;
            break;
        }
    }
    if (!ok)
    {
        g_ScDone = 1;   /* 形状不对就永久放弃, 不每帧重试刷日志 */
        OU_LogTag("selpool: ", "OFF (unexpected exe)");
        return;
    }

    /* 全部解锁后才写入, 防止中途失败导致读写站点分别访问新旧表。 */
    for (i = 0; i < SC_SITES; i++)
    {
        if (!VirtualProtect((void*)kScSites[i].addr, kScSites[i].len,
                            PAGE_EXECUTE_READWRITE, &s_ScOld[i]))
        {
            SC_LogAddr("VirtualProtect fail at", kScSites[i].addr);
            SC_Relock();
            g_ScDone = 1;
            return;
        }
        ++s_ScUnlocked;
    }
    for (i = 0; i < SC_SITES; i++)
    {
        site = (unsigned char*)kScSites[i].addr;
        SC_Imm(&kScSites[i], &want, &immNew);
        *(unsigned*)(site + kScSites[i].immOff) = immNew;
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    SC_Relock();

    g_ScOn = 1;
    g_ScDone = 1;
    OU_LogTag("selpool: ", "cap 500->4096 ON (14 sites)");
}

unsigned SelPool_Capacity(void)
{
    return g_ScOn ? SC_N : 500u;
}

unsigned char* SelPool_Base(void)
{
    return g_ScOn ? s_ScBuf : (unsigned char*)(size_t)ADDR_TACTICAL_SELARR;
}
