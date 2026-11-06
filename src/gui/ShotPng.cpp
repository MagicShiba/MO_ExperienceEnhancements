/* ShotPng.cpp - 565/555 像素 → 8 位 RGB 非隔行 PNG, 见 ShotPng.h。
 *
 * 编码走系统的 gdiplus.dll。选它的理由是实测三项 (同一张 1920x1048 截图,
 * 都由本机跑出来): GDI+ 105 ms / 2.33 MB, WIC 176 ms / 3.64 MB, 自己写的
 * 固定 Huffman LZ77 + 哈希链 155 ms / 3.09 MB。GDI+ 的编码器在抖动画面上
 * 找到的重复比我们那份多, 所以又快又小 —— 没有理由再维护一份 deflate。
 *
 * gdiplus.dll 是 XP SP1 起的系统组件, 但仍然动态加载: 这台机器如果真没有,
 * 只该让截图功能不可用, 不该连累 MOExperience.dll 整体加载失败。
 *
 * 只需要 5 个导出, 所以不 #include <gdiplus.h> (那个头是 C++ 类封装, 还得
 * 链导入库), 直接声明 flat API 的函数指针 + 硬编码 PNG 编码器 CLSID。
 */

#include <windows.h>
#include "ShotPng.h"

/* ---------------- GDI+ flat API ---------------- */

typedef int  (WINAPI *SPNG_STARTUP)(ULONG_PTR* token, const void* in, void* out);
typedef int  (WINAPI *SPNG_CREATE)(int w, int h, int stride, int fmt,
                                   void* scan0, void** bmp);
typedef int  (WINAPI *SPNG_SAVE)(void* image, const WCHAR* file,
                                 const GUID* clsid, const void* params);
typedef int  (WINAPI *SPNG_DISPOSE)(void* image);

/* GdiplusStartupInput 的 32 位布局 (头文件里是 C++ 结构, 这里按字段顺序排)。 */
typedef struct {
    UINT32 ver;
    void* cb;
    int suppressBg;
    int suppressExt;
} SPNG_IN;

#define SPNG_PIXFMT_24BPP_RGB 0x00021808  /* PixelFormat24bppRGB, 内存序是 BGR */

/* PNG 编码器的 CLSID, 与 GdipGetImageEncoders 报出来的一致 (常量, 不必枚举)。 */
static const GUID SPNG_CLSID_PNG =
    { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };

static HMODULE s_Mod;
static SPNG_CREATE s_Create;
static SPNG_SAVE s_Save;
static SPNG_DISPOSE s_Dispose;
static int s_Ready;

int ShotPng_Init(void)
{
    static int tried = 0;
    SPNG_STARTUP startup;
    SPNG_IN si;
    ULONG_PTR tok = 0;

    if (tried) return s_Ready;
    tried = 1;

    s_Mod = LoadLibraryA("gdiplus.dll");
    if (!s_Mod) return 0;
    startup = (SPNG_STARTUP)GetProcAddress(s_Mod, "GdiplusStartup");
    s_Create = (SPNG_CREATE)GetProcAddress(s_Mod, "GdipCreateBitmapFromScan0");
    s_Save = (SPNG_SAVE)GetProcAddress(s_Mod, "GdipSaveImageToFile");
    s_Dispose = (SPNG_DISPOSE)GetProcAddress(s_Mod, "GdipDisposeImage");
    if (!startup || !s_Create || !s_Save || !s_Dispose) return 0;

    si.ver = 1;
    si.cb = NULL;
    si.suppressBg = 0;      /* 抑制后台线程要额外手工挂 NotificationHook, 不折腾 */
    si.suppressExt = 0;
    if (startup(&tok, &si, NULL) != 0) return 0;
    /* token 不保存: 进程退出时 GDI+ 的后台线程跟着进程走, 没有该 shutdown 的时机。 */

    s_Ready = 1;
    return 1;
}

int ShotPng_Write565(const char* path, const unsigned short* pix, int w, int h,
                     int gbits)
{
    unsigned char* bgr;
    size_t stride, bytes;
    WCHAR wp[MAX_PATH];
    void* bmp = NULL;
    int x, y, ok = 0;

    if (!path || !pix || w <= 0 || h <= 0) return 0;
    if (!ShotPng_Init()) return 0;

    /* GDI+ 的 24bpp 是 BGR 顺序, 行宽按 4 字节对齐 (位图 buffer 的通行约束)。 */
    stride = ((size_t)w * 3u + 3u) & ~(size_t)3u;
    bytes = stride * (size_t)h;
    bgr = (unsigned char*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    if (!bgr) return 0;

    for (y = 0; y < h; y++)
    {
        const unsigned short* sp = pix + (size_t)y * (size_t)w;
        unsigned char* dp = bgr + (size_t)y * stride;
        for (x = 0; x < w; x++)
        {
            unsigned v = sp[x];
            unsigned r, g, b;
            if (gbits == 6)
            {
                r = (v >> 11) & 0x1Fu;
                g = (v >> 5) & 0x3Fu;
                b = v & 0x1Fu;
                g = (g << 2) | (g >> 4);
            }
            else
            {
                r = (v >> 10) & 0x1Fu;
                g = (v >> 5) & 0x1Fu;
                b = v & 0x1Fu;
                g = (g << 3) | (g >> 2);
            }
            dp[x * 3 + 0] = (unsigned char)((b << 3) | (b >> 2));
            dp[x * 3 + 1] = (unsigned char)g;
            dp[x * 3 + 2] = (unsigned char)((r << 3) | (r >> 2));
        }
        {   /* 行尾对齐填充清零, 免得把未初始化内存交给 GDI+ */
            size_t pad = stride - (size_t)w * 3u;
            unsigned char* e = dp + (size_t)w * 3u;
            while (pad--) *e++ = 0;
        }
    }

    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wp, MAX_PATH)) goto done;
    if (s_Create(w, h, (int)stride, SPNG_PIXFMT_24BPP_RGB, bgr, &bmp) == 0 && bmp)
    {
        ok = (s_Save(bmp, wp, &SPNG_CLSID_PNG, NULL) == 0);
        s_Dispose(bmp);
    }
    if (!ok) DeleteFileA(path);   /* GDI+ 失败时可能已经建了个空文件 */

done:
    VirtualFree(bgr, 0, MEM_RELEASE);
    return ok;
}
