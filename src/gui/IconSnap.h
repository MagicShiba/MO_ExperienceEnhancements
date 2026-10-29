/* IconSnap - cameo 图标快照底座 (UnitPanel 单位面板 / SWSBar 超武侧栏共用)。
 * 从引擎 PCX 面或 SHP 现渲出 16 位色图标像素, 透明统一落 ICONSNAP_KEY,
 * 各自的绘制层跳该键。只读引擎数据 + 现成调色板, 不写模拟。
 * 会往引擎 PCX/BSurface 池装载, **只能在各自模块的扫描期调用**,
 * 取到的像素落进自有缓冲后引擎面指针即丢弃, 不跨帧持有。 */
#ifndef ICONSNAP_H
#define ICONSNAP_H

#define ICONSNAP_KEY 0xF81Fu   /* 紫 = COLOR_RED|COLOR_BLUE, 引擎 PCX 透明键 */

/* 取 PCX 并最近邻缩样到 w*h, 返回 LocalAlloc 像素 (调用方 LocalFree), 失败 NULL。
 * 源首像素色视为透明键, 同色像素统一换成 ICONSNAP_KEY。
 * srcW/srcH 可为 NULL, 回传源面尺寸 (cameo 该是 60x48, 供日志)。 */
unsigned short* IconSnap_Pcx(const char* file, int w, int h, int* srcW, int* srcH);

/* 用 CAMEO_PAL 把 shp 的 frame 0 现渲到 w*h, 透明像素落 ICONSNAP_KEY。
 * 返回 LocalAlloc 像素 (调用方 LocalFree), 失败 NULL。 */
unsigned short* IconSnap_Shp(void* shp, int w, int h);

#endif
