/* IconMap.cpp - 独立地图图像 (非文字字形) 的只读 blit (声明见 IconMap.h)。
 * 数据来自构建期生成的 icons/mapicons.inc (mkicons.py 由 icon_*.png 转换)。
 * 混合复用 GdiText (Calibrate + Blend16); 只读, 不写模拟。 */

#include <windows.h>
#include "GdiText.h"
#include "IconMap.h"
#include "mapicons.inc"

int IconMap_Width(int id)
{
    if (id < 0 || id >= MAPICON_N) return 0;
    return g_MapIconW[id];
}

int IconMap_Height(int id)
{
    if (id < 0 || id >= MAPICON_N) return 0;
    return g_MapIconH[id];
}

int IconMap_Blit(unsigned char* locked, int w, int h, int pitch,
                 int id, int x, int y, unsigned alpha)
{
    int iw, ih, sy, dx;
    const unsigned char* rgba;
    if (id < 0 || id >= MAPICON_N) return -1;
    if (!locked || !IsPlausiblePtr(locked)) return -1;
    if (w <= 0 || h <= 0 || pitch <= 0) return -1;
    iw = g_MapIconW[id];
    ih = g_MapIconH[id];
    if (iw <= 0 || ih <= 0 || iw * ih > MAPICON_PIXMAX) return -1;
    if (alpha == 0) return 0;
    Calibrate();
    rgba = g_MapIconRGBA[id];
    for (sy = 0; sy < ih; sy++)
    {
        int yy = y + sy;
        unsigned short* row;
        if (yy < 0 || yy >= h) continue;
        row = (unsigned short*)(locked + yy * pitch);
        for (dx = 0; dx < iw; dx++)
        {
            int sx = x + dx;
            const unsigned char* p;
            unsigned a;
            if (sx < 0 || sx >= w) continue;
            p = rgba + (sy * iw + dx) * 4;
            a = p[3];
            if (!a) continue;
            row[sx] = Blend16(row[sx], p[0], p[1], p[2], (a * alpha) / 255u);
        }
    }
    return 0;
}
