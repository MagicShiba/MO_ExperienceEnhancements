/* AudioCapacity.cpp - 扩充本地音频资源, 不改模拟状态或音效优先级。 */
#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"

#define AUDIO_VOICES         32
#define AUDIO_NATIVE_VOICES  16
#define AUDIO_MIN_VOICES     16
#define AUDIO_CACHE_BYTES    (8u * 1024u * 1024u)
#define AUDIO_NATIVE_CACHE   (1u * 1024u * 1024u)
#define AC_SECTION           "Features"
#define AC_KEY_ENABLE        "AudioCapacity"

typedef int (__fastcall *CreateVoicesFn)(void* device, int count);

/* 音频设备初始化早于 UI 帧的 MOE_EnsureInit, 开关必须在钩子里自己读 ini。 */
static int Audio_CapacityOn(void)
{
    static int logged = 0;
    int on;
    if (!g_IniPath[0]) BuildPaths();
    GtEnsureLog();
    on = GetPrivateProfileIntA(AC_SECTION, AC_KEY_ENABLE, 1, g_IniPath) ? 1 : 0;
    if (!logged)
    {
        logged = 1;
        LogLine(on ? "audio: capacity ON" : "audio: capacity OFF (ini)");
    }
    return on;
}

static DWORD Audio_CreateVoices(void* regs, DWORD after)
{
    void* device = *(void**)((char*)regs + REG_OFF_ECX);
    int on = Audio_CapacityOn();
    int count = ((CreateVoicesFn)(size_t)FN_AUDIO_CREATE_VOICES)(device,
                    on ? AUDIO_VOICES : AUDIO_NATIVE_VOICES);
    char msg[80];
    unsigned p = 0;
    p = AppendCStr(msg, p, "audio: voices=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), count);
    LogLine(msg);

    /* 扩容时部分创建仍可使用, 但调用方要 EAX>=16 才启用声音; 关闭时交回实际
       创建数, 与被跳过的原 call 返回值一致。 */
    *(unsigned*)((char*)regs + REG_OFF_EAX) =
        (on && count >= AUDIO_MIN_VOICES) ? AUDIO_MIN_VOICES : (unsigned)count;
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
    int on = Audio_CapacityOn();
    *(unsigned*)((char*)regs + REG_OFF_EDX) =
        on ? AUDIO_CACHE_BYTES : AUDIO_NATIVE_CACHE;
    if (on) LogLine("audio: sample cache budget=8MiB");
    return ADDR_AUDIO_CACHE_BUDGET + 5;
}
