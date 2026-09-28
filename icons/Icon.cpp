/* Icon.cpp - 提示栏自定义图标运行时 (声明见 Icon.h)。
 *
 * 数据来自构建期生成的 icons/icons.inc (mkicons.py 由 icons/icon_*.png 转换,
 * 黑=墨白=忽略, 等宽归一化; 见文件头)。此处只做只读 blit, 不写模拟。
 * 混合管线复用 GdiText (Calibrate + GtUnpack16 + Blend16), 坐标裁剪口径
 * 与 VecText VecRender 一致 (Tp/B 取 t+0x34/0x3C, 横向只认面界)。
 */

#include <windows.h>
#include "GdiText.h"
#include "Icon.h"
#include "icons.inc"

static int Icon_Slot(unsigned wc)
{
    if (wc == ICON_PWR_UP) return 0;
    if (wc == ICON_PWR_DOWN) return 1;
    if (wc == ICON_TIME) return 2;
    if (wc == ICON_COST) return 3;
    return -1;
}

int Icon_Is(unsigned wc)
{
    return Icon_Slot(wc) >= 0 ? 1 : 0;
}

int Icon_Advance(unsigned wc)
{
    if (Icon_Slot(wc) < 0) return 0;
    if (ICON_W < 1 || ICON_W > 64) return 0;
    return ICON_W;
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
    if (slot < 0 || slot >= ICON_N) return 0;
    return g_IconH[slot];
}

int Icon_Blit(void* t, unsigned wc, int x, int y, int colorW, int h)
{
    int slot = Icon_Slot(wc);
    int iw, ih, dy, dx, sy;
    unsigned short* buf;
    int pitchW, Tp, B;
    unsigned char fr, fg, fb;
    const unsigned char* px;
    if (slot < 0 || slot >= ICON_N) return -1;
    if (!t || !IsPlausiblePtr(t)) return -1;
    iw = g_IconW[slot];
    ih = g_IconH[slot];
    if (iw <= 0 || iw > ICON_W || ih <= 0 || ih > ICON_HMAX) return -1;
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
    px = g_IconPx[slot];
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
            a = px[sy * ICON_W + dx];
            if (!a) continue;
            row[sx] = Blend16(row[sx], fr, fg, fb, a);
        }
    }
    return 0;
}
