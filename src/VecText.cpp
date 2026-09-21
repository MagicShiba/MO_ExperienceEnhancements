/* VecText.cpp - 全局文本矢量接管 (超级武器列表/简报/按钮等)。
 *
 * 接管点: BitFont::Blit (0x434120, 7B)。DrawText/度量/换行/对齐全部保留,
 * 版式零风险。game.fnt 格式不用管: 字宽来源是引擎自己的 GetTextDimension。
 *
 * 渲染 (Render=1): 跳过原版 1bpp 光栅, GDI 黑体灰度覆盖率经 565/555 校准
 *   混入已 Lock 的 16 位面, advance 与原版逐字一致 (探针 421/421 验证)。
 * 任何异常 (度量失败/面信息非法/字形超大) 都走跳板调原版, 绝不花屏。
 *
 * BitFont 对象实测布局 (Lock 0x4348F0 反汇编实证):
 *   +0x0C 锁定的 WORD 像素缓冲, +0x10 pitch(WORD 数),
 *   +0x24 默认色 WORD, +0x30..+0x3C 裁剪 LTRB (含端)。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MsgFont 同一 DLL)。
 */
#include <windows.h>
#include "GdiText.h"
#include "VecText.h"

#define VT_SECTION       "VecText"
#define ADDR_BLIT        0x00434120u
#define ADDR_BLIT_BACK   0x00434127u
#define ADDR_GETDIM      0x00433CF0u

/* Blit 头 7 字节 (sub esp,0x30 + push ebx/ebp/esi/edi), 下钩前校验 */
static const unsigned char kWantBlit[7] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x57 };

typedef struct {
    int Enable;
    char FontFace[64];
    int FontPx;     /* 0=自动校准(墨水与原字形等高); >0 固定像素字号 */
    int Bold;
    int VShift;     /* 字形垂直偏移 (像素, 正=下移) */
    int LogAdv;
    int Render;
} VTConfig;

static VTConfig g_VT;
static wchar_t g_VTFace[FONT_FACE_MAX];

/* thiscall (ECX=this): __fastcall 占位 (this, edxUnused) + 栈上 4 参数,
 * 编译器生成 ret 0x10, 与原 Blit 尾完全一致, JMP 钩可直接跳入。 */
typedef int (__fastcall *BlitFn)(void* t, void* edxUnused,
                                 int wch, int x, int y, int color);
static BlitFn g_OrigBlit = NULL;

typedef int (__fastcall *GetDimFn)(void* t, void* edxUnused,
                                   const wchar_t* s, int* w, int* h, int maxW);
static GetDimFn g_GetDim = NULL;

/* ---------------- 探针记录 ---------------- */

#define PROBE_MASK 4095
#define PROBE_CAP  1200

typedef struct {
    int seen;
    wchar_t wch;
    int adv;      /* 引擎单字宽 (探针已验证 = 原版返回值 - X) */
    int gh;       /* 引擎单字高 */
    int color;    /* 有效色 WORD */
    unsigned hits;
} ProbeEnt;

static ProbeEnt g_Probe[PROBE_MASK + 1];
static int g_ProbeLogs = 0;
static unsigned g_BlitN = 0;

/* 引擎单字度量: 成功返回宽, 高进 *ph; 失败返回 -1 */
static int VecMeasure(void* t, wchar_t wch, int* ph)
{
    wchar_t s[2];
    int w = 0, h = 0;
    if (!t || !IsPlausiblePtr(t) || !g_GetDim) return -1;
    s[0] = wch; s[1] = 0;
    if (!g_GetDim(t, NULL, s, &w, &h, 10000)) return -1;
    if (w <= 0 || w > 64 || h <= 0 || h > 64) return -1;
    if (ph) *ph = h;
    return w;
}

static void VecProbeLog(wchar_t wch, int adv, int gh, int color)
{
    unsigned idx = ((unsigned)wch) & PROBE_MASK;
    ProbeEnt* e = &g_Probe[idx];
    if (e->seen && e->wch == wch) { e->hits++; return; }
    if (g_ProbeLogs >= PROBE_CAP)
    {
        if (g_ProbeLogs == PROBE_CAP)
        {
            char b1[16], buf[128];
            unsigned p = 0;
            g_ProbeLogs++;
            p = AppendCStr(buf, p, "vadv: cap 1200 quiet totalBlit=", sizeof(buf));
            UtoA(g_BlitN, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            LogLine(buf);
        }
        return;
    }
    {
        char b1[16], b2[16], b3[16], buf[160];
        unsigned p = 0;
        e->seen = 1; e->wch = wch; e->adv = adv; e->gh = gh;
        e->color = color; e->hits = 1;
        g_ProbeLogs++;
        p = AppendCStr(buf, p, "vadv: wch=", sizeof(buf));
        p += HtoA((unsigned)wch, buf + p);
        p = AppendCStr(buf, p, " adv=", sizeof(buf));
        UtoA((unsigned)adv, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " h=", sizeof(buf));
        UtoA((unsigned)gh, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " color=", sizeof(buf));
        p += HtoA((unsigned)color & 0xFFFFu, buf + p);
        (void)b3;
        LogLine(buf);
    }
}

/* ---------------- 字形覆盖率缓存 ----------------
 * 与颜色无关 (灰度), 按 (wch, InternalData, adv) 命中。 */

#define VC_MASK 511
#define VC_MAXW 32
#define VC_MAXH 32

typedef struct {
    int used;
    wchar_t wch;
    void* idata;    /* BitFont+4, 字体重载即失效 */
    int px;         /* 校准字号, 换字号即换键 */
    int adv;
    int h;
    unsigned char cov[VC_MAXW * VC_MAXH];
} VCEnt;

static VCEnt g_VC[VC_MASK + 1];

/* ---------------- 基准窗校准 ----------------
 * 某套 (字号/粗体/字体) 首次使用时, 渲染 永(全高 CJK)+g/j(拉丁降部),
 * 取墨水上下界的并集为垂直基准窗。之后所有字形共用同一窗口映射,
 * 基线全字体一致; 窗比格高时统一压 (同一比例, 不错位)。
 * y0 与正式渲染一致 (GL_Y0), 量出来即用, 不猜 GDI 内部 leading。 */
#define GL_M 8
#define GL_Y0 8

static int g_RefValid = 0;
static int g_RefPx = -1, g_RefBold = -1, g_RefT = 0, g_RefB = 0;
static wchar_t g_RefFace[FONT_FACE_MAX];

static void VecRefScan(wchar_t ch, int* pT, int* pB)
{
    int y, x, gw, scanW;
    unsigned char* bits;
    int pitch;
    if (GtRenderGlyphAt(ch, GL_M, GL_Y0, NULL) <= 0) return;
    gw = 0;
    {
        SIZE sz;
        if (g_G && g_G->dc && GetTextExtentPoint32W(g_G->dc, &ch, 1, &sz))
            gw = sz.cx;
    }
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    bits = g_G->bits;
    pitch = g_G->pitch;
    for (y = 0; y < DIB_H; y++)
    {
        unsigned char* row = bits + y * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (y < *pT) *pT = y; if (y > *pB) *pB = y; }
    }
}

static void VecRefCalibrate(int px)
{
    int T, B;
    char b1[16], b2[16], b3[16], buf[160];
    unsigned p;
    if (g_RefValid && g_RefPx == px && g_RefBold == g_VT.Bold &&
        FaceSame(g_RefFace, g_VTFace)) return;
    T = DIB_H; B = -1;
    VecRefScan(0x6C38, &T, &B);   /* 永 */
    VecRefScan(0x0067, &T, &B);   /* g */
    VecRefScan(0x006A, &T, &B);   /* j */
    if (B < T) { T = GL_Y0; B = GL_Y0 + px - 1; }   /* 量不到就退化为等同 */
    g_RefValid = 1; g_RefPx = px; g_RefBold = g_VT.Bold;
    g_RefT = T; g_RefB = B;
    CopyFace(g_RefFace, g_VTFace);
    p = 0;
    p = AppendCStr(buf, p, "vref: px=", sizeof(buf));
    UtoA((unsigned)px, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " T=", sizeof(buf));
    UtoA((unsigned)T, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
    p = AppendCStr(buf, p, " B=", sizeof(buf));
    UtoA((unsigned)B, b3); p = AppendCStr(buf, p, b3, sizeof(buf));
    LogLine(buf);
}

/* ---------------- 字号校准 ----------------
 * GDI 字号按原字形真实高度定: InternalData->Lines 在运行时从引擎内存读
 * (不是读 game.fnt 文件, 文件换了也自动跟)。试渲染"永",
 * px = px0*origH/inkH, 墨水高度线性跟字号, 一步收敛。
 * 键=(idata,face,bold), 字体重载/换字体自动重算。 */

static int g_PxValid = 0;
static void* g_PxIdata = NULL;
static int g_PxPx = 19, g_PxBold = -1, g_PxPxSrc = 0;
static wchar_t g_PxFace[FONT_FACE_MAX];

static int VecPxCalibrate(void* t)
{
    void* idata;
    int origH, px0, T, B, inkH, px, gw, x, y, scanW;
    unsigned char* bits;
    int pitch;
    char b1[16], b2[16], b3[16], buf[160];
    unsigned p;
    if (!t || !IsPlausiblePtr(t)) return -1;
    idata = *(void**)((char*)t + 4);
    if (g_PxValid && g_PxIdata == idata && g_PxBold == g_VT.Bold &&
        g_PxPxSrc == (g_VT.FontPx > 0 ? g_VT.FontPx : 0) &&
        FaceSame(g_PxFace, g_VTFace)) return g_PxPx;
    if (!idata || !IsPlausiblePtr(idata)) return -1;
    if (g_VT.FontPx > 0)
    {
        /* 固定字号: 只放大墨水, 格宽/行距/advance 全不动 (大了会蹭到相邻行) */
        px = g_VT.FontPx;
        if (px < 8) px = 8;
        if (px > 40) px = 40;
        origH = *(int*)((char*)idata + 0x0C);
        if (origH < 8 || origH > 40) origH = 19;
        g_PxValid = 1; g_PxIdata = idata; g_PxPx = px; g_PxBold = g_VT.Bold;
        g_PxPxSrc = g_VT.FontPx;
        CopyFace(g_PxFace, g_VTFace);
        p = 0;
        p = AppendCStr(buf, p, "vpx: fixed px=", sizeof(buf));
        UtoA((unsigned)px, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " origH=", sizeof(buf));
        UtoA((unsigned)origH, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
        LogLine(buf);
        return px;
    }
    origH = *(int*)((char*)idata + 0x0C);   /* Lines: 原字形数据行数 */
    if (origH < 8 || origH > 40) return -1;
    px0 = 18;
    if (!GdiEnsure(px0, g_VT.Bold, 2, g_VTFace)) return -1;
    if (GtRenderGlyphAt(0x6C38, GL_M, GL_Y0, NULL) <= 0) return -1;
    {
        SIZE sz;
        wchar_t ch = 0x6C38;
        gw = 0;
        if (g_G && g_G->dc && GetTextExtentPoint32W(g_G->dc, &ch, 1, &sz))
            gw = sz.cx;
    }
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    bits = g_G->bits;
    pitch = g_G->pitch;
    T = DIB_H; B = -1;
    for (y = 0; y < DIB_H; y++)
    {
        unsigned char* row = bits + y * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (y < T) T = y; if (y > B) B = y; }
    }
    if (B < T) return -1;
    inkH = B - T + 1;
    px = (px0 * origH + inkH / 2) / inkH;
    if (px < 8) px = 8;
    if (px > 40) px = 40;
    g_PxValid = 1; g_PxIdata = idata; g_PxPx = px; g_PxBold = g_VT.Bold;
    g_PxPxSrc = 0;
    CopyFace(g_PxFace, g_VTFace);
    p = 0;
    p = AppendCStr(buf, p, "vpx: origH=", sizeof(buf));
    UtoA((unsigned)origH, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " inkH=", sizeof(buf));
    UtoA((unsigned)inkH, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
    p = AppendCStr(buf, p, " px=", sizeof(buf));
    UtoA((unsigned)px, b3); p = AppendCStr(buf, p, b3, sizeof(buf));
    LogLine(buf);
    return px;
}

static VCEnt* VecGlyph(void* t, wchar_t wch, int adv, int h)
{
    unsigned idx = ((unsigned)wch) & VC_MASK;
    VCEnt* e = &g_VC[idx];
    void* idata = *(void**)((char*)t + 4);
    int gw, dx, y, L, R, inkW, x, px;
    /* 字形四周边距: GDI 墨水会超出 extent (左右 overhang), 顶边画必被裁。
     * 垂直用基准窗映射 (见 VecRefCalibrate), 基线全字体一致。
     * 字号已按原字形高度校准, 窗行->格行 1:1, 不压不设上限。 */
    int scanW;
    unsigned char* bits;
    int pitch;
    int winTop;
    if (!t || !IsPlausiblePtr(t)) return NULL;
    px = VecPxCalibrate(t);
    if (px < 0) return NULL;
    if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
        e->adv == adv && e->h == h)
        return e;
    /* 未命中: GDI 单字光栅 (带边距), 掃出真实墨水盒再摆进格子 */
    if (!GdiEnsure(px, g_VT.Bold, 2, g_VTFace)) return NULL;
    VecRefCalibrate(px);
    gw = GtRenderGlyphAt(wch, GL_M, GL_Y0, NULL);
    if (gw <= 0 || h > VC_MAXH) return NULL;
    bits = g_G->bits;
    pitch = g_G->pitch;
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    /* 基准窗: 上界=基准顶。字号已校准, 墨水天然 fits, 1:1 摆, 不压。 */
    winTop = g_RefT;
    if (winTop < 0 || winTop + h > DIB_H) return NULL;
    /* 墨水盒 (只掃窗内列; 行按映射取) */
    L = scanW; R = -1;
    for (y = 0; y < h; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
    }
    for (y = 0; y < h; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        unsigned char* dst = e->cov + y * VC_MAXW;
        for (dx = 0; dx < adv; dx++) dst[dx] = 0;
        if (R < L) continue;   /* 空白字 (空格): 占宽, 不画 */
        inkW = R - L + 1;
        {
            /* 摆位: 满格 (CJK/数字) 左对齐, 与原版起笔一致;
             * 窄字形 (标点等, 墨水不足格宽 2/3) 按 GDI 自然边距摆,
             * 格子比 GDI 步进宽出的部分左右均分 (如":"不再贴边)。
             * 超宽才水平挤, 起点为摆位偏移。 */
            int off = 0, avail;
            if (inkW * 3 <= adv * 2 && g_G && g_G->dc)
            {
                ABC abc;
                if (GetCharABCWidthsW(g_G->dc, wch, wch, &abc))
                {
                    int gdiAdv = abc.abcA + abc.abcB + abc.abcC;
                    off = abc.abcA + (adv - gdiAdv) / 2;
                    if (off < 0) off = 0;
                    if (off > adv - 1) off = adv - 1;
                }
            }
            avail = adv - off;
            if (avail < 1) avail = 1;
            if (inkW <= avail)
            {
                int n = inkW < avail ? inkW : avail;
                for (dx = 0; dx < n; dx++) dst[off + dx] = row[(L + dx) * 4 + 2];
            }
            else
            {
                for (dx = 0; dx < avail; dx++)
                {
                    int sx = L + (dx * inkW) / avail;
                    dst[off + dx] = row[sx * 4 + 2];
                }
            }
        }
    }
    e->used = 1; e->wch = wch; e->idata = idata; e->px = px; e->adv = adv; e->h = h;
    return e;
}

/* 矢量替换: 成功返回 X+adv (与原版返回值口径一致), 失败返回 -1 走原版 */
static int VecRender(void* t, wchar_t wch, int adv, int h, int x, int y, int colorW)
{
    unsigned short* buf;
    int pitchW, L, Tp, R, B, dx, dy;
    unsigned char fr, fg, fb;
    VCEnt* e;
    if (!t || !IsPlausiblePtr(t)) return -1;
    if (adv > VC_MAXW || h > VC_MAXH) return -1;
    buf = *(unsigned short**)((char*)t + 0x0C);
    pitchW = *(int*)((char*)t + 0x10);
    if (!buf || !IsPlausiblePtr(buf)) return -1;
    if (pitchW < 64 || pitchW > 4096) return -1;
    L = *(int*)((char*)t + 0x30);
    Tp = *(int*)((char*)t + 0x34);
    R = *(int*)((char*)t + 0x38);
    B = *(int*)((char*)t + 0x3C);
    if (R < L || B < Tp) return -1;
    if (L < 0) L = 0;
    if (Tp < 0) Tp = 0;
    if (R > 4096) R = 4096;
    if (B > 4096) B = 4096;
    Calibrate();
    e = VecGlyph(t, wch, adv, h);
    if (!e) return -1;
    GtUnpack16((unsigned short)(colorW & 0xFFFF), &fr, &fg, &fb);
    dy = y + g_VT.VShift;
    for (y = 0; y < h; y++)
    {
        int sy = dy + y;
        unsigned short* row;
        unsigned char* cov;
        if (sy < Tp || sy > B) continue;
        row = buf + sy * pitchW;
        cov = e->cov + y * VC_MAXW;
        for (dx = 0; dx < adv; dx++)
        {
            int sx = x + dx;
            unsigned a;
            if (sx < L || sx > R) continue;
            if (!cov[dx]) continue;
            a = cov[dx];   /* 灰度即 alpha, 全浓度 */
            row[sx] = Blend16(row[sx], fr, fg, fb, a);
        }
    }
    return x + adv;
}

int __fastcall VecText_BlitHook(void* t, void* edxUnused, int wch, int x, int y, int color)
{
    wchar_t wc = (wchar_t)((unsigned)wch & 0xFFFFu);
    int eff, adv, h = 0, r;
    (void)edxUnused;
    if (!g_OrigBlit) return 0;   /* 不该发生, 守一下不断原版链 */
    g_BlitN++;
    /* TAB 走原版特殊公式, 缺字/度量失败也走原版 (保留 0x5555 兜底行为) */
    if (!t || !IsPlausiblePtr(t) || wc == 9)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    eff = (color == -1) ? (int)(*(unsigned short*)((char*)t + 0x24)) : color;
    adv = VecMeasure(t, wc, &h);
    if (adv < 0)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    if (g_VT.LogAdv) VecProbeLog(wc, adv, h, eff);
    if (g_VT.Render)
    {
        r = VecRender(t, wc, adv, h, x, y, eff);
        if (r >= 0) return r;
    }
    return g_OrigBlit(t, NULL, wch, x, y, color);
}

/* ---------------- 下钩 ---------------- */

static int BytesSame(const unsigned char* a, const unsigned char* b, int n)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int BuildTramp(void)
{
    unsigned char* b;
    int i;
    b = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!b) return 0;
    for (i = 0; i < 7; i++) b[i] = ((unsigned char*)ADDR_BLIT)[i];
    b[7] = 0xE9;   /* jmp 回 Blit+7 */
    *(int*)(b + 8) = (int)(ADDR_BLIT_BACK - ((unsigned)(size_t)b + 12));
    FlushInstructionCache(GetCurrentProcess(), b, 12);
    g_OrigBlit = (BlitFn)b;
    return 1;
}

static int PatchBlit(void)
{
    unsigned char* p = (unsigned char*)ADDR_BLIT;
    DWORD old;
    unsigned target = (unsigned)(size_t)VecText_BlitHook;
    int rel = (int)(target - ((unsigned)(size_t)p + 5));
    if (!VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0xE9;
    *(int*)(p + 1) = rel;
    p[5] = 0x90; p[6] = 0x90;
    VirtualProtect(p, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 7);
    return 1;
}

static void LogBytes(const unsigned char* p)
{
    char b1[16], buf[128];
    unsigned q = 0;
    int i;
    q = AppendCStr(buf, q, "vtext: got=", sizeof(buf));
    for (i = 0; i < 7; i++)
    {
        UtoA(p[i], b1);
        q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCh(buf, q, ' ', sizeof(buf));
    }
    LogLine(buf);
}

void VecText_Install(void)
{
    char b1[16], buf[192];
    unsigned p;
    GtEnsureLog();
    if (!g_Log) return;   /* 日志都打不开就不装钩 (无意义) */
    g_VT.Enable = GetPrivateProfileIntA(VT_SECTION, "Enable", 1, g_IniPath);
    GetPrivateProfileStringA(VT_SECTION, "FontFace", "simhei.ttf",
                             g_VT.FontFace, sizeof(g_VT.FontFace), g_IniPath);
    g_VT.FontFace[sizeof(g_VT.FontFace) - 1] = 0;
    g_VT.Bold = GetPrivateProfileIntA(VT_SECTION, "Bold", 0, g_IniPath);
    g_VT.FontPx = GetPrivateProfileIntA(VT_SECTION, "FontPx", 0, g_IniPath);
    g_VT.VShift = GetPrivateProfileIntA(VT_SECTION, "VShift", 0, g_IniPath);
    g_VT.LogAdv = GetPrivateProfileIntA(VT_SECTION, "LogAdv", 1, g_IniPath);
    g_VT.Render = GetPrivateProfileIntA(VT_SECTION, "Render", 0, g_IniPath);
    g_VT.Enable = g_VT.Enable ? 1 : 0;
    g_VT.Bold = g_VT.Bold ? 1 : 0;
    if (g_VT.FontPx < 0) g_VT.FontPx = 0;
    if (g_VT.FontPx > 40) g_VT.FontPx = 40;
    if (g_VT.VShift < -8) g_VT.VShift = -8;
    if (g_VT.VShift > 8) g_VT.VShift = 8;
    g_VT.LogAdv = g_VT.LogAdv ? 1 : 0;
    g_VT.Render = g_VT.Render ? 1 : 0;
    g_GetDim = (GetDimFn)ADDR_GETDIM;
    ResolveFontFace(g_VT.FontFace, "vtext", g_VTFace, L"SimHei");
    p = 0;
    p = AppendCStr(buf, p, "vtext: start en=", sizeof(buf));
    UtoA((unsigned)g_VT.Enable, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " log=", sizeof(buf));
    UtoA((unsigned)g_VT.LogAdv, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " render=", sizeof(buf));
    UtoA((unsigned)g_VT.Render, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " px=", sizeof(buf));
    UtoA((unsigned)g_VT.FontPx, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
    if (!g_VT.Enable) { LogLine("vtext: disabled"); return; }
    if (!BytesSame((unsigned char*)ADDR_BLIT, kWantBlit, 7))
    {
        LogLine("vtext: blit bytes mismatch, hook OFF");
        LogBytes((unsigned char*)ADDR_BLIT);
        return;
    }
    if (!BuildTramp()) { LogLine("vtext: tramp fail, hook OFF"); return; }
    if (!PatchBlit()) { LogLine("vtext: patch fail, hook OFF"); return; }
    LogLine(g_VT.Render ? "vtext: blit hook ON (render)" : "vtext: blit hook ON (probe)");
}
