/* LightingPreset.cpp - 游戏内光照预设。
 *
 * 改的是地图 ini [Lighting] 段读进 ScenarioClass 的那几个字段 (OFF_SCEN_*, 见
 * EngineAddr.h), 再让引擎按新值重算格子光照与调色板 —— 全是客户端渲染量, 不碰
 * 模拟状态, 联机安全。
 *
 * 全是临时设置: 不写 ini, 不广播。ESC 菜单取消回滚到打开菜单时的值; 换图时回到
 * 新地图自带的值; 运行期改过的光照会随存档保留 (存盘写的就是这些字段)。
 *
 * 自动那条链路 (场景重开后按引擎现值重算 + 全格 convert 重建) 属于"光照修复",
 * 受 [LightingRefresh] 总开关管; 菜单里的手动改光照不受影响。
 */

#include <windows.h>
#include "LightingPreset.h"
#include "EngineAddr.h"
#include "GdiText.h"
#include "LightConvFix.h"

/* 参数顺序见 LightingPreset.h (LIGHT_P_*): 环境 / 等级 / 地面 / 红 / 绿 / 蓝。
 * 单位 = 引擎字段单位 (环境与 tint = ini ×100, 等级/地面 = ini ×1000)。 */
enum { P_AMBIENT = LIGHT_P_AMBIENT, P_LEVEL = LIGHT_P_LEVEL,
       P_GROUND = LIGHT_P_GROUND, P_RED = LIGHT_P_RED,
       P_GREEN = LIGHT_P_GREEN, P_BLUE = LIGHT_P_BLUE };

/* 预设值, 单位同上。下标 = 预设号 - 1 (标准/早上/下午/黄昏/晚上); 原图不用表 ——
 * 用地图自带值。预设只给环境光与三色, 等级/地面保持地图原值 (那几个脚本本来也
 * 只写这四项)。 */
typedef struct { int amb, r, g, b; } LightPresetValue;

static const LightPresetValue s_PresetTable[LIGHT_PRESET_COUNT - 1] = {
    { 100, 100, 100, 100 },   /* 标准: 环境光与三色都是 1.00 */
    {  75, 109,  80,  49 },   /* 早上 */
    { 100, 108,  94,  68 },   /* 下午 */
    {  35,  51,  46, 141 },   /* 黄昏 */
    {  75,  71,  81, 119 }    /* 晚上 */
};

/* 每个参数的引擎字段偏移 (读值用 AmbientOriginal, 它与 Current 始终同步)。 */
static const int s_ParamOff[LIGHT_PARAM_N] = {
    OFF_SCEN_AMBIENT_ORIG, OFF_SCEN_LEVEL, OFF_SCEN_GROUND,
    OFF_SCEN_TINT_R, OFF_SCEN_TINT_G, OFF_SCEN_TINT_B
};

static int s_Orig[LIGHT_PARAM_N];   /* 地图自带值 (首次改动前快照一次) */
static int s_Saved[LIGHT_PARAM_N];  /* ESC 菜单"取消"用 */
static int s_Wrote[LIGHT_PARAM_N];  /* 上次写进引擎的值 */
static int s_SavedPreset = LIGHT_PRESET_ORIGINAL;
static int s_Preset = LIGHT_PRESET_ORIGINAL;
static int s_SavedMode = LIGHT_MODE_PERCEL;
static int s_Mode = LIGHT_MODE_PERCEL;   /* 调色方式, 只在本次运行有效 */
static int s_HaveOrig = 0;          /* s_Orig 是否已快照 */
static int s_WroteOk = 0;           /* s_Wrote 是否有效 */
static int s_Pending = 0;           /* 场景重开了, 等一下再刷新光照 */
static int s_PendWait = 0;          /* pending 已经等了多少帧 */

typedef void (__attribute__((fastcall)) *FnRecalcLighting)(int r, int g, int b,
                                                          int tint);

static unsigned char* Light_Scen(void)
{
    return *(unsigned char**)(size_t)ADDR_SCENARIO;
}

static int Light_InGame(void)
{
    return *(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED ? 1 : 0;
}

static int Light_Read(int off)
{
    unsigned char* s = Light_Scen();

    if (!s) return 0;
    return *(volatile int*)(s + off);
}

static void Light_Write(int off, int v)
{
    unsigned char* s = Light_Scen();

    if (!s) return;
    *(volatile int*)(s + off) = v;
}

static void Light_ReadAll(int* v)
{
    int i;

    for (i = 0; i < LIGHT_PARAM_N; i++)
        v[i] = Light_Read(s_ParamOff[i]);
}

/* 环境光要写三处: Current==Target 引擎才不做渐变 (立刻生效), 而 UpdateLighting
 * 的普通分支每次都把 Target 复位成 Original, 所以 Original 也得跟着改。
 * 等级/地面各只有一格 (建 convert 时读)。 */
static void Light_WriteAll(const int* v)
{
    Light_Write(OFF_SCEN_AMBIENT_ORIG, v[P_AMBIENT]);
    Light_Write(OFF_SCEN_AMBIENT_CUR,  v[P_AMBIENT]);
    Light_Write(OFF_SCEN_AMBIENT_TGT,  v[P_AMBIENT]);
    Light_Write(OFF_SCEN_TINT_R, v[P_RED]);
    Light_Write(OFF_SCEN_TINT_G, v[P_GREEN]);
    Light_Write(OFF_SCEN_TINT_B, v[P_BLUE]);
    Light_Write(OFF_SCEN_LEVEL,  v[P_LEVEL]);
    Light_Write(OFF_SCEN_GROUND, v[P_GROUND]);
}

static int Light_Same(const int* a, const int* b)
{
    int i;

    for (i = 0; i < LIGHT_PARAM_N; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* 第一次改动之前快照一次地图自带的值 —— 这样"恢复原图"不必依赖引擎读 ini
 * 与进局时序的先后, 快照到的就是引擎当时给出的值。 */
static void Light_EnsureOrig(void)
{
    if (s_HaveOrig) return;
    Light_ReadAll(s_Orig);
    s_HaveOrig = 1;
}

/* 引擎当前值 == 地图自带值而我们还留着用户改过的那组 = 引擎把光照重置成了地图
 * 自带值 (读档/重开), 该盖回用户那组。 */
static int Light_UseOurs(const int* v)
{
    return s_HaveOrig && Light_Same(v, s_Orig) && !Light_Same(s_Wrote, s_Orig);
}

/* 让新光照上屏。两条路 (见 notes 3.1 / 3.2):
 *
 * 逐格: 每格颜色只能靠"重建 convert" (= 建图那条路, 每格自己读 AmbientCurrent
 *   与 tint 算色, 归一化后夹到 1000)。RecalcLighting(具体 R,G,B) 是另一套语义:
 *   它把那组数写进池里每个 convert 的 Color1, 逐格差异全没了。之后再传
 *   RecalcLighting(-1,-1,-1,0) —— 那才是"按各自 Color1 重算渐变、不改颜色",
 *   引擎每帧的普通分支就是这么调的。切换前先把统一模式染过的底件复原。
 *
 * 统一: 原样走触发器, 整池 + 玩家色一起改色, 等级/地面不参与 (convert 不再按格
 *   重算)。 */
static void Light_Refresh(const int* v)
{
    if (!Light_Scen()) return;
    if (s_Mode == LIGHT_MODE_UNIFORM)
    {
        LightConv_TriggerTint(v[P_RED] * 10, v[P_GREEN] * 10, v[P_BLUE] * 10);
        return;
    }
    LightConv_ResetBase();
    LightConv_RebuildCells();          /* 不受 [LightingRefresh] 管: 手动改必须能上屏 */
    ((FnRecalcLighting)(size_t)FN_RECALC_LIGHTING)(-1, -1, -1, 0);
}

static void Light_Apply(const int* v)
{
    int i;
    char msg[128];
    unsigned p;

    if (s_WroteOk && !Light_Same(v, s_Wrote))
    {
        p = 0;
        p = AppendCStr(msg, p, "lighting: apply amb=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_AMBIENT]);
        p = AppendCStr(msg, p, " tint=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_RED]);
        p = AppendCh(msg, p, ',', sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_GREEN]);
        p = AppendCh(msg, p, ',', sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_BLUE]);
        p = AppendCStr(msg, p, " lv=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_LEVEL]);
        p = AppendCStr(msg, p, " gr=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_GROUND]);
        LogLine(msg);
    }

    Light_WriteAll(v);
    for (i = 0; i < LIGHT_PARAM_N; i++) s_Wrote[i] = v[i];
    s_WroteOk = 1;
    Light_Refresh(v);
}

/* 场景重开 (新开/读档/换图) 后按"引擎此刻给的值"重算一遍光照。
 *
 * 要解决的问题: 读档后地形是按"重建那一刻的光照"建的, 而 LoadGame (0x67E440)
 * 不调 RecalcLighting —— 字段是对的, 画面却停在重建时的亮度。
 *
 * 触发: 两个场景就绪钩子, 即引擎把"对局中" (0xA8ED5C) 置 1 的全部两条指令
 * (新开局 0x683E75 / 读档 0x67E6C4, 两条路径互不经过) 置 pending。
 *
 * 等什么: 过了载入闸、格子数组回来了才动手; 但**最多等 240 帧** —— 任何一个闸门
 * 卡住都会让 Poll 从此不干活, 到点就无视闸门硬刷一次。多刷一次只是白干一次活,
 * 不刷就是永久错。 */
void Lighting_Poll(void)
{
    int started = Light_InGame();
    int v[LIGHT_PARAM_N];
    int taken, use_ours, waited;
    char msg[192];
    unsigned p;

    /* 光照修复整体关闭: 不自动重算、不盖回用户那组, 引擎原版怎么做就怎么做。 */
    if (!LightFix_Enabled()) return;

    if (s_Pending)
    {
        s_PendWait++;
        if (s_PendWait < 2) return;       /* 让引擎把这一帧的收尾走完再动手 */
        if ((!started || *(volatile int*)(size_t)ADDR_LOAD_PHASE ||
             !LightConv_WorldReady()) && s_PendWait < 240)
            return;                       /* 载入中/世界没搭好: 继续等 */
        waited = s_PendWait;
        s_Pending = 0;
        s_PendWait = 0;
        /* 场景对象/格子数组都还没有: 没东西可刷, 等下一次场景就绪 (主菜单就是
         * 这个状态)。超时只可能出现在"世界始终没准备好"这种情形。 */
        if (!Light_Scen() || !LightConv_WorldReady()) return;

        Light_ReadAll(v);
        taken = (s_WroteOk && !Light_Same(v, s_Wrote));
        use_ours = (taken && Light_UseOurs(v));

        Light_Apply(use_ours ? s_Wrote : v);

        if (!use_ours && taken)
        {
            s_HaveOrig = 0;               /* 引擎给了第三组值 → 旧快照作废 */
            s_Preset = LIGHT_PRESET_ORIGINAL;
        }

        p = 0;
        p = AppendCStr(msg, p, "lighting: scene reopen amb=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_AMBIENT]);
        p = AppendCStr(msg, p, " tint=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_RED]);
        p = AppendCh(msg, p, ',', sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_GREEN]);
        p = AppendCh(msg, p, ',', sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), v[P_BLUE]);
        p = AppendCStr(msg, p, use_ours ? " -> ours" : " -> engine", sizeof(msg));
        p = AppendCStr(msg, p, " n=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), waited);
        LogLine(msg);
        return;
    }

    if (!started) return;                 /* 不在对局里不动手 */
    if (*(volatile int*)(size_t)ADDR_LOAD_PHASE) return;   /* 还在载入 */

    Light_ReadAll(v);
    taken = (s_WroteOk && !Light_Same(v, s_Wrote));
    if (!taken) return;

    /* 引擎改了光照 → 用它的值重算, 画面跟上。例外: 引擎把光照重置回地图自带值
     * 是"读档/重开", 不是地图动作, 盖回用户那组。读档的 ini 应用可能晚于场景
     * 就绪钩子, 落在这里, 所以这一条不能只写在重开分支里。 */
    use_ours = Light_UseOurs(v);
    Light_Apply(use_ours ? s_Wrote : v);
    s_Preset = LIGHT_PRESET_ORIGINAL;
}

/* 场景就绪钩子 (0x683E75: mov byte ptr [0xa8ed5c], 1 —— 新开局路径把"对局中"
 * 置 1 的指令)。只置标志, 真正的刷新留给 Lighting_Poll。
 * 两处被偷字节都是纯 mov (无 rel), 关闭时返回 0 重放是安全的。 */
extern "C" __declspec(dllexport) DWORD __cdecl Lighting_ScenStartHook(void* regs)
{
    (void)regs;
    if (!LightFix_Enabled()) return 0;     /* 关闭: 连 pending 都不置 */
    s_Pending = 1;
    s_PendWait = 0;
    return 0;                             /* 回放 mov byte ptr [0xa8ed5c], 1 */
}

/* 读档就绪钩子 (0x67E6C4: LoadGame (0x67E440) 体内 mov [0xa8ed5c], bl —— 读档
 * 路径把"对局中"置 1 的指令, bl 在 0x67E694 处置 1 后不被破坏, 恒为 1)。读档
 * 不经过新开局的 0x683E75, 漏了这条读档就完全没触发。 */
extern "C" __declspec(dllexport) DWORD __cdecl Lighting_LoadReadyHook(void* regs)
{
    (void)regs;
    if (!LightFix_Enabled()) return 0;
    s_Pending = 1;
    s_PendWait = 0;
    return 0;                             /* 回放 mov [0xa8ed5c], bl */
}

int Lighting_Preset(void)
{
    return s_Preset;
}

void Lighting_ApplyPreset(int preset)
{
    int v[LIGHT_PARAM_N];

    if (!Light_InGame()) return;
    Light_EnsureOrig();
    if (preset == LIGHT_PRESET_ORIGINAL)
    {
        Light_Apply(s_Orig);
        s_Preset = LIGHT_PRESET_ORIGINAL;
        return;
    }
    if (preset < 1 || preset >= LIGHT_PRESET_COUNT) return;

    /* 预设只覆盖环境光与三色, 等级/地面沿用当前值 (几个脚本本来也没写这两项)。 */
    Light_ReadAll(v);
    v[P_AMBIENT] = s_PresetTable[preset - 1].amb;
    v[P_RED]     = s_PresetTable[preset - 1].r;
    v[P_GREEN]   = s_PresetTable[preset - 1].g;
    v[P_BLUE]    = s_PresetTable[preset - 1].b;
    Light_Apply(v);
    s_Preset = preset;
}

int Lighting_Mode(void)
{
    return s_Mode;
}

/* 换调色方式: 值一个都不动, 只按新方式把同一组字段重算一遍, 方便直接对比。 */
void Lighting_SetMode(int mode)
{
    int v[LIGHT_PARAM_N];

    if (mode < 0 || mode >= LIGHT_MODE_COUNT) return;
    if (mode == s_Mode) return;
    s_Mode = mode;
    LogLine(mode == LIGHT_MODE_UNIFORM ? "lighting: mode uniform"
                                       : "lighting: mode percell");

    if (!Light_InGame()) return;
    Light_ReadAll(v);
    Light_Apply(v);
}

int Lighting_GetParam(int idx)
{
    if (idx < 0 || idx >= LIGHT_PARAM_N) return 0;
    return Light_Read(s_ParamOff[idx]);
}

void Lighting_SetParam(int idx, int value)
{
    int v[LIGHT_PARAM_N];

    if (idx < 0 || idx >= LIGHT_PARAM_N) return;
    if (value < 0) value = 0;
    if (value > LIGHT_PARAM_MAX) value = LIGHT_PARAM_MAX;

    Light_EnsureOrig();
    Light_ReadAll(v);
    v[idx] = value;
    Light_Apply(v);
    s_Preset = LIGHT_PRESET_CUSTOM;
}

void Lighting_SaveState(void)
{
    Light_ReadAll(s_Saved);
    s_SavedPreset = s_Preset;
    s_SavedMode = s_Mode;
}

void Lighting_RestoreState(void)
{
    if (!Light_InGame()) return;
    s_Mode = s_SavedMode;                 /* 先回方式再刷, 免得白刷一次 */
    Light_Apply(s_Saved);
    s_Preset = s_SavedPreset;
}
