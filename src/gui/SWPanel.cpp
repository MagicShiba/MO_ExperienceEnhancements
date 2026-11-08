/*
 * SWPanel.cpp - 超武面板 (右下整块重绘), 配置节 [SWPanel]。
 *
 * 原版列表由 VecText 的 Print 入口整体隐藏+收集 (见 VecText.h 的收集表),
 * 此处每帧从收集表取新鲜行画统一面板: 单个半透明底 + 右/底边距锚定右下 +
 * 名左/计时器右分栏 + Ready 变绿。收集不到新鲜行 (菜单/无超武) 即隐藏,
 * 不可能画到别处。
 * 颜色用收集的原前景色; 150ms 未刷新即过期, 引擎闪烁原样透出。
 */

#include <windows.h>
#include <string.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "VecText.h"
#include "SWPanel.h"

typedef struct {
    int Enable;
    int FontPx;      /* 0 = 跟 BitFont 行高 */
    int Bold;
    char FontFace[64];
    int Opacity;     /* 底板不透明度 0~100 (0=不画底板, 文字阴影照画) */
    int RightMargin; /* 面板右沿距屏右像素 */
    int BottomMargin;/* 面板底沿距屏底像素 */
    int PadX;        /* 横向内边距像素 (左右) */
    int PadY;        /* 纵向内边距像素 (上下, 经典行距=字高+2*PadY) */
    int Gap;         /* 名列与计时器列间距像素 */
    int Classic;     /* 1=原版式 (右对齐+逐行贴合黑条+行回引擎原位), 0=统一面板 */
    int Shadow;      /* 1 = 画阴影 */
    int ShadowDX;    /* 阴影 X 偏移像素 */
    int ShadowDY;    /* 阴影 Y 偏移像素 */
    int ShadowOpacity; /* 阴影不透明度 0~100 */
    int ShadowSize;  /* 阴影粗细 (1=单层, >1 加厚) */
} SWPanelCfg;

static SWPanelCfg g_SW;
static wchar_t g_SWFace[FONT_FACE_MAX];
static int g_InitDone = 0;
/* 锚点丢失只记一次 (拿不到 g_MapRight 时贴左缘)。 */
static int s_AnchorLost = 0;
/* 字面指纹: 换字体后内容没变也得重建层 (hash 里混一份)。 */
static unsigned s_FaceHash = 0;

static unsigned SWFaceHash(const wchar_t* s)
{
    unsigned h = 2166136261u;
    int i;
    for (i = 0; s && s[i] && i < FONT_FACE_MAX; i++) { h ^= (unsigned)s[i]; h *= 16777619u; }
    return h;
}

void SWPanel_EnsureInit(void)
{
    char buf[256];
    unsigned p;
    if (g_InitDone) return;
    g_InitDone = 1;
    GtEnsureLog();
    if (!g_Log) return;
    g_SW.Enable = GetPrivateProfileIntA(SWPANEL_SECTION, "Enable", 1, g_IniPath);
    g_SW.FontPx = GetPrivateProfileIntA(SWPANEL_SECTION, "FontPx", 16, g_IniPath);
    g_SW.Bold = GetPrivateProfileIntA(SWPANEL_SECTION, "Bold", 0, g_IniPath);
    OU_ReadString(SWPANEL_SECTION, "FontFace", "msyh.ttc",
                  g_SW.FontFace, sizeof(g_SW.FontFace));
    g_SW.Opacity = GetPrivateProfileIntA(SWPANEL_SECTION, "Opacity", 60, g_IniPath);
    g_SW.RightMargin = GetPrivateProfileIntA(SWPANEL_SECTION, "RightMargin", 8, g_IniPath);
    g_SW.BottomMargin = GetPrivateProfileIntA(SWPANEL_SECTION, "BottomMargin", 8, g_IniPath);
    g_SW.PadX = GetPrivateProfileIntA(SWPANEL_SECTION, "PadX", 6, g_IniPath);
    g_SW.PadY = GetPrivateProfileIntA(SWPANEL_SECTION, "PadY", 2, g_IniPath);
    g_SW.Gap = GetPrivateProfileIntA(SWPANEL_SECTION, "Gap", 12, g_IniPath);
    g_SW.Classic = GetPrivateProfileIntA(SWPANEL_SECTION, "Classic", 0, g_IniPath);
    g_SW.Shadow = GetPrivateProfileIntA(SWPANEL_SECTION, "Shadow", 1, g_IniPath);
    g_SW.ShadowDX = GetPrivateProfileIntA(SWPANEL_SECTION, "ShadowDX", 1, g_IniPath);
    g_SW.ShadowDY = GetPrivateProfileIntA(SWPANEL_SECTION, "ShadowDY", 1, g_IniPath);
    g_SW.ShadowOpacity = GetPrivateProfileIntA(SWPANEL_SECTION, "ShadowOpacity", 60, g_IniPath);
    g_SW.ShadowSize = GetPrivateProfileIntA(SWPANEL_SECTION, "ShadowSize", 1, g_IniPath);

    g_SW.Enable = g_SW.Enable ? 1 : 0;
    g_SW.Bold = g_SW.Bold ? 1 : 0;
    if (g_SW.FontPx < 0) g_SW.FontPx = 0;
    if (g_SW.FontPx > 64) g_SW.FontPx = 64;
    if (g_SW.Opacity < 0) g_SW.Opacity = 0;
    if (g_SW.Opacity > 100) g_SW.Opacity = 100;
    if (g_SW.RightMargin < 0) g_SW.RightMargin = 0;
    if (g_SW.RightMargin > 2000) g_SW.RightMargin = 2000;
    if (g_SW.BottomMargin < 0) g_SW.BottomMargin = 0;
    if (g_SW.BottomMargin > 2000) g_SW.BottomMargin = 2000;
    if (g_SW.PadX < 0) g_SW.PadX = 0;
    if (g_SW.PadX > 64) g_SW.PadX = 64;
    if (g_SW.PadY < 0) g_SW.PadY = 0;
    if (g_SW.PadY > 64) g_SW.PadY = 64;
    if (g_SW.Gap < 0) g_SW.Gap = 0;
    if (g_SW.Gap > 200) g_SW.Gap = 200;
    g_SW.Classic = g_SW.Classic ? 1 : 0;
    g_SW.Shadow = g_SW.Shadow ? 1 : 0;
    if (g_SW.ShadowDX < 0) g_SW.ShadowDX = 0;
    if (g_SW.ShadowDX > 8) g_SW.ShadowDX = 8;
    if (g_SW.ShadowDY < 0) g_SW.ShadowDY = 0;
    if (g_SW.ShadowDY > 8) g_SW.ShadowDY = 8;
    if (g_SW.ShadowOpacity < 0) g_SW.ShadowOpacity = 0;
    if (g_SW.ShadowOpacity > 100) g_SW.ShadowOpacity = 100;
    if (g_SW.ShadowSize < 1) g_SW.ShadowSize = 1;
    if (g_SW.ShadowSize > 3) g_SW.ShadowSize = 3;
    g_SWPanelOn = g_SW.Enable;

    /* 字体解析放日志打开之后, 解析结果才能记入日志 */
    ResolveFontFace(g_SW.FontFace, "sw", g_SWFace, L"Microsoft YaHei");
    s_FaceHash = SWFaceHash(g_SWFace);
    p = 0;
    p = AppendCStr(buf, p, "config: SWPanel en=", sizeof(buf));
    p += UtoA((unsigned)g_SW.Enable, buf + p);
    p = AppendCStr(buf, p, " op=", sizeof(buf));
    p += UtoA((unsigned)g_SW.Opacity, buf + p);
    p = AppendCStr(buf, p, " px=", sizeof(buf));
    p += UtoA((unsigned)g_SW.FontPx, buf + p);
    p = AppendCStr(buf, p, " classic=", sizeof(buf));
    p += UtoA((unsigned)g_SW.Classic, buf + p);
    p = AppendCStr(buf, p, " gap=", sizeof(buf));
    p += UtoA((unsigned)g_SW.Gap, buf + p);
    LogLine(buf);
}

/* ---------------- 设置页运行期读写 ----------------
 * 只改内存里的 g_SW (帧内下次绘制读它), 不碰 INI; 落盘由设置页排队。 */

static SWPanelCfg s_SavedSw;
static int s_HaveSavedSw = 0;

int SWPanel_GetOption(int id)
{
    SWPanel_EnsureInit();
    switch (id)
    {
    case SWO_ENABLE:  return g_SW.Enable;
    case SWO_FONT_PX: return g_SW.FontPx;
    case SWO_BOLD:    return g_SW.Bold;
    case SWO_CLASSIC: return g_SW.Classic;
    case SWO_OPACITY: return g_SW.Opacity;
    default:          return 0;
    }
}

void SWPanel_SetOption(int id, int value)
{
    SWPanel_EnsureInit();
    switch (id)
    {
    case SWO_ENABLE:
        g_SW.Enable = value ? 1 : 0;
        /* 收集门看这个全局: 关掉就整块不画, 原版列表照常显示。 */
        g_SWPanelOn = g_SW.Enable;
        break;
    case SWO_FONT_PX:
        if (value < 0) value = 0;
        if (value > 64) value = 64;
        g_SW.FontPx = value;
        break;
    case SWO_BOLD:
        g_SW.Bold = value ? 1 : 0;
        break;
    case SWO_CLASSIC:
        g_SW.Classic = value ? 1 : 0;
        break;
    case SWO_OPACITY:
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        g_SW.Opacity = value;
        break;
    default:
        break;
    }
}

void SWPanel_GetFace(wchar_t* out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    SWPanel_EnsureInit();
    MultiByteToWideChar(CP_UTF8, 0, g_SW.FontFace, -1, out, cap);
    out[cap - 1] = 0;
}

void SWPanel_SetFace(const wchar_t* face)
{
    SWPanel_EnsureInit();
    if (!face || !face[0]) face = L"msyh.ttc";
    WideCharToMultiByte(CP_UTF8, 0, face, -1, g_SW.FontFace,
                        sizeof(g_SW.FontFace), NULL, NULL);
    g_SW.FontFace[sizeof(g_SW.FontFace) - 1] = 0;
    ResolveFontFace(g_SW.FontFace, "sw", g_SWFace, L"Microsoft YaHei");
    s_FaceHash = SWFaceHash(g_SWFace);
}

void SWPanel_SaveOptions(void)
{
    SWPanel_EnsureInit();
    s_SavedSw = g_SW;
    s_HaveSavedSw = 1;
}

void SWPanel_RestoreOptions(void)
{
    SWPanel_EnsureInit();
    if (!s_HaveSavedSw) return;
    g_SW = s_SavedSw;
    g_SWPanelOn = g_SW.Enable;
    ResolveFontFace(g_SW.FontFace, "sw", g_SWFace, L"Microsoft YaHei");
    s_FaceHash = SWFaceHash(g_SWFace);
}

static int SWLen(const wchar_t* s)
{
    int l = 0;
    while (s[l] && l < 63) l++;
    return l;
}

/* 原前景色解包 (非法值吃 kind 缺省: 名青/计时器白) */
static void SWRowColor(int fore, int dflt, ColorStruct* out)
{
    unsigned char r, g, b;
    int col = (fore >= 0 && fore <= 0xFFFFFF) ? fore : dflt;
    GtUnpack16((unsigned short)col, &r, &g, &b);
    out->R = r; out->G = g; out->B = b;
}

/* ================= 合成层缓存 =================
 * 面板每帧都得重画 (地图每帧把这块覆盖掉), 但内容只按秒变。全画要两轮
 * RenderLine 量宽 (10 次 GDI 度量) + 引擎 alpha 底板 + 阴影/正文逐像素, 约 200us/帧。
 * 内容与几何不变时把"底+阴影+正文"预合成到一张 32 位 alpha 层 (0xAARRGGBB,
 * A=0 = 透明), 每帧只做一趟整矩形 over (约 40us)。
 *
 * 层坐标相对包围盒左上角, 四周各留 PC_PAD 给阴影溢出 (ShadowDX/DY <= 8)。
 * hash 命中即认为内容/字体/几何/锚点全同 —— 布局与像素都可复用。 */
#define PC_PAD 8
#define PC_MAX_PX (4 * 1024 * 1024)

static unsigned* s_PcLay = NULL;
static int s_PcLayCap = 0;         /* 分配容量 (像素) */
static int s_PcW = 0, s_PcH = 0;   /* 当前层尺寸 */
static int s_PcX = 0, s_PcY = 0;   /* 层左上角在画面上的位置 */
static int s_PcValid = 0;
static unsigned s_PcHash = 0;

/* ---- 贴层分档 ----
 * 层的像素只有三种身份, 建层时一次分类 (tag), 贴层时按"段"走最省的路径:
 *   tag0 = 全透明 (A=0)      -> 整段跳过, 一个字节都不读写
 *   tag1 = 纯底板 (0x<A>000000) -> 底板是**纯黑 + 固定 alpha**, 混合结果逐通道
 *                                可分离, 用 32/64/32 项通道表直接算 (3 次小表查),
 *                                与 Blend16(dst,0,0,0,a) 逐位相同
 *   tag2 = 文字/阴影/边缘    -> 才走通用 Blend16
 * 一轮 47000 像素里 tag2 只有一两千 (字 + 阴影), 其余全是 tag1/tag0。 */
static unsigned char* s_PcTag = NULL;
static unsigned s_PcBgA = 0xFFFFFFFFu;   /* 通道表现对应的底板 alpha (0xFFFFFFFF = 未建) */
static unsigned short s_BgR[32], s_BgG[64], s_BgB[32];
static int s_BgBits6 = 6;

/* 底板通道表: 直接问 Blend16 要"只留一个通道"的答案, 保证与逐像素混合逐位一致。
 * 三通道项数不同: R/B 恒 32 项, G 在 565 下有 64 项 (6 位) —— 必须各建各的,
 * 按 32 项一起建会让 dst 绿分量 >=32 的像素查到表外 (恒 0), 整块底板偏品红。 */
static void PcBuildBgTab(unsigned a)
{
    int i, gm, rs;
    unsigned short v;
    if (s_PcBgA == a) return;
    s_PcBgA = a;
    s_BgBits6 = g_GBits;
    rs = (g_GBits == 6) ? 11 : 10;
    gm = (g_GBits == 6) ? 63 : 31;      /* 绿通道最大值 */
    for (i = 0; i < 32; i++)
    {
        v = Blend16((unsigned short)((unsigned)i << rs), 0, 0, 0, a);
        s_BgR[i] = (unsigned short)((v >> rs) & 31u);
        v = Blend16((unsigned short)i, 0, 0, 0, a);
        s_BgB[i] = (unsigned short)(v & 31u);
    }
    for (i = 0; i <= gm; i++)
    {
        v = Blend16((unsigned short)((unsigned)i << 5), 0, 0, 0, a);
        s_BgG[i] = (unsigned short)((v >> 5) & (unsigned)gm);
    }
}

static int PcEnsure(int w, int h)
{
    int need = w * h;
    unsigned* p;
    unsigned char* t;
    if (w <= 0 || h <= 0 || need <= 0 || need > PC_MAX_PX) return 0;
    if (need > s_PcLayCap)
    {
        /* 无 CRT: 走进程堆 (与 PlacementPreview.cpp 同做法)。 */
        HANDLE hp = GetProcessHeap();
        p = (unsigned*)(s_PcLay
            ? HeapReAlloc(hp, 0, s_PcLay, (size_t)need * sizeof(unsigned))
            : HeapAlloc(hp, 0, (size_t)need * sizeof(unsigned)));
        if (!p) return 0;
        s_PcLay = p;
        /* tag 与层同尺寸, 建层时一次重分配 */
        t = (unsigned char*)(s_PcTag
            ? HeapReAlloc(hp, 0, s_PcTag, (size_t)need)
            : HeapAlloc(hp, 0, (size_t)need));
        if (t) s_PcTag = t;
        s_PcLayCap = need;
        if (!t) return 0;
    }
    s_PcW = w; s_PcH = h;
    return 1;
}

/* 建层完: 按像素身份填 tag (a = 底板 alpha, 0 = 本帧没底板)。 */
static void PcBuildTag(unsigned a)
{
    int i, n;
    unsigned bv = a << 24;
    if (!s_PcLay || !s_PcTag) return;
    n = s_PcW * s_PcH;
    for (i = 0; i < n; i++)
    {
        unsigned v = s_PcLay[i];
        unsigned da = v >> 24;
        s_PcTag[i] = (unsigned char)(da ? (v == bv ? 1 : 2) : 0);
    }
    PcBuildBgTab(a);
}

/* 层内 over 合成 (src 非预乘, 8 位 alpha)。层初值 0 = 全透明。 */
static void PcOverAt(int x, int y, unsigned sr, unsigned sg, unsigned sb, unsigned sa)
{
    unsigned d, da, dr, dg, db, inv, oa, w1, w2;
    if (!s_PcLay || x < 0 || y < 0 || x >= s_PcW || y >= s_PcH || !sa) return;
    if (sa >= 255u)
    {
        s_PcLay[y * s_PcW + x] = 0xFF000000u | (sr << 16) | (sg << 8) | sb;
        return;
    }
    d = s_PcLay[y * s_PcW + x];
    da = d >> 24;
    if (!da)
    {
        s_PcLay[y * s_PcW + x] = (sa << 24) | (sr << 16) | (sg << 8) | sb;
        return;
    }
    inv = 255u - sa;
    w1 = sa;
    w2 = da * inv / 255u;
    oa = w1 + w2;
    if (!oa) { s_PcLay[y * s_PcW + x] = 0; return; }
    dr = (d >> 16) & 255u; dg = (d >> 8) & 255u; db = d & 255u;
    s_PcLay[y * s_PcW + x] = (oa << 24) |
        (((sr * w1 + dr * w2) / oa) << 16) |
        (((sg * w1 + dg * w2) / oa) << 8) |
        ((sb * w1 + db * w2) / oa);
}

/* 底板: 直接写 (此处层还是全透明, over 黑 == 赋值)。 */
static void PcFill(int x, int y, int w, int h, unsigned a)
{
    int i, j;
    if (!s_PcLay || !a) return;
    for (j = y; j < y + h; j++)
    {
        unsigned* row;
        if (j < 0 || j >= s_PcH) continue;
        row = s_PcLay + j * s_PcW;
        for (i = x; i < x + w; i++)
            if (i >= 0 && i < s_PcW) row[i] = (a << 24);
    }
}

/* 正文: 取 DIB 覆盖率 (g_G->bits) 的 [0,w)x[0,h) 段 over 到层 (lx,ly)。 */
static void PcStampCov(int lx, int ly, int w, int h, unsigned col, unsigned aScale)
{
    int y, x;
    unsigned sr = (col >> 16) & 255u, sg = (col >> 8) & 255u, sb = col & 255u;
    if (!g_G || !g_G->bits) return;
    for (y = 0; y < h; y++)
    {
        const unsigned char* row = g_G->bits + (size_t)y * g_G->pitch;
        for (x = 0; x < w; x++)
        {
            unsigned cov = row[x * 4 + 2];
            if (!cov) continue;
            if (aScale < 255u) cov = (cov * aScale + 127u) / 255u;
            PcOverAt(lx + x, ly + y, sr, sg, sb, cov);
        }
    }
}

/* 阴影: 与 BlitCoverageShadow 同一套"偏移并集"算法, 目标换成层, 纯黑 over。 */
static void PcShadow(int lx, int ly, int sw, int sh, int Size, int offX, int offY,
                     unsigned alphaScale)
{
    int X, Y, ox, oy;
    if (!s_PcLay || !g_G || !g_G->bits) return;
    if (Size < 1) Size = 1;
    if (Size > 4) Size = 4;
    if (alphaScale > 255u) alphaScale = 255u;
    for (Y = 0; Y < Size - 1 + sh; Y++)
    {
        int ty = ly + offY + Y;
        if (ty < 0 || ty >= s_PcH) continue;
        for (X = 0; X < Size - 1 + sw; X++)
        {
            unsigned m = 255u, a;
            int xs = lx + offX + X, any = 0;
            if (xs < 0 || xs >= s_PcW) continue;
            for (oy = 0; oy < Size; oy++)
            {
                int sy = Y - oy;
                if (sy < 0 || sy >= sh) continue;
                for (ox = 0; ox < Size; ox++)
                {
                    int sx = X - ox;
                    unsigned cov;
                    if (sx < 0 || sx >= sw) continue;
                    cov = g_G->bits[(size_t)sy * g_G->pitch + sx * 4 + 2];
                    if (!cov) continue;
                    a = (cov * alphaScale + 50u) / 100u;
                    if (a > 255u) a = 255u;
                    m = (m * (255u - a) + 127u) / 255u;
                    any = 1;
                }
            }
            if (!any) continue;
            PcOverAt(xs, ty, 0, 0, 0, 255u - m);
        }
    }
}

/* 画一行的一部分 (名或计时器): 渲染+阴影+正文 写进层。返回像素宽, <=0=无内容。 */
static int SWStampPart(int lx, int ly, const wchar_t* s, int adv,
                       unsigned fr, unsigned fg, unsigned fb)
{
    int l = SWLen(s), w;
    if (l <= 0) return 0;
    w = RenderLine(s, l);
    if (w <= 0) return 0;
    if (g_SW.Shadow && g_SW.ShadowOpacity > 0)
        PcShadow(lx, ly, w + 2, adv, g_SW.ShadowSize,
                 g_SW.ShadowDX, g_SW.ShadowDY, (unsigned)g_SW.ShadowOpacity);
    PcStampCov(lx, ly, w + 2, adv, (fr << 16) | (fg << 8) | fb, 255u);
    return w;
}

/* 层 over 到画面 (每帧唯一的一趟逐像素)。返回 1=画了。
 * 边界/裁剪全部提到循环外 (每像素只做一次 alpha 判断 + 一次混合)。 */
static int PcBlitToSurface(void* surf, int surfW, int surfH, int surfPitch)
{
    unsigned char* locked;
    int y, x0 = 0, x1 = s_PcW, y0 = 0, y1 = s_PcH;
    int rs, gm;
    if (!s_PcValid || !s_PcLay) return 0;
    if (!s_PcTag) return 0;
    if (s_PcX < 0) x0 = -s_PcX;
    if (s_PcX + s_PcW > surfW) x1 = surfW - s_PcX;
    if (s_PcY < 0) y0 = -s_PcY;
    if (s_PcY + s_PcH > surfH) y1 = surfH - s_PcY;
    if (x1 <= x0 || y1 <= y0) return 0;
    locked = GtLock(surf);
    if (!locked) return -2;
    rs = (s_BgBits6 == 6) ? 11 : 10;
    gm = (s_BgBits6 == 6) ? 63 : 31;
    for (y = y0; y < y1; y++)
    {
        const unsigned* lrow = s_PcLay + (size_t)y * s_PcW;
        const unsigned char* trow = s_PcTag + (size_t)y * s_PcW;
        unsigned short* drow = (unsigned short*)(locked + (size_t)(s_PcY + y) * surfPitch)
                             + s_PcX;
        int x = x0;
        while (x < x1)
        {
            unsigned char t = trow[x];
            int xe = x + 1;
            while (xe < x1 && trow[xe] == t) xe++;
            if (t == 1)
            {
                for (; x < xe; x++)
                {
                    unsigned d = drow[x];
                    drow[x] = (unsigned short)((s_BgR[(d >> rs) & 31u] << rs) |
                                               (s_BgG[(d >> 5) & (unsigned)gm] << 5) |
                                                s_BgB[d & 31u]);
                }
            }
            else if (t == 2)
            {
                for (; x < xe; x++)
                {
                    unsigned v = lrow[x];
                    unsigned a = v >> 24;
                    if (!a) continue;
                    drow[x] = Blend16(drow[x], (v >> 16) & 255u, (v >> 8) & 255u,
                                      v & 255u, a);
                }
            }
            else x = xe;
        }
    }
    GtUnlock(surf);
    return 1;
}

/* 内容/几何指纹: 命中即布局与像素都可复用 (文本逐字混入, 碰撞概率可忽略)。 */
static unsigned SWPcHash(int n, const int* ys, const int* ni, const int* ti,
                         int fontPx, int surfH)
{
    unsigned h = 2166136261u;
    int i, k;
#define SWMIX(v) do { h ^= (unsigned)(v); h *= 16777619u; } while (0)
    SWMIX(n); SWMIX(fontPx); SWMIX(surfH); SWMIX(g_MapRight);
    SWMIX(g_SW.Classic); SWMIX(g_SW.Opacity); SWMIX(g_SW.PadX); SWMIX(g_SW.PadY);
    SWMIX(g_SW.Gap); SWMIX(g_SW.RightMargin); SWMIX(g_SW.BottomMargin);
    SWMIX(g_SW.Shadow); SWMIX(g_SW.ShadowDX); SWMIX(g_SW.ShadowDY);
    SWMIX(g_SW.ShadowOpacity); SWMIX(g_SW.ShadowSize); SWMIX(g_SW.Bold);
    SWMIX(s_FaceHash);
    for (i = 0; i < n; i++)
    {
        SWMIX(ys[i]);
        for (k = 0; k < 2; k++)
        {
            int idx = (k == 0) ? ni[i] : ti[i];
            int j;
            SWMIX(idx);
            if (idx < 0) continue;
            SWMIX(g_SWCap[idx].fore);
            for (j = 0; j < 64 && g_SWCap[idx].s[j]; j++)
                SWMIX((unsigned)g_SWCap[idx].s[j]);
            SWMIX(0xFFFFu);
        }
    }
#undef SWMIX
    return h;
}

/* ================= 行几何 =================
 * 行数可能超过一屏高 (78 超武的大剧本 + 多人)，所以布局是一条降级链:
 *   行距 (经典式先吃 PadY) → 分栏 → 丢最早的几行
 * 列从左到右 = 引擎打印顺序 (先出现的行在左列顶部)，首列顶部留空槽让整块底对齐。
 * 绘制/底板/贴层都只读这套几何, 不许各自再算一遍。 */
#define SW_TOP_SAFE 32   /* 收集门只认 y>=32, 布局上沿守同一条线, 否则最上一行画进顶栏 */
#define SW_COL_GAP  12   /* 相邻两列内容右缘的间距 */
#define SW_MAX_COLS 4

/* 本列实际行数: 行按 seq 填 [excess, excess+shown), 只有最左列可能不满。 */
static int SWColRows(int col, int excess, int shown, int perCol)
{
    int lo = col * perCol;
    int hi = lo + perCol;
    int a = (excess > lo) ? excess : lo;
    int b = (excess + shown < hi) ? (excess + shown) : hi;
    return (b > a) ? (b - a) : 0;
}

/* 第 k 行: 所在列的右缘 + 槽顶 (画面坐标)。seq 一定落在 [0, cols*perCol) 内,
 * 越界不算: 调用方只传 shown 范围内的 k。 */
static void SWRowGeom(int k, int k0, int excess, int cols, int perCol, int colW,
                      int rightEdge, int top, int unit, int* outR, int* outY)
{
    int seq = excess + (k - k0);
    int col = seq / perCol;
    int slot = seq - col * perCol;
    *outR = rightEdge - (cols - 1 - col) * colW;
    *outY = top + slot * unit;
}

/* 经典式单行黑条左缘: 名右对齐到名栏; 没有名的行以计时器左缘为准。
 * 两栏都空的行 (只剩空串条目) 返回右缘 = 零宽条, 不画。 */
static int SWBarLeft(int k, int cr, const int* ws, const int* wt,
                     int maxTW, int hasTimer)
{
    int contentRight, nameEnd, nx, b;
    if (ws[k] <= 0 && wt[k] <= 0) return cr;
    contentRight = cr - g_SW.PadX;
    nameEnd = contentRight - (hasTimer ? maxTW + g_SW.Gap : 0);
    nx = (ws[k] > 0) ? nameEnd - ws[k] : contentRight - wt[k];
    b = nx - g_SW.PadX;
    return b < 0 ? 0 : b;
}

/* 主入口: 收集 → (命中?) 贴层 : 重量宽/布局/重建层 → 贴层
 * Classic=0: 右下锚定单个半透明底, 名左计时器右。
 * Classic=1: 原版式 (右对齐 + 逐行贴合黑条 + 名回引擎原位)。 */
int SWPanel_Draw(void* surf, int bitLineH, int surfW, int surfH, int surfPitch)
{
    int ys[SWROW_MAX], ni[SWROW_MAX], ti[SWROW_MAX];
    int ws[SWROW_MAX], wt[SWROW_MAX];
    int n = 0, i, j, k;
    unsigned now;
    int fontPx, adv, maxNW = 0, maxTW = 0, hasTimer = 0;
    int panelW, rightEdge = 0;
    int unit = 0, top = 0, anchor;
    int cols = 1, perCol = 1, slotN = 0, shown = 0, k0 = 0, excess = 0, slotTop = 0;
    int colW = 0, maxCols = 1;
    int lw, lh, lx, ly;
    unsigned alpha, hh;
    long long tA = 0;

    if (!g_SWPanelOn) { s_PcValid = 0; return 0; }
    now = GetTickCount();
    for (i = 0; i < g_SWCapN; i++)
    {
        SWCapRow* c = &g_SWCap[i];
        /* ★ 过期只用来剔"陈旧碎片", 不能让它把整块面板判没: 某一帧没收到 (切模式
         * 那几帧) 早退会让**原版列表**趁虚画出来 (inband 放行)。故放宽到
         * 1000ms: 宁可短暂显示上一次的内容, 也不能让面板凭空消失。*/
        if (now - c->tick > (unsigned)SW_STALE_MS) continue;
        /* 倒着找: 引擎按行连续打印, 同一 y 的条目就在刚处理过的那几个里。 */
        for (j = n - 1; j >= 0; j--) if (ys[j] == c->y) break;
        if (j < 0)
        {
            if (n >= SWROW_MAX) break;
            j = n;
            ys[n] = c->y; ni[n] = -1; ti[n] = -1; n++;
        }
        if (c->kind == 0) ni[j] = i;
        else ti[j] = i;
    }
    if (n <= 0) { s_PcValid = 0; return 0; }
    for (i = 1; i < n; i++)   /* 按 y 排序 (插入, 行数小): 只定上下顺序 */
    {
        int ty = ys[i], tn = ni[i], tt = ti[i];
        j = i - 1;
        while (j >= 0 && ys[j] > ty)
        {
            ys[j + 1] = ys[j]; ni[j + 1] = ni[j]; ti[j + 1] = ti[j];
            j--;
        }
        ys[j + 1] = ty; ni[j + 1] = tn; ti[j + 1] = tt;
    }
    fontPx = g_SW.FontPx > 0 ? g_SW.FontPx : bitLineH;

    /* ---- 命中: 内容与几何都没变, 直接贴层 (不量宽、不碰 GDI) ---- */
    hh = SWPcHash(n, ys, ni, ti, fontPx, surfH);
    if (s_PcValid && hh == s_PcHash)
        return PcBlitToSurface(surf, surfW, surfH, surfPitch);

    tA = Perf_Now();
    /* 槽号 9: 不能与 PowerDisplay (槽 3, 同一套 [VecText] 字体) 共用 —— 字号/字面
     * 不同会让 GdiEnsure 每次调用都拆了重建 (CreateDIBSection + CreateFontW)。 */
    if (!GdiEnsure(fontPx, g_SW.Bold, 9, g_SWFace)) return -1;
    adv = g_G->adv > 0 ? g_G->adv : fontPx + 2;
    for (k = 0; k < n; k++)   /* 先量后画 (DIB 复用, 画时重渲) */
    {
        int l;
        ws[k] = wt[k] = 0;
        if (ni[k] >= 0 && (l = SWLen(g_SWCap[ni[k]].s)) > 0)
            ws[k] = RenderLine(g_SWCap[ni[k]].s, l);
        if (ti[k] >= 0 && (l = SWLen(g_SWCap[ti[k]].s)) > 0)
        {
            wt[k] = RenderLine(g_SWCap[ti[k]].s, l);
            hasTimer = 1;
        }
        if (ws[k] > maxNW) maxNW = ws[k];
        if (wt[k] > maxTW) maxTW = wt[k];
    }
    Perf_Add(PF_SWP_MEAS, tA);
    if (maxNW <= 0 && maxTW <= 0) { s_PcValid = 0; return 0; }

    /* 锚点: 右缘锚到**地图区**而不是画布 (侧栏显示时地图只到 1752, 锚画布 1920
     * 会把整块列表塞到侧栏底下)。拿不到时**不要**静默退回 surfW —— 那正是
     * "隐藏侧栏后整块列表跳到屏幕最右缘"的表现, 退回 0 = 贴左缘。 */
    if (g_MapRight > 0 && g_MapRight <= surfW) anchor = g_MapRight;
    else
    {
        anchor = 0;
        if (!s_AnchorLost)
        {
            char b1[16], buf[128];
            unsigned q = 0;
            s_AnchorLost = 1;
            q = AppendCStr(buf, q, "swpanel: anchor lost, g_MapRight=", sizeof(buf));
            UtoA((unsigned)g_MapRight, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " surfW=", sizeof(buf));
            UtoA((unsigned)surfW, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            LogLine(buf);
        }
    }
    rightEdge = anchor - g_SW.RightMargin;
    if (rightEdge < 0) rightEdge = 0;
    panelW = g_SW.PadX * 2 + maxNW + (hasTimer ? g_SW.Gap + maxTW : 0);

    /* ---- 行距 → 分栏 → 丢行 (降级链, 见文件内 SW_TOP_SAFE 一节) ----
     * unit = 一个槽的行高: 经典式含上下 PadY (条要首尾相接), 统一面板只到字高。 */
    {
        int availH = surfH - g_SW.BottomMargin - SW_TOP_SAFE;
        if (availH < 1) availH = 1;
        unit = g_SW.Classic ? (adv + g_SW.PadY * 2) : adv;
        if (unit < 1) unit = 1;
        perCol = availH / unit;
        if (g_SW.Classic && perCol < n && unit > adv)  /* 一级: 先吃行内上下留白 */
        {
            unit = adv;
            perCol = availH / unit;
        }
        if (perCol < 1) perCol = 1;
        cols = (n + perCol - 1) / perCol;
        colW = panelW + SW_COL_GAP;
        maxCols = rightEdge / colW;
        if (maxCols < 1) maxCols = 1;
        if (maxCols > SW_MAX_COLS) maxCols = SW_MAX_COLS;
        if (cols > maxCols) cols = maxCols;
        slotN = cols * perCol;
        k0 = (n > slotN) ? (n - slotN) : 0;   /* 真放不下才丢, 丢的是最早的行 */
        shown = n - k0;
        excess = slotN - shown;
        top = surfH - g_SW.BottomMargin - perCol * unit;
        if (!g_SW.Classic) top -= g_SW.PadY;
        if (top < SW_TOP_SAFE) top = SW_TOP_SAFE;
    }
    alpha = (unsigned)(g_SW.Opacity > 0 ? g_SW.Opacity * 255 / 100 : 0);

    /* ---- 包围盒 → 层 ----
     * 纵向只取**用到的槽**: 行数少时 perCol 远大于 n, 按整列算会把层撑到整屏高
     * (多出来的透明带每帧白扫一遍)。首列顶部的空槽 (excess) 不算进来。 */
    lx = rightEdge;
    for (k = k0; k < k0 + shown; k++)
    {
        int cr, ry, b;
        SWRowGeom(k, k0, excess, cols, perCol, colW, rightEdge, top, unit, &cr, &ry);
        b = g_SW.Classic ? SWBarLeft(k, cr, ws, wt, maxTW, hasTimer) : cr - panelW;
        if (b < lx) lx = b;
    }
    slotTop = (cols > 1) ? 0 : excess;   /* 多列时右列顶到 0, 只首列有空槽 */
    ly = top + slotTop * unit - (g_SW.Classic ? 0 : g_SW.PadY);
    lw = rightEdge - lx;
    lh = (perCol - slotTop) * unit + (g_SW.Classic ? 0 : g_SW.PadY * 2);
    if (lw < 1 || lh < 1) { s_PcValid = 0; return 0; }
    s_PcX = lx - PC_PAD;
    s_PcY = ly - PC_PAD;
    if (!PcEnsure(lw + PC_PAD * 2, lh + PC_PAD * 2))
    {
        s_PcValid = 0;
        return 0;
    }
    memset(s_PcLay, 0, (size_t)s_PcW * s_PcH * sizeof(unsigned));

    /* 底板 (层坐标 = 画面坐标 - 层原点) */
    if (alpha)
    {
        if (g_SW.Classic)
        {
            for (k = k0; k < k0 + shown; k++)
            {
                int cr, ry, b, bw;
                SWRowGeom(k, k0, excess, cols, perCol, colW, rightEdge, top, unit, &cr, &ry);
                b = SWBarLeft(k, cr, ws, wt, maxTW, hasTimer);
                bw = cr - b;
                if (bw > 0) PcFill(PC_PAD + b - lx, PC_PAD + ry - ly, bw, unit, alpha);
            }
        }
        else
        {
            /* 统一面板: 每列一块, 底对齐 (只有最左列可能短) */
            for (i = 0; i < cols; i++)
            {
                int cnt = SWColRows(i, excess, shown, perCol);
                int cr = rightEdge - (cols - 1 - i) * colW;
                int py;
                if (cnt <= 0) continue;
                py = top + (perCol - cnt) * unit - g_SW.PadY;
                PcFill(PC_PAD + cr - panelW - lx, PC_PAD + py - ly, panelW,
                       cnt * unit + g_SW.PadY * 2, alpha);
            }
        }
    }

    /* 文字 */
    if (!g_Calibrated) Calibrate();
    for (k = k0; k < k0 + shown; k++)
    {
        int cr, ry, ty, contentRight, nameEnd, tx;
        ColorStruct fg;
        SWRowGeom(k, k0, excess, cols, perCol, colW, rightEdge, top, unit, &cr, &ry);
        /* 经典式字在条内居中 (unit - adv)/2 = PadY; 降级到 unit=adv 时自然为 0 */
        ty = ry + (g_SW.Classic ? ((unit - adv) / 2) : 0);
        contentRight = cr - g_SW.PadX;
        nameEnd = contentRight - (hasTimer ? maxTW + g_SW.Gap : 0);
        if (ni[k] >= 0 && ws[k] > 0)
        {
            SWRowColor(g_SWCap[ni[k]].fore, 0x07FF, &fg);
            tx = g_SW.Classic ? (nameEnd - ws[k]) : (cr - panelW + g_SW.PadX);
            SWStampPart(PC_PAD + tx - lx, PC_PAD + ty - ly, g_SWCap[ni[k]].s,
                        adv, fg.R, fg.G, fg.B);
        }
        if (ti[k] >= 0 && wt[k] > 0)
        {
            SWRowColor(g_SWCap[ti[k]].fore, 0xFFFF, &fg);
            tx = contentRight - wt[k];
            if (tx < cr - panelW + g_SW.PadX) tx = cr - panelW + g_SW.PadX;
            SWStampPart(PC_PAD + tx - lx, PC_PAD + ty - ly, g_SWCap[ti[k]].s,
                        adv, fg.R, fg.G, fg.B);
        }
    }
    s_PcHash = hh;
    s_PcValid = 1;
    PcBuildTag(alpha);
    tA = Perf_Now();
    k = PcBlitToSurface(surf, surfW, surfH, surfPitch);
    Perf_Add(PF_SWP_BG, tA);   /* 此处只记"贴层"这一趟 */
    return k;
}
