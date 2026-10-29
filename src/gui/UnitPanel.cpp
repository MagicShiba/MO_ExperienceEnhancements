/* UnitPanel.cpp - 左下角选中信息面板 (版面/点击语义/角标见 notes/选中信息面板.md)。
 *
 * 三条必须守住的:
 *  1. 图标装载 (PCX) 与 SHP 现渲都会动引擎面池 → 只在 UP_Poll 里备, 绘制帧只读缓存。
 *  2. 面板压在视口上, 引擎会把落点当地图点击 → 由 0x692361 的门吞掉; 门里只记坐标,
 *     真正的选中动作放到下一次 UP_Poll (那时已不在引擎的点击链路里)。
 *  3. 只读模拟状态 + 只改本机选中集, 联机安全。例外是 WASD 索敌换目标 (直接
 *     SetTarget, 不走事件), 它只在单机启用 —— 见 Up_KmDropStale。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "Favorite.h"
#include "FullMap.h"
#include "GdiText.h"
#include "IconSnap.h"
#include "OrderUtil.h"
#include "PauseView.h"
#include "PcxImage.h"
#include "SingleUnload.h"
#include "UnitPanel.h"
#include "VecText.h"

#define UP_SECTION "UnitPanel"

/* ---------------- 版式常量 ---------------- */
#define UP_CELL_W   60          /* cameo 原生尺寸, 不缩放 */
#define UP_CELL_H   48
#define UP_CELL_GAP 1           /* 相邻图标之间的空隙, 让边框/悬停框不互相贴死 */
/* 九宫格分割线 (源图 border_mental_n9_B.pcx 36x36 上的取值): 上 8 / 下 6 / 左右 7。
 * 目标内边距与分割线同值 → 四边都 1:1 贴, 框带不做重采样。 */
#define UP_SRC_T    8
#define UP_SRC_B    6
#define UP_SRC_L    7
#define UP_SRC_R    7
#define UP_PAD_L    UP_SRC_L
#define UP_PAD_R    UP_SRC_R
#define UP_PAD_T    UP_SRC_T
#define UP_PAD_B    UP_SRC_B
#define UP_NAME_H   12          /* cameo 底部烧死的单位名高度, 弹药格要让开这一条 */
#define UP_BTN_W    18
#define UP_BTN_H    18
#define UP_BTN_GAP  2
#define UP_BTN_FIXED 2          /* 折叠 / 操作模式: 恒定两个 */
#define UP_BTN_MAX  (UP_BTN_FIXED + FAV_SLOT_MAX)
#define UP_GAP      1           /* 按钮列与九宫格的间距 */
#define UP_COLS_MAX 3           /* 每页最多 3x3; 实际用几行几列由 s_Cols/s_Rows 决定 */
#define UP_ROWS_MAX 3
#define UP_CELLN    (UP_COLS_MAX * UP_ROWS_MAX)
#define UP_PAGES    3
#define UP_ENT_MAX  (UP_CELLN * UP_PAGES)

#define UP_ICON_W   UP_CELL_W    /* 图标缓存尺寸 = 格子尺寸, 恒不缩放 */
#define UP_ICON_H   UP_CELL_H
#define UP_LIST_CAP 512
#define UP_CAP_N    384

/* cameo 渲染的色键 (COLOR_PURPLE)。SHP 透明像素落成它, 贴图时跳过。 */
#define UP_KEY      0xF81Fu

#define UP_N9_MAX   64          /* 九宫格原图缓冲上限 */
#define UP_GDI_SLOT 12          /* GdiText 槽: 左下选中面板 */
#define UP_BADGE_SLOT 13        /* GdiText 槽: 数量角标 (资金/顶栏资源那套字体与字号) */

/* 按钮下标 (自下而上)。收藏槽 0..FAV_SLOT_MAX-1 对应 UP_BTN_FAV1 + 槽号。 */
#define UP_BTN_FOLD 0
#define UP_BTN_MODE 1
#define UP_BTN_FAV1 2

/* ---------------- 类型 ---------------- */

typedef struct {
    void* obj;
    void* type;
    int   count;
} UpEnt;

typedef struct {
    void* type;
    unsigned short* pix;
} UpCap;

/* ---------------- 配置 / 状态 ---------------- */

static int s_Init = 0;
static int s_Enable = 1;
static int s_Margin = 0;
static int s_FontPx = 11;
static int s_Collapsed = 0;
static int s_ActionLines = 0;    /* 1=强制开启引擎单位行动线 (原版点击移动线) */
static int s_Vis = 0;            /* 本帧该不该显示 */
static int s_LoggedGeom = 0;     /* 摆位只记一次日志 (没有截图时靠它核对几何) */
static wchar_t s_Face[FONT_FACE_MAX];
static char s_FaceCfg[64];

/* 版式 */
static int s_Px, s_Py, s_Pw, s_Ph;
static int s_Gx, s_Gy;
static int s_Bx, s_By[UP_BTN_MAX];
static int s_BtnN = UP_BTN_FIXED;  /* 本帧真实按钮数 = 2 + 露出的收藏槽数 */
static int s_BtnTop;             /* 按钮列顶端 (框比按钮列矮时, 命中区要往上扩) */
static int s_Cbx, s_Cby;         /* 折叠态那个按钮 */

/* 半成品: 待处理点击 (门里记, Poll 里做) */
static int s_Pend = 0, s_PendX = 0, s_PendY = 0;
static int s_PendRb = 0, s_PendRbX = 0, s_PendRbY = 0;   /* 右键: 只用于星星按钮切换箭头 */
static int s_GateLb = 0;         /* 门侧左键按下沿标记: ProcessClickCoords 一次按下会进多趟 */
static int s_GateRb = 0;         /* 同上, 右键 */
/* 载员多选 (shift + 点载员格): 只对单选版式有意义 —— 那一版式里才看得见载员格。
 * 存的是载员指针, 归属容器一换就整组清掉 (s_PickHost 记归属)。 */
#define UP_PICK_MAX SU_PAX_MAX   /* SingleUnload.h 导出: 两张表口径必须一致 */
static void* s_Pick[UP_PICK_MAX];
static int   s_PickN = 0;
static void* s_PickHost = NULL;
static int Up_PickHas(void* o);          /* 定义在输入段, 绘制段要用 */
/* 悬停 */
static int s_HoverCell = -1;
static int s_HoverBtn = -1;

/* 键盘移动 (UP_BTN_MODE): 每个单位记一个"上次下令的目标格"作锚点。
 * 连按/按住时从锚点往前推, 不按当前格反复下同一个点; 松开一段时间后锚点作废。
 * 数量按"少量单位"设计, 超出上限的忽略。 */
/* 时间量一律用帧: 引擎帧锁定, 一帧 = 一次逻辑步进, 帧率就是游戏速度。
 * 只有 UP_KM_COMBO_MS 用墙钟 —— 它量的是人的手速。 */
#define UP_KM_MAX        64
#define UP_KM_MIN_GAP    4      /* 同向连续令的最小间隔(帧) */
#define UP_KM_STUCK      30     /* 坐标一个 lepton 都没挪这么多帧 = 走不动, 重发一格 */
#define UP_KM_TIMEOUT    240    /* 坐标在动却始终到不了目标格 (绕路/被挤) 的兜底重发 */
#define UP_KM_ANCHOR_TTL 180    /* 松手后锚点保留(帧), 连按同向可累加 */
#define UP_KM_COMBO_MS   100    /* 方向键组合窗口(ms): W 与 D 前后脚按下也只出一格斜向 */
#define UP_KM_AIR_GAP    6      /* 战机续令间隔(帧) */
#define UP_KM_AIR_AHEAD  12     /* 战机续令目标 = 当前格前方 N 格; 悬在正前方恒定距离,
                                 * 快慢都追不上, 不甩尾 */
#define UP_KM_DROP_TTL   15     /* 目标飘出射程后的重估间隔(帧) */

typedef struct {
    void* unit;
    int   id;
    short tx, ty;       /* 上次下令的目标格 */
    short fx, fy;       /* 发那次令时单位所在格: 令后挪没挪窝, 区分"在走"和"卡死" */
    short mdx, mdy;     /* 那次令的方向: 和当前按住方向比, 判断是不是改向 */
    int   lastFrame;    /* 上次下令帧 */
    int   lastX, lastY; /* 上次采样到的原始坐标 (判"原地不动") */
    int   still;        /* 连续几次采样坐标没变 */
    int   lastDrop;     /* 上次换掉超程目标的帧 (限速用; -1 = 还没有过) */
    int   has;          /* 是否有过锚点 (帧号 0 也能用) */
    int   seen;         /* 本轮选中集里是否还有它 */
} UpKmAnchor;

static int s_KeyMove = 0;        /* 模式开关 */
static int s_KeyMoveSel = 0;     /* 本帧可动单位数; 钩子只在 >0 时吞键 */
static int s_KeyPrev[4] = {0, 0, 0, 0};  /* W/A/S/D 上一帧状态 */
static int s_KeyDirX = 0, s_KeyDirY = 0; /* 上一次真正下令的方向 */
static int s_KeyPendX = 0, s_KeyPendY = 0; /* 待确认方向 (组合键窗口) */
static unsigned s_KeyPendMs = 0;
static int s_KeyPendAnchor = 0;  /* 0=从当前格起, 1=从锚点累加 */
static int s_KeyPendActive = 0;
static int s_ActionKickFrame = -1;
static UpKmAnchor s_Km[UP_KM_MAX];
static int s_KmN = 0;
static int s_KmMp = 0;           /* 本局是否联机 (GameMode 3/4): 换目标那套不启用 */
static wchar_t s_KmMap[0x2D];    /* 上一局的地图名 ([Basic] Name, 读档同名时恢复开关) */
static int s_KmWant = 0;         /* 离开对局的那一刻 WASD 是开的 */

/* 选中快照 */
static UpEnt s_List[UP_LIST_CAP];
static int s_ListN = 0;
static unsigned s_ListMs = 0;      /* 上次重排格子的墙钟 */
static unsigned s_ListSig = 0;

/* 当前页的格子 (s_Page 变或模型变时重建)。只有前 s_Cols*s_Rows 个有效:
 * 单选 = 2 格, 4 架 = 2x2, 9 个以上才铺满 3x3 —— 不留空槽。 */
static UpEnt s_Cell[UP_CELLN];
static int s_Cols = 1;
static int s_Rows = 1;
static int s_Pages = 1;
static int s_Page = 0;
static int s_Trunc = 0;
/* 0 = 单选版式一页装完 (图标+信息+载员), 1 = 列表分页版式 (每页 9 格平铺),
 * 2 = 单选版式分页 (前两格每页固定, 第 3 格起是当前页的载员)。 */
static int s_Paged = 0;

/* 九宫格原图 */
static int s_Bw = 0, s_Bh = 0;
static unsigned short s_Border[UP_N9_MAX * UP_N9_MAX];
static unsigned short s_BorderKey = UP_KEY;
static int s_BorderOK = 0;
static unsigned short* s_Bg = NULL;
static int s_BgW = 0, s_BgH = 0;

/* cameo 缓存 */
static UpCap s_Cap[UP_CAP_N];
static int s_CapN = 0;

/* ---------------- 引擎入口 ---------------- */

typedef int (__attribute__((thiscall)) *Fn_WhatAmI)(void*);
typedef int (__attribute__((thiscall)) *Fn_Select)(void*);
typedef void (__fastcall *Fn_UnselectAll)(void);

/* ---------------- 小工具 ---------------- */

static int Up_Min(int a, int b) { return a < b ? a : b; }

static void Up_Fill(unsigned char* base, int pitch, int W, int H,
                    int x, int y, int w, int h,
                    unsigned r, unsigned g, unsigned b, int a)
{
    int j, i;
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (j = 0; j < h; j++)
    {
        unsigned short* row = (unsigned short*)(base + (y + j) * pitch) + x;
        for (i = 0; i < w; i++) row[i] = Blend16(row[i], r, g, b, (unsigned)a);
    }
}

/* 一行文本。align: 0=左 1=中 2=右 (以 boxX/boxW 为基准)。 */
static void Up_Text(unsigned char* base, int pitch, int W, int H,
                    int x, int y, const wchar_t* s, int n,
                    unsigned r, unsigned g, unsigned b,
                    int align, int boxX, int boxW)
{
    int w;
    if (!s || n <= 0) return;
    w = RenderLine(s, n);
    if (w <= 0) return;
    if (align == 1) x = boxX + (boxW - w) / 2;
    else if (align == 2) x = boxX + boxW - w;
    /* 末位是透明度百分比 (100 = 不透明), 覆盖率原样进混合; 传 255 会把 cov>=100
     * 的像素全饱和, 抗锯齿边缘被削平, 小字发胖发花。 */
    BlitCoverage(base, pitch, W, H, x, y, w + 2, g_G->adv, r, g, b, 100);
}

/* 覆盖率里墨迹的上下界 (行内相对 y, 不含行距空档)。
 * 行盒高度 g_G->adv 含 ascent+descent, 数字只占其中一段, 按行盒铺底会显得上下各空 4~5px。 */
static void Up_InkTB(int w, int* top, int* bot)
{
    unsigned char* p;
    int x, y, first = -1, last = -1;
    *top = 0;
    *bot = g_G->adv - 1;
    if (!g_G || !g_G->bits || w <= 0) return;
    for (y = 0; y < g_G->adv; y++)
    {
        p = g_G->bits + y * g_G->pitch;
        for (x = 0; x < w; x++)
        {
            if (p[x * 4 + 2])
            {
                if (first < 0) first = y;
                last = y;
                break;
            }
        }
    }
    if (first >= 0) { *top = first; *bot = last; }
}

/* 数字串 (无 CRT 的 UtoA) */
static int Up_Num(wchar_t* dst, int cap, int v)
{
    char b[16];
    int i = 0, n = 0;
    unsigned u;
    if (cap <= 1) return 0;
    if (v < 0) { dst[n++] = (wchar_t)'-'; v = -v; }
    u = (unsigned)v;
    UtoA(u, b);
    while (b[i] && n < cap - 1) dst[n++] = (wchar_t)(unsigned char)b[i++];
    dst[n] = 0;
    return n;
}

/* "123/200" */
static int Up_NumPair(wchar_t* dst, int cap, int a, int b)
{
    int n = Up_Num(dst, cap, a);
    if (n < cap - 1) dst[n++] = (wchar_t)'/';
    if (n < cap - 1)
    {
        wchar_t t[16];
        int m = Up_Num(t, 16, b), i;
        for (i = 0; i < m && n < cap - 1; i++) dst[n++] = t[i];
    }
    dst[n] = 0;
    return n;
}

/* "45%" / "MAX" */
static int Up_Percent(wchar_t* dst, int cap, int pc, int isMax)
{
    int n;
    if (isMax)
    {
        dst[0] = (wchar_t)'M'; dst[1] = (wchar_t)'A'; dst[2] = (wchar_t)'X';
        dst[3] = 0;
        return 3;
    }
    n = Up_Num(dst, cap - 1, pc);
    if (n < cap - 1) { dst[n++] = (wchar_t)'%'; dst[n] = 0; }
    return n;
}

/* 日志里的类型 ID (id = 类型对象 + OFF_TYPE_ID): 可读且非空时限长逐字符抄,
 * 否则填一个占位符。 */
static unsigned Up_AppendId(char* buf, unsigned cap, unsigned q, const char* id,
                            int maxn, char fill)
{
    if (MemReadableBytes(id, 0x18) >= 0x18 && id[0])
    {
        int m;
        for (m = 0; m < maxn && id[m] && q + 1 < cap; m++) buf[q++] = id[m];
        buf[q] = 0;
    }
    else q = AppendCh(buf, q, fill, cap);
    return q;
}

/* ---------------- 配置 ---------------- */

void UP_EnsureInit(void)
{
    static unsigned s_RetryMs = 0;
    char buf[256];
    unsigned q = 0;
    if (s_Init) return;
    /* prepare 钩在引擎初始化早期就会跑: 路径/日志还没建好时读 ini 会把默认值钉死。
     * 没就绪就退回, 下次再来 (100ms 一次, 不白跑 BuildPaths)。 */
    if (!g_IniPath[0] || !g_Log)
    {
        unsigned now = GetTickCount();
        if (s_RetryMs && (unsigned)(now - s_RetryMs) < 100u) return;
        s_RetryMs = now;
        BuildPaths();
        GtEnsureLog();
        if (!g_IniPath[0] || !g_Log) return;
    }
    s_Init = 1;
    s_Enable = GetPrivateProfileIntA(UP_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    s_Margin = GetPrivateProfileIntA(UP_SECTION, "Margin", 0, g_IniPath);
    s_FontPx = GetPrivateProfileIntA(UP_SECTION, "FontPx", 11, g_IniPath);
    s_Collapsed = GetPrivateProfileIntA(UP_SECTION, "Collapsed", 0, g_IniPath) ? 1 : 0;
    s_ActionLines = GetPrivateProfileIntA(UP_SECTION, "ActionLines", 0, g_IniPath) ? 1 : 0;
    if (s_Margin < 0) s_Margin = 0;
    if (s_Margin > 200) s_Margin = 200;
    if (s_FontPx < 8) s_FontPx = 8;
    if (s_FontPx > 16) s_FontPx = 16;
    OU_ReadString(UP_SECTION, "FontFace", "msyh.ttc", s_FaceCfg, (int)sizeof(s_FaceCfg));
    ResolveFontFace(s_FaceCfg, "up", s_Face, L"Microsoft YaHei");
    if (!g_Log) return;
    q = AppendCStr(buf, q, "up: enable=", sizeof(buf));
    q += UtoA((unsigned)s_Enable, buf + q);
    q = AppendCStr(buf, q, " margin=", sizeof(buf));
    q += UtoA((unsigned)s_Margin, buf + q);
    q = AppendCStr(buf, q, " px=", sizeof(buf));
    q += UtoA((unsigned)s_FontPx, buf + q);
    q = AppendCStr(buf, q, " fold=", sizeof(buf));
    q += UtoA((unsigned)s_Collapsed, buf + q);
    q = AppendCStr(buf, q, " face=", sizeof(buf));
    AppendCStr(buf, q, s_FaceCfg, sizeof(buf));
    LogLine(buf);
}

/* ---------------- 版式 ---------------- */

static void Up_Layout(void)
{
    /* 基准 = 满视图区 A (1x 时 == 引擎视口)。缩放档把 ViewBounds 缓存
     * (0xB0CE28) 改成左上锚定的 R = A*scale, 拿它当界线的 UI 会跟着缩放跑,
     * 与 MsgText/CCToolTip 同一批问题 —— 锚定一律用 A。 */
    RectangleStruct va;
    int vx, vy, vh;
    int i, by;
    FM_GetViewRects(&va, 0);
    vx = va.X; vy = va.Y; vh = va.Height;
    /* 九宫格只包格子; 按钮列在框外左侧, 整块贴视口左下角。
     * 上/下内边距不对称 (8/6): 框带上方留宽一点压住标题感, 下方留给页码块。 */
    s_Pw = UP_PAD_L + UP_PAD_R + s_Cols * UP_CELL_W + (s_Cols - 1) * UP_CELL_GAP;
    s_Ph = UP_PAD_T + UP_PAD_B + s_Rows * UP_CELL_H + (s_Rows - 1) * UP_CELL_GAP;
    s_Px = vx + s_Margin + UP_BTN_W + UP_GAP;
    s_Py = vy + vh - s_Margin - s_Ph;
    if (s_Py < vy) s_Py = vy;
    s_Bx = vx + s_Margin;
    s_Gx = s_Px + UP_PAD_L;
    s_Gy = s_Py + UP_PAD_T;
    /* 按钮列自下而上排; 底边钉在视口底 (== 框底), 不随面板格数浮动。
     * 收藏按钮按用过的槽逐个露出, 新按钮出现在上一格上方; [Favorite] 关掉时
     * Fav_RevealCount() 回 0, 这里就只剩两个固定按钮。 */
    s_BtnN = UP_BTN_FIXED + Fav_RevealCount();
    if (s_BtnN > UP_BTN_MAX) s_BtnN = UP_BTN_MAX;
    if (s_BtnN < UP_BTN_FIXED) s_BtnN = UP_BTN_FIXED;
    by = vy + vh - s_Margin;
    for (i = 0; i < s_BtnN; i++)
    {
        s_By[i] = by - UP_BTN_H;
        by = s_By[i] - UP_BTN_GAP;
    }
    s_BtnTop = by + UP_BTN_GAP;
    /* 折叠态: 只剩折叠键, 贴视口左下角 */
    s_Cbx = vx + s_Margin;
    s_Cby = vy + vh - s_Margin - UP_BTN_H;
    if (s_Cby < vy) s_Cby = vy;
}

static int Up_PtIn(int x, int y, int rx, int ry, int rw, int rh)
{
    return (x >= rx && x < rx + rw && y >= ry && y < ry + rh) ? 1 : 0;
}

static int Up_HitPanel(int x, int y)
{
    int top;
    if (s_Collapsed) return Up_PtIn(x, y, s_Cbx, s_Cby, UP_BTN_W, UP_BTN_H);
    /* 按钮列可能比框还高 (框只有一两格时), 命中区取两者并集 */
    top = s_Py;
    if (s_BtnTop < top) top = s_BtnTop;
    /* 没选中东西时框不显示, 只有按钮列算面板 */
    if (!s_ListN)
        return Up_PtIn(x, y, s_Bx, s_BtnTop, UP_BTN_W, s_Py + s_Ph - s_BtnTop);
    /* 按钮列 + 九宫格框 = 一整块 */
    return Up_PtIn(x, y, s_Bx, top, (s_Px + s_Pw) - s_Bx, (s_Py + s_Ph) - top);
}

/* 第 i 格左上角 (含 1px 间隙的行列步进) */
static void Up_CellPos(int i, int* px, int* py)
{
    *px = s_Gx + (i % s_Cols) * (UP_CELL_W + UP_CELL_GAP);
    *py = s_Gy + (i / s_Cols) * (UP_CELL_H + UP_CELL_GAP);
}

/* 格子命中: 0..s_Cols*s_Rows-1, -1 = 不在格子上 (落到间隙里也算不在) */
static int Up_HitCell(int x, int y)
{
    int cw = UP_CELL_W + UP_CELL_GAP;
    int ch = UP_CELL_H + UP_CELL_GAP;
    int gw = s_Cols * UP_CELL_W + (s_Cols - 1) * UP_CELL_GAP;
    int gh = s_Rows * UP_CELL_H + (s_Rows - 1) * UP_CELL_GAP;
    int cx, cy;
    if (s_Collapsed || !s_ListN) return -1;
    if (!Up_PtIn(x, y, s_Gx, s_Gy, gw, gh)) return -1;
    cx = (x - s_Gx) / cw;
    cy = (y - s_Gy) / ch;
    if (cx < 0 || cx >= s_Cols || cy < 0 || cy >= s_Rows) return -1;
    /* 落在格子的右侧/下侧空隙里: 不算命中 */
    if ((x - s_Gx) % cw >= UP_CELL_W) return -1;
    if ((y - s_Gy) % ch >= UP_CELL_H) return -1;
    return cy * s_Cols + cx;
}

/* 按钮命中: 0..s_BtnN-1, -1 = 不在按钮上 (折叠态用 UP_BTN_FOLD) */
static int Up_HitBtn(int x, int y)
{
    int i;
    if (s_Collapsed) return Up_PtIn(x, y, s_Cbx, s_Cby, UP_BTN_W, UP_BTN_H) ? UP_BTN_FOLD : -1;
    for (i = 0; i < s_BtnN; i++)
        if (Up_PtIn(x, y, s_Bx, s_By[i], UP_BTN_W, UP_BTN_H)) return i;
    return -1;
}

/* ---------------- 键盘移动 ---------------- */

static void Up_KmClear(void)
{
    s_KmN = 0;
    s_KeyPendActive = 0;
    s_KeyPendAnchor = 0;
}

/* 键盘移动关停时的统一收尾 (模式指示熄掉 + 锚点表作废)。 */
static void Up_KmOff(void)
{
    s_KeyMoveSel = 0;
    Up_KmClear();
}

static void Up_KmReset(void)
{
    Up_KmClear();
    s_KeyPrev[0] = OU_KeyDown('W') ? 1 : 0;
    s_KeyPrev[1] = OU_KeyDown('A') ? 1 : 0;
    s_KeyPrev[2] = OU_KeyDown('S') ? 1 : 0;
    s_KeyPrev[3] = OU_KeyDown('D') ? 1 : 0;
    s_KeyDirX = 0;
    s_KeyDirY = 0;
    s_ActionKickFrame = -1;
}

/* 当前地图名 (ScenarioClass::Name)。FileName 读档后是存档名, 比不出图;
 * Name 没写的地图退到 UIName。空名/读不到/脏数据都回 0, 调用方放弃比对。 */
static int Up_KmMapName(wchar_t* out)
{
    char* sc = *(char**)ADDR_SCENARIO;
    int i;

    out[0] = 0;
    if (!sc || !IsPlausiblePtr(sc)) return 0;
    if (MemReadableBytes(sc + OFF_SCEN_NAME, 0x2D * 2) >= 0x2D * 2)
    {
        for (i = 0; i < 0x2C; i++)
        {
            wchar_t c = *(wchar_t*)(sc + OFF_SCEN_NAME + i * 2);
            if (!c) break;
            if (c < 0x20 || c == 0xFFFF) { i = 0; break; }   /* 脏数据 */
            out[i] = c;
        }
        out[i] = 0;
        if (i > 0) return 1;
    }
    if (MemReadableBytes(sc + OFF_SCEN_UINAME, 0x20) >= 0x20)
    {
        for (i = 0; i < 0x1F; i++)
        {
            char c = sc[OFF_SCEN_UINAME + i];
            if (!c) break;
            if ((unsigned char)c < 0x20) return 0;
            out[i] = (wchar_t)(unsigned char)c;
        }
        out[i] = 0;
        return i > 0;
    }
    return 0;
}

static int Up_KmWStrEq(const wchar_t* a, const wchar_t* b)
{
    int i;
    for (i = 0; a[i] || b[i]; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

/* 刚进一局: 记下地图名, 与上一局同名就恢复 WASD 开关 (读档接着用, 换图保持
 * 关闭)。锚点不恢复 —— 它们按单位指针+id 索引, 读档后旧对象已经没了。 */
static void Up_KmMatchEnter(void)
{
    wchar_t buf[0x2D];
    int i;

    if (Up_KmMapName(buf))
    {
        /* 折叠态没有模式指示, 恢复等于"键被吞却看不到提示", 所以不恢复 */
        if (s_KmWant && !s_Collapsed && s_KmMap[0] && Up_KmWStrEq(buf, s_KmMap))
        {
            s_KeyMove = 1;
            Up_KmReset();
        }
        for (i = 0; i < 0x2C && buf[i]; i++) s_KmMap[i] = buf[i];
        s_KmMap[i] = 0;
    }
    else s_KmMap[0] = 0;
    s_KmWant = 0;
}

/* 进/出对局的判定: ScenarioStarted 的 0->1 边沿, 或帧号倒退 (每局开始把
 * 0xA8ED84 写 0)。读档时主循环停着, 0->1 那一拍轮不到 Poll, 只能靠帧号倒退认。
 * 离开对局时记下当时 WASD 的开关状态, 供下次进局比对。 */
static void Up_KmMatchEdge(int started, int frame)
{
    static int prev = 0;
    static int lastFrame = -1;

    if (!started)
    {
        if (prev) s_KmWant = s_KeyMove ? 1 : 0;   /* 只在 1 -> 0 那一拍照一次 */
        prev = 0;
        return;
    }
    if (!prev || frame < lastFrame) Up_KmMatchEnter();
    prev = 1;
    lastFrame = frame;
}

/* 新锚点: 清零后 lastDrop 记 -1 (还没有放弃过任何目标)。 */
static void Up_KmInitEntry(UpKmAnchor* e, void* unit, int id)
{
    ZeroMemory(e, sizeof(*e));
    e->unit = unit;
    e->id = id;
    e->lastDrop = -1;
}

/* 按 unit 指针 + id 找锚点; add=1 时没有就建。指针被复用 (id 变了) 时重置。 */
static UpKmAnchor* Up_KmFind(void* unit, int id, int add)
{
    int i;
    for (i = 0; i < s_KmN; i++)
    {
        if (s_Km[i].unit != unit) continue;
        if (s_Km[i].id != id)
        {
            if (!add) return NULL;
            Up_KmInitEntry(&s_Km[i], unit, id);
        }
        return &s_Km[i];
    }
    if (!add || s_KmN >= UP_KM_MAX) return NULL;
    Up_KmInitEntry(&s_Km[s_KmN], unit, id);
    return &s_Km[s_KmN++];
}

static void Up_KmPrune(void)
{
    int i, n = 0;
    for (i = 0; i < s_KmN; i++)
        if (s_Km[i].seen) s_Km[n++] = s_Km[i];
    s_KmN = n;
}

/* 8 方向: 屏幕方向 (sx,sy) 折算成地图格偏移。斜向也是一格 (0,-1)/(-1,0) 等,
 * 不是两个正向相加的两格。键态由调用方传入 (同帧只采一次), 有方向回 1。 */
static int Up_KeyDir(int w, int a, int s, int d, int* dx, int* dy)
{
    int sx = d - a;
    int sy = s - w;
    if (!sx && !sy) { *dx = 0; *dy = 0; return 0; }
    if (sx && sy) { *dx = (sx + sy) / 2; *dy = (sy - sx) / 2; }
    else { *dx = sx + sy; *dy = sy - sx; }
    return 1;
}

static int Up_ChatTyping(void)
{
    return *(volatile unsigned char*)(size_t)(ADDR_MSGLIST + OFF_MSGLIST_EDIT) ? 1 : 0;
}

/* 让引擎的行动线从头开始画 (只写本地渲染计时)。 */
typedef void (__stdcall *Fn_StartActionLines)(void);

static void Up_ActionLineKick(int frame)
{
    if (!s_ActionLines || s_ActionKickFrame == frame) return;
    s_ActionKickFrame = frame;
    ((Fn_StartActionLines)(void*)FN_START_ACTIONLINES)();
}

#define UP_KM_LEP_CELL 256      /* 1 格 = 256 leptons; 判定留一格余量给目标体积 */

typedef void* (__attribute__((thiscall)) *Fn_UpGetWeapon)(void* self, int idx);
typedef void* (__attribute__((thiscall)) *Fn_UpThreat)(void* self, int threat,
                                                       CoordStruct* from, int onlyEnemy);
typedef void  (__attribute__((thiscall)) *Fn_UpSetTarget)(void* self, void* target);

/* 对象的虚表指针; 不可信回 NULL。 */
static void** Up_KmVT(void* o)
{
    void** vt;
    if (!o || !IsPlausiblePtr(o)) return NULL;
    vt = *(void***)o;
    return (vt && IsPlausiblePtr(vt)) ? vt : NULL;
}

/* 单位身上最远的武器射程 (leptons); 0 = 没有武器。 */
static int Up_KmMaxRange(void* unit)
{
    void** vt;
    Fn_UpGetWeapon gw;
    int i, best = 0;

    vt = Up_KmVT(unit);
    if (!vt) return 0;
    gw = (Fn_UpGetWeapon)vt[OFF_VT_GETWEAPON / 4];
    if (!gw || !IsPlausiblePtr((const void*)gw)) return 0;
    for (i = 0; i < 3; i++)   /* 0=主 1=副 2=精英/部署 */
    {
        void* ws = gw(unit, i);
        void* wt;
        int r;
        if (!ws || !IsPlausiblePtr(ws)) continue;
        wt = *(void**)ws;
        if (!wt || !IsPlausiblePtr(wt)) continue;
        r = *(int*)((char*)wt + OFF_WT_RANGE);
        if (r > best) best = r;
    }
    return best;
}

/* 单位到目标的平面距离平方; 取不到回 -1。高度差不参与 —— 山顶/桥上的目标
 * 中心距本来就会偏大。只比大小, 不开方。 */
static long long Up_KmDistSq(void* unit, void* tar)
{
    CoordStruct a, b;
    long long dx, dy;

    if (!OU_GetCoords(unit, &a) || !OU_GetCoords(tar, &b)) return -1;
    dx = (long long)a.X - (long long)b.X;
    dy = (long long)a.Y - (long long)b.Y;
    return dx * dx + dy * dy;
}

/* 目标此刻是否值得保留/回填: 活着、可作战类型、武器射程够得着。
 * 射程自己算。引擎的 IsCloseEnoughToAttack (0x6F7780) 在武器类型取不到时走
 * "武器为空"分支, 返回值不反映真实射程, 而"超程就弃"全靠这一个判据。 */
static int Up_KmKeepable(void* unit, void* tar)
{
    int what, range;
    long long d2, lim;

    if (!tar || !IsPlausiblePtr(tar)) return 0;
    if (!*(unsigned char*)((char*)tar + OFF_ISALIVE)) return 0;
    what = OU_WhatAmI(tar);
    if (what != ABS_UNIT && what != ABS_INFANTRY &&
        what != ABS_AIRCRAFT && what != ABS_BUILDING) return 0;
    range = Up_KmMaxRange(unit);
    if (range <= 0) return 0;
    d2 = Up_KmDistSq(unit, tar);
    if (d2 < 0) return 0;
    lim = (long long)range + UP_KM_LEP_CELL;
    return (d2 <= lim * lim) ? 1 : 0;
}

#define THREAT_RANGE 1  /* Yrpp ThreatType::Range: 只在本单位射程内选 */

/* 挑一个"现在就能开火"的目标 (只读): 借引擎的最大威胁扫描在射程内选, 再用
 * Up_KmKeepable 复核。必须主动给目标 —— 只传空让引擎重选, 它会立刻把够不着的
 * 远处目标选回来, 它仍是最"近"的敌人。 */
static void* Up_KmBestTarget(void* unit)
{
    void** vt;
    Fn_UpThreat f;
    void* t;
    CoordStruct c;

    vt = Up_KmVT(unit);
    if (!vt) return NULL;
    f = (Fn_UpThreat)vt[OFF_VT_GREATEST_THREAT / 4];
    if (!f) return NULL;
    if (!OU_GetCoords(unit, &c)) return NULL;
    t = f(unit, THREAT_RANGE, &c, 0);
    if (!t || !IsPlausiblePtr(t)) return NULL;
    if (*(void**)((char*)t + OFF_OWNER) == *(void**)((char*)unit + OFF_OWNER)) return NULL;
    return Up_KmKeepable(unit, t) ? t : NULL;
}

/* 一条标准移动令: 与手点右键移动同形, 不调 ClickedMission, 不播语音。
 * tgt 槽 = 0 表示清掉攻击目标 (原版事件执行会 SetTarget(事件Target槽))。 */
static void Up_KmFireMove(int house, void* unit, int sid, int ur,
                          int tx, int ty, int tid, int tr, int frame)
{
    unsigned char dr = 0;
    int dst = OU_CellTgt(tx, ty, &dr);
    OU_FireMega(house, sid, ur, MISSION_MOVE,
                tid, tr, dst, dr, 0, 0, 0, unit, sid, 1);
    Up_ActionLineKick(frame);
}

/* 给一个单位下令。useAnchor=1 (同向连按/按住) 从锚点往前推, 便于累加;
 * useAnchor=0 (换方向) 从当前格起 —— 一次按键 = 当前格旁一格。
 * steps = 一次推进几格, 只有战机按住推进才 >1。同时记下所在格与方向, 供推进
 * 循环区分"在走"和"卡死"。
 *
 * 目标槽: 移动令会把事件 Target 槽原样 SetTarget, 所以只给"此刻真能开火"的
 * 目标 —— 够得着就原样回填 (保持锁定, 边走边打), 够不着就换射程内目标, 一个
 * 都没有才传空。
 * 联机 (s_KmMp) 留空: 移动令与手点右键同形, 索敌交回引擎。 */
static void Up_KmIssue(int house, void* unit, int uid, int dx, int dy, int frame,
                       int useAnchor, int steps, int keepTgt)
{
    CoordStruct c;
    UpKmAnchor* a;
    unsigned char ur = 0, tr = 0;
    int cx, cy, tx, ty, sid, tid = 0;
    void* tar, *alt;

    if (!OU_GetCoords(unit, &c)) return;
    cx = c.X / 256;
    cy = c.Y / 256;
    a = Up_KmFind(unit, uid, 1);
    if (useAnchor && a && a->has &&
        (unsigned)(frame - a->lastFrame) <= UP_KM_ANCHOR_TTL)
    {
        tx = a->tx + dx * steps;
        ty = a->ty + dy * steps;
    }
    else
    {
        tx = cx + dx * steps;
        ty = cy + dy * steps;
    }
    if (!OU_CellValid(tx, ty)) return;
    sid = OU_ObjTgt(unit, &ur);
    if (sid < 0) return;
    tar = (keepTgt && !s_KmMp) ? *(void**)((char*)unit + OFF_TECH_TARGET) : NULL;
    if (Up_KmKeepable(unit, tar))
        alt = tar;
    else if (tar)
        /* 够不着才去找替代 (空目标不扫, 交给引擎自己按原版移动攻击选) */
        alt = Up_KmBestTarget(unit);
    else
        alt = NULL;
    if (alt)
    {
        tid = OU_ObjTgt(alt, &tr);
        if (tid < 0) { tid = 0; tr = 0; }
    }
    Up_KmFireMove(house, unit, sid, ur, tx, ty, tid, tr, frame);
    if (a)
    {
        a->tx = (short)tx;
        a->ty = (short)ty;
        a->fx = (short)cx;
        a->fy = (short)cy;
        a->mdx = (short)dx;
        a->mdy = (short)dy;
        a->lastFrame = frame;
        a->lastX = c.X;
        a->lastY = c.Y;
        a->still = 0;
        a->has = 1;
    }
}

/* 手上的目标打不到了就换掉, 只调 SetTarget, 不碰 Mission/Destination/Path
 * (重发移动令会反复重置寻路, 单位迈不开步)。
 * 不记"这个目标已弃过": 引擎会把它选回来, 记死就再也清不掉。
 * 只在按住方向键时管; 松手后目标状态交回原版 (玩家可能刚点了远处敌人要追)。
 * 联机跳过: SetTarget 只改本机模拟, 不走事件, 两边会分叉。 */
static void Up_KmDropStale(void* unit, int uid, int frame)
{
    UpKmAnchor* a = Up_KmFind(unit, uid, 0);   /* 锚点按 指针+id 索引, id 不能省 */
    void** vt;
    Fn_UpSetTarget f;
    void* tar, *alt;

    if (s_KmMp) return;
    if (!a || !a->has) return;
    if ((unsigned)(frame - a->lastFrame) > UP_KM_ANCHOR_TTL) return;
    if ((unsigned)(frame - a->lastDrop) < UP_KM_DROP_TTL) return;
    tar = *(void**)((char*)unit + OFF_TECH_TARGET);
    if (!tar || !IsPlausiblePtr(tar)) return;
    if (Up_KmKeepable(unit, tar)) return;
    alt = Up_KmBestTarget(unit);
    if (alt == tar) return;
    vt = Up_KmVT(unit);
    if (!vt) return;
    f = (Fn_UpSetTarget)vt[OFF_VT_SETTARGET / 4];
    if (!f) return;
    a->lastDrop = frame;
    f(unit, alt);
}

/* UP_Poll 每循环调一次。键态每帧都采; 只有模式开 + 有可动单位时才真的下令。 */
static void Up_KeyMovePoll(void)
{
    void* cur;
    void** items;
    void* eu[UP_KM_MAX];
    int   ei[UP_KM_MAX];
    int   ew[UP_KM_MAX];
    int n, i, en = 0, selN = 0, house = 0, frame;
    int w, a, s, d, edge, removed, dx = 0, dy = 0, dir, changed;
    unsigned now;

    s_KmMp = OU_IsMP();
    if (!s_Vis) { s_KeyMove = 0; Up_KmOff(); return; }
    if (!s_KeyMove) { Up_KmOff(); return; }

    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) { Up_KmOff(); return; }
    house = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    frame = *(int*)ADDR_FRAME;
    n = OU_VecItems(ADDR_CUR_OBJECTS, &items);
    if (!items || n <= 0) { Up_KmOff(); return; }

    for (i = 0; i < s_KmN; i++) s_Km[i].seen = 0;

    /* 可动 = 自己的、活着的、不在运输/工厂里的 Unit/Infantry/Aircraft。
     * 建筑、敌方、载员一律不碰 (与手点选中的可下令集合一致)。 */
    for (i = 0; i < n; i++)
    {
        void* u = items[i];
        int uid, what;
        if (!u || !IsPlausiblePtr(u)) continue;
        if (*(void**)((char*)u + OFF_OWNER) != cur) continue;
        if (!*(unsigned char*)((char*)u + OFF_ISALIVE)) continue;
        if (*(unsigned char*)((char*)u + OFF_INLIMBO)) continue;
        what = OU_WhatAmI(u);
        if (what != ABS_UNIT && what != ABS_INFANTRY && what != ABS_AIRCRAFT) continue;
        uid = OU_ObjTgt(u, NULL);
        if (uid < 0) continue;
        selN++;
        if (en < UP_KM_MAX)
        {
            UpKmAnchor* km;
            eu[en] = u;
            ei[en] = uid;
            ew[en] = what;
            en++;
            km = Up_KmFind(u, uid, 1);
            if (km) km->seen = 1;
        }
    }
    s_KeyMoveSel = selN;
    if (!selN) { Up_KmClear(); return; }

    /* 键态每帧采一次, 门控期间也更新: 免得松手/关聊天后补触发。 */
    w = OU_KeyDown('W') ? 1 : 0;
    a = OU_KeyDown('A') ? 1 : 0;
    s = OU_KeyDown('S') ? 1 : 0;
    d = OU_KeyDown('D') ? 1 : 0;
    edge = (w && !s_KeyPrev[0]) || (a && !s_KeyPrev[1]) ||
           (s && !s_KeyPrev[2]) || (d && !s_KeyPrev[3]);
    removed = (!w && s_KeyPrev[0]) || (!a && s_KeyPrev[1]) ||
              (!s && s_KeyPrev[2]) || (!d && s_KeyPrev[3]);
    s_KeyPrev[0] = w; s_KeyPrev[1] = a; s_KeyPrev[2] = s; s_KeyPrev[3] = d;

    if (!GtGameFocused() || Up_ChatTyping() ||
        *(volatile int*)(size_t)ADDR_SPECIAL_DIALOG || PV_Paused() ||
        OU_KeyDown('Z') || OU_KeyDown(VK_CONTROL) ||
        OU_KeyDown(VK_SHIFT) || OU_KeyDown(VK_MENU))
    {
        s_KeyPendActive = 0;
        s_KeyPendAnchor = 0;
        Up_KmPrune();
        return;
    }

    dir = Up_KeyDir(w, a, s, d, &dx, &dy);
    if (!dir)
    {
        /* 松手: 组合窗口里还没发出的那一下必须补发, 否则快速点按会丢输入。 */
        if (s_KeyPendActive)
        {
            for (i = 0; i < en; i++)
                Up_KmIssue(house, eu[i], ei[i],
                           s_KeyPendX, s_KeyPendY, frame, s_KeyPendAnchor, 1, 1);
            s_KeyDirX = s_KeyPendX;
            s_KeyDirY = s_KeyPendY;
            s_KeyPendActive = 0;
            s_KeyPendAnchor = 0;
        }
        Up_KmPrune();
        return;
    }

    now = GetTickCount();
    changed = (dx != s_KeyDirX || dy != s_KeyDirY);

    /* 按住期间目标飘出射程就换掉; 只换目标, 与下面的推进下令互不干扰。 */
    for (i = 0; i < en; i++)
        Up_KmDropStale(eu[i], ei[i], frame);

    /* 组合键窗口: 先记下方向, 窗口过了再出令 —— W 与 D 前后脚到达时不会被
     * 拆成"先走一格 W 再改斜向"。
     * 松键不改待确认方向: 斜向点按常常先松一个键, 一松就降级会丢掉斜向。 */
    if (s_KeyPendActive)
    {
        if (edge && (dx != s_KeyPendX || dy != s_KeyPendY))
        {
            s_KeyPendX = dx;
            s_KeyPendY = dy;
            s_KeyPendAnchor = 0;
            s_KeyPendMs = now;
        }
        else if (edge)
        {
            s_KeyPendAnchor = 1;
            s_KeyPendMs = now;
        }
        /* removed 且方向变了: 保持原待确认方向, 等窗口到期/全松时结算 */
    }
    else if (edge || (changed && !removed))
    {
        s_KeyPendActive = 1;
        s_KeyPendX = dx;
        s_KeyPendY = dy;
        s_KeyPendAnchor = changed ? 0 : 1;
        s_KeyPendMs = now;
    }
    else if (changed)
    {
        /* 纯松开导致的方向变化 (如 W+D 松 D 变 W): 不另发一格,
         * 改成跟随到达旧目标后的按住推进。 */
        s_KeyDirX = dx;
        s_KeyDirY = dy;
    }

    if (s_KeyPendActive)
    {
        if ((unsigned)(now - s_KeyPendMs) >= UP_KM_COMBO_MS)
        {
            /* 按当前真实键态结算, 不按 pend 锁的方向 (中途松键会让 dx 永远
             * 不等于 pend)。方向相对 pend 变了就按点按处理, 从当前格起。 */
            int ax = s_KeyPendAnchor;
            if (dx != s_KeyPendX || dy != s_KeyPendY) ax = 0;
            for (i = 0; i < en; i++)
                Up_KmIssue(house, eu[i], ei[i], dx, dy, frame, ax, 1, 1);
            s_KeyDirX = dx;
            s_KeyDirY = dy;
            s_KeyPendActive = 0;
            s_KeyPendAnchor = 0;
        }
        Up_KmPrune();
        return;
    }

    /* 方向没变: 持续推进下一格。
     * - 挪过窝 = 还在走 (绕路/慢), 等它到, 保住"走完这一格再接新方向"的手感;
     * - 坐标完全不动 UP_KM_STUCK 帧 = 令被吃掉或目标格不可达, 立刻重发一格;
     * - 没挪过窝且方向已改 = 旧目标不可达, 立即从当前格改向。
     * 战机不等到达: 按固定间隔把目标往前叠, 消除"到点减速-再加速"的顿挫。 */
    for (i = 0; i < en; i++)
    {
        UpKmAnchor* km = Up_KmFind(eu[i], ei[i], 0);
        if (km && km->has)
        {
            CoordStruct c;
            if (OU_GetCoords(eu[i], &c))
            {
                int cx = c.X / 256, cy = c.Y / 256;
                int gap = frame - km->lastFrame;
                if (ew[i] == ABS_AIRCRAFT)
                {
                    /* 从当前格起算, 不从旧锚点累加: 锚点累加会让目标前移速度
                     * 固定, 快战机抖、慢战机目标逃逸。 */
                    if (gap >= UP_KM_AIR_GAP)
                        Up_KmIssue(house, eu[i], ei[i], dx, dy, frame,
                                   0, UP_KM_AIR_AHEAD, 1);
                }
                else
                {
                    int moved = (cx != km->fx || cy != km->fy);
                    int turned = (km->mdx != dx || km->mdy != dy);
                    int go = 0, anchor = 1, keepTgt = 1;
                    /* 每次 Poll 采样一次坐标: 一个 lepton 都没挪就累加, 挪了清零 */
                    if (c.X == km->lastX && c.Y == km->lastY)
                    {
                        if (km->still < 100000) km->still++;
                    }
                    else
                    {
                        km->lastX = c.X;
                        km->lastY = c.Y;
                        km->still = 0;
                    }
                    if (cx == km->tx && cy == km->ty && gap >= UP_KM_MIN_GAP)
                    {
                        /* 到了上一个目标格再发下一格, 频率跟着单位速度走 */
                        go = 1;
                    }
                    else if (km->still >= UP_KM_STUCK)
                    {
                        /* 一动不动: 从当前格重发一格, 且不带目标 —— 挂着射程内
                         * 的目标重发, 单位会站在原地打不挪窝。 */
                        go = 1;
                        anchor = 0;
                        keepTgt = 0;
                    }
                    else if (gap >= UP_KM_TIMEOUT)
                        go = 1;
                    else if (turned && !moved && gap >= UP_KM_MIN_GAP)
                    {
                        go = 1;
                        anchor = 0;
                    }
                    if (go)
                        Up_KmIssue(house, eu[i], ei[i], dx, dy, frame, anchor, 1,
                                   keepTgt);
                }
            }
        }
    }
    Up_KmPrune();
}

/* ---------------- 选中集快照 ---------------- */

static int Up_WhatAmI(void* o)
{
    void** vt;
    Fn_WhatAmI f;
    if (!o || !IsPlausiblePtr(o)) return -1;
    vt = *(void***)o;
    if (!vt || !IsPlausiblePtr(vt)) return -1;
    f = (Fn_WhatAmI)vt[OFF_VT_WHATAMI / 4];
    if (!f) return -1;
    return f(o);
}

static void* Up_TypeOf(void* o, int am)
{
    unsigned off;
    void* t;
    if (am == ABS_UNIT) off = OFF_UNIT_TYPE;
    else if (am == ABS_AIRCRAFT) off = OFF_AIRCRAFT_TYPE;
    else if (am == ABS_INFANTRY) off = OFF_INFANTRY_TYPE;
    else if (am == ABS_BUILDING) off = OFF_BLD_TYPE;
    else return NULL;
    t = *(void**)((char*)o + off);
    if (!t || !IsPlausiblePtr(t)) return NULL;
    return t;
}

static int Up_Hp(void* o) { return *(int*)((char*)o + OFF_HEALTH); }
static int Up_HpMax(void* t) { return *(int*)((char*)t + OFF_TYPE_STRENGTH); }
static int Up_Ammo(void* o) { return *(int*)((char*)o + OFF_TECH_AMMO); }
static int Up_AmmoMax(void* t) { return *(int*)((char*)t + OFF_TT_AMMO); }
/* rules 的 `PipWrap=`: 弹药格一排几个 (0/缺省按 6) */
static int Up_PipWrap(void* t) { return *(int*)((char*)t + OFF_TT_PIPWRAP); }
static int Up_Level(void* o)
{
    float v = *(float*)((char*)o + OFF_TECH_VETERANCY);
    if (v < 0.0f) return 0;
    if (v >= 2.0f) return 2;
    if (v >= 1.0f) return 1;
    return 0;
}
static int Up_IsEnemy(void* o)
{
    void* own = *(void**)((char*)o + OFF_OWNER);
    void* me = *(void**)(size_t)ADDR_CURPLAYER;
    if (!own || !me) return 0;
    return own != me;
}

static void Up_BuildList(void)
{
    void** items = *(void***)(size_t)(ADDR_CUR_OBJECTS + OFF_VEC_ITEMS);
    int n = *(int*)(size_t)(ADDR_CUR_OBJECTS + OFF_VEC_COUNT);
    int i;
    s_ListN = 0;
    if (!items || !IsPlausiblePtr(items)) return;
    if (n <= 0) return;
    if (n > UP_LIST_CAP) n = UP_LIST_CAP;
    for (i = 0; i < n; i++)
    {
        void* o = items[i];
        void* t;
        int am;
        if (!o || !IsPlausiblePtr(o)) continue;
        am = Up_WhatAmI(o);
        t = Up_TypeOf(o, am);
        if (!t) continue;
        s_List[s_ListN].obj = o;
        s_List[s_ListN].type = t;
        s_List[s_ListN].count = 1;
        s_ListN++;
    }
}

/* 同型判定 (类型指针相同即同型) */
static int Up_AllSameType(void)
{
    int i;
    if (s_ListN < 2) return 1;
    for (i = 1; i < s_ListN; i++)
        if (s_List[i].type != s_List[0].type) return 0;
    return 1;
}

/* 弹药比例 (没有弹药的一律算"满", 排后面) */
static int Up_AmmoPct(const UpEnt* e)
{
    int mx = Up_AmmoMax(e->type);
    int am;
    if (mx <= 0) return 1000;
    am = Up_Ammo(e->obj);
    if (am < 0) am = 0;
    if (am > mx) am = mx;
    return am * 1000 / mx;
}

/* 按弹药从少到多排 (空弹优先显示) */
static void Up_SortByAmmo(UpEnt* a, int n)
{
    int i, j;
    for (i = 1; i < n; i++)
    {
        UpEnt k = a[i];
        int kp = Up_AmmoPct(&k);
        for (j = i - 1; j >= 0; j--)
        {
            int jp = Up_AmmoPct(&a[j]);
            if (jp <= kp) break;
            a[j + 1] = a[j];
        }
        a[j + 1] = k;
    }
}

/* ---------------- NoManualUnload (Ares 的 TechnoType 键) ----------------
 * 有些单位的"特殊功能"就是载员本体 (猎杀者的弹头、磁电束、鹦鹉螺的鱼雷、
 * 清道夫的抓取), rules 里用 `NoManualUnload=yes` 关掉引擎的手动卸载。
 * 这类载员不是可下车的乘客, 面板一律不列 —— 列出来就是个点了也没反应的格子,
 * 还会让人以为能把功能部件卸下来。
 *
 * 读法: INI_Rules (ADDR_INI_RULES 里存 CCINIClass*) + ReadBool, 节名用类型
 * 自己的 ID (持久指针, 绕开 INIClass 的节名指针缓存)。按 type 缓存; 换图/重开
 * 会换 INI_Rules 指针, 那时整组作废重查。 */
#define UP_NMU_CAP 64
typedef struct { void* type; int val; } UpNmu;
static UpNmu s_Nmu[UP_NMU_CAP];
static int   s_NmuN = 0;
static void* s_NmuIni = NULL;
static int   s_NmuProbe = 0;      /* 本世代是否已记过一条诊断 */

static int Up_NoManualUnload(void* type)
{
    typedef unsigned char (__attribute__((thiscall)) *Fn_ReadBool)(void* ini, const char* sec,
                                                                 const char* key, int def);
    void* ini = *(void**)ADDR_INI_RULES;
    const char* id;
    int i, v;

    if (!type || !IsPlausiblePtr(type)) return 0;
    if (ini != s_NmuIni) { s_NmuIni = ini; s_NmuN = 0; s_NmuProbe = 0; }
    for (i = 0; i < s_NmuN; i++) if (s_Nmu[i].type == type) return s_Nmu[i].val;
    if (!ini || !IsPlausiblePtr(ini)) return 0;

    id = (const char*)((char*)type + OFF_TYPE_ID);
    if (MemReadableBytes(id, 0x18) < 0x18 || !id[0]) return 0;
    v = ((Fn_ReadBool)(void*)FN_INI_READBOOL)(ini, id, "NoManualUnload", 0) ? 1 : 0;

    if (s_NmuN < UP_NMU_CAP)
    {
        s_Nmu[s_NmuN].type = type;
        s_Nmu[s_NmuN].val = v;
        s_NmuN++;
    }
    else
    {
        s_Nmu[0].type = type;
        s_Nmu[0].val = v;
    }

    /* 诊断: 每个 rules 时代第一条 (不论真假) 记下来 —— ini 指针为 0 或读不出值
     * 都能一眼看出来; 之后只有真值才记, 免得日志被普通单位刷满。 */
    if (g_Log && (!s_NmuProbe || v))
    {
        char buf[112];
        unsigned q = 0;
        q = AppendCStr(buf, q, "up: nmu id=", sizeof(buf));
        q = Up_AppendId(buf, sizeof(buf), q, id, 16, '?');
        q = AppendCStr(buf, q, " v=", sizeof(buf));
        q += UtoA((unsigned)v, buf + q);
        q = AppendCStr(buf, q, " ini=", sizeof(buf));
        q += HtoA((unsigned)(size_t)ini, buf + q);
        LogLine(buf);
        s_NmuProbe = 1;
    }
    return v;
}

/* 驻军建筑: 载员在 BuildingClass::Occupants 向量里 (Items 0x688 / Count 0x694),
 * 与载具的 Passengers 链是两套存储。向量序就是面板序, 点击时按同一序反查
 * (SingleUnload.cpp)。 */
static int Up_AddOccupants(void* o, UpEnt* all, int n)
{
    void** items = *(void***)((char*)o + OFF_BLD_OCC_ITEMS);
    int cnt = *(int*)((char*)o + OFF_BLD_OCC_COUNT);
    int i;

    if (!items || !IsPlausiblePtr(items)) return n;
    if (cnt < 0) return n;
    if (cnt > 64) cnt = 64;
    for (i = 0; i < cnt && n < UP_ENT_MAX; i++)
    {
        void* q = items[i];
        void* t;
        if (!q || !IsPlausiblePtr(q)) continue;
        t = Up_TypeOf(q, Up_WhatAmI(q));
        if (!t) continue;
        all[n].obj = q; all[n].type = t; all[n].count = 1;
        n++;
    }
    return n;
}

/* 重建整张表 + 当前页的 9 格 */
static void Up_FitGrid(int n);
static void Up_Rebuild(void)
{
    static UpEnt all[UP_ENT_MAX];
    int n = 0, i;

    s_Paged = 1;
    s_Trunc = 0;
    if (s_ListN == 1)
    {
        /* 单选: 图标 + 信息 + 载员。载员超过 (9-2) 个就翻页, 前两格每页固定。
         * NoManualUnload 的单位载员是功能本体, 不算乘客, 一格都不列。 */
        void* o = s_List[0].obj;
        int cap = UP_CELLN - 2;
        int occ;
        int showPax = Up_NoManualUnload(s_List[0].type) ? 0 : 1;
        all[0].obj = o; all[0].type = s_List[0].type; all[0].count = 1;
        all[1].obj = o; all[1].type = s_List[0].type; all[1].count = 0;   /* 0 => 信息格 */
        n = 2;
        if (showPax && Up_WhatAmI(o) == ABS_BUILDING)
        {
            n = Up_AddOccupants(o, all, n);
        }
        else if (showPax)
        {
            int pn = *(int*)((char*)o + OFF_TECH_PASSNUM);
            void* p = *(void**)((char*)o + OFF_TECH_PASSFIRST);
            if (pn > 0 && p && IsPlausiblePtr(p))
            {
                int guard = 0;
                while (p && IsPlausiblePtr(p) && guard < 64 && n < UP_ENT_MAX)
                {
                    void* t = Up_TypeOf(p, Up_WhatAmI(p));
                    if (t)
                    {
                        all[n].obj = p; all[n].type = t; all[n].count = 1;
                        n++;
                    }
                    p = *(void**)((char*)p + OFF_NEXT_OBJ);
                    guard++;
                }
            }
        }
        occ = n - 2;
        if (occ <= cap)
        {
            s_Paged = 0;
            s_Pages = 1;
        }
        else
        {
            s_Paged = 2;
            s_Pages = (occ + cap - 1) / cap;
            /* 超过 3 页: 末页最后一格当"更多"翻页 (与列表版式一致)。 */
            s_Trunc = (occ > UP_PAGES * cap) ? 1 : 0;
        }
    }
    else if (s_ListN > 1 && Up_AllSameType())
    {
        /* 同型: 每个一 格, 弹药少的在前 */
        for (i = 0; i < s_ListN && i < UP_ENT_MAX; i++) all[i] = s_List[i];
        n = Up_Min(s_ListN, UP_ENT_MAX);
        Up_SortByAmmo(all, n);
        s_Pages = (n + UP_CELLN - 1) / UP_CELLN;
    }
    else if (s_ListN <= UP_CELLN)
    {
        /* 少量混型: 全展开, 同类型相邻 */
        int j;
        for (i = 0; i < s_ListN; i++) all[i] = s_List[i];
        n = s_ListN;
        for (i = 1; i < n; i++)
        {
            UpEnt k = all[i];
            for (j = i - 1; j >= 0 && (unsigned)(size_t)all[j].type > (unsigned)(size_t)k.type; j--)
                all[j + 1] = all[j];
            all[j + 1] = k;
        }
        s_Pages = 1;
    }
    else
    {
        /* 大量混型: 按组显示 */
        UpEnt g[UP_ENT_MAX];
        int gn = 0, j;
        for (i = 0; i < s_ListN; i++)
        {
            for (j = 0; j < gn; j++) if (g[j].type == s_List[i].type) break;
            if (j < gn) g[j].count++;
            else if (gn < UP_ENT_MAX)
            {
                g[gn].obj = s_List[i].obj;
                g[gn].type = s_List[i].type;
                g[gn].count = 1;
                gn++;
            }
        }
        for (i = 1; i < gn; i++)
        {
            UpEnt k = g[i];
            for (j = i - 1; j >= 0 && g[j].count < k.count; j--) g[j + 1] = g[j];
            g[j + 1] = k;
        }
        for (i = 0; i < gn && i < UP_ENT_MAX; i++) all[i] = g[i];
        n = Up_Min(gn, UP_ENT_MAX);
        s_Trunc = (gn > UP_ENT_MAX) ? 1 : 0;
        s_Pages = (n + UP_CELLN - 1) / UP_CELLN;
    }

    if (s_Pages > UP_PAGES) s_Pages = UP_PAGES;
    if (s_Pages < 1) s_Pages = 1;
    if (s_Page >= s_Pages) s_Page = s_Pages - 1;
    if (s_Page < 0) s_Page = 0;

    /* 当前页: 单选分页 (s_Paged==2) 时前两格固定, 其余按 (UP_CELLN-2) 个一页 */
    for (i = 0; i < UP_CELLN; i++)
    {
        int idx;
        if (s_Paged == 2)
            idx = (i < 2) ? i : (2 + s_Page * (UP_CELLN - 2) + (i - 2));
        else
            idx = s_Paged ? (s_Page * UP_CELLN + i) : i;
        if (idx < n) s_Cell[i] = all[idx];
        else { s_Cell[i].obj = NULL; s_Cell[i].type = NULL; s_Cell[i].count = -1; }
    }
    /* 超 3 页: 第 27 格 (== 末页最后一个) 换成"更多" */
    if (s_Trunc && s_Page == s_Pages - 1)
    {
        s_Cell[UP_CELLN - 1].obj = NULL;
        s_Cell[UP_CELLN - 1].type = NULL;
        s_Cell[UP_CELLN - 1].count = -2;   /* -2 => 更多 */
    }
    Up_FitGrid(n);
}

/* 面板只铺需要的格数: 2 格就 2x1, 4 架就 2x2, 9 个以上才 3x3。
 * 多页时固定 3x3 (每页 9 格, 末格可能是"更多")。 */
static void Up_FitGrid(int n)
{
    static const unsigned char kCols[10] = { 1, 1, 2, 3, 2, 3, 3, 3, 3, 3 };
    int cols;
    if (s_Pages > 1) { s_Cols = UP_COLS_MAX; s_Rows = UP_ROWS_MAX; return; }
    if (n < 1) n = 1;
    if (n > UP_CELLN) n = UP_CELLN;
    cols = kCols[n];
    s_Cols = cols;
    s_Rows = (n + cols - 1) / cols;
}

/* 是否需要重建 (选中集变化 / 弹药可能变化) */
static unsigned Up_Sig(void)
{
    unsigned s = (unsigned)s_ListN * 2246822519u;
    int i;
    for (i = 0; i < s_ListN; i++)
        s = s * 31u + (unsigned)(size_t)s_List[i].obj;
    return s;
}

/* 快照每轮重建 (选中集会随时变), 但**重排格子只在签名变了或 250ms 到点时做**:
 * 排序/分组每帧跑一遍是白烧, 而弹药顺序只影响显示顺序, 250ms 的滞后看不出来。 */
static void Up_Refresh(void)
{
    unsigned now = GetTickCount();
    unsigned sig;
    int msDue;

    Up_BuildList();
    sig = Up_Sig();
    msDue = (unsigned)(now - s_ListMs) >= 250u;
    if (sig != s_ListSig)
    {
        s_ListSig = sig;
        s_Page = 0;
    }
    else if (!msDue) return;
    s_ListMs = now;
    Up_Rebuild();
}

static unsigned short* Up_Cameo(void* type);
static unsigned short* Up_CapFind(void* type);

/* 备齐当前页的图标: PCX 装载/ SHP 现渲都动引擎, 必须在扫描期做。 */
static void Up_PrepareIcons(void)
{
    int i;
    if (s_Collapsed) return;
    for (i = 0; i < UP_CELLN; i++)
    {
        const UpEnt* e = &s_Cell[i];
        if (e->type && e->count >= 0) Up_Cameo(e->type);
    }
}

static void Up_ForceRebuild(void)
{
    s_ListSig = 0;
    s_ListMs = 0;
}

/* ---------------- 图标 (cameo) ----------------
 * MO 的单位图标绝大多数是 `Cameo=xxx.pcx` (Ares 键), 这时 `TechnoTypeClass::Cameo`
 * 指向 XXICON.SHP 占位图 —— 照 SHP 画出来的就是那个紫球。所以先读 CameoFile:
 * 是 .pcx 就走 IconSnap_Pcx (与超武侧栏同一条快照路), 否则才用 IconSnap_Shp
 * 现渲 (旧式固定调色板图标)。
 * PCX 装载会动引擎的面池, 只能在 UP_Poll (扫描期) 做, 绘制帧里只读缓存。 */

static unsigned short* Up_CapFind(void* type)
{
    int i;
    for (i = 0; i < s_CapN; i++) if (s_Cap[i].type == type) return s_Cap[i].pix;
    return NULL;
}

/* 图标文件名: 先问 art ini 的 `CameoPCX=` (Ares 键, MO 的单位图标都在这), 没有再
 * 退回类型自己的 `Cameo=` 字段。返回 1 = 拿到 .pcx 名。
 * ⚠ 节名必须传类型对象里的持久字符串 (ImageFile / ID): INIClass 按**指针**缓存上一次
 * 查的节, 传栈上数组会让所有类型都命中第一次的缓存 (详见 EngineAddr.h 的 ADDR_INI_ART)。
 * secOut 只回带节名给日志看, 不参与查询。 */
static int Up_CameoPcxName(void* type, char* out, int cap, char* secOut, int secCap)
{
    typedef int (__attribute__((thiscall)) *Fn_ReadStr)(void* ini, const char* sec,
                                                        const char* key, const char* def,
                                                        char* buf, unsigned size);
    const char* img = (const char*)((char*)type + OFF_TYPE_IMAGEFILE);
    const char* id = (const char*)((char*)type + OFF_TYPE_ID);
    const char* use;
    int n, i;

    out[0] = 0;
    if (secOut && secCap > 0) secOut[0] = 0;
    /* img/id 各自判可读再选: 任一边不可读都不能解引用, 两边都不可读才放弃。 */
    if (MemReadableBytes(img, 0x19) >= 0x19 && img[0])
        use = img;
    else
    {
        if (MemReadableBytes(id, 0x18) < 0x18 || !id[0]) return 0;
        use = id;
    }
    if (secOut)
    {
        int k;
        for (k = 0; k + 1 < secCap && use[k]; k++) secOut[k] = use[k];
        secOut[k] = 0;
    }

    if (IsPlausiblePtr((void*)ADDR_INI_ART))
        ((Fn_ReadStr)(void*)FN_INI_READSTRING)((void*)ADDR_INI_ART, use, "CameoPCX",
                                              "", out, (unsigned)cap);
    if (!out[0])
    {
        /* 类型自带的 Cameo= (可能是 .pcx, 也可能空) */
        const char* f = (const char*)((char*)type + OFF_TT_CAMEOFILE);
        if (MemReadableBytes(f, 0x19) < 0x19) return 0;
        for (n = 0; n < 0x19 && f[n]; n++) if (n < cap - 1) out[n] = f[n];
        out[n < cap ? n : cap - 1] = 0;
    }
    n = 0;
    while (out[n]) n++;
    if (n < 5) { out[0] = 0; return 0; }
    for (i = 0; i < n; i++)
    {
        if (out[i] == '\\') out[i] = '/';
        else if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    }
    if (out[n - 4] == '.' && out[n - 3] == 'p' && out[n - 2] == 'c' && out[n - 1] == 'x')
        return 1;
    out[0] = 0;
    return 0;
}

/* 取某类型的 60x48 图标 (首次现备), NULL = 这个类型没有图标。
 * 只在 UP_Poll 里调: PCX 装载会动引擎面池。 */
static unsigned short* Up_Cameo(void* type)
{
    unsigned short* px;
    char name[64];
    char sec[32];
    int fromPcx = 0;
    int pcxW = 0, pcxH = 0;   /* 源面尺寸, 只给日志看 (cameo 该是 60x48) */
    void* shp;

    if (!type || !IsPlausiblePtr(type)) return NULL;
    px = Up_CapFind(type);
    if (px) return px;

    name[0] = 0;
    sec[0] = 0;
    if (Up_CameoPcxName(type, name, (int)sizeof(name), sec, (int)sizeof(sec)))
    {
        px = IconSnap_Pcx(name, UP_ICON_W, UP_ICON_H, &pcxW, &pcxH);
        fromPcx = (px != NULL);
    }
    if (!px)
    {
        shp = *(void**)((char*)type + OFF_TT_CAMEO);
        if (shp && IsPlausiblePtr(shp)) px = IconSnap_Shp(shp, UP_ICON_W, UP_ICON_H);
        if (!px) return NULL;
    }

    if (s_CapN < 12 && g_Log)
    {
        /* 对应关系排查: 类型 ID / 查的 art 节 / 实际贴的 pcx */
        char buf[192];
        unsigned q = 0;
        const char* id = (const char*)((char*)type + OFF_TYPE_ID);
        q = AppendCStr(buf, q, "up: icon id=", sizeof(buf));
        q = Up_AppendId(buf, sizeof(buf), q, id, 16, '?');
        q = AppendCStr(buf, q, " sec=", sizeof(buf));
        q = AppendCStr(buf, q, sec[0] ? sec : "?", sizeof(buf));
        q = AppendCStr(buf, q, fromPcx ? " pcx=" : " shp=", sizeof(buf));
        q = AppendCStr(buf, q, name[0] ? name : "none", sizeof(buf));
        if (fromPcx && q + 8 < sizeof(buf))
        {
            q = AppendCh(buf, q, ' ', sizeof(buf));
            q += UtoA((unsigned)pcxW, buf + q);
            q = AppendCh(buf, q, 'x', sizeof(buf));
            q += UtoA((unsigned)pcxH, buf + q);
        }
        LogLine(buf);
    }

    if (s_CapN < UP_CAP_N)
    {
        s_Cap[s_CapN].type = type;
        s_Cap[s_CapN].pix = px;
        s_CapN++;
    }
    else
    {
        /* 满了就换掉第 0 条 (老图标基本不会被再选到) */
        LocalFree(s_Cap[0].pix);
        s_Cap[0].type = type;
        s_Cap[0].pix = px;
    }
    return px;
}

static void Up_BlitIcon(unsigned char* base, int pitch, int W, int H,
                        int dx, int dy, const unsigned short* px)
{
    int y, x;
    if (!px) return;
    for (y = 0; y < UP_ICON_H; y++)
    {
        int sy = dy + y;
        const unsigned short* srow;
        unsigned short* drow;
        if (sy < 0 || sy >= H) continue;
        srow = px + y * UP_ICON_W;
        drow = (unsigned short*)(base + sy * pitch);
        for (x = 0; x < UP_ICON_W; x++)
        {
            int sx = dx + x;
            unsigned short v;
            if (sx < 0 || sx >= W) continue;
            v = srow[x];
            if (v == UP_KEY) continue;
            drow[sx] = v;
        }
    }
}

/* ---------------- 九宫格外框 ---------------- */

static void Up_LoadBorder(void)
{
    PcxImage img;
    int x, y;
    if (s_BorderOK) return;
    if (!PcxImage_Load(&img, "border_mental_n9_B.pcx", 2)) return;
    if (img.width < UP_SRC_L + UP_SRC_R + 2 || img.height < UP_SRC_T + UP_SRC_B + 2) return;
    if (img.width > UP_N9_MAX || img.height > UP_N9_MAX) return;
    s_Bw = img.width;
    s_Bh = img.height;
    s_BorderKey = img.key;
    for (y = 0; y < s_Bh; y++)
        for (x = 0; x < s_Bw; x++)
            s_Border[y * UP_N9_MAX + x] =
                ((const unsigned short*)img.px)[y * s_Bw + x];
    s_BorderOK = 1;
    if (g_Log)
    {
        char buf[128];
        unsigned q = 0;
        q = AppendCStr(buf, q, "up: border n9 ", sizeof(buf));
        q += UtoA((unsigned)s_Bw, buf + q);
        q = AppendCh(buf, q, 'x', sizeof(buf));
        q += UtoA((unsigned)s_Bh, buf + q);
        q = AppendCStr(buf, q, " key=", sizeof(buf));
        q += HtoA((unsigned)s_BorderKey, buf + q);
        LogLine(buf);
    }
}

/* 九宫格坐标映射: 四边框带按源分割线 1:1 贴, 中间段拉伸。
 * padA/padB 是目标带宽, srcA/srcB 是源分割线; 两者相等时直接原样取 (不重采样)。 */
static int Up_Slice(int v, int outLen, int srcLen, int padA, int padB,
                    int srcA, int srcB)
{
    int om, sm;
    if (padA < 0) padA = 0;
    if (padB < 0) padB = 0;
    if (v < padA)
        return (padA == srcA) ? v : ((padA > 0) ? (v * srcA / padA) : 0);
    if (v >= outLen - padB)
    {
        int k = outLen - 1 - v;                       /* 从末端往回数 */
        int s = srcLen - 1 - ((padB == srcB) ? k : (padB > 0 ? (k * srcB / padB) : 0));
        if (s < srcLen - srcB) s = srcLen - srcB;
        if (s < 0) s = 0;
        if (s >= srcLen) s = srcLen - 1;
        return s;
    }
    om = outLen - padA - padB;
    sm = srcLen - srcA - srcB;
    if (om <= 0) return srcA;
    return srcA + (v - padA) * sm / om;
}

static int Up_BuildBg(void)
{
    int x, y;
    Up_LoadBorder();
    if (!s_BorderOK) return 0;
    if (s_Bg && s_BgW == s_Pw && s_BgH == s_Ph) return 1;
    if (s_Bg) { LocalFree(s_Bg); s_Bg = NULL; }
    s_Bg = (unsigned short*)LocalAlloc(LMEM_FIXED,
            (SIZE_T)s_Pw * (SIZE_T)s_Ph * sizeof(unsigned short));
    if (!s_Bg) return 0;
    for (y = 0; y < s_Ph; y++)
    {
        int sy = Up_Slice(y, s_Ph, s_Bh, UP_PAD_T, UP_PAD_B, UP_SRC_T, UP_SRC_B);
        if (sy < 0) sy = 0;
        if (sy >= s_Bh) sy = s_Bh - 1;
        for (x = 0; x < s_Pw; x++)
        {
            int sx = Up_Slice(x, s_Pw, s_Bw, UP_PAD_L, UP_PAD_R, UP_SRC_L, UP_SRC_R);
            if (sx < 0) sx = 0;
            if (sx >= s_Bw) sx = s_Bw - 1;
            s_Bg[y * s_Pw + x] = s_Border[sy * UP_N9_MAX + sx];
        }
    }
    s_BgW = s_Pw;
    s_BgH = s_Ph;
    return 1;
}

/* 把 bg 贴到 Composite (色键像素跳过, 四角是透的)。
 * ringOnly=1 只贴四边框带: 这张九宫格中心是不透明近黑色, 整块贴会把 cameo 盖掉,
 * 所以分两遍 —— 先满贴当内底, 画完格子再只补框带。 */
static void Up_BlitBg(unsigned char* base, int pitch, int W, int H, int dx, int dy,
                      int ringOnly)
{
    int y, x, step = pitch >> 1;
    for (y = 0; y < s_Ph; y++)
    {
        int sy = dy + y;
        const unsigned short* srow;
        unsigned short* drow;
        if (sy < 0 || sy >= H) continue;
        srow = s_Bg + y * s_Pw;
        drow = (unsigned short*)base + sy * step;
        for (x = 0; x < s_Pw; x++)
        {
            int sx = dx + x;
            unsigned short v = srow[x];
            if (sx < 0 || sx >= W) continue;
            if (ringOnly && x >= UP_PAD_L && x < s_Pw - UP_PAD_R &&
                y >= UP_PAD_T && y < s_Ph - UP_PAD_B) continue;
            if (v == s_BorderKey) continue;
            drow[sx] = v;
        }
    }
}

/* ---------------- 绘制: 小图元 ---------------- */

static void Up_Bar(unsigned char* base, int pitch, int W, int H,
                   int x, int y, int w, int h, int pct, int maxed,
                   unsigned r, unsigned g, unsigned b)
{
    int fw;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (maxed) pct = 100;
    Up_Fill(base, pitch, W, H, x, y, w, h, 24, 24, 26, 235);
    fw = w * pct / 100;
    if (fw > 0) Up_Fill(base, pitch, W, H, x, y, fw, h, r, g, b, 255);
    /* 一像素暗边, 让条在深底上也看得清 */
    Up_Fill(base, pitch, W, H, x, y, w, 1, 60, 60, 70, 200);
}

/* 军衔标记: 与游戏一致 —— 一级一个金色 ︿, 二级金色星星。
 * 两者都先按 3x3 铺一层深色轮廓再压金色芯, 压在亮 cameo 上也看得清。 */
static const char* const k_Vet1[5] = {
    "....X....",
    "...XXX...",
    "..XX.XX..",
    ".XX...XX.",
    "X.......X"
};
static const char* const k_Vet2[9] = {
    "....X....",
    "...XXX...",
    "XXXXXXXXX",
    ".XXXXXXX.",
    "..XXXXX..",
    "..XXXXX..",
    ".XX...XX.",
    ".X.....X.",
    "........."
};

static void Up_Glyph(unsigned char* base, int pitch, int W, int H,
                     int x, int y, const char* const* g, int n, int w,
                     unsigned r, unsigned gg, unsigned b)
{
    int j, i;
    for (j = 0; j < n; j++)
        for (i = 0; i < w; i++)
            if (g[j][i] == 'X')
                Up_Fill(base, pitch, W, H, x + i - 1, y + j - 1, 3, 3, 0, 0, 0, 225);
    for (j = 0; j < n; j++)
        for (i = 0; i < w; i++)
            if (g[j][i] == 'X')
                Up_Fill(base, pitch, W, H, x + i, y + j, 1, 1, r, gg, b, 255);
}

static void Up_Rank(unsigned char* base, int pitch, int W, int H,
                    int x, int y, int level)
{
    if (level >= 2)
        Up_Glyph(base, pitch, W, H, x, y, k_Vet2, 9, 9, 255, 208, 60);
    else if (level >= 1)
        Up_Glyph(base, pitch, W, H, x, y + 2, k_Vet1, 5, 9, 255, 208, 60);
}

static const char* const k_Star[11] = {
    ".....X.....",
    "....XXX....",
    "....XXX....",
    "XXXXXXXXXXX",
    ".XXXXXXXXX.",
    "..XXXXXXX..",
    "...XXXXX...",
    "...XXXXX...",
    "..XX...XX..",
    ".XX.....XX.",
    "..........."
};

static void Up_Star(unsigned char* base, int pitch, int W, int H,
                    int x, int y, unsigned r, unsigned g, unsigned b, int a)
{
    int j, i;
    for (j = 0; j < 11; j++)
        for (i = 0; i < 11; i++)
            if (k_Star[j][i] == 'X')
                OU_Put(base, W, H, pitch, NULL, x + i, y + j, r, g, b, a);
}

static void Up_Tri(unsigned char* base, int pitch, int W, int H,
                   int x, int y, int right, unsigned r, unsigned g, unsigned b)
{
    int j;
    for (j = 0; j < 11; j++)
    {
        int half = (j < 5) ? (5 - j) : (j - 5);
        int len = 6 - half;
        int i;
        for (i = 0; i < len; i++)
            OU_Put(base, W, H, pitch, NULL,
                   x + (right ? i : (len - 1 - i)), y + j, r, g, b, 255);
    }
}

/* 按钮底板: 平时不画 (让图标直接压在地图上), 只有悬停/激活才给底 */
static void Up_BtnBg(unsigned char* base, int pitch, int W, int H,
                     int x, int y, int hot, int active)
{
    unsigned char br, bg2, bb;
    if (!hot && !active) return;
    Up_Fill(base, pitch, W, H, x, y, UP_BTN_W, UP_BTN_H, 22, 22, 26, 200);
    if (active) { br = 240; bg2 = 200; bb = 80; }
    else { br = 210; bg2 = 210; bb = 225; }
    Up_Fill(base, pitch, W, H, x, y, UP_BTN_W, 1, br, bg2, bb, 235);
    Up_Fill(base, pitch, W, H, x, y + UP_BTN_H - 1, UP_BTN_W, 1, br, bg2, bb, 235);
    Up_Fill(base, pitch, W, H, x, y, 1, UP_BTN_H, br, bg2, bb, 235);
    Up_Fill(base, pitch, W, H, x + UP_BTN_W - 1, y, 1, UP_BTN_H, br, bg2, bb, 235);
}

/* ---------------- 绘制: 格子 ---------------- */

static void Up_DrawCellBg(unsigned char* base, int pitch, int W, int H,
                          int x, int y)
{
    Up_Fill(base, pitch, W, H, x + 1, y + 1, UP_CELL_W - 2, UP_CELL_H - 2,
            10, 10, 12, 90);
}

/* 高亮框: 2px 厚 (外 1px 贴格边 + 内 1px 紧贴它), 必须画在图标**之后**,
 * 否则 60x48 的 cameo 会整个盖掉它。 */
static void Up_CellFrame(unsigned char* base, int pitch, int W, int H,
                         int x, int y, int hot)
{
    unsigned char r = 255, g = 205, b = 60;
    int k;
    if (!hot) return;
    for (k = 0; k < 2; k++)
    {
        int xx = x + k, yy = y + k;
        int ww = UP_CELL_W - k * 2, hh = UP_CELL_H - k * 2;
        if (ww <= 0 || hh <= 0) break;
        Up_Fill(base, pitch, W, H, xx, yy, ww, 1, r, g, b, 255);
        Up_Fill(base, pitch, W, H, xx, yy + hh - 1, ww, 1, r, g, b, 255);
        Up_Fill(base, pitch, W, H, xx, yy, 1, hh, r, g, b, 255);
        Up_Fill(base, pitch, W, H, xx + ww - 1, yy, 1, hh, r, g, b, 255);
    }
}

/* 弹药格: MO 的 cameo 底部烧死了单位名 (约 12px), 所以弹药格不贴格底,
 * 而是抬到名字上方、从左下往右铺; 一排几个看 rules 的 `PipWrap=`,
 * 格数看 `Ammo=` (最多发数), 满的发亮黄、空的是暗块。
 * 格子 2(宽) x 3(高), 横竖各自留 1px 缝; 整块外面包 1px 深色轮廓。 */
static void Up_AmmoPips(unsigned char* base, int pitch, int W, int H,
                        int x, int y, int ammo, int ammoMax, int wrap)
{
    int pw = 2, ph = 3, gap = 1, outline = 1;
    int hs, vs, rows, cols, k, left, bottom, bw, bh, top;

    if (ammoMax <= 0 || ammoMax > 48) return;
    hs = pw + gap;
    vs = ph + gap;
    left = x + 1;
    bottom = y + UP_CELL_H - UP_NAME_H - 1 + 2;    /* 名字上方, 再下移 2 */
    if (bottom - (y + 10) < vs) return;
    if (wrap < 1 || wrap > 12) wrap = 6;           /* 没配 PipWrap 时默认 6 个一排 */
    cols = wrap;
    if (cols > ammoMax) cols = ammoMax;
    {   /* 宽度上限: 60 - 左右轮廓 */
        int fit = (UP_CELL_W - outline * 2 - 1 + gap) / hs;
        if (cols > fit) cols = fit;
    }
    if (cols < 1) return;
    rows = (ammoMax + cols - 1) / cols;
    if (rows * vs > bottom - (y + 10)) rows = (bottom - (y + 10)) / vs;
    if (rows < 1) return;

    bw = cols * pw + (cols - 1) * gap;
    bh = rows * ph + (rows - 1) * gap;
    top = bottom - bh;                             /* 最后一排的下沿就是 bottom-1 */
    {   /* 轮廓不许越过格子左边界 (会压到相邻格的 1px 缝上) */
        int bx = left - outline;
        int ow = bw + outline * 2;
        if (bx < x) { ow -= (x - bx); bx = x; }
        Up_Fill(base, pitch, W, H, bx, top - outline, ow, bh + outline * 2,
                10, 10, 12, 200);
    }

    for (k = 0; k < ammoMax; k++)
    {
        int cy = k / cols;
        int px = left + (k % cols) * hs;
        int py = bottom - (cy + 1) * vs + gap;
        if (cy >= rows) break;
        if (k < ammo)
            Up_Fill(base, pitch, W, H, px, py, pw, ph, 250, 205, 75, 255);
        else
            Up_Fill(base, pitch, W, H, px, py, pw, ph, 74, 74, 80, 235);
    }
}

/* 数量角标 (组显示时右上角的数字): 字体/字号/粗细全取资金 (顶栏资源数字) 那套,
 * 底色贴格子右上角不留缝, 四边各留 2px, 底按墨迹高度铺。 */
static void Up_CountBadge(unsigned char* base, int pitch, int W, int H,
                          int cx, int cy, int count)
{
    wchar_t t[16];
    int n, w, top, bot, bw, bh, bx, by, px;
    if (count <= 1) return;
    px = VecText_CreditPx();
    if (px <= 0) px = VecText_FontPx();
    if (px < 8) px = 8;
    if (px > 24) px = 24;
    if (!GdiEnsure(px, VecText_Bold(), UP_BADGE_SLOT, VecText_FontFace())) return;
    n = Up_Num(t, 16, count);
    w = RenderLine(t, n);
    if (w > 0)
    {
        Up_InkTB(w, &top, &bot);
        bw = w + 4;
        bh = (bot - top + 1) + 4;
        if (bw > UP_CELL_W) bw = UP_CELL_W;
        if (bh > UP_CELL_H) bh = UP_CELL_H;
        bx = cx + UP_CELL_W - bw;              /* 贴右上角, 不留缝 */
        by = cy;
        Up_Fill(base, pitch, W, H, bx, by, bw, bh, 0, 0, 0, 150);
        BlitCoverage(base, pitch, W, H, bx + 2, by + 2 - top, w + 2, g_G->adv,
                     255, 255, 255, 100);
    }
    /* 换回面板正文字号, 后面的文本才按 s_FontPx 排 */
    GdiEnsure(s_FontPx, 0, UP_GDI_SLOT, s_Face);
}

/* 单格图标 + 角标 (等级/弹药/数量) */
static void Up_DrawIconCell(unsigned char* base, int pitch, int W, int H,
                            int x, int y, const UpEnt* e)
{
    unsigned short* cameo;
    int enemy, ammoMax, ammo, level, wrap;

    Up_DrawCellBg(base, pitch, W, H, x, y);
    if (!e || !e->type || !e->obj) return;
    cameo = Up_Cameo(e->type);
    if (cameo) Up_BlitIcon(base, pitch, W, H, x, y, cameo);   /* 没有 cameo 就只留底与角标 */
    enemy = Up_IsEnemy(e->obj);
    ammoMax = Up_AmmoMax(e->type);
    ammo = Up_Ammo(e->obj);
    level = Up_Level(e->obj);
    wrap = Up_PipWrap(e->type);

    /* 等级 (仅我方): 图标左上, 一级 ︿ / 二级 星星 */
    if (!enemy && level > 0) Up_Rank(base, pitch, W, H, x + 2, y + 1, level);

    /* 弹药 (仅我方): 名字上方的小方块阵 */
    if (!enemy && ammoMax > 0 && ammo >= 0)
        Up_AmmoPips(base, pitch, W, H, x, y, ammo, ammoMax, wrap);

    /* 数量 (组): 右上角, 资金那套字体 */
    if (e->count > 1) Up_CountBadge(base, pitch, W, H, x, y, e->count);
}

/* 信息格: 血量 (当前/总) + 经验
 * 纵向排布按字号算 (by/ty/ey/ec 都是绝对 y): 血条 -> 血量文本 -> 经验条 -> 经验文本。
 * 字号大到最后一行放不下时只丢经验文本, 条仍然画, 绝不越出 48px。 */
static void Up_DrawInfoCell(unsigned char* base, int pitch, int W, int H,
                            int x, int y, const UpEnt* e)
{
    int hp, mx, pc, enemy, level, adv;
    float vet;
    wchar_t t[24];
    int n;
    unsigned char r = 60, g = 210, b = 60;
    unsigned char er = 90, eg = 150, eb = 240;
    int by, ty, ey, ec, bar1 = 5, bar2 = 4;

    Up_DrawCellBg(base, pitch, W, H, x, y);
    if (!e || !e->obj || !e->type) return;
    adv = g_G->adv;
    hp = Up_Hp(e->obj);
    mx = Up_HpMax(e->type);
    if (mx <= 0) mx = 1;
    if (hp < 0) hp = 0;
    if (hp > mx) hp = mx;
    pc = hp * 100 / mx;
    enemy = Up_IsEnemy(e->obj);
    level = Up_Level(e->obj);
    vet = *(float*)((char*)e->obj + OFF_TECH_VETERANCY);

    /* 血量色: 与游戏同口径 (>=50% 绿即默认色, >=25% 黄, 否则红) */
    if (pc >= 25) { r = 235; g = 210; b = 60; }
    else { r = 230; g = 60; b = 60; }
    /* 经验条颜色即等级: 新兵蓝 / 老兵青 / 精英金 */
    if (level >= 2) { er = 255; eg = 200; eb = 80; }
    else if (level >= 1) { er = 110; eg = 200; eb = 255; }

    /* 字号大到四行塞不下 (字号 11 时 adv=18, 四行正好 45) 时先压条高;
     * 再不行就由下面的守卫丢掉经验文本, 条照画, 绝不越出格子。 */
    if (bar1 + adv + bar2 + adv > UP_CELL_H) { bar1 = 4; bar2 = 3; }

    if (enemy)
        by = y + (UP_CELL_H - (bar1 + adv)) / 2;      /* 敌方: 只有条 + 血量, 竖向居中 */
    else
        by = y + (UP_CELL_H - (bar1 + adv + bar2 + adv)) / 2;
    if (by < y + 1) by = y + 1;

    /* 四行从上到下紧排 (文本自带行距, 不再额外留空): 条 / 血量 / 条 / 经验 */
    ty = by + bar1;
    ey = ty + adv;
    ec = ey + bar2;

    Up_Bar(base, pitch, W, H, x + 4, by, UP_CELL_W - 8, bar1, pc, 0, r, g, b);
    n = Up_NumPair(t, 24, hp, mx);
    /* 4 位以上的血量 "2000/2000" 会超格宽: 放不下就只留当前值 (比例看条) */
    if (RenderLine(t, n) > UP_CELL_W - 4) n = Up_Num(t, 24, hp);
    Up_Text(base, pitch, W, H, 0, ty, t, n, 235, 235, 235, 1, x + 2, UP_CELL_W - 4);
    if (enemy) return;   /* 敌方: 只显示图标 + 血量 */

    /* 经验: 当前等级内的进度 (精英 = MAX) */
    {
        int epc, isMax = (level >= 2) ? 1 : 0;
        if (isMax) epc = 100;
        else
        {
            float frac = vet;
            if (frac < 0.0f) frac = 0.0f;
            frac = frac - (float)(int)frac;
            epc = (int)(frac * 100.0f);
            if (epc < 0) epc = 0;
            if (epc > 99) epc = 99;
        }
        if (ey + bar2 > y + UP_CELL_H) return;
        Up_Bar(base, pitch, W, H, x + 4, ey, UP_CELL_W - 8, bar2, epc, isMax,
               er, eg, eb);
        if (ec + adv > y + UP_CELL_H) return;
        n = Up_Percent(t, 24, epc, isMax);
        Up_Text(base, pitch, W, H, 0, ec, t, n, er, eg, eb, 1, x + 2, UP_CELL_W - 4);
    }
}

static void Up_DrawMoreCell(unsigned char* base, int pitch, int W, int H,
                            int x, int y)
{
    static const wchar_t s_more[] = { 0x66F4, 0x591A, 0 };   /* 更多 */
    Up_DrawCellBg(base, pitch, W, H, x, y);
    Up_Text(base, pitch, W, H, 0, y + (UP_CELL_H - g_G->adv) / 2, s_more, 2,
            235, 235, 235, 1, x, UP_CELL_W);
}

/* ---------------- 绘制入口 ---------------- */

static void Up_DrawButtons(unsigned char* base, int pitch, int W, int H)
{
    int i;
    for (i = 0; i < s_BtnN; i++)
    {
        int x = s_Bx, y = s_By[i];
        int hot = (s_HoverBtn == i);
        int active = (i == UP_BTN_FOLD && s_Collapsed) ||
                     (i == UP_BTN_MODE && s_KeyMove);
        unsigned char cr, cg, cb;
        Up_BtnBg(base, pitch, W, H, x, y, hot, active);
        /* 图标按 18px 框居中: 三角 6x11, 三条 10x2, 星 11x11 */
        if (i == UP_BTN_FOLD) { cr = 235; cg = 235; cb = 245; }
        else if (active) { cr = 240; cg = 200; cb = 80; }
        else if (hot) { cr = 255; cg = 240; cb = 200; }
        else { cr = 195; cg = 195; cb = 205; }
        if (i == UP_BTN_FOLD)
            Up_Tri(base, pitch, W, H, x + 6, y + 4, s_Collapsed, cr, cg, cb);
        else if (i == UP_BTN_MODE)
        {
            int k;
            for (k = 0; k < 3; k++)
                Up_Fill(base, pitch, W, H, x + 4, y + 4 + k * 4, 10, 2,
                        cr, cg, cb, 255);
        }
        else
        {
            /* 收藏槽: 有收藏 = 该槽满色星; 空槽 = 同色暗星 (提示"这里还能收藏")。 */
            int slot = i - UP_BTN_FAV1;
            unsigned r = 0, g = 0, b = 0;
            Fav_Color(slot, &r, &g, &b);
            if (!Fav_Used(slot))
            {
                r = r / 3 + 20;
                g = g / 3 + 20;
                b = b / 3 + 20;
            }
            else if (hot)
            {
                r = (r + 255) / 2;
                g = (g + 255) / 2;
                b = (b + 255) / 2;
            }
            Up_Star(base, pitch, W, H, x + 4, y + 4, r, g, b, 255);
        }
    }
}

static void Up_DrawCollapsedButton(unsigned char* base, int pitch, int W, int H)
{
    int hot = (s_HoverBtn == UP_BTN_FOLD);
    Up_BtnBg(base, pitch, W, H, s_Cbx, s_Cby, hot, 1);
    Up_Tri(base, pitch, W, H, s_Cbx + 6, s_Cby + 4, 1, 240, 220, 120);
}

int UP_Draw(void)
{
    void* surf;
    unsigned char* base;
    int w, h, pitch, i, cx, cy;

    UP_EnsureInit();
    if (!s_Enable || !s_Vis) return 0;
    Up_Layout();
    Up_BuildBg();

    surf = *(void**)ADDR_SURF_COMPOSITE;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    w = GtSurfGet(surf, GtVT_GET_WIDTH);
    h = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (w <= 0 || h <= 0 || pitch < w * 2) return 0;

    if (!GdiEnsure(s_FontPx, 0, UP_GDI_SLOT, s_Face)) return 0;

    base = GtLock(surf);
    if (!base) return 0;

    if (s_Collapsed) Up_DrawCollapsedButton(base, pitch, w, h);
    else
    {
        int nslot = s_Cols * s_Rows;
        /* shift = 多选模式的显式开关: 只在按住时给"选中/参与规划"的载员格描框。
         * 松开就回到单人视图, 面板上不留任何多选痕迹。 */
        int sh = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;

        /* 没选中东西时不画九宫格 (只留左侧按钮列), 免得平白占一块黑板 */
        if (s_ListN > 0)
        {
            /* 第一遍: 满贴, 给格子和 1px 缝提供底 (九宫格中间是深色内底) */
            if (s_BorderOK) Up_BlitBg(base, pitch, w, h, s_Px, s_Py, 0);
            else Up_Fill(base, pitch, w, h, s_Px, s_Py, s_Pw, s_Ph, 8, 8, 10, 210);

            for (i = 0; i < nslot; i++)
            {
                const UpEnt* e = &s_Cell[i];
                int hot = (s_HoverCell == i);
                int pick = 0;
                Up_CellPos(i, &cx, &cy);
                /* 悬停/选中高亮统一由下面的 Up_CellFrame 画, 格内容不区分状态。 */
                if (e->count == -2) Up_DrawMoreCell(base, pitch, w, h, cx, cy);
                else if (!e->type || !e->obj) Up_DrawCellBg(base, pitch, w, h, cx, cy);
                else if (e->count == 0) Up_DrawInfoCell(base, pitch, w, h, cx, cy, e);
                else Up_DrawIconCell(base, pitch, w, h, cx, cy, e);
                /* 载员格高亮 (2px 黄框, 与悬停同一个框), 两种来源:
                 *   · 参与本次规划的载员 —— 一直画, 单人规划也画 (那就是"选中了这个人"),
                 *     包括 Z+点载员格认下的那个 (他不在 shift 名单里);
                 *   · shift 名单 —— 只在按住 shift 时画。
                 * 排除本车那一格用对象比较: 分页版式下第 0/1 格也是载员。 */
                if (e->obj && e->type && e->count > 0 && e->obj != s_List[0].obj &&
                    (SU_PlanHasPax(e->obj) || (sh && Up_PickHas(e->obj)))) pick = 1;
                Up_CellFrame(base, pitch, w, h, cx, cy, hot || pick);
            }
            /* 第二遍: 只补 9px 外框带 —— 它是最外一层, 不能被图标盖住,
             * 而它的内底又不许盖图标, 所以只能拆成两遍贴。 */
            if (s_BorderOK) Up_BlitBg(base, pitch, w, h, s_Px, s_Py, 1);
        }
        Up_DrawButtons(base, pitch, w, h);

        /* 页码: 小长方块直接压在下边框带上 (竖着居中), 不额外占行 */
        if (s_ListN > 0 && s_Pages > 1)
        {
            int bw = 6, bh = 3, bgap = 2;
            int total = s_Pages * bw + (s_Pages - 1) * bgap;
            int dx0 = s_Px + (s_Pw - total) / 2;
            int dy = s_Py + s_Ph - UP_PAD_B + (UP_PAD_B - bh) / 2;
            for (i = 0; i < s_Pages; i++)
            {
                int dx = dx0 + i * (bw + bgap);
                if (i == s_Page)
                    Up_Fill(base, pitch, w, h, dx, dy, bw, bh, 245, 225, 150, 255);
                else
                    Up_Fill(base, pitch, w, h, dx, dy, bw, bh, 150, 150, 160, 235);
            }
        }
    }
    GtUnlock(surf);

    if (!s_LoggedGeom && g_Log)
    {
        char buf[160];
        unsigned q = 0;
        s_LoggedGeom = 1;
        q = AppendCStr(buf, q, "up: geom p=", sizeof(buf));
        q += UtoA((unsigned)s_Px, buf + q); q = AppendCh(buf, q, ',', sizeof(buf));
        q += UtoA((unsigned)s_Py, buf + q); q = AppendCStr(buf, q, " ", sizeof(buf));
        q += UtoA((unsigned)s_Pw, buf + q); q = AppendCh(buf, q, 'x', sizeof(buf));
        q += UtoA((unsigned)s_Ph, buf + q); q = AppendCStr(buf, q, " cells=", sizeof(buf));
        q += UtoA((unsigned)(s_Cols * s_Rows), buf + q); q = AppendCStr(buf, q, " pages=", sizeof(buf));
        q += UtoA((unsigned)s_Pages, buf + q);
        LogLine(buf);
    }
    return 1;
}

/* ---------------- 载员多选 (shift + 点载员格) ---------------- */

/* 归属容器是不是当前这个单选目标。不是就必须清空: 指针会被引擎复用。 */
static int Up_PickHostOK(void)
{
    return (s_ListN == 1 && s_List[0].obj && s_List[0].obj == s_PickHost);
}

static void Up_PickClear(void)
{
    s_PickN = 0;
    s_PickHost = NULL;
}

static int Up_PickHas(void* o)
{
    int i;
    if (!o) return 0;
    for (i = 0; i < s_PickN; i++) if (s_Pick[i] == o) return 1;
    return 0;
}

/* 选中集换了容器 / 回到多选版式: 整组丢弃。每帧调一次。 */
static void Up_PickPrune(void)
{
    if (s_PickN <= 0) { Up_PickClear(); return; }
    if (!Up_PickHostOK()) Up_PickClear();
}

/* shift + 点一个载员格: 加入 / 移出多选。 */
static void Up_PickToggle(void* host, void* pax)
{
    int i, found = -1;

    if (!host || !pax) return;
    if (s_PickHost != host) { Up_PickClear(); s_PickHost = host; }
    for (i = 0; i < s_PickN; i++) if (s_Pick[i] == pax) { found = i; break; }
    if (found >= 0)
    {
        for (i = found; i + 1 < s_PickN; i++) s_Pick[i] = s_Pick[i + 1];
        s_PickN--;
    }
    else if (s_PickN < UP_PICK_MAX) s_Pick[s_PickN++] = pax;
}

/* ---------------- 输入 ---------------- */

static void Up_Select(const UpEnt* e)
{
    void* keep[UP_LIST_CAP];
    int n = 0, i;
    int cnt = e->count > 0 ? e->count : 1;

    if (!s_ListN) return;
    if (cnt > 1)
    {
        for (i = 0; i < s_ListN; i++)
            if (s_List[i].type == e->type) keep[n++] = s_List[i].obj;
    }
    else keep[n++] = e->obj;
    if (!n) return;

    ((Fn_UnselectAll)(size_t)FN_MAP_UNSELECTALL)();
    for (i = 0; i < n; i++)
    {
        Fn_Select f = (Fn_Select)(size_t)FN_TECHNO_SELECT;
        f(keep[i]);
    }
    Up_ForceRebuild();
    Up_Refresh();
}

static void Up_ClickAt(int mx, int my)
{
    int btn = Up_HitBtn(mx, my);
    int cell;
    if (btn == UP_BTN_FOLD)
    {
        s_Collapsed = s_Collapsed ? 0 : 1;
        s_HoverCell = -1;
        s_PendRb = 0;
        /* 折叠后看不到模式指示, 必须关掉; 否则 WASD 被吞却没有任何提示。 */
        if (s_Collapsed) { s_KeyMove = 0; s_KeyMoveSel = 0; Up_KmClear(); }
        LogLine(s_Collapsed ? "up: collapsed" : "up: expanded");
        return;
    }
    if (s_Collapsed) return;
    if (btn == UP_BTN_MODE)
    {
        s_KeyMove = s_KeyMove ? 0 : 1;
        s_KeyMoveSel = 0;
        Up_KmReset();
        LogLine(s_KeyMove ? "up: keymove on" : "up: keymove off");
        return;
    }
    if (btn >= UP_BTN_FAV1)
    {
        /* 收藏槽: Ctrl+左键 = 收藏当前选中集 (整组); 不按 Ctrl 时按下只记起点,
         * 长按清空 / 松开跳转都在 Favorite 的 PressPoll 里结算。 */
        void* sel[FAV_GROUP_MAX];
        int n = 0, i;
        int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0;
        for (i = 0; i < s_ListN && n < FAV_GROUP_MAX; i++)
            if (s_List[i].obj) sel[n++] = s_List[i].obj;
        Fav_PressBegin(btn - UP_BTN_FAV1, sel, n, ctrl);
        return;
    }
    cell = Up_HitCell(mx, my);
    if (cell < 0 || cell >= s_Cols * s_Rows) return;
    {
        const UpEnt* e = &s_Cell[cell];
        /* shift = 多选模式的显式开关: 名单只在按住 shift 时参与决策, 松开回到
         * "给这一个 / 给全部"的默认语义。 */
        int sh = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1 : 0;
        if (e->count == -2)   /* 更多: 翻页 */
        {
            s_Page = (s_Page + 1) % s_Pages;
            Up_Rebuild();
            return;
        }
        /* 单选版式第 0/1 格 = 本车/本建筑自己: Z 按住时点它 = 给全部载员规划。
         * 不对容器本身下令 —— 建筑载员时它本来就不能动, 载具可以直接在地图上点。
         * 按住 shift 且有多选名单时只给名单里的 (shift 选过就是明确指定)。
         * NoManualUnload 的单位不给规划: 那些载员是功能本体, 卸下来单位就废了,
         * 面板既然不列它们, 也不能留这条"给全部"的暗门。 */
        if (s_ListN == 1 && cell < 2 && e->obj && e->type &&
            !Up_NoManualUnload(e->type))
        {
            if (sh && Up_PickHostOK() && s_PickN > 0)
            {
                if (SU_PlanArmList(s_List[0].obj, s_Pick, s_PickN)) return;
            }
            if (SU_PlanArmAll(s_List[0].obj)) return;
        }
        /* 单选版式第 0 格是本车/本建筑、第 1 格是信息格, 第 2 格起才是载员
         * (分页时前两格每页都在)。载员在车内不可选, 所以这里绝不落到 Up_Select ——
         * 否则 UnselectAll 之后选不回来, 面板会整块消失。
         * shift = 多选 (只挂名单, 不下令也不下车); 不按 shift = 只给这一个
         * (Z 按住时开单人规划, 否则单卸)。 */
        if (s_ListN == 1 && cell >= 2)
        {
            if (!e->obj || !e->type || e->count <= 0) return;
            if (sh)
            {
                Up_PickToggle(s_List[0].obj, e->obj);
                /* 已经在给这辆车规划时, 名单变化要立刻反映到参与集合上 */
                SU_PlanResync(s_List[0].obj, s_Pick, s_PickN);
                return;
            }
            /* 不按 shift = 明确的单人语义: 先把多选名单收掉, 再走单卸/单人规划。
             * 不收的话下一次按住 shift 按 Z 又会把旧名单整组认下来。 */
            Up_PickClear();
            SU_ClickPassenger(s_List[0].obj, e->obj);
            return;
        }
        if (!e->obj || !e->type || e->count == 0) return;
        Up_Select(e);
    }
}

/* 右键落在星星按钮上: 切换指示箭头类型 (贴边 / 中心同心圆)。 */
static void Up_RightClickAt(int mx, int my)
{
    if (Up_HitBtn(mx, my) >= UP_BTN_FAV1) Fav_ToggleArrowMode();
}

static void UP_NoteClick(int mx, int my, int left, int right)
{
    if (!left) s_GateLb = 0;
    if (!right) s_GateRb = 0;
    if (left)
    {
        /* 一次按下可能多次进 ProcessClickCoords (引擎按帧重算落点), 只认按下沿,
         * 否则点一下折叠键会来回折十几次。 */
        if (s_GateLb) return;
        s_GateLb = 1;
        s_Pend = 1;
        s_PendX = mx;
        s_PendY = my;
        return;
    }
    if (right)
    {
        if (s_GateRb) return;
        s_GateRb = 1;
        s_PendRb = 1;
        s_PendRbX = mx;
        s_PendRbY = my;
    }
}

int UP_WheelIn(int mx, int my, int dir)
{
    UP_EnsureInit();
    if (!s_Enable || !s_Vis) return 0;
    if (!Up_HitPanel(mx, my)) return 0;
    if (s_Collapsed) return 1;
    if (s_Pages > 1)
    {
        int p = s_Page + (dir > 0 ? 1 : -1);
        if (p < 0) p = 0;
        if (p > s_Pages - 1) p = s_Pages - 1;
        if (p != s_Page) { s_Page = p; Up_Rebuild(); }
    }
    return 1;
}

void UP_Poll(void)
{
    int mx = -1, my = -1;
    UP_EnsureInit();
    /* 收藏槽先走一拍 (换图清空 / 成员剪枝 / 结算挂起的取消), 再取本帧点击;
     * 面板关掉时也要跑: 星星/箭头还在, 换图后不能让旧指针留着。 */
    Fav_Poll();
    /* 1=强制开启引擎自带的单位行动线 (右键移动时那条); 0=不改游戏内设置。 */
    if (s_ActionLines) *(volatile unsigned char*)(size_t)(ADDR_GAMEOPT + OFF_GO_ACTIONLINES) = 1;
    if (!s_Enable)
    {
        s_Vis = 0;
        s_KeyMove = 0;
        s_KeyMoveSel = 0;
        Up_KmClear();
        return;
    }
    s_Vis = (*(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED) ? 1 : 0;
    Up_KmMatchEdge(s_Vis, *(int*)ADDR_FRAME);
    if (!s_Vis)
    {
        s_Pend = 0;
        s_PendRb = 0;
        s_KeyMove = 0;
        s_KeyMoveSel = 0;
        Up_KmClear();
        return;
    }
    /* 先刷模型再排版: 格数决定面板宽高, 反过来会让几何滞后一帧 */
    Up_Refresh();
    Up_Layout();
    Up_PrepareIcons();
    /* 多选名单: 换了容器/换了版式就整组清掉 (载员指针会被引擎复用) */
    Up_PickPrune();
    /* 有 shift 名单时按 Z 就直接认下, 之后的地图点击都算名单的路径点。
     * 认人要早于第一次点击至少一拍, 垫点才排得进第一个节点前面。
     * 必须按住 shift: 否则名单一留下就没法切回"只给一个单位下路径点"。 */
    if (!SU_PlanActive() && (GetAsyncKeyState(VK_SHIFT) & 0x8000) &&
        Up_PickHostOK() && s_PickN > 0)
        SU_PlanAutoArm(s_List[0].obj, s_Pick, s_PickN);
    /* 载员规划: 落点格只能在这里取 (门内调 ProcessClickCoords 会递归)。
     * 本轮 SU_PlanPoll() 之前先记下规划状态: 真下过点才收名单。 */
    {
        int planWas = SU_PlanActive();        /* 本轮结算前规划是否在跑 (结束时收名单用) */
        int planFired = SU_PlanFiredCount();  /* 本轮结算前的节点轮数 (同上) */
        SU_PlanPoll();
        /* 这次规划真下过点才收名单: 只按一下 Z 又松开 (误触) 不该清掉。
         * 留着的话下次按 Z 又被它接管, 想给载具本身下点就下不成。 */
        if (planWas && !SU_PlanActive() && planFired > 0) Up_PickClear();
    }
    /* 松开时清门侧按下沿标记 (松手那一下未必再进门) */
    if (s_GateLb && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) s_GateLb = 0;
    if (s_GateRb && !(GetAsyncKeyState(VK_RBUTTON) & 0x8000)) s_GateRb = 0;

    if (s_Pend)
    {
        int px = s_PendX, py = s_PendY;
        s_Pend = 0;
        Up_ClickAt(px, py);
    }
    if (s_PendRb)
    {
        int px = s_PendRbX, py = s_PendRbY;
        s_PendRb = 0;
        Up_RightClickAt(px, py);
    }
    /* 收藏星星按钮的按住状态: 长按到时清空 / 松开跳转。
     * 每循环都要走 (按下沿只来一次, 松开未必再进点击门)。 */
    Fav_PressPoll();

    /* 模式刚在本帧被点开/关掉时也走这里: 开的那一拍会立即更新可动单位数。 */
    Up_KeyMovePoll();

    if (!GtMouseGameXY(&mx, &my)) { s_HoverCell = -1; s_HoverBtn = -1; return; }
    s_HoverBtn = Up_HitBtn(mx, my);
    s_HoverCell = (s_HoverBtn >= 0) ? -1 : Up_HitCell(mx, my);
}

/* ---------------- 点击门 (Syringe 0x692361, 6B) ---------------- */

extern "C" __declspec(dllexport) unsigned __cdecl UP_ClickGate(void* regs)
{
    int mx = -1, my = -1;
    int lb, rb;
    (void)regs;
    UP_EnsureInit();
    if (!s_Enable || !s_Vis) return 0;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    if (!Up_HitPanel(mx, my))
    {
        /* Z 按住的载员规划: 吞掉这次地图点击 —— 选中表里是载具, 不拦的话引擎
         * 会顺手给载具也加一个路径点。 */
        if (SU_PlanClick(mx, my)) return (unsigned)ADDR_CLICKGATE_SKIP;
        return 0;
    }
    lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    rb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
    /* 这里每帧被引擎重算落点调用多次, 不能写日志。 */
    if (lb || rb) UP_NoteClick(mx, my, lb, rb);
    /* 落在面板上就不许引擎再拿它当地图点击 (否则会取消选择/下令)。 */
    return (unsigned)ADDR_CLICKGATE_SKIP;
}

/* ---------------- 键盘门 (Syringe 0x55DEE0, 5B) ----------------
 * KeyboardProcess 入口, ECX = &输入 DWORD。只在"键盘移动开 + 有可动单位"时
 * 把裸 W/A/S/D 清零, 原函数继续跑 (查不到命令)。带修饰的组合键不碰
 * (Alt+A 结盟 / Ctrl+S 截图等仍走原版)。语音由原命令派发, 键被吞后自然不响。 */

extern "C" __declspec(dllexport) unsigned __cdecl UP_KeyGate(void* regs)
{
    void* p;
    unsigned v;

    if (!s_KeyMove || s_KeyMoveSel <= 0) return 0;
    if (!*(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED) return 0;
    if (*(volatile int*)(size_t)ADDR_SPECIAL_DIALOG) return 0;
    if (Up_ChatTyping()) return 0;
    if (!regs) return 0;
    p = *(void**)((char*)regs + 0x20);   /* Syringe: +0x20 = 入口 ECX */
    if (!p || !IsPlausiblePtr(p) || ((size_t)p & 3u)) return 0;
    v = *(unsigned*)p;
    if (v & 0x700u) return 0;            /* Shift/Ctrl/Alt: 留给原版组合键 */
    switch (v & 0xFFu)
    {
    case 'W':
    case 'A':
    case 'S':
    case 'D':
        *(unsigned*)p = 0;
        break;
    default:
        break;
    }
    return 0;
}
