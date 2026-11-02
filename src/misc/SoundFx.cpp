/* SoundFx.cpp - 按 sound.ini 的名字播引擎音效 (VocClass)。
 *
 * 名字 -> 下标走引擎自己的 FindIndex (大小写不敏感), 不自己扫表: 条目字段 Ares
 * 可能改过, 只有引擎的解析算权威。
 *
 * 播放在屏幕层, 没有"世界里的声源", 所以走 PlayGlobal 而不是 PlayAt, 距离感由
 * 音量与 pan 直接表达。音量参数是倍率, 见 EngineAddr.h。
 *
 * 循环音 (Control 含 loop) 必须自持一条 AudioController: PlayGlobal 把事件绑到
 * 它身上, 之后 End 才能淡出停掉; AudioIndex 必须填 ADDR_AUDIO_IDX_INST, 否则
 * End 认不出自己的事件, 静默空转。
 *
 * 纯本地发声: 只写音频事件表, 不碰模拟状态, 联机安全。
 */

#include <windows.h>
#include "SoundFx.h"
#include "EngineAddr.h"

/* VocClass::PlayGlobal: fastcall(ecx=下标, edx=pan, 栈=音量 float / 控制器),
 * 回归时弹 8 字节 (ret 8) —— 与 GCC 的 __fastcall 对栈参的处理一致。 */
typedef void (__fastcall *SfxPlayGlobalFn)(int idx, int pan, float vol,
                                           void* ctrl);

/* VocClass::FindIndex: fastcall(ecx=名字) -> 下标 / -1。 */
typedef int (__fastcall *SfxFindIndexFn)(const char* name);

/* AudioController::End: thiscall (ecx=this), 无栈参 (ret)。
 * 32 位下 thiscall 与"ecx 传第一个参数、edx 是个无关的寄存器"的 fastcall 同构。 */
typedef void (__fastcall *SfxCtrlEndFn)(void* self, void* edxUnused);

/* YRpp AudioController 的逐字段复制 (只在这里用, 所以不进 EngineAddr.h:
 * 那是"引擎地址与结构偏移"的出处, 这里要的是完整布局)。 */
typedef struct {
    void*  Event;
    unsigned Stamp;
    void*  EventType;
    void** AudioIndex;
    unsigned Unused;
} SfxCtrl;

#define SFX_CACHE_N   16
#define SFX_LOOP_N    2   /* 通道数, 与 SoundFx.h 的 SFX_LOOP_* 对齐 */

typedef struct {
    char name[24];
    int  idx;
} SfxCache;

static SfxCache s_Cache[SFX_CACHE_N];
static int      s_CacheN;
static SfxCtrl  s_Loop[SFX_LOOP_N];
static int      s_LoopIdx[SFX_LOOP_N] = {-1, -1};   /* 每通道在循环的音效下标, -1 = 无 */

int Sfx_Index(const char* name)
{
    int i;

    if (!name || !*name) return -1;
    for (i = 0; i < s_CacheN; i++)
        if (!lstrcmpiA(s_Cache[i].name, name)) return s_Cache[i].idx;

    /* 表未装载时返回 -1: 不写缓存, 下一次调用再问 (读到音效表是要等的)。 */
    i = ((SfxFindIndexFn)(size_t)FN_VOC_FINDINDEX)(name);
    if (i < 0) return -1;
    if (s_CacheN < SFX_CACHE_N)
    {
        lstrcpynA(s_Cache[s_CacheN].name, name, (int)sizeof(s_Cache[0].name));
        s_Cache[s_CacheN].idx = i;
        s_CacheN++;
    }
    return i;
}

/* 音量百分比 -> PlayGlobal 的倍率。夹在 0~200: 超过 2 倍只是削波, 还会盖住
 * 游戏里其它声音。 */
static float Sfx_Vol(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 200) vol = 200;
    return (float)vol / 100.0f;
}

int Sfx_Play(const char* name, int pan, int vol)
{
    int idx = Sfx_Index(name);

    if (idx < 0) return 0;
    ((SfxPlayGlobalFn)(size_t)FN_VOC_PLAYGLOBAL)(idx, pan, Sfx_Vol(vol), NULL);
    return 1;
}

int Sfx_LoopStart(int ch, const char* name, int pan, int vol)
{
    SfxCtrl* ctrl;
    int idx;

    if (ch < 0 || ch >= SFX_LOOP_N) return 0;
    idx = Sfx_Index(name);
    if (idx < 0) return 0;
    Sfx_LoopStop(ch);

    ctrl = &s_Loop[ch];
    ctrl->Event = NULL;
    ctrl->Stamp = 0;
    ctrl->EventType = NULL;
    ctrl->AudioIndex = (void**)ADDR_AUDIO_IDX_INST;
    ctrl->Unused = 0;

    ((SfxPlayGlobalFn)(size_t)FN_VOC_PLAYGLOBAL)(idx, pan, Sfx_Vol(vol), ctrl);
    /* PlayGlobal 末尾的 SetEvent 把事件写进控制器首字段: 这里还是空, 说明这次
     * 没起声 (声音总开关关着 / 采样没装载 / 条目 Limit 已满)。不认成功, 让调用
     * 方过一会儿再试。 */
    if (!ctrl->Event) return 0;
    s_LoopIdx[ch] = idx;
    return 1;
}

void Sfx_LoopStop(int ch)
{
    if (ch < 0 || ch >= SFX_LOOP_N || s_LoopIdx[ch] < 0) return;
    s_LoopIdx[ch] = -1;
    ((SfxCtrlEndFn)(size_t)FN_AUDIO_CTRL_END)(&s_Loop[ch], NULL);
    s_Loop[ch].Event = NULL;
    s_Loop[ch].EventType = NULL;
}
