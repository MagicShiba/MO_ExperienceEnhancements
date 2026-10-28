/* PiP.cpp - 画中画 (第二摄像机小窗)。
 * 侧栏按钮开关; 离屏跑引擎 TacticalClass::Render (mode 0/1/2) 后降采样贴回 Composite。
 * 只写渲染面和本地摄像机状态, 不改模拟状态。原理见 PiP.h / notes/画中画.md。
 * 引擎地址/偏移全部在 EngineAddr.h, 本文件不写本地 #define。
 */

#include <windows.h>
#include "GdiText.h"
#include "FullMap.h"
#include "MsgText.h"
#include "ScaleBar.h"
#include "PcxImage.h"
#include "SidebarSkin.h"
#include "OrderUtil.h"      /* OU_ObjId / OU_PtrAliveRaw: 跟随目标跨帧验活 */
#include "PiP.h"

#define PIP_SECTION "PiP"

typedef void (__attribute__((thiscall)) *Fn_Render)(void* self, void* surf, int flag, int mode);
typedef void (__attribute__((thiscall)) *Fn_MakeSel)(void* self, void* filter);

/* 需要快照/原样还原的 TacticalClass 相机字段, 平铺成 12 个 int (顺序即读写顺序)。
 * F7C/F7D 是两个相邻的 1 字节标记, 按 int 读写会互相覆盖 —— 读写顺序固定为
 * 先 F7C 后 F7D, 别拆开也别调序。 */
static const unsigned short kCamOff[] = {
    OFF_TAC_TACX, OFF_TAC_TACY,   /* 0-1 视口原点 */
    OFF_TAC_LSTX, OFF_TAC_LSTY,   /* 2-3 上次视口原点 (位移=0 => 整幅重画) */
    OFF_TAC_CURX, OFF_TAC_CURY,   /* 4-5 相机中心 */
    OFF_TAC_PRX,  OFF_TAC_PRY,    /* 6-7 */
    OFF_TAC_TGX,  OFF_TAC_TGY,    /* 8-9 */
    OFF_TAC_F7C,  OFF_TAC_F7D,    /* 10-11 缩放档标记 (各 1 字节) */
};
#define PIP_CAM_N ((int)(sizeof(kCamOff) / sizeof(kCamOff[0])))

/* 相机 / 视口 / 缓存状态快照: PiP 临时改完必须原样还原。 */
typedef struct {
    void* tac;
    int v[PIP_CAM_N];           /* 顺序同 kCamOff */
    RectangleStruct vb;
    int cache[4];               /* ViewBounds 缓存副本 X/Y/W/H */
} PiP_CamSave;

/* ---------------- 配置 / 状态 ---------------- */

static int g_Init = 0;
static int g_Enable = 0;      /* [PiP] Enable: 模块总开关 (含侧栏按钮) */
static int g_Show = 0;        /* 小窗是否打开 (默认关, 由侧栏按钮切换) */
static int g_DispW = 288, g_DispH = 192;   /* 小窗显示尺寸 */
static int g_ScaleNum = 2;    /* 离屏渲染尺寸 = 显示尺寸 * ScaleNum (1 或 2) */
static int g_Interval = 1;    /* 每 N 帧刷一次第二摄像机 */
static int g_PadX = 8, g_PadY = 8;   /* 小窗在地图区内的落点 (右上角) */
static int g_Border = 1;
static int g_AutoJump = 0;    /* [PiP] AutoJump: 自动跟随最新雷达事件 (窗内按钮) */
static int g_Operate  = 0;    /* [PiP] Operate: 窗内左键操作 (窗内按钮) */
static int g_FollowCfg = 1;   /* [PiP] Follow: 小窗跟随 (F) 总开关 */

static int   g_InRender = 0;
static void* g_Comp = 0;      /* 离屏"合成"面 (最终画面) */
static void* g_Map = 0;       /* 离屏"地图"面 (地形趟的暂存) */
static void* g_Z = 0;
static void* g_A = 0;
static int   g_RW = 0, g_RH = 0;
static int   g_Fail = 0;
static int   g_FailFrame = 0;     /* 上次离屏面分配失败的帧号 (过一会儿重试) */
static int   g_LastFrame = -1;
static int   g_HasContent = 0;    /* g_Comp 里有可贴的有效画面 */
static int   g_SurfJustBuilt = 0;
static int   g_ForceDue = 0;      /* 开窗后补渲几帧 (首帧可能还没地形) */

/* ---- 聚焦 / 小地图定位 ---- */
static int g_Focus = 0;        /* 小窗是否被点中 (点小窗/小地图之外即失焦) */
static int g_Pinned = 0;       /* 目标是否已定 (开窗首帧取主相机中心, 之后固定) */
static int g_PinX = 0, g_PinY = 0;
static int g_LastMainX = 0, g_LastMainY = 0, g_HaveLastMain = 0;
static int g_RadarSteal = 0;   /* 相机定位钩标记: 本帧有一次程序化相机跳转 */
static int g_PipX = 0, g_PipY = 0, g_PipW = 0, g_PipH = 0;  /* 小窗物理游戏坐标 */
/* 用户拖出来的落点 (屏幕绝对坐标), -1 = 还没放过 (按右上角默认落点算)。
 * 存绝对值而非"相对视口右上角的偏移": 侧栏一收/一放视口右边界就变 (1752 <-> 1920),
 * 跟着锚会把窗整个甩到新边界上 —— 只留小地图时正好盖住小地图。 */
static int s_WantX = -1, s_WantY = -1;
static int g_Drag = 0, g_DragGX = 0, g_DragGY = 0;
static int g_Pan = 0;                     /* 窗内右键拖动中 (含按下但还没过阈值) */
static int g_PanBaseX = 0, g_PanBaseY = 0;/* 右键拖动期间被钉住的主相机位置 */
static int g_PanMX = 0, g_PanMY = 0;      /* 右键按下点 (物理坐标) */
static int g_PanRate = 8;                 /* [PiP] PanRate: 每帧推进 = 距离 * 该值 / 6 (与游戏同式) */
static int g_PanMax = 6;                  /* [PiP] PanMax: 每帧推进上限 = PanMax * PanRate 屏幕像素 */
static int s_RbHeld = 0;                  /* 右键上一帧是否按住 (按下沿判定) */

/* 窗内左键框选 (仅操作模式)。按下点/当前点用物理游戏坐标, 帧末或松开时换算成
 * 小窗离屏视口坐标, 再借引擎 MakeSelection 按小窗可选表选人。 */
static int g_SelDrag = 0;                  /* 窗内左键拖动中 (含未过阈值) */
static int g_SelStartGX = 0, g_SelStartGY = 0;
static int g_SelCurGX = 0, g_SelCurGY = 0;
static int g_Frame = 0;        /* 本工程帧号: 相机钩与帧末判定共用同一值 */
static int g_PendingUnfocus = 0;/* 本帧按下点在小窗外(且非小地图) → 帧末失焦 */
static int g_RawCX = 0, g_RawCY = 0;   /* 小地图点击的原始(未钳制)格 */
static int g_RawAt = -1000;    /* 最近一次拿到原始格的帧号 */
static int g_CamWX = 0, g_CamWY = 0, g_CamWZ = 0, g_CamW = 0; /* 本帧程序化相机跳转的精确世界坐标 */
static unsigned g_AutoSeq = 0; /* 已跟随的雷达事件序号 */
static int g_AutoSeen = 0;     /* g_AutoSeq 是否有意义 (开窗/开按钮后先跳一次最新事件) */
static int g_LastRtX = 0x7fffffff, g_LastRtY = 0x7fffffff;  /* 上次记日志的目标 */

/* ---- 小窗跟随 (F) ----
 * 引擎的跟随挂在选中集上: 换选择/取消选中/下别的令都会把它清掉, 主窗口当场停下。
 * 小窗跟随时独立状态, 定下目标后与选中集无关, 只有用户明确"要看别处"才取消。 */
static int   g_FolOn = 0;      /* 小窗跟随中 */
static void* g_FolObj = 0;     /* 目标 ObjectClass* (跨帧持有, 每帧验活) */
static int   g_FolId = 0;      /* 目标 Fetch_ID (验活用) */
static int   g_FolIdxOk = 0;   /* Fetch_ID 索引自检通过 (否则只靠字节判据) */

/* 定义在后面 (需要 PiP_IsGameCode / PiP_ClampScreen 等), 这里先声明给前面的调用点。 */
static void PiP_FollowClear(const char* why);
static void PiP_FollowTick(void);

/* 侧栏按钮坐标。[PiP] ButtonPos 依次是盟军/苏联/厄普西隆/焚风的 X,Y:
 * X 相对侧栏列左缘 (= 侧栏面局部 x), Y 相对屏幕顶边 (= 侧栏面局部 y)。 */
static int g_BtnPos[4][2] = { { 8, 155 }, { 25, 158 }, { 0, 160 }, { 0, 168 } };
#define PIP_BTN_W     20
#define PIP_BTN_H     20
#define PIP_ICON_PAD  3      /* 移动图标到小窗右上角的内边距 */
#define PIP_ICON_W    13     /* 窗内按钮尺寸 */
#define PIP_ICON_H    13
#define PIP_ICON_GAP  2      /* 窗内按钮间距 (从右到左: 移动 / 自动跟随 / 操作) */
#define PIP_ICON_N    3

/* 16bpp 颜色 (RGB565)。 */
#define PIP_C_WHITE   0xFFFF
#define PIP_C_GREEN   0x07E0
#define PIP_C_GRAY    0x8410
#define PIP_C_AMBER   0xFFE0

/* 游戏右键拖动模型: 按下点附近 8 物理像素内不算拖动 (松开=右键取消),
 * 越过阈值后按 8 方向量化, 速度 = 距离(mouse, 按下点) * PanRate / 6 屏幕像素/帧。 */
#define PIP_PAN_DRAG  8      /* 拖动判定阈值 (物理像素) */
#define PIP_PAN_DIV   6      /* 距离 → 速度 的除数 (与游戏一致) */
#define PIP_SEL_DRAG  4      /* 左键框选判定阈值 (物理像素, 与引擎 0x4AC380 同) */

/* 可选/可见单元表快照 (最多各留这么多项)。脏区表只存项数即可:
 * 渲染只会在 count>0 时"删"末项, 提前把 count 清 0 就一项不动。 */
#define PIP_SEL_ENT   12
#define PIP_SEL_MAX   500     /* 引擎 AddSelectable 上限 0x1F4; 整表保存/还原 */
#define PIP_VIS_MAX   800
#define PIP_FAIL_RETRY 120    /* 离屏面分配失败后隔多少帧再试 (别每帧刷日志) */

static unsigned char* g_ScratchSel = 0;
static unsigned char* g_ScratchVis = 0;
static unsigned char* g_ScratchPipSel = 0;   /* 小窗渲染产出的可选物件表快照 */
static int g_PipSelN = 0;
static int g_ScratchOk = 0;

/* ---------------- 小工具 ---------------- */

/* 模块没开 / 离屏面刚失败 (小窗一律不动作; AfterMap 的重试路径自己判)。 */
static int PiP_Off(void) { return !g_Enable || g_Fail; }

/* 有效的 TacticalClass::Instance (0 = 还没有 / 已失效)。 */
static void* PiP_Tac(void)
{
    void* t = *(void**)ADDR_TACTICAL;
    return (t && IsPlausiblePtr(t)) ? t : 0;
}

static int PiP_PointIn(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* 物理点是否落在小窗矩形内 (小窗已开且尺寸有效)。 */
static int PiP_InWindow(int gx, int gy)
{
    return g_Show && g_PipW > 0 &&
           PiP_PointIn(gx, gy, g_PipX, g_PipY, g_PipW, g_PipH);
}

static int PiP_Abs(int v) { return v < 0 ? -v : v; }

/* 拖框判定用的距离 = max(|dx|, |dy|) (与引擎 0x4AC380 同)。 */
static int PiP_DragDist(int dx, int dy)
{
    int ax = PiP_Abs(dx), ay = PiP_Abs(dy);
    return ax > ay ? ax : ay;
}

/* 右键拖动距离 (八方向近似: max + min/2)。 */
static int PiP_PanDist(int dx, int dy)
{
    int ax = PiP_Abs(dx), ay = PiP_Abs(dy);
    return (ax > ay ? ax : ay) + (ax < ay ? ax : ay) / 2;
}

/* 表项数钳到快照缓冲上限 (负数当 0)。 */
static int PiP_ClampCount(int n, int cap)
{
    if (n < 0) return 0;
    return n > cap ? cap : n;
}

static int PiP_KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0; }

static void PiP_Order2(int* a, int* b)
{
    if (*a > *b) { int t = *a; *a = *b; *b = t; }
}

/* 逐字节搬运。本工程无 CRT, memcpy 是 volatile 字节循环 (比这里慢), 所以自己写:
 * 非 volatile 的循环 -O2 会展开/向量化。只走点击/开窗这类低频路径 (上限 ~6KB)。 */
static void PiP_CopyBytes(void* dst, const void* src, int n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    int i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

static int PiP_Scratch(void)
{
    unsigned need = PIP_SEL_ENT * PIP_SEL_MAX * 2 + 4 * PIP_VIS_MAX;
    if (g_ScratchOk) return 1;
    g_ScratchSel = (unsigned char*)VirtualAlloc(0, need, MEM_COMMIT | MEM_RESERVE,
                                                PAGE_READWRITE);
    if (!g_ScratchSel) return 0;
    g_ScratchVis = g_ScratchSel + PIP_SEL_ENT * PIP_SEL_MAX;
    g_ScratchPipSel = g_ScratchVis + 4 * PIP_VIS_MAX;
    g_ScratchOk = 1;
    return 1;
}

/* 逗号/分号/空白分隔的整数表。返回读出的个数。 */
static int PiP_ParseInts(const char* s, int* out, int cap)
{
    int i = 0, n = 0;
    while (s[i] && n < cap)
    {
        int sign = 1, v = 0;
        while (s[i] == ',' || s[i] == ';' || s[i] == ' ' || s[i] == '\t') i++;
        if (!s[i]) break;
        if (s[i] == '-') { sign = -1; i++; }
        if (s[i] < '0' || s[i] > '9') { i++; continue; }
        while (s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
        out[n++] = sign * v;
    }
    return n;
}

/* 读 [PiP] 的一个整数键 (不钳制)。 */
static int PiP_IniInt(const char* key, int def)
{
    return GetPrivateProfileIntA(PIP_SECTION, key, def, g_IniPath);
}

/* 读 [PiP] 的一个整数键并钳到 [lo, hi]。 */
static int PiP_IniRange(const char* key, int def, int lo, int hi)
{
    int v = PiP_IniInt(key, def);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

/* 读 [PiP] 的开关 (非 0 即开, 含负数)。 */
static int PiP_IniBool(const char* key, int def)
{
    return PiP_IniInt(key, def) ? 1 : 0;
}

void PiP_EnsureInit(void)
{
    char raw[128];
    int pos[8], v;
    if (g_Init) return;
    g_Init = 1;
    GtEnsureLog();
    g_Enable   = PiP_IniBool("Enable", 1);
    g_DispW    = PiP_IniRange("Width", 288, 64, 1024);
    g_DispH    = PiP_IniRange("Height", 192, 48, 1024);
    g_ScaleNum = (PiP_IniInt("Scale", 2) == 1) ? 1 : 2;   /* 只认 1, 其余按 2 */
    g_Interval = PiP_IniRange("Interval", 1, 1, 60);
    g_PadX     = PiP_IniInt("PadX", 8);
    g_PadY     = PiP_IniInt("PadY", 8);
    g_Border   = PiP_IniBool("Border", 1);
    g_PanRate  = PiP_IniRange("PanRate", 8, 2, 64);
    g_PanMax   = PiP_IniRange("PanMax", 6, 1, 32);
    g_AutoJump = PiP_IniBool("AutoJump", 0);
    g_Operate  = PiP_IniBool("Operate", 0);
    g_FollowCfg = PiP_IniBool("Follow", 1);
    GetPrivateProfileStringA(PIP_SECTION, "ButtonPos",
                             "8,155,25,158,0,160,0,168", raw, sizeof(raw), g_IniPath);
    if (PiP_ParseInts(raw, pos, 8) == 8)
        for (v = 0; v < 4; v++)
        {
            g_BtnPos[v][0] = pos[v * 2];
            g_BtnPos[v][1] = pos[v * 2 + 1];
        }
    if (g_Enable) LogLine("pip: module ON (sidebar button)");
    else LogLine("pip: module OFF");
}

int PiP_InRender(void) { return g_InRender; }

/* ---------------- 1px 像素绘制 (在已锁定的 16bpp 面上直接写) ---------------- */

static void PiP_PxPut(unsigned short* db, int dp, int cw, int ch, int x, int y,
                      unsigned short c)
{
    if (x < 0 || y < 0 || x >= cw || y >= ch) return;
    *(unsigned short*)((unsigned char*)db + (size_t)y * dp + (size_t)x * 2) = c;
}

static void PiP_PxFrame(unsigned short* db, int dp, int cw, int ch,
                        int x, int y, int w, int h, unsigned short c)
{
    int i;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < w; i++) { PiP_PxPut(db, dp, cw, ch, x + i, y, c);
                              PiP_PxPut(db, dp, cw, ch, x + i, y + h - 1, c); }
    for (i = 0; i < h; i++) { PiP_PxPut(db, dp, cw, ch, x, y + i, c);
                              PiP_PxPut(db, dp, cw, ch, x + w - 1, y + i, c); }
}

static int PiP_BtnSide(void)
{
    int side = SidebarSkin_Side();
    return (side >= 0 && side <= 3) ? side : 0;
}

/* 侧栏开关按钮在屏幕上的矩形 (物理游戏坐标)。0 = 拿不到侧栏位置。 */
static int PiP_BtnRect(int* x, int* y, int* w, int* h)
{
    int side = PiP_BtnSide();
    int colX = FM_SidebarColumnX();
    if (colX < 0) return 0;
    *x = colX + g_BtnPos[side][0]; *y = g_BtnPos[side][1];
    *w = PIP_BTN_W; *h = PIP_BTN_H;
    return 1;
}

/* 贴当前阵营的 btn_small_window 格 (20x20); hover 时叠白提亮。 */
static void PiP_DrawBtnImg(unsigned short* db, int dp, int cw, int ch,
                           int side, int x, int y, int hover)
{
    PcxImage img;
    int cellW, cellH, cx, cy;
    if (!PcxImage_Load(&img, "btn_small_window.pcx", 2)) return;
    cellW = img.width / 4;
    cellH = img.height;
    if (cellW <= 0 || cellH <= 0 || side * cellW + cellW > img.width) return;
    for (cy = 0; cy < cellH; cy++)
    {
        int ty = y + cy;
        const unsigned short* srow =
            (const unsigned short*)img.px + (size_t)cy * img.width + side * cellW;
        unsigned short* drow;
        if (ty < 0 || ty >= ch) continue;
        drow = (unsigned short*)((unsigned char*)db + (size_t)ty * dp);
        for (cx = 0; cx < cellW; cx++)
        {
            int tx = x + cx;
            unsigned short c;
            if (tx < 0 || tx >= cw) continue;
            c = srow[cx];
            if (c == img.key) continue;
            if (hover) c = Blend16(c, 255, 255, 255, 64);
            drow[tx] = c;
        }
    }
}

/* 窗内按钮的左上角: slot 0=移动 (最右, 按住拖动小窗), 1=自动跟随, 2=操作。
 * 从右上角往左依次排开; 尺寸恒为 PIP_ICON_W x PIP_ICON_H。 */
static void PiP_IconAt(int pipX, int pipY, int pipW, int slot, int* ix, int* iy)
{
    *ix = pipX + pipW - PIP_ICON_PAD - PIP_ICON_W - slot * (PIP_ICON_W + PIP_ICON_GAP);
    *iy = pipY + PIP_ICON_PAD;
}

/* 命中窗内哪个按钮: 0=无, 1=移动, 2=自动跟随, 3=操作。 */
static int PiP_HitIcon(int gx, int gy)
{
    int slot;
    if (g_PipW <= 0) return 0;
    for (slot = 0; slot < PIP_ICON_N; slot++)
    {
        int x, y;
        PiP_IconAt(g_PipX, g_PipY, g_PipW, slot, &x, &y);
        if (PiP_PointIn(gx, gy, x, y, PIP_ICON_W, PIP_ICON_H)) return slot + 1;
    }
    return 0;
}

/* ---------------- 离屏面/缓冲 ---------------- */

static void PiP_DropSurfs(void)
{
    if (g_Comp) { SB_FreeOffscreen(g_Comp); g_Comp = 0; }
    if (g_Map) { SB_FreeOffscreen(g_Map); g_Map = 0; }
    if (g_Z || g_A) { SB_FreeBufPair(g_Z, g_A); g_Z = 0; g_A = 0; }
    g_RW = g_RH = 0;
    g_PipSelN = 0;
}

/* 分配失败: 先关掉 PiP 一段时间, 过后自动重试 (瞬时 OOM / 分辨率切换)。 */
static void PiP_MarkFail(const char* msg)
{
    static int s_Logged = 0;
    if (!s_Logged) { s_Logged = 1; LogLine(msg); }
    g_Fail = 1;
    g_FailFrame = g_Frame;
}

static int PiP_EnsureSurfs(int rw, int rh)
{
    if (g_Comp && g_Map && g_Z && g_A && g_RW == rw && g_RH == rh) return 1;
    PiP_DropSurfs();
    g_Comp = SB_NewOffscreen(rw, rh);
    g_Map = SB_NewOffscreen(rw, rh);
    if (!g_Comp || !g_Map || !SB_NewBufPair(&g_Z, &g_A, rw, rh))
    {
        PiP_MarkFail((g_Comp && g_Map) ? "pip: zbuf/abuf FAILED (retry later)"
                                       : "pip: offscreen surface FAILED (retry later)");
        PiP_DropSurfs();
        return 0;
    }
    SB_ClearOffscreen(g_Comp);
    SB_ClearOffscreen(g_Map);
    g_RW = rw; g_RH = rh;
    g_SurfJustBuilt = 1;
    g_HasContent = 0;
    LogLine("pip: offscreen ON");
    return 1;
}

/* 开/关小窗 (侧栏按钮)。打开时落回右上角, 且目标重取"打开时的主视图中心"。 */
static void PiP_Toggle(void)
{
    g_Show = !g_Show;
    g_Focus = g_Drag = g_Pan = g_PendingUnfocus = g_HasContent = 0;
    if (!g_Show)
    {
        PiP_FollowClear("pip: follow OFF (window closed)");
        PiP_DropSurfs();
        g_PipX = g_PipY = g_PipW = g_PipH = 0;
        LogLine("pip: window OFF (sidebar button)");
        return;
    }
    s_WantX = -1; s_WantY = -1;    /* 落回默认右上角 */
    g_Pinned = 0;                  /* 目标 = 当前主相机中心 (渲染时取) */
    g_SurfJustBuilt = 0;
    g_ForceDue = 4;                /* 首帧地形/缓存可能未就绪, 连续补渲 */
    g_LastFrame = -1;
    g_AutoSeen = 0;                /* 自动跟随已开: 开窗就跳到当前最新事件 */
    g_CamW = 0;
    g_Fail = 0;                    /* 用户主动开窗 = 立刻重试离屏面 */
    LogLine("pip: window ON (sidebar button)");
}

/* ---------------- 相机 / 视口状态快照 ---------------- */

/* ViewBounds 缓存副本: 4 个连续 int (X, Y, W, H) @0xB0CE28。put=1 = 写回。 */
static void PiP_CacheXfer(int* v, int put)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        int* p = (int*)(size_t)(ADDR_VIEWCACHE + 4u * (unsigned)i);
        if (put) *p = v[i]; else v[i] = *p;
    }
}

/* 缓存副本里的视口宽高 (主相机钉位用, 与引擎 0x6D6070 读的同一对)。 */
static int PiP_CacheW(void) { return *(int*)(size_t)(ADDR_VIEWCACHE + 8); }
static int PiP_CacheH(void) { return *(int*)(size_t)(ADDR_VIEWCACHE + 12); }

static void PiP_CamSnapshot(PiP_CamSave* s, void* tac)
{
    int i;
    s->tac = tac;
    for (i = 0; i < PIP_CAM_N; i++) s->v[i] = *(int*)((char*)tac + kCamOff[i]);
    s->vb = *(RectangleStruct*)(size_t)ADDR_VIEWBOUNDS;
    PiP_CacheXfer(s->cache, 0);
}

static void PiP_CamRestore(const PiP_CamSave* s)
{
    int i;
    *(RectangleStruct*)(size_t)ADDR_VIEWBOUNDS = s->vb;
    PiP_CacheXfer((int*)s->cache, 1);
    if (!s->tac || !IsPlausiblePtr(s->tac)) return;
    for (i = 0; i < PIP_CAM_N; i++) *(int*)((char*)s->tac + kCamOff[i]) = s->v[i];
}

/* 视口 / 缓存改成离屏面的 (0,0,w,h)。 */
static void PiP_CamSetView(int w, int h)
{
    int cache[4] = { 0, 0, w, h };
    RectangleStruct vb = { 0, 0, w, h };
    *(RectangleStruct*)(size_t)ADDR_VIEWBOUNDS = vb;
    PiP_CacheXfer(cache, 1);
}

/* 把相机中心钉到屏幕坐标 (x,y), 同步派生字段 (与引擎 0x6D6070 同式)。
 * 前两对 (索引 0-3) 是视口原点 (= 中心 - 半视口), 其余是相机中心。 */
static void PiP_CamSetTarget(void* tac, int x, int y, int w, int h)
{
    const int ox = x - w / 2, oy = y - h / 2;
    int i;
    for (i = 0; i < PIP_CAM_N - 2; i++)   /* 末尾两个是缩放标记, 不动 */
        *(int*)((char*)tac + kCamOff[i]) = (i < 4) ? (i & 1 ? oy : ox) : (i & 1 ? y : x);
}

/* 清零缩放档相关标记 (离屏渲染必须从"无缩放"开始)。 */
static void PiP_CamZeroZoom(void* tac)
{
    *(int*)((char*)tac + OFF_TAC_F7C) = *(int*)((char*)tac + OFF_TAC_F7D) = 0;
}

/* ---------------- 离屏渲染期间要换掉的全局面 / 缓冲槽 ---------------- */

enum { PIP_S_COMP = 0, PIP_S_MAP, PIP_S_CUR, PIP_S_Z, PIP_S_A, PIP_S_N };
static const unsigned kSurfAddr[PIP_S_N] = {
    ADDR_SURF_COMPOSITE, ADDR_SURF_MAP, ADDR_SURF_TEMP, ADDR_TAC_ZBUF, ADDR_TAC_ABUF,
};

static void* PiP_SurfGet(int slot) { return *(void**)(size_t)kSurfAddr[slot]; }
static void  PiP_SurfSet(int slot, void* p) { *(void**)(size_t)kSurfAddr[slot] = p; }

/* ---------------- 第二摄像机渲染 ---------------- */

/* 用 PiP 摄像机跑一遍引擎的三趟 TacticalClass::Render (mode 0 准备 / 1 地形 / 2 覆盖+物件),
 * 画面落在 g_Comp。全程保存/还原主摄像机全局, 退出后主视图不受影响。 */
static int PiP_RenderCamera(int tx, int ty)
{
    void* tac = PiP_Tac();
    Fn_Render fn = (Fn_Render)(size_t)ADDR_TAC_RENDER;
    int RW = g_RW, RH = g_RH;
    PiP_CamSave cam;
    void *s_surf[PIP_S_N], *pip_surf[PIP_S_N];
    int s_dirtycnt, s_fr, sel_n, vis_n, i;

    if (!tac || !PiP_Scratch()) return 0;

    /* 脏区表: 只记项数 (提前清 0, 渲染就不会删任何项)。 */
    s_dirtycnt = *(int*)(size_t)ADDR_DIRTY_COUNT;
    /* 可选物件表 (12B/项) + 可见单元表 (4B/项)。计数钳到缓冲上限,
     * 免得还原时计数指向没被还原的项。 */
    sel_n = PiP_ClampCount(*(int*)((char*)tac + OFF_TAC_SELCNT), PIP_SEL_MAX);
    vis_n = PiP_ClampCount(*(int*)((char*)tac + OFF_TAC_VISCNT), PIP_VIS_MAX);
    PiP_CopyBytes(g_ScratchSel, (const void*)(size_t)ADDR_TACTICAL_SELARR,
                  sel_n * PIP_SEL_ENT);
    PiP_CopyBytes(g_ScratchVis, (const void*)((char*)tac + OFF_TAC_VISARR), vis_n * 4);

    /* 主摄像机 / 面 / 缓冲 / 视口缓存 */
    PiP_CamSnapshot(&cam, tac);
    for (i = 0; i < PIP_S_N; i++) s_surf[i] = PiP_SurfGet(i);
    s_fr = *(int*)(size_t)ADDR_FULLREDRAW;

    /* 摆 PiP 摄像机 */
    PiP_CamSetView(RW, RH);
    PiP_CamSetTarget(tac, tx, ty, RW, RH);
    PiP_CamZeroZoom(tac);

    /* 面 / 缓冲: 地形画进 g_Map, 合成与物件画进 g_Comp。Composite 也指过去,
     * 兜住个别 UI 覆盖层直接拿 Composite 当目标的写法 (反正随后原样还原)。 */
    pip_surf[PIP_S_COMP] = pip_surf[PIP_S_CUR] = g_Comp;
    pip_surf[PIP_S_MAP] = g_Map; pip_surf[PIP_S_Z] = g_Z; pip_surf[PIP_S_A] = g_A;
    for (i = 0; i < PIP_S_N; i++) PiP_SurfSet(i, pip_surf[i]);
    /* 清空共享脏区表与两张表, 免得把主视图残留的脏矩形/屏坐标按 PiP 坐标画错
     * (计数与内容随后还原)。 */
    *(int*)((char*)tac + OFF_TAC_SELCNT) = *(int*)((char*)tac + OFF_TAC_VISCNT) = 0;
    *(int*)(size_t)ADDR_DIRTY_COUNT = 0;

    g_InRender = 1;
    /* 整幅重画标志: 引擎 GScreenClass::Render 每帧先经 mode 0 趟消费它, 这里补上。
     * 趟序列照抄引擎 (mode 0/1/2), flag=1 = 强制整幅, 绕开脏区增量早退。 */
    *(int*)(size_t)ADDR_FULLREDRAW = 1;
    for (i = 0; i <= 2; i++)
        fn(tac, g_Comp, /*flag=*/1, /*mode=*/i);
    g_InRender = 0;

    /* 小窗这次渲染同时重建了"可选物件表"(物件屏坐标)。快照下来: 操作模式点击
     * 重映射时临时装回去 —— ProcessClickCoords (0x6DA380) 正是靠这张表拾取物件,
     * 而表里的坐标是小窗视口坐标, 与重映射后的点击点同系。 */
    g_PipSelN = PiP_ClampCount(*(int*)((char*)tac + OFF_TAC_SELCNT), PIP_SEL_MAX);
    PiP_CopyBytes(g_ScratchPipSel, (const void*)(size_t)ADDR_TACTICAL_SELARR,
                  g_PipSelN * PIP_SEL_ENT);

    /* 还原主状态 (面 / 缓冲 / 视口 / 摄像机 / 各表) */
    for (i = 0; i < PIP_S_N; i++) PiP_SurfSet(i, s_surf[i]);
    PiP_CamRestore(&cam);
    *(int*)(size_t)ADDR_FULLREDRAW = s_fr;
    *(int*)(size_t)ADDR_DIRTY_COUNT = s_dirtycnt;
    *(int*)((char*)tac + OFF_TAC_SELCNT) = sel_n;
    PiP_CopyBytes((void*)(size_t)ADDR_TACTICAL_SELARR, g_ScratchSel,
                  sel_n * PIP_SEL_ENT);
    *(int*)((char*)tac + OFF_TAC_VISCNT) = vis_n;
    PiP_CopyBytes((void*)((char*)tac + OFF_TAC_VISARR), g_ScratchVis, vis_n * 4);
    return 1;
}

/* ---------------- 视野定位 ---------------- */

/* 防出界: 与缩放档同一套算法, 用 SB_MapPx 把小窗视口整个塞进地图矩形,
 * 视口比地图大时钉在地图正中。 */
static void PiP_ClampScreen(int* tx, int* ty)
{
    int ml, mt, mw, mh, vw, vh, mn, mx, mid;
    if (!SB_MapPx(&ml, &mt, &mw, &mh)) return;
    vw = g_RW; vh = g_RH;
    if (vw <= 16 || vh <= 16) return;
    /* Y 先夹, 再夹 X (与缩放档同序)。 */
    mid = mt + mh / 2;
    if (mh > vh)
    {
        mn = mt + vh / 2; mx = mn + mh - vh;
        if (*ty < mn) *ty = mn; else if (*ty > mx) *ty = mx;
    }
    else *ty = mid;
    mid = ml + mw / 2;
    if (mw > vw)
    {
        mn = ml + vw / 2; mx = mn + mw - vw;
        if (*tx < mn) *tx = mn; else if (*tx > mx) *tx = mx;
    }
    else *tx = mid;
}

/* 与 0x6D6070 相同的 256 取整: (v + (v<0 ? 255 : 0)) >> 8 (向下取整)。 */
static int PiP_Div256(int v)
{
    return (v + ((v >> 31) & 255)) >> 8;
}

/* 地图格中心 → 小窗摄像机屏幕坐标 (与引擎 0x6D6070 同式)。 */
static void PiP_CellToScreen(int cx, int cy, int* tx, int* ty)
{
    *tx = 30 * (cx - cy);
    *ty = 15 * (cx + cy) + 15 -
          MsgText_ZToScreen(MsgText_CellZ((short)cx, (short)cy));
}

/* 按 8 方向 + 速度 (屏幕像素/帧) 推进小窗视野, 防出界钳死。 */
static void PiP_PanStep(int dx, int dy, int speed)
{
    int tx = g_PinX + dx * speed * g_ScaleNum;
    int ty = g_PinY + dy * speed * g_ScaleNum;
    PiP_ClampScreen(&tx, &ty);
    if (tx == g_PinX && ty == g_PinY) return;
    g_PinX = tx; g_PinY = ty; g_Pinned = 1;
}

/* 按位移矢量推进小窗视野: 8 方向量化, 速度 = 距离 * PanRate / 6 (越远越快),
 * 钳在 [1, PanMax * PanRate]。 */
static void PiP_PanBy(int ox, int oy)
{
    int ax = PiP_Abs(ox), ay = PiP_Abs(oy);
    int sx = ox > 0 ? 1 : (ox < 0 ? -1 : 0);
    int sy = oy > 0 ? 1 : (oy < 0 ? -1 : 0);
    int speed = PiP_PanDist(ox, oy) * g_PanRate / PIP_PAN_DIV;
    /* 手动拖视野 = "我要看别处", 跟随让路 (否则下一帧就被拉回目标)。 */
    PiP_FollowClear("pip: follow OFF (pan)");
    if (ay * 2 <= ax) sy = 0; else if (ax * 2 <= ay) sx = 0;    /* 8 方向量化 */
    if (speed > g_PanMax * g_PanRate) speed = g_PanMax * g_PanRate;
    if (speed < 1) speed = 1;
    PiP_PanStep(sx, sy, speed);
}

/* 把主摄像机钉到屏幕坐标 (x,y) 为中心, 并同步派生字段 (整幅重画可选)。 */
static void PiP_PinMainCam(void* tac, int x, int y, int fullRedraw)
{
    PiP_CamSetTarget(tac, x, y, PiP_CacheW(), PiP_CacheH());
    if (fullRedraw) *(int*)(size_t)ADDR_FULLREDRAW = 1;
}

/* 丢掉跨帧的临时输入/相机标记 (禁用、失焦、实例无效时)。 */
static void PiP_ResetTransient(void)
{
    g_HaveLastMain = 0;
    g_RadarSteal = 0;
    g_Pan = 0;
    s_RbHeld = 0;
    g_CamW = g_CamWZ = 0;
}

/* 地图视口矩形; 0 = 还没有视口 / 正在切分辨率。 */
static int PiP_ViewRect(RectangleStruct* a)
{
    FM_GetViewRects(a, 0);
    return a->Width >= 128 && a->Height >= 96;
}

/* 主渲染入口 (RenderEntry 钩) 调, 在主视图渲染前改相机:
 *   - 右键拖动期间钉住主相机, 小窗视野由 PiP_PollMouse 平移;
 *   - 聚焦时把程序化跳转 (0x6D6070) 的目标钉给小窗, 主相机退回原位。 */
void PiP_PreRender(void)
{
    void* tac;
    int cx, cy, mx = 0, my = 0, rmbIn = 0, rbNow;

    PiP_EnsureInit();
    tac = PiP_Tac();
    if (PiP_Off() || g_PipW <= 0 || !GtGameFocused() || !tac)
    {
        PiP_ResetTransient();
        return;
    }
    cx = *(int*)((char*)tac + OFF_TAC_CURX);
    cy = *(int*)((char*)tac + OFF_TAC_CURY);

    rbNow = PiP_KeyDown(VK_RBUTTON);
    if (g_Show && rbNow && GtMouseGameXY(&mx, &my))
        rmbIn = PiP_InWindow(mx, my);

    /* 拖动只在"按下边沿落在小窗内"时开始, 由 PiP_PollMouse 在松开时结束
     * (s_RbHeld 就是上一帧的右键, 因为本函数跑在 PiP_PollMouse 之前)。 */
    if (g_Show && rmbIn && !g_Pan && !s_RbHeld)   /* 按下沿: 记住主相机原位 */
    {
        g_Pan = 1;
        g_PanBaseX = g_HaveLastMain ? g_LastMainX : cx;
        g_PanBaseY = g_HaveLastMain ? g_LastMainY : cy;
    }
    s_RbHeld = rbNow;

    if (g_Pan)                        /* 拖动期间钉住主相机 (松开的当帧也钉) */
    {
        PiP_PinMainCam(tac, g_PanBaseX, g_PanBaseY, 0);
    }
    else if (g_Focus && g_HaveLastMain && g_RadarSteal)
    {
        int tx = cx, ty = cy;       /* 默认: 退用引擎(被主视野钳过的)中心 */
        /* 小地图点击 / 雷达事件点击都会重定小窗视野 = 用户要看别处, 跟随让路
         * (否则下一帧就被拉回目标, 点了等于没点)。 */
        PiP_FollowClear("pip: follow OFF (radar)");
        /* 有本帧小地图原始格 => 换成小窗摄像机屏幕坐标, 再按小窗自己的视口尺寸
         * 走"缩放档同款"防出界钳制。 */
        if (g_RawAt >= 0 && g_Frame - g_RawAt <= 1)
        {
            PiP_CellToScreen(g_RawCX, g_RawCY, &tx, &ty);
            PiP_ClampScreen(&tx, &ty);
        }
        else if (g_CamW)
        {
            /* 非小地图的程序化跳转 (雷达文本点击/脚本镜头): 用入口收到的精确世界坐标,
             * 不被主视野的边距钳制。换算与 0x6D6070 同式 (含 256 取整)。 */
            tx = PiP_Div256(30 * (g_CamWX - g_CamWY));
            ty = PiP_Div256(15 * (g_CamWX + g_CamWY)) - MsgText_ZToScreen(g_CamWZ);
            PiP_ClampScreen(&tx, &ty);
        }
        g_PinX = tx; g_PinY = ty; g_Pinned = 1;
        PiP_PinMainCam(tac, g_LastMainX, g_LastMainY, 1);
    }
    g_RadarSteal = g_CamW = g_CamWZ = 0;
    g_LastMainX = *(int*)((char*)tac + OFF_TAC_CURX);
    g_LastMainY = *(int*)((char*)tac + OFF_TAC_CURY);
    g_HaveLastMain = 1;
}

void PiP_OnCameraSet(const int* world)
{
    if (PiP_Off() || g_InRender || !g_Focus) return;
    g_RadarSteal = 1;
    if (world && IsPlausiblePtr((void*)world))
    {
        g_CamWX = world[0]; g_CamWY = world[1]; g_CamWZ = world[2];
        g_CamW = 1;
    }
    else g_CamW = g_CamWZ = 0;
}

/* 小地图点击的原始格 (钳制前) 观测入口 (0x653D92 钩)。只存值。 */
void PiP_OnRadarRawCell(int cx, int cy)
{
    if (PiP_Off() || g_InRender || !g_Show) return;
    g_RawCX = cx; g_RawCY = cy; g_RawAt = g_Frame;
}

/* ---------------- 小窗跟随 (F) ---------------- */

/* 引擎代码段 / rdata 区间: 判"这个指针是不是引擎的代码 / 对象虚表"。
 * 跟随目标跨帧验活与下面点击重映射的对象过滤共用同一口径。 */
#define PIP_GAME_TEXT_LO   0x00401000u
#define PIP_GAME_TEXT_HI   0x007E038Du
#define PIP_GAME_RDATA_LO  0x007E1000u
#define PIP_GAME_RDATA_HI  0x00811074u

static int PiP_IsGameCode(const void* p)
{
    size_t v = (size_t)p;
    return v >= PIP_GAME_TEXT_LO && v < PIP_GAME_TEXT_HI;
}

/* 目标还活着吗 (跨帧持有 ObjectClass*, 每帧必须验一遍)。
 * 先查字节判据 (不碰 Fetch_ID 索引), 再按索引反查 —— 索引自检不过时 fail-open,
 * 只留字节判据, 与 OrderUtil 各处同一口径。 */
static int PiP_FolUsable(void* o)
{
    void** vt;
    if (!o || !IsPlausiblePtr(o)) return 0;
    vt = *(void***)o;
    if (!vt || (size_t)vt < PIP_GAME_RDATA_LO || (size_t)vt >= PIP_GAME_RDATA_HI)
        return 0;
    if (!PiP_IsGameCode(vt[0x2C / 4]) || !PiP_IsGameCode(vt[0x48 / 4])) return 0;
    if (!((unsigned char*)o)[OFF_ISALIVE]) return 0;   /* 已死 (尸体/正在移除) */
    if (((unsigned char*)o)[OFF_INLIMBO]) return 0;    /* 进工厂/运输/已离场 */
    return OU_PtrAlive(o, g_FolId, g_FolIdxOk);
}

/* 关跟随并清引擎侧状态 (主窗口不会再跟着它)。why 只用于日志。 */
static void PiP_FollowClear(const char* why)
{
    if (!g_FolOn) return;
    g_FolOn = 0;
    g_FolObj = 0;
    g_FolId = 0;
    /* 引擎自己的跟随如果还挂着同一个对象, 一并清掉, 免得主窗口替我们继续跟。 */
    ((void (__attribute__((thiscall)) *)(void*, void*))FN_DISP_SETFOLLOW)(
        (void*)(size_t)ADDR_DISPLAY, 0);
    if (why) LogLine(why);
}

/* 把目标设为小窗跟随对象。返回 1 = 已开始跟随。 */
static int PiP_FollowSet(void* o)
{
    void** vt;
    if (!o || !IsPlausiblePtr(o)) return 0;
    vt = *(void***)o;
    if (!vt || (size_t)vt < PIP_GAME_RDATA_LO || (size_t)vt >= PIP_GAME_RDATA_HI)
        return 0;
    if (!PiP_IsGameCode(vt[0x2C / 4]) || !PiP_IsGameCode(vt[0x48 / 4])) return 0;
    if (!((unsigned char*)o)[OFF_ISALIVE]) return 0;

    g_FolObj = o;
    g_FolId = OU_ObjId(o);
    /* 索引自检: 拿目标本身查一遍, 查得到才启用严格验活。 */
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        g_FolIdxOk = (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 200000 &&
                      g_FolId > 0 && OU_PtrAliveRaw(o, g_FolId)) ? 1 : 0;
    }
    g_FolOn = 1;
    /* 与"自动跟随雷达事件"互斥: 两者都写小窗目标, 同时开会互相拉。 */
    g_AutoJump = 0;
    /* 主窗口不动: 清掉引擎的跟随状态。 */
    ((void (__attribute__((thiscall)) *)(void*, void*))FN_DISP_SETFOLLOW)(
        (void*)(size_t)ADDR_DISPLAY, 0);
    LogLine("pip: follow ON (F, window-only)");
    return 1;
}

/* Follow 命令 (0x537A10) 钩回调。返回 1 = 小窗接管, 0 = 放行原版。 */
int PiP_FollowCommand(void)
{
    void** items;
    int n;
    void* o;

    PiP_EnsureInit();
    if (!g_Enable || !g_FollowCfg) return 0;
    /* 跟随开着时 F 一律是"关", 不管此刻焦点在哪 —— 否则焦点一丢就没法关。 */
    if (g_FolOn)
    {
        PiP_FollowClear("pip: follow OFF (F)");
        return 1;
    }
    /* 只有"小窗开着 + 被点中"才把 F 抢过来; 其余位置留给主窗口原版跟随。 */
    if (!g_Show || !g_Focus || g_PipW <= 0) return 0;
    items = *(void***)(size_t)(ADDR_CUR_OBJECTS + OFF_VEC_ITEMS);
    n = *(int*)(size_t)(ADDR_CUR_OBJECTS + OFF_VEC_COUNT);
    if (!items || !IsPlausiblePtr(items) || n <= 0) return 0;
    o = items[0];
    if (!PiP_FollowSet(o)) return 0;
    g_ForceDue = 2;              /* 立刻补渲, 别等 Interval */
    return 1;
}

/* 每帧推进 (PiP_AfterMap 里调): 目标 Location → 小窗摄像机屏幕坐标 → 防出界。
 * 换算与引擎 0x6D6070 同式 (含 256 取整), 与 PiP_PreRender 里那份一致。 */
static void PiP_FollowTick(void)
{
    int* loc;
    int x, y;
    if (!g_FolOn) return;
    if (!g_Show || !PiP_FolUsable(g_FolObj))
    {
        PiP_FollowClear("pip: follow OFF (target gone)");
        return;
    }
    loc = (int*)((char*)g_FolObj + OFF_OBJ_LOCATION);
    x = PiP_Div256(30 * (loc[0] - loc[1]));
    y = PiP_Div256(15 * (loc[0] + loc[1])) - MsgText_ZToScreen(loc[2]);
    PiP_ClampScreen(&x, &y);
    g_PinX = x; g_PinY = y;
    g_Pinned = 1;
}

/* ---------------- 降采样贴窗 ---------------- */

/* 窗内三个图标的 14x14 位图 (13x13 图形 + 右下 1px 黑影溢出): 低 14 位 = 本体,
 * 高 14 位 = 黑影, 位序即列 (bit0 = 最左)。由原手绘公式导出, 逐像素等价 ——
 * 黑影压住本体的地方按最终像素取值, 改表前先照原图重算。
 * slot 0 的黑影是后补的: 原实现只用 PxPut 画本体没画影, 三个按钮里它独缺一道。 */
static const unsigned kIconBits[PIP_ICON_N][14] = {
    { 0x00000040u, 0x000000E0u, 0x01800040u, 0x00800040u,   /* 0 移动: 十字 + 四箭头 */
      0x00800040u, 0x00800842u, 0x00001FFFu, 0x37BC0842u,
      0x10840040u, 0x00800040u, 0x00800040u, 0x000000E0u,
      0x01800040u, 0x00800000u },
    { 0x00000000u, 0x00000040u, 0x000000A0u, 0x014000A0u,   /* 1 自动跟随: 三角感叹号 */
      0x00000150u, 0x02A00150u, 0x00A00248u, 0x04900248u,
      0x00900404u, 0x08080444u, 0x00880802u, 0x10000FFEu,
      0x1FFC0000u, 0x00000000u },
    { 0x00000000u, 0x000001F0u, 0x01800278u, 0x04C00238u,   /* 2 操作: 鼠标 + 按下块 */
      0x04C00238u, 0x04F00208u, 0x04900248u, 0x04900208u,
      0x04100208u, 0x04100208u, 0x04100208u, 0x04100208u,
      0x040001F0u, 0x03E00000u },
};
static const unsigned short kIconOn[PIP_ICON_N] = { PIP_C_WHITE, PIP_C_AMBER, PIP_C_GREEN };
static int* const kIconState[PIP_ICON_N] = { 0, &g_AutoJump, &g_Operate };

/* 画窗内图标 (slot 0=移动 1=自动跟随 2=操作); 关着的按钮画灰, 移动图标恒白。 */
static void PiP_DrawIcon(unsigned short* db, int dp, int cw, int ch,
                         int slot, int bx, int by)
{
    const unsigned short c = (kIconState[slot] && !*kIconState[slot]) ? PIP_C_GRAY
                                                                     : kIconOn[slot];
    const unsigned* row = kIconBits[slot];
    int x, y;
    for (y = 0; y < 14; y++, row++)
        for (x = 0; x < 14; x++)
            if (*row & (1u << x)) PiP_PxPut(db, dp, cw, ch, bx + x, by + y, c);
            else if ((*row >> 16) & (1u << x)) PiP_PxPut(db, dp, cw, ch, bx + x, by + y, 0);
}

/* 把 g_Comp (RW x RH) 按 ScaleNum 最近邻降采样, 贴到窗口面 dst 的 (X,Y) 处。
 * 边框/窗内按钮在同一把锁里画。 */
static int PiP_Blit(void* dst, int X, int Y, int dW, int dH)
{
    unsigned char *sb;
    unsigned short *db;
    int sp, dp, cw, ch, x, y, sc, i, ix, iy;
    if (!IsPlausiblePtr(dst)) return 0;
    sp = GtSurfGet(g_Comp, GtVT_GET_PITCH);
    dp = GtSurfGet(dst, GtVT_GET_PITCH);
    cw = GtSurfGet(dst, GtVT_GET_WIDTH);
    ch = GtSurfGet(dst, GtVT_GET_HEIGHT);
    /* 几何校验: 行距必须与宽一致, 窗口必须整块落在面内。 */
    if (sp <= 0 || sp < g_RW * 2 || sp > g_RW * 2 + 64) return 0;
    if (dp <= 0 || dp < cw * 2 || dp > cw * 2 + 64) return 0;
    if (cw < 8 || ch < 8 || dW <= 0 || dH <= 0) return 0;
    if (X < 0 || Y < 0 || X + dW > cw || Y + dH > ch) return 0;
    sc = g_ScaleNum;
    sb = GtLock(g_Comp);
    if (!sb) return 0;
    db = (unsigned short*)GtLock(dst);
    if (!db) { GtUnlock(g_Comp); return 0; }
    /* 最近邻: 源像素 = 显示像素 * sc。dp 是字节行距, 目标行按字节算。 */
    for (y = 0; y < dH; y++)
    {
        const unsigned short* srow =
            (const unsigned short*)(sb + (size_t)(y * sc) * sp);
        unsigned short* drow =
            (unsigned short*)((unsigned char*)db + (size_t)(Y + y) * dp) + X;
        for (x = 0; x < dW; x++) drow[x] = srow[x * sc];
    }
    /* 聚焦=绿框, 跟随时=琥珀框 (跟随时主窗口不动, 这个色就是状态提示) */
    if (g_Border)
        PiP_PxFrame(db, dp, cw, ch, X, Y, dW, dH,
                    g_FolOn ? PIP_C_AMBER : (g_Focus ? PIP_C_GREEN : PIP_C_GRAY));
    /* 三个窗内按钮无论画不画边框都画 (它们是控件, 不是装饰)。 */
    for (i = 0; i < PIP_ICON_N; i++)
    {
        PiP_IconAt(X, Y, dW, i, &ix, &iy);
        PiP_DrawIcon(db, dp, cw, ch, i, ix, iy);
    }
    GtUnlock(dst);
    GtUnlock(g_Comp);
    return 1;
}

/* ---------------- 点击门 ---------------- */

/* ProcessClickCoords 入口处理方式 (物理坐标, 不改聚焦状态):
 *   0 = 放行, 1 = 吞掉, 2 = 重映射到小窗世界位置。
 * 窗内控件按住时吞; 右键放行; 左键/悬停按操作模式决定。 */
int PiP_ClickCoordsMode(int gx, int gy)
{
    int lb, rb;
    PiP_EnsureInit();
    if (PiP_Off() || !g_Show || g_PipW <= 0 || g_PipH <= 0) return 0;
    lb = PiP_KeyDown(VK_LBUTTON);
    rb = PiP_KeyDown(VK_RBUTTON);
    /* 窗内左键框选的松开: 记录松手点, 帧末统一框选。先于窗内判定: 拖到窗外松手
     * 也要按框选收尾。引擎在松开路径不看 ProcessClickCoords 的返回值
     * (0x69325E 后直接接 DecideAction), 吞不掉收尾令, 靠 PiP_LeftUpGate
     * (0x4AB9B0) 把整段左键收尾跳过; 这里只把格/目标算成合法值。 */
    if (g_SelDrag && !lb)
    {
        if (PiP_DragDist(gx - g_SelStartGX, gy - g_SelStartGY) >= PIP_SEL_DRAG)
        {
            g_SelCurGX = gx;
            g_SelCurGY = gy;
            /* 窗内重映射让 DecideAction 拿到合法的格/目标; 窗外放行原版填输出。 */
            return PiP_InWindow(gx, gy) ? 2 : 0;
        }
        g_SelDrag = 0;               /* 抖动不足阈值 = 普通单击, 继续走重映射 */
    }
    if (!PiP_InWindow(gx, gy)) return 0;
    /* 窗内控件: 按住时吞掉, 悬停时不影响原版。 */
    if (PiP_HitIcon(gx, gy)) return (lb || rb) ? 1 : 0;
    /* 右键交给游戏: 不拖动 = 取消选中/当前动作; 拖动 = 游戏自己平移,
     * PiP 的右键拖动照样由 PiP_PollMouse 接管并在 PreRender 钉住主相机。 */
    if (rb) return 0;
    if (!lb) return g_Operate ? 2 : 0;
    return g_Operate ? 2 : 1;
}

/* 左键按下记录钩 (0x4AC310): 窗内按下时跳过 LeftDownPosition 记录,
 * 防止拖框按主视图坐标误选。单击选择/下令走松开路径, 不受影响。 */
#define PIP_LEFTDOWN_RET 0x004AC36Eu
extern "C" __declspec(dllexport) unsigned __cdecl PiP_LeftDownGate(void* regs)
{
    int mx = 0, my = 0, mode;
    (void)regs;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    mode = PiP_ClickCoordsMode(mx, my);
    return mode ? PIP_LEFTDOWN_RET : 0u;
}

/* 左键收尾 (DisplayClass::LeftMouseButtonUp 0x4AB9B0) 跳过出口: 原函数尾 ret 0x14。
 * 框选松手那一下不能再走点选/下令 (按下沿已按原版语义处理过), 否则会在松手点
 * 再下一次令。跳过整个函数后调用方 0x693290 照样清鼠标捕获, 不会卡住。 */
__attribute__((naked)) static void PiP_Ret14(void)
{
    __asm__ volatile ("ret $0x14\n\t");
}
extern "C" __declspec(dllexport) unsigned __cdecl PiP_LeftUpGate(void* regs)
{
    (void)regs;
    /* 只要"窗内左键拖框"还挂着, 这次左键收尾就是框选松手: 整段跳过。
     * 抖动不足阈值的普通单击已由 PiP_ClickCoordsMode 提前清掉 g_SelDrag
     * (它在同一条收尾路径上, 必先于本门), 不会误吞真单击。 */
    if (!g_SelDrag) return 0;
    return (unsigned)(size_t)PiP_Ret14;
}

/* 框选带绘制 (0x6DA180, 每次 TacticalClass::Render 都调) 跳过出口。
 * 主视图拖框时引擎会把 Band (主视口局部坐标) 也画进小窗的离屏面; 第二摄像机
 * 内容与这套坐标无关, 贴出来就是错位的幽灵框。离屏渲染期间整段跳过即可,
 * 主视图自己的带照常画 (那时 g_InRender=0)。 */
__attribute__((naked)) static void PiP_Ret(void)
{
    __asm__ volatile ("ret\n\t");
}
extern "C" __declspec(dllexport) unsigned __cdecl PiP_BandGate(void* regs)
{
    (void)regs;
    if (!g_InRender) return 0;
    return (unsigned)(size_t)PiP_Ret;
}

/* ---------------- 操作模式: 点击重映射 ----------------
 * 物理点转成小窗离屏视口坐标, 临时摆好小窗相机状态再调原函数, 返回前还原。 */
static PiP_CamSave g_CamSave;
static int g_CamSwapped = 0;
static unsigned char g_ClkSelSave[PIP_SEL_ENT * PIP_SEL_MAX];  /* 小窗点击前的可选物件表备份 */
static unsigned char g_ClkSelTmp[PIP_SEL_ENT * PIP_SEL_MAX];   /* 过滤失效对象后的表 */
static int g_ClkSelN = 0;
static int g_ClkSelCount = 0;

/* 小窗表是上一帧快照, 对象可能已销毁。ProcessClickCoords 会直接调用
 * [obj->vtable+0x88]; 装入前过滤对象/vtable 与关键槽, 失效项丢弃。
 * (PIP_GAME_* 与 PiP_IsGameCode 在前面小窗跟随那节定义, 两处共用。) */

static int PiP_SelEntryUsable(const unsigned char* e)
{
    void* obj;
    void** vt;
    obj = *(void**)e;
    if (!obj || !IsPlausiblePtr(obj)) return 0;
    vt = *(void***)obj;
    if (!vt || (size_t)vt < PIP_GAME_RDATA_LO || (size_t)vt >= PIP_GAME_RDATA_HI)
        return 0;
    if (!PiP_IsGameCode(vt[0x2C / 4])) return 0;
    if (!PiP_IsGameCode(vt[0x48 / 4])) return 0;
    if (!PiP_IsGameCode(vt[0x88 / 4])) return 0;
    return 1;
}

/* 装上小窗的相机与可选物件表 (点击重映射和框选共用), 成功后必须成对还原。 */
static int PiP_SwapIn(void)
{
    void* tac;
    int srcN, i;

    if (PiP_Off() || !g_Show || g_RW <= 0 || !g_HasContent || g_CamSwapped) return 0;
    tac = PiP_Tac();
    if (!tac) return 0;

    PiP_CamSnapshot(&g_CamSave, tac);
    PiP_CamSetView(g_RW, g_RH);
    /* 用"上次真正渲染"的目标 (g_LastRtX/Y), 不用可能已被右键拖动改过的 g_PinX/Y:
     * 可选物件表就是那次渲染产出的, 两者必须来自同一帧。 */
    PiP_CamSetTarget(tac, g_LastRtX, g_LastRtY, g_RW, g_RH);
    PiP_CamZeroZoom(tac);

    /* 装上小窗上一次渲染产出的可选物件表 (物件屏坐标 = 小窗视口坐标)。
     * ProcessClickCoords 用这张表在点击点附近找物件; 不换表就只会按主视图的位置
     * 找, 小窗里点到的单位会错位/点不中。 */
    g_ClkSelCount = *(int*)((char*)tac + OFF_TAC_SELCNT);
    srcN = PiP_ClampCount(g_PipSelN, PIP_SEL_MAX);
    g_ClkSelN = 0;
    for (i = 0; i < srcN; i++)
    {
        const unsigned char* src = g_ScratchPipSel + i * PIP_SEL_ENT;
        if (!PiP_SelEntryUsable(src)) continue;
        PiP_CopyBytes(g_ClkSelTmp + g_ClkSelN * PIP_SEL_ENT, src, PIP_SEL_ENT);
        g_ClkSelN++;
    }
    if (g_ClkSelN > 0)
    {
        unsigned char* p = (unsigned char*)(size_t)ADDR_TACTICAL_SELARR;
        PiP_CopyBytes(g_ClkSelSave, p, g_ClkSelN * PIP_SEL_ENT);
        PiP_CopyBytes(p, g_ClkSelTmp, g_ClkSelN * PIP_SEL_ENT);
    }
    *(int*)((char*)tac + OFF_TAC_SELCNT) = g_ClkSelN;

    g_CamSwapped = 1;
    return 1;
}

int PiP_BeginClickMap(int gx, int gy, int* px, int* py)
{
    int lx, ly;
    if (PiP_Off() || !g_Show || g_PipW <= 0 || g_RW <= 0) return 0;
    lx = (gx - g_PipX) * g_ScaleNum;
    ly = (gy - g_PipY) * g_ScaleNum;
    if (lx < 0 || ly < 0 || lx >= g_RW || ly >= g_RH) return 0;
    if (!PiP_SwapIn()) return 0;
    if (px) *px = lx;
    if (py) *py = ly;
    return 1;
}

void PiP_EndClickMap(void)
{
    void* tac;
    if (!g_CamSwapped) return;
    g_CamSwapped = 0;
    /* 用 Begin 存的实例指针还原, 不能带着小窗状态回到主循环。 */
    tac = g_CamSave.tac;
    if (g_ClkSelN > 0)
        PiP_CopyBytes((void*)(size_t)ADDR_TACTICAL_SELARR, g_ClkSelSave,
                      g_ClkSelN * PIP_SEL_ENT);
    PiP_CamRestore(&g_CamSave);
    if (tac && IsPlausiblePtr(tac))
        *(int*)((char*)tac + OFF_TAC_SELCNT) = g_ClkSelCount;
}

/* 窗内左键框选: 物理矩形换成小窗视口坐标, 临时装上小窗相机/可选物件表,
 * 再用引擎自带的 MakeSelection (0x6D9FF0, 过滤回调 0x4AC2B0) 选人。
 * 与主视图框选同一条标准路径, 只借用小窗那次渲染的屏幕坐标表。 */
static void PiP_BoxSelect(int gx1, int gy1, int gx2, int gy2)
{
    static const unsigned short kBand[4] = { OFF_TAC_BAND_X1, OFF_TAC_BAND_Y1,
                                             OFF_TAC_BAND_X2, OFF_TAC_BAND_Y2 };
    void* tac;
    Fn_MakeSel fn = (Fn_MakeSel)(size_t)ADDR_TAC_MAKESEL;
    int x1, y1, x2, y2, ob[4], nb[4], i;

    if (PiP_Off() || !g_Show || g_RW <= 0 || g_RH <= 0 || g_PipW <= 0) return;
    x1 = (gx1 - g_PipX) * g_ScaleNum; y1 = (gy1 - g_PipY) * g_ScaleNum;
    x2 = (gx2 - g_PipX) * g_ScaleNum; y2 = (gy2 - g_PipY) * g_ScaleNum;
    PiP_Order2(&x1, &x2); PiP_Order2(&y1, &y2);
    x1 = x1 < 0 ? 0 : x1; y1 = y1 < 0 ? 0 : y1;
    x2 = x2 >= g_RW ? g_RW - 1 : x2; y2 = y2 >= g_RH ? g_RH - 1 : y2;
    if (x2 < x1 || y2 < y1) return;

    if (!PiP_SwapIn()) return;
    tac = g_CamSave.tac;
    for (i = 0; i < 4; i++) ob[i] = *(int*)((char*)tac + kBand[i]);
    /* 0x6D9FF0 开头会把"起点 (0,0)"当空带直接返回 (原版就这样)。倒着存起点,
     * 让左上角贴边的小框也能选; MakeSelection 自己会再做一次 min/max。 */
    nb[0] = x2; nb[1] = y2; nb[2] = x1; nb[3] = y1;
    for (i = 0; i < 4; i++) *(int*)((char*)tac + kBand[i]) = nb[i];
    fn(tac, (void*)(size_t)ADDR_TAC_SELFILTER);
    for (i = 0; i < 4; i++) *(int*)((char*)tac + kBand[i]) = ob[i];
    PiP_EndClickMap();
}

/* ---------------- 鼠标轮询 ---------------- */

/* 每帧鼠标轮询 (AfterMap 调), 用真实按下沿判定, 避免悬停划过误触发:
 *   侧栏按钮开关小窗; 移动图标拖动; 窗内按下聚焦; 小地图按下重定位;
 *   其余按下在帧末失焦。右键按住按游戏模型平移小窗视野。 */
static void PiP_PollMouse(void)
{
    static int s_LbPrev = 0, s_RbPrev = 0;
    int lb, rb, mx, my, inRect, icon, inBtn;
    int bx, by, bw, bh;

    if (!GtGameFocused())
    {
        s_LbPrev = 0; s_RbPrev = 0;
        g_Drag = 0; g_Pan = 0; g_SelDrag = 0;
        return;
    }
    lb = PiP_KeyDown(VK_LBUTTON);
    rb = PiP_KeyDown(VK_RBUTTON);
    if (!GtMouseGameXY(&mx, &my)) { s_LbPrev = lb; s_RbPrev = rb; return; }

    inBtn = PiP_BtnRect(&bx, &by, &bw, &bh) && PiP_PointIn(mx, my, bx, by, bw, bh);
    inRect = PiP_InWindow(mx, my);
    icon = inRect ? PiP_HitIcon(mx, my) : 0;

    if (lb && !s_LbPrev)                 /* 左键按下沿 (整个按住期只触发一次) */
    {
        if (inBtn)
        {
            PiP_Toggle();
        }
        else if (icon == 1)
        {
            g_Drag = 1;
            g_DragGX = mx - g_PipX;
            g_DragGY = my - g_PipY;
        }
        else if (icon == 2)
        {
            g_AutoJump = !g_AutoJump;
            g_AutoSeen = 0;              /* 开启时立即跳到当前最新事件 */
            LogLine(g_AutoJump ? "pip: auto-jump ON" : "pip: auto-jump OFF");
        }
        else if (icon == 3)
        {
            g_Operate = !g_Operate;
            LogLine(g_Operate ? "pip: operate ON" : "pip: operate OFF");
        }
        else if (inRect)
        {
            /* 聚焦 = 允许之后的小地图点击重新给小窗定位; 不改小窗当前目标
             * (小窗内容本来就固定不动, 聚焦只解锁重新定位)。 */
            if (!g_Focus) g_Focus = 1;
            if (g_Operate)               /* 操作模式: 按住拖动 = 框选 (松开才选) */
            {
                g_SelDrag = 1;
                g_SelStartGX = mx; g_SelStartGY = my;
                g_SelCurGX = mx; g_SelCurGY = my;
            }
        }
        else if (g_Show && !FM_RadarHit(mx, my))
        {
            /* 小地图按下是给小窗重新定位, 不算"点了小窗外"。 */
            g_PendingUnfocus = 1;
        }
    }

    if (g_SelDrag)                       /* 窗内左键框选: 松开那帧收尾 */
    {
        if (!g_Show || !g_Operate)
        {
            g_SelDrag = 0;
        }
        else if (lb)
        {
            g_SelCurGX = mx; g_SelCurGY = my;
        }
        else                             /* 松开: 不管点击门/LeftUpGate 谁先跑都收尾 */
        {
            if (PiP_DragDist(g_SelCurGX - g_SelStartGX, g_SelCurGY - g_SelStartGY)
                >= PIP_SEL_DRAG)
                PiP_BoxSelect(g_SelStartGX, g_SelStartGY, g_SelCurGX, g_SelCurGY);
            g_SelDrag = 0;
        }
    }

    if (g_Drag)                          /* 拖动小窗 (移动图标) */
    {
        s_WantX = mx - g_DragGX;
        s_WantY = my - g_DragGY;
        if (!lb) g_Drag = 0;
    }

    if (rb && !s_RbPrev)                 /* 右键按下沿: 记锚点 (之后只按位移给速度) */
    {
        g_PanMX = mx; g_PanMY = my;
    }

    if (g_Pan)                           /* 右键拖动小窗视野: 按游戏模型给速度 */
    {
        int ox = mx - g_PanMX, oy = my - g_PanMY;
        if (!rb) g_Pan = 0;              /* 松开: 立即停 (游戏右键拖动没有惯性) */
        else if (PiP_PanDist(ox, oy) > PIP_PAN_DRAG)   /* 阈值内不动: 那是"右键取消" */
            PiP_PanBy(ox, oy);
    }

    s_LbPrev = lb;
    s_RbPrev = rb;
}

/* ---------------- 帧入口 ---------------- */

/* AfterMap 钩调: 把第二摄像机画面渲进离屏面, 不贴 Composite。
 * 贴窗放到缩放呈现之后 (PiP_DrawOnComposite), 避免小窗被一起缩放。 */
void PiP_AfterMap(void)
{
    RectangleStruct a;
    void* tac;
    void* comp;
    int fr, dW, dH, rw, rh, tx, ty, due;

    PiP_EnsureInit();
    if (!g_Enable) goto done;

    PiP_PollMouse();                     /* 离屏面失败期间也要能点侧栏按钮关窗 */
    if (!g_Show) goto done;
    if (g_Fail) { if (g_Frame - g_FailFrame < PIP_FAIL_RETRY) goto done; g_Fail = 0; }

    /* 仅在有效地图视口 + 窗口面就绪时跑。 */
    if (!PiP_ViewRect(&a)) goto done;
    comp = *(void**)ADDR_SURF_COMPOSITE;
    tac = PiP_Tac();
    if (!comp || !IsPlausiblePtr(comp) || !tac) goto done;

    dW = g_DispW; dH = g_DispH;
    if (dW > a.Width - 16) dW = a.Width - 16;
    if (dH > a.Height - 16) dH = a.Height - 16;
    if (dW < 64 || dH < 48) goto done;
    rw = dW * g_ScaleNum;
    rh = dH * g_ScaleNum;
    if (!PiP_EnsureSurfs(rw, rh)) goto done;

    /* 第二摄像机渲染可节流 (Interval), 但贴窗每帧都做 (见 PiP_DrawOnComposite)。*/
    fr = *(int*)(size_t)ADDR_FRAME;
    due = (fr != g_LastFrame) && (g_Interval <= 1 || (fr % g_Interval) == 0);
    if (g_SurfJustBuilt) { due = 1; g_SurfJustBuilt = 0; }
    if (g_ForceDue > 0) { due = 1; g_ForceDue--; }
    if (g_Pan) due = 1;                  /* 拖动时每帧都刷, 否则手感发滞 */

    /* 自动跟随最新雷达事件: 只在"来了新事件"时改目标 (同一事件不打断手动平移)。
     * 按钮刚打开/窗口刚打开时 g_AutoSeen=0, 立即跳到当前最新一条。 */
    if (g_AutoJump)
    {
        short acx = 0, acy = 0;
        unsigned aseq = 0;
        if (MsgText_LatestRadar(&acx, &acy, &aseq) &&
            (!g_AutoSeen || aseq != g_AutoSeq))
        {
            /* 跳新事件 = 用户要看别处, 小窗跟随让路。 */
            PiP_FollowClear("pip: follow OFF (auto-jump)");
            PiP_CellToScreen(acx, acy, &tx, &ty);
            PiP_ClampScreen(&tx, &ty);
            g_PinX = tx; g_PinY = ty; g_Pinned = 1;
            g_AutoSeq = aseq; g_AutoSeen = 1;
            due = 1;
        }
    }

    /* 小窗跟随: 每帧把目标钉成小窗视野中心, 并强制重渲 (不能按 Interval 节流,
     * 否则跟随会一卡一顿)。放在自动跟随之后: 两者互斥, 同时只会有一种在跑。 */
    if (g_FolOn)
    {
        PiP_FollowTick();
        if (g_FolOn) due = 1;
    }

    if (due)
    {
        /* 目标**固定不变**: 开窗首帧取"打开时的主视图中心"(屏幕坐标), 之后只由
         * 聚焦后的小地图点击 / 窗内右键拖动改写。绝不跟随主视图移动。 */
        if (!g_Pinned)
        {
            tx = *(int*)((char*)tac + OFF_TAC_CURX);
            ty = *(int*)((char*)tac + OFF_TAC_CURY);
            PiP_ClampScreen(&tx, &ty);
            g_PinX = tx; g_PinY = ty;
            g_Pinned = 1;
        }
        tx = g_PinX; ty = g_PinY;
        g_LastFrame = fr;
        if (PiP_RenderCamera(tx, ty))
        {
            g_HasContent = 1;
            g_LastRtX = tx; g_LastRtY = ty;
        }
    }

done:
    /* 帧末失焦: 只有"按下点既不在小窗内也不在小地图上"才会挂起。
     * 小地图点击在 PiP_PollMouse 里就直接排除, 故这里不依赖时序, 多点几次也不会丢。 */
    if (g_PendingUnfocus)
    {
        g_PendingUnfocus = 0;
        g_Focus = 0;                 /* 只丢焦点: 小窗内容/目标保持固定不动 */
    }
    g_Frame++;
}

/* 小窗在地图区里的物理位置: 用记下来的绝对落点 (没放过就取右上角默认点),
 * 钳进地图区, 再避让侧栏顶部那一块。 */
static void PiP_PipPos(const RectangleStruct* a, int dW, int dH, int* X, int* Y)
{
    RectangleStruct blk;
    int x, y;
    if (s_WantX < 0 || s_WantY < 0)
    {
        s_WantX = a->X + a->Width - dW - g_PadX;
        s_WantY = a->Y + g_PadY;
    }
    x = s_WantX; y = s_WantY;
    x = x < a->X ? a->X : x; y = y < a->Y ? a->Y : y;
    if (x + dW > a->X + a->Width) x = a->X + a->Width - dW;
    if (y + dH > a->Y + a->Height) y = a->Y + a->Height - dH;
    /* 侧栏只留顶部那一块 (资金条/小地图/两个按钮) 时, 视口已铺到屏幕右缘, 那一块
     * 却仍压在画面上 —— 小窗压上去就盖住小地图, 而且点进去先被 FM_RadarHit 吃掉。
     * 相交就整体左移到块的左侧。全隐藏档该块不存在 (返回 0), 右上角可随便放。*/
    if (FM_KeepTopRectScreen(&blk) &&
        x < blk.X + blk.Width && x + dW > blk.X &&
        y < blk.Y + blk.Height && y + dH > blk.Y)
    {
        x = blk.X - dW;
        if (x < a->X) x = a->X;
    }
    *X = x; *Y = y;
}

/* 窗内框选时在 Composite 上画实线框 (物理坐标, 只画在小窗矩形内)。
 * 引擎的框选带没参与 (按下记录被跳过), 没有它用户看不到拖到哪。 */
static void PiP_DrawSelBand(void* dst)
{
    unsigned short* db;
    int dp, cw, ch, x1, y1, x2, y2;
    if (!g_SelDrag || !g_Show || g_PipW <= 0) return;
    if (!dst || !IsPlausiblePtr(dst)) return;
    dp = GtSurfGet(dst, GtVT_GET_PITCH);
    cw = GtSurfGet(dst, GtVT_GET_WIDTH);
    ch = GtSurfGet(dst, GtVT_GET_HEIGHT);
    if (dp <= 0 || cw <= 0 || ch <= 0) return;
    x1 = g_SelStartGX; y1 = g_SelStartGY;
    x2 = g_SelCurGX;   y2 = g_SelCurGY;
    PiP_Order2(&x1, &x2); PiP_Order2(&y1, &y2);
    x1 = x1 < g_PipX ? g_PipX : x1; y1 = y1 < g_PipY ? g_PipY : y1;
    if (x2 >= g_PipX + g_PipW) x2 = g_PipX + g_PipW - 1;
    if (y2 >= g_PipY + g_PipH) y2 = g_PipY + g_PipH - 1;
    if (x2 < x1 || y2 < y1) return;
    db = (unsigned short*)GtLock(dst);
    if (!db) return;
    PiP_PxFrame(db, dp, cw, ch, x1, y1, x2 - x1 + 1, y2 - y1 + 1, PIP_C_WHITE);
    GtUnlock(dst);
}

/* 贴 Composite (消息帧钩里, 缩放呈现之后调): 每帧用缓存画面贴, 尺寸由
 * g_RW/g_ScaleNum 反推。surf 为消息帧钩校验过的窗口面, 传 0 时回退全局 Composite。 */
void PiP_DrawOnComposite(void* surf)
{
    RectangleStruct a;
    void* comp;
    int dW, dH, X, Y;

    PiP_EnsureInit();
    if (PiP_Off() || !g_Show || !g_HasContent || g_RW <= 0) return;
    if (!PiP_ViewRect(&a)) return;
    comp = surf;
    if (!comp || !IsPlausiblePtr(comp)) comp = *(void**)ADDR_SURF_COMPOSITE;
    if (!comp || !IsPlausiblePtr(comp)) return;
    dW = g_RW / g_ScaleNum;
    dH = g_RH / g_ScaleNum;
    PiP_PipPos(&a, dW, dH, &X, &Y);
    if (!PiP_Blit(comp, X, Y, dW, dH)) return;
    /* 记下小窗的物理游戏坐标 (点击门 / 移动图标 / 右键拖动判定用)。 */
    g_PipX = X; g_PipY = Y; g_PipW = dW; g_PipH = dH;
    PiP_DrawSelBand(comp);
}

/* 侧栏开关按钮: 和底部电量一样画进侧栏面, 跟着条带上屏, 避免 Composite
 * 被侧栏 blit 盖掉造成闪烁。坐标为侧栏面局部坐标 (x 从侧栏列左缘算)。 */
void PiP_DrawButton(void)
{
    void* sb;
    int side, x, y, dp, cw, ch, hover = 0;
    int mx, my, bx, by, bw, bh;
    unsigned short* db;

    PiP_EnsureInit();
    if (!g_Enable) return;
    sb = *(void**)ADDR_SURF_SIDEBAR;
    if (!sb || !IsPlausiblePtr(sb)) return;
    side = PiP_BtnSide();
    x = g_BtnPos[side][0];
    y = g_BtnPos[side][1];
    if (PiP_BtnRect(&bx, &by, &bw, &bh) && GtMouseGameXY(&mx, &my) &&
        PiP_PointIn(mx, my, bx, by, bw, bh))
        hover = 1;
    cw = GtSurfGet(sb, GtVT_GET_WIDTH);
    ch = GtSurfGet(sb, GtVT_GET_HEIGHT);
    dp = GtSurfGet(sb, GtVT_GET_PITCH);
    if (dp <= 0 || x >= cw || y >= ch) return;
    db = (unsigned short*)GtLock(sb);
    if (!db) return;
    PiP_DrawBtnImg(db, dp, cw, ch, side, x, y, hover);
    GtUnlock(sb);
}
