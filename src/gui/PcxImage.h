/* PcxImage - 内嵌 PCX 的加载与绘制。
 *
 * 资源由 icons.rc 编译进 DLL；首次加载写到 %TEMP%，读入引擎缓存后删除，
 * 之后复用引擎面表，不重复读文件。1 plane -> 8 位索引面，3 plane -> 16 位
 * 真彩面。只读，不改模拟状态。 */
#pragma once

/* 已加载的 PCX 面；surf/px 属于引擎缓存，不可跨帧保存。 */
typedef struct {
    void* surf;
    const unsigned char* px;    /* 面像素: 8bpp=索引, 16bpp=像素 */
    int width, height, bpp;
    unsigned short key;
} PcxImage;

/* 取 PCX 面；wantBpp=1/2 时校验位深。首次读盘加载，之后命中引擎缓存。 */
int PcxImage_Load(PcxImage* img, const char* name, int wantBpp);

/* 画到 16 位目标面左上角 (x,y)，自动夹进目标面。返回 0=画出。 */
int PcxImage_Draw(const PcxImage* img, void* target, int x, int y);
