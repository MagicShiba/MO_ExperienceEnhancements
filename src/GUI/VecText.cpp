/* VecText.cpp - 全局文本矢量接管 (简报/按钮/顶栏数字等)。
 * 只换字形光栅, 度量/换行/对齐走引擎。异常回落原版。
 * BitFont 布局: +0x0C 像素缓冲, +0x10 pitch, +0x24 默认色, +0x30..0x3C 裁剪。
 */
#include <windows.h>
#include "GdiText.h"
#include "VecText.h"
#include "OrderUtil.h"
#include "Icon.h"
#include "FullMap.h"
#include "ScaleBar.h"
#include "MOExperience.h"

#define VC_MAXW 32
#define VC_MAXH 32

/* 后定义的前置声明 */
static int VecPickPx(void* t);
#define ADDR_BLIT        0x00434120u
#define ADDR_BLIT_BACK   0x00434127u
#define ADDR_GETDIM 0x00433CF0u
#define ADDR_GETDIM_BACK 0x00433CF6u
#define ADDR_DRAWTEXT    0x00434CD0u
#define ADDR_DRAWTEXT_BACK 0x00434CD6u
#define ADDR_PRINT       0x00434B90u
#define ADDR_PRINT_BACK  0x00434B9Au
#define ADDR_SIMPLE      0x004A5EB0u
#define ADDR_SIMPLE_BACK 0x004A5EB7u

/* Blit 头 7 字节, 下钩前校验 */
static const unsigned char kWantBlit[7] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x57 };
/* DrawText 头 6 字节 */
static const unsigned char kWantDrawText[6] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56 };
/* Print 头 10 字节 */
static const unsigned char kWantPrint[10] = { 0x56, 0x8B, 0x74, 0x24, 0x08,
                                              0x57, 0x8B, 0x7C, 0x24, 0x18 };
/* Simple_ 头 7 字节 */
static const unsigned char kWantSimple[7] = { 0x83, 0xEC, 0x28,
                                              0x8B, 0x44, 0x24, 0x34 };
/* GetDim 头 6 字节 */
static const unsigned char kWantGetDim[6] = { 0x83, 0xEC, 0x1C,
                                              0x8B, 0x41, 0x04 };

/* 场景区分只认位置条带, 不认函数。 */

typedef struct {
    int Enable;
    char FontFace[64];
    int FontPx;     /* 全局字号: 0=自动校准; >0 固定像素, 墨水与字宽等比缩放 */
    int CreditPx;   /* 顶栏数字单独字号 (0=跟随全局) */
    int CreditDY;   /* 顶栏数字垂直偏移 (负=上移) */
    int MenuTake;   /* 1=菜单按钮整体替换 */
    int DigitTrack; /* 数字字距微调 (只影响 0-9) */
    int TipBox;     /* 1=电力提示定制盒 */
    int BriefLine;  /* 简报行距百分比 */
    int BriefOwn;   /* 1=简报正文自绘, 0=交回引擎 */
    int BriefTrack; /* 简报字距 */
    int BriefMark;  /* 1=自绘块描边框 */
    int BriefWidth; /* 简报盒宽 (0=沿用引擎 453) */
    int BriefAdv;   /* 格宽基准: 0=字号+字距, 1=字体前进宽+字距 */
    int Bold;
    int LogDetail;  /* 1=逐字日志 */
    int Perf;       /* 1=每 2s 记性能账 */
    int Render;
} VTConfig;

static int g_VtInit = 0;

/* ---------------- 性能账 (Perf=1) ----------------
 * glyph=Blit 字数; miss=字形未命中; gdHit/gdMiss=GetDim 命中/未命中;
 * gdUs=GetDim 自耗时; gpus=每秒字数。 */
static unsigned g_PfGlyph, g_PfMiss, g_PfGdHit, g_PfGdMiss;
static unsigned g_PfGdUs;
static unsigned g_PfLastLog = 0;
static int g_PfOn = 0;

static unsigned PfQpc(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned)c.LowPart;
}

static void PfTick(void)
{
    unsigned now = GetTickCount();
    char b1[16], b2[16], b3[16], b4[16], buf[192];
    unsigned q = 0, ms;
    if (!g_PfOn) return;
    if (!g_PfLastLog) { g_PfLastLog = now; return; }
    if (now - g_PfLastLog < 2000) return;
    ms = now - g_PfLastLog;
    g_PfLastLog = now;
    q = AppendCStr(buf, q, "perf: ms=", sizeof(buf));
    UtoA(ms, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " glyph=", sizeof(buf));
    UtoA(g_PfGlyph, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " miss=", sizeof(buf));
    UtoA(g_PfMiss, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " gdHit=", sizeof(buf));
    UtoA(g_PfGdHit, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " gdMiss=", sizeof(buf));
    UtoA(g_PfGdMiss, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
    q = AppendCStr(buf, q, " gdUs=", sizeof(buf));
    UtoA(g_PfGdUs, b3); q = AppendCStr(buf, q, b3, sizeof(buf));
    q = AppendCStr(buf, q, " gpus=", sizeof(buf));
    UtoA(ms ? g_PfGlyph * 1000u / ms : 0, b4); q = AppendCStr(buf, q, b4, sizeof(buf));
    LogLine(buf);
    g_PfGlyph = g_PfMiss = g_PfGdHit = g_PfGdMiss = g_PfGdUs = 0;
}

static VTConfig g_VT;
static wchar_t g_VTFace[FONT_FACE_MAX];

/* VecRender 失败码, 只给失败日志用。 */
static int g_VecWhy = 0;
/* 自然摆位: 简报自绘时为 1, 进字形缓存键。 */
static int g_VecNat = 0;

/* thiscall (ECX=this): __fastcall 占位 (this, edxUnused) + 栈上 4 参数,
 * 编译器生成 ret 0x10, 与原 Blit 尾完全一致, JMP 钩可直接跳入。 */
typedef int (__fastcall *BlitFn)(void* t, void* edxUnused,
                                 int wch, int x, int y, int color);
static BlitFn g_OrigBlit = NULL;

typedef int (__fastcall *GetDimFn)(void* t, void* edxUnused,
                                   const wchar_t* s, int* w, int* h, int maxW);
static GetDimFn g_OrigGetDim = NULL;   /* 度量原版 (跳板); 引擎算字宽唯一入口,
                                        * 由 VecGetDimEntry 接管改宽 */

/* ---------------- 入口钩 ----------------
 * DrawText/Print 入口只做诊断记录 (vcall) 与简报盒补丁;
 * 字号在 Blit 层定: 全局 FontPx > 自动校准, 顶栏条带数字另吃 CreditPx。
 * 转发必须原样 (参数个数决定 ret 清栈数, 类型无所谓, 全 4 字节):
 * DrawText = thiscall + 10 个栈参 (ret 0x28, 反汇编实证),
 * Print = thiscall + 7 个栈参 (ret 0x1c)。
 * 管线内层调用 (Simple/Fancy/DrawText/Print 互调) 在栈上找真正的
 * 发起者 (须过 call 指令校验), 只为诊断日志 (vcall outer) 服务。 */

typedef int (__fastcall *DrawTextFn)(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5,
    int a6, int a7, int a8, int a9, int a10);
static DrawTextFn g_OrigDrawText = NULL;

typedef int (__fastcall *PrintFn)(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7);
static PrintFn g_OrigPrint = NULL;

/* Simple_Text_Print_Wide (0x4A5EB0): fastcall + 7 栈参。只观察不改。 */
typedef int (__fastcall *SimpleFn)(int ecxIn, int edxIn,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7);
static SimpleFn g_OrigSimple = NULL;

/* 场景区分只认位置条带 (Blit 层 y<32 的数字), 不认函数:
 * 顶栏那几个函数也会画别的文本 (如按 House 遍历连超武列表一起画),
 * 函数粒度分不开, CreditPx 漏过去造成列表大小闪烁。 */

/* 简报 DrawText 判定。 */
static int VecIsBriefCaller(unsigned caller)
{
    if (caller >= 0x00479000u && caller < 0x00479100u) return 1;
    if (caller == 0x00621144u) return 1;
    return 0;
}

/* 坐标归一: 引擎笔位只低 16 位有效, 符号扩展保留负坐标。 */
static int VecNormC(int v)
{
    return (int)(short)(v & 0xFFFF);
}

/* DrawText 嵌套深度 + 正文盒标志 */
static int g_DTDepth = 0;
static int g_DTBody = 0;

/* ---------------- 简报正文自绘 ---------------- */
#define BRIEF_BOTTOM_MAX 1000

/* fnt 格高, 行距下限 */
static int g_BriefOrigH = 0;

/* 行距 = 字号 × BriefLine%, 下限格高。 */
static int VecBriefLineH(int px)
{
    int h = (g_BriefOrigH > 0) ? g_BriefOrigH : px;
    if (g_VT.BriefLine > 0)
    {
        int v = (px * g_VT.BriefLine + 50) / 100;
        if (v > h) h = v;
    }
    if (h > VC_MAXH) h = VC_MAXH;
    if (h < 8) h = 8;
    return h;
}

/* 跨钩状态: DrawText 存, Blit 用。引擎那次照跑 (它负责绑面),
 * Blit 抢首字整块自绘, 之后逐字抑制。
 * state: 0=未定 1=已接管 2=不可用。 */
static wchar_t* g_BriefStr = NULL;
static int g_BriefX = 0, g_BriefY = 0, g_BriefW = 0, g_BriefH = 0;
static int g_BriefState = 0;
static int g_BriefOwnLogged = 0;
static int g_BriefWhyLogged = 0;

static int VecPickPx(void* t);
static int VecMeasure(void* t, wchar_t wch, int* ph);
static void VecBriefProbeW(wchar_t wch, int oa, int cw, int full);
/* 简报单字格宽 = 字体前进宽 + 字距 */
static int VecBriefCellW(void* font, wchar_t ch, int px, int tr);
static int VecGdiSlot(int px);
static int VecRender(void* t, wchar_t wch, int adv, int h, int x, int y,
                     int colorW, int px, int yOff);

/* ---------------- 简报正文自绘 ----------------
 * 引擎排版绑死 fnt 度量, 行高/字宽/换行互相牵制, 故整块自绘:
 * 行高跟 BriefLine, 字距跟 BriefTrack, 换行按墨水宽。
 * 面与颜色与引擎同源 (font+0x0C/+0x10/+0x30/+0x24)。
 * 返回 0=已接管, -1=走引擎。 */
#define BRIEF_MAXCH 4096
#define BRIEF_PAD 20

/* 裁剪底上限取窗口高减一。 */
static int VecBriefMaxY(void)
{
    int h = GtGameH();
    if (h < 64 || h > 8192) return -1;
    return h - 1;
}

/* 文本边界标记: 外粗线 + 内细线, 深灰。 */
#define BRIEF_MARK_RGB 0x40      /* 深灰 */
#define BRIEF_MARK_OUT  2        /* 外线宽 */
#define BRIEF_MARK_GAP  2        /* 内外之间的空隙 */
#define BRIEF_MARK_IN   1        /* 内线宽 */

static void VecBriefRect(unsigned short* buf, int pitchW, const int* LTRB,
                         int x0, int y0, int x1, int y1, int thick)
{
    unsigned short* row;
    int lef = LTRB[0], top = LTRB[1], rig = LTRB[2], bot = LTRB[3];
    int i, t, sx, sy;
    if (x1 < x0) { i = x0; x0 = x1; x1 = i; }
    if (y1 < y0) { i = y0; y0 = y1; y1 = i; }
    if (lef < 0) lef = 0;
    if (top < 0) top = 0;
    if (rig > pitchW - 1) rig = pitchW - 1;
    if (bot > pitchW - 1) bot = pitchW - 1;
    for (i = 0; i < 2; i++)                       /* 上下两条 */
    {
        sy = i ? y1 : y0;
        row = buf + sy * pitchW;
        for (t = 0; t < thick; t++, sy++)
        {
            if (sy < top || sy > bot) continue;
            for (sx = x0; sx <= x1; sx++)
                if (sx >= lef && sx <= rig)
                    row[sx] = Blend16(row[sx], BRIEF_MARK_RGB,
                                       BRIEF_MARK_RGB, BRIEF_MARK_RGB, 255);
        }
    }
    for (i = 0; i < 2; i++)                       /* 左右两条 */
    {
        sx = i ? x1 : x0;
        for (t = 0; t < thick; t++, sx++)
        {
            if (sx < lef || sx > rig) continue;
            for (sy = y0; sy <= y1; sy++)
                if (sy >= top && sy <= bot)
                    buf[sy * pitchW + sx] =
                        Blend16(buf[sy * pitchW + sx], BRIEF_MARK_RGB,
                                BRIEF_MARK_RGB, BRIEF_MARK_RGB, 255);
        }
    }
}

static void VecBriefMark(unsigned short* buf, int pitchW, const int* LTRB,
                         int x0, int y0, int x1, int y1)
{
    int d = BRIEF_MARK_OUT + BRIEF_MARK_GAP;
    VecBriefRect(buf, pitchW, LTRB, x0, y0, x1, y1, BRIEF_MARK_OUT);
    if (x1 - d <= x0 + d || y1 - d <= y0 + d) return;   /* 太小就不画内框 */
    VecBriefRect(buf, pitchW, LTRB, x0 + d, y0 + d, x1 - d, y1 - d,
                 BRIEF_MARK_IN);
}

/* 退回引擎的一次性原因日志 (why 见下), 免得只看到"没变化"没处查。 */
static void VecBriefWhy(int why, int v)
{
    char b1[16], buf[96];
    unsigned q = 0;
    if (g_BriefWhyLogged) return;
    g_BriefWhyLogged = 1;
    q = AppendCStr(buf, q, "brief: own skip why=", sizeof(buf));
    UtoA((unsigned)why, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCh(buf, q, ' ', sizeof(buf));
    UtoA((unsigned)v, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    LogLine(buf);
}

/* 单字格宽 = 字体自己的前进宽 (ABC 全宽) + 字距。
 * 这才是一切正常排版的做法 (GDI DrawText / DirectWrite / 浏览器都是
 * 逐字取 advance 再加 letter-spacing):
 *   汉字/CJK 标点 = 1 em (18px), 拉丁按字形比例, 空格窄。
 * 用统一格宽 (或引擎 fnt 宽度表) 排 CJK 混排, 宽窄不一的字就会
 * 间距忽大忽小 —— fnt 那张表是 16x16 点阵时代的产物, 逐字不同。 */
static int VecBriefCellW(void* font, wchar_t ch, int px, int tr)
{
    int ga;
    if (Icon_Is((unsigned)ch))
    {
        int ia = Icon_Advance((unsigned)ch);
        if (ia > 0) return ia;
    }
    ga = GtCharAdv(ch);
    if (ga <= 0 || ga > 64) ga = px;   /* 字体没这个字: 退回字号 */
    if (ga < 1) ga = 1;
    return ga + tr;
}

static int VecBriefBody(void* font, const wchar_t* s,
                       int x, int y, int w)
{
    int px, lh, tr, adv, color, cx, cy, lines, seg, per, want, oldB, n, oa;
    int pitchW, maxCx, maxY, oldL, oldT, oldR;
    int* LTRB;
    const wchar_t* p;

    if (!font || !IsPlausiblePtr(font)) { VecBriefWhy(1, 0); return -1; }
    if (!s || !IsPlausiblePtr((void*)s)) { VecBriefWhy(2, 0); return -1; }
    if (!*(unsigned short**)((char*)font + 0x0C)) { VecBriefWhy(3, 0); return -1; }
    pitchW = *(int*)((char*)font + 0x10);
    if (pitchW < 1024) { VecBriefWhy(4, pitchW); return -1; }
    if (w < 64) { VecBriefWhy(5, w); return -1; }
    if (g_VT.BriefWidth > 0 && g_VT.BriefWidth < 4096)
        w = g_VT.BriefWidth;
    px = VecPickPx(font);
    if (px < 8 || px > 40) { VecBriefWhy(6, px); return -1; }
    lh = VecBriefLineH(px);
    tr = g_VT.BriefTrack;
    if (tr < 0) tr = 0;
    if (tr > VC_MAXW - px) tr = VC_MAXW - px;
    adv = px + tr;                       /* 全角格宽 = 字号 + 字距 */
    color = (int)(*(unsigned short*)((char*)font + 0x24));
    maxY = VecBriefMaxY();
    if (maxY < 0) { VecBriefWhy(7, 0); return -1; }
    per = w / adv;
    if (per < 1) { VecBriefWhy(8, per); return -1; }
    /* GDI 面先备好, 与字形光栅同槽 */
    if (!GdiEnsure(px, g_VT.Bold, VecGdiSlot(px), g_VTFace)) { VecBriefWhy(9, 0); return -1; }

    /* 行数预估: 与折行同规则跑一遍。 */
    lines = 1;
    cx = x;
    for (p = s; *p && lines < BRIEF_MAXCH; p++)
    {
        if (*p == 0x0A) { cx = x; lines++; continue; }
        seg = VecBriefCellW(font, *p, px, tr);
        if (cx + seg > x + w) { cx = x; lines++; }
        cx += seg;
    }

    /* 裁剪四边临时放开, 画完还原。 */
    LTRB = (int*)((char*)font + 0x30);
    oldL = LTRB[0]; oldT = LTRB[1]; oldR = LTRB[2]; oldB = LTRB[3];
    /* 边框在文字外, 裁剪须再外放才画得全。 */
    if (LTRB[0] > x - BRIEF_PAD - 4)
        LTRB[0] = (x - BRIEF_PAD - 4 > 0) ? x - BRIEF_PAD - 4 : 0;
    if (LTRB[1] > y - BRIEF_PAD - 4)
        LTRB[1] = (y - BRIEF_PAD - 4 > 0) ? y - BRIEF_PAD - 4 : 0;
    if (LTRB[2] < x + w + BRIEF_PAD + 4)
        LTRB[2] = (x + w + BRIEF_PAD + 4 < pitchW)
                  ? x + w + BRIEF_PAD + 4 : pitchW - 1;
    want = y + lines * lh + BRIEF_PAD + 4;
    if (want > BRIEF_BOTTOM_MAX) want = BRIEF_BOTTOM_MAX;
    if (want > maxY) want = maxY;
    if (LTRB[3] < want) LTRB[3] = want;

    maxCx = x;
    g_VecNat = g_VT.BriefOwn ? 1 : 0;
    for (cx = x, cy = y, p = s; *p; p++)
    {
        int cw = adv;
        if (*p == 0x0A) { cx = x; cy += lh; continue; }
        if (Icon_Is((unsigned)*p))
        {
            int ia = Icon_Advance((unsigned)*p);
            if (ia <= 0) continue;
            Icon_Blit(font, (unsigned)*p, cx, cy, color, lh);
            cx += ia;
            if (cx > maxCx) maxCx = cx;
            continue;
        }
        /* BriefAdv=1: 字体前进宽 + 字距; 0: 统一字号 + 字距。 */
        oa = GtCharAdv(*p);
        if (g_VT.BriefAdv && oa > 0)
            cw = VecBriefCellW(font, *p, px, tr);
        else
            cw = adv;
        if (cw > VC_MAXW) cw = VC_MAXW;
        VecBriefProbeW(*p, oa, cw, adv);
        if (cx + cw > x + w) { cx = x; cy += lh; }
        if (cy + lh > y + lines * lh + lh) break;
        /* 摆位格去掉字距, 字距由笔位加。 */
        n = VecRender(font, *p, cw - tr, lh, cx, cy, color, px, 0);
        if (n < 0) { cx += cw; continue; }
        cx = n + tr;
        if (cx > maxCx) maxCx = cx;
    }
    g_VecNat = 0;
    if (g_VT.BriefMark)
        VecBriefMark(*(unsigned short**)((char*)font + 0x0C), pitchW, LTRB,
                     x - BRIEF_PAD, y - BRIEF_PAD,
                     maxCx + BRIEF_PAD, cy + lh + BRIEF_PAD);
    LTRB[0] = oldL; LTRB[1] = oldT; LTRB[2] = oldR; LTRB[3] = oldB;
    if (!g_BriefOwnLogged)
    {
        char b1[16], b2[16], buf[128];
        unsigned q = 0;
        g_BriefOwnLogged = 1;
        q = AppendCStr(buf, q, "brief: own draw box=", sizeof(buf));
        UtoA((unsigned)x, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCh(buf, q, ',', sizeof(buf));
        UtoA((unsigned)y, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCh(buf, q, ' ', sizeof(buf));
        UtoA((unsigned)w, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCh(buf, q, ' ', sizeof(buf));
        UtoA((unsigned)lh, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, "px/char ", sizeof(buf));
        UtoA((unsigned)adv, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " lines=", sizeof(buf));
        UtoA((unsigned)lines, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
        q = AppendCStr(buf, q, " pitch=", sizeof(buf));
        UtoA((unsigned)pitchW, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " maxY=", sizeof(buf));
        UtoA((unsigned)maxY, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        LogLine(buf);
    }
    return 0;
}

/* 字宽抽查: 前 8 个不同字各记一条。 */
static void VecBriefProbeW(wchar_t wch, int oa, int cw, int full)
{
    static unsigned seen[8];
    static int seenN = 0;
    static const char* hexd = "0123456789ABCDEF";
    char b1[16], buf[96];
    unsigned q;
    int i, k;
    unsigned wc = (unsigned)wch & 0xFFFFu;
    for (i = 0; i < seenN; i++) if (seen[i] == wc) return;
    if (seenN >= 8) return;
    seen[seenN++] = wc;
    q = AppendCStr(buf, q = 0, "brief: w U+", sizeof(buf));
    for (k = 0; k < 4; k++)
        q = AppendCh(buf, q, hexd[(wc >> (12 - 4 * k)) & 15u], sizeof(buf));
    q = AppendCStr(buf, q, " gdi=", sizeof(buf));
    UtoA((unsigned)(oa > 0 ? oa : 0), b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " cell=", sizeof(buf));
    UtoA((unsigned)cw, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " full=", sizeof(buf));
    UtoA((unsigned)full, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    LogLine(buf);
}

/* 简报 DrawText 普查: 每个 (调用者,x,y,w,h) 只记一条。
 * 纯诊断 (LogDetail=1 才记, 默认不记): 普查定案后即噪音, 16 槽一满未命中全漏记。 */
static void VecBriefProbe(unsigned caller, int x, int y, int w, int h,
                             int txt, const char* tag)
{
    static unsigned seen[16][5];
    static int seenN = 0;
    char b1[16], buf[160];
    unsigned q;
    int i, len = 0;
    const wchar_t* p = 0;
    if (!g_LogDetail) return;
    for (i = 0; i < seenN; i++)
        if (seen[i][0] == caller && seen[i][1] == (unsigned)x &&
            seen[i][2] == (unsigned)y && seen[i][3] == (unsigned)w &&
            seen[i][4] == (unsigned)h)
            return;
    if (seenN < 16)
    {
        seen[seenN][0] = caller; seen[seenN][1] = (unsigned)x;
        seen[seenN][2] = (unsigned)y; seen[seenN][3] = (unsigned)w;
        seen[seenN][4] = (unsigned)h;
        seenN++;
    }
    p = (const wchar_t*)(size_t)txt;
    if (p && IsPlausiblePtr((void*)p))
        while (len < 9999 && p[len]) len++;
    else
        p = 0;
    q = 0;
    q = AppendCStr(buf, q, "brief: dt ", sizeof(buf));
    UtoA(caller, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCh(buf, q, ' ', sizeof(buf));
    UtoA((unsigned)x, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCh(buf, q, ',', sizeof(buf));
    UtoA((unsigned)y, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCh(buf, q, ' ', sizeof(buf));
    UtoA((unsigned)w, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCh(buf, q, 'x', sizeof(buf));
    UtoA((unsigned)h, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCh(buf, q, ' ', sizeof(buf));
    UtoA((unsigned)len, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCh(buf, q, ' ', sizeof(buf));
    q = AppendCStr(buf, q, tag, sizeof(buf));
    LogLine(buf);
}

/* 原版格高 */
static int g_FontOrigH = 19;

/* ---------------- 超武列表整体接管 ----------------
 * Simple_ 入口收表并跳过 (底+字同死); 空串只收表。Print 入口只收冒号计时器后备。 */

/* 前 16 字含 '=' 即非列表串。 */
static int VecSWHasEq(const wchar_t* s)
{
    int k;
    if (!s || !IsPlausiblePtr((void*)s)) return 0;
    for (k = 0; k < 16; k++)
    {
        unsigned c = (unsigned)s[k] & 0xFFFFu;
        if (!c) break;
        if (c == 0x3Du) return 1;
    }
    return 0;
}
/* Ares FPS 计数器前缀: 钉住收串与消盒共用判据 (超武计时器永不用此前缀)。 */
static int VecSWIsFps(const wchar_t* s)
{
    if (!s || !IsPlausiblePtr((void*)s)) return 0;
    return (s[0] == L'F' && s[1] == L'P' && s[2] == L'S' && s[3] == L':') ? 1 : 0;
}

/* 前 16 字含冒号即计时器。 */
static int VecSWColon(const wchar_t* s)
{
    int k;
    if (!s || !IsPlausiblePtr(s)) return 0;
    for (k = 0; k < 16; k++)
    {
        unsigned c = (unsigned)s[k] & 0xFFFFu;
        if (!c) break;
        if (c == 0x3Au) return 1;
    }
    return 0;
}

/* 超武列表横向带, 跟 ViewBounds 走。 */
#define SW_BAND_BACK 400
#define SW_BAND_FWD  8

static void VecSWBand(int* lo, int* hi)
{
    RectangleStruct vb;
    int right;
    /* 同 right 直接复用 (lo/hi 是 right 的纯函数, ViewBounds 帧内不变)。 */
    static int s_BandRight = -1, s_BandLo = 0, s_BandHi = 0;
    vb = *(const RectangleStruct*)(size_t)ADDR_SURF_VIEWBOUNDS;
    right = vb.X + vb.Width;
    if (right == s_BandRight && right >= 128 && right <= 0x10000)
    {
        *lo = s_BandLo; *hi = s_BandHi;
        return;
    }
    if (right < 128 || right > 0x10000)
    {
        /* ViewBounds 没写好时退回老口径 (只宽不窄, 不漏拦截)。 */
        *lo = 1300;
        *hi = (g_SWCompW > 1300) ? g_SWCompW : 1300;
        return;
    }
    *lo = right - SW_BAND_BACK;
    *hi = right + SW_BAND_FWD;
    s_BandRight = right; s_BandLo = *lo; s_BandHi = *hi;
}

SWCapRow g_SWCap[SWCAP_MAX];
int g_SWCapN = 0;
int g_SWCompW = 0, g_SWCompH = 0;
int g_MapRight = 0;
int g_SWPanelOn = 0;

/* 收表 (同 y/kind 归并, 空串也收)。后备不覆盖新鲜正文。 */
static void VecSWStore(int kind, const wchar_t* s, int n, int fore, int x,
                       int y, int src)
{
    int i;
    int idx = -1;
    SWCapRow* r;
    for (i = 0; i < g_SWCapN; i++)
        if (g_SWCap[i].y == y && g_SWCap[i].kind == kind) { idx = i; break; }
    if (idx < 0)
    {
        if (g_SWCapN < SWCAP_MAX)
        {
            idx = g_SWCapN++;
            r = &g_SWCap[idx];
            r->x = x; r->y = y;
            r->fore = -1;
        }
        else
        {
            /* 满了踢最旧一行。 */
            int old = 0;
            for (i = 1; i < g_SWCapN; i++)
                if (g_SWCap[i].tick < g_SWCap[old].tick) old = i;
            idx = old;
            r = &g_SWCap[idx];
            r->x = x; r->y = y;
            r->fore = -1;
        }
    }
    else
    {
        r = &g_SWCap[idx];
        r->x = x;
        if (src == 1 && fore < 0 && r->fore >= 0 &&
            GetTickCount() - r->tick <= SW_FRESH_MS) return;
    }
    if (n > 63) n = 63;
    for (i = 0; i <= n; i++) r->s[i] = s[i];
    r->s[63] = 0;
    r->kind = kind;
    if (fore >= 0) r->fore = fore;
    else if (r->fore < 0) r->fore = (kind == 0) ? 0x07FF : 0xFFFF;
    r->tick = GetTickCount();
}

/* 主画布判据 (窗口面或缩小档大面)。 */
static int VecIsCanvas(void* surf, int w, int h)
{
    if (!surf || w <= 0 || h <= 0) return 0;
    if (g_SWCompW > 0 && w == g_SWCompW && h == g_SWCompH) return 1;
    if (SB_IsBigSurface(surf)) return 1;
    if (h == SB_CanvasH(w)) return 1;
    return 0;
}

/* Print 层后备收集。 */
static int VecSWCapture(void* surf, int kind, const wchar_t* s, int x, int y)
{
    int sw = 0, sh = 0, n = 0;
    if (!g_SWPanelOn) return 0;
    if (!s || !IsPlausiblePtr(s)) return 0;
    if (surf && IsPlausiblePtr(surf))
    {
        int* b = (int*)surf;
        sw = b[1]; sh = b[2];
    }
    if (!VecIsCanvas(surf, sw, sh)) return 0;
    {
        int lo, hi;
        VecSWBand(&lo, &hi);
        if (x < lo || x > hi || y < 32 || y > sh) return 0;
    }
    while (s[n] && n < 63) n++;
    if (g_SWCapN >= SWCAP_MAX && n > 0)
    {
        int i, found = 0;
        for (i = 0; i < g_SWCapN; i++)
            if (g_SWCap[i].y == y && g_SWCap[i].kind == kind) { found = 1; break; }
        if (!found) return 1;
    }
    VecSWStore(kind, s, n, -1, x, y, 1);
    return n > 0 ? 1 : 0;
}


/* ---------------- 菜单按钮整体替换 ----------------
 * DrawText caller=0x621144, flags=5, 123x24/121x20。入口只收串,
 * 首个 Blit 建字形居中起笔。 */
static int g_MenuDepth = 0;
#define MENU_MAXCH 64
static wchar_t g_MenuStr[MENU_MAXCH + 1];
static int g_MenuAdv[MENU_MAXCH];
static int g_MenuN = 0;
static int g_MenuRX = 0, g_MenuRW = 0;
static int g_MenuIdx = 0;      /* 已画字数 */
static int g_MenuPenX = 0;     /* 自有笔位 */
static int g_MenuBuilt = 0;

static int VecMenuMatch(unsigned caller, int w, int h, int flags)
{
    if (!g_VT.MenuTake) return 0;
    if (caller != 0x00621144u) return 0;
    if (flags != 5) return 0;
    if ((w == 123 && h == 24) || (w == 121 && h == 20)) return 1;
    return 0;
}


/* PUA 串行步进: max(原步进, 字号, 图标高)。 */
static int VecTipStep(void* t, const wchar_t* s)
{
    int s0, px, mh = 0, q;
    if (!t || !IsPlausiblePtr(t) || !s) return -1;
    s0 = *(int*)((char*)t + 0x1C);
    if (s0 < 8 || s0 > 64) return -1;
    px = VecPickPx(t);
    if (px < 8 || px > 40) px = s0;
    for (q = 0; q < 256; q++)
    {
        wchar_t c = s[q];
        int hh;
        if (!c) break;
        if (!Icon_Is((unsigned)c & 0xFFFFu)) continue;
        hh = Icon_Height((unsigned)c & 0xFFFFu);
        if (hh > mh) mh = hh;
    }
    {
        int st = s0;
        if (px > st) st = px;
        if (mh > st) st = mh;
        if (st > 64) st = 64;
        return st;
    }
}

/* GDI 槽按字号分: 主字号用 2, 顶栏字号用 8 (混用会互拆重建)。 */
static int VecGdiSlot(int px)
{
    return (g_VT.CreditPx > 0 && px == g_VT.CreditPx) ? 8 : 2;
}

int __fastcall VecDrawTextEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5,
    int a6, int a7, int a8, int a9, int a10)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int brief, r, menuTake = 0;
    brief = VecIsBriefCaller(caller);
    menuTake = VecMenuMatch(caller, a6, a7, a8);
    if (menuTake)
    {
        if (g_MenuDepth == 0)
        {
            const wchar_t* ms = (const wchar_t*)a3;
            int mn = 0;
            if (ms && IsPlausiblePtr((void*)ms))
            {
                while (mn < MENU_MAXCH && ms[mn] && ms[mn] != 0x0Au)
                { g_MenuStr[mn] = ms[mn]; mn++; }
            }
            g_MenuStr[mn] = 0;
            g_MenuN = mn;
            g_MenuRX = a4; g_MenuRW = a6;
            g_MenuIdx = 0; g_MenuBuilt = 0;
        }
        g_MenuDepth++;
    }
    /* DrawText 只记上下文, 绘制走原版。 */
    if (!g_OrigDrawText) { if (menuTake) g_MenuDepth--; return 0; }
    /* 简报正文: 引擎照跑, Blit 抢首字整块自绘。 */
    if (brief && caller == 0x621144u && a6 >= 400 && a7 >= 100)
    {
        g_BriefStr = (wchar_t*)(size_t)a3;
        g_BriefX = a4; g_BriefY = a5; g_BriefW = a6; g_BriefH = a7;
        g_BriefState = 0;
        VecBriefProbe(caller, a4, a5, a6, a7, a3,
                      g_VT.BriefOwn ? "own" : "eng");
    }
    else if (brief)
    {
        VecBriefProbe(caller, a4, a5, a6, a7, a3, "eng");
    }
    if (brief)
    {
        g_DTDepth++;
        if (g_DTDepth == 1)
        {
            g_DTBody = (caller == 0x00621144u && a6 >= 400 && a7 >= 100);
        }
    }
    r = g_OrigDrawText(t, NULL, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    if (menuTake) g_MenuDepth--;
    if (brief)
    {
        g_DTDepth--;
        if (g_DTDepth == 0)
        {
            g_DTBody = 0;
        }
    }
    return r;
}

/* 上一帧电力提示 Print 落笔 x (Simple 入口的 x 自校验用) */
static int g_LastTipX = -1;
/* 当帧提示串宽 (Blit 首字画盒用, 0=跳过) */
static int g_TipTot = 0;
static int g_TipOw = 0;
/* 当帧提示落笔 */
static int g_TipY = -9999;
/* 当帧提示首字是否已扫描 */
static int g_BlitTipSeen = 0;
/* 提示边框色粘存 (采不到时沿用, 不回落)。 */
static unsigned g_TipBcCache = 0;
static int g_TipBcOK = 0;
/* 宽度表: ring + 常驻 sticky, 全串比对。键含字体/maxW。 */
#define DIMCACHE_N 64
typedef struct {
    const wchar_t* ptr;
    unsigned w0;
    int n, ow, nw;
    void* t;            /* BitFont: 换字体对象即失效 */
    void* idata;
    int maxW;           /* 换折行上限即失效 (ow/nw 都跟它有关) */
    int oh;             /* 原版总高 */
    int ok;             /* r (原版返回值) */
} DimCE;
static DimCE g_DimC[DIMCACHE_N];
static int g_DimCPos = 0;
/* 防冲 sticky: 主表被冲后仍能命中。 */
#define STICKY_N 16
typedef struct {
    int used;
    unsigned w0;
    int n, ow, nw;
    wchar_t tx[64];
} StkCE;
static StkCE g_StkC[STICKY_N];
static int g_StkRR = 0;

/* FPS 钉住: Simple_ 收到的计数器串, Render 尾固定位 1x 重画 (见 VecFpsPinEntry)。 */
#define FPS_PX 16
#define FPS_SLOT 9
#define FPS_MAXCH 63
static wchar_t s_FpsBuf[FPS_MAXCH + 1];
static int s_FpsN = 0;
static int s_FpsHave = 0;
void* g_FpsPinTramp = NULL;

/* 全串宽度查表, 找不到返回 0。 */
static int VecDimLookup(const wchar_t* s, int len)
{
    int ci, si, k;
    if (!s || len <= 0) return 0;
    for (ci = 0; ci < DIMCACHE_N; ci++)
    {
        DimCE* e = &g_DimC[ci];
        if (!e->ptr || e->w0 != ((unsigned)s[0] & 0xFFFFu) || e->n != len)
            continue;
        if (!IsPlausiblePtr((void*)e->ptr)) continue;
        for (k = 0; k < len; k++)
            if (e->ptr[k] != s[k]) break;
        if (k == len) return e->nw;
    }
    for (si = 0; si < STICKY_N; si++)
    {
        StkCE* st = &g_StkC[si];
        if (!st->used || st->w0 != ((unsigned)s[0] & 0xFFFFu) || st->n != len)
            continue;
        for (k = 0; k < len; k++)
            if (st->tx[k] != s[k]) break;
        if (k == len) return st->nw;
    }
    return 0;
}

int __fastcall VecPrintEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int r;
    /* 只认世界悬浮 Print, 排除菜单/列表同字串。 */
    {
        const wchar_t* ps = (const wchar_t*)a3;
        if (caller == 0x004A5FF2u && ps && IsPlausiblePtr((void*)ps) &&
            (((unsigned)ps[0] & 0xFFFFu) == 0x7535u))
        {
            int tl = 0, ci;
            g_LastTipX = a4;
            g_TipY = a5;
            g_BlitTipSeen = 0;
            /* 取本串宽度, 查不到为 0。 */
            g_TipTot = 0;
            g_TipOw = 0;
            while (tl < 64 && ps[tl]) tl++;
            /* 全串比对。 */
            for (ci = 0; ci < DIMCACHE_N; ci++)
            {
                DimCE* e = &g_DimC[ci];
                int k, same;
                if (!e->ptr ||
                    e->w0 != ((unsigned)ps[0] & 0xFFFFu) ||
                    e->n != tl)
                    continue;
                if (!IsPlausiblePtr((void*)e->ptr)) continue;
                same = 1;
                for (k = 0; k < tl; k++)
                {
                    if (e->ptr[k] != ps[k]) { same = 0; break; }
                }
                if (same)
                {
                    int si, done = 0;
                    g_TipTot = e->nw; g_TipOw = e->ow;
                    /* 写透 sticky。 */
                    for (si = 0; si < STICKY_N; si++)
                    {
                        StkCE* s = &g_StkC[si];
                        if (s->used && s->w0 == e->w0 && s->n == tl)
                        {
                            s->nw = e->nw; s->ow = e->ow;
                            for (k = 0; k < tl; k++) s->tx[k] = ps[k];
                            s->tx[tl] = 0;
                            done = 1;
                            break;
                        }
                    }
                    if (!done)
                    {
                        StkCE* s = &g_StkC[g_StkRR % STICKY_N];
                        g_StkRR++;
                        s->used = 1; s->w0 = e->w0; s->n = tl;
                        s->nw = e->nw; s->ow = e->ow;
                        for (k = 0; k < tl; k++) s->tx[k] = ps[k];
                        s->tx[tl] = 0;
                    }
                    break;
                }
            }
            /* 主表 miss 回 sticky。 */
            if (g_TipTot == 0 && tl >= 10 && tl < 64)
            {
                int si;
                for (si = 0; si < STICKY_N; si++)
                {
                    StkCE* s = &g_StkC[si];
                    int k2, same2;
                    if (!s->used ||
                        s->w0 != ((unsigned)ps[0] & 0xFFFFu) ||
                        s->n != tl)
                        continue;
                    same2 = 1;
                    for (k2 = 0; k2 < tl; k2++)
                    {
                        if (s->tx[k2] != ps[k2]) { same2 = 0; break; }
                    }
                    if (same2) { g_TipTot = s->nw; g_TipOw = s->ow; break; }
                }
            }
        }
    }
    /* 子串回退: 引擎只传可见子串时按 sticky 全文匹配, 估盒宽。 */
    {
        const wchar_t* qs = (const wchar_t*)a3;
        int ql = 0;
        if (qs && IsPlausiblePtr((void*)qs) &&
            caller == 0x004A5FF2u &&
            (g_TipTot == 0 || ((unsigned)qs[0] & 0xFFFFu) != 0x7535u) &&
            g_TipY > -9000 && a5 >= g_TipY - 24 && a5 <= g_TipY + 24 &&
            a4 >= g_LastTipX - 64 && a4 <= g_LastTipX + 400)
        {
            int si;
            while (ql < 64 && qs[ql]) ql++;
            if (ql >= 4 && ql < 64)
            {
                for (si = 0; si < STICKY_N; si++)
                {
                    StkCE* s = &g_StkC[si];
                    int kk;
                    if (!s->used || s->n < ql) continue;
                    for (kk = 0; kk + ql <= s->n; kk++)
                    {
                        int m2, ok = 1;
                        for (m2 = 0; m2 < ql; m2++)
                        {
                            if (s->tx[kk + m2] != qs[m2])
                            {
                                ok = 0;
                                break;
                            }
                        }
                        if (!ok) continue;
                        {
                            /* 子串宽优先用表, 没有按平均估。 */
                            int ew = VecDimLookup(qs, ql);
                            int t2 = (ew > 0) ? ew : (s->nw - kk * s->nw / s->n);
                            int o2 = s->ow - kk * s->ow / s->n;
                            if (o2 < 40) o2 = t2;
                            if (t2 >= 40)
                            {
                                g_TipTot = t2; g_TipOw = o2;
                                g_LastTipX = a4; g_TipY = a5;
                                g_BlitTipSeen = 0;
                            }
                        }
                        break;
                    }
                    if (g_TipTot != 0) break;
                }
            }
        }
    }
    /* 超武列表: 只收冒号计时器。CJK 名串不在此收 (与提示不可区分)。 */
    if (a4 >= 1300 && a5 >= 32)
    {
        const wchar_t* s = (const wchar_t*)a3;
        if (VecSWColon(s) && !VecSWHasEq(s) &&
            VecSWCapture((void*)a2, 1, s, a4, a5))
            return 1;
    }
    if (!g_OrigPrint) return 0;
    r = g_OrigPrint(t, NULL, a1, a2, a3, a4, a5, a6, a7);
    return r;
}

/* 文本槽判定: CJK 首字=0, 前 8 字含冒号=1, 否则 -1。只认 EDX。 */
static int VecSWTextKind(int slot, const wchar_t** out)
{
    const wchar_t* s;
    unsigned c;
    int k;
    if (!IsPlausiblePtr((void*)slot)) return -1;
    s = (const wchar_t*)slot;
    c = (unsigned)s[0] & 0xFFFFu;
    if ((c >= 0x2E80u && c < 0xA000u) ||
        (c >= 0xFF00u && c < 0xFFF0u)) { *out = s; return 0; }
    for (k = 0; k < 8; k++)
    {
        c = (unsigned)s[k] & 0xFFFFu;
        if (!c) break;
        if (c == 0x3Au) { *out = s; return 1; }
    }
    return -1;
}

/* Simple_ 入口: 命中即收表并跳过, 空串只收表。 */
int __fastcall VecSimpleEntry(int ecxIn, int edxIn,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    if (!g_OrigSimple) return 0;
    /* Ares FPS 计数器钉住: 收串并跳过原绘制, Render 尾固定位 1x 重画。
     * 只按内容认 ("FPS:" 前缀超武计时器永不用), 不认调用者:
     * Ares 经 Fancy 走 Simple_ (返回点 0x4A6345) 或直调 Simple_
     * (新版 YRpp static DrawText → Simple_, 返回点在 Ares.dll 内), 两种都收。
     * 与超武面板开关无关; Render 关或钉住钩没装上则不收 (看原版)。 */
    if (g_VT.Render && g_FpsPinTramp)
    {
        const wchar_t* fs = (const wchar_t*)(size_t)edxIn;
        if (VecSWIsFps(fs) && IsPlausiblePtr((void*)a1))
        {
            int* b = (int*)a1;
            if (VecIsCanvas((void*)a1, b[1], b[2]) &&
                IsPlausiblePtr((void*)a2) && IsPlausiblePtr((void*)a3))
            {
                int* p2 = (int*)a2;
                int* p3 = (int*)a3;
                int i, n = 0;
                while (n < FPS_MAXCH && fs[n]) n++;
                for (i = 0; i < n; i++) s_FpsBuf[i] = fs[i];
                s_FpsBuf[n] = 0;
                s_FpsN = n;
                s_FpsHave = (n > 0) ? 1 : 0;
                if (IsPlausiblePtr((void*)ecxIn))
                {
                    ((int*)ecxIn)[0] = p2[0] + p3[0];
                    ((int*)ecxIn)[1] = p2[1] + p3[1];
                }
                return ecxIn;
            }
        }
    }
    /* 0x4A6345 是 Fancy 内部调 Simple_ 的返回点 (反汇编实证),
     * 所有 Fancy 文本都经此, 不只侧栏名单; 名单另靠横向带+形状过滤。 */
    if (g_SWPanelOn && g_SWCompW &&
        (unsigned)__builtin_return_address(0) == 0x004A6345u &&
        IsPlausiblePtr((void*)a1))
    {
        int* b = (int*)a1;
        if (VecIsCanvas((void*)a1, b[1], b[2]) &&
            IsPlausiblePtr((void*)a2) && IsPlausiblePtr((void*)a3))
        {
            int* p2 = (int*)a2;
            int* p3 = (int*)a3;
            const wchar_t* text = NULL;
            const wchar_t* raw = (const wchar_t*)(size_t)edxIn;
            int kind = VecSWTextKind(edxIn, &text);
            int n = 0, sx, sy;
            int inband, isEmpty;
            sx = p2[0] + p3[0]; sy = p2[1] + p3[1];
            {
                int lo, hi;
                VecSWBand(&lo, &hi);
                inband = (sx >= lo && sx <= hi && sy >= 32 && sy <= b[2]);
            }
            if (inband)
            {
                /* 空串 = 该行底色条: 只拦不收 (收了多空行), 一起藏掉。 */
                isEmpty = (!IsPlausiblePtr((void*)raw) || raw[0] == 0);
                if (isEmpty)
                {
                    if (IsPlausiblePtr((void*)ecxIn))
                    {
                        ((int*)ecxIn)[0] = sx;
                        ((int*)ecxIn)[1] = sy;
                    }
                    return ecxIn;
                }
                /* 长度只在命中后数 (大多数普通文本走不到这里)。 */
                if (text) { while (text[n] && n < 64) n++; }
                if (text && !VecSWHasEq(text) && n > 0)
                {
                    VecSWStore(kind, text, n, a4, sx, sy, 0);
                    if (IsPlausiblePtr((void*)ecxIn))
                    {
                        ((int*)ecxIn)[0] = sx;
                        ((int*)ecxIn)[1] = sy;
                    }
                    return ecxIn;   /* 与原版同口径 (pRetVal 非空) */
                }
            }
        }
    }
    return g_OrigSimple(ecxIn, edxIn, a1, a2, a3, a4, a5, a6, a7);
}

/* GetDim 宽度接管: 先走原版取高, 宽用 GDI 步进和。控制串/超长/触顶走原版。 */
static int g_GDDepth = 0;

/* 整串记忆: 键=(字体,idata,maxW,串)。只存接管成功的。 */
static DimCE* DimFind(const void* t, const void* idata, const wchar_t* s,
                      int maxW, int n)
{
    int ci, k;
    if (n <= 0) return 0;
    for (ci = 0; ci < DIMCACHE_N; ci++)
    {
        DimCE* d = &g_DimC[ci];
        if (!d->ptr || d->n != n || d->t != t || d->idata != idata ||
            d->maxW != maxW) continue;
        if (d->w0 != ((unsigned)s[0] & 0xFFFFu)) continue;
        if (!IsPlausiblePtr((void*)d->ptr)) continue;
        for (k = 0; k < n; k++)
            if (d->ptr[k] != s[k]) break;
        if (k == n) return d;
    }
    return 0;
}

/* 存一条。 */
static void DimStore(const void* t, const void* idata, const wchar_t* s,
                     int n, int maxW, int ow, int oh, int nw, int ok)
{
    DimCE* d;
    if (n <= 0) return;
    d = &g_DimC[g_DimCPos % DIMCACHE_N];
    g_DimCPos++;
    d->ptr = s; d->w0 = (unsigned)s[0] & 0xFFFFu;
    d->n = n; d->ow = ow; d->nw = nw;
    d->t = (void*)t; d->idata = (void*)idata;
    d->maxW = maxW; d->oh = oh; d->ok = ok;
}

int __fastcall VecGetDimEntry(void* t, void* e, const wchar_t* s,
                              int* w, int* h, int maxW)
{
    int ow = 0, oh = 0, r;
    int px, i, tot, n = 0;
    void* idata;
    (void)e;
    if (!g_OrigGetDim) return 0;
    if (g_GDDepth > 0 || !g_VT.Render)
        return g_OrigGetDim(t, e, s, w, h, maxW);
    if (!t || !IsPlausiblePtr(t) || !s || !IsPlausiblePtr((void*)s) ||
        !w)
        return g_OrigGetDim(t, e, s, w, h, maxW);
    idata = *(void**)((char*)t + 4);
    for (i = 0; i < 256; i++) { if (!s[i]) break; n++; }
    if (i >= 256) return g_OrigGetDim(t, e, s, w, h, maxW);
    /* 记忆命中。 */
    {
        DimCE* d = DimFind(t, idata, s, maxW, n);
        if (d)
        {
            if (g_PfOn) g_PfGdHit++;
            *w = d->nw; if (h) *h = d->oh;
            return d->ok;
        }
    }
    if (g_PfOn) g_PfGdMiss++;
    g_GDDepth++;
    if (g_PfOn)
    {
        unsigned q0 = PfQpc();
        r = g_OrigGetDim(t, e, s, &ow, &oh, maxW);
        g_PfGdUs += PfQpc() - q0;
    }
    else
    {
        r = g_OrigGetDim(t, e, s, &ow, &oh, maxW);
    }
    /* Ares FPS 计数器: 宽度报 0 → 黑底盒消失 (高照原版; 字由 Simple_ 拦下钉住重画)。 */
    if (VecSWIsFps(s)) { if (r) { *w = 0; if (h) *h = oh; } g_GDDepth--; return r; }
    if (!r || (maxW > 0 && ow >= maxW)) goto fwd;
    px = VecPickPx(t);
    if (px < 8 || px > 40 || !GdiEnsure(px, g_VT.Bold, VecGdiSlot(px), g_VTFace))
        goto fwd;
    tot = 0;
    /* 含图标串自己按行度量, 无图标走老路。 */
    {
        int hasIcon = 0, hasNL = 0, hasCtl = 0, j;
        for (j = 0; j < 256; j++)
        {
            wchar_t c = s[j];
            if (!c) break;
            if (Icon_Is((unsigned)c & 0xFFFFu)) { hasIcon = 1; break; }
            if (c == 0x0A || c == 0x0D) hasNL = 1;
            else if (c < 0x20) hasCtl = 1;
        }
        if (hasIcon)
        {
            int lineW = 0;
            tot = 0;
            for (i = 0; i < 256; i++)
            {
                wchar_t wc = s[i];
                int ga;
                if (!wc) break;
                if (wc == 0x0A)   /* 行尾: 取最大行宽 */
                {
                    if (lineW > tot) tot = lineW;
                    lineW = 0;
                    continue;
                }
                if (wc < 0x20) goto fwd;
                if (Icon_Is((unsigned)wc & 0xFFFFu))
                {
                    ga = Icon_Advance((unsigned)wc & 0xFFFFu);
                    if (ga <= 0) goto fwd;
                }
                else
                {
                    ga = GtCharAdv(wc);
                    if (ga < 0) goto fwd;
                    if (wc >= 0x30 && wc <= 0x39) ga += g_VT.DigitTrack;
                }
                if (ga < 0) ga = 0;
                lineW += ga;
                if (maxW > 0 && lineW >= maxW) goto fwd;
            }
            if (i >= 256) goto fwd;
            if (lineW > tot) tot = lineW;
            /* 高度跟步进走 (只增不减, 与 DrawText 钩同公式)。 */
            {
                int nlines = 1, k, v0 = -1, v1;
                for (k = 0; k < i; k++) if (s[k] == 0x0A) nlines++;
                if (t && IsPlausiblePtr(t))
                {
                    int v = *(int*)((char*)t + 0x1C);
                    if (v >= 8 && v <= 64) v0 = v;
                }
                v1 = VecTipStep(t, s);
                if (v0 > 0 && v1 > v0 && oh > 0 && oh < 4096)
                {
                    int nh = oh - nlines * v0 + nlines * v1;
                    if (nh > oh && nh < 4096) oh = nh;
                }
            }
            *w = tot; if (h) *h = oh;
            DimStore(t, idata, s, i, maxW, ow, oh, tot, r);
            g_GDDepth--;
            return r;
        }
        /* 非 PUA 多行串 grow-only 修正 (超武提示窄盒溢出修复):
         * 原版度量 (0x433CF0, 反汇编实证) 换行分支直接清零行宽不存最大,
         * maxW>0 (提示布局传视图宽, 0x478C36/0x478C83) 时盒宽常只按某行算
         * ("打击的击"/"回溯的800"溢出即此因, 与 PUA 分支是两套路径)。
         * 这里按行取 GDI 真最大, 只增不减: 真>原+6 且不超 maxW 才接管
         * (单行/他种控制符/需换行一律原样, 零回归); 高度不动 (行数不变)。 */
        else if (hasNL && !hasCtl)
        {
            int mx = 0, lw = 0, m, gdBad = 0;
            for (m = 0; m < 256; m++)
            {
                wchar_t wc = s[m];
                int ga;
                if (!wc) break;
                if (wc == 0x0A || wc == 0x0D)
                {
                    if (lw > mx) mx = lw;
                    lw = 0;
                    continue;
                }
                ga = GtCharAdv(wc);
                if (ga < 0) { gdBad = 1; break; }
                if (wc >= 0x30 && wc <= 0x39) ga += g_VT.DigitTrack;
                if (ga < 0) ga = 0;
                lw += ga;
                if (lw >= maxW) { gdBad = 1; break; }
            }
            if (!gdBad && m < 256)
            {
                if (lw > mx) mx = lw;
                if (mx > ow + 6 && mx <= maxW)
                {
                    *w = mx; if (h) *h = oh;
                    DimStore(t, idata, s, m, maxW, ow, oh, mx, r);
                    g_GDDepth--;
                    return r;
                }
            }
        }
    }
    for (i = 0; i < 256; i++)
    {
        wchar_t wc = s[i];
        int ga;
        if (!wc) break;
        if (wc < 0x20) goto fwd;
        ga = GtCharAdv(wc);
        if (ga < 0) goto fwd;
        if (wc >= 0x30 && wc <= 0x39) ga += g_VT.DigitTrack;
        if (ga < 0) ga = 0;
        tot += ga;
        if (maxW > 0 && tot >= maxW) goto fwd;
    }
    if (i >= 256) goto fwd;
    *w = tot; if (h) *h = oh;
    DimStore(t, idata, s, i, maxW, ow, oh, tot, r);
    g_GDDepth--;
    return r;
fwd:
    if (r) { *w = ow; if (h) *h = oh; }
    g_GDDepth--;
    return r;
}

/* 引擎单字度量 (走原版)。记忆表键含行高, 失败也记。 */
#define VM_N 1024
typedef struct {
    void* t;
    void* idata;
    wchar_t w;
    short adv;
    short h;
    int step;      /* font+0x1C */
} VMEnt;
static VMEnt g_VM[VM_N];

static int VecMeasure(void* t, wchar_t wch, int* ph)
{
    wchar_t s[2];
    int w = 0, h = 0, idx, step;
    void* idata;
    VMEnt* e;
    if (!t || !IsPlausiblePtr(t) || !g_OrigGetDim) return -1;
    idata = *(void**)((char*)t + 4);
    step = *(int*)((char*)t + 0x1C);
    idx = (int)((((unsigned)wch) * 2654435761u >> 16) & (VM_N - 1));
    e = &g_VM[idx];
    if (e->t == t && e->idata == idata && e->w == wch && e->step == step &&
        e->adv != 0)
    {
        if (e->adv < 0) return -1;
        if (ph) *ph = e->h;
        return e->adv;
    }
    s[0] = wch; s[1] = 0;
    if (!g_OrigGetDim(t, NULL, s, &w, &h, 10000) ||
        w <= 0 || w > 64 || h <= 0 || h > 64)
    {
        e->t = t; e->idata = idata; e->w = wch; e->step = step;
        e->adv = -1; e->h = 0;
        return -1;
    }
    e->t = t; e->idata = idata; e->w = wch; e->step = step;
    e->adv = (short)w; e->h = (short)h;
    if (ph) *ph = h;
    return w;
}

/* 字形缓存: 灰度, 键=(字,字体,字号,格宽,格高,摆位)。 */
#define VC_MASK 2047
/* 失败哨兵 (负笔位合法, 不用负值判失败)。 */
#define VC_FAIL (-1000000)

typedef struct {
    int used;
    wchar_t wch;
    void* idata;    /* BitFont+4 */
    int px;
    int adv;
    int h;
    int outh;
    int fullW;
    int gdiW;
    int off, inkW;
    int nat;
    /* 逐行墨水区间, 渲染只扫有墨区间。 */
    unsigned char rowLo[VC_MAXH], rowHi[VC_MAXH];
    unsigned char cov[VC_MAXW * VC_MAXH];
} VCEnt;

static VCEnt g_VC[VC_MASK + 1];

/* ---------------- 基准窗校准 ----------------
 * 首用时取 永/g 并集为垂直窗, 之后共用 1:1 输出。 */
#define GL_M 8
#define GL_Y0 8

static int g_RefValid = 0;
static int g_RefPx = -1, g_RefBold = -1, g_RefT = 0, g_RefB = 0;
/* 字体键存指针 (g_VTFace 初始化后不变, 逐字判等省循环)。 */
static const wchar_t* g_RefFace = NULL;

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
    if (g_RefValid && g_RefPx == px && g_RefBold == g_VT.Bold &&
        g_RefFace == g_VTFace) return;
    T = DIB_H; B = -1;
    VecRefScan(0x6C38, &T, &B);   /* 永 */
    VecRefScan(0x0067, &T, &B);   /* g */
    VecRefScan(0x006A, &T, &B);   /* j */
    if (B < T) { T = GL_Y0; B = GL_Y0 + px - 1; }   /* 量不到就退化为等同 */
    g_RefValid = 1; g_RefPx = px; g_RefBold = g_VT.Bold;
    g_RefT = T; g_RefB = B;
    g_RefFace = g_VTFace;
}

/* 原版每字格内垂直居中, 墨水盒不照抄改版 fnt。 */

/* ---------------- 字号校准 ----------------
 * 对格高试渲染取墨水高, 一步收敛。键=(idata,face,bold)。 */

static int g_PxValid = 0;
static void* g_PxIdata = NULL;
static int g_PxPx = 19, g_PxBold = -1, g_PxPxSrc = 0;
static const wchar_t* g_PxFace = NULL;

static int VecPxCalibrate(void* t)
{
    void* idata;
    int origH, px0, T, B, inkH, px, gw, x, y, scanW;
    unsigned char* bits;
    int pitch;
    if (!t || !IsPlausiblePtr(t)) return -1;
    idata = *(void**)((char*)t + 4);
    if (g_PxValid && g_PxIdata == idata && g_PxBold == g_VT.Bold &&
        g_PxPxSrc == (g_VT.FontPx > 0 ? g_VT.FontPx : 0) &&
        g_PxFace == g_VTFace) return g_PxPx;
    if (!idata || !IsPlausiblePtr(idata)) return -1;
    if (g_VT.FontPx > 0)
    {
        px = g_VT.FontPx;
        if (px < 8) px = 8;
        if (px > 40) px = 40;
        origH = *(int*)((char*)idata + 0x0C);
        if (origH < 8 || origH > 40) origH = 19;
        g_PxValid = 1; g_PxIdata = idata; g_PxPx = px; g_PxBold = g_VT.Bold;
        g_PxPxSrc = g_VT.FontPx;
        g_FontOrigH = origH;
        g_PxFace = g_VTFace;
        g_BriefOrigH = origH;
        return px;
    }
    origH = *(int*)((char*)idata + 0x0C);
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
    g_FontOrigH = origH;
    g_PxFace = g_VTFace;
    g_BriefOrigH = origH;
    return px;
}

/* 字号 = 固定或自动校准。顶栏数字由 Blit 覆盖。 */
static int VecPickPx(void* t)
{
    return VecPxCalibrate(t);
}

/* 字形横向摆位: 自然模式摆 abcA, 窄墨水按边距居中。返回 off, gdiW 进 *pg. */
static int VecGlyphOff(wchar_t wch, int adv, int inkW, int* pg)
{
    int a = 0, b = 0, c = 0, gw2 = GtCharABC(wch, &a, &b, &c), off = 0;
    int gdiW = (gw2 > 0) ? gw2 : 0;
    if (gw2 > 0)
    {
        if (g_VecNat)
            off = a;
        else if (inkW > 0 && inkW * 3 <= adv * 2)
        {
            off = a + (adv - gdiW) / 2;
            if (off < 0) off = 0;
            if (off > adv - 1) off = adv - 1;
        }
    }
    if (pg) *pg = gdiW;
    return off;
}

static VCEnt* VecGlyph(void* t, wchar_t wch, int adv, int h, int px)
{
    unsigned idx = ((unsigned)wch) & VC_MASK;
    VCEnt* e = &g_VC[idx];
    void* idata = *(void**)((char*)t + 4);
    int gw, dx, y, L, R, inkW, x, off, gdiW;
    /* 边距: 墨水可超 extent, 顶边不顶着画。垂直用基准窗 1:1。 */
    int scanW;
    unsigned char* bits;
    int pitch;
    int winTop, outH;
    if (!t || !IsPlausiblePtr(t)) { g_VecWhy = 10; return NULL; }
    if (px < 8 || px > 40) { g_VecWhy = 11; return NULL; }
    if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
        e->adv == adv && e->h == h && e->nat == g_VecNat)
        return e;
    if (g_PfOn) g_PfMiss++;   /* 未命中 = 一次 GDI 光栅 + DIB 清 + ABC 填充 */
    /* 未命中: GDI 单字光栅 (带边距), 掃出真实墨水盒再摆进格子 */
    if (!GdiEnsure(px, g_VT.Bold, VecGdiSlot(px), g_VTFace)) { g_VecWhy = 12; return NULL; }
    VecRefCalibrate(px);
    gw = GtRenderGlyphAt(wch, GL_M, GL_Y0, NULL);
    if (gw <= 0 || h > VC_MAXH) { g_VecWhy = 13; return NULL; }
    bits = g_G->bits;
    pitch = g_G->pitch;
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    /* 自然高度 1:1, 不压缩。 */
    winTop = g_RefT;
    outH = g_RefB - g_RefT + 1;
    if (outH < 1) outH = h;
    if (outH > VC_MAXH) { g_VecWhy = 14; return NULL; }
    if (winTop < 0 || winTop + outH > DIB_H) { g_VecWhy = 15; return NULL; }
    /* 墨水盒只定左右。 */
    L = scanW; R = -1;
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
    }
    /* 全墨水不压缩, 溢出由面界收尾。 */
    inkW = (R >= L) ? R - L + 1 : 0;
    off = VecGlyphOff(wch, adv, inkW, &gdiW);
    if (off + inkW > VC_MAXW) { g_VecWhy = 16; return NULL; }
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        unsigned char* dst = e->cov + y * VC_MAXW;
        int lo = VC_MAXW, hi = -1;
        for (dx = 0; dx < VC_MAXW; dx++) dst[dx] = 0;
        if (!inkW) { e->rowLo[y] = 1; e->rowHi[y] = 0; continue; }  /* 空白字 */
        for (dx = 0; dx < inkW; dx++)
        {
            unsigned char a = row[(L + dx) * 4 + 2];
            dst[off + dx] = a;
            if (a) { if (off + dx < lo) lo = off + dx; if (off + dx > hi) hi = off + dx; }
        }
        e->rowLo[y] = (unsigned char)(hi >= lo ? lo : 1);
        e->rowHi[y] = (unsigned char)(hi >= lo ? hi : 0);
    }
    e->used = 1; e->wch = wch; e->idata = idata; e->px = px; e->adv = adv; e->h = h;
    e->nat = g_VecNat;
    e->outh = outH;
    e->fullW = off + inkW;
    e->gdiW = gdiW;
    e->off = off; e->inkW = inkW;
    return e;
}

/* 矢量替换: 成功返回新笔位, 失败返回 VC_FAIL。 */
static int VecRender(void* t, wchar_t wch, int adv, int h, int x, int y,
                     int colorW, int px, int yOff)
{
    unsigned short* buf;
    int pitchW, Tp, B, dx, dy;
    unsigned char fr, fg, fb;
    VCEnt* e;
    g_VecWhy = 0;
    if (!t || !IsPlausiblePtr(t)) { g_VecWhy = 1; return VC_FAIL; }
    if (adv > VC_MAXW || h > VC_MAXH) { g_VecWhy = 2; return VC_FAIL; }
    buf = *(unsigned short**)((char*)t + 0x0C);
    pitchW = *(int*)((char*)t + 0x10);
    if (!buf || !IsPlausiblePtr(buf)) { g_VecWhy = 3; return VC_FAIL; }
    if (pitchW < 64 || pitchW > 4096) { g_VecWhy = 4; return VC_FAIL; }
    Tp = VecNormC(*(int*)((char*)t + 0x34));
    B = VecNormC(*(int*)((char*)t + 0x3C));
    if (B < Tp)
    {
        /* 空裁剪哨兵退主面边界。 */
        int cR = VecNormC(*(int*)((char*)t + 0x38));
        if (cR >= 0 && B >= 0) { g_VecWhy = 5; return VC_FAIL; }
        {
            int sh = SB_CanvasH(pitchW);
            if (sh < 1) { g_VecWhy = 5; return VC_FAIL; }
            Tp = 0; B = sh - 1;
        }
    }
    if (Tp < 0) Tp = 0;
    if (B > 4096) B = 4096;
    Calibrate();
    e = VecGlyph(t, wch, adv, h, px);
    if (!e) { if (g_VecWhy == 0) g_VecWhy = 6; return VC_FAIL; }
    GtUnpack16((unsigned short)(colorW & 0xFFFF), &fr, &fg, &fb);
    /* 整体垂直居中, 字间相对位置不动。小矩形紧裁剪时贴上沿。 */
    dy = y + (h - e->outh) / 2;
    {
        int lo = Tp, hi = B - e->outh + 1;
        if (hi >= lo)
        {
            if (dy < lo) dy = lo;
            if (dy > hi) dy = hi;
        }
    }
    dy += yOff;
    for (y = 0; y < e->outh; y++)
    {
        int sy = dy + y;
        unsigned short* row;
        unsigned char* cov;
        int lo = e->rowLo[y], hi = e->rowHi[y], x0, x1;
        if (hi < lo) continue;
        if (sy < Tp || sy > B) continue;
        row = buf + sy * pitchW;
        cov = e->cov + y * VC_MAXW;
        /* 横向只认面界, 溢出为正常 overhang。只扫有墨区间。 */
        x0 = lo;
        x1 = hi;
        if (x + x0 < 0) x0 = -x;
        if (x + x1 >= pitchW) x1 = pitchW - 1 - x;
        for (dx = x0; dx <= x1; dx++)
        {
            unsigned a = cov[dx];
            if (!a) continue;
            row[x + dx] = Blend16(row[x + dx], fr, fg, fb, a);
        }
    }
    /* 步进: GDI 步进优先, 只加宽不收窄。正文盒内保持原笔位, 数字除外。 */
    {
        int digit = (wch >= 0x30 && wch <= 0x39);
        int st = (e->gdiW > 0) ? e->gdiW + (digit ? g_VT.DigitTrack : 0)
                               : e->fullW;
        if (st > adv && (digit || !g_DTBody)) return x + st;
    }
    return x + adv;
}

/* 菜单字形缓存。 */
#define MV_MASK 127
typedef struct {
    int used;
    wchar_t wch;
    void* idata;
    int px, adv, h;
    int off, inkW, outh;
    int gdiW;
    unsigned char cov[VC_MAXW * VC_MAXH];
} MVEnt;
static MVEnt g_MV[MV_MASK + 1];

static MVEnt* VecMenuGlyph(void* t, wchar_t wch, int adv, int h, int px)
{
    unsigned idx = ((unsigned)wch) & MV_MASK;
    MVEnt* e = &g_MV[idx];
    void* idata = *(void**)((char*)t + 4);
    int gw, dx, y, L, R, inkW, x, off, gdiW;
    int scanW;
    unsigned char* bits;
    int pitch;
    int winTop, outH;
    if (!t || !IsPlausiblePtr(t)) return NULL;
    if (px < 8 || px > 40) return NULL;
    if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
        e->adv == adv && e->h == h)
        return e;
    if (!GdiEnsure(px, g_VT.Bold, VecGdiSlot(px), g_VTFace)) return NULL;
    VecRefCalibrate(px);
    gw = GtRenderGlyphAt(wch, GL_M, GL_Y0, NULL);
    if (gw <= 0 || h > VC_MAXH) return NULL;
    bits = g_G->bits;
    pitch = g_G->pitch;
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    winTop = g_RefT;
    outH = g_RefB - g_RefT + 1;
    if (outH < 1) outH = h;
    if (outH > VC_MAXH) return NULL;
    if (winTop < 0 || winTop + outH > DIB_H) return NULL;
    L = scanW; R = -1;
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
    }
    inkW = (R >= L) ? R - L + 1 : 0;
    off = VecGlyphOff(wch, adv, inkW, &gdiW);
    if (off + inkW > VC_MAXW) return NULL;
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        unsigned char* dst = e->cov + y * VC_MAXW;
        for (dx = 0; dx < VC_MAXW; dx++) dst[dx] = 0;
        if (!inkW) continue;   /* 空白字: 占宽, 不画 */
        for (dx = 0; dx < inkW; dx++)
            dst[off + dx] = row[(L + dx) * 4 + 2];
    }
    e->used = 1; e->wch = wch; e->idata = idata; e->px = px; e->adv = adv;
    e->h = h; e->off = off; e->inkW = inkW;
    e->outh = outH;
    e->gdiW = gdiW;
    return e;
}

/* 菜单步进: GDI 步进优先, 无则墨水右缘。 */
static int VecMenuStep(MVEnt* me, int fallback)
{
    int st;
    if (!me) return fallback;
    if (me->gdiW <= 0)
        return (me->inkW > 0) ? me->off + me->inkW : fallback;
    st = me->gdiW;
    if (me->wch >= 0x30 && me->wch <= 0x39) st += g_VT.DigitTrack;
    if (st < 1) st = 1;
    return st;
}

/* 菜单字形绘制: 成功返回新笔位, 失败 -1。 */
static int VecMenuRender(void* t, wchar_t wch, int adv, int h, int x, int y,
                         int colorW, int px)
{
    unsigned short* buf;
    int pitchW, Tp, B, dx, dy;
    unsigned char fr, fg, fb;
    MVEnt* e;
    if (!t || !IsPlausiblePtr(t)) return -1;
    if (adv > VC_MAXW || h > VC_MAXH) return -1;
    buf = *(unsigned short**)((char*)t + 0x0C);
    pitchW = *(int*)((char*)t + 0x10);
    if (!buf || !IsPlausiblePtr(buf)) return -1;
    if (pitchW < 64 || pitchW > 4096) return -1;
    Tp = VecNormC(*(int*)((char*)t + 0x34));
    B = VecNormC(*(int*)((char*)t + 0x3C));
    if (B < Tp) return -1;
    if (Tp < 0) Tp = 0;
    if (B > 4096) B = 4096;
    Calibrate();
    e = VecMenuGlyph(t, wch, adv, h, px);
    if (!e) return -1;
    GtUnpack16((unsigned short)(colorW & 0xFFFF), &fr, &fg, &fb);
    dy = y + (h - e->outh) / 2;
    {
        int lo = Tp, hi = B - e->outh + 1;
        if (hi >= lo)
        {
            if (dy < lo) dy = lo;
            if (dy > hi) dy = hi;
        }
    }
    for (y = 0; y < e->outh; y++)
    {
        int sy = dy + y;
        unsigned short* row;
        unsigned char* cov;
        if (sy < 0 || sy > B) continue;
        row = buf + sy * pitchW;
        cov = e->cov + y * VC_MAXW;
        for (dx = 0; dx < e->inkW; dx++)
        {
            int sx = x + e->off + dx;
            unsigned a;
            if (sx < 0 || sx >= pitchW) continue;
            if (!cov[e->off + dx]) continue;
            a = cov[e->off + dx];
            row[sx] = Blend16(row[sx], fr, fg, fb, a);
        }
    }
    return x + adv;
}

/* 超武逐行绘制已移除: 整体接管改为 Print 入口隐藏+收集, 消息帧统一整块重绘
 * (见 MOExperience 超武面板)。 */

int __fastcall VecText_BlitHook(void* t, void* edxUnused, int wch, int x, int y, int color)
{
    wchar_t wc = (wchar_t)((unsigned)wch & 0xFFFFu);
    int eff, adv, h = 0, r, px;
    (void)edxUnused;
    if (!g_OrigBlit) return 0;   /* 不该发生, 守一下不断原版链 */
    /* TAB 走原版特殊公式, 缺字/度量失败也走原版 (保留 0x5555 兜底行为) */
    if (!t || !IsPlausiblePtr(t) || wc == 9)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    if (g_PfOn) { g_PfGlyph++; PfTick(); }
    /* 坐标归一 (脏高位清掉, 见 VecNormC): 以下 inTip 跨度
     * 与 VecRender 全用真坐标。 */
    x = VecNormC(x);
    y = VecNormC(y);
    eff = (color == -1) ? (int)(*(unsigned short*)((char*)t + 0x24)) : color;
    /* PUA 自定义图标 (建造提示第三行): PNG 位图直画, 不走 GDI/原版点阵。
     * 放在 VecMeasure 之前 (原版度量不认 PUA, 必失败回落出 tofu)。 */
    if (Icon_Is((unsigned)wc & 0xFFFFu))
    {
        int ia = Icon_Advance((unsigned)wc & 0xFFFFu);
        if (ia > 0)
        {
            int rr = Icon_Blit(t, (unsigned)wc & 0xFFFFu, x, y, eff,
                               Icon_LineH(t));
            if (rr >= 0) return x + ia;
        }
        return g_OrigBlit(t, NULL, wch, x, y, color);
    }
    adv = VecMeasure(t, wc, &h);
    if (adv < 0)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    /* 简报正文自绘 (BriefOwn=1): 面到这里已经绑好了 (引擎 0x4348F0 装的),
     * 借它的面把整块画掉, 并抑制这一次 DrawText 余下的逐字绘制。笔位照
     * 原样还给引擎 (x+adv), 引擎版式照跑, 只是不再落墨。 */
    if (g_DTBody && g_BriefState != 2)
    {
        if (g_BriefState == 1) return x + adv;
        g_BriefState = 1;
        if (VecBriefBody(t, g_BriefStr, g_BriefX, g_BriefY, g_BriefW) == 0)
            return x + adv;
        g_BriefState = 2;   /* 不可用: 这一字照原版画, 后面也不抑制 */
    }
    int tx = x;
    int inTip = (g_LastTipX >= 0 && g_TipTot > 0 &&
                 tx >= g_LastTipX - g_TipTot - 16 &&
                 tx <= g_LastTipX + g_TipTot + 8 &&
                 y >= g_TipY - 8 && y <= g_TipY + 8);
    {
        /* 位置门 (颜色盲): 本 Print 的首个 Blit 落在当帧提示跨度内。
         * 边缘出屏字会被引擎跳过 (不调 Blit), 首 Blit 未必是串首,
         * 故按跨度 [tipx-tot-16, tipx+tot+8] 认; 不按原点 ±8。
         * 右界 tot+8: 本串字 ENGINE x 不会超 a4+tot (账本即此)。
         * 每批只进一次 (seen 守卫): 串内后续字的 tx 天然递增, 绝不能按"漂移"
         * 重画盒, 否则盒跟着字走、越走越偏。单线程 Print→Blits 同步,
         * 陈旧跨 Print 不可能, 无需漂移。 */
        if (!g_BlitTipSeen &&
            tx >= g_LastTipX - g_TipTot - 16 &&
            tx <= g_LastTipX + g_TipTot + 8 &&
            y >= g_TipY - 8 && y <= g_TipY + 8 &&
            t && IsPlausiblePtr(t))
        {
            unsigned short* bb = *(unsigned short**)((char*)t + 0x0C);
            int pp = *(int*)((char*)t + 0x10);
            g_BlitTipSeen = 1;
            /* 出界修复 (TipBox): 定制盒。引擎窄盒 [cx-63,cx+62] 已在,
             * 按列调映射 r=盒内/盒旁 (逐通道钳 0.15~0.95) 补两侧条带
             * (纹理保留, 同 tone 无缝), 采样红画新边框, 字随后画上。
             * 坐标全落在已扫描安全区内; 关游戏即无残留 (世界每帧重画)。
             * TipBox=0 关闭。 */
            /* 定制盒: 每批首个落点 Blit 进一次 (seen 守卫),
             * 串内后续字不进; 锚点/几何见下, 出屏即真裁剪。 */
            if (g_VT.TipBox && g_TipTot >= 50 && g_TipTot <= 400 &&
                bb && IsPlausiblePtr((void*)bb) &&
                pp >= 64 && pp <= 4096)
            {
/* 盒锚点取 max(Print 落笔, 首 Blit x):
                  * 首字出屏被跳过时首 Blit 偏右 (前面几个字已在屏外),
                  * 跟 Blit x 左沿才与可见字对齐, 右沿仍用 Print 真右沿
                  * (tipx+tot, 不多占); 取 min 会把盒钉在旧落笔、字溢出右沿。
                  * cx (引擎窄盒中心) 仍按 Print 版式取, 与裁剪无关。 */
                int anchor = (tx > g_LastTipX) ? tx : g_LastTipX;
                int cx = g_LastTipX + g_TipTot / 2;
                int oldHW = (g_TipOw > 0 ? g_TipOw : 118) / 2 + 4;
                int oldL = cx - oldHW, oldR = cx + oldHW - 1;
                int newL = anchor - 4, newR = g_LastTipX + g_TipTot + 4;
                int yT = y - 2, yB = y + 18;
                int xx, yy;
                /* 边缘钳制: 只画屏内部分。oldL/oldR 随 ow 自适应
                 * (118->±63 实测; 长串按 ow+8 估, 截图校准)。 */
                int L0 = newL < 0 ? 0 : newL;
                int R0 = newR >= pp ? pp - 1 : newR;
                int cL = oldL < L0 ? L0 : oldL;
                int cR = oldR > R0 ? R0 : oldR;
                /* 真裁剪 (无模式切换): 盒与字永远同宽同锚
                 * ([anchor-4, tipx+tot+4]), 作为一个整体进出屏幕;
                 * 出屏侧不画边框。Print:Simple_ 是 1:1, 无额外绘制。 */
                if (L0 <= R0 && yT >= 0 && yB <= 4096)
                {
                    unsigned bc = 0xF800u;
                    unsigned bestR = 0;
                    /* 边框色沿用顶行采样 (引擎窄盒顶边=玩家色);
                     * 出界后顶行剩下全黑、采不到 (bestR==0) 就用粘存。 */
                    for (xx = cL; xx <= cR; xx++)
                    {
                        unsigned c = bb[yT * pp + xx];
                        unsigned R = (c >> 11) & 31u;
                        if (R > bestR)
                        {
                            bestR = R; bc = c;
                        }
                    }
                    if (bestR > 0)
                    {
                        g_TipBcCache = bc; g_TipBcOK = 1;
                    }
                    else if (g_TipBcOK) bc = g_TipBcCache;
                    /* 底一律填不透黑 (图 1 定案): 出界后映射填充会漏出
                     * 引擎半透明底 (图 2 的病); 实色填充与边框同范围,
                     * 字随后画上, 不盖字。 */
                    for (yy = yT + 1; yy <= yB - 1; yy++)
                    {
                        for (xx = L0; xx <= R0; xx++)
                            bb[yy * pp + xx] = 0;
                    }
                    for (xx = L0; xx <= R0; xx++)
                    {
                        bb[yT * pp + xx] = (unsigned short)bc;
                        bb[yB * pp + xx] = (unsigned short)bc;
                    }
                    /* 竖边框只画屏内段: 出屏侧顶着屏沿画会造出一条
                     * 本不存在的假边框 (左对齐即此), 直接跳过。 */
                    for (yy = yT + 1; yy <= yB - 1; yy++)
                    {
                        if (newL >= 0)
                            bb[yy * pp + L0] = (unsigned short)bc;
                        if (newR < pp)
                            bb[yy * pp + R0] = (unsigned short)bc;
                    }
                }
            }
        }
    }
    int ox = x;
    if (g_MenuDepth > 0)
    {
        /* 菜单整行自有版式: 首 Blit 建全串字形、算总宽、矩形内居中起笔,
         * 之后按实际墨水宽度走笔 (空格按引擎 adv, 只加宽不收窄)。
         * 串序对上才用自有笔位, 否则回落引擎笔位 (保底不错位);
         * 原字形一次不画, 双双失败才走原版 (保底可读)。 */
        int built = g_MenuBuilt;
        px = VecPickPx(t);
        if (px >= 0 && g_MenuN > 0 && !built)
        {
            int i, tot = 0, ok = 1;
            for (i = 0; i < g_MenuN; i++)
            {
                int ca, chh = 0;
                MVEnt* gi;
                ca = VecMeasure(t, g_MenuStr[i], &chh);
                if (ca < 0) ca = 8;
                g_MenuAdv[i] = ca;
                gi = VecMenuGlyph(t, g_MenuStr[i], ca, h, px);
                if (!gi) { ok = 0; break; }
                tot += VecMenuStep(gi, ca);
            }
            if (ok)
            {
                g_MenuPenX = g_MenuRX + (g_MenuRW - tot) / 2;
                if (g_MenuPenX < g_MenuRX) g_MenuPenX = g_MenuRX;
                g_MenuBuilt = 1;
                built = 1;
            }
        }
        if (built && g_MenuIdx < g_MenuN && wc == g_MenuStr[g_MenuIdx])
        {
            MVEnt* me;
            int step;
            r = VecMenuRender(t, wc, g_MenuAdv[g_MenuIdx], h, g_MenuPenX,
                              y, eff, px);
            if (r >= 0)
            {
                me = VecMenuGlyph(t, wc, g_MenuAdv[g_MenuIdx], h, px);
                step = VecMenuStep(me, g_MenuAdv[g_MenuIdx]);
                if (step < 1) step = 1;
                g_MenuPenX += step;
                g_MenuIdx++;
                return x + adv;   /* 引擎账本照旧, 笔位我们自己走 */
            }
        }
        if (px >= 0)
        {
            r = VecMenuRender(t, wc, adv, h, x, y, eff, px);
            if (r >= 0) return r;
        }
        return g_OrigBlit(t, NULL, wch, x, y, color);
    }
    if (g_VT.Render)
    {
        px = VecPickPx(t);
        if (px >= 0)
        {
            /* 顶栏条带 (y<32) 的数字: 唯一的顶栏判据, 与发起者无关。
             * 同一条顶栏至少 5 个发起者反复画, 按位置一刀切,
             * 大小偏移永远一致, 闪烁自止。只在用户配了 CreditPx/DY 时生效,
             * 默认 0/0 全不碰。超武列表等不在条带内, 不受影响。 */
            int cred = (wc >= 0x30 && wc <= 0x39 && y >= -8 && y < 32 &&
                (g_VT.CreditPx > 0 || g_VT.CreditDY != 0));
            if (cred && g_VT.CreditPx > 0)
            {
                px = g_VT.CreditPx;
                if (px < 8) px = 8;
                if (px > 40) px = 40;
            }
            /* 字号 (全局): 墨水与字宽等比缩放, 版式才对。
             * 条带数字除外 (只换墨水字宽不动, 不碰已调好的顶栏)。
             * 行距/换行仍是引擎的, 字号偏离原版太多可能溢出盒子蹭行。 */
            if (!cred && g_VT.FontPx > 0 && g_FontOrigH >= 8 &&
                g_FontOrigH <= 40)
            {
                adv = (adv * g_VT.FontPx + g_FontOrigH / 2) / g_FontOrigH;
                if (adv < 1) adv = 1;
            }
            r = VecRender(t, wc, adv, h, (inTip ? tx : x), y, eff,
                          px, (cred ? g_VT.CreditDY : 0));
            /* VecRender 失败时记一行 (字/坐标/失败点 why), 上限 8。 */
            if (r == VC_FAIL)
            {
                static int nF = 0;
                if (nF < 8)
                {
                    char fb1[16], ff[112];
                    unsigned fz = 0;
                    nF++;
                    fz = AppendCStr(ff, fz, "vfb: wch=", sizeof(ff));
                    fz += HtoA((unsigned)wc & 0xFFFFu, ff + fz);
                    fz = AppendCStr(ff, fz, " x=", sizeof(ff));
                    UtoA((unsigned)(tx + 2000), fb1);
                    fz = AppendCStr(ff, fz, fb1, sizeof(ff));
                    fz = AppendCStr(ff, fz, " y=", sizeof(ff));
                    UtoA((unsigned)y, fb1);
                    fz = AppendCStr(ff, fz, fb1, sizeof(ff));
                    fz = AppendCStr(ff, fz, " adv=", sizeof(ff));
                    UtoA((unsigned)adv, fb1);
                    fz = AppendCStr(ff, fz, fb1, sizeof(ff));
                    fz = AppendCStr(ff, fz, " why=", sizeof(ff));
                    UtoA((unsigned)g_VecWhy, fb1);
                    fz = AppendCStr(ff, fz, fb1, sizeof(ff));
                    LogLine(ff);
                }
            }
            if (r != VC_FAIL) return r;   /* x == ox (本函数内 x 只读)。 */
        }
    }
    return g_OrigBlit(t, NULL, wch, ox, y, color);
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

/* 通用跳板/补丁 (入口钩用)。
 * `n` 须落在完整指令末字节上 (落半截即从半个指令反汇编跑飞)。
 * `n` 范围禁 E8/E9 (rel32 基准变跳板自己 ⇒ 跳数据地址去，见 notes §23)，
 * 这里直接拒绝。用前先反汇编确认末字节。 */
static void* BuildTrampAt(unsigned at, int n, unsigned back)
{
    unsigned char* b;
    int i;
    if (n < 1 || n > 16) return NULL;
    for (i = 0; i < n; i++)
    {
        unsigned char c = ((unsigned char*)at)[i];
        if (c == 0xE8 || c == 0xE9)
        {
            char m[80], n1[16];
            unsigned q = 0;
            q = AppendCStr(m, q, "tramp refused: PC-relative at ", sizeof(m));
            UtoA(at, n1); q = AppendCStr(m, q, n1, sizeof(m));
            q = AppendCh(m, q, ' ', sizeof(m));
            UtoA((unsigned)c, n1); q = AppendCStr(m, q, n1, sizeof(m));
            LogLine(m);
            return NULL;
        }
    }
    b = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!b) return NULL;
    for (i = 0; i < n; i++) b[i] = ((unsigned char*)at)[i];
    b[n] = 0xE9;
    *(int*)(b + n + 1) = (int)(back - ((unsigned)(size_t)b + n + 5));
    FlushInstructionCache(GetCurrentProcess(), b, (SIZE_T)(n + 5));
    return b;
}

static int PatchAt(unsigned at, void* target, int n)
{
    unsigned char* p = (unsigned char*)at;
    DWORD old;
    int rel = (int)((unsigned)(size_t)target - (at + 5));
    int i;
    if (n < 5) return 0;
    if (!VirtualProtect(p, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0xE9;
    *(int*)(p + 1) = rel;
    for (i = 5; i < n; i++) p[i] = 0x90;
    VirtualProtect(p, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, (SIZE_T)n);
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

/* 顶栏资金字号。 */
int VecText_CreditPx(void)
{
    return g_VT.CreditPx;
}
/* 资金字体+粗体, 电力同用。 */
const wchar_t* VecText_FontFace(void)
{
    return g_VTFace;
}

int VecText_FontPx(void)
{
	return g_VT.FontPx;
}
int VecText_Bold(void)
{
	return g_VT.Bold;
}

void VecText_EnsureInit(void)
{
    char b1[16], buf[192];
    unsigned p;
    if (g_VtInit) return;
    g_VtInit = 1;
    GtEnsureLog();
    if (!g_Log) return;
    g_VT.Enable = GetPrivateProfileIntA(VT_SECTION, "Enable", 1, g_IniPath);
    OU_ReadString(VT_SECTION, "FontFace", "simhei.ttf",
                  g_VT.FontFace, sizeof(g_VT.FontFace));
    g_VT.Bold = GetPrivateProfileIntA(VT_SECTION, "Bold", 0, g_IniPath);
    g_VT.FontPx = GetPrivateProfileIntA(VT_SECTION, "FontPx", 0, g_IniPath);
    g_VT.CreditPx = GetPrivateProfileIntA(VT_SECTION, "CreditPx", 0, g_IniPath);
    g_VT.CreditDY = GetPrivateProfileIntA(VT_SECTION, "CreditDY", 0, g_IniPath);
    g_VT.MenuTake = GetPrivateProfileIntA(VT_SECTION, "MenuTake", 1, g_IniPath);
    g_VT.DigitTrack = GetPrivateProfileIntA(VT_SECTION, "DigitTrack", 0, g_IniPath);
    g_VT.TipBox = GetPrivateProfileIntA(VT_SECTION, "TipBox", 1, g_IniPath);
    g_VT.BriefLine = GetPrivateProfileIntA(VT_SECTION, "BriefLine", 130, g_IniPath);
    g_VT.BriefOwn = GetPrivateProfileIntA(VT_SECTION, "BriefOwn", 1, g_IniPath);
    g_VT.BriefTrack = GetPrivateProfileIntA(VT_SECTION, "BriefTrack", 2, g_IniPath);
    g_VT.BriefMark = GetPrivateProfileIntA(VT_SECTION, "BriefMark", 1, g_IniPath);
    g_VT.BriefWidth = GetPrivateProfileIntA(VT_SECTION, "BriefWidth", 620, g_IniPath);
    g_VT.BriefAdv = GetPrivateProfileIntA(VT_SECTION, "BriefAdv", 1, g_IniPath);
    g_VT.LogDetail = GetPrivateProfileIntA("Log", "LogDetail", 0, g_IniPath);
    g_VT.Perf = GetPrivateProfileIntA(VT_SECTION, "Perf", 0, g_IniPath);
    g_VT.Render = GetPrivateProfileIntA(VT_SECTION, "Render", 0, g_IniPath);
    g_VT.Enable = g_VT.Enable ? 1 : 0;
    g_VT.Bold = g_VT.Bold ? 1 : 0;
    if (g_VT.FontPx < 0) g_VT.FontPx = 0;
    if (g_VT.FontPx > 40) g_VT.FontPx = 40;
    if (g_VT.CreditPx < 0) g_VT.CreditPx = 0;
    if (g_VT.CreditPx > 40) g_VT.CreditPx = 40;
    if (g_VT.CreditDY < -8) g_VT.CreditDY = -8;
    if (g_VT.CreditDY > 8) g_VT.CreditDY = 8;
    g_VT.MenuTake = g_VT.MenuTake ? 1 : 0;
    if (g_VT.DigitTrack < -8) g_VT.DigitTrack = -8;
    if (g_VT.DigitTrack > 8) g_VT.DigitTrack = 8;
    g_VT.TipBox = g_VT.TipBox ? 1 : 0;
    g_VT.BriefOwn = g_VT.BriefOwn ? 1 : 0;
    if (g_VT.BriefTrack < 0) g_VT.BriefTrack = 0;
    if (g_VT.BriefTrack > 12) g_VT.BriefTrack = 12;
    g_VT.BriefMark = g_VT.BriefMark ? 1 : 0;
    if (g_VT.BriefWidth < 0) g_VT.BriefWidth = 0;
    if (g_VT.BriefWidth > 4000) g_VT.BriefWidth = 4000;
    g_VT.BriefAdv = g_VT.BriefAdv ? 1 : 0;
    if (g_VT.BriefLine < 0) g_VT.BriefLine = 0;
    if (g_VT.BriefLine > 300) g_VT.BriefLine = 300;
    g_VT.LogDetail = g_VT.LogDetail ? 1 : 0;
    g_VT.Perf = g_VT.Perf ? 1 : 0;
    g_PfOn = g_VT.Perf;
    g_LogDetail = g_VT.LogDetail;
    g_VT.Render = g_VT.Render ? 1 : 0;
    ResolveFontFace(g_VT.FontFace, "vtext", g_VTFace, L"Microsoft YaHei");
    p = 0;
    p = AppendCStr(buf, p, "vtext: start en=", sizeof(buf));
    UtoA((unsigned)g_VT.Enable, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " dtail=", sizeof(buf));
    UtoA((unsigned)g_VT.LogDetail, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " render=", sizeof(buf));
    UtoA((unsigned)g_VT.Render, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " px=", sizeof(buf));
    UtoA((unsigned)g_VT.FontPx, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " cred=", sizeof(buf));
    UtoA((unsigned)g_VT.CreditPx, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " credy=", sizeof(buf));
    {
        int dy = g_VT.CreditDY;
        if (dy < 0) { p = AppendCh(buf, p, '-', sizeof(buf)); dy = -dy; }
        UtoA((unsigned)dy, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    }
    p = AppendCStr(buf, p, " mt=", sizeof(buf));
    UtoA((unsigned)g_VT.MenuTake, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " dt=", sizeof(buf));
    {
        int dtr = g_VT.DigitTrack;
        if (dtr < 0) { p = AppendCh(buf, p, '-', sizeof(buf)); dtr = -dtr; }
        UtoA((unsigned)dtr, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    }
    LogLine(buf);
}

/* ---------------- FPS 钉住绘制 (Render 尾, Ares 之后) ----------------
 * 本帧 Simple_ 收到的计数器串, 以固定字号画到固定位 (x=0, 底上 32,
 * 与 Ares 同锚点), 任何档位都是 1x。只画窗口面, 大面跳过。 */
static void VecFpsPinDraw(void)
{
    void* comp;
    int cw, chh, pitch, w, y, adv;
    unsigned char* locked;
    RectangleStruct rc;
    ColorStruct black;
    if (!s_FpsHave || !g_VT.Render) { s_FpsHave = 0; return; }
    s_FpsHave = 0;
    if (s_FpsN <= 0) return;
    if (!GdiEnsure(FPS_PX, 0, FPS_SLOT, g_VTFace)) return;
    comp = *(void**)ADDR_SURF_COMPOSITE;
    if (!comp || !IsPlausiblePtr(comp) || SB_IsBigSurface(comp)) return;
    cw = GtSurfGet(comp, GtVT_GET_WIDTH);
    chh = GtSurfGet(comp, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(comp, GtVT_GET_PITCH);
    if (cw < 64 || chh < 64 || pitch < cw * 2) return;
    Calibrate();
    w = RenderLine(s_FpsBuf, s_FpsN);
    if (w <= 0) return;
    adv = g_G->adv;
    y = chh - adv - 32;
    if (y < 0) y = 0;
    rc.X = 0; rc.Y = y; rc.Width = w + 2; rc.Height = adv;
    black.R = 0; black.G = 0; black.B = 0;
    GtFillRectTrans(comp, &rc, &black, 100);
    locked = GtLock(comp);
    if (!locked) return;
    BlitCoverage(locked, pitch, cw, chh, 0, y, w + 2, adv,
                 255, 255, 255, 100);
    GtUnlock(comp);
}

static void FpsPinC(void)
{
    VecFpsPinDraw();
}

static void* g_CallFpsPin = (void*)FpsPinC;
__attribute__((naked)) void VecFpsPinEntry(void)
{
    __asm__ volatile (
        "pushal\n\t"
        "pushfl\n\t"
        "call *%0\n\t"
        "popfl\n\t"
        "popal\n\t"
        "jmp *%1\n\t"
        :
        : "m" (g_CallFpsPin), "m" (g_FpsPinTramp)
        : "memory", "cc");
}

void VecText_Install(void)
{
    VecText_EnsureInit();
    if (!g_Log) return;
    /* Enable 只门控文本五钩; 以下共用钩无条件装 (缩放/底电等地基)。 */
    if (g_VT.Enable)
    {
        if (!BytesSame((unsigned char*)ADDR_BLIT, kWantBlit, 7))
        {
            LogLine("vtext: blit bytes mismatch, hook OFF");
            LogBytes((unsigned char*)ADDR_BLIT);
        }
        else if (!BuildTramp()) LogLine("vtext: tramp fail, hook OFF");
        else if (!PatchBlit()) LogLine("vtext: patch fail, hook OFF");
        else LogLine(g_VT.Render ? "vtext: blit hook ON (render)"
                                 : "vtext: blit hook ON (probe)");
        /* DrawText/Print 上下文钩 (只记调用者)。 */
    if (BytesSame((unsigned char*)ADDR_DRAWTEXT, kWantDrawText, 6))
    {
        g_OrigDrawText = (DrawTextFn)BuildTrampAt(ADDR_DRAWTEXT, 6, ADDR_DRAWTEXT_BACK);
        if (g_OrigDrawText && PatchAt(ADDR_DRAWTEXT, (void*)(size_t)VecDrawTextEntry, 6))
            LogLine("vtext: drawtext ctx hook ON");
        else { LogLine("vtext: drawtext ctx hook OFF"); g_OrigDrawText = NULL; }
    }
    else { LogLine("vtext: drawtext bytes mismatch"); LogBytes((unsigned char*)ADDR_DRAWTEXT); }
    if (BytesSame((unsigned char*)ADDR_PRINT, kWantPrint, 10))
    {
        g_OrigPrint = (PrintFn)BuildTrampAt(ADDR_PRINT, 10, ADDR_PRINT_BACK);
        if (g_OrigPrint && PatchAt(ADDR_PRINT, (void*)(size_t)VecPrintEntry, 10))
            LogLine("vtext: print ctx hook ON");
        else { LogLine("vtext: print ctx hook OFF"); g_OrigPrint = NULL; }
    }
    else { LogLine("vtext: print bytes mismatch"); LogBytes((unsigned char*)ADDR_PRINT); }
    /* Simple_ 收集钩 (全文本必经, 命中跳过): 字节对不上就不装 */
    if (BytesSame((unsigned char*)ADDR_SIMPLE, kWantSimple, 7))
    {
        g_OrigSimple = (SimpleFn)BuildTrampAt(ADDR_SIMPLE, 7, ADDR_SIMPLE_BACK);
        if (g_OrigSimple && PatchAt(ADDR_SIMPLE, (void*)(size_t)VecSimpleEntry, 7))
            LogLine("vtext: simple hook ON");
        else { LogLine("vtext: simple hook OFF"); g_OrigSimple = NULL; }
    }
    else { LogLine("vtext: simple bytes mismatch"); LogBytes((unsigned char*)ADDR_SIMPLE); }
    /* GetDim 宽度接管钩 (引擎算字宽唯一入口, 一处替换处处生效) */
    if (BytesSame((unsigned char*)ADDR_GETDIM, kWantGetDim, 6))
    {
        g_OrigGetDim = (GetDimFn)BuildTrampAt(ADDR_GETDIM, 6, ADDR_GETDIM_BACK);
        if (g_OrigGetDim && PatchAt(ADDR_GETDIM, (void*)(size_t)VecGetDimEntry, 6))
            LogLine("vtext: getdim hook ON");
        else { LogLine("vtext: getdim hook OFF"); g_OrigGetDim = NULL; }
    }
    else { LogLine("vtext: getdim bytes mismatch"); LogBytes((unsigned char*)ADDR_GETDIM); }
    /* FPS 钉住钩 (0x4F4589, Render 尾 Ares 之后): 计数器固定位 1x。没装上就看原版。 */
    {
        static const unsigned char kWantFpsPin[5] = { 0xA1, 0x1C, 0x73, 0x88, 0x00 };
        if (BytesSame((unsigned char*)0x4F4589u, kWantFpsPin, 5))
        {
            g_FpsPinTramp = BuildTrampAt(0x4F4589u, 5, 0x4F458Eu);
            if (!g_FpsPinTramp || !PatchAt(0x4F4589u, (void*)(size_t)VecFpsPinEntry, 5))
                g_FpsPinTramp = NULL;
        }
    }
    }
    else LogLine("vtext: text hooks OFF (Enable=0)");
    /* 以下共用钩与 Enable 无关。 */
    {
        static const unsigned char kWantPrepare[5] = { 0xA1, 0x24, 0x73, 0x88, 0x00 };
        if (BytesSame((unsigned char*)0x4F42F0u, kWantPrepare, 5))
        {
            g_PrepareTramp = BuildTrampAt(0x4F42F0u, 5, 0x4F42F5u);
            if (g_PrepareTramp && PatchAt(0x4F42F0u, (void*)MOExperience_PrepareEntry, 5))
                LogLine("vtext: prepare hook ON");
            else { LogLine("vtext: prepare hook OFF"); g_PrepareTramp = NULL; }
        }
        else { LogLine("vtext: prepare bytes mismatch"); LogBytes((unsigned char*)0x4F42F0u); }
    }
    /* 上屏入口钩。 */
    {
        static const unsigned char kWantBlitSb[6] = { 0x83, 0xEC, 0x38, 0x53, 0x55, 0x56 };
        if (BytesSame((unsigned char*)0x6A70E0u, kWantBlitSb, 6))
        {
            g_BlitSbTramp = BuildTrampAt(0x6A70E0u, 6, 0x6A70E6u);
            if (g_BlitSbTramp && PatchAt(0x6A70E0u, (void*)MOExperience_BlitSbEntry, 6))
                LogLine("vtext: blitsb hook ON");
            else { LogLine("vtext: blitsb hook OFF"); g_BlitSbTramp = NULL; }
        }
        else { LogLine("vtext: blitsb bytes mismatch"); LogBytes((unsigned char*)0x6A70E0u); }
    }
    /* 放大前提示框绘制改直跑。 */
    {
        static const unsigned char kWantTipPre[7] =
            { 0x8B, 0x11, 0x6A, 0x01, 0xFF, 0x52, 0x0C };
        if (BytesSame((unsigned char*)0x6A7089u, kWantTipPre, 7))
        {
            g_TipPreDrawTramp = BuildTrampAt(0x6A7089u, 7, 0x6A7090u);
            if (g_TipPreDrawTramp && PatchAt(0x6A7089u, (void*)MOExperience_TipPreDrawEntry, 7))
                LogLine("vtext: tip-pre hook ON");
            else { LogLine("vtext: tip-pre hook OFF"); g_TipPreDrawTramp = NULL; }
        }
        else { LogLine("vtext: tip-pre bytes mismatch"); LogBytes((unsigned char*)0x6A7089u); }
    }
    /* 提示框矩形改指 A 影子。 */
        {
            unsigned char* p;
            DWORD old;
            unsigned i, n;
            int bad = -1;
            unsigned abase = (unsigned)(size_t)SB_AViewBlock();
            static const struct {
                unsigned at; unsigned imm; unsigned off; unsigned oplen;
                unsigned char op[2];
            } rep[] =
            {
                { 0x478BD1u, 2u, 0u, 2u, { 0x8B, 0x15 } },  /* mov edx,[A.X]     */
                { 0x478BD7u, 1u, 8u, 1u, { 0xA1, 0x00 } },  /* mov eax,[A.Width] */
                { 0x478C17u, 1u, 0u, 1u, { 0xBF, 0x00 } },  /* mov edi,A         */
                { 0x478E4Au, 2u, 0u, 2u, { 0x8B, 0x35 } },  /* mov esi,[A.X]     */
                { 0x478E50u, 2u, 8u, 2u, { 0x8B, 0x1D } }   /* mov ebx,[A.Width] */
            };
            n = (unsigned)(sizeof(rep) / sizeof(rep[0]));
            /* 先全验再改。 */
            for (i = 0; i < n; i++)
            {
                p = (unsigned char*)rep[i].at;
                if (p[0] != rep[i].op[0]) { bad = (int)i; break; }
                if (rep[i].oplen > 1 && p[1] != rep[i].op[1]) { bad = (int)i; break; }
            }
            if (bad < 0)
            {
                for (i = 0; i < n; i++)
                {
                    unsigned imm = rep[i].at + rep[i].imm;
                    if (!VirtualProtect((void*)imm, 4, PAGE_EXECUTE_READWRITE, &old))
                    { bad = (int)i; break; }
                    *(unsigned*)(size_t)imm = abase + rep[i].off;
                    VirtualProtect((void*)imm, 4, old, &old);
                    FlushInstructionCache(GetCurrentProcess(), (void*)imm, 4);
                }
            }
            if (bad < 0)
                LogLine("vtext: tooltip surface rect -> A shadow");
            else
            {
                char b1[16], buf[96];
                unsigned q = 0;
                q = AppendCStr(buf, q, "vtext: tooltip rect patch OFF at=",
                                sizeof(buf));
                UtoA(rep[bad].at, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
                LogLine(buf);
            }
    }
    /* UpdatePrimarySurface 两调用点字节被 Ares 改写过, 不挂功能钩, 只监听补丁存活。 */
    /* 侧栏列纠正钩 (InitGUI 一次性布局须先纠, FM_Poll 每循环纠来不及)。 */
    {
        static const unsigned char kWantInitGui[5] = { 0x83, 0xEC, 0x24, 0x53, 0x55 };
        if (BytesSame((unsigned char*)0x6ABD30u, kWantInitGui, 5))
        {
            g_InitGuiTramp = BuildTrampAt(0x6ABD30u, 5, 0x6ABD35u);
            if (g_InitGuiTramp && PatchAt(0x6ABD30u, (void*)MOExperience_InitGuiEntry, 5))
                LogLine("vtext: initgui hook ON");
            else { LogLine("vtext: initgui hook OFF"); g_InitGuiTramp = NULL; }
        }
        else { LogLine("vtext: initgui bytes mismatch"); LogBytes((unsigned char*)0x6ABD30u); }
    }
    /* 视口 gadget 输入钩 (命中链入口, FullMap 在此收窄横跨视口的容器)。 */
    {
        static const unsigned char kWantGadgetInput[6] =
            { 0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56 };
        if (BytesSame((unsigned char*)0x4E1640u, kWantGadgetInput, 6))
        {
            g_GadgetInputTramp = BuildTrampAt(0x4E1640u, 6, 0x4E1646u);
            if (g_GadgetInputTramp && PatchAt(0x4E1640u, (void*)MOExperience_GadgetInputEntry, 6))
                LogLine("vtext: gadgetinput hook ON");
            else { LogLine("vtext: gadgetinput hook OFF"); g_GadgetInputTramp = NULL; }
        }
        else { LogLine("vtext: gadgetinput bytes mismatch"); LogBytes((unsigned char*)0x4E1640u); }
    }
/* 呈现入口钩 (缩放模块在这条路之后做放大呈现)。 */
    {
        static const unsigned char kWantUpdate[7] = { 0x83, 0xEC, 0x5C, 0x8B, 0x44, 0x24, 0x60 };
        if (BytesSame((unsigned char*)0x4F4780u, kWantUpdate, 7))
        {
            g_UpdateTramp = BuildTrampAt(0x4F4780u, 7, 0x4F4787u);
            if (g_UpdateTramp && PatchAt(0x4F4780u, (void*)MOExperience_UpdateEntry, 7))
                LogLine("vtext: update hook ON");
            else { LogLine("vtext: update hook OFF"); g_UpdateTramp = NULL; }
        }
    }
/* 缩放 (缩小档): GScreenClass::Render (0x4F4480) 入口, 首8B = 83 EC 08 A1 1C 73
     * 88 00 (sub esp,8 / mov eax,[Composite])。缩小档 (scale>1) 时把 [0x88731C]/
     * [0x8872FC] 临时换成 SB 自建的 R 大离屏面; 渲染完消息帧钩还原 + 降采样。
     * 安装见 MOExperience (MOExperience_RenderEntry/g_RenderTramp)。 */
    {
        static const unsigned char kWantRender[8] =
            { 0x83, 0xEC, 0x08, 0xA1, 0x1C, 0x73, 0x88, 0x00 };
        if (BytesSame((unsigned char*)0x4F4480u, kWantRender, 8))
        {
            g_RenderTramp = BuildTrampAt(0x4F4480u, 8, 0x4F4488u);
            if (g_RenderTramp && PatchAt(0x4F4480u, (void*)MOExperience_RenderEntry, 8))
                LogLine("vtext: render hook ON");
            else { LogLine("vtext: render hook OFF"); g_RenderTramp = NULL; }
        }
        else { LogLine("vtext: render bytes mismatch"); LogBytes((unsigned char*)0x4F4480u); }
    }
/* Render 里两条 (0x4F4492 让出 / 0x4F44CB 装回): 大面暴露窗口只剩三趟地图。 */
    {
        static const unsigned char kWantPreCur[5] = { 0xA3, 0x14, 0x73, 0x88, 0x00 };
        if (BytesSame((unsigned char*)0x4F4492u, kWantPreCur, 5))
        {
            g_PreCurTramp = BuildTrampAt(0x4F4492u, 5, 0x4F4497u);
            if (g_PreCurTramp && PatchAt(0x4F4492u, (void*)MOExperience_PreCurEntry, 5))
                LogLine("vtext: precur hook ON");
            else { LogLine("vtext: precur hook OFF"); g_PreCurTramp = NULL; }
        }
        else { LogLine("vtext: precur bytes mismatch"); LogBytes((unsigned char*)0x4F4492u); }
    }
    {
        static const unsigned char kWantMap[5] = { 0xA1, 0x1C, 0x73, 0x88, 0x00 };
        if (BytesSame((unsigned char*)0x4F44CBu, kWantMap, 5))
        {
            g_MapTramp = BuildTrampAt(0x4F44CBu, 5, 0x4F44D0u);
            if (g_MapTramp && PatchAt(0x4F44CBu, (void*)MOExperience_MapEntry, 5))
                LogLine("vtext: mapentry hook ON");
            else { LogLine("vtext: mapentry hook OFF"); g_MapTramp = NULL; }
        }
        else { LogLine("vtext: mapentry bytes mismatch"); LogBytes((unsigned char*)0x4F44CBu); }
    }
/* chrome pass 那趟 (窗口坐标画): 0x4F44FD 让出, 0x4F4505 装回。 */
    {
        static const unsigned char kWantChromePre[5] =
            { 0x8B, 0x16, 0x50, 0x8B, 0xCE };
        if (BytesSame((unsigned char*)0x4F44FDu, kWantChromePre, 5))
        {
            g_ChromePreTramp = BuildTrampAt(0x4F44FDu, 5, 0x4F4502u);
            if (g_ChromePreTramp && PatchAt(0x4F44FDu, (void*)MOExperience_ChromePreEntry, 5))
                LogLine("vtext: chrome-pre hook ON");
            else { LogLine("vtext: chrome-pre hook OFF"); g_ChromePreTramp = NULL; }
        }
        else { LogLine("vtext: chrome-pre bytes mismatch"); LogBytes((unsigned char*)0x4F44FDu); }
    }
    {
        static const unsigned char kWantChromePost[6] =
            { 0x8B, 0x0D, 0x1C, 0x73, 0x88, 0x00 };
        if (BytesSame((unsigned char*)0x4F4505u, kWantChromePost, 6))
        {
            g_ChromePostTramp = BuildTrampAt(0x4F4505u, 6, 0x4F450Bu);
            if (g_ChromePostTramp && PatchAt(0x4F4505u, (void*)MOExperience_ChromePostEntry, 6))
                LogLine("vtext: chrome-post hook ON");
            else { LogLine("vtext: chrome-post hook OFF"); g_ChromePostTramp = NULL; }
        }
        else { LogLine("vtext: chrome-post bytes mismatch"); LogBytes((unsigned char*)0x4F4505u); }
    }
/* 三趟地图后、引擎 UI 前还原 + 降采样 (0x4F451B)。 */
    {
        static const unsigned char kWantAfterMap[5] = { 0xA0, 0x19, 0xB5, 0xB0, 0x00 };
        if (BytesSame((unsigned char*)0x4F451Bu, kWantAfterMap, 5))
        {
            g_AfterMapTramp = BuildTrampAt(0x4F451Bu, 5, 0x4F4520u);
            if (g_AfterMapTramp && PatchAt(0x4F451Bu, (void*)MOExperience_AfterMapEntry, 5))
                LogLine("vtext: aftermap hook ON");
            else { LogLine("vtext: aftermap hook OFF"); g_AfterMapTramp = NULL; }
        }
        else { LogLine("vtext: aftermap bytes mismatch"); LogBytes((unsigned char*)0x4F451Bu); }
    }
/* 边缘卷屏入口: 点换回物理鼠标 (0x692B60)。 */
    {
        static const unsigned char kWantEdgeFix[7] =
            { 0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x57 };
        if (BytesSame((unsigned char*)0x692B60u, kWantEdgeFix, 7))
        {
            g_EdgeFixTramp = BuildTrampAt(0x692B60u, 7, 0x692B67u);
            if (g_EdgeFixTramp && PatchAt(0x692B60u, (void*)MOExperience_EdgeFixEntry, 7))
                LogLine("vtext: edgefix hook ON");
            else { LogLine("vtext: edgefix hook OFF"); g_EdgeFixTramp = NULL; }
        }
        else { LogLine("vtext: edgefix bytes mismatch"); LogBytes((unsigned char*)0x692B60u); }
    }
/* ProcessClickCoords 入口: 视口外的点拒掉 (该函数无上界判定, 见 .cpp)。 */
    {
        static const unsigned char kWantClickCoords[7] =
            { 0x8B, 0x44, 0x24, 0x10, 0x83, 0xEC, 0x18 };
        if (BytesSame((unsigned char*)0x692300u, kWantClickCoords, 7))
        {
            g_ClickCoordsTramp = BuildTrampAt(0x692300u, 7, 0x692307u);
            if (g_ClickCoordsTramp && PatchAt(0x692300u, (void*)MOExperience_ClickCoordsEntry, 7))
                LogLine("vtext: clickcoords hook ON");
            else { LogLine("vtext: clickcoords hook OFF"); g_ClickCoordsTramp = NULL; }
        }
        else { LogLine("vtext: clickcoords bytes mismatch"); LogBytes((unsigned char*)0x692300u); }
    }
/* Clamp_Center 整条替换原版、不回调, 只在缩放启用时装; 不装即原版钳制。 */
    if (!SB_Enabled())
    {
        LogLine("vtext: clampcenter hook OFF (scale off -> vanilla clamp)");
    }
    else
    {
        static const unsigned char kWantClamp[5] =
            { 0xA1, 0xE4, 0xF8, 0x87, 0x00 };
        if (BytesSame((unsigned char*)0x6D8640u, kWantClamp, 5))
        {
            if (PatchAt(0x6D8640u, (void*)MOExperience_ClampCenterEntry, 5))
                LogLine("vtext: clampcenter hook ON");
            else LogLine("vtext: clampcenter hook OFF");
        }
        else { LogLine("vtext: clampcenter bytes mismatch"); LogBytes((unsigned char*)0x6D8640u); }
    }
/* 通用拉伸 blit 守卫钩 (0x6BA580, cameo/图标越界写拦)。 */
    {
        static const unsigned char kWantBlitGuard[5] =
            { 0x83, 0xEC, 0x14, 0x55, 0x56 };
        if (BytesSame((unsigned char*)0x6BA580u, kWantBlitGuard, 5))
        {
            g_BlitGuardTramp = BuildTrampAt(0x6BA580u, 5, 0x6BA585u);
            if (g_BlitGuardTramp && PatchAt(0x6BA580u, (void*)MOExperience_BlitGuardEntry, 5))
                LogLine("vtext: blitguard hook ON");
            else { LogLine("vtext: blitguard hook OFF"); g_BlitGuardTramp = NULL; }
        }
        else { LogLine("vtext: blitguard bytes mismatch"); LogBytes((unsigned char*)0x6BA580u); }
    }
/* 迷雾 DrawPixelEffects 越界写护栏 (挂函数入口, 不挂 E8 调用点)。 */
    {
        static const unsigned char kWantShroud[5] = { 0x83, 0xEC, 0x48, 0x56, 0x57 };
        if (BytesSame((unsigned char*)0x6D7840u, kWantShroud, 5))
        {
            g_ShroudTramp = BuildTrampAt(0x6D7840u, 5, 0x6D7845u);
            if (g_ShroudTramp && PatchAt(0x6D7840u, (void*)MOExperience_ShroudEntry, 5))
                LogLine("sbar: shroudguard hook ON");
            else { LogLine("sbar: shroudguard hook OFF"); g_ShroudTramp = NULL; }
        }
        else { LogLine("sbar: shroudguard bytes mismatch"); LogBytes((unsigned char*)0x6D7840u); }
    }
/* 鼠标折算在源头做: 换虚表 +0x34, 11 个消费点一次覆盖 (详见 .cpp)。 */
    MOE_InstallMousePosHook();
/* 鼠标按钮消息路 (0x6930A0): 第 4 实参指针换折算副本 (不过 WWMouse, 详见 .cpp)。 */
    {
        static const unsigned char kWantWndMouse[8] =
            { 0xA0, 0x78, 0xE3, 0xA8, 0x00, 0x83, 0xEC, 0x1C };
        if (BytesSame((unsigned char*)0x6930A0u, kWantWndMouse, 8))
        {
            g_WndMouseTramp = BuildTrampAt(0x6930A0u, 8, 0x6930A8u);
            if (g_WndMouseTramp && PatchAt(0x6930A0u, (void*)MOExperience_WndMouseEntry, 8))
                LogLine("vtext: wndmouse hook ON");
            else { LogLine("vtext: wndmouse hook OFF"); g_WndMouseTramp = NULL; }
        }
        else { LogLine("vtext: wndmouse bytes mismatch"); LogBytes((unsigned char*)0x6930A0u); }
    }
}
