/* GdiText.h - MOExperience.dll 内共享的无 CRT 工具 / 日志 / 字体解析 /
 * GDI 光栅 / 16 位混合管线 / 画面虚表底座 (全 DLL 共用)。
 */#pragma once

#include <windows.h>
#include "EngineAddr.h"

#define MAX_PATH_LEN 260
#define LOG_LINE_MAX 1024
#define FONT_FACE_MAX 48

typedef struct { int X, Y; } Point2D;
typedef struct { int X, Y, Width, Height; } RectangleStruct;
typedef struct { unsigned char R, G, B; } ColorStruct;

/* ---------------- 无 CRT 工具 (定义在 GdiText.cpp, 全 DLL 一份) ----------------
 * 注意返回值口径不同, 混用会静默把后续内容写到字符串外 (日志被截断):
 *   UtoA/HtoA(v, p)            写到 p 处, 返回**写入长度**  → pos += UtoA(...)
 *   AppendCStr/Ch/Int(dst,pos) 返回**新的绝对位置**        → pos =  AppendCStr(...) */
unsigned UtoA(unsigned v, char* p);
unsigned HtoA(unsigned v, char* p);
unsigned AppendCStr(char* dst, unsigned pos, const char* src, unsigned cap);
unsigned AppendCh(char* dst, unsigned pos, char c, unsigned cap);
unsigned AppendInt(char* dst, unsigned pos, unsigned cap, int v);
int IsPlausiblePtr(const void* p);
/* 从 p 起**真的**可安全读取的字节数 (最多 maxBytes, 0 = 不可读)。
 * IsPlausiblePtr 只查范围/对齐, 引擎偶尔会传进野指针 (实测 0x32AC8010,
 * 读第一个 wchar 就页错误), 解引用外来指针前用它兜一层。 */
unsigned MemReadableBytes(const void* p, unsigned maxBytes);

/* ---------------- 日志 ---------------- */
extern HANDLE g_Log;
extern char g_GameDir[MAX_PATH_LEN];   /* 游戏根目录, BuildPaths 填 */
extern char g_IniPath[MAX_PATH_LEN];
extern char g_LogPath[MAX_PATH_LEN];
void BuildPaths(void);
/* 游戏根目录 (无尾分隔符; 取不到 = 空串)。**拼根目录下的文件一律用它**,
 * 不要自己 GetModuleFileNameA + 往回找分隔符。 */
const char* GtGameDir(void);
void GtEnsureLog(void);   /* 定义在 MOExperience.cpp; 幂等: 建路径+打开日志, 失败则 g_Log=NULL */
void LogLine(const char* body);
/* ---------------- 鼠标游戏坐标 (WWMouse 原始客户区坐标, 供 PatrolCircle 等输入模块用), 返回 1=有效 */
int GtMouseGameXY(int* x, int* y);

/* 详细日志总开关: 0=只记关键事件 (默认, 日志小); 1=逐字/逐帧普查 */
extern int g_LogDetail;

/* ---------------- 帧内耗时探针 ([Log] Perf=1) ----------------
 * 只在主线程用; 关掉时 Perf_Now 直接返回 0, Perf_Add 见 0 立刻返回 (每帧几十次
 * 调用也只是几十条 if), 对帧率无可测影响。每 2s 往日志写一行 us/帧。
 * 用法: t = Perf_Now(); ...干活...; Perf_Add(PF_XXX, t);
 * Perf_Frame 每帧末调一次 (消息帧钩尾部), 负责累计帧数与结算。 */
enum
{
    PF_GATE = 0,    /* PV_MainGate 整段 */
    PF_FC,          /* FrameCap_Gate */
    PF_GS,          /* GameSpeed_Frame */
    PF_PREPARE,     /* 每循环 prepare (PowerDrawBoth) 整段 */
    PF_PREP_FM,     /* 其内 FM_Poll */
    PF_PREP_SB,     /* 其内 SB_Poll */
    PF_PREP_SS,     /* 其内 SS_Poll */
    /* prepare 里逐个 Poll (A 段: 输入/战斗类) */
    PF_A_AW, PF_A_BQ, PF_A_PC, PF_A_SO, PF_A_RP, PF_A_TC, PF_A_FA, PF_A_AM,
    PF_A_SA, PF_A_DP, PF_A_MS, PF_A_OT, PF_A_WPC, PF_A_MLC, PF_A_SWS,
    PF_A_RA, PF_A_PS, PF_A_AG, PF_A_OB, PF_A_LIGHT, PF_A_BC,
    /* prepare 后段 (B 段: 底栏/绘制类) */
    PF_B_PV, PF_B_SHOT, PF_B_VTS, PF_B_PUB, PF_B_MHK, PF_B_PWR, PF_B_PIPB,
    PF_B_SKIN, PF_B_PVP,
    PF_MSG,         /* 消息帧钩整段 */
    PF_MSG_TEXT,    /* MsgText_DrawFrame */
    PF_MSG_SWP,     /* SWPanel_Draw */
    PF_MSG_DRAWS,   /* 其余各 *_Draw + FM_DrawRadarTo */
    PF_MSG_TAIL,    /* ChatEdit + 底电 + 条带/按钮/截图 */
    PF_BLITSB,      /* 侧栏呈现入口钩 */
    PF_AFTERMAP,    /* 三趟地图后的钩 */
    PF_RENDERIN,    /* Render 入口钩 */
    PF_VECBLIT,     /* VecText_BlitHook 累计 (每次一个字) */
    PF_VECGETDIM,   /* VecText 字宽度量钩累计 (每次一次度量) */
    PF_PLACEPREV,   /* 放置预览绘制钩 0x6D528A */
    PF_SHROUD,      /* 迷雾 DrawPixelEffects 入口钩 */
    /* ---- 别再往事件型钩子里加纯计次的埋点: 想要数值就给 Perf_Add 计时,
     * 想查频次用临时开关 ---- */
    /* SWPanel 内部细分 (找 200us 花在哪: 量宽 / 底板 / 文字) */
    PF_SWP_MEAS, PF_SWP_BG, PF_SWP_TXT,
    PF_H_PRINT, PF_H_SIMPLE,   /* VecText 的 Print / Simple_ 入口 (含被拦截的串) */
    PF_H_PRTO, PF_H_SMPO,      /* 上面两者里"放行给引擎原版"那部分 */
    PF_H_VB1, PF_H_VB2,        /* BlitBody: 入口→VecMeasure 完 / Measure→返回 */
    PF_H_CLAER,                /* 侧栏面整条抹黑 (Fm_ClearStripExceptRadar) */
    /* ---- 调用时机 (谁在每帧重复跑 / 谁在空转) ---- */
    PF_SB_SS, PF_SB_PWR, PF_SB_PIP, PF_SB_FM,   /* BlitSb 入口四段 (含与 prepare 重复的两段) */
    PF_D_SWS, PF_D_SO, PF_D_PC, PF_D_RP, PF_D_FA, PF_D_SA, PF_D_TC, PF_D_DP,
    PF_D_WR, PF_D_AG, PF_D_RADAR,               /* msgdraw 里 10 个 *_Draw + 雷达上屏 */
    PF_T_CE, PF_T_PWR, PF_T_SB, PF_T_PV, PF_T_SHOT,  /* msgtail 五段 */
    PF_VHIT, PF_VMISS,         /* VecGlyph 字形缓存 命中/未命中 (未命中 = 一次 GDI 光栅) */
    PF_LOCK,                   /* 全部 GtLock+GtUnlock 的 vtable 调用累计 */
    PF_SB2_SETUP, PF_SB2_FILL, PF_SB2_LBL,   /* 刻度条: 取面/布局 / 填充 / 档位文字 */
    PF_SWS_BUILD, PF_SWS_BLIT,  /* 超武侧栏: 重建缓存 (帧门 + 墙钟门) / 每帧带 alpha 贴层 */
    PF_D_WTH,                   /* 天气覆盖层: 引擎 blit 平铺 + 粒子 */
    PF_D_CLD,                   /* 其中云影那一层 (逐像素压暗) */
    PF_A_UP,                    /* 左下选中面板: 每循环快照/摆位/取点击 */
    PF_D_UP,                    /* 左下选中面板: 画进 Composite */
    PF_COUNT
};
long long Perf_Now(void);
void Perf_Add(int slot, long long t0);
/* 只计次 (不做 QPC): 放钩子最前面, 用来普查调用频率。关探针时两三条指令返回。 */
void Perf_Hit(int slot);
void Perf_AddDelta(int slot, long long dt);   /* 直接累加时间差 (分段算差值用) */
void Perf_Frame(void);
void Perf_EnsureInit(void);
/* VecText 最近一次字形缓存未命中的原因码 (1=首次 2=字符 3=idata 4=px 5=adv 6=h 7=nat) */
extern int g_VecMissWhy;

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
/* 游戏坐标系整帧宽高 (窗口 Composite 面 = 引擎逻辑分辨率; 0=读不到)。
 * **不是 Primary**: 虚拟分辨率/上采样下 Primary 是显示器尺寸, 拿它当基准会把
 * 底部 UI (刻度条/暂停按钮) 锚到面外。鼠标不再用它换算 (见 GtMouseGameXY)。 */
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
/* 游戏根目录下是否有该文件 (g_GameDir 无尾分隔符, 内部补齐) */
int FileAtGameRoot(const char* name);
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
/* 5 位 alpha 快速混合: TB[inv][dst] = dst*inv/31, inv = 31 - (alpha>>3)。
 * 与前景色无关 (前景部分按字现建), 供逐字光栅那条热循环用。 */
extern unsigned char g_BlendR[32][32], g_BlendG[32][64];
extern int g_BlendReady;
void GtBuildBlend5(void);
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
 * 5=超武充能% 6=超武悬停名 7=缩放刻度 8=全局文本顶栏字号 9=FPS钉住
 * 10=ESC标题 11=ESC正文 12=左下选中面板 13=左下选中面板的数量角标。一槽一字号。 */
#define GDI_SLOTS 14

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
    /* 字体自己的垂直度量 (GetTextMetrics): 逐字按基线摆位, 行高按 cell。 */
    int asc;    /* tmAscent: 行顶到基线 */
    int cell;   /* tmHeight: 一行的自然高度 */
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
/* DIB 覆盖率 blit 到已 Lock 的 16 位面。
 * alphaScale 是**透明度百分比**: 100 = 覆盖率原样 (不透明); 超过 100 一律按 100。
 * 不是 0~255: 传 255 会让 cov>=100 的像素全饱和, 抗锯齿边缘被削平 (小字发胖发花)。 */
void BlitCoverage(unsigned char* dstBase, int dstPitch, int surfW, int surfH,
                  int dx, int dy, int w, int h,
                  unsigned fr, unsigned fg, unsigned fb, unsigned alphaScale);

/* 阴影单遍合成 (Size×Size 偏移块并集, 每像素只混一次)。
 * sw/sh 为源区 (惯例: 文字宽+2 × 行高)。alphaScale 同 BlitCoverage (百分比)。 */
void BlitCoverageShadow(unsigned char* dstBase, int dstPitch,
                        int surfW, int surfH,
                        int dx, int dy, int sw, int sh,
                        int Size, int offX, int offY,
                        unsigned alphaScale);
