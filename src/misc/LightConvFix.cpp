/* LightConvFix.cpp - 光照变更后的每格 LightConvert 刷新 + 两种上色方式。
 *
 * 每格绘制用的 CellClass::LightConvert (+0x34) 是建格子时按当时的
 * AmbientCurrent / tint 算好颜色的, 引擎只在场景初始化与零散事件点重建它
 * (CellClass::InitLightConvert 0x483E30), 而 UpdateCellLighting (0x4AE4C0)
 * 只重算每格标量 —— 于是改过光照后没被事件碰到的格子仍用旧 convert 绘制,
 * 与邻居形成 2:1 阶梯锯齿 (原版同病)。
 *
 * 两条上色方式 (菜单"调色方式"):
 *   逐格 = 按当前字段重建每格 convert, 与建图同路, 逐像素一致;
 *   统一 = 原样调引擎触发器用的 RecalcLighting(具体 R,G,B), 整池一起改色。
 * 详见 notes/光照预设.md 3.1 / 3.2。
 *
 * 只读模拟数据 + 改渲染资源, 不碰模拟状态, 联机安全。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "LightConvFix.h"

#define LF_SCEN        0x87F7E8u          /* MapClass 全局对象 (格子数组所在) */
#define LF_OFF_BASE    0x13Cu             /* MapClass+0x13C = 格子指针数组基址 */
#define LF_CELL_CONV   0x34u              /* CellClass::LightConvert */

#define LF_SCENINST    0xA8B230u          /* ScenarioClass::Instance (指针) */
#define LF_OFF_AMB     0x352Cu            /* AmbientCurrent */
#define LF_OFF_R       0x3534u
#define LF_OFF_G       0x3538u
#define LF_OFF_B       0x353Cu

#define LF_F_CELLUPD   0x4AE4C0u          /* ScenarioClass::UpdateCellLighting (fastcall, 无参) */
#define LF_F_INITCONV  0x483E30u          /* CellClass::InitLightConvert (thiscall) */
#define LF_F_RECALC    0x53AD00u          /* ScenarioClass::RecalcLighting (fastcall) */
#define LF_F_UPDCOLORS 0x556090u          /* LightConvertClass::UpdateColors (thiscall) */

#define LF_AFTER_RECALC 0x53ADE0u         /* 0x53ADDB + 5 */
#define LF_AFTER_STEP   0x55B4CBu         /* 0x55B4C6 + 5 */

#define LF_SLOTS        (512 * 512)       /* 指针数组槽数 (引擎越界判定同值) */
#define LF_SECTION      "LightingRefresh"

typedef void (__fastcall *LF_FnScen)(void* scen);
typedef void (__attribute__((thiscall)) *LF_FnInitConv)(void* cell, void* drawer,
                                                        int nIntensity, int nAmbient,
                                                        int r, int g, int b);
typedef void (__fastcall *LF_FnRecalc)(int r, int g, int b, int tint);
typedef void (__attribute__((thiscall)) *LF_FnUpdColors)(void* conv, int r, int g,
                                                        int b, int tint);

static LF_FnScen s_CellUpd;
static LF_FnInitConv s_InitConv;
static int s_Init, s_On = 1, s_HaveSig;
static unsigned s_LastAmb, s_LastR, s_LastG, s_LastB;
static unsigned s_LastTick, s_LastSig;  /* 刷新日志去重 (规模 + 1 秒) */
static int s_Tinted;                    /* 统一模式动过整池没有 (决定要不要复原) */

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
    LogLine(s_On ? "lightingfix: ON"
                 : "lightingfix: OFF (engine original, no auto refresh)");
}

int LightFix_Enabled(void)
{
    LF_EnsureInit();
    return s_On;
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
 * call 的下一条。
 *
 * 关闭开关也必须自己调原函数后返回 after, 不能返回 0 交给 Syringe 重放: 站点
 * 这 5 字节就是 `E8 rel32`, 桩池副本不重定位 rel32, 换地址执行后目标平移成野
 * 地址 (见 notes/崩溃定位.md)。 */
static DWORD LF_HookBody(DWORD after)
{
    LF_EnsureInit();
    s_CellUpd((void*)(size_t)LF_SCEN);
    if (!s_On) return after;
    if (LF_SigChanged()) LF_RefreshAll(0);
    LF_StoreSig();
    return after;
}

/* 0x53ADDB: RecalcLighting 内 call UpdateCellLighting (5B)。 */
extern "C" __declspec(dllexport) DWORD __cdecl LightConv_Recalc(void* regs)
{
    (void)regs;
    return LF_HookBody(LF_AFTER_RECALC);
}

/* 0x55B4C6: 环境光渐变步进内 call UpdateCellLighting (5B)。 */
extern "C" __declspec(dllexport) DWORD __cdecl LightConv_Step(void* regs)
{
    (void)regs;
    return LF_HookBody(LF_AFTER_STEP);
}

/* 逐格重建 convert (标量重算 + 全数组 InitLightConvert), 不看开关。
 * 这是"建图"那条路: InitLightConvert(null) 内部走 CalculateLightSourceLighting
 * (0x484180) 读 AmbientCurrent 与 tint, 每格算出自己那份颜色, ScenarioLighting
 * 夹到 1000, 再从池里取同色的件。
 * 强制不看签名: 读档/建图期间引擎按瞬态字段值建过 convert, 签名可能已经"对齐",
 * 靠签名比对会漏掉这一轮。 */
void LightConv_RebuildCells(void)
{
    LF_EnsureInit();
    s_CellUpd((void*)(size_t)LF_SCEN);
    LF_RefreshAll(1);
    LF_StoreSig();
}

/* ---------------- 统一改色: 引擎触发器那条路 ---------------- */

/* 地图触发动作 SetTintRed/Green/Blue (0x6E2E60 / 0x6E2EB0 / 0x6E2F00) 写的是
 *
 *     RecalcLighting(新值*10, 旧G*10, 旧B*10, 0);   然后才写 +0x3534/38/3C
 *
 * RecalcLighting 一次改三处: LightConvert 池全部成员、每份 ColorScheme 的
 * LightConvert (玩家色)、0x626690 容器里的对象, 各调一次 UpdateColors(R,G,B,tint),
 * 收尾 UpdateCellLighting + MapClass::Prepare(1)。触发器的画面 = 整池一起改色。 */

#define LF_POOL_ITEMS   0x87F69Cu         /* Vector<LightConvert*>::Items */
#define LF_POOL_COUNT   0x87F6A8u         /* 同一个 Vector 的 Count (含 [0] 默认件) */
#define LF_SCHEME_ITEMS 0xB054D4u         /* ColorScheme 数组指针 */
#define LF_SCHEME_COUNT 0xB054E0u
#define LF_SCHEME_CONV  0x30Cu            /* ColorScheme::LightConvert */
#define LF_SCHEME_USE   0x310u            /* >1 才算这份 scheme 在用 (引擎同判据) */

static unsigned LF_PoolLiveCount(void)
{
    unsigned n = *(unsigned*)(size_t)LF_POOL_COUNT;

    if (!n || !*(unsigned*)(size_t)LF_POOL_ITEMS) return 0;
    return n;
}

static void* LF_PoolAt(unsigned i)
{
    return *(void**)(size_t)(*(unsigned*)(size_t)LF_POOL_ITEMS + i * 4);
}

static unsigned LF_SchemeLiveCount(void)
{
    unsigned n = *(unsigned*)(size_t)LF_SCHEME_COUNT;

    if (!n || !*(unsigned*)(size_t)LF_SCHEME_ITEMS) return 0;
    return n;
}

static void* LF_SchemeAt(unsigned i)
{
    unsigned scheme = *(unsigned*)(size_t)(*(unsigned*)(size_t)LF_SCHEME_ITEMS + i * 4);

    if (!scheme) return 0;
    if (*(int*)(size_t)(scheme + LF_SCHEME_USE) <= 1) return 0;
    return *(void**)(size_t)(scheme + LF_SCHEME_CONV);
}

/* 统一改色 = 触发器的原样: RecalcLighting 改整池 + 玩家色 + 0x626690, 并重算重画。
 * r/g/b 单位 = tint 字段 ×10 (传 150/90/60 表示 1.5/0.9/0.6), 与触发器一致:
 * 不归一化、上限 2000, 所以比逐格偏亮。 */
void LightConv_TriggerTint(int r, int g, int b)
{
    static LF_FnRecalc s_Recalc;
    unsigned pool, p;
    char msg[128];

    LF_EnsureInit();
    if (!s_Recalc) s_Recalc = (LF_FnRecalc)(size_t)LF_F_RECALC;

    pool = LF_PoolLiveCount();
    s_Recalc(r, g, b, 0);
    s_Tinted = 1;

    p = 0;
    p = AppendCStr(msg, p, "lightconv: trigger tint=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), r);
    p = AppendCh(msg, p, ',', sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), g);
    p = AppendCh(msg, p, ',', sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), b);
    p = AppendCStr(msg, p, " pool=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), (int)pool);
    LogLine(msg);
}

/* 切回逐格前复原两类"底件"的 Color1 = 1000,1000,1000。**不需要记录原值**:
 *
 *  1) 池 [0] 默认件 —— 建它时就是 1000,1000,1000。而且 InitLightConvert 的快速
 *     路径 (0x544E94: 目标色三分量都等于 1000) 直接返回这一件, **不校验它的
 *     Color1**; 统一模式把它染过色, 之后所有"该用默认件"的格子就永远拿错,
 *     逐格重建也救不回来 (引擎自己也不会还)。
 *  2) 每份在用 ColorScheme 的 convert —— 建它时也是 1000,1000,1000: 光照不烘进
 *     它的 Color1 (scheme 的明暗靠 FullColorData)。不修就是整局玩家色偏色。
 *
 * 池里其余件的 Color1 就是它自己的身份 (按色去重), 逐格归一到 1000, 用不到被
 * 染过的那批 (染过的是原始 tint*10), 不用管。0x626690 容器拿不到清单, 不复原。 */
int LightConv_ResetBase(void)
{
    static LF_FnUpdColors s_Upd;
    unsigned i, n, done = 0;
    void* conv;
    char msg[128];
    unsigned p;

    if (!s_Tinted) return 0;
    s_Tinted = 0;

    if (!s_Upd) s_Upd = (LF_FnUpdColors)(size_t)LF_F_UPDCOLORS;

    n = LF_PoolLiveCount();
    if (n && (conv = LF_PoolAt(0)) != 0)
    {
        s_Upd(conv, 1000, 1000, 1000, 0);
        done++;
    }
    n = LF_SchemeLiveCount();
    for (i = 0; i < n; i++)
    {
        conv = LF_SchemeAt(i);
        if (!conv) continue;
        s_Upd(conv, 1000, 1000, 1000, 0);
        done++;
    }

    p = 0;
    p = AppendCStr(msg, p, "lightconv: reset base convs=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), (int)done);
    LogLine(msg);
    return (int)done;
}
