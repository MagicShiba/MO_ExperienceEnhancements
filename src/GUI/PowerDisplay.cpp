/*
 * PowerDisplay.cpp - 底部电量指示, 见 PowerDisplay.h。
 *
 * 只画侧栏面, 随条带上屏: (1) 消息帧 (渲染内由 MOExperience.cpp 调), (2) prepare
 * (每循环, 裸钩 MOExperience_PrepareEntry), (3) BlitSidebar 入口 (每次条带呈现前
 * 先恢复, 裸钩 MOExperience_BlitSbEntry)。直画 Primary/Hidden/Composite 会盖过
 * 鼠标且侧栏不呈现时仍冒字; 侧栏面一份 (168 侧栏坐标) 层级天然在鼠标之下。
 * 上屏链反汇编实证见 VecText 笔记"底部电量"节。
 *
 * 总开关: [VecText] PowerBottom=0 即关 (缺省 1 开)。
 * 内容 "闪电+负载/电力" (PowerMode=1 则"闪电+余量纯数字"): 侧栏内居中
 * (PowerX>0 则侧栏绝对 x), y 锚底边 (y0=H-PowerPad-adv, 0 即贴底)。
 * 数值: CurrentPlayer(0xA83D4C)+0x53A4=PowerOutput/+0x53A8=PowerDrain
 * (UpdatePower 0x508C30 反汇编实证, VecText 笔记 v88 节)。
 * 颜色按游戏比例 (Phobos PowerDelta 默认 0.75/1.0): d*4<o*3 绿, d<=o 黄,
 * 否则红。字号/字体/粗体对标资金: CreditPx>0 即资金真字号 (同 GDI 管线, 面取
 * VecText_FontFace); CreditPx=0 时退原字行高 (无+4)。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "VecText.h"
#include "PowerDisplay.h"

typedef struct {
    int Bottom; /* 1=底部电量显示 (闪电+负载/电力, 侧栏面) */
    int X;      /* 0=侧栏内居中 (默认); >0=侧栏绝对 x */
    int Pad;    /* 底部电量距底边像素 (默认 6) */
    int Mode;   /* 0=负载/电力 (默认); 1=余量纯数字 (够用正/不够负) */
} PowerCfg;

static PowerCfg g_Cfg;
static int g_InitDone = 0;

void PowerDisplay_EnsureInit(void)
{
    if (g_InitDone) return;
    g_InitDone = 1;
    GtEnsureLog();
    if (!g_Log) return;
    /* 电力键归 [VecText] (挨着 CreditPx/CreditDY), 不在 [MsgText]。 */
    g_Cfg.Bottom = GetPrivateProfileIntA(VT_SECTION, "PowerBottom", 1, g_IniPath);
    g_Cfg.X = GetPrivateProfileIntA(VT_SECTION, "PowerX", 0, g_IniPath);
    g_Cfg.Pad = GetPrivateProfileIntA(VT_SECTION, "PowerPad", 6, g_IniPath);
    g_Cfg.Mode = GetPrivateProfileIntA(VT_SECTION, "PowerMode", 0, g_IniPath);
    g_Cfg.Bottom = g_Cfg.Bottom ? 1 : 0;
    if (g_Cfg.X < 0) g_Cfg.X = 0;
    if (g_Cfg.X > 4096) g_Cfg.X = 4096;
    if (g_Cfg.Pad < 0) g_Cfg.Pad = 0;
    if (g_Cfg.Pad > 200) g_Cfg.Pad = 200;
    g_Cfg.Mode = g_Cfg.Mode ? 1 : 0;
}

#define ADDR_PLAYERP 0xA83D4Cu
#define HOUSE_POWER_OUT 0x53A4
#define HOUSE_POWER_DRAIN 0x53A8
/* 闪电 10x16 二值点阵 ('X'=墨): Segoe UI Emoji U+26A1 剪影阈值烘焙,
 * 不透明直写 (幂等, 不叠加不闪); 文本仍走 Blend16。 */
static const char* kPwrBolt[16] = {
    "......X...",
    ".....XX...",
    ".....XX...",
    "....XXX...",
    "...XXXX...",
    "..XXXXX...",
    "..XXXXX...",
    ".XXXXXX...",
    "XXXXXXXXXX",
    "...XXXXXX.",
    "...XXXXX..",
    "...XXXX...",
    "...XXXX...",
    "...XXX....",
    "...XX.....",
    "...X......",
};
#define PWR_BOLT_W 10
#define PWR_BOLT_H 16
#define PWR_COV_W 128
#define PWR_COV_H 48
/* 底电字形缓存 (值/字号不变即复用, 变了才重光栅+拷 DIB)。 */
static struct {
    int drain, output, fontPx, bold, mode, w, adv;
    unsigned char cov[PWR_COV_W * PWR_COV_H];
} g_PwrCache = { -1, -1, -1, -1, -1, 0, 0 };

/* 读值+配字号+备字形: 1=缓存有效, 0=跳过(无玩家/值非法), -1=GDI失败/超大。
 * 无锁无面, 锁内可调。 */
static int PwrCacheEnsure(void)
{
    void* house;
    int output, drain, bitLineH, credPx, fontPx, adv, w, i, n;
    wchar_t line[32];
    char b1[16], b2[16];
    house = *(void**)(size_t)ADDR_PLAYERP;
    if (!house || !IsPlausiblePtr(house)) return 0;
    output = *(int*)((char*)house + HOUSE_POWER_OUT);
    drain = *(int*)((char*)house + HOUSE_POWER_DRAIN);
    if (output < 0 || drain < 0 || output > 2000000 || drain > 2000000)
        return 0;
    bitLineH = GtBitLineHeight();
    credPx = VecText_CreditPx();
    fontPx = credPx > 0 ? credPx : bitLineH;
    if (fontPx < 8) fontPx = 8;
    if (fontPx > 64) fontPx = 64;
    if (output == g_PwrCache.output && drain == g_PwrCache.drain &&
        fontPx == g_PwrCache.fontPx && VecText_Bold() == g_PwrCache.bold &&
        g_Cfg.Mode == g_PwrCache.mode && g_PwrCache.w > 0)
        return 1;
    /* 与资金同字体: 面/字号/粗体全取 VecText (slot 3 独立, 字形与资金一致)。 */
    if (!GdiEnsure(fontPx, VecText_Bold(), 3, VecText_FontFace())) return -1;
    adv = g_G->adv > 0 ? g_G->adv : fontPx;
    n = 0;
    if (g_Cfg.Mode == 1)
    {
        /* 余量纯数字: 输出-负载, 够用正数/不够负数 (颜色仍走三色)。 */
        int delta = output - drain;
        if (delta < 0) { line[n++] = (wchar_t)'-'; delta = -delta; }
        UtoA((unsigned)delta, b1);
        for (i = 0; b1[i] && n < 30; i++) line[n++] = (wchar_t)(unsigned char)b1[i];
    }
    else
    {
        UtoA((unsigned)drain, b1);
        for (i = 0; b1[i] && n < 30; i++) line[n++] = (wchar_t)(unsigned char)b1[i];
        line[n++] = (wchar_t)'/';
        UtoA((unsigned)output, b2);
        for (i = 0; b2[i] && n < 30; i++) line[n++] = (wchar_t)(unsigned char)b2[i];
    }
    w = RenderLine(line, n);
    if (w <= 0 || w + 2 > PWR_COV_W || adv > PWR_COV_H) return -1;
    {
        int yy, xx;
        for (yy = 0; yy < adv; yy++)
        {
            unsigned char* srow = g_G->bits + yy * g_G->pitch;
            unsigned char* drow = g_PwrCache.cov + yy * PWR_COV_W;
            for (xx = 0; xx < w + 2; xx++) drow[xx] = srow[xx * 4 + 2];
        }
    }
    g_PwrCache.output = output; g_PwrCache.drain = drain;
    g_PwrCache.fontPx = fontPx; g_PwrCache.bold = VecText_Bold();
    g_PwrCache.mode = g_Cfg.Mode;
    g_PwrCache.w = w; g_PwrCache.adv = adv;
    return 1;
}
/* 缓存字落面 (调用方保证面已锁定): 纯内存写, 锁内安全。 */
static void PwrBlitCache(unsigned char* dstBase, int pitch, int W, int H,
                         int dx, int dy, unsigned char fr, unsigned char fg,
                         unsigned char fb, unsigned alpha)
{
    int w = g_PwrCache.w, adv = g_PwrCache.adv, y, x;
    for (y = 0; y < adv; y++)
    {
        int sy = dy + y;
        unsigned char* srow;
        unsigned short* drow;
        if (sy < 0 || sy >= H) continue;
        srow = g_PwrCache.cov + y * PWR_COV_W;
        drow = (unsigned short*)(dstBase + sy * pitch);
        for (x = 0; x < w + 2; x++)
        {
            int sx = dx + x;
            unsigned a;
            if (sx < 0 || sx >= W) continue;
            if (!srow[x]) continue;
            a = (srow[x] * alpha + 50u) / 100u;
            if (a > 255u) a = 255u;
            drow[sx] = Blend16(drow[sx], fr, fg, fb, a);
        }
    }
}
/* 闪电落面 (调用方保证面已锁定): 纯内存写, 锁内安全。
 * 二值不透明 (幂等, 无叠加闪烁); 颜色经 Blend16(a=255, 恒等于直写),
 * 与文本逐位同色。 */
static void PwrBoltPx(unsigned char* dstBase, int pitch, int W, int H,
                      int x0, int by, unsigned char fr, unsigned char fg,
                      unsigned char fb)
{
    int r, c;
    for (r = 0; r < PWR_BOLT_H; r++)
    {
        int sy = by + r;
        unsigned short* row16;
        if (sy < 0 || sy >= H) continue;
        row16 = (unsigned short*)(dstBase + sy * pitch);
        for (c = 0; c < PWR_BOLT_W; c++)
        {
            int sx = x0 + c;
            if (kPwrBolt[r][c] != 'X') continue;
            if (sx < 0 || sx >= W) continue;
            row16[sx] = Blend16(row16[sx], fr, fg, fb, 255);
        }
    }
}
/* 底电配色 (值->RGB): 与缓存值同口径, 调用方先 Ensure。 */
static void PwrColor(unsigned char* fr, unsigned char* fg, unsigned char* fb)
{
    *fr = 255; *fg = 0; *fb = 0;                       /* 红: 入不敷出 */
    if (g_PwrCache.output > 0 &&
        g_PwrCache.drain * 4 < g_PwrCache.output * 3)
    { *fr = 0; *fg = 255; *fb = 0; }                   /* 绿: 充足 */
    else if (g_PwrCache.drain <= g_PwrCache.output)
    { *fr = 255; *fg = 255; *fb = 0; }                 /* 黄: 危险 */
}
/* 底电版式 (侧栏面相对坐标): x0 居中 (PowerX>0 则绝对), y0 锚底边。
 * 只服务侧栏面, 不再有屏坐标系。调用方先 PwrCacheEnsure。
 * y0 = H - PowerPad - adv, PowerPad=0 即贴底, 由配置全权控制。 */
static void PwrLayout(int W, int H, int* x0, int* y0)
{
    int sx = g_Cfg.X;
    if (sx <= 0)
    {
        sx = (W - (PWR_BOLT_W + 4 + g_PwrCache.w)) / 2;
        if (sx < 0) sx = 0;
    }
    *x0 = sx;
    *y0 = H - g_Cfg.Pad - g_PwrCache.adv;
    if (*y0 < 0) *y0 = 0;
}

int PowerDisplay_DrawSb(void)
{
    void* sb;
    int sbW, sbH, sbPitch;
    int x0, y0, tx, by;
    unsigned char fr, fg, fb;
    unsigned char* locked;
    int cr;
    if (!g_Cfg.Bottom) return 0;
    sb = *(void**)ADDR_SURF_SIDEBAR;
    if (!sb || !IsPlausiblePtr(sb)) return 0;
    /* 先过玩家/数值门 (无面操作): 半初始化态直接跳过。 */
    cr = PwrCacheEnsure();
    if (cr <= 0) return (cr < 0) ? -1 : 0;
    sbW = GtSurfGet(sb, GtVT_GET_WIDTH);
    sbH = GtSurfGet(sb, GtVT_GET_HEIGHT);
    sbPitch = GtSurfGet(sb, GtVT_GET_PITCH);
    if (sbW < 60 || sbW > 400 || sbH < 400 || sbPitch <= 0) return 0;
    /* PwrBoltPx / PwrBlitCache 都按 16bpp 写; pitch < 2*width 就说明不是 16bpp 的面,
     * 那样每行会越过行尾、最后几行直接写出缓冲。*/
    if (sbPitch < sbW * 2) return 0;
    PwrColor(&fr, &fg, &fb);
    PwrLayout(sbW, sbH, &x0, &y0);
    locked = GtLock(sb);
    if (!locked) return -2;
    tx = x0 + PWR_BOLT_W + 4;
    by = y0 + (g_PwrCache.adv - PWR_BOLT_H) / 2;
    if (by < 0) by = 0;
    PwrBoltPx(locked, sbPitch, sbW, sbH, x0, by, fr, fg, fb);
    /* 电力文本无阴影 (用户要求)。 */
    PwrBlitCache(locked, sbPitch, sbW, sbH, tx, y0, fr, fg, fb, 100);
    GtUnlock(sb);
    {
        static int s_pwrLogged = 0;
        if (!s_pwrLogged)
        {
            char pb[128], t1[16], t2[16], t3[16], t4[16];
            unsigned q = 0;
            s_pwrLogged = 1;
            q = AppendCStr(pb, q, "mpwr: sb=", sizeof(pb));
            UtoA((unsigned)sbW, t1);
            q = AppendCStr(pb, q, t1, sizeof(pb));
            q = AppendCh(pb, q, 'x', sizeof(pb));
            UtoA((unsigned)sbH, t2);
            q = AppendCStr(pb, q, t2, sizeof(pb));
            q = AppendCStr(pb, q, " xy=", sizeof(pb));
            UtoA((unsigned)x0, t3);
            q = AppendCStr(pb, q, t3, sizeof(pb));
            q = AppendCh(pb, q, ',', sizeof(pb));
            UtoA((unsigned)y0, t4);
            q = AppendCStr(pb, q, t4, sizeof(pb));
            q = AppendCStr(pb, q, " d/o=", sizeof(pb));
            UtoA((unsigned)g_PwrCache.drain, t1);
            q = AppendCStr(pb, q, t1, sizeof(pb));
            q = AppendCh(pb, q, '/', sizeof(pb));
            UtoA((unsigned)g_PwrCache.output, t2);
            q = AppendCStr(pb, q, t2, sizeof(pb));
            q = AppendCStr(pb, q, " mode=", sizeof(pb));
            UtoA((unsigned)g_Cfg.Mode, t3);
            q = AppendCStr(pb, q, t3, sizeof(pb));
            q = AppendCStr(pb, q, " rgb=", sizeof(pb));
            UtoA((unsigned)fr, t1);
            q = AppendCStr(pb, q, t1, sizeof(pb));
            q = AppendCh(pb, q, ',', sizeof(pb));
            UtoA((unsigned)fg, t2);
            q = AppendCStr(pb, q, t2, sizeof(pb));
            q = AppendCh(pb, q, ',', sizeof(pb));
            UtoA((unsigned)fb, t3);
            q = AppendCStr(pb, q, t3, sizeof(pb));
            LogLine(pb);
        }
    }
    return 1;
}
