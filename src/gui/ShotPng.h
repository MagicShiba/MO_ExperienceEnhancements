/* ShotPng.h - 16 位视口像素写成 8 位 RGB 真彩 PNG。
 *
 * 编码交给系统的 gdiplus.dll (GDI+ 的 PNG 编码器), 不自己实现 deflate:
 * 同一张 1920x1048 的截图, GDI+ 105 ms / 2.33 MB, 自己写的固定 Huffman LZ77
 * 155 ms / 3.09 MB, WIC 176 ms / 3.64 MB —— 系统实现又快又小。
 * 只碰内存与本地文件, 联机安全。
 */
#pragma once

/* 主线程调一次, 装 gdiplus.dll 并 GdiplusStartup。可重复调 (幂等)。
 * 返回 0 = 这台机器用不了 GDI+ (此时截图功能整体不可用)。
 * 预热放在按钮出现时做, 别等第一次截图才加载。 */
int ShotPng_Init(void);

/* pix: 自上而下的 16 位像素, stride = w。
 * gbits == 6 ⇒ RGB565, 否则 RGB555 (与 BMP 掩码同一口径)。
 * 成功返回 1; 失败时不留半截文件。 */
int ShotPng_Write565(const char* path, const unsigned short* pix, int w, int h,
                     int gbits);
