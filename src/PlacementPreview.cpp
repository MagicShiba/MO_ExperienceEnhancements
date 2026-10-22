/* PlacementPreview.cpp - 建筑放置半透明预览 (移植 Phobos PlacementPreview)。
 *
 * 机制 (references/Phobos/src/Ext/BuildingType/Hooks.cpp:68):
 *   引擎 TacticalClass::DrawPlacement 画完绿/红地基格后 (0x6D528A,
 *   6 字节 = mov ecx,[0x880990]) 追加一次静态 SHP 半透明 blit。
 *   原版正常对局只画地基格, 不画建筑影像, 因此这里只是额外补一层预览。
 *
 * 数据全部只读:
 *   - DisplayClass (0x87F7E8) +0x1174/0x1178 = 当前地基中心格/左上偏移
 *   - +0x11A4/+0x11A8 = CurrentBuilding / CurrentBuildingType (本地幽灵对象)
 *   - DisplayClass::GetFloorHeight + TacticalClass::CoordsToClient 只做投影
 *   - BuildingTypeClass vtable 槽 39 = GetImage, 48 = LoadBuildup
 *     (vtable 0x7E4570 实测; LoadBuildup 内部按需加载 Buildup, 不做开局预载)
 *   - 绘制走引擎 CC_Draw_Shape (0x4AED70) 到 DSurface::Temp (0x887314)
 * 不写任何模拟状态; 每个客户端画自己的放置幽灵, 联机安全。
 *
 * 与 Phobos 避让: 每帧检测 Phobos.dll, 已加载则完全不画, 避免双层预览。
 *
 * 配置 [PlacementPreview]: Enable=1, Translucency=0/25/50/75 (默认 75)。
 * Translucency 同时是展开预览虚影 ([DeployPreview] Ghost) 的透明度,
 * 由 DeployPreview 通过 PlacementPreview_GetTranslucency 直接取用。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "ScaleBar.h"
#include "PlacementPreview.h"

#define PP_SECTION "PlacementPreview"

/* ---------------- 只读地址 ---------------- */

#define PP_ADDR_DISPLAY          0x87F7E8u
#define PP_OFF_FND_CENTER        0x1174u   /* CellStruct 当前地基中心格 */
#define PP_OFF_FND_TOPLEFT       0x1178u   /* CellStruct 中心到地基左上偏移 */
#define PP_OFF_CUR_BUILDING      0x11A4u   /* BuildingClass* 本地放置幽灵 */
#define PP_OFF_CUR_BUILDING_TYPE 0x11A8u   /* BuildingTypeClass* */

#define PP_ADDR_TACTICAL         0x887324u
#define PP_ADDR_TEMP_SURF        0x887314u /* DSurface::Temp 当前绘制面 */
#define PP_FN_COORDS_TO_CLIENT   0x6D2140u
#define PP_FN_FLOOR_HEIGHT       0x47B3A0u
#define PP_FN_GET_DRAWER         0x705D70u /* TechnoClass::GetDrawer */
#define PP_FN_CC_DRAW_SHAPE      0x4AED70u
#define PP_ADDR_UNITX_PAL        0x87F6B4u /* FileSystem::UNITx_PAL */

/* BuildingTypeClass vtable 槽 (gamemd 0x7E4570 实测):
 *   39 = ObjectTypeClass::GetImage (this+0xA4, 需加载时懒加载)
 *   48 = BuildingTypeClass::LoadBuildup (this+0xE00/0xE5C)
 * 与 Phobos 用虚函数的语义一致; 先用 Buildup, 空则退回 Image 第 0 帧。 */
#define PP_VT_GET_IMAGE    39
#define PP_VT_LOAD_BUILDUP 48

/* BlitterFlags */
#define PP_FLAG_TRANS25   0x2u
#define PP_FLAG_TRANS50   0x4u
#define PP_FLAG_TRANS75   0x6u
#define PP_FLAG_MULTIPASS 0x100u
#define PP_FLAG_CENTERED  0x200u
#define PP_FLAG_NONZERO   0x20000u

/* Phobos 默认偏移 (palette/shape 用默认): 屏幕 X/Y 加 0,-15, Z 抬 1。 */
#define PP_OFFSET_X 0
#define PP_OFFSET_Y (-15)
#define PP_OFFSET_Z 1

/* 缩放档虚影 (自绘缩放) 用的尺寸常量。 */
#define PP_CANVAS_MAX   1024      /* 画布尺寸上限 (防坏数据/巨图) */
#define PP_SCR_MAX      640       /* 离屏面单边上限 */
#define PP_GHOST_MARGIN 4         /* 画布四周留白 (缩放取整别裁边) */
#define PP_ZOOM_PIX_MAX 2000000L  /* 单块缩放虚影像素上限 (逐像素混合, 太大卡帧) */

typedef int (__attribute__((thiscall)) *PpC2C)(void* tac,
    const CoordStruct* c, Point2D* out);
typedef int (__attribute__((thiscall)) *PpFloorHeight)(void* cell,
    const Point2D* p);
typedef void* (__attribute__((thiscall)) *PpGetDrawer)(void* building);
typedef void* (__attribute__((thiscall)) *PpTypeFn)(void* type);
/* 与 SWSBar.cpp 的 SwsDrawShpFn 同签名: __fastcall ecx=Surface, edx=Palette,
 * 其余按栈传, ret 0x38。 */
typedef void (__fastcall *PpDrawShp)(void* surf, void* pal, void* shp,
    int frame, Point2D* pos, RectangleStruct* bounds, int flags, int remap,
    int zadjust, int zgrad, int bright, int tint, void* zshape, int zframe,
    int xoff, int yoff);

/* ---------------- 配置 ---------------- */

static int g_PpInit = 0;
static int g_PpEnable = 1;
static int g_PpTrans = 75;

static void PP_EnsureInit(void)
{
    char buf[48];
    unsigned p = 0;
    int t;
    if (g_PpInit) return;
    g_PpInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_PpEnable = GetPrivateProfileIntA(PP_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    t = GetPrivateProfileIntA(PP_SECTION, "Translucency", 75, g_IniPath);
    if (t >= 75) t = 75;
    else if (t >= 50) t = 50;
    else if (t >= 25) t = 25;
    else t = 0;
    g_PpTrans = t;
    if (!g_PpEnable)
    {
        LogLine("placeprev: OFF");
        return;
    }
    p = AppendCStr(buf, p, "placeprev: ON trans=", sizeof(buf));
    p += UtoA((unsigned)t, buf + p);
    LogLine(buf);
}

/* ---------------- 公共虚影绘制 (放置预览 / 展开预览共用) ---------------- */

/* [PlacementPreview] Translucency 当前生效值 (展开预览等模块共用同一档)。 */
int PlacementPreview_GetTranslucency(void)
{
    PP_EnsureInit();
    return g_PpTrans;
}

/* ---------------- 形状/调色板解析 (1:1 与缩放两条路径共用) ---------------- */

/* 解出要画的 SHP + 帧 + 画布尺寸 (SHPStruct 宽高 = 该文件最大画布)。
 * 1=成功; 画布尺寸拿不到时回 0 (1:1 路径仍能画, 缩放路径不画)。
 * wOut/hOut 可传 NULL (1:1 路径不需要画布尺寸)。 */
static int PP_ResolveShape(void* type, void** shpOut, int* frameOut,
                           int* wOut, int* hOut)
{
    void** vt;
    void* shp;
    unsigned fn;
    int frame, frames, w, h;

    if (!type || !IsPlausiblePtr(type)) return 0;
    vt = *(void***)type;
    if (!vt || !IsPlausiblePtr(vt)) return 0;

    /* 默认 Buildup 的“最后一非影子帧”: 前半段升起到完成, 后半段是影子。
     * 无 Buildup 时退回建筑 Image 第 0 帧 (与 Phobos 同)。 */
    shp = NULL;
    frame = 0;
    fn = (unsigned)vt[PP_VT_LOAD_BUILDUP];
    if (OU_InExe(fn))
        shp = ((PpTypeFn)(void*)fn)(type);

    if (shp && IsPlausiblePtr(shp))
    {
        frames = (int)*(short*)((char*)shp + 6);   /* SHPStruct::Frames */
        if (frames <= 0) return 0;
        frame = frames / 2 - 1;
        if (frame < 0) frame = 0;
        if (frame >= frames) frame = frames - 1;
    }
    else
    {
        fn = (unsigned)vt[PP_VT_GET_IMAGE];
        if (!OU_InExe(fn)) return 0;
        shp = ((PpTypeFn)(void*)fn)(type);
        if (!shp || !IsPlausiblePtr(shp)) return 0;
        frame = 0;
    }

    /* 画布尺寸: SHP 头里的最大宽高。缩放路径靠它算离屏面大小, 1:1 路径不用,
     * 所以取不到也不算失败 (回 0, 由调用方决定能不能画)。 */
    w = (int)*(short*)((char*)shp + 2);            /* SHPStruct::Width  */
    h = (int)*(short*)((char*)shp + 4);            /* SHPStruct::Height */
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    if (w > PP_CANVAS_MAX) w = 0;
    if (h > PP_CANVAS_MAX) h = 0;

    *shpOut = shp;
    *frameOut = frame;
    if (wOut) *wOut = w;
    if (hOut) *hOut = h;
    return 1;
}

/* 绘制用调色板: owner 的玩家色/光照; 无 owner 或取失败退回 UNITx_PAL。 */
static void* PP_ResolvePal(void* owner)
{
    void* pal = NULL;
    if (owner && IsPlausiblePtr(owner))
        pal = ((PpGetDrawer)(void*)PP_FN_GET_DRAWER)(owner);
    if (!pal || !IsPlausiblePtr(pal))
        pal = *(void**)(PP_ADDR_UNITX_PAL);
    if (!pal || !IsPlausiblePtr(pal)) return NULL;
    return pal;
}

/* 原尺寸引擎 blit: (cx,cy) = 画布中心, 裁剪框 (0,0,bw,bh)。 */
static void PP_Blit1to1(void* surf, void* shp, int frame, void* pal,
                        int cx, int cy, int trans, int bw, int bh)
{
    Point2D point;
    RectangleStruct bounds;
    unsigned flags;

    flags = PP_FLAG_CENTERED | PP_FLAG_NONZERO | PP_FLAG_MULTIPASS;
    if (trans == 25) flags |= PP_FLAG_TRANS25;
    else if (trans == 50) flags |= PP_FLAG_TRANS50;
    else if (trans == 75) flags |= PP_FLAG_TRANS75;

    bounds.X = 0;
    bounds.Y = 0;
    bounds.Width = bw;
    bounds.Height = bh;
    point.X = cx;
    point.Y = cy;
    ((PpDrawShp)(void*)PP_FN_CC_DRAW_SHAPE)(surf, pal, shp, frame, &point,
        &bounds, (int)flags, 0, 0, 0 /* ZGradient::Ground */, 1000, 0,
        NULL, 0, 0, 0);
}

static void PP_DrawGhost1to1(void* type, void* owner, void* surf,
    int x, int y, int trans)
{
    void* shp;
    void* pal;
    int frame, w, h;

    if (!surf || !IsPlausiblePtr(surf)) return;
    if (!PP_ResolveShape(type, &shp, &frame, NULL, NULL)) return;
    pal = PP_ResolvePal(owner);
    if (!pal) return;

    w = GtSurfGet(surf, GtVT_GET_WIDTH);
    h = GtSurfGet(surf, GtVT_GET_HEIGHT);
    if (w <= 0 || h <= 32) return;
    PP_Blit1to1(surf, shp, frame, pal, x, y, trans, w, h - 32);  /* 不画进工具栏 */
}

/* ---------------- 缩放档: 离屏原尺寸渲染 + 最近邻缩放 + 半透明混合 ----------------
 * 界面缩放下 (ScaleBar) 展开预览画在合成面 A 上, 而引擎 SHP blit 不会缩放,
 * 所以这里自己来: 先把虚影 1:1 画进一块离屏面 (引擎负责调色板/玩家色/透明),
 * 再最近邻缩放到屏幕矩形, 按 trans 与目标面像素混合写回。
 * 离屏面清 0x0000, 引擎 blit 带 Nonzero 跳过透明像素 ⇒ 非 0 即虚影像素。
 * 缩放结果按 (SHP,帧,调色板,目标宽高) 缓存, 换档/换类型才重建。 */

static void* s_PpScr = 0;
static int s_PpScrW = 0, s_PpScrH = 0;

static void* s_PpBmp = 0;         /* 缩放后 RGB565 位图 (0x0000 = 透明) */
static int s_PpBmpCap = 0;        /* 容量 (像素) */
static int s_PpBmpW = 0, s_PpBmpH = 0;
static void* s_PpKeyShp = 0;
static int s_PpKeyFrame = -1;

/* 取足够大的离屏面 (尺寸变化就重建)。 */
static void* PP_Scratch(int w, int h)
{
    if (w <= 0 || h <= 0 || w > PP_SCR_MAX || h > PP_SCR_MAX) return 0;
    if (s_PpScr && s_PpScrW == w && s_PpScrH == h) return s_PpScr;
    if (s_PpScr)
    {
        SB_FreeOffscreen(s_PpScr);
        s_PpScr = 0;
        s_PpScrW = s_PpScrH = 0;
    }
    s_PpScr = SB_NewOffscreen(w, h);
    if (!s_PpScr) return 0;
    s_PpScrW = w;
    s_PpScrH = h;
    return s_PpScr;
}

/* 把 16bpp 离屏面 (bw x bh) 最近邻缩放到 s_PpBmp (dw x dh)。 */
static int PP_BuildScaled(unsigned char* src, int srcPitch, int bw, int bh,
                          int dw, int dh)
{
    int need, x, y, sx, sy, xacc, yacc;
    unsigned short* dst;

    need = dw * dh;
    if (need <= 0 || bw <= 0 || bh <= 0) return 0;
    if (!s_PpBmp || s_PpBmpCap < need)
    {
        void* nb;
        nb = HeapAlloc(GetProcessHeap(), 0,
                       (SIZE_T)need * sizeof(unsigned short));
        if (!nb) return 0;
        if (s_PpBmp) HeapFree(GetProcessHeap(), 0, s_PpBmp);
        s_PpBmp = nb;
        s_PpBmpCap = need;
        s_PpBmpW = s_PpBmpH = 0;
    }
    dst = (unsigned short*)s_PpBmp;

    yacc = 0; sy = 0;
    for (y = 0; y < dh; y++)
    {
        unsigned short* srow = (unsigned short*)(src + (size_t)sy * srcPitch);
        unsigned short* drow = dst + (size_t)y * dw;
        xacc = 0; sx = 0;
        for (x = 0; x < dw; x++)
        {
            drow[x] = srow[sx];
            xacc += bw;
            while (xacc >= dw) { xacc -= dw; sx++; }
        }
        yacc += bh;
        while (yacc >= dh) { yacc -= dh; sy++; }
        if (sy >= bh) sy = bh - 1;
    }
    s_PpBmpW = dw;
    s_PpBmpH = dh;
    return 1;
}

/* 16bpp 单像素混合: ws = 源权重 (0..256)。通道位宽按 GdiText 标定
 * (565 或 555; 表面格式由引擎当前视频模式决定)。 */
static unsigned short PP_Mix16(unsigned short s, unsigned short d, unsigned ws)
{
    unsigned gb, rs, gm, sr, sg, sb, dr, dg, db, wd;

    gb = (g_GBits == 6) ? 6u : 5u;
    gm = (gb == 6) ? 63u : 31u;
    rs = gb + 5u;                     /* R 起点: 565=11, 555=10 */
    wd = 256u - ws;
    sr = (s >> rs) & 31u; sg = (s >> 5) & gm; sb = s & 31u;
    dr = (d >> rs) & 31u; dg = (d >> 5) & gm; db = d & 31u;
    return (unsigned short)((((sr * ws + dr * wd) >> 8) << rs)
                          | (((sg * ws + dg * wd) >> 8) << 5)
                          |   ((sb * ws + db * wd) >> 8));
}

/* 把缓存位图混合写进目标面 (调用方已锁面), 裁剪到 (0,0,w,h)。 */
static void PP_BlendBmp(unsigned char* base, int pitch, int w, int h,
                        int dx, int dy, int trans)
{
    unsigned short* bmp = (unsigned short*)s_PpBmp;
    unsigned ws;
    int x, y, x0, x1, y0, y1;

    if (!bmp || s_PpBmpW <= 0 || s_PpBmpH <= 0) return;
    if (trans >= 75) ws = 64u;
    else if (trans >= 50) ws = 128u;
    else if (trans >= 25) ws = 192u;
    else ws = 256u;

    x0 = dx < 0 ? -dx : 0;
    y0 = dy < 0 ? -dy : 0;
    x1 = s_PpBmpW;
    if (dx + x1 > w) x1 = w - dx;
    y1 = s_PpBmpH;
    if (dy + y1 > h) y1 = h - dy;
    if (x0 >= x1 || y0 >= y1) return;

    for (y = y0; y < y1; y++)
    {
        unsigned short* srow = bmp + (size_t)y * s_PpBmpW;
        unsigned short* drow = (unsigned short*)(base + (size_t)(dy + y) * pitch)
                             + (dx + x0);
        for (x = x0; x < x1; x++)
        {
            unsigned short s = srow[x];
            if (!s) continue;                 /* 0 = 非虚影像素 */
            drow[x - x0] = (ws >= 256u) ? s : PP_Mix16(s, drow[x - x0], ws);
        }
    }
}

/* 缩放档绘制: 视图空间锚点 (vx,vy) → 屏幕矩形, 半透明混合写入 surf。
 * 返回消耗的目标面像素数 (0 = 没画)。 */
static int PP_DrawGhostZoomed(void* type, void* owner, void* surf,
                              int vx, int vy, int trans)
{
    void *shp, *pal, *scr;
    int frame, sw, sh, bw, bh, dw, dh;
    int x0, y0, x1, y1, numX, numY;
    int tw, th, tpit, tbpp, spit;
    unsigned char* sc;
    unsigned char* db;

    if (!surf || !IsPlausiblePtr(surf)) return 0;
    if (!PP_ResolveShape(type, &shp, &frame, &sw, &sh)) return 0;
    if (sw <= 0 || sh <= 0) return 0;      /* 缩放要按画布尺寸开面, 取不到就不画 */
    bw = sw + PP_GHOST_MARGIN * 2;
    bh = sh + PP_GHOST_MARGIN * 2;
    pal = PP_ResolvePal(owner);
    if (!pal) return 0;
    scr = PP_Scratch(bw, bh);
    if (!scr) return 0;

    /* 屏幕缩放比: 每 1000 视图像素映到多少屏幕像素 (与实际投影同一套整数除法,
     * 位置无关, 避免每帧 ±1 抖导致缓存反复重建)。 */
    if (!SB_ViewToScreen(vx, vy, &x0, &y0)) return 0;
    if (!SB_ViewToScreen(vx + 1000, vy + 1000, &x1, &y1)) return 0;
    numX = x1 - x0;
    numY = y1 - y0;
    if (numX <= 0 || numY <= 0) return 0;
    dw = (int)(((long)bw * numX + 500) / 1000);
    dh = (int)(((long)bh * numY + 500) / 1000);
    if (dw < 8 || dh < 8 || dw > PP_SCR_MAX * 4) return 0;
    if ((long)dw * (long)dh > PP_ZOOM_PIX_MAX) return 0;   /* 太大就别画, 免逐像素卡帧 */

    /* 缓存命中就不重建 (1:1 渲染 + 缩放都省掉)。 */
    /* 调色板不进 key: 它随光照/单位状态抖动, 进 key 会逐帧重建 (只影响浓淡)。 */
    if (s_PpKeyShp != shp || s_PpKeyFrame != frame
        || s_PpBmpW != dw || s_PpBmpH != dh)
    {
        s_PpKeyShp = 0;
        s_PpBmpW = s_PpBmpH = 0;
        /* 离屏面清 0 (缓冲内容未定义), 再 1:1 画进去 (不带透明, 只留原始像素)。 */
        if (GtSurfGet(scr, GtVT_GET_BPP) != 2) return 0;   /* 2 = 16bpp */
        spit = GtSurfGet(scr, GtVT_GET_PITCH);
        if (spit <= 0) return 0;
        sc = GtLock(scr);
        if (!sc) return 0;
        memset(sc, 0, (size_t)spit * (size_t)bh);
        GtUnlock(scr);
        PP_Blit1to1(scr, shp, frame, pal, bw / 2, bh / 2, 0, bw, bh);
        sc = GtLock(scr);
        if (!sc) return 0;
        if (!PP_BuildScaled(sc, spit, bw, bh, dw, dh))
        {
            GtUnlock(scr);
            return 0;
        }
        GtUnlock(scr);
        s_PpKeyShp = shp;
        s_PpKeyFrame = frame;
    }

    /* 写回: 只有 16bpp 面能按 565/555 通道混合。 */
    tbpp = GtSurfGet(surf, GtVT_GET_BPP);
    if (tbpp != 2) return 0;            /* 2 = 16bpp (按字节) */
    tw = GtSurfGet(surf, GtVT_GET_WIDTH);
    th = GtSurfGet(surf, GtVT_GET_HEIGHT);
    tpit = GtSurfGet(surf, GtVT_GET_PITCH);
    if (tw <= 0 || th <= 32 || tpit < tw * 2) return 0;
    db = GtLock(surf);
    if (!db) return 0;
    PP_BlendBmp(db, tpit, tw, th - 32, x0 - dw / 2, y0 - dh / 2, trans);
    GtUnlock(surf);
    return dw * dh;
}

/* 视图空间锚点画虚影 (展开预览等界面层用): 1x 直通引擎 blit,
 * 缩放档走离屏缩放+混合。返回消耗的目标面像素数 (1x 恒 1)。 */
int PlacementPreview_DrawGhostView(void* type, void* owner, void* surf,
                                   int vx, int vy, int trans)
{
    int sx, sy;
    if (!type || !IsPlausiblePtr(type)) return 0;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    vx += PP_OFFSET_X;
    vy += PP_OFFSET_Y;
    /* 缩放路径失败 (面不合适/画布过大) 不退回 1:1: 大小和位置都会错。 */
    if (SB_Zoomed())
        return PP_DrawGhostZoomed(type, owner, surf, vx, vy, trans);
    if (!SB_ViewToScreen(vx, vy, &sx, &sy)) { sx = vx; sy = vy; }
    PP_DrawGhost1to1(type, owner, surf, sx, sy, trans);
    return 1;
}

/* ---------------- 绘制 hook ---------------- */

extern "C" __declspec(dllexport) DWORD __cdecl PlacementPreview_DrawHook(void* regs)
{
    void* building;
    void* type;
    void* cell;
    void* tac;
    void* surf;
    int cx, cy, z;
    CellStruct center;
    CellStruct topLeft;
    CoordStruct coords;
    Point2D point;
    Point2D zero;

    (void)regs;
    PP_EnsureInit();
    if (!g_PpEnable) return 0;

    /* 避让 Phobos: 已加载则完全跳过 (不加载图像, 也不画)。 */
    if (GetModuleHandleA("Phobos.dll") != NULL) return 0;

    building = *(void**)(PP_ADDR_DISPLAY + PP_OFF_CUR_BUILDING);
    type = *(void**)(PP_ADDR_DISPLAY + PP_OFF_CUR_BUILDING_TYPE);
    if (!building || !IsPlausiblePtr(building)) return 0;
    if (!type || !IsPlausiblePtr(type)) return 0;
    if (OU_WhatAmI(building) != ABS_BUILDING) return 0;

    center = *(CellStruct*)(PP_ADDR_DISPLAY + PP_OFF_FND_CENTER);
    topLeft = *(CellStruct*)(PP_ADDR_DISPLAY + PP_OFF_FND_TOPLEFT);
    cx = (int)center.X + (int)topLeft.X;
    cy = (int)center.Y + (int)topLeft.Y;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return 0;

    cell = OU_CellPtr(cx, cy);
    if (!cell || !IsPlausiblePtr(cell)) return 0;
    zero.X = 0;
    zero.Y = 0;
    z = ((PpFloorHeight)(void*)PP_FN_FLOOR_HEIGHT)(cell, &zero);
    if (z < -8192 || z > 8192) return 0;

    /* CellClass::Cell2Coord: 格中心 + 地面高度; 再加预览偏移。 */
    coords.X = cx * 256 + 128;
    coords.Y = cy * 256 + 128;
    coords.Z = z + PP_OFFSET_Z;

    tac = *(void**)(PP_ADDR_TACTICAL);
    if (!tac || !IsPlausiblePtr(tac)) return 0;
    point.X = 0;
    point.Y = 0;
    ((PpC2C)(void*)PP_FN_COORDS_TO_CLIENT)(tac, &coords, &point);
    point.X += PP_OFFSET_X;
    point.Y += PP_OFFSET_Y;

    surf = *(void**)(PP_ADDR_TEMP_SURF);
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    PP_DrawGhost1to1(type, building, surf, point.X, point.Y, g_PpTrans);
    return 0;   /* 返回 0 = 继续执行被覆盖的原指令 (mov ecx,[0x880990]) */
}
