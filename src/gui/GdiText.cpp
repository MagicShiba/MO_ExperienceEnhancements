/* GdiText.cpp - 见 GdiText.h。
 * 全 DLL 共享底座: 无 CRT 工具 / 日志 / 字体解析 / GDI 光栅 / 16 位混合 /
 * 游戏窗口与鼠标坐标 / 引擎时钟 / 画面 (DSURFACE) 虚表。
 * 虚表槽号只有这一份, 各模块不要自己抄。
 */
#include "GdiText.h"

/* ---------------- 无 CRT 工具 ---------------- */

unsigned UtoA(unsigned v, char* p)
{
    char tmp[12];
    int i = 0, j = 0;
    if (!v) tmp[i++] = '0';
    while (v && i < 11) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i) p[j++] = tmp[--i];
    p[j] = 0;
    return (unsigned)j;
}

unsigned HtoA(unsigned v, char* p)
{
    static const char* hex = "0123456789ABCDEF";
    unsigned n = 0;
    int i;
    p[n++] = '0'; p[n++] = 'x';
    for (i = 28; i >= 0; i -= 4) p[n++] = hex[(v >> i) & 0xF];
    p[n] = 0;
    return n;
}

unsigned AppendCStr(char* dst, unsigned pos, const char* src, unsigned cap)
{
    while (*src && pos < cap - 1) dst[pos++] = *src++;
    dst[pos] = 0;
    return pos;
}

unsigned AppendCh(char* dst, unsigned pos, char c, unsigned cap)
{
    if (pos < cap - 1) { dst[pos++] = c; dst[pos] = 0; }
    return pos;
}

/* 有符号整数追加 (坐标/界值可能为负)。 */
unsigned AppendInt(char* dst, unsigned pos, unsigned cap, int v)
{
    char n[16];
    if (v < 0) { pos = AppendCh(dst, pos, '-', cap); v = -v; }
    UtoA((unsigned)v, n);
    return AppendCStr(dst, pos, n, cap);
}

int IsPlausiblePtr(const void* p)
{
    unsigned u = (unsigned)(size_t)p;
    return (u >= 0x00010000u && u < 0x7FF00000u && (u & 3u) == 0);
}

/* 真实可读长度 (见 .h 说明)。只做一次 VirtualQuery, 取该区剩余可读字节。 */
unsigned MemReadableBytes(const void* p, unsigned maxBytes)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned long long left;

    if (!IsPlausiblePtr(p) || !maxBytes) return 0;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (!mbi.Protect) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    left = (unsigned long long)(size_t)mbi.BaseAddress + mbi.RegionSize -
           (unsigned long long)(size_t)p;
    return (left > maxBytes) ? maxBytes : (unsigned)left;
}

/* 无 CRT: 编译器可能把拷贝/清零循环优化成 memcpy/memset 调用, 自备实现。
 * 注意: 实现体必须用 volatile, 否则 -O2 会把函数体自身再认作标准语义、
 * 优化成一次对自己的调用 (实测 ToolTip 假对象清零因此死循环)。 */
extern "C" void* memcpy(void* dst, const void* src, unsigned n)
{
    volatile unsigned char* d = (volatile unsigned char*)dst;
    volatile const unsigned char* s = (volatile const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}

extern "C" void* memset(void* s, int c, unsigned n)
{
    volatile unsigned char* p = (volatile unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

extern "C" void* memmove(void* dst, const void* src, unsigned n)
{
    volatile unsigned char* d = (volatile unsigned char*)dst;
    volatile const unsigned char* s = (volatile const unsigned char*)src;
    if (d < s)
    {
        while (n--) *d++ = *s++;
    }
    else if (d > s)
    {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

/* ---------------- 日志 ---------------- */

HANDLE g_Log = NULL;
char   g_GameDir[MAX_PATH_LEN];
char   g_IniPath[MAX_PATH_LEN];
char   g_LogPath[MAX_PATH_LEN];
int    g_LogDetail = 0;

void BuildPaths(void)
{
    char exe[MAX_PATH_LEN];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH_LEN);
    DWORD i;
    unsigned k;

    if (n == 0 || n >= MAX_PATH_LEN) { exe[0] = 0; n = 0; }
    i = n;
    while (i > 0 && exe[i] != '\\' && exe[i] != '/') i--;
    exe[i] = 0;

    AppendCStr(g_GameDir, 0, exe, MAX_PATH_LEN);   /* 根目录: 各模块拼路径都用它 */
    k = AppendCStr(g_LogPath, 0, exe, MAX_PATH_LEN);
    AppendCStr(g_LogPath, k, "\\MOExperience.log", MAX_PATH_LEN);
    k = AppendCStr(g_IniPath, 0, exe, MAX_PATH_LEN);
    AppendCStr(g_IniPath, k, "\\MOExperience.ini", MAX_PATH_LEN);
}

/* 游戏根目录 (exe 所在处, 无尾分隔符; 取不到 = 空串)。首次调用自动建路径。 */
const char* GtGameDir(void)
{
    static int built = 0;
    if (!built) { built = 1; BuildPaths(); }
    return g_GameDir;
}

/* ---------------- 游戏窗口与鼠标坐标 ----------------
 * WWMouse 的 +0x1C/+0x20 是窗口客户区像素; 引擎的命中/投影/UI 都在引擎逻辑
 * 坐标系里 (窗口 Composite = GtGameW/H)。实测两者恒等 —— 引擎窗口就是按游戏
 * 分辨率建的 (原生 1920=1920, 虚拟分辨率下客户区 1440=Composite 1440), 所以
 * 不做窗口→游戏的换算, 直接返回原始坐标。
 * (曾按 游戏面/客户区 比缩放: 游戏面取 Primary, 虚拟分辨率/全屏上采样下
 *  Primary 是显示器尺寸 2560x1600, 会把鼠标整体放大 → 线与光标错位。)
 *
 * 游戏逻辑尺寸 GtGameW/H 由 MOE 消息帧每帧喂 (窗口 Composite 尺寸), 供
 * ScaleBar/PauseView 等按屏幕宽高定位的 UI 用。 */

static int g_GameW = 0, g_GameH = 0;

int GtGameW(void) { return g_GameW; }
int GtGameH(void) { return g_GameH; }
void GtSetGameSize(int w, int h) { g_GameW = w; g_GameH = h; }

#define GT_GAME_HWND_ADDR 0x00B73550u   /* 引擎存的主窗句柄 (同 ChatEdit/FullMap) */

/* 前台判定: 窗口化切走时引擎停渲染, 焦点追踪日志靠它 (见 MsgText 悬停淡入) */
int GtGameFocused(void)
{
    HWND h;
    if (!IsPlausiblePtr((void*)GT_GAME_HWND_ADDR)) return 0;
    h = *(HWND*)GT_GAME_HWND_ADDR;
    if (!h || !IsWindow(h)) return 0;
    return (GetForegroundWindow() == h) ? 1 : 0;
}

int GtMouseGameXY(int* x, int* y)
{
    void* m = *(void**)0x00887640u;   /* WWMouse::Instance */
    int mx, my;
    static int s_mapLogged = 0;
    if (!m || !IsPlausiblePtr(m)) return 0;
    mx = *(int*)((char*)m + 0x1C);
    my = *(int*)((char*)m + 0x20);
    if (!s_mapLogged)
    {
        char mb[128], m1[16], m2[16];
        unsigned q = 0;
        s_mapLogged = 1;
        q = AppendCStr(mb, q, "mouse: raw=", sizeof(mb));
        UtoA((unsigned)(mx < 0 ? 0 : mx), m1); q = AppendCStr(mb, q, m1, sizeof(mb));
        q = AppendCh(mb, q, ',', sizeof(mb));
        UtoA((unsigned)(my < 0 ? 0 : my), m2); q = AppendCStr(mb, q, m2, sizeof(mb));
        LogLine(mb);
    }
    *x = mx; *y = my;
    return 1;
}

void LogLine(const char* body)
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

/* ---------------- 帧内耗时探针 ----------------
 * 定位"开了 MOE 掉一半帧"这类问题时, 只知道总帧率没用, 得知道每帧这 2~5ms 花在
 * 哪一段。这里按段累计 QPC, 每 2s 输出一次 us/帧 (除以帧数, 所以与帧率无关)。
 * 关闭时 Perf_Now 返回 0, 全部调用点立刻短路, 常驻无成本。 */
static int       s_PfInit = 0;
static int       s_PfOn = 0;
static int       s_PfFps = 0;   /* 1 = Perf_Frame 结算帧率 (Perf=1/2 都开) */
static long long s_PfQpf = 0;
static long long s_PfAcc[PF_COUNT];
static unsigned  s_PfCnt[PF_COUNT];
static long long s_PfLast = 0;
static unsigned  s_PfFrames = 0;
/* 帧间隔直方图 (50us 一档, 0~20ms): 用来报"中位帧率"。
 * 1 秒窗口的帧数 f= 会被载入/GC 之类的一次性卡顿拉低 (窗口内有几个 60ms 的尖峰
 * 就掉 10 帧), 同一台机器两轮读数能差 20 —— 中位数不受这些尖峰影响, 是可比的口径。
 * 只增一个 int, 可忽略。 */
#define PF_HIST_N 400
static unsigned s_PfHist[PF_HIST_N];
static long long s_PfLastFrame = 0;

/* 槽位名表必须与枚举逐项对齐。数组故意不写长度, 下面用一句编译期断言卡数量:
 * 漏一项会让后面的名字整体错位, 日志就会张冠李戴, 而这种错不会报错。 */
static const char* const kPfName[] =
{
    "gate", "fc", "gs", "prep", "fm", "sb", "ss",
    "aw", "bq", "pc", "so", "rp", "tc", "fa", "am",
    "sa", "dp", "ms", "ot", "wpc", "mlc", "sws",
    "ra", "ps", "ag", "ob", "light",
    "pv", "shot", "vts", "pub", "mhk", "pwr", "pipb",
    "skin", "pvp",
    "msg", "msgtext", "msgswp", "msgdraw", "msgtail",
    "blitsb", "aftermap", "renderin", "vecblit", "vecgdim", "pprev", "shroud",
    "swpm", "swpb", "swpt",
    "vprint", "vsimple", "vprtO", "vsmpO", "vb1", "vb2", "clr",
    "sbSS", "sbPWR", "sbPIP", "sbFM",
    "dSWS", "dSO", "dPC", "dRP", "dFA", "dSA", "dTC", "dDP", "dWR", "dAG", "dRAD",
    "tCE", "tPWR", "tSB", "tPV", "tSHOT",
    "vhit", "vmiss", "lock", "sbSet", "sbFill", "sbLbl",
    "swsB", "swsL"
};
typedef char kPfName_align_check[
    (sizeof(kPfName) / sizeof(kPfName[0]) == (unsigned)PF_COUNT) ? 1 : -1];

void Perf_EnsureInit(void)
{
    LARGE_INTEGER f;
    int i;
    if (s_PfInit) return;
    s_PfInit = 1;
    if (!g_IniPath[0]) BuildPaths();
    /* 1 = 全量分段账; 2 = 只记帧率 (几乎零开销, 平时判断"掉没掉帧"用它)。 */
    {
        int mode = GetPrivateProfileIntA("Log", "Perf", 0, g_IniPath);
        s_PfOn = (mode == 1) ? 1 : 0;
        s_PfFps = (mode >= 1) ? 1 : 0;
    }
    if (!s_PfFps) return;
    if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) s_PfQpf = f.QuadPart;
    for (i = 0; i < PF_COUNT; i++) s_PfAcc[i] = 0;
    LogLine(s_PfOn ? "perf: probe ON (us/frame, 2s window)"
                   : "perf: fps only (no per-segment timing)");
}

long long Perf_Now(void)
{
    LARGE_INTEGER t;
    if (!s_PfOn) return 0;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void Perf_Add(int slot, long long t0)
{
    LARGE_INTEGER t;
    if (!t0 || slot < 0 || slot >= PF_COUNT) return;
    QueryPerformanceCounter(&t);
    s_PfAcc[slot] += t.QuadPart - t0;
    if (slot < (int)PF_COUNT) s_PfCnt[slot]++;
}

void Perf_AddDelta(int slot, long long dt)
{
    if (!s_PfOn || slot < 0 || slot >= PF_COUNT || dt <= 0) return;
    s_PfAcc[slot] += dt;
    s_PfCnt[slot]++;
}

void Perf_Hit(int slot)
{
    if (!s_PfOn || slot < 0 || slot >= PF_COUNT) return;
    s_PfCnt[slot]++;
}

void Perf_Frame(void)
{
    LARGE_INTEGER t;
    long long now, dt;
    unsigned i, ms;
    char buf[LOG_LINE_MAX], b1[16];
    unsigned p = 0;

    if (!s_PfFps || !s_PfQpf) return;
    QueryPerformanceCounter(&t);
    now = t.QuadPart;
    s_PfFrames++;
    /* 直方图: 本帧距上一帧的 us / 50 (钳到最后一档)。 */
    if (s_PfLastFrame)
    {
        unsigned us = (unsigned)((now - s_PfLastFrame) * 1000000ll / s_PfQpf);
        unsigned b = us / 50u;
        if (b >= PF_HIST_N) b = PF_HIST_N - 1;
        s_PfHist[b]++;
    }
    s_PfLastFrame = now;
    if (!s_PfLast) { s_PfLast = now; return; }
    dt = now - s_PfLast;
    if (dt < s_PfQpf) return;                     /* 不满 1s 不结算 */

    ms = (unsigned)(dt * 1000 / s_PfQpf);
    p = AppendCStr(buf, p, "perf: ms=", sizeof(buf));
    UtoA(ms, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " f=", sizeof(buf));
    UtoA(s_PfFrames, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    /* 中位帧率 (50us 粒度档): 抗尖峰, 与上面 f= 对照看。 */
    {
        unsigned tot = 0, half, acc = 0, i2, med = 0;
        for (i2 = 0; i2 < PF_HIST_N; i2++) tot += s_PfHist[i2];
        half = tot / 2;
        for (i2 = 0; i2 < PF_HIST_N; i2++)
        {
            acc += s_PfHist[i2];
            if (acc >= half) { med = (i2 + 1) * 50u; break; }
        }
        p = AppendCStr(buf, p, " fmed=", sizeof(buf));
        UtoA(med ? 1000000u / med : 0, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        for (i2 = 0; i2 < PF_HIST_N; i2++) s_PfHist[i2] = 0;
    }
    /* 每帧 prepare 被调用几次 (0x4F42F0 有上百个调用点, 正常应接近 1)。 */
    p = AppendCStr(buf, p, " prepN=", sizeof(buf));
    UtoA(s_PfFrames ? s_PfCnt[PF_PREPARE] / s_PfFrames : 0, b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    for (i = 0; s_PfOn && i < PF_COUNT; i++)
    {
        /* us/帧 = acc * 1e6 / qpf / frames; cnt = 次数/帧 */
        long long us = s_PfFrames ? (s_PfAcc[i] * 1000000ll / s_PfQpf / (long long)s_PfFrames) : 0;
        if (!s_PfAcc[i] && !s_PfCnt[i]) continue;
        p = AppendCh(buf, p, ' ', sizeof(buf));
        p = AppendCStr(buf, p, kPfName[i], sizeof(buf));
        p = AppendCh(buf, p, '=', sizeof(buf));
        UtoA((unsigned)(us < 0 ? 0 : us), b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        if (s_PfCnt[i])
        {
            p = AppendCh(buf, p, '/', sizeof(buf));
            UtoA(s_PfFrames ? s_PfCnt[i] / s_PfFrames : s_PfCnt[i], b1);
            p = AppendCStr(buf, p, b1, sizeof(buf));
        }
        s_PfAcc[i] = 0;
        s_PfCnt[i] = 0;
    }
    if (s_PfOn)
    {
        p = AppendCStr(buf, p, " vwhy=", sizeof(buf));
        UtoA((unsigned)g_VecMissWhy, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
    }
    LogLine(buf);
    s_PfFrames = 0;
    s_PfLast = now;
}

/* ---------------- 字体查找 ---------------- */

void CopyFace(wchar_t* dst, const wchar_t* src)
{
    int i = 0;
    while (src[i] && i < FONT_FACE_MAX - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int FaceSame(const wchar_t* a, const wchar_t* b)
{
    int i = 0;
    while (i < FONT_FACE_MAX - 1 && a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static unsigned RdU16BE(const unsigned char* p)
{
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}

static unsigned RdU32BE(const unsigned char* p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static int ReadAt(HANDLE h, unsigned off, unsigned char* buf, unsigned n)
{
    DWORD got = 0;
    if (off >= 0x80000000u) return 0;
    if (SetFilePointer(h, (LONG)off, NULL, FILE_BEGIN) == (DWORD)-1) return 0;
    if (!ReadFile(h, buf, n, &got, NULL) || got != n) return 0;
    return 1;
}

/* 从字体文件读 family 名 (nameID=1): ttf/otf 与 ttc (取首个字体) 均可。
 * 优先 Windows 英文名 (3,1,0409), 其次中文 (3,1,0804), 再其它。
 * 成功返回 1 (名字进 out); 失败返回 0。纯手写解析, 无 CRT。 */
static int ParseFamilyName(const char* path, wchar_t* out)
{
    HANDLE h;
    DWORD fsize;
    unsigned char head[16];
    unsigned fontOff = 0, nTab, i;
    unsigned char dir[12 + 64 * 16];
    unsigned nameOff = 0, nameLen = 0;
    unsigned char nh[6];
    unsigned count, strOff, r;
    int best = 99;
    unsigned bestLen = 0, bestStr = 0, bestPlat = 0;
    unsigned char rec[12];
    unsigned char sbuf[FONT_FACE_MAX * 2];
    unsigned want, k, c;

    h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    fsize = GetFileSize(h, NULL);
    if (fsize == (DWORD)-1 || fsize < 16) { CloseHandle(h); return 0; }
    if (!ReadAt(h, 0, head, 16)) { CloseHandle(h); return 0; }
    if (head[0] == 't' && head[1] == 't' && head[2] == 'c' && head[3] == 'f')
    {
        unsigned nFonts = RdU32BE(head + 8);
        if (nFonts < 1) { CloseHandle(h); return 0; }
        fontOff = RdU32BE(head + 12);
        if (fontOff + 12 > fsize || fontOff + 12 < fontOff) { CloseHandle(h); return 0; }
    }
    if (!ReadAt(h, fontOff, dir, 12)) { CloseHandle(h); return 0; }
    nTab = RdU16BE(dir + 4);
    if (nTab < 1 || nTab > 64) { CloseHandle(h); return 0; }
    if (nTab * 16 > fsize - fontOff - 12) { CloseHandle(h); return 0; }
    if (!ReadAt(h, fontOff + 12, dir + 12, nTab * 16)) { CloseHandle(h); return 0; }
    for (i = 0; i < nTab; i++)
    {
        unsigned char* t = dir + 12 + i * 16;
        unsigned to, ln;
        if (t[0] == 'n' && t[1] == 'a' && t[2] == 'm' && t[3] == 'e')
        {
            to = RdU32BE(t + 8);
            ln = RdU32BE(t + 12);
            if (to >= fsize || ln > fsize - to || ln < 6) continue;
            nameOff = to;
            nameLen = ln;
            break;
        }
    }
    if (!nameOff) { CloseHandle(h); return 0; }
    if (!ReadAt(h, nameOff, nh, 6)) { CloseHandle(h); return 0; }
    count = RdU16BE(nh + 2);
    strOff = RdU16BE(nh + 4);
    if (count > 300 || strOff >= nameLen) { CloseHandle(h); return 0; }
    for (r = 0; r < count; r++)
    {
        unsigned plat, lang, nid, len, off;
        int score;
        if (!ReadAt(h, nameOff + 6 + r * 12, rec, 12)) break;
        plat = RdU16BE(rec + 0);
        lang = RdU16BE(rec + 4);
        nid  = RdU16BE(rec + 6);
        len  = RdU16BE(rec + 8);
        off  = RdU16BE(rec + 10);
        if (nid != 1 || !len) continue;
        if (off >= nameLen - strOff || len > nameLen - strOff - off) continue;
        if (plat == 3) score = (lang == 0x0409) ? 0 : (lang == 0x0804) ? 1 : 2;
        else if (plat == 1) score = 3;
        else continue;
        if (score < best) { best = score; bestLen = len; bestStr = strOff + off; bestPlat = plat; }
        if (best == 0) break;
    }
    if (best > 3 || !bestLen) { CloseHandle(h); return 0; }
    want = bestLen;
    if (want > (FONT_FACE_MAX - 1) * 2) want = (FONT_FACE_MAX - 1) * 2;
    if (!ReadAt(h, nameOff + bestStr, sbuf, want)) { CloseHandle(h); return 0; }
    CloseHandle(h);
    k = 0;
    if (bestPlat == 3)
    {
        for (c = 0; c + 1 < want && k < FONT_FACE_MAX - 1; c += 2)
            out[k++] = (wchar_t)(((unsigned)sbuf[c] << 8) | sbuf[c + 1]);
    }
    else
    {
        for (c = 0; c < want && k < FONT_FACE_MAX - 1; c++)
            out[k++] = (wchar_t)sbuf[c];
    }
    out[k] = 0;
    return k > 0;
}

/* 通用文件存在检查 */
int FileExists(const char* path)
{
    DWORD a;
    if (!path || !*path) return 0;
    a = GetFileAttributesA(path);
    return (a != (DWORD)-1 && !(a & FILE_ATTRIBUTE_DIRECTORY));
}

/* 按 游戏目录 -> 系统字体目录 顺序找文件 (原样匹配)。
 * 找到写全路径进 outPath, 返回 1=游戏目录, 2=系统目录; 都没有返回 0。 */
int FindFontFile(const char* name, char* outPath, int cap, const char** tag)
{
    char exe[MAX_PATH_LEN];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH_LEN);
    DWORD i;
    unsigned k;
    char dir[MAX_PATH_LEN];
    if (name && *name)
    {
        if (n > 0 && n < MAX_PATH_LEN)
        {
            i = n;
            while (i > 0 && exe[i] != '\\' && exe[i] != '/') i--;
            k = 0;
            while (k < i && k + 1 < (unsigned)cap) { outPath[k] = exe[k]; k++; }
            outPath[k] = 0;
            k = AppendCStr(outPath, k, "\\", (unsigned)cap);
            k = AppendCStr(outPath, k, name, (unsigned)cap);
            if (FileExists(outPath)) { if (tag) *tag = "cur"; return 1; }
        }
        if (GetWindowsDirectoryA(dir, MAX_PATH_LEN))
        {
            k = AppendCStr(outPath, 0, dir, (unsigned)cap);
            k = AppendCStr(outPath, k, "\\Fonts\\", (unsigned)cap);
            k = AppendCStr(outPath, k, name, (unsigned)cap);
            if (FileExists(outPath)) { if (tag) *tag = "sys"; return 2; }
        }
    }
    if (tag) *tag = "none";
    return 0;
}

static void LogFace(const char* tag, const char* what, const char* v)
{
    char logb[256];
    unsigned q = 0;
    q = AppendCStr(logb, q, "font: ", sizeof(logb));
    q = AppendCStr(logb, q, tag, sizeof(logb));
    q = AppendCStr(logb, q, what, sizeof(logb));
    q = AppendCStr(logb, q, v, sizeof(logb));
    LogLine(logb);
}

/* face 转 ASCII (非 ASCII 记 '?'), 回 1=全 ASCII。 */
static int FaceToAscii(const wchar_t* face, char* out, int cap)
{
    int i = 0, ascii = 1;
    while (face[i] && i < cap - 1)
    {
        wchar_t wc = face[i];
        if (wc >= 32 && wc < 127) out[i] = (char)wc;
        else { out[i] = '?'; ascii = 0; }
        i++;
    }
    out[i] = 0;
    return ascii;
}

/* 字体解析只记一行: font: <tag> file=<f> found=<where> [use=<Face>] [tail]。
 * 非 ASCII 名 use= 里是 '?', 才追记一行 useHex= 前 8 字备查 (ASCII 名不记)。 */
static void LogFaceOne(const char* tag, const char* file, const char* found,
                       const wchar_t* face, const char* tail)
{
    char logb[256], tmp[FONT_FACE_MAX];
    unsigned q = 0;
    int ascii = 1, k;
    q = AppendCStr(logb, q, "font: ", sizeof(logb));
    q = AppendCStr(logb, q, tag, sizeof(logb));
    q = AppendCStr(logb, q, " file=", sizeof(logb));
    q = AppendCStr(logb, q, file, sizeof(logb));
    q = AppendCStr(logb, q, " found=", sizeof(logb));
    q = AppendCStr(logb, q, found, sizeof(logb));
    if (face)
    {
        ascii = FaceToAscii(face, tmp, sizeof(tmp));
        q = AppendCStr(logb, q, " use=", sizeof(logb));
        q = AppendCStr(logb, q, tmp, sizeof(logb));
    }
    if (tail)
    {
        q = AppendCStr(logb, q, " ", sizeof(logb));
        q = AppendCStr(logb, q, tail, sizeof(logb));
    }
    LogLine(logb);
    if (face && !ascii)
    {
        char hx[64];
        q = 0;
        k = 0;
        while (face[k] && k < 8 && q + 6 < sizeof(hx))
        {
            unsigned w = (unsigned)face[k] & 0xFFFFu;
            int sh;
            hx[q++] = 'U'; hx[q++] = '+';
            for (sh = 12; sh >= 0; sh -= 4)
                hx[q++] = "0123456789ABCDEF"[(w >> sh) & 15u];
            hx[q++] = ' ';
            k++;
        }
        hx[q] = 0;
        LogFace(tag, " useHex=", hx);
    }
}

static void ResolveFontFaceRaw(const char* cfg, const char* tag, wchar_t* out,
                     const wchar_t* fallback)
{
    char tmp[64], path[MAX_PATH_LEN * 2];
    int j = 0;
    const char* where = "none";
    /* 输入在加载时已由 OU_ReadString 去注释/去两端空格, 这里只做有界拷贝。 */
    if (!cfg) { CopyFace(out, fallback); return; }
    while (cfg[j] && j < (int)sizeof(tmp) - 1) { tmp[j] = cfg[j]; j++; }
    tmp[j] = 0;
    if (!tmp[0]) { CopyFace(out, fallback); return; }
    if (!FindFontFile(tmp, path, sizeof(path), &where))
    {
        LogFaceOne(tag, tmp, "none", NULL, "fallback");
        CopyFace(out, fallback);
        return;
    }
    /* 从文件读出真名再加载 (游戏目录自带字体靠这一步可用) */
    if (ParseFamilyName(path, out))
    {
        AddFontResourceExA(path, FR_PRIVATE, 0);
        LogFaceOne(tag, tmp, where, out, NULL);
        return;
    }
    LogFaceOne(tag, tmp, where, NULL, "parse-failed fallback");
    CopyFace(out, fallback);
}

/* 候选字体 CJK 字形自检: 临时 DC + 16px 测试字, 一(U+4E2D)/：(U+FF1A)
 * 都有字形才算过。CreateFont 失败/无字形一律 0。纯 Win32, 无游戏依赖。
 * 背景: 某机器上 "Microsoft YaHei" 解析正常但 CreateFont 实际落到无 CJK
 * 字形 (ASCII 照常、CJK 空白, RenderLine 整串无兜底); VecText 有逐字回原版
 * 兜底不受影响, DrawBox/ChatEdit 整串 ExtTextOut 直接空白。 */
static int FaceHasCJK(const wchar_t* face)
{
    static const wchar_t probe[3] = { 0x4E2Du, 0xFF1Au, 0 };
    HDC screen, dc;
    HFONT f, old;
    WORD idx[2];
    DWORD n;
    int ok = 0;
    if (!face || !face[0]) return 0;
    screen = GetDC(NULL);
    if (!screen) return 0;
    dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!dc) return 0;
    f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!f) { DeleteDC(dc); return 0; }
    old = (HFONT)SelectObject(dc, f);
    n = GetGlyphIndicesW(dc, probe, 2, idx, 0);
    if (n == 2 && idx[0] != 0xFFFFu && idx[1] != 0xFFFFu) ok = 1;
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return ok;
}

void ResolveFontFace(const char* cfg, const char* tag, wchar_t* out,
                     const wchar_t* fallback)
{
    static const wchar_t* cands[] = {
        L"Microsoft YaHei", L"SimSun", L"NSimSun", L"SimHei"
    };
    unsigned i;
    ResolveFontFaceRaw(cfg, tag, out, fallback);
    if (FaceHasCJK(out)) return;   /* 解析/回退本就有字形, 原样 */
    for (i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
    {
        if (FaceSame(cands[i], out)) continue;
        if (FaceHasCJK(cands[i]))
        {
            char cn[FONT_FACE_MAX];
            FaceToAscii(cands[i], cn, sizeof(cn));   /* 候选全 ASCII, 单行 */
            LogFace(tag, " cjkHeal=", cn);
            CopyFace(out, cands[i]);
            return;
        }
    }
    /* 都没字形: 原样返回 (尽力), 行为不变 */
}

/* ---------------- 画面底座 (DSURFACE 虚表) ----------------
 * 引擎面对象首成员是虚表指针; 所有调用都先过指针合理性判定, 半初始化面
 * (loading 态) 的虚表是垃圾, 撞上去必崩, 故一律守卫后静默跳过。 */

typedef int (__fastcall *GtGetIntFn)(void* s, void* edx);
typedef int (__fastcall *GtFillFn)(void* s, void* edx, RectangleStruct*, ColorStruct*, int);
typedef void* (__fastcall *GtLockFn)(void* s, void* edx, int, int);
typedef int (__fastcall *GtUnlockFn)(void* s, void* edx);

static void* GtVtable(void* surf, unsigned vtOff)
{
    unsigned char* vt;
    if (!surf || !IsPlausiblePtr(surf)) return NULL;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return NULL;
    return *(void**)(vt + vtOff);
}

int GtSurfGet(void* surf, unsigned vtOff)
{
    GtGetIntFn fn = (GtGetIntFn)GtVtable(surf, vtOff);
    if (!fn) return -1;
    return fn(surf, NULL);
}

void GtFillRectTrans(void* surf, RectangleStruct* r, ColorStruct* c, int opacity)
{
    GtFillFn fn = (GtFillFn)GtVtable(surf, GtVT_FILL_RECT_TRANS);
    if (!fn) return;
    fn(surf, NULL, r, c, opacity);
}

unsigned char* GtLock(void* surf)
{
    GtLockFn fn = (GtLockFn)GtVtable(surf, GtVT_LOCK);
    unsigned char* p;
    long long t;
    if (!fn) return NULL;
    if (!IsPlausiblePtr((const void*)fn)) return NULL;
    t = Perf_Now();
    p = (unsigned char*)fn(surf, NULL, 0, 0);
    Perf_Add(PF_LOCK, t);
    return (p && IsPlausiblePtr(p)) ? p : NULL;
}

void GtUnlock(void* surf)
{
    GtUnlockFn fn = (GtUnlockFn)GtVtable(surf, GtVT_UNLOCK);
    long long t;
    if (!fn) return;
    if (!IsPlausiblePtr((const void*)fn)) return;
    t = Perf_Now();
    fn(surf, NULL);
    Perf_Add(PF_LOCK, t);
}

void GtDrawOutline(void* surf, int x, int y, int w, int h,
                   unsigned char r, unsigned char g, unsigned char b, int op)
{
    RectangleStruct e;
    ColorStruct c;
    if (w < 8 || h < 8) return;
    c.R = r; c.G = g; c.B = b;
    e.X = x; e.Y = y; e.Width = w; e.Height = 2;
    GtFillRectTrans(surf, &e, &c, op);
    e.Y = y + h - 2;
    GtFillRectTrans(surf, &e, &c, op);
    e.Y = y + 2; e.Height = h - 4; e.Width = 2;
    GtFillRectTrans(surf, &e, &c, op);
    e.X = x + w - 2;
    GtFillRectTrans(surf, &e, &c, op);
}

int GtBitLineHeight(void)
{
    void* font = *(void**)ADDR_BITFONT_INSTANCE;
    int h = font ? *(int*)((char*)font + FONT_OFF_HEIGHT) : 14;
    if (h < 6 || h > 64) h = 14;
    return h;
}

int GtScreenW(void)
{
    RectangleStruct* vb = (RectangleStruct*)ADDR_SURF_VIEWBOUNDS;
    return (vb && vb->Width > 200) ? vb->Width : 640;
}

/* ---------------- 游戏时钟 (引擎式过期的时间基准) ----------------
 * 反汇编实证 (AddMessage 0x5D3DC0 / Manage 0x5D4441):
 *   时钟 = [0x887340] + (Tick() - [0x887338]), Tick = timeGetTime()>>4 (毫秒/16),
 *   [0x887338]==-1 时不做 Tick 补偿 (暂停/未启动状态)。
 * 这就是不限速下原版消息"活得更久"的原因: 超时是墙钟, 不是逻辑帧。 */

#define ADDR_TICK_FN   0x006C8C40u
#define ADDR_CLK_REF   0x00887338u
#define ADDR_CLK_BASE  0x00887340u
#define ADDR_CLK_ARG   0x0088733Cu

typedef unsigned (__fastcall *GtTickFn)(void* a, void* edxUnused);

unsigned GtTickNow(void)
{
    static GtTickFn fn = NULL;
    if (!fn) fn = (GtTickFn)ADDR_TICK_FN;
    return fn((void*)ADDR_CLK_ARG, NULL);
}

unsigned GtClockNow(void)
{
    unsigned ref = *(unsigned*)ADDR_CLK_REF;
    unsigned b = *(unsigned*)ADDR_CLK_BASE;
    if (ref != 0xFFFFFFFFu) b += GtTickNow() - ref;
    return b;
}

/* ---------------- 像素格式校准 ----------------
 * 在 Temp 面上 FillRect 纯红再 GetPixel 回读, 判定 565 还是 555。
 * 只做一次, 结果记入日志。 */

int g_RBits = 5, g_GBits = 6, g_BBits = 5;  /* 默认 565 */
int g_Calibrated = 0;

/* 位深展开表, 混合查表用。Calibrate 后清零重建。 */
unsigned char g_ExpR[32], g_ExpG[64], g_ExpB[32];
int g_ExpReady = 0;
unsigned char g_BlendR[32][32], g_BlendG[32][64];
int g_BlendReady = 0;

void GtBuildExpand(void)
{
    int i, rN = (1 << g_RBits) - 1, gN = (1 << g_GBits) - 1, bN = (1 << g_BBits) - 1;
    if (rN < 1) rN = 31;
    if (gN < 1) gN = 63;
    if (bN < 1) bN = 31;
    for (i = 0; i < 32; i++) g_ExpR[i] = (unsigned char)((i * 255 + rN / 2) / rN);
    for (i = 0; i < 32; i++) g_ExpB[i] = (unsigned char)((i * 255 + bN / 2) / bN);
    for (i = 0; i < 64; i++)
    {
        unsigned v = (unsigned)((i * 255 + gN / 2) / gN);
        g_ExpG[i] = (unsigned char)(v > 255u ? 255u : v);
    }
    GtBuildBlend5();
    g_ExpReady = 1;
}

/* 5 位 alpha 的"目标乘数"表: TB[inv][dst] = dst * inv / 31 (inv=31-aq)。
 * 与前景色无关, 所以全局建一次; 前景部分按字建 32 项 (见 VecRender)。
 * 精度: alpha 8 位 -> 5 位 (丢 3 位), 与 8 位混合相比每个通道最多差 1 个色阶,
 * 18px 抗锯齿边缘看不出来; 换来的是每像素 3 次小表查 + 3 次加, 没有乘法/除法。 */
void GtBuildBlend5(void)
{
    int inv, d;
    for (inv = 0; inv < 32; inv++)
    {
        for (d = 0; d < 32; d++)
            g_BlendR[inv][d] = (unsigned char)(((unsigned)d * (unsigned)inv + 15u) / 31u);
        for (d = 0; d < 64; d++)
            g_BlendG[inv][d] = (unsigned char)(((unsigned)d * (unsigned)inv + 15u) / 31u);
    }
    g_BlendReady = 1;
}

void Calibrate(void)
{
    void* tmp;
    unsigned char* vt;
    RectangleStruct rc;
    char b1[16], buf[256];
    unsigned p;
    typedef int (__fastcall *FillFn)(void*, void*, RectangleStruct*, unsigned);
    typedef unsigned (__fastcall *GetPxFn)(void*, void*, Point2D*);
    FillFn fill;
    GetPxFn getpx;
    Point2D pt;
    unsigned raw;

    if (g_Calibrated) return;

    tmp = *(void**)ADDR_SURF_TEMP;
    if (!tmp || !IsPlausiblePtr(tmp)) { LogLine("cal: no temp surface, assume 565"); g_Calibrated = 1; return; }
    vt = *(unsigned char**)tmp;
    if (!vt || !IsPlausiblePtr(vt)) { LogLine("cal: bad temp vtable, assume 565"); g_Calibrated = 1; return; }

    rc.X = 0; rc.Y = 0; rc.Width = 4; rc.Height = 4;
    fill = (FillFn)(*(void**)(vt + GtVT_FILL_RECT));
    getpx = (GetPxFn)(*(void**)(vt + GtVT_GET_PIXEL));
    if (!fill || !getpx) { LogLine("cal: no fill/getpx, assume 565"); g_Calibrated = 1; return; }

    fill(tmp, NULL, &rc, 0x000000FFu);   /* COLORREF 纯红 */
    pt.X = 1; pt.Y = 1;
    raw = getpx(tmp, NULL, &pt) & 0xFFFFu;

    if ((raw & 0xF800u) == 0xF800u)      { g_RBits = 5; g_GBits = 6; g_BBits = 5; }
    else if ((raw & 0x7C00u) == 0x7C00u) { g_RBits = 5; g_GBits = 5; g_BBits = 5; }
    /* 否则保持默认 565 */
    g_ExpReady = 0;

    p = 0;
    p = AppendCStr(buf, p, "cal: raw=", sizeof(buf));
    p += HtoA(raw, buf + p);
    p = AppendCStr(buf, p, " fmt=", sizeof(buf));
    UtoA((unsigned)g_RBits, b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, '.', sizeof(buf));
    UtoA((unsigned)g_GBits, b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, '.', sizeof(buf));
    UtoA((unsigned)g_BBits, b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);

    g_Calibrated = 1;
}

/* Blend16 见 GdiText.h 内联。 */

void GtUnpack16(unsigned short px, unsigned char* r, unsigned char* g,
                unsigned char* b)
{
    if (!g_ExpReady) GtBuildExpand();
    if (g_GBits == 6)
    {
        *r = g_ExpR[(px >> 11) & 0x1Fu];
        *g = g_ExpG[(px >> 5) & 0x3Fu];
        *b = g_ExpB[px & 0x1Fu];
    }
    else
    {
        *r = g_ExpR[(px >> 10) & 0x1Fu];
        *g = g_ExpG[(px >> 5) & 0x1Fu];
        *b = g_ExpB[px & 0x1Fu];
    }
}

/* ---------------- GDI 光栅化 (七槽: 战役/提示/全局文本/超武面板/聊天输入/
 * 超武侧栏充能%/超武侧栏悬停名 各缓存一套) ----------------
 * 超武侧栏占两槽是有原因的: 充能百分比走 [SWSidebar] FontPx, 悬停名走全局
 * [VecText] 字号, 共用一个槽会让 GdiEnsure 每次都拆了重建 (CreateDIBSection
 * + CreateFontW), 反而比重新光栅还贵。 */

/* 全零起步, dc==NULL 即空槽。 */
static GdiSlot g_Slots[GDI_SLOTS];
GdiSlot* g_G = &g_Slots[0];

/* ---------------- 行光栅缓存 (RenderLine 同串复用) ----------------
 * 命中时不调 GDI, 只写回红通道。按槽代号作废。容量须装下一帧不同串数。 */
#define ROWC_N 64            /* 条目数 */
#define ROWC_W 256           /* 单行像素宽上限 */
#define ROWC_H 32            /* 单行像素高上限 (行高 = g_G->adv) */
#define ROWC_TX 80           /* 键里存的字数 (超出的长串不入缓存) */
typedef struct {
    int gen;
    const void* owner;     /* GdiSlot*: 同串不同槽不共用 */
    int len, w, h;
    wchar_t tx[ROWC_TX];
    unsigned char cov[ROWC_W * ROWC_H];
} RowCE;
static RowCE g_RowC[ROWC_N];
static int g_RowGen[GDI_SLOTS] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
static int g_RowRR = 0;

static void GdiFreeSlot(GdiSlot* s)
{
    int i;
    if (s->dc)
    {
        SelectObject(s->dc, s->oldBmp);
        DeleteObject(s->dib);
        DeleteObject(s->font);
        DeleteDC(s->dc);
    }
    s->dc = NULL; s->dib = NULL; s->oldBmp = NULL; s->font = NULL;
    s->bits = NULL; s->pitch = 0; s->px = -1; s->bold = -1; s->adv = 0;
    s->asc = 0; s->cell = 0;
    s->face[0] = 0;
    /* ABC 记忆表随槽作废, 键值一起清。 */
    for (i = 0; i < ABC_MEMO_N; i++) { s->abcM[i] = -1; s->abcK[i] = 0; }
    if (s->sidx >= 0 && s->sidx < GDI_SLOTS) g_RowGen[s->sidx]++;
}

int GdiEnsure(int fontPx, int bold, int slot, const wchar_t* face)
{
    HDC screen;
    BITMAPINFO bi;
    TEXTMETRICW tm;
    HFONT f;
    int wantBold = bold ? 1 : 0;
    GdiSlot* s;
    char b1[16], b2[16], buf[256];
    unsigned p;

    if (slot < 0 || slot >= GDI_SLOTS) slot = 0;
    s = &g_Slots[slot];
    s->sidx = slot;

    if (s->dc && fontPx == s->px && wantBold == s->bold && FaceSame(face, s->face)) { g_G = s; return 1; }
    GdiFreeSlot(s);

    screen = GetDC(NULL);
    if (!screen) return 0;
    s->dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!s->dc) return 0;

    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = DIB_W;
    bi.bmiHeader.biHeight = -DIB_H;   /* 顶向下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biSizeImage = 0;
    bi.bmiHeader.biXPelsPerMeter = 0;
    bi.bmiHeader.biYPelsPerMeter = 0;
    bi.bmiHeader.biClrUsed = 0;
    bi.bmiHeader.biClrImportant = 0;

    s->dib = CreateDIBSection(s->dc, &bi, DIB_RGB_COLORS, (void**)&s->bits, NULL, 0);
    if (!s->dib || !s->bits) { GdiFreeSlot(s); return 0; }
    s->oldBmp = (HBITMAP)SelectObject(s->dc, s->dib);
    s->pitch = DIB_W * 4;

    f = CreateFontW(-fontPx, 0, 0, 0, wantBold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!f) { GdiFreeSlot(s); return 0; }
    s->font = f;
    SelectObject(s->dc, s->font);
    SetBkMode(s->dc, TRANSPARENT);
    SetTextColor(s->dc, RGB(255, 255, 255));

    GetTextMetricsW(s->dc, &tm);
    s->px   = fontPx;
    s->bold = wantBold;
    s->adv  = tm.tmHeight + 2;
    s->asc  = tm.tmAscent;
    s->cell = tm.tmHeight;
    CopyFace(s->face, face);
    g_G = s;

    if (g_LogDetail)
    {
        p = 0;
        p = AppendCStr(buf, p, "gdi: slot=", sizeof(buf));
        UtoA((unsigned)slot, b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " fontPx=", sizeof(buf));
        p += UtoA((unsigned)fontPx, buf + p);
        p = AppendCStr(buf, p, " bold=", sizeof(buf));
        UtoA((unsigned)wantBold, b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " tmHeight=", sizeof(buf));
        UtoA((unsigned)tm.tmHeight, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " lineAdv=", sizeof(buf));
        UtoA((unsigned)s->adv, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " face=", sizeof(buf));
        { int fi = 0; while (face[fi] && p < sizeof(buf) - 1)
          { wchar_t wc = face[fi++]; buf[p++] = (wc >= 32 && wc < 127) ? (char)wc : '?'; } buf[p] = 0; }
        LogLine(buf);
    }
    return 1;
}

/* 量一段文本在 maxW 内能放几个字符 */
/* 单字 ABC 三段 (槽内记忆, 同字只问 GDI 一次)。 */
int GtCharABC(wchar_t ch, int* pa, int* pb, int* pc)
{
    GdiSlot* s = g_G;
    ABC abc;
    int idx;
    if (!s || !s->dc || !ch) return -1;
    /* 乘性散列: CJK 连号直接取模会互撞。 */
    idx = (int)(((((unsigned)ch) * 2654435761u) >> 16) & (ABC_MEMO_N - 1));
    if (s->abcK[idx] != ch)
    {
        if (!GetCharABCWidthsW(s->dc, ch, ch, &abc)) return -1;
        s->abcA[idx] = (short)abc.abcA;
        s->abcB[idx] = (short)abc.abcB;
        s->abcC[idx] = (short)abc.abcC;
        s->abcM[idx] = (short)((abc.abcA + abc.abcB + abc.abcC) & 0x7FFF);
        s->abcK[idx] = ch;
    }
    if (pa) *pa = s->abcA[idx];
    if (pb) *pb = s->abcB[idx];
    if (pc) *pc = s->abcC[idx];
    return s->abcM[idx];
}

/* 单字前进宽 (ABC 全宽)。需先 GdiEnsure。 */
int GtCharAdv(wchar_t ch)
{
    return GtCharABC(ch, 0, 0, 0);
}

int CharsFit(const wchar_t* text, int len, int maxW)
{
    int fit = 0;
    SIZE sz;
    if (!g_G->dc || maxW <= 0 || len <= 0) return 0;
    if (!GetTextExtentExPointW(g_G->dc, text, len, maxW, &fit, NULL, &sz)) return 0;
    return fit;
}

/* 清 DIB 用到的矩形 (整幅 256KB, 每帧几十次, 只清所需)。 */
static void ClearDibRect(int cols, int rows)
{
    int y, i, w;
    if (cols > DIB_W) cols = DIB_W;
    if (rows > DIB_H) rows = DIB_H;
    if (cols < 0) cols = 0;
    if (rows < 0) rows = 0;
    if (!g_G || !g_G->bits) return;
    w = (cols * 4 + 3) >> 2;
    for (y = 0; y < rows; y++)
    {
        unsigned int* p = (unsigned int*)(g_G->bits + y * g_G->pitch);
        for (i = 0; i < w; i++) p[i] = 0;
    }
}

/* 行缓存按返回宽+2 列存取, 与调用方读取一致。 */
#define ROW_PAD 2

/* 行缓存命中: 写回 DIB 红通道。 */
static void RowCE_Blit(RowCE* e)
{
    int y, x, cw = e->w + ROW_PAD;
    if (cw > ROWC_W) cw = ROWC_W;
    for (y = 0; y < e->h; y++)
    {
        unsigned char* row = g_G->bits + y * g_G->pitch;
        const unsigned char* cv = e->cov + y * ROWC_W;
        for (x = 0; x < cw; x++) row[x * 4 + 2] = cv[x];
    }
}

/* 取缓存条目; 命中填 *pe 返回 1。 */
static int RowCE_Get(const wchar_t* text, int len, RowCE** pe)
{
    int i, k;
    if (len > ROWC_TX) return 0;
    for (i = 0; i < ROWC_N; i++)
    {
        RowCE* e = &g_RowC[i];
        if (e->gen != g_RowGen[g_G->sidx] || e->owner != g_G || e->len != len)
            continue;
        for (k = 0; k < len; k++)
            if (e->tx[k] != text[k]) break;
        if (k == len) { *pe = e; return 1; }
    }
    return 0;
}

/* 存当前 DIB 有效区 (超宽/超高不入)。 */
static void RowCE_Put(const wchar_t* text, int len, int w, int h)
{
    RowCE* e;
    int y, x, cw;
    cw = w + ROW_PAD;
    if (w <= 0 || cw > ROWC_W || h <= 0 || h > ROWC_H || len > ROWC_TX) return;
    e = &g_RowC[g_RowRR % ROWC_N];
    g_RowRR++;
    e->gen = g_RowGen[g_G->sidx];
    e->owner = g_G;
    e->len = len;
    e->w = w;
    e->h = h;
    for (x = 0; x < len; x++) e->tx[x] = text[x];
    for (y = 0; y < h; y++)
    {
        const unsigned char* row = g_G->bits + y * g_G->pitch;
        unsigned char* cv = e->cov + y * ROWC_W;
        for (x = 0; x < cw; x++) cv[x] = row[x * 4 + 2];
    }
}

int RenderLine(const wchar_t* text, int len)
{
    SIZE sz;
    RowCE* e;
    int fit, keyLen = len;
    if (!g_G->dc || len <= 0) return 0;
    /* 命中优先: 缓存里已有本串的像素, 连 CharsFit 那次 GDI 度量都省掉
     * (刻度条档位字/超武面板行都是同串反复画, 每帧会量 4 次)。
     * 键用**原始 len** (限宽只影响画多宽, 不影响这是哪一串)。 */
    if (RowCE_Get(text, keyLen, &e))
    {
        RowCE_Blit(e);
        return e->w;
    }
    /* 精确限宽: 按实际字形 fit DIB (粗估会砍掉长文尾部还留黑盒;
     * CharsFit 同 dc 同字体, 精确无浪费)。 */
    fit = CharsFit(text, len, DIB_W - 8);
    if (fit < 0) fit = 0;
    if (fit < len) len = fit;
    if (len <= 0) return 0;
    if (!GetTextExtentPoint32W(g_G->dc, text, len, &sz)) return 0;
    if (sz.cx > DIB_W - 8) sz.cx = DIB_W - 8;
    if (g_G->adv < 1) ClearDibRect(sz.cx + 8, DIB_H);
    else ClearDibRect(sz.cx + 8, g_G->adv + 8);
    ExtTextOutW(g_G->dc, 0, 0, 0, NULL, text, (UINT)len, NULL);
    RowCE_Put(text, keyLen, sz.cx, g_G->adv);
    return sz.cx;
}

/* 单字形光栅: 在 (x0,y0) 画一个字, 返回像素宽。先取 extent 再清。 */
int GtRenderGlyphAt(wchar_t ch, int x0, int y0, int* outW)
{
    SIZE sz;
    if (!g_G || !g_G->dc) return 0;
    if (x0 < 0 || y0 < 0 || x0 > DIB_W - 32) return 0;
    if (!GetTextExtentPoint32W(g_G->dc, &ch, 1, &sz)) return 0;
    ClearDibRect(x0 + sz.cx + 16, DIB_H);
    ExtTextOutW(g_G->dc, x0, y0, 0, NULL, &ch, 1, NULL);
    if (outW) *outW = sz.cx;
    return sz.cx;
}

/* DIB 覆盖率 blit 到已 Lock 的 16 位面。 */
void BlitCoverage(unsigned char* dstBase, int dstPitch, int surfW, int surfH,
                  int dx, int dy, int w, int h,
                  unsigned fr, unsigned fg, unsigned fb, unsigned alphaScale)
{
    int y, x, x0, x1;
    if (!g_G || !g_G->bits || w <= 0 || h <= 0) return;
    x0 = (dx < 0) ? -dx : 0;
    x1 = (dx + w > surfW) ? surfW - dx : w;
    if (x1 <= x0) return;
    if (alphaScale > 255u) alphaScale = 255u;
    /* 纯黑走 BlendDark, 与 Blend16(…,0,0,0,a) 相同。 */
    if (fr == 0 && fg == 0 && fb == 0)
    {
        for (y = 0; y < h; y++)
        {
            int sy = dy + y;
            unsigned char* dibRow;
            unsigned short* dstRow;
            if (sy < 0 || sy >= surfH) continue;
            dibRow = g_G->bits + y * g_G->pitch + x0 * 4;
            dstRow = (unsigned short*)(dstBase + sy * dstPitch) + dx + x0;
            if (alphaScale == 100u)
            {
                for (x = x0; x < x1; x++)
                {
                    unsigned cov = dibRow[x * 4 + 2];
                    if (!cov) continue;
                    dstRow[x] = BlendDark(dstRow[x], cov);
                }
            }
            else
            {
                for (x = x0; x < x1; x++)
                {
                    unsigned cov = dibRow[x * 4 + 2];
                    unsigned a;
                    if (!cov) continue;
                    a = (cov * alphaScale + 50u) / 100u;
                    if (a > 255u) a = 255u;
                    dstRow[x] = BlendDark(dstRow[x], a);
                }
            }
        }
        return;
    }
    for (y = 0; y < h; y++)
    {
        int sy = dy + y;
        unsigned char* dibRow;
        unsigned short* dstRow;
        if (sy < 0 || sy >= surfH) continue;
        dibRow = g_G->bits + y * g_G->pitch + x0 * 4;
        dstRow = (unsigned short*)(dstBase + sy * dstPitch) + dx + x0;
        if (alphaScale == 100u)
        {
            for (x = x0; x < x1; x++)
            {
                unsigned cov = dibRow[x * 4 + 2];
                if (!cov) continue;
                dstRow[x] = Blend16(dstRow[x], fr, fg, fb, cov);
            }
        }
        else
        {
            for (x = x0; x < x1; x++)
            {
                unsigned cov = dibRow[x * 4 + 2];
                unsigned a;
                if (!cov) continue;
                a = (cov * alphaScale + 50u) / 100u;
                if (a > 255u) a = 255u;
                dstRow[x] = Blend16(dstRow[x], fr, fg, fb, a);
            }
        }
    }
}

/* 阴影单遍合成: 各遍保留系数连乘, 每像素只混一次。 */
#define SHD_MAXK 16
void BlitCoverageShadow(unsigned char* dstBase, int dstPitch,
                        int surfW, int surfH,
                        int dx, int dy, int sw, int sh,
                        int Size, int offX, int offY,
                        unsigned alphaScale)
{
    const unsigned char* srow[SHD_MAXK];
    unsigned short* drow;
    int scol[SHD_MAXK];
    int X0, X1, Y, X, ox, oy, i, k;
    if (!g_G || !g_G->bits || sw <= 0 || sh <= 0) return;
    if (Size < 1) Size = 1;
    if (Size > 4) Size = 4;
    if (alphaScale > 255u) alphaScale = 255u;
    X0 = dx + offX;
    X1 = dx + offX + Size - 1 + sw - 1;
    if (X1 < X0 || surfW <= 0 || surfH <= 0) return;
    if (X1 < 0) return;
    if (X0 < 0) X0 = 0;
    if (X1 > surfW - 1) X1 = surfW - 1;
    for (Y = dy + offY; Y <= dy + offY + Size - 1 + sh - 1; Y++)
    {
        if (Y < 0 || Y >= surfH) continue;
        drow = (unsigned short*)(dstBase + (size_t)Y * dstPitch);
        k = 0;
        for (oy = 0; oy < Size && k < SHD_MAXK; oy++)
        {
            /* dy 为画面行坐标, 换算源行须减去。 */
            int sy = Y - dy - offY - oy;
            if (sy < 0 || sy >= sh) continue;
            for (ox = 0; ox < Size && k < SHD_MAXK; ox++)
            {
                srow[k] = g_G->bits + (size_t)sy * g_G->pitch;
                scol[k] = dx + offX + ox;
                k++;
            }
        }
        if (!k) continue;
        for (X = X0; X <= X1; X++)
        {
            unsigned m = 255u;
            int any = 0;
            for (i = 0; i < k; i++)
            {
                int sx = X - scol[i];
                unsigned cov, a;
                if (sx < 0 || sx >= sw) continue;
                cov = srow[i][sx * 4 + 2];
                if (!cov) continue;
                a = (cov * alphaScale + 50u) / 100u;
                if (a > 255u) a = 255u;
                m = (m * (255u - a) + 127u) / 255u;
                any = 1;
            }
            if (!any) continue;
            drow[X] = BlendDark(drow[X], 255u - m);
        }
    }
}
