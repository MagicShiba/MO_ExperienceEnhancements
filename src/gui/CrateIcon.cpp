/* CrateIcon.cpp - 已定型箱子在格子上方显示类型图标。
 *
 * 数据源是引擎拾取时读的那两个字段: CellClass::OverlayTypeIndex (+0x44) 指向
 * OverlayTypeClass::Crate (+0x2AA) 为真, 再取 CellClass::OverlayData (+0x11E) 作
 * 类型字节。只扫视图区覆盖的格子; 不走 OverlayClass::Array —— 覆盖物对象的位置不
 * 是格坐标, 按它反推会全塌到 (0,0)。
 *
 * 类型字节 0xFF = 未定型 (随机刷的箱子, 拾取时才 roll) 不显示; 16~18 没有图标;
 * 6 在引擎分派前被改写成 0, 复用金钱图标。
 *
 * 画在 Composite (天气之后、UI 之前)。游戏目录有散文件 shroud.shp = 黑幕换成了
 * 半透明件 (原版在 mix 里): 黑幕格压暗一半照画; 没有则黑幕格跳过。是否在黑幕下
 * 读 CellClass::AltFlags 的 bit3 (置位 = 已揭示)。
 *
 * 不要往引擎的地图缓冲 [0x87E8A4] 写: 它是环形缓冲, 取址函数 0x4114B0 内部锁完
 * 立刻解锁, 外层没持锁时指针已失效 —— 屏幕上看不见, 还会写坏堆。
 */

#include <windows.h>
#include <string.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "GridView.h"
#include "OrderUtil.h"
#include "PcxImage.h"
#include "CrateIcon.h"

#define CI_SECTION  "CrateIcon"
#define CI_FILE     "icon_crate_type.pcx"
#define CI_SHROUD   "shroud.shp"

#define CI_COLS     9
#define CI_ROWS     2
#define CI_SHEET_W  216
#define CI_SHEET_H  50
#define CI_CELL_W   (CI_SHEET_W / CI_COLS)
#define CI_CELL_H   (CI_SHEET_H / CI_ROWS)

#define CI_MAX       256          /* 同屏箱子上限 (超出的不画) */
#define CI_SCAN_MS   200u         /* 列表重建间隔 (拾取/新生成靠它跟上) */
#define CI_LIFT      16           /* 图标底边离格中心的像素高度 */
#define CI_OFF_X     (-2)         /* 水平微调: 投影点不是木箱视觉中心 */
#define CI_OFF_Y     (-4)         /* 垂直微调 */
#define CI_TYPES_MAX 512          /* OverlayTypes 个数上限 (缓存表容量) */
#define CI_CAP_CELLS 20000        /* 视野过宽 (拉远整图) 时不扫 */
#define CI_LEP_CELL  256

#define CI_KEY_PINK 0xF81Fu       /* 品红透明键 (表的第一像素就是它) */

/* 类型字节 -> 图标格位 (行主序)。-1 = 没有图标。 */
static const signed char s_Slot[19] = {
     0,  1,  2,  3,  4,  5,  0,  6,  7,  8,
     9, 10, 11, 12, 13, 14, -1, -1, -1
};

typedef struct { short cx, cy; unsigned char slot; } CiCrate;

static int s_Init, s_On = 1, s_Semi, s_Loaded;
static unsigned s_LastScan, s_LastTry;

static CiCrate s_List[CI_MAX];
static int s_N;
static int s_View[4] = { -1, -1, -1, -1 };   /* 上次扫描的视野格矩形 */

static unsigned char s_CrateType[CI_TYPES_MAX];
static unsigned short s_Sheet[CI_SHEET_W * CI_SHEET_H];
static unsigned short s_Key;

/* ---------------- 配置 ---------------- */

static void Ci_Init(void)
{
    if (s_Init) return;
    s_Init = 1;
    BuildPaths();
    s_On = GetPrivateProfileIntA(CI_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    s_Semi = FileAtGameRoot(CI_SHROUD);
    if (s_On) LogLine(s_Semi ? "crateicon: on (half shroud)" : "crateicon: on");
}

int CrateIcon_On(void)
{
    Ci_Init();
    return s_On;
}

void CrateIcon_SetOn(int on)
{
    Ci_Init();
    on = on ? 1 : 0;
    if (on == s_On) return;
    s_On = on;
    /* 扫描缓存一起作废: 重新打开时按当前视野重扫一遍。 */
    s_N = 0;
    s_View[0] = -1;
    if (on) LogLine("crateicon: on");
}

/* ---------------- 图标表 ---------------- */

/* 读一次整张表进本地缓冲 (引擎的 PCX 面不保证跨帧有效), 尺寸不对就每 500ms 重试。 */
static int Ci_LoadSheet(void)
{
    PcxImage img;
    if (s_Loaded) return 1;
    if (s_LastTry && (unsigned)(GetTickCount() - s_LastTry) < 500u) return 0;
    s_LastTry = GetTickCount();
    if (!PcxImage_Load(&img, CI_FILE, 2) ||
        img.width != CI_SHEET_W || img.height != CI_SHEET_H)
        return 0;
    memcpy(s_Sheet, img.px, sizeof(s_Sheet));
    s_Key = img.key;
    s_Loaded = 1;
    return 1;
}

/* ---------------- 箱子列表 (缓存) ---------------- */

/* 覆盖物类型索引 -> 是否箱子, 返回类型个数。每轮重建刷一次: 换图/重载 rules
 * 后索引会变。 */
static int Ci_BuildTypeCache(void)
{
    void** items = *(void***)ADDR_OVLTYPES_ITEMS;
    int cnt = *(int*)ADDR_OVLTYPES_COUNT;
    int i;

    if (cnt < 0 || cnt > CI_TYPES_MAX || (cnt && (!items || !IsPlausiblePtr(items)))) cnt = 0;
    for (i = 0; i < CI_TYPES_MAX; i++)
        s_CrateType[i] = (i < cnt && items[i] && IsPlausiblePtr(items[i]) &&
                          *(unsigned char*)((char*)items[i] + OFF_OVLTYPE_CRATE)) ? 1 : 0;
    return cnt;
}

/* 扫一片格子, 结果换进 s_List。视野过大 (拉远) 直接清空。 */
static void Ci_ScanRect(int x0, int y0, int x1, int y1)
{
    int x, y, ntype;

    s_LastScan = GetTickCount();
    s_N = 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 511) x1 = 511;
    if (y1 > 511) y1 = 511;
    if (x1 < x0 || y1 < y0) return;
    if ((x1 - x0 + 1) * (y1 - y0 + 1) > CI_CAP_CELLS) return;

    ntype = Ci_BuildTypeCache();
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
        {
            const char* cell = (const char*)OU_CellPtr(x, y);
            unsigned data;
            int ovt, slot;
            if (!cell) continue;
            ovt = *(const int*)(cell + OFF_CELL_OVERLAY);
            if (ovt < 0 || ovt >= ntype || !s_CrateType[ovt]) continue;
            data = *(const unsigned char*)(cell + OFF_CELL_OVDATA);
            if (data >= sizeof(s_Slot)) continue;
            slot = s_Slot[data];
            if (slot < 0 || s_N >= CI_MAX) continue;
            s_List[s_N].cx = (short)x;
            s_List[s_N].cy = (short)y;
            s_List[s_N].slot = (unsigned char)slot;
            s_N++;
        }
}

/* ---------------- 绘制 ---------------- */

/* RGB565 每通道右移一位 = 亮度减半 (半透明黑幕的遮盖效果)。 */
static unsigned short Ci_Half(unsigned short v)
{
    unsigned r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    return (unsigned short)(((r >> 1) << 11) | ((g >> 1) << 5) | (b >> 1));
}

/* 从表里取一格贴到 16 位面, 品红跳过; 只在 [0,W)×[0,H) 内落笔。 */
static void Ci_Blit(unsigned char* base, int pitch, int W, int H,
                    int x, int y, int slot, int half)
{
    const unsigned short* src = s_Sheet + (slot / CI_COLS * CI_CELL_H) * CI_SHEET_W
                                        + (slot % CI_COLS) * CI_CELL_W;
    int r, c, c0 = 0, c1 = CI_CELL_W, r0 = 0, r1 = CI_CELL_H;

    if (x >= W || y >= H || x + CI_CELL_W <= 0 || y + CI_CELL_H <= 0) return;
    if (x < 0) c0 = -x;
    if (y < 0) r0 = -y;
    if (x + CI_CELL_W > W) c1 = W - x;
    if (y + CI_CELL_H > H) r1 = H - y;

    for (r = r0; r < r1; r++)
    {
        unsigned short* dst = (unsigned short*)(base + (size_t)(y + r) * (size_t)pitch);
        const unsigned short* s = src + r * CI_SHEET_W;
        for (c = c0; c < c1; c++)
        {
            unsigned short v = s[c];
            if (v == s_Key || v == CI_KEY_PINK) continue;
            dst[x + c] = half ? Ci_Half(v) : v;
        }
    }
}

int CrateIcon_Draw(void* surf, int W, int H, int pitch)
{
    const RectangleStruct* vb = (const RectangleStruct*)ADDR_VIEWBOUNDS;
    unsigned char* base;
    int v[4], i, n = 0;

    Ci_Init();
    if (!s_On || !surf || !IsPlausiblePtr(surf)) return 0;
    if (W <= 0 || H <= 0 || pitch < W * 2) return 0;
    if (!Ci_LoadSheet()) return 0;
    if (!GV_ViewCellRect(&v[0], &v[1], &v[2], &v[3])) return 0;

    /* 视野一变立刻重建, 否则按墙钟兜底 (拾取/新生成靠它跟上)。 */
    if (memcmp(v, s_View, sizeof(v)) ||
        (unsigned)(GetTickCount() - s_LastScan) >= CI_SCAN_MS)
    {
        memcpy(s_View, v, sizeof(v));
        Ci_ScanRect(v[0], v[1], v[2], v[3]);
    }
    if (s_N <= 0) return 0;

    base = GtLock(surf);
    if (!base) return 0;
    for (i = 0; i < s_N; i++)
    {
        int cx = s_List[i].cx, cy = s_List[i].cy;
        const char* cell = (const char*)OU_CellPtr(cx, cy);
        int sx, sy, z = 0, half = 0;
        if (!cell) continue;
        /* 黑幕下 (bit3 未置): 半透明黑幕压暗照画, 不透明黑幕跳过。 */
        if (!(*(const unsigned*)(cell + OFF_CELL_ALTFLAGS) & CELL_SHROUD_REVEALED))
        {
            if (!s_Semi) continue;
            half = 1;
        }
        OU_CellSurfaceZ(cx, cy, &z);   /* 失败用 0, 桥面优先 */
        if (!OU_ProjWorld(cx * CI_LEP_CELL + CI_LEP_CELL / 2,
                          cy * CI_LEP_CELL + CI_LEP_CELL / 2, z, &sx, &sy))
            continue;
        Ci_Blit(base, pitch, W, H, sx - vb->X - CI_CELL_W / 2 + CI_OFF_X,
                sy - vb->Y - CI_CELL_H - CI_LIFT + CI_OFF_Y, s_List[i].slot, half);
        n++;
    }
    GtUnlock(surf);
    return n > 0;
}
