/* GridView.cpp - 单机地图格网显示 (缓存 + 绘制, 只读 + 渲染面写入)。
 * 每格存自家 4 角地面高度 (0=NW 1=NE 2=SE 3=SW), 每格画自家 4 条边:
 * 平地共享边坐标相同、只画一次, 悬崖两侧坐标不同、各画一条 (崖顶+崖底,
 * 两端竖线连上下即崖面)。key (视口/视图区/缩放/场景) 连 150ms 不变才重建,
 * 移动帧隐藏旧缓存。配置见 [BottomBar] GridCullBack / GridShowBlocked,
 * 结论见 notes/地图格网.md。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "ScaleBar.h"
#include "FullMap.h"
#include "GridView.h"

#define GV_SECTION "BottomBar"

#define GV_ADDR_SCEN 0x00A8B230u /* ScenarioClass::Instance (换图看它) */

/* ---------------- 配置/状态 ---------------- */
static int s_GvInit = 0;
static int s_GridOn = 0;            /* 格网开关 (默认关, 点按切换) */
static int s_GridCullBack = 1;      /* [BottomBar] GridCullBack: 悬崖背面剔除, 默认开 */
static int s_GridShowBlocked = 1;   /* [BottomBar] GridShowBlocked: 不可走格边线画红, 默认开 */

void GV_EnsureInit(void)
{
    if (s_GvInit) return;
    s_GvInit = 1;
    BuildPaths();
    GtEnsureLog();
    if (g_Log)
    {
        s_GridCullBack = GetPrivateProfileIntA(GV_SECTION, "GridCullBack", 1, g_IniPath) ? 1 : 0;
        s_GridShowBlocked = GetPrivateProfileIntA(GV_SECTION, "GridShowBlocked", 1, g_IniPath) ? 1 : 0;
    }
}

int GV_CullBack(void) { return s_GridCullBack; }
int GV_ShowBlocked(void) { return s_GridShowBlocked; }
int GV_IsOn(void) { return s_GridOn; }

/* ---------------- 格网缓存 (逐格自有平面, 停稳重建) ---------------- */

#define GV_CAP_XY 160
#define GV_CAP_CELLS 20000
#define GV_STABLE_MS 150u    /* key 连续一致满此时长才判停稳 (去慢速卷屏抖动, 无感知) */
#define GV_RETRY_MS 500u   /* 重建瞬时失败后的重试间隔 (GetTickCount 真毫秒) */

typedef int __attribute__((thiscall)) (*Fn_GVC2C)(void* tac,
    const CoordStruct* c, Point2D* out);
typedef int __attribute__((thiscall)) (*Fn_GVFloorH)(void* cell, const Point2D* p);
#define FN_GV_C2C ((Fn_GVC2C)(void*)0x6D2140u)
#define FN_GV_FLOORH ((Fn_GVFloorH)(void*)0x47B3A0u)
#define GV_TAC_INSTANCE 0x00887324u
#define GV_VIEW_W 0xB0CE30u
#define GV_VIEW_H 0xB0CE34u
/* CellClass::Passability (YRpp GeneralDefinitions.h, WallOwnerIndex-4, 只读)。 */
#define GV_OFF_PASSABILITY 0x4Cu
/* CellClass::OverlayTypeIndex (-1=无) / WallOwnerIndex (-1=无覆盖物墙, 只读)。 */
#define GV_OFF_OVERLAYTYPE 0x44u
#define GV_OFF_WALLOWN 0x50u
/* 只读查询: Tile_Is_Cliff (0x4863D0, 悬崖图块) / GetTiberiumType (0x5FDD20, >=0=矿)。 */
typedef int __attribute__((thiscall)) (*Fn_GVIsCliff)(void* cell);
#define FN_GV_ISCLIFF ((Fn_GVIsCliff)(void*)0x4863D0u)
typedef int (__attribute__((fastcall)) *Fn_GVTibType)(int idx);
#define FN_GV_TIBTYPE ((Fn_GVTibType)(void*)0x5FDD20u)
/* 特殊覆盖物下标 (rules [OverlayTypes]): 70=PALET01 Block Base (阻建造),
 * 73=PALET04 Block Base and Movement (阻建造+通行)。 */
#define GV_OV_PALET_BUILD 70
#define GV_OV_PALET_BOTH 73

typedef struct { int ox, oy, vw, vh, ax, ay, aw, ah, sq; void* scen; } GridKey;

/* 格内角偏移: 0=NW 1=NE 2=SE 3=SW (与 GetFloorHeight 子坐标同向)。 */
static const int g_GKX[4] = { 0, 1, 1, 0 };
static const int g_GKY[4] = { 0, 0, 1, 1 };
/* 边: 0=北 1=东 2=南 3=西。MY=本侧两角, NBI=对侧对应两角, NOF=对侧格偏移。 */
static const int g_GMY[4][2]  = { { 0, 1 }, { 1, 2 }, { 3, 2 }, { 0, 3 } };
static const int g_GNBI[4][2] = { { 3, 2 }, { 0, 3 }, { 0, 1 }, { 1, 2 } };
static const int g_GNOF[4][2] = { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } };

static int s_GX0 = 0, s_GY0 = 0, s_GNX = 0, s_GNY = 0;
static short* s_GPX = 0;               /* 每格 4 角屏 X [(iy*nx+ix)*4+k] */
static short* s_GPY = 0;               /* 每格 4 角屏 Y */
static unsigned char* s_GPOK = 0;      /* 每格 4 角有效 */
static unsigned char* s_GCellOk = 0;   /* nx*ny 格有效 */
static unsigned char* s_GFlag = 0;     /* nx*ny 阻挡标志: 1硬挡 2崖块 4城墙 8矿 */
static unsigned char* s_GOV = 0;       /* nx*ny 特殊覆盖物: 1=70阻建造 2=73阻通行兼建造 */
static int s_GAllocN = 0;              /* 已分配格*4 */
static int s_GkValid = 0;              /* 当前缓存可画 */
static int s_GkSettled = 0;            /* 当前 key 已结算 (有效或永久无效, key 变前不再试) */
static GridKey s_Gk;
static GridKey s_GPrev;                /* 上一帧 key (判移动用) */
static int s_GPrevOn = 0;
static unsigned s_GStableMs = 0;       /* key 连续一致的起始时刻 (慢速卷屏防抖) */
static unsigned s_GRetryMs = 0;        /* 重建瞬时失败后, 下次允许重试的时刻 */

static int GV_KeyEq(const GridKey* a, const GridKey* b)
{
    return (a->ox == b->ox && a->oy == b->oy && a->vw == b->vw &&
            a->vh == b->vh && a->ax == b->ax && a->ay == b->ay &&
            a->aw == b->aw && a->ah == b->ah && a->sq == b->sq &&
            a->scen == b->scen) ? 1 : 0;
}

/* 读当前 key。读不到回 0 (调用方保留旧缓存, 不抖动)。 */
static int GV_ReadKey(GridKey* out)
{
    void* tac;
    RectangleStruct a;
    double sc;
    if (!out) return 0;
    tac = *(void**)(size_t)GV_TAC_INSTANCE;
    if (!tac || !IsPlausiblePtr(tac)) return 0;
    out->ox = *(volatile int*)((unsigned char*)tac + 0xB0);
    out->oy = *(volatile int*)((unsigned char*)tac + 0xB4);
    out->vw = *(volatile int*)(size_t)GV_VIEW_W;
    out->vh = *(volatile int*)(size_t)GV_VIEW_H;
    if (out->vw < 16 || out->vh < 16) return 0;
    if (out->ox < -1000000 || out->ox > 1000000) return 0;
    if (out->oy < -1000000 || out->oy > 1000000) return 0;
    FM_GetViewRects(&a, 0);
    if (a.Width < 32 || a.Height < 32) return 0;
    out->ax = a.X; out->ay = a.Y; out->aw = a.Width; out->ah = a.Height;
    sc = SB_Scale();
    out->sq = (int)(sc * 10000.0 + 0.5);
    out->scen = *(void**)(size_t)GV_ADDR_SCEN;
    return 1;
}

static void GV_Free(void)
{
    if (s_GPX) VirtualFree(s_GPX, 0, MEM_RELEASE);
    if (s_GPY) VirtualFree(s_GPY, 0, MEM_RELEASE);
    if (s_GPOK) VirtualFree(s_GPOK, 0, MEM_RELEASE);
    if (s_GCellOk) VirtualFree(s_GCellOk, 0, MEM_RELEASE);
    if (s_GFlag) VirtualFree(s_GFlag, 0, MEM_RELEASE);
    if (s_GOV) VirtualFree(s_GOV, 0, MEM_RELEASE);
    s_GPX = 0; s_GPY = 0; s_GPOK = 0; s_GCellOk = 0; s_GFlag = 0; s_GOV = 0;
    s_GAllocN = 0;
    s_GNX = s_GNY = 0;
}

/* 标记缓存作废 (保留已分配内存供下次复用)。 */
void GV_Drop(void)
{
    s_GkValid = 0;
    s_GkSettled = 0;
    s_GPrevOn = 0;
    s_GRetryMs = 0;
}

int GV_Toggle(void)
{
    GV_EnsureInit();
    s_GridOn = s_GridOn ? 0 : 1;
    if (!s_GridOn) GV_Free();   /* 关: 释放内存; 开: 防抖后重建 (内存复用) */
    GV_Drop();
    return s_GridOn;
}

static void* GV_AllocBytes(unsigned n)
{
    if (n == 0) return 0;
    return VirtualAlloc(0, (size_t)n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

/* 格取 cell 指针 (有效才回非 0)。 */
static void* GV_CellPtr(void** cells, int maxc, int cx, int cy)
{
    int idx;
    void* cell;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return 0;
    if (!cells || maxc <= 0 || maxc > 512 * 512) return 0;
    idx = (cy << 9) + cx;
    if (idx < 0 || idx >= maxc) return 0;
    cell = cells[idx];
    if (!cell || !IsPlausiblePtr(cell)) return 0;
    return cell;
}

/* (ax,ay) 格在角点 (gx,gy) 的地面高度: 角在东/南边取子坐标 255,
 * 西/北边取 0 (lepton mod 256, Ramp 格内插值靠它)。cell 由调用方取好。 */
static int GV_SampleZ(void* cell, int ax, int ay, int gx, int gy, int* zOut)
{
    Point2D sub;
    int z;
    if (!cell) return 0;
    sub.X = (gx > ax) ? 255 : 0;
    sub.Y = (gy > ay) ? 255 : 0;
    z = FN_GV_FLOORH(cell, &sub);
    if (z < -8192 || z > 8192) return 0;
    *zOut = z;
    return 1;
}

/* 角点 (gx,gy 格) 以给定高度 → 屏像素。tac 由调用方保证有效, 回 1 = 有效。 */
static int GV_ProjCorner(void* tac, int gx, int gy, int z, int* sx, int* sy)
{
    CoordStruct c;
    Point2D o;
    int vx, vy, px, py;
    c.X = gx * 256; c.Y = gy * 256; c.Z = z;
    o.X = 0; o.Y = 0;
    FN_GV_C2C(tac, &c, &o);
    vx = o.X; vy = o.Y;
    if (!SB_ViewToScreen(vx, vy, &px, &py)) { px = vx; py = vy; }
    if (px < -4096 || px > 16384 || py < -4096 || py > 16384) return 0;
    *sx = px; *sy = py;
    return 1;
}

/* 屏上点 → 格 (OU_MouseCellPx 同式, 但允许栏带外推: 角点常落在图外)。 */
static int GV_ScreenToCell(int sx, int sy, int* cx, int* cy)
{
    Point2D pt;
    CellStruct center;
    CoordStruct crd;
    void* tgtObj = 0;
    unsigned char b5 = 0, b6 = 0;
    center.X = center.Y = 0;
    crd.X = crd.Y = crd.Z = 0;
    pt.X = sx; pt.Y = sy;
    SB_MapPoint(&pt);   /* 物理→视口 (外推也算, 返回值只表是否在 A 内) */
    if (!FN_CLICKCOORDS((void*)(size_t)ADDR_DISPLAY, &pt, &center, &crd,
                        &tgtObj, &b5, &b6))
        return 0;
    *cx = (int)center.X; *cy = (int)center.Y;
    return OU_CellValid(*cx, *cy);
}

/* 按 key 重建缓存。回 1 = 有可画内容; 回 0 = 瞬时失败 (调用方退避重试)。
 * 范围超限 (远缩整图) 则记永久无效回 -1。 */
static int GV_Rebuild(const GridKey* key)
{
    RectangleStruct a;
    int corners[4][2];
    int cx[4], cy[4], ok[4];
    int i, x0, y0, x1, y1, nx, ny, nn;
    int ix, iy, nOk = 0, nCell = 0;
    char buf[224], b1[16], b2[16];
    unsigned p = 0;
    (void)key;
    FM_GetViewRects(&a, 0);
    if (a.Width < 32 || a.Height < 32) return 0;
    corners[0][0] = a.X; corners[0][1] = a.Y;
    corners[1][0] = a.X + a.Width - 1; corners[1][1] = a.Y;
    corners[2][0] = a.X; corners[2][1] = a.Y + a.Height - 1;
    corners[3][0] = a.X + a.Width - 1; corners[3][1] = a.Y + a.Height - 1;
    for (i = 0; i < 4; i++)
    {
        cx[i] = cy[i] = 0; ok[i] = 0;
        ok[i] = GV_ScreenToCell(corners[i][0], corners[i][1], &cx[i], &cy[i]);
    }
    if (!ok[0] && !ok[1] && !ok[2] && !ok[3]) return 0;
    x0 = y0 = 512; x1 = y1 = -1;
    for (i = 0; i < 4; i++)
    {
        if (!ok[i]) continue;
        if (cx[i] < x0) x0 = cx[i];
        if (cy[i] < y0) y0 = cy[i];
        if (cx[i] > x1) x1 = cx[i];
        if (cy[i] > y1) y1 = cy[i];
    }
    x0 -= 2; y0 -= 2; x1 += 2; y1 += 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 511) x1 = 511;
    if (y1 > 511) y1 = 511;
    nx = x1 - x0 + 1; ny = y1 - y0 + 1;
    if (nx <= 0 || ny <= 0) return 0;
    if (nx > GV_CAP_XY || ny > GV_CAP_XY || nx * ny > GV_CAP_CELLS)
    {
        p = AppendCStr(buf, p, "pv: grid skip (too wide ", sizeof(buf));
        UtoA((unsigned)nx, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, 'x', sizeof(buf));
        UtoA((unsigned)ny, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, ") zoom in to show", sizeof(buf));
        LogLine(buf);
        return -1;
    }
    nn = nx * ny;
    {
        int need4 = nn * 4;
        if (need4 > s_GAllocN)
        {
            GV_Free();
            /* 每格 4 角屏坐标 (20000 格约 200KB)。 */
            s_GPX = (short*)GV_AllocBytes((unsigned)need4 * 2u);
            s_GPY = (short*)GV_AllocBytes((unsigned)need4 * 2u);
            s_GPOK = (unsigned char*)GV_AllocBytes((unsigned)need4);
            s_GCellOk = (unsigned char*)GV_AllocBytes((unsigned)nn);
            s_GFlag = (unsigned char*)GV_AllocBytes((unsigned)nn);
            s_GOV = (unsigned char*)GV_AllocBytes((unsigned)nn);
            if (!s_GPX || !s_GPY || !s_GPOK || !s_GCellOk || !s_GFlag || !s_GOV) { GV_Free(); return 0; }
            s_GAllocN = need4;
        }
    }
    s_GX0 = x0; s_GY0 = y0; s_GNX = nx; s_GNY = ny;
    {
        void** cells = *(void***)(size_t)(ADDR_DISPLAY + OFF_MAP_CELLS);
        int maxc = *(int*)(size_t)(ADDR_DISPLAY + OFF_MAP_MAXC);
        void* tac;
        if (!cells || !IsPlausiblePtr(cells) || maxc <= 0 || maxc > 512 * 512)
            return 0;
        /* 重建内同步使用, 入口校验一次即可 (逐角不再重复读/验)。 */
        tac = *(void**)(size_t)GV_TAC_INSTANCE;
        if (!tac || !IsPlausiblePtr(tac)) return 0;
        /* 逐格: 有效则采自家 4 角地面高度并各投影一次 (自有平面, 不共享)。 */
        for (iy = 0; iy < ny; iy++)
        {
            for (ix = 0; ix < nx; ix++)
            {
                int cx = x0 + ix, cy = y0 + iy;
                int ci = iy * nx + ix;
                int n4 = ci * 4;
                int k, okc = 0;
                void* cellp;
                s_GCellOk[ci] = 0;
                s_GFlag[ci] = 0;
                s_GOV[ci] = 0;
                s_GPOK[n4] = s_GPOK[n4 + 1] = s_GPOK[n4 + 2] = s_GPOK[n4 + 3] = 0;
                cellp = GV_CellPtr(cells, maxc, cx, cy);
                if (!cellp) continue;
                /* 阻挡快照 (静态, 不管动态单位): 1硬挡(6) 2崖块 4城墙 8矿;
                 * 覆盖物 70阻建造, 73阻建造+通行; 水/图外不管。 */
                {
                    int fl = 0;
                    int pass = *(int*)((char*)cellp + GV_OFF_PASSABILITY);
                    int ovt, gov = 0;
                    if (pass < 0 || pass > 7) pass = 0;
                    if (pass == 6) fl |= 1;
                    if (FN_GV_ISCLIFF(cellp)) fl |= 2;
                    if (*(int*)((char*)cellp + GV_OFF_WALLOWN) != -1) fl |= 4;
                    ovt = *(int*)((char*)cellp + GV_OFF_OVERLAYTYPE);
                    if (ovt >= 0 && FN_GV_TIBTYPE(ovt) >= 0) fl |= 8;
                    if (ovt == GV_OV_PALET_BUILD) gov = 1;
                    else if (ovt == GV_OV_PALET_BOTH) gov = 2;
                    s_GOV[ci] = (unsigned char)gov;
                    s_GFlag[ci] = (unsigned char)fl;
                }
                nCell++;
                for (k = 0; k < 4; k++)
                {
                    int z, sx, sy;
                    if (!GV_SampleZ(cellp, cx, cy,
                                    cx + g_GKX[k], cy + g_GKY[k], &z))
                        continue;
                    if (GV_ProjCorner(tac, cx + g_GKX[k], cy + g_GKY[k], z, &sx, &sy))
                    {
                        s_GPX[n4 + k] = (short)sx;
                        s_GPY[n4 + k] = (short)sy;
                        s_GPOK[n4 + k] = 1;
                        nOk++;
                        okc = 1;
                    }
                }
                if (!okc) continue;      /* 四角全投影失败: 不画这一格 */
                s_GCellOk[ci] = 1;
            }
        }
    }
    if (nOk <= 0 || nCell <= 0) return 0;
    return 1;
}

/* 每循环调: 移动的帧隐藏, 停稳 (key 连 150ms 一致) 后重建。 */
void GV_PollStable(void)
{
    GridKey cur;
    unsigned now;
    int r;
    GV_EnsureInit();
    if (!s_GridOn) return;
    if (!GV_ReadKey(&cur)) return;   /* 读不到: 保留旧缓存, 不抖动 */
    if (s_GkSettled && GV_KeyEq(&cur, &s_Gk)) return;
    if (!s_GPrevOn || !GV_KeyEq(&cur, &s_GPrev))
    {
        /* key 变了 = 在移动 (含首次采样): 旧缓存立即隐藏, 本帧不重建。 */
        s_GPrev = cur;
        s_GPrevOn = 1;
        s_GStableMs = 0;
        s_GkValid = 0;
        s_GkSettled = 0;
        s_GRetryMs = 0;
        return;
    }
    /* 与上帧同 key: 计时, 满 150ms 才算停稳 (慢速卷屏 key 也会偶尔连两帧一致)。 */
    now = GetTickCount();
    if (s_GStableMs == 0) s_GStableMs = now;
    if ((int)(now - s_GStableMs) < (int)GV_STABLE_MS) return;
    /* 瞬时失败限频重试。 */
    if (s_GRetryMs != 0 && (int)(now - s_GRetryMs) < 0) return;
    s_GRetryMs = 0;
    r = GV_Rebuild(&cur);
    if (r > 0) { s_Gk = cur; s_GkValid = 1; s_GkSettled = 1; }
    else if (r < 0) { s_Gk = cur; s_GkValid = 0; s_GkSettled = 1; }  /* 超限: key 变前不再试 */
    else s_GRetryMs = now + GV_RETRY_MS;
}

/* ---------------- 格网绘制 (读缓存, 只落裁剪区内) ---------------- */

static unsigned char* s_GvLocked = 0;
static int s_GvW = 0, s_GvH = 0, s_GvPitch = 0;
static int s_GvCx0 = 0, s_GvCy0 = 0, s_GvCx1 = 0, s_GvCy1 = 0;
static int s_GvClip = 0;

/* 不可走格边线色 (与灰格网同系, 偏红稍亮)。 */
#define GV_BLK_R 215
#define GV_BLK_G 70
#define GV_BLK_B 70
#define GV_BLK_A 140
/* 真断崖屏差阈值 (1x 档 8px, 按缩放折算; Tile_Is_Cliff 不覆盖本图崖块, 只走几何)。 */
#define GV_STEP_RED_PX 8

static void GV_PlotC(int x, int y, int r, int g, int b, int a)
{
    unsigned short* dstRow;
    if (x < 0 || y < 0 || x >= s_GvW || y >= s_GvH) return;
    if (s_GvClip &&
        (x < s_GvCx0 || x >= s_GvCx1 || y < s_GvCy0 || y >= s_GvCy1))
        return;
    dstRow = (unsigned short*)(s_GvLocked + y * s_GvPitch);
    dstRow[x] = Blend16(dstRow[x], r, g, b, a);
}

/* 整段落在裁剪区外? (Bresenham 前的粗剔, 跨区段由 Plot 逐像素夹)。 */
static int GV_SegOut(int x0, int y0, int x1, int y1)
{
    if (!s_GvClip) return 0;
    if (x0 < s_GvCx0 && x1 < s_GvCx0) return 1;
    if (x0 >= s_GvCx1 && x1 >= s_GvCx1) return 1;
    if (y0 < s_GvCy0 && y1 < s_GvCy0) return 1;
    if (y0 >= s_GvCy1 && y1 >= s_GvCy1) return 1;
    return 0;
}

static void GV_LineC(int x0, int y0, int x1, int y1,
                     int r, int g, int b, int a)
{
    int dx, dy, sx, sy, err, e2, x, y, n;
    if (GV_SegOut(x0, y0, x1, y1)) return;
    dx = x1 - x0; dy = y1 - y0;
    sx = (dx >= 0) ? 1 : -1;
    sy = (dy >= 0) ? 1 : -1;
    dx = dx >= 0 ? dx : -dx; dy = dy >= 0 ? dy : -dy;
    err = dx - dy;
    x = x0; y = y0;
    for (n = 0; n < 8192; n++)
    {
        GV_PlotC(x, y, r, g, b, a);
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

static void GV_Line(int x0, int y0, int x1, int y1)
{
    GV_LineC(x0, y0, x1, y1, 170, 170, 170, 110);
}

static void GV_DrawCached(void)
{
    int ix, iy, e;
    if (!s_GridOn || !s_GkValid) return;   /* 移动中: 隐藏 */
    if (!s_GPX || !s_GPY || !s_GPOK || !s_GCellOk || !s_GFlag || !s_GOV) return;
    if (s_GNX <= 0 || s_GNY <= 0) return;
    /* 每格画自家 4 条边; 对侧同边两端坐标相同才跳过 (平地只画一次,
     * 悬崖画两条: 崖顶 + 崖底)。阻挡/73格/大落差边画红;
     * 70画叉, 73画叉+内框。 */
    for (iy = 0; iy < s_GNY; iy++)
    {
        for (ix = 0; ix < s_GNX; ix++)
        {
            int ci = iy * s_GNX + ix;
            int n4;
            if (!s_GCellOk[ci]) continue;
            n4 = ci * 4;
            for (e = 0; e < 4; e++)
            {
                int a = g_GMY[e][0], b = g_GMY[e][1];
                int nx2, ny2, tci = -1, tn4 = 0, na = 0, nb = 0, dup = 0;
                int ax, ay, bx, by;
                int hasNb, nax = 0, nay = 0, nbx = 0, nby = 0;
                int backCull = 0, skipLine = 0;
                int blocked = 0;
                if (!s_GPOK[n4 + a] || !s_GPOK[n4 + b]) continue;
                /* 同一条边只画一次 (序号小的格画): 坐标相同时两格都会命中,
                 * 必须按序号分边; 坐标不同 = 崖顶/崖底各一条 + 两端竖线 (崖面)。 */
                nx2 = s_GX0 + ix + g_GNOF[e][0];
                ny2 = s_GY0 + iy + g_GNOF[e][1];
                hasNb = 0;
                if (nx2 >= s_GX0 && ny2 >= s_GY0 &&
                    nx2 < s_GX0 + s_GNX && ny2 < s_GY0 + s_GNY)
                {
                    tci = (ny2 - s_GY0) * s_GNX + (nx2 - s_GX0);
                    if (s_GCellOk[tci])
                    {
                        na = g_GNBI[e][0]; nb = g_GNBI[e][1];
                        tn4 = tci * 4;
                        if (s_GPOK[tn4 + na] && s_GPOK[tn4 + nb])
                        {
                            hasNb = 1;
                            nax = s_GPX[tn4 + na]; nay = s_GPY[tn4 + na];
                            nbx = s_GPX[tn4 + nb]; nby = s_GPY[tn4 + nb];
                            if (nax == s_GPX[n4 + a] && nay == s_GPY[n4 + a] &&
                                nbx == s_GPX[n4 + b] && nby == s_GPY[n4 + b] &&
                                tci > ci)
                                dup = 1;   /* 对侧序号更小: 交给它画 */
                        }
                    }
                }
                if (dup) continue;
                ax = s_GPX[n4 + a]; ay = s_GPY[n4 + a];
                bx = s_GPX[n4 + b]; by = s_GPY[n4 + b];
                /* 背面剔除 (Y 小者高, 两端同向差 3px 以上才是干净台阶):
                 * 高→低朝北/西的面背向镜头, 高侧只留顶线, 低侧不画。 */
                if (s_GridCullBack && hasNb)
                {
                    int dAy = ay - nay, dBy = by - nby;
                    int adAy = dAy >= 0 ? dAy : -dAy;
                    int adBy = dBy >= 0 ? dBy : -dBy;
                    if (adAy >= 3 && adBy >= 3 &&
                        ((dAy > 0 && dBy > 0) || (dAy < 0 && dBy < 0)))
                    {
                        int curHigh = (dAy < 0) ? 1 : 0;
                        int back = (curHigh && (e == 0 || e == 3)) ||
                                   (!curHigh && (e == 1 || e == 2));
                        if (back)
                        {
                            if (curHigh) backCull = 1;   /* 高侧: 留顶线, 去竖线 */
                            else skipLine = 1;           /* 低侧: 底线被山挡, 整条不画 */
                        }
                    }
                }
                if (skipLine) continue;
                /* 红边: 任一侧带阻挡标志/73, 或两端屏差 >= 阈值 (真断崖)。 */
                blocked = 0;
                if (s_GridShowBlocked && s_GFlag && s_GOV)
                {
                    if (s_GFlag[ci] || (hasNb && s_GFlag[tci]) ||
                        s_GOV[ci] == 2 || (hasNb && s_GOV[tci] == 2))
                        blocked = 1;
                    else if (hasNb)
                    {
                        int q1x = ax - nax, q1y = ay - nay;
                        int q2x = bx - nbx, q2y = by - nby;
                        int qm1, qm2, qm, thr;
                        if (q1x < 0) q1x = -q1x;
                        if (q1y < 0) q1y = -q1y;
                        if (q2x < 0) q2x = -q2x;
                        if (q2y < 0) q2y = -q2y;
                        qm1 = q1x > q1y ? q1x : q1y;
                        qm2 = q2x > q2y ? q2x : q2y;
                        qm = qm1 > qm2 ? qm1 : qm2;
                        thr = (GV_STEP_RED_PX * s_Gk.sq + 5000) / 10000;
                        if (thr < 2) thr = 2;
                        if (qm >= thr) blocked = 1;
                    }
                }
                /* 崖面竖线: 同格点高低两角相连, 每条共享边画一次;
                 * 3px 以下按坡道噪声跳过。 */
                if (hasNb && tci > ci && !backCull)
                {
                    int dx, dy;
                    dx = ax - nax; if (dx < 0) dx = -dx;
                    dy = ay - nay; if (dy < 0) dy = -dy;
                    if ((dx > dy ? dx : dy) >= 3)
                    {
                        if (blocked)
                            GV_LineC(ax, ay, nax, nay, GV_BLK_R, GV_BLK_G, GV_BLK_B, GV_BLK_A);
                        else
                            GV_Line(ax, ay, nax, nay);
                    }
                    dx = bx - nbx; if (dx < 0) dx = -dx;
                    dy = by - nby; if (dy < 0) dy = -dy;
                    if ((dx > dy ? dx : dy) >= 3)
                    {
                        if (blocked)
                            GV_LineC(bx, by, nbx, nby, GV_BLK_R, GV_BLK_G, GV_BLK_B, GV_BLK_A);
                        else
                            GV_Line(bx, by, nbx, nby);
                    }
                }
                if (blocked)
                    GV_LineC(ax, ay, bx, by, GV_BLK_R, GV_BLK_G, GV_BLK_B, GV_BLK_A);
                else
                    GV_Line(ax, ay, bx, by);
            }
            /* 特殊覆盖物 (本格, 与邻格无关只画一次): 70阻建造画叉,
             * 73阻建造+通行画叉+内框。四角有效才画。 */
            if (s_GridShowBlocked && s_GOV && s_GOV[ci] &&
                s_GPOK[n4] && s_GPOK[n4 + 1] && s_GPOK[n4 + 2] && s_GPOK[n4 + 3])
            {
                int qx0 = s_GPX[n4], qy0 = s_GPY[n4];
                int qx1 = s_GPX[n4 + 1], qy1 = s_GPY[n4 + 1];
                int qx2 = s_GPX[n4 + 2], qy2 = s_GPY[n4 + 2];
                int qx3 = s_GPX[n4 + 3], qy3 = s_GPY[n4 + 3];
                GV_LineC(qx0, qy0, qx2, qy2, GV_BLK_R, GV_BLK_G, GV_BLK_B, GV_BLK_A);
                GV_LineC(qx1, qy1, qx3, qy3, GV_BLK_R, GV_BLK_G, GV_BLK_B, GV_BLK_A);
                if (s_GOV[ci] == 2)
                {
                    int mx1x = (qx0 + qx1) / 2, my1y = (qy0 + qy1) / 2;
                    int mx2x = (qx1 + qx2) / 2, my2y = (qy1 + qy2) / 2;
                    int mx3x = (qx2 + qx3) / 2, my3y = (qy2 + qy3) / 2;
                    int mx4x = (qx3 + qx0) / 2, my4y = (qy3 + qy0) / 2;
                    GV_LineC(mx1x, my1y, mx2x, my2y, GV_BLK_R, GV_BLK_G, GV_BLK_B, GV_BLK_A);
                    GV_LineC(mx2x, my2y, mx3x, my3y, GV_BLK_R, GV_BLK_G, GV_BLK_B, GV_BLK_A);
                    GV_LineC(mx3x, my3y, mx4x, my4y, GV_BLK_R, GV_BLK_G, GV_BLK_B, GV_BLK_A);
                    GV_LineC(mx4x, my4y, mx1x, my1y, GV_BLK_R, GV_BLK_G, GV_BLK_B, GV_BLK_A);
                }
            }
        }
    }
}

void GV_DrawTo(unsigned char* base, int pitch, int W, int H,
               int cx0, int cy0, int cx1, int cy1, int clip)
{
    GV_EnsureInit();
    if (!base || W <= 0 || H <= 0) return;
    s_GvLocked = base; s_GvW = W; s_GvH = H; s_GvPitch = pitch;
    s_GvCx0 = cx0; s_GvCy0 = cy0; s_GvCx1 = cx1; s_GvCy1 = cy1;
    s_GvClip = clip;
    GV_DrawCached();
    s_GvLocked = 0;
}
