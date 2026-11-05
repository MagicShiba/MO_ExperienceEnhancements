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
 * 云层: 视口上盖一层自平铺 Perlin 云图, **贴在世界坐标上** (相机右移 1px, 屏幕上的
 * 云纹左移 1px), 按逻辑时间整像素漂移。压暗是云影, 混向雾色是雾气; 独立开关,
 * 与雨/雪无关。
 *
 * 图像档的节奏: 引擎动画按逻辑帧播 —— art.ini 的 Rate 在读 ini 时就已经换算成
 * "每动画帧过几个逻辑帧" 存进类型 (RAIN/SNOW = 2, 即 900/400 截断), 直接拿
 * 它当间隔; 不能再拿 art 的原始 Rate 算一遍 (会变成 450 帧一帧, 看着是静止的)。
 *
 * 雨 / 雪 / 火雨 各自独立开关 (雨+雪 = 雨夹雪), 只存在于本次运行;
 * **云层开关与画法**是长期设置, 落盘 (见 Wth_LoadState)。
 * 纯渲染: 只读相机与调色板, 只写渲染面。联机安全。
 *
 * 画在哪: AfterMap 钩 (三趟地图画完、UI 之前) 调 Weather_Draw, 面与矩形取自
 * SB_RawView —— 即"本帧未缩放视口原图" (与无UI截图同源)。缩小档下原图是大面,
 * 天气画进大面后被一起降采样进窗口面; 放大档下画进低分辨率 R 块后随地图放大。
 * 天气因此成为"战场原图的一部分": 屏幕上随缩放档一起缩放 (与原版引擎把
 * RAIN/SNOW 画在地图层的行为一致), 无UI截图与存档缩略图也天然包含天气。
 */

#include "Weather.h"
#include "GdiText.h"
#include "FullMap.h"
#include "ScaleBar.h"
#include "EnvIni.h"
#include "OrderUtil.h"
#include "SoundFx.h"
#include "LightingPreset.h"
#include "WeatherCloudBlend.h"

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

/* 三种降水 (雨/雪/火雨) 的编号, 也是 g_On 的下标。 */
enum { WTH_K_RAIN = 0, WTH_K_SNOW = 1, WTH_K_FIRE = 2, WTH_K_N = 3 };

/* ---------------- 图像档: 单个图层 (雨 / 雪) ---------------- */

typedef struct {
    const char* id;         /* art.ini 的动画 ID */
    int kind;               /* 对应的 WTH_K_* (开关用) */
    void* type;             /* AnimTypeClass* */
    void* shp;              /* SHPStruct* */
    int frames, w, h;
    int loopStart, loopEnd;
    int step;               /* 每动画帧要过的逻辑帧数 (引擎已算好的值, 见 Wth_Ensure) */
    int tick;               /* 未满一帧的余数, 单位 = 1/100 逻辑帧 (要吃得下 ImageSpeed) */
    int frame;
    int fail;               /* 加载失败过就不再每帧重试 */
    int scrollX, scrollY;   /* 相机滚动累计 (像素) */
} WthLayer;

static WthLayer g_Layer[2] = {
    { "RAIN", WTH_K_RAIN },
    { "SNOW", WTH_K_SNOW }
};

/* ---------------- 参数 (ini 的 [Environment] 段) ----------------
 * 这里只有表现参数。落盘的开关只有"云层"和它的画法 (见 Wth_LoadState), 降水
 * 开关只在本次运行内有效 —— 配置里不该留下"每次进游戏都下火雨"。
 * 速度统一按"60 帧基准下的像素/秒"给, 内部再换算成 1/16 像素/逻辑帧。
 * 节名与跨模块键名见 core/EnvIni.h (环境显示组的配置全在一节里)。 */
#define WTH_SECTION ENV_SECTION

/* ---------------- 状态 ----------------
 * 三个降水开关 + 云层模式 + 降水画法, 互不影响: 雨 + 雪 = 雨夹雪, 云影/雾气
 * 可以单独开。只有云层模式与画法是长期设置 (见 Wth_LoadState); 降水开关每次进游戏
 * 都从关开始, 免得配置里留下"恒定天气"。 */
static int g_Mode = WEATHER_DRAW_IMAGE;
static int g_On[WTH_K_N];       /* 按 WTH_K_* 索引 */
static int g_CloudMode = WEATHER_CLOUD_OFF;
static int g_Thunder = 0;       /* 雷电开关 (只在本次运行有效) */
static int g_Init = 0;

/* 雷声候选池: 每次打闪随机挑一条, 比一道反复响自然。名字是 sound.ini 的条目,
 * 写在 [Environment] 的 ThunderSound 里, 逗号分隔。 */
#define WTH_TH_SND_N  3

/* ---------------- 粒子档容量 ----------------
 * 坐标与速度都是 1/16 像素定点 (整数, 不走浮点)。三种天气共用**一个**池子,
 * 布点时按 kind 顺序依次分配 (见 Wth_PxInit), 总和不会超过 WTH_PX_MAX。 */
#define WTH_FX        4            /* 定点小数位 */
#define WTH_PX_MAX    4096
#define WTH_SMOKE_MAX 192
#define WTH_DROP_MAX  1024          /* SplashRate 上限 ~1000/s 时水花并发约 1000, 320 会饱和闪烁 */
#define WTH_RIP_MAX   256           /* 1000/s x 寿命15帧 = 并发 250, 96 同理会饱和 */
#define WTH_SM_RMAX   8            /* 烟团最大半径 (像素) */
#define WTH_RIP_RMAX  12           /* 涟漪最大半径 (像素; 水纹铺开才看得见, 比烟团大) */
#define WTH_DROP_GRAV 7            /* 溅射重力: 每逻辑帧 1/16 像素/帧 的增量 */
#define WTH_FLAKE_N   4            /* 雪花造型数 */

/* 头部画法: 拖尾是共用的一段代码, 只有头不一样。 */
enum {
    WTH_HEAD_LINE = 0,      /* 有宽度的直线: 雨丝 */
    WTH_HEAD_FLAKE,         /* 十字 + 下沿暗影: 雪花 */
    WTH_HEAD_GLOW           /* 加法光斑 + 尾迹之上的烟柱: 火球 */
};

/* 加法混合 (自发光) 与光照着色都跟着 head 走: GLOW 就是"会发光的那一类",
 * 加新天气时不用再去落笔里补 kind == WTH_K_FIRE 的判断。 */
#define WTH_ADD(k)  ((k)->head == WTH_HEAD_GLOW)

/* 一种天气 = 一行参数。雨 / 雪 / 火雨 的差别全在这里, 推进与落笔是同一份代码。 */
typedef struct {
    int density;                    /* 每 N 平方像素 1 个粒子 (与分辨率无关的密度) */
    int vLo, vHi;                   /* 速度 px/s */
    int aLo, aHi;                   /* alpha 0~255 */
    int lenLo, lenHi;               /* 拖尾长度 (像素, 0 = 没有拖尾) */
    int sizeLo, sizeHi;             /* 线宽 / 雪臂长 / 火头半径 (每个粒子自己抽一个) */
    int windLo, windHi;             /* 水平速度 = 下落速度*wind/100, 区间内随机;
                                       负 = 向左; 两值全 0 = 用 spread 抖动 */
    int spread;                     /* 水平速度随机抖动 (%) */
    int sway;                       /* 左右摇摆 (×1/5 像素每帧), 0 = 不摇 */
    int bigRate;                    /* 大片比例 (%), 0 = 全小片 */
    int splashLo, splashHi;         /* 落地溅起几颗 (区间内随机), 0 = 不溅 */
    int splashRate;                 /* 每秒落点次数, 0 = 按数量推算 */
    int head;                       /* WTH_HEAD_* */
    int lit;                        /* 1 = 颜色跟环境光照走 (雨/雪); 0 = 自发光 (火雨) */
    int alwaysParticle;             /* 1 = 图像档下也走粒子 (火雨没有 SHP 形态) */
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
    { "streak",   0xC3E5D7A2u,   5, 3, 50,  14, 3, 50,  60, 25, 30, 40, 34, 1 },
    /* 卷云: 强域扭曲拉出丝缕 + 压到最扁, 高频细侵蚀收薄云边, 稀疏的一层高空薄纱 */
    { "cirrus",   0xD4F19B37u,   8, 3, 45,  18, 3, 40, 130,  0, 22, 34, 32, 1 }
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
    int rippleLo, rippleHi;         /* 落地水纹的最大半径范围 (像素, 2~WTH_RIP_RMAX) */
    int powderLifeLo, powderLifeHi; /* 雪粉寿命范围 (逻辑帧, 60=1秒); 首颗在区间上半段抽 */
    int lightMix;                   /* 雨雪受环境光着色的百分比 (0=原色, 100=完全跟光照) */

    int cloudOpacity;               /* 云层浓度上限 (0~255): 云心压暗 / 雾心混色的 alpha */
    int cloudScale;                 /* 云块尺度 (像素) */
    int cloudStyle;                 /* 云层风格, 索引 kCldStyle */
    int cloudFogFloor;              /* 雾气底浓度 (%): 薄处也留这一层, 0 = 最薄处可全透明 */

    /* 雷电与声音。音效名字是 sound.ini 的条目名 (不是 wav 文件名), 走
     * SoundFx 的 FindIndex 解析; 写成不存在的名字 = 静默不发声 (不报错)。 */
    int thFlash;                    /* 闪电全屏提亮的峰值 (0~255), 0 = 只画电弧 */
    int thBolt;                     /* 1 = 画电弧 */
    int thVol;                      /* 打雷音量倍率 (%), 100 = sound.ini 原音量 */
    int thLo, thHi;                 /* 两道闪电的间隔范围 (逻辑帧) */
    int rainVol;                    /* 雨声音量倍率 (%) */
    int rainSnd;                    /* 1 = 下雨时播 RainLoop */
    int snowVol;                    /* 风声音量倍率 (%) */
    int snowSnd;                    /* 1 = 下雪时播 SnowSound 风声 */
    int thCol[3];                   /* 闪电的加法色 (全屏提亮与电弧都用它调) */
    char thSnd[WTH_TH_SND_N][24];   /* 雷声候选池, 每次打闪随机挑一条 */
    char rainName[24];              /* 雨声循环 (sound.ini 里 Control 含 loop) */
    char snowName[24];              /* 风声循环 (sound.ini 里 Control 含 loop) */

    int smokeLight[3], smokeDark[3];
} WthCfg;

static WthCfg g_Cfg;
static WthKind g_Kind[WTH_K_N];

static void Wth_Col(int* d, int r, int g, int b)
{
    d[0] = r; d[1] = g; d[2] = b;
}

/* 一行里逗号分隔的若干名字 -> out[0..n-1], 顺带裁掉每段的空白。少给的槽位清空,
 * 这样 Sfx_Index 拿到空串会返回 -1, 挑雷声时自然跳过。输入已由 OU_ReadString
 * 去过注释, 这里只管切分。 */
static void Wth_CfgList(char out[WTH_TH_SND_N][24], const char* src)
{
    int i, n;

    for (n = 0; n < WTH_TH_SND_N; n++) out[n][0] = 0;
    if (!src) return;
    for (n = 0, i = 0; n < WTH_TH_SND_N; n++)
    {
        int j = 0;

        while (src[i] == ' ' || src[i] == '\t') i++;
        while (src[i] && src[i] != ',' && j < 23) out[n][j++] = src[i++];
        while (j > 0 && (out[n][j - 1] == ' ' || out[n][j - 1] == '\t')) j--;
        out[n][j] = 0;
        if (src[i] != ',') break;
        i++;
    }
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
    g_Cfg.rippleLo = 5;      g_Cfg.rippleHi = 9;

    g_Cfg.cloudOpacity = 64;        /* 云心压暗 25%: 再重就像"天黑了"而不是"过云" */
    g_Cfg.cloudScale = 400;         /* 一团云约 400px, 一屏七八团 */
    g_Cfg.cloudStyle = 0;           /* cumulus */
    /* 雾气底浓度: 薄处也留 20%, 整屏都有雾 => 找不到"雾从哪儿开始" */
    g_Cfg.cloudFogFloor = 20;

    /* 雷电: 打闪 + 打雷。间隔按**逻辑帧**给 (游戏里时间与帧号绑定), 60 帧下
     * 5~15 秒; 换成 30 帧玩就是 10~30 秒, 与游戏自己的时间一致。 */
    g_Cfg.thFlash = 110;
    g_Cfg.thBolt = 1;
    g_Cfg.thVol = 100;
    g_Cfg.thLo = 300;
    g_Cfg.thHi = 900;
    g_Cfg.rainVol = 100;
    g_Cfg.rainSnd = 1;
    g_Cfg.snowVol = 100;
    g_Cfg.snowSnd = 1;
    g_Cfg.powderLifeLo = 30;
    g_Cfg.powderLifeHi = 60;
    g_Cfg.lightMix = 70;
    Wth_Col(g_Cfg.thCol, 176, 194, 228);

    Wth_Col(g_Cfg.smokeLight, 178, 176, 172);
    Wth_Col(g_Cfg.smokeDark, 70, 66, 62);

    /* 雨: 细长线, 带风, 落地溅水花 */
    k = &g_Kind[WTH_K_RAIN];
    k->density = 1100;
    k->vLo = 600;   k->vHi = 1020;
    k->aLo = 150;   k->aHi = 220;
    k->lenLo = 12;  k->lenHi = 24;
    k->sizeLo = 1;  k->sizeHi = 2;
    k->windLo = -22; k->windHi = -22;
    k->splashLo = 9; k->splashHi = 9;
    k->splashRate = 55;
    k->head = WTH_HEAD_LINE;
    k->lit = 1;
    k->fade = 0;
    Wth_Col(k->colA, 206, 227, 252);
    Wth_Col(k->colB, 206, 227, 252);

    /* 雪: 慢、造型随机的片、左右摇摆, 落地只有一点点雪粉 (不是水花) */
    k = &g_Kind[WTH_K_SNOW];
    k->density = 1100;
    k->vLo = 22;    k->vHi = 67;
    k->aLo = 185;   k->aHi = 255;
    k->sizeLo = 1;  k->sizeHi = 1;
    k->spread = 45;
    k->sway = 1;
    k->bigRate = 25;
    k->windLo = 0;  k->windHi = 0;
    k->splashLo = 2; k->splashHi = 2;
    k->splashRate = 9;
    k->head = WTH_HEAD_FLAKE;
    k->lit = 1;
    Wth_Col(k->colA, 246, 250, 255);
    Wth_Col(k->colB, 246, 250, 255);

    /* 火雨: 快、亮、带烟 */
    k = &g_Kind[WTH_K_FIRE];
    k->density = 7500;
    k->vLo = 840;   k->vHi = 1560;
    k->aLo = 190;   k->aHi = 240;
    k->lenLo = 20;  k->lenHi = 38;
    k->sizeLo = 2;  k->sizeHi = 2;
    k->spread = 15;
    k->splashLo = 8; k->splashHi = 8;
    k->splashRate = 12;
    k->head = WTH_HEAD_GLOW;
    k->alwaysParticle = 1;
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

/* 单值夹取: 表达式与语句两种用法 (Wth_ReadCfg 的 clamp 段全走 WTH_CLAMP)。 */
static __inline int Wth_ClampI(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}
#define WTH_CLAMP(v, lo, hi)  ((v) = Wth_ClampI((v), (lo), (hi)))

/* 日志拼接: 每个字段一行, 消掉手工 Append 链的样板。m 必须是数组名 (取 sizeof)。 */
#define WTH_L_S(m, q, s)  ((q) = AppendCStr((m), (q), (s), sizeof(m)))
#define WTH_L_C(m, q, c)  ((q) = AppendCh((m), (q), (c), sizeof(m)))
#define WTH_L_I(m, q, v)  ((q) = AppendInt((m), (q), sizeof(m), (v)))

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
        for (n = 0; n < 3; n++) out[n] = Wth_ClampI(v[n], 0, 255);
        return;
    }
    for (n = 0; n < 3; n++)
        out[n] = Wth_ClampI(v[n], 0, 255);
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
        if (!lstrcmpiA(s, kCldStyle[i].name)) return i;
    return -1;
}

/* 三种天气的键名只差前缀, 用宏展开成三份 —— 加一种天气 = 加一行 WTH_KCFG。
 * 单值键在 WTH_KCFG, 成对的在 WTH_KPAIR: 配置里写成 "600,1020" 一行,
 * 只写一个数就是上下限相同。 */
#define WTH_KCFG(pfx, i)                                                     \
    { pfx "Density",    &g_Kind[i].density },                                \
    { pfx "SplashRate", &g_Kind[i].splashRate }

#define WTH_KPAIR(pfx, i)                                               \
    { pfx "Size",   &g_Kind[i].sizeLo,   &g_Kind[i].sizeHi },           \
    { pfx "Speed",  &g_Kind[i].vLo,      &g_Kind[i].vHi },              \
    { pfx "Alpha",  &g_Kind[i].aLo,      &g_Kind[i].aHi },              \
    { pfx "Len",    &g_Kind[i].lenLo,    &g_Kind[i].lenHi },            \
    { pfx "Splash", &g_Kind[i].splashLo, &g_Kind[i].splashHi }

/* "lo,hi" 一行两个数, 支持负数 (Wind 用)。只写一个数 = 上下限相同;
 * 读不到或写坏了一律保持默认。 */
static void Wth_CfgPair(const char* key, int* lo, int* hi)
{
    char buf[48];
    const char* p;
    int v[2], n = 0;

    OU_ReadString(WTH_SECTION, key, "", buf, sizeof(buf));
    if (!buf[0]) return;
    for (p = buf; n < 2; )
    {
        int val = 0, got = 0, neg = 0;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '-') { neg = 1; p++; }
        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; got = 1; }
        if (got) v[n++] = neg ? -val : val;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != ',') break;
        p++;
    }
    if (n <= 0) return;
    *lo = v[0];
    *hi = (n > 1) ? v[1] : v[0];
}

static void Wth_ReadCfg(void)
{
    char buf[64];
    int i;
    static const struct { const char* k; int* p; } t[] = {
        { "SpeedScale", &g_Cfg.speedScale },  { "ImageSpeed", &g_Cfg.imageSpeed },
        WTH_KCFG("Rain", WTH_K_RAIN),
        WTH_KCFG("Snow", WTH_K_SNOW),
        WTH_KCFG("Fire", WTH_K_FIRE),
        { "SnowBigRate", &g_Kind[WTH_K_SNOW].bigRate },
        { "FireTailSmoke", &g_Kind[WTH_K_FIRE].tailSmoke },
        { "FirePlume",     &g_Kind[WTH_K_FIRE].plume },
        { "SmokeRadius",   &g_Cfg.smokeR },
        { "CloudOpacity",  &g_Cfg.cloudOpacity }, { "CloudScale", &g_Cfg.cloudScale },
        { "CloudFogFloor", &g_Cfg.cloudFogFloor },
        { "ThunderFlash",  &g_Cfg.thFlash },      { "ThunderBolt", &g_Cfg.thBolt },
        { "ThunderVolume", &g_Cfg.thVol },
        { "RainVolume",    &g_Cfg.rainVol },      { "RainSoundEnable", &g_Cfg.rainSnd },
        { "SnowVolume",    &g_Cfg.snowVol },      { "SnowSoundEnable", &g_Cfg.snowSnd },
        { "WeatherLightMix", &g_Cfg.lightMix }
    };
    static const struct { const char* k; int* lo; int* hi; } t2[] = {
        WTH_KPAIR("Rain", WTH_K_RAIN),
        WTH_KPAIR("Snow", WTH_K_SNOW),
        WTH_KPAIR("Fire", WTH_K_FIRE),
        { "FireSmoke",  &g_Kind[WTH_K_FIRE].smokeLo, &g_Kind[WTH_K_FIRE].smokeHi },
        { "SnowPowderLife", &g_Cfg.powderLifeLo, &g_Cfg.powderLifeHi },
        { "RainWind",   &g_Kind[WTH_K_RAIN].windLo, &g_Kind[WTH_K_RAIN].windHi },
        { "SnowWind",   &g_Kind[WTH_K_SNOW].windLo, &g_Kind[WTH_K_SNOW].windHi },
        { "RippleSize", &g_Cfg.rippleLo,   &g_Cfg.rippleHi },
        { "SmokeLife",  &g_Cfg.smokeLifeLo, &g_Cfg.smokeLifeHi },
        { "SmokeAlpha", &g_Cfg.smokeALo,    &g_Cfg.smokeAHi },
        { "SmokeGrow",  &g_Cfg.smokeGrowLo, &g_Cfg.smokeGrowHi },
        { "ThunderFrames", &g_Cfg.thLo,     &g_Cfg.thHi }
    };
    static const struct { const char* k; int* rgb; } c[] = {
        { "RainColor",       g_Kind[WTH_K_RAIN].colA },
        { "SnowColor",       g_Kind[WTH_K_SNOW].colA },
        { "FireHeadColor",   g_Kind[WTH_K_FIRE].colC },
        { "FireMidColor",    g_Kind[WTH_K_FIRE].colA },
        { "FireEndColor",    g_Kind[WTH_K_FIRE].colB },
        { "SmokeLightColor", g_Cfg.smokeLight },
        { "SmokeDarkColor",  g_Cfg.smokeDark },
        { "ThunderColor",    g_Cfg.thCol }
    };

    for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++)
        *t[i].p = GetPrivateProfileIntA(WTH_SECTION, t[i].k, *t[i].p, g_IniPath);
    for (i = 0; i < (int)(sizeof(t2) / sizeof(t2[0])); i++)
        Wth_CfgPair(t2[i].k, t2[i].lo, t2[i].hi);

    for (i = 0; i < (int)(sizeof(c) / sizeof(c[0])); i++)
    {
        OU_ReadString(WTH_SECTION, c[i].k, "", buf, sizeof(buf));
        Wth_ParseColor(buf, c[i].rgb);          /* 空串 / 坏串保持默认 */
    }

    /* 音效条目名 = sound.ini 的段名。写错只是不出声, 启动日志里有解析结果。
     * 一条键里逗号分隔若干条雷声, 每次打闪随机挑一条。 */
    OU_ReadString(WTH_SECTION, "ThunderSound",
                  "WeatherIntro,WeatherStart,WeatherStrike", buf, sizeof(buf));
    Wth_CfgList(g_Cfg.thSnd, buf);
    OU_ReadString(WTH_SECTION, "RainSound", "RainLoop",
                  g_Cfg.rainName, sizeof(g_Cfg.rainName));
    OU_ReadString(WTH_SECTION, "SnowSound", "_Amb_WindCold",
                  g_Cfg.snowName, sizeof(g_Cfg.snowName));
    /* CloudStyle 写字或写序号都行, Wth_StyleParse 两种都认 */
    OU_ReadString(WTH_SECTION, "CloudStyle", "", buf, sizeof(buf));
    i = Wth_StyleParse(buf);
    if (i >= 0) g_Cfg.cloudStyle = i;

    /* 单调色的天气 (雨/雪) 尾端色必须跟根部一致, 否则就变成"渐变色雨"了 */
    for (i = 0; i < 3; i++)
    {
        g_Kind[WTH_K_RAIN].colB[i] = g_Kind[WTH_K_RAIN].colA[i];
        g_Kind[WTH_K_SNOW].colB[i] = g_Kind[WTH_K_SNOW].colA[i];
    }

    /* 夹一遍, 免得手改配置时把数组/循环写飞。
     * 成对参数 (lo,hi) 必须 lo<=hi: Wth_Range 里 hi-lo+1 变成 0 或负数会除零;
     * size 当遍历上限、density 当除数, 越界会画到面外或布出天量粒子。 */
#define WTH_PAIRCLAMP(lo, hi, minv, maxv)                                     \
    do { WTH_CLAMP(lo, minv, maxv); WTH_CLAMP(hi, minv, maxv);                \
         if ((hi) < (lo)) (hi) = (lo); } while (0)
    WTH_CLAMP(g_Cfg.speedScale, 5, 1000);
    WTH_CLAMP(g_Cfg.imageSpeed, 5, 1000);
    WTH_CLAMP(g_Cfg.smokeR, 1, WTH_SM_RMAX);
    WTH_CLAMP(g_Cfg.cloudOpacity, 0, 255);
    WTH_CLAMP(g_Cfg.cloudScale, 32, 4096);
    WTH_CLAMP(g_Cfg.cloudFogFloor, 0, 100);
    WTH_CLAMP(g_Cfg.thFlash, 0, 255);
    WTH_CLAMP(g_Cfg.thVol, 0, 200);
    WTH_PAIRCLAMP(g_Cfg.thLo, g_Cfg.thHi, 30, 3600);
    WTH_CLAMP(g_Cfg.rainVol, 0, 200);
    WTH_CLAMP(g_Cfg.lightMix, 0, 100);
    WTH_CLAMP(g_Cfg.snowVol, 0, 200);
    g_Cfg.thBolt = g_Cfg.thBolt ? 1 : 0;
    g_Cfg.rainSnd = g_Cfg.rainSnd ? 1 : 0;
    g_Cfg.snowSnd = g_Cfg.snowSnd ? 1 : 0;

    for (i = 0; i < WTH_K_N; i++)
    {
        WthKind* k = &g_Kind[i];

        if (k->density < 100) k->density = 100;     /* 除数: 太小会布出天量粒子 */
        WTH_PAIRCLAMP(k->vLo, k->vHi, 1, 8000);
        WTH_PAIRCLAMP(k->aLo, k->aHi, 1, 255);
        WTH_PAIRCLAMP(k->lenLo, k->lenHi, 0, 200);
        WTH_PAIRCLAMP(k->sizeLo, k->sizeHi, 1, 4);
        WTH_PAIRCLAMP(k->windLo, k->windHi, -300, 300);
        WTH_PAIRCLAMP(k->splashLo, k->splashHi, 0, 8);
        WTH_CLAMP(k->spread, 0, 200);
        WTH_CLAMP(k->sway, 0, 4);
        WTH_CLAMP(k->bigRate, 0, 100);
        WTH_CLAMP(k->splashRate, 0, 2000);
        WTH_CLAMP(k->fade, 0, 90);
        WTH_CLAMP(k->tailSmoke, 0, 255);
        WTH_CLAMP(k->plume, 0, 64);
        if (k->smokeHi > 0) WTH_PAIRCLAMP(k->smokeLo, k->smokeHi, 1, 250);
    }
    WTH_PAIRCLAMP(g_Cfg.smokeLifeLo, g_Cfg.smokeLifeHi, 2, 250);
    WTH_PAIRCLAMP(g_Cfg.smokeALo, g_Cfg.smokeAHi, 1, 255);
    WTH_PAIRCLAMP(g_Cfg.smokeGrowLo, g_Cfg.smokeGrowHi, 0, 64);
    WTH_PAIRCLAMP(g_Cfg.rippleLo, g_Cfg.rippleHi, 2, WTH_RIP_RMAX);
    WTH_PAIRCLAMP(g_Cfg.powderLifeLo, g_Cfg.powderLifeHi, 1, 200);
#undef WTH_PAIRCLAMP
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
    unsigned char wide;     /* 线宽 / 臂长 / 火头半径, 出生时按 Size 范围抽定 */
    unsigned char shape;    /* 雪花造型 (0..WTH_FLAKE_N-1) */
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

/* 溅射水滴/火星: 落地时崩出来的小颗粒, 有自己的重力/阻尼/寿命, 颜色取 kind 的 colA。
 * px/py 是上一帧的位置, 绘制时连成一小段线 —— 单像素点在地面上等于看不见。
 * drag 是每帧速度衰减 (/256): 雪粉零重力 + 高阻尼, 滑几像素就停住原地淡掉。 */
typedef struct {
    int x, y;
    int px, py;
    int vx, vy;
    unsigned char age, life, a0, grav, drag, kind;
} WthDrop;

/* 落地的一个扩散环。雨/火是水面/地面的水纹 (扁椭圆), 雪不画。
 * 半径从 r0 均匀长到 rmax (都是 1/16 像素定点), 长多大由 RippleSize 决定。 */
typedef struct {
    int x, y;
    unsigned char age, life, r0, rmax, a0, kind;
} WthRip;

static WthPx g_Px[WTH_PX_MAX];     /* 三种天气共用; 按 kind 顺序排 (见 Wth_PxInit) */
static int g_PxN;                   /* 有效粒子总数 */
static int g_PxCnt[WTH_K_N];        /* 各 kind 的粒子数 */
static WthSmoke g_Smoke[WTH_SMOKE_MAX];
static WthDrop g_Drop[WTH_DROP_MAX];
static WthRip g_Rip[WTH_RIP_MAX];
static unsigned g_SmokePut, g_DropPut, g_RipPut;
/* 半径 r 的圆形每行半宽: g_SmHalf[r][|dy|], 建一次 (整数, 不引 libm)。 */
static unsigned char g_SmHalf[WTH_RIP_RMAX + 1][WTH_RIP_RMAX + 1];
static int g_SmKernel;
static int g_PxW, g_PxH;        /* 上次布点用的视图尺寸 */
static int g_PxOk = 0;          /* 0 = 下一帧重新布点 */
static unsigned g_Rnd = 0x9E3779B9u;
static int g_Tick;              /* 逻辑帧累计 (摇摆相位 / 云影漂移) */

/* 相机 / 逻辑帧 */
static unsigned g_LastNo = 0;
static int g_HaveNo = 0;
static int g_Rewound;           /* 这一帧帧号倒退过 (读档/新局), 见 Wth_SceneCheck */
static int g_CamX, g_CamY, g_CamHave = 0;

/* ---------------- 初始化 ---------------- */

/* 长期设置: 云层模式与降水画法。其余开关只在本次运行内有效, 所以不读。
 * 键名见 core/EnvIni.h。 */
static void Wth_LoadState(void)
{
    char val[24];

    g_CloudMode = GetPrivateProfileIntA(WTH_SECTION, ENV_KEY_CLOUDMODE,
                                        WEATHER_CLOUD_OFF, g_IniPath);
    if (g_CloudMode < WEATHER_CLOUD_OFF || g_CloudMode >= WEATHER_CLOUD_MODE_N)
        g_CloudMode = WEATHER_CLOUD_OFF;
    OU_ReadString(WTH_SECTION, ENV_KEY_DRAWMODE, "image", val, sizeof(val));
    g_Mode = (!lstrcmpiA(val, "particle"))
             ? WEATHER_DRAW_PARTICLE : WEATHER_DRAW_IMAGE;
}

/* 参数自检: 装载后打一行实际生效的值, 手改 ini 没生效 (键名写错/没重进游戏)
 * 时对着这行就能看出来。 */
static void Wth_LogCfg(void)
{
    /* 数值项全在一张表里: name=lo 或 name=lo,hi (hi 为空就只打一个)。 */
    static const struct { const char* k; int* lo; int* hi; } f[] = {
        { "speed",      &g_Cfg.speedScale, NULL },
        { "rainWind",   &g_Kind[WTH_K_RAIN].windLo, &g_Kind[WTH_K_RAIN].windHi },
        { "rainSplash", &g_Kind[WTH_K_RAIN].splashLo, &g_Kind[WTH_K_RAIN].splashHi },
        { "rainRate",   &g_Kind[WTH_K_RAIN].splashRate, NULL },
        { "snowWind",   &g_Kind[WTH_K_SNOW].windLo, &g_Kind[WTH_K_SNOW].windHi },
        { "snowSplash", &g_Kind[WTH_K_SNOW].splashLo, &g_Kind[WTH_K_SNOW].splashHi },
        { "snowRate",   &g_Kind[WTH_K_SNOW].splashRate, NULL },
        { "snowBig",    &g_Kind[WTH_K_SNOW].bigRate, NULL },
        { "powder",     &g_Cfg.powderLifeLo, &g_Cfg.powderLifeHi },
        { "lightMix",   &g_Cfg.lightMix, NULL }
    };
    char msg[256];
    unsigned q = 0;
    int i;

    q = WTH_L_S(msg, q, "weather: cfg");
    for (i = 0; i < (int)(sizeof(f) / sizeof(f[0])); i++)
    {
        q = WTH_L_C(msg, q, ' ');
        q = WTH_L_S(msg, q, f[i].k);
        q = WTH_L_C(msg, q, '=');
        q = WTH_L_I(msg, q, *f[i].lo);
        if (f[i].hi)
        {
            q = WTH_L_C(msg, q, ',');
            q = WTH_L_I(msg, q, *f[i].hi);
        }
    }
    q = WTH_L_S(msg, q, " snowSnd=");
    q = WTH_L_S(msg, q, g_Cfg.snowName);
    q = WTH_L_S(msg, q, " vol=");
    q = WTH_L_I(msg, q, g_Cfg.snowVol);
    q = WTH_L_S(msg, q, " en=");
    q = WTH_L_I(msg, q, g_Cfg.snowSnd);
    LogLine(msg);
}

static void Wth_SndReset(void);
static void Wth_SndSetup(void);

static void Wth_Init(void)
{
    if (g_Init) return;
    g_Init = 1;
    BuildPaths();
    GtEnsureLog();
    Wth_Defaults();
    Wth_ReadCfg();      /* 参数只在装载时读一次: 改 ini 需重进游戏 */
    Wth_LoadState();
    Wth_SndSetup();     /* 循环音的通道/条目/音量装载时定死 (音量只从 ini 读一次) */
    Wth_LogCfg();
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
int Weather_CloudMode(void) { Wth_Init(); return g_CloudMode; }
int Weather_Thunder(void) { Wth_Init(); return g_Thunder; }

static void Wth_SetKind(int kind, int on)
{
    on = on ? 1 : 0;
    if (on == g_On[kind]) return;
    g_On[kind] = on;
    g_PxOk = 0;
    /* 雨一停立刻收声: 菜单里关雨时主循环是停的, 等下一帧就成了"关掉了还在响"。 */
    if (kind == WTH_K_RAIN && !on) Wth_SndReset();
}

void Weather_SetRain(int on) { Wth_SetKind(WTH_K_RAIN, on); }
void Weather_SetSnow(int on) { Wth_SetKind(WTH_K_SNOW, on); }
void Weather_SetFire(int on) { Wth_SetKind(WTH_K_FIRE, on); }

void Weather_SetThunder(int on)
{
    Wth_Init();
    on = on ? 1 : 0;
    if (on == g_Thunder) return;
    g_Thunder = on;
    if (!on) Wth_SndReset();
}

/* 云层模式: 关 / 云影 / 雾气。两种口径各有一套浓度场, 换档后下一帧按烘焙键重烘
 * (见 Wth_CloudBake)。 */
void Weather_SetCloudMode(int mode)
{
    Wth_Init();
    if (mode < WEATHER_CLOUD_OFF || mode >= WEATHER_CLOUD_MODE_N) return;
    g_CloudMode = mode;
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
        if (!lstrcmpiA((const char*)((char*)t + OFF_ANIMTYPE_ID), id))
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

    p = WTH_L_S(msg, p, "weather: ");
    p = WTH_L_S(msg, p, l->id);
    p = WTH_L_S(msg, p, " frames=");
    p = WTH_L_I(msg, p, l->frames);
    p = WTH_L_S(msg, p, " loop=");
    p = WTH_L_I(msg, p, l->loopStart);
    p = WTH_L_C(msg, p, '-');
    p = WTH_L_I(msg, p, l->loopEnd - 1);
    p = WTH_L_S(msg, p, " size=");
    p = WTH_L_I(msg, p, l->w);
    p = WTH_L_C(msg, p, 'x');
    p = WTH_L_I(msg, p, l->h);
    p = WTH_L_S(msg, p, " step=");
    p = WTH_L_I(msg, p, step);
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

/* 这帧渲染跨过了几个逻辑帧 (渲染与逻辑不一定 1:1)。跳号 (读档/重开) 不补帧,
 * 只置 g_Rewound 让换局判定认得这一拍。 */
static int Wth_LogicDelta(void)
{
    unsigned now = *(unsigned*)(size_t)ADDR_FRAME;
    int d;

    g_Rewound = 0;
    if (!g_HaveNo) { g_HaveNo = 1; g_LastNo = now; return 0; }
    d = (int)(now - g_LastNo);
    g_LastNo = now;
    if (d < 0 || d > 240) { g_Rewound = 1; return 0; }
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
    return Wth_ClampI(pxPerSec * 4 * g_Cfg.speedScale / 1500, 1, 8000);
}

/* 一条粒子的初始参数, 全部来自 g_Kind[kind] (单位见 Wth_V)。 */
static void Wth_Spawn(WthPx* p, int kind, int w, int h)
{
    const WthKind* k = &g_Kind[kind];

    p->kind = (unsigned char)kind;
    p->sz = 0;
    p->shape = (unsigned char)(Wth_Rand() % WTH_FLAKE_N);
    p->st = 0;
    p->vy = Wth_Range(Wth_V(k->vLo), Wth_V(k->vHi));
    /* 风向优先: wind 区间不全为 0 就按下落速度的百分比给水平速度 (拖尾方向由
     * sx 自动跟上), 没有就按 spread 随机抖一点。 */
    p->vx = (k->windLo || k->windHi)
            ? p->vy * Wth_Range(k->windLo, k->windHi) / 100
            : p->vy * Wth_Range(-k->spread, k->spread) / 100;
    p->len = (unsigned char)(k->lenHi > 0 ? Wth_Range(k->lenLo, k->lenHi) : 0);
    p->a0 = (unsigned char)Wth_Range(k->aLo, k->aHi);
    p->wide = (unsigned char)Wth_Range(k->sizeLo, k->sizeHi);
    if (k->bigRate > 0 && (int)(Wth_Rand() % 100) < k->bigRate)
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
    for (r = 0; r <= WTH_RIP_RMAX; r++)
        for (dy = 0; dy <= WTH_RIP_RMAX; dy++)
        {
            hw = 0;
            if (dy <= r)
                while (hw < r && (hw + 1) * (hw + 1) + dy * dy <= r * r) hw++;
            g_SmHalf[r][dy] = (unsigned char)hw;
        }
}

/* 这一种天气这一帧是否走粒子落笔: 图像档下只有 SHP 没有的形态 (火雨) 才画粒子,
 * 免得和 RAIN/SNOW.SHP 自带的雨丝叠成两层。布点与落笔共用这一条规则。 */
static __inline int Wth_KindOn(int i)
{
    return g_On[i] && (g_Kind[i].alwaysParticle || g_Mode == WEATHER_DRAW_PARTICLE);
}

/* 数量按视图面积给, 视图尺寸变了或换过开关就重新布点。
 * 三种天气共用一个池子, 按 kind 顺序依次分配, 累计不超过 WTH_PX_MAX。 */
static void Wth_PxInit(int w, int h)
{
    int area, i, j, at = 0, total = 0;
    int need[WTH_K_N];

    if (g_PxOk && w == g_PxW && h == g_PxH) return;
    g_PxW = w;
    g_PxH = h;
    g_PxOk = 1;
    Wth_BuildKernel();
    area = w * h;

    /* 数量 = 视图面积 / 密度, 与分辨率无关: 同一个密度在 800x600 与 1920x1080 下
     * 看起来一样疏密。只给这一帧真要画的 kind 布点 —— 图像档的雨/雪交给 SHP,
     * 不占池子。 */
    for (i = 0; i < WTH_K_N; i++)
    {
        int on = Wth_KindOn(i);

        need[i] = 0;
        if (on)
        {
            need[i] = area / g_Kind[i].density;
            if (need[i] < 4) need[i] = 4;
            total += need[i];
        }
    }
    /* 池子装不下就按比例缩 (雨夹雪再叠火雨): 一起变稀, 而不是谁先布谁占满 */
    for (i = 0; i < WTH_K_N; i++)
    {
        int n = need[i];

        if (total > WTH_PX_MAX) n = n * WTH_PX_MAX / total;
        g_PxCnt[i] = n;
        for (j = 0; j < n; j++) Wth_Spawn(&g_Px[at + j], i, w, h);
        at += n;
    }
    g_PxN = at;

    for (i = 0; i < WTH_SMOKE_MAX; i++) g_Smoke[i].life = 0;
    for (i = 0; i < WTH_DROP_MAX; i++) g_Drop[i].life = 0;
    for (i = 0; i < WTH_RIP_MAX; i++) g_Rip[i].life = 0;
    g_SmokePut = 0;
    g_DropPut = 0;
    g_RipPut = 0;

    /* 布点只在开关/尺寸变化时发生, 顺手留一行现场 (调密度用)。 */
    {
        char msg[128];
        unsigned p = 0;

        p = WTH_L_S(msg, p, "weather: particles view=");
        p = WTH_L_I(msg, p, w);
        p = WTH_L_C(msg, p, 'x');
        p = WTH_L_I(msg, p, h);
        p = WTH_L_S(msg, p, " rain=");
        p = WTH_L_I(msg, p, g_PxCnt[WTH_K_RAIN]);
        p = WTH_L_S(msg, p, " snow=");
        p = WTH_L_I(msg, p, g_PxCnt[WTH_K_SNOW]);
        p = WTH_L_S(msg, p, " fire=");
        p = WTH_L_I(msg, p, g_PxCnt[WTH_K_FIRE]);
        LogLine(msg);
    }
}

/* 落地溅射: 一个扩散环 + 一根溅起的水柱 + 几颗甩出去的水花。雨是水花 (近水平飞),
 * 火是火星 (向上崩) 并留一团烟, 雪只有轻轻扬起的雪粉 —— 地上不会出一圈涟漪。
 * 颗粒有自己的重力与寿命, 与降水粒子的回绕无关。
 * 落点是**画面内随机一点**, 不查地形高度 —— 取地块 Z 要穿 TacticalClass 的世界
 * 坐标换算, 代价和风险都不值当; 溅射本来只是氛围。 */
static void Wth_Splash(int x, int y, int kind)
{
    const WthKind* k = &g_Kind[kind];
    int fire = (kind == WTH_K_FIRE);
    int n = Wth_Range(k->splashLo, k->splashHi);
    int i;

    /* 环是落地最显眼的那一笔: 单靠几颗小点在贴图密集的地面上看不出来。
     * 半径从 2px 均匀长大, 长多大按 RippleSize 范围逐个抽, 环线只有一圈, 中间留空。 */
    if (kind != WTH_K_SNOW)
    {
        WthRip* r = &g_Rip[g_RipPut % WTH_RIP_MAX];

        g_RipPut++;
        r->x = x;
        r->y = y;
        r->age = 0;
        r->life = (unsigned char)Wth_Range(12, 18);
        r->r0 = 2 << WTH_FX;
        r->rmax = (unsigned char)(Wth_Range(g_Cfg.rippleLo, g_Cfg.rippleHi) << WTH_FX);
        r->a0 = (unsigned char)(fire ? Wth_Range(170, 230) : Wth_Range(165, 225));
        r->kind = (unsigned char)kind;
    }

    for (i = 0; i < n; i++)
    {
        WthDrop* d = &g_Drop[g_DropPut % WTH_DROP_MAX];
        int life, side, up, spread = (kind == WTH_K_SNOW) ? 3 : 8;

        g_DropPut++;
        d->x = x + (i ? Wth_Range(-spread, spread) : 0);
        d->y = y;
        d->px = d->x;
        d->py = d->y;
        d->age = 0;
        d->kind = (unsigned char)kind;
        d->grav = (unsigned char)WTH_DROP_GRAV;
        d->drag = 0;

        if (kind == WTH_K_SNOW)
        {
            /* 雪粉不是水: 一半原地淡掉, 一半滑动几像素后停稳 (零重力, 高阻尼)。
             * 寿命在 SnowPowderLife 里配, 首颗在区间上半段抽 (多亮一会儿)。 */
            int move = (int)(Wth_Rand() & 1);

            side = move ? Wth_Range(8, 28) : 0;
            up = move ? Wth_Range(4, 16) : 0;
            life = (i == 0)
                   ? Wth_Range((g_Cfg.powderLifeLo + g_Cfg.powderLifeHi) / 2,
                               g_Cfg.powderLifeHi)
                   : Wth_Range(g_Cfg.powderLifeLo, g_Cfg.powderLifeHi);
            d->a0 = (unsigned char)((i == 0) ? 190 : Wth_Range(110, 180));
            d->grav = 0;
            d->drag = 60;
        }
        else if (i == 0)
        {
            /* 砸出来的水柱/火星: 几乎竖直往上冲, 头几帧一条竖线 */
            side = Wth_Range(2, 12);
            up = fire ? Wth_Range(140, 210) : Wth_Range(96, 156);
            life = fire ? Wth_Range(6, 10) : Wth_Range(3, 4);
            d->a0 = 255;
        }
        else
        {
            int lateral = (i & 1) != 0;

            side = lateral ? Wth_Range(22, 74) : Wth_Range(4, fire ? 24 : 16);
            up = lateral ? Wth_Range(12, 48) : Wth_Range(48, fire ? 150 : 96);
            life = fire ? Wth_Range(8, 16) : Wth_Range(7, 13);
            d->a0 = (unsigned char)(fire ? Wth_Range(190, 255)
                                         : (lateral ? Wth_Range(170, 240)
                                                    : Wth_Range(150, 230)));
        }
        /* 方向随机取号, 与颗粒序号无关 (Splash=2 时按序号会恒朝一边) */
        d->vx = (Wth_Rand() & 1) ? -side : side;
        d->vy = -up;
        if (life < 3) life = 3;
        d->life = (unsigned char)life;
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
    int rate = k->splashRate > 0 ? k->splashRate : g_PxCnt[kind] / 24;
    int x, y;

    if (rate <= 0 || d <= 0 || w < 32 || h < 32) return;
    g_LandAcc[kind] += rate * 256 * d / 60;
    /* 卡一下之后别一次炸一串: 16 次/帧 = 60 逻辑帧下 ~1000 次/秒的天花板,
     * 再大的 SplashRate 也等效于这个值 */
    if (g_LandAcc[kind] > 256 * 16) g_LandAcc[kind] = 256 * 16;
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

/* 落地池推进 (溅射颗粒 + 涟漪): 两池都在落点节拍之前走, 顺序固定。
 * 溅射是抛物线, 落到视图底边以下就回收; 涟漪只跟着相机走, 半径绘制时按 age 算。 */
static void Wth_LandStep(int d, int sdx, int sdy, int h)
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
        p->vy += p->grav * d;
        if (p->drag)
        {
            /* 每帧速度乘 (256-drag)/256; 先乘后除, 小速度才不会被整数截断卡住 */
            int f = 256 - p->drag * d;

            if (f < 0) f = 0;
            p->vx = p->vx * f / 256;
            p->vy = p->vy * f / 256;
        }
        p->px = p->x;
        p->py = p->y;
        p->x += p->vx * d - (sdx << WTH_FX);
        p->y += p->vy * d - (sdy << WTH_FX);
    }
    for (i = 0; i < WTH_RIP_MAX; i++)
    {
        WthRip* p = &g_Rip[i];

        if (!p->life) continue;
        p->age = (unsigned char)(p->age + d);
        if (p->age >= p->life) { p->life = 0; continue; }
        p->x -= sdx << WTH_FX;
        p->y -= sdy << WTH_FX;
    }
}

/* ---------------- 粒子: 落笔 ---------------- */

/* 加法混合 (火雨): dst 各通道 + fg*a, 饱和。暗底上才亮得起来。 */
static unsigned short Wth_Add(unsigned short dst, unsigned r, unsigned g,
                              unsigned b, unsigned a)
{
    int rsh;
    unsigned gm;
    unsigned dr, dg, db;

    if (a == 0) return dst;
    if (!g_ExpReady) GtBuildExpand();
    /* 565 与 555 只差 R 的起始位和 G 的位宽, 其余流程一样, 用变量带过。
     * 565: R5@11 G6@5 B5 | 555: R5@10 G5@5 B5 */
    rsh = (g_GBits == 6) ? 11 : 10;
    gm  = (g_GBits == 6) ? 63u : 31u;
    dr = g_ExpR[(dst >> rsh) & 0x1Fu] + r * a / 255u;
    dg = g_ExpG[(dst >> 5) & gm] + g * a / 255u;
    db = g_ExpB[dst & 0x1Fu] + b * a / 255u;
    if (dr > 255u) dr = 255u;
    if (dg > 255u) dg = 255u;
    if (db > 255u) db = 255u;
    return (unsigned short)((((dr * 31u + 127u) / 255u) << rsh) |
                            (((dg * gm  + 127u) / 255u) << 5) |
                             ((db * 31u + 127u) / 255u));
}

static __inline unsigned short* Wth_At(unsigned char* base, int pitch,
                                       int ox, int oy, int px, int py)
{
    return (unsigned short*)(base + (size_t)(oy + py) * (size_t)pitch) + ox + px;
}

/* 带裁剪的落笔 (单像素): add != 0 走加法混合, 否则普通混合。裁剪放在这里,
 * 各个画法就不用自己判边界。 */
static __inline void Wth_Put(unsigned char* base, int pitch, int ox, int oy,
                             int w, int h, int px, int py,
                             unsigned r, unsigned g, unsigned b, unsigned a,
                             int add)
{
    unsigned short* d;

    if (a == 0 || px < 0 || px >= w || py < 0 || py >= h) return;
    d = Wth_At(base, pitch, ox, oy, px, py);
    *d = add ? Wth_Add(*d, r, g, b, a) : Blend16(*d, r, g, b, a);
}

static __inline void Wth_BlendAt(unsigned char* base, int pitch, int ox, int oy,
                                 int w, int h, int px, int py,
                                 unsigned r, unsigned g, unsigned b, unsigned a)
{
    Wth_Put(base, pitch, ox, oy, w, h, px, py, r, g, b, a, 0);
}

static __inline void Wth_AddAt(unsigned char* base, int pitch, int ox, int oy,
                               int w, int h, int px, int py,
                               unsigned r, unsigned g, unsigned b, unsigned a)
{
    Wth_Put(base, pitch, ox, oy, w, h, px, py, r, g, b, a, 1);
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

/* 粒子头离屏判定 (光斑/雪花, 半径最大 ~5, 统一留 8 像素余量)。 */
static __inline int Wth_PxOff(int hx, int hy, int w, int h)
{
    return hx < -8 || hx >= w + 8 || hy < -8 || hy >= h + 8;
}

/* 雨/雪的整体着色: 每通道乘 (环境光 + 色调x等级), 与引擎地形同一光照模型
 * (等级是千分量级, 贡献 = tint*lv/1000 个百分点, 系数不能放大)。结果与原色按
 * WeatherLightMix 插值; 火雨自发光不参与。每帧重算一遍。 */
static int g_LitA[WTH_K_N][3];

/* 这一种天气的落笔色: 受光照的取 g_LitA, 自发光 (火雨) 取配置原色。 */
static __inline const int* Wth_ColOf(int kind)
{
    return g_Kind[kind].lit ? g_LitA[kind] : g_Kind[kind].colA;
}

static void Wth_LightTick(void)
{
    int amb = Lighting_GetParam(LIGHT_P_AMBIENT);
    int lv  = Lighting_GetParam(LIGHT_P_LEVEL);
    int rgb[3], kd, c;

    rgb[0] = Lighting_GetParam(LIGHT_P_RED);
    rgb[1] = Lighting_GetParam(LIGHT_P_GREEN);
    rgb[2] = Lighting_GetParam(LIGHT_P_BLUE);
    for (kd = 0; kd < WTH_K_N; kd++)
    {
        const WthKind* k = &g_Kind[kd];

        if (!k->lit) continue;
        for (c = 0; c < 3; c++)
        {
            int m = amb + rgb[c] * lv / 1000;      /* tint/100 x lv/1000 -> 百分比 */
            int vA;

            if (m < 0) m = 0;
            if (m > 200) m = 200;
            vA = k->colA[c] * m / 100;
            g_LitA[kd][c] = (k->colA[c] * (100 - g_Cfg.lightMix) + vA * g_Cfg.lightMix) / 100;
        }
    }
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
    int wide = p->wide;
    int lead = (wide - 1) >> 1;         /* 让线条以头部为轴 (宽 3 时左移 1) */
    unsigned a0 = p->a0;
    const int* sc = NULL;
    const int* ca = Wth_ColOf(p->kind);
    const int* tb = k->lit ? ca : k->colB;      /* 雨/雪 colB==colA, 单色 */
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
        cr = Wth_Lerp(ca[0], tb[0], t);
        cg = Wth_Lerp(ca[1], tb[1], t);
        cb = Wth_Lerp(ca[2], tb[2], t);
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
    int sz = p->wide;
    unsigned a0 = p->a0;
    int dx, dy;

    if (Wth_PxOff(hx, hy, w, h)) return;
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

/* 雪花: 中心亮点 + 按造型长臂。造型与大小各自随机 —— 一屏里混着飘才不像
 * "一堆大小不同的十字"。几种造型都只占 3~5px, 再大就不像雪, 像贴图上的图标。
 * 臂用淡一档的同色系; 朝下那几笔压暗, 亮底上才看得见 (纯白在雪原上会糊掉)。
 * 臂长由 size 给, 大片 (sz) 再 +1。 */
static void Wth_PxFlake(unsigned char* base, int pitch, int ox, int oy,
                        int w, int h, const WthPx* p, const WthKind* k)
{
    const int* lc = g_LitA[p->kind];
    const int cr = lc[0], cg = lc[1], cb = lc[2];
    const int nr = (cr * 3 + 200) / 4, ng = (cg * 3 + 200) / 4, nb = (cb * 3 + 200) / 4;
    int hx = p->x >> WTH_FX;
    int hy = p->y >> WTH_FX;
    unsigned a = p->a0;
    int arm = p->wide + (p->sz ? 1 : 0);
    int d;

    if (Wth_PxOff(hx, hy, w, h)) return;

#define FL(dx, dy, r, g, bl, av) \
    Wth_BlendAt(base, pitch, ox, oy, w, h, hx + (dx), hy + (dy), r, g, bl, av)

    FL(0, 0, cr, cg, cb, a);
    switch (p->shape)
    {
    case 0:     /* 六角星: 横臂 + 上臂, 斜臂只朝上斜 (雪花主枝的方向) */
        for (d = 1; d <= arm; d++)
        {
            unsigned da = a * 4u / (unsigned)(4 + d * 4);

            FL(-d, 0, nr, ng, nb, da);
            FL(d, 0, nr, ng, nb, da);
            FL(0, -d, nr, ng, nb, da);
            FL(-d, -d, nr, ng, nb, da / 3u);
            FL(d, -d, nr, ng, nb, da / 3u);
        }
        FL(0, 1, nr / 2, ng / 2, nb / 2, a / 2u);
        break;

    case 1:     /* 十字: 四向等长, 对角一圈很淡的影子 */
        for (d = 1; d <= arm; d++)
        {
            unsigned da = a * 4u / (unsigned)(4 + d * 4);

            FL(-d, 0, nr, ng, nb, da);
            FL(d, 0, nr, ng, nb, da);
            FL(0, -d, nr, ng, nb, da);
            FL(0, d, nr / 2, ng / 2, nb / 2, da);
            if (d < arm)
            {
                FL(-d, -d, nr, ng, nb, a / 8u);
                FL(d, -d, nr, ng, nb, a / 8u);
                FL(-d, d, nr, ng, nb, a / 8u);
                FL(d, d, nr, ng, nb, a / 8u);
            }
        }
        break;

    case 2:     /* 绒球: 实心 3x3, 大片再往外一圈 —— 近处糊成一团的那种雪 */
        FL(-1, 0, nr, ng, nb, a * 3u / 4u);
        FL(1, 0, nr, ng, nb, a * 3u / 4u);
        FL(0, -1, nr, ng, nb, a * 3u / 4u);
        FL(0, 1, nr, ng, nb, a / 3u);
        FL(-1, -1, nr, ng, nb, a / 2u);
        FL(1, -1, nr, ng, nb, a / 2u);
        FL(-1, 1, nr, ng, nb, a / 4u);
        FL(1, 1, nr, ng, nb, a / 4u);
        if (arm > 1)
        {
            FL(-2, 0, nr, ng, nb, a / 5u);
            FL(2, 0, nr, ng, nb, a / 5u);
            FL(0, -2, nr, ng, nb, a / 5u);
            FL(-2, -1, nr, ng, nb, a / 8u);
            FL(2, -1, nr, ng, nb, a / 8u);
        }
        break;

    default:    /* 斜叉: 两条对角, 飘着看像在转 */
        for (d = 1; d <= arm; d++)
        {
            unsigned da = a * 4u / (unsigned)(4 + d * 4);

            FL(-d, -d, nr, ng, nb, da);
            FL(d, -d, nr, ng, nb, da);
            FL(-d, d, nr, ng, nb, da / 2u);
            FL(d, d, nr, ng, nb, da / 2u);
        }
        break;
    }
#undef FL
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

/* 线段步进步数 = max(|dx|,|dy|) (Wth_DropSeg/Wth_BltSeg 共用)。 */
static __inline int Wth_SegLen(int dx, int dy)
{
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;

    return adx > ady ? adx : ady;
}

/* 溅射颗粒的一小段轨迹: 从上一帧位置连到这一帧, 尾淡头亮。单像素点在贴图密集的
 * 地面上等于看不见; 溅起的水柱就是这么一条竖线。卡帧造成的大跳只留末端 8px,
 * 免得拉出一道横贯屏幕的白线。 */
static void Wth_DropSeg(unsigned char* base, int pitch, int ox, int oy,
                        int w, int h, int x0, int y0, int x1, int y1,
                        int r, int g, int b, unsigned a, int add)
{
    int dx = x1 - x0, dy = y1 - y0;
    int n = Wth_SegLen(dx, dy), i;

    if (n <= 0)
    {
        Wth_Put(base, pitch, ox, oy, w, h, x1, y1,
                (unsigned)r, (unsigned)g, (unsigned)b, a, add);
        return;
    }
    if (n > 8)
    {
        x0 = x1 - dx * 8 / n;
        y0 = y1 - dy * 8 / n;
        dx = x1 - x0;
        dy = y1 - y0;
        n = 8;
    }
    for (i = 0; i <= n; i++)
    {
        int px = x0 + dx * i / n;
        int py = y0 + dy * i / n;
        unsigned ai = a * (unsigned)(i + 1) / (unsigned)(n + 1);

        if (ai == 0) continue;
        Wth_Put(base, pitch, ox, oy, w, h, px, py,
                (unsigned)r, (unsigned)g, (unsigned)b, ai, add);
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
        const int* lc = Wth_ColOf(p->kind);
        int px, py, add;
        unsigned a;

        if (!p->life) continue;
        px = p->x >> WTH_FX;
        py = p->y >> WTH_FX;
        a = (unsigned)p->a0 * (unsigned)(p->life - p->age) / (unsigned)p->life;
        if (a == 0) continue;
        add = WTH_ADD(k);
        Wth_DropSeg(base, pitch, ox, oy, w, h, p->px >> WTH_FX, p->py >> WTH_FX,
                    px, py, lc[0], lc[1], lc[2], a, add);
        if (add)
        {
            Wth_AddAt(base, pitch, ox, oy, w, h, px, py + 1,
                      k->colB[0], k->colB[1], k->colB[2], a * 45u / 100u);
            if (a > 120)
                Wth_AddAt(base, pitch, ox, oy, w, h, px, py - 1,
                          k->colC[0], k->colC[1], k->colC[2], a / 4u);
        }
        else
            Wth_BlendAt(base, pitch, ox, oy, w, h, px + 1, py,
                        lc[0], lc[1], lc[2], a * 2u / 3u);
    }
}

/* 落地涟漪: 扁椭圆的环 (2:1 等距视图里水面上的圆本来就是扁的)。环线画两个像素
 * 厚 (外圈实、内圈半透), 单像素线在贴图密集的地面上基本看不见; 中间留空 ——
 * 实心圆会像"地上多了个球"。 */
static void Wth_PxRip(unsigned char* base, int pitch, int ox, int oy,
                      int w, int h)
{
    int i;

    for (i = 0; i < WTH_RIP_MAX; i++)
    {
        const WthRip* p = &g_Rip[i];
        const int* lc = Wth_ColOf(p->kind);
        int r, cx, cy, dy, a, add;

        if (!p->life) continue;
        r = (p->r0 + (int)(p->rmax - p->r0) * (int)p->age / (int)p->life) >> WTH_FX;
        if (r < 1) r = 1;
        if (r > WTH_RIP_RMAX) r = WTH_RIP_RMAX;
        cx = p->x >> WTH_FX;
        cy = p->y >> WTH_FX;
        a = (int)p->a0 * (int)(p->life - p->age) / (int)p->life;
        if (a <= 0) continue;
        add = WTH_ADD(&g_Kind[p->kind]);

#define RIP(px2, py2, av)                                                     \
    Wth_Put(base, pitch, ox, oy, w, h, px2, py2,                              \
            (unsigned)lc[0], (unsigned)lc[1], (unsigned)lc[2], av, add)

        for (dy = -(r / 2); dy <= r / 2; dy++)
        {
            int ad = (dy < 0) ? -dy : dy;
            int hw = g_SmHalf[r][ad * 2];
            int in = (hw > 1) ? hw - 1 : hw;

            RIP(cx - hw, cy + dy, (unsigned)a);
            RIP(cx + hw, cy + dy, (unsigned)a);
            RIP(cx - in, cy + dy, (unsigned)(a / 2));
            RIP(cx + in, cy + dy, (unsigned)(a / 2));
        }
    }
#undef RIP
}

/* ---------------- 云层 (云影 = 压暗, 雾气 = 混向雾色; 两套浓度场) ----------------
 * 云影的浓度场照抄 Guerilla Games 的体积云做法 (wedesoft.de "Procedural Volumetric
 * Clouds" 引述的那段), 只留二维部分:
 *
 *     base    = 低频 fBm —— 云在哪、有多厚
 *     detail  = 高频 fBm —— 只在云内部起作用
 *     density = clamp( (detail - (1 - base)) / base )
 *
 * 云心 base≈1 时阈值≈0, detail 整个被填满 => **实心**; 云边 base 小、阈值高, 只有
 * detail 的尖峰能过 => 边被侵蚀成絮状。"实心 + 絮边"就是云的特征, 也正因此不能
 * 拿来当雾 —— 雾没有边。雾气走另一套场 (Wth_CldFogFill)。
 *
 * 烘焙完按目标覆盖率**二分校准**阈值: 上面的 remap 会把面积再缩一次 (云心要求
 * detail 也过阈值), 直接拿覆盖率当阈值的话 45% 只剩零头。
 *
 * 256² 浓度图只在风格/口径变化时生成, 再预展开成像素密度的周期 alpha 图。
 * 每帧只按世界坐标偏移顺序读取, 不做逐像素噪声、插值或除法。
 * 混色在最终 5/6 位通道量化前加入屏幕固定 Bayer 抖动, 标量与 SSE2 逐位一致。 */
#define CLD_TEX        256      /* 云图边长 (2 的幂: 采样回绕靠 & (CLD_TEX-1)) */
#define CLD_MASK       (CLD_TEX - 1)
/* y 方向格数上限: 压扁会把 y 的格数乘 100/sq (常见 2 倍), 最细一层就成了 3~4 个纹素
 * 的噪声, 云边会被啃成锯末。夹到 8 个纹素就够细了 (在屏幕上还有 ~9 倍放大)。 */
#define CLD_YMAX       (CLD_TEX / 8)
#define CLD_TILE_MUL   6        /* 平铺周期 = CloudScale * 6 像素 */
#define CLD_FOG_DET    40       /* 雾气里高频层的相对权重 (%): 0 = 纯缓坡 (像蒙了块布) */
#define CLD_DRIFT_X    2        /* 云影漂移 (px/s, 60 帧基准) */
#define CLD_DRIFT_Y    1

static unsigned char g_CldTex[CLD_TEX * CLD_TEX];    /* 烘焙结果: 0..255 = 浓度 */
static unsigned char g_CldBase[CLD_TEX * CLD_TEX];   /* 临时: 低频层 */
static unsigned char g_CldDet[CLD_TEX * CLD_TEX];    /* 临时: 高频层 (烘焙完当模糊缓存) */
static unsigned char g_CldPerm[512];
/* 烘焙缓存键 = 风格号 * 2 + 口径 (0 云影 / 1 雾气); -1 = 没烘 */
static int g_CldBaked = -1;

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
    float inverse[256];
    unsigned char root[256];

    /* 阈值校准会多次扫描同一张图, 非线性变换按 256 种输入建表即可。 */
    for (i = 0; i < 256; i++)
    {
        float cv = (float)(i - thr) / (float)edge255;
        if (cv <= 0.0f) inverse[i] = 0.0f;
        else
        {
            if (cv > 1.0f) cv = 1.0f;
            cv = cv * cv * (3.0f - 2.0f * cv);
            inverse[i] = 1.0f / cv;
        }
        if (out) root[i] = (unsigned char)Wth_ISqrt((unsigned)(i * 255));
    }
    for (i = 0; i < n; i++)
    {
        float inv = inverse[g_CldBase[i]];
        float d;

        if (inv == 0.0f) { if (out) out[i] = 0; continue; }
        d = 1.0f - (1.0f - (float)g_CldDet[i] * (1.0f / 255.0f)) * inv;
        if (d < 0.0f) d = 0.0f; else if (d > 1.0f) d = 1.0f;
        if (d > 0.03f) hits++;
        if (out)
        {
            /* 开方: remap 的中性值落在 0.5, 直接当 alpha 会整体偏淡, 开方把中段抬
             * 起来, 云心才是"实"的而不是一层薄纱 */
            out[i] = root[(int)(d * 255.0f + 0.5f)];
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
        /* 源和目标分离, 纵向核也按行访问以保持连续读写。 */
        for (y = 0; y < CLD_TEX; y++)
        {
            const unsigned char* prev = g_CldDet + ((y + CLD_MASK) & CLD_MASK) * CLD_TEX;
            const unsigned char* cur = g_CldDet + y * CLD_TEX;
            const unsigned char* next = g_CldDet + ((y + 1) & CLD_MASK) * CLD_TEX;
            unsigned char* dst = g_CldTex + y * CLD_TEX;

            for (x = 0; x < CLD_TEX; x++)
                dst[x] = (unsigned char)((prev[x] + 2 * cur[x] + next[x]) >> 2);
        }
    }
}

/* 雾气的浓度场。整段绕开阈值/侵蚀/覆盖率校准 (那些都是给"有边的东西"用的):
 * 按场自身的 2%/98% 分位拉成 0..1 (256 格直方图, 场本来就只有 0..255), smoothstep
 * 之后再平方一次, 最后压进 [floor, 255]。结果恒 >= floor —— 云影那种 0~255 的落差
 * 不存在, 也就没有可见的轮廓线, 只剩"这片浓一点、那片淡一点"的缓坡。
 *
 * 平方那一步: 分位拉伸后中位数正好落在 [floor,255] 正中间 (整屏蒙一层 15% 的白,
 * 像盖了块布)。平方把中段推到 1/4 处, 既有通透处也有雾团, 反差还在但没有边。 */
static void Wth_CldFogFill(int floor255)
{
    const int n = CLD_TEX * CLD_TEX;
    const int cutLo = n / 50;            /* 低端 2% 当 0, 极端值不参与定标 */
    const int cutHi = n - cutLo;
    int hist[256];
    int i, lo = 0, hi = 255, cum;
    unsigned char remap[256];

    for (i = 0; i < 256; i++) hist[i] = 0;
    for (i = 0; i < n; i++) hist[g_CldBase[i]]++;
    for (cum = 0, i = 0; i < 256; i++)
    {
        cum += hist[i];
        if (cum > cutLo) { lo = i; break; }
    }
    for (cum = 0, i = 0; i < 256; i++)
    {
        cum += hist[i];
        if (cum >= cutHi) { hi = i; break; }
    }
    if (hi <= lo) hi = lo + 1;

    for (i = 0; i < 256; i++)
    {
        float t = (float)(i - lo) / (float)(hi - lo);

        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        t = t * t * (3.0f - 2.0f * t);      /* smoothstep */
        t = t * t;                          /* 中段压低: 薄处更薄, 浓处不变 */
        remap[i] = (unsigned char)((float)floor255 +
                                  (float)(255 - floor255) * t + 0.5f);
    }
    for (i = 0; i < n; i++) g_CldTex[i] = remap[g_CldBase[i]];
}

/* 烘焙云图。换风格或切换云影/雾气 (两套浓度场) 会重烘一次, 其余时候只是查表。 */
static void Wth_CloudBake(void)
{
    const WthCldStyle* st;
    const int n = CLD_TEX * CLD_TEX;
    const float inv = 1.0f / (float)CLD_TEX;
    LARGE_INTEGER q0, q1, fq;
    int tx, ty, lo, hi, thr = 0, hits = 0, target, edge255, fog, key;
    int bOct, bAmp;

    fog = (g_CloudMode == WEATHER_CLOUD_FOG);
    key = g_Cfg.cloudStyle * 2 + fog;
    if (g_CldBaked == key) return;
    g_CldBaked = key;
    st = &kCldStyle[g_Cfg.cloudStyle];
    edge255 = st->edge * 255 / 100;
    if (edge255 < 4) edge255 = 4;
    /* 雾只留最低两倍频, 层幅也夹到 34% 以下: 再高的层会把场切成一个个小团, 又回到
     * "云"的观感。第一层占绝对多数 = 大尺度缓坡。 */
    bOct = (fog && st->bOct > 2) ? 2 : st->bOct;
    bAmp = (fog && st->bAmp > 34) ? 34 : st->bAmp;
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

            rb[tx] = (unsigned char)Wth_CldFbm(u, v, st->bPer, bOct, st->squash,
                                               st->perm,
                                               (float)st->warp * 0.001f, bAmp, st->clump);
            rd[tx] = (unsigned char)Wth_CldFbm(u, v, st->dPer, st->dOct, st->squash,
                                               st->perm ^ 0xA5A5u, 0.0f, st->dAmp, 0);
        }
    }

    if (fog)
    {
        /* 高频层只掺 CLD_FOG_DET%: 一点纹理让雾看起来在飘, 但没有阈值去截它,
         * 掺多少都长不出云边。底浓度必须烘进图里 —— 采样端再抬下限就变成整屏加
         * 一层白, 浓淡对比被压平, 反而更像蒙了块布。 */
        int i;

        for (i = 0; i < n; i++)
        {
            int b = (int)g_CldBase[i] + (((int)g_CldDet[i] - 128) * CLD_FOG_DET) / 100;

            if (b < 0) b = 0; else if (b > 255) b = 255;
            g_CldBase[i] = (unsigned char)b;
        }
        Wth_CldFogFill(g_Cfg.cloudFogFloor * 255 / 100);
        Wth_CldBlur(st->blur + 1);      /* 再抹一遍: 雾里不该有任何纹理感 */
    }
    else
    {
        /* 阈值是整数, 区间只剩相邻值时 lo 已收敛, 不必再扫描云图。 */
        target = n * st->cover / 100;
        lo = 0;
        hi = 255;
        while (hi - lo > 1)
        {
            thr = (lo + hi) >> 1;
            if (Wth_CldFill(thr, edge255, NULL) < target) hi = thr;
            else lo = thr;
        }
        thr = lo;
        hits = Wth_CldFill(thr, edge255, g_CldTex);
        Wth_CldBlur(st->blur);
    }

    QueryPerformanceCounter(&q1);
    {
        char msg[160];
        unsigned p = 0;

        p = WTH_L_S(msg, p, "weather: cloud style=");
        p = WTH_L_S(msg, p, st->name);
        p = WTH_L_S(msg, p, " seed=");
        p = WTH_L_I(msg, p, (int)(st->perm & 0xFFFFu));
        p = WTH_L_S(msg, p, " sq=");
        p = WTH_L_I(msg, p, st->squash);
        if (fog)
        {
            p = WTH_L_S(msg, p, " fog floor=");
            p = WTH_L_I(msg, p, g_Cfg.cloudFogFloor);
            p = WTH_L_S(msg, p, "%");
        }
        else
        {
            p = WTH_L_S(msg, p, " cover=");
            p = WTH_L_I(msg, p, hits * 100 / n);
            p = WTH_L_S(msg, p, "% thr=");
            p = WTH_L_I(msg, p, thr);
        }
        p = WTH_L_S(msg, p, " bake=");
        p = WTH_L_I(msg, p, (int)((q1.QuadPart - q0.QuadPart) * 1000 / fq.QuadPart));
        p = WTH_L_S(msg, p, "ms");
        LogLine(msg);
    }
}

/* 当前口径只保留一张预展开 alpha 图。周期上限 4096, 内存上限 16MiB;
 * 极大 CloudScale 使用受限周期, 不让显示选项占掉 32 位游戏的大块地址空间。 */
#define CLD_ALPHA_MAX 4096
static unsigned char* g_CldAlpha;
static int g_CldPeriod, g_CldAlphaKey = -1, g_CldAlphaOpacity = -1;
static int g_CldBakedFloor = -1;
static WthCloudTone g_CldTone;

#ifdef WTH_HAVE_SSE2
static int WthSse2Ok(void)
{
    static int available = -1;
    if (available < 0) available = IsProcessorFeaturePresent(10) ? 1 : 0;
    return available;
}
#endif

static int Wth_CloudEnsure(void)
{
    int period = Wth_ClampI(g_Cfg.cloudScale * CLD_TILE_MUL, CLD_TEX, CLD_ALPHA_MAX);
    int key = g_Cfg.cloudStyle * 2 + (g_CloudMode == WEATHER_CLOUD_FOG);
    if (g_CldBakedFloor != g_Cfg.cloudFogFloor)
    {
        g_CldBaked = -1;
        g_CldBakedFloor = g_Cfg.cloudFogFloor;
        g_CldAlphaKey = -1;
    }
    if (g_CldAlpha && g_CldPeriod == period && g_CldAlphaKey == key &&
        g_CldAlphaOpacity == g_Cfg.cloudOpacity) return 1;

    if (!g_CldAlpha || g_CldPeriod != period)
    {
        /* 仅尺寸变化才分配, 失败时保留旧内存但不绘制失配的数据。 */
        unsigned char* next = (unsigned char*)LocalAlloc(LMEM_FIXED, (SIZE_T)period * period);
        if (!next) return 0;
        if (g_CldAlpha) LocalFree(g_CldAlpha);
        g_CldAlpha = next;
        g_CldPeriod = period;
    }
    Wth_CloudBake();
    LARGE_INTEGER begin, end, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&begin);
    /* 横向坐标只计算一次; 插值全精度合并后再量化, 避免两次取整带来的暗偏。 */
    unsigned short coord[CLD_ALPHA_MAX];
    unsigned short weight[CLD_ALPHA_MAX];
    for (int x = 0; x < period; x++)
    {
        int q = x * (CLD_TEX * 256) / period;
        coord[x] = (unsigned short)(q >> 8);
        weight[x] = (unsigned short)(q & 255);
    }
    for (int y = 0; y < period; y++)
    {
        int q = y * (CLD_TEX * 256) / period;
        int ty = q >> 8, fy = q & 255;
        const unsigned char* r0 = g_CldTex + ty * CLD_TEX;
        const unsigned char* r1 = g_CldTex + ((ty + 1) & CLD_MASK) * CLD_TEX;
        unsigned char* dst = g_CldAlpha + (size_t)y * period;
        for (int x = 0; x < period; x++)
        {
            int tx = coord[x], nx = (tx + 1) & CLD_MASK, fx = weight[x];
            int top = r0[tx] * (256 - fx) + r0[nx] * fx;
            int bottom = r1[tx] * (256 - fx) + r1[nx] * fx;
            int density = (top * (256 - fy) + bottom * fy + 32768) >> 16;
            dst[x] = (unsigned char)((density * g_Cfg.cloudOpacity + 127) / 255);
        }
    }
    g_CldAlphaKey = key;
    g_CldAlphaOpacity = g_Cfg.cloudOpacity;
    QueryPerformanceCounter(&end);
    char msg[160];
    unsigned p = 0;
    p = WTH_L_S(msg, p, "weather: cloud alpha period=");
    p = WTH_L_I(msg, p, period);
    p = WTH_L_S(msg, p, " requested=");
    p = WTH_L_I(msg, p, g_Cfg.cloudScale * CLD_TILE_MUL);
    p = WTH_L_S(msg, p, " KiB=");
    p = WTH_L_I(msg, p, period * period / 1024);
    p = WTH_L_S(msg, p, " expand=");
    p = WTH_L_I(msg, p, (int)((end.QuadPart - begin.QuadPart) * 1000 / frequency.QuadPart));
    p = WTH_L_S(msg, p, "ms");
    LogLine(msg);
    return 1;
}

/* 相机可为负值; 漂移按逻辑时间, 用 64 位中间值防止长局乘法溢出。 */
static int Wth_CloudOffset(int camera, int speed)
{
    long long pos = (long long)camera + (long long)(unsigned)g_Tick * speed / 60;
    int r = (int)(pos % g_CldPeriod);
    return r < 0 ? r + g_CldPeriod : r;
}

/* 落笔的目标色, 每帧一次。压暗 (云影) 时清零; 雾气按环境光算 —— 与雨雪同一条
 * 公式 (m = 环境光 + 色调 x 等级/1000, 单位都是百分比), 强度共用 WeatherLightMix
 * (0 = 纯白, 100 = 完全跟光照)。雾不发光, 夜里该是灰的。
 *
 * m 夹在 0..100 (雨雪那边放到 200): 目标色不超过通道满值, 最终量化前保留 Bayer 抖动。 */
static void Wth_CldToneCalc(unsigned gm)
{
    int amb, lv, rgb[3], maxc[3], c;

    if (g_CloudMode != WEATHER_CLOUD_FOG)
    {
        g_CldTone.target[0] = g_CldTone.target[1] = g_CldTone.target[2] = 0;
        return;
    }
    amb = Lighting_GetParam(LIGHT_P_AMBIENT);
    lv = Lighting_GetParam(LIGHT_P_LEVEL);
    rgb[0] = Lighting_GetParam(LIGHT_P_RED);
    rgb[1] = Lighting_GetParam(LIGHT_P_GREEN);
    rgb[2] = Lighting_GetParam(LIGHT_P_BLUE);
    maxc[0] = 31;
    maxc[1] = (int)gm;
    maxc[2] = 31;
    for (c = 0; c < 3; c++)
    {
        int m = amb + rgb[c] * lv / 1000;
        int v;

        if (m < 0) m = 0; else if (m > 100) m = 100;
        v = maxc[c] * m / 100;
        g_CldTone.target[c] = (maxc[c] * (100 - g_Cfg.lightMix) + v * g_Cfg.lightMix) / 100;
    }
}

/* 每行按纹理周期切 span。即使视口比周期宽也可多次回绕, 抖动相位不重置。 */
template<bool Fog, bool Simd>
static void Wth_CloudRows(unsigned char* base, int pitch, int ox, int oy,
                           int w, int h, int sx, int sy)
{
#ifdef WTH_HAVE_SSE2
    WthCloudSimd simd;
    if (Simd) simd.Init(g_CldTone);
#endif
    for (int y = 0; y < h; y++)
    {
        const unsigned char* row = g_CldAlpha + (size_t)sy * g_CldPeriod;
        unsigned short* dst = (unsigned short*)(base + (size_t)(oy + y) * pitch) + ox;
        int x = 0, tx = sx;
        while (x < w)
        {
            int n = g_CldPeriod - tx;
            if (n > w - x) n = w - x;
#ifdef WTH_HAVE_SSE2
            if (Simd) WthCloudSse2<Fog>(dst + x, row + tx, n, ox + x, oy + y, g_CldTone, simd);
            else
#endif
                WthCloudScalar<Fog>(dst + x, row + tx, n, ox + x, oy + y, g_CldTone);
            x += n;
            tx = 0;
        }
        if (++sy == g_CldPeriod) sy = 0;
    }
}

static void Wth_CloudFill(unsigned char* base, int pitch, int ox, int oy, int w, int h)
{
    g_CldTone.rsh = (g_GBits == 6) ? 11u : 10u;
    g_CldTone.gm = (g_GBits == 6) ? 63u : 31u;
    Wth_CldToneCalc(g_CldTone.gm);
    int sx = Wth_CloudOffset(g_CamX, CLD_DRIFT_X);
    int sy = Wth_CloudOffset(g_CamY, CLD_DRIFT_Y);
    bool fog = (g_CloudMode == WEATHER_CLOUD_FOG);
#ifdef WTH_HAVE_SSE2
    if (WthSse2Ok())
    {
        if (fog) Wth_CloudRows<true, true>(base, pitch, ox, oy, w, h, sx, sy);
        else Wth_CloudRows<false, true>(base, pitch, ox, oy, w, h, sx, sy);
        return;
    }
#endif
    if (fog) Wth_CloudRows<true, false>(base, pitch, ox, oy, w, h, sx, sy);
    else Wth_CloudRows<false, false>(base, pitch, ox, oy, w, h, sx, sy);
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

/* ---------------- 雷电 (闪电 / 打雷) 与雨声 ----------------
 *
 * 声音走 sound.ini 的条目 (见 SoundFx), 名字在 [Environment] 可换:
 *   ThunderSound  逗号分隔的几条雷声, 默认 WeatherIntro, WeatherStart, WeatherStrike
 *   RainSound     雨声循环, 默认 RainLoop
 *   SnowSound     雪天的风声循环, 默认 _Amb_WindCold
 *
 * 时间线按**逻辑帧**: 这游戏的速度与帧号绑定, 按墙钟走会出现"游戏变慢雷照打"。
 * 雷电是独立开关 (不必下雨) → 2 秒后第一道 → 之后每 Min~Max 帧一道, 每道先见光、
 * 后闻声 (延迟按帧算), 离得远的声音轻而钝。
 *
 * 落点在归一化空间里可以越出视图 (±半屏): 天边的雷看不见电弧, 只剩提亮和声音,
 * 声像跟着落点走。
 *
 * 画面 = 电弧 (自绘折线 + 三层光晕, 加法混合) + 整屏提亮, 共用一条寿命曲线。曲线
 * 第 4 帧的第二个峰是回击: 真实闪电都闪两下, 单峰像闪光灯。只画在视图区原图面上
 * (与雨/雪同源), 缩放、无UI截图、存档缩略图都一致。
 */

#define WTH_BLT_NODE   9      /* 主折线节点数 (含首尾) */
#define WTH_BLT_BR     3      /* 分叉数 */
#define WTH_BLT_BRN    4      /* 每个分叉的节点数 */
#define WTH_BLT_LIFE   10     /* 一道闪电的寿命 (逻辑帧) */
#define WTH_BLT_OFF    512    /* 落点最多越出视图多远 (归一化半屏) */
#define WTH_TH_FIRST   120    /* 开雷电之后多久打第一道 (逻辑帧) */
#define WTH_TH_DLY     6      /* 见光到闻声的最短延迟 (逻辑帧) */

static const unsigned char kFlashA[WTH_BLT_LIFE] =
    { 255, 170, 92, 40, 128, 58, 18, 0, 0, 0 };
static const unsigned char kBoltA[WTH_BLT_LIFE] =
    { 255, 236, 196, 132, 224, 146, 64, 0, 0, 0 };

/* 电弧坐标归一化到 0..1023 (可以越界), 绘制时按视图尺寸展开 —— 换分辨率不用重生成。 */
typedef struct {
    int on;
    int age;                        /* 已过几个逻辑帧 */
    int prox;                       /* 落点离视图多近 (255 = 视图内), 压暗屏外那道闪的提亮 */
    unsigned char n, bn;
    unsigned char bFrom[WTH_BLT_BR];
    short mx[WTH_BLT_NODE], my[WTH_BLT_NODE];
    short bx[WTH_BLT_BR][WTH_BLT_BRN], by[WTH_BLT_BR][WTH_BLT_BRN];
} WthBolt;

/* 一条循环音 (雨声 / 风声): 通道、条目、音量在装载时定死。 */
typedef struct {
    int started;                    /* 已建立循环事件, 不代表始终可闻 */
    unsigned retry;                 /* 下次尝试起的时刻 (ms) */
    int sfx;                        /* SFX_LOOP_* 通道 */
    const char* name;               /* sound.ini 条目名 (g_Cfg 里的数组) */
    int vol;
} WthLoop;

typedef struct {
    int on;                         /* 雷电在跑 */
    WthLoop rain, wind;             /* 雨声 / 风声循环, 各跟各的开关 */
    unsigned next;                  /* 下一道闪电的帧号 */
    unsigned sndAt;                 /* 待播的那声雷的帧号, 0 = 无 */
    int sndPan, sndVol, sndIdx;
} WthStorm;

static WthBolt  g_Bolt;
static WthStorm g_Storm;

static void Wth_ThunderStop(void)
{
    g_Storm.on = 0;
    g_Storm.sndAt = 0;
    g_Bolt.on = 0;
    g_Bolt.age = 0;
}

/* Wth_Init 调: 循环音的通道/条目/音量装载时定死。 */
static void Wth_SndSetup(void)
{
    g_Storm.rain.sfx = SFX_LOOP_RAIN;
    g_Storm.rain.name = g_Cfg.rainName;
    g_Storm.rain.vol = g_Cfg.rainVol;
    g_Storm.wind.sfx = SFX_LOOP_WIND;
    g_Storm.wind.name = g_Cfg.snowName;
    g_Storm.wind.vol = g_Cfg.snowVol;
}

static void Wth_LoopStop(WthLoop* l)
{
    if (l->started)
    {
        Sfx_LoopStop(l->sfx);
        l->started = 0;
    }
}

/* 循环音事件被抢占/回收后不再有效: 开关仍开着就退避重试, 不提高优先级。
 * 事件存活不等于可闻, 待播/延时态必须留给引擎, 不能重复创建; 关掉则取消重试。 */
static void Wth_LoopTick(WthLoop* l, int on, unsigned now)
{
    if (!on)
    {
        Wth_LoopStop(l);
        return;
    }
    if (l->started) l->started = Sfx_LoopAlive(l->sfx);
    if (!l->started && (int)(now - l->retry) >= 0)
    {
        /* 名字无效时长退避, 避免反复扫描音效表; 资源竞争仅限频200ms。 */
        if (Sfx_Index(l->name) < 0)
        {
            l->retry = now + 2000;
            return;
        }
        l->started = Sfx_LoopStart(l->sfx, l->name, 0x2000, l->vol);
        l->retry = now + 200;
    }
}

/* 雨停 / 雪停 / 雷电关: 循环音与待播的雷一起收掉 (菜单里点完立刻生效)。 */
static void Wth_SndReset(void)
{
    Wth_ThunderStop();
    Wth_LoopStop(&g_Storm.rain);
    Wth_LoopStop(&g_Storm.wind);
    g_Storm.rain.retry = 0;
    g_Storm.wind.retry = 0;
}

/* 生成一道电弧: 主折线从视图上方 (或更高的云外) 扎到视图 55%~110% 处, 每段左右甩;
 * 再从主干中段各分出一条叉 (真实闪电的叉都从中段斜着甩出去)。
 * 横向不夹到视图内: 多数落点贴着视图, 偶尔整个甩到屏外 —— 那一次只剩提亮和一声
 * 从那个方向传来的雷。 */
static void Wth_BoltGen(void)
{
    int i, b, j, x, y0, yEnd;

    g_Bolt.n = WTH_BLT_NODE;
    g_Bolt.bn = WTH_BLT_BR;
    if (Wth_Range(0, 2) == 0)
        x = Wth_Range(-WTH_BLT_OFF, 1023 + WTH_BLT_OFF);
    else
        x = Wth_Range(-100, 1123);
    y0 = -Wth_Range(0, 80);
    yEnd = Wth_Range(520, 1120);
    for (i = 0; i < WTH_BLT_NODE; i++)
    {
        int yy = y0 + (yEnd - y0) * i / (WTH_BLT_NODE - 1);

        if (i)
        {
            x += Wth_Range(-54, 54);
            yy += Wth_Range(-16, 16);
        }
        g_Bolt.mx[i] = (short)x;
        g_Bolt.my[i] = (short)yy;
    }
    for (b = 0; b < WTH_BLT_BR; b++)
    {
        int from = 2 + b * 2;
        int cx = g_Bolt.mx[from], cy = g_Bolt.my[from];
        int dir = (b & 1) ? 1 : -1;

        g_Bolt.bFrom[b] = (unsigned char)from;
        for (j = 0; j < WTH_BLT_BRN; j++)
        {
            cx += dir * Wth_Range(18, 46);
            cy += Wth_Range(24, 56);
            g_Bolt.bx[b][j] = (short)cx;
            g_Bolt.by[b][j] = (short)cy;
        }
    }
}

/* 电弧上的一个点: 白芯 + 三层蓝光晕, 加法混合。光晕按半径迅速衰减, 衰减慢了边缘
 * 会有一圈硬边, 看着像加粗的线而不是光。 */
static void Wth_BltDot(unsigned char* base, int pitch, int ox, int oy,
                       int w, int h, int px, int py, int a)
{
    Wth_AddAt(base, pitch, ox, oy, w, h, px, py, 236, 244, 255, (unsigned)a);
    Wth_AddAt(base, pitch, ox, oy, w, h, px - 1, py, 190, 214, 255, (unsigned)(a * 62 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px + 1, py, 190, 214, 255, (unsigned)(a * 62 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px, py - 1, 190, 214, 255, (unsigned)(a * 45 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px, py + 1, 190, 214, 255, (unsigned)(a * 45 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px - 1, py - 1, 150, 178, 240, (unsigned)(a * 28 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px + 1, py - 1, 150, 178, 240, (unsigned)(a * 28 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px - 1, py + 1, 150, 178, 240, (unsigned)(a * 28 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px + 1, py + 1, 150, 178, 240, (unsigned)(a * 28 / 100));
    if (a < 32) return;
    Wth_AddAt(base, pitch, ox, oy, w, h, px - 2, py, 92, 128, 200, (unsigned)(a * 14 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px + 2, py, 92, 128, 200, (unsigned)(a * 14 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px, py - 2, 92, 128, 200, (unsigned)(a * 14 / 100));
    Wth_AddAt(base, pitch, ox, oy, w, h, px, py + 2, 92, 128, 200, (unsigned)(a * 14 / 100));
}

/* 一条电弧线段: 沿线逐像素铺点。线段自身给的是"发光方向", 粗细由 Wth_BltDot 给。 */
static void Wth_BltSeg(unsigned char* base, int pitch, int ox, int oy,
                       int w, int h, int ax, int ay, int bx, int by, int a)
{
    int n = Wth_SegLen(bx - ax, by - ay);
    int i;

    if (n <= 0)
    {
        Wth_BltDot(base, pitch, ox, oy, w, h, ax, ay, a);
        return;
    }
    for (i = 0; i <= n; i++)
        Wth_BltDot(base, pitch, ox, oy, w, h, ax + (bx - ax) * i / n,
                   ay + (by - ay) * i / n, a);
}

static void Wth_BoltDraw(unsigned char* base, int pitch, int ox, int oy,
                         int w, int h)
{
    int a, i, b, j;

    if (!g_Bolt.on) return;
    a = kBoltA[g_Bolt.age];
    if (a <= 0) return;

    for (i = 0; i + 1 < g_Bolt.n; i++)
        Wth_BltSeg(base, pitch, ox, oy, w, h,
                   g_Bolt.mx[i] * w / 1024, g_Bolt.my[i] * h / 1024,
                   g_Bolt.mx[i + 1] * w / 1024, g_Bolt.my[i + 1] * h / 1024, a);

    for (b = 0; b < g_Bolt.bn; b++)
    {
        int from = g_Bolt.bFrom[b];
        int ax = g_Bolt.mx[from] * w / 1024;
        int ay = g_Bolt.my[from] * h / 1024;

        for (j = 0; j < WTH_BLT_BRN; j++)
        {
            int bx = g_Bolt.bx[b][j] * w / 1024;
            int by = g_Bolt.by[b][j] * h / 1024;

            Wth_BltSeg(base, pitch, ox, oy, w, h, ax, ay, bx, by, a * 3 / 4);
            ax = bx;
            ay = by;
        }
    }
}

/* 整屏提亮 (一次闪电的一"闪")。
 *
 * 加法混合逐像素算太贵: 一屏近两百万像素, 每帧要多花十几毫秒, 而闪电正好落
 * 在最需要帧率的时候。565 用 SSE2 一次 8 像素 (拆通道 -> 加 -> 夹 -> 拼回),
 * 555 与老 CPU 走标量 (逐像素同一套算法, 结果一致)。
 * 行间从 aTop 渐变到 aBot: 雷电的亮是"从天上来"的, 上下一样亮像有人开灯。 */
#ifdef WTH_HAVE_SSE2
__attribute__((target("sse2")))
static void Wth_FlashRowSse2(unsigned short* row, int n, int ar, int ag, int ab)
{
    const __m128i m5 = _mm_set1_epi16(0x1F);
    const __m128i m6 = _mm_set1_epi16(0x3F);
    const __m128i vr = _mm_set1_epi16((short)ar);
    const __m128i vg = _mm_set1_epi16((short)ag);
    const __m128i vb = _mm_set1_epi16((short)ab);
    int x;

    for (x = 0; x + 8 <= n; x += 8)
    {
        __m128i v = _mm_loadu_si128((const __m128i*)(row + x));
        __m128i r = _mm_and_si128(_mm_srli_epi16(v, 11), m5);
        __m128i g = _mm_and_si128(_mm_srli_epi16(v, 5), m6);
        __m128i b = _mm_and_si128(v, m5);

        r = _mm_min_epi16(_mm_add_epi16(r, vr), m5);
        g = _mm_min_epi16(_mm_add_epi16(g, vg), m6);
        b = _mm_min_epi16(_mm_add_epi16(b, vb), m5);
        v = _mm_or_si128(_mm_or_si128(_mm_slli_epi16(r, 11),
                                      _mm_slli_epi16(g, 5)), b);
        _mm_storeu_si128((__m128i*)(row + x), v);
    }
}
#endif

static void Wth_FlashFill(unsigned char* base, int pitch, int ox, int oy,
                          int w, int h, int aTop, int aBot)
{
    int y;

    if (!base || w <= 0 || h <= 0) return;
    for (y = 0; y < h; y++)
    {
        int a = (h > 1) ? aTop + (aBot - aTop) * y / (h - 1) : aTop;
        int ar, ag, ab, gmax = (g_GBits == 6) ? 0x3F : 0x1F;
        unsigned short* row;
        int x = 0;

        if (a <= 0) continue;
        ar = g_Cfg.thCol[0] * a / 255;
        ag = g_Cfg.thCol[1] * a / 255;
        ab = g_Cfg.thCol[2] * a / 255;
        ar = ar * 31 / 255;
        ag = ag * gmax / 255;
        ab = ab * 31 / 255;
        if (!ar && !ag && !ab) continue;

        row = (unsigned short*)(base + (size_t)(oy + y) * (size_t)pitch) + ox;

#ifdef WTH_HAVE_SSE2
        if (g_GBits == 6 && WthSse2Ok())
        {
            Wth_FlashRowSse2(row, w, ar, ag, ab);
            x = w & ~7;
        }
#endif
        for (; x < w; x++)
        {
            unsigned v = row[x];
            unsigned r, g, b;

            if (g_GBits == 6)
            {
                r = (v >> 11) & 0x1Fu;
                g = (v >> 5) & 0x3Fu;
                b = v & 0x1Fu;
            }
            else
            {
                r = (v >> 10) & 0x1Fu;
                g = (v >> 5) & 0x1Fu;
                b = v & 0x1Fu;
            }
            r += (unsigned)ar; if (r > 0x1Fu) r = 0x1Fu;
            g += (unsigned)ag; if (g > (unsigned)gmax) g = (unsigned)gmax;
            b += (unsigned)ab; if (b > 0x1Fu) b = 0x1Fu;
            row[x] = (unsigned short)((r << (g_GBits == 6 ? 11 : 10)) |
                                      (g << 5) | b);
        }
    }
}

/* 一次性自检: 把音效名解析到的下标打一行 —— 名字写错 (或行内注释没截掉) 时
 * 这里直接是 -1, 不用听声音猜。 */
static int s_SndLogged = 0;

static void Wth_LogSnd(void)
{
    char msg[192];
    unsigned q = 0;
    int i;

    if (s_SndLogged) return;
    s_SndLogged = 1;
    q = WTH_L_S(msg, q, "weather: sound ");
    q = WTH_L_S(msg, q, g_Cfg.rainName);
    q = WTH_L_C(msg, q, '=');
    q = WTH_L_I(msg, q, Sfx_Index(g_Cfg.rainName));
    q = WTH_L_C(msg, q, ' ');
    q = WTH_L_S(msg, q, g_Cfg.snowName);
    q = WTH_L_C(msg, q, '=');
    q = WTH_L_I(msg, q, Sfx_Index(g_Cfg.snowName));
    for (i = 0; i < WTH_TH_SND_N; i++)
    {
        q = WTH_L_C(msg, q, ' ');
        q = WTH_L_S(msg, q, g_Cfg.thSnd[i]);
        q = WTH_L_C(msg, q, '=');
        q = WTH_L_I(msg, q, Sfx_Index(g_Cfg.thSnd[i]));
    }
    LogLine(msg);
}

/* 打一道闪电: 生成电弧 + 排一声雷。落点可以在视图外 —— 声像跟着落点走 (看见
 * 哪边闪就是哪边响), "距离"只影响延迟与音量, 屏外那道还有额外的衰减。 */
static void Wth_Strike(void)
{
    int dist = Wth_Range(0, 255);
    int x, off, start, k;

    Wth_BoltGen();
    g_Bolt.on = 1;
    g_Bolt.age = 0;
    x = g_Bolt.mx[WTH_BLT_NODE - 1];
    off = (x < 0) ? -x : (x > 1023 ? x - 1023 : 0);
    if (off > WTH_BLT_OFF) off = WTH_BLT_OFF;
    /* 落点越出视图后, 提亮逐渐压到三成: 天边的雷只剩一声闷响与一层微光。 */
    g_Bolt.prox = 255 - off * 180 / WTH_BLT_OFF;

    g_Storm.sndPan = 0x2000 + (x - 512) * 0x4000 / 1023;
    if (g_Storm.sndPan < 0) g_Storm.sndPan = 0;
    if (g_Storm.sndPan > 0x4000) g_Storm.sndPan = 0x4000;
    g_Storm.sndVol = g_Cfg.thVol * (100 - dist * 35 / 255) / 100;
    g_Storm.sndVol = g_Storm.sndVol * (255 - off * 90 / WTH_BLT_OFF) / 255;

    /* 从随机起点找一个装得下的: ini 里漏写一条也不至于整场哑掉。 */
    g_Storm.sndIdx = -1;
    start = Wth_Range(0, WTH_TH_SND_N - 1);
    for (k = 0; k < WTH_TH_SND_N; k++)
    {
        int c = (start + k) % WTH_TH_SND_N;

        if (Sfx_Index(g_Cfg.thSnd[c]) >= 0) { g_Storm.sndIdx = c; break; }
    }

    g_Storm.sndAt = (unsigned)g_Tick + WTH_TH_DLY
                    + (unsigned)(dist * 18 / 255 + off * 12 / WTH_BLT_OFF);
}

/* ---------------- 换局 / 读档 ----------------
 * 规则: 进对局时按**地图**判 —— 与上一局同一张图就整套保留 (开关、雷电、雨声),
 * 换了地图全部复位。场景指针每次开局都会换 (读同一张图也换), 比不出"是不是同
 * 一张图", 所以比 OU_ScenarioName (Name, 没写则 UIName; 读档后 FileName 是存档
 * 名, 更不能比)。 */

/* 进对局那一拍: ScenarioStarted 的 0->1 边沿, 或者帧号倒退 (每局开始把帧号写 0)。
 * 读档时主循环停着, 0->1 那一拍轮不到这里, 只能靠帧号倒退认 (见 Wth_LogicDelta)。 */
static void Wth_SceneCheck(void)
{
    static wchar_t s_Last[OU_SCEN_NAME_MAX];
    static int s_Seen = 0, s_WasOn = 0;
    int started = *(volatile unsigned char*)(size_t)ADDR_SCEN_STARTED;
    int enter = (started && !s_WasOn) || g_Rewound;
    wchar_t name[OU_SCEN_NAME_MAX];
    int i;

    s_WasOn = started;
    if (!enter || !OU_ScenarioName(name)) return;

    if (s_Seen && lstrcmpW(name, s_Last))
    {
        /* 换了地图: 天气全部收掉, 与新一局"默认无天气"一致。 */
        Wth_SndReset();
        for (i = 0; i < WTH_K_N; i++) g_On[i] = 0;
        g_Thunder = 0;
        g_PxOk = 0;
        LogLine("weather: reset (new map)");
    }
    else
    {
        /* 同一张图: 开关与状态都保留, 只把雷电重新计时; 引擎读档时销毁了全部
         * 音频事件, 循环音的标记要清掉才会重新起 (不然就是"天气在, 声音没了")。
         * 不能走 Wth_LoopStop —— 通道句柄已随引擎销毁, 只清标记。 */
        Wth_ThunderStop();
        g_Storm.rain.started = 0;
        g_Storm.rain.retry = 0;
        g_Storm.wind.started = 0;
        g_Storm.wind.retry = 0;
    }
    lstrcpynW(s_Last, name, OU_SCEN_NAME_MAX);
    s_Seen = 1;
}

/* 每帧的声音与雷电状态机。d = 这一帧过了几个逻辑帧 (0 = 暂停/卡住)。
 *
 * 开头那段与"开关"无关地跑, 因为它要负责把雨声收掉 —— 早退之后再判断就永远
 * 收不了 (全部关掉时 Weather_Draw 直接 return)。 */
static void Wth_SndTick(int d)
{
    unsigned now;

    Wth_SceneCheck();

    if (g_Bolt.on)
    {
        g_Bolt.age += d;
        if (g_Bolt.age >= WTH_BLT_LIFE)
        {
            g_Bolt.on = 0;
            g_Bolt.age = 0;
        }
    }

    /* GtTickNow 一个 tick = 16ms (timeGetTime() >> 4), 这里统一换成毫秒。
     * 只有循环音的重试退避用它, 雷雨时间线走 g_Tick (逻辑帧)。 */
    now = GtTickNow() * 16u;

    /* 循环音各跟各的开关 ("安静的雪" 用 ini 关)。条目自带 5~10 秒 Delay,
     * 开雪后过几秒才有第一声是正常的。 */
    Wth_LoopTick(&g_Storm.rain,
                 g_On[WTH_K_RAIN] && g_Cfg.rainSnd && g_Cfg.rainVol > 0, now);
    Wth_LoopTick(&g_Storm.wind,
                 g_On[WTH_K_SNOW] && g_Cfg.snowSnd && g_Cfg.snowVol > 0, now);

    /* 雷电是独立开关: 不下雨也能打雷 (光开雷电就是"干雷暴")。 */
    if (!g_Thunder)
    {
        Wth_ThunderStop();
        return;
    }
    /* 暂停 (逻辑帧不动) 时不推进: 时间线是帧号, 不拦会"暂停时还在打雷"。 */
    if (d <= 0) return;

    if (!g_Storm.on)
    {
        g_Storm.on = 1;
        g_Storm.next = (unsigned)g_Tick + WTH_TH_FIRST;
        Wth_LogSnd();
        return;
    }
    if (g_Storm.sndAt && (int)((unsigned)g_Tick - g_Storm.sndAt) >= 0)
    {
        g_Storm.sndAt = 0;
        if (g_Storm.sndIdx >= 0)
            Sfx_Play(g_Cfg.thSnd[g_Storm.sndIdx], g_Storm.sndPan, g_Storm.sndVol);
    }
    if ((int)((unsigned)g_Tick - g_Storm.next) >= 0)
    {
        Wth_Strike();
        g_Storm.next = (unsigned)g_Tick + (unsigned)Wth_Range(g_Cfg.thLo, g_Cfg.thHi);
    }
}

/* ---------------- 每帧入口 ---------------- */

int Weather_Draw(void)
{
    RectangleStruct clip;
    void* surf;
    unsigned mask = 0;
    int cdx = 0, cdy = 0, d, dp, i, drew = 0;
    int surfW, surfH, surfPitch;
    int needPx;

    Wth_Init();
    Wth_CamStep(&cdx, &cdy);

    /* 图像层的锚点每帧都跟相机走 (哪怕这一档没开), 打开时才不会跳一下。 */
    for (i = 0; i < 2; i++)
    {
        g_Layer[i].scrollX += cdx;
        g_Layer[i].scrollY += cdy;
    }

    d = Wth_LogicDelta();
    g_Tick += d;
    dp = (d > 4) ? 4 : d;   /* 渲染远慢于逻辑时别让粒子一步跨过整屏 */

    /* 声音/雷电状态机要在"全关早退"之前跑: 雨停了要负责把雨声收掉。 */
    Wth_SndTick(d);
    Wth_LightTick();            /* 雨/雪颜色跟环境光照走, 每帧重算一遍 */

    /* 雷电可以单独开 (不必下雨), 所以它也算"这一帧有事要做"。 */
    if (!g_On[WTH_K_RAIN] && !g_On[WTH_K_SNOW] && !g_On[WTH_K_FIRE] &&
        g_CloudMode == WEATHER_CLOUD_OFF && !(g_Thunder && g_Bolt.on))
        return 0;

    /* 目标面 = 本帧"未缩放视口原图" (与无UI截图同源): 1x/放大档是窗口
     * Composite (放大档取放大之前那块 R), 缩小档是大面。天气画进原图,
     * 后面的缩放呈现与截图才都带着它。 */
    if (!SB_RawView(&surf, &clip.X, &clip.Y, &clip.Width, &clip.Height))
        return 0;
    surfW = GtSurfGet(surf, GtVT_GET_WIDTH);
    surfH = GtSurfGet(surf, GtVT_GET_HEIGHT);
    surfPitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (surfW <= 0 || surfH <= 0 || surfPitch <= 0) return 0;
    if (clip.Width < 16 || clip.Height < 16) return 0;
    if (clip.X < 0 || clip.Y < 0 || clip.X > surfW || clip.Y > surfH ||
        clip.Width > surfW - clip.X || clip.Height > surfH - clip.Y) return 0;

    /* d / g_Tick / dp 只算一次: Wth_LogicDelta 会推进帧号基准, 再调一次必返回 0,
     * 雨雪就不动了。 */

    /* 这一帧哪些 kind 走粒子落笔 (规则见 Wth_KindOn)。落地溅射、涟漪都挂在
     * 同一份 mask 上 —— 图像档的 RAIN/SNOW.SHP 自带落地水花, 再叠一层我们的
     * 粒子只会显重复。 */
    for (i = 0; i < WTH_K_N; i++)
        if (Wth_KindOn(i)) mask |= 1u << i;
    needPx = mask != 0;

    /* 云层画在降水之下, 免得雨雪被混两遍; 它是独立开关, 不跟着雨/雪走。 */
    if (g_CloudMode != WEATHER_CLOUD_OFF && g_Cfg.cloudOpacity > 0 && surfPitch >= surfW * 2)
    {
        long long tCld = Perf_Now();

        /* 云图采样/首次烘焙不依赖渲染面, 放在锁外避免长时间占用引擎表面。 */
        if (Wth_CloudEnsure())
        {
            unsigned char* cbase = GtLock(surf);

            if (cbase)
            {
                Wth_CloudFill(cbase, surfPitch, clip.X, clip.Y,
                              clip.Width, clip.Height);
                GtUnlock(surf);
                drew = 1;
            }
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

                if (!g_On[l->kind]) continue;
                if (!Wth_Ensure(l)) continue;
                Wth_Advance(l, d);
                Wth_DrawLayer(l, surf, pal, &clip);
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
            Wth_LandStep(dp, cdx, cdy, clip.Height);
            /* 落点节拍: 铺满整个视图, 按 mask 里的 kind 走 (与粒子同一份集合)。 */
            for (i = 0; i < WTH_K_N; i++)
                if ((mask & (1u << i)) && g_Kind[i].splashHi > 0)
                    Wth_LandTick(i, dp, clip.Width, clip.Height);
            Wth_PxStep(dp, clip.Width, clip.Height, cdx, cdy, mask);
            Wth_SmokeStep(dp, cdx, cdy);
            /* 烟先画: 火球压在烟上面才有"烧穿"的层次。 */
            Wth_PxSmoke(base, surfPitch, clip.X, clip.Y, clip.Width, clip.Height);
            Wth_PxDraw(base, surfPitch, clip.X, clip.Y, clip.Width, clip.Height, mask);
            Wth_PxRip(base, surfPitch, clip.X, clip.Y, clip.Width, clip.Height);
            Wth_PxDrop(base, surfPitch, clip.X, clip.Y, clip.Width, clip.Height);
            GtUnlock(surf);
            drew = 1;
        }
    }

    /* 雷电画在最后: 闪电是整屏的一次曝光, 压在雨丝之上 (写在下面像在雨幕后面
     * 打手电)。提亮从视图顶到底衰减 —— 光是从天上来的。 */
    if (g_Bolt.on && surfPitch >= surfW * 2)
    {
        unsigned char* b2 = GtLock(surf);

        if (b2)
        {
            int fa = kFlashA[g_Bolt.age] * g_Cfg.thFlash / 255;

            fa = fa * g_Bolt.prox / 255;    /* 屏外那道只留一层微光 */
            if (fa > 0)
                Wth_FlashFill(b2, surfPitch, clip.X, clip.Y, clip.Width,
                              clip.Height, fa, fa * 45 / 100);
            if (g_Cfg.thBolt)
                Wth_BoltDraw(b2, surfPitch, clip.X, clip.Y, clip.Width,
                             clip.Height);
            GtUnlock(surf);
            drew = 1;
        }
    }
    return drew;
}
