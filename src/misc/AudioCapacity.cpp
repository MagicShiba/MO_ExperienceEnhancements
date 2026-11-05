/* AudioCapacity.cpp - 扩充本地音频资源, 不改模拟状态或音效优先级。 */
#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"

#define AUDIO_VOICES       32
#define AUDIO_MIN_VOICES   16
#define AUDIO_CACHE_BYTES  (8u * 1024u * 1024u)

typedef int (__fastcall *CreateVoicesFn)(void* device, int count);

static DWORD Audio_CreateVoices(void* regs, DWORD after)
{
    void* device = *(void**)((char*)regs + REG_OFF_ECX);
    int count = ((CreateVoicesFn)(size_t)FN_AUDIO_CREATE_VOICES)(device, AUDIO_VOICES);
    char msg[80];
    unsigned p = 0;
    p = AppendCStr(msg, p, "audio: voices=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), count);
    LogLine(msg);

    /* 部分创建仍可使用; 保留原调用方至少16个才启用声音的门槛。 */
    *(unsigned*)((char*)regs + REG_OFF_EAX) =
        count >= AUDIO_MIN_VOICES ? AUDIO_MIN_VOICES : count;
    return after;
}

extern "C" __declspec(dllexport) DWORD __cdecl AudioCapacity_VoiceInit(void* regs)
{
    return Audio_CreateVoices(regs, ADDR_AUDIO_VOICE_INIT + 5);
}

extern "C" __declspec(dllexport) DWORD __cdecl AudioCapacity_VoiceReinit(void* regs)
{
    return Audio_CreateVoices(regs, ADDR_AUDIO_VOICE_REINIT + 5);
}

extern "C" __declspec(dllexport) DWORD __cdecl AudioCapacity_Cache(void* regs)
{
    *(unsigned*)((char*)regs + REG_OFF_EDX) = AUDIO_CACHE_BYTES;
    LogLine("audio: sample cache budget=8MiB");
    return ADDR_AUDIO_CACHE_BUDGET + 5;
}
