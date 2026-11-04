/* LightConvFix.cpp - 光照变更后的每格 LightConvert 刷新 + 按比例染色。
 *
 * 每格绘制用的 CellClass::LightConvert (+0x34) 是建格子时按当时的
 * AmbientCurrent / tint 算好颜色的, 引擎只在场景初始化与零散事件点重建它
 * (CellClass::InitLightConvert 0x483E30), 而 UpdateCellLighting (0x4AE4C0)
 * 只重算每格标量 —— 于是改过光照后没被事件碰到的格子仍用旧 convert 绘制,
 * 与邻居形成 2:1 阶梯锯齿 (原版同病)。
 *
 * 地形颜色只来自逐格重建 (建图同路, 点光源色也在这一条里); 另外可选地让引擎
 * 把玩家色 (ColorScheme) 与建筑/单位 (0x626690 容器) 也染成同一组色调, 强度由
 * 菜单"比例"决定。见 notes/光照预设.md 3.1 / 3.2。
 *
 * 只读模拟数据 + 改渲染资源, 不碰模拟状态, 联机安全。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "LightConvFix.h"
#include "EnvIni.h"

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
#define LF_SEEN_MAX     512               /* 地形用到的不同 convert 件数上限 */
#define LF_SECTION      ENV_SECTION

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
static int s_Busy;                      /* 池 Count 被临时压过, 期间禁止钩子重建 */
static unsigned s_LastAmb, s_LastR, s_LastG, s_LastB;
static unsigned s_LastTick, s_LastSig;  /* 刷新日志去重 (规模 + 1 秒) */

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
    s_On = GetPrivateProfileIntA(LF_SECTION, ENV_KEY_LIGHTFIX, 1, g_IniPath) ? 1 : 0;
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

/* ESC 的开关只改内存里的门, 落盘走设置页统一的队列。关掉不撤销已重建的 convert
 * (它与当时的光照一致, 留着不会退化成锯齿)。 */
void LightFix_SetEnabled(int on)
{
    LF_EnsureInit();
    s_On = on ? 1 : 0;
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

/* force=1 跳过去重, 必然留日志 (场景重开的那次刷新正是要看的)。
 * convs = 这轮之后地形用到几种不同 convert —— 只有 1 种说明点光源色被抹平了
 * (引擎触发器就是这样), 多种才是每格保留了自己的光源色。 */
static void LF_RefreshAll(int force)
{
    unsigned base, slot, old, cells = 0, rebuilt = 0, nSeen = 0, i;
    unsigned tick, sig, p;
    void* c;
    void* conv;
    void* seen[LF_SEEN_MAX];
    char msg[160];

    base = LF_CellBase();
    if (!base) return;

    for (slot = 0; slot < LF_SLOTS; slot++)
    {
        c = *(void**)(size_t)(base + slot * 4);
        if (!c) continue;
        cells++;
        old = *(unsigned*)(size_t)((unsigned)(size_t)c + LF_CELL_CONV);
        s_InitConv(c, NULL, 0x10000, 0, 1000, 1000, 1000);
        conv = *(void**)(size_t)((unsigned)(size_t)c + LF_CELL_CONV);
        if ((unsigned)(size_t)conv != old)
            rebuilt++;
        if (!conv) continue;
        for (i = 0; i < nSeen; i++)
            if (seen[i] == conv) break;
        if (i == nSeen && nSeen < LF_SEEN_MAX) seen[nSeen++] = conv;
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
    p = AppendCStr(msg, p, " convs=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), (int)nSeen);
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
    if (!s_On || s_Busy) return after;
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

/* ---------------- 按比例给"非地形"上色 ---------------- */

#define LF_POOL_ITEMS   0x87F69Cu         /* Vector<LightConvert*>::Items */
#define LF_POOL_COUNT   0x87F6A8u         /* 同一个 Vector 的 Count (含 [0] 默认件) */

static void* LF_PoolAt(unsigned i)
{
    return *(void**)(size_t)(*(unsigned*)(size_t)LF_POOL_ITEMS + i * 4);
}

/* Color1 是倍率 (1000 = 原色), 引擎写的是绝对值, 所以"混合"就是把目标色从原色
 * 按比例插过去; mix = 0 得到原色, 顺带把上一次的染色清掉。 */
static int LF_MixColor(int v, int mix)
{
    int d = v - 1000;

    if (mix <= 0) return 1000;
    if (mix >= 100) return v;
    return 1000 + (d * mix + (d >= 0 ? 50 : -50)) / 100;
}

/* 改色: 让引擎把 ColorScheme (玩家色) 与 0x626690 容器 (单位/建筑) 染成
 * (r,g,b), **地形不动** —— 地形的颜色由逐格重建现算, 点光源色只在那条路里。
 *
 * 临时把池的 Count 压到 1, RecalcLighting 就只碰到 items[0] 默认件 (整池一起染
 * 会把灯光建筑的 LightRedTint/GreenTint/BlueTint 压成亮度, 引擎触发器的固有
 * 行为); 染完立刻复原 Count, 再把默认件写回 1000,1000,1000 (InitLightConvert
 * 快速路径 0x544E94 不校验它的 Color1)。地形那批留给 LightConv_RebuildCells
 * 按格重算。
 *
 * r/g/b 单位 = tint 字段 ×10 (150/90/60 表示 1.5/0.9/0.6), 上限 2000;
 * mix = 0..100 见 LF_MixColor。 */
void LightConv_TriggerTint(int r, int g, int b, int mix)
{
    static LF_FnRecalc s_Recalc;
    static LF_FnUpdColors s_Upd;
    unsigned* cnt = (unsigned*)(size_t)LF_POOL_COUNT;
    unsigned saved, p;
    void* def;
    char msg[160];

    LF_EnsureInit();
    if (!s_Recalc) s_Recalc = (LF_FnRecalc)(size_t)LF_F_RECALC;
    if (!s_Upd) s_Upd = (LF_FnUpdColors)(size_t)LF_F_UPDCOLORS;

    r = LF_MixColor(r, mix);
    g = LF_MixColor(g, mix);
    b = LF_MixColor(b, mix);

    saved = *cnt;
    s_Busy = 1;                  /* 这期间池只剩默认件, 钩子若重建会误建一大批 */
    if (saved > 1) *cnt = 1;
    s_Recalc(r, g, b, 0);
    *cnt = saved;
    s_Busy = 0;

    def = saved ? LF_PoolAt(0) : 0;
    if (def) s_Upd(def, 1000, 1000, 1000, 0);

    p = 0;
    p = AppendCStr(msg, p, "lightconv: tint=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), r);
    p = AppendCh(msg, p, ',', sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), g);
    p = AppendCh(msg, p, ',', sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), b);
    p = AppendCStr(msg, p, " mix=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), mix);
    p = AppendCStr(msg, p, " pool=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), (int)saved);
    LogLine(msg);
}
