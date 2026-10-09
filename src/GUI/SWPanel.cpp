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
/* 几何诊断 (只在变化时打一行): 排查名/计时两列错位看这个 */
static int s_LastAnchor = -1, s_LastRightEdge = -1, s_LastMapRight = -1;
static int s_LastRows = -1;   /* 行数也进触发条件: 条目变少要能看出来 */
static int s_AnchorLost = 0;

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
    p = 0;
    p = AppendCStr(buf, p, "config: SWPanel en=", sizeof(buf));
    p += UtoA((unsigned)g_SW.Enable, buf + p);
    p = AppendCStr(buf, p, " op=", sizeof(buf));
    p += UtoA((unsigned)g_SW.Opacity, buf + p);
    p = AppendCStr(buf, p, " px=", sizeof(buf));
    p += UtoA((unsigned)g_SW.FontPx, buf + p);
    p = AppendCStr(buf, p, " classic=", sizeof(buf));
    p += UtoA((unsigned)g_SW.Classic, buf + p);
    LogLine(buf);
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

/* 画一行的一部分 (名或计时器): 渲染+阴影+正文。返回像素宽, <=0=无内容。 */
static int SWBlitPart(unsigned char* locked, int surfPitch, int surfW,
                      int surfH, const wchar_t* s, int x, int ly, int adv,
                      unsigned char fr, unsigned char fg, unsigned char fb)
{
    int l = SWLen(s), w;
    if (l <= 0) return 0;
    w = RenderLine(s, l);
    if (w <= 0) return 0;
    if (g_SW.Shadow && g_SW.ShadowOpacity > 0)
    {
        /* 单遍并集阴影 (见 GdiText.h 的 BlitCoverageShadow) */
        BlitCoverageShadow(locked, surfPitch, surfW, surfH,
                           x, ly, w + 2, adv,
                           g_SW.ShadowSize, g_SW.ShadowDX, g_SW.ShadowDY,
                           (unsigned)g_SW.ShadowOpacity);
    }
    BlitCoverage(locked, surfPitch, surfW, surfH, x, ly,
                 w + 2, adv, fr, fg, fb, 100);
    return w;
}

/* 统一面板 (Classic=0): 右下锚定单个半透明底, 名左计时器右。
 * 原版式 (Classic=1): 右对齐 + 逐行贴合黑条 (名右对齐到计时器列,
 *   计时器右对齐到右缘; 每行高=字高+上下 PadY, 条四面留边无缝衔接,
 *   字永不出条; 整块底部锚定 BottomMargin, 引擎 y 只定上下顺序)。 */
int SWPanel_Draw(void* surf, int bitLineH, int surfW, int surfH, int surfPitch)
{
    int ys[SWCAP_MAX], ni[SWCAP_MAX], ti[SWCAP_MAX];
    int ws[SWCAP_MAX], wt[SWCAP_MAX];
    int n = 0, i, j, k;
    unsigned now;
    int fontPx, adv, maxNW = 0, maxTW = 0, hasTimer = 0;
    int panelW = 0, panelH = 0, px = 0, py = 0;
    int rightEdge = 0, contentRight = 0, nameEndCol = 0;
    int step = 0, top = 0, anchor;
    RectangleStruct rect;
    ColorStruct board;
    unsigned char* locked;

    if (!g_SWPanelOn) return 0;
    now = GetTickCount();
    for (i = 0; i < g_SWCapN && i < SWCAP_MAX; i++)
    {
        SWCapRow* c = &g_SWCap[i];
        /* ★ 过期只用来剔"陈旧碎片", 不能让它把整块面板判没: 某一帧没收到 (切模式
         * 那几帧) 早退会让**原版列表**趁虚画出来 (inband 放行)。故放宽到
         * 1000ms: 宁可短暂显示上一次的内容, 也不能让面板凭空消失。*/
        if (now - c->tick > (unsigned)SW_STALE_MS) continue;
        for (j = 0; j < n; j++) if (ys[j] == c->y) break;
        if (j >= n)
        {
            if (n >= SWCAP_MAX) break;
            ys[n] = c->y; ni[n] = -1; ti[n] = -1; n++;
        }
        if (c->kind == 0) ni[j] = i;
        else ti[j] = i;
    }
    if (n > SWCAP_MAX) n = SWCAP_MAX;
    if (n <= 0) return 0;
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
    if (!GdiEnsure(fontPx, g_SW.Bold, 3, g_SWFace)) return -1;
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
    if (maxNW <= 0 && maxTW <= 0) return 0;
    board.R = board.G = board.B = 0;
    /* 右缘锚到**地图区**而不是画布: 侧栏显示时地图只到 1752, 锚画布 (1920) 会把
     * 整块列表塞到侧栏底下 (看不见, 名/计时两列还被侧栏切开像重影)。
     * ⚠️ 锚点拿不到时**不要**静默退回 surfW —— 那正是"隐藏侧栏后整块列表跳到
     * 屏幕最右缘"的表现。退回 0 = 贴左缘, 至少位置稳定可预期。*/
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
    /* 几何变化就打一行: 排查"名/计时两列错位"直接看这里 (错位=两处几何不一致,
     * 同一次绘制里 anchor/rightEdge/contentRight/nameEndCol 必须自洽)。*/
    if (anchor != s_LastAnchor || rightEdge != s_LastRightEdge
        || g_MapRight != s_LastMapRight || n != s_LastRows)
    {
        char b1[16], buf[192];
        unsigned q = 0;
        s_LastAnchor = anchor; s_LastRightEdge = rightEdge;
        s_LastMapRight = g_MapRight; s_LastRows = n;
        q = AppendCStr(buf, q, "swpanel: geom mapRight=", sizeof(buf));
        UtoA((unsigned)g_MapRight, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " anchor=", sizeof(buf));
        UtoA((unsigned)anchor, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " rightEdge=", sizeof(buf));
        UtoA((unsigned)rightEdge, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " contentRight=", sizeof(buf));
        UtoA((unsigned)contentRight, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " rows=", sizeof(buf));
        UtoA((unsigned)n, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        LogLine(buf);
    }
    contentRight = rightEdge - g_SW.PadX;
    nameEndCol = contentRight - (hasTimer ? maxTW + g_SW.Gap : 0);
    /* 经典行距 (字高+上下 PadY): 条无缝衔接, 底部锚定守界 (引擎 y 只定顺序) */
    step = adv + g_SW.PadY * 2;
    if (step < 1) step = 1;
    top = surfH - g_SW.BottomMargin - n * step;
    if (top < 0) top = 0;
    if (g_SW.Classic)
    {
        /* 原版式: 逐行贴合黑条 (条按本行宽度贴到右缘, 四面各留 Pad,
         * 上下行条首尾相接不重叠; 字永不出条)。 */
        if (g_SW.Opacity > 0)
        {
            for (k = 0; k < n; k++)
            {
                int barTop = top + k * step, bh, bx0, nx;
                if (barTop >= surfH) continue;
                if (ws[k] <= 0 && wt[k] <= 0) continue;
                nx = (ws[k] > 0) ? nameEndCol - ws[k] : contentRight - wt[k];
                bx0 = nx - g_SW.PadX;
                if (bx0 < 0) bx0 = 0;
                bh = step;
                if (barTop + bh > surfH) bh = surfH - barTop;
                if (bh < 1) continue;
                rect.X = bx0; rect.Y = barTop;
                rect.Width = rightEdge - bx0; rect.Height = bh;
                if (rect.Width > 0 && rect.Y < surfH)
                    GtFillRectTrans(surf, &rect, &board, g_SW.Opacity);
            }
        }
    }
    else
    {
        panelW = g_SW.PadX * 2 + maxNW + (hasTimer ? g_SW.Gap + maxTW : 0);
        panelH = g_SW.PadY * 2 + n * adv;
        px = anchor - g_SW.RightMargin - panelW;
        py = surfH - g_SW.BottomMargin - panelH;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        rect.X = px; rect.Y = py; rect.Width = panelW; rect.Height = panelH;
        if (g_SW.Opacity > 0)
            GtFillRectTrans(surf, &rect, &board, g_SW.Opacity);
    }
    locked = GtLock(surf);
    if (!locked) return -2;
    if (!g_Calibrated) Calibrate();
    if (g_SW.Classic)
    {
        /* 原版式: 名右对齐到计时器列, 计时器右对齐; 字在条内顶留 Pad */
        for (k = 0; k < n; k++)
        {
            int ty = top + k * step + g_SW.PadY;
            ColorStruct fg;
            if (ni[k] >= 0 && ws[k] > 0)
            {
                SWRowColor(g_SWCap[ni[k]].fore, 0x07FF, &fg);
                SWBlitPart(locked, surfPitch, surfW, surfH, g_SWCap[ni[k]].s,
                           nameEndCol - ws[k], ty, adv, fg.R, fg.G, fg.B);
            }
            if (ti[k] >= 0 && wt[k] > 0)
            {
                SWRowColor(g_SWCap[ti[k]].fore, 0xFFFF, &fg);
                SWBlitPart(locked, surfPitch, surfW, surfH, g_SWCap[ti[k]].s,
                           contentRight - wt[k], ty, adv, fg.R, fg.G, fg.B);
            }
        }
    }
    else
    {
        for (k = 0; k < n; k++)
        {
            int ly = py + g_SW.PadY + k * adv, tx;
            ColorStruct fg;
            if (ni[k] >= 0 && ws[k] > 0)
            {
                SWRowColor(g_SWCap[ni[k]].fore, 0x07FF, &fg);
                SWBlitPart(locked, surfPitch, surfW, surfH, g_SWCap[ni[k]].s,
                           px + g_SW.PadX, ly, adv, fg.R, fg.G, fg.B);
            }
            if (ti[k] >= 0 && wt[k] > 0)
            {
                SWRowColor(g_SWCap[ti[k]].fore, 0xFFFF, &fg);
                tx = px + panelW - g_SW.PadX - wt[k];
                if (tx < px + g_SW.PadX) tx = px + g_SW.PadX;
                SWBlitPart(locked, surfPitch, surfW, surfH, g_SWCap[ti[k]].s,
                           tx, ly, adv, fg.R, fg.G, fg.B);
            }
        }
    }
    GtUnlock(surf);
    return 1;
}
