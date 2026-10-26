/* LightingPreset.cpp - 游戏内光照预设。
 *
 * 地图 ini 的 [Lighting] 段 (Ambient / Red / Green / Blue) 在载入时被引擎读进
 * ScenarioClass 的几个字段 (见 EngineAddr.h 的 OFF_SCEN_*)。这里做的事就是改
 * 这些字段, 再调 ScenarioClass::RecalcLighting 让引擎重算格子光照与调色板 ——
 * 全是客户端渲染量, 不碰模拟状态, 联机安全。
 *
 * 预设数值来自地图编辑器脚本 "创造{早上,下午,黄昏,晚上}光照.fscript": 那几个
 * 脚本写的正是地图 ini 的 [Lighting] 段, 四个预设之间只有 Ambient 与
 * Red/Green/Blue 不同, 所以这里只改这四个值, 不动 Level/Ground/Ion*。
 *
 * 全是临时设置: 不写 ini, 不广播。取消菜单回滚到打开菜单时的值, 换局后回到
 * 新地图自带的值。
 */

#include <windows.h>
#include "LightingPreset.h"
#include "EngineAddr.h"

/* 参数顺序: 环境光 / 红 / 绿 / 蓝。单位 = 引擎字段单位 (ini 值 ×100)。 */
enum { P_AMBIENT = 0, P_RED = 1, P_GREEN = 2, P_BLUE = 3 };

/* 预设值, 单位同上。下标 = 预设号 - 1 (标准/早上/下午/黄昏/晚上);
 * 原图不用表 —— 用地图自带值。 */
static const int s_PresetTable[LIGHT_PRESET_COUNT - 1][LIGHT_PARAM_N] = {
    { 100, 100, 100, 100 },   /* 标准: 环境光与三色都是 1.00 */
    {  75, 109,  80,  49 },   /* 早上 */
    { 100, 108,  94,  68 },   /* 下午 */
    {  35,  51,  46, 141 },   /* 黄昏 */
    {  75,  71,  81, 119 }    /* 晚上 */
};

/* 每个参数的引擎字段偏移 (读值用 AmbientOriginal, 它与 Current 始终同步)。 */
static const int s_ParamOff[LIGHT_PARAM_N] = {
    OFF_SCEN_AMBIENT_ORIG, OFF_SCEN_TINT_R, OFF_SCEN_TINT_G, OFF_SCEN_TINT_B
};

static int s_Orig[LIGHT_PARAM_N];   /* 地图自带值 (首次改动前快照一次) */
static int s_Saved[LIGHT_PARAM_N];  /* ESC 菜单"取消"用 */
static int s_Wrote[LIGHT_PARAM_N];  /* 上次写进引擎的值 */
static int s_SavedPreset = LIGHT_PRESET_ORIGINAL;
static int s_Preset = LIGHT_PRESET_ORIGINAL;
static int s_HaveOrig = 0;          /* s_Orig 是否已快照 */
static int s_WroteOk = 0;           /* s_Wrote 是否有效 */
static int s_LastFrame = -1;        /* 上次看到的逻辑帧号 (倒退 = 场景重开) */
static int s_Pending = 0;           /* 场景重开了, 等一下再恢复光照 */
static int s_Wait = 0;              /* 场景重开后等几帧 (让引擎把场景摆好) */
static int s_UserSet = 0;           /* 用户改过光照, 还没被恢复掉 */

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

/* 读出当前生效的四个值。 */
static void Light_ReadAll(int* v)
{
    int i;

    for (i = 0; i < LIGHT_PARAM_N; i++)
        v[i] = Light_Read(s_ParamOff[i]);
}

/* 写回四个值。环境光要写三处: Current==Target 引擎才不做渐变 (立刻生效),
 * 而 UpdateLighting 的普通分支每次都把 Target 复位成 Original, 所以 Original
 * 也得跟着改。 */
static void Light_WriteAll(const int* v)
{
    Light_Write(OFF_SCEN_AMBIENT_ORIG, v[P_AMBIENT]);
    Light_Write(OFF_SCEN_AMBIENT_CUR,  v[P_AMBIENT]);
    Light_Write(OFF_SCEN_AMBIENT_TGT,  v[P_AMBIENT]);
    Light_Write(OFF_SCEN_TINT_R, v[P_RED]);
    Light_Write(OFF_SCEN_TINT_G, v[P_GREEN]);
    Light_Write(OFF_SCEN_TINT_B, v[P_BLUE]);
}

/* 重算格子光照 + 调色板, 并让地图面重画 (RecalcLighting 内部都做了)。
 * 引擎的 tint 单位是"真实值 ×1000" (上限 1000), 字段是真实值 ×100。 */
static void Light_Refresh(void)
{
    FnRecalcLighting fn;
    int r, g, b;

    if (!Light_Scen()) return;
    r = Light_Read(OFF_SCEN_TINT_R);
    g = Light_Read(OFF_SCEN_TINT_G);
    b = Light_Read(OFF_SCEN_TINT_B);
    fn = (FnRecalcLighting)(size_t)FN_RECALC_LIGHTING;
    fn(r * 10, g * 10, b * 10, 0);
}

/* 套用一组值。 */
static void Light_Apply(const int* v)
{
    int i;

    Light_WriteAll(v);
    for (i = 0; i < LIGHT_PARAM_N; i++) s_Wrote[i] = v[i];
    s_WroteOk = 1;
    Light_Refresh();
}

/* 第一次改动之前快照一次地图自带的值 —— 这样"恢复原图"不必依赖引擎读 ini
 * 与进局时序的先后, 快照到的就是引擎当时给出的值。 */
static void Light_EnsureOrig(void)
{
    if (s_HaveOrig) return;
    Light_ReadAll(s_Orig);
    s_HaveOrig = 1;
}

static int Light_Same(const int* a, const int* b)
{
    int i;

    for (i = 0; i < LIGHT_PARAM_N; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* 场景重开 (新开/读档) 后把光照恢复回地图原值。
 *
 * 判据是"引擎接管了光照字段": 引擎重建场景时会用地图 ini (或存档里的值)
 * 重写那几处, 与我们最后写的不同。这个判据不依赖时序 —— 引擎读 ini 与
 * 进局谁先谁后都无所谓。
 *
 * 恢复成什么:
 *   - 引擎给的值 == 我们最后写的那组 → 场景重开了但没改光照 (存档里存的是
 *     我们改过的), 回到地图原值 (s_Orig);
 *   - 否则 → 引擎给的就是这份存档/这张地图该有的值, 以它为准。
 *
 * 恢复 = 写全部六个字段 + RecalcLighting, 格子光照与调色板一起回来;
 * 只改字段不重算的话画面还是旧的 (看着像"恢复不彻底")。
 * 场景重开本身用逻辑帧号倒退来认 (引擎复用 Scenario 对象, 指针不变)。 */
void Lighting_Poll(void)
{
    int started = Light_InGame();
    int frame = *(volatile int*)(size_t)ADDR_FRAME;
    int v[LIGHT_PARAM_N];
    int taken;

    if (s_LastFrame >= 0 && frame < s_LastFrame)
        s_Pending = 1;                    /* 帧号倒退 = 场景重开 */
    s_LastFrame = frame;
    if (!started) return;                 /* 不在对局里不动手 */
    if (*(volatile int*)(size_t)ADDR_LOAD_PHASE) return;   /* 还在载入 */

    Light_ReadAll(v);
    taken = (s_WroteOk && !Light_Same(v, s_Wrote));

    if (s_Pending)
    {
        if (++s_Wait < 3) return;         /* 等引擎把场景摆好 */
        s_Pending = 0;
        s_Wait = 0;
        /* 只有用户改过光照才需要按"场景重开"恢复: 帧号倒退偶尔不只发生在
         * 读档 (换图/进任务简报也会), 无条件恢复会把用户的预设刷掉。 */
        if (!s_UserSet) return;
        s_UserSet = 0;
        Light_ReadAll(v);
        if (!s_HaveOrig)
        {
            Light_ReadAll(s_Orig);        /* 没有地图原值快照 → 以引擎为准 */
            s_HaveOrig = 1;
        }
        else if (!s_WroteOk || !Light_Same(v, s_Wrote))
        {
            Light_ReadAll(s_Orig);        /* 引擎给了自己的值 → 以它为准 */
        }
        Light_Apply(s_Orig);
        s_Preset = LIGHT_PRESET_ORIGINAL;
        return;
    }
    if (!taken) return;

    /* 引擎改了光照 (读档/换图/地图动作) → 把当前值写回并重算, 画面跟上。
     * 地图原值快照不动 (它仍然是"原图")。 */
    Light_Apply(v);
    s_UserSet = 0;
    s_Preset = LIGHT_PRESET_ORIGINAL;
}

int Lighting_Preset(void)
{
    return s_Preset;
}

void Lighting_ApplyPreset(int preset)
{
    if (!Light_InGame()) return;
    if (preset == LIGHT_PRESET_ORIGINAL)
    {
        Light_EnsureOrig();
        Light_Apply(s_Orig);
        s_Preset = LIGHT_PRESET_ORIGINAL;
        s_UserSet = 0;
        return;
    }
    if (preset < 1 || preset >= LIGHT_PRESET_COUNT) return;

    Light_EnsureOrig();
    Light_Apply(s_PresetTable[preset - 1]);
    s_Preset = preset;
    s_UserSet = 1;
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
    s_UserSet = 1;
}

void Lighting_SaveState(void)
{
    Light_ReadAll(s_Saved);
    s_SavedPreset = s_Preset;
}

void Lighting_RestoreState(void)
{
    if (!Light_InGame()) return;
    Light_Apply(s_Saved);
    s_Preset = s_SavedPreset;
}
