/* SoundFx.cpp - 按 sound.ini 条目名调用引擎全局音效, 不碰模拟状态。
 * 名字由引擎解析, 音量与声像直接传给 PlayGlobal。
 * 循环音自持 AudioController; 调用方检查存活并限频重起, 不修改条目优先级。
 */

#include <windows.h>
#include "SoundFx.h"
#include "EngineAddr.h"

/* PlayGlobal: fastcall(ecx=下标, edx=pan, 栈=音量/控制器), ret 8。 */
typedef void (__fastcall *SfxPlayGlobalFn)(int idx, int pan, float vol,
                                           void* ctrl);

/* VocClass::FindIndex: fastcall(ecx=名字) -> 下标 / -1。 */
typedef int (__fastcall *SfxFindIndexFn)(const char* name);

/* 无栈参的 thiscall 用 fastcall 传 this, edx 不使用。 */
typedef void (__fastcall *SfxCtrlEndFn)(void* self, void* edxUnused);
typedef void* (__fastcall *SfxCtrlGetEventFn)(void* self, void* edxUnused);

/* YRpp AudioController 布局, 20 字节。 */
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
static int      s_LoopActive[SFX_LOOP_N];           /* 通道持有控制器 */

int Sfx_Index(const char* name)
{
    int i;

    if (!name || !*name) return -1;
    for (i = 0; i < s_CacheN; i++)
        if (!lstrcmpiA(s_Cache[i].name, name)) return s_Cache[i].idx;

    /* 失败不缓存, 允许音效表装载后重试。 */
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
    /* 未绑定事件时由调用方限频重试。 */
    if (!ctrl->Event) return 0;

    s_LoopActive[ch] = 1;
    return 1;
}

int Sfx_LoopAlive(int ch)
{
    void* event;

    if (ch < 0 || ch >= SFX_LOOP_N || !s_LoopActive[ch]) return 0;
    /* 引擎核对类型与创建代次, 不能把复用后的事件当成自己的继续播放。 */
    event = ((SfxCtrlGetEventFn)(size_t)FN_AUDIO_CTRL_GETEVENT)(&s_Loop[ch], NULL);
    if (event && *(unsigned*)((char*)event + OFF_AUDIO_EVENT_STATE) != 4)
        return 1;

    /* GetEvent 可能已清 Event; Stop 仍负责清通道占用与剩余控制器字段。 */
    Sfx_LoopStop(ch);
    return 0;
}

void Sfx_LoopStop(int ch)
{
    if (ch < 0 || ch >= SFX_LOOP_N || !s_LoopActive[ch]) return;
    s_LoopActive[ch] = 0;
    /* 设备关闭时不访问旧事件, 只清本地句柄。 */
    if (*(volatile unsigned*)(size_t)ADDR_AUDIO_ENABLED)
        ((SfxCtrlEndFn)(size_t)FN_AUDIO_CTRL_END)(&s_Loop[ch], NULL);
    s_Loop[ch].Event = NULL;
    s_Loop[ch].EventType = NULL;
}
