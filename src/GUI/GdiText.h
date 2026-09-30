/* GdiText.h - MOExperience.dll 内共享的无 CRT 工具 / 日志 / 字体解析 /
 * GDI 光栅 / 16 位混合管线 / 画面虚表底座 (全 DLL 共用)。
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 单 DLL 多源文件)。
 */#pragma once

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
/* ---------------- 鼠标游戏坐标 (WWMouse+窗口映射, 供 PatrolCircle 等输入模块用), 返回 1=有效 */
int GtMouseGameXY(int* x, int* y);
/* 每帧刷新游戏客户区尺寸 (窗口化/拉伸呈现时鼠标要换算, 见实现) */
void GtRefreshWinSize(unsigned tick);
/* 详细日志总开关: 0=只记关键事件 (默认, 日志小); 1=逐字/逐帧普查 */
extern int g_LogDetail;

/* ---------------- 画面底座 (引擎 DSURFACE 虚表, 全 DLL 共用) ----------------
 * 引擎面对象首成员是虚表指针, 槽号见下; 全部只读调用 + 渲染面写入, 联机安全。 */
#define GtVT_FILL_RECT        0x14
#define GtVT_FILL_RECT_TRANS  0x1C
#define GtVT_SET_PIXEL        0x24
#define GtVT_GET_PIXEL        0x28
#define GtVT_LOCK             0x5C
#define GtVT_UNLOCK           0x60
#define GtVT_GET_BPP          0x70
#define GtVT_GET_PITCH        0x74
#define GtVT_GET_WIDTH        0x7C
#define GtVT_GET_HEIGHT       0x80

#define ADDR_SURF_COMPOSITE   0x0088731Cu
#define ADDR_SURF_SIDEBAR     0x00887300u
#define ADDR_SURF_PRIMARY     0x00887308u
#define ADDR_SURF_TEMP        0x00887314u
#define ADDR_SURF_VIEWBOUNDS  0x00886FA0u
#define ADDR_BITFONT_INSTANCE 0x0089C4D0u
#define FONT_OFF_HEIGHT       0x1C

/* 调虚表槽取 int (宽/高/pitch/bpp), 失败返回负数 */
int GtSurfGet(void* surf, unsigned vtOff);
/* 半透明填充 (opacity 0~100); 槽坏静默跳过 */
void GtFillRectTrans(void* surf, RectangleStruct* r, ColorStruct* c, int opacity);
/* Lock 面拿可写像素 (非空=成功, 调用方须配 GtUnlock); 槽坏/半初始化返回 NULL */
unsigned char* GtLock(void* surf);
void GtUnlock(void* surf);
/* 画框线 (上/下/左/右各 2px 半透明带), 太小自动跳过 */
void GtDrawOutline(void* surf, int x, int y, int w, int h,
                   unsigned char r, unsigned char g, unsigned char b, int op);
/* 点阵字行高 (BitFont 实例), 读不到回退 14 */
int GtBitLineHeight(void);
/* 游戏坐标系整帧宽高 (Primary 面; 0=读不到, 鼠标映射退回原始坐标) */
int GtGameW(void);
int GtGameH(void);
void GtSetGameSize(int w, int h);
/* 视口宽 (ViewBounds), 读不到回退 640 */
int GtScreenW(void);
/* 游戏窗口当前是否前台 (窗口化切走时引擎停渲染, 悬停淡入日志靠它) */
int GtGameFocused(void);

/* ---------------- 游戏时钟 (引擎式, 全 DLL 共用) ----------------
 * 时钟 = [0x887340] + (Tick() - [0x887338]), Tick = timeGetTime()>>4 (毫秒/16),
 * [0x887338]==-1 时不做 Tick 补偿 (暂停/未启动状态)。 */
unsigned GtTickNow(void);
unsigned GtClockNow(void);

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
    unsigned gen;                /* DIB clear/redraw generation (for render caching) */
    wchar_t face[FONT_FACE_MAX];  /* 缓存键: 字体名不同即重建 */
} GdiSlot;

extern GdiSlot* g_G;      /* 当前槽 (GdiEnsure 成功后指向它) */
/* 槽号: 0=战役框, 1=提示框, 2=全局文本 (VecText), 3=超武面板/底电,
 * 4=聊天输入, 5=超武侧栏充能百分比, 6=超武侧栏悬停名 (5/6 分槽是因为两者
 * 字号来源不同, 共用一槽会让 GdiEnsure 每次拆了重建)。 */int GdiEnsure(int fontPx, int bold, int slot, const wchar_t* face);
/* 单字前进宽 (字体自己的 ABC 全宽); 需先 GdiEnsure, 失败 0 */
int GtCharAdv(wchar_t ch);
/* 量一段文本在 maxW 内能放几个字符 */
int CharsFit(const wchar_t* text, int len, int maxW);
/* 把一行画进 DIB (黑底白字), 返回实际像素宽; 覆盖率=红色通道 */
int RenderLine(const wchar_t* text, int len);
/* 单字形光栅: 清 DIB 后在 (x0,y0) 画一个 wchar, 返回 GDI 像素宽 (*outW 同) */
int GtRenderGlyphAt(wchar_t ch, int x0, int y0, int* outW);
/* DIB 覆盖率 blit 到已 Lock 的 16 位面 (裁剪到面内) */
/* Current slot DIB generation; bumped on every clear/redraw. Lets callers
 * tell whether the DIB still holds what they last drew, so repeated GDI
 * text renders of the same string can be skipped. */
unsigned GdiSlotGen(void);

void BlitCoverage(unsigned char* dstBase, int dstPitch, int surfW, int surfH,
                  int dx, int dy, int w, int h,
                  unsigned fr, unsigned fg, unsigned fb, unsigned alphaScale);
