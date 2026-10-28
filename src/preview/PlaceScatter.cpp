/* PlaceScatter.cpp - 侧栏放建筑时把压住的自家地面单位向外驱离。
 *
 * Mode ([PlaceScatter]): 0=禁用; 1=点下时驱离 (默认, 晃幽影不碰单位);
 * 2=幽影跟随自动驱离。每次触发跑同一套逻辑。
 * 设计与偏移依据见 notes/PlaceScatter驱离.md。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "PlaceScatter.h"

#define PS_SECTION "PlaceScatter"

/* ---------------- 只读地址 (Display 基址与地图槽见 OrderUtil) ---------------- */

#define PS_OFF_CENTER 0x1174u    /* CellStruct: X +0 / Y +2 (鼠标格) */
#define PS_OFF_TOPLEFT 0x1178u   /* CellStruct: X +0 / Y +2 (中心到左上角) */
#define PS_OFF_DATA 0x117Cu      /* CellStruct* (空=不在放置模式) */

#define PS_ADDR_FRAME 0xA8ED84u

#define PS_ARR_UNIT 0x8B4108u
#define PS_ARR_INF 0xA83DE8u
#define ARR_ITEMS(base) (*(void***)((base) + 4u))
#define ARR_COUNT(base) (*(int*)((base) + 16u))

#define PS_OFF_HEALTH 0x6Cu
#define PS_OFF_INLIMBO 0x81u
#define PS_OFF_ISALIVE 0x90u
#define PS_OFF_OWNER 0x21Cu

/* 格子内容链: [cell+0xE4]=地面层首对象, [cell+0xE8]=桥面层首对象,
 * 链后继 [obj+0x30], WhatAmI 槽同 OrderUtil (+0x2C)。
 * 地形 [cell+0xEC]=LandType (Water=2/Rock=3, 不可走)。 */
#define PS_OFF_CELL_FIRST 0xE4u
#define PS_OFF_CELL_ALT 0xE8u
#define PS_OFF_OBJ_NEXT 0x30u
#define PS_OFF_CELL_LAND 0xECu
#define PS_OFF_CELL_WALLOWN 0x50u     /* WallOwnerIndex (-1=无覆盖物墙) */
#define LAND_WATER 2
#define LAND_ROCK 3
#define PS_ABS_TERRAIN 36

#define EVT_PLACE 0xBu          /* EventType::Place */
#define PS_OFF_EV_LOCX 0x13u    /* Place.Location.X (CellStruct, short) */
#define PS_OFF_EV_LOCY 0x15u    /* Place.Location.Y (CellStruct, short) */
#define PS_CACHE_FRESH 5        /* 点下时缓存至多是几帧前的幽影 */

#define PS_DATA_CAP 120         /* 引擎 CurrentFoundation_Data 上限 120 */
#define PS_CELL_CAP 128
#define PS_QCAP 64

/* ---------------- 配置 ---------------- */

static int g_PsInit = 0;
static int g_PsMode = 1;        /* 0=禁用 1=点下驱离 2=自动+点下 */
static int g_PsInterval = 30;   /* 悬停补扫间隔 (逻辑帧, 5~300) */
static int g_PsBatch = 16;      /* 每次触发最多发出的移动事件 (1~64) */
static int g_PsDist = 2;        /* 出框后再向外推的格数 (1~4) */

static void PS_EnsureInit(void)
{
    char buf[96];
    unsigned p = 0;
    if (g_PsInit) return;
    g_PsInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_PsMode = GetPrivateProfileIntA(PS_SECTION, "Mode", 1, g_IniPath);
    if (g_PsMode < 0) g_PsMode = 0;
    if (g_PsMode > 2) g_PsMode = 2;
    g_PsInterval = GetPrivateProfileIntA(PS_SECTION, "Interval", 30, g_IniPath);
    if (g_PsInterval < 5) g_PsInterval = 5;
    if (g_PsInterval > 300) g_PsInterval = 300;
    g_PsBatch = GetPrivateProfileIntA(PS_SECTION, "Batch", 16, g_IniPath);
    if (g_PsBatch < 1) g_PsBatch = 1;
    if (g_PsBatch > 64) g_PsBatch = 64;
    g_PsDist = GetPrivateProfileIntA(PS_SECTION, "Dist", 2, g_IniPath);
    if (g_PsDist < 1) g_PsDist = 1;
    if (g_PsDist > 4) g_PsDist = 4;
    p = AppendCStr(buf, p, "placescatter: mode=", sizeof(buf));
    p += UtoA((unsigned)g_PsMode, buf + p);
    p = AppendCStr(buf, p, " interval=", sizeof(buf));
    p += UtoA((unsigned)g_PsInterval, buf + p);
    p = AppendCStr(buf, p, " batch=", sizeof(buf));
    p += UtoA((unsigned)g_PsBatch, buf + p);
    p = AppendCStr(buf, p, " dist=", sizeof(buf));
    p += UtoA((unsigned)g_PsDist, buf + p);
    LogLine(buf);
}

/* ---------------- 幽影读取 (Display 三件套 + Data) ---------------- */

static int PS_ReadGhost(int cells[][2])
{
    short cx, cy, ox, oy;
    void* data;
    int bx, by, n = 0, k;
    cx = *(short*)(ADDR_DISPLAY + PS_OFF_CENTER);
    cy = *(short*)(ADDR_DISPLAY + PS_OFF_CENTER + 2);
    ox = *(short*)(ADDR_DISPLAY + PS_OFF_TOPLEFT);
    oy = *(short*)(ADDR_DISPLAY + PS_OFF_TOPLEFT + 2);
    data = *(void**)(ADDR_DISPLAY + PS_OFF_DATA);
    if (!data) return 0;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return 0;
    if (ox < -8 || ox > 8 || oy < -8 || oy > 8) return 0;
    if (!IsPlausiblePtr(data)) return 0;
    bx = (int)cx + (int)ox;
    by = (int)cy + (int)oy;
    for (k = 0; k < PS_DATA_CAP; k++)
    {
        short dx, dy;
        int gx, gy;
        dx = *(short*)((char*)data + (unsigned)k * 4u);
        dy = *(short*)((char*)data + (unsigned)k * 4u + 2u);
        if (dx == (short)0x7FFF && dy == (short)0x7FFF) break;
        if (dx < -10 || dx > 10 || dy < -10 || dy > 10) break;
        gx = bx + (int)dx;
        gy = by + (int)dy;
        if (gx < 0 || gy < 0 || gx >= 512 || gy >= 512) continue;
        cells[n][0] = gx;
        cells[n][1] = gy;
        n++;
    }
    return n;
}

static int PS_InSet(int cells[][2], int n, int cx, int cy)
{
    int i;
    for (i = 0; i < n; i++)
        if (cells[i][0] == cx && cells[i][1] == cy) return 1;
    return 0;
}

/* 当前 footprint 包围盒 (PS_ClearFoot 每次设定): 供 PS_InFoot 先做 O(1) 排除。 */
static int s_FootX0 = 0, s_FootY0 = 0, s_FootX1 = -1, s_FootY1 = -1;

/* 格是否在当前 footprint 内: 先包围盒短路 (绝大多数是框外格) 再线性比对。 */
static int PS_InFoot(int x, int y, int cells[][2], int n)
{
    if (x < s_FootX0 || x > s_FootX1 || y < s_FootY0 || y > s_FootY1) return 0;
    return PS_InSet(cells, n, x, y);
}

/* 包围盒 (nCell>0 时恒有效)。 */
static void PS_BBox(int cells[][2], int nCell, int* x0, int* y0, int* x1, int* y1)
{
    int k;
    *x0 = 512; *y0 = 512; *x1 = -1; *y1 = -1;
    for (k = 0; k < nCell; k++)
    {
        if (cells[k][0] < *x0) *x0 = cells[k][0];
        if (cells[k][1] < *y0) *y0 = cells[k][1];
        if (cells[k][0] > *x1) *x1 = cells[k][0];
        if (cells[k][1] > *y1) *y1 = cells[k][1];
    }
}

/* ---------------- 幽影缓存 (点下后幽影立即清空, 凭此还原 footprint) ---------------- */

static int s_CacheCells[PS_CELL_CAP][2];
static int s_CacheN = 0;
static int s_CacheFrame = -100000;

/* ---------------- 落点判定 ---------------- */

/* 单格障碍 (不含 footprint): 水岩/覆盖物墙/建筑占位/链内建筑地形; 单位飞机不挡。只读。
 * skipFnd: footprint 内格把幽灵地基占位 (AltFlags 0x2) 视为可走。放建筑悬停时引擎给
 * 当前地基逐格打 0x2 (SetActiveFoundation 0x4A8BF0 → MarkFoundation 0x4A95A0), 点下时
 * 先逐格清掉 (0x4ABC46 SetActiveFoundation(null)) 再算; 自动模式据活幽影得知 footprint,
 * 靠该豁免与点下同视。0x2 只被 CellClass 画格函数读 (渲染), 寻路读的是 OccupationFlags
 * (0x124)/AltOccupationFlags(0x128); 故这里不碰引擎位 (摘 0x2 会让该格渲染闪一下),
 * 只改本模块判定。 */
static int PS_BlockedEx(int cx, int cy, int skipFnd)
{
    void** cells;
    int maxc, idx, k;
    void* cell;
    int land;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return 1;
    cells = *(void***)(ADDR_DISPLAY + OFF_MAP_CELLS);
    maxc = *(int*)(ADDR_DISPLAY + OFF_MAP_MAXC);
    if (!cells || !IsPlausiblePtr(cells) || maxc <= 0 || maxc > 512 * 512) return 1;
    idx = (cy << 9) + cx;
    if (idx >= maxc) return 1;
    cell = cells[idx];
    if (!cell || !IsPlausiblePtr(cell)) return 1;
    land = *(int*)((char*)cell + PS_OFF_CELL_LAND);
    if (land == LAND_WATER || land == LAND_ROCK) return 1;
    /* 覆盖物墙不在对象链里, 认 WallOwnerIndex (-1=无墙)。 */
    if (*(int*)((char*)cell + PS_OFF_CELL_WALLOWN) != -1) return 1;
    /* 格内建筑占位标记, 与对象链双保险。 */
    if (!skipFnd && (*(unsigned*)((char*)cell + OFF_CELL_ALTFLAGS) & CELL_ALT_BUILDING)) return 1;
    for (k = 0; k < 2; k++)
    {
        void* o = (k == 0) ? *(void**)((char*)cell + PS_OFF_CELL_FIRST)
                           : *(void**)((char*)cell + PS_OFF_CELL_ALT);
        int n = 0;
        while (o && n < 32)
        {
            int w;
            if (!IsPlausiblePtr(o)) break;
            w = OU_WhatAmI(o);
            if (w == ABS_BUILDING || w == PS_ABS_TERRAIN) return 1;
            o = *(void**)((char*)o + PS_OFF_OBJ_NEXT);
            n++;
        }
    }
    return 0;
}

/* 单格可走 (地图有效且不被挡), footprint 内格不豁免 0x2 (落点/外推用)。
 * 越界/未加载格由 PS_BlockedEx 内判 (回 1=挡), 不必再 OU_CellValid 查一遍地图。 */
static int PS_Pass(int x, int y)
{
    return !PS_BlockedEx(x, y, 0);
}

/* BFS 展开用可走判定: 本次 footprint 内格豁免幽灵地基占位 0x2, 与点下同视。 */
static int PS_PassBfs(int x, int y, int cells[][2], int nCell)
{
    return !PS_BlockedEx(x, y, PS_InFoot(x, y, cells, nCell));
}

/* 对角步禁切角: 两正交邻格须都可走。 */
static int PS_DiagFree(int ux, int uy, int qx, int qy,
                       int cells[][2], int nCell)
{
    if (!PS_PassBfs(ux + qx, uy, cells, nCell)) return 0;
    if (!PS_PassBfs(ux, uy + qy, cells, nCell)) return 0;
    return 1;
}

/* 候选落点可用: 在框外且可走。 */
static int PS_CandOk(int cells[][2], int nCell, int x, int y)
{
    if (PS_InSet(cells, nCell, x, y)) return 0;
    if (!PS_Pass(x, y)) return 0;
    return 1;
}

/* 标准移动令 (与 DeployPreview/SmartOrder 同形)。 */
static int PS_FireMove(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstX, int dstY)
{
    char ev[EVT_SIZE];
    unsigned char dstR;
    int dstID = OU_CellTgt(dstX, dstY, &dstR);
    OU_MakeMega(ev, houseIdx, srcID, srcRtti, MISSION_MOVE,
                0, 0, dstID, dstR, 0, 0, 0);
    return OU_Enqueue(ev);
}

/* ---------------- 下令 (每次触发同一套) ---------------- */

/* 屏八方向表: N,NE,E,SE,S,SW,W,NW (菱形地图: 屏正上=格(-1,-1),
 * 屏正右=格(+1,-1), 屏正下=格(+1,+1), 屏正左=格(-1,+1))。 */
static const int PS_DDX[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };
static const int PS_DDY[8] = { -1, -1, -1, 0, 1, 1, 1, 0 };
/* BFS 邻居展开序: 反方向优先螺旋 (同向, ±45°, ±90°, ±135°, 反向)。 */
static const int PS_ROT[8] = { 0, 1, 7, 2, 6, 3, 5, 4 };

/* 屏符号 → 表序号 (N=0,NE=1,E=2,SE=3,S=4,SW=5,W=6,NW=7)。
 * 输入是屏横/纵符号, 须经此变换, 不可直接当格向量查表。 */
static int PS_ScreenIdx(int qx, int qy)
{
    if (qy < 0) return (qx < 0) ? 7 : ((qx > 0) ? 1 : 0);
    if (qy > 0) return (qx < 0) ? 5 : ((qx > 0) ? 3 : 4);
    return (qx < 0) ? 6 : 2;
}

/* BFS 最近可达框外格 (反方向优先, 半径 10/节点 384)。纯只读。 */
#define PS_BFS_CAP 384
#define PS_BFS_R 10
static int s_BfsX[PS_BFS_CAP];
static int s_BfsY[PS_BFS_CAP];

/* 访问位图: 窗口 (2R+1)² 每格 1 bit (56B), 替代每次邻居线性扫全表 (原 O(N²))。
 * 记"已处理过"(含被挡): 只读世界内同一格结果稳定, 去重安全。 */
#define PS_BFS_W (PS_BFS_R * 2 + 1)
static unsigned char s_BfsSeen[(PS_BFS_W * PS_BFS_W + 7) / 8];

/* 首次遇到 (x,y) 置位回 0, 已见过回 1 (窗外也回 1)。 */
static int PS_BfsSeen(int ux, int uy, int x, int y)
{
    int ix = x - ux + PS_BFS_R, iy = y - uy + PS_BFS_R;
    unsigned b;
    if (ix < 0 || iy < 0 || ix >= PS_BFS_W || iy >= PS_BFS_W) return 1;
    b = (unsigned)(iy * PS_BFS_W + ix);
    if (s_BfsSeen[b >> 3] & (unsigned char)(1u << (b & 7))) return 1;
    s_BfsSeen[b >> 3] = (unsigned char)(s_BfsSeen[b >> 3] | (unsigned char)(1u << (b & 7)));
    return 0;
}

static int PS_BFS(int cells[][2], int nCell, int ux, int uy, int base,
                  int* ox, int* oy)
{
    int head = 0, tail = 1, i, d, order[8];
    for (i = 0; i < (int)sizeof(s_BfsSeen); i++) s_BfsSeen[i] = 0;
    s_BfsX[0] = ux; s_BfsY[0] = uy;
    PS_BfsSeen(ux, uy, ux, uy);
    for (i = 0; i < 8; i++) order[i] = (base + PS_ROT[i]) & 7;
    while (head < tail)
    {
        int cx = s_BfsX[head], cy = s_BfsY[head];
        head++;
        if (!(cx == ux && cy == uy) && !PS_InFoot(cx, cy, cells, nCell))
        {
            *ox = cx; *oy = cy;
            return 1;
        }
        for (d = 0; d < 8; d++)
        {
            int qx = PS_DDX[order[d]], qy = PS_DDY[order[d]];
            int nx = cx + qx, ny = cy + qy;
            int ax, ay, rr;
            if (nx < 0 || ny < 0 || nx >= 512 || ny >= 512) continue;
            ax = nx - ux; if (ax < 0) ax = -ax;
            ay = ny - uy; if (ay < 0) ay = -ay;
            rr = (ax > ay) ? ax : ay;
            if (rr > PS_BFS_R) continue;   /* 窗外: 不标记不处理 */
            if (qx && qy && !PS_DiagFree(cx, cy, qx, qy, cells, nCell)) continue;
            if (PS_BfsSeen(ux, uy, nx, ny)) continue;   /* 首次遇到即置位 (含被挡) */
            if (!PS_PassBfs(nx, ny, cells, nCell)) continue;
            if (tail >= PS_BFS_CAP) return 0;
            s_BfsX[tail] = nx; s_BfsY[tail] = ny; tail++;
        }
    }
    return 0;
}

/* ---------------- 待发队列 (头索引消费, 避免搬移) ---------------- */

static void* s_PsQPtr[PS_QCAP];
static int s_PsQHead = 0;
static int s_PsQTail = 0;

static void PS_Push(void* o)
{
    if (s_PsQTail >= PS_QCAP) return;
    s_PsQPtr[s_PsQTail] = o;
    s_PsQTail++;
}

/* 取对象事件 ID (TargetFromObj 槽)。失败回 0。 */
static int PS_ObjID(void* o, int* id, unsigned char* rtti)
{
    char tb[8];
    unsigned i;
    for (i = 0; i < 8; i++) tb[i] = 0;
    if (!FN_TGT_OBJ(tb, o)) return 0;
    if (*(int*)(tb + 0) <= 0) return 0;
    *id = *(int*)(tb + 0);
    *rtti = (unsigned char)tb[4];
    return 1;
}

/* 扫一个 Techno 数组, 把压在 footprint 里的自家活单位入队。 */
static void PS_ScanArr(unsigned arrBase, void* cur, int cells[][2], int nCell)
{
    void** items;
    int count, i;
    items = ARR_ITEMS(arrBase);
    count = ARR_COUNT(arrBase);
    if (!items || !IsPlausiblePtr(items) || count <= 0 || count > 20000) return;
    for (i = 0; i < count; i++)
    {
        void* o = items[i];
        int w, hp, cx, cy;
        CoordStruct cs;
        if (s_PsQTail >= PS_QCAP) return;
        if (!o || !IsPlausiblePtr(o)) continue;
        if (*(void**)((char*)o + PS_OFF_OWNER) != cur) continue;
        if (!((unsigned char*)o)[PS_OFF_ISALIVE]) continue;
        if (((unsigned char*)o)[PS_OFF_INLIMBO]) continue;
        hp = *(int*)((char*)o + PS_OFF_HEALTH);
        if (hp <= 0) continue;
        w = OU_WhatAmI(o);   /* 虚调用, 放最后 (前面几项已滤掉大半对象) */
        if (w != ABS_UNIT && w != ABS_INFANTRY) continue;
        if (!OU_GetCoords(o, &cs)) continue;
        cx = cs.X / 256;
        cy = cs.Y / 256;
        if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) continue;
        if (!PS_InFoot(cx, cy, cells, nCell)) continue;
        PS_Push(o);
    }
}

/* 逐个下令: BFS 找落点 → 外推, 下 Move 令。OutList 满则停。 */
static int PS_Drain(void* cur, int houseIdx,
                    int cells[][2], int nCell, int cx2, int cy2)
{
    int sent = 0;
    while (s_PsQHead < s_PsQTail && sent < g_PsBatch)
    {
        void* o;
        int ux, uy, dx, dy, ox, oy, ok;
        CoordStruct cs;
        int id;
        unsigned char rtti;
        o = s_PsQPtr[s_PsQHead];
        s_PsQHead++;
        if (!o || !IsPlausiblePtr(o)) continue;
        if (*(void**)((char*)o + PS_OFF_OWNER) != cur) continue;
        if (!OU_GetCoords(o, &cs)) continue;
        ux = cs.X / 256; uy = cs.Y / 256;
        if (!PS_InFoot(ux, uy, cells, nCell))
            continue;   /* 已在框外: 不用动 */
        /* 屏反方向 (偶数宽恒有定义, 2x2 四格屏对角散开)。 */
        {
            int ddx = (ux * 2 + 1) - cx2;
            int ddy = (uy * 2 + 1) - cy2;
            int sx = ddx - ddy, sy = ddx + ddy;
            dx = (sx > 0) ? 1 : ((sx < 0) ? -1 : 0);
            dy = (sy > 0) ? 1 : ((sy < 0) ? -1 : 0);
        }
        if (!dx && !dy) dx = 1;   /* 正压中心默认屏右 */
        /* 第一圈直搜即 BFS 深度 1 (同展开序同结果), 直接跑 BFS。
         * 命中再沿出框方向外推 Dist-1 格 (逐格验)。 */
        ok = PS_BFS(cells, nCell, ux, uy, PS_ScreenIdx(dx, dy), &ox, &oy);
        if (ok)
        {
            int ex = (ox > ux) ? 1 : ((ox < ux) ? -1 : 0);
            int ey = (oy > uy) ? 1 : ((oy < uy) ? -1 : 0);
            int st, nx, ny;
            for (st = 1; st < g_PsDist; st++)
            {
                nx = ox + ex; ny = oy + ey;
                if (!PS_CandOk(cells, nCell, nx, ny)) break;
                ox = nx; oy = ny;
            }
        }
        if (!ok) continue;   /* 半径内没出路: 跳过, 下次触发重找 */
        if (!PS_ObjID(o, &id, &rtti)) continue;
        if (!PS_FireMove(houseIdx, id, rtti, ox, oy))
            return sent;   /* OutList 满: 剩下的丢到下次触发 */
        sent++;
    }
    return sent;
}

/* 一次驱离: 收集框内自家单位并逐个下令。回发出的条数。 */
static int PS_ClearFoot(void* cur, int houseIdx,
                        int cells[][2], int nCell)
{
    int x0, y0, x1, y1, cx2, cy2;
    if (nCell <= 0) return 0;
    PS_BBox(cells, nCell, &x0, &y0, &x1, &y1);
    s_FootX0 = x0; s_FootY0 = y0; s_FootX1 = x1; s_FootY1 = y1;
    cx2 = x0 + x1 + 1;   /* 包围盒 [x0,x1+1) 中心×2 */
    cy2 = y0 + y1 + 1;
    s_PsQHead = 0;
    s_PsQTail = 0;
    PS_ScanArr(PS_ARR_UNIT, cur, cells, nCell);
    PS_ScanArr(PS_ARR_INF, cur, cells, nCell);
    return PS_Drain(cur, houseIdx, cells, nCell, cx2, cy2);
}

/* ---------------- OutList 哨兵: 抓自家 Place 事件 (BuildQueue 同式) ---------------- */

static unsigned s_PsEvNext = 0;
static int s_PsEvInit = 0;

#define PS_EV_GUARD 64u   /* 单轮最多扫槽数; 上限须大于 Batch, 否则驱离的 Move 会挤掉 Place */

static void PS_WatchPlace(void* cur, int houseIdx, int fr)
{
    unsigned next = *(unsigned*)ADDR_OUT_NEXT;
    unsigned adv, p;
    int guard;
    if (!s_PsEvInit)
    {
        s_PsEvNext = next;
        s_PsEvInit = 1;
        return;
    }
    adv = (next - s_PsEvNext) & 0x7Fu;
    if (!adv) return;
    if (adv > PS_EV_GUARD)   /* 环形槽转半圈以上 (如读档/菜单): 重同步 */
    {
        s_PsEvNext = next;
        return;
    }
    p = s_PsEvNext;
    guard = 0;
    while (p != next && guard < (int)PS_EV_GUARD)
    {
        const unsigned char* e =
            (const unsigned char*)(ADDR_OUT_BUF + p * EVT_SIZE);
        if (e[0] == EVT_PLACE && (int)e[2] == houseIdx)
        {
            int d, fire = 0;
            short ex, ey;
            int nCell = s_CacheN;
            /* 用缓存还原点下时的 footprint (0~5 帧内有效, 防读档帧号回绕)。
             * 点下当帧引擎同步清幽影 (0x4ABC46 调 SetActiveFoundation(null)),
             * 到 Poll 时活幽影已不可读, 缓存是点下驱离的唯一依据。 */
            d = fr - s_CacheFrame;
            if (nCell > 0 && d >= 0 && d <= PS_CACHE_FRESH)
            {
                /* 事件自带 Location 须落在本次幽影内: 非点下之处的 Place
                 * (如自动展开等其他来源) 不得驱离。 */
                ex = *(short*)(e + PS_OFF_EV_LOCX);
                ey = *(short*)(e + PS_OFF_EV_LOCY);
                if (PS_InSet(s_CacheCells, nCell, (int)ex, (int)ey))
                    fire = 1;
                else
                {
                    int x0, y0, x1, y1;
                    PS_BBox(s_CacheCells, nCell, &x0, &y0, &x1, &y1);
                    if ((int)ex >= x0 && (int)ex <= x1
                        && (int)ey >= y0 && (int)ey <= y1)
                        fire = 1;
                }
            }
            if (fire)
                PS_ClearFoot(cur, houseIdx, s_CacheCells, nCell);
        }
        p = (p + 1u) & 0x7Fu;
        guard++;
    }
    s_PsEvNext = next;
}

/* ---------------- 每循环轮询 (挂 PowerDrawBoth) ---------------- */

static int s_PsLastFrame = -1;
static int s_PsLastAuto = -100000;

void PlaceScatter_Poll(void)
{
    int fr;
    int cells[PS_CELL_CAP][2];
    int nCell;
    void* cur;
    int houseIdx;
    PS_EnsureInit();
    if (!g_PsMode) return;
    /* PowerDrawBoth 每循环调多次, 按帧号门控每帧只跑一次。 */
    fr = *(int*)PS_ADDR_FRAME;
    if (fr == s_PsLastFrame) return;
    s_PsLastFrame = fr;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return;
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    /* 点下后幽影立即清空, 靠缓存还原 footprint。 */
    nCell = PS_ReadGhost(cells);
    if (nCell > 0)
    {
        int k;
        s_CacheN = nCell;
        for (k = 0; k < nCell; k++)
        {
            s_CacheCells[k][0] = cells[k][0];
            s_CacheCells[k][1] = cells[k][1];
        }
        s_CacheFrame = fr;

        /* Mode=2: 幽影跟随自动驱离。 */
        if (g_PsMode == 2 && fr - s_PsLastAuto >= g_PsInterval)
        {
            s_PsLastAuto = fr;
            PS_ClearFoot(cur, houseIdx, cells, nCell);
        }
    }
    /* 点下驱离: OutList 哨兵抓自家 Place 事件 (Mode=1/2 都有)。 */
    PS_WatchPlace(cur, houseIdx, fr);
}

/* GUI 设置菜单用的运行时模式/查询 (ESC 菜单 "放置时建筑驱逐单位")。 */
int PS_Mode(void)
{
    PS_EnsureInit();
    return g_PsMode;
}

void PS_SetMode(int mode)
{
    PS_EnsureInit();
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_PsMode = mode;
    LogLine(mode == 0 ? "placescatter: mode=0 (menu)"
                      : (mode == 1 ? "placescatter: mode=1 (menu)"
                                   : "placescatter: mode=2 (menu)"));
}
