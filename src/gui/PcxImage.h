/* PcxImage - 内嵌 PCX 的加载与绘制。
 *
 * 资源由 icons.rc 编译进 DLL；首次加载写到 %TEMP%，读入引擎缓存后删除，
 * 之后复用引擎面表，不重复读文件。1 plane -> 8 位索引面，3 plane -> 16 位
 * 真彩面。只读，不改模拟状态。 */
#pragma once

/* 已加载的 PCX 像素；px 属于引擎缓存，不可跨帧保存。 */
typedef struct {
    const unsigned char* px;    /* 8bpp=索引, 16bpp=像素 */
    int width, height, bpp;
    unsigned short key;
} PcxImage;

/* 取 PCX 面；wantBpp=1/2 时校验位深。首次读盘加载，之后命中引擎缓存。 */
int PcxImage_Load(PcxImage* img, const char* name, int wantBpp);

/* 画到已锁定的 16 位面 (base/pitch) 的 (x,y)，只写图像与 (cx0,cy0)-(cx1,cy1)
 * 的交集，key 色跳过。越界坐标不夹取：整张在裁剪区外即不画。
 * 返回 1=写入了像素。 */
int PcxImage_DrawClip(const PcxImage* img, unsigned char* base, int pitch,
                      int x, int y, int cx0, int cy0, int cx1, int cy1);
