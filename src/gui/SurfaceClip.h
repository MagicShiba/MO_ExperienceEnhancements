/* SurfaceClip.h - SHP 相对坐标裁剪范围不能越过目标面右/下边界。 */
#pragma once

/* 引擎先按 Width/Height 裁相对坐标，再加 X/Y 锁面；X/Y 是坐标原点，
 * 必须保留，不能把负原点钳零而平移图像。左/上界仍由原生裁剪/Lock处理。 */
static inline int SurfaceClipBounds(int* x, int* y, int* w, int* h,
                                    int surfaceW, int surfaceH)
{
    long long maxW = (long long)surfaceW - *x;
    long long maxH = (long long)surfaceH - *y;
    if (surfaceW <= 0 || surfaceH <= 0 || *w <= 0 || *h <= 0
        || maxW <= 0 || maxH <= 0) return 0;
    if (*w > maxW) *w = (int)maxW;
    if (*h > maxH) *h = (int)maxH;
    return 1;
}
