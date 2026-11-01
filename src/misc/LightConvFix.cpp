/* LightConvFix.cpp - 光照变更后的每格 LightConvert 刷新。
 *
 * 每格绘制用的 CellClass::LightConvert (+0x34) 按"光照签名"缓存, 签名含
 * Scenario+0x352C AmbientCurrent —— 构建时的环境光被烘焙进调色板。
 * ScenarioClass::UpdateCellLighting (0x4AE4C0) 只重算每格标量, 不重建 convert;
 * InitLightConvert(null) (0x483E30, thiscall) 才按新签名重建 (签名没变的复用
 * 旧件, convert 是引用计数共享池), 但它只在场景初始化和零散事件点被调。于是
 * RecalcLighting 之后没被事件碰到的格子长期用旧环境光的 convert 绘制, 与邻居
 * 形成 2:1 阶梯锯齿 (原版同病)。
 *
 * 修复 = 从源头 hook: 引擎所有标量重算都经过两处 call UpdateCellLighting
 * (0x53ADDB 在 RecalcLighting 内 / 0x55B4C6 在环境光渐变步进内), 在调用点后
 * 按环境光/tint 签名决定是否全数组逐格重建 convert。读档另由 LightingPreset.cpp
 * 的场景就绪钩子触发无条件重建 (LightConv_RefreshNow): LoadGame (0x67E440) 不调
 * RecalcLighting, 且地形重建期间 convert 是按瞬态字段值建的, 签名比对发现不了
 * "字段对、画面旧"。
 *
 * 只读模拟数据 + 重建渲染资源, 不碰模拟状态, 联机安全。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "LightConvFix.h"

#define LF_SCEN      0x87F7E8u            /* MapClass 全局对象 (格子数组所在) */
#define LF_OFF_BASE  0x13Cu               /* MapClass+0x13C = 格子指针数组基址 */
#define LF_SCENINST  0xA8B230u            /* ScenarioClass::Instance (指针) */
#define LF_OFF_AMB   0x352Cu              /* AmbientCurrent */
#define LF_OFF_R     0x3534u              /* NormalLighting.Tint.R */
#define LF_OFF_G     0x3538u
#define LF_OFF_B     0x353Cu

#define LF_F_CELLUPD 0x4AE4C0u            /* ScenarioClass::UpdateCellLighting (fastcall, 无参) */
#define LF_F_INITCONV 0x483E30u           /* CellClass::InitLightConvert (thiscall) */
#define LD_AFTER_RECALC   0x53ADE0u       /* 0x53ADDB + 5 */
#define LD_AFTER_STEP     0x55B4CBu       /* 0x55B4C6 + 5 */

#define LF_CELL_CONV 0x34u                /* CellClass::LightConvert */

#define LF_SLOTS     (512 * 512)          /* 指针数组槽数 (引擎越界判定同值) */

#define LF_SECTION "LightingRefresh"

typedef void (__fastcall *LF_FnScen)(void* scen);
typedef void (__attribute__((thiscall)) *LF_FnInitConv)(void* cell, void* drawer,
                                                        int nIntensity, int nAmbient,
                                                        int r, int g, int b);

static LF_FnScen s_CellUpd;
static LF_FnInitConv s_InitConv;
static int s_Init, s_On = 1, s_HaveSig;
static unsigned s_LastAmb, s_LastR, s_LastG, s_LastB;
static unsigned s_LastTick, s_LastSig;    /* 刷新日志去重 (规模 + 1 秒) */

/* MapClass+0x13C = 格子指针数组; 读档把世界拆掉时它是 NULL, 重建完才回来。 */
static unsigned LF_CellBase(void)
{
    return *(unsigned*)(size_t)(LF_SCEN + LF_OFF_BASE);
}

int LightConv_WorldReady(void)
{
    return LF_CellBase() != 0;
}

static void LF_EnsureInit(void)
{
    if (s_Init) return;
    s_Init = 1;
    s_On = GetPrivateProfileIntA(LF_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    s_CellUpd  = (LF_FnScen)(size_t)LF_F_CELLUPD;
    s_InitConv = (LF_FnInitConv)(size_t)LF_F_INITCONV;
}

/* 光照字段在 ScenarioClass 对象 (Instance 指针解引用) 上, 不是 MapClass。 */
static unsigned LF_ScenObj(void)
{
    return *(unsigned*)(size_t)LF_SCENINST;
}

static unsigned LF_ScenGet(unsigned scen, unsigned off)
{
    return *(unsigned*)(size_t)(scen + off);
}

static void LF_StoreSig(void)
{
    unsigned scen = LF_ScenObj();

    if (!scen) return;
    s_LastAmb = LF_ScenGet(scen, LF_OFF_AMB);
    s_LastR = LF_ScenGet(scen, LF_OFF_R);
    s_LastG = LF_ScenGet(scen, LF_OFF_G);
    s_LastB = LF_ScenGet(scen, LF_OFF_B);
    s_HaveSig = 1;
}

/* 当前字段与上次刷新时相同 → 0 */
static int LF_SigChanged(void)
{
    unsigned scen, a, r, g, b;

    scen = LF_ScenObj();
    if (!scen) return 0;
    a = LF_ScenGet(scen, LF_OFF_AMB);
    r = LF_ScenGet(scen, LF_OFF_R);
    g = LF_ScenGet(scen, LF_OFF_G);
    b = LF_ScenGet(scen, LF_OFF_B);
    if (s_HaveSig && a == s_LastAmb && r == s_LastR && g == s_LastG && b == s_LastB)
        return 0;
    return 1;
}

/* force=1 跳过去重, 必然留日志 (场景重开的那次刷新正是要看的)。 */
static void LF_RefreshAll(int force)
{
    unsigned base, slot, old, cells = 0, rebuilt = 0;
    unsigned tick, sig, p;
    void* c;
    char msg[128];

    base = LF_CellBase();
    if (!base) return;

    for (slot = 0; slot < LF_SLOTS; slot++)
    {
        c = *(void**)(size_t)(base + slot * 4);
        if (!c) continue;
        cells++;
        old = *(unsigned*)(size_t)((unsigned)(size_t)c + LF_CELL_CONV);
        s_InitConv(c, NULL, 0x10000, 0, 1000, 1000, 1000);
        if (*(unsigned*)(size_t)((unsigned)(size_t)c + LF_CELL_CONV) != old)
            rebuilt++;
    }

    sig = cells ^ (rebuilt << 3);
    tick = GetTickCount();
    if (!force && sig == s_LastSig && tick - s_LastTick < 1000) return;
    s_LastSig = sig;
    s_LastTick = tick;

    p = 0;
    p = AppendCStr(msg, p, "lightconv: cells=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), (int)cells);
    p = AppendCStr(msg, p, " rebuilt=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), (int)rebuilt);
    p = AppendCStr(msg, p, " amb=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), (int)LF_ScenGet(LF_ScenObj(), LF_OFF_AMB));
    LogLine(msg);
}

/* ---------------- 钩子 ---------------- */

/* 两处引擎调用点 (call UpdateCellLighting, 钩子替换掉的就是这条 call) 的公共体:
 * 自己调原函数完成标量重算, 环境光/tint 签名变了才全格重建 convert, 返回被覆盖
 * call 的下一条。返回 0 = 回放原 call, 开关关闭时引擎照常。 */
static DWORD LF_HookBody(DWORD after)
{
    LF_EnsureInit();
    if (!s_On) return 0;
    s_CellUpd((void*)(size_t)LF_SCEN);
    if (LF_SigChanged()) LF_RefreshAll(0);
    LF_StoreSig();
    return after;
}

/* 0x53ADDB: RecalcLighting 内 call UpdateCellLighting (5B)。 */
extern "C" __declspec(dllexport) DWORD __cdecl LightConv_Recalc(void* regs)
{
    (void)regs;
    return LF_HookBody(LD_AFTER_RECALC);
}

/* 0x55B4C6: 环境光渐变步进内 call UpdateCellLighting (5B)。 */
extern "C" __declspec(dllexport) DWORD __cdecl LightConv_Step(void* regs)
{
    (void)regs;
    return LF_HookBody(LD_AFTER_STEP);
}

/* 无条件全格重建 (不看签名), 场景重开时用: 读档/建图期间引擎会按瞬态光照值
 * (字段还是默认 100 的时候) 建过 convert, 签名可能已经"对齐", 只靠签名比对
 * 会漏掉这一轮, 必须强制重建。必然留日志 (普通刷新按 规模+1 秒 去重)。 */
void LightConv_RefreshNow(void)
{
    LF_EnsureInit();
    if (!s_On) return;
    s_CellUpd((void*)(size_t)LF_SCEN);
    LF_RefreshAll(1);
    LF_StoreSig();
}
