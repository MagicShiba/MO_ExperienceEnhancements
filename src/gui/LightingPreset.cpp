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
 * 全是临时设置: 不写 ini, 不广播。取消菜单回滚到打开菜单时的值; 换到另一张
 * 地图时回到新地图自带的值; 读同一个场景的存档时保留用户改过的值 (引擎读档
 * 会把光照重置回地图自带值, 见 Lighting_Poll 的"用哪组值")。
 */

#include <windows.h>
#include "LightingPreset.h"
#include "EngineAddr.h"
#include "GdiText.h"
#include "LightConvFix.h"

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

/* 引擎当前值 == 地图自带值 (s_Orig) 而我们手上还留着用户改过的那组
 * = 引擎把光照重置成了地图自带值, 该盖回用户那组 (运行期改的光照不跟着
 * 存档走)。要求调用方已确认这是"引擎主动改字段"而非我们自己的写入。 */
static int Light_UseOurs(const int* v)
{
    return s_HaveOrig && Light_Same(v, s_Orig) && !Light_Same(s_Wrote, s_Orig);
}

/* 场景重开 (新开/读档/换图) 后按"引擎此刻给的值"重算一遍光照。
 *
 * 要解决的问题: 读档后地形/格子是按"重建那一刻的光照"建起来的, 而 LoadGame
 * (0x67E440) 不调 RecalcLighting —— 于是字段是对的, 画面却停在重建时的亮度。
 * 这里照抄手动改参数那条路: 用现有值走 写字段 + RecalcLighting + 全格 convert
 * 重建。
 *
 * 触发: 两个场景就绪钩子, 即引擎把"对局中" (0xA8ED5C) 置 1 的全部两条指令
 * (新开局 0x683E75 / 读档 0x67E6C4, 两条路径互不经过) 置 pending。
 *
 * 等什么: 过了载入闸、格子数组回来了才动手; 但**最多等 240 帧** —— 任何一个
 * 闸门卡住都会让 Poll 从此不干活, 到点就无视闸门硬刷一次。多刷一次只是白干
 * 一次活, 不刷就是永久错。
 *
 * 用哪组值: 引擎把光照重置成了地图自带值 (s_Orig) 而我们手上还留着用户改过的
 * 那组 → 盖回用户那组 (运行期改的光照不跟着存档走); 其它情况以引擎为准。 */
void Lighting_Poll(void)
{
    int started = Light_InGame();
    int v[LIGHT_PARAM_N];
    int taken, use_ours, waited;
    int wasAmb;
    char msg[192];
    unsigned p;

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

        wasAmb = s_WroteOk ? s_Wrote[P_AMBIENT] : -1;
        Light_Apply(use_ours ? s_Wrote : v);
        LightConv_RefreshNow();           /* 读档期间可能已按瞬态值建过 convert */

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
        p = AppendCh(msg, p, sizeof(msg), ',');
        p = AppendInt(msg, p, sizeof(msg), v[P_GREEN]);
        p = AppendCh(msg, p, sizeof(msg), ',');
        p = AppendInt(msg, p, sizeof(msg), v[P_BLUE]);
        p = AppendCStr(msg, p, " last=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), wasAmb);
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

    /* 引擎改了光照 → 用它的值重算, 画面跟上。
     * 例外: 引擎把光照重置回地图自带值是"读档/重开", 不是地图动作, 盖回用户
     * 那组。读档的 ini 应用可能晚于场景就绪钩子, 落在这里, 所以这一条不能只在
     * 重开分支写。 */
    use_ours = Light_UseOurs(v);
    Light_Apply(use_ours ? s_Wrote : v);
    s_Preset = LIGHT_PRESET_ORIGINAL;
}

/* 场景就绪钩子 (0x683E75: mov byte ptr [0xa8ed5c], 1 —— 新开局路径把"对局中"
 * 置 1 的指令)。只置标志, 真正的刷新留给 Lighting_Poll。 */
extern "C" __declspec(dllexport) DWORD __cdecl Lighting_ScenStartHook(void* regs)
{
    (void)regs;
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
    if (!Light_InGame()) return;
    if (preset == LIGHT_PRESET_ORIGINAL)
    {
        Light_EnsureOrig();
        Light_Apply(s_Orig);
        s_Preset = LIGHT_PRESET_ORIGINAL;
        return;
    }
    if (preset < 1 || preset >= LIGHT_PRESET_COUNT) return;

    Light_EnsureOrig();
    Light_Apply(s_PresetTable[preset - 1]);
    s_Preset = preset;
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
}

void Lighting_RestoreState(void)
{
    if (!Light_InGame()) return;
    Light_Apply(s_Saved);
    s_Preset = s_SavedPreset;
}
