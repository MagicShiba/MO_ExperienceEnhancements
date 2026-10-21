/* Icon - 提示栏自定义图标。
 *
 * 从 icons/icon_*.pcx 取 8bpp 覆盖率 (0=透明 255=全墨)，首次使用后缓存。
 * 混合复用 GdiText 的 Calibrate/GtUnpack16/Blend16；只读，不写模拟。 */

#include <windows.h>
#include "GdiText.h"
#include "PcxImage.h"
#include "Icon.h"

#define ICON_CAP_W 64
#define ICON_CAP_H 64

static const char* const s_IconFile[4] = {
    "icon_power_up.pcx",
    "icon_power_down.pcx",
    "icon_time.pcx",
    "icon_cost.pcx"
};
static unsigned char s_IconPx[4][ICON_CAP_W * ICON_CAP_H];
static int s_IconW[4];
static int s_IconH[4];
static int s_IconState[4];   /* 0=未取 1=成功 2=失败 (只试一次) */

static int Icon_Slot(unsigned wc)
{
    if (wc == ICON_PWR_UP) return 0;
    if (wc == ICON_PWR_DOWN) return 1;
    if (wc == ICON_TIME) return 2;
    if (wc == ICON_COST) return 3;
    return -1;
}

static int Icon_Ensure(int slot)
{
    PcxImage img;
    int x, y;
    if (slot < 0 || slot >= 4) return 0;
    if (s_IconState[slot] == 1) return 1;
    if (s_IconState[slot] == 2) return 0;
    if (!PcxImage_Load(&img, s_IconFile[slot], 1) ||
        img.width < 1 || img.height < 1 ||
        img.width > ICON_CAP_W || img.height > ICON_CAP_H)
    {
        s_IconState[slot] = 2;
        return 0;
    }
    for (y = 0; y < img.height; y++)
        for (x = 0; x < img.width; x++)
            s_IconPx[slot][y * ICON_CAP_W + x] = img.px[y * img.width + x];
    s_IconW[slot] = img.width;
    s_IconH[slot] = img.height;
    s_IconState[slot] = 1;
    return 1;
}

/* 统一预载全部图标 (含地图标记), 与各功能开关无关。 */
void Icon_Preload(void)
{
    PcxImage img;
    int i;
    for (i = 0; i < 4; i++) Icon_Ensure(i);
    (void)PcxImage_Load(&img, "icon_aggressive.pcx", 2);
}

int Icon_Is(unsigned wc)
{
    return Icon_Slot(wc) >= 0 ? 1 : 0;
}

int Icon_Advance(unsigned wc)
{
    int slot = Icon_Slot(wc);
    if (slot < 0 || !Icon_Ensure(slot)) return 0;
    return s_IconW[slot];
}

int Icon_LineH(void* t)
{
    int oh;
    if (!t || !IsPlausiblePtr(t)) return 19;
    oh = *(int*)((char*)t + 0x1C);
    if (oh < 8 || oh > 64) return 19;
    return oh;
}

int Icon_Height(unsigned wc)
{
    int slot = Icon_Slot(wc);
    if (slot < 0 || !Icon_Ensure(slot)) return 0;
    return s_IconH[slot];
}

int Icon_Blit(void* t, unsigned wc, int x, int y, int colorW, int h)
{
    int slot = Icon_Slot(wc);
    int iw, ih, dy, dx, sy;
    unsigned short* buf;
    int pitchW, Tp, B;
    unsigned char fr, fg, fb;
    const unsigned char* px;
    if (slot < 0 || !Icon_Ensure(slot)) return -1;
    if (!t || !IsPlausiblePtr(t)) return -1;
    iw = s_IconW[slot];
    ih = s_IconH[slot];
    if (h < 8 || h > 64) return -1;
    buf = *(unsigned short**)((char*)t + 0x0C);
    pitchW = *(int*)((char*)t + 0x10);
    if (!buf || !IsPlausiblePtr(buf)) return -1;
    if (pitchW < 64 || pitchW > 4096) return -1;
    Tp = (int)(short)(((unsigned)*(int*)((char*)t + 0x34)) & 0xFFFFu);
    B = (int)(short)(((unsigned)*(int*)((char*)t + 0x3C)) & 0xFFFFu);
    if (B < Tp) return -1;
    if (Tp < 0) Tp = 0;
    if (B > 4096) B = 4096;
    Calibrate();
    GtUnpack16((unsigned short)(colorW & 0xFFFF), &fr, &fg, &fb);
    /* 1:1 居中 (图标不缩放): 行高由引擎行步进保证装下 (见 ToolTip),
     * 此处只做上下钳进裁剪, 横向只认面界 (VecRender 同式)。 */
    dy = y + (h - ih) / 2;
    {
        int lo = Tp, hi = B - ih + 1;
        if (hi >= lo)
        {
            if (dy < lo) dy = lo;
            if (dy > hi) dy = hi;
        }
    }
    px = s_IconPx[slot];
    for (sy = 0; sy < ih; sy++)
    {
        int yy = dy + sy;
        unsigned short* row;
        if (yy < Tp || yy > B) continue;
        row = buf + yy * pitchW;
        for (dx = 0; dx < iw; dx++)
        {
            int sx = x + dx;
            unsigned a;
            if (sx < 0 || sx >= pitchW) continue;
            a = px[sy * ICON_CAP_W + dx];
            if (!a) continue;
            row[sx] = Blend16(row[sx], fr, fg, fb, a);
        }
    }
    return 0;
}
