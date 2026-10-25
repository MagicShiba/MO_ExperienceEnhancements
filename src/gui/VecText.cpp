/* VecText.cpp - 全局文本矢量接管 (简报/按钮/顶栏数字等)。
 * 只换字形光栅, 度量/换行/对齐走引擎。异常回落原版。
 * BitFont 布局: +0x0C 像素缓冲, +0x10 pitch, +0x24 默认色, +0x30..0x3C 裁剪。
 */
#include <windows.h>
#include <stdarg.h>             /* LogV 的变参 (编译器内建, 不依赖 CRT) */
#include "GdiText.h"
#include "VecText.h"
#include "OrderUtil.h"
#include "Icon.h"
#include "FullMap.h"
#include "ScaleBar.h"
#include "PiP.h"
#include "MOExperience.h"

#define VC_MAXW 32
#define VC_MAXH 32
#define VBUF    192             /* 日志行缓冲长度 (LogV 用) */

/* 单行多字段日志: tag 之后键值交替, 末尾传 NULL。键自带分隔符, 首字节:
 *   '#' = 值按十六进制 (不带 0x, 同 HtoA)   '-' = 值按有符号 (自带负号)
 *   LogV("vfb:", " wch=", 0x4E00, " x=", 300, NULL);
 * 输出与手写 "AppendCStr + UtoA" 逐字节一致; 装不下就截断, 不越界。 */
static void LogV(const char* tag, ...)
{
    va_list ap;
    const char* k;
    char b[VBUF], n[16];
    unsigned q = AppendCStr(b, 0, tag, VBUF);
    va_start(ap, tag);
    while ((k = va_arg(ap, const char*)) != NULL)
    {
        int v = va_arg(ap, int);
        int hex = (*k == '#'), sgn = (*k == '-');
        if (hex || sgn) k++;
        q = AppendCStr(b, q, k, VBUF);
        if (sgn) q = AppendInt(b, q, VBUF, v);
        else
        {
            if (hex) HtoA((unsigned)v, n);
            else     UtoA((unsigned)v, n);
            q = AppendCStr(b, q, n, VBUF);
        }
    }
    va_end(ap);
    LogLine(b);
}

/* 后定义的前置声明 */
static int VecPickPx(void* t);
/* 要接管的入口 (跳板落点 = 地址 + 覆盖长度, 由表算出, 不再单独写 _BACK) */
#define ADDR_BLIT        0x00434120u
#define ADDR_BLIT_BACK   0x00434127u
#define ADDR_GETDIM      0x00433CF0u
#define ADDR_DRAWTEXT    0x00434CD0u
#define ADDR_PRINT       0x00434B90u
#define ADDR_SIMPLE      0x004A5EB0u

/* Blit 头 7 字节, 下钩前校验 (其余钩的原版字节见下面的 kW_* 表) */
static const unsigned char kWantBlit[7] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x57 };

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
/* 最近一次字形未命中的原因码 (1=首次 2=字符 3=idata 4=px 5=adv 6=h 7=nat)。
 * 只有 Perf=1 时更新, 由 Perf_Frame 打到 vwhy=。 */
int g_VecMissWhy = 0;

static unsigned PfQpc(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned)c.LowPart;
}

static void PfTick(void)
{
    unsigned now = GetTickCount(), ms;
    if (!g_PfOn) return;
    if (!g_PfLastLog) { g_PfLastLog = now; return; }
    if (now - g_PfLastLog < 2000) return;
    ms = now - g_PfLastLog;
    g_PfLastLog = now;
    LogV("perf:", " ms=", ms, " glyph=", g_PfGlyph, " miss=", g_PfMiss,
         " gdHit=", g_PfGdHit, " gdMiss=", g_PfGdMiss, " gdUs=", g_PfGdUs,
         " gpus=", ms ? g_PfGlyph * 1000u / ms : 0, NULL);
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
/* 结算界面打开期间 (>0): 遭遇战积分 0x108 / 任务积分 0x103 模态对话框。
 * 这两屏用全局字体逐字排版、列宽很窄, 矢量字形会撑开/错位; 打开期间直接
 * 整条回落原版 (Blit/GetDim 都放行), 关闭即恢复。由显示函数钩置旗。 */
static int g_ScoreScreen = 0;

/* ---------------- 简报正文自绘 ---------------- */
#define BRIEF_BOTTOM_MAX 1000
/* 字形光栅原点 (DIB 内), 左右留边距, 纵轴用 TA_TOP。 */
#define GL_M 8
#define GL_Y0 8

/* 点阵字格高, 行距下限的兜底。 */
static int g_BriefOrigH = 0;
/* 字体自然行高 (GdiSlot.cell), 行距下限用 */
static int VecRefTextH(void);

/* 行距 = max(点阵格高, 字号 x BriefLine%, 字体自然行高 tmHeight)。
 * 最后一项兜底: 行高小于一行字的高度, 相邻两行就会咬在一起。 */
static int VecBriefLineH(int px)
{
    int h = (g_BriefOrigH > 0) ? g_BriefOrigH : px;
    int cell = VecRefTextH();
    if (g_VT.BriefLine > 0)
    {
        int v = (px * g_VT.BriefLine + 50) / 100;
        if (v > h) h = v;
    }
    if (cell > h) h = cell;
    if (h > VC_MAXH) h = VC_MAXH;
    if (h < 8) h = 8;
    return h;
}

/* 跨钩状态: DrawText 存, Blit 用。引擎那次照跑 (它负责绑面),
 * Blit 抢首字整块自绘, 之后逐字抑制。
 * state: 0=未定 1=已接管 2=不可用。 */
static wchar_t* g_BriefStr = NULL;
static int g_BriefX = 0, g_BriefY = 0, g_BriefW = 0;
static int g_BriefState = 0;
static int g_BriefOwnLogged = 0;
static int g_BriefWhyLogged = 0;

static int VecPickPx(void* t);
static int VecMeasure(void* t, wchar_t wch, int* ph);
/* 简报单字格宽 = 字体前进宽 + 字距 */
static int VecBriefCellW(void* font, wchar_t ch, int px, int tr);
static int VecGdiSlot(int px);
static int VecRender(void* t, wchar_t wch, int adv, int h, int x, int y,
                     int colorW, int px, int yOff);

/* 简报整块自绘: 引擎排版绑死 fnt 度量, 行高/字宽/换行互相牵制, 故整块自绘:
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
    if (g_BriefWhyLogged) return;
    g_BriefWhyLogged = 1;
    LogV("brief: own skip why=", "", why, " ", v, NULL);
}

/* 单字格宽 = 字体前进宽 (ABC 全宽) + 字距, 与 GDI/DirectWrite 同规则。
 * 不用统一格宽或引擎 fnt 宽度表, 否则 CJK 混排间距不均。 */
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

/* 简报正文整块自绘: 行高跟 BriefLine, 字距跟 BriefTrack, 换行按墨水宽。
 * 返回 0=已接管, -1=走引擎。 */
static int VecBriefBody(void* font, const wchar_t* s, int x, int y, int w)
{
    int px, lh, cell, tr, adv, color, cx, cy, lines, seg, per, want, oldB, n, oa;
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
    /* GDI 面先备好, 与字形光栅同槽 (行高下限要读它的 tmHeight)。 */
    if (!GdiEnsure(px, g_VT.Bold, VecGdiSlot(px), g_VTFace)) { VecBriefWhy(9, 0); return -1; }
    tr = g_VT.BriefTrack;
    if (tr < 0) tr = 0;
    if (tr > VC_MAXW - px) tr = VC_MAXW - px;
    adv = px + tr;                       /* 全角格宽 = 字号 + 字距 */
    per = w / adv;
    if (per < 1) { VecBriefWhy(8, per); return -1; }
    maxY = VecBriefMaxY();
    if (maxY < 0) { VecBriefWhy(7, 0); return -1; }
    cell = VecRefTextH();
    lh = VecBriefLineH(px);
    color = (int)(*(unsigned short*)((char*)font + 0x24));

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
        /* BriefAdv=1: 字体前进宽 + 字距; 0: 统一字号 + 字距 (cw 初值就是 adv)。 */
        oa = GtCharAdv(*p);
        if (g_VT.BriefAdv && oa > 0)
            cw = VecBriefCellW(font, *p, px, tr);
        if (cw > VC_MAXW) cw = VC_MAXW;
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
        g_BriefOwnLogged = 1;
        LogV("brief: own draw box=", "", x, ",", y, " ", w, " ", lh,
             "px/char ", adv, " lines=", lines, " cell=", cell,
             " pitch=", pitchW, " maxY=", maxY, NULL);
    }
    return 0;
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

/* 超武串首尾补白 (半角空格/制表/全角空格): 引擎名串尾、计时器串首常带填充,
 * 右/左对齐时正好落在名与计时中间缝里, Gap 再小也盖不掉。收表时统一剥掉。 */
static int VecSWBlank(wchar_t c)
{
    unsigned u = (unsigned)c & 0xFFFFu;
    return (u == 0x20u || u == 0x09u || u == 0x3000u) ? 1 : 0;
}

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
    /* 剥首尾补白 (见 VecSWBlank): 全空串照常收 (只拦底色条, 不拦正文)。 */
    {
        int a = 0, b = n;
        while (a < b && VecSWBlank(s[a])) a++;
        while (b > a && VecSWBlank(s[b - 1])) b--;
        s += a; n = b - a;
    }
    for (i = 0; i < n; i++) r->s[i] = s[i];
    r->s[n] = 0;
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

/* ---------------- 结算界面显示钩 ----------------
 * 展示函数是模态的: 0x5C9720 (遭遇战积分/多人积分, 对话框 0x108) 与
 * 0x6C8E80 (任务积分, 对话框 0x103) 内部跑游戏帧循环, 直到窗口关闭才返回。
 * 期间置 g_ScoreScreen, 关闭即清。
 * 另: 任务结算对话框 (0x103) 上挂一个“退出游戏”按钮, 见 VecScoreDlgEntry。
 * 按钮只置 0x60D380 的模态退出标志, 等展示函数返回、帧循环收尾后再退出。 */
typedef int (__fastcall *ShowDlgFn)(void* thisptr, void* edx);
static ShowDlgFn g_OrigMPScoreShow = NULL;
static ShowDlgFn g_OrigStandaloneShow = NULL;

/* 任务结算对话框过程 0x6C8FF0: 标准 DLGPROC (stdcall, ret 0x10)。 */
typedef INT_PTR (__stdcall *DlgProcFn)(HWND, UINT, WPARAM, LPARAM);
static DlgProcFn g_OrigScoreDlgProc = NULL;
static int g_ExitPending = 0;

/* 引擎退出例程: 0x6BEC60 收尾 (关主窗/清资源), 0x6BEC50(1) 走 CRT exit。 */
static void VecExitGameNow(void)
{
    typedef void (__cdecl *ShutdownFn)(void);
    typedef void (__fastcall *CrtExitFn)(int);
    ((ShutdownFn)0x6BEC60u)();
    ((CrtExitFn)0x6BEC50u)(1);
}

/* 0xAC1B00: HWND -> ownerdraw 元素表。表项 { HWND, elem, ..., next@+0x204 }，
 * elem 即 OwnerDrawDialogElement。只读查找，找不到返回 NULL。 */
static unsigned* VecOwnerDrawElem(HWND h)
{
    void** buckets;
    unsigned log2, n, i, guard;
    if (!h) return NULL;
    buckets = *(void***)0xAC1B00u;
    log2 = *(unsigned*)0xAC1B0Cu;
    if (!buckets || log2 > 14) return NULL;
    n = 1u << log2;
    for (i = 0; i < n; ++i)
    {
        unsigned char* e = (unsigned char*)buckets[i];
        for (guard = 0; e && guard < 4096; ++guard)
        {
            if (*(HWND*)e == h) return (unsigned*)(e + 4);
            e = *(unsigned char**)(e + 0x204);
        }
    }
    return NULL;
}

static INT_PTR __stdcall VecScoreDlgEntry(HWND hDlg, UINT msg,
                                          WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG)
    {
        INT_PTR r;
        HWND cont = NULL;
        HWND b = NULL;
        if (!g_OrigScoreDlgProc) return FALSE;
        /* 必须在原版 WM_INITDIALOG 前建: 才能被 0x60F9A0 挂上
         * 0x610CA0 子类并走 0x612B70 原版自绘。 */
        if (g_OrigStandaloneShow)
        {
            RECT rc;
            cont = GetDlgItem(hDlg, 0x423);   /* 原版“继续” */
            if (cont && !GetDlgItem(hDlg, 0x424) && GetWindowRect(cont, &rc))
            {
                MapWindowPoints(NULL, hDlg, (POINT*)&rc, 2);
                /* 文本是 CSF 键, 与原版“继续”模板里的 GUI:Continue 同机制;
                 * 0x60F9A0/0x612B70 会走 StringTable 按游戏语言加载。 */
                b = CreateWindowExW(0, L"Button", L"GUI:ExitGame",
                                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                    rc.left,
                                    rc.top - (rc.bottom - rc.top) - 7,
                                    rc.right - rc.left, rc.bottom - rc.top,
                                    hDlg, (HMENU)(INT_PTR)0x424,
                                    GetModuleHandleW(NULL), NULL);
            }
        }
        r = g_OrigScoreDlgProc(hDlg, msg, wp, lp);
        /* 初始化后方可确认“继续”最终位置/字体; 按它再校正一次。 */
        if (b && cont)
        {
            RECT cr;
            HFONT f;
            if (GetWindowRect(cont, &cr))
            {
                MapWindowPoints(NULL, hDlg, (POINT*)&cr, 2);
                MoveWindow(b, cr.left, cr.top - (cr.bottom - cr.top) - 7,
                           cr.right - cr.left, cr.bottom - cr.top, TRUE);
            }
            f = (HFONT)SendMessageW(cont, WM_GETFONT, 0, 0);
            if (f) SendMessageW(b, WM_SETFONT, (WPARAM)f, TRUE);
            /* +0xB0: 复用“继续”的按钮底图 band。 */
            {
                unsigned* ec = VecOwnerDrawElem(cont);
                unsigned* eb = VecOwnerDrawElem(b);
                if (ec && eb)
                {
                    *(unsigned*)((char*)eb + 0xB0) =
                        *(unsigned*)((char*)ec + 0xB0);
                    InvalidateRect(b, NULL, TRUE);
                }
            }
        }
        return r;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == 0x424 &&
        HIWORD(wp) == BN_CLICKED)
    {
        /* DWL_USER = 0x60D380 的局部标志; 置 1 让模态帧循环退出。
         * 真正退出放到 0x6C8E80 返回后 (见 VecStandaloneShowEntry)。 */
        int* flag = (int*)GetWindowLongA(hDlg, 8);
        if (flag) *flag = 1;
        g_ExitPending = 1;
        return TRUE;
    }
    if (!g_OrigScoreDlgProc) return FALSE;
    return g_OrigScoreDlgProc(hDlg, msg, wp, lp);
}

static void __fastcall VecMPScoreShowEntry(void* thisptr, void* edx)
{
    g_ScoreScreen++;
    if (g_OrigMPScoreShow) g_OrigMPScoreShow(thisptr, edx);
    g_ScoreScreen--;
}

static void __fastcall VecStandaloneShowEntry(void* thisptr, void* edx)
{
    g_ScoreScreen++;
    if (g_OrigStandaloneShow) g_OrigStandaloneShow(thisptr, edx);
    g_ScoreScreen--;
    if (g_ExitPending)
    {
        g_ExitPending = 0;
        VecExitGameNow();
    }
}

int __fastcall VecDrawTextEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5,
    int a6, int a7, int a8, int a9, int a10)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int brief, r, menuTake = 0;
    /* 结算界面打开期间: 整条回落原版, 不记简报/菜单上下文。 */
    if (g_ScoreScreen)
        return g_OrigDrawText ? g_OrigDrawText(t, NULL, a1, a2, a3, a4, a5,
                                                a6, a7, a8, a9, a10) : 0;
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
        g_BriefX = a4; g_BriefY = a5; g_BriefW = a6;
        g_BriefState = 0;
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

/* 放行给引擎原版那一段单独计时 (区分"我们做的活"与"引擎本来就要做的活")。 */
static int __fastcall VecCallOrigPrint(void* t, int a1, int a2, int a3, int a4,
                                       int a5, int a6, int a7)
{
    long long q = Perf_Now();
    int r = g_OrigPrint ? g_OrigPrint(t, NULL, a1, a2, a3, a4, a5, a6, a7) : 0;
    Perf_Add(PF_H_PRTO, q);
    return r;
}

static int __fastcall VecCallOrigSimple(int ecxIn, int edxIn, int a1, int a2,
                                        int a3, int a4, int a5, int a6, int a7)
{
    long long q = Perf_Now();
    int r = g_OrigSimple(ecxIn, edxIn, a1, a2, a3, a4, a5, a6, a7);
    Perf_Add(PF_H_SMPO, q);
    return r;
}

static int __fastcall VecPrintBody(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7);
int __fastcall VecPrintEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    long long tPf = Perf_Now();
    int r = VecPrintBody(t, e, a1, a2, a3, a4, a5, a6, a7);
    Perf_Add(PF_H_PRINT, tPf);
    return r;
}

static int __fastcall VecPrintBody(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int r;
    if (g_ScoreScreen)
        return VecCallOrigPrint(t, a1, a2, a3, a4, a5, a6, a7);
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
    r = VecCallOrigPrint(t, a1, a2, a3, a4, a5, a6, a7);
    return r;
}

/* 文本槽判定: CJK 首字=0, 前 8 字含冒号=1, 否则 -1。只认 EDX。 */
static int VecSWTextKind(int slot, const wchar_t** out)
{
    const wchar_t* s;
    unsigned c, lim;
    int k;
    if (!IsPlausiblePtr((void*)slot)) return -1;
    /* EDX 是猜出来的指针, 先确认真的可读 (最多看 9 个 wchar)。 */
    lim = MemReadableBytes((void*)(size_t)slot, 18);
    if (lim < 2) return -1;
    s = (const wchar_t*)slot;
    c = (unsigned)s[0] & 0xFFFFu;
    if ((c >= 0x2E80u && c < 0xA000u) ||
        (c >= 0xFF00u && c < 0xFFF0u)) { *out = s; return 0; }
    for (k = 0; k < 8 && (unsigned)(k * 2 + 1) < lim; k++)
    {
        c = (unsigned)s[k] & 0xFFFFu;
        if (!c) break;
        if (c == 0x3Au) { *out = s; return 1; }
    }
    return -1;
}

/* Simple_ 入口: 命中即收表并跳过, 空串只收表。 */
static int __fastcall VecSimpleBody(int ecxIn, int edxIn,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7);
int __fastcall VecSimpleEntry(int ecxIn, int edxIn,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    long long tPf = Perf_Now();
    int r = VecSimpleBody(ecxIn, edxIn, a1, a2, a3, a4, a5, a6, a7);
    Perf_Add(PF_H_SIMPLE, tPf);
    return r;
}

static int __fastcall VecSimpleBody(int ecxIn, int edxIn,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    if (!g_OrigSimple) return 0;
    /* 画中画离屏渲染期间: 一律不画文本 —— 引擎这趟会把超武原始列表/操作提示/战役框
     * 等文字画到小窗离屏面上 (主画布判据在此不成立, 拦不住)。按"已绘制"口径返回
     * (与超武列表隐藏同约定, pRetVal=入参位置), 免得调用方改走别的绘制路径。 */
    if (PiP_InRender())
    {
        if (IsPlausiblePtr((void*)ecxIn))
        {
            int sx = 0, sy = 0;
            if (IsPlausiblePtr((void*)a2))
            { sx = ((int*)(size_t)a2)[0]; sy = ((int*)(size_t)a2)[1]; }
            if (IsPlausiblePtr((void*)a3))
            { sx += ((int*)(size_t)a3)[0]; sy += ((int*)(size_t)a3)[1]; }
            ((int*)ecxIn)[0] = sx;
            ((int*)ecxIn)[1] = sy;
        }
        return ecxIn;
    }
    if (g_ScoreScreen)
        return VecCallOrigSimple(ecxIn, edxIn, a1, a2, a3, a4, a5, a6, a7);
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
    return VecCallOrigSimple(ecxIn, edxIn, a1, a2, a3, a4, a5, a6, a7);
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

/* 野指针兜底日志计数 (最多记 8 条) */
static int s_BadStrN = 0;

static int __fastcall VecGetDimBody(void* t, void* e, const wchar_t* s,
                                    int* w, int* h, int maxW)
{
    int ow = 0, oh = 0, r;
    int px, i, tot, n = 0;
    unsigned lim;
    void* idata;
    (void)e;
    if (!g_OrigGetDim) return 0;
    if (g_GDDepth > 0 || g_ScoreScreen > 0 || !g_VT.Render)
        return g_OrigGetDim(t, e, s, w, h, maxW);
    if (!t || !IsPlausiblePtr(t) || !s || !IsPlausiblePtr((void*)s) ||
        !w)
        return g_OrigGetDim(t, e, s, w, h, maxW);
    /* 野指针兜底: 引擎在部分对话框绘制路径上会传进不可读的 s (实测
     * 0x32AC8010, 读第一个 wchar 就页错误 —— 原版函数 0x433D1F 的
     * `mov di,[ebx]` 同一位置也会读它, 转发回去同样是崩)。按空串返回。 */
    lim = MemReadableBytes(s, 512);
    if (lim < 2)
    {
        if (g_Log && s_BadStrN < 8) { s_BadStrN++; LogLine("vtext: getdim bad str, skipped"); }
        *w = 0;
        if (h) *h = 0;
        return 0;
    }
    idata = *(void**)((char*)t + 4);
    for (i = 0; i < 256 && (unsigned)(i * 2 + 1) < lim; i++) { if (!s[i]) break; n++; }
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

/* 度量钩的导出入口: 只做 Perf 记账 (引擎每字都要量一次, 用来确认这条路径的
 * 固定开销)。 */
int __fastcall VecGetDimEntry(void* t, void* e, const wchar_t* s,
                              int* w, int* h, int maxW)
{
    long long tPf = Perf_Now();
    int r = VecGetDimBody(t, e, s, w, h, maxW);
    Perf_Add(PF_VECGETDIM, tPf);
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
    /* 逐字测量: 本字墨水高 + 墨水顶到基线的距离 + 光栅时的字体行度量。 */
    int asc;       /* 墨水顶 -> 基线 */
    int cell;      /* 字体自然行高 (tmHeight) */
    int ascd;      /* 行格内基线偏移 (tmAscent) */
    int fullW;
    int gdiW;
    int off, inkW;
    int nat;
    /* 逐行墨水区间, 渲染只扫有墨区间。 */
    unsigned char rowLo[VC_MAXH], rowHi[VC_MAXH];
    unsigned char cov[VC_MAXW * VC_MAXH];
} VCEnt;

static VCEnt g_VC[VC_MASK + 1];

/* 字体自然行高 (tmHeight); 0 = GDI 未就绪。 */
static int VecRefTextH(void)
{
    if (g_G && g_G->cell >= 8 && g_G->cell <= VC_MAXH) return g_G->cell;
    return 0;
}

/* 原版每字格内垂直居中, 墨水盒不照抄改版 fnt。 */

/* ---------------- 字号校准 ----------------
 * 对格高试渲染取墨水高, 一步收敛。键=(idata,face,bold)。 */

static int g_PxValid = 0;
static void* g_PxIdata = NULL;
static int g_PxPx = 19, g_PxBold = -1, g_PxPxSrc = 0;
static const wchar_t* g_PxFace = NULL;

/* 记下这次校准结果 (键 + 派生量), 下次同键直接命中。 */
static void PxStore(void* idata, int px, int origH, int pxSrc)
{
    g_PxValid = 1; g_PxIdata = idata; g_PxPx = px; g_PxBold = g_VT.Bold;
    g_PxPxSrc = pxSrc; g_FontOrigH = origH; g_BriefOrigH = origH;
    g_PxFace = g_VTFace;
}

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
        PxStore(idata, px, origH, g_VT.FontPx);
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
    PxStore(idata, px, origH, 0);   /* 0 = 自动档 (不进 FontPx 分子) */
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
    /* CJK 的 abcA 可能为负 (字身左出边), 须钳制 off >= 0,
     * 否则墨水越界写入上一行且该行整行不画。 */
    if (off < 0) off = 0;
    if (pg) *pg = gdiW;
    return off;
}

static VCEnt* VecGlyph(void* t, wchar_t wch, int adv, int h, int px)
{
    unsigned idx = ((unsigned)wch) & VC_MASK;
    VCEnt* e = &g_VC[idx];
    void* idata = *(void**)((char*)t + 4);
    int gw, dx, y, L, R, inkW, x, off, gdiW;
    /* 逐字测量: 扫本字自己的墨水盒, 并记它到基线的距离。 */
    int scanW, T, B, base, outH;
    unsigned char* bits;
    int pitch;
    if (!t || !IsPlausiblePtr(t)) { g_VecWhy = 10; return NULL; }
    if (px < 8 || px > 40) { g_VecWhy = 11; return NULL; }
    if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
        e->adv == adv && e->h == h && e->nat == g_VecNat)
    {
        Perf_Hit(PF_VHIT);
        return e;
    }
    if (g_PfOn) g_PfMiss++;   /* 未命中 = 一次 GDI 光栅 + DIB 清 + ABC 填充 */
    /* 未命中率是"每帧每字都重光栅化"还是"只换内容才重做"的判据。
     * 同时记下是哪个 key 字段变了 (见 Perf_Frame 的 vwhy)。 */
    if (g_PfOn)
    {
        Perf_Hit(PF_VMISS);
        g_VecMissWhy = !e->used ? 1
                     : (e->wch != wch) ? 2
                     : (e->idata != idata) ? 3
                     : (e->px != px) ? 4
                     : (e->adv != adv) ? 5
                     : (e->h != h) ? 6 : 7;
    }
    /* 未命中: GDI 单字光栅 (带边距), 扫出本字真实墨水盒 */
    if (!GdiEnsure(px, g_VT.Bold, VecGdiSlot(px), g_VTFace)) { g_VecWhy = 12; return NULL; }
    gw = GtRenderGlyphAt(wch, GL_M, GL_Y0, NULL);
    if (gw <= 0 || h > VC_MAXH) { g_VecWhy = 13; return NULL; }
    bits = g_G->bits;
    pitch = g_G->pitch;
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    /* 基线行: GL_Y0 是 TA_TOP (行顶), 基线在其下 tmAscent 处。 */
    base = GL_Y0 + (g_G->asc > 0 ? g_G->asc : px);
    /* 本字自己的墨水盒 (整幅 DIB, 不截)。 */
    T = DIB_H; B = -1;
    for (y = 0; y < DIB_H; y++)
    {
        unsigned char* row = bits + y * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (y < T) T = y; if (y > B) B = y; }
    }
    if (B < T)
    {
        /* 空白字 (空格/控制符): 留一个空行, 基线偏移按字体给。 */
        for (y = 0; y < VC_MAXH; y++) { e->rowLo[y] = 1; e->rowHi[y] = 0; }
        for (dx = 0; dx < VC_MAXW * VC_MAXH; dx++) e->cov[dx] = 0;
        e->used = 1; e->wch = wch; e->idata = idata; e->px = px;
        e->adv = adv; e->h = h; e->nat = g_VecNat;
        e->outh = 1; e->asc = g_G->asc > 0 ? g_G->asc : px;
        e->cell = g_G->cell > 0 ? g_G->cell : h;
        e->ascd = g_G->asc > 0 ? g_G->asc : px;
        e->fullW = 0; e->gdiW = 0; e->off = 0; e->inkW = 0;
        return e;
    }
    outH = B - T + 1;
    if (outH > VC_MAXH) { g_VecWhy = 14; return NULL; }
    /* 横向墨水盒只在本字墨水行内找。 */
    L = scanW; R = -1;
    for (y = T; y <= B; y++)
    {
        unsigned char* row = bits + y * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
    }
    /* 全墨水不压缩, 溢出由面界收尾。 */
    inkW = (R >= L) ? R - L + 1 : 0;
    off = VecGlyphOff(wch, adv, inkW, &gdiW);
    if (inkW < 0 || inkW > VC_MAXW || off < 0 ||
        off + inkW > VC_MAXW) { g_VecWhy = 16; return NULL; }
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (T + y) * pitch;
        unsigned char* dst = e->cov + y * VC_MAXW;
        int lo = VC_MAXW, hi = -1;
        for (dx = 0; dx < VC_MAXW; dx++) dst[dx] = 0;
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
    e->asc = base - T;                       /* 墨水顶 -> 基线 */
    e->cell = g_G->cell > 0 ? g_G->cell : h;
    e->ascd = g_G->asc > 0 ? g_G->asc : px;
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
    /* 垂直按基线对齐: 格内居中基线, 本字贴回 dy = 基线 - asc。 */
    {
        int cell = e->cell > 0 ? e->cell : h;
        int by = y + (h - cell) / 2 + e->ascd;
        dy = by - e->asc;
    }
    {
        int lo = Tp, hi = B - e->outh + 1;
        if (hi >= lo)
        {
            if (dy < lo) dy = lo;
            if (dy > hi) dy = hi;
        }
    }
    dy += yOff;
    /* 混合热循环: 5 位 alpha 查表版。
     * 逐像素朴素算法是 3 次展表 + 6 次乘除 (8 位空间混合再压位, ~50 cycles);
     * 现在把 alpha 压到 5 位 (aq = a>>3), 目标侧用全局表 TB[inv][dst]
     * = dst*inv/31, 前景侧按字现建 32 项, 每像素只剩 4 次小表查 + 3 次加。
     * 边界精确: a=0 -> aq=0/inv=31 -> 结果等于原像素; a=255 -> aq=31/inv=0
     * -> 结果等于纯色。每个通道与 8 位混合最多差 1 个色阶 (18px 字形不可见)。 */
    {
        int b6 = (g_GBits == 6);
        unsigned char frq[32], fgq[32], fbq[32];
        if (!g_ExpReady) GtBuildExpand();
        {
            unsigned sr = (fr * 31u + 127u) / 255u;
            unsigned sg = b6 ? (fg * 63u + 127u) / 255u : (fg * 31u + 127u) / 255u;
            unsigned sb = (fb * 31u + 127u) / 255u;
            for (dx = 0; dx < 32; dx++)
            {
                unsigned u = (unsigned)dx;
                frq[dx] = (unsigned char)((sr * u + 15u) / 31u);
                fgq[dx] = (unsigned char)((sg * u + 15u) / 31u);
                fbq[dx] = (unsigned char)((sb * u + 15u) / 31u);
            }
        }
        for (y = 0; y < e->outh; y++)
        {
            int sy = dy + y;
            unsigned short* row;
            unsigned char* cov;
            int lo = e->rowLo[y], hi = e->rowHi[y], x0, x1;
            unsigned short* rw;
            if (hi < lo) continue;
            if (sy < Tp || sy > B) continue;
            row = buf + sy * pitchW;
            cov = e->cov + y * VC_MAXW;
            x0 = lo;
            x1 = hi;
            if (x + x0 < 0) x0 = -x;
            if (x + x1 >= pitchW) x1 = pitchW - 1 - x;
            rw = row + x;
            if (b6)
            {
                for (dx = x0; dx <= x1; dx++)
                {
                    unsigned d = rw[dx];
                    unsigned aq = (unsigned)cov[dx] >> 3;
                    unsigned inv = 31u - aq;
                    unsigned rr = (unsigned)frq[aq] + g_BlendR[inv][(d >> 11) & 0x1Fu];
                    unsigned gg = (unsigned)fgq[aq] + g_BlendG[inv][(d >> 5) & 0x3Fu];
                    unsigned bb = (unsigned)fbq[aq] + g_BlendR[inv][d & 0x1Fu];
                    if (rr > 31u) rr = 31u;
                    if (gg > 63u) gg = 63u;
                    if (bb > 31u) bb = 31u;
                    rw[dx] = (unsigned short)((rr << 11) | (gg << 5) | bb);
                }
            }
            else
            {
                for (dx = x0; dx <= x1; dx++)
                {
                    unsigned d = rw[dx];
                    unsigned aq = (unsigned)cov[dx] >> 3;
                    unsigned inv = 31u - aq;
                    unsigned rr = (unsigned)frq[aq] + g_BlendR[inv][(d >> 10) & 0x1Fu];
                    unsigned gg = (unsigned)fgq[aq] + g_BlendR[inv][(d >> 5) & 0x1Fu];
                    unsigned bb = (unsigned)fbq[aq] + g_BlendR[inv][d & 0x1Fu];
                    if (rr > 31u) rr = 31u;
                    if (gg > 31u) gg = 31u;
                    if (bb > 31u) bb = 31u;
                    rw[dx] = (unsigned short)((rr << 10) | (gg << 5) | bb);
                }
            }
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
    int asc, cell, ascd;
    int gdiW;
    unsigned char cov[VC_MAXW * VC_MAXH];
} MVEnt;
static MVEnt g_MV[MV_MASK + 1];

/* 与 VecGlyph 同一套: 逐字量本字墨水盒 + 记到基线的距离。 */
static MVEnt* VecMenuGlyph(void* t, wchar_t wch, int adv, int h, int px)
{
    unsigned idx = ((unsigned)wch) & MV_MASK;
    MVEnt* e = &g_MV[idx];
    void* idata = *(void**)((char*)t + 4);
    int gw, dx, y, L, R, inkW, x, off, gdiW;
    int scanW, T, B, base, outH;
    unsigned char* bits;
    int pitch;
    if (!t || !IsPlausiblePtr(t)) return NULL;
    if (px < 8 || px > 40) return NULL;
    if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
        e->adv == adv && e->h == h)
        return e;
    if (!GdiEnsure(px, g_VT.Bold, VecGdiSlot(px), g_VTFace)) return NULL;
    gw = GtRenderGlyphAt(wch, GL_M, GL_Y0, NULL);
    if (gw <= 0 || h > VC_MAXH) return NULL;
    bits = g_G->bits;
    pitch = g_G->pitch;
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    base = GL_Y0 + (g_G->asc > 0 ? g_G->asc : px);
    T = DIB_H; B = -1;
    for (y = 0; y < DIB_H; y++)
    {
        unsigned char* row = bits + y * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (y < T) T = y; if (y > B) B = y; }
    }
    if (B < T) { outH = 1; T = base; inkW = 0; off = 0; }
    else
    {
        outH = B - T + 1;
        if (outH > VC_MAXH) return NULL;
        L = scanW; R = -1;
        for (y = T; y <= B; y++)
        {
            unsigned char* row = bits + y * pitch;
            for (x = 0; x < scanW; x++)
                if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
        }
        inkW = (R >= L) ? R - L + 1 : 0;
        off = VecGlyphOff(wch, adv, inkW, &gdiW);
        if (inkW < 0 || inkW > VC_MAXW || off < 0 ||
            off + inkW > VC_MAXW) return NULL;
    }
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (T + y) * pitch;
        unsigned char* dst = e->cov + y * VC_MAXW;
        for (dx = 0; dx < VC_MAXW; dx++) dst[dx] = 0;
        for (dx = 0; dx < inkW; dx++)
            dst[off + dx] = row[(L + dx) * 4 + 2];
    }
    e->used = 1; e->wch = wch; e->idata = idata; e->px = px; e->adv = adv;
    e->h = h; e->off = off; e->inkW = inkW;
    e->outh = outH;
    e->asc = base - T;
    e->cell = g_G->cell > 0 ? g_G->cell : h;
    e->ascd = g_G->asc > 0 ? g_G->asc : px;
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
    dy = y + (h - e->cell) / 2 + e->ascd - e->asc;
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

static long long g_BlitMeasT = 0;

static int __fastcall VecText_BlitBody(void* t, void* edxUnused, int wch, int x, int y, int color)
{
    wchar_t wc = (wchar_t)((unsigned)wch & 0xFFFFu);
    int eff, adv, h = 0, r, px;
    (void)edxUnused;
    if (!g_OrigBlit) return 0;   /* 不该发生, 守一下不断原版链 */
    /* 结算界面 (遭遇战积分/任务积分): 打开期间整条回落原版, 不做字形
     * 替换, 笔位也照引擎原样走。 */
    if (g_ScoreScreen > 0)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    /* TAB 走原版特殊公式, 缺字/度量失败也走原版 (保留 0x5555 兜底行为) */
    if (!t || !IsPlausiblePtr(t) || wc == 9)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    if (g_PfOn) { g_PfGlyph++; PfTick(); }
    /* 坐标归一 (脏高位清掉, 见 VecNormC): 以下 inTip 跨度
     * 与 VecRender 全用真坐标。 */
    x = VecNormC(x);
    y = VecNormC(y);
    eff = (color == -1) ? (int)(*(unsigned short*)((char*)t + 0x24)) : color;
    /* PUA 自定义图标 (建造提示第三行): PCX 索引直画, 不走 GDI/原版点阵。
     * 放在 VecMeasure 之前 (原版度量不认 PUA, 必失败回落出 tofu)。
     * PCX 缺失/加载失败时跳过, 不要回落成 tofu。 */
    if (Icon_Is((unsigned)wc & 0xFFFFu))
    {
        int ia = Icon_Advance((unsigned)wc & 0xFFFFu);
        if (ia > 0)
        {
            int rr = Icon_Blit(t, (unsigned)wc & 0xFFFFu, x, y, eff,
                               Icon_LineH(t));
            if (rr >= 0) return x + ia;
        }
        return x;
    }
    adv = VecMeasure(t, wc, &h);
    g_BlitMeasT = Perf_Now();   /* 分段探针: Measure 结束时刻 (见 BlitHook) */
    if (adv < 0)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    /* 简报正文自绘 (BriefOwn=1): 面到这里已经绑好了 (引擎 0x4348F0 装的),
     * 借它的面把整块画掉, 并抑制这一次 DrawText 余下的逐字绘制。笔位照
     * 原样还给引擎 (x+adv), 引擎版式照跑, 只是不再落墨。
     * 必须跟 Render 一起关: 本块在下面的 if (g_VT.Render) 之前, 不挂门的话
     * Render=0 时简报仍是矢量重绘, 而其余文本走原版点阵 (两种字混排)。 */
    if (g_VT.Render && g_DTBody && g_BriefState != 2)
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
        /* 位置门 (颜色盲): 本 Print 的首个 Blit 落在当帧提示跨度内 (inTip)。
         * 边缘出屏字会被引擎跳过 (不调 Blit), 首 Blit 未必是串首, 所以按跨度
         * [tipx-tot-16, tipx+tot+8] 认, 不按原点 ±8。
         * seen 守卫: 每批只进一次 —— 串内后续字的 tx 天然递增, 按"漂移"重画会
         * 让盒跟着字走、越走越偏。Print→Blits 单线程同步, 不存在跨 Print 陈旧。 */
        if (!g_BlitTipSeen && inTip && t && IsPlausiblePtr(t))
        {
            unsigned short* bb = *(unsigned short**)((char*)t + 0x0C);
            int pp = *(int*)((char*)t + 0x10);
            g_BlitTipSeen = 1;
            /* 出界修复 (TipBox): 引擎窄盒 [cx-63,cx+62] 已在, 这里补两侧条带
             * (纹理保留, 同 tone 无缝)、采样顶行红画新边框, 字随后画上。
             * 坐标全在已扫描安全区内; 世界每帧重画, 无残留。TipBox=0 关闭。 */
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
                if (nF++ < 8)
                    LogV("vfb:", "# wch=", (unsigned)wc & 0xFFFFu, " x=",
                         tx + 2000, " y=", y, " adv=", adv, " why=",
                         g_VecWhy, NULL);
            }
            if (r != VC_FAIL) return r;   /* x == ox (本函数内 x 只读)。 */
        }
    }
    return g_OrigBlit(t, NULL, wch, ox, y, color);
}

/* 导出给钩子安装点的入口 (名字被 VecText_Install 的 JMP 钩引用): 只做 Perf 记账,
 * 逐字累计, 用来判断"引擎每帧画字"这条路径占多少帧时间。 */
int __fastcall VecText_BlitHook(void* t, void* edxUnused, int wch, int x, int y, int color)
{
    long long tPf = Perf_Now();
    long long tEnd;
    int r;
    g_BlitMeasT = 0;
    r = VecText_BlitBody(t, edxUnused, wch, x, y, color);
    tEnd = Perf_Now();
    Perf_Add(PF_VECBLIT, tPf);
    if (g_BlitMeasT)
    {
        Perf_AddDelta(PF_H_VB1, g_BlitMeasT - tPf);
        Perf_AddDelta(PF_H_VB2, tEnd - g_BlitMeasT);
    }
    return r;
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

/* 打原版字节 (字节对不上时用: 现场只看日志就能判断是不是被别的模块改过)。 */
static void LogBytes(const unsigned char* p)
{
    char b[96];
    unsigned q = AppendCStr(b, 0, "vtext: got=", sizeof(b));
    int i;
    for (i = 0; i < 7; i++)
    {
        q = AppendInt(b, q, sizeof(b), p[i]);
        q = AppendCh(b, q, ' ', sizeof(b));
    }
    LogLine(b);
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
    /* [VecText] 的整型键: 键名 / 落到哪 / 默认值 / 钳制区间。lo==0&&hi==1 视为
     * 布尔 (只归一到 0/1)。新增配置项只加一行。LogDetail 在别的节, 单独读。 */
    static const struct { const char* k; int* p; int def, lo, hi; } kKey[] = {
        { "Enable",     &g_VT.Enable,     1,   0, 1    },
        { "Bold",       &g_VT.Bold,       0,   0, 1    },
        { "FontPx",     &g_VT.FontPx,     0,   0, 40   },
        { "CreditPx",   &g_VT.CreditPx,   0,   0, 40   },
        { "CreditDY",   &g_VT.CreditDY,   0,  -8, 8    },
        { "MenuTake",   &g_VT.MenuTake,   1,   0, 1    },
        { "DigitTrack", &g_VT.DigitTrack, 0,  -8, 8    },
        { "TipBox",     &g_VT.TipBox,     1,   0, 1    },
        { "BriefLine",  &g_VT.BriefLine,  130, 0, 300  },
        { "BriefOwn",   &g_VT.BriefOwn,   1,   0, 1    },
        { "BriefTrack", &g_VT.BriefTrack, 2,   0, 12   },
        { "BriefMark",  &g_VT.BriefMark,  1,   0, 1    },
        { "BriefWidth", &g_VT.BriefWidth, 620, 0, 4000 },
        { "BriefAdv",   &g_VT.BriefAdv,   1,   0, 1    },
        { "Perf",       &g_VT.Perf,       0,   0, 1    },
        { "Render",     &g_VT.Render,     0,   0, 1    },
    };
    unsigned i;
    if (g_VtInit) return;
    g_VtInit = 1;
    GtEnsureLog();
    if (!g_Log) return;
    for (i = 0; i < sizeof(kKey) / sizeof(kKey[0]); i++)
    {
        int* p = kKey[i].p;
        int v = GetPrivateProfileIntA(VT_SECTION, kKey[i].k, kKey[i].def, g_IniPath);
        if (v < kKey[i].lo) v = kKey[i].lo;
        if (v > kKey[i].hi) v = kKey[i].hi;
        *p = v;
    }
    OU_ReadString(VT_SECTION, "FontFace", "simhei.ttf",
                  g_VT.FontFace, sizeof(g_VT.FontFace));
    g_VT.LogDetail = GetPrivateProfileIntA("Log", "LogDetail", 0, g_IniPath);
    g_VT.LogDetail = g_VT.LogDetail ? 1 : 0;
    g_PfOn = g_VT.Perf;
    g_LogDetail = g_VT.LogDetail;
    ResolveFontFace(g_VT.FontFace, "vtext", g_VTFace, L"Microsoft YaHei");
    LogV("vtext: start", " en=", g_VT.Enable, " dtail=", g_VT.LogDetail,
         " render=", g_VT.Render, " px=", g_VT.FontPx, " cred=", g_VT.CreditPx,
         "- credy=", g_VT.CreditDY, " mt=", g_VT.MenuTake,
         "- dt=", g_VT.DigitTrack, NULL);
}

/* ---------------- FPS 钉住绘制 (Render 尾 0x4F4589, Ares 之后) ----------------
 * 本帧 Simple_ 收到的计数器串, 以固定字号画到固定位 (x=0, 底上 32,
 * 与 Ares 同锚点), 任何档位都是 1x。只画窗口面, 大面跳过。
 * 这里只绘制, 不参与限帧 (限帧见 FrameCap.cpp)。 */
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

/* ---------------- 运行期后装钩 (避让外部模块同址) ----------------
 * 结算显示钩 (0x5C9720 遭遇战/多人, 0x6C8E80 任务) 与 FPS 钉住钩 (0x4F4589)
 * 在本 exe 里可能被其它模块 (如 CnCNet-Spawner.dll) 以 Syringe 钩占用同址。
 * Syringe 的 .syhks00 钩在本 DLL 的 DllMain 之后才写入: 若本模块在 DllMain
 * 抢先装自建跳板, 对方随后会覆盖本模块的 E9, 并把本模块的 E9 当"原始指令"
 * 拷进它的跳板 -> 链路错乱 (同类隐患, 见 notes/聚集点多段路径.md)。
 * 故这些钩改到每帧、外部钩就位之后再装: 先验原版字节, 对不上 (已被外部钩占)
 * 即放弃本钩并记录, 绝不同址争抢。deps 均已在此前定义。 */
static int g_LateHooksDone = 0;

void VecText_LateInstall(void)
{
    static const unsigned char kWantScoreShow[8] =
        { 0x83, 0xEC, 0x10, 0xA1, 0x30, 0xB2, 0xA8, 0x00 };
    static const unsigned char kWantScoreProc[8] =
        { 0x8B, 0x44, 0x24, 0x10, 0x83, 0xEC, 0x40, 0x53 };
    static const unsigned char kWantFpsPin[5] = { 0xA1, 0x1C, 0x73, 0x88, 0x00 };
    if (g_LateHooksDone) return;
    if (!g_Log) return;
    /* 结算界面显示钩: 打开期间 g_ScoreScreen>0, 文本钩整条回落原版。
     * 与文本开关无关 (任务结算的退出按钮也靠它收尾)。 */
    if (BytesSame((unsigned char*)0x5C9720u, kWantScoreShow, 8))
    {
        g_OrigMPScoreShow = (ShowDlgFn)BuildTrampAt(0x5C9720u, 8, 0x5C9728u);
        if (!g_OrigMPScoreShow ||
            !PatchAt(0x5C9720u, (void*)(size_t)VecMPScoreShowEntry, 8))
            g_OrigMPScoreShow = NULL;
        else LogLine("vtext: mpscore show hook ON (late)");
    }
    else LogLine("vtext: mpscore show site not pristine - OFF (foreign hook?)");
    if (BytesSame((unsigned char*)0x6C8E80u, kWantScoreShow, 8))
    {
        g_OrigStandaloneShow = (ShowDlgFn)BuildTrampAt(0x6C8E80u, 8, 0x6C8E88u);
        if (!g_OrigStandaloneShow ||
            !PatchAt(0x6C8E80u, (void*)(size_t)VecStandaloneShowEntry, 8))
            g_OrigStandaloneShow = NULL;
        else LogLine("vtext: standalone show hook ON (late)");
    }
    else LogLine("vtext: standalone show site not pristine - OFF (foreign hook?)");
    /* 任务结算对话框过程钩: 加“退出游戏”按钮。展示钩没装上就不加
     * (按钮点了也走不到收尾)。 */
    if (g_OrigStandaloneShow)
    {
        if (BytesSame((unsigned char*)0x6C8FF0u, kWantScoreProc, 8))
        {
            g_OrigScoreDlgProc =
                (DlgProcFn)BuildTrampAt(0x6C8FF0u, 8, 0x6C8FF8u);
            if (!g_OrigScoreDlgProc ||
                !PatchAt(0x6C8FF0u, (void*)(size_t)VecScoreDlgEntry, 8))
                g_OrigScoreDlgProc = NULL;
        }
    }
    /* FPS 钉住钩 (0x4F4589, GScreenClass::Render 尾): 只为本模块的文本服务,
     * 文本接管关了就不装。 */
    if (!g_VT.Enable) { g_LateHooksDone = 1; return; }
    if (BytesSame((unsigned char*)0x4F4589u, kWantFpsPin, 5))
    {
        g_FpsPinTramp = BuildTrampAt(0x4F4589u, 5, 0x4F458Eu);
        if (!g_FpsPinTramp || !PatchAt(0x4F4589u, (void*)(size_t)VecFpsPinEntry, 5))
            g_FpsPinTramp = NULL;
        else LogLine("vtext: fpspin hook ON (late)");
    }
    else LogLine("vtext: fpspin site not pristine - OFF (foreign hook?)");
    g_LateHooksDone = 1;
}

/* ---------------- 装钩: 表驱动 ----------------
 * 每个钩都是同一件事: 校验原版头字节 -> 建跳板 -> 打 E9 补丁。原版字节对不上
 * = 该地址已被别的模块改过 (Ares/CnCNet 等), 整条放弃, 只记日志不争抢。
 * 逐个手写只是样板, 这里一张表装完; 新增钩加一行。 */

typedef struct {
    unsigned at;                 /* 挂钩地址 */
    const unsigned char* want;   /* 原版头字节 */
    unsigned char n;             /* 被覆盖的字节数 */
    void* entry;                 /* 我们的入口 */
    void** tramp;                /* 跳板落到哪 (失败清 NULL) */
    const char* name;            /* 日志前缀, 输出 "<name> hook ON/OFF" */
} VecHookEnt;

/* 原版头字节 (对不上时 LogBytes 会把现场打出来对照) */
static const unsigned char kW_DrawText[6]   = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56 };
static const unsigned char kW_Print[10]     = { 0x56, 0x8B, 0x74, 0x24, 0x08,
                                                0x57, 0x8B, 0x7C, 0x24, 0x18 };
static const unsigned char kW_Simple[7]     = { 0x83, 0xEC, 0x28, 0x8B, 0x44, 0x24, 0x34 };
static const unsigned char kW_GetDim[6]     = { 0x83, 0xEC, 0x1C, 0x8B, 0x41, 0x04 };
static const unsigned char kW_Prepare[5]    = { 0xA1, 0x24, 0x73, 0x88, 0x00 };
static const unsigned char kW_BlitSb[6]     = { 0x83, 0xEC, 0x38, 0x53, 0x55, 0x56 };
static const unsigned char kW_TipPre[7]     = { 0x8B, 0x11, 0x6A, 0x01, 0xFF, 0x52, 0x0C };
static const unsigned char kW_InitGui[5]    = { 0x83, 0xEC, 0x24, 0x53, 0x55 };
static const unsigned char kW_GadgetIn[6]   = { 0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56 };
static const unsigned char kW_Update[7]     = { 0x83, 0xEC, 0x5C, 0x8B, 0x44, 0x24, 0x60 };
static const unsigned char kW_Render[8]     = { 0x83, 0xEC, 0x08, 0xA1, 0x1C, 0x73, 0x88, 0x00 };
static const unsigned char kW_PreCur[5]     = { 0xA3, 0x14, 0x73, 0x88, 0x00 };
static const unsigned char kW_Map[5]        = { 0xA1, 0x1C, 0x73, 0x88, 0x00 };
static const unsigned char kW_ChromePre[5]  = { 0x8B, 0x16, 0x50, 0x8B, 0xCE };
static const unsigned char kW_ChromePost[6] = { 0x8B, 0x0D, 0x1C, 0x73, 0x88, 0x00 };
static const unsigned char kW_AfterMap[5]   = { 0xA0, 0x19, 0xB5, 0xB0, 0x00 };
static const unsigned char kW_EdgeFix[7]    = { 0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x57 };
static const unsigned char kW_ClickCoord[7] = { 0x8B, 0x44, 0x24, 0x10, 0x83, 0xEC, 0x18 };
static const unsigned char kW_CamSet[5]     = { 0x83, 0xEC, 0x08, 0x53, 0x55 };
static const unsigned char kW_RadarCell[6]  = { 0x8B, 0x0D, 0xA8, 0x6F, 0x88, 0x00 };
static const unsigned char kW_BlitGuard[5]  = { 0x83, 0xEC, 0x14, 0x55, 0x56 };
static const unsigned char kW_Shroud[5]     = { 0x83, 0xEC, 0x48, 0x56, 0x57 };
static const unsigned char kW_WndMouse[8]   = { 0xA0, 0x78, 0xE3, 0xA8, 0x00,
                                                0x83, 0xEC, 0x1C };

/* 两段字符串拼一行日志 ("<name> hook ON" 这类)。 */
static void LogCat(const char* a, const char* b)
{
    char s[96];
    unsigned q = AppendCStr(s, 0, a, sizeof(s));
    AppendCStr(s, q, b, sizeof(s));
    LogLine(s);
}

/* 装一个钩。返回 1 = 装上。 */
static int VecHookAt(const VecHookEnt* h)
{
    *h->tramp = NULL;
    if (!BytesSame((const unsigned char*)h->at, h->want, (int)h->n))
    {
        LogCat(h->name, " bytes mismatch");
        LogBytes((const unsigned char*)h->at);
        return 0;
    }
    *h->tramp = BuildTrampAt(h->at, (int)h->n, h->at + (unsigned)h->n);
    if (*h->tramp && PatchAt(h->at, h->entry, (int)h->n))
    {
        LogCat(h->name, " hook ON");
        return 1;
    }
    *h->tramp = NULL;
    LogCat(h->name, " hook OFF");
    return 0;
}

static void VecHookRun(const VecHookEnt* t, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) VecHookAt(&t[i]);
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
        /* 文本四钩: DrawText/Print 只记上下文, Simple_ 收表并跳过, GetDim 接管宽度。 */
        {
            const VecHookEnt kText[] = {
                { ADDR_DRAWTEXT, kW_DrawText, 6, (void*)VecDrawTextEntry,
                  (void**)&g_OrigDrawText, "vtext: drawtext ctx" },
                { ADDR_PRINT, kW_Print, 10, (void*)VecPrintEntry,
                  (void**)&g_OrigPrint, "vtext: print ctx" },
                { ADDR_SIMPLE, kW_Simple, 7, (void*)VecSimpleEntry,
                  (void**)&g_OrigSimple, "vtext: simple" },
                { ADDR_GETDIM, kW_GetDim, 6, (void*)VecGetDimEntry,
                  (void**)&g_OrigGetDim, "vtext: getdim" },
            };
            VecHookRun(kText, sizeof(kText) / sizeof(kText[0]));
        }
        /* 结算显示钩 / FPS 钉住钩改到运行期后装, 见 VecText_LateInstall():
         * 这些地址可能与外部模块 (CnCNet-Spawner 等) 的 Syringe 钩同址, 必须
         * 等对方就位后再装, 否则本模块自建跳板会与对方的同址钩互相破坏。 */
    }
    else LogLine("vtext: text hooks OFF (Enable=0)");
    /* 以下共用钩与 Enable 无关。 */
    {
        /* Prepare / 上屏入口 / 放大前提示框绘制改直跑。 */
        const VecHookEnt kH1[] = {
            { 0x4F42F0u, kW_Prepare, 5, (void*)MOExperience_PrepareEntry,
              &g_PrepareTramp, "vtext: prepare" },
            { 0x6A70E0u, kW_BlitSb, 6, (void*)MOExperience_BlitSbEntry,
              &g_BlitSbTramp, "vtext: blitsb" },
            { 0x6A7089u, kW_TipPre, 7, (void*)MOExperience_TipPreDrawEntry,
              &g_TipPreDrawTramp, "vtext: tip-pre" },
        };
        VecHookRun(kH1, sizeof(kH1) / sizeof(kH1[0]));
    }
    /* 提示框矩形改指 A 影子 (5 处立即数: 引擎画提示框时读的面几何)。 */
    {
        unsigned char* p;
        DWORD old;
        unsigned i, n, abase = (unsigned)(size_t)SB_AViewBlock();
        int bad = -1;
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
        /* 先全验再改 (半途失败会留下改了一半的现场)。 */
        for (i = 0; i < n; i++)
        {
            p = (unsigned char*)rep[i].at;
            if (p[0] != rep[i].op[0] ||
                (rep[i].oplen > 1 && p[1] != rep[i].op[1])) { bad = (int)i; break; }
        }
        for (i = 0; bad < 0 && i < n; i++)
        {
            unsigned imm = rep[i].at + rep[i].imm;
            if (!VirtualProtect((void*)imm, 4, PAGE_EXECUTE_READWRITE, &old))
            { bad = (int)i; break; }
            *(unsigned*)(size_t)imm = abase + rep[i].off;
            VirtualProtect((void*)imm, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)imm, 4);
        }
        if (bad < 0) LogLine("vtext: tooltip surface rect -> A shadow");
        else LogV("vtext: tooltip rect patch OFF at=", "", rep[bad].at, NULL);
    }
    /* UpdatePrimarySurface 两调用点字节被 Ares 改写过, 不挂功能钩, 只监听补丁存活。 */
    /* 其余共用钩: 侧栏列纠正 / 视口命中链 / 呈现与缩放 / 大面让出还原 /
     * 边框 chrome 两趟 / 边缘卷屏 / ProcessClickCoords 上界。数组顺序即安装顺序。 */
    {
        const VecHookEnt kH2[] = {
            { 0x6ABD30u, kW_InitGui, 5, (void*)MOExperience_InitGuiEntry,
              &g_InitGuiTramp, "vtext: initgui" },
            { 0x4E1640u, kW_GadgetIn, 6, (void*)MOExperience_GadgetInputEntry,
              &g_GadgetInputTramp, "vtext: gadgetinput" },
            { 0x4F4780u, kW_Update, 7, (void*)MOExperience_UpdateEntry,
              &g_UpdateTramp, "vtext: update" },
            { 0x4F4480u, kW_Render, 8, (void*)MOExperience_RenderEntry,
              &g_RenderTramp, "vtext: render" },
            { 0x4F4492u, kW_PreCur, 5, (void*)MOExperience_PreCurEntry,
              &g_PreCurTramp, "vtext: precur" },
            { 0x4F44CBu, kW_Map, 5, (void*)MOExperience_MapEntry,
              &g_MapTramp, "vtext: mapentry" },
            { 0x4F44FDu, kW_ChromePre, 5, (void*)MOExperience_ChromePreEntry,
              &g_ChromePreTramp, "vtext: chrome-pre" },
            { 0x4F4505u, kW_ChromePost, 6, (void*)MOExperience_ChromePostEntry,
              &g_ChromePostTramp, "vtext: chrome-post" },
            { 0x4F451Bu, kW_AfterMap, 5, (void*)MOExperience_AfterMapEntry,
              &g_AfterMapTramp, "vtext: aftermap" },
            { 0x692B60u, kW_EdgeFix, 7, (void*)MOExperience_EdgeFixEntry,
              &g_EdgeFixTramp, "vtext: edgefix" },
            { 0x692300u, kW_ClickCoord, 7, (void*)MOExperience_ClickCoordsEntry,
              &g_ClickCoordsTramp, "vtext: clickcoords" },
        };
        VecHookRun(kH2, sizeof(kH2) / sizeof(kH2[0]));
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
/* 小地图/程序化相机定位 (PiP 聚焦时标记跳转, 与 Enable/缩放无关); 小地图原始格
 * (0x653D92 = 主视野出界钳制前, 只读留一份未钳制格, PiP 用小窗尺寸重钳);
 * 通用拉伸 blit 越界写守卫 (cameo/图标); 迷雾 DrawPixelEffects 越界写护栏。 */
    {
        const VecHookEnt kH3[] = {
            { 0x6D6070u, kW_CamSet, 5, (void*)MOExperience_CameraSetEntry,
              &g_CameraSetTramp, "pip: cameraset" },
            { 0x653D92u, kW_RadarCell, 6, (void*)MOExperience_RadarClampEntry,
              &g_RadarCellTramp, "pip: radarraw" },
            { 0x6BA580u, kW_BlitGuard, 5, (void*)MOExperience_BlitGuardEntry,
              &g_BlitGuardTramp, "vtext: blitguard" },
            { 0x6D7840u, kW_Shroud, 5, (void*)MOExperience_ShroudEntry,
              &g_ShroudTramp, "sbar: shroudguard" },
        };
        VecHookRun(kH3, sizeof(kH3) / sizeof(kH3[0]));
    }
/* 鼠标折算在源头做: 换虚表 +0x34, 11 个消费点一次覆盖。按钮消息路 (0x6930A0)
 * 再换第 4 实参指针为折算副本 (不过 WWMouse)。详见 notes。 */
    MOE_InstallMousePosHook();
    {
        const VecHookEnt kH4[] = {
            { 0x6930A0u, kW_WndMouse, 8, (void*)MOExperience_WndMouseEntry,
              &g_WndMouseTramp, "vtext: wndmouse" },
        };
        VecHookRun(kH4, sizeof(kH4) / sizeof(kH4[0]));
    }
}
