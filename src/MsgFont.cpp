/*
 * MsgFont.cpp - 心灵终结消息队列矢量字体 (v1: 只替换显示, 无交互)。
 *
 * 钩子 (见 notes/ChatBox参考.md):
 *   0x5D3BA0 (6B) AddMessage 入口: 只读捕获 name/text/cs/timeout/frame。
 *   0x5D49A0 (6B) Draw 入口: 只画绿框触发区+标签 (零消息也跑), 返回 0 跑原版。
 *   0x5D4A94 (7B) Draw 内部画消息列表处: 画完自制消息框后返回 0x5D4A9B。
 * 任何一步失败 (Lock 不上、GDI 未就绪) 都返回 0 走原版绘制, 绝不花屏。
 *
 * 渲染: Microsoft YaHei 经 GDI 光栅化到 32 位内存 DIB (灰度抗锯齿取覆盖率),
 * 经启动校准得到的 16 位格式 (565/555) alpha 混入 Composite。
 * 取色: ColorScheme BaseColor 经 0x517440 转显示色 (Phobos 同用法)。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT)。
 */

#include <windows.h>
#include "GdiText.h"
#include "VecText.h"

#define MF_VERSION   "0.2.0"
#define MF_SECTION   "MsgFont"

/* ---------------- 地址 ---------------- */

#define ADDR_DSURFACE_COMPOSITE  0x0088731Cu
#define ADDR_DSURFACE_TEMP       0x00887314u
#define ADDR_DSURFACE_VIEWBOUNDS 0x00886FA0u
#define ADDR_BITFONT_INSTANCE    0x0089C4D0u
#define ADDR_COLORSCHEME_ARRAY   0x00B054D0u
#define ADDR_CURRENT_FRAME       0x00A8ED84u
#define ADDR_SCHEME_TO_COLOR     0x00517440u
#define ADDR_AFTER_MSGLIST_DRAW  0x005D4A9Bu

#define OFF_REG_ESP              0x14
#define OFF_REG_ECX              0x20
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

#define VT_FILL_RECT             0x14
#define VT_FILL_RECT_TRANS       0x1C
#define VT_SET_PIXEL             0x24
#define VT_GET_PIXEL             0x28
#define VT_LOCK                  0x5C
#define VT_UNLOCK                0x60
#define VT_GET_BPP               0x70
#define VT_GET_PITCH             0x74
#define VT_GET_WIDTH             0x7C
#define VT_GET_HEIGHT            0x80

/* Point2D / RectangleStruct / ColorStruct 见 GdiText.h */

/* ---------------- 配置 ---------------- */

typedef struct {
    int Enable;
    int FontHeight;    /* 0 = 跟 BitFont 字高 */
    int Bold;          /* 1 = 粗体 */
    char FontFace[64]; /* 字体文件 (系统字体文件夹) 或字体名, 缺省 msyh.ttc */
    int Shadow;        /* 1 = 画阴影 */
    int ShadowDX;      /* 阴影 X 偏移像素 */
    int ShadowDY;      /* 阴影 Y 偏移像素 */
    int ShadowOpacity; /* 阴影不透明度 0~100 */
    int MaxLines;
    int BoardOpacity;
    int PosX;
    int PosY;
    int TopGapLines;
    int MaxWidth;      /* >1 = 像素; 0 = 自动半屏 (小数走 MaxWidthPm) */
    int MaxWidthPm;    /* >0 = 按屏宽千分比 (1~1000, 如500=50%), 优先于 MaxWidth */
    int CampBottom;    /* 1 = 底部显示 (底边固定, 文本向上增长), 0 = 顶部显示 */
    int CampBottomMargin; /* 底部模式时距底边像素 */
    int CampCentered;  /* 1 = 水平居中, 0 = 左对齐 (PosX) */
    int LogMessages;
    int ShowHotZone;   /* 1 = 画出悬停生效范围 (调试悬停用) */
    int ScrollStep;    /* 滚轮步长(行) */
    int ShadowSize;    /* 阴影粗细 (1=单层, >1 加厚) */
    int HintShow;      /* 1 = 左下显示操作提示框 (sp=1) */
    int HintPosX;
    int HintBottom;    /* 距底边像素 */
    int HintFontHeight;/* 0 = 跟 BitFont 字高 */
    int HintBold;
    char HintFontFace[64]; /* 同上 (提示框用), 缺省 msyh.ttc */
    int HintColorFollow;/* 1 = 追随玩家实际颜色 (原版色), 0 = 白字 */
    int HintMaxLines;
    int HintMaxWidth;  /* 0 = 自动 */
    int HintOpacity;   /* 底板不透明度 0~100; 0=不画底板但文字阴影照画 */
    int HintTimeoutMul;/* 提示显示时长倍率 (1=原版速度) */
} MsgFontConfig;

static MsgFontConfig g_Cfg;

/* ---------------- 超武面板 (右下整块重绘, 跟战役框同一管线) ----------------
 * 原版列表由 Print 入口整体隐藏+收集 (见 VecText, 不管内容细节不逐条画),
 * 此处每帧从收集表取新鲜行画统一面板: 单个半透明底 + 右/底边距锚定右下 +
 * 名左/计时器右分栏 + Ready 变绿。收集不到新鲜行 (菜单/无超武) 即隐藏,
 * 不可能画到别处。 */
#define SWPANEL_SECTION "SWPanel"

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
} SWPanelCfg;

static SWPanelCfg g_SW;
static wchar_t g_SWFace[FONT_FACE_MAX];
/* g_SWPanelOn 定义在 VecText.cpp (收集门), 此处经 VecText.h extern 使用 */



/* 解析 MaxWidth: 整数=像素; 含小数点则按屏宽百分比走,
 * 返回像素值, 百分比时返回 0 并写 *pmille (千分比 1~1000)。
 * 无 CRT, 手写解析 (只认可选符号/数字/一个小数点)。 */
static int ParseWidth(const char* s, int* pmille)
{
    int intPart = 0, frac = 0, fdiv = 1, seenDot = 0, seenDigit = 0, neg = 0;
    *pmille = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s)
    {
        if (*s == '.') { if (seenDot) break; seenDot = 1; }
        else if (*s >= '0' && *s <= '9')
        {
            seenDigit = 1;
            if (!seenDot) intPart = intPart * 10 + (*s - '0');
            else if (fdiv < 1000) { fdiv *= 10; frac = frac * 10 + (*s - '0'); }
        }
        else break;
        s++;
    }
    if (!seenDigit) return 0;
    if (seenDot)
    {
        int pm = intPart * 1000 + frac * (1000 / fdiv);
        if (pm < 1) pm = 1;
        if (pm > 1000) pm = 1000;
        *pmille = pm;
        return 0;
    }
    return neg ? -intPart : intPart;
}

static wchar_t g_CampFace[FONT_FACE_MAX];
static wchar_t g_HintFace[FONT_FACE_MAX];


static void LoadConfig(void)
{
    g_Cfg.Enable      = GetPrivateProfileIntA(MF_SECTION, "Enable", 1, g_IniPath);
    g_Cfg.FontHeight  = GetPrivateProfileIntA(MF_SECTION, "FontHeight", 0, g_IniPath);
    g_Cfg.Bold        = GetPrivateProfileIntA(MF_SECTION, "Bold", 0, g_IniPath);
    GetPrivateProfileStringA(MF_SECTION, "FontFace", "msyh.ttc",
                             g_Cfg.FontFace, sizeof(g_Cfg.FontFace), g_IniPath);
    g_Cfg.FontFace[sizeof(g_Cfg.FontFace) - 1] = 0;
    g_Cfg.Shadow      = GetPrivateProfileIntA(MF_SECTION, "Shadow", 1, g_IniPath);
    g_Cfg.ShadowDX    = GetPrivateProfileIntA(MF_SECTION, "ShadowDX", 1, g_IniPath);
    g_Cfg.ShadowDY    = GetPrivateProfileIntA(MF_SECTION, "ShadowDY", 1, g_IniPath);
    g_Cfg.ShadowOpacity = GetPrivateProfileIntA(MF_SECTION, "ShadowOpacity", 60, g_IniPath);
    g_Cfg.MaxLines    = GetPrivateProfileIntA(MF_SECTION, "MaxLines", 8, g_IniPath);
    g_Cfg.BoardOpacity= GetPrivateProfileIntA(MF_SECTION, "BoardOpacity", 45, g_IniPath);
    g_Cfg.PosX        = GetPrivateProfileIntA(MF_SECTION, "PosX", 8, g_IniPath);
    g_Cfg.PosY        = GetPrivateProfileIntA(MF_SECTION, "PosY", 8, g_IniPath);
    g_Cfg.TopGapLines = GetPrivateProfileIntA(MF_SECTION, "TopGapLines", 1, g_IniPath);
    {   /* MaxWidth: 整数=像素, 0=自动; 0~1小数=按屏宽百分比 */
        char wb[32];
        int pm = 0;
        GetPrivateProfileStringA(MF_SECTION, "MaxWidth", "0", wb, sizeof(wb), g_IniPath);
        g_Cfg.MaxWidth = ParseWidth(wb, &pm);
        g_Cfg.MaxWidthPm = pm;
    }
    g_Cfg.CampBottom = GetPrivateProfileIntA(MF_SECTION, "CampBottom", 0, g_IniPath);
    g_Cfg.CampBottomMargin = GetPrivateProfileIntA(MF_SECTION, "CampBottomMargin", 8, g_IniPath);
    g_Cfg.CampCentered = GetPrivateProfileIntA(MF_SECTION, "CampCentered", 0, g_IniPath);
    g_Cfg.LogMessages = GetPrivateProfileIntA(MF_SECTION, "LogMessages", 0, g_IniPath);
    g_Cfg.ShowHotZone = GetPrivateProfileIntA(MF_SECTION, "ShowHotZone", 1, g_IniPath);
    g_Cfg.ScrollStep  = GetPrivateProfileIntA(MF_SECTION, "ScrollStep", 3, g_IniPath);
    g_Cfg.ShadowSize  = GetPrivateProfileIntA(MF_SECTION, "ShadowSize", 1, g_IniPath);
    g_Cfg.HintShow    = GetPrivateProfileIntA(MF_SECTION, "HintShow", 1, g_IniPath);
    g_Cfg.HintPosX    = GetPrivateProfileIntA(MF_SECTION, "HintPosX", 8, g_IniPath);
    g_Cfg.HintBottom  = GetPrivateProfileIntA(MF_SECTION, "HintBottom", 8, g_IniPath);
    g_Cfg.HintFontHeight = GetPrivateProfileIntA(MF_SECTION, "HintFontHeight", 14, g_IniPath);
    g_Cfg.HintBold    = GetPrivateProfileIntA(MF_SECTION, "HintBold", 0, g_IniPath);
    GetPrivateProfileStringA(MF_SECTION, "HintFontFace", "msyh.ttc",
                             g_Cfg.HintFontFace, sizeof(g_Cfg.HintFontFace), g_IniPath);
    g_Cfg.HintFontFace[sizeof(g_Cfg.HintFontFace) - 1] = 0;
    g_Cfg.HintColorFollow = GetPrivateProfileIntA(MF_SECTION, "HintColorFollow", 0, g_IniPath);
    g_Cfg.HintMaxLines= GetPrivateProfileIntA(MF_SECTION, "HintMaxLines", 3, g_IniPath);
    g_Cfg.HintMaxWidth= GetPrivateProfileIntA(MF_SECTION, "HintMaxWidth", 320, g_IniPath);
    g_Cfg.HintOpacity = GetPrivateProfileIntA(MF_SECTION, "HintOpacity", 45, g_IniPath);
    g_Cfg.HintTimeoutMul = GetPrivateProfileIntA(MF_SECTION, "HintTimeoutMul", 2, g_IniPath);

    g_Cfg.Enable = g_Cfg.Enable ? 1 : 0;
    g_Cfg.LogMessages = g_Cfg.LogMessages ? 1 : 0;
    g_Cfg.ShowHotZone = g_Cfg.ShowHotZone ? 1 : 0;
    g_Cfg.Bold = g_Cfg.Bold ? 1 : 0;
    g_Cfg.Shadow = g_Cfg.Shadow ? 1 : 0;
    if (g_Cfg.ShadowDX < 0) g_Cfg.ShadowDX = 0;
    if (g_Cfg.ShadowDX > 8) g_Cfg.ShadowDX = 8;
    if (g_Cfg.ShadowDY < 0) g_Cfg.ShadowDY = 0;
    if (g_Cfg.ShadowDY > 8) g_Cfg.ShadowDY = 8;
    if (g_Cfg.ShadowOpacity < 0) g_Cfg.ShadowOpacity = 0;
    if (g_Cfg.ShadowOpacity > 100) g_Cfg.ShadowOpacity = 100;
    if (g_Cfg.FontHeight < 0) g_Cfg.FontHeight = 0;
    if (g_Cfg.FontHeight > 64) g_Cfg.FontHeight = 64;
    if (g_Cfg.MaxLines < 1) g_Cfg.MaxLines = 1;
    if (g_Cfg.MaxLines > 40) g_Cfg.MaxLines = 40;
    if (g_Cfg.BoardOpacity < 0) g_Cfg.BoardOpacity = 0;
    if (g_Cfg.BoardOpacity > 100) g_Cfg.BoardOpacity = 100;
    if (g_Cfg.MaxWidth < 0) g_Cfg.MaxWidth = 0;
    g_Cfg.CampBottom = g_Cfg.CampBottom ? 1 : 0;
    if (g_Cfg.CampBottomMargin < 0) g_Cfg.CampBottomMargin = 0;
    if (g_Cfg.CampBottomMargin > 2000) g_Cfg.CampBottomMargin = 2000;
    g_Cfg.CampCentered = g_Cfg.CampCentered ? 1 : 0;
    if (g_Cfg.TopGapLines < 0) g_Cfg.TopGapLines = 0;
    if (g_Cfg.TopGapLines > 10) g_Cfg.TopGapLines = 10;
    if (g_Cfg.ScrollStep < 1) g_Cfg.ScrollStep = 1;
    if (g_Cfg.ScrollStep > 50) g_Cfg.ScrollStep = 50;
    if (g_Cfg.ShadowSize < 1) g_Cfg.ShadowSize = 1;
    if (g_Cfg.ShadowSize > 3) g_Cfg.ShadowSize = 3;
    g_Cfg.HintShow = g_Cfg.HintShow ? 1 : 0;
    if (g_Cfg.HintPosX < 0) g_Cfg.HintPosX = 0;
    if (g_Cfg.HintBottom < 0) g_Cfg.HintBottom = 0;
    if (g_Cfg.HintFontHeight < 0) g_Cfg.HintFontHeight = 0;
    if (g_Cfg.HintFontHeight > 64) g_Cfg.HintFontHeight = 64;
    g_Cfg.HintBold = g_Cfg.HintBold ? 1 : 0;
    g_Cfg.HintColorFollow = g_Cfg.HintColorFollow ? 1 : 0;
    if (g_Cfg.HintMaxLines < 1) g_Cfg.HintMaxLines = 1;
    if (g_Cfg.HintMaxLines > 10) g_Cfg.HintMaxLines = 10;
    if (g_Cfg.HintMaxWidth < 0) g_Cfg.HintMaxWidth = 0;
    if (g_Cfg.HintOpacity < 0) g_Cfg.HintOpacity = 0;
    if (g_Cfg.HintOpacity > 100) g_Cfg.HintOpacity = 100;
    if (g_Cfg.HintTimeoutMul < 1) g_Cfg.HintTimeoutMul = 1;
    if (g_Cfg.HintTimeoutMul > 10) g_Cfg.HintTimeoutMul = 10;

    g_SW.Enable = GetPrivateProfileIntA(SWPANEL_SECTION, "Enable", 1, g_IniPath);
    g_SW.FontPx = GetPrivateProfileIntA(SWPANEL_SECTION, "FontPx", 16, g_IniPath);
    g_SW.Bold = GetPrivateProfileIntA(SWPANEL_SECTION, "Bold", 0, g_IniPath);
    GetPrivateProfileStringA(SWPANEL_SECTION, "FontFace", "msyh.ttc",
                             g_SW.FontFace, sizeof(g_SW.FontFace), g_IniPath);
    g_SW.FontFace[sizeof(g_SW.FontFace) - 1] = 0;
    g_SW.Opacity = GetPrivateProfileIntA(SWPANEL_SECTION, "Opacity", 60, g_IniPath);
    g_SW.RightMargin = GetPrivateProfileIntA(SWPANEL_SECTION, "RightMargin", 8, g_IniPath);
    g_SW.BottomMargin = GetPrivateProfileIntA(SWPANEL_SECTION, "BottomMargin", 8, g_IniPath);
    g_SW.PadX = GetPrivateProfileIntA(SWPANEL_SECTION, "PadX", 6, g_IniPath);
    g_SW.PadY = GetPrivateProfileIntA(SWPANEL_SECTION, "PadY", 2, g_IniPath);
    g_SW.Gap = GetPrivateProfileIntA(SWPANEL_SECTION, "Gap", 12, g_IniPath);
    g_SW.Classic = GetPrivateProfileIntA(SWPANEL_SECTION, "Classic", 0, g_IniPath);

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
    g_SWPanelOn = g_SW.Enable;
}

static int g_InitDone = 0;

static void EnsureInit(void)
{
    char b1[16], buf[256];
    unsigned p;
    if (g_InitDone) return;
    g_InitDone = 1;
    GtEnsureLog();
    if (!g_Log) return;
    LoadConfig();
    LogLine("==================================================");
    LogLine("MsgFont " MF_VERSION " start");
    p = 0;
    p = AppendCStr(buf, p, "config: MaxLines=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.MaxLines, buf + p);
    p = AppendCStr(buf, p, " FontHeight=", sizeof(buf));
    UtoA((unsigned)g_Cfg.FontHeight, b1);
    p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " (0 = follow BitFont)", sizeof(buf));
    LogLine(buf);
    /* 启动回显关键值 (ini 改后必须重启游戏, 配置只读一次) */
    p = 0;
    p = AppendCStr(buf, p, "config: HintOp=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.HintOpacity, buf + p);
    p = AppendCStr(buf, p, " BoardOp=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.BoardOpacity, buf + p);
    p = AppendCStr(buf, p, " ShowHZ=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.ShowHotZone, buf + p);
    p = AppendCStr(buf, p, " MaxW=", sizeof(buf));
    if (g_Cfg.MaxWidthPm > 0)
    {
        p += UtoA((unsigned)g_Cfg.MaxWidthPm, buf + p);
        p = AppendCStr(buf, p, "/1000", sizeof(buf));
    }
    else p += UtoA((unsigned)g_Cfg.MaxWidth, buf + p);
    p = AppendCStr(buf, p, " Shadow=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.Shadow, buf + p);
    p = AppendCh(buf, p, '/', sizeof(buf));
    p += UtoA((unsigned)g_Cfg.ShadowOpacity, buf + p);
    LogLine(buf);
    /* 字体解析放日志打开之后, 解析结果才能记入日志 */
    ResolveFontFace(g_Cfg.FontFace, "camp", g_CampFace, L"Microsoft YaHei");
    ResolveFontFace(g_Cfg.HintFontFace, "hint", g_HintFace, L"Microsoft YaHei");
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

/* ---------------- 消息存储 ---------------- */

#define MAX_MSGS   200
#define MAX_TEXT_W 512
#define MAX_NAME_W 64

typedef struct {
    wchar_t  text[MAX_TEXT_W];
    wchar_t  name[MAX_NAME_W];
    int      color;
    unsigned expire;   /* 引擎式过期戳 (毫秒/16 时钟); 0 = 永不过期 */
    unsigned born;     /* 入队时时钟 (提示框倍率延长用) */
    int      sp;        /* 1 = 操作提示类 (快捷键/路径点), 0 = 战役/聊天/系统 */
} ChatMsg;

#define MAX_CAMP_MSGS 200
#define MAX_HINT_MSGS 64

/* 双流分离存储: 战役/提示各用各的环, 快捷键刷屏再也顶不掉战役历史 */
typedef struct {
    ChatMsg* buf;
    int cap;
    int head;
    int count;
} MsgRing;

static ChatMsg g_CampBuf[MAX_CAMP_MSGS];
static ChatMsg g_HintBuf[MAX_HINT_MSGS];
static MsgRing g_CampRing = { g_CampBuf, MAX_CAMP_MSGS, 0, 0 };
static MsgRing g_HintRing = { g_HintBuf, MAX_HINT_MSGS, 0, 0 };

static ChatMsg* MsgAt(MsgRing* r, int i)
{
    int idx = r->head - r->count + i;
    while (idx < 0) idx += r->cap;
    return &r->buf[idx % r->cap];
}

static void CopyW(wchar_t* dst, const wchar_t* src, int cap)
{
    int i = 0;
    if (!src || !IsPlausiblePtr(src)) { dst[0] = 0; return; }
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void RingPush(MsgRing* r, const wchar_t* name, const wchar_t* text,
                     int color, unsigned expire, unsigned born, int sp)
{
    ChatMsg* m = &r->buf[r->head];
    CopyW(m->text, text, MAX_TEXT_W);
    CopyW(m->name, name, MAX_NAME_W);
    m->color  = color;
    m->expire = expire;
    m->born   = born;
    m->sp     = sp ? 1 : 0;
    r->head = (r->head + 1) % r->cap;
    if (r->count < r->cap) r->count++;
}

/* ---------------- 引擎式过期 (与 Manage 完全一致) ----------------
 *
 * 反汇编实证 (AddMessage 0x5D3DC0 / Manage 0x5D4441):
 *   时钟 = [0x887340] + (Tick() - [0x887338]), Tick = timeGetTime()>>4 (毫秒/16),
 *   [0x887338]==-1 时不做 Tick 补偿 (暂停/未启动状态)。
 *   入队: timeout==-1 → 过期戳=0 (永不过期); 否则 过期戳=时钟+timeout。
 *   存活: 过期戳==0, 或 (int)时钟 <= (int)过期戳 (引擎用 jle, 有符号比较)。
 *
 * 这就是不限速下原版消息"活得更久"的原因: 超时是墙钟, 不是逻辑帧。 */

#define ADDR_TICK_FN   0x006C8C40u
#define ADDR_CLK_REF   0x00887338u
#define ADDR_CLK_BASE  0x00887340u
#define ADDR_CLK_ARG   0x0088733Cu

typedef unsigned (__fastcall *TickFn)(void* a, void* edxUnused);

static unsigned TickNow(void)
{
    static TickFn fn = NULL;
    if (!fn) fn = (TickFn)ADDR_TICK_FN;
    return fn((void*)ADDR_CLK_ARG, NULL);
}

static unsigned ClockNow(void)
{
    unsigned ref = *(unsigned*)ADDR_CLK_REF;
    unsigned b = *(unsigned*)ADDR_CLK_BASE;
    if (ref != 0xFFFFFFFFu) b += TickNow() - ref;
    return b;
}

static unsigned ExpireStamp(int timeout)
{
    if (timeout < 0) return 0;
    return ClockNow() + (unsigned)timeout;
}

static int MsgVisibleMul(ChatMsg* m, unsigned mul)
{
    unsigned span;
    if (m->expire == 0) return 1;
    if (mul < 1) mul = 1;
    span = (m->expire - m->born) * mul;
    return (int)(ClockNow() - m->born) <= (int)span;
}

static int MsgVisible(ChatMsg* m)
{
    return MsgVisibleMul(m, 1);
}

/* ---------------- 引擎调用包装 ---------------- */

typedef int (__fastcall *GetIntFn)(void* t, void* edxUnused);

static int SurfGet(void* surf, unsigned vtOff)
{
    unsigned char* vt;
    GetIntFn fn;
    if (!surf || !IsPlausiblePtr(surf)) return -1;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return -2;
    fn = (GetIntFn)(*(void**)(vt + vtOff));
    if (!fn) return -3;
    return fn(surf, NULL);
}

static void FillRectTransVt(void* surf, RectangleStruct* pRect, ColorStruct* pColor, int opacity)
{
    unsigned char* vt;
    typedef int (__fastcall *Fn)(void*, void*, RectangleStruct*, ColorStruct*, int);
    Fn fn;
    if (!surf) return;
    vt = *(unsigned char**)surf;
    if (!vt) return;
    fn = (Fn)(*(void**)(vt + VT_FILL_RECT_TRANS));
    if (fn) fn(surf, NULL, pRect, pColor, opacity);
}

/* ColorScheme -> 显示色: 与引擎 Fancy_Text_Print_Wide (0x4A61C0) 完全一致。
 *
 * 引擎做法 (反汇编实证):
 *   ECX = scheme+0x308 (即 &BaseColor), 栈传 3 字节输出缓冲,
 *   call 0x517440 (stdcall, ret 4), 返回输出缓冲指针。
 * 输出 3 字节的通道顺序由运行时移位表 (0x8A0DD0..0x8A0DE4) 决定,
 * 启动时标定 (out[0] 用 D4/D0, out[2] 用 DC/D8, out[1] 用 E4/E0)。 */
/* 画框线 (悬停生效范围可视化): 上/下/左/右各一条 2px 半透明带 */
static void DrawOutline(void* surf, int x, int y, int w, int h,
                        unsigned char r, unsigned char g, unsigned char b, int op)
{
    RectangleStruct e;
    ColorStruct c;
    if (w < 8 || h < 8) return;
    c.R = r; c.G = g; c.B = b;
    e.X = x; e.Y = y; e.Width = w; e.Height = 2;
    FillRectTransVt(surf, &e, &c, op);
    e.Y = y + h - 2;
    FillRectTransVt(surf, &e, &c, op);
    e.Y = y + 2; e.Height = h - 4; e.Width = 2;
    FillRectTransVt(surf, &e, &c, op);
    e.X = x + w - 2;
    FillRectTransVt(surf, &e, &c, op);
}

typedef unsigned char* (__fastcall *SchemeToColorFn)(void* base, void* edxUnused, unsigned char* out3);

static int g_IdxR = 0, g_IdxG = 1, g_IdxB = 2;
static int g_MapDone = 0;

static void MapChannels(void)
{
    unsigned sL0 = *(unsigned*)0x8A0DD0u;   /* out[0] 的 shl */
    unsigned sL1 = *(unsigned*)0x8A0DD8u;   /* out[2] 的 shl */
    unsigned sL2 = *(unsigned*)0x8A0DE0u;   /* out[1] 的 shl */
    char b1[16], b2[16], b3[16], buf[256];
    unsigned p = 0;
    /* 红在高位 (565:11 / 555:10), 绿在中 (5), 蓝在低 (0) */
    if (sL0 == 11 || sL0 == 10) g_IdxR = 0;
    else if (sL1 == 11 || sL1 == 10) g_IdxR = 2;
    else if (sL2 == 11 || sL2 == 10) g_IdxR = 1;
    if (sL0 == 5) g_IdxG = 0;
    else if (sL1 == 5) g_IdxG = 2;
    else if (sL2 == 5) g_IdxG = 1;
    if (sL0 == 0) g_IdxB = 0;
    else if (sL1 == 0) g_IdxB = 2;
    else if (sL2 == 0) g_IdxB = 1;
    p = AppendCStr(buf, p, "colormap: shl=", sizeof(buf));
    UtoA(sL0, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, '/', sizeof(buf));
    UtoA(sL1, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, '/', sizeof(buf));
    UtoA(sL2, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " R=out", sizeof(buf));
    UtoA((unsigned)g_IdxR, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " G=out", sizeof(buf));
    UtoA((unsigned)g_IdxG, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
    p = AppendCStr(buf, p, " B=out", sizeof(buf));
    UtoA((unsigned)g_IdxB, b3); p = AppendCStr(buf, p, b3, sizeof(buf));
    LogLine(buf);
}

static void SchemeColor(void* scheme, ColorStruct* out)
{
    static SchemeToColorFn fn = NULL;
    static void* s_sampledSch[8];
    static int s_sampledN = 0;
    unsigned char buf[4];
    unsigned char* r;
    int si, seen = 0;
    if (!g_MapDone) { g_MapDone = 1; MapChannels(); }
    if (!scheme || !IsPlausiblePtr(scheme)) { out->R = out->G = out->B = 255; return; }
    if (!fn) fn = (SchemeToColorFn)ADDR_SCHEME_TO_COLOR;
    buf[0] = buf[1] = buf[2] = buf[3] = 0;
    r = fn((char*)scheme + OFF_CSCHEME_BASECOLOR, NULL, buf);
    if (!r || !IsPlausiblePtr(r)) { out->R = out->G = out->B = 255; return; }
    out->R = r[g_IdxR]; out->G = r[g_IdxG]; out->B = r[g_IdxB];
    if (s_sampledN < 8)
    {
        for (si = 0; si < s_sampledN; si++)
            if (s_sampledSch[si] == scheme) { seen = 1; break; }
    }
    else seen = 1;
    if (!seen)
    {
        char b1[16], b2[16], b3[16], b4[16], sb[160];
        unsigned char* base = (unsigned char*)scheme + OFF_CSCHEME_BASECOLOR;
        unsigned q = 0;
        s_sampledSch[s_sampledN++] = scheme;
        q = AppendCStr(sb, q, "colorsample: scheme=", sizeof(sb));
        q += HtoA((unsigned)(size_t)scheme, sb + q);
        q = AppendCStr(sb, q, " base=", sizeof(sb));
        UtoA(base[0], b1); q = AppendCStr(sb, q, b1, sizeof(sb));
        q = AppendCh(sb, q, ',', sizeof(sb));
        UtoA(base[1], b2); q = AppendCStr(sb, q, b2, sizeof(sb));
        q = AppendCh(sb, q, ',', sizeof(sb));
        UtoA(base[2], b3); q = AppendCStr(sb, q, b3, sizeof(sb));
        q = AppendCStr(sb, q, " conv=", sizeof(sb));
        UtoA(r[0], b1); q = AppendCStr(sb, q, b1, sizeof(sb));
        q = AppendCh(sb, q, ',', sizeof(sb));
        UtoA(r[1], b2); q = AppendCStr(sb, q, b2, sizeof(sb));
        q = AppendCh(sb, q, ',', sizeof(sb));
        UtoA(r[2], b3); q = AppendCStr(sb, q, b3, sizeof(sb));
        q = AppendCStr(sb, q, " used=", sizeof(sb));
        UtoA(out->R, b1); q = AppendCStr(sb, q, b1, sizeof(sb));
        q = AppendCh(sb, q, ',', sizeof(sb));
        UtoA(out->G, b2); q = AppendCStr(sb, q, b2, sizeof(sb));
        q = AppendCh(sb, q, ',', sizeof(sb));
        UtoA(out->B, b4); q = AppendCStr(sb, q, b4, sizeof(sb));
        LogLine(sb);
    }
}

static void* SchemePtr(int cs)
{
    void** items;
    int count;
    if (cs < 0) return NULL;
    items = *(void***)(ADDR_COLORSCHEME_ARRAY + OFF_CSCHEME_ITEMS);
    count = *(int*)(ADDR_COLORSCHEME_ARRAY + OFF_CSCHEME_COUNT);
    if (!items || count <= 0 || count > 1000 || cs >= count) return NULL;
    return items[cs];
}



/* ---------------- 排版与绘制 ---------------- */

#define MAX_PICKED 64
#define MAX_LPP    8

typedef struct { const wchar_t* start; int len; } TextLine;

static TextLine g_LineBuf[MAX_PICKED][MAX_LPP];
static int      g_LineCnt[MAX_PICKED];
static int      g_Picked[MAX_PICKED];
static int      g_RowBase[MAX_PICKED];
static wchar_t  g_Compose[MAX_PICKED][MAX_NAME_W + MAX_TEXT_W + 4];

extern "C" __declspec(dllexport) DWORD __cdecl MsgFont_AddMessageHook(void* regs){
    unsigned char* esp;
    const wchar_t* argName;
    const wchar_t* argMsg;
    unsigned argColor, argTimeout, argSP;
    char b1[16], b2[16], b3[16], buf[256];
    unsigned p;

    EnsureInit();
    if (!g_Cfg.Enable) return 0;

    esp = *(unsigned char**)((char*)regs + OFF_REG_ESP);
    if (!esp || !IsPlausiblePtr(esp)) return 0;

    argName    = *(const wchar_t**)(esp + STK_ARG_NAME);
    argMsg     = *(const wchar_t**)(esp + STK_ARG_MESSAGE);
    argColor   = *(unsigned*)(esp + STK_ARG_CS);
    argTimeout = *(unsigned*)(esp + STK_ARG_TIMEOUT);
    argSP      = *(unsigned*)(esp + STK_ARG_SP);

    {
        unsigned born = ClockNow();
        unsigned exp = ExpireStamp((int)argTimeout);
        int sp = (int)argSP ? 1 : 0;
        if (sp) RingPush(&g_HintRing, argName, argMsg, (int)argColor, exp, born, sp);
        else RingPush(&g_CampRing, argName, argMsg, (int)argColor, exp, born, sp);
    }

    if (g_Cfg.LogMessages)
    {
        p = 0;
        p = AppendCStr(buf, p, "msg: cs=", sizeof(buf));
        UtoA(argColor, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " to=", sizeof(buf));
        UtoA(argTimeout, b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " sp=", sizeof(buf));
        UtoA(argSP, b3);
        p = AppendCStr(buf, p, b3, sizeof(buf));
        LogLine(buf);
    }
    return 0;
}

/* 回退原因计数: kind 0=矢量成功 1=无可见行 2=GDI失败 3=尺寸异常 4=Lock失败 5=无画布 */
static unsigned g_StVec = 0, g_StEmpty = 0, g_StGdi = 0;
static unsigned g_StDims = 0, g_StLock = 0, g_StSurf = 0;
static unsigned g_StLastFrame = 0;

static void StatTick(unsigned frame, int kind)
{
    char b1[16], b2[16], b3[16], b4[16], b5[16], b6[16], buf[256];
    unsigned p = 0;
    if (kind == 0) g_StVec++;
    else if (kind == 1) g_StEmpty++;
    else if (kind == 2) g_StGdi++;
    else if (kind == 3) g_StDims++;
    else if (kind == 4) g_StLock++;
    else g_StSurf++;
    if (frame - g_StLastFrame < 1800) return;
    g_StLastFrame = frame;
    p = AppendCStr(buf, p, "stat: vec=", sizeof(buf));
    p += UtoA(g_StVec, buf + p);
    p = AppendCStr(buf, p, " empty=", sizeof(buf));
    UtoA(g_StEmpty, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " gdi=", sizeof(buf));
    UtoA(g_StGdi, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
    p = AppendCStr(buf, p, " dims=", sizeof(buf));
    UtoA(g_StDims, b3); p = AppendCStr(buf, p, b3, sizeof(buf));
    p = AppendCStr(buf, p, " lock=", sizeof(buf));
    UtoA(g_StLock, b4); p = AppendCStr(buf, p, b4, sizeof(buf));
    p = AppendCStr(buf, p, " surf=", sizeof(buf));
    UtoA(g_StSurf, b5); p = AppendCStr(buf, p, b5, sizeof(buf));
    (void)b6;
    LogLine(buf);
    g_StVec = g_StEmpty = g_StGdi = g_StDims = g_StLock = g_StSurf = 0;
}

/* ---------------- 双框: 战役长留 + 提示短命 ----------------
 * sp=1 (操作提示: 快捷键/路径点) 进左下提示框, 按引擎超时自然消失;
 * sp=0 (战役/聊天/系统) 进左上战役框, 忽略超时长留历史, 可翻看。 */

typedef void* (__fastcall *LockFn)(void*, void*, int, int);
typedef int (__fastcall *UnlockFn)(void*, void*);

typedef struct {
    int wantSp;
    int ignoreTimeout;
    int x;
    int yBase;
    int topGap;
    int autoBottom;
    int bottomMargin;
    int centered;    /* 1 = 水平居中, 0 = 左对齐 (x) */
    int maxW;        /* >1 = 像素; <=0 = 自动半屏; maxWPm 优先 */
    int maxWPm;      /* >0 = 按屏宽千分比 (1~1000) */
    int limit;       /* 显示行数 */
    int rowBudget;   /* 挑选行预算 (战役长留给大, 提示给小) */
    int opacity;
    int shadowNoBoard; /* 1 = 底板透明度为 0 时仍画文字阴影 (提示框用) */
    int fontPx;      /* 0 = 跟随 BitFont */
    int bold;
    int* pSkip;      /* 战役滚动 (NULL=不滚动) */
    int bar;         /* 滚动条 */
    int schemeColor; /* 1 = 用颜色方案 (原版色), 0 = 白字 */
    unsigned tmoMul; /* 超时倍率 (提示框延长用, 战役=1) */
    int slot;        /* GDI 槽位 (战役=0, 提示=1) */
    const wchar_t* face; /* 字体名 (g_CampFace/g_HintFace) */
    MsgRing* ring;   /* 专属消息环 (双流分离存储) */
} BoxSpec;

static int g_Skip = 0;          /* 0 = 跟随最新 */
static int s_prevCampTotal = 0;

/* 鼠标位置 (游戏绘制坐标, 与 Composite 同系, ChatBox 同用法) */
#define ADDR_WWMOUSE_INSTANCE 0x00887640u
#define OFF_WWMOUSE_XY1       0x1C

/* 游戏窗口客户区尺寸 (鼠标是窗口像素, 画布是游戏分辨率像素,
 * cnc-ddraw 拉伸呈现, 必须映射; 否则命中判定整体偏右下) */
static HWND g_GameWnd = NULL;
static int g_WinW = 0, g_WinH = 0;
static int g_SurfW = 0, g_SurfH = 0;

static int __stdcall EnumGameWnd(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    RECT rc;
    (void)lp;
    if (!IsWindowVisible(h)) return 1;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return 1;
    if (!GetClientRect(h, &rc)) return 1;
    if (rc.right < 100 || rc.bottom < 100) return 1;
    *(HWND*)lp = h;
    return 0;
}

static void RefreshWinSize(unsigned tick)
{
    static unsigned s_last = 0;
    RECT rc;
    if (tick - s_last < 60) return;
    s_last = tick;
    if (!g_GameWnd || !IsWindow(g_GameWnd))
    {
        g_GameWnd = NULL;
        EnumWindows((WNDENUMPROC)EnumGameWnd, (LPARAM)&g_GameWnd);
    }
    if (g_GameWnd && GetClientRect(g_GameWnd, &rc) && rc.right > 100 && rc.bottom > 100)
    {
        if (rc.right != g_WinW || rc.bottom != g_WinH)
        {
            char wb[128], w1[16], w2[16];
            unsigned q = 0;
            g_WinW = rc.right; g_WinH = rc.bottom;
            q = AppendCStr(wb, q, "win: client=", sizeof(wb));
            UtoA((unsigned)g_WinW, w1); q = AppendCStr(wb, q, w1, sizeof(wb));
            q = AppendCh(wb, q, 'x', sizeof(wb));
            UtoA((unsigned)g_WinH, w2); q = AppendCStr(wb, q, w2, sizeof(wb));
            LogLine(wb);
        }
    }
}

static int MouseXY(int* x, int* y)
{
    void* m = *(void**)ADDR_WWMOUSE_INSTANCE;
    int mx, my;
    static int s_mapLogged = 0;
    if (!m || !IsPlausiblePtr(m)) return 0;
    mx = *(int*)((char*)m + OFF_WWMOUSE_XY1 + 0);
    my = *(int*)((char*)m + OFF_WWMOUSE_XY1 + 4);
    if (g_WinW > 100 && g_WinH > 100 && g_SurfW > 100 && g_SurfH > 100)
    {
        int sx = mx * g_SurfW / g_WinW;
        int sy = my * g_SurfH / g_WinH;
        if (!s_mapLogged)
        {
            char mb[160], m1[16], m2[16], m3[16], m4[16];
            unsigned q = 0;
            s_mapLogged = 1;
            q = AppendCStr(mb, q, "mouse: raw=", sizeof(mb));
            UtoA((unsigned)(mx < 0 ? 0 : mx), m1); q = AppendCStr(mb, q, m1, sizeof(mb));
            q = AppendCh(mb, q, ',', sizeof(mb));
            UtoA((unsigned)(my < 0 ? 0 : my), m2); q = AppendCStr(mb, q, m2, sizeof(mb));
            q = AppendCStr(mb, q, " mapped=", sizeof(mb));
            UtoA((unsigned)(sx < 0 ? 0 : sx), m3); q = AppendCStr(mb, q, m3, sizeof(mb));
            q = AppendCh(mb, q, ',', sizeof(mb));
            UtoA((unsigned)(sy < 0 ? 0 : sy), m4); q = AppendCStr(mb, q, m4, sizeof(mb));
            LogLine(mb);
        }
        *x = sx; *y = sy;
    }
    else { *x = mx; *y = my; }
    return 1;
}

/* 战役框上一帧矩形 (滚轮命中 + 悬停判定用) */
static RectangleStruct g_CampRect;
static int g_CampValid = 0;

/* 触发条几何与顶部战役文本同源 (见 CampTriggerRect): 左右对齐、1 行高。
 * 悬停/滚轮命中沿用同一判定, 不再使用固定的 340x240 大热角。 */

static int MouseInCampRect(int mx, int my)
{
    if (!g_CampValid) return 0;
    return (mx >= g_CampRect.X - 4 && mx < g_CampRect.X + g_CampRect.Width + 6 &&
            my >= g_CampRect.Y - 4 && my < g_CampRect.Y + g_CampRect.Height + 4);
}

/* 文本是否有可见内容 (名字非空也算; 空白/换行不算) */
static int TextHasContent(const wchar_t* s)
{
    while (*s)
    {
        wchar_t c = *s++;
        if (c != L' ' && c != L'\t' && c != L'\n' && c != L'\r') return 1;
    }
    return 0;
}

static int MsgHasContent(ChatMsg* m)
{
    if (m->name[0]) return 1;
    return TextHasContent(m->text);
}

/* 战役环里是否有可渲染的历史 (有内容的才算; 空记录不算——
 * 否则一条空系统消息招出绿框, 悬停展开却一片空白) */
static int HasCampHistory(void)
{
    int i;
    for (i = g_CampRing.count - 1; i >= 0; i--)
        if (MsgHasContent(MsgAt(&g_CampRing, i))) return 1;
    return 0;
}

/* 是否还有活着的战役消息 (不过期检查只做可见性, 不排版, 便宜) */
static int HasLiveCamp(void)
{
    int i, n = 0;
    for (i = g_CampRing.count - 1; i >= 0 && n < 64; i--, n++)
    {
        ChatMsg* m = MsgAt(&g_CampRing, i);
        /* 无内容消息不算存活: 否则一条永不过期的空消息会让盒子永远"活着",
         * 表现为"字都没了但绿框也不出" */
        if (m->sp == 0 && MsgHasContent(m) && MsgVisible(m)) return 1;
    }
    return 0;
}

/* 存活原因: 0=无 1=有计时内消息 2=仅有永不过期消息 */
static int LiveKind(void)
{
    int i, n = 0, eternal = 0;
    for (i = g_CampRing.count - 1; i >= 0 && n < 64; i--, n++)
    {
        ChatMsg* m = MsgAt(&g_CampRing, i);
        if (m->sp != 0 || !MsgHasContent(m)) continue;
        if (m->expire == 0) eternal = 1;
        else if (MsgVisible(m)) return 1;
    }
    return eternal ? 2 : 0;
}

/* 提示框显隐跳变日志 (newestExpIn = 最新提示距引擎过期还剩多少 tick, 62.5 tick≈1秒;
 * 若 hide 时它还很大, 就是提前强制消失的实证) */
static void LogHintWhy(int on)
{
    char sb[160], t1[16];
    unsigned q = 0;
    int i, n, found = 0, expIn = 0;
    for (i = g_HintRing.count - 1, n = 0; i >= 0 && n < 16; i--, n++)
    {
        ChatMsg* m = MsgAt(&g_HintRing, i);
        if (m->sp != 1) continue;
        found = 1;
        /* 与 MsgVisibleMul 同口径 (span=(expire-born)*mul), 否则 hide 行的
         * 剩余数对不上真实判定 (曾报出 -241 虚惊一场) */
        if (!m->expire) expIn = -1;
        else expIn = (int)((m->expire - m->born) * (unsigned)g_Cfg.HintTimeoutMul) -
                     (int)(ClockNow() - m->born);
        break;
    }
    q = AppendCStr(sb, q, on ? "hint: show newestExpIn=" : "hint: hide newestExpIn=", sizeof(sb));
    if (!found) q = AppendCStr(sb, q, "none", sizeof(sb));
    else if (expIn < 0)
    {
        q = AppendCh(sb, q, '-', sizeof(sb));
        UtoA((unsigned)(-expIn), t1); q = AppendCStr(sb, q, t1, sizeof(sb));
    }
    else { UtoA((unsigned)expIn, t1); q = AppendCStr(sb, q, t1, sizeof(sb)); }
    LogLine(sb);
}

/* 挑选+折行 (用当前 GDI 字体度量), 结果进 g_LineBuf/g_Picked;
 * picked 条数写 *pPicked, 返回总行数。 */
static int PickLines(MsgRing* R, int wantSp, int ignoreTimeout, unsigned tmoMul,
                     int textW, int rowBudget, int* pPicked)
{
    int picked = 0, rows = 0, i, k, total = 0;
    for (i = R->count - 1; i >= 0 && picked < MAX_PICKED && rows < rowBudget; i--)
    {
        ChatMsg* m = MsgAt(R, i);
        const wchar_t* body;
        const wchar_t* seg;
        int cnt = 0;
        if (m->sp != wantSp) continue;
        if (!ignoreTimeout && !MsgVisibleMul(m, tmoMul)) continue;
        if (m->name[0])
        {
            wchar_t* comp = g_Compose[picked];
            int pp = 0, q = 0;
            while (m->name[q] && pp < MAX_NAME_W + MAX_TEXT_W) comp[pp++] = m->name[q++];
            if (pp < MAX_NAME_W + MAX_TEXT_W - 2) { comp[pp++] = L':'; comp[pp++] = L' '; }
            q = 0;
            while (m->text[q] && pp < MAX_NAME_W + MAX_TEXT_W - 1) comp[pp++] = m->text[q++];
            comp[pp] = 0;
            body = comp;
        }
            else body = m->text;
            if (!TextHasContent(body) && !m->name[0]) continue;
        seg = body;
        while (*seg && cnt < MAX_LPP && rows < rowBudget)
        {
            int segLen = 0;
            int fit;
            while (seg[segLen] && seg[segLen] != L'\n') segLen++;
            if (segLen == 0) { seg++; continue; }
            fit = CharsFit(seg, segLen, textW);
            if (fit <= 0) fit = 1;
            if (fit > segLen) fit = segLen;
            g_LineBuf[picked][cnt].start = seg;
            g_LineBuf[picked][cnt].len = fit;
            cnt++; rows++;
            seg += fit;
            if (*seg == L'\n') seg++;
        }
        if (cnt <= 0) continue;
        g_LineCnt[picked] = cnt;
        g_Picked[picked] = i;
        picked++;
    }
    *pPicked = picked;
    for (k = 0; k < picked; k++) total += g_LineCnt[k];
    return total;
}

/* 画一个框。返回 1=画出, 0=无内容, -1=GDI失败, -2=Lock失败。 */
static int DrawBox(void* surf, BoxSpec* B, int bitLineH, int screenW,
                   int surfW, int surfH, int surfPitch)
{
    int fontPx, adv, maxW, textW, x, y;
    int picked = 0, totalAll, k, r, rowCursor, rowBegin, rowEnd, limit, totalRows, skip = 0;
    int drawn = 0;
    RectangleStruct rect;
    ColorStruct board;
    unsigned char* locked;
    unsigned char* vt;
    LockFn lockFn;
    UnlockFn unlockFn;

    fontPx = B->fontPx > 0 ? B->fontPx : bitLineH;
    if (!GdiEnsure(fontPx, B->bold, B->slot, B->face)) return -1;
    adv = g_G->adv > 0 ? g_G->adv : bitLineH;

    maxW = B->maxW;
    if (B->maxWPm > 0) maxW = screenW * B->maxWPm / 1000;   /* 百分比宽度 */
    else if (maxW <= 0) maxW = screenW / 2;                 /* 自动半屏 */
    if (maxW > screenW - 170) maxW = screenW - 170;         /* 上限: 屏宽-170 */
    x = B->centered ? (screenW - maxW) / 2 : B->x;             /* 居中或左对齐 */
    if (maxW < 80) maxW = 80;
    if (maxW > screenW - x - 8) maxW = screenW - x - 8;
    if (maxW < 80) maxW = 80;
    textW = maxW;
    if (textW < 40) textW = 40;

    totalAll = PickLines(B->ring, B->wantSp, B->ignoreTimeout, B->tmoMul, textW, B->rowBudget, &picked);
    if (picked <= 0 || totalAll <= 0) return 0;
    limit = B->limit;
    if (limit > totalAll) limit = totalAll;

    if (B->pSkip)
    {
        if (g_Skip > 0 && totalAll > s_prevCampTotal) g_Skip += totalAll - s_prevCampTotal;
        s_prevCampTotal = totalAll;
        if (g_Skip > totalAll - limit) g_Skip = totalAll - limit;
        if (g_Skip < 0) g_Skip = 0;
        skip = g_Skip;
    }

    rowCursor = totalAll;
    for (k = 0; k < picked; k++) { rowCursor -= g_LineCnt[k]; g_RowBase[k] = rowCursor; }
    rowEnd = totalAll - skip;
    rowBegin = rowEnd - limit;
    if (rowBegin < 0) rowBegin = 0;
    totalRows = rowEnd - rowBegin;
    if (totalRows <= 0) return 0;

    if (B->autoBottom) y = surfH - B->bottomMargin - (totalRows * adv + 4) + 2;
    else y = B->yBase + B->topGap * adv;

    rect.X = x - 3;
    rect.Y = y - 2;
    rect.Width = maxW + 6;
    rect.Height = totalRows * adv + 4;
    board.R = 0; board.G = 0; board.B = 0;
    /* 透明度 0 = 不画底板 (战役框同时省阴影; 提示框阴影照画, 见下) */
    if (B->opacity > 0)
        FillRectTransVt(surf, &rect, &board, B->opacity);

    vt = *(unsigned char**)surf;
    lockFn = (LockFn)(*(void**)(vt + VT_LOCK));
    unlockFn = (UnlockFn)(*(void**)(vt + VT_UNLOCK));
    if (!lockFn || !unlockFn) return -2;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return -2;

    for (k = 0; k < picked && drawn < totalRows; k++)
    {
        ChatMsg* m = MsgAt(B->ring, g_Picked[k]);
        ColorStruct fg;
        void* scheme = NULL;
        if (B->schemeColor)
        {
            scheme = SchemePtr(m->color);
            if (!scheme) scheme = SchemePtr(0);
        }
        if (scheme) SchemeColor(scheme, &fg);
        else { fg.R = fg.G = fg.B = 255; }

        for (r = 0; r < g_LineCnt[k]; r++)
        {
            int globalRow = g_RowBase[k] + r;
            int ly, w;
            if (globalRow < rowBegin || globalRow >= rowEnd) continue;
            ly = y + (globalRow - rowBegin) * adv;
            w = RenderLine(g_LineBuf[k][r].start, g_LineBuf[k][r].len);
            if (w <= 0) { drawn++; continue; }
            /* 阴影 (战役框底板透明度 0 时跳过; 提示框 0 也画; ShadowSize>1 加厚) */
            if (g_Cfg.Shadow && g_Cfg.ShadowOpacity > 0 &&
                (B->opacity > 0 || B->shadowNoBoard))
            {
                int ox, oy;
                for (oy = 0; oy < g_Cfg.ShadowSize; oy++)
                    for (ox = 0; ox < g_Cfg.ShadowSize; ox++)
                        BlitCoverage(locked, surfPitch, surfW, surfH,
                                     x + g_Cfg.ShadowDX + ox, ly + g_Cfg.ShadowDY + oy,
                                     w + 2, adv, 0, 0, 0, (unsigned)g_Cfg.ShadowOpacity);
            }
            BlitCoverage(locked, surfPitch, surfW, surfH, x, ly, w + 2, adv, fg.R, fg.G, fg.B, 100);
            drawn++;
        }
    }

    unlockFn(surf, NULL);

    /* 记下战役框矩形 (滚轮命中 + 悬停用) */
    if (B->pSkip) { g_CampRect = rect; g_CampValid = 1; }

    if (B->bar && totalAll > limit)
    {
        int maxSkip = totalAll - limit;
        int barX = rect.X + rect.Width + 1;
        int barY = rect.Y + 2;
        int barH = rect.Height - 4;
        if (barH > 12)
        {
            RectangleStruct bar;
            ColorStruct c;
            int thumbH = barH * limit / totalAll;
            int thumbY;
            if (thumbH < 8) thumbH = 8;
            if (thumbH > barH) thumbH = barH;
            thumbY = (maxSkip > 0) ? barY + (barH - thumbH) * (maxSkip - skip) / maxSkip
                                   : barY + barH - thumbH;
            bar.X = barX; bar.Y = barY; bar.Width = 4; bar.Height = barH;
            c.R = 30; c.G = 30; c.B = 30;
            FillRectTransVt(surf, &bar, &c, 55);
            bar.Y = thumbY; bar.Height = thumbH;
            c.R = 170; c.G = 170; c.B = 170;
            FillRectTransVt(surf, &bar, &c, 90);
        }
    }

    return 1;
}

/* 触发条几何: 与战役文本框同 x / 同宽、1 行高。
 * 与 DrawBox 的战役分支同口径 (rect.X=x-3, rect.Y=y-2,
 * rect.Width=maxW+6, 高=adv+4), 只是行数固定为 1。
 * 底部模式时与 DrawBox 的 autoBottom 同公式 (totalRows=1),
 * 底边固定在 surfH-bottomMargin, 文本向上增长。
 * 返回战役字体行高 adv (GDI 未就绪时回退估算)。 */
static int CampTriggerRect(int bitLineH, int screenW, int surfH, RectangleStruct* out)
{
    int fontPx, adv, maxW, x, y;
    fontPx = g_Cfg.FontHeight > 0 ? g_Cfg.FontHeight : bitLineH;
    if (fontPx <= 0) fontPx = bitLineH;
    if (GdiEnsure(fontPx, g_Cfg.Bold, 0, g_CampFace))
        adv = (g_G->adv > 0 ? g_G->adv : fontPx + 2);
    else
        adv = fontPx + 2;
    maxW = g_Cfg.MaxWidth;
    if (g_Cfg.MaxWidthPm > 0) maxW = screenW * g_Cfg.MaxWidthPm / 1000;
    else if (maxW <= 0) maxW = screenW / 2;
    if (maxW > screenW - 170) maxW = screenW - 170;
    x = g_Cfg.CampCentered ? (screenW - maxW) / 2 : g_Cfg.PosX;
    if (maxW < 80) maxW = 80;
    if (maxW > screenW - x - 8) maxW = screenW - x - 8;
    if (maxW < 80) maxW = 80;
    if (g_Cfg.CampBottom)
        y = surfH - g_Cfg.CampBottomMargin - (adv + 4) + 2;
    else
        y = g_Cfg.PosY + g_Cfg.TopGapLines * adv;
    out->X = x - 3;
    out->Y = y - 2;
    out->Width = maxW + 6;
    out->Height = adv + 4;
    return adv;
}

/* 触发条命中 (外扩几像素防抖, 与 MouseInCampRect 同手感) */
static int MouseInTriggerRect(int mx, int my, int bitLineH, int screenW, int surfH)
{
    RectangleStruct tr;
    CampTriggerRect(bitLineH, screenW, surfH, &tr);
    return (mx >= tr.X - 4 && mx < tr.X + tr.Width + 6 &&
            my >= tr.Y - 4 && my < tr.Y + tr.Height + 4);
}

/* 超武面板整块重绘: 收集行按 y 分组 (名+计时器并成一行), 按 y 排序。
 * 统一面板 (Classic=0): 右下锚定单个半透明底, 名左计时器右。
 * 原版式 (Classic=1): 右对齐 + 逐行贴合黑条 (名右对齐到计时器列,
 *   计时器右对齐到右缘; 每行高=字高+上下 PadY, 条四面留边无缝衔接,
 *   字永不出条; 整块底部锚定 BottomMargin, 引擎 y 只定上下顺序)。
 * 颜色用收集的原前景色; 150ms 未刷新即过期, 引擎闪烁原样透出。
 * 返回 1=画出, 0=无新鲜行, -1=GDI失败, -2=Lock失败。 */

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
    int l = SWLen(s), w, ox, oy;
    if (l <= 0) return 0;
    w = RenderLine(s, l);
    if (w <= 0) return 0;
    if (g_Cfg.Shadow && g_Cfg.ShadowOpacity > 0)
    {
        for (oy = 0; oy < g_Cfg.ShadowSize; oy++)
            for (ox = 0; ox < g_Cfg.ShadowSize; ox++)
                BlitCoverage(locked, surfPitch, surfW, surfH,
                             x + g_Cfg.ShadowDX + ox, ly + g_Cfg.ShadowDY + oy,
                             w + 2, adv, 0, 0, 0,
                             (unsigned)g_Cfg.ShadowOpacity);
    }
    BlitCoverage(locked, surfPitch, surfW, surfH, x, ly,
                 w + 2, adv, fr, fg, fb, 100);
    return w;
}

static int DrawSWPanel(void* surf, int bitLineH, int surfW, int surfH,
                       int surfPitch)
{
    int ys[SWCAP_MAX], ni[SWCAP_MAX], ti[SWCAP_MAX];
    int ws[SWCAP_MAX], wt[SWCAP_MAX];
    int n = 0, i, j, k;
    unsigned now;
    int fontPx, adv, maxNW = 0, maxTW = 0, hasTimer = 0;
    int panelW = 0, panelH = 0, px = 0, py = 0, rightEdge, contentRight, nameEndCol;
    int step = 0, top = 0;
    RectangleStruct rect;
    ColorStruct board;
    unsigned char* locked;
    unsigned char* vt;
    LockFn lockFn;
    UnlockFn unlockFn;
    static int s_lastN = -1;
    char sb[128], t1[16], t2[16], t3[16], t4[16];
    unsigned q;

    if (!g_SWPanelOn) return 0;
    now = GetTickCount();
    for (i = 0; i < g_SWCapN && i < SWCAP_MAX; i++)
    {
        SWCapRow* c = &g_SWCap[i];
        if (now - c->tick > SW_FRESH_MS) continue;
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
    if (n != s_lastN)
    {
        s_lastN = n;
        q = 0;
        q = AppendCStr(sb, q, n > 0 ? "swpanel: show n=" : "swpanel: hide", sizeof(sb));
        if (n > 0)
        {
            UtoA((unsigned)n, t1); q = AppendCStr(sb, q, t1, sizeof(sb));
            q = AppendCStr(sb, q, " surf=", sizeof(sb));
            UtoA((unsigned)surfW, t2); q = AppendCStr(sb, q, t2, sizeof(sb));
            q = AppendCh(sb, q, 'x', sizeof(sb));
            UtoA((unsigned)surfH, t3); q = AppendCStr(sb, q, t3, sizeof(sb));
            (void)t4;
        }
        LogLine(sb);
    }
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
    rightEdge = surfW - g_SW.RightMargin;
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
                    FillRectTransVt(surf, &rect, &board, g_SW.Opacity);
            }
        }
    }
    else
    {
        panelW = g_SW.PadX * 2 + maxNW + (hasTimer ? g_SW.Gap + maxTW : 0);
        panelH = g_SW.PadY * 2 + n * adv;
        px = surfW - g_SW.RightMargin - panelW;
        py = surfH - g_SW.BottomMargin - panelH;
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        rect.X = px; rect.Y = py; rect.Width = panelW; rect.Height = panelH;
        if (g_SW.Opacity > 0)
            FillRectTransVt(surf, &rect, &board, g_SW.Opacity);
    }
    vt = *(unsigned char**)surf;
    lockFn = (LockFn)(*(void**)(vt + VT_LOCK));
    unlockFn = (UnlockFn)(*(void**)(vt + VT_UNLOCK));
    if (!lockFn || !unlockFn) return -2;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return -2;
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
    unlockFn(surf, NULL);
    return 1;
}

/* 老钩的前向声明 (定义在后, 入口钩在引擎链表为空时直接调用它) */
extern "C" DWORD __cdecl MsgFont_MessageDrawHook(void* regs);

/* 引擎消息链表是否非空: 反汇编实证 (0x5D4A8D):
 *   mov ecx,[esi]; test ecx,ecx; je return —— this[0] 为空直接返回,
 *   全程跳过 0x5D4A94 老钩。引擎 Manage 会摘掉过期项, 链表空后老钩
 *   再也不触发, 悬停也就展不开历史 (必须等一条新消息)。
 * this 在 Draw 入口即 ECX (thiscall), 从 regs+OFF_REG_ECX 取。
 * 拿不到/不可信时按"非空"返回 (保守走老路, 绝不崩)。 */
static int EngineListAlive(void* regs)
{
    void* self;
    if (!regs) return 1;
    self = *(void**)((char*)regs + OFF_REG_ECX);
    if (!self || !IsPlausiblePtr(self)) return 1;
    return (*(void**)self != NULL);
}

/* 绿框入口钩 (0x5D49A0, 6B): MessageListClass::Draw 入口, 每帧都跑。
 * 反汇编实证: 0x5D4A94 的老钩在链表为空时根本走不到——Draw 入口先测
 * 0x19(%esi) 为零则 je 到 0x5D4A8D, 再测 (%esi) 为空则 je 到返回,
 * 全程跳过 0x5D4A94。所以零消息时老钩不触发, 绿框必须先有一条消息。
 * 绿框只是触发区的可视化 (不带文本), 受 ShowHotZone 开关:
 * 1=画出, 0=隐藏但触发判定仍然有效。返回 0 跑原版。
 * 附加职责: 引擎链表为空 (this[0]==NULL) 时老钩走不到, 在此直接调
 * 老钩的完整渲染, 悬停随时能展历史, 不再依赖新消息到来。 */
extern "C" __declspec(dllexport) DWORD __cdecl MsgFont_GreenHook(void* regs)
{
    void* surf;
    (void)regs;
    EnsureInit();
    if (!g_Cfg.Enable) return 0;
    surf = *(void**)ADDR_DSURFACE_COMPOSITE;
    if (!surf) return 0;
    /* 绿框可视化受 ShowHotZone 开关 (0=隐藏, 但触发判定不受影响) */
    if (g_Cfg.ShowHotZone)
    {
        int bitLineH, screenW, surfH;
        RectangleStruct zr;
        ColorStruct zc;
        void* font = *(void**)ADDR_BITFONT_INSTANCE;
        RectangleStruct* vb = (RectangleStruct*)ADDR_DSURFACE_VIEWBOUNDS;
        bitLineH = font ? *(int*)((char*)font + FONT_OFF_HEIGHT) : 14;
        if (bitLineH < 6 || bitLineH > 64) bitLineH = 14;
        screenW = (vb && vb->Width > 200) ? vb->Width : 640;
        surfH = SurfGet(surf, VT_GET_HEIGHT);
        if (surfH <= 0) return 0;
        /* 触发条: 与顶部文本左右对齐、1 行高 (几何见 CampTriggerRect), 不带文本 */
        CampTriggerRect(bitLineH, screenW, surfH, &zr);
        zc.R = 0; zc.G = 255; zc.B = 0;
        FillRectTransVt(surf, &zr, &zc, 30);
        DrawOutline(surf, zr.X, zr.Y, zr.Width, zr.Height, 0, 255, 0, 90);
    }
    /* 引擎链表为空时老钩 (0x5D4A94) 走不到, 历史只能在这里画:
     * 直接调老钩的完整渲染 (悬停/活消息/提示都在里面), 返回值丢弃,
     * 本钩固定返回 0 跑原版, 原版随后无害地提前返回。 */
    if (!EngineListAlive(regs))
    {
        static int s_logged = 0;
        if (!s_logged) { s_logged = 1; LogLine("green: engine list empty, render from entry"); }
        MsgFont_MessageDrawHook(regs);
    }
    return 0;
}

extern "C" __declspec(dllexport) DWORD __cdecl MsgFont_MessageDrawHook(void* regs)
{
    void* surf;
    unsigned tick;
    int bitLineH, screenW, surfW, surfH, surfPitch;
    int drew = 0, rc, showCamp = 0, zoneDrew = 0;
    int liveCamp = 0, hoverCamp = 0, hintOn = 0;
    int dbgMX = -1, dbgMY = -1;
    static int s_lastShow = -1;
    BoxSpec camp, hint;
    static int s_firstDraw = 1;

    (void)regs;
    EnsureInit();
    if (!g_Cfg.Enable) return 0;

    tick = TickNow();

    surf = *(void**)ADDR_DSURFACE_COMPOSITE;
    /* 绿框常驻: 只依赖画布, 不依赖有无历史/提示。
     * 旧条件 (Camp+Hint 条数<=0 直接 return 0) 导致开局零消息时
     * 钩子直接走原版、绿框一次都不画, 必须先刷一条底部提示
     * (Hint 环非空) 才肯出现——这就是"要触发底部信息才显示"。 */
    if (!surf) { StatTick(tick, 5); return 0; }

    if (!g_Calibrated) Calibrate();

    {
        void* font = *(void**)ADDR_BITFONT_INSTANCE;
        bitLineH = font ? *(int*)((char*)font + FONT_OFF_HEIGHT) : 14;
        if (bitLineH < 6 || bitLineH > 64) bitLineH = 14;
    }
    {
        RectangleStruct* vb = (RectangleStruct*)ADDR_DSURFACE_VIEWBOUNDS;
        screenW = (vb && vb->Width > 200) ? vb->Width : 640;
    }
    surfW = SurfGet(surf, VT_GET_WIDTH);
    surfH = SurfGet(surf, VT_GET_HEIGHT);
    surfPitch = SurfGet(surf, VT_GET_PITCH);
    if (surfW <= 0 || surfH <= 0 || surfPitch <= 0) { StatTick(tick, 3); return 0; }
    g_SurfW = surfW; g_SurfH = surfH;
    RefreshWinSize(tick);

    /* 焦点追踪: 窗口化切走时引擎停渲染、逻辑照跑; 切回来旧消息同时过期绿框重现,
     * 看起来像"按键召唤绿框"。丢失瞬间钩子不跑记不了, 只记恢复时的静默时长 */
    {
        static int s_lastFocus = -1;
        static unsigned s_lastSeen = 0;
        unsigned nowC = ClockNow();
        if (g_GameWnd)
        {
            int focus = (GetForegroundWindow() == g_GameWnd) ? 1 : 0;
            if (focus != s_lastFocus)
            {
                s_lastFocus = focus;
                if (focus)
                {
                    char fb[96], f1[16];
                    unsigned fq = 0;
                    fq = AppendCStr(fb, fq, "focus: gained gap=", sizeof(fb));
                    if (s_lastSeen && nowC > s_lastSeen) UtoA(nowC - s_lastSeen, f1);
                    else { f1[0] = '0'; f1[1] = 0; }
                    fq = AppendCStr(fb, fq, f1, sizeof(fb));
                    LogLine(fb);
                }
            }
            s_lastSeen = nowC;
        }
    }

    /* 时钟回拨 (新开局/读档): 旧过期戳失效, 清历史否则僵尸消息复活 */
    {
        static unsigned s_lastClock = 0;
        unsigned nowC = ClockNow();
        if (s_lastClock && (int)(nowC - s_lastClock) < -600)
        {
            g_CampRing.head = g_CampRing.count = 0;
            g_HintRing.head = g_HintRing.count = 0;
            g_Skip = 0;
            g_CampValid = 0;
            s_prevCampTotal = 0;
            LogLine("clock: reset, history cleared");
        }
        else if (s_lastClock && (nowC - s_lastClock) > 3600)
        {
            /* 长时间无帧 (切走/暂停/读档, 约 1 分钟): 旧记录已全过期,
             * 留着只会污染悬停历史 (上局的消息混进这局), 清掉 */
            g_CampRing.head = g_CampRing.count = 0;
            g_HintRing.head = g_HintRing.count = 0;
            g_Skip = 0;
            g_CampValid = 0;
            s_prevCampTotal = 0;
            LogLine("clock: fwd-reset, stale history cleared");
        }
        s_lastClock = nowC;
    }

    /* 战役框可见性: 有活消息, 或鼠标悬停 (已画出的框区/触发条);
     * 超时隐藏后, 鼠标移到触发条 (与顶部文本左右对齐的 1 行高绿框)
     * 即重现历史。触发条细长, 不再留边缘余量。 */
    {
        int mx = -1, my = -1;
        if (MouseXY(&mx, &my))
        {
            dbgMX = mx; dbgMY = my;
            /* 并集防抖: 命中上帧框区, 或落在触发条上, 都算悬停。
             * g_CampValid 是上帧残留, 若二选一, 鼠标停在"触发条内/旧框区外"
             * 会隔帧翻转 show/hide。 */
            hoverCamp = MouseInCampRect(mx, my) ||
                        MouseInTriggerRect(mx, my, bitLineH, screenW, surfH);
        }
        liveCamp = HasLiveCamp();
        showCamp = liveCamp || hoverCamp;
        camp.bar = hoverCamp;   /* 滚动条平时隐藏, 悬停才显示 */
        /* 平时只显示活消息 (超时即隐藏); 悬停才展开全部历史 */
        camp.ignoreTimeout = hoverCamp;
        {
            static int s_lastHover = -1;
            if (hoverCamp != s_lastHover) { s_lastHover = hoverCamp; g_Skip = 0; }
        }
        /* 本帧 DrawBox 会重设新鲜度 (画出才置 1), 先清零 */
        g_CampValid = 0;
        if (showCamp != s_lastShow)
        {
            char sb[160], t1[16], t2[16], t3[16], t4[16], t5[16], t6[16], t7[16];
            unsigned q = 0;
            s_lastShow = showCamp;
            q = AppendCStr(sb, q, showCamp ? "camp: show live=" : "camp: hide live=", sizeof(sb));
            UtoA((unsigned)liveCamp, t1); q = AppendCStr(sb, q, t1, sizeof(sb));
            q = AppendCStr(sb, q, " hover=", sizeof(sb));
            UtoA((unsigned)hoverCamp, t2); q = AppendCStr(sb, q, t2, sizeof(sb));
            q = AppendCStr(sb, q, " mouse=", sizeof(sb));
            UtoA((unsigned)(dbgMX < 0 ? 0 : dbgMX), t3); q = AppendCStr(sb, q, t3, sizeof(sb));
            q = AppendCh(sb, q, ',', sizeof(sb));
            UtoA((unsigned)(dbgMY < 0 ? 0 : dbgMY), t4); q = AppendCStr(sb, q, t4, sizeof(sb));
            q = AppendCStr(sb, q, " campN=", sizeof(sb));
            UtoA((unsigned)g_CampRing.count, t5); q = AppendCStr(sb, q, t5, sizeof(sb));
            q = AppendCStr(sb, q, " hintN=", sizeof(sb));
            UtoA((unsigned)g_HintRing.count, t6); q = AppendCStr(sb, q, t6, sizeof(sb));
            if (showCamp)
            {
                /* 最新战役记录还剩多久: 按键附带的瞬态系统消息通常只有几百 tick,
                 * 剧情感知消息一般是永生(-1)或长超时, 不开 LogMessages 也能归因 */
                q = AppendCStr(sb, q, " newExp=", sizeof(sb));
                if (g_CampRing.count > 0)
                {
                    ChatMsg* nm = MsgAt(&g_CampRing, g_CampRing.count - 1);
                    int expIn = nm->expire ? (int)(nm->expire - ClockNow()) : -1;
                    if (expIn < 0)
                    {
                        q = AppendCh(sb, q, '-', sizeof(sb));
                        UtoA((unsigned)(-expIn), t7);
                    }
                    else UtoA((unsigned)expIn, t7);
                    q = AppendCStr(sb, q, t7, sizeof(sb));
                }
                else q = AppendCStr(sb, q, "none", sizeof(sb));
            }
            else
            {
                q = AppendCStr(sb, q, " hist=", sizeof(sb));
                UtoA((unsigned)HasCampHistory(), t7);
                q = AppendCStr(sb, q, t7, sizeof(sb));
            }
            LogLine(sb);
        }
    }

    /* 战役框: sp=0 (ignoreTimeout 在可见性处按悬停设置);
     * 顶部或底部锚定 (底部时底边固定、文本向上增长), 可水平居中 */
    camp.wantSp = 0;
    camp.x = g_Cfg.PosX; camp.yBase = g_Cfg.PosY; camp.topGap = g_Cfg.TopGapLines;
    camp.autoBottom = g_Cfg.CampBottom; camp.bottomMargin = g_Cfg.CampBottomMargin;
    camp.centered = g_Cfg.CampCentered;
    camp.maxW = g_Cfg.MaxWidth; camp.maxWPm = g_Cfg.MaxWidthPm;
    camp.limit = g_Cfg.MaxLines; camp.rowBudget = 400;
    camp.opacity = g_Cfg.BoardOpacity; camp.shadowNoBoard = 0;
    camp.fontPx = g_Cfg.FontHeight; camp.bold = g_Cfg.Bold;
    camp.pSkip = &g_Skip;
    camp.ring = &g_CampRing;
    camp.schemeColor = 1;
    camp.tmoMul = 1;
    camp.slot = 0;
    camp.face = g_CampFace;

    /* 提示框: 左下, sp=1 自然消失 */
    hint.wantSp = 1; hint.ignoreTimeout = 0;
    hint.x = g_Cfg.HintPosX; hint.yBase = 0; hint.topGap = 0;
    hint.autoBottom = 1; hint.bottomMargin = g_Cfg.HintBottom;
    hint.centered = 0;
    hint.maxW = g_Cfg.HintMaxWidth; hint.maxWPm = 0;
    hint.limit = g_Cfg.HintMaxLines;
    hint.rowBudget = g_Cfg.HintMaxLines;
    hint.opacity = g_Cfg.HintOpacity; hint.shadowNoBoard = 1;  /* 0=无底但阴影照画 */
    hint.fontPx = g_Cfg.HintFontHeight; hint.bold = g_Cfg.HintBold;
    hint.pSkip = NULL; hint.bar = 0;
    hint.ring = &g_HintRing;
    hint.schemeColor = g_Cfg.HintColorFollow;
    hint.tmoMul = (unsigned)g_Cfg.HintTimeoutMul;
    hint.slot = 1;
    hint.face = g_HintFace;

    /* 触发条绿框 (测试可视化, 不带文本): 与战役文本框同 x / 同宽、1 行高
     * (顶部或底部, 由 CampBottom 决定), 先画绿底+绿边, 再画战役/提示文本压在上层。
     * ShowHotZone=0 时隐藏绿框, 但触发判定 (hoverCamp) 仍然有效。
     * 鼠标放上去时上面的 DrawBox 会展开历史文本。 */
    if (g_Cfg.ShowHotZone)
    {
        RectangleStruct zr;
        ColorStruct zc;
        CampTriggerRect(bitLineH, screenW, surfH, &zr);
        zc.R = 0; zc.G = 255; zc.B = 0;
        FillRectTransVt(surf, &zr, &zc, 30);
        DrawOutline(surf, zr.X, zr.Y, zr.Width, zr.Height, 0, 255, 0, 90);
        drew = 1;
        zoneDrew = 1;
    }

    if (showCamp)
    {
        rc = DrawBox(surf, &camp, bitLineH, screenW, surfW, surfH, surfPitch);
        if (rc < 0) { StatTick(tick, rc == -1 ? 2 : 4); return 0; }
        if (rc == 1) drew = 1;
    }

    if (g_Cfg.HintShow)
    {
        static int s_lastHint = -1;
        int on;
        rc = DrawBox(surf, &hint, bitLineH, screenW, surfW, surfH, surfPitch);
        on = (rc == 1);
        if (on != s_lastHint) { s_lastHint = on; LogHintWhy(on); }
        if (rc < 0) { StatTick(tick, rc == -1 ? 2 : 4); return 0; }
        if (rc == 1) { drew = 1; hintOn = 1; }
    }

    /* 超武面板: 右下整块重绘 (Print 入口已隐藏+收集, 此处统一画)。
     * 主画布尺寸同时回填给收集门: 坐标系一致才收, 否则走原版。 */
    g_SWCompW = surfW; g_SWCompH = surfH;
    {
        rc = DrawSWPanel(surf, bitLineH, surfW, surfH, surfPitch);
        if (rc < 0) { StatTick(tick, rc == -1 ? 2 : 4); return 0; }
        if (rc == 1) drew = 1;
    }

    if (!drew && !g_Cfg.ShowHotZone) { StatTick(tick, 1); return 0; }
    StatTick(tick, drew ? 0 : 1);
    {
        static unsigned s_dbgLast = 0;
        if (tick - s_dbgLast >= 300)
        {
            char db[256], n1[16], n2[16], n3[16], n4[16], n5[16], n6[16];
            unsigned q = 0;
            s_dbgLast = tick;
            q = AppendCStr(db, q, "dbg: mouse=", sizeof(db));
            UtoA((unsigned)(dbgMX < 0 ? 0 : dbgMX), n1); q = AppendCStr(db, q, n1, sizeof(db));
            q = AppendCh(db, q, ',', sizeof(db));
            UtoA((unsigned)(dbgMY < 0 ? 0 : dbgMY), n2); q = AppendCStr(db, q, n2, sizeof(db));
            q = AppendCStr(db, q, " valid=", sizeof(db));
            UtoA((unsigned)g_CampValid, n3); q = AppendCStr(db, q, n3, sizeof(db));
            q = AppendCStr(db, q, " rect=", sizeof(db));
            UtoA((unsigned)(g_CampRect.X < 0 ? 0 : g_CampRect.X), n4); q = AppendCStr(db, q, n4, sizeof(db));
            q = AppendCh(db, q, ',', sizeof(db));
            UtoA((unsigned)(g_CampRect.Y < 0 ? 0 : g_CampRect.Y), n5); q = AppendCStr(db, q, n5, sizeof(db));
            q = AppendCh(db, q, ',', sizeof(db));
            UtoA((unsigned)g_CampRect.Width, n6); q = AppendCStr(db, q, n6, sizeof(db));
            q = AppendCh(db, q, ',', sizeof(db));
            UtoA((unsigned)g_CampRect.Height, n1); q = AppendCStr(db, q, n1, sizeof(db));
            q = AppendCStr(db, q, " show=", sizeof(db));
            UtoA((unsigned)showCamp, n2); q = AppendCStr(db, q, n2, sizeof(db));
            q = AppendCStr(db, q, " live=", sizeof(db));
            UtoA((unsigned)liveCamp, n3); q = AppendCStr(db, q, n3, sizeof(db));
            q = AppendCStr(db, q, " hover=", sizeof(db));
            UtoA((unsigned)hoverCamp, n4); q = AppendCStr(db, q, n4, sizeof(db));
            q = AppendCStr(db, q, " hint=", sizeof(db));
            UtoA((unsigned)hintOn, n5); q = AppendCStr(db, q, n5, sizeof(db));
            q = AppendCStr(db, q, " lk=", sizeof(db));
            UtoA((unsigned)LiveKind(), n1); q = AppendCStr(db, q, n1, sizeof(db));
            q = AppendCStr(db, q, " skip=", sizeof(db));
            UtoA((unsigned)g_Skip, n6); q = AppendCStr(db, q, n6, sizeof(db));
            q = AppendCStr(db, q, " zone=", sizeof(db));
            UtoA((unsigned)zoneDrew, n1); q = AppendCStr(db, q, n1, sizeof(db));
            q = AppendCStr(db, q, " zh=", sizeof(db));
            UtoA((unsigned)HasCampHistory(), n2); q = AppendCStr(db, q, n2, sizeof(db));
            LogLine(db);
        }
    }
    if (s_firstDraw)
    {
        char b1[16], b2[16], b3[16], buf[256];
        unsigned p = 0;
        s_firstDraw = 0;
        p = AppendCStr(buf, p, "draw: ok dual surf=", sizeof(buf));
        UtoA((unsigned)surfW, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, 'x', sizeof(buf));
        UtoA((unsigned)surfH, b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " pitch=", sizeof(buf));
        UtoA((unsigned)surfPitch, b3);
        p = AppendCStr(buf, p, b3, sizeof(buf));
        LogLine(buf);
    }
    /* 显示态悬停范围可视化 (调试): 有框才描白框 */
    if (g_Cfg.ShowHotZone && g_CampValid)
        DrawOutline(surf, g_CampRect.X, g_CampRect.Y, g_CampRect.Width, g_CampRect.Height,
                    255, 255, 255, 80);
    return ADDR_AFTER_MSGLIST_DRAW;
}

/* 旧单框实现已由双框 DrawBox 取代 (见上)。 */

/* 滚轮 (0x777998, 6B): 进入时 ECX = 原始 WParam (shr 尚未执行),
 * bit31=1 为下滚 (回最新), 否则上滚 (翻历史)。只在战役框内生效。 */
extern "C" __declspec(dllexport) DWORD __cdecl MsgFont_WheelHook(void* regs)
{
    unsigned wparam;
    int mx, my;
    static unsigned s_wheelN = 0;

    EnsureInit();
    if (!g_Cfg.Enable || !g_CampValid) return 0;
    if (!MouseXY(&mx, &my) || !MouseInCampRect(mx, my)) return 0;

    wparam = *(unsigned*)((char*)regs + OFF_REG_ECX);
    if (wparam & 0x80000000u)
    {
        g_Skip -= g_Cfg.ScrollStep;
        if (g_Skip < 0) g_Skip = 0;
    }
    else g_Skip += g_Cfg.ScrollStep;
    s_wheelN++;
    if (s_wheelN == 1)
    {
        char b1[16], buf[128];
        unsigned p = 0;
        p = AppendCStr(buf, p, "wheel: first dir=", sizeof(buf));
        UtoA((wparam & 0x80000000u) ? 1u : 0u, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        LogLine(buf);
    }
    return 0;
}

/* 侧栏滚动 (0x533F50, 5B): 悬停战役框时跳过, 滚轮只滚消息。 */
#define ADDR_SKIP_SCROLL_SIDEBAR 0x00533FC3u

extern "C" __declspec(dllexport) DWORD __cdecl MsgFont_SidebarHook(void* regs)
{
    int mx, my;
    (void)regs;
    EnsureInit();
    if (!g_Cfg.Enable || !g_CampValid) return 0;
    if (!MouseXY(&mx, &my) || !MouseInCampRect(mx, my)) return 0;
    return ADDR_SKIP_SCROLL_SIDEBAR;
}

extern "C" BOOL __stdcall DllMain(HINSTANCE h, DWORD reason, LPVOID res)
{
    (void)h; (void)res;
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
        VecText_Install();
    }
    return TRUE;
}
