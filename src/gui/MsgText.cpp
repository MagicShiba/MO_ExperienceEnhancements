/*
 * MsgText.cpp - 消息文本显示 (战役框 + 操作与战斗提示), 配置节 [MsgText]。
 *
 * 钩子 (Syringe, 见 dist/MOExperience.dll.inj):
 *   消息: 0x5D3BA0 AddMessage 捕获; 0x5D49A0 Draw 入口绿框;
 *         0x5D4A94 Draw 内部 (自制消息框画完回 0x5D4A9B, 由 MOExperience.cpp
 *         的消息帧统一出口, 故本模块只提供 MsgText_DrawFrame)。
 *   交互: 0x777998 滚轮翻历史; 0x533F50 悬停时跳过侧栏滚动;
 *         0x55E477/62F/693/746 单人聊天框 (跳过 session 检查);
 *         0x69234B 雷达事件行点击门 (点文本 = 视口跳到事件发生地)。
 * 任何一步失败 (Lock 不上、GDI 未就绪) 都返回 0 走原版绘制, 绝不花屏。
 *
 * 渲染: Microsoft YaHei 经 GDI 光栅化到 32 位内存 DIB (灰度抗锯齿取覆盖率),
 * 经启动校准得到的 16 位格式 (565/555) alpha 混入 Composite。
 * 取色: ColorScheme BaseColor (HSV) 经 0x517440 转显示色 (Phobos 同用法)。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "Lang.h"
#include "OrderUtil.h"
#include "ChatEdit.h"
#include "MsgText.h"

/* ---------------- 地址 ---------------- */

#define ADDR_COLORSCHEME_ARRAY   0x00B054D0u
#define ADDR_SCHEME_TO_COLOR     0x00517440u

/* 雷达事件 (RadarEventClass) 活对象表: 引擎 Create 成功才往里挂。
 * 反汇编实证 (Create 0x65FA70 → ctor 0x65FB80 尾部 0x65FCD5 起):
 *   0xB04DAC = 指针数组基址, 0xB04DB8 = 在用个数 (追加时 count++, 退场时前移压缩)。
 * 每个对象: +0x00 = RadarEventType, +0x20 = MapCoords (CellStruct: X 低16/Y 高16)。
 * ★ 这套活对象表只收走完"同型同格抑制"的事件 (0x65FA70 里的距离去重),
 *   所以扫到的每一项都是真能在雷达上看到、也真能用空格跳过去的那个事件。 */
#define ADDR_RE_ITEMS            0x00B04DACu
#define ADDR_RE_COUNT            0x00B04DB8u
#define RE_OFF_TYPE              0x00u
#define RE_OFF_CELL              0x20u
#define RE_MAX_SCAN              48          /* 一帧最多扫几个 (引擎容量 10 起, 会长) */

/* 视口跳转: 见下面 MsgText_JumpToCell。ADDR_SET_TACTICAL_POS 在 EngineAddr.h。 */
#define ADDR_MAP_INSTANCE        0x0087F7E8u  /* MapClass::Instance */
#define ADDR_MAP_GET_CELL        0x005657A0u  /* CellClass* MapClass::GetCellAt(CellStruct*) */
#define ADDR_CELL_GET_COORDS     0x00480A30u  /* CoordStruct* CellClass::GetCellCoords(CoordStruct*) */
#define ADDR_Z_TO_SCREEN         0x006D20E0u  /* int TacticalClass::ZToScreen(int z) */

/* 点击门落点: ProcessClickCoords (0x692300) 的"未处理"出口, 栈形对齐。 */

#define OFF_REG_ESP              0x14
#define STK_ARG_NAME             0x04
#define STK_ARG_MESSAGE          0x0C
#define STK_ARG_CS               0x10
#define STK_ARG_TIMEOUT          0x18
#define STK_ARG_SP               0x1C

#define OFF_CSCHEME_ITEMS        0x04
#define OFF_CSCHEME_COUNT        0x10
#define OFF_CSCHEME_BASECOLOR    0x308

/* ---------------- 雷达事件 ----------------
 * 事件名走配置节 [MsgRadar] 的 Name0..Name16 (下标 = RadarEventType),
 * 用户想改文案只动 ini, 不必重编。ini 是 UTF-8, 这里是手写的 UTF-8→UTF-16
 * (无 CRT, 也不走 MultiByteToWideChar —— 那按 ACP 解码, 会把 UTF-8 中文吃坏)。*/
#define RADAR_SEC      "MsgRadar"
#define RADAR_NAME_N   17
#define RADAR_NAME_CAP 40

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
    int ExpandDelayMs; /* 悬停停放多少毫秒后才展开历史 (0=立即) */
    int PosX;
    int PosY;
    int TopGapLines;
    int MaxWidth;      /* >1 = 像素; 0 = 自动半屏 (小数走 MaxWidthPm) */
    int MaxWidthPm;    /* >0 = 按屏宽千分比 (1~1000, 如500=50%), 优先于 MaxWidth */
    int CampEnable;    /* 1 = 战役文本框显示 */
    int CampBottom;    /* 1 = 底部显示 (底边固定, 文本向上增长), 0 = 顶部显示 */
    int CampBottomMargin; /* 底部模式时距底边像素 */
    int CampCentered;  /* 1 = 水平居中, 0 = 左对齐 (PosX) */
    int CampAnim;      /* 1 = 打字机逐字 (原版 Animate 同速率), 0 = 整行即时 */
    int CampBarSide;   /* 滚动条: 0=不显示, 1=左侧, 2=右侧 */
    int LogMessages;
    int ShowHotZone;   /* 1 = 画出悬停生效范围 (调试悬停用) */
    int ScrollStep;    /* 滚轮步长(行) */
    int HintShow;      /* 1 = 左下显示操作与战斗提示 (sp=1) */
    int HintPosX;
    int HintBottom;    /* 距底边像素 */
    int HintFontHeight;/* 0 = 跟 BitFont 字高 */
    int HintBold;
    char HintFontFace[64]; /* 同上 (提示框用), 缺省 msyh.ttc */
    int HintColorFollow;/* 1 = 追随玩家实际颜色 (原版色), 0 = 白字 */
    int HintMaxLines;  /* 操作与战斗提示总行数 (雷达事件占剩下的) */
    int HintOpMaxLines;/* 其中"操作提示"最多占几行 (剩下的行数留给雷达事件) */
    int HintRadarShow; /* 1 = 把雷达事件写进操作与战斗提示 */
    int HintRadarClick;/* 1 = 雷达事件行可点击, 点一下视口跳过去 */
    int HintRadarHold; /* 雷达事件行显示时长 (引擎时钟刻, 1刻≈16ms) */
    int HintMaxWidth;  /* 0 = 自动 */
    int HintOpacity;   /* 底板不透明度 0~100; 0=不画底板但文字阴影照画 */
    int HintBarSide;   /* 滚动条: 0=不显示, 1=左侧, 2=右侧 */
    int HintTimeoutMul;/* 操作提示显示时长倍率 (1=原版速度) */
    char RadarName[RADAR_NAME_N][RADAR_NAME_CAP];   /* [MsgRadar] Name0..16 */
    char RadarNameOther[RADAR_NAME_CAP];            /* [MsgRadar] NameOther */
    wchar_t RadarNameW[RADAR_NAME_N][FONT_FACE_MAX];
    wchar_t RadarNameOtherW[FONT_FACE_MAX];
} MsgTextConfig;

static MsgTextConfig g_Cfg;
static MsgTextConfig s_SavedCfg;
static int s_HaveSavedCfg = 0;

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
    int      sp;        /* 1 = 提示类 (操作提示/雷达事件), 0 = 战役/聊天/系统 */
    int      kind;      /* 0 = 操作提示, 1 = 雷达事件 (可点击跳转) */
    short    cellX;     /* kind=1 的目标格 (点击跳转用) */
    short    cellY;
} ChatMsg;

/* 操作与战斗提示内部再分两路: 操作提示行有行数上限, 雷达事件行吃剩下的,
 * 行内区分靠 kind。 */
#define KIND_OP    0
#define KIND_RADAR 1

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
    m->kind   = KIND_OP;
    m->cellX  = 0;
    m->cellY  = 0;
    r->head = (r->head + 1) % r->cap;
    if (r->count < r->cap) r->count++;
}

/* 雷达事件入提示环: 文案就是 [MsgRadar] 里配的类型名 (不带坐标)。
 * 按 HintRadarHold 正常过期消失 (不永久驻留); 收起时只留最近几行,
 * 悬停末行触发区可展开看到同一段时间里的其余几条。 */
static unsigned g_RadarSeq = 0;   /* 新雷达事件序号 (只增不减, 清历史也不归零) */

static void RingPushRadar(int type, short cx, short cy)
{
    ChatMsg* m = &g_HintBuf[g_HintRing.head];
    const wchar_t* nm = (type >= 0 && type < RADAR_NAME_N)
                      ? g_Cfg.RadarNameW[type] : g_Cfg.RadarNameOtherW;
    wchar_t* p = m->text;
    unsigned now = GtClockNow();

    if (!nm || !nm[0]) nm = g_Cfg.RadarNameOtherW;
    while (*nm && p < m->text + MAX_TEXT_W - 1) *p++ = *nm++;
    *p = 0;

    m->name[0] = 0;
    m->color  = 0;
    m->born   = now;
    m->expire = now + (unsigned)g_Cfg.HintRadarHold;
    m->sp     = 1;
    m->kind   = KIND_RADAR;
    m->cellX  = cx;
    m->cellY  = cy;

    g_HintRing.head = (g_HintRing.head + 1) % g_HintRing.cap;
    if (g_HintRing.count < g_HintRing.cap) g_HintRing.count++;
    g_RadarSeq++;
}

/* 已报过的雷达事件, 跨帧保留: 扫到刷时间戳, 未扫到即引擎已删;
 * 同地址复用时以 (类型,格) 签名区分。 */
#define RE_SEEN_MAX 64
static void*    s_ReSeenP[RE_SEEN_MAX];
static int      s_ReSeenT[RE_SEEN_MAX];
static int      s_ReSeenX[RE_SEEN_MAX];
static int      s_ReSeenY[RE_SEEN_MAX];
static int      s_ReSeenF[RE_SEEN_MAX];
static int      s_ReSeenN = 0;

static void MsgText_PollRadar(void)
{
    static int s_lastFrame = -1;
    void* arr;                  /* 指针数组基址 */
    int cnt, i, j, k, fr, keep;

    if (!g_Cfg.Enable || !g_Cfg.HintShow || !g_Cfg.HintRadarShow)
    {
        s_ReSeenN = 0;
        return;
    }
    /* 一帧能被调好几次, 必须自门控帧号 (同 RepairAll/SWSBar 的做法)。 */
    fr = *(int*)0x00A8ED84u;
    if (fr == s_lastFrame) return;
    s_lastFrame = fr;

    arr = *(void**)ADDR_RE_ITEMS;
    cnt = IsPlausiblePtr(arr) ? *(int*)ADDR_RE_COUNT : 0;
    if (cnt < 0) cnt = 0;
    if (cnt > RE_MAX_SCAN) cnt = RE_MAX_SCAN;

    for (i = 0; i < cnt; i++)
    {
        void* obj = *(void**)((char*)arr + i * 4);
        unsigned char* ev = (unsigned char*)obj;
        int t, cell, sx, sy;
        if (!IsPlausiblePtr(ev)) continue;
        if (!IsPlausiblePtr((char*)ev + RE_OFF_CELL + 4)) continue;
        t = *(int*)(ev + RE_OFF_TYPE);
        cell = *(int*)(ev + RE_OFF_CELL);
        sx = (int)(short)(cell & 0xFFFF);
        sy = (int)(short)((cell >> 16) & 0xFFFF);

        for (j = 0; j < s_ReSeenN; j++) if (s_ReSeenP[j] == obj) break;
        if (j < s_ReSeenN)
        {
            int same = (s_ReSeenT[j] == t && s_ReSeenX[j] == sx && s_ReSeenY[j] == sy);
            s_ReSeenT[j] = t; s_ReSeenX[j] = sx; s_ReSeenY[j] = sy;
            s_ReSeenF[j] = fr;
            if (same) continue;          /* 还在表里 = 已经报过了 */
        }
        else
        {
            if (s_ReSeenN >= RE_SEEN_MAX)
            {
                /* 表满: 挤掉最老的一条 (帧号最小) */
                int old = 0;
                for (k = 1; k < s_ReSeenN; k++)
                    if ((int)(s_ReSeenF[k] - s_ReSeenF[old]) < 0) old = k;
                for (k = old; k < s_ReSeenN - 1; k++)
                {
                    s_ReSeenP[k] = s_ReSeenP[k + 1]; s_ReSeenT[k] = s_ReSeenT[k + 1];
                    s_ReSeenX[k] = s_ReSeenX[k + 1]; s_ReSeenY[k] = s_ReSeenY[k + 1];
                    s_ReSeenF[k] = s_ReSeenF[k + 1];
                }
                s_ReSeenN--;
            }
            j = s_ReSeenN++;
            s_ReSeenP[j] = obj;
            s_ReSeenT[j] = t; s_ReSeenX[j] = sx; s_ReSeenY[j] = sy;
            s_ReSeenF[j] = fr;
        }
        RingPushRadar(t, (short)sx, (short)sy);
    }

    /* 出表: 本帧没扫到的就是引擎那边已经删掉了 (对象活着就一直在表里) */
    keep = 0;
    for (i = 0; i < s_ReSeenN; i++)
    {
        if (s_ReSeenF[i] != fr) continue;
        if (keep != i)
        {
            s_ReSeenP[keep] = s_ReSeenP[i]; s_ReSeenT[keep] = s_ReSeenT[i];
            s_ReSeenX[keep] = s_ReSeenX[i]; s_ReSeenY[keep] = s_ReSeenY[i];
            s_ReSeenF[keep] = s_ReSeenF[i];
        }
        keep++;
    }
    s_ReSeenN = keep;
}

static unsigned ExpireStamp(int timeout)
{
    if (timeout < 0) return 0;
    return GtClockNow() + (unsigned)timeout;
}

static int MsgVisibleMul(ChatMsg* m, unsigned mul)
{
    unsigned span;
    if (m->expire == 0) return 1;   /* 永不过期 */
    if (m->kind == KIND_RADAR) mul = 1;
    if (mul < 1) mul = 1;
    span = (m->expire - m->born) * mul;
    return (int)(GtClockNow() - m->born) <= (int)span;
}

/* 最新一条仍可见的雷达事件 (PiP 自动跟随用)。倒扫提示环: 环里的顺序就是入队顺序,
 * 最新的在 count-1。只读, 不消耗任何状态。 */
int MsgText_LatestRadar(short* cx, short* cy, unsigned* seq)
{
    int i;
    for (i = g_HintRing.count - 1; i >= 0; i--)
    {
        ChatMsg* m = MsgAt(&g_HintRing, i);
        if (m->kind != KIND_RADAR) continue;
        if (!MsgVisibleMul(m, 1)) continue;
        if (cx) *cx = m->cellX;
        if (cy) *cy = m->cellY;
        if (seq) *seq = g_RadarSeq;
        return 1;
    }
    return 0;
}

/* ---------------- 取色 (ColorScheme -> 显示色) ----------------
 * 与引擎 Fancy_Text_Print_Wide (0x4A61C0) 完全一致。
 * 引擎做法 (反汇编实证): ECX = scheme+0x308 (即 &BaseColor, HSV 三元组),
 * 栈传 3 字节输出缓冲, call 0x517440 (HSV->RGB, stdcall, ret 4)。
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
    g_Cfg.ExpandDelayMs = GetPrivateProfileIntA(MSG_SECTION, "ExpandDelayMs", 300, g_IniPath);
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
    g_Cfg.CampEnable = GetPrivateProfileIntA(MSG_SECTION, "CampEnable", 1, g_IniPath);
    g_Cfg.CampBarSide = GetPrivateProfileIntA(MSG_SECTION, "CampBarSide", 2, g_IniPath);
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
    g_Cfg.HintMaxLines= GetPrivateProfileIntA(MSG_SECTION, "HintMaxLines", 8, g_IniPath);
    g_Cfg.HintOpMaxLines = GetPrivateProfileIntA(MSG_SECTION, "HintOpMaxLines", 4, g_IniPath);
    g_Cfg.HintRadarShow = GetPrivateProfileIntA(MSG_SECTION, "HintRadarShow", 1, g_IniPath);
    g_Cfg.HintRadarClick = GetPrivateProfileIntA(MSG_SECTION, "HintRadarClick", 1, g_IniPath);
    g_Cfg.HintRadarHold = GetPrivateProfileIntA(MSG_SECTION, "HintRadarHold", 900, g_IniPath);
    g_Cfg.HintMaxWidth= GetPrivateProfileIntA(MSG_SECTION, "HintMaxWidth", 320, g_IniPath);
    g_Cfg.HintOpacity = GetPrivateProfileIntA(MSG_SECTION, "HintOpacity", 45, g_IniPath);
    g_Cfg.HintBarSide = GetPrivateProfileIntA(MSG_SECTION, "HintBarSide", 2, g_IniPath);
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
    if (g_Cfg.ExpandDelayMs < 0) g_Cfg.ExpandDelayMs = 0;
    if (g_Cfg.ExpandDelayMs > 3000) g_Cfg.ExpandDelayMs = 3000;
    if (g_Cfg.MaxWidth < 0) g_Cfg.MaxWidth = 0;
    g_Cfg.CampBottom = g_Cfg.CampBottom ? 1 : 0;
    if (g_Cfg.CampBottomMargin < 0) g_Cfg.CampBottomMargin = 0;
    if (g_Cfg.CampBottomMargin > 2000) g_Cfg.CampBottomMargin = 2000;
    g_Cfg.CampCentered = g_Cfg.CampCentered ? 1 : 0;
    g_Cfg.CampAnim = g_Cfg.CampAnim ? 1 : 0;
    g_Cfg.CampEnable = g_Cfg.CampEnable ? 1 : 0;
    if (g_Cfg.CampBarSide < 0) g_Cfg.CampBarSide = 0;
    if (g_Cfg.CampBarSide > 2) g_Cfg.CampBarSide = 2;
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
    if (g_Cfg.HintMaxLines > 40) g_Cfg.HintMaxLines = 40;
    /* 操作提示行预算必须留在总行数之内, 且至少给雷达事件留一行 */
    if (g_Cfg.HintOpMaxLines < 0) g_Cfg.HintOpMaxLines = 0;
    if (g_Cfg.HintOpMaxLines > g_Cfg.HintMaxLines - 1) g_Cfg.HintOpMaxLines = g_Cfg.HintMaxLines - 1;
    if (g_Cfg.HintOpMaxLines < 0) g_Cfg.HintOpMaxLines = 0;
    g_Cfg.HintRadarShow = g_Cfg.HintRadarShow ? 1 : 0;
    g_Cfg.HintRadarClick = g_Cfg.HintRadarClick ? 1 : 0;
    if (g_Cfg.HintRadarHold < 1) g_Cfg.HintRadarHold = 1;
    if (g_Cfg.HintRadarHold > 100000) g_Cfg.HintRadarHold = 100000;
    if (g_Cfg.HintMaxWidth < 0) g_Cfg.HintMaxWidth = 0;
    if (g_Cfg.HintOpacity < 0) g_Cfg.HintOpacity = 0;
    if (g_Cfg.HintOpacity > 100) g_Cfg.HintOpacity = 100;
    if (g_Cfg.HintBarSide < 0) g_Cfg.HintBarSide = 0;
    if (g_Cfg.HintBarSide > 2) g_Cfg.HintBarSide = 2;
    if (g_Cfg.HintTimeoutMul < 1) g_Cfg.HintTimeoutMul = 1;
    if (g_Cfg.HintTimeoutMul > 10) g_Cfg.HintTimeoutMul = 10;
}

int MsgText_GetOption(int id)
{
    MsgText_EnsureInit();
    switch (id)
    {
    case MSG_OPT_CAMP_ENABLE:  return g_Cfg.CampEnable;
    case MSG_OPT_CAMP_LINES:   return g_Cfg.MaxLines;
    case MSG_OPT_CAMP_OPACITY: return g_Cfg.BoardOpacity;
    case MSG_OPT_CAMP_BOTTOM:  return g_Cfg.CampBottom;
    case MSG_OPT_CAMP_CENTERED:return g_Cfg.CampCentered;
    case MSG_OPT_CAMP_WIDTH:   return g_Cfg.MaxWidth > 0 ? g_Cfg.MaxWidth : 550;
    case MSG_OPT_CAMP_ANIM:    return g_Cfg.CampAnim;
    case MSG_OPT_CAMP_BAR:     return g_Cfg.CampBarSide;
    case MSG_OPT_HINT_SHOW:    return g_Cfg.HintShow;
    case MSG_OPT_HINT_COLOR:   return g_Cfg.HintColorFollow;
    case MSG_OPT_HINT_LINES:   return g_Cfg.HintMaxLines;
    case MSG_OPT_HINT_WIDTH:   return g_Cfg.HintMaxWidth;
    case MSG_OPT_HINT_BAR:     return g_Cfg.HintBarSide;
    case MSG_OPT_HINT_RADAR:   return g_Cfg.HintRadarShow;
    default:                   return 0;
    }
}

void MsgText_SetOption(int id, int value)
{
    MsgText_EnsureInit();
    switch (id)
    {
    case MSG_OPT_CAMP_ENABLE:
        g_Cfg.CampEnable = value ? 1 : 0;
        break;
    case MSG_OPT_CAMP_LINES:
        if (value < 1) value = 1;
        if (value > 40) value = 40;
        g_Cfg.MaxLines = value;
        break;
    case MSG_OPT_CAMP_OPACITY:
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        g_Cfg.BoardOpacity = value;
        break;
    case MSG_OPT_CAMP_BOTTOM:
        g_Cfg.CampBottom = value ? 1 : 0;
        break;
    case MSG_OPT_CAMP_CENTERED:
        g_Cfg.CampCentered = value ? 1 : 0;
        break;
    case MSG_OPT_CAMP_WIDTH:
        if (value < 200) value = 200;
        if (value > 1600) value = 1600;
        g_Cfg.MaxWidth = value;
        g_Cfg.MaxWidthPm = 0;
        break;
    case MSG_OPT_CAMP_ANIM:
        g_Cfg.CampAnim = value ? 1 : 0;
        break;
    case MSG_OPT_CAMP_BAR:
        if (value < 0) value = 0;
        if (value > 2) value = 2;
        g_Cfg.CampBarSide = value;
        break;
    case MSG_OPT_HINT_SHOW:
        g_Cfg.HintShow = value ? 1 : 0;
        break;
    case MSG_OPT_HINT_COLOR:
        g_Cfg.HintColorFollow = value ? 1 : 0;
        break;
    case MSG_OPT_HINT_LINES:
        if (value < 1) value = 1;
        if (value > 40) value = 40;
        g_Cfg.HintMaxLines = value;
        if (g_Cfg.HintOpMaxLines > g_Cfg.HintMaxLines - 1)
            g_Cfg.HintOpMaxLines = g_Cfg.HintMaxLines - 1;
        if (g_Cfg.HintOpMaxLines < 0) g_Cfg.HintOpMaxLines = 0;
        break;
    case MSG_OPT_HINT_WIDTH:
        if (value < 100) value = 100;
        if (value > 1600) value = 1600;
        g_Cfg.HintMaxWidth = value;
        break;
    case MSG_OPT_HINT_BAR:
        if (value < 0) value = 0;
        if (value > 2) value = 2;
        g_Cfg.HintBarSide = value;
        break;
    case MSG_OPT_HINT_RADAR:
        g_Cfg.HintRadarShow = value ? 1 : 0;
        break;
    default:
        break;
    }
}

void MsgText_SaveOptions(void)
{
    MsgText_EnsureInit();
    s_SavedCfg = g_Cfg;
    s_HaveSavedCfg = 1;
}

void MsgText_RestoreOptions(void)
{
    MsgText_EnsureInit();
    if (s_HaveSavedCfg) g_Cfg = s_SavedCfg;
}

static const char* const kRadarNameDef[RADAR_NAME_N] =
{
    "战斗",       "非战斗",     "空投区",     "基地遭袭",
    "矿车遭袭",   "发现敌军",   "单位出厂",   "单位损失",
    "单位修复",   "建筑被渗透", "建筑被占领", "信标已放置",
    "探测到超武", "超武已发动", "桥梁修复",   "驻军撤离",
    "盟友基地遭袭"
};

/* UTF-8 → UTF-16, 返回写完的尾指针。带 BOM 会被吃掉, 坏字节按替换字符。 */
static wchar_t* Utf8ToWide(wchar_t* dst, wchar_t* end, const unsigned char* s)
{
    unsigned c, c2;
    if (s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) s += 3;
    while (*s)
    {
        if (*s < 0x80)                       c = *s++;
        else if ((*s & 0xE0) == 0xC0)        { c = *s++ & 0x1Fu; c = (c << 6) | (*s++ & 0x3Fu); }
        else if ((*s & 0xF0) == 0xE0)
        {
            c = *s++ & 0x0Fu;
            c = (c << 6) | (*s++ & 0x3Fu);
            c = (c << 6) | (*s++ & 0x3Fu);
        }
        else if ((*s & 0xF8) == 0xF0)
        {
            c  = *s++ & 0x07u;
            c2 = *s++ & 0x3Fu;
            c  = (c << 6) | c2;
            c  = (c << 6) | (*s++ & 0x3Fu);
            c2 = *s++ & 0x3Fu;
            c  = (c << 6) | c2;
            c -= 0x10000u;
            if (dst + 1 < end) *dst++ = (wchar_t)(0xD800u + ((c >> 10) & 0x3FFu));
            if (dst < end)     *dst++ = (wchar_t)(0xDC00u + (c & 0x3FFu));
            continue;
        }
        else { s++; c = 0xFFFDu; }
        if (dst >= end) break;
        *dst++ = (wchar_t)c;
    }
    if (dst < end) *dst = 0;
    else if (end > dst) end[-1] = 0;
    return dst;
}

static void MsgText_LoadRadarNames(void)
{
    char key[16], kb[8];
    unsigned q;
    int i;
    for (i = 0; i < RADAR_NAME_N; i++)
    {
        q = 0;
        q = AppendCStr(key, q, "Name", sizeof(key));
        UtoA((unsigned)i, kb);
        q = AppendCStr(key, q, kb, sizeof(key));
        Lang_GetA(RADAR_SEC, key, kRadarNameDef[i],
                  g_Cfg.RadarName[i], sizeof(g_Cfg.RadarName[i]));
    }
    Lang_GetA(RADAR_SEC, "NameOther", "雷达事件",
              g_Cfg.RadarNameOther, sizeof(g_Cfg.RadarNameOther));
    /* 顺手转成宽字符 (每帧不转, 只在载入时一次) */
    for (i = 0; i < RADAR_NAME_N; i++)
        Utf8ToWide(g_Cfg.RadarNameW[i], g_Cfg.RadarNameW[i] + FONT_FACE_MAX,
                   (const unsigned char*)g_Cfg.RadarName[i]);
    Utf8ToWide(g_Cfg.RadarNameOtherW, g_Cfg.RadarNameOtherW + FONT_FACE_MAX,
               (const unsigned char*)g_Cfg.RadarNameOther);
    /* 配成空串就退回默认, 不让用户一条空行把整个提示搞没 */
    for (i = 0; i < RADAR_NAME_N; i++)
        if (!g_Cfg.RadarNameW[i][0])
            Utf8ToWide(g_Cfg.RadarNameW[i], g_Cfg.RadarNameW[i] + FONT_FACE_MAX,
                       (const unsigned char*)kRadarNameDef[i]);
    if (!g_Cfg.RadarNameOtherW[0])
        Utf8ToWide(g_Cfg.RadarNameOtherW, g_Cfg.RadarNameOtherW + FONT_FACE_MAX,
                   (const unsigned char*)"雷达事件");
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
    Lang_EnsureInit();
    MsgText_LoadConfig();
    MsgText_LoadRadarNames();
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
    /* 操作与战斗提示的行预算 (总行 / 操作行 / 雷达事件行) */
    p = 0;
    p = AppendCStr(buf, p, "hint: MaxLines=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.HintMaxLines, buf + p);
    p = AppendCStr(buf, p, " OpMax=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.HintOpMaxLines, buf + p);
    p = AppendCStr(buf, p, " RadarShow=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.HintRadarShow, buf + p);
    p = AppendCStr(buf, p, " RadarClick=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.HintRadarClick, buf + p);
    p = AppendCStr(buf, p, " RadarHold=", sizeof(buf));
    p += UtoA((unsigned)g_Cfg.HintRadarHold, buf + p);
    LogLine(buf);
    /* 字体解析放日志打开之后, 解析结果才能记入日志 */
    ResolveFontFace(g_Cfg.FontFace, "camp", g_CampFace, L"Microsoft YaHei");
    ResolveFontFace(g_Cfg.HintFontFace, "hint", g_HintFace, L"Microsoft YaHei");
}

void MsgText_ReloadLang(void)
{
    if (g_InitDone) MsgText_LoadRadarNames();
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

/* ---------------- 双框: 战役长留 + 操作与战斗提示短命 ----------------
 * sp=1 (操作提示: 快捷键/路径点; 雷达事件) 进左下操作与战斗提示框,
 *   按引擎超时自然消失;
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
    int opRowCap;    /* >0: "操作提示"路的行数上限 (雷达事件吃剩下的) */
    int opacity;
    int fade;        /* 0-100, DrawBox 内缩放底板/阴影/文字/滚动条透明度 */
    int shadowNoBoard; /* 1 = 底板透明度为 0 时仍画文字阴影 (提示框用) */
    int fontPx;      /* 0 = 跟随 BitFont */
    int bold;
    int* pSkip;      /* 战役框专用: 写聊天输入框锚点 (提示框不碰) */
    int* pScroll;   /* 非 NULL = 该框可滚轮翻历史 (内部维护的 skip 行数) */
    int* pPrevTotal;/* pScroll 配套: 上一帧总行数 (保持 skip 锚定) */
    int bar;         /* 滚动条: 0=不画, 1=按 barSide 画 */
    int barSide;     /* 1=左侧, 2=右侧 */
    int schemeColor; /* 1 = 用颜色方案 (原版色), 0 = 白字 */
    unsigned tmoMul; /* 超时倍率 (操作提示延长用, 战役=1; 雷达事件不吃) */
    int slot;        /* GDI 槽位 (战役=0, 提示=1) */
    int pickHits;    /* 1 = 记下雷达事件行的屏上矩形 + 悬停区 (点击门/展开用) */
    const wchar_t* face; /* 字体名 (g_CampFace/g_HintFace) */
    MsgRing* ring;   /* 专属消息环 (双流分离存储) */
    int fullBoard;   /* 1=整块底板 (悬停/展开, Phobos hover 态), 0=包裹文本 */
    int anim;        /* 1=打字机逐字 (只战役框), 0=整行即时 */
    int expanded;    /* 1 = 展开态 (把过期但仍留环里的也算进来, 于是总数能超窗口) */
} BoxSpec;

static int g_Skip = 0;          /* 0 = 跟随最新 */
static int s_prevCampTotal = 0;

/* 战役框上一帧矩形 (滚轮命中 + 悬停判定用) */
static RectangleStruct g_CampRect;
static int g_CampValid = 0;
/* 操作与战斗提示的悬停区。只有提示框 (pickHits=1) 写。
 *   box     = 整框 (本帧画出来才有, 供"展开后框内任意位置都算悬停")
 *   trigger = 末行那一行高, **跟有没有内容无关**, 独立算 (见 HintTriggerGeom)。
 * 收起时只认 trigger 才展开; 展开后框内任意位置都留着, 鼠标才能移上去点
 * 历史条目 —— 早先只认 trigger, 一往上移就收起, 历史根本点不到。 */
static RectangleStruct g_HintBox;
static int g_HintZonesOk = 0;
static int s_hintExpand = 0;
static int g_HintSkip = 0;
static int s_prevHintTotal = 0;
/* 聊天输入框锚点缓存: DrawBox 画战役框时顺手写下 (提前返回之前), 输入框直接读,
 * 不自己重算 —— 两边各算各的就会出现"首次不对齐、之后才对齐"。*/
static int g_AnchorX = 0, g_AnchorY = 0, g_AnchorValid = 0;

/* 注: 悬停展开带停放延迟 (ExpandDelayMs), 鼠标只是划过不会立刻弹历史;
 * 停放够时间才展开, 透明度斜坡保证无硬闪, 历史随底板一起淡入。
 * 划过即见淡影 (HoverMinOpacity) 供找区。 */

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
        if (m->sp == 0 && MsgHasContent(m) && MsgVisibleMul(m, 1)) return 1;
    }
    return 0;
}

/* 操作与战斗提示框显隐跳变日志 (newestExpIn = 最新提示距引擎过期还剩多少 tick, 62.5 tick≈1秒;
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

/* 雷达事件行的屏上命中区 (DrawBox 每帧重写, 点击门当帧直接查表)。
 * 只有行首两三个字的宽度才算可点 —— 整行铺开的话, 框压在地图上, 想点框
 * 底下的地图就总被抢走。 */
#define MAX_RE_HIT 40
#define RE_HIT_GLYPHS 3
typedef struct { int x, y, w, h; short cx, cy; } ReHit;
static ReHit g_ReHit[MAX_RE_HIT];
static int   g_ReHitN = 0;

/* 行首 N 个字的像素宽 (可点区宽度)。取不到字宽就退回 3 倍行高。 */
static int ReHitWidth(const wchar_t* s, int vis, int adv, int maxW)
{
    int i, w = 0, a;
    for (i = 0; i < RE_HIT_GLYPHS && i < vis && s[i]; i++)
    {
        a = GtCharAdv(s[i]);
        if (a > 0) w += a;
    }
    if (w <= 0) w = adv * RE_HIT_GLYPHS;
    if (w > maxW) w = maxW;
    return w;
}

/* 挑选+折行 (用当前 GDI 字体度量), 结果进 g_LineBuf/g_Picked;
 * picked 条数写 *pPicked, 返回总行数。
 * 行预算三道闸 (操作与战斗提示):
 *   - rowBudget  = 总行数上限 (HintMaxLines);
 *   - opRowCap   = "操作提示"路的行数上限 (HintOpMaxLines);
 *   - radarCap   = "雷达事件"路的行数上限 (rowBudget - opRowCap)。
 * 三道闸各自独立 ⇒ 一波战斗事件顶不掉操作提示那几行, 反过来也一样。
 * 某一路的预算满了就整条跳过 (不折半显示), 免得单条长消息把预算吃光。 */
static int PickLines(MsgRing* R, int wantSp, int ignoreTimeout, unsigned tmoMul,
                     int textW, int rowBudget, int opRowCap, int* pPicked)
{
    int picked = 0, rows = 0, i, k, total = 0;
    int opRows = 0, radarRows = 0;
    int radarCap = rowBudget - opRowCap;
    /* 名字前缀: 联机恒显示 (要区分发言者), 单机默认不显示 (只有自己一个名字, 是噪音)。 */
    int showName = GameIsMultiplayer() || ChatEdit_ShowNameSP();

    if (radarCap < 0) radarCap = 0;
    for (i = R->count - 1; i >= 0 && picked < MAX_PICKED && rows < rowBudget; i--)
    {
        ChatMsg* m = MsgAt(R, i);
        const wchar_t* body;
        const wchar_t* seg;
        int cnt = 0, off = 0;
        int cap;                       /* 这一条所属路的行数上限 */
        if (m->sp != wantSp) continue;
        /* ignoreTimeout (展开态) 只冻住雷达事件历史 —— 操作提示照常按引擎
         * 超时消失, 免得展开时框里堆着一堆早就该走的操作提示。 */
        if (m->kind == KIND_RADAR)
        {
            if (!ignoreTimeout && !MsgVisibleMul(m, 1)) continue;
            cap = radarCap;
            if (radarRows >= cap) continue;
        }
        else
        {
            if (!ignoreTimeout && !MsgVisibleMul(m, tmoMul)) continue;
            cap = opRowCap;
            if (opRows >= cap) continue;
        }
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
        if (m->kind == KIND_RADAR) radarRows += cnt;
        else opRows += cnt;
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
    int rowBudget = 0;
    int drawn = 0, boardOp = 0;
    RectangleStruct rect;
    ColorStruct board;
    unsigned char* locked;

    fontPx = B->fontPx > 0 ? B->fontPx : bitLineH;
    if (!GdiEnsure(fontPx, B->bold, B->slot, B->face)) return -1;
    adv = g_G->adv > 0 ? g_G->adv : bitLineH;

    /* 雷达事件行的命中区与悬停区本框独占, 每帧先清空 (放在所有提前返回之前):
     * 框空掉/画不出时表也必须是空的, 否则上一帧的行还能点。 */
    if (B->pickHits) { g_ReHitN = 0; g_HintZonesOk = 0; }

    maxW = MsgBoxGeom(screenW, B->maxW, B->maxWPm, B->centered, B->x, &x);
    textW = maxW;
    if (textW < 40) textW = 40;

    /* 可见窗口恒为 B->limit (= HintMaxLines), 不另设行数。
     * 展开态改的是**挑选预算**: 收起只挑还活着的 (≤ limit), 展开把过期但
     * 仍留环里的历史也挑进来 (预算放到 MAX_PICKED) —— totalAll 于是能超过
     * limit, 滚动条/滚轮才有东西可翻。窗口大小始终不变。 */
    limit = B->limit;
    rowBudget = B->expanded ? MAX_PICKED : B->rowBudget;

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

    totalAll = PickLines(B->ring, B->wantSp, B->ignoreTimeout, B->tmoMul, textW,
                        rowBudget, B->opRowCap, &picked);
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
    if (limit > totalAll) limit = totalAll;

    if (B->pScroll)
    {
        int* ps = B->pScroll;
        if (B->pPrevTotal)
        {
            if (*ps > 0 && totalAll > *B->pPrevTotal) *ps += totalAll - *B->pPrevTotal;
            *B->pPrevTotal = totalAll;
        }
        if (*ps > totalAll - limit) *ps = totalAll - limit;
        if (*ps < 0) *ps = 0;
        skip = *ps;
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
    /* 底板透明度叠渐隐 (fade=100 即引擎原版值); 0 不画底板
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
        int isRadar = (m->kind == KIND_RADAR);
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
            /* 雷达事件行: 记命中区 (只有行首几个字宽) + 悬停提亮做"可点"示意。
             * 命中区在 Blit 之前写, 免得覆盖到还没解完的字。 */
            if (isRadar && B->pickHits)
            {
                int hx = x - 3;
                int hw = ReHitWidth(g_LineBuf[k][r].start, vis, adv, maxW + 3);
                if (g_ReHitN < MAX_RE_HIT)
                {
                    ReHit* hit = &g_ReHit[g_ReHitN++];
                    hit->x = hx; hit->y = ly - 2; hit->w = hw; hit->h = adv + 4;
                    hit->cx = m->cellX; hit->cy = m->cellY;
                }
                if (g_Cfg.HintRadarClick)
                {
                    int mx = -1, my = -1;
                    if (GtMouseGameXY(&mx, &my) &&
                        mx >= hx && mx < hx + hw && my >= ly - 2 && my < ly - 2 + adv + 4)
                    {
                        RectangleStruct mk;
                        mk.X = hx; mk.Y = ly - 2; mk.Width = 2; mk.Height = adv + 4;
                        GtFillRectTrans(surf, &mk, &fg, 90 * B->fade / 100);
                        fg.R = 255; fg.G = 240; fg.B = 160;
                    }
                }
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

    /* 悬停区: 整框 + 末行那一行高的触发区。底边锚定 ⇒ 末行位置与总行数无关,
     * 展开/收起都不会抖。收起时只认 trigger 才展开; 展开后框内任意位置
     * 都留着, 鼠标才能移上去点历史条目。 */
    if (B->pickHits)
    {
        g_HintBox = rect;
        g_HintZonesOk = 1;
    }

    if (B->bar && B->barSide != 0 && totalAll > limit)
    {
        int maxSkip = totalAll - limit;
        int barX = (B->barSide == 1) ? rect.X - 5 : rect.X + rect.Width + 1;
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

/* ---------------- 雷达事件行点击 → 视口跳转 ----------------
 * 格 (X,Y) → 格心 lepton 后调 TacticalClass::SetTacticalPosition。
 * 不经 MapClass::GetTargetCell 查格高: 点击门时机 Cells 表可能在重建,
 * 查表会崩, 省掉只差半个格高。SetTacticalPosition 只读 ViewBounds, 不碰 Cells。
 * 联机安全: 只动本地摄像机, 不写模拟对象、不发事件。 */
typedef struct { int X, Y, Z; } Crd;
typedef void (__attribute__((thiscall)) *FnSetTacticalPos)(void*, const Crd*);
typedef void* (__attribute__((thiscall)) *FnMapGetCell)(void*, void*);
typedef void* (__attribute__((thiscall)) *FnCellGetCoords)(void*, void*);
typedef int (__attribute__((fastcall)) *FnZToScreen)(int);

/* 格子的地形高度 (只读)。雷达事件/单位在坡上时, 引擎的相机换算会减去
 * ZToScreen(Z), 否则目标会偏高 (事件看起来在小窗上方)。 */
int MsgText_CellZ(short cx, short cy)
{
    /* MapClass::Instance 是 0x87F7E8 处的对象本身, 不是指针变量。 */
    void* map = (void*)(size_t)ADDR_MAP_INSTANCE;
    void* pCell;
    short cell[2];
    Crd c;

    cell[0] = cx;
    cell[1] = cy;
    pCell = ((FnMapGetCell)(size_t)ADDR_MAP_GET_CELL)(map, cell);
    if (!pCell || !IsPlausiblePtr(pCell) || pCell == (void*)0x00ABDC50u) return 0;
    c.X = c.Y = c.Z = 0;
    ((FnCellGetCoords)(size_t)ADDR_CELL_GET_COORDS)(pCell, &c);
    return c.Z;
}

int MsgText_ZToScreen(int z)
{
    if (z <= 0) return 0;
    return ((FnZToScreen)(size_t)ADDR_Z_TO_SCREEN)(z);
}

static void MsgText_JumpToCell(short cx, short cy)
{
    void* tac = *(void**)ADDR_TACTICAL;
    Crd c;

    if (!IsPlausiblePtr(tac)) return;
    c.X = ((int)cx << 8) + 0x80;
    c.Y = ((int)cy << 8) + 0x80;
    c.Z = MsgText_CellZ(cx, cy);
    ((FnSetTacticalPos)(size_t)ADDR_SET_TACTICAL_POS)(tac, &c);
}

/* 落点是不是某条雷达事件行。命中返回 1 并写出目标格。 */
static int MsgText_HitRadarRow(int mx, int my, short* pcx, short* pcy)
{
    int i;
    for (i = 0; i < g_ReHitN; i++)
    {
        ReHit* h = &g_ReHit[i];
        if (mx < h->x || mx >= h->x + h->w) continue;
        if (my < h->y || my >= h->y + h->h) continue;
        *pcx = h->cx; *pcy = h->cy;
        return 1;
    }
    return 0;
}

/* 点击门 (0x69234B): 落在雷达事件行上跳视口并吞掉这次地图点击,
 * 否则返回 0 走原版。门内读按键加已处理标记, 保证一次按下只处理一次。 */
extern "C" __declspec(dllexport) unsigned __cdecl MsgText_RadarClickGate(void* regs)
{
    static int s_LbPress = 0;
    int mx, my, lb;
    short cx = 0, cy = 0;
    (void)regs;

    if (!g_Cfg.Enable || !g_Cfg.HintShow || !g_Cfg.HintRadarClick ||
        !g_Cfg.HintRadarShow || g_ReHitN <= 0)
        return 0;
    lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    if (!lb) { s_LbPress = 0; return 0; }   /* 松开: 下一次按下重新允许处理 */
    if (s_LbPress) return 0;                /* 本次按下已处理过 (按住拖动不重复) */
    s_LbPress = 1;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    if (!MsgText_HitRadarRow(mx, my, &cx, &cy)) return 0;
    MsgText_JumpToCell(cx, cy);
    return ADDR_CLICKGATE_SKIP;
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

/* 末行触发区的几何: **不依赖本帧画没画出内容**, 和战役框的 CampTriggerRect
 * 一个路子。底边锚定 ⇒ 末行位置与总行数无关, 展开/收起都不抖。
 * 所以"一条信息都不剩"时触发区照样在那儿, 悬停它照样能把历史拉出来。 */
static void HintTriggerGeom(int bitLineH, int screenW, int surfH,
                           RectangleStruct* out)
{
    int fontPx, adv, maxW, x;
    fontPx = g_Cfg.HintFontHeight > 0 ? g_Cfg.HintFontHeight : bitLineH;
    if (fontPx <= 0) fontPx = bitLineH;
    if (GdiEnsure(fontPx, g_Cfg.HintBold, 1, g_HintFace))
        adv = (g_G->adv > 0 ? g_G->adv : fontPx + 2);
    else
        adv = fontPx + 2;
    maxW = MsgBoxGeom(screenW, g_Cfg.HintMaxWidth, 0, 0, g_Cfg.HintPosX, &x);
    out->X = x - 3;
    out->Width = maxW + 6;
    /* 与 DrawBox 的末行命中带同一口径 (ly-2, 高 adv+4):
     * 末行 ly = surfH - bottomMargin - adv - 2 */
    out->Y = surfH - g_Cfg.HintBottom - adv - 4;
    out->Height = adv + 4;
}

static int InRectXY(int mx, int my, RectangleStruct* r)
{
    return (mx >= r->X && mx < r->X + r->Width &&
            my >= r->Y && my < r->Y + r->Height);
}

/* 输入框锚点 (供 ChatEdit.cpp): 底部贴战役框下沿+6px, 顶部贴战役框顶边。
 * 读 DrawBox 缓存的绘制结果 (与战役框同源, 不另算); 缓存未就绪时退回 1 行触发矩形。
 * 返回 1=有锚点, 0=无 (调用方退回原生位置)。 */
int MsgText_ChatAnchor(int surfW, int surfH, int bitLineH, int* px, int* py)
{
    RectangleStruct tr;
    int adv, gap = 6, ax, ay;
    if (!g_Cfg.Enable) return 0;
    if (!g_Cfg.CampEnable) return 0;
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

/* 渲染期主体: 战役框可见性/淡入淡出 + 触发条绿框 + 战役框 + 操作与战斗提示。
 * 返回 1=画出, 0=无内容, -1=GDI失败, -2=Lock失败。 */
int MsgText_DrawFrame(void* surf, int bitLineH, int screenW,
                      int surfW, int surfH, int surfPitch)
{
    int drew = 0, rc, showCamp = 0;
    int liveCamp = 0, hoverCamp = 0;
    BoxSpec camp, hint;

    if (!g_Cfg.Enable) return 0;
    /* 自定义战役文本框关闭 = 整个消息框交回引擎原版绘制 (战役与提示一起),
     * 见 MsgText_Enabled 同一开关。清掉上一帧的命中/锚点缓存, 免得残留在
     * 未绘制时仍能滚轮/悬停/点击。 */
    if (!g_Cfg.CampEnable)
    {
        g_CampValid = 0;
        g_HintZonesOk = 0;
        g_ReHitN = 0;
        g_AnchorValid = 0;
        return 0;
    }
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

    /* 雷达事件: 扫引擎的活对象表, 把新冒出来的事件写进提示环。
     * 放在排版之前 —— 本帧排版就能看见刚发生的事件。 */
    if (g_Cfg.HintShow && g_Cfg.HintRadarShow) MsgText_PollRadar();

    /* 战役框可见性: 有活消息, 或鼠标悬停 (已画出的框区/触发条);
     * 超时隐藏后, 鼠标移到触发条 (与顶部文本左右对齐的 1 行高绿框)
     * 即重现历史。触发条细长, 不再留边缘余量。 */
    {
        int mx = -1, my = -1;
        int parkedExpand = 0;
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
            /* 展开用独立延迟: 鼠标停放够 ExpandDelayMs 才展开历史, 免得划过/
             * 操作那片区域时立刻弹出一整块历史。0 = 立即 (旧行为)。 */
            parkedExpand = (hoverCamp &&
                            (unsigned)(nowMs - s_moveMs) >= (unsigned)g_Cfg.ExpandDelayMs);
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
            /* 展开: 悬停停放一会 (ExpandDelayMs) 才展开显示全部历史, 免得鼠标
             * 只是划过/要操作那片区域时立刻弹出一整块历史。
             * 一旦展开就锁存到鼠标离开 (区内移动不收起, 鼠标才能移上去点历史
             * 条目); 淡出中保持上帧 (hover 离开后历史可见淡出, 否则淡出的是空盒、
             * 看起来像瞬灭); fade 到 0 才收回。活消息独占时仍只看活的。 */
            static int s_hoverExpand = 0;
            static int s_expand = 0;
            if (!hoverCamp) s_hoverExpand = 0;
            else if (parkedExpand) s_hoverExpand = 1;
            /* else: 悬停但还没停放够 —— 保持上帧值, 不新设 */
            if (s_hoverExpand) s_expand = 1;
            else if (liveCamp) s_expand = 0;
            else if (camp.fade == 0) s_expand = 0;
            else if (!HasCampHistory()) s_expand = 0;
            /* else: 保持上帧 s_expand (淡出中), 不新设 */
            camp.bar = s_expand;   /* 滚动条悬停/淡出即显示 (alpha 随斜坡, 不闪) */
            camp.barSide = g_Cfg.CampBarSide;
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
    camp.limit = g_Cfg.MaxLines; camp.rowBudget = 400; camp.opRowCap = 400;
    camp.opacity = g_Cfg.BoardOpacity; camp.shadowNoBoard = 0;
    camp.fontPx = g_Cfg.FontHeight; camp.bold = g_Cfg.Bold;
    camp.pSkip = &g_Skip;
    camp.pScroll = &g_Skip; camp.pPrevTotal = &s_prevCampTotal;
    camp.anim = g_Cfg.CampAnim;
    camp.ring = &g_CampRing;
    camp.schemeColor = 1;
    camp.tmoMul = 1;
    camp.slot = 0;
    camp.pickHits = 0;
    camp.expanded = 0;
    camp.face = g_CampFace;

    /* 操作与战斗提示的展开态。触发区几何独立算 (HintTriggerGeom), 不看本帧
     * 有没有内容 —— 所以一条信息都不剩时, 悬停末行照样能把历史拉出来。
     * 收起时只认末行触发区; 一旦展开, 框内任意位置都算悬停, 鼠标才能往上
     * 移去点历史条目。 */
    if (g_Cfg.HintShow && g_Cfg.HintRadarShow)
    {
        int hmx = -1, hmy = -1;
        if (GtMouseGameXY(&hmx, &hmy))
        {
            RectangleStruct tr;
            HintTriggerGeom(bitLineH, screenW, surfH, &tr);
            if (InRectXY(hmx, hmy, &tr) || (s_hintExpand && g_HintZonesOk &&
                                            InRectXY(hmx, hmy, &g_HintBox)))
                s_hintExpand = 1;
            else
                s_hintExpand = 0;
        }
        else s_hintExpand = 0;
    }
    else s_hintExpand = 0;
    /* 展开/收起切换时把滚动位置归零 (同战役框 hover 变化时清 g_Skip)。 */
    {
        static int s_lastExpand = -1;
        if (s_lastExpand != s_hintExpand) { s_lastExpand = s_hintExpand; g_HintSkip = 0; }
    }

    /* 操作与战斗提示: 左下, sp=1 自然消失。
     * HintMaxLines = 框里最多同时显示几行; 其中操作提示最多占 HintOpMaxLines,
     * 剩下的行数给雷达事件 (见 PickLines 的三道闸)。
     * 雷达事件到 HintRadarHold 就过期; 悬停末行触发区展开后, 过期但仍留环
     * 里的也算进来 —— 窗口还是 HintMaxLines 行, 多的用滚轮翻。 */
    hint.wantSp = 1;
    hint.x = g_Cfg.HintPosX; hint.yBase = 0; hint.topGap = 0;
    hint.autoBottom = 1; hint.bottomMargin = g_Cfg.HintBottom;
    hint.centered = 0;
    hint.maxW = g_Cfg.HintMaxWidth; hint.maxWPm = 0;
    hint.limit = g_Cfg.HintMaxLines;
    hint.rowBudget = g_Cfg.HintMaxLines;
    hint.opRowCap = g_Cfg.HintRadarShow ? g_Cfg.HintOpMaxLines : g_Cfg.HintMaxLines;
    hint.opacity = g_Cfg.HintOpacity; hint.shadowNoBoard = 1;  /* 0=无底但阴影照画 */
    hint.fade = 100;   /* 提示框不渐隐, 行为不变 */
    hint.fontPx = g_Cfg.HintFontHeight; hint.bold = g_Cfg.HintBold;
    hint.pSkip = NULL;   /* 聊天输入框锚点只归战役框 */
    hint.pScroll = &g_HintSkip; hint.pPrevTotal = &s_prevHintTotal;
    /* 滚动条: 收起时 totalAll<=limit 天然不画, 展开看到更多行才出来 */
    hint.bar = 1;
    hint.barSide = g_Cfg.HintBarSide;
    hint.fullBoard = 1; hint.anim = 0;   /* 提示框行为不变: 整块底板+整行即时 */
    hint.ring = &g_HintRing;
    hint.schemeColor = g_Cfg.HintColorFollow;
    hint.tmoMul = (unsigned)g_Cfg.HintTimeoutMul;
    hint.slot = 1;
    hint.pickHits = g_Cfg.HintRadarShow;
    hint.expanded = s_hintExpand;
    /* 展开时冻住雷达事件历史 (免得看着看着行就过期没了, 点不中);
     * 操作提示不吃这个 ignoreTimeout, 照常按引擎超时走 (见 PickLines)。*/
    hint.ignoreTimeout = s_hintExpand;
    hint.face = g_HintFace;

    /* 触发条绿框 (测试可视化, 不带文本): 与战役文本框同 x / 同宽、1 行高
     * (顶部或底部, 由 CampBottom 决定), 先画绿底+绿边, 再画战役/提示文本压在上层。
     * ShowHotZone=0 时隐藏绿框, 但触发判定 (hoverCamp) 仍然有效。
     * 鼠标放上去时上面的 DrawBox 会展开历史文本。 */
    if (g_Cfg.CampEnable && g_Cfg.ShowHotZone)
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
        RectangleStruct tr;
        rc = DrawBox(surf, &hint, bitLineH, screenW, surfW, surfH, surfPitch);
        on = (rc == 1);
        if (on != s_lastHint) { s_lastHint = on; LogHintWhy(on); }
        if (rc < 0) return rc;
        if (rc == 1) drew = 1;
        /* 框空的时候把末行触发区画出来 (ShowHotZone 调试用): 免得"一条信息都
         * 不剩"时那块地方看着什么都没有, 用户不知道那儿能悬停翻历史。 */
        if (!on && g_Cfg.ShowHotZone)
        {
            HintTriggerGeom(bitLineH, screenW, surfH, &tr);
            GtDrawOutline(surf, tr.X, tr.Y, tr.Width, tr.Height, 0, 255, 0, 90);
            drew = 1;
        }
    }

    /* 显示态悬停范围可视化 (调试): 有框才描白框 */
    if (g_Cfg.ShowHotZone && g_CampValid)
        GtDrawOutline(surf, g_CampRect.X, g_CampRect.Y, g_CampRect.Width, g_CampRect.Height,
                      255, 255, 255, 80);

    return drew ? 1 : 0;
}

/* 滚轮接管: 命中战役框则翻历史并吞掉滚轮, 返回 1=已吞。
 * 操作与战斗提示只在**展开态**才接滚轮 (收起时只有 HintMaxLines 行, 本来
 * 就没得滚)。两框语义一致: 最新的一行在**下**底, 所以上滚=往新的看。 */
int MsgText_WheelIn(int mx, int my, int dirUp)
{
    static unsigned s_wheelN = 0;
    if (!g_Cfg.Enable) return 0;

    if (s_hintExpand && g_HintZonesOk && InRectXY(mx, my, &g_HintBox))
    {
        /* 与战役框同语义: 最新的一行在**下**底, 上滚=往新的看。 */
        if (dirUp)
        {
            g_HintSkip -= g_Cfg.ScrollStep;
            if (g_HintSkip < 0) g_HintSkip = 0;
        }
        else g_HintSkip += g_Cfg.ScrollStep;
        return 1;
    }
    if (!g_CampValid) return 0;
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

/* 是否接管 (供 MOExperience.cpp): 与 MsgText_DrawFrame 同一开关。
 * 战役文本框关闭时返回 0, 让引擎原版消息框照常绘制。 */
int MsgText_Enabled(void)
{
    return (g_Cfg.Enable && g_Cfg.CampEnable) ? 1 : 0;
}
