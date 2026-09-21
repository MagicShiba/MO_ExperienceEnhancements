/*
 * Probe.cpp - MsgFont 阶段 1: 只读探针, 不改画面、不改状态。
 *
 * 钩住 MessageListClass::AddMessage 入口 (0x5D3BA0)。首条消息到达时
 * dump 矢量替换绘制所需的环境:
 *   DSurface::Composite 指针、像素格式(字段+虚函数交叉)、Pitch、尺寸、
 *   ColorScheme 数组与目标方案的 BaseColor、BitFont 字高、当前帧。
 * 全程返回 0, 原版行为不受任何影响。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 只链 kernel32)。
 */

#include <windows.h>

#define PROBE_VERSION   "0.1.0"

#define ADDR_DSURFACE_COMPOSITE  0x0088731Cu
#define ADDR_BITFONT_INSTANCE    0x0089C4D0u
#define ADDR_COLORSCHEME_ARRAY   0x00B054D0u
#define ADDR_CURRENT_FRAME       0x00A8ED84u

#define OFF_REG_ESP              0x14

#define STK_RETURN_ADDR          0x00
#define STK_ARG_NAME             0x04
#define STK_ARG_ID               0x08
#define STK_ARG_MESSAGE          0x0C
#define STK_ARG_CS               0x10
#define STK_ARG_STYLE            0x14
#define STK_ARG_TIMEOUT          0x18
#define STK_ARG_SP               0x1C

#define FONT_OFF_HEIGHT          0x1C
#define OFF_CSCHEME_ITEMS        0x04
#define OFF_CSCHEME_COUNT        0x10
#define OFF_CSCHEME_BASECOLOR    0x308

/* Surface 虚表偏移(按 YRpp/Surface.h 虚函数顺序数出):
 * GetBytesPerPixel=28, GetPitch=29, GetWidth=31, GetHeight=32 */
#define VT_GET_BPP               0x70
#define VT_GET_PITCH             0x74
#define VT_GET_WIDTH             0x7C
#define VT_GET_HEIGHT            0x80
/* XSurface::BytesPerPixel 字段: vptr(0) Width(4) Height(8) LockLevel(12) BPP(16) */
#define OFF_XSURF_BPP_FIELD      0x10

#define MAX_PATH_LEN             260
#define LOG_LINE_MAX             1024
#define MSG_LOG_CAP              200

/* ---------------- 无 CRT 工具 ---------------- */

static unsigned UtoA(unsigned v, char* p)
{
    char tmp[12];
    int i = 0, j = 0;
    if (!v) tmp[i++] = '0';
    while (v && i < 11) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) p[j++] = tmp[--i];
    p[j] = 0;
    return (unsigned)j;
}

static unsigned HtoA(unsigned v, char* p)
{
    static const char* hex = "0123456789ABCDEF";
    unsigned n = 0;
    int i;
    p[n++] = '0'; p[n++] = 'x';
    for (i = 28; i >= 0; i -= 4) p[n++] = hex[(v >> i) & 0xF];
    p[n] = 0;
    return n;
}

static unsigned AppendCStr(char* dst, unsigned pos, const char* src, unsigned cap)
{
    while (*src && pos < cap - 1) dst[pos++] = *src++;
    dst[pos] = 0;
    return pos;
}

static unsigned AppendCh(char* dst, unsigned pos, char c, unsigned cap)
{
    if (pos < cap - 1) { dst[pos++] = c; dst[pos] = 0; }
    return pos;
}

static int IsPlausiblePtr(const void* p)
{
    unsigned u = (unsigned)(size_t)p;
    return (u >= 0x00010000u && u < 0x7FF00000u && (u & 3u) == 0);
}

/* ---------------- 日志 ---------------- */

static HANDLE g_Log = NULL;
static char   g_GameDir[MAX_PATH_LEN];
static char   g_LogPath[MAX_PATH_LEN];

static void BuildPaths(void)
{
    char exe[MAX_PATH_LEN];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH_LEN);
    DWORD i;
    unsigned k = 0;

    if (n == 0 || n >= MAX_PATH_LEN) { exe[0] = 0; n = 0; }
    i = n;
    while (i > 0 && exe[i] != '\\' && exe[i] != '/') i--;
    exe[i] = 0;

    k = AppendCStr(g_GameDir, k, exe, MAX_PATH_LEN);

    k = AppendCStr(g_LogPath, 0, exe, MAX_PATH_LEN);
    AppendCStr(g_LogPath, k, "\\ProbeMsgFont.log", MAX_PATH_LEN);
}

static void LogLine(const char* body)
{
    char buf[LOG_LINE_MAX];
    unsigned n = 0;
    DWORD written = 0;
    const char* p;

    if (!g_Log) return;
    for (p = body; *p && n < LOG_LINE_MAX - 3; p++) buf[n++] = *p;
    buf[n++] = 13; buf[n++] = 10;

    SetFilePointer(g_Log, 0, NULL, FILE_END);
    WriteFile(g_Log, buf, n, &written, NULL);
}

static void LogOpen(void)
{
    if (g_Log) return;
    BuildPaths();
    g_Log = CreateFileA(g_LogPath, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_Log == INVALID_HANDLE_VALUE) { g_Log = NULL; return; }

    LogLine("==================================================");
    LogLine("MsgFontProbe " PROBE_VERSION " start");
    {
        char buf[512];
        unsigned p = 0;
        p = AppendCStr(buf, p, "log file: ", sizeof(buf));
        p = AppendCStr(buf, p, g_LogPath, sizeof(buf));
        LogLine(buf);
    }
}

/* ---------------- 环境 dump ---------------- */

typedef int (__fastcall *GetIntFn)(void* t, void* edxUnused);

static int CallGetter(void* obj, unsigned vtOff)
{
    unsigned char* vt;
    GetIntFn fn;
    if (!obj || !IsPlausiblePtr(obj)) return -1;
    vt = *(unsigned char**)obj;
    if (!vt || !IsPlausiblePtr(vt)) return -2;
    fn = (GetIntFn)(*(void**)(vt + vtOff));
    if (!fn) return -3;
    return fn(obj, NULL);
}

static void DumpEnv(unsigned retAddr, unsigned cs, unsigned timeout, unsigned sp)
{
    char b1[16], b2[16], b3[16], b4[16];
    char buf[512];
    unsigned p;
    void* surf;
    void* font;
    void** items;
    int count;

    surf  = *(void**)ADDR_DSURFACE_COMPOSITE;
    font  = *(void**)ADDR_BITFONT_INSTANCE;
    items = *(void***)(ADDR_COLORSCHEME_ARRAY + OFF_CSCHEME_ITEMS);
    count = *(int*)(ADDR_COLORSCHEME_ARRAY + OFF_CSCHEME_COUNT);

    /* 画布 */
    p = 0;
    p = AppendCStr(buf, p, "env: composite=", sizeof(buf));
    p += HtoA((unsigned)(size_t)surf, buf + p);
    p = AppendCStr(buf, p, " bppField=", sizeof(buf));
    p += UtoA((unsigned)(surf ? *(int*)((char*)surf + OFF_XSURF_BPP_FIELD) : 0xFFFFFFFFu), buf + p);
    p = AppendCStr(buf, p, " bppVirt=", sizeof(buf));
    p += UtoA((unsigned)CallGetter(surf, VT_GET_BPP), buf + p);
    p = AppendCStr(buf, p, " pitch=", sizeof(buf));
    p += UtoA((unsigned)CallGetter(surf, VT_GET_PITCH), buf + p);
    LogLine(buf);

    p = 0;
    p = AppendCStr(buf, p, "env: width=", sizeof(buf));
    p += UtoA((unsigned)CallGetter(surf, VT_GET_WIDTH), buf + p);
    p = AppendCStr(buf, p, " height=", sizeof(buf));
    p += UtoA((unsigned)CallGetter(surf, VT_GET_HEIGHT), buf + p);
    p = AppendCStr(buf, p, " font=", sizeof(buf));
    p += HtoA((unsigned)(size_t)font, buf + p);
    p = AppendCStr(buf, p, " lineH=", sizeof(buf));
    p += UtoA(font ? (unsigned)*(int*)((char*)font + FONT_OFF_HEIGHT) : 0xFFFFFFFFu, buf + p);
    p = AppendCStr(buf, p, " frame=", sizeof(buf));
    p += UtoA(*(unsigned*)ADDR_CURRENT_FRAME, buf + p);
    LogLine(buf);

    /* 颜色 */
    p = 0;
    p = AppendCStr(buf, p, "env: schemes=", sizeof(buf));
    p += UtoA((unsigned)count, buf + p);
    p = AppendCStr(buf, p, " firstMsgCs=", sizeof(buf));
    p += UtoA(cs, buf + p);
    if (items && count > 0 && count < 1000 && (int)cs >= 0 && (int)cs < count && items[cs])
    {
        unsigned char* base = (unsigned char*)items[cs] + OFF_CSCHEME_BASECOLOR;
        p = AppendCStr(buf, p, " baseColor=R", sizeof(buf));
        UtoA(base[0], b1); UtoA(base[1], b2); UtoA(base[2], b3);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " G", sizeof(buf));
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " B", sizeof(buf));
        p = AppendCStr(buf, p, b3, sizeof(buf));
    }
    else
    {
        p = AppendCStr(buf, p, " baseColor=<unavailable>", sizeof(buf));
    }
    LogLine(buf);

    /* 首条消息 */
    p = 0;
    p = AppendCStr(buf, p, "env: firstMsg ret=", sizeof(buf));
    p += HtoA(retAddr, buf + p);
    p = AppendCStr(buf, p, " timeout=", sizeof(buf));
    UtoA(timeout, b4);
    p = AppendCStr(buf, p, b4, sizeof(buf));
    p = AppendCStr(buf, p, " sp=", sizeof(buf));
    p += UtoA(sp, buf + p);
    LogLine(buf);

    (void)b1; (void)b2; (void)b3;
}

/* ---------------- 钩子 ---------------- */

static int g_InitDone = 0;
static int g_EnvDone  = 0;
static unsigned g_MsgCount = 0;

extern "C" __declspec(dllexport) DWORD __cdecl Probe_AddMessageHook(void* regs)
{
    unsigned char* esp;
    unsigned retAddr, cs, timeout, sp;

    if (!g_InitDone) { g_InitDone = 1; LogOpen(); }
    if (!g_Log) return 0;

    esp = *(unsigned char**)((char*)regs + OFF_REG_ESP);
    if (!esp || !IsPlausiblePtr(esp)) return 0;

    retAddr = *(unsigned*)(esp + STK_RETURN_ADDR);
    cs      = *(unsigned*)(esp + STK_ARG_CS);
    timeout = *(unsigned*)(esp + STK_ARG_TIMEOUT);
    sp      = *(unsigned*)(esp + STK_ARG_SP);

    g_MsgCount++;

    if (!g_EnvDone)
    {
        g_EnvDone = 1;
        DumpEnv(retAddr, cs, timeout, sp);
    }
    else if (g_MsgCount <= MSG_LOG_CAP)
    {
        char b1[16], b2[16], b3[16], b4[16], buf[256];
        unsigned p = 0;
        p = AppendCh(buf, p, '#', sizeof(buf));
        p += UtoA(g_MsgCount, buf + p);
        p = AppendCStr(buf, p, " ret=", sizeof(buf));
        p += HtoA(retAddr, buf + p);
        p = AppendCStr(buf, p, " cs=", sizeof(buf));
        UtoA(cs, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " to=", sizeof(buf));
        UtoA(timeout, b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " sp=", sizeof(buf));
        UtoA(sp, b3);
        p = AppendCStr(buf, p, b3, sizeof(buf));
        (void)b4;
        LogLine(buf);
    }

    return 0;
}

extern "C" BOOL __stdcall DllMain(HINSTANCE h, DWORD reason, LPVOID res)
{
    (void)h; (void)res;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
