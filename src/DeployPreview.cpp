/* DeployPreview.cpp - 中键基地车展开位置预览 + 到位自动展开。
 * (只读查询 + 渲染面绘制 + 标准事件下令, 联机安全。)
 *
 * 预览: 物理中键按住时, 以光标格画展开后地基黄框 (引擎 CoordsToClient
 * 投影, 每角取其框内格高, 平地高地同式), 细框套目标格, 方便对墙对齐。
 * 落点 = 光标格 + (大地基 -1,-1), 预览=移动目标=实际落点同一套数学
 * (TryToDeploy 0x7393C0 反汇编+运行时双确认)。
 *
 * 自动展开 ([DeployPreview] AutoDeploy=1): 选中含可展开单位 (UnitType+
 * 0x404 DeploysInto 非空) 的裸中键点按由本模块认领 (SmartOrder 让路)。
 * 单车框跟鼠标自由走; 多车点按按实际地基螺旋平铺 (块块判交不重叠),
 * 划线 (轨迹弧长 ≥5 格) 沿手绘轨迹弧长均分, Shift 锁横/竖轴排一排。
 * 混编其它单位按配额 (步兵3/格, 飞机/车辆1/格) 散开并避让房框外扩1格。
 * 下令: 移动 MegaMission(Move)+到位 Deploy 事件 (0x9, 与手按 D 同形);
 * 死车/换边/超时 (DeployTimeout 秒)/无处可铺则放弃, 新点按取代旧跟踪。
 * 认领态另画白色轨迹线 (SmartOrder 熄火时补上, 同观感)。
 * 开销: 地基尺寸指纹缓存 (选中签名+500ms), 帧内一次 ClickCoords。
 * 日志: 启动行 + 首次命中 + 每次 plots/fired/timeout。
 * 构建见 build-fast.bat。渲染挂 MOExperience_MessageDrawHook。
 */

#include <windows.h>
#include <math.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"
#include "ScaleBar.h"
#include "SmartArea.h"
#include "DeployPreview.h"

#define DP_SECTION "DeployPreview"

/* ---------------- 私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define DP_ADDR_TYPARRAY 0xA8EB00u
#define DP_OFF_VEC_ITEMS 4u
#define DP_OFF_VEC_COUNT 16u

/* 反汇编实证偏移: UnitType+0x404 = DeploysInto (TryToDeploy 两处独立同式)。
 * 地基宽/高经 0x45EC90/0x45ECA0 取得 (后者栈传 0=不含 bib)。 */
#define DP_OFF_DEPLOYSINTO 0x404u

typedef short (__attribute__((thiscall)) *Fn_FoundationWH)(void* bldType);
typedef short (__attribute__((thiscall)) *Fn_FoundationH)(void* bldType, int includeBib);
#define DP_FN_FW ((Fn_FoundationWH)(void*)0x45EC90u)
#define DP_FN_FH ((Fn_FoundationH)(void*)0x45ECA0u)

/* 标准移动/展开事件构造 (与 SmartOrder/手点同形, 联机安全) */
typedef void* __attribute__((thiscall)) (*Fn_DPTgtCell)(void* tgt, const CellStruct* cell);
typedef void* __attribute__((thiscall)) (*Fn_DPDeployCtor)(void* ev, int houseIdx,
    int evtType, int id, unsigned rttiW);
#define FN_DP_TGTCELL ((Fn_DPTgtCell)(void*)0x6E6B20u)
#define FN_DP_DEPLOY ((Fn_DPDeployCtor)(void*)0x4C65E0u)
#define EVT_DP_DEPLOY 0x9

typedef void* (__fastcall *DP_LockFn)(void*, void*, int, int);
typedef int (__fastcall *DP_UnlockFn)(void*, void*);
#define VT_DP_LOCK   0x5C
#define VT_DP_UNLOCK 0x60

/* ---------------- 配置 ---------------- */

static int g_DpInit = 0;
static int g_DpEnable = 1;
static int g_DpAuto = 1;     /* 1=单基地车裸中键点按自动开过去+到位展开 */
static int g_DpTimeoutMs = 45000;

static void DP_EnsureInit(void)
{
    int to;
    if (g_DpInit) return;
    g_DpInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_DpEnable = GetPrivateProfileIntA(DP_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    g_DpAuto = GetPrivateProfileIntA(DP_SECTION, "AutoDeploy", 1, g_IniPath) ? 1 : 0;
    to = GetPrivateProfileIntA(DP_SECTION, "DeployTimeout", 45, g_IniPath);
    if (to < 5) to = 5;
    if (to > 300) to = 300;
    g_DpTimeoutMs = to * 1000;
    LogLine("deployprev: ON (middle hold)");
    if (!g_DpEnable) LogLine("deployprev: OFF");
    if (!g_DpAuto) LogLine("deployprev: autodeploy OFF");
}

/* ---------------- 类型取得 (固定偏移, 无扫描无字符串探测) ----------------
 * Unit+0x6C4 = UnitType*; UnitType+0x404 = DeploysInto (TryToDeploy
 * 0x739460/0x73931C 两处独立同式)。只解引用选中数组活对象与静态类型
 * 数组 (InTypeSet 只比指针), 无扫描、无 pin, 又快又不会崩。 */

static int DP_TypeCount(void*** itemsOut)
{
    void** items;
    int cnt;
    *itemsOut = 0;
    cnt = *(int*)(DP_ADDR_TYPARRAY + DP_OFF_VEC_COUNT);
    items = *(void***)(DP_ADDR_TYPARRAY + DP_OFF_VEC_ITEMS);
    if (cnt <= 0 || cnt > 20000) return 0;
    if (!items || !IsPlausiblePtr(items)) return 0;
    *itemsOut = items;
    return cnt;
}

static int DP_InTypeSet(void* p)
{
    void** items = 0;
    int cnt, i;
    cnt = DP_TypeCount(&items);
    if (!cnt) return 0;
    for (i = 0; i < cnt; i++)
        if (items[i] == p) return 1;
    return 0;
}

#define DP_OFF_UNIT_TYPE 0x6C4u

static void* DP_TypeOf(void* u)
{
    void* p;
    if (!u || !IsPlausiblePtr(u)) return 0;
    p = *(void**)((char*)u + DP_OFF_UNIT_TYPE);
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (!DP_InTypeSet(p)) return 0;
    return p;
}

/* 选中里找第一个可展开单位的地基宽高。回 1=找到 (w/h 写出)。 */

static int DP_FindFootprint(int* wOut, int* hOut)
{
    void** items;
    int selN, i;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return 0;
    /* 大编队保护: 只看前 64 个, 防止上百载具选中时重扫抖动。
     * (重扫本就被指纹缓存限到 500ms 一次, 这里再封顶。) */
    if (selN > 64) selN = 64;
    for (i = 0; i < selN; i++)
    {
        void* o = items[i];
        void* tp;
        void* bld;
        int w, h;
        if (!o || !IsPlausiblePtr(o)) continue;
        if (OU_WhatAmI(o) != ABS_UNIT) continue;
        tp = DP_TypeOf(o);
        if (!tp) continue;
        bld = *(void**)((char*)tp + DP_OFF_DEPLOYSINTO);
        if (!bld || !IsPlausiblePtr(bld)) continue;
        w = (int)DP_FN_FW(bld);
        h = (int)DP_FN_FH(bld, 0);
        if (w < 1 || w > 10 || h < 1 || h > 10) continue;
        *wOut = w; *hOut = h;
        return 1;
    }
    return 0;
}

/* 地基尺寸缓存: 选中签名 (前 16 个指针+数量) 一致且 500ms 内直接复用
 * (含“无可展开单位”的阴性结果), 渲染帧内不再逐帧全扫。 */
#define DP_CACHE_PTRS 16
static int s_DpFw = 0, s_DpFh = 0, s_DpHas = 0;
static void* s_DpSelPtr[DP_CACHE_PTRS];
static int s_DpSelN = -1;
static unsigned s_DpSelMs = 0;

static int DP_FootprintCached(int* wOut, int* hOut)
{
    void** items;
    int selN, i, n, same;
    unsigned now;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items))
    {
        s_DpHas = 0; s_DpSelN = selN; s_DpSelMs = GetTickCount();
        return 0;
    }
    now = GetTickCount();
    same = (selN == s_DpSelN);
    if (same)
    {
        n = selN < DP_CACHE_PTRS ? selN : DP_CACHE_PTRS;
        for (i = 0; i < n; i++)
        {
            if (!IsPlausiblePtr(items[i]) || items[i] != s_DpSelPtr[i]) { same = 0; break; }
        }
    }
    if (same && (now - s_DpSelMs) < 500)
    {
        if (!s_DpHas) return 0;
        *wOut = s_DpFw; *hOut = s_DpFh;
        return 1;
    }
    if (DP_FindFootprint(&s_DpFw, &s_DpFh)) s_DpHas = 1;
    else s_DpHas = 0;
    s_DpSelN = selN;
    n = selN < DP_CACHE_PTRS ? selN : DP_CACHE_PTRS;
    for (i = 0; i < n; i++) s_DpSelPtr[i] = items[i];
    s_DpSelMs = now;
    if (!s_DpHas) return 0;
    *wOut = s_DpFw; *hOut = s_DpFh;
    return 1;
}

/* 引擎投影 (只读定点数学, 渲染期可调): 格线角点 → 屏像素。
 * TacticalClass::CoordsToClient 0x6D2140 (thiscall ECX=单例[0x887324],
 * 栈传 coord/out, 回可见性, 坐标恒写入)。输出即游戏渲染同系像素,
 * 直接画到 Composite 面上, 与引擎画其它东西同精度。
 * 高度: 每角 Z 取其框内格地面高度, 无效格回落光标高度 (平地与高地同式)。 */
typedef int __attribute__((thiscall)) (*Fn_DPC2C)(void* tac,
    const CoordStruct* c, Point2D* out);
#define FN_DP_C2C ((Fn_DPC2C)(void*)0x6D2140u)
#define DP_TAC_INSTANCE 0x887324u

/* 格子地面高度 (lep, 只读): CellClass::GetFloorHeight (0x47B3A0,
 * thiscall 传 Point2D* 单指针 + ret 4, 与按值 8 字节严格区分)。
 * 每角取其框内格高度, 无效格回落光标高度。 */

typedef int __attribute__((thiscall)) (*Fn_DPFloorH)(void* cell, const Point2D* p);
#define FN_DP_FLOORH ((Fn_DPFloorH)(void*)0x47B3A0u)

static int DP_CellZ(int cx, int cy, int* zOut)
{
    void** cells;
    int maxc, idx;
    void* cell;
    Point2D zero;
    int z;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return 0;
    cells = *(void***)(ADDR_DISPLAY + OFF_MAP_CELLS);
    maxc = *(int*)(ADDR_DISPLAY + OFF_MAP_MAXC);
    if (!cells || !IsPlausiblePtr(cells) || maxc <= 0 || maxc > 512 * 512) return 0;
    idx = (cy << 9) + cx;
    if (idx < 0 || idx >= maxc) return 0;
    cell = cells[idx];
    if (!cell || !IsPlausiblePtr(cell)) return 0;
    zero.X = 0; zero.Y = 0;
    z = FN_DP_FLOORH(cell, &zero);
    if (z < -8192 || z > 8192) return 0;
    *zOut = z;
    return 1;
}

/* 鼠标格 + 光标地面高度 (ProcessClickCoords 同 OU_MouseCell 入口, 只读;
 * 高度作每角取高失败时的回落)。 */
static int DP_MouseCellZ(int* mcx, int* mcy, int* lz)
{
    int mx, my;
    Point2D pt;
    CellStruct center;
    CoordStruct crd;
    void* tgtObj = 0;
    unsigned char b5 = 0, b6 = 0;
    center.X = center.Y = 0;
    crd.X = crd.Y = crd.Z = 0;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    pt.X = mx; pt.Y = my;
    if (!SB_MapPoint(&pt)) return 0;   /* 栏带不进引擎查询 (无上界) */
    if (!FN_CLICKCOORDS((void*)ADDR_DISPLAY, &pt, &center, &crd, &tgtObj, &b5, &b6))
        return 0;
    *mcx = (int)center.X; *mcy = (int)center.Y;
    *lz = crd.Z;
    if (*lz < -8192 || *lz > 8192) *lz = 0;
    return OU_CellValid(*mcx, *mcy);
}

/* 角点高度 = 其框内格高度, 失败回落 fb (光标高度)。 */
static int DP_CornerZ(int cx, int cy, int fb, int* zOut)
{
    if (DP_CellZ(cx, cy, zOut)) return 1;
    *zOut = fb;
    return 1;
}

/* 任意 lep 点 → 屏像素 (唯一投影出口, 调引擎 CoordsToClient)。
 * CoordsToClient 给的是**渲染面坐标**, 缩放档还要折到屏幕坐标 (ScaleBar)。 */
static int DP_CornerLep(int lx, int ly, int lz, int* px, int* py)
{
    void* tac = *(void**)DP_TAC_INSTANCE;
    CoordStruct c;
    Point2D o;
    if (!tac || !IsPlausiblePtr(tac)) return 0;
    c.X = lx; c.Y = ly; c.Z = lz;
    o.X = 0; o.Y = 0;
    FN_DP_C2C(tac, &c, &o);
    if (!SB_ViewToScreen(o.X, o.Y, px, py)) { *px = o.X; *py = o.Y; }
    return 1;
}

/* 格线角点 (gx,gy 格) → 屏像素。 */
static int DP_Corner(int gx, int gy, int gz, int* px, int* py)
{
    return DP_CornerLep(gx * 256, gy * 256, gz, px, py);
}

/* 格心投影 (轨迹线用, 高度取该格)。 */
static int DP_CenterProj(int cx, int cy, int fbZ, int* px, int* py)
{
    int z;
    DP_CornerZ(cx, cy, fbZ, &z);
    return DP_CornerLep(cx * 256 + 128, cy * 256 + 128, z, px, py);
}

/* ---------------- 单车认领点按 + 到位自动展开 ---------------- */

/* 认领条件 (纯函数, SmartOrder 按下点/按住中调用): 开关开且自动展开开,
 * 物理中键按住, 无 Z/Ctrl/组合中键, 且选中含可展开载具 (单多不论)。
 * SmartArea 默认热键就是 Shift+中键, 与本模块的 Shift 锁轴排直线重合:
 * 该组合 (无附加键/其它修饰) 有可展开单位时本模块优先, 其余组合仍让路。
 * 代价由指纹缓存兜底 (500ms 内同选中直接复用)。 */
int DeployPreview_ClaimsMiddle(void)
{
    int fw, fh;
    DP_EnsureInit();
    if (!g_DpEnable || !g_DpAuto) return 0;
    if (!OU_KeyDown(VK_MBUTTON)) return 0;
    if (OU_KeyDown('Z') || OU_KeyDown(VK_CONTROL)) return 0;
    if (SmartArea_ClaimsMiddle() && !SmartArea_ShiftMiddleOnly()) return 0;
    return DP_FootprintCached(&fw, &fh);
}

/* Fetch_ID 存活自检 (SmartOrder 同式, 失败则 fail-open 由调用方定)。 */
static int DP_IdxSelfCheck(void* u, int id)
{
    void* base = *(void**)ADDR_IDBASE;
    int cnt = *(int*)ADDR_IDCOUNT;
    if (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 20000 &&
        id > 0 && OU_PtrAliveRaw(u, id))
        return 1;
    return 0;
}

/* 落点校验: 首选格无效则方形环由近及远找有效格 (半径 5, SmartOrder 同式)。 */
static int DP_NearValid(int x, int y, int* ox, int* oy)
{
    int r, dx, dy;
    if (OU_CellValid(x, y)) { *ox = x; *oy = y; return 1; }
    for (r = 1; r <= 5; r++)
    {
        for (dy = -r; dy <= r; dy++)
        {
            for (dx = -r; dx <= r; dx++)
            {
                int cx, cy;
                if (dx != r && dx != -r && dy != r && dy != -r) continue;
                cx = x + dx; cy = y + dy;
                if (OU_CellValid(cx, cy)) { *ox = cx; *oy = cy; return 1; }
            }
        }
    }
    return 0;
}

/* 普通移动令: Mission=Move, Target=空, Destination=格, IsPlanningEvent=0
 * (与 SmartOrder SO_FireMove、手点右键移动同形)。 */
static int DP_FireMove(int houseIdx, int srcID, unsigned char srcRtti,
                       int dstID, unsigned char dstRtti,
                       void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_MOVE,
            0, 0, dstID, (unsigned)dstRtti, 0, 0);
    ev[OFF_PLANFLAG] = 0;
    return OT_Fire(ev, unit, uid, idxOk);
}

/* 展开令: EventType 0x9 (Deploy), Whom=该车 (与手按 D 同形, ctor 0x4C65E0)。 */
static int DP_FireDeploy(int houseIdx, int id, unsigned char rtti,
                         void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_DP_DEPLOY(ev, houseIdx, EVT_DP_DEPLOY, id, (unsigned)rtti);
    return OT_Fire(ev, unit, uid, idxOk);
}

static int DP_UnitCell(void* u, int* cx, int* cy)
{
    CoordStruct cs;
    if (!OU_GetCoords(u, &cs)) return 0;
    *cx = cs.X / 256; *cy = cs.Y / 256;
    if (*cx < 0 || *cy < 0 || *cx >= 512 || *cy >= 512) return 0;
    return 1;
}

typedef struct { void* unit; int id; short dx, dy; } DPMvE;
#define DP_DEP_CAP 24    /* 跟踪基地车上限 (分配/快照同) */
#define DP_OTH_CAP 256   /* 快照其它单位上限 */
#define DP_MV_CAP 320    /* 待发移动令上限 */
#define DP_PLOT_Q 512    /* 散开候选格上限 */
#define DP_DRAIN 64      /* 每轮询最多发令数 */

typedef struct {
    void* unit; int id; short dx, dy;
    void* house; int houseIdx; unsigned startMs;
} DPDepE;

static DPMvE s_DpMv[DP_MV_CAP];
static int s_DpMvN = 0;
static DPDepE s_DpDep[DP_DEP_CAP];
static int s_DpDepN = 0;
static int s_DpIdxOk = 0;
static void* s_DpHouse = 0;
static int s_DpHouseIdx = 0;

static int s_DpArmed = 0;
static int s_DpPrevTrig = 0;
/* 按下快照: 基地车 (上限) + 其它单位 + 各车实际地基 + 起点格 */
static void* s_DpDepU[DP_DEP_CAP];
static int s_DpDepId[DP_DEP_CAP];
static int s_DpDepFw[DP_DEP_CAP];
static int s_DpDepFh[DP_DEP_CAP];
static int s_DpDepN0 = 0;
static void* s_DpOthU[DP_OTH_CAP];
static int s_DpOthId[DP_OTH_CAP];
static int s_DpOthW[DP_OTH_CAP];
static int s_DpOthN0 = 0;
static int s_DpPressCx = 0, s_DpPressCy = 0;
/* 手绘轨迹节点 (格, 稀疏记录, SmartOrder 同步长 2, 上限 64)。
 * 按住拖动即采样, 松开按轨迹弧长均分铺房 (Shift 锁轴时走直线)。 */
#define DP_NODE_CAP 64
#define DP_NODE_STEP 2
#define DP_DRAG_MINLEN 5.0
static int s_DpNx[DP_NODE_CAP], s_DpNy[DP_NODE_CAP];
static int s_DpNodeN = 0;

static void DP_NodeReset(int cx, int cy)
{
    s_DpNx[0] = cx; s_DpNy[0] = cy;
    s_DpNodeN = 1;
}

static void DP_NodePush(int cx, int cy)
{
    int lx, ly, dx, dy, adx, ady;
    if (s_DpNodeN <= 0) { DP_NodeReset(cx, cy); return; }
    if (s_DpNodeN >= DP_NODE_CAP) return;
    lx = s_DpNx[s_DpNodeN - 1]; ly = s_DpNy[s_DpNodeN - 1];
    dx = cx - lx; dy = cy - ly;
    adx = dx >= 0 ? dx : -dx; ady = dy >= 0 ? dy : -dy;
    if (adx < DP_NODE_STEP && ady < DP_NODE_STEP) return;
    s_DpNx[s_DpNodeN] = cx; s_DpNy[s_DpNodeN] = cy;
    s_DpNodeN++;
}

/* 四舍五入 (弧长→格用)。 */
static int DP_Rnd(double v)
{
    return (int)(v + (v >= 0 ? 0.5 : -0.5));
}

/* 轨迹弧长 (格, 含末段到终点; 终点与末节点重合则不重复计)。 */
static double DP_ArcLen(const int* nx, const int* ny, int nn, int ex, int ey)
{
    double L = 0.0;
    int j, px, py;
    if (!nx || !ny || nn <= 0) return 0.0;
    px = nx[0]; py = ny[0];
    for (j = 1; j < nn && j < DP_NODE_CAP; j++)
    {
        double dx = (double)(nx[j] - px), dy = (double)(ny[j] - py);
        L += sqrt(dx * dx + dy * dy);
        px = nx[j]; py = ny[j];
    }
    if (ex != px || ey != py)
    {
        double dx = (double)(ex - px), dy = (double)(ey - py);
        L += sqrt(dx * dx + dy * dy);
    }
    return L;
}

/* 轨迹上弧长 s 处插值格 (超出尾端沿末段方向外延)。 */
static void DP_ArcPos(const int* nx, const int* ny, int nn, int ex, int ey,
                      double s, int* ox, int* oy)
{
    double acc = 0.0;
    int j, px, py;
    double ldx = 0.0, ldy = 0.0;
    int haveDir = 0;
    if (!nx || !ny || nn <= 0) { *ox = ex; *oy = ey; return; }
    if (s <= 0.0) { *ox = nx[0]; *oy = ny[0]; return; }
    px = nx[0]; py = ny[0];
    for (j = 1; j < nn && j < DP_NODE_CAP; j++)
    {
        double dx = (double)(nx[j] - px), dy = (double)(ny[j] - py);
        double seg = sqrt(dx * dx + dy * dy);
        if (seg > 0.0001) { ldx = dx / seg; ldy = dy / seg; haveDir = 1; }
        if (acc + seg >= s && seg > 0.0001)
        {
            double t = (s - acc) / seg;
            *ox = px + DP_Rnd((nx[j] - px) * t);
            *oy = py + DP_Rnd((ny[j] - py) * t);
            return;
        }
        acc += seg;
        px = nx[j]; py = ny[j];
    }
    {
        double dx = (double)(ex - px), dy = (double)(ey - py);
        double seg = sqrt(dx * dx + dy * dy);
        if (seg > 0.0001) { ldx = dx / seg; ldy = dy / seg; haveDir = 1; }
        if (acc + seg >= s && seg > 0.0001)
        {
            double t = (s - acc) / seg;
            *ox = px + DP_Rnd((ex - px) * t);
            *oy = py + DP_Rnd((ey - py) * t);
            return;
        }
        acc += seg;
    }
    if (!haveDir) { *ox = ex; *oy = ey; return; }
    *ox = px + DP_Rnd(ldx * (s - acc));
    *oy = py + DP_Rnd(ldy * (s - acc));
}

/* 矩形相交 / 房框命中 (margin 外扩) / 矩形全格有效 */
static int DP_RectOverlap(int ax, int ay, int aw, int ah,
                          int bx, int by, int bw, int bh)
{
    return (ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by);
}

static int DP_InPlots(int cx, int cy, int bases[][2], int n,
                      const int plotFw[], const int plotFh[], int margin)
{
    int i;
    if (!plotFw || !plotFh) return 0;
    for (i = 0; i < n; i++)
    {
        if (cx >= bases[i][0] - margin && cx < bases[i][0] + plotFw[i] + margin &&
            cy >= bases[i][1] - margin && cy < bases[i][1] + plotFh[i] + margin)
            return 1;
    }
    return 0;
}

static int DP_RectValid(int bx, int by, int fw, int fh)
{
    int x, y;
    for (y = 0; y < fh; y++)
        for (x = 0; x < fw; x++)
            if (!OU_CellValid(bx + x, by + y)) return 0;
    return 1;
}

/* 排房步长 = 各车 max(宽,高) 之大者 (块块不重叠的最小间距)。 */
static int DP_MaxStep(int depN, const int depFw[], const int depFh[])
{
    int step = 1, i;
    for (i = 0; i < depN; i++)
    {
        int f = depFw[i] > depFh[i] ? depFw[i] : depFh[i];
        if (f > step) step = f;
    }
    return step;
}

/* 点按铺房: 首块锚点=C (落点=C+偏移=黄框, 单车行为不变),
 * 其余按步长螺旋平铺。每车按实际尺寸判交, 整块有效。
 * depN 输入车数, 返回铺好块数 (车按序认领前 nPlot 块)。 */
static int DP_AssignPoint(int cx, int cy,
                          int depN, const int depFw[], const int depFh[],
                           int bases[][2], int dests[][2])
{
    int step, n = 0;
    int r, ox, oy, tries = 0;
    int g0;
    if (depN <= 0 || depN > DP_DEP_CAP) return 0;
    if (!depFw || !depFh) return 0;
    step = DP_MaxStep(depN, depFw, depFh);
    g0 = (depFw[0] > 2 || depFh[0] > 2) ? 1 : 0;
    bases[0][0] = cx - g0;
    bases[0][1] = cy - g0;
    dests[0][0] = cx;
    dests[0][1] = cy;
    if (!DP_RectValid(bases[0][0], bases[0][1], depFw[0], depFh[0])) return 0;
    n = 1;
    for (r = 1; r <= 6 && n < depN; r++)
    {
        for (oy = -r; oy <= r && n < depN; oy++)
        {
            for (ox = -r; ox <= r && n < depN; ox++)
            {
                int ax, ay, bx, by, gi, k, hit;
                if (ox != r && ox != -r && oy != r && oy != -r) continue;
                if (++tries > 200) return n;
                ax = cx + ox * step; ay = cy + oy * step;
                gi = (depFw[n] > 2 || depFh[n] > 2) ? 1 : 0;
                bx = ax - gi; by = ay - gi;
                if (!DP_RectValid(bx, by, depFw[n], depFh[n])) continue;
                hit = 0;
                for (k = 0; k < n; k++)
                {
                    if (DP_RectOverlap(bx, by, depFw[n], depFh[n],
                                       bases[k][0], bases[k][1],
                                       depFw[k], depFh[k]))
                    { hit = 1; break; }
                }
                if (hit) continue;
                bases[n][0] = bx; bases[n][1] = by;
                dests[n][0] = ax; dests[n][1] = ay;
                n++;
            }
        }
    }
    return n;
}

/* 直线排房 (Shift 锁轴): N 块沿 P→C 主导轴全段均匀分布
 * (间距=步长与全长均分之大者, 短线自动续到 C 之后)。
 * 每车按实际尺寸判交, 整块有效, 否则丢弃该车。 */
static int DP_AssignStraight(int px, int py, int cx, int cy,
                             int depN, const int depFw[], const int depFh[],
                              int bases[][2], int dests[][2])
{
    int step, n = 0, i;
    int dx, dy, adx, ady;
    double ux = 0.0, uy = 0.0, len, span;
    if (depN <= 0 || depN > DP_DEP_CAP) return 0;
    if (!depFw || !depFh) return 0;
    step = DP_MaxStep(depN, depFw, depFh);
    dx = cx - px; dy = cy - py;
    adx = dx >= 0 ? dx : -dx; ady = dy >= 0 ? dy : -dy;
    if (adx >= ady && dx != 0) ux = (dx > 0) ? 1.0 : -1.0;
    else if (dy != 0) uy = (dy > 0) ? 1.0 : -1.0;
    else { ux = 1.0; uy = 0.0; }
    len = (double)(adx >= ady ? adx : ady);
    if (len < 0.5) len = 0.5;
    span = (depN > 1) ? len / (depN - 1) : 0.0;
    if (span < (double)step) span = (double)step;
    for (i = 0; i < depN; i++)
    {
        int k, ax = px, ay = py, ok = 0;
        for (k = 0; k < 12 && !ok; k++)
        {
            double t = i * span + k;
            int tx = px + DP_Rnd(ux * t), ty = py + DP_Rnd(uy * t);
            int gi = (depFw[i] > 2 || depFh[i] > 2) ? 1 : 0;
            int tbx = tx - gi, tby = ty - gi;
            int q;
            if (!DP_RectValid(tbx, tby, depFw[i], depFh[i])) continue;
            for (q = 0; q < n; q++)
            {
                if (DP_RectOverlap(tbx, tby, depFw[i], depFh[i],
                                   bases[q][0], bases[q][1],
                                   depFw[q], depFh[q]))
                    break;
            }
            if (q < n) continue;
            ax = tx; ay = ty; ok = 1;
        }
        if (!ok) break;   /* 排不下: 后面的车顺延放弃 (调用方转散开) */
        {
            int gi = (depFw[i] > 2 || depFh[i] > 2) ? 1 : 0;
            bases[n][0] = ax - gi; bases[n][1] = ay - gi;
            dests[n][0] = ax; dests[n][1] = ay;
            n++;
        }
    }
    return n;
}

/* 轨迹排房: N 块沿手绘轨迹弧长均匀分布 (SmartOrder 散开同手感,
 * 间距=步长与全长均分之大者, 短线自动续到尾端之后)。
 * 位置沿轨迹插值 (超尾端沿末段方向外延), 无效/重叠沿线顺延。
 * 每车按实际尺寸判交, 整块有效, 否则丢弃该车。 */
static int DP_AssignLine(const int* nx, const int* ny, int nn, int ex, int ey,
                         int depN, const int depFw[], const int depFh[],
                              int bases[][2], int dests[][2])
{
    int step, n = 0, i;
    double L, span;
    if (!nx || !ny || nn <= 0) return 0;
    if (depN <= 0 || depN > DP_DEP_CAP) return 0;
    if (!depFw || !depFh) return 0;
    step = DP_MaxStep(depN, depFw, depFh);
    L = DP_ArcLen(nx, ny, nn, ex, ey);
    if (L < 0.5) L = 0.5;
    span = (depN > 1) ? L / (depN - 1) : 0.0;
    if (span < (double)step) span = (double)step;
    for (i = 0; i < depN; i++)
    {
        int k, ax = nx[0], ay = ny[0], ok = 0;
        for (k = 0; k < 12 && !ok; k++)
        {
            double s = i * span + k;
            int tx, ty;
            int gi, tbx, tby, q;
            DP_ArcPos(nx, ny, nn, ex, ey, s, &tx, &ty);
            gi = (depFw[i] > 2 || depFh[i] > 2) ? 1 : 0;
            tbx = tx - gi; tby = ty - gi;
            if (!DP_RectValid(tbx, tby, depFw[i], depFh[i])) continue;
            for (q = 0; q < n; q++)
            {
                if (DP_RectOverlap(tbx, tby, depFw[i], depFh[i],
                                   bases[q][0], bases[q][1],
                                   depFw[q], depFh[q]))
                    break;
            }
            if (q < n) continue;
            ax = tx; ay = ty; ok = 1;
        }
        if (!ok) break;   /* 排不下: 后面的车顺延放弃 (调用方转散开) */
        {
            int gi = (depFw[i] > 2 || depFh[i] > 2) ? 1 : 0;
            bases[n][0] = ax - gi; bases[n][1] = ay - gi;
            dests[n][0] = ax; dests[n][1] = ay;
            n++;
        }
    }
    return n;
}

/* 发移动 (OutList 满则挂起下轮续发)。回 1=已发/无需发。 */
static int DP_TryMoveHead(void)
{
    void* cur;
    int houseIdx;
    char srcB[8], dstB[8];
    CellStruct cs;
    int srcID, dstID;
    unsigned char srcR, dstR;
    unsigned i;
    DPMvE* e;
    if (s_DpMvN <= 0) return 1;
    e = &s_DpMv[0];
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || cur != s_DpHouse) { s_DpMvN = 0; return 1; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (houseIdx != s_DpHouseIdx) { s_DpMvN = 0; return 1; }
    for (i = 0; i < 8; i++) srcB[i] = dstB[i] = 0;
    cs.X = e->dx; cs.Y = e->dy;
    FN_DP_TGTCELL(dstB, &cs);
    dstID = *(int*)(dstB + 0); dstR = (unsigned char)dstB[4];
    if (!OU_PtrAlive(e->unit, e->id, s_DpIdxOk)) goto drop;
    if (OU_WhatAmI(e->unit) != ABS_UNIT &&
        OU_WhatAmI(e->unit) != ABS_AIRCRAFT &&
        OU_WhatAmI(e->unit) != ABS_INFANTRY) goto drop;
    if (!FN_TGT_OBJ(srcB, e->unit)) goto drop;
    srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
    if (!DP_FireMove(houseIdx, srcID, srcR, dstID, dstR,
                      e->unit, e->id, s_DpIdxOk))
        return 0; /* OutList 满: 下轮继续 */
drop:
    {
        int k;
        for (k = 0; k + 1 < s_DpMvN; k++)
        {
            s_DpMv[k].unit = s_DpMv[k + 1].unit;
            s_DpMv[k].id = s_DpMv[k + 1].id;
            s_DpMv[k].dx = s_DpMv[k + 1].dx;
            s_DpMv[k].dy = s_DpMv[k + 1].dy;
        }
        s_DpMvN--;
    }
    return 1;
}

static void DP_DrainMoves(void)
{
    int sent = 0;
    while (s_DpMvN > 0 && sent < DP_DRAIN)
    {
        int before = s_DpMvN;
        if (!DP_TryMoveHead()) return;
        if (s_DpMvN < before) sent++;
        else break;
    }
}

static void DP_QueueMove(void* unit, int id, int dx, int dy)
{
    int ox, oy;
    if (s_DpMvN >= DP_MV_CAP) return;
    if (!DP_NearValid(dx, dy, &ox, &oy)) return;
    s_DpMv[s_DpMvN].unit = unit;
    s_DpMv[s_DpMvN].id = id;
    s_DpMv[s_DpMvN].dx = (short)ox;
    s_DpMv[s_DpMvN].dy = (short)oy;
    s_DpMvN++;
}

static void DP_DepShiftDown(int idx)
{
    int k;
    /* 无 CRT 禁 memmove: 逐字段搬 (结构体赋值会被编译成 memmove)。 */
    for (k = idx; k + 1 < s_DpDepN; k++)
    {
        s_DpDep[k].unit = s_DpDep[k + 1].unit;
        s_DpDep[k].id = s_DpDep[k + 1].id;
        s_DpDep[k].dx = s_DpDep[k + 1].dx;
        s_DpDep[k].dy = s_DpDep[k + 1].dy;
        s_DpDep[k].house = s_DpDep[k + 1].house;
        s_DpDep[k].houseIdx = s_DpDep[k + 1].houseIdx;
        s_DpDep[k].startMs = s_DpDep[k + 1].startMs;
    }
    s_DpDepN--;
}

/* 跟踪服务: 到位发 Deploy, 死车/换边/超时摘除。 */
static void DP_ServiceDeps(void)
{
    int i = 0;
    while (i < s_DpDepN)
    {
        DPDepE* e = &s_DpDep[i];
        void* cur = *(void**)ADDR_CURPLAYER;
        int arrived = 0;
        if (!cur || cur != e->house) { DP_DepShiftDown(i); continue; }
        if (GetTickCount() - e->startMs > (unsigned)g_DpTimeoutMs)
        {
            DP_DepShiftDown(i);
            LogLine("deploy: timeout, abandoned");
            continue;
        }
        if (!OU_PtrAlive(e->unit, e->id, s_DpIdxOk)) { DP_DepShiftDown(i); continue; }
        {
            int ux, uy;
            if (DP_UnitCell(e->unit, &ux, &uy) &&
                ux == (int)e->dx && uy == (int)e->dy)
                arrived = 1;
        }
        if (!arrived) { i++; continue; }
        {
            char tb[8];
            unsigned ti;
            int id;
            unsigned char rt;
            int hIdx;
            for (ti = 0; ti < 8; ti++) tb[ti] = 0;
            if (!OU_PtrAlive(e->unit, e->id, s_DpIdxOk)) { DP_DepShiftDown(i); continue; }
            if (!FN_TGT_OBJ(tb, e->unit)) { DP_DepShiftDown(i); continue; }
            id = *(int*)(tb + 0); rt = (unsigned char)tb[4];
            hIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
            if (hIdx != e->houseIdx) { DP_DepShiftDown(i); continue; }
            if (DP_FireDeploy(hIdx, id, rt, e->unit, e->id, s_DpIdxOk))
            {
                DP_DepShiftDown(i);
                LogLine("deploy: fired");
                continue;
            }
            i++; /* OutList 满: 下轮继续 (车已在位)。 */
        }
    }
}

/* 按下快照: 拆基地车 (记每车实际地基) /其它单位, 记起点格
 * (新意图取代旧跟踪)。 */
static void DP_Snapshot(void)
{
    void** items;
    int selN, i;
    char tb[8];
    unsigned ti;
    int firstId = -1;
    void* firstU = 0;
    for (ti = 0; ti < 8; ti++) tb[ti] = 0;
    s_DpDepN0 = 0; s_DpOthN0 = 0;
    s_DpIdxOk = 0;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) return;
    for (i = 0; i < selN; i++)
    {
        void* o = items[i];
        int w;
        void* tp;
        void* bld;
        int bw, bh;
        if (!o || !IsPlausiblePtr(o)) continue;
        w = OU_WhatAmI(o);
        if (w != ABS_UNIT && w != ABS_AIRCRAFT && w != ABS_INFANTRY) continue;
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        if (!FN_TGT_OBJ(tb, o)) continue;
        if (firstId < 0) { firstId = *(int*)(tb + 0); firstU = o; }
        if (w == ABS_UNIT && (tp = DP_TypeOf(o)) &&
            (bld = *(void**)((char*)tp + DP_OFF_DEPLOYSINTO)) &&
            bld && IsPlausiblePtr(bld) &&
            (bw = (int)DP_FN_FW(bld)) >= 1 && bw <= 10 &&
            (bh = (int)DP_FN_FH(bld, 0)) >= 1 && bh <= 10)
        {
            if (s_DpDepN0 < DP_DEP_CAP)
            {
                s_DpDepU[s_DpDepN0] = o;
                s_DpDepId[s_DpDepN0] = *(int*)(tb + 0);
                s_DpDepFw[s_DpDepN0] = bw;
                s_DpDepFh[s_DpDepN0] = bh;
                s_DpDepN0++;
                continue;
            }
            /* 名额满的基地车转入其它 (只走不展开)。 */
        }
        if (s_DpOthN0 < DP_OTH_CAP)
        {
            s_DpOthU[s_DpOthN0] = o;
            s_DpOthId[s_DpOthN0] = *(int*)(tb + 0);
            s_DpOthW[s_DpOthN0] = w;
            s_DpOthN0++;
        }
    }
    if (firstId > 0) s_DpIdxOk = DP_IdxSelfCheck(firstU, firstId);
    if (!OU_MouseCell(&s_DpPressCx, &s_DpPressCy))
    { s_DpDepN0 = 0; s_DpOthN0 = 0; }
}

/* 其它单位散开 (配额与 SmartOrder 点按一致, 跳过房框实际尺寸外扩 1 格)。
 * bases/nPlot 为已定房框 (避让用, 尺寸取 plotFw/Fh), C 为散开中心。 */
static void DP_ScatterOthers(int ccx, int ccy, int bases[][2], int nPlot,
                             const int plotFw[], const int plotFh[])
{
    static int qcx[DP_PLOT_Q], qcy[DP_PLOT_Q];
    static int quA[DP_PLOT_Q], quI[DP_PLOT_Q], quV[DP_PLOT_Q];
    int qn = 0, r, k;
    qcx[0] = ccx; qcy[0] = ccy;
    quA[0] = quI[0] = quV[0] = 0;
    qn = (OU_CellValid(ccx, ccy) &&
          !DP_InPlots(ccx, ccy, bases, nPlot, plotFw, plotFh, 1)) ? 1 : 0;
    for (r = 1; r <= 40 && qn < DP_PLOT_Q; r++)
    {
        int dx, dy;
        for (dx = -r; dx <= r && qn < DP_PLOT_Q; dx++)
        {
            int x = ccx + dx, y = ccy - r;
            if (OU_CellValid(x, y) && !DP_InPlots(x, y, bases, nPlot, plotFw, plotFh, 1))
            { qcx[qn] = x; qcy[qn] = y; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
        }
        for (dy = -r + 1; dy <= r && qn < DP_PLOT_Q; dy++)
        {
            int x = ccx + r, y = ccy + dy;
            if (OU_CellValid(x, y) && !DP_InPlots(x, y, bases, nPlot, plotFw, plotFh, 1))
            { qcx[qn] = x; qcy[qn] = y; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
        }
        for (dx = r - 1; dx >= -r && qn < DP_PLOT_Q; dx--)
        {
            int x = ccx + dx, y = ccy + r;
            if (OU_CellValid(x, y) && !DP_InPlots(x, y, bases, nPlot, plotFw, plotFh, 1))
            { qcx[qn] = x; qcy[qn] = y; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
        }
        for (dy = r - 1; dy >= -r + 1 && qn < DP_PLOT_Q; dy--)
        {
            int x = ccx - r, y = ccy + dy;
            if (OU_CellValid(x, y) && !DP_InPlots(x, y, bases, nPlot, plotFw, plotFh, 1))
            { qcx[qn] = x; qcy[qn] = y; quA[qn] = quI[qn] = quV[qn] = 0; qn++; }
        }
    }
    for (k = 0; k < s_DpOthN0; k++)
    {
        void* u = s_DpOthU[k];
        int w = s_DpOthW[k], c;
        if (!OU_PtrAlive(u, s_DpOthId[k], s_DpIdxOk)) continue;
        if (OU_WhatAmI(u) != w) continue;
        for (c = 0; c < qn; c++)
        {
            int cap;
            if (w == ABS_INFANTRY) { cap = 3; if (quI[c] >= cap) continue; quI[c]++; }
            else if (w == ABS_AIRCRAFT) { cap = 1; if (quA[c] >= cap) continue; quA[c]++; }
            else { cap = 1; if (quV[c] >= cap) continue; quV[c]++; }
            DP_QueueMove(u, s_DpOthId[k], qcx[c], qcy[c]);
            break;
        }
    }
}

void DeployPreview_Poll(void)
{
    int trigDown;
    DP_EnsureInit();
    trigDown = OU_KeyDown(VK_MBUTTON);
    /* 先服务: 移动排空 + 到位跟踪 (成功才消费, 满则下轮)。 */
    DP_DrainMoves();
    DP_ServiceDeps();
    if (!g_DpEnable || !g_DpAuto) { s_DpArmed = 0; s_DpPrevTrig = trigDown; return; }
    if (trigDown && s_DpArmed && !DeployPreview_ClaimsMiddle())
        s_DpArmed = 0;   /* 按住途中改主意 (丢车/Z/Ctrl/组合键): 取消待命 */
    if (trigDown && s_DpArmed)
    {
        /* 按住拖动: 采样轨迹节点 (多车划线沿轨迹弧长均分布置用)。 */
        int nx, ny;
        if (OU_MouseCell(&nx, &ny)) DP_NodePush(nx, ny);
    }
    if (trigDown && !s_DpPrevTrig)
    {
        /* 按下: 认领成功则快照 (新意图取代旧跟踪/排队)。 */
        if (DeployPreview_ClaimsMiddle())
        {
            s_DpMvN = 0; s_DpDepN = 0;
            s_DpArmed = 0;
            DP_Snapshot();
            if (s_DpDepN0 > 0)
            {
                s_DpArmed = 1;
                DP_NodeReset(s_DpPressCx, s_DpPressCy);
            }
        }
    }
    else if (!trigDown && s_DpPrevTrig && s_DpArmed)
    {
        /* 松开: 单车跟随终点; 多车按住 Shift 走锁轴直线,
         * 划线够长走手绘轨迹, 否则点按平铺。
         * 车进自家框目标, 其它单位散开避让 → 排空。 */
        int ccx, ccy, nPlot, k, dropped = 0;
        int apx, apy;
        void* cur;
        static int bases[DP_DEP_CAP][2], dests[DP_DEP_CAP][2];
        s_DpArmed = 0;
        cur = *(void**)ADDR_CURPLAYER;
        if (!OU_MouseCell(&ccx, &ccy) || !cur || !IsPlausiblePtr(cur))
        { s_DpPrevTrig = trigDown; return; }
        s_DpHouse = cur;
        s_DpHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
        /* 单车: 起点=终点 (框跟鼠标自由走, 不管按哪); 多车才划线。 */
        apx = (s_DpDepN0 == 1) ? ccx : s_DpPressCx;
        apy = (s_DpDepN0 == 1) ? ccy : s_DpPressCy;
        if (s_DpDepN0 == 1)
        {
            nPlot = DP_AssignPoint(ccx, ccy, s_DpDepN0,
                                   s_DpDepFw, s_DpDepFh,
                                   bases, dests);
        }
        else if (OU_KeyDown(VK_SHIFT))
        {
            nPlot = DP_AssignStraight(apx, apy, ccx, ccy,
                                      s_DpDepN0, s_DpDepFw, s_DpDepFh,
                                      bases, dests);
        }
        else if (DP_ArcLen(s_DpNx, s_DpNy, s_DpNodeN, ccx, ccy) >= DP_DRAG_MINLEN)
        {
            nPlot = DP_AssignLine(s_DpNx, s_DpNy, s_DpNodeN, ccx, ccy,
                                  s_DpDepN0, s_DpDepFw, s_DpDepFh,
                                  bases, dests);
        }
        else
        {
            nPlot = DP_AssignPoint(ccx, ccy, s_DpDepN0,
                                   s_DpDepFw, s_DpDepFh,
                                   bases, dests);
        }
        for (k = 0; k < nPlot; k++)
        {
            int dx, dy;
            void* u = s_DpDepU[k];
            int id = s_DpDepId[k];
            if (!OU_PtrAlive(u, id, s_DpIdxOk)) { dropped++; continue; }
            if (!DP_NearValid(dests[k][0], dests[k][1], &dx, &dy)) { dropped++; continue; }
            DP_QueueMove(u, id, dx, dy);
            if (s_DpDepN < DP_DEP_CAP)
            {
                s_DpDep[s_DpDepN].unit = u;
                s_DpDep[s_DpDepN].id = id;
                s_DpDep[s_DpDepN].dx = (short)dx;
                s_DpDep[s_DpDepN].dy = (short)dy;
                s_DpDep[s_DpDepN].house = cur;
                s_DpDep[s_DpDepN].houseIdx = s_DpHouseIdx;
                s_DpDep[s_DpDepN].startMs = GetTickCount();
                s_DpDepN++;
            }
        }
        /* 没分到房的基地车转入散开 (只走不展开, 不挡路)。 */
        for (k = nPlot; k < s_DpDepN0; k++)
        {
            if (s_DpOthN0 < DP_OTH_CAP)
            {
                s_DpOthU[s_DpOthN0] = s_DpDepU[k];
                s_DpOthId[s_DpOthN0] = s_DpDepId[k];
                s_DpOthW[s_DpOthN0] = ABS_UNIT;
                s_DpOthN0++;
            }
            else dropped++;
        }
        DP_ScatterOthers(ccx, ccy, bases, nPlot, s_DpDepFw, s_DpDepFh);
        DP_DrainMoves();
        {
            char sb[96], t1[16], t2[16];
            unsigned q = 0;
            q = AppendCStr(sb, q, "deploy: plots=", sizeof(sb));
            UtoA((unsigned)nPlot, t1); q = AppendCStr(sb, q, t1, sizeof(sb));
            q = AppendCStr(sb, q, " dropped=", sizeof(sb));
            UtoA((unsigned)dropped, t2); q = AppendCStr(sb, q, t2, sizeof(sb));
            LogLine(sb);
        }
    }
    s_DpPrevTrig = trigDown;
}

/* ---------------- 绘制 ---------------- */

static unsigned char* s_DpLocked = 0;
static int s_DpW = 0, s_DpH = 0, s_DpPitch = 0;

/* 3px 粗线 (Bresenham + 上下各 1px, 与 SmartOrder 同风格零依赖)。
 * 黄=房框/预览, 白=认领态轨迹 (SmartOrder 熄火时补上)。 */
static void DP_PlotC(int x, int y, int r, int g, int b, unsigned a)
{
    unsigned short* dstRow;
    if (x < 0 || y < 0 || x >= s_DpW || y >= s_DpH) return;
    dstRow = (unsigned short*)(s_DpLocked + y * s_DpPitch);
    dstRow[x] = Blend16(dstRow[x], r, g, b, a);
}

static void DP_LineC(int x0, int y0, int x1, int y1,
                     int r, int g, int b, unsigned a)
{
    int dx, dy, sx, sy, err, e2, x, y, n;
    dx = x1 - x0; dy = y1 - y0;
    sx = (dx >= 0) ? 1 : -1;
    sy = (dy >= 0) ? 1 : -1;
    dx = dx >= 0 ? dx : -dx; dy = dy >= 0 ? dy : -dy;
    err = dx - dy;
    x = x0; y = y0;
    for (n = 0; n < 8192; n++)
    {
        DP_PlotC(x, y, r, g, b, a);
        DP_PlotC(x, y - 1, r, g, b, a);
        DP_PlotC(x, y + 1, r, g, b, a);
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

static void DP_MarkC(int cx, int cy, int m, int r, int g, int b, unsigned a)
{
    int k, j;
    for (k = -m; k <= m; k++)
        for (j = -m; j <= m; j++)
            DP_PlotC(cx + k, cy + j, r, g, b, a);
}

/* 细线 (Bresenham 单像素, 目标格描边用; PlotC 自带裁剪)。 */
static void DP_ThinLine(int x0, int y0, int x1, int y1, unsigned a)
{
    int dx, dy, sx, sy, err, e2, x, y, n;
    dx = x1 - x0; dy = y1 - y0;
    sx = (dx >= 0) ? 1 : -1;
    sy = (dy >= 0) ? 1 : -1;
    dx = dx >= 0 ? dx : -dx; dy = dy >= 0 ? dy : -dy;
    err = dx - dy;
    x = x0; y = y0;
    for (n = 0; n < 8192; n++)
    {
        DP_PlotC(x, y, 255, 255, 0, a);
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

static int g_DpLoggedOnce = 0;

void DeployPreview_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* vt;
    DP_LockFn lockFn;
    DP_UnlockFn unlockFn;
    unsigned char* locked;
    int mcx, mcy, mlz;
    int fw, fh, bx, by;
    int x0, y0, x1, y1, x2, y2, x3, y3;
    int z0, z1, z2, z3, zt;
    extern int g_Calibrated;
    DP_EnsureInit();
    if (!g_DpEnable) return;
    if (!OU_KeyDown(VK_MBUTTON)) return;   /* 物理中键按住才显示 */
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    if (!DP_MouseCellZ(&mcx, &mcy, &mlz)) return;   /* 逻辑格+光标地面高度 */
    if (!DP_FootprintCached(&fw, &fh)) return;
    /* 基点 = 光标格 + 引擎偏移 (大地基 -1,-1; 小地基 0) */
    if (fw > 2 || fh > 2) { bx = mcx - 1; by = mcy - 1; }
    else { bx = mcx; by = mcy; }
    if (!g_DpLoggedOnce)
    {
        char sb[160], t1[16], t2[16], t5[16], t6[16], t7[16], t8[16];
        unsigned q = 0;
        g_DpLoggedOnce = 1;
        q = AppendCStr(sb, q, "deployprev: fw=", sizeof(sb));
        UtoA((unsigned)fw, t1); q = AppendCStr(sb, q, t1, sizeof(sb));
        q = AppendCStr(sb, q, " fh=", sizeof(sb));
        UtoA((unsigned)fh, t2); q = AppendCStr(sb, q, t2, sizeof(sb));
        q = AppendCStr(sb, q, " cur=", sizeof(sb));
        UtoA((unsigned)(mcx < 0 ? 0 : mcx), t7); q = AppendCStr(sb, q, t7, sizeof(sb));
        q = AppendCh(sb, q, ',', sizeof(sb));
        UtoA((unsigned)(mcy < 0 ? 0 : mcy), t8); q = AppendCStr(sb, q, t8, sizeof(sb));
        q = AppendCStr(sb, q, " base=", sizeof(sb));
        UtoA((unsigned)(bx < 0 ? 0 : bx), t5); q = AppendCStr(sb, q, t5, sizeof(sb));
        q = AppendCh(sb, q, ',', sizeof(sb));
        UtoA((unsigned)(by < 0 ? 0 : by), t6); q = AppendCStr(sb, q, t6, sizeof(sb));
        LogLine(sb);
    }
    /* 四角投影在认领分支之后算 (认领态提前返回, 省 8 次投影)。 */
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    lockFn = (DP_LockFn)(*(void**)(vt + VT_DP_LOCK));
    unlockFn = (DP_UnlockFn)(*(void**)(vt + VT_DP_UNLOCK));
    if (!lockFn || !unlockFn) return;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return;
    s_DpLocked = locked; s_DpW = w; s_DpH = h; s_DpPitch = pitch;
    /* 认领态多框预览 (与松手执行同一套分配, 所见即所得): 起点=按下格,
     * 终点=当前格, Shift 锁轴。未认领时走下面单框路径。 */
    if (s_DpArmed && s_DpDepN0 > 0)
    {
        int ccx, ccy;
        static int bases[DP_DEP_CAP][2], dests[DP_DEP_CAP][2];
        int nPlot, k;
        int apx, apy;
        if (OU_MouseCell(&ccx, &ccy))
        {
            /* 单车起点=终点 (跟鼠标自由走); 多车 Shift 锁轴直线,
             * 轨迹够长走手绘线, 否则点按平铺 (与松手执行同式)。 */
            apx = (s_DpDepN0 == 1) ? ccx : s_DpPressCx;
            apy = (s_DpDepN0 == 1) ? ccy : s_DpPressCy;
            if (s_DpDepN0 == 1)
            {
                nPlot = DP_AssignPoint(ccx, ccy, s_DpDepN0,
                                       s_DpDepFw, s_DpDepFh,
                                       bases, dests);
            }
            else if (OU_KeyDown(VK_SHIFT))
            {
                nPlot = DP_AssignStraight(apx, apy, ccx, ccy,
                                          s_DpDepN0, s_DpDepFw, s_DpDepFh,
                                          bases, dests);
            }
            else if (DP_ArcLen(s_DpNx, s_DpNy, s_DpNodeN, ccx, ccy) >= DP_DRAG_MINLEN)
            {
                nPlot = DP_AssignLine(s_DpNx, s_DpNy, s_DpNodeN, ccx, ccy,
                                      s_DpDepN0, s_DpDepFw, s_DpDepFh,
                                      bases, dests);
            }
            else
            {
                nPlot = DP_AssignPoint(ccx, ccy, s_DpDepN0,
                                       s_DpDepFw, s_DpDepFh,
                                       bases, dests);
            }
            /* 轨迹白线 (认领后 SmartOrder 熄火, 这里补上; 与裸中键同观感):
             * 节点格心连线 + 当前格心橡皮尾巴, 引擎投影吸格。 */
            {
                int j, lx, ly;
                int nn = s_DpNodeN;
                if (nn > DP_NODE_CAP) nn = DP_NODE_CAP;
                if (nn > 0 && DP_CenterProj(s_DpNx[0], s_DpNy[0], mlz, &lx, &ly))
                {
                    int px = lx, py = ly;
                    DP_MarkC(px, py, 3, 255, 255, 255, 255);   /* 锚点 7x7 */
                    for (j = 1; j < nn; j++)
                    {
                        int qx, qy;
                        if (!DP_CenterProj(s_DpNx[j], s_DpNy[j], mlz, &qx, &qy))
                            continue;
                        DP_LineC(px, py, qx, qy, 255, 255, 255, 230);
                        DP_MarkC(qx, qy, 1, 255, 255, 255, 230);
                        px = qx; py = qy;
                    }
                    {
                        int qx = px, qy = py, ok = 0;
                        if (DP_CenterProj(ccx, ccy, mlz, &qx, &qy))
                        {
                            ok = 1;
                            if (qx != px || qy != py)
                                DP_LineC(px, py, qx, qy, 255, 255, 255, 230);
                        }
                        if (ok) DP_MarkC(qx, qy, 2, 255, 255, 255, 255);   /* 当前 5x5 */
                    }
                }
            }
            for (k = 0; k < nPlot; k++)
            {
                int px0, py0, px1, py1, px2, py2, px3, py3;
                int qx0, qy0, qx1, qy1, qx2, qy2, qx3, qy3;
                int za, zb, zc, zd, zte;
                int icx0 = bases[k][0], icy0 = bases[k][1];
                int icx1 = bases[k][0] + s_DpDepFw[k] - 1, icy1 = bases[k][1];
                int icx2 = bases[k][0] + s_DpDepFw[k] - 1, icy2 = bases[k][1] + s_DpDepFh[k] - 1;
                int icx3 = bases[k][0], icy3 = bases[k][1] + s_DpDepFh[k] - 1;
                DP_CornerZ(icx0, icy0, mlz, &za);
                DP_CornerZ(icx1, icy1, mlz, &zb);
                DP_CornerZ(icx2, icy2, mlz, &zc);
                DP_CornerZ(icx3, icy3, mlz, &zd);
                if (!DP_Corner(bases[k][0], bases[k][1], za, &px0, &py0)) continue;
                if (!DP_Corner(bases[k][0] + s_DpDepFw[k], bases[k][1], zb, &px1, &py1)) continue;
                if (!DP_Corner(bases[k][0] + s_DpDepFw[k], bases[k][1] + s_DpDepFh[k], zc, &px2, &py2)) continue;
                if (!DP_Corner(bases[k][0], bases[k][1] + s_DpDepFh[k], zd, &px3, &py3)) continue;
                DP_LineC(px0, py0, px1, py1, 255, 255, 0, 255);
                DP_LineC(px1, py1, px2, py2, 255, 255, 0, 255);
                DP_LineC(px2, py2, px3, py3, 255, 255, 0, 255);
                DP_LineC(px3, py3, px0, py0, 255, 255, 0, 255);
                DP_CornerZ(dests[k][0], dests[k][1], mlz, &zte);
                if (!DP_Corner(dests[k][0], dests[k][1], zte, &qx0, &qy0)) continue;
                if (!DP_Corner(dests[k][0] + 1, dests[k][1], zte, &qx1, &qy1)) continue;
                if (!DP_Corner(dests[k][0] + 1, dests[k][1] + 1, zte, &qx2, &qy2)) continue;
                if (!DP_Corner(dests[k][0], dests[k][1] + 1, zte, &qx3, &qy3)) continue;
                DP_ThinLine(qx0, qy0, qx1, qy1, 200);
                DP_ThinLine(qx1, qy1, qx2, qy2, 200);
                DP_ThinLine(qx2, qy2, qx3, qy3, 200);
                DP_ThinLine(qx3, qy3, qx0, qy0, 200);
            }
            s_DpLocked = 0;
            unlockFn(surf, NULL);
            return;
        }
    }
    /* 未认领单框路径: 投影四角 (与游戏渲染同系同精度, 吸格线;
     * 每角取其框内格高, 平地高地同式)。 */
    DP_CornerZ(bx, by, mlz, &z0);
    DP_CornerZ(bx + fw - 1, by, mlz, &z1);
    DP_CornerZ(bx + fw - 1, by + fh - 1, mlz, &z2);
    DP_CornerZ(bx, by + fh - 1, mlz, &z3);
    if (!DP_Corner(bx, by, z0, &x0, &y0)) return;
    if (!DP_Corner(bx + fw, by, z1, &x1, &y1)) return;
    if (!DP_Corner(bx + fw, by + fh, z2, &x2, &y2)) return;
    if (!DP_Corner(bx, by + fh, z3, &x3, &y3)) return;
    DP_LineC(x0, y0, x1, y1, 255, 255, 0, 255);
    DP_LineC(x1, y1, x2, y2, 255, 255, 0, 255);
    DP_LineC(x2, y2, x3, y3, 255, 255, 0, 255);
    DP_LineC(x3, y3, x0, y0, 255, 255, 0, 255);
    DP_MarkC(x0, y0, 1, 255, 255, 0, 255);
    DP_MarkC(x1, y1, 1, 255, 255, 0, 255);
    DP_MarkC(x2, y2, 1, 255, 255, 0, 255);
    DP_MarkC(x3, y3, 1, 255, 255, 0, 255);
    /* 目标格细描边 (车会开到的格 = 光标格, 松手即下令): 光标在哪格,
     * 细框就套哪格, 与黄框是否对齐一眼可见。 */
    {
        int tx0, ty0, tx1, ty1, tx2, ty2, tx3, ty3;
        DP_CornerZ(mcx, mcy, mlz, &zt);
        if (!DP_Corner(mcx, mcy, zt, &tx0, &ty0)) return;
        if (!DP_Corner(mcx + 1, mcy, zt, &tx1, &ty1)) return;
        if (!DP_Corner(mcx + 1, mcy + 1, zt, &tx2, &ty2)) return;
        if (!DP_Corner(mcx, mcy + 1, zt, &tx3, &ty3)) return;
        DP_ThinLine(tx0, ty0, tx1, ty1, 200);
        DP_ThinLine(tx1, ty1, tx2, ty2, 200);
        DP_ThinLine(tx2, ty2, tx3, ty3, 200);
        DP_ThinLine(tx3, ty3, tx0, ty0, 200);
    }
    s_DpLocked = 0;
    unlockFn(surf, NULL);
}
