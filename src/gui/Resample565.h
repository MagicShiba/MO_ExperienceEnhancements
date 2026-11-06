#ifndef MOE_RESAMPLE565_H
#define MOE_RESAMPLE565_H

/* 精确 2:1 最近邻：count 是输出像素数；源须有 2*count 个像素。
 * 源与目标不能重叠；行首无需 16 字节对齐。 */
void R565_DownsampleRow2(const unsigned short* src, unsigned short* dst, int count);

/* 通用最近邻：dst[i] = src[map[i]]；map 中的源索引有效性由调用方保证。
 * 源、映射与目标不能重叠；count <= 0 时不访问任何指针。 */
void R565_MapRow(const unsigned short* src, unsigned short* dst, const int* map, int count);

/* 非重叠像素复制；源须有 count 个像素，源尾不超读，行首无需对齐。
 * count <= 0 时不访问任何指针。 */
void R565_CopyRow(const unsigned short* src, unsigned short* dst, int count);

#endif
