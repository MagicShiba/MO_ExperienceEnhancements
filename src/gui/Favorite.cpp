/* Favorite.cpp - 选中单位收藏槽, 见 Favorite.h。
 *
 * 槽里存整组单位 (对象指针 + Fetch_ID), 每帧验活剪枝, 死了就掉出槽。
 * 头顶星星只画在"该格已揭示"的单位上, 黑幕里不画星也不画箭头 —— 收藏是
 * 本机信息, 但标记露在画面上就等于把黑幕里的位置说了出来。
 *
 * 按钮语义: Ctrl+左键按下 = 立即收藏当前选中集; 不按 Ctrl 时按下只是记起点,
 * 按住超过 HoldMs = 清空该槽, 松开而未超时 = 镜头跳过去。这样"单击跳转"
 * 不会被长按误触, 也不需要双击窗口。
 *
 * 星星与箭头分两趟画: 星星在 UI 层之前 (超武侧栏面板要能盖住它), 箭头在
 * 最上层 (离屏指示必须始终看得见)。
 *
 * 只读模拟状态 + 只画渲染面 + 只动本地摄像机 (SetTacticalPosition), 联机安全。 */

#include <windows.h>
#include "EngineAddr.h"
#include "Favorite.h"
#include "FullMap.h"
#include "GdiText.h"
#include "OrderUtil.h"

#define FAV_SECTION "Favorite"

/* 星星字形 13x13 (面板按钮里那个是 11x11, 画在单位头顶要再大一圈)。 */
#define FAV_STAR_W 13
#define FAV_STAR_H 13
/* 一槽每帧最多画几颗星: 大组选中的星星会糊成一片, 而且每颗星要 300 次像素
 * 混合, 上限按"看得清"定, 不按槽容量定 (箭头/跳转仍用整组)。 */
#define FAV_STAR_MAX 32
static const char* const k_FavStar[FAV_STAR_H] = {
    ".............",
    "......X......",
    ".....XXX.....",
    ".....XXX.....",
    "XXXXXXXXXXXXX",
    ".XXXXXXXXXXX.",
    "..XXXXXXXXX..",
    "...XXXXXXX...",
    "...XXXXXXX...",
    "..XX.....XX..",
    ".XX.......XX.",
    ".X.........X.",
    "............."
};

typedef struct {
    void* obj;
    int   id;           /* Fetch_ID; 0 = 索引取不到, 只靠字节判据 */
} FavMem;

typedef struct {
    FavMem m[FAV_GROUP_MAX];
    int n;
} FavSlot;

/* ---------------- 配置 / 状态 ---------------- */

static int s_Init = 0;
static int s_Enable = 1;
static int s_HoldMs = 600;      /* 按住多久算"长按清空" */
static int s_Lift = 150;        /* 星星在单位中心上方多少 leptons */
static int s_LiftPx = 25;       /* 投影后再上移的像素 (贴地/建筑高矮不一致时统一抬高) */
static int s_ArrowInset = 16;   /* 边缘箭头离视图边的像素 */
static int s_ArrowLen = 11;     /* 箭头大小 (像素) */
static int s_ArrowCenter = 0;   /* 1 = 箭头画在画面中心的同心圆轨道上 */
static int s_CenterR = 36;      /* 最内圈半径 (像素) */
static int s_CenterGap = 26;    /* 每个槽向外多一圈的增量 (像素) */
static int s_ArrowLite = 15;    /* 一槽离屏箭头超过这么多个就整体半透明 */
static int s_ArrowMax = 40;     /* 一槽最多画这么多个箭头, 多出的不画 */
static int s_BeaconArrow = 1;   /* 为离屏的通信信标也画箭头 (用各自玩家的颜色) */
static int s_GroupMax = FAV_GROUP_MAX;
static int s_Rgb[FAV_SLOT_MAX] = { 0x40E060, 0xFFD040, 0xFF4838, 0x40D8F0 };

static FavSlot s_Slot[FAV_SLOT_MAX];
static int s_Used[FAV_SLOT_MAX];          /* 槽里有收藏 (n>0 时才置 1) */
static int s_Reveal = 1;                  /* 该露几个收藏按钮 (1..FAV_SLOT_MAX) */
/* 按钮按住状态: -1 = 没按住。s_PressDone = 本次按住已有结论 (收藏/清空),
 * 松开不再跳转。 */
static int s_PressSlot = -1;
static unsigned s_PressT0 = 0;
static int s_PressDone = 0;
static void* s_LastScen = NULL;
static int s_ScenSeen = 0;

/* ---------------- 小工具 ---------------- */

static int Fav_Min(int a, int b) { return a < b ? a : b; }
static int Fav_Max(int a, int b) { return a > b ? a : b; }

static unsigned Fav_Now(void)
{
    unsigned t = GetTickCount();
    return t ? t : 1;                /* 0 留给"没有" */
}

/* 整数平方根 (Newtown 迭代, 无 libm)。 */
static int Fav_ISqrt(int v)
{
    int x, y;
    if (v <= 0) return 0;
    x = v;
    y = (x + 1) >> 1;
    while (y < x) { x = y; y = (x + v / x) >> 1; }
    return x;
}

/* 三角形填充 (三个顶点任意方向, 重心坐标判内外)。 */
static void Fav_TriFill(unsigned char* base, int pitch, int W, int H,
                        int x0, int y0, int x1, int y1, int x2, int y2,
                        unsigned r, unsigned g, unsigned b, unsigned a)
{
    int minx, maxx, miny, maxy, x, y;
    long long w0, w1, w2;

    minx = Fav_Min(x0, Fav_Min(x1, x2));
    maxx = Fav_Max(x0, Fav_Max(x1, x2));
    miny = Fav_Min(y0, Fav_Min(y1, y2));
    maxy = Fav_Max(y0, Fav_Max(y1, y2));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > W - 1) maxx = W - 1;
    if (maxy > H - 1) maxy = H - 1;
    if (minx > maxx || miny > maxy) return;
    if (maxx - minx > 64 || maxy - miny > 64) return;   /* 防脏顶点 */
    for (y = miny; y <= maxy; y++)
        for (x = minx; x <= maxx; x++)
        {
            w0 = (long long)(x1 - x0) * (y - y0) - (long long)(y1 - y0) * (x - x0);
            w1 = (long long)(x2 - x1) * (y - y1) - (long long)(y2 - y1) * (x - x1);
            w2 = (long long)(x0 - x2) * (y - y2) - (long long)(y0 - y2) * (x - x2);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                OU_Put(base, W, H, pitch, NULL, x, y, r, g, b, a);
        }
}

/* 星星: 先画一圈深色描边 (亮地形/雪地上单色星看不清), 再压彩色芯。 */
static void Fav_StarAt(unsigned char* base, int pitch, int W, int H,
                       int x, int y, unsigned r, unsigned g, unsigned b)
{
    static const int ox[4] = { -1, 1, 0, 0 };
    static const int oy[4] = { 0, 0, -1, 1 };
    int j, i, k;
    for (k = 0; k < 4; k++)
        for (j = 0; j < FAV_STAR_H; j++)
            for (i = 0; i < FAV_STAR_W; i++)
                if (k_FavStar[j][i] == 'X')
                    OU_Put(base, W, H, pitch, NULL,
                           x + i + ox[k], y + j + oy[k], 0, 0, 0, 205);
    for (j = 0; j < FAV_STAR_H; j++)
        for (i = 0; i < FAV_STAR_W; i++)
            if (k_FavStar[j][i] == 'X')
                OU_Put(base, W, H, pitch, NULL, x + i, y + j, r, g, b, 255);
}

/* 指向 (dx,dy) 的箭头, 尖端落在 (ex,ey)。深色描边 + 彩色芯。a = 整体不透明度。 */
static void Fav_ArrowAt(unsigned char* base, int pitch, int W, int H,
                        int ex, int ey, int dx, int dy,
                        unsigned r, unsigned g, unsigned b, unsigned a)
{
    int len, ux, uy, px, py, L, hw;
    int tx, ty, bx, by, p1x, p1y, p2x, p2y;
    unsigned oa;

    len = Fav_ISqrt(dx * dx + dy * dy);
    if (len <= 0) return;
    ux = dx * 256 / len;
    uy = dy * 256 / len;
    px = -uy;
    py = ux;
    L = s_ArrowLen;
    hw = L * 5 / 11;
    if (hw < 3) hw = 3;

    /* 尖端略微探出视图边, 视觉上"指向外面" */
    tx = ex + ux * 3 / 256;
    ty = ey + uy * 3 / 256;
    bx = ex - ux * L / 256;
    by = ey - uy * L / 256;
    p1x = bx + px * hw / 256;
    p1y = by + py * hw / 256;
    p2x = bx - px * hw / 256;
    p2y = by - py * hw / 256;
    oa = (205u * a) / 255u;
    /* 描边: 整体往外扩 1px */
    Fav_TriFill(base, pitch, W, H,
                tx + ux / 256, ty + uy / 256,
                p1x + px / 256, p1y + py / 256,
                p2x - px / 256, p2y - py / 256,
                0, 0, 0, oa);
    Fav_TriFill(base, pitch, W, H, tx, ty, p1x, p1y, p2x, p2y, r, g, b, a);
}

/* ---------------- 配置 ---------------- */

void Fav_EnsureInit(void)
{
    static unsigned s_RetryMs = 0;
    char buf[192];
    unsigned q = 0;
    int i;

    if (s_Init) return;
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
    s_Enable = GetPrivateProfileIntA(FAV_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    s_HoldMs = GetPrivateProfileIntA(FAV_SECTION, "HoldMs", 600, g_IniPath);
    s_HoldMs = Fav_Max(150, Fav_Min(3000, s_HoldMs));
    s_Lift = GetPrivateProfileIntA(FAV_SECTION, "LiftLeptons", 150, g_IniPath);
    s_Lift = Fav_Max(-512, Fav_Min(2048, s_Lift));
    s_LiftPx = GetPrivateProfileIntA(FAV_SECTION, "StarLiftPx", 25, g_IniPath);
    s_LiftPx = Fav_Max(-200, Fav_Min(400, s_LiftPx));
    s_ArrowInset = GetPrivateProfileIntA(FAV_SECTION, "ArrowInset", 16, g_IniPath);
    s_ArrowInset = Fav_Max(0, Fav_Min(200, s_ArrowInset));
    s_ArrowLen = GetPrivateProfileIntA(FAV_SECTION, "ArrowSize", 11, g_IniPath);
    s_ArrowLen = Fav_Max(6, Fav_Min(40, s_ArrowLen));
    {
        char mode[16];
        OU_ReadString(FAV_SECTION, "ArrowMode", "edge", mode, sizeof(mode));
        s_ArrowCenter = (mode[0] == 'c' || mode[0] == 'C') ? 1 : 0;
    }
    s_CenterR = GetPrivateProfileIntA(FAV_SECTION, "CenterRadius", 36, g_IniPath);
    s_CenterR = Fav_Max(8, Fav_Min(400, s_CenterR));
    s_CenterGap = GetPrivateProfileIntA(FAV_SECTION, "CenterGap", 26, g_IniPath);
    s_CenterGap = Fav_Max(4, Fav_Min(200, s_CenterGap));
    s_ArrowLite = GetPrivateProfileIntA(FAV_SECTION, "ArrowLite", 15, g_IniPath);
    s_ArrowLite = Fav_Max(1, Fav_Min(FAV_GROUP_MAX, s_ArrowLite));
    s_ArrowMax = GetPrivateProfileIntA(FAV_SECTION, "ArrowMax", 40, g_IniPath);
    s_ArrowMax = Fav_Max(1, Fav_Min(FAV_GROUP_MAX, s_ArrowMax));
    s_BeaconArrow = GetPrivateProfileIntA(FAV_SECTION, "BeaconArrow", 1, g_IniPath) ? 1 : 0;
    s_GroupMax = GetPrivateProfileIntA(FAV_SECTION, "GroupMax", FAV_GROUP_MAX, g_IniPath);
    s_GroupMax = Fav_Max(1, Fav_Min(FAV_GROUP_MAX, s_GroupMax));
    for (i = 0; i < FAV_SLOT_MAX; i++)
    {
        char key[8];
        key[0] = 'C'; key[1] = 'o'; key[2] = 'l'; key[3] = 'o'; key[4] = 'r';
        key[5] = (char)('1' + i);
        key[6] = 0;
        s_Rgb[i] = GetPrivateProfileIntA(FAV_SECTION, key, s_Rgb[i], g_IniPath) & 0xFFFFFF;
    }
    if (!g_Log) return;
    q = AppendCStr(buf, q, "fav: enable=", sizeof(buf));
    q += UtoA((unsigned)s_Enable, buf + q);
    q = AppendCStr(buf, q, " hold=", sizeof(buf));
    q += UtoA((unsigned)s_HoldMs, buf + q);
    q = AppendCStr(buf, q, " lift=", sizeof(buf));
    q += UtoA((unsigned)s_Lift, buf + q);
    q = AppendCStr(buf, q, "/", sizeof(buf));
    q += UtoA((unsigned)s_LiftPx, buf + q);
    q = AppendCStr(buf, q, " arrow=", sizeof(buf));
    q = AppendCStr(buf, q, s_ArrowCenter ? "center" : "edge", sizeof(buf));
    q = AppendCStr(buf, q, " lite=", sizeof(buf));
    q += UtoA((unsigned)s_ArrowLite, buf + q);
    q = AppendCStr(buf, q, " max=", sizeof(buf));
    q += UtoA((unsigned)s_ArrowMax, buf + q);
    q = AppendCStr(buf, q, " bc=", sizeof(buf));
    q += UtoA((unsigned)s_BeaconArrow, buf + q);
    q = AppendCStr(buf, q, " grp=", sizeof(buf));
    q += UtoA((unsigned)s_GroupMax, buf + q);
    LogLine(buf);
}

static void Fav_Reset(void)
{
    int i;
    for (i = 0; i < FAV_SLOT_MAX; i++)
    {
        s_Slot[i].n = 0;
        s_Used[i] = 0;
    }
    s_Reveal = 1;
    s_PressSlot = -1;
    s_PressDone = 0;
}

/* ---------------- 验活 ---------------- */

/* 对象还活着吗。id>0 时再按 Fetch_ID 索引反查一遍 (指针被复用时它能兜住);
 * 索引取不到 (id==0) 就只留字节判据, 不因此丢掉收藏。 */
static int Fav_Alive(void* o, int id)
{
    void** vt;
    if (!o || !IsPlausiblePtr(o)) return 0;
    vt = *(void***)o;
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    if (!((unsigned char*)o)[OFF_ISALIVE]) return 0;
    if (((unsigned char*)o)[OFF_INLIMBO]) return 0;
    if (*(int*)((char*)o + OFF_HEALTH) <= 0) return 0;
    if (id > 0) return OU_PtrAliveRaw(o, id);
    return 1;
}

/* 该格是否已揭示 (黑幕下不画标记)。格取不到按"看不见"处理。 */
static int Fav_CellHidden(int lx, int ly)
{
    void* cell;
    unsigned flags;
    int cx = lx >> 8, cy = ly >> 8;
    if (cx < 0 || cy < 0) return 1;
    cell = OU_CellPtr(cx, cy);
    if (!cell || !IsPlausiblePtr(cell)) return 1;
    flags = *(unsigned*)((char*)cell + OFF_CELL_ALTFLAGS);
    return (flags & CELL_SHROUD_REVEALED) ? 0 : 1;
}

static void Fav_Prune(void)
{
    int s, i, n;
    for (s = 0; s < FAV_SLOT_MAX; s++)
    {
        FavSlot* f = &s_Slot[s];
        if (f->n <= 0) { s_Used[s] = 0; continue; }
        n = 0;
        for (i = 0; i < f->n; i++)
            if (Fav_Alive(f->m[i].obj, f->m[i].id)) f->m[n++] = f->m[i];
        if (n != f->n)
        {
            char buf[80];
            unsigned q = 0;
            f->n = n;
            if (n <= 0) s_Used[s] = 0;
            q = AppendCStr(buf, q, "fav: slot ", sizeof(buf));
            q += UtoA((unsigned)(s + 1), buf + q);
            q = AppendCStr(buf, q, " -> ", sizeof(buf));
            q += UtoA((unsigned)n, buf + q);
            LogLine(buf);
        }
    }
}

/* 换图清空。s_LastScen 记的是 ScenarioClass 指针, 换局/换图都会变。 */
static void Fav_ScenCheck(void)
{
    void* sc = *(void**)ADDR_SCENARIO;
    if (!s_ScenSeen)
    {
        s_ScenSeen = 1;
        s_LastScen = sc;
        return;
    }
    if (sc == s_LastScen) return;
    s_LastScen = sc;
    Fav_Reset();
    LogLine("fav: reset (scenario)");
}

/* 收藏状态每循环维护: 换图清空 + 成员验活剪枝; 开关/未开局时把按住状态丢掉。 */
void Fav_Poll(void)
{
    Fav_EnsureInit();
    Fav_ScenCheck();
    if (!s_Enable)
    {
        s_PressSlot = -1;
        return;
    }
    if (!*(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED)
    {
        s_PressSlot = -1;
        return;
    }
    Fav_Prune();
}

/* ---------------- 对外查询 ---------------- */

int Fav_RevealCount(void)
{
    Fav_EnsureInit();
    if (!s_Enable) return 0;
    return s_Reveal;
}

int Fav_Used(int slot)
{
    Fav_EnsureInit();
    if (slot < 0 || slot >= FAV_SLOT_MAX) return 0;
    return s_Used[slot];
}

void Fav_Color(int slot, unsigned* r, unsigned* g, unsigned* b)
{
    int v;
    Fav_EnsureInit();
    if (slot < 0 || slot >= FAV_SLOT_MAX) slot = 0;
    v = s_Rgb[slot];
    if (r) *r = (unsigned)((v >> 16) & 0xFF);
    if (g) *g = (unsigned)((v >> 8) & 0xFF);
    if (b) *b = (unsigned)(v & 0xFF);
}

/* ---------------- 收藏 / 跳转 ---------------- */

static void Fav_Store(int slot, void** sel, int selN)
{
    FavSlot* f = &s_Slot[slot];
    int i, n = 0;
    f->n = 0;
    for (i = 0; i < selN && n < s_GroupMax; i++)
    {
        void* o = sel[i];
        int id, k, dup = 0;
        if (!o || !IsPlausiblePtr(o)) continue;
        if (!((unsigned char*)o)[OFF_ISALIVE]) continue;
        for (k = 0; k < n; k++) if (f->m[k].obj == o) { dup = 1; break; }
        if (dup) continue;
        id = OU_ObjId(o);
        if (id > 0 && !OU_PtrAliveRaw(o, id)) id = 0;   /* 索引不认 → 只走字节判据 */
        f->m[n].obj = o;
        f->m[n].id = id;
        n++;
    }
    f->n = n;
    s_Used[slot] = (n > 0) ? 1 : 0;
    if (n > 0 && s_Reveal < FAV_SLOT_MAX && slot + 1 >= s_Reveal)
        s_Reveal = Fav_Min(FAV_SLOT_MAX, slot + 2);
    {
        char buf[80];
        unsigned q = 0;
        q = AppendCStr(buf, q, "fav: store slot ", sizeof(buf));
        q += UtoA((unsigned)(slot + 1), buf + q);
        q = AppendCStr(buf, q, " n=", sizeof(buf));
        q += UtoA((unsigned)n, buf + q);
        q = AppendCStr(buf, q, " trunc=", sizeof(buf));
        q += UtoA((unsigned)((selN > n) ? (selN - n) : 0), buf + q);
        q = AppendCStr(buf, q, " reveal=", sizeof(buf));
        q += UtoA((unsigned)s_Reveal, buf + q);
        LogLine(buf);
    }
}

/* 组中心 → 本地摄像机。只读坐标 + 只动摄像机, 不写模拟、不发事件。 */
static int Fav_Jump(int slot)
{
    FavSlot* f = &s_Slot[slot];
    void* tac = *(void**)ADDR_TACTICAL;
    typedef void (__attribute__((thiscall)) *FnSetTacPos)(void*, const CoordStruct*);
    CoordStruct c;
    long long sx = 0, sy = 0, sz = 0;
    int i, n = 0, z = 0;

    if (!tac || !IsPlausiblePtr(tac)) return 0;
    for (i = 0; i < f->n; i++)
    {
        CoordStruct t;
        if (!Fav_Alive(f->m[i].obj, f->m[i].id)) continue;
        if (!OU_GetCoords(f->m[i].obj, &t)) continue;
        sx += t.X; sy += t.Y; sz += t.Z; n++;
    }
    if (!n) return 0;
    c.X = (int)(sx / n);
    c.Y = (int)(sy / n);
    c.Z = (int)(sz / n);
    /* 相机要落在地面上: 飞行单位的 Z 不能直接拿来定视口高度。 */
    if (OU_SurfaceZAt(c.X, c.Y, c.Z, &z)) c.Z = z;
    ((FnSetTacPos)(size_t)ADDR_SET_TACTICAL_POS)(tac, &c);
    {
        char buf[64];
        unsigned q = 0;
        q = AppendCStr(buf, q, "fav: jump slot ", sizeof(buf));
        q += UtoA((unsigned)(slot + 1), buf + q);
        LogLine(buf);
    }
    return 1;
}

/* 星星按钮按下沿。Ctrl = 立即收当前选中集; 否则只记起点, 由 Fav_PressPoll
 * 决定是长按清空还是松开跳转。 */
void Fav_PressBegin(int slot, void** sel, int selN, int ctrl)
{
    Fav_EnsureInit();
    if (!s_Enable) return;
    if (slot < 0 || slot >= FAV_SLOT_MAX) return;
    if (slot >= s_Reveal) return;            /* 还没露出来的槽不响应 */
    s_PressSlot = slot;
    s_PressT0 = Fav_Now();
    s_PressDone = 0;
    if (!ctrl) return;
    s_PressDone = 1;                         /* Ctrl 语义只收藏, 不参与长按 */
    if (!sel || selN <= 0)
    {
        LogLine("fav: store skip (no selection)");
        return;
    }
    Fav_Store(slot, sel, selN);
}

void Fav_PressPoll(void)
{
    int lb, slot;
    unsigned now;

    if (s_PressSlot < 0) return;
    if (!s_Enable || !*(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED)
    {
        s_PressSlot = -1;
        return;
    }
    lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    slot = s_PressSlot;
    if (lb)
    {
        if (s_PressDone || !s_Used[slot]) return;
        now = Fav_Now();
        if ((unsigned)(now - s_PressT0) < (unsigned)s_HoldMs) return;
        s_PressDone = 1;
        s_Slot[slot].n = 0;
        s_Used[slot] = 0;
        {
            char buf[64];
            unsigned q = 0;
            q = AppendCStr(buf, q, "fav: clear slot ", sizeof(buf));
            q += UtoA((unsigned)(slot + 1), buf + q);
            LogLine(buf);
        }
        return;
    }
    /* 松开: 没被 Ctrl/长按吃掉才跳转 */
    s_PressSlot = -1;
    if (!s_PressDone) Fav_Jump(slot);
}

void Fav_ToggleArrowMode(void)
{
    Fav_EnsureInit();
    if (!s_Enable) return;
    s_ArrowCenter = s_ArrowCenter ? 0 : 1;
    LogLine(s_ArrowCenter ? "fav: arrow=center" : "fav: arrow=edge");
}

/* ---------------- 绘制: 星星 + 指向箭头 (分两趟) ---------------- */

/* 成员投影 (含黑幕判据); 成功回 1。px/py 是未加 StarLiftPx 的投影点。 */
static int Fav_MemberProject(int slot, int i, int* px, int* py)
{
    CoordStruct c;
    if (!OU_GetCoords(s_Slot[slot].m[i].obj, &c)) return 0;
    if (Fav_CellHidden(c.X, c.Y)) return 0;
    if (!OU_ProjWorld(c.X, c.Y, c.Z + s_Lift, px, py)) return 0;
    return 1;
}

static int Fav_InView(const RectangleStruct* a, int px, int py)
{
    return (px >= a->X && px < a->X + a->Width &&
            py >= a->Y && py < a->Y + a->Height) ? 1 : 0;
}

/* 一槽的星星: 每个在画面上的成员一颗, 最多 FAV_STAR_MAX 颗。 */
static void Fav_DrawSlotStars(unsigned char* base, int pitch, int w, int h,
                              const RectangleStruct* a, int slot)
{
    FavSlot* f = &s_Slot[slot];
    unsigned r, g, b;
    int i, drawn = 0;

    Fav_Color(slot, &r, &g, &b);
    for (i = 0; i < f->n && drawn < FAV_STAR_MAX; i++)
    {
        int px, py;
        if (!Fav_MemberProject(slot, i, &px, &py)) continue;
        py -= s_LiftPx;
        if (!Fav_InView(a, px, py)) continue;
        drawn++;
        Fav_StarAt(base, pitch, w, h,
                   px - FAV_STAR_W / 2, py - FAV_STAR_H / 2, r, g, b);
    }
}

/* center 模式的圈参数: 收藏四槽各占一圈 (0..3), 信标接在外面。
 * 视图装不下四圈时压缩圈距; 回可用最大半径, 0 = 太小不画。 */
static int Fav_RingPlan(const RectangleStruct* a, int* rIn, int* rStep)
{
    int maxR = Fav_Min(a->Width, a->Height) / 2 - s_ArrowInset;

    *rIn = s_CenterR;
    *rStep = s_CenterGap;
    if (maxR < 12) return 0;
    if (*rIn + *rStep * (FAV_SLOT_MAX - 1) > maxR)
    {
        int span;
        if (*rIn > maxR / 2) *rIn = maxR / 2;
        span = maxR - *rIn;
        *rStep = span / (FAV_SLOT_MAX - 1);
        if (*rStep < 4) *rStep = 4;
    }
    return maxR;
}

/* 箭尖落点: center = 以视图中心为圆心、半径 radius, 沿目标方向;
 * edge = 射线与内缩 inset 像素的视图矩形求交。dx/dy 回目标方向 (未归一化)。 */
static int Fav_ArrowAnchor(const RectangleStruct* a, int px, int py,
                           int radius, int inset,
                           int* ex, int* ey, int* dx, int* dy)
{
    int acx = a->X + a->Width / 2, acy = a->Y + a->Height / 2;
    int rx = px - acx, ry = py - acy;

    *dx = rx;
    *dy = ry;
    if (!rx && !ry) return 0;
    if (s_ArrowCenter)
    {
        int len = Fav_ISqrt(rx * rx + ry * ry);
        if (len <= 0 || radius <= 0) return 0;
        *ex = acx + (int)((long long)rx * radius / len);
        *ey = acy + (int)((long long)ry * radius / len);
    }
    else
    {
        int ax = (rx < 0) ? -rx : rx;
        int ay = (ry < 0) ? -ry : ry;
        int hw = a->Width / 2 - inset;
        int hh = a->Height / 2 - inset;
        if (hw < 8) hw = 8;
        if (hh < 8) hh = 8;
        if ((long long)ax * hh >= (long long)ay * hw)
        {
            *ex = acx + (rx > 0 ? hw : -hw);
            *ey = acy + (int)((long long)ry * hw / (ax ? ax : 1));
        }
        else
        {
            *ey = acy + (ry > 0 ? hh : -hh);
            *ex = acx + (int)((long long)rx * hh / (ay ? ay : 1));
        }
    }
    return 1;
}

/* 一槽的箭头: 每个离屏成员一个, ArrowMax 之外的丢掉; 超过 ArrowLite 个整体半透明。
 * edge = 贴在屏幕边缘; center = 每个槽一个半径 (slot 越大圈越大), 同槽的箭头
 * 都在自己那圈上按各自方向滑动, 四槽仍构成同心圆。 */
static void Fav_DrawSlotArrows(unsigned char* base, int pitch, int w, int h,
                               const RectangleStruct* a, int slot)
{
    FavSlot* f = &s_Slot[slot];
    unsigned r, g, b, alpha;
    int i, count = 0, drawn = 0;
    int maxR = 0, rIn = 0, rStep = 0;

    /* 先数离屏成员: 决定半透明与上限 */
    for (i = 0; i < f->n; i++)
    {
        int px, py;
        if (!Fav_MemberProject(slot, i, &px, &py)) continue;
        if (Fav_InView(a, px, py)) continue;
        count++;
    }
    if (count <= 0) return;
    alpha = (count > s_ArrowLite) ? 128u : 255u;

    if (s_ArrowCenter)
    {
        maxR = Fav_RingPlan(a, &rIn, &rStep);
        if (!maxR) return;
    }
    Fav_Color(slot, &r, &g, &b);
    for (i = 0; i < f->n && drawn < s_ArrowMax; i++)
    {
        int px, py, dx, dy, ex, ey, rr = 0;
        if (!Fav_MemberProject(slot, i, &px, &py)) continue;
        if (Fav_InView(a, px, py)) continue;
        if (s_ArrowCenter)
        {
            rr = rIn + slot * rStep;
            if (rr > maxR) rr = maxR;
        }
        if (!Fav_ArrowAnchor(a, px, py, rr, s_ArrowInset, &ex, &ey, &dx, &dy)) continue;
        Fav_ArrowAt(base, pitch, w, h, ex, ey, dx, dy, r, g, b, alpha);
        drawn++;
    }
}

/* ---------------- 通信信标 (B 键) 的箭头 ----------------
 * 信标用各自玩家的颜色: HouseClass::ColorSchemeIndex → ColorScheme::Array[..]
 * 的 Colors[MainShadeIndex], 与 ESC 玩家色色块、单位颜色是同一套。
 * 只给本机看得见的信标画: 先问引擎 BeaconClass::VisibleToPlayer (0x4308B0,
 * 只读: 已放置 + 与本家同盟 + 该房可见), 再叠本地黑幕判据 —— 不能靠箭头把
 * 己方看不到的信标位置漏出来。 */
#define BC_HOUSE_MAX    8          /* BeaconManagerClass::Beacons[8][3] */
#define BC_SLOT_MAX     3
#define BC_MAX          (BC_HOUSE_MAX * BC_SLOT_MAX)

typedef unsigned char (__attribute__((thiscall)) *Fn_BcVisible)(void* self);

typedef struct {
    int px, py;         /* 屏幕投影 (离屏) */
    unsigned r, g, b;   /* 该信标所属玩家的颜色 */
} FavBcMark;

static int Fav_BeaconVisible(void* b)
{
    static Fn_BcVisible fn = NULL;
    /* 引擎函数直接解引用本家房指针, 开局前它可能是空。 */
    if (!*(void**)(size_t)ADDR_CURPLAYER) return 0;
    if (!fn) fn = (Fn_BcVisible)(void*)FN_BC_VISIBLE;
    return fn(b) ? 1 : 0;
}

/* 每个离屏的、本机看得见的信标一个箭头, 用该玩家的颜色。
 * center 模式下信标接在收藏四槽的圈外面; 同向挤在一起时往外多走一层错开。 */
int Fav_DrawBeaconArrows(void* surf, int w, int h, int pitch)
{
    RectangleStruct a;
    unsigned char* base;
    FavBcMark mk[BC_MAX];
    int mx[BC_MAX], my[BC_MAX];
    int i, n = 0, placed = 0, rIn = 0, rStep = 0, maxR = 0, sep;

    Fav_EnsureInit();
    Fav_ScenCheck();
    if (!s_Enable || !s_BeaconArrow) return 0;
    if (!g_Calibrated) return 0;
    if (!*(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED) return 0;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    if (w <= 0 || h <= 0 || pitch < w * 2) return 0;
    FM_GetViewRects(&a, NULL);
    if (a.Width < 64 || a.Height < 64) return 0;

    for (i = 0; i < BC_MAX; i++)
    {
        unsigned char* b = *(unsigned char**)((size_t)ADDR_BEACON_MGR + (size_t)i * 4u);
        int px, py, hid, r, g, bl;

        if (!b || !IsPlausiblePtr(b)) continue;
        if (!(b[OFF_BC_BITFIELD] & 1)) continue;        /* 只认已放置的槽 */
        hid = *(int*)(b + OFF_BC_HOUSEID);
        if (!OU_HouseIndexColor(hid, &r, &g, &bl)) continue;
        if (!Fav_BeaconVisible(b)) continue;
        if (Fav_CellHidden(*(int*)b, *(int*)(b + 4))) continue;
        if (!OU_ProjWorld(*(int*)b, *(int*)(b + 4), *(int*)(b + 8), &px, &py)) continue;
        if (Fav_InView(&a, px, py)) continue;          /* 在画面上就有信标本体 */
        mk[n].px = px;
        mk[n].py = py;
        mk[n].r = (unsigned)r;
        mk[n].g = (unsigned)g;
        mk[n].b = (unsigned)bl;
        n++;
    }
    if (n <= 0) return 0;

    base = GtLock(surf);
    if (!base) return 0;
    sep = s_ArrowLen + 6;
    if (s_ArrowCenter)
    {
        maxR = Fav_RingPlan(&a, &rIn, &rStep);
        if (!maxR) { GtUnlock(surf); return 0; }
    }
    for (i = 0; i < n; i++)
    {
        int layer, ex = 0, ey = 0, dx = 0, dy = 0, ok = 0;

        for (layer = 0; layer <= 8; layer++)
        {
            int rr, inset, k, hit = 0;
            rr = rIn + (FAV_SLOT_MAX + layer) * rStep;
            if (s_ArrowCenter && rr > maxR) rr = maxR;
            inset = s_ArrowInset + layer * sep;
            if (!Fav_ArrowAnchor(&a, mk[i].px, mk[i].py, rr, inset,
                                 &ex, &ey, &dx, &dy)) break;
            ok = 1;
            for (k = 0; k < placed; k++)
                if (ex >= mx[k] - sep && ex <= mx[k] + sep &&
                    ey >= my[k] - sep && ey <= my[k] + sep) { hit = 1; break; }
            if (!hit) break;
            ok = 0;                                     /* 压在别人身上: 往外一层再试 */
        }
        if (!ok) continue;
        Fav_ArrowAt(base, pitch, w, h, ex, ey, dx, dy, mk[i].r, mk[i].g, mk[i].b, 255);
        mx[placed] = ex;
        my[placed] = ey;
        placed++;
    }
    GtUnlock(surf);
    return 1;
}

static int Fav_CanDraw(void* surf, int w, int h, int pitch)
{
    int slot, any = 0;
    Fav_EnsureInit();
    Fav_ScenCheck();
    if (!s_Enable) return 0;
    for (slot = 0; slot < FAV_SLOT_MAX; slot++)
        if (s_Used[slot] && s_Slot[slot].n > 0) any = 1;
    if (!any) return 0;
    if (!g_Calibrated) return 0;
    if (!*(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED) return 0;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    if (w <= 0 || h <= 0 || pitch < w * 2) return 0;
    return 1;
}

int Fav_DrawStars(void* surf, int w, int h, int pitch)
{
    RectangleStruct a;
    unsigned char* base;
    int slot;

    if (!Fav_CanDraw(surf, w, h, pitch)) return 0;
    FM_GetViewRects(&a, NULL);
    if (a.Width < 64 || a.Height < 64) return 0;
    base = GtLock(surf);
    if (!base) return 0;
    for (slot = 0; slot < FAV_SLOT_MAX; slot++)
    {
        if (!s_Used[slot] || s_Slot[slot].n <= 0) continue;
        Fav_DrawSlotStars(base, pitch, w, h, &a, slot);
    }
    GtUnlock(surf);
    return 1;
}

int Fav_DrawArrows(void* surf, int w, int h, int pitch)
{
    RectangleStruct a;
    unsigned char* base;
    int slot;

    if (!Fav_CanDraw(surf, w, h, pitch)) return 0;
    FM_GetViewRects(&a, NULL);
    if (a.Width < 64 || a.Height < 64) return 0;
    base = GtLock(surf);
    if (!base) return 0;
    for (slot = 0; slot < FAV_SLOT_MAX; slot++)
    {
        if (!s_Used[slot] || s_Slot[slot].n <= 0) continue;
        Fav_DrawSlotArrows(base, pitch, w, h, &a, slot);
    }
    GtUnlock(surf);
    return 1;
}