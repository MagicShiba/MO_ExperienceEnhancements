/* IconMap.h - 独立地图图像 (非文字字形) 的只读 blit。
 *
 * 与 Icon.h 的字形图标不同: 这些图不进提示栏文字, 直接画到地图叠加面
 * (如侵略模式的标记图)。数据来自构建期生成的 icons/mapicons.inc
 * (mkicons.py 由 icons/icon_*.png 转换, 保留原图 RGBA), 编进 DLL, 无外部文件。
 * 槽位宏 MAPICON_* 与 mapicons.inc 的顺序一一对应。实现见 IconMap.cpp。 */

#pragma once

/* 槽位 (顺序须与 mkicons.py 的 IMAGES 一致) */
#define MAPICON_AGGRESSIVE 0

/* 图像自身宽/高像素 (未知回 0) */
int IconMap_Width(int id);
int IconMap_Height(int id);

/* 画到已锁定的 16 位渲染面 (不经文本面): (x,y)=左上角像素,
 * alpha=全局不透明度 (0~255)。按原图 RGBA 混合, 按面界裁剪。
 * 0=画出, <0=失败 (槽无效/尺寸非法)。 */
int IconMap_Blit(unsigned char* locked, int w, int h, int pitch,
                 int id, int x, int y, unsigned alpha);
