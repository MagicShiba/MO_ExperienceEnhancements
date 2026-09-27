/* GdiText.cpp - 见 GdiText.h。
 * 全部由 MsgFont.cpp 原样移出 (行为不变), 外加:
 *   GtEnsureLog (幂等日志打开), GtUnpack16 (16 位像素拆 RGB),
 *   GtRenderGlyph (单字形光栅), Calibrate 首行幂等守卫,
 *   ResolveFontFace 多一个 fallback 参数。
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

int IsPlausiblePtr(const void* p)
{
    unsigned u = (unsigned)(size_t)p;
    return (u >= 0x00010000u && u < 0x7FF00000u && (u & 3u) == 0);
}

/* 无 CRT: 编译器可能把拷贝/清零循环优化成 memcpy/memset 调用, 自备实现 */
extern "C" void* memcpy(void* dst, const void* src, unsigned n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) *d++ = *s++;
    return dst;
}

extern "C" void* memset(void* s, int c, unsigned n)
{
    unsigned char* p = (unsigned char*)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

/* ---------------- 日志 ---------------- */

HANDLE g_Log = NULL;
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

    k = AppendCStr(g_LogPath, 0, exe, MAX_PATH_LEN);
    AppendCStr(g_LogPath, k, "\\MsgFont.log", MAX_PATH_LEN);
    k = AppendCStr(g_IniPath, 0, exe, MAX_PATH_LEN);
    AppendCStr(g_IniPath, k, "\\MsgFont.ini", MAX_PATH_LEN);
}

void GtEnsureLog(void)
{
    if (g_Log) return;
    BuildPaths();
    g_Log = CreateFileA(g_LogPath, FILE_APPEND_DATA,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_Log == INVALID_HANDLE_VALUE) g_Log = NULL;
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

static void LogFaceW(const char* tag, const wchar_t* face)
{
    char tmp[FONT_FACE_MAX];
    int i = 0;
    while (face[i] && i < FONT_FACE_MAX - 1)
    {
        wchar_t wc = face[i];
        tmp[i] = (wc >= 32 && wc < 127) ? (char)wc : '?';
        i++;
    }
    tmp[i] = 0;
    LogFace(tag, " use=", tmp);
    /* 非 ASCII 名在 use= 里全是 '?', 另记前 8 个字的 U+ 值备查 */
    {
        char hx[64];
        unsigned q = 0;
        int k = 0;
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

void ResolveFontFace(const char* cfg, const char* tag, wchar_t* out,
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
        LogFace(tag, " file=", tmp);
        LogFace(tag, " found=none fallback", "");
        CopyFace(out, fallback);
        return;
    }
    /* 从文件读出真名再加载 (游戏目录自带字体靠这一步可用) */
    if (ParseFamilyName(path, out))
    {
        AddFontResourceExA(path, FR_PRIVATE, 0);
        LogFace(tag, " file=", tmp);
        LogFace(tag, " found=", where);
        LogFaceW(tag, out);
        return;
    }
    LogFace(tag, " file=", tmp);
    LogFace(tag, " parse failed fallback", "");
    CopyFace(out, fallback);
}

/* ---------------- 像素格式校准 ----------------
 * 在 Temp 面上 FillRect 纯红再 GetPixel 回读, 判定 565 还是 555。
 * 只做一次, 结果记入日志。 */

#define ADDR_DSURFACE_TEMP       0x00887314u
#define VT_FILL_RECT             0x14
#define VT_GET_PIXEL             0x28

int g_RBits = 5, g_GBits = 6, g_BBits = 5;  /* 默认 565 */
int g_Calibrated = 0;

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

    tmp = *(void**)ADDR_DSURFACE_TEMP;
    if (!tmp || !IsPlausiblePtr(tmp)) { LogLine("cal: no temp surface, assume 565"); g_Calibrated = 1; return; }
    vt = *(unsigned char**)tmp;
    if (!vt || !IsPlausiblePtr(vt)) { LogLine("cal: bad temp vtable, assume 565"); g_Calibrated = 1; return; }

    rc.X = 0; rc.Y = 0; rc.Width = 4; rc.Height = 4;
    fill = (FillFn)(*(void**)(vt + VT_FILL_RECT));
    getpx = (GetPxFn)(*(void**)(vt + VT_GET_PIXEL));
    if (!fill || !getpx) { LogLine("cal: no fill/getpx, assume 565"); g_Calibrated = 1; return; }

    fill(tmp, NULL, &rc, 0x000000FFu);   /* COLORREF 纯红 */
    pt.X = 1; pt.Y = 1;
    raw = getpx(tmp, NULL, &pt) & 0xFFFFu;

    if ((raw & 0xF800u) == 0xF800u)      { g_RBits = 5; g_GBits = 6; g_BBits = 5; }
    else if ((raw & 0x7C00u) == 0x7C00u) { g_RBits = 5; g_GBits = 5; g_BBits = 5; }
    /* 否则保持默认 565 */

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

/* 16 位混合: dst = fg*a + dst*(1-a), a=0..255 */
unsigned short Blend16(unsigned short dst, unsigned fr, unsigned fg, unsigned fb, unsigned a)
{
    unsigned dr, dg, db, or_, og, ob;
    unsigned rShift, gShift;
    if (a == 0) return dst;
    if (g_GBits == 6) { rShift = 11; gShift = 5; }
    else              { rShift = 10; gShift = 5; }
    dr = (dst >> rShift) & 0x1Fu;
    dg = (dst >> gShift) & (g_GBits == 6 ? 0x3Fu : 0x1Fu);
    db = dst & 0x1Fu;
    or_ = (fr * a + (dr * ((1u << g_RBits) - 1) / 255u) * (255u - a) + 127u) / 255u;
    og = (fg * a + (dg * ((1u << g_GBits) - 1) / 255u) * (255u - a) + 127u) / 255u;
    ob = (fb * a + (db * ((1u << g_BBits) - 1) / 255u) * (255u - a) + 127u) / 255u;
    /* fr/fg/fb 是 8 位输入, 先压到目标位深再混入会丢精度;
     * 这里把 dst 侧归一到 8 位再混, 最后压回, 精度更高 */
    {
        unsigned dr8 = (dr * 255u + ((1u << g_RBits) - 1) / 2) / ((1u << g_RBits) - 1);
        unsigned dg8 = (dg * 255u + ((1u << g_GBits) - 1) / 2) / ((1u << g_GBits) - 1);
        unsigned db8 = (db * 255u + ((1u << g_BBits) - 1) / 2) / ((1u << g_BBits) - 1);
        or_ = (fr * a + dr8 * (255u - a) + 127u) / 255u;
        og = (fg * a + dg8 * (255u - a) + 127u) / 255u;
        ob = (fb * a + db8 * (255u - a) + 127u) / 255u;
        or_ = (or_ * ((1u << g_RBits) - 1) + 127u) / 255u;
        og = (og * ((1u << g_GBits) - 1) + 127u) / 255u;
        ob = (ob * ((1u << g_BBits) - 1) + 127u) / 255u;
    }
    return (unsigned short)((or_ << rShift) | (og << gShift) | ob);
}

void GtUnpack16(unsigned short px, unsigned char* r, unsigned char* g,
                unsigned char* b)
{
    unsigned rShift, gShift, rv, gv, bv;
    if (g_GBits == 6) { rShift = 11; gShift = 5; }
    else              { rShift = 10; gShift = 5; }
    rv = (px >> rShift) & 0x1Fu;
    gv = (px >> gShift) & (g_GBits == 6 ? 0x3Fu : 0x1Fu);
    bv = px & 0x1Fu;
    *r = (unsigned char)((rv * 255u + ((1u << g_RBits) - 1) / 2) / ((1u << g_RBits) - 1));
    *g = (unsigned char)((gv * 255u + ((1u << g_GBits) - 1) / 2) / ((1u << g_GBits) - 1));
    *b = (unsigned char)((bv * 255u + ((1u << g_BBits) - 1) / 2) / ((1u << g_BBits) - 1));
}

/* ---------------- GDI 光栅化 (四槽: 战役/提示/全局文本/超武面板各缓存一套) ---------------- */

static GdiSlot g_Slots[4] = {
    { NULL, NULL, NULL, NULL, NULL, 0, -1, -1, 0, { 0 } },
    { NULL, NULL, NULL, NULL, NULL, 0, -1, -1, 0, { 0 } },
    { NULL, NULL, NULL, NULL, NULL, 0, -1, -1, 0, { 0 } },
    { NULL, NULL, NULL, NULL, NULL, 0, -1, -1, 0, { 0 } }
};
GdiSlot* g_G = &g_Slots[0];

static void GdiFreeSlot(GdiSlot* s)
{
    if (s->dc)
    {
        SelectObject(s->dc, s->oldBmp);
        DeleteObject(s->dib);
        DeleteObject(s->font);
        DeleteDC(s->dc);
    }
    s->dc = NULL; s->dib = NULL; s->oldBmp = NULL; s->font = NULL;
    s->bits = NULL; s->pitch = 0; s->px = -1; s->bold = -1; s->adv = 0;
    s->face[0] = 0;
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

    if (slot < 0 || slot > 3) slot = 0;
    s = &g_Slots[slot];

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
int CharsFit(const wchar_t* text, int len, int maxW)
{
    int fit = 0;
    SIZE sz;
    if (!g_G->dc || maxW <= 0 || len <= 0) return 0;
    if (!GetTextExtentExPointW(g_G->dc, text, len, maxW, &fit, NULL, &sz)) return 0;
    return fit;
}

static void ClearDib(void)
{
    int y, x;
    unsigned char* row;
    for (y = 0; y < DIB_H; y++)
    {
        row = g_G->bits + y * g_G->pitch;
        for (x = 0; x < DIB_W; x++) { row[x*4+0] = 0; row[x*4+1] = 0; row[x*4+2] = 0; row[x*4+3] = 0; }
    }
}

/* 把一行画进 DIB (黑底白字), 返回实际像素宽; 覆盖率=红色通道 */
int RenderLine(const wchar_t* text, int len)
{
    SIZE sz;
    if (!g_G->dc || len <= 0) return 0;
    if (len * 24 > DIB_W - 8) len = (DIB_W - 8) / 24;  /* 粗限宽, 防溢出 */
    if (!GetTextExtentPoint32W(g_G->dc, text, len, &sz)) return 0;
    if (sz.cx > DIB_W - 8) sz.cx = DIB_W - 8;
    ClearDib();
    ExtTextOutW(g_G->dc, 0, 0, 0, NULL, text, (UINT)len, NULL);
    return sz.cx;
}

/* 单字形光栅: 清 DIB 后在 (x0,y0) 画一个 wchar, 返回 GDI 像素宽 (*outW 同)。
 * 调用方在字形四周留边距 (墨水 overhang 会超出 extent, 顶着边画会被裁)。 */
int GtRenderGlyphAt(wchar_t ch, int x0, int y0, int* outW)
{
    SIZE sz;
    if (!g_G || !g_G->dc) return 0;
    if (x0 < 0 || y0 < 0 || x0 > DIB_W - 32) return 0;
    ClearDib();
    if (!GetTextExtentPoint32W(g_G->dc, &ch, 1, &sz)) return 0;
    ExtTextOutW(g_G->dc, x0, y0, 0, NULL, &ch, 1, NULL);
    if (outW) *outW = sz.cx;
    return sz.cx;
}

/* DIB 覆盖率 blit 到已 Lock 的 16 位面 (裁剪到面内) */
void BlitCoverage(unsigned char* dstBase, int dstPitch, int surfW, int surfH,
                  int dx, int dy, int w, int h,
                  unsigned fr, unsigned fg, unsigned fb, unsigned alphaScale)
{
    int y, x;
    for (y = 0; y < h; y++)
    {
        int sy = dy + y;
        unsigned char* dibRow;
        unsigned short* dstRow;
        if (sy < 0 || sy >= surfH) continue;
        dibRow = g_G->bits + y * g_G->pitch;
        dstRow = (unsigned short*)(dstBase + sy * dstPitch);
        for (x = 0; x < w; x++)
        {
            int sx = dx + x;
            unsigned cov;
            unsigned a;
            if (sx < 0 || sx >= surfW) continue;
            cov = dibRow[x * 4 + 2];   /* 白字: R=G=B, 取 R */
            if (!cov) continue;
            a = (cov * alphaScale + 50u) / 100u;
            if (a > 255u) a = 255u;
            dstRow[sx] = Blend16(dstRow[sx], fr, fg, fb, a);
        }
    }
}
