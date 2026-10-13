/*
 * MsgText.cpp - 消息文本显示 (战役框 + 提示框), 配置节 [MsgText]。
 *
 * 钩子 (Syringe, 见 dist/MOExperience.dll.inj):
 *   消息: 0x5D3BA0 AddMessage 捕获; 0x5D49A0 Draw 入口绿框;
 *         0x5D4A94 Draw 内部 (自制消息框画完回 0x5D4A9B, 由 MOExperience.cpp
 *         的消息帧统一出口, 故本模块只提供 MsgText_DrawFrame)。
 *   交互: 0x777998 滚轮翻历史; 0x533F50 悬停时跳过侧栏滚动;
 *         0x55E477/62F/693/746 单人聊天框 (跳过 session 检查)。
 * 任何一步失败 (Lock 不上、GDI 未就绪) 都返回 0 走原版绘制, 绝不花屏。
 *
 * 渲染: Microsoft YaHei 经 GDI 光栅化到 32 位内存 DIB (灰度抗锯齿取覆盖率),
 * 经启动校准得到的 16 位格式 (565/555) alpha 混入 Composite。
 * 取色: ColorScheme BaseColor 经 0x517440 转显示色 (Phobos 同用法)。
 * 构建见 build-fast.bat (32 位 mingw, 无 CRT)。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "ChatEdit.h"
#include "MsgText.h"

/* ---------------- 地址 ---------------- */

#define ADDR_COLORSCHEME_ARRAY   0x00B054D0u
#define ADDR_SCHEME_TO_COLOR     0x00517440u
#define ADDR_SESSION             0x00A8B238u /* SessionClass::Instance, 首成员即 GameMode */
#define ADDR_AFTER_MSGLIST_DRAW  0x005D4A9Bu

#define OFF_REG_ESP              0x14
#define OFF_REG_ECX              0x20
#define STK_ARG_NAME             0x04
#define STK_ARG_MESSAGE          0x0C
#define STK_ARG_CS               0x10
#define STK_ARG_TIMEOUT          0x18
#define STK_ARG_SP               0x1C

#define OFF_CSCHEME_ITEMS        0x04
#define OFF_CSCHEME_COUNT        0x10
#define OFF_CSCHEME_BASECOLOR    0x308

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
    int ShadowSize;    /* 阴影粗细 (1=单层, >1 加厚) */
    int MaxLines;
    int BoardOpacity;
    int HoverMinOpacity; /* 悬停最低透明度 0~100 (找触发区用, 默认5); 0=沿用渐隐 */
    int PosX;
    int PosY;
    int TopGapLines;
    int MaxWidth;      /* >1 = 像素; 0 = 自动半屏 (小数走 MaxWidthPm) */
    int MaxWidthPm;    /* >0 = 按屏宽千分比 (1~1000, 如500=50%), 优先于 MaxWidth */
    int CampBottom;    /* 1 = 底部显示 (底边固定, 文本向上增长), 0 = 顶部显示 */
    int CampBottomMargin; /* 底部模式时距底边像素 */
    int CampCentered;  /* 1 = 水平居中, 0 = 左对齐 (PosX) */
    int CampAnim;      /* 1 = 打字机逐字 (原版 Animate 同速率), 0 = 整行即时 */
    int LogMessages;
    int ShowHotZone;   /* 1 = 画出悬停生效范围 (调试悬停用) */
    int ScrollStep;    /* 滚轮步长(行) */
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
} MsgTextConfig;

static MsgTextConfig g_Cfg;

static wchar_t g_CampFace[FONT_FACE_MAX];
static wchar_t g_HintFace[FONT_FACE_MAX];

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

/* 联机判定: [SessionClass] 的首成员就是 GameMode (同 YRPP SessionClass::IsMultiplayer)
 * 3=LAN / 4=Internet 才是联机; 0=战役 / 5=遭遇战 是单机。
 * 地址读不到时按联机处理: 宁可多显示一次名字, 也不误吞别人的发言归属。 */
static int GameIsMultiplayer(void)
{
    int sess;
    if (!IsPlausiblePtr((void*)ADDR_SESSION)) return 1;
    sess = *(int*)ADDR_SESSION;
    return (sess == 3 || sess == 4);
}

/* ---------------- 消息存储 ---------------- */

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

static unsigned ExpireStamp(int timeout)
{
    if (timeout < 0) return 0;
    return GtClockNow() + (unsigned)timeout;
}

static int MsgVisibleMul(ChatMsg* m, unsigned mul)
{
    unsigned span;
    if (m->expire == 0) return 1;
    if (mul < 1) mul = 1;
    span = (m->expire - m->born) * mul;
    return (int)(GtClockNow() - m->born) <= (int)span;
}

static int MsgVisible(ChatMsg* m)
{
    return MsgVisibleMul(m, 1);
}

/* ---------------- 取色 (ColorScheme -> 显示色) ----------------
 * 与引擎 Fancy_Text_Print_Wide (0x4A61C0) 完全一致。
 * 引擎做法 (反汇编实证): ECX = scheme+0x308 (即 &BaseColor), 栈传 3 字节
 * 输出缓冲, call 0x517440 (stdcall, ret 4), 返回输出缓冲指针。
 * 输出 3 字节的通道顺序由运行时移位表 (0x8A0DD0..0x8A0DE4) 的 shl 决定,
 * 首次使用读表定序: 红 11/10, 绿 5, 蓝 0。 */
typedef unsigned char* (__fastcall *SchemeToColorFn)(void* base, void* edxUnused, unsigned char* out3);

static int g_IdxR = 0, g_IdxG = 1, g_IdxB = 2;
static int g_MapDone = 0;

static void MapChannels(void)
{
    unsigned sL0 = *(unsigned*)0x8A0DD0u;   /* out[0] 的 shl */
    unsigned sL1 = *(unsigned*)0x8A0DD8u;   /* out[2] 的 shl */
    unsigned sL2 = *(unsigned*)0x8A0DE0u;   /* out[1] 的 shl */
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
}

static void SchemeColor(void* scheme, ColorStruct* out)
{
    static SchemeToColorFn fn = NULL;
    unsigned char buf[4];
    unsigned char* r;
    if (!g_MapDone) { g_MapDone = 1; MapChannels(); }
    if (!scheme || !IsPlausiblePtr(scheme)) { out->R = out->G = out->B = 255; return; }
    if (!fn) fn = (SchemeToColorFn)ADDR_SCHEME_TO_COLOR;
    buf[0] = buf[1] = buf[2] = buf[3] = 0;
    r = fn((char*)scheme + OFF_CSCHEME_BASECOLOR, NULL, buf);
    if (!r || !IsPlausiblePtr(r)) { out->R = out->G = out->B = 255; return; }
    out->R = r[g_IdxR]; out->G = r[g_IdxG]; out->B = r[g_IdxB];
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

/* 本地玩家颜色方案: CurrentPlayer 房 +0x16054, 裸值即真方案号。
 * 三重实证: (1) 房内搜模板名命中 UIName @+0x1602A, +42 恰是此偏移
 * (YRpp 版式); (2) 青局读 7, 采样 (0,252,255) 正青, 与玩家色一致;
 * (3) 橙局游戏回声 cs=17, 采样 (255,120,0) 正橙。
 * LUT/0xA8B394 在单机皆不可靠, 已弃用。只读查询, 联机安全。 */
#define ADDR_CURPLAYER_HOUSE 0x00A83D4Cu
#define OFF_HOUSE_COLORIDX   0x16054u

static int LocalPlayerScheme(int* outIdx)
{
    void* house;
    int sc;
    if (outIdx) *outIdx = -99;
    if (!IsPlausiblePtr((void*)ADDR_CURPLAYER_HOUSE)) return -1;
    house = *(void**)ADDR_CURPLAYER_HOUSE;
    if (!house || !IsPlausiblePtr(house)) return -1;
    if (!IsPlausiblePtr((char*)house + OFF_HOUSE_COLORIDX + 4)) return -1;
    sc = *(int*)((char*)house + OFF_HOUSE_COLORIDX);
    if (outIdx) *outIdx = sc;
    if (!SchemePtr(sc)) return -1;
    return sc;
}

/* ---------------- 配置载入 ---------------- */

static void MsgText_LoadConfig(void)
{
    g_Cfg.Enable      = GetPrivateProfileIntA(MSG_SECTION, "Enable", 1, g_IniPath);
    g_Cfg.FontHeight  = GetPrivateProfileIntA(MSG_SECTION, "FontHeight", 0, g_IniPath);
    g_Cfg.Bold        = GetPrivateProfileIntA(MSG_SECTION, "Bold", 0, g_IniPath);
    OU_ReadString(MSG_SECTION, "FontFace", "msyh.ttc",
                  g_Cfg.FontFace, sizeof(g_Cfg.FontFace));
    g_Cfg.Shadow      = GetPrivateProfileIntA(MSG_SECTION, "Shadow", 1, g_IniPath);
    g_Cfg.ShadowDX    = GetPrivateProfileIntA(MSG_SECTION, "ShadowDX", 1, g_IniPath);
    g_Cfg.ShadowDY    = GetPrivateProfileIntA(MSG_SECTION, "ShadowDY", 1, g_IniPath);
    g_Cfg.ShadowOpacity = GetPrivateProfileIntA(MSG_SECTION, "ShadowOpacity", 60, g_IniPath);
    g_Cfg.ShadowSize  = GetPrivateProfileIntA(MSG_SECTION, "ShadowSize", 1, g_IniPath);
    g_Cfg.MaxLines    = GetPrivateProfileIntA(MSG_SECTION, "MaxLines", 8, g_IniPath);
    g_Cfg.BoardOpacity= GetPrivateProfileIntA(MSG_SECTION, "BoardOpacity", 45, g_IniPath);
    g_Cfg.HoverMinOpacity = GetPrivateProfileIntA(MSG_SECTION, "HoverMinOpacity", 5, g_IniPath);
    g_Cfg.PosX        = GetPrivateProfileIntA(MSG_SECTION, "PosX", 8, g_IniPath);
    g_Cfg.PosY        = GetPrivateProfileIntA(MSG_SECTION, "PosY", 8, g_IniPath);
    g_Cfg.TopGapLines = GetPrivateProfileIntA(MSG_SECTION, "TopGapLines", 1, g_IniPath);
    {   /* MaxWidth: 整数=像素, 0=自动; 0~1小数=按屏宽百分比 */
        char wb[32];
        int pm = 0;
        OU_ReadString(MSG_SECTION, "MaxWidth", "0", wb, sizeof(wb));
        g_Cfg.MaxWidth = ParseWidth(wb, &pm);
        g_Cfg.MaxWidthPm = pm;
    }
    g_Cfg.CampBottom = GetPrivateProfileIntA(MSG_SECTION, "CampBottom", 0, g_IniPath);
    g_Cfg.CampBottomMargin = GetPrivateProfileIntA(MSG_SECTION, "CampBottomMargin", 8, g_IniPath);
    g_Cfg.CampCentered = GetPrivateProfileIntA(MSG_SECTION, "CampCentered", 0, g_IniPath);
    g_Cfg.CampAnim = GetPrivateProfileIntA(MSG_SECTION, "CampAnim", 1, g_IniPath);
    g_Cfg.LogMessages = GetPrivateProfileIntA(MSG_SECTION, "LogMessages", 0, g_IniPath);
    g_Cfg.ShowHotZone = GetPrivateProfileIntA(MSG_SECTION, "ShowHotZone", 1, g_IniPath);
    g_Cfg.ScrollStep  = GetPrivateProfileIntA(MSG_SECTION, "ScrollStep", 3, g_IniPath);
    g_Cfg.HintShow    = GetPrivateProfileIntA(MSG_SECTION, "HintShow", 1, g_IniPath);
    g_Cfg.HintPosX    = GetPrivateProfileIntA(MSG_SECTION, "HintPosX", 8, g_IniPath);
    g_Cfg.HintBottom  = GetPrivateProfileIntA(MSG_SECTION, "HintBottom", 8, g_IniPath);
    g_Cfg.HintFontHeight = GetPrivateProfileIntA(MSG_SECTION, "HintFontHeight", 14, g_IniPath);
    g_Cfg.HintBold    = GetPrivateProfileIntA(MSG_SECTION, "HintBold", 0, g_IniPath);
    OU_ReadString(MSG_SECTION, "HintFontFace", "msyh.ttc",
                  g_Cfg.HintFontFace, sizeof(g_Cfg.HintFontFace));
    g_Cfg.HintColorFollow = GetPrivateProfileIntA(MSG_SECTION, "HintColorFollow", 0, g_IniPath);
    g_Cfg.HintMaxLines= GetPrivateProfileIntA(MSG_SECTION, "HintMaxLines", 3, g_IniPath);
    g_Cfg.HintMaxWidth= GetPrivateProfileIntA(MSG_SECTION, "HintMaxWidth", 320, g_IniPath);
    g_Cfg.HintOpacity = GetPrivateProfileIntA(MSG_SECTION, "HintOpacity", 45, g_IniPath);
    g_Cfg.HintTimeoutMul = GetPrivateProfileIntA(MSG_SECTION, "HintTimeoutMul", 2, g_IniPath);

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
    if (g_Cfg.HoverMinOpacity < 0) g_Cfg.HoverMinOpacity = 0;
    if (g_Cfg.HoverMinOpacity > 100) g_Cfg.HoverMinOpacity = 100;
    if (g_Cfg.MaxWidth < 0) g_Cfg.MaxWidth = 0;
    g_Cfg.CampBottom = g_Cfg.CampBottom ? 1 : 0;
    if (g_Cfg.CampBottomMargin < 0) g_Cfg.CampBottomMargin = 0;
    if (g_Cfg.CampBottomMargin > 2000) g_Cfg.CampBottomMargin = 2000;
    g_Cfg.CampCentered = g_Cfg.CampCentered ? 1 : 0;
    g_Cfg.CampAnim = g_Cfg.CampAnim ? 1 : 0;
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
}

static int g_InitDone = 0;

void MsgText_EnsureInit(void)
{
    char b1[16], buf[256];
    unsigned p;
    if (g_InitDone) return;
    g_InitDone = 1;
    GtEnsureLog();
    if (!g_Log) return;
    MsgText_LoadConfig();
    LogLine("msgtext: start");
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
    p = AppendCStr(buf, p, " HovMinOp=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.HoverMinOpacity, buf + p);
    LogLine(buf);
    /* 字体解析放日志打开之后, 解析结果才能记入日志 */
    ResolveFontFace(g_Cfg.FontFace, "camp", g_CampFace, L"Microsoft YaHei");
    ResolveFontFace(g_Cfg.HintFontFace, "hint", g_HintFace, L"Microsoft YaHei");
}

/* 战役框文字样式 (ChatEdit 的输入框同源, 不另设字体键)。
 * *face=已解析的字体名, *px=字高 (<=0 表示跟游戏点阵), *bold=1/0。 */
void MsgText_CampStyle(const wchar_t** face, int* px, int* bold)
{
    MsgText_EnsureInit();
    if (face) *face = g_CampFace[0] ? g_CampFace : L"Microsoft YaHei";
    if (px) *px = g_Cfg.FontHeight;
    if (bold) *bold = g_Cfg.Bold;
}

/* ---------------- 排版与绘制 ---------------- */

#define MAX_PICKED 64
#define MAX_LPP    8

typedef struct { const wchar_t* start; int len; } TextLine;

static TextLine g_LineBuf[MAX_PICKED][MAX_LPP];
static int      g_LineCnt[MAX_PICKED];
static int      g_Picked[MAX_PICKED];
static int      g_RowBase[MAX_PICKED];
static int      g_LineOff[MAX_PICKED][MAX_LPP]; /* 行在合成串中的字符偏移 (打字机用) */
static int      g_BodyLen[MAX_PICKED];          /* 合成串总字符数 */
static int      g_Reveal[MAX_PICKED];           /* 本帧已揭示字符数 (DrawBox 填写) */
static wchar_t  g_Compose[MAX_PICKED][MAX_NAME_W + MAX_TEXT_W + 4];

extern "C" __declspec(dllexport) DWORD __cdecl MsgText_AddMessageHook(void* regs){
    unsigned char* esp;
    const wchar_t* argName;
    const wchar_t* argMsg;
    unsigned argColor, argTimeout, argSP;
    char b1[16], b2[16], b3[16], buf[256];
    unsigned p;

    MsgText_EnsureInit();
    if (!g_Cfg.Enable) return 0;

    esp = *(unsigned char**)((char*)regs + OFF_REG_ESP);
    if (!esp || !IsPlausiblePtr(esp)) return 0;

    argName    = *(const wchar_t**)(esp + STK_ARG_NAME);
    argMsg     = *(const wchar_t**)(esp + STK_ARG_MESSAGE);
    argColor   = *(unsigned*)(esp + STK_ARG_CS);
    argTimeout = *(unsigned*)(esp + STK_ARG_TIMEOUT);
    argSP      = *(unsigned*)(esp + STK_ARG_SP);

    /* 自己的话跟本地玩家色, 只补无名回声 (战役单机回声无名+默认色)。
     * 有名消息游戏自带真方案号, 一律不动。
     * 联机一律不插手: 联机回声游戏自带真名+真方案号, 且 MatchSent 只按文本
     * 匹配, 同名同句会误伤别人的发言归属。
     * 直接改栈上颜色参数: 原生标签和我方环同色 (纯显示, 非模拟)。 */
    if (!GameIsMultiplayer())
    {
        if (ChatEdit_MatchSent(argMsg))
        {
            int ls = LocalPlayerScheme(NULL);
            if (ls >= 0)
            {
                *(unsigned*)(esp + STK_ARG_CS) = (unsigned)ls;
                argColor = (unsigned)ls;
            }
        }
    }

    {
        unsigned born = GtClockNow();
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

/* ---------------- 双框: 战役长留 + 提示短命 ----------------
 * sp=1 (操作提示: 快捷键/路径点) 进左下提示框, 按引擎超时自然消失;
 * sp=0 (战役/聊天/系统) 进左上战役框, 忽略超时长留历史, 可翻看。 */

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
    int fade;        /* 0-100, DrawBox 内缩放底板/阴影/文字/滚动条透明度 */
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
    int fullBoard;   /* 1=整块底板 (悬停/展开, Phobos hover 态), 0=包裹文本 */
    int anim;        /* 1=打字机逐字 (只战役框), 0=整行即时 */
} BoxSpec;

static int g_Skip = 0;          /* 0 = 跟随最新 */
static int s_prevCampTotal = 0;

/* 战役框上一帧矩形 (滚轮命中 + 悬停判定用) */
static RectangleStruct g_CampRect;
static int g_CampValid = 0;
/* 聊天输入框锚点缓存: DrawBox 画战役框时顺手写下 (提前返回之前), 输入框直接读,
 * 不自己重算 —— 两边各算各的就会出现"首次不对齐、之后才对齐"。*/
static int g_AnchorX = 0, g_AnchorY = 0, g_AnchorValid = 0;

/* 注: 悬停展开无需防抖——透明度斜坡已保证无硬闪, 历史随底板一起淡入。
 * 展开跟瞬时悬停 (原版语义), 划过即见淡影找区。 */

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

/* 提示框显隐跳变日志 (newestExpIn = 最新提示距引擎过期还剩多少 tick, 62.5 tick≈1秒;
     * 若 hide 时它还很大, 就是提前强制消失的实证)
     * 与 MsgVisibleMul 同口径 (span=(expire-born)*mul), 否则 hide 行的
     * 剩余数对不上真实判定。*/
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
        if (!m->expire) expIn = -1;
        else expIn = (int)((m->expire - m->born) * (unsigned)g_Cfg.HintTimeoutMul) -
                     (int)(GtClockNow() - m->born);
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
    /* 名字前缀: 联机恒显示 (要区分发言者), 单机默认不显示 (只有自己一个名字, 是噪音)。 */
    int showName = GameIsMultiplayer() || ChatEdit_ShowNameSP();
    for (i = R->count - 1; i >= 0 && picked < MAX_PICKED && rows < rowBudget; i--)
    {
        ChatMsg* m = MsgAt(R, i);
        const wchar_t* body;
        const wchar_t* seg;
        int cnt = 0, off = 0;
        if (m->sp != wantSp) continue;
        if (!ignoreTimeout && !MsgVisibleMul(m, tmoMul)) continue;
        if (m->name[0] && showName)
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
            if (!TextHasContent(body) && !(m->name[0] && showName)) continue;
        {
            int bl = 0;
            while (body[bl]) bl++;
            g_BodyLen[picked] = bl;
        }
        seg = body;
        while (*seg && cnt < MAX_LPP && rows < rowBudget)
        {
            int segLen = 0;
            int fit;
            while (seg[segLen] && seg[segLen] != L'\n') segLen++;
            if (segLen == 0) { seg++; off++; continue; }  /* 跳过的换行也占揭示位 */
            fit = CharsFit(seg, segLen, textW);
            if (fit <= 0) fit = 1;
            if (fit > segLen) fit = segLen;
            g_LineBuf[picked][cnt].start = seg;
            g_LineBuf[picked][cnt].len = fit;
            g_LineOff[picked][cnt] = off;
            cnt++; rows++;
            seg += fit; off += fit;
            if (*seg == L'\n') { seg++; off++; }
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

/* 框宽与列起点 (DrawBox 与 CampTriggerRect 同口径, 含夹取顺序)。 */
static int MsgBoxGeom(int screenW, int cfgW, int cfgWPm, int centered, int cfgX, int* px)
{
    int maxW = cfgW, x;
    if (cfgWPm > 0) maxW = screenW * cfgWPm / 1000;
    else if (maxW <= 0) maxW = screenW / 2;
    if (maxW > screenW - 170) maxW = screenW - 170;
    x = centered ? (screenW - maxW) / 2 : cfgX;
    if (maxW < 80) maxW = 80;
    if (maxW > screenW - x - 8) maxW = screenW - x - 8;
    if (maxW < 80) maxW = 80;
    if (px) *px = x;
    return maxW;
}

/* 画一个框。返回 1=画出, 0=无内容, -1=GDI失败, -2=Lock失败。 */
static int DrawBox(void* surf, BoxSpec* B, int bitLineH, int screenW,
                   int surfW, int surfH, int surfPitch)
{
    int fontPx, adv, maxW, textW, x, y;
    int picked = 0, totalAll, k, r, rowCursor, rowBegin, rowEnd, limit, totalRows, skip = 0;
    int drawn = 0, boardOp = 0;
    RectangleStruct rect;
    ColorStruct board;
    unsigned char* locked;

    fontPx = B->fontPx > 0 ? B->fontPx : bitLineH;
    if (!GdiEnsure(fontPx, B->bold, B->slot, B->face)) return -1;
    adv = g_G->adv > 0 ? g_G->adv : bitLineH;

    maxW = MsgBoxGeom(screenW, B->maxW, B->maxWPm, B->centered, B->x, &x);
    textW = maxW;
    if (textW < 40) textW = 40;

    /* 输入框锚点缓存: 必须在下面所有提前返回之前写。
     * 底部取底边, 顶部取顶边, 均与行数无关、不随条数抖。 */
    if (B->pSkip)
    {
        g_AnchorX = x;
        g_AnchorY = B->autoBottom
                  ? surfH - B->bottomMargin + 2
                  : (B->yBase + B->topGap * adv);
        g_AnchorValid = 1;
    }

    totalAll = PickLines(B->ring, B->wantSp, B->ignoreTimeout, B->tmoMul, textW, B->rowBudget, &picked);
    if (picked <= 0 || totalAll <= 0) return 0;
    /* 打字机揭示位: 原版速率 1 字/16ms (AnimPos += (time-timing)>>4) 与首帧 1 字;
     * 时钟与 born 同系 (游戏时钟, 暂停即停); 关 anim 整串即时 (提示框/原版 sp=1 同)。 */
    {
        unsigned nowC = GtClockNow();
        for (k = 0; k < picked; k++)
        {
            if (!B->anim) { g_Reveal[k] = g_BodyLen[k]; continue; }
            {
                ChatMsg* m = MsgAt(B->ring, g_Picked[k]);
                int el = (int)(nowC - m->born);
                int rr = (el <= 0) ? 1 : el;
                if (rr > g_BodyLen[k]) rr = g_BodyLen[k];
                g_Reveal[k] = rr;
            }
        }
    }
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
    /* 底板透明度叠渐隐 (fade=100 与原来一致); 0 不画底板
     * (战役框同时省阴影; 提示框阴影照画, 见下)。
     * 整块只在悬停/展开时铺 (Phobos hover 态); 平时逐行包裹已揭示文本。 */
    {
        boardOp = B->opacity * B->fade / 100;
        if (boardOp > 0 && B->fullBoard)
            GtFillRectTrans(surf, &rect, &board, boardOp);
    }

    locked = GtLock(surf);
    if (!locked) return -2;

    for (k = 0; k < picked && drawn < totalRows; k++)
    {
        ChatMsg* m = MsgAt(B->ring, g_Picked[k]);
        ColorStruct fg;
        void* scheme = NULL;
        if (B->schemeColor)
        {
            /* 消息自带色 (自己的话已在 AddMessageHook 入口改过栈上参数,
             * 这里直接用, 原生/战役统一)。无名警报/系统保持原色。 */
            scheme = SchemePtr(m->color);
            if (!scheme) scheme = SchemePtr(0);
        }
        if (scheme) SchemeColor(scheme, &fg);
        else { fg.R = fg.G = fg.B = 255; }

        for (r = 0; r < g_LineCnt[k]; r++)
        {
            int globalRow = g_RowBase[k] + r;
            int ly, w, vis;
            if (globalRow < rowBegin || globalRow >= rowEnd) continue;
            ly = y + (globalRow - rowBegin) * adv;
            /* 本行揭示字数: 未揭示的行留空占位 (版式稳定, 文字长进格子) */
            vis = g_Reveal[k] - g_LineOff[k][r];
            if (vis <= 0) { drawn++; continue; }
            if (vis > g_LineBuf[k][r].len) vis = g_LineBuf[k][r].len;
            w = RenderLine(g_LineBuf[k][r].start, vis);
            if (w <= 0) { drawn++; continue; }
            /* 包裹模式: 黑底只包已揭示的墨水。相邻行在行缝处拼接不重叠,
             * 否则重叠区被混合两次、透明度翻倍发黑。 */
            if (!B->fullBoard && boardOp > 0)
            {
                RectangleStruct wr;
                int vi = globalRow - rowBegin;
                wr.X = x - 3; wr.Width = w + 6;
                wr.Y = (vi == 0) ? ly - 2 : ly;
                wr.Height = adv + (vi == 0 ? 2 : 0) + (vi == totalRows - 1 ? 2 : 0);
                GtFillRectTrans(surf, &wr, &board, boardOp);
            }
            /* 阴影 (战役框底板透明度 0 时跳过; 提示框 0 也画; ShadowSize>1 加厚) */
            if (g_Cfg.Shadow && g_Cfg.ShadowOpacity > 0 &&
                (boardOp > 0 || B->shadowNoBoard))
            {
                /* 单遍并集阴影 (见 GdiText.h 的 BlitCoverageShadow) */
                BlitCoverageShadow(locked, surfPitch, surfW, surfH,
                                   x, ly, w + 2, adv,
                                   g_Cfg.ShadowSize, g_Cfg.ShadowDX, g_Cfg.ShadowDY,
                                   (unsigned)(g_Cfg.ShadowOpacity * B->fade / 100));
            }
            BlitCoverage(locked, surfPitch, surfW, surfH, x, ly, w + 2, adv, fg.R, fg.G, fg.B, (unsigned)B->fade);
            drawn++;
        }
    }

    GtUnlock(surf);

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
            /* ★ 战役框的最新消息在**下**底 (rowEnd = totalAll - skip, skip=0 就是看最下面那
             * limit 行)。所以 skip=0 (最下面) 时滑块要在**最下面**, 滑块位置与
             * skip **反向**。写成正比会让滑块跑到顶上, 和内容正好对调。*/
            thumbY = (maxSkip > 0) ? barY + (barH - thumbH) * (maxSkip - skip) / maxSkip
                                   : barY + barH - thumbH;
            bar.X = barX; bar.Y = barY; bar.Width = 4; bar.Height = barH;
            c.R = 30; c.G = 30; c.B = 30;
            GtFillRectTrans(surf, &bar, &c, 55 * B->fade / 100);
            bar.Y = thumbY; bar.Height = thumbH;
            c.R = 170; c.G = 170; c.B = 170;
            GtFillRectTrans(surf, &bar, &c, 90 * B->fade / 100);
        }
    }

    return 1;
}

/* 触发条几何: 与战役文本框同 x / 同宽、1 行高, 底部时底边固定向上增长。
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
    maxW = MsgBoxGeom(screenW, g_Cfg.MaxWidth, g_Cfg.MaxWidthPm,
                      g_Cfg.CampCentered, g_Cfg.PosX, &x);
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

/* 输入框锚点 (供 ChatEdit.cpp): 底部贴战役框下沿+6px, 顶部贴战役框顶边。
 * 读 DrawBox 缓存的绘制结果 (与战役框同源, 不另算); 缓存未就绪时退回 1 行触发矩形。
 * 返回 1=有锚点, 0=无 (调用方退回原生位置)。 */
int MsgText_ChatAnchor(int surfW, int surfH, int bitLineH, int* px, int* py)
{
    RectangleStruct tr;
    int adv, gap = 6, ax, ay;
    if (!g_Cfg.Enable) return 0;
    if (surfW <= 0 || surfH <= 0) return 0;
    /* 缓存是本次绘制用的同一批数字, 与战役框天然对齐。 */
    if (g_AnchorValid)
    {
        ax = g_AnchorX;
        ay = g_Cfg.CampBottom ? g_AnchorY + gap : g_AnchorY;
    }
    else
    {
        /* 首帧缓存未就绪时退回 1 行触发矩形, 下一帧被缓存接管。 */
        adv = CampTriggerRect(bitLineH, surfW, surfH, &tr);
        if (adv < 8) adv = 16;
        ax = tr.X + 3;
        if (g_Cfg.CampBottom)
            ay = tr.Y + tr.Height + gap;
        else
            ay = tr.Y + 2;
    }
    if (ax < 0) ax = 0;
    if (ax > surfW - 40) ax = surfW - 40;
    if (ay < 0) ay = 0;
    if (ay > surfH - 4) ay = surfH - 4;
    *px = ax;
    *py = ay;
    return 1;
}

/* 触发条命中 (外扩几像素防抖, 与 MouseInCampRect 同手感) */
static int MouseInTriggerRect(int mx, int my, int bitLineH, int screenW, int surfH)
{
    RectangleStruct tr;
    CampTriggerRect(bitLineH, screenW, surfH, &tr);
    return (mx >= tr.X - 4 && mx < tr.X + tr.Width + 6 &&
            my >= tr.Y - 4 && my < tr.Y + tr.Height + 4);
}

/* 渲染期主体: 战役框可见性/淡入淡出 + 触发条绿框 + 战役框 + 提示框。
 * 返回 1=画出, 0=无内容, -1=GDI失败, -2=Lock失败。 */
int MsgText_DrawFrame(void* surf, int bitLineH, int screenW,
                      int surfW, int surfH, int surfPitch)
{
    int drew = 0, rc, showCamp = 0;
    int liveCamp = 0, hoverCamp = 0;
    BoxSpec camp, hint;

    if (!g_Cfg.Enable) return 0;
    if (!surf || surfW <= 0 || surfH <= 0 || surfPitch <= 0) return 0;
    /* 565/555 判定: 本函数也可能被绿框入口钩直接调 (不经消息帧), 故自带守卫;
     * 幂等, 协调层消息帧已经校准过就是空操作。 */
    if (!g_Calibrated) Calibrate();

    /* 焦点追踪: 窗口化切走时引擎停渲染、逻辑照跑; 切回来旧消息同时过期绿框重现,
     * 看起来像"按键召唤绿框"。丢失瞬间钩子不跑记不了, 只记恢复时的静默时长 */
    {
        static int s_lastFocus = -1;
        static unsigned s_lastSeen = 0;
        unsigned nowC = GtClockNow();
        if (GtGameFocused())
        {
            int focus = 1;
            if (focus != s_lastFocus)
            {
                char fb[96], f1[16];
                unsigned fq = 0;
                s_lastFocus = focus;
                if (focus)
                {
                    fq = AppendCStr(fb, fq, "focus: gained gap=", sizeof(fb));
                    if (s_lastSeen && nowC > s_lastSeen) UtoA(nowC - s_lastSeen, f1);
                    else { f1[0] = '0'; f1[1] = 0; }
                    fq = AppendCStr(fb, fq, f1, sizeof(fb));
                    LogLine(fb);
                }
            }
            s_lastSeen = nowC;
        }
        else s_lastFocus = 0;
    }

    /* 时钟回拨 (新开局/读档): 旧过期戳失效, 清历史否则僵尸消息复活 */
    {
        static unsigned s_lastClock = 0;
        unsigned nowC = GtClockNow();
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
        if (GtMouseGameXY(&mx, &my))
        {
            /* 并集防抖: 命中上帧框区, 或落在触发条上, 都算悬停。
             * g_CampValid 是上帧残留, 若二选一, 鼠标停在"触发条内/旧框区外"
             * 会隔帧翻转 show/hide。 */
            hoverCamp = MouseInCampRect(mx, my) ||
                        MouseInTriggerRect(mx, my, bitLineH, screenW, surfH);
        }
        liveCamp = HasLiveCamp();
        /* 显隐淡入淡出 0.5s (现实毫秒 GetTickCount, 与游戏帧数无关):
         * 全程 500ms, 步进按流逝毫秒算故与帧率无关; 滑过只走到一半
         * 就回来, 不闪。fade==0 时 showCamp=0, 整帧跳过绘制。
         * 目标分三档: 活消息=100; 区内驻留 (静止 300ms)=100;
         * 区内移动=托底 (HoverMinOpacity, 找区用); 离开=0。
         * 展开 (滚动条 + 全部历史) 跟瞬时悬停, 随底板一起淡入淡出。 */
        {
            static int s_fade = 0;
            static unsigned s_lastMs = 0;
            static int s_px = -9999, s_py = -9999;
            static unsigned s_moveMs = 0;
            unsigned nowMs = GetTickCount();
            int target;
            int parked;
            if (mx != -1 && (mx < s_px - 2 || mx > s_px + 2 ||
                             my < s_py - 2 || my > s_py + 2))
            {
                s_px = mx; s_py = my;
                s_moveMs = nowMs;
            }
            parked = (hoverCamp && nowMs - s_moveMs >= 300);
            if (liveCamp) target = 100;
            else if (parked) target = 100;
            else if (hoverCamp) target = g_Cfg.HoverMinOpacity;
            else target = 0;
            /* 全显锁存: 已到 100 且鼠标还在区上, 区内移动不再压暗,
             * 离开才淡出 (重进移动中则仍走托底, 住下再提亮)。 */
            if (hoverCamp && s_fade >= 100) target = 100;
            unsigned dt = s_lastMs ? nowMs - s_lastMs : 0;
            int step;
            if (dt > 500) dt = 500;
            step = (int)(dt * 100 / 500);
            if (step < 1) step = 1;
            s_lastMs = nowMs;
            if (s_fade < target) { s_fade += step; if (s_fade > target) s_fade = target; }
            else if (s_fade > target) { s_fade -= step; if (s_fade < target) s_fade = target; }
            /* 活消息即时满亮 (治时差): 新消息与引擎提示音同帧出现 (原版首字同帧),
             * 不再爬 500ms 斜坡; 淡出与悬停淡入仍走斜坡。 */
            if (liveCamp) s_fade = 100;
            showCamp = (s_fade > 0);
            camp.fade = s_fade;
        }
        {
            /* 展开锁存: 悬停即展开; 淡出中保持上帧 (hover 离开后历史可见淡出,
             * 否则淡出的是空盒、看起来像瞬灭); fade 到 0 才收回。活消息独占
             * 时仍只看活的。注意: 锁存只保持、不新设——末条消息过期时上帧
             * 是收拢态, 保持收拢直接消失 (原版行为); 若新设展开, 会闪出整块
             * 历史再整体消失。 */
            static int s_expand = 0;
            if (hoverCamp) s_expand = 1;
            else if (liveCamp) s_expand = 0;
            else if (camp.fade == 0) s_expand = 0;
            else if (!HasCampHistory()) s_expand = 0;
            /* else: 保持上帧 s_expand (淡出中), 不新设 */
            camp.bar = s_expand;   /* 滚动条悬停/淡出即显示 (alpha 随斜坡, 不闪) */
            camp.ignoreTimeout = s_expand;
            camp.fullBoard = s_expand;   /* 悬停/展开铺整块 (Phobos hover 态), 平时包裹 */
        }
        {
            static int s_lastHover = -1;
            if (hoverCamp != s_lastHover) { s_lastHover = hoverCamp; g_Skip = 0; }
        }
        /* 本帧 DrawBox 会重设新鲜度 (画出才置 1), 先清零 */
        g_CampValid = 0;
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
    camp.anim = g_Cfg.CampAnim;
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
    hint.fade = 100;   /* 提示框不渐隐, 行为不变 */
    hint.fontPx = g_Cfg.HintFontHeight; hint.bold = g_Cfg.HintBold;
    hint.pSkip = NULL; hint.bar = 0;
    hint.fullBoard = 1; hint.anim = 0;   /* 提示框行为不变: 整块底板+整行即时 */
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
        GtFillRectTrans(surf, &zr, &zc, 30);
        GtDrawOutline(surf, zr.X, zr.Y, zr.Width, zr.Height, 0, 255, 0, 90);
        drew = 1;
    }

    if (showCamp)
    {
        rc = DrawBox(surf, &camp, bitLineH, screenW, surfW, surfH, surfPitch);
        if (rc < 0) return rc;
        if (rc == 1) drew = 1;
    }

    if (g_Cfg.HintShow)
    {
        static int s_lastHint = -1;
        int on;
        rc = DrawBox(surf, &hint, bitLineH, screenW, surfW, surfH, surfPitch);
        on = (rc == 1);
        if (on != s_lastHint) { s_lastHint = on; LogHintWhy(on); }
        if (rc < 0) return rc;
        if (rc == 1) drew = 1;
    }

    /* 显示态悬停范围可视化 (调试): 有框才描白框 */
    if (g_Cfg.ShowHotZone && g_CampValid)
        GtDrawOutline(surf, g_CampRect.X, g_CampRect.Y, g_CampRect.Width, g_CampRect.Height,
                      255, 255, 255, 80);

    return drew ? 1 : 0;
}

/* 滚轮接管: 命中战役框则翻历史并吞掉滚轮, 返回 1=已吞。
 * dirUp: 1=上滚 (往更新的看), 0=下滚 (翻历史)。 */
int MsgText_WheelIn(int mx, int my, int dirUp)
{
    static unsigned s_wheelN = 0;
    if (!g_Cfg.Enable || !g_CampValid) return 0;
    if (!MouseInCampRect(mx, my)) return 0;
    if (dirUp)
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
        UtoA(dirUp ? 1u : 0u, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        LogLine(buf);
    }
    return 1;
}

/* 侧栏滚动让位: 命中战役框返回 1, 调用方跳到让位点。 */
int MsgText_OverCamp(int mx, int my)
{
    if (!g_Cfg.Enable || !g_CampValid) return 0;
    if (!MouseInCampRect(mx, my)) return 0;
    return 1;
}

/* 是否接管 (供 MOExperience.cpp): 与 MsgText_DrawFrame 同一开关。 */
int MsgText_Enabled(void)
{
    return g_Cfg.Enable ? 1 : 0;
}
