/* Weather.cpp - 全屏天气覆盖层 (图像 / 粒子两种画法)。
 *
 * 图像档: RAIN/SNOW.SHP 是整屏单帧 (768x576 / 800x600, 19 帧循环), 按相机
 * 滚动量平铺铺满视图区。**直接调引擎 CC_Draw_Shape 画到 Composite**, 没有中间面
 * 也没有逐像素搬运: SHP 的透明像素由 blitter 自己跳过, Bounds 参数就是裁剪框。
 *
 * 粒子档: 自绘雨丝/雪花/火星, 每帧推进, 连续平滑。三种天气是**同一套粒子**的三份
 * 参数 (WthKind 表): 一个池子、一套推进、一个落笔循环。位置与速度都是 1/16 像素
 * 定点, 速度以**逻辑帧**为单位 —— 与图像档、引擎动画同一把尺子, 游戏速度降到 15
 * 帧时雨也跟着慢下来。粒子活在视图区局部坐标系里, 相机一移动就整批反向平移。
 *
 * 落地: 按节拍在**整个视图区**里随机取点 (不按上下半屏筛 —— 这是正交俯视, 屏幕上方
 * 同样是地面), 溅起几颗水滴/火星, 不查地形高度也不拿视图底边当地面 (底边只是屏幕
 * 边界); 火雨再留一团烟。
 * 云影: 视图区上盖一层自平铺 Perlin 云图的压暗, 独立开关, 与雨/雪无关。
 *
 * 图像档的节奏: 引擎动画按逻辑帧播 —— art.ini 的 Rate 在读 ini 时就已经换算成
 * "每动画帧过几个逻辑帧" 存进类型 (RAIN/SNOW = 2, 即 900/400 截断), 直接拿
 * 它当间隔; 不能再拿 art 的原始 Rate 算一遍 (会变成 450 帧一帧, 看着是静止的)。
 *
 * 雨 / 雪 / 火雨 各自独立开关 (雨+雪 = 雨夹雪), 只存在于本次运行;
 * **云影开关与画法**是长期设置, 落盘 (见 Wth_LoadState)。
 * 纯渲染: 只读相机与调色板, 只写 Composite。联机安全。
 */

#include "Weather.h"
#include "GdiText.h"
#include "FullMap.h"
#include <emmintrin.h>          /* 云影压暗的 SSE2 版 (见 Wth_CloudFillSse2) */

/* ---------------- 引擎结构 (只读) ---------------- */

/* SHPStruct 头 (YRpp FileFormats/SHP.h): Type@0, Width@2, Height@4, Frames@6。
 * Type==0xFFFF 是 SHPReference; CC_Draw_Shape 两种都收。 */
typedef struct {
    unsigned short Type;
    short Width;
    short Height;
    short Frames;
} WthShpHead;

/* CC_Draw_Shape (gamemd 0x4AED70): __fastcall ecx=Surface, edx=ConvertClass*,
 * ret 0x38。栈参 = SHP, Frame, Pos, Bounds, Flags, Remap, ZAdjust, ZGradient,
 * Brightness, Tint, ZShape, ZFrame, XOff, YOff (YRpp DSurface::DrawSHP 顺序)。
 * 无 CENTERED 标志时 Pos 是图形左上角; Bounds 是**裁剪框**, 出界部分引擎自己
 * 裁掉 —— 所以平铺时把视图区直接当裁剪框传, 不用自己算交点。 */
typedef void (__fastcall *WthDrawShpFn)(void* surf, void* pal, void* shp,
    int frame, void* pos, void* bounds, int flags, int remap, int zadjust,
    int zgrad, int bright, int tint, void* zshape, int zframe, int xoff, int yoff);

#define WTH_FLAGS   0x400     /* 与侧栏/放置预览画 SHP 用的是同一组参数 */
#define WTH_BRIGHT  1000

typedef struct { int X, Y; } WthPt;

/* ---------------- 图像档: 单个图层 (雨 / 雪) ---------------- */

typedef struct {
    const char* id;         /* art.ini 的动画 ID */
    void* type;             /* AnimTypeClass* */
    void* shp;              /* SHPStruct* */
    int frames, w, h;
    int loopStart, loopEnd;
    int step;               /* 每动画帧要过的逻辑帧数 (引擎已算好的值, 见 Wth_Ensure) */
    int tick;               /* 未满一帧的余数, 单位 = 1/100 逻辑帧 (要吃得下 ImageSpeed) */
    int frame;
    int fail;               /* 加载失败过就不再每帧重试 */
    int scrollX, scrollY;   /* 相机滚动累计 (像素) */
    unsigned draws, adv;    /* 诊断: 画了几帧 / 推进了几个动画帧 (打一次日志用) */
    int diagDone;
} WthLayer;

static WthLayer g_Layer[2] = { { "RAIN" }, { "SNOW" } };

/* ---------------- 参数 (ini 的 [Weather] 段) ----------------
 * 这里只有表现参数。落盘的开关只有"云影"和"画法"两项 (见 Wth_LoadState), 降水
 * 开关只在本次运行内有效 —— 配置里不该留下"每次进游戏都下火雨"。
 * 速度统一按"60 帧基准下的像素/秒"给, 内部再换算成 1/16 像素/逻辑帧。 */
#define WTH_SECTION "Weather"

enum { WTH_K_RAIN = 0, WTH_K_SNOW = 1, WTH_K_FIRE = 2, WTH_K_N = 3 };

/* ---------------- 状态 ----------------
 * 三个降水开关 + 云影 + 画法, 互不影响: 雨 + 雪 = 雨夹雪, 云影可以单独开。
 * 只有"云影"和"画法"两项落盘 (长期设置, 见 Wth_LoadState); 降水开关每次进游戏
 * 都从关开始, 免得配置里留下"恒定天气"。 */
static int g_Mode = WEATHER_DRAW_IMAGE;
static int g_On[WTH_K_N];       /* 按 WTH_K_* 索引 */
static int g_CloudOn = 0;
static int g_Init = 0;

/* ---------------- 粒子档容量 ----------------
 * 坐标与速度都是 1/16 像素定点 (整数, 不走浮点)。三种天气共用**一个**池子,
 * 布点时按 kind 顺序依次分配 (见 Wth_PxInit), 总和不会超过 WTH_PX_MAX。 */
#define WTH_FX        4            /* 定点小数位 */
#define WTH_PX_MAX    1536
#define WTH_SMOKE_MAX 192
#define WTH_DROP_MAX  320
#define WTH_SM_RMAX   8            /* 烟团最大半径 (像素) */
#define WTH_DROP_GRAV 7            /* 溅射重力: 每逻辑帧 1/16 像素/帧 的增量 */

/* 头部画法: 拖尾是共用的一段代码, 只有头不一样。 */
enum {
    WTH_HEAD_LINE = 0,      /* 有宽度的直线: 雨丝 */
    WTH_HEAD_FLAKE,         /* 十字 + 下沿暗影: 雪花 */
    WTH_HEAD_GLOW           /* 加法光斑 + 尾迹之上的烟柱: 火球 */
};

/* 一种天气 = 一行参数。雨 / 雪 / 火雨 的差别全在这里, 推进与落笔是同一份代码。 */
typedef struct {
    int count, countMin, countMax;  /* 数量 = 视图面积/count, 再夹到 [min,max] */
    int vLo, vHi;                   /* 速度 px/s */
    int aLo, aHi;                   /* alpha 0~255 */
    int lenLo, lenHi;               /* 拖尾长度 (像素, 0 = 没有拖尾) */
    int size;                       /* 线宽 / 雪臂长 / 火头半径 */
    int wind;                       /* 水平速度 = 下落速度*wind/100, 0 = 用 spread */
    int spread;                     /* 水平速度随机抖动 (%) */
    int sway;                       /* 左右摇摆 (×1/5 像素每帧), 0 = 不摇 */
    int bigRate;                    /* 1/n 概率变大, 0 = 恒定 */
    int splash;                     /* 落地溅起几颗 (0 = 不溅) */
    int splashRate;                 /* 每秒落点次数, 0 = 按数量推算 */
    int head;                       /* WTH_HEAD_* */
    int fade;                       /* 尾端 alpha 占头部的百分比, 0 = 淡到无 */
    int tailSmoke;                  /* 尾迹后段并入烟色的程度 (0~255) */
    int plume;                      /* 尾迹之上的烟柱长度 (像素, 0 = 不画) */
    int smokeLo, smokeHi;           /* >0 = 沿尾迹冒烟, 每隔几个逻辑帧一团 */
    int colA[3], colB[3], colC[3];  /* 拖尾根 / 尾端 / 头部 */
} WthKind;

/* 云影风格 (预设, ESC 菜单里选)。烘焙算法见 Wth_CloudBake。
 * 每种风格有自己的置换表种子: 同一套表下团块位置整屏相关, 换阈值只改浓淡,
 * 改不了"哪里长云", 所以云形靠换种子而不靠调参。
 *
 *   perm       置换表种子 (打乱梯度表用): 换它 = 换一张完全无关的云图
 *   bPer/bOct  低频层: 决定"一屏几团云", 也是云块大小的来源 (bPer=6 时一团云 ≈ CloudScale)
 *   bAmp       低频层每层幅度衰减 (%): 50=经典 fBm。调小 → 低频压死高频 → 平滑;
 *              调大 → 高频占比上去 → 絮多、碎
 *   dPer/dOct  高频层: 只在云内部起作用, 决定边缘侵蚀的细度。最细一层的格子必须
 *              >= 6 个纹素, 否则就是像素噪声, 云边会碎成屑
 *   dAmp       高频层每层幅度衰减 (%), 同上
 *   warp       低频层的域扭曲 (‰), 0 = 云边不起浪 (层云/薄云)
 *   clump      超大尺度疏密不均 (0~100): 在整块图上叠一层 per=2 的单层噪声,
 *              让"这一大片云多、那一片几乎空", 覆盖总量由二分校准补回
 *   squash     y 压缩 (%): 本作是 2:1 等距视图, 地面上的圆在屏幕上本是扁的。压缩
 *              **烘进纹理** (y 向格数 = x * 100/squash), 采样与绘制仍 1:1, 两轴平铺
 *              周期一样; 改到采样端压会让 y 周期减半、一屏重复加倍。100 = 不压 (俯视)。
 *   cover      目标覆盖面积 (%)
 *   edge       覆盖率到"实心"的过渡带 (%): 越大云边越软, 越小越像撕开的纸
 *   blur       [1,2,1] 模糊次数, 抹掉纹素级锯齿 */
typedef struct {
    const char* name;
    unsigned perm;
    int bPer, bOct, bAmp, dPer, dOct, dAmp;
    int warp, clump, squash;
    int cover, edge, blur;
} WthCldStyle;

static const WthCldStyle kCldStyle[] = {
    /* 密集团状积云: 中等团, 边絮, 全屏疏密均匀 —— 最常见的夏天云 */
    { "cumulus",  0x01234567u,   6, 3, 50,  14, 3, 52,  90,  0, 50, 46, 30, 1 },
    /* 大团稀疏: 一团几十像素, 团与团之间大片晴空, 再加疏密不均 */
    { "big",      0x2F8A31C5u,   3, 3, 55,   9, 3, 45,  85, 22, 54, 32, 26, 1 },
    /* 细碎: 层幅衰减慢 (62), 高频顶上来压过低频, 团小边毛, 满天碎棉 */
    { "small",    0x51D0B473u,  11, 3, 62,  26, 3, 62,  80,  0, 48, 42, 30, 1 },
    /* 平滑层云: 两层低频 + 无扭曲, 边软成一片纱; 压得最扁 (贴地的薄云带) */
    { "sheet",    0x7C31F09Bu,   5, 2, 40,   9, 2, 38,   0, 20, 40, 70, 55, 2 },
    /* 阴天: 几乎全在影里; detail 权重压到最低 (28) + 模糊 3 次, 免得像石块 */
    { "overcast", 0x9E5A2D03u,   6, 3, 42,  10, 3, 28,  60, 35, 58, 88, 58, 3 },
    /* 不均匀: 硬边 + 最强的疏密不均, 一半晴朗一半压在云底 */
    { "patchy",   0xB4C7E119u,   4, 3, 52,  16, 3, 55, 100, 55, 47, 42, 18, 1 },
    /* 条状: 大团 + 压到 30%, 一个圆团在屏幕上就是一条横带 (碎团压扁只会变成碎点) */
    { "streak",   0xC3E5D7A2u,   5, 3, 50,  14, 3, 50,  60, 25, 30, 40, 34, 1 }
};

#define CLD_STYLE_N ((int)(sizeof(kCldStyle) / sizeof(kCldStyle[0])))

/* 菜单里的风格个数写死在 Weather.h (那边不能 include 本文件), 这里对齐一下 */
typedef char WthCldStyleCountCheck[(CLD_STYLE_N == WEATHER_CLOUD_STYLE_N) ? 1 : -1];

typedef struct {
    int speedScale;                 /* 粒子速度统一倍率 (%), 100 = 配置值原速 */
    int imageSpeed;                 /* 图像档播放速度倍率 (%), 100 = 引擎原速 */

    /* 烟团池: 火雨的尾迹烟与落地烟共用, 与"哪种天气"无关 */
    int smokeLifeLo, smokeLifeHi;
    int smokeALo, smokeAHi;
    int smokeR;                     /* 烟团最大半径 (像素, <= WTH_SM_RMAX) */
    int smokeGrowLo, smokeGrowHi;   /* 每逻辑帧半径增量 (1/16 像素) */

    int cloudOpacity;               /* 云心最大压暗 alpha (0~255), 0 = 关 */
    int cloudScale;                 /* 云块尺度 (像素) */
    int cloudStyle;                 /* 云影风格, 索引 kCldStyle */

    int smokeLight[3], smokeDark[3];
} WthCfg;

static WthCfg g_Cfg;
static WthKind g_Kind[WTH_K_N];

static void Wth_Col(int* d, int r, int g, int b)
{
    d[0] = r; d[1] = g; d[2] = b;
}

static void Wth_Defaults(void)
{
    WthKind* k;

    g_Cfg.speedScale = 100;
    g_Cfg.imageSpeed = 100;

    g_Cfg.smokeLifeLo = 30;  g_Cfg.smokeLifeHi = 44;
    g_Cfg.smokeALo = 90;     g_Cfg.smokeAHi = 140;
    g_Cfg.smokeR = WTH_SM_RMAX;
    g_Cfg.smokeGrowLo = 3;   g_Cfg.smokeGrowHi = 6;

    g_Cfg.cloudOpacity = 64;        /* 云心压暗 25%: 再重就像"天黑了"而不是"过云" */
    g_Cfg.cloudScale = 400;         /* 一团云约 400px, 一屏七八团 */
    g_Cfg.cloudStyle = 0;           /* cumulus */

    Wth_Col(g_Cfg.smokeLight, 178, 176, 172);
    Wth_Col(g_Cfg.smokeDark, 70, 66, 62);

    /* 雨: 细长线, 带风, 落地溅水花 */
    k = &g_Kind[WTH_K_RAIN];
    k->count = 1100; k->countMin = 90; k->countMax = 640;
    k->vLo = 600;   k->vHi = 1020;
    k->aLo = 150;   k->aHi = 220;
    k->lenLo = 12;  k->lenHi = 24;
    k->size = 2;
    k->wind = -22;
    k->splash = 2;
    k->head = WTH_HEAD_LINE;
    k->fade = 0;
    Wth_Col(k->colA, 206, 227, 252);
    Wth_Col(k->colB, 206, 227, 252);

    /* 雪: 慢、十字片、左右摇摆, 不溅 */
    k = &g_Kind[WTH_K_SNOW];
    k->count = 1200; k->countMin = 80; k->countMax = 640;
    k->vLo = 22;    k->vHi = 67;
    k->aLo = 185;   k->aHi = 255;
    k->size = 1;
    k->spread = 45;
    k->sway = 1;
    k->bigRate = 4;
    k->head = WTH_HEAD_FLAKE;
    Wth_Col(k->colA, 246, 250, 255);
    Wth_Col(k->colB, 246, 250, 255);

    /* 火雨: 快、亮、带烟 */
    k = &g_Kind[WTH_K_FIRE];
    k->count = 7500; k->countMin = 24; k->countMax = 96;
    k->vLo = 840;   k->vHi = 1560;
    k->aLo = 190;   k->aHi = 240;
    k->lenLo = 20;  k->lenHi = 38;
    k->size = 2;
    k->spread = 15;
    k->splash = 3;
    k->head = WTH_HEAD_GLOW;
    k->fade = 25;                   /* 尾端留 1/4 alpha, 与烟柱起点接得上 */
    k->tailSmoke = 150;
    k->plume = 22;
    k->smokeLo = 20; k->smokeHi = 34;
    Wth_Col(k->colA, 255, 196, 84);   /* 拖尾根: 亮橙 */
    Wth_Col(k->colB, 150, 28, 10);    /* 尾端: 暗红 */
    Wth_Col(k->colC, 255, 252, 220);  /* 头部: 白热 */
}

/* "RRGGBB" / "#RRGGBB" / "R,G,B" 三种写法都认, 解析失败保持原值。 */
static int Wth_Hex(int c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void Wth_ParseColor(const char* s, int* out)
{
    int v[3] = { 0, 0, 0 }, n = 0;
    const char* p;
    int hasComma = 0;

    if (!s || !*s) return;
    if (*s == '#') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (p = s; *p; p++)
        if (*p == ',') { hasComma = 1; break; }

    if (hasComma)                       /* R,G,B */
    {
        while (*s && n < 3)
        {
            int val = 0;

            while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; }
            v[n++] = val;
            while (*s == ',' || *s == ' ') s++;
        }
    }
    else                                /* 十六进制整串 */
    {
        long val = 0;
        int d, len = 0;

        while ((d = Wth_Hex((unsigned char)*s)) >= 0) { val = val * 16 + d; s++; len++; }
        if (len == 0) return;
        if (len <= 3)                   /* RGB 简写 */
        {
            v[0] = (int)((val >> 8) & 0xF) * 17;
            v[1] = (int)((val >> 4) & 0xF) * 17;
            v[2] = (int)(val & 0xF) * 17;
        }
        else
        {
            v[0] = (int)((val >> 16) & 0xFF);
            v[1] = (int)((val >> 8) & 0xFF);
            v[2] = (int)(val & 0xFF);
        }
        out[0] = v[0] > 255 ? 255 : v[0];
        out[1] = v[1] > 255 ? 255 : v[1];
        out[2] = v[2] > 255 ? 255 : v[2];
        return;
    }
    for (n = 0; n < 3; n++)
        out[n] = (v[n] < 0) ? 0 : (v[n] > 255 ? 255 : v[n]);
}

/* CloudStyle 允许写序号或名字 (cumulus / big / small / sheet / overcast, 不分大小写)。
 * 认不出来返回 -1, 调用处保持原值。 */
static int Wth_StyleParse(const char* s)
{
    int i, v = 0;

    if (!s) return -1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s >= '0' && *s <= '9')
    {
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
        return (v < CLD_STYLE_N) ? v : -1;
    }
    for (i = 0; i < CLD_STYLE_N; i++)
    {
        const char* a = s;
        const char* b = kCldStyle[i].name;

        while (*a && *b)
        {
            int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
            int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;

            if (ca != cb) break;
            a++; b++;
        }
        if (!*a && !*b) return i;
    }
    return -1;
}

/* 三种天气的键名只差前缀, 用宏展开成三份 —— 加一种天气 = 加一行 WTH_KCFG。
 * 键名沿用老配置 (RainCount / SnowCount / FireCount ...), 老 ini 不用改。 */
#define WTH_KCFG(pfx, i)                                                     \
    { pfx "Count",      &g_Kind[i].count },                                  \
    { pfx "CountMin",   &g_Kind[i].countMin },                               \
    { pfx "CountMax",   &g_Kind[i].countMax },                               \
    { pfx "SpeedMin",   &g_Kind[i].vLo },                                    \
    { pfx "SpeedMax",   &g_Kind[i].vHi },                                    \
    { pfx "AlphaMin",   &g_Kind[i].aLo },                                    \
    { pfx "AlphaMax",   &g_Kind[i].aHi },                                    \
    { pfx "LenMin",     &g_Kind[i].lenLo },                                  \
    { pfx "LenMax",     &g_Kind[i].lenHi },                                  \
    { pfx "Size",       &g_Kind[i].size },                                   \
    { pfx "Splash",     &g_Kind[i].splash },                                 \
    { pfx "SplashRate", &g_Kind[i].splashRate }

static void Wth_ReadCfg(void)
{
    char buf[64];
    int i;
    static const struct { const char* k; int* p; } t[] = {
        { "SpeedScale", &g_Cfg.speedScale },  { "ImageSpeed", &g_Cfg.imageSpeed },
        WTH_KCFG("Rain", WTH_K_RAIN),
        WTH_KCFG("Snow", WTH_K_SNOW),
        WTH_KCFG("Fire", WTH_K_FIRE),
        { "RainWind",  &g_Kind[WTH_K_RAIN].wind },
        { "SnowBigRate", &g_Kind[WTH_K_SNOW].bigRate },
        { "FireTailSmoke", &g_Kind[WTH_K_FIRE].tailSmoke },
        { "FirePlume",     &g_Kind[WTH_K_FIRE].plume },
        { "FireSmokeMin",  &g_Kind[WTH_K_FIRE].smokeLo },
        { "FireSmokeMax",  &g_Kind[WTH_K_FIRE].smokeHi },
        { "SmokeLifeMin",  &g_Cfg.smokeLifeLo },  { "SmokeLifeMax", &g_Cfg.smokeLifeHi },
        { "SmokeAlphaMin", &g_Cfg.smokeALo },     { "SmokeAlphaMax", &g_Cfg.smokeAHi },
        { "SmokeRadius",   &g_Cfg.smokeR },       { "SmokeGrowMin", &g_Cfg.smokeGrowLo },
        { "SmokeGrowMax",  &g_Cfg.smokeGrowHi },
        { "CloudOpacity",  &g_Cfg.cloudOpacity }, { "CloudScale", &g_Cfg.cloudScale },
        { "CloudStyle",    &g_Cfg.cloudStyle }
    };

    for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++)
        *t[i].p = GetPrivateProfileIntA(WTH_SECTION, t[i].k, *t[i].p, g_IniPath);

    if (GetPrivateProfileStringA(WTH_SECTION, "RainColor", "", buf,
                                 sizeof(buf), g_IniPath) > 0)
        Wth_ParseColor(buf, g_Kind[WTH_K_RAIN].colA);
    if (GetPrivateProfileStringA(WTH_SECTION, "SnowColor", "", buf,
                                 sizeof(buf), g_IniPath) > 0)
        Wth_ParseColor(buf, g_Kind[WTH_K_SNOW].colA);
    if (GetPrivateProfileStringA(WTH_SECTION, "FireHeadColor", "", buf,
                                 sizeof(buf), g_IniPath) > 0)
        Wth_ParseColor(buf, g_Kind[WTH_K_FIRE].colC);
    if (GetPrivateProfileStringA(WTH_SECTION, "FireMidColor", "", buf,
                                 sizeof(buf), g_IniPath) > 0)
        Wth_ParseColor(buf, g_Kind[WTH_K_FIRE].colA);
    if (GetPrivateProfileStringA(WTH_SECTION, "FireEndColor", "", buf,
                                 sizeof(buf), g_IniPath) > 0)
        Wth_ParseColor(buf, g_Kind[WTH_K_FIRE].colB);
    if (GetPrivateProfileStringA(WTH_SECTION, "SmokeLightColor", "", buf,
                                 sizeof(buf), g_IniPath) > 0)
        Wth_ParseColor(buf, g_Cfg.smokeLight);
    if (GetPrivateProfileStringA(WTH_SECTION, "SmokeDarkColor", "", buf,
                                 sizeof(buf), g_IniPath) > 0)
        Wth_ParseColor(buf, g_Cfg.smokeDark);
    /* CloudStyle 写字或写序号都行; 上面按整数读过了, 这里再按名字试一次 */
    if (GetPrivateProfileStringA(WTH_SECTION, "CloudStyle", "", buf,
                                 sizeof(buf), g_IniPath) > 0)
    {
        int si = Wth_StyleParse(buf);

        if (si >= 0) g_Cfg.cloudStyle = si;
    }

    /* 单调色的天气 (雨/雪) 尾端色必须跟根部一致, 否则就变成"渐变色雨"了 */
    for (i = 0; i < 3; i++)
    {
        g_Kind[WTH_K_RAIN].colB[i] = g_Kind[WTH_K_RAIN].colA[i];
        g_Kind[WTH_K_SNOW].colB[i] = g_Kind[WTH_K_SNOW].colA[i];
    }

    /* 夹一遍, 免得手改配置时把数组/循环写飞 */
    if (g_Cfg.speedScale < 5) g_Cfg.speedScale = 5;
    if (g_Cfg.speedScale > 1000) g_Cfg.speedScale = 1000;
    if (g_Cfg.imageSpeed < 5) g_Cfg.imageSpeed = 5;
    if (g_Cfg.imageSpeed > 1000) g_Cfg.imageSpeed = 1000;
    if (g_Cfg.smokeR > WTH_SM_RMAX) g_Cfg.smokeR = WTH_SM_RMAX;
    if (g_Cfg.smokeR < 1) g_Cfg.smokeR = 1;
    if (g_Cfg.smokeLifeHi > 250) g_Cfg.smokeLifeHi = 250;
    if (g_Cfg.cloudOpacity < 0) g_Cfg.cloudOpacity = 0;
    if (g_Cfg.cloudOpacity > 255) g_Cfg.cloudOpacity = 255;
    if (g_Cfg.cloudScale < 32) g_Cfg.cloudScale = 32;
    if (g_Cfg.cloudScale > 4096) g_Cfg.cloudScale = 4096;
    if (g_Cfg.cloudStyle < 0 || g_Cfg.cloudStyle >= CLD_STYLE_N) g_Cfg.cloudStyle = 0;

    /* 每对 lo/hi 都必须 lo<=hi, 否则 Wth_Range 里 hi-lo+1 会变成 0 或负数;
     * size/wind/spread 在落笔里当循环上限与除数用, 越界会画到面外或除零。 */
#define WTH_FIX(lo, hi, minv) \
    do { if ((lo) < (minv)) (lo) = (minv); if ((hi) < (lo)) (hi) = (lo); } while (0)
    for (i = 0; i < WTH_K_N; i++)
    {
        WthKind* k = &g_Kind[i];

        if (k->count < 200) k->count = 200;         /* 除数 */
        if (k->countMin < 1) k->countMin = 1;
        if (k->countMax > WTH_PX_MAX) k->countMax = WTH_PX_MAX;
        WTH_FIX(k->countMin, k->countMax, 1);
        WTH_FIX(k->vLo, k->vHi, 1);
        WTH_FIX(k->aLo, k->aHi, 1);
        WTH_FIX(k->lenLo, k->lenHi, 0);
        if (k->lenHi > 200) k->lenHi = 200;
        if (k->size < 1) k->size = 1;
        if (k->size > 4) k->size = 4;               /* 宽度也是遍历上限 */
        if (k->wind < -300) k->wind = -300;
        if (k->wind > 300) k->wind = 300;
        if (k->spread < 0) k->spread = 0;
        if (k->spread > 200) k->spread = 200;
        if (k->sway < 0) k->sway = 0;
        if (k->sway > 4) k->sway = 4;
        if (k->bigRate < 0) k->bigRate = 0;
        if (k->splash < 0) k->splash = 0;
        if (k->splash > 8) k->splash = 8;
        if (k->splashRate < 0) k->splashRate = 0;
        if (k->splashRate > 2000) k->splashRate = 2000;
        if (k->fade < 0) k->fade = 0;
        if (k->fade > 90) k->fade = 90;
        if (k->tailSmoke < 0) k->tailSmoke = 0;
        if (k->tailSmoke > 255) k->tailSmoke = 255;
        if (k->plume < 0) k->plume = 0;
        if (k->plume > 64) k->plume = 64;
        if (k->smokeHi > 0)
        {
            WTH_FIX(k->smokeLo, k->smokeHi, 1);
            if (k->smokeHi > 250) k->smokeHi = 250;
        }
    }
    WTH_FIX(g_Cfg.smokeLifeLo, g_Cfg.smokeLifeHi, 2);
    WTH_FIX(g_Cfg.smokeALo, g_Cfg.smokeAHi, 1);
    WTH_FIX(g_Cfg.smokeGrowLo, g_Cfg.smokeGrowHi, 0);
#undef WTH_FIX
}

/* ---------------- 粒子档 ----------------
 * 雨 / 雪 / 火雨 是同一套粒子的三份参数: 一个池子、一套推进、一个落笔循环, 差别
 * 全在 WthKind 表里 (数量/速度/长度/颜色/头画法)。加一种天气 = 加一行参数。
 *
 * 粒子只在视图区里活动, 掉出底边就从顶上回来 (取模, 不是重新布点 —— 密度天然
 * 守恒, 也不用一次算几百个随机数)。坐标与速度都是 1/16 像素定点。
 * 火雨另带两套烟: 跟着火球走的尾迹烟 (画在拖尾里) 和会飘会散的烟团 (独立环形缓冲)。 */

typedef struct {
    int x, y;               /* 视图内坐标, 1/16 像素 */
    int vx, vy;             /* 每逻辑帧位移, 1/16 像素 */
    int sx;                 /* 拖尾斜率: 每上移 1 像素的 x 位移, 1/256 像素 */
    unsigned char len;      /* 拖尾长度 (像素) */
    unsigned char a0;       /* 头部 alpha */
    unsigned char sz;       /* 雪花: 大片 (臂长再 +1) */
    unsigned char st;       /* 冒烟倒计时 (逻辑帧) */
    unsigned char kind;     /* WTH_K_* */
} WthPx;

/* 烟团: 出生后一边上升一边变大变淡, 寿命到了就地回收。
 * 环形缓冲直接覆盖最老的那团, 不做搬移/压实。 */
typedef struct {
    int x, y;
    int vx, vy;
    unsigned char age, life;
    unsigned char r0, grow;   /* 起始半径 / 每逻辑帧半径增量, 1/16 像素 */
    unsigned char a0;
} WthSmoke;

/* 溅射水滴/火星: 落地时崩出来的小颗粒, 走抛物线, 有自己的寿命与颜色来源 (kind)。 */
typedef struct {
    int x, y;
    int vx, vy;
    unsigned char age, life, a0, kind;
} WthDrop;

static WthPx g_Px[WTH_PX_MAX];     /* 三种天气共用; 按 kind 顺序排 (见 Wth_PxInit) */
static int g_PxN;                   /* 有效粒子总数 */
static int g_PxCnt[WTH_K_N];        /* 各 kind 的粒子数 */
static WthSmoke g_Smoke[WTH_SMOKE_MAX];
static WthDrop g_Drop[WTH_DROP_MAX];
static unsigned g_SmokePut, g_DropPut;
/* 半径 r 的圆形每行半宽: g_SmHalf[r][|dy|], 建一次 (整数, 不引 libm)。 */
static unsigned char g_SmHalf[WTH_SM_RMAX + 1][WTH_SM_RMAX + 1];
static int g_SmKernel;
static int g_PxW, g_PxH;        /* 上次布点用的视图尺寸 */
static int g_PxOk = 0;          /* 0 = 下一帧重新布点 */
static unsigned g_Rnd = 0x9E3779B9u;
static int g_Tick;              /* 逻辑帧累计 (摇摆相位 / 云影漂移) */

/* 相机 / 逻辑帧 */
static unsigned g_LastNo = 0;
static int g_HaveNo = 0;
static int g_CamX, g_CamY, g_CamHave = 0;

/* ---------------- 初始化 ---------------- */

static int Wth_StrCaseCmp(const char* a, const char* b);

/* 长期设置: 云影开关与画法。其余开关只在本次运行内有效, 所以不读。
 * 由 ESC 菜单写入 (section "Weather" / key "CloudEnable" / "DrawMode")。 */
static void Wth_LoadState(void)
{
    char buf[16];

    g_CloudOn = GetPrivateProfileIntA(WTH_SECTION, "CloudEnable", 0, g_IniPath)
                ? 1 : 0;
    if (GetPrivateProfileStringA(WTH_SECTION, "DrawMode", "", buf,
                                 sizeof(buf), g_IniPath) > 0)
        g_Mode = (Wth_StrCaseCmp(buf, "particle") == 0)
                 ? WEATHER_DRAW_PARTICLE : WEATHER_DRAW_IMAGE;
}

static void Wth_Init(void)
{
    if (g_Init) return;
    g_Init = 1;
    BuildPaths();
    GtEnsureLog();
    Wth_Defaults();
    Wth_ReadCfg();      /* 参数只在装载时读一次: 改 ini 需重进游戏 */
    Wth_LoadState();
}

int Weather_DrawMode(void)
{
    Wth_Init();
    return g_Mode;
}

void Weather_SetDrawMode(int mode)
{
    Wth_Init();
    if (mode < 0 || mode >= WEATHER_DRAW_N) return;
    if (mode == g_Mode) return;
    g_Mode = mode;
    g_PxOk = 0;     /* 换画法重新布点, 免得粒子从上一档的位置接着跑 */
}

/* 菜单会在第一帧渲染之前就读一遍这些开关做快照, 所以 getter 也要保证已初始化
 * (否则会读到全 0, 然后在"取消"时把 ini 里的长期设置盖成关)。 */
int Weather_Rain(void)  { Wth_Init(); return g_On[WTH_K_RAIN]; }
int Weather_Snow(void)  { Wth_Init(); return g_On[WTH_K_SNOW]; }
int Weather_Fire(void)  { Wth_Init(); return g_On[WTH_K_FIRE]; }
int Weather_Cloud(void) { Wth_Init(); return g_CloudOn; }

static void Wth_SetKind(int kind, int on)
{
    on = on ? 1 : 0;
    if (on == g_On[kind]) return;
    g_On[kind] = on;
    g_PxOk = 0;
}

void Weather_SetRain(int on) { Wth_SetKind(WTH_K_RAIN, on); }
void Weather_SetSnow(int on) { Wth_SetKind(WTH_K_SNOW, on); }
void Weather_SetFire(int on) { Wth_SetKind(WTH_K_FIRE, on); }

void Weather_SetCloud(int on)
{
    Wth_Init();
    g_CloudOn = on ? 1 : 0;
}

/* 云影风格: 预设表 kCldStyle 的序号。改动会在下一帧烘焙时生效 (换风格必须重烘)。 */
int Weather_CloudStyle(void)
{
    Wth_Init();
    return g_Cfg.cloudStyle;
}

int Weather_CloudStyleCount(void) { return CLD_STYLE_N; }

void Weather_SetCloudStyle(int idx)
{
    Wth_Init();
    if (idx < 0 || idx >= CLD_STYLE_N) return;
    g_Cfg.cloudStyle = idx;
}

/* ---------------- 资源获取 ---------------- */

/* 不引 CRT: 本 DLL 以 -nostdlib 链接, _stricmp 不可用。 */
static int Wth_StrCaseCmp(const char* a, const char* b)
{
    while (*a && *b)
    {
        int ca = (unsigned char)*a;
        int cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* 在 AnimTypeClass 表里按 ID 找类型。表是 DynamicVectorClass<AnimTypeClass*>,
 * 数据指针在 +0x04, 个数在 +0x10 (与 ADDR_ANIMTYPE_COUNT 同址, 各写一遍防呆)。 */
static void* Wth_FindAnimType(const char* id)
{
    unsigned char* vec = (unsigned char*)ADDR_ANIMTYPE_ARR;
    void** items = *(void***)(vec + 0x04);
    int count = *(int*)(vec + 0x10);
    int i;

    if (!id) return NULL;
    if (!items || !IsPlausiblePtr(items)) return NULL;
    if (count <= 0 || count > 4096) return NULL;
    for (i = 0; i < count; i++)
    {
        void* t = items[i];
        if (!t || !IsPlausiblePtr(t)) continue;
        if (Wth_StrCaseCmp((const char*)((char*)t + OFF_ANIMTYPE_ID), id) == 0)
            return t;
    }
    return NULL;
}

/* ObjectTypeClass 虚表槽 39 = LoadImage: Image 非空直接返回, 否则按
 * ImageFile/ID 懒加载。 */
typedef void* (__thiscall *WthLoadImageFn)(void* type);
#define WTH_VT_LOAD_IMAGE 39

static void* Wth_GetShp(const char* id, void** typeOut)
{
    void* type;
    void* img;
    void** vt;
    WthShpHead* head;

    type = Wth_FindAnimType(id);
    if (!type) return NULL;
    vt = *(void***)type;
    if (!vt || !IsPlausiblePtr(vt)) return NULL;
    if (!vt[WTH_VT_LOAD_IMAGE]) return NULL;
    img = ((WthLoadImageFn)vt[WTH_VT_LOAD_IMAGE])(type);
    if (!img || !IsPlausiblePtr(img)) return NULL;
    head = (WthShpHead*)img;
    if (head->Frames <= 0 || head->Frames > 4096) return NULL;
    if (head->Width < 16 || head->Height < 16) return NULL;
    if (head->Width > 4096 || head->Height > 4096) return NULL;
    *typeOut = type;
    return img;
}

static void Wth_LogLayer(WthLayer* l, int step)
{
    char msg[160];
    unsigned p = 0;

    p = AppendCStr(msg, p, "weather: ", sizeof(msg));
    p = AppendCStr(msg, p, l->id, sizeof(msg));
    p = AppendCStr(msg, p, " frames=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), l->frames);
    p = AppendCStr(msg, p, " loop=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), l->loopStart);
    p = AppendCh(msg, p, '-', sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), l->loopEnd - 1);
    p = AppendCStr(msg, p, " size=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), l->w);
    p = AppendCh(msg, p, 'x', sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), l->h);
    p = AppendCStr(msg, p, " step=", sizeof(msg));
    p = AppendInt(msg, p, sizeof(msg), step);
    LogLine(msg);
}

/* 取 SHP + 播放参数。只做一次; 失败也只是本条图层不出图。 */
static int Wth_Ensure(WthLayer* l)
{
    WthShpHead* head;
    int ls, le;

    if (l->shp) return 1;
    if (l->fail) return 0;
    l->shp = Wth_GetShp(l->id, &l->type);
    if (!l->shp) { l->fail = 1; LogLine("weather: shp load failed"); return 0; }

    head = (WthShpHead*)l->shp;
    l->frames = head->Frames;
    l->w = head->Width;
    l->h = head->Height;

    /* 循环段取类型声明值 (RAIN/SNOW: 0..18, 而 SHP 有 31/20 帧)。引擎的
     * LoopEnd 是开区间 (0x4246FC 用 Value>=End 判循环), 可见末帧 = LoopEnd-1。 */
    ls = *(int*)((char*)l->type + OFF_ANIMTYPE_LOOPSTART);
    le = *(int*)((char*)l->type + OFF_ANIMTYPE_LOOPEND);
    if (ls < 0 || ls >= l->frames) ls = 0;
    if (le <= ls || le > l->frames) le = l->frames;
    l->loopStart = ls;
    l->loopEnd = le;
    l->frame = ls;

    /* 播放速度: +0x2B0 是引擎**已经换算过**的间隔 —— 活进程实测 (animdump):
     * RAIN/SNOW 这里读出来是 2, 而 artmo.ini 写的是 Rate=400, 正好等于
     * 900/400 截断 (= ModEnc 的"900/Rate 个 tick 一帧", tick 即逻辑帧)。
     * 所以直接把它当"每动画帧要过几个逻辑帧", 不能再拿 art 的原始 Rate 算一遍
     * (算成 900/2 = 450 就冻住了)。 */
    l->step = *(int*)((char*)l->type + OFF_ANIMTYPE_RATE);
    if (l->step < 1) l->step = 1;
    if (l->step > 900) l->step = 900;

    Wth_LogLayer(l, l->step);
    return 1;
}

/* ---------------- 每帧状态 ---------------- */

/* 这帧渲染跨过了几个逻辑帧 (渲染与逻辑不一定 1:1)。跳号 (读档/重开) 不补帧。 */
static int Wth_LogicDelta(void)
{
    unsigned now = *(unsigned*)(size_t)ADDR_FRAME;
    int d;

    if (!g_HaveNo) { g_HaveNo = 1; g_LastNo = now; return 0; }
    d = (int)(now - g_LastNo);
    g_LastNo = now;
    if (d < 0 || d > 240) return 0;
    return d;
}

/* 相机像素位移 (TACX/TACY 是视口原点)。粒子与图像平铺都用它做锚点。
 * 一帧跳太多 = 切镜头/读档, 不搬 (否则整屏雨会瞬移)。 */
static void Wth_CamStep(int* dx, int* dy)
{
    void* tac = *(void**)ADDR_TACTICAL;
    int tx, ty, ddx, ddy;

    *dx = 0;
    *dy = 0;
    if (!tac || !IsPlausiblePtr(tac)) return;
    tx = *(int*)((char*)tac + OFF_TAC_TACX);
    ty = *(int*)((char*)tac + OFF_TAC_TACY);
    if (!g_CamHave) { g_CamHave = 1; g_CamX = tx; g_CamY = ty; return; }
    ddx = tx - g_CamX;
    ddy = ty - g_CamY;
    g_CamX = tx;
    g_CamY = ty;
    if (ddx < -64 || ddx > 64 || ddy < -64 || ddy > 64) return;
    *dx = ddx;
    *dy = ddy;
}

/* 图像档推进。tick 的单位是 1/100 逻辑帧, 这样 ImageSpeed 不是 100 时也不丢精度
 * (直接用逻辑帧整数会在 50% 速度下每帧都截断成 0)。 */
static void Wth_Advance(WthLayer* l, int dFrames)
{
    int need, n, span;

    if (l->step <= 0 || dFrames <= 0) return;
    need = l->step * 100;
    l->tick += dFrames * g_Cfg.imageSpeed;
    if (l->tick < need) return;
    n = l->tick / need;
    l->tick -= n * need;
    l->frame += n;
    span = l->loopEnd - l->loopStart;
    if (span <= 0) { l->frame = l->loopStart; return; }
    if (l->frame >= l->loopEnd)
        l->frame = l->loopStart + (l->frame - l->loopStart) % span;
    l->adv += (unsigned)n;
}

/* ---------------- 粒子: 布点与推进 ---------------- */

static unsigned Wth_Rand(void)
{
    g_Rnd ^= g_Rnd << 13;
    g_Rnd ^= g_Rnd >> 17;
    g_Rnd ^= g_Rnd << 5;
    return g_Rnd;
}

static int Wth_Range(int lo, int hi)
{
    return lo + (int)(Wth_Rand() % (unsigned)(hi - lo + 1));
}

/* 在 (x, y) 冒一团烟。环形缓冲, 满了直接盖掉最老的 —— 烟是纯装饰, 不需要
 * 精确的对象生命周期。起始半径很小, 主要靠 grow 长起来 (见 Wth_PxSmoke)。 */
static void Wth_SmokeAt(int x, int y)
{
    WthSmoke* s = &g_Smoke[g_SmokePut % WTH_SMOKE_MAX];

    g_SmokePut++;
    s->x = x;
    s->y = y;
    s->vx = Wth_Range(-10, 10);
    s->vy = -Wth_Range(3, 12);      /* 负 = 向上飘 */
    s->r0 = (unsigned char)Wth_Range(2, 7);
    s->grow = (unsigned char)Wth_Range(g_Cfg.smokeGrowLo, g_Cfg.smokeGrowHi);
    s->life = (unsigned char)Wth_Range(g_Cfg.smokeLifeLo, g_Cfg.smokeLifeHi);
    s->age = 0;
    s->a0 = (unsigned char)Wth_Range(g_Cfg.smokeALo, g_Cfg.smokeAHi);
}

/* 配置里的速度是"60 帧基准的像素/秒", 内部单位是 1/16 像素/逻辑帧:
 * px/s ÷ 60 × 16 = px/s × 4/15。SpeedScale 是统一的百分比倍率。 */
static int Wth_V(int pxPerSec)
{
    int v = pxPerSec * 4 * g_Cfg.speedScale / 1500;

    if (v < 1) v = 1;
    if (v > 8000) v = 8000;
    return v;
}

/* 一条粒子的初始参数, 全部来自 g_Kind[kind] (单位见 Wth_V)。 */
static void Wth_Spawn(WthPx* p, int kind, int w, int h)
{
    const WthKind* k = &g_Kind[kind];

    p->kind = (unsigned char)kind;
    p->sz = 0;
    p->st = 0;
    p->vy = Wth_Range(Wth_V(k->vLo), Wth_V(k->vHi));
    /* 风向优先: 有 wind 就按下落速度的百分比给水平速度 (拖尾方向由 sx 自动跟上),
     * 没有就按 spread 随机抖一点。 */
    p->vx = k->wind ? p->vy * k->wind / 100
                    : p->vy * Wth_Range(-k->spread, k->spread) / 100;
    p->len = (unsigned char)(k->lenHi > 0 ? Wth_Range(k->lenLo, k->lenHi) : 0);
    p->a0 = (unsigned char)Wth_Range(k->aLo, k->aHi);
    if (k->bigRate > 1 && (int)(Wth_Rand() % (unsigned)k->bigRate) == 0)
        p->sz = 1;
    if (k->smokeHi > 0)
        p->st = (unsigned char)Wth_Range(1, k->smokeHi);
    p->sx = (p->vy != 0) ? (p->vx << 8) / p->vy : 0;
    p->x = Wth_Range(0, w - 1) << WTH_FX;
    p->y = Wth_Range(0, h + (int)p->len - 1) << WTH_FX;
}

/* 半径 r 的圆, 每行半宽是多少 (整数开方, 不引 libm)。 */
static void Wth_BuildKernel(void)
{
    int r, dy, hw;

    if (g_SmKernel) return;
    g_SmKernel = 1;
    for (r = 0; r <= WTH_SM_RMAX; r++)
        for (dy = 0; dy <= WTH_SM_RMAX; dy++)
        {
            hw = 0;
            if (dy <= r)
                while (hw < r && (hw + 1) * (hw + 1) + dy * dy <= r * r) hw++;
            g_SmHalf[r][dy] = (unsigned char)hw;
        }
}

/* 数量按视图面积给, 视图尺寸变了或换过开关就重新布点。
 * 三种天气共用一个池子, 按 kind 顺序依次分配, 累计不超过 WTH_PX_MAX。 */
static void Wth_PxInit(int w, int h)
{
    int area, i, kind, at = 0;

    if (g_PxOk && w == g_PxW && h == g_PxH) return;
    g_PxW = w;
    g_PxH = h;
    g_PxOk = 1;
    Wth_BuildKernel();
    area = w * h;

    for (kind = 0; kind < WTH_K_N; kind++)
    {
        const WthKind* k = &g_Kind[kind];
        int n = area / k->count;

        if (n < k->countMin) n = k->countMin;
        if (n > k->countMax) n = k->countMax;
        if (n > WTH_PX_MAX - at) n = WTH_PX_MAX - at;   /* 池子满了就少给 */
        if (n < 0) n = 0;
        g_PxCnt[kind] = n;
        for (i = 0; i < n; i++) Wth_Spawn(&g_Px[at + i], kind, w, h);
        at += n;
    }
    g_PxN = at;

    for (i = 0; i < WTH_SMOKE_MAX; i++) g_Smoke[i].life = 0;
    for (i = 0; i < WTH_DROP_MAX; i++) g_Drop[i].life = 0;
    g_SmokePut = 0;
    g_DropPut = 0;

    /* 布点只在开关/尺寸变化时发生, 顺手留一行现场 (调密度用)。 */
    {
        char msg[128];
        unsigned p = 0;

        p = AppendCStr(msg, p, "weather: particles view=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), w);
        p = AppendCh(msg, p, 'x', sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), h);
        p = AppendCStr(msg, p, " rain=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), g_PxCnt[WTH_K_RAIN]);
        p = AppendCStr(msg, p, " snow=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), g_PxCnt[WTH_K_SNOW]);
        p = AppendCStr(msg, p, " fire=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), g_PxCnt[WTH_K_FIRE]);
        LogLine(msg);
    }
}

/* 落地溅射: 在指定点崩出几颗小颗粒。雨/雪是水花 (近水平飞), 火是火星 (向上崩)
 * 并留一团烟。颗粒有自己的重力与寿命, 与降水粒子的回绕无关。
 * 落点是**画面内随机一点**, 不查地形高度 —— 取地块 Z 要穿 TacticalClass 的世界
 * 坐标换算, 代价和风险都不值当; 溅射本来只是氛围。 */
static void Wth_Splash(int x, int y, int kind)
{
    const WthKind* k = &g_Kind[kind];
    int fire = (kind == WTH_K_FIRE);
    int n = k->splash;
    int i;

    for (i = 0; i < n; i++)
    {
        WthDrop* d = &g_Drop[g_DropPut % WTH_DROP_MAX];
        int life;

        g_DropPut++;
        d->x = x + Wth_Range(-6, 6);
        d->y = y;
        if (i & 1)      /* 一半是横向甩出去的水花, 一半是向上崩的 */
        {
            d->vx = Wth_Range(14, 52) * ((i & 2) ? 1 : -1);
            d->vy = -Wth_Range(10, 40);
        }
        else
        {
            d->vx = Wth_Range(-18, 18) * (fire ? 3 : 2) / 3;
            d->vy = -Wth_Range(40, fire ? 130 : 80);
        }
        life = fire ? Wth_Range(8, 15) : Wth_Range(6, 12);
        if (life < 3) life = 3;
        d->age = 0;
        d->life = (unsigned char)life;
        d->a0 = (unsigned char)(fire ? Wth_Range(170, 240) : Wth_Range(90, 170));
        d->kind = (unsigned char)kind;
    }
    if (fire)
        Wth_SmokeAt(x, y - (2 << WTH_FX));      /* 火球砸地那一下也带一团烟 */
}

/* 落点节拍: 画面内**任意**一点, 每秒 rate 次。rate 默认由粒子数量推出来 (见调用
 * 处), 雨越密落地越频繁; ini 里给了 SplashRate 就用它 (0 = 自动)。
 *
 * 落点铺满整个视图, 不按"下部 3/4"筛: 这是正交俯视, 屏幕上方同样是地面, 只在下半
 * 屏落会像"半空里炸开"。也正因为铺满, 这些水花本身就成了不挂在雨丝上的**纯溅射**
 * (屏幕上一部分雨丝看不见是正常的, 但地面处处都在溅), 上密下疏的错觉自然消失。
 * 不查地形高度 —— 取地块 Z 要穿 TacticalClass 的世界坐标换算, 代价和风险都不值当。 */
static int g_LandAcc[WTH_K_N];   /* 1/256 次 */

static void Wth_LandTick(int kind, int d, int w, int h)
{
    const WthKind* k = &g_Kind[kind];
    int rate = k->splashRate > 0 ? k->splashRate : g_PxCnt[kind] / 5;
    int x, y;

    if (rate <= 0 || d <= 0 || w < 32 || h < 32) return;
    g_LandAcc[kind] += rate * 256 * d / 60;
    /* 卡一下之后别一次炸一串 */
    if (g_LandAcc[kind] > 256 * 8) g_LandAcc[kind] = 256 * 8;
    while (g_LandAcc[kind] >= 256)
    {
        g_LandAcc[kind] -= 256;
        x = Wth_Range(8, w - 8);
        y = Wth_Range(8, h - 8);
        Wth_Splash(x << WTH_FX, y << WTH_FX, kind);
    }
}

/* 推进 + 按视图区取模回绕。sdx/sdy 是相机这帧的像素位移 (粒子跟着世界走)。
 * mask 决定这一帧哪些 kind 参与 (图像档时雨/雪不推进, 免得切回来时位置乱了)。 */
static void Wth_PxStep(int d, int w, int h, int sdx, int sdy, unsigned mask)
{
    int i, wx = w << WTH_FX;

    if (d <= 0) return;
    for (i = 0; i < g_PxN; i++)
    {
        WthPx* p = &g_Px[i];
        const WthKind* k = &g_Kind[p->kind];
        int band;

        if (!(mask & (1u << p->kind))) continue;
        band = (h + (int)p->len) << WTH_FX;
        p->x += p->vx * d - (sdx << WTH_FX);
        p->y += p->vy * d - (sdy << WTH_FX);
        if (k->sway)
        {
            /* 左右摇摆: 三角形波 (不用 sin, 也不引 libm)。摆幅约 0.3px/帧,
             * 雪本身落得慢, 摆太猛会像左右横跳。 */
            int s = (g_Tick + (int)p->a0 * 5) & 127;
            if (s > 64) s = 128 - s;
            p->x += (s - 32) * k->sway * d / 5;
        }
        if (k->smokeHi > 0)
        {
            /* 拖尾持续冒烟: 烟从尾端离手, 所以看着是火球后面追着一串烟。 */
            int st = (int)p->st - d;

            if (st <= 0)
            {
                Wth_SmokeAt(p->x, p->y - (((int)p->len + 8) << WTH_FX));
                st = Wth_Range(k->smokeLo, k->smokeHi);
            }
            p->st = (unsigned char)st;
        }
        p->x %= wx;
        if (p->x < 0) p->x += wx;
        p->y %= band;
        if (p->y < 0) p->y += band;
    }
}

/* 烟团推进: 按逻辑帧变老/上升/变大, 老了就地回收 (不搬数组)。 */
static void Wth_SmokeStep(int d, int sdx, int sdy)
{
    int i;

    if (d <= 0) return;
    for (i = 0; i < WTH_SMOKE_MAX; i++)
    {
        WthSmoke* s = &g_Smoke[i];

        if (!s->life) continue;
        s->age = (unsigned char)(s->age + d);
        if (s->age >= s->life) { s->life = 0; continue; }
        s->x += s->vx * d - (sdx << WTH_FX);
        s->y += s->vy * d - (sdy << WTH_FX);
    }
}

/* 溅射颗粒推进: 抛物线, 落到视图底边以下就回收。 */
static void Wth_DropStep(int d, int sdx, int sdy, int h)
{
    int i, gy;

    if (d <= 0) return;
    gy = h << WTH_FX;
    for (i = 0; i < WTH_DROP_MAX; i++)
    {
        WthDrop* p = &g_Drop[i];

        if (!p->life) continue;
        p->age = (unsigned char)(p->age + d);
        if (p->age >= p->life || p->y >= gy + (6 << WTH_FX))
        {
            p->life = 0;
            continue;
        }
        p->vy += WTH_DROP_GRAV * d;
        p->x += p->vx * d - (sdx << WTH_FX);
        p->y += p->vy * d - (sdy << WTH_FX);
    }
}

/* ---------------- 粒子: 落笔 ---------------- */

/* 加法混合 (火雨): dst 各通道 + fg*a, 饱和。暗底上才亮得起来。 */
static unsigned short Wth_Add(unsigned short dst, unsigned r, unsigned g,
                              unsigned b, unsigned a)
{
    unsigned dr, dg, db;

    if (a == 0) return dst;
    if (!g_ExpReady) GtBuildExpand();
    if (g_GBits == 6)
    {
        dr = g_ExpR[(dst >> 11) & 0x1Fu];
        dg = g_ExpG[(dst >> 5) & 0x3Fu];
        db = g_ExpB[dst & 0x1Fu];
    }
    else
    {
        dr = g_ExpR[(dst >> 10) & 0x1Fu];
        dg = g_ExpG[(dst >> 5) & 0x1Fu];
        db = g_ExpB[dst & 0x1Fu];
    }
    dr += r * a / 255u; if (dr > 255u) dr = 255u;
    dg += g * a / 255u; if (dg > 255u) dg = 255u;
    db += b * a / 255u; if (db > 255u) db = 255u;
    if (g_GBits == 6)
        return (unsigned short)((((dr * 31u + 127u) / 255u) << 11) |
                                (((dg * 63u + 127u) / 255u) << 5) |
                                 ((db * 31u + 127u) / 255u));
    return (unsigned short)((((dr * 31u + 127u) / 255u) << 10) |
                            (((dg * 31u + 127u) / 255u) << 5) |
                             ((db * 31u + 127u) / 255u));
}

static __inline unsigned short* Wth_At(unsigned char* base, int pitch,
                                       int ox, int oy, int px, int py)
{
    return (unsigned short*)(base + (size_t)(oy + py) * (size_t)pitch) + ox + px;
}

/* 带裁剪的混合/加法落笔 (单像素)。裁剪放在这里, 各个画法就不用自己判边界。 */
static __inline void Wth_BlendAt(unsigned char* base, int pitch, int ox, int oy,
                                 int w, int h, int px, int py,
                                 unsigned r, unsigned g, unsigned b, unsigned a)
{
    unsigned short* d;

    if (a == 0 || px < 0 || px >= w || py < 0 || py >= h) return;
    d = Wth_At(base, pitch, ox, oy, px, py);
    *d = Blend16(*d, r, g, b, a);
}

static __inline void Wth_AddAt(unsigned char* base, int pitch, int ox, int oy,
                               int w, int h, int px, int py,
                               unsigned r, unsigned g, unsigned b, unsigned a)
{
    unsigned short* d;

    if (a == 0 || px < 0 || px >= w || py < 0 || py >= h) return;
    d = Wth_At(base, pitch, ox, oy, px, py);
    *d = Wth_Add(*d, r, g, b, a);
}

/* 取目标像素的绿色通道当亮度 (烟要按底下的明暗选深浅)。 */
static int Wth_LumaAt(unsigned char* base, int pitch, int ox, int oy,
                      int w, int h, int px, int py)
{
    unsigned short v;

    if (px < 0 || px >= w || py < 0 || py >= h) return 0;
    if (!g_ExpReady) GtBuildExpand();
    v = *Wth_At(base, pitch, ox, oy, px, py);
    return (g_GBits == 6) ? (int)g_ExpG[(v >> 5) & 0x3Fu]
                          : (int)g_ExpG[(v >> 5) & 0x1Fu];
}

/* 一行里把 [xa, xb] 铺满 (含两端)。拖尾每行之间 x 会跳, 不补中间就是断线。 */
static __inline void Wth_HFill(unsigned char* base, int pitch, int ox, int oy,
                               int w, int h, int xa, int xb, int y,
                               int r, int g, int b, unsigned a)
{
    int t;

    if (y < 0 || y >= h) return;
    if (xa > xb) { t = xa; xa = xb; xb = t; }
    for (; xa <= xb; xa++)
        Wth_BlendAt(base, pitch, ox, oy, w, h, xa, y,
                    (unsigned)r, (unsigned)g, (unsigned)b, a);
}

static __inline int Wth_Lerp(int a, int b, int t)     /* t: 0..256 */
{
    return a + ((b - a) * t >> 8);
}

/* 拖尾: 从头部往斜上方一条线, 越远越淡, 颜色由 colA 渐变到 colB。
 *
 * 三件事一定要守住:
 *   1) 每行 x 是按拖尾斜率跳的, 跳超过 1 像素中间就空一格 —— 用 Wth_HFill 补中间,
 *      否则看着断成一节一节;
 *   2) 烟柱每行的浓淡不能乘随机数 (行与行差 75% 就断), 用慢周期三角波;
 *   3) 尾迹后段按 tailSmoke 并入烟色, 白尾是**渐变**成烟, 不是硬切换。 */
static void Wth_PxTrail(unsigned char* base, int pitch, int ox, int oy,
                        int w, int h, const WthPx* p, const WthKind* k)
{
    int hx = p->x >> WTH_FX;
    int hy = p->y >> WTH_FX;
    int len = p->len ? p->len : 1;
    int wide = k->size;
    int lead = (wide - 1) >> 1;         /* 让线条以头部为轴 (宽 3 时左移 1) */
    unsigned a0 = p->a0;
    const int* sc = NULL;
    int xoff = 0, prevX = hx, c, i;

    if (hy < 0 || hy - len >= h) return;
    if (hx < -8 || hx >= w + 8) return;

    if (k->tailSmoke > 0 || k->plume > 0)
    {
        /* 烟色按尾端上方那片地的明暗选: 亮底用暗烟, 暗底用亮烟 */
        int lum = Wth_LumaAt(base, pitch, ox, oy, w, h, hx, hy - len - 4);

        sc = (lum > 110) ? g_Cfg.smokeDark : g_Cfg.smokeLight;
    }

    for (i = 0; i < len; i++)
    {
        int t = i * 256 / len;                  /* 0 = 头部, 256 = 尾端 */
        int px = hx + (xoff >> 8) - lead;
        int py = hy - i;
        unsigned a = (unsigned)((int)a0 * (100 * len - (100 - k->fade) * i)
                                / (100 * len));
        int cr, cg, cb;

        xoff -= p->sx;
        if (a == 0) break;
        cr = Wth_Lerp(k->colA[0], k->colB[0], t);
        cg = Wth_Lerp(k->colA[1], k->colB[1], t);
        cb = Wth_Lerp(k->colA[2], k->colB[2], t);
        if (sc && t > 168)
        {
            int s2 = (t - 168) * 256 / 88 * k->tailSmoke / 256;

            cr = Wth_Lerp(cr, sc[0], s2);
            cg = Wth_Lerp(cg, sc[1], s2);
            cb = Wth_Lerp(cb, sc[2], s2);
        }
        if (px != prevX)
            Wth_HFill(base, pitch, ox, oy, w, h, prevX, px, py, cr, cg, cb, a * 2 / 3);
        prevX = px;
        for (c = 0; c < wide; c++)
            Wth_BlendAt(base, pitch, ox, oy, w, h, px + c, py, cr, cg, cb,
                        c ? a * (unsigned)(wide - c) / (unsigned)wide : a);
    }

    /* 烟柱: 接着拖尾末端继续往上, 越远越宽越淡, 末端淡到 0 —— 没有"突然到头"
     * 的截断。摆动与浓淡都用慢周期, 一行一行是连续的。 */
    if (k->plume <= 0 || !sc) return;
    for (i = 0; i < k->plume; i++)
    {
        int px = hx + (xoff >> 8);
        int py = hy - len - i;
        int t = i * 256 / k->plume;
        int hw = (t > 170) ? 2 : ((t > 60) ? 1 : 0);
        int ph = (i * 2 + (int)p->len * 3) & 31;
        int pw, wob;
        unsigned a;

        xoff -= p->sx;
        if (ph > 16) ph = 32 - ph;              /* 0..16 三角波, 每 32 行一回 */
        wob = (ph - 8) / 6;                     /* -1..1, 变化平滑 */
        a = (unsigned)((int)(a0 / 4) * (256 - t) / 256);
        a = a * (unsigned)(176 + ph * 5) / 256u;
        if (a == 0) continue;
        px += wob;
        for (pw = -hw; pw <= hw; pw++)
        {
            int ax = pw < 0 ? -pw : pw;
            unsigned da = (pw == 0) ? a
                         : a * (unsigned)(hw + 1 - ax) / (unsigned)(hw + 1);

            Wth_BlendAt(base, pitch, ox, oy, w, h, px + pw, py,
                        sc[0], sc[1], sc[2], da);
        }
    }
}

/* 头部: 加法光斑 (火球), 中心白热、外圈过橙到暗红。**必须在拖尾之后画** —— 它
 * 走加法混, 先画会被拖尾的普通混合盖成橙色, 白热芯就没了。 */
static void Wth_PxGlow(unsigned char* base, int pitch, int ox, int oy,
                       int w, int h, const WthPx* p, const WthKind* k)
{
    int hx = p->x >> WTH_FX;
    int hy = p->y >> WTH_FX;
    int sz = k->size;
    unsigned a0 = p->a0;
    int dx, dy;

    if (hx < -8 || hx >= w + 8 || hy < -8 || hy >= h + 8) return;
    for (dy = -sz; dy <= sz; dy++)
        for (dx = -sz; dx <= sz; dx++)
        {
            int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

            if (d > sz + 1) continue;
            if (d == 0)
                Wth_AddAt(base, pitch, ox, oy, w, h, hx, hy,
                          k->colC[0], k->colC[1], k->colC[2], a0);
            else if (d <= 2)
                Wth_AddAt(base, pitch, ox, oy, w, h, hx + dx, hy + dy,
                          k->colA[0], k->colA[1], k->colA[2],
                          a0 * (d == 1 ? 3u : 2u) / 4u);
            else
                Wth_AddAt(base, pitch, ox, oy, w, h, hx + dx, hy + dy,
                          k->colB[0], k->colB[1], k->colB[2], a0 / 4u);
        }
}

/* 雪花: 十字亮点 + 下方一像素冷灰影。纯亮色在雪原/浅色地形上会糊成一片,
 * 补一像素深色边 —— 亮底上看得见暗边, 暗底上看得见亮点。
 * 臂长由 size 给, 大片 (sz) 再 +1。 */
static void Wth_PxFlake(unsigned char* base, int pitch, int ox, int oy,
                        int w, int h, const WthPx* p, const WthKind* k)
{
    const int cr = k->colA[0], cg = k->colA[1], cb = k->colA[2];
    /* 四邻用比底色淡一点的同色系, 对角再淡; 下方那像素刻意压暗当影 */
    const int nr = (cr * 3 + 200) / 4, ng = (cg * 3 + 200) / 4, nb = (cb * 3 + 200) / 4;
    int hx = p->x >> WTH_FX;
    int hy = p->y >> WTH_FX;
    unsigned a = p->a0;
    int arm = k->size + (p->sz ? 1 : 0);
    int d;

    if (hx < -4 || hx >= w + 4 || hy < -4 || hy >= h + 4) return;
    Wth_BlendAt(base, pitch, ox, oy, w, h, hx, hy, cr, cg, cb, a);
    for (d = 1; d <= arm; d++)
    {
        unsigned da = a * 4u / (unsigned)(4 + d * 4);

        Wth_BlendAt(base, pitch, ox, oy, w, h, hx - d, hy, nr, ng, nb, da);
        Wth_BlendAt(base, pitch, ox, oy, w, h, hx + d, hy, nr, ng, nb, da);
        Wth_BlendAt(base, pitch, ox, oy, w, h, hx, hy - d, nr, ng, nb, da);
        Wth_BlendAt(base, pitch, ox, oy, w, h, hx, hy + d, nr / 2, ng / 2, nb / 2, da);
        if (d < arm)
        {
            Wth_BlendAt(base, pitch, ox, oy, w, h, hx - d, hy - d, nr, ng, nb, a / 8u);
            Wth_BlendAt(base, pitch, ox, oy, w, h, hx + d, hy - d, nr, ng, nb, a / 8u);
            Wth_BlendAt(base, pitch, ox, oy, w, h, hx - d, hy + d, nr, ng, nb, a / 8u);
            Wth_BlendAt(base, pitch, ox, oy, w, h, hx + d, hy + d, nr, ng, nb, a / 8u);
        }
    }
}

/* 一个落笔循环管三种天气: 拖尾共用, 头按 WTH_HEAD_* 分。
 * mask 决定这一帧画哪些 kind (图像档时雨/雪交给引擎的 SHP, 不走这里)。 */
static void Wth_PxDraw(unsigned char* base, int pitch, int ox, int oy,
                       int w, int h, unsigned mask)
{
    int i;

    for (i = 0; i < g_PxN; i++)
    {
        const WthPx* p = &g_Px[i];
        const WthKind* k = &g_Kind[p->kind];

        if (!(mask & (1u << p->kind))) continue;
        if (k->head == WTH_HEAD_FLAKE)
        {
            Wth_PxFlake(base, pitch, ox, oy, w, h, p, k);
            continue;
        }
        Wth_PxTrail(base, pitch, ox, oy, w, h, p, k);
        if (k->head == WTH_HEAD_GLOW)
            Wth_PxGlow(base, pitch, ox, oy, w, h, p, k);
    }
}

/* 烟团: 按行铺横条 (圆的一半宽查表), 越靠中心 alpha 越高、越靠边缘越淡。
 * alpha 走抛物线 (出生 0 -> 中段满 -> 死前 0): 线性淡入淡出在半径已经很大时
 * 还会剩一格可见的圆, 看着就是"突然消失"; 抛物线两头都贴着 0。 */
static void Wth_PxSmoke(unsigned char* base, int pitch, int ox, int oy,
                        int w, int h)
{
    int i;

    for (i = 0; i < WTH_SMOKE_MAX; i++)
    {
        const WthSmoke* s = &g_Smoke[i];
        const int* col;
        int r, cx, cy, a, dy, lum;

        if (!s->life) continue;
        r = ((int)s->r0 + (int)s->age * (int)s->grow) >> WTH_FX;
        if (r < 1) r = 1;
        if (r > g_Cfg.smokeR) r = g_Cfg.smokeR;
        cx = s->x >> WTH_FX;
        cy = s->y >> WTH_FX;
        if (cx < -r || cx >= w + r || cy < -r || cy >= h + r) continue;

        a = (int)s->a0 * 4 * (int)s->age * (int)(s->life - s->age)
            / ((int)s->life * (int)s->life);
        if (a <= 0) continue;
        lum = Wth_LumaAt(base, pitch, ox, oy, w, h, cx, cy);
        col = (lum > 110) ? g_Cfg.smokeDark : g_Cfg.smokeLight;

        for (dy = -r; dy <= r; dy++)
        {
            int ad = (dy < 0) ? -dy : dy;
            int hw = g_SmHalf[r][ad];
            int y = cy + dy;
            int ar = a * (r - ad + 1) / (r + 1);
            int x0 = cx - hw, x1 = cx + hw, x;

            if (y < 0 || y >= h || ar <= 0) continue;
            if (x0 < 0) x0 = 0;
            if (x1 >= w) x1 = w - 1;
            for (x = x0; x <= x1; x++)
            {
                /* 离左右边越近越淡 (最外 1/3, 次外 2/3), 不然圆边是一道硬台阶 */
                int de = x - x0, dr2 = x1 - x;

                if (dr2 < de) de = dr2;
                if (de > 2) de = 2;
                Wth_BlendAt(base, pitch, ox, oy, w, h, x, y, col[0], col[1], col[2],
                            (unsigned)(ar * (de + 1) / 3));
            }
        }
    }
}

/* 溅射颗粒: 雨/雪是水花 (普通混合), 火是火星 (加法, 才有烧起来的亮度)。
 * 颜色直接取对应天气的 colA, 不用为它单开一组参数。 */
static void Wth_PxDrop(unsigned char* base, int pitch, int ox, int oy,
                       int w, int h)
{
    int i;

    for (i = 0; i < WTH_DROP_MAX; i++)
    {
        const WthDrop* p = &g_Drop[i];
        const WthKind* k = &g_Kind[p->kind];
        int px, py;
        unsigned a;

        if (!p->life) continue;
        px = p->x >> WTH_FX;
        py = p->y >> WTH_FX;
        a = (unsigned)p->a0 * (unsigned)(p->life - p->age) / (unsigned)p->life;
        if (a == 0) continue;
        if (p->kind == WTH_K_FIRE)
        {
            Wth_AddAt(base, pitch, ox, oy, w, h, px, py,
                      k->colA[0], k->colA[1], k->colA[2], a);
            if (a > 90)
                Wth_AddAt(base, pitch, ox, oy, w, h, px, py + 1,
                          k->colB[0], k->colB[1], k->colB[2], a / 3u);
        }
        else
        {
            Wth_BlendAt(base, pitch, ox, oy, w, h, px, py,
                        k->colA[0], k->colA[1], k->colA[2], a);
            Wth_BlendAt(base, pitch, ox, oy, w, h, px, py + 1,
                        k->colA[0], k->colA[1], k->colA[2], a / 3u);
        }
    }
}

/* ---------------- 云影 (低频定形 + 高频侵蚀) ----------------
 * 算法照抄 Guerilla Games 的体积云做法 (见 wedesoft.de "Procedural Volumetric Clouds"
 * 里引述的那段代码), 只保留二维的部分:
 *
 *     base     = 低频 fBm —— 云在哪、有多厚 (coverage)
 *     detail   = 高频 fBm —— 只在云内部起作用
 *     density  = clamp( (detail - (1 - base)) / base )
 *
 * 云心 base≈1 时阈值 (1-base)≈0, detail 整个被填满 => **实心**; 云边 base 小、阈值高,
 * 只有 detail 的尖峰能过 => 边被"侵蚀"成絮状。这就是"云"和"雾"的区别。
 *
 * 烘焙完按目标覆盖率**二分校准**阈值: 上面的 remap 会把面积再缩一次 (云心要求
 * detail 也过阈值), 直接拿覆盖率当阈值的话 45% 只剩零头。
 *
 * 压暗是**乘法** (各通道 ×(256-a)/256), 色相不变: 草地还是绿、水还是蓝, 只是变暗,
 * 不会出现"盖一层灰"或水面发紫那种偏色。
 *
 * 两条必须守住的点:
 *   1) 场是 (cols+1) * (rows+1) 的堆上数组, 按**实际视图尺寸**分配。固定尺寸静态
 *      数组在 1752x1048 视图下就越界, 把后面的全局变量当 alpha 读成整片黑块。
 *   2) 落笔时在 8x8 的块内做 smoothstep 插值。整块共用一个 alpha 会留下台阶;
 *      线性插值只有 C0 连续, 云边陡处会有一圈 Mach 带 (像 8x8 马赛克网)。
 *
 * 开销: 压暗走 SSE2 (8 像素一批, 块内 alpha 用向量斜坡一次生成), 与标量同一条
 * 公式 (c*m8+128)>>8, 结果逐位一致。 */
#define WTH_CLOUD_BLK    8

#define CLD_TEX        256      /* 云图边长 (2 的幂: 采样回绕靠 & (CLD_TEX-1)) */
#define CLD_MASK       (CLD_TEX - 1)
/* y 方向格数上限: 压扁会把 y 的格数乘 100/sq (常见 2 倍), 最细一层就成了 3~4 个纹素
 * 的噪声, 云边会被啃成锯末。夹到 8 个纹素就够细了 (在屏幕上还有 ~9 倍放大)。 */
#define CLD_YMAX       (CLD_TEX / 8)
#define CLD_TILE_MUL   6        /* 平铺周期 = CloudScale * 6 像素 */
#define CLD_DRIFT_X    2        /* 云影漂移 (px/s, 60 帧基准) */
#define CLD_DRIFT_Y    1

static unsigned char g_CldTex[CLD_TEX * CLD_TEX];    /* 烘焙结果: 0..255 = 影的浓度 */
static unsigned char g_CldBase[CLD_TEX * CLD_TEX];   /* 临时: 低频层 */
static unsigned char g_CldDet[CLD_TEX * CLD_TEX];    /* 临时: 高频层 (烘焙完当模糊缓存) */
static unsigned char g_CldPerm[512];
static int g_CldTexOk = -1;      /* -1 = 没烘, 否则是烘的时候用的风格号 */

/* 置换表: 固定 LCG 打乱 0..255, 再复制成 512 长的双份 (Perlin 的经典写法)。
 * seed 来自风格的 perm 字段 —— 每种风格一张表, 云形之间才没有共同的骨架。 */
static void Wth_PermInit(unsigned seed)
{
    unsigned s = seed;
    int i;

    for (i = 0; i < 256; i++) g_CldPerm[i] = (unsigned char)i;
    for (i = 255; i > 0; i--)
    {
        unsigned char t;
        int j;

        s = s * 1103515245u + 12345u;
        j = (int)((s >> 8) % (unsigned)(i + 1));
        t = g_CldPerm[i];
        g_CldPerm[i] = g_CldPerm[j];
        g_CldPerm[j] = t;
    }
    for (i = 0; i < 256; i++) g_CldPerm[i + 256] = g_CldPerm[i];
}

/* 8 个方向的梯度点积。 */
static float Wth_Grad(int h, float x, float y)
{
    switch (h & 7)
    {
    case 0:  return  x + y;
    case 1:  return  x - y;
    case 2:  return -x + y;
    case 3:  return -x - y;
    case 4:  return  x;
    case 5:  return -x;
    case 6:  return  y;
    default: return -y;
    }
}

/* Perlin 五次 fade: 6t^5-15t^4+10t^3, 一阶二阶导在格点都为 0。 */
static float Wth_Fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* 两个格点坐标 -> 梯度索引。seed 的低 8 位当表内平移量: 同一风格里各倍频层靠它
 * 错开, 不同风格则连置换表本身都不一样 (见 Wth_PermInit)。 */
static unsigned _perm2(unsigned x, unsigned y, unsigned seed)
{
    return g_CldPerm[(g_CldPerm[(x + (seed & 255u)) & 255u] + y) & 255u];
}

/* 周期梯度噪声: 格点索引两轴分别取模, 所以在 perX/perY 格处**自我衔接** —— 这就是
 * "能无缝平铺"的全部秘密 (参考文献: gamedev.stackexchange "How do you generate
 * tileable Perlin noise" —— 每层 octave 用 per*2^o, 各层都正好铺满整张图一遍)。
 * 两轴周期可以不同: 这就是云块"横向拉长"的来源 (perY > perX), 不影响平铺。
 * (x, y) 以格为单位, 必须 >= 0; 返回大约 -1..1。 */
static float Wth_Perlin(float x, float y, int perX, int perY, unsigned seed)
{
    int ix = (int)x, iy = (int)y;
    float fx = x - (float)ix, fy = y - (float)iy;
    unsigned x0 = (unsigned)(ix % perX), y0 = (unsigned)(iy % perY);
    unsigned x1 = (x0 + 1u) % (unsigned)perX, y1 = (y0 + 1u) % (unsigned)perY;
    float u = Wth_Fade(fx), v = Wth_Fade(fy);
    unsigned h00 = _perm2(x0, y0, seed), h10 = _perm2(x1, y0, seed);
    unsigned h01 = _perm2(x0, y1, seed), h11 = _perm2(x1, y1, seed);
    float g00 = Wth_Grad((int)h00, fx, fy);
    float g10 = Wth_Grad((int)h10, fx - 1.0f, fy);
    float g01 = Wth_Grad((int)h01, fx, fy - 1.0f);
    float g11 = Wth_Grad((int)h11, fx - 1.0f, fy - 1.0f);
    float a = g00 + (g10 - g00) * u;
    float b = g01 + (g11 - g01) * u;

    return a + (b - a) * v;
}

/* 整数开方 (逐位法, 不引 libm)。用在 0..255 的浓度上: 结果 ≈ sqrt(v/255)*255。 */
static int Wth_ISqrt(unsigned n)
{
    unsigned r = 0, bit = 16384u;

    while (bit > n) bit >>= 2;
    while (bit)
    {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (int)r;
}

/* y 方向的格数 = x 的 100/sq% (至少 1)。云块"扁"就是靠这个比值 —— 见 squash 注释。
 * 两轴各自取模, 所以压扁不影响平铺。 */
static int Wth_PeryOf(int per, int sq)
{
    int perY;

    if (sq < 10) sq = 10; else if (sq > 100) sq = 100;
    perY = (per * 100 + sq / 2) / sq;
    return (perY < 1) ? 1 : perY;
}

/* 一层 fBm, 返回 0..255 (128 = 均值)。格子从 per0 起每层翻倍、幅度乘 ampPct%,
 * y 方向的格数多 100/sq%。两轴格点各自取模 => 整张图首尾自我衔接, 平铺无缝。
 * ampPct=50 是经典 fBm (幅度减半)。调小让低频独占 -> 大团平滑; 调大让高频顶上来
 * -> 絮多、轮廓碎。clumpPct > 0 时再叠一层 per=2 的单层噪声 (半张图一个起伏),
 * 把密度按大区域拉开: 那边云多、这边几乎空。 */
static int Wth_CldFbm(float u, float v, int per0, int octs, int sq, unsigned seed,
                      float warp, int ampPct, int clumpPct)
{
    float tot = 0.0f, amp = 1.0f, norm = 0.0f;
    int perX = per0, perY = Wth_PeryOf(per0, sq), o, iv;

    if (warp > 0.0f)
    {
        /* y 的位移要按 sq% 缩小: 位移是"域"单位, y 的域被压过 1/sq, 同样的位移在 y 上
         * 落到的格数会被放大, 不缩云边在 y 方向就拧成波浪 */
        float wy = warp * (float)((sq < 10) ? 10 : ((sq > 100) ? 100 : sq)) * 0.01f;

        u += Wth_Perlin(u * 2.0f, v * 2.0f, 2, 2, seed + 11) * warp;
        v += Wth_Perlin(u * 2.0f + 0.37f, v * 2.0f + 0.11f, 2, 2, seed + 47) * wy;
    }
    for (o = 0; o < octs; o++)
    {
        tot += amp * Wth_Perlin(u * (float)perX, v * (float)perY, perX, perY,
                                seed + 101 + o * 13);
        norm += amp;
        amp *= (float)ampPct * 0.01f;
        perX *= 2;
        perY *= 2;
        if (perY > CLD_YMAX) perY = CLD_YMAX;
    }
    tot /= norm;
    if (clumpPct > 0)
    {
        /* 2 格 -> 整块图就 2x2 个大区; 同样按 squash 压扁 */
        int cy = Wth_PeryOf(2, sq);

        if (cy > CLD_YMAX) cy = CLD_YMAX;
        tot += Wth_Perlin(u * 2.0f + 5.0f, v * 2.0f + 3.0f, 2, cy, seed + 191)
               * (float)clumpPct * 0.01f;
    }
    iv = (int)(128.0f + 127.5f * tot + 0.5f);
    if (iv < 0) iv = 0; else if (iv > 255) iv = 255;
    return iv;
}

/* 按阈值 thr 算一遍密度场 (out 非空时写出去), 返回"浓于 3%"的像素数 —— 二分校准用。
 * 公式就是 Guerilla 的 remap: density = clamp((detail - (1 - cv)) / cv)。 */
static int Wth_CldFill(int thr, int edge255, unsigned char* out)
{
    const int n = CLD_TEX * CLD_TEX;
    int i, hits = 0;

    for (i = 0; i < n; i++)
    {
        float cv = (float)((int)g_CldBase[i] - thr) / (float)edge255;
        float d;

        if (cv <= 0.0f) { if (out) out[i] = 0; continue; }
        if (cv > 1.0f) cv = 1.0f;
        cv = cv * cv * (3.0f - 2.0f * cv);       /* smoothstep: 阈值处导数连续 */
        d = ((float)g_CldDet[i] * (1.0f / 255.0f) - (1.0f - cv)) / cv;
        if (d < 0.0f) d = 0.0f; else if (d > 1.0f) d = 1.0f;
        if (d > 0.03f) hits++;
        if (out)
        {
            /* 开方: remap 的中性值落在 0.5, 直接当 alpha 会整体偏淡, 开方把中段抬
             * 起来, 云心才是"实"的而不是一层薄纱 */
            out[i] = (unsigned char)Wth_ISqrt((unsigned)((int)(d * 255.0f + 0.5f) * 255));
        }
    }
    return hits;
}

/* [1,2,1] 分离模糊 (环面), 抹掉纹素级锯齿。借 g_CldDet 当横向缓存 (detail 已经用完)。 */
static void Wth_CldBlur(int passes)
{
    int p, x, y;

    for (p = 0; p < passes; p++)
    {
        for (y = 0; y < CLD_TEX; y++)
        {
            int row = y * CLD_TEX;

            for (x = 0; x < CLD_TEX; x++)
                g_CldDet[row + x] = (unsigned char)
                    ((g_CldTex[row + ((x + CLD_MASK) & CLD_MASK)] +
                      2 * g_CldTex[row + x] +
                      g_CldTex[row + ((x + 1) & CLD_MASK)]) >> 2);
        }
        for (x = 0; x < CLD_TEX; x++)
        {
            for (y = 0; y < CLD_TEX; y++)
                g_CldTex[y * CLD_TEX + x] = (unsigned char)
                    ((g_CldDet[((y + CLD_MASK) & CLD_MASK) * CLD_TEX + x] +
                      2 * g_CldDet[y * CLD_TEX + x] +
                      g_CldDet[((y + 1) & CLD_MASK) * CLD_TEX + x]) >> 2);
        }
    }
}

/* 烘焙云图。换风格 (菜单里选) 会重烘一次, 其余时候只是查表。 */
static void Wth_CloudBake(void)
{
    const WthCldStyle* st;
    const int n = CLD_TEX * CLD_TEX;
    const float inv = 1.0f / (float)CLD_TEX;
    LARGE_INTEGER q0, q1, fq;
    int tx, ty, lo, hi, k, thr, hits, target, edge255;

    if (g_CldTexOk == g_Cfg.cloudStyle) return;
    g_CldTexOk = g_Cfg.cloudStyle;
    st = &kCldStyle[g_Cfg.cloudStyle];
    edge255 = st->edge * 255 / 100;
    if (edge255 < 4) edge255 = 4;
    Wth_PermInit(st->perm);
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&q0);

    for (ty = 0; ty < CLD_TEX; ty++)
    {
        unsigned char* rb = g_CldBase + ty * CLD_TEX;
        unsigned char* rd = g_CldDet + ty * CLD_TEX;

        for (tx = 0; tx < CLD_TEX; tx++)
        {
            /* +8: 域扭曲会把坐标推到 0 以下, 而 Wth_Perlin 用 (int) 截断; 8 是整数,
             * 平移 8 格不改变周期, 平铺依旧无缝 */
            float u = (float)tx * inv + 8.0f;
            float v = (float)ty * inv + 8.0f;

            rb[tx] = (unsigned char)Wth_CldFbm(u, v, st->bPer, st->bOct, st->squash,
                                               st->perm,
                                               (float)st->warp * 0.001f, st->bAmp, st->clump);
            rd[tx] = (unsigned char)Wth_CldFbm(u, v, st->dPer, st->dOct, st->squash,
                                               st->perm ^ 0xA5A5u, 0.0f, st->dAmp, 0);
        }
    }

    /* 二分校准阈值: 覆盖率对阈值单调, 12 次就到 1/4096 精度 */
    target = n * st->cover / 100;
    lo = 0;
    hi = 255;
    for (k = 0; k < 12; k++)
    {
        thr = (lo + hi) >> 1;
        if (Wth_CldFill(thr, edge255, NULL) < target) hi = thr;
        else lo = thr;
    }
    thr = lo;
    hits = Wth_CldFill(thr, edge255, g_CldTex);
    Wth_CldBlur(st->blur);

    QueryPerformanceCounter(&q1);
    {
        char msg[160];
        unsigned p = 0;

        p = AppendCStr(msg, p, "weather: cloud style=", sizeof(msg));
        p = AppendCStr(msg, p, st->name, sizeof(msg));
        p = AppendCStr(msg, p, " seed=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), (int)(st->perm & 0xFFFFu));
        p = AppendCStr(msg, p, " sq=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), st->squash);
        p = AppendCStr(msg, p, " cover=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), hits * 100 / n);
        p = AppendCStr(msg, p, "% thr=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), thr);
        p = AppendCStr(msg, p, " bake=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg),
                      (int)((q1.QuadPart - q0.QuadPart) * 1000 / fq.QuadPart));
        p = AppendCStr(msg, p, "ms", sizeof(msg));
        LogLine(msg);
    }
}

/* 屏幕像素 -> 云影浓度 0..255。双线性; 负坐标靠 >>8 / &CLD_MASK 自然取模。
 * 两轴同一个平铺周期 —— 云块"扁"是烘焙时压进纹理的, 这里 1:1 采样即可。 */
static int Wth_CloudSample(int wx, int wy)
{
    int tile = g_Cfg.cloudScale * CLD_TILE_MUL;
    int qx, qy, fx, fy, x0, y0, x1, a, b;
    const unsigned char* r0;
    const unsigned char* r1;

    if (tile < CLD_TEX) tile = CLD_TEX;
    qx = (int)((long long)wx * (CLD_TEX * 256) / tile);
    qy = (int)((long long)wy * (CLD_TEX * 256) / tile);
    x0 = (qx >> 8) & CLD_MASK;
    y0 = (qy >> 8) & CLD_MASK;
    fx = qx & 255;
    fy = qy & 255;
    x1 = (x0 + 1) & CLD_MASK;
    r0 = g_CldTex + y0 * CLD_TEX;
    r1 = g_CldTex + (((y0 + 1) & CLD_MASK) * CLD_TEX);
    a = r0[x0] + ((r0[x1] - r0[x0]) * fx >> 8);
    b = r1[x0] + ((r1[x1] - r1[x0]) * fx >> 8);
    return a + ((b - a) * fy >> 8);
}

/* 一处云影的暗度 = 烘焙出来的浓度 × 不透明度。阈值已经烘进图里了, 这里不再算。 */
static int Wth_CloudAlpha(int wx, int wy)
{
    return Wth_CloudSample(wx, wy) * g_Cfg.cloudOpacity / 255;
}

static unsigned char* g_CldF;      /* alpha 场: g_CldStride * (rows+1) 字节 */
static int g_CldStride, g_CldCap;
static int g_CldCols, g_CldRows;
static int g_CldW, g_CldH, g_CldKeyX, g_CldKeyY, g_CldKeyStyle, g_CldHave;

/* 场按需扩容 (只在视图变大时), 放不下就当没有云影, 不会越界。 */
static int Wth_CloudAlloc(int cols, int rows)
{
    int need = (cols + 1) * (rows + 1);

    if (need <= g_CldCap) return 1;
    if (g_CldF) LocalFree(g_CldF);
    g_CldF = (unsigned char*)LocalAlloc(LMEM_FIXED, (SIZE_T)need);
    g_CldCap = g_CldF ? need : 0;
    return g_CldF != NULL;
}

static void Wth_CloudBuild(int w, int h)
{
    int cols = (w + WTH_CLOUD_BLK - 1) / WTH_CLOUD_BLK;
    int rows = (h + WTH_CLOUD_BLK - 1) / WTH_CLOUD_BLK;
    int dx = g_Tick * CLD_DRIFT_X / 60;
    int dy = g_Tick * CLD_DRIFT_Y / 60;
    int bx, by;

    g_CldCols = cols;
    g_CldRows = rows;
    if (!Wth_CloudAlloc(cols, rows))
    {
        g_CldCols = g_CldRows = 0;
        g_CldHave = 0;                  /* 下次再试, 别把"没分配"记成"已就绪" */
        return;
    }
    g_CldStride = cols + 1;
    Wth_CloudBake();        /* 幂等; 只有第一次开云影时才真跑 */
    /* 多采一行一列: 插值时要用到右/下邻居, 不用在落笔时再判边界 */
    for (by = 0; by <= rows; by++)
    {
        unsigned char* row = g_CldF + by * g_CldStride;
        int wy = g_CamY + by * WTH_CLOUD_BLK + dy;

        for (bx = 0; bx <= cols; bx++)
            row[bx] = (unsigned char)Wth_CloudAlpha(g_CamX + bx * WTH_CLOUD_BLK + dx, wy);
    }
}

/* 场只在镜头/漂移走了 4 像素或尺寸变了时重算: 云影的梯度很缓, 挪 4 像素引起的
 * alpha 变化不到 1, 肉眼看不出来, 但省掉了每帧几万次查表 + 覆盖率换算。
 * 云图换了风格也要重算, 所以 g_CldTexOk 一起进缓存键。 */
static void Wth_CloudEnsure(int w, int h)
{
    int kx = (g_CamX + g_Tick * CLD_DRIFT_X / 60) >> 2;
    int ky = (g_CamY + g_Tick * CLD_DRIFT_Y / 60) >> 2;

    if (g_CldHave && w == g_CldW && h == g_CldH && kx == g_CldKeyX &&
        ky == g_CldKeyY && g_CldKeyStyle == g_Cfg.cloudStyle)
        return;
    g_CldW = w;
    g_CldH = h;
    g_CldKeyX = kx;
    g_CldKeyY = ky;
    g_CldKeyStyle = g_Cfg.cloudStyle;
    g_CldHave = 1;
    Wth_CloudBuild(w, h);
}

/* 4x4 有序抖动 (Bayer)。16bpp 的 5/6/5 在暗色上只有几档可走: 深蓝水 (5 位 R=6,
 * 6 位 G=12, 5 位 B=8) 压 20% 时, R 要掉 8% 才走一格、B 要掉 12.5% 才走一格, 于是
 * 一条平滑的 alpha 被量化成五六级台阶 —— 画面上就是一片片"断层"; 各通道还各自取整,
 * 色相比会漂 15% (深蓝发紫红就是这么来的)。
 * 把固定的 +128 换成按 4x4 Bayer 走的 8..248: 取整变成邻域里的比例混合, **均值不变**
 * (所以色相不变), 台阶变成细噪点。
 * 相位固定跟着屏幕走: 块起点恒为 8 的倍数, 所以块内相位恒为 i&3。 */
static const unsigned char kBayer[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5
};

/* 压暗一个像素: 各通道乘 m8/256 (m8 = 256-alpha), 余数按 dith/256 抖动。
 * 标量与 SSE2 同一条公式, 逐位一致。 */
static __inline unsigned short Wth_DarkPx(unsigned short v, unsigned m8,
                                          unsigned rsh, unsigned gm, unsigned dith)
{
    unsigned r, g, b;

    r = (((v >> rsh) & 31u) * m8 + dith) >> 8;
    g = (((v >> 5) & gm) * m8 + dith) >> 8;
    b = ((v & 31u) * m8 + dith) >> 8;
    return (unsigned short)((r << rsh) | (g << 5) | b);
}

/* 块内插值的权重表: 8 个像素中心在块内的位置 (0.5/8 … 7.5/8) 过 smoothstep,
 * 再放大到 256。**必须用 smoothstep 而不是 i/8 的线性权重** —— 线性插值只有 C0
 * 连续, 云边陡的地方 (alpha 每 8 像素跳十几级) 会在块边界留下一圈 Mach 带, 看着
 * 就是一张 8x8 的马赛克网。 */
static const unsigned char kCldW[8] = { 3, 24, 59, 104, 152, 197, 232, 253 };

/* 标量版: 逐像素。右边缘的残块也走它 (n < 8)。 */
static void Wth_CloudFillScalar(unsigned char* base, int pitch, int ox, int oy,
                                int cols, int rows, int w, int h,
                                unsigned rsh, unsigned gm)
{
    int by, bx;

    for (by = 0; by < rows; by++)
    {
        const unsigned char* a0 = g_CldF + by * g_CldStride;
        const unsigned char* a1 = a0 + g_CldStride;
        int y0 = by * WTH_CLOUD_BLK;
        int y1 = y0 + WTH_CLOUD_BLK;

        if (y1 > h) y1 = h;
        for (bx = 0; bx < cols; bx++)
        {
            int v00 = a0[bx], v10 = a0[bx + 1];
            int v01 = a1[bx], v11 = a1[bx + 1];
            int x0 = bx * WTH_CLOUD_BLK;
            int x1 = x0 + WTH_CLOUD_BLK;
            int n, y, i;

            if ((v00 | v10 | v01 | v11) < 2) continue;
            if (x1 > w) x1 = w;
            n = x1 - x0;
            for (y = y0; y < y1; y++)
            {
                const unsigned char* dr = kBayer + (y & 3) * 4;
                int fy = kCldW[y - y0];
                int at = v00 + ((((v01 - v00) >> 1) * fy) >> 7);
                int ab = v10 + ((((v11 - v10) >> 1) * fy) >> 7);
                unsigned short* dp = (unsigned short*)
                    (base + (size_t)(oy + y) * (size_t)pitch) + ox + x0;

                if (at < 0) at = 0; else if (at > 255) at = 255;
                if (ab < 0) ab = 0; else if (ab > 255) ab = 255;
                if ((at | ab) < 2) continue;
                for (i = 0; i < n && i < WTH_CLOUD_BLK; i++)
                {
                    int a = at + ((((ab - at) >> 1) * kCldW[i]) >> 7);

                    if (a < 0) a = 0; else if (a > 255) a = 255;
                    if (a)
                        dp[i] = Wth_DarkPx(dp[i], 256u - (unsigned)a, rsh, gm,
                                           (unsigned)dr[i & 3] * 16u + 8u);
                }
            }
        }
    }
}

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#define WTH_HAVE_SSE2 1
/* PF_XMMI64_INSTRUCTIONS_AVAILABLE = 10 (SSE2)。老 CPU 自动退回标量。 */
static int s_WthSse2 = -1;
static int WthSse2Ok(void)
{
    if (s_WthSse2 < 0) s_WthSse2 = IsProcessorFeaturePresent(10) ? 1 : 0;
    return s_WthSse2;
}

__attribute__((target("sse2")))
static void Wth_CloudFillSse2(unsigned char* base, int pitch, int ox, int oy,
                              int cols, int rows, int w, int h,
                              unsigned rsh, unsigned gm)
{
    const __m128i m5 = _mm_set1_epi16(0x1F);
    const __m128i mg = _mm_set1_epi16((short)gm);
    const __m128i k256 = _mm_set1_epi16(256);
    const __m128i zero = _mm_setzero_si128();
    const __m128i kw = _mm_setr_epi16(kCldW[0], kCldW[1], kCldW[2], kCldW[3],
                                      kCldW[4], kCldW[5], kCldW[6], kCldW[7]);
    const __m128i c5 = _mm_cvtsi32_si128(5);
    const __m128i crs = _mm_cvtsi32_si128((int)rsh);
    /* 抖动的 4 个行相位 (块恒 8 像素宽, 所以列相位就是 i&3) */
    __m128i kd[4];
    int by, bx, k;

    for (k = 0; k < 4; k++)
        kd[k] = _mm_setr_epi16(
            (short)(kBayer[k * 4 + 0] * 16 + 8), (short)(kBayer[k * 4 + 1] * 16 + 8),
            (short)(kBayer[k * 4 + 2] * 16 + 8), (short)(kBayer[k * 4 + 3] * 16 + 8),
            (short)(kBayer[k * 4 + 0] * 16 + 8), (short)(kBayer[k * 4 + 1] * 16 + 8),
            (short)(kBayer[k * 4 + 2] * 16 + 8), (short)(kBayer[k * 4 + 3] * 16 + 8));

    for (by = 0; by < rows; by++)
    {
        const unsigned char* a0 = g_CldF + by * g_CldStride;
        const unsigned char* a1 = a0 + g_CldStride;
        int y0 = by * WTH_CLOUD_BLK;
        int y1 = y0 + WTH_CLOUD_BLK;

        if (y1 > h) y1 = h;
        for (bx = 0; bx < cols; bx++)
        {
            int v00 = a0[bx], v10 = a0[bx + 1];
            int v01 = a1[bx], v11 = a1[bx + 1];
            int x0 = bx * WTH_CLOUD_BLK;
            int x1 = x0 + WTH_CLOUD_BLK;
            int n, y;

            if ((v00 | v10 | v01 | v11) < 2) continue;   /* 整块几乎全透明 */
            if (x1 > w) x1 = w;
            n = x1 - x0;
            for (y = y0; y < y1; y++)
            {
                int fy = kCldW[y - y0];                  /* smoothstep 权重 0..253 */
                int at = v00 + ((((v01 - v00) >> 1) * fy) >> 7);
                int ab = v10 + ((((v11 - v10) >> 1) * fy) >> 7);
                unsigned short* dp = (unsigned short*)
                    (base + (size_t)(oy + y) * (size_t)pitch) + ox + x0;
                __m128i km;
                __m128i dv;
                int i;

                /* 差值先 >>1 再乘权重, 是为了让 16 位乘法不溢出 (|差|≤255 × 253)。
                 * 代价是负的奇数差会多算一档 (最多 2 级 alpha), 所以夹回 0..255。 */
                if (at < 0) at = 0; else if (at > 255) at = 255;
                if (ab < 0) ab = 0; else if (ab > 255) ab = 255;
                if ((at | ab) < 2) continue;
                if (n < WTH_CLOUD_BLK)                   /* 右边缘的残块走标量 */
                {
                    const unsigned char* dr = kBayer + (y & 3) * 4;

                    for (i = 0; i < n; i++)
                    {
                        int a = at + ((((ab - at) >> 1) * kCldW[i]) >> 7);

                        if (a < 0) a = 0; else if (a > 255) a = 255;
                        if (a)
                            dp[i] = Wth_DarkPx(dp[i], 256u - (unsigned)a, rsh, gm,
                                               (unsigned)dr[i & 3] * 16u + 8u);
                    }
                    continue;
                }
                /* 块内 alpha: a_i = at + ((ab-at)/2 * w_i >> 7), 一次算 8 个 */
                km = _mm_sub_epi16(k256,
                        _mm_min_epi16(_mm_set1_epi16(255), _mm_max_epi16(zero,
                            _mm_add_epi16(
                                _mm_set1_epi16((short)at),
                                _mm_srai_epi16(
                                    _mm_mullo_epi16(
                                        _mm_srai_epi16(_mm_set1_epi16((short)(ab - at)), 1),
                                        kw), 7)))));
                dv = kd[y & 3];
                {
                    __m128i v = _mm_loadu_si128((const __m128i*)dp);
                    __m128i r = _mm_and_si128(_mm_srl_epi16(v, crs), m5);
                    __m128i g = _mm_and_si128(_mm_srl_epi16(v, c5), mg);
                    __m128i b = _mm_and_si128(v, m5);

                    r = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(r, km), dv), 8);
                    g = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(g, km), dv), 8);
                    b = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(b, km), dv), 8);
                    _mm_storeu_si128((__m128i*)dp,
                        _mm_or_si128(_mm_sll_epi16(r, crs),
                            _mm_or_si128(_mm_slli_epi16(g, 5), b)));
                }
            }
        }
    }
}

/* 一次性自检: 同一块缓冲上把 SSE2 与标量各跑一遍, 逐字节比对。
 * 只在第一帧跑一次 (小窗口), 不一致才写日志 —— 两条路必须逐位一致, 否则
 * "换台机器 / 关掉 SSE2" 就会看到不一样的云影。 */
static void Wth_CloudVerify(int w, int h)
{
    static int done = 0;
    unsigned char* a;
    unsigned char* b;
    size_t pitch, n;
    int i, bad, cols, rows;
    unsigned rsh = (g_GBits == 6) ? 11u : 10u;
    unsigned gm = (g_GBits == 6) ? 63u : 31u;

    if (done || !WthSse2Ok() || g_CldCols <= 0 || g_CldRows <= 0) return;
    done = 1;
    if (w > 128) w = 128;
    if (h > 40) h = 40;
    cols = (w + WTH_CLOUD_BLK - 1) / WTH_CLOUD_BLK;
    rows = (h + WTH_CLOUD_BLK - 1) / WTH_CLOUD_BLK;
    if (cols >= g_CldCols || rows >= g_CldRows) return;   /* 场没铺满就跳, 不越界 */
    pitch = (size_t)w * 2;
    n = pitch * (size_t)h;
    a = (unsigned char*)LocalAlloc(LMEM_FIXED, n);
    b = (unsigned char*)LocalAlloc(LMEM_FIXED, n);
    if (!a || !b)
    {
        if (a) LocalFree(a);
        if (b) LocalFree(b);
        return;
    }
    /* 底图取一条遍历全 16 位值的序列: 0 和全 1 的极端像素都要过一遍 */
    for (i = 0; i < (int)(n / 2); i++)
    {
        unsigned v = (unsigned)i * 2654435761u;

        ((unsigned short*)a)[i] = (unsigned short)(v >> 16);
    }
    memcpy(b, a, n);
    Wth_CloudFillSse2(a, (int)pitch, 0, 0, cols, rows, w, h, rsh, gm);
    Wth_CloudFillScalar(b, (int)pitch, 0, 0, cols, rows, w, h, rsh, gm);
    bad = 0;
    for (i = 0; i < (int)n; i++) if (a[i] != b[i]) bad++;
    if (bad)
    {
        char msg[80];
        unsigned p = 0;

        p = AppendCStr(msg, p, "weather: cloud sse2 mismatch=", sizeof(msg));
        p = AppendInt(msg, p, sizeof(msg), bad);
        LogLine(msg);
    }
    LocalFree(a);
    LocalFree(b);
}
#else
static void Wth_CloudVerify(int w, int h)
{
    (void)w; (void)h;
}
#endif

static void Wth_CloudFill(unsigned char* base, int pitch, int ox, int oy,
                          int cols, int rows, int w, int h)
{
    unsigned rsh = (g_GBits == 6) ? 11u : 10u;
    unsigned gm = (g_GBits == 6) ? 63u : 31u;

#ifdef WTH_HAVE_SSE2
    if (WthSse2Ok())
    {
        Wth_CloudFillSse2(base, pitch, ox, oy, cols, rows, w, h, rsh, gm);
        return;
    }
#endif
    Wth_CloudFillScalar(base, pitch, ox, oy, cols, rows, w, h, rsh, gm);
}

/* ---------------- 图像档绘制 ---------------- */

/* 平铺的格距比图小 WTH_IMG_OVERLAP 像素。
 *
 * RAIN/SNOW.SHP 是"整屏一张"的动画 (768x576 / 800x600), 它的边缘不是可无缝平铺
 * 的贴图 —— 最外一两列/行往往是空的或者被裁掉了笔画, 按整图尺寸铺就沿格线露出一
 * 条缝 (画面中间那两道十字形的"缝隙"就是格线)。这里让相邻两格重叠几个像素把缝压
 * 住: 重叠区画两遍, 但只占 4/768 ≈ 0.5%, 密度差看不出来, 缝没了。 */
#define WTH_IMG_OVERLAP 4

/* 平铺: 每个格子一次引擎 blit, 裁剪框恒为视图区 (侧栏/底栏自然被裁掉)。 */
static void Wth_DrawLayer(WthLayer* l, void* surf, void* pal,
                          const RectangleStruct* clip)
{
    int px = l->w - WTH_IMG_OVERLAP;
    int py = l->h - WTH_IMG_OVERLAP;
    int gx, gy, x, y, x1, y1;

    if (px < 32) px = l->w;
    if (py < 32) py = l->h;
    gx = ((clip->X + l->scrollX) % px + px) % px;
    gy = ((clip->Y + l->scrollY) % py + py) % py;
    x1 = clip->X + clip->Width;
    y1 = clip->Y + clip->Height;
    for (y = clip->Y - gy; y < y1; y += py)
    {
        for (x = clip->X - gx; x < x1; x += px)
        {
            WthPt pt;
            RectangleStruct bounds = *clip;
            pt.X = x;
            pt.Y = y;
            ((WthDrawShpFn)(void*)FN_DRAW_SHAPE)(surf, pal, l->shp, l->frame,
                &pt, &bounds, WTH_FLAGS, 0, 0, 0, WTH_BRIGHT, 0,
                NULL, 0, 0, 0);
        }
    }
}

/* ---------------- 每帧入口 ---------------- */

int Weather_Draw(void* surf, int surfW, int surfH, int surfPitch)
{
    RectangleStruct view, clip;
    unsigned mask = 0;
    int cdx = 0, cdy = 0, d, dp, i, drew = 0;
    int needPx;

    Wth_Init();
    Wth_CamStep(&cdx, &cdy);

    /* 图像层的锚点每帧都跟相机走 (哪怕这一档没开), 打开时才不会跳一下。 */
    for (i = 0; i < 2; i++)
    {
        g_Layer[i].scrollX += cdx;
        g_Layer[i].scrollY += cdy;
    }

    if (!g_On[WTH_K_RAIN] && !g_On[WTH_K_SNOW] && !g_On[WTH_K_FIRE] && !g_CloudOn)
        return 0;
    if (!surf || !IsPlausiblePtr(surf) || surfW <= 0 || surfH <= 0) return 0;

    d = Wth_LogicDelta();
    g_Tick += d;
    dp = (d > 4) ? 4 : d;   /* 渲染远慢于逻辑时别让粒子一步跨过整屏 */

    /* 这一帧哪些 kind 走粒子落笔: 火雨恒为粒子, 雨/雪看画法。 */
    if (g_Mode == WEATHER_DRAW_PARTICLE)
    {
        if (g_On[WTH_K_RAIN]) mask |= 1u << WTH_K_RAIN;
        if (g_On[WTH_K_SNOW]) mask |= 1u << WTH_K_SNOW;
    }
    if (g_On[WTH_K_FIRE]) mask |= 1u << WTH_K_FIRE;
    needPx = mask != 0;

    /* 视图区取 A (铺满的可见地图区): 1x 时就是视口, 缩放档也覆盖到该覆盖的地方,
     * 且天然不含侧栏。视图区落在面外时 (渲染面比窗口小) 以面为界。 */
    FM_GetViewRects(&view, NULL);
    clip = view;
    if (clip.X < 0) { clip.Width += clip.X; clip.X = 0; }
    if (clip.Y < 0) { clip.Height += clip.Y; clip.Y = 0; }
    if (clip.X + clip.Width > surfW) clip.Width = surfW - clip.X;
    if (clip.Y + clip.Height > surfH) clip.Height = surfH - clip.Y;
    if (clip.Width < 16 || clip.Height < 16) return 0;

    /* 云影: 盖在地面上、降水之下 (雨雪不该被云影再压暗一遍)。它是**独立开关**,
     * 不跟着雨/雪走 —— 想要"多云但没下雨"也能开。 */
    if (g_CloudOn && g_Cfg.cloudOpacity > 0 && surfPitch >= surfW * 2)
    {
        long long tCld = Perf_Now();
        unsigned char* cbase = GtLock(surf);

        if (cbase)
        {
            Wth_CloudEnsure(clip.Width, clip.Height);
            /* 场的行列数来自 Wth_CloudBuild (按这次视图尺寸算的), 不在这里重算,
             * 否则两边一旦不一致就会读到场外 */
            if (g_CldCols > 0 && g_CldRows > 0)
            {
                Wth_CloudVerify(clip.Width, clip.Height);
                Wth_CloudFill(cbase, surfPitch, clip.X, clip.Y,
                              g_CldCols, g_CldRows, clip.Width, clip.Height);
                drew = 1;
            }
            GtUnlock(surf);
        }
        Perf_Add(PF_D_CLD, tCld);
    }

    /* 图像档: 雨/雪 各画一层, 引擎 blit 自己裁。 */
    if (g_Mode == WEATHER_DRAW_IMAGE &&
        (g_On[WTH_K_RAIN] || g_On[WTH_K_SNOW]))
    {
        void* pal = *(void**)ADDR_ANIM_PAL;

        if (pal && IsPlausiblePtr(pal))
        {
            for (i = 0; i < 2; i++)
            {
                WthLayer* l = &g_Layer[i];

                if (!g_On[i ? WTH_K_SNOW : WTH_K_RAIN]) continue;
                if (!Wth_Ensure(l)) continue;
                Wth_Advance(l, d);
                Wth_DrawLayer(l, surf, pal, &clip);
                l->draws++;
                /* 一次性自检: 画满 120 帧时报"推进了几个动画帧", 一眼看出是不是
                 * 动态。60 帧游戏下 step=2 应该约等于 60 个动画帧。 */
                if (!l->diagDone && l->draws >= 120)
                {
                    char msg[128];
                    unsigned q = 0;

                    l->diagDone = 1;
                    q = AppendCStr(msg, q, "weather: ", sizeof(msg));
                    q = AppendCStr(msg, q, l->id, sizeof(msg));
                    q = AppendCStr(msg, q, " 120 draws -> ", sizeof(msg));
                    q = AppendInt(msg, q, sizeof(msg), (int)l->adv);
                    q = AppendCStr(msg, q, " anim frames", sizeof(msg));
                    LogLine(msg);
                }
                drew = 1;
            }
        }
    }

    /* 粒子: 三种天气走同一套推进与落笔, mask 决定这一帧谁参与。
     * surfPitch < 2*宽 ⇒ 不是 16bpp 面, 逐像素写会越过行尾, 直接不画。 */
    if (needPx && surfPitch >= surfW * 2)
    {
        unsigned char* base = GtLock(surf);

        if (base)
        {
            Wth_PxInit(clip.Width, clip.Height);
            Wth_DropStep(dp, cdx, cdy, clip.Height);
            /* 落点节拍: 与粒子数量同比例 (雨越密落得越勤), ini 给了定值就用定值 */
            for (i = 0; i < WTH_K_N; i++)
                if ((mask & (1u << i)) && g_Kind[i].splash > 0)
                    Wth_LandTick(i, dp, clip.Width, clip.Height);
            Wth_PxStep(dp, clip.Width, clip.Height, cdx, cdy, mask);
            Wth_SmokeStep(dp, cdx, cdy);
            /* 烟先画: 火球压在烟上面才有"烧穿"的层次。 */
            Wth_PxSmoke(base, surfPitch, clip.X, clip.Y, clip.Width, clip.Height);
            Wth_PxDraw(base, surfPitch, clip.X, clip.Y, clip.Width, clip.Height, mask);
            Wth_PxDrop(base, surfPitch, clip.X, clip.Y, clip.Width, clip.Height);
            GtUnlock(surf);
            drew = 1;
        }
    }
    return drew;
}
