/* GdiText.h - MOExperience.dll 内共享的无 CRT 工具 / 日志 / 字体解析 /
 * GDI 光栅 / 16 位混合管线 / 画面虚表底座 (全 DLL 共用)。
 * 构建见 build-fast.bat (32 位 mingw, 无 CRT, 单 DLL 多源文件)。
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
void GtEnsureLog(void);   /* 定义在 MOExperience.cpp; 幂等: 建路径+打开日志, 失败则 g_Log=NULL */
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
/* 位深展开表 (5/6 位 -> 8 位), 建一次供混合查表。 */
extern unsigned char g_ExpR[32], g_ExpG[64], g_ExpB[32];
extern int g_ExpReady;
void GtBuildExpand(void);
/* 16 位混合: dst = fg*a + dst*(1-a), fg 为 8 位, a=0..255。热路径内联。
 * 压位按比例 ((v*N+127)/255), 不是移位。 */
static __inline unsigned short Blend16(unsigned short dst, unsigned fr,
                                       unsigned fg, unsigned fb, unsigned a)
{
    unsigned dr8, dg8, db8, or_, og, ob;
    if (a == 0) return dst;
    if (!g_ExpReady) GtBuildExpand();
    /* a=255 直接写颜色。 */
    if (a == 255)
    {
        if (g_GBits == 6)
            return (unsigned short)((((fr * 31u + 127u) / 255u) << 11) |
                                    (((fg * 63u + 127u) / 255u) << 5) |
                                     ((fb * 31u + 127u) / 255u));
        return (unsigned short)((((fr * 31u + 127u) / 255u) << 10) |
                                (((fg * 31u + 127u) / 255u) << 5) |
                                 ((fb * 31u + 127u) / 255u));
    }
    if (g_GBits == 6)
    {
        dr8 = g_ExpR[(dst >> 11) & 0x1Fu];
        dg8 = g_ExpG[(dst >> 5) & 0x3Fu];
        db8 = g_ExpB[dst & 0x1Fu];
        or_ = (fr * a + dr8 * (255u - a) + 127u) / 255u;
        og = (fg * a + dg8 * (255u - a) + 127u) / 255u;
        ob = (fb * a + db8 * (255u - a) + 127u) / 255u;
        or_ = (or_ * 31u + 127u) / 255u;
        og = (og * 63u + 127u) / 255u;
        ob = (ob * 31u + 127u) / 255u;
        return (unsigned short)((or_ << 11) | (og << 5) | ob);
    }
    dr8 = g_ExpR[(dst >> 10) & 0x1Fu];
    dg8 = g_ExpG[(dst >> 5) & 0x1Fu];
    db8 = g_ExpB[dst & 0x1Fu];
    or_ = (fr * a + dr8 * (255u - a) + 127u) / 255u;
    og = (fg * a + dg8 * (255u - a) + 127u) / 255u;
    ob = (fb * a + db8 * (255u - a) + 127u) / 255u;
    or_ = (or_ * 31u + 127u) / 255u;
    og = (og * 31u + 127u) / 255u;
    ob = (ob * 31u + 127u) / 255u;
    return (unsigned short)((or_ << 10) | (og << 5) | ob);
}

/* 纯黑叠加 (阴影): 与 Blend16(…,0,0,0,a) 结果相同, 更省。 */
static __inline unsigned short BlendDark(unsigned short dst, unsigned a)
{
    unsigned m = 255u - a, or_, og, ob_;
    if (a == 0) return dst;
    if (a == 255) return 0;
    if (!g_ExpReady) GtBuildExpand();
    if (g_GBits == 6)
    {
        or_  = (g_ExpR[(dst >> 11) & 0x1Fu] * m + 127u) / 255u;
        og   = (g_ExpG[(dst >> 5) & 0x3Fu] * m + 127u) / 255u;
        ob_  = (g_ExpB[dst & 0x1Fu] * m + 127u) / 255u;
        or_  = (or_ * 31u + 127u) / 255u;
        og   = (og * 63u + 127u) / 255u;
        ob_  = (ob_ * 31u + 127u) / 255u;
        return (unsigned short)((or_ << 11) | (og << 5) | ob_);
    }
    or_ = (g_ExpR[(dst >> 10) & 0x1Fu] * m + 127u) / 255u;
    og  = (g_ExpG[(dst >> 5) & 0x1Fu] * m + 127u) / 255u;
    ob_ = (g_ExpB[dst & 0x1Fu] * m + 127u) / 255u;
    or_ = (or_ * 31u + 127u) / 255u;
    og  = (og * 31u + 127u) / 255u;
    ob_ = (ob_ * 31u + 127u) / 255u;
    return (unsigned short)((or_ << 10) | (og << 5) | ob_);
}
/* 16 位像素拆回 8 位 RGB (用标定格式) */
void GtUnpack16(unsigned short px, unsigned char* r, unsigned char* g,
                unsigned char* b);

/* ---------------- GDI 光栅化 ---------------- */
#define DIB_W 1024
#define DIB_H 64

#define ABC_MEMO_N 1024

/* 槽: 0=战役框 1=提示框 2=全局文本主字号 3=超武面板/底电 4=聊天输入
 * 5=超武充能% 6=超武悬停名 7=缩放刻度 8=全局文本顶栏字号 9=FPS钉住。一槽一字号。 */
#define GDI_SLOTS 10

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
    int sidx;               /* 槽序号 (行缓存按槽代号作废) */
    wchar_t face[FONT_FACE_MAX];  /* 缓存键 */
    /* 单字 ABC 记忆表: 槽重建时键值一起清。 */
    short abcM[ABC_MEMO_N];       /* -1 = 空 */
    short abcA[ABC_MEMO_N], abcB[ABC_MEMO_N], abcC[ABC_MEMO_N];
    wchar_t abcK[ABC_MEMO_N];
} GdiSlot;

extern GdiSlot* g_G;      /* 当前槽 */
int GdiEnsure(int fontPx, int bold, int slot, const wchar_t* face);
/* 单字前进宽 (ABC 全宽); 需先 GdiEnsure。失败 -1 (0 是合法宽度)。 */
int GtCharAdv(wchar_t ch);
/* 单字 ABC 三段; 需 abcA 摆位的调用方用。 */
int GtCharABC(wchar_t ch, int* pa, int* pb, int* pc);
/* 一行画进 DIB, 返回像素宽; 覆盖率=红通道, 带行缓存。
 * 有效区为 [0, 返回宽+2) x [0, g_G->adv)。 */
int RenderLine(const wchar_t* text, int len);
/* 量一段文本在 maxW 内能放几个字符 */
int CharsFit(const wchar_t* text, int len, int maxW);
/* 单字形光栅: 在 (x0,y0) 画一个字, 返回像素宽。只清用到的矩形。 */
int GtRenderGlyphAt(wchar_t ch, int x0, int y0, int* outW);
/* DIB 覆盖率 blit 到已 Lock 的 16 位面。 */
void BlitCoverage(unsigned char* dstBase, int dstPitch, int surfW, int surfH,
                  int dx, int dy, int w, int h,
                  unsigned fr, unsigned fg, unsigned fb, unsigned alphaScale);

/* 阴影单遍合成 (Size×Size 偏移块并集, 每像素只混一次)。
 * sw/sh 为源区 (惯例: 文字宽+2 × 行高)。 */
void BlitCoverageShadow(unsigned char* dstBase, int dstPitch,
                        int surfW, int surfH,
                        int dx, int dy, int sw, int sh,
                        int Size, int offX, int offY,
                        unsigned alphaScale);
