/* IconSnap - 见 IconSnap.h。
 * 原为 UnitPanel (Up_PcxSurf/Up_SnapPcx/Up_RenderShp) 与 SWSBar (SwsPcxRawSurf/
 * SwsSnapPcxCap/SwsRenderShp) 各抄一份, 伪 BSurface 布局与 CC_Draw_Shape 参数
 * 逐字节同源, 收进一处防漂移。 */

#include <windows.h>
#include "GdiText.h"
#include "IconSnap.h"

typedef void* (__attribute__((thiscall)) *Fn_PcxGetSurf)(void* pcx, const char* name, void* pal);
typedef int (__attribute__((thiscall)) *Fn_PcxLoad)(void* pcx, const char* name, int f1, int f2);
/* gamemd 0x4AED70 `CC_Draw_Shape` (__fastcall: ecx=Surface, edx=Palette, ret 0x38)。
 * 栈参顺序与 YRpp `DrawSHP` 一致: SHP, Frame, Position, Bounds, Flags, Remap,
 * ZAdjust, ZGradient, Brightness, Tint, ZShape, ZFrame, XOff, YOff。 */
typedef void (__fastcall *Fn_DrawShape)(void* surf, void* pal, void* shp,
    int frame, Point2D* pos, RectangleStruct* bounds, int flags, int remap,
    int zadjust, int zgrad, int bright, int tint, void* zshape, int zframe,
    int xoff, int yoff);

/* 取 PCX 面 (GetSurface 命中直接用, 未命中则 ForceLoadFile 再取)。
 * 两条命门: GetSurface 未加载给的是占位图 (必须先 ForceLoadFile);
 * BSurface* 不可跨帧缓存 (切页签/重载美术会失效), 每次现取。 */
static void* SnapPcxRawSurf(const char* file)
{
    void* pcx, *surf;
    if (!file || !file[0]) return NULL;
    pcx = (void*)ADDR_PCX_INSTANCE;
    if (!pcx || !IsPlausiblePtr(pcx)) return NULL;
    surf = ((Fn_PcxGetSurf)(void*)FN_PCX_GETSURF)(pcx, file, NULL);
    if (!surf)
    {
        ((Fn_PcxLoad)(void*)FN_PCX_LOAD)(pcx, file, 2, 0);
        surf = ((Fn_PcxGetSurf)(void*)FN_PCX_GETSURF)(pcx, file, NULL);
    }
    if (surf && !IsPlausiblePtr(surf)) return NULL;
    if (surf && !IsPlausiblePtr(*(void**)surf)) return NULL;   /* 虚表必须指向代码段 */
    return surf;
}

/* BSurface (0x20): +0 虚表 / +4 W / +8 H / +0xC LockLevel / +0x10 字节数 /
 * +0x14 缓冲指针 / +0x18 字节长 / +0x1C own。`CC_Draw_Shape` 只用 GetRect 不锁面,
 * 直接写 +0x14, 栈上拼个假面就够。调色板/参数复刻引擎画防御页那一次:
 * CAMEO_PAL, frame 0, flags CAMEO_BLIT_FLAGS, brightness CAMEO_BRIGHTNESS。 */
static int SnapRenderShp(void* shp, unsigned short* out, int w, int h)
{
    unsigned int surf[8];          /* 0x20 假 BSurface */
    unsigned char* s = (unsigned char*)surf;
    void* pal;
    Point2D pos;
    RectangleStruct b;
    int i;
    if (!shp || !IsPlausiblePtr(shp)) return 0;
    if (!IsPlausiblePtr((void*)ADDR_CAMEO_PAL)) return 0;
    pal = *(void**)ADDR_CAMEO_PAL;
    if (!pal || !IsPlausiblePtr(pal)) return 0;
    for (i = 0; i < 0x20; i++) s[i] = 0;
    *(void**)(s + 0x00) = (void*)ADDR_BSHAPE_VTBL;
    *(int*)(s + 0x04) = w;
    *(int*)(s + 0x08) = h;
    *(int*)(s + 0x0C) = 0;
    *(int*)(s + 0x10) = 2;
    *(void**)(s + 0x14) = (void*)out;
    *(int*)(s + 0x18) = w * h * 2;
    *(int*)(s + 0x1C) = 1;
    /* 先铺色键 (紫), 让 SHP 的透明像素落成色键 (绘制层会跳过)。 */
    for (i = 0; i < w * h; i++) out[i] = ICONSNAP_KEY;
    pos.X = 0; pos.Y = 0;
    b.X = 0; b.Y = 0; b.Width = w; b.Height = h;
    ((Fn_DrawShape)(void*)FN_DRAW_SHAPE)(surf, pal, shp, 0, &pos, &b,
        CAMEO_BLIT_FLAGS, 0, 0, 0, CAMEO_BRIGHTNESS, 0, NULL, 0, 0, 0);
    return 1;
}

unsigned short* IconSnap_Pcx(const char* file, int w, int h, int* srcW, int* srcH)
{
    const unsigned short* src;
    unsigned short* dst;
    unsigned key;
    int sw, sh, bpp, r, c;
    void* surf = SnapPcxRawSurf(file);
    if (!surf) return NULL;
    sw = *(int*)((char*)surf + 0x04);
    sh = *(int*)((char*)surf + 0x08);
    bpp = *(int*)((char*)surf + 0x10);
    src = *(const unsigned short**)((char*)surf + 0x14);
    if (sw < 1 || sw > 256 || sh < 1 || sh > 256 || bpp != 2) return NULL;
    if (!src || !IsPlausiblePtr((void*)src)) return NULL;
    if (*(int*)((char*)surf + 0x18) != sw * sh * 2) return NULL;
    dst = (unsigned short*)LocalAlloc(LMEM_FIXED,
                     (SIZE_T)(w * h) * sizeof(unsigned short));
    if (!dst) return NULL;
    /* 缩放取样一次做完 (最近邻)。源首像素就是键色, 同色像素统一换成
     * ICONSNAP_KEY, 绘制层只认这一个键 (对标准紫键 PCX 是恒等变换)。 */
    key = src[0];
    for (r = 0; r < h; r++)
    {
        const unsigned short* srow = src + (size_t)((r * sh) / h) * sw;
        unsigned short* drow = dst + (size_t)r * w;
        for (c = 0; c < w; c++)
        {
            unsigned short v = srow[(c * sw) / w];
            drow[c] = (v == key) ? ICONSNAP_KEY : v;
        }
    }
    if (srcW) *srcW = sw;
    if (srcH) *srcH = sh;
    return dst;
}

unsigned short* IconSnap_Shp(void* shp, int w, int h)
{
    unsigned short* dst = (unsigned short*)LocalAlloc(LMEM_FIXED,
                     (SIZE_T)(w * h) * sizeof(unsigned short));
    if (!dst) return NULL;
    if (!SnapRenderShp(shp, dst, w, h)) { LocalFree(dst); return NULL; }
    return dst;
}
