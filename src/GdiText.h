/* GdiText.h - MsgFont.dll 内共享的无 CRT 工具 / 日志 / 字体解析 /
 * GDI 光栅 / 16 位混合管线 (MsgFont.cpp 与 VecText.cpp 共用)。
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 单 DLL 多源文件)。
 */
#pragma once

#include <windows.h>

#define MAX_PATH_LEN 260
#define LOG_LINE_MAX 1024
#define FONT_FACE_MAX 48

typedef struct { int X, Y; } Point2D;
typedef struct { int X, Y, Width, Height; } RectangleStruct;
typedef struct { unsigned char R, G, B; } ColorStruct;

/* ---------------- 无 CRT 工具 (定义在 GdiText.cpp, 全 DLL 一份) ---------------- */
unsigned UtoA(unsigned v, char* p);
unsigned HtoA(unsigned v, char* p);
unsigned AppendCStr(char* dst, unsigned pos, const char* src, unsigned cap);
unsigned AppendCh(char* dst, unsigned pos, char c, unsigned cap);
int IsPlausiblePtr(const void* p);

/* ---------------- 日志 ---------------- */
extern HANDLE g_Log;
extern char g_IniPath[MAX_PATH_LEN];
extern char g_LogPath[MAX_PATH_LEN];
void BuildPaths(void);
void GtEnsureLog(void);   /* 幂等: 建路径+打开日志, 失败则 g_Log=NULL */
void LogLine(const char* body);

/* ---------------- 字体查找 ----------------
 * 配置里写字体文件名 (如 simhei.ttf, 原样匹配)。
 * 顺序: 游戏当前文件夹 -> 系统字体文件夹 -> fallback 字体名。 */
void CopyFace(wchar_t* dst, const wchar_t* src);
int FaceSame(const wchar_t* a, const wchar_t* b);
int FileExists(const char* path);
/* 找到写全路径进 outPath, 返回 1=游戏目录, 2=系统目录; 都没有返回 0 */
int FindFontFile(const char* name, char* outPath, int cap, const char** tag);
void ResolveFontFace(const char* cfg, const char* tag, wchar_t* out,
                     const wchar_t* fallback);

/* ---------------- 像素格式 ---------------- */
extern int g_RBits, g_GBits, g_BBits;
extern int g_Calibrated;
void Calibrate(void);     /* 幂等: Temp 面填纯红回读, 判定 565/555 */
/* 16 位混合: dst = fg*a + dst*(1-a), fg 为 8 位, a=0..255 */
unsigned short Blend16(unsigned short dst, unsigned fr, unsigned fg,
                       unsigned fb, unsigned a);
/* 16 位像素拆回 8 位 RGB (用标定格式) */
void GtUnpack16(unsigned short px, unsigned char* r, unsigned char* g,
                unsigned char* b);

/* ---------------- GDI 光栅化 ---------------- */
#define DIB_W 1024
#define DIB_H 64

typedef struct {
    HDC dc;
    HBITMAP dib;
    HBITMAP oldBmp;
    HFONT font;
    unsigned char* bits;
    int pitch;
    int px;     /* -1 = 空槽 */
    int bold;
    int adv;
    wchar_t face[FONT_FACE_MAX];  /* 缓存键: 字体名不同即重建 */
} GdiSlot;

extern GdiSlot* g_G;      /* 当前槽 (GdiEnsure 成功后指向它) */
/* 槽号: 0=战役框, 1=提示框, 2=全局文本 (VecText, 独立一套避免互踢) */
int GdiEnsure(int fontPx, int bold, int slot, const wchar_t* face);
/* 量一段文本在 maxW 内能放几个字符 */
int CharsFit(const wchar_t* text, int len, int maxW);
/* 把一行画进 DIB (黑底白字), 返回实际像素宽; 覆盖率=红色通道 */
int RenderLine(const wchar_t* text, int len);
/* 单字形光栅: 清 DIB 后在 (x0,y0) 画一个 wchar, 返回 GDI 像素宽 (*outW 同) */
int GtRenderGlyphAt(wchar_t ch, int x0, int y0, int* outW);
/* DIB 覆盖率 blit 到已 Lock 的 16 位面 (裁剪到面内) */
void BlitCoverage(unsigned char* dstBase, int dstPitch, int surfW, int surfH,
                  int dx, int dy, int w, int h,
                  unsigned fr, unsigned fg, unsigned fb, unsigned alphaScale);
