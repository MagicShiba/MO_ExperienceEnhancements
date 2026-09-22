/* VecText.cpp - 全局文本矢量接管 (超级武器列表/简报/按钮/顶栏数字等)。
 *
 * 接管点: BitFont::Blit (0x434120, 7B)。DrawText/度量/换行/对齐全部保留,
 * 版式零风险。game.fnt 格式不用管: 字宽来源是引擎自己的 GetTextDimension。
 *
 * 渲染 (Render=1): 跳过原版 1bpp 光栅, GDI 灰度覆盖率经 565/555 校准
 *   混入已 Lock 的 16 位面, advance 与原版逐字一致。
 * 任何异常 (度量失败/面信息非法/字形超大) 都走跳板调原版, 绝不花屏。
 *
 * 场景区分只剩一条: 顶栏条带 (Blit 层 y<32 的数字吃 CreditPx/DY)。
 *   函数白名单已删 (顶栏函数也会画超武列表等, 函数粒度分不开导致串扰)。
 * 字号: 全局 FontPx (墨水与字宽等比) > 自动校准 (墨水对原墨水高)。
 * 摆位整体垂直居中 (整块在格内上下均分, 不关心字体多高)。
 * 简报正文: 盒子加高 (BriefBox) + 裁剪底同步 (BriefClip, 带 pitch 安全阀)。
 *
 * BitFont 对象实测布局 (Lock 0x4348F0 反汇编实证):
 *   +0x0C 锁定的 WORD 像素缓冲, +0x10 pitch(WORD 数),
 *   +0x24 默认色 WORD, +0x30..+0x3C 裁剪 LTRB (含端)。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MsgFont 同一 DLL)。
 */
#include <windows.h>
#include "GdiText.h"
#include "VecText.h"

#define VT_SECTION       "VecText"
#define ADDR_BLIT        0x00434120u
#define ADDR_BLIT_BACK   0x00434127u
#define ADDR_GETDIM      0x00433CF0u
#define ADDR_DRAWTEXT    0x00434CD0u
#define ADDR_DRAWTEXT_BACK 0x00434CD6u
#define ADDR_PRINT       0x00434B90u
#define ADDR_PRINT_BACK  0x00434B9Au

/* Blit 头 7 字节 (sub esp,0x30 + push ebx/ebp/esi/edi), 下钩前校验 */
static const unsigned char kWantBlit[7] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x57 };
/* DrawText 头 6 字节 (sub esp,0x30 + push ebx/ebp/esi) */
static const unsigned char kWantDrawText[6] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56 };
/* Print 头 10 字节 (push esi + mov esi,[esp+8] + push edi + mov edi,[esp+0x18]) */
static const unsigned char kWantPrint[10] = { 0x56, 0x8B, 0x74, 0x24, 0x08,
                                              0x57, 0x8B, 0x7C, 0x24, 0x18 };

/* 场景区分只认位置条带 (Blit 层 y<32 的数字, 见 BlitHook), 不认函数。
 * 函数白名单已删除: 顶栏那几个函数也会画别的文本 (如按 House 遍历的
 * 函数连超武列表一起画), 函数粒度分不开, CreditPx 漏过去造成列表
 * 大小闪烁 (vlen 实证: 同一调用者既画顶栏也画它处小字)。
 * 发起者查找 (栈上找 + call 指令校验) 保留, 只给诊断日志 (vcall) 用。 */

typedef struct {
    int Enable;
    char FontFace[64];
    int FontPx;     /* 全局字号: 0=自动校准; >0 固定像素, 墨水与字宽等比缩放 */
    int CreditPx;   /* 顶栏条带数字单独字号 (0=跟随全局; >0 覆盖, 仅换墨水字宽不动) */
    int CreditDY;   /* 顶栏数字垂直偏移 (像素, 负=上移) */
    int SuperPx;    /* 超武列表接管字号 (0=关闭, 原样; >0 抑制原字形后重画) */
    int BriefBox;   /* 1=简报正文盒加高 (界面适配字体), 0=保持原版盒子 */
    int BriefClip;  /* 1=简报裁剪底边跟盒子走 (面板子面切，需配合 BriefBox) */
    int Bold;
    int VShift;     /* 字形垂直偏移 (像素, 正=下移) */
    int LogAdv;
    int Render;
} VTConfig;

static VTConfig g_VT;
static wchar_t g_VTFace[FONT_FACE_MAX];

/* thiscall (ECX=this): __fastcall 占位 (this, edxUnused) + 栈上 4 参数,
 * 编译器生成 ret 0x10, 与原 Blit 尾完全一致, JMP 钩可直接跳入。 */
typedef int (__fastcall *BlitFn)(void* t, void* edxUnused,
                                 int wch, int x, int y, int color);
static BlitFn g_OrigBlit = NULL;

typedef int (__fastcall *GetDimFn)(void* t, void* edxUnused,
                                   const wchar_t* s, int* w, int* h, int maxW);
static GetDimFn g_GetDim = NULL;

/* ---------------- 入口钩 ----------------
 * DrawText/Print 入口只做诊断记录 (vcall) 与简报盒补丁;
 * 字号在 Blit 层定: 全局 FontPx > 自动校准, 顶栏条带数字另吃 CreditPx。
 * 转发必须原样 (参数个数决定 ret 清栈数, 类型无所谓, 全 4 字节):
 * DrawText = thiscall + 10 个栈参 (ret 0x28, 反汇编实证),
 * Print = thiscall + 7 个栈参 (ret 0x1c)。
 * 管线内层调用 (Simple/Fancy/DrawText/Print 互调) 在栈上找真正的
 * 发起者 (须过 call 指令校验), 只为诊断日志 (vcall outer) 服务。 */

typedef int (__fastcall *DrawTextFn)(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5,
    int a6, int a7, int a8, int a9, int a10);
static DrawTextFn g_OrigDrawText = NULL;

typedef int (__fastcall *PrintFn)(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7);
static PrintFn g_OrigPrint = NULL;

/* 文本管线内部地址段 (互调不刷新上下文) */
static int InPipeline(unsigned caller)
{
    if (caller >= 0x00434100u && caller < 0x00435600u) return 1;   /* BitFont/BitText */
    if (caller >= 0x004A5E00u && caller < 0x004A6900u) return 1;   /* Simple/Fancy */
    return 0;
}

/* 返回地址校验: 真正的调用返回点前面必是 call 指令。
 * E8 (近调用 5B) / FF D0-D7 (call *reg 2B) / FF 15 (call *m32 6B)。
 * 栈上掃到的整数经常恰好落在代码段内 (如顶栏某次掃到 0x6D005C,
 * 反汇编一看落在 cmp 指令中间, 是数据不是返回地址),
 * 不校验会张冠李戴, 场景判全错。 */
static int VecIsRetAddr(unsigned c)
{
    unsigned char* p;
    if (c < 0x00401008u || c >= 0x007E0400u) return 0;
    p = (unsigned char*)c;
    if (p[-5] == 0xE8) return 1;                       /* call rel32 */
    if (p[-6] == 0xFF && p[-5] == 0x15) return 1;      /* call *m32 */
    if (p[-2] == 0xFF && ((p[-1] >> 3) & 7u) == 2) return 1;  /* call *reg */
    if (p[-3] == 0xFF && ((p[-2] >> 3) & 7u) == 2) return 1;  /* call *[reg+disp8] */
    if (p[-4] == 0xFF && ((p[-3] >> 3) & 7u) == 2) return 1;  /* call *[reg+disp32] */
    return 0;
}

/* 栈上找发起者: 从当前 esp 往上扫, 第一个落在游戏代码段
 * (0x00400000~0x00900000) 且不在管线段内、且通过返回地址校验的。
 * 整数参数都是小值, 指针都在代码段外, 找不到返回 0。 */
static unsigned VecFindOuter(void)
{
    unsigned esp, i;
    __asm__ volatile ("movl %%esp, %0" : "=r"(esp));
    for (i = 0; i < 256; i++)
    {
        unsigned v;
        if (!IsPlausiblePtr((void*)esp)) break;
        v = *(unsigned*)esp;
        if (v >= 0x00400000u && v < 0x00900000u && !InPipeline(v) &&
            VecIsRetAddr(v))
            return v;
        esp += 4;
    }
    return 0;
}

/* 调用者普查: 每个不同调用者记一样本 (字符串前 4 字 + 次数), 上限 256。
 * 用途: 反查顶栏/列表等文本真正的绘制点。 */
#define CALLER_MASK 255
typedef struct {
    int used;
    unsigned caller;
    unsigned outer;    /* 直接调用者在管线内时, 栈上找的发起者 (0=无/直达) */
    char entry;      /* 'D'=DrawText, 'P'=Print */
    wchar_t s0, s1, s2, s3;
    unsigned hits;
} CallerEnt;
static CallerEnt g_Callers[CALLER_MASK + 1];
static int g_CallerN = 0;

static void VecCallerLog(char entry, unsigned caller, int a3str)
{
    unsigned idx = (caller >> 2) & CALLER_MASK;
    CallerEnt* e;
    const wchar_t* s;
    unsigned outer = 0;
    int probe;
    /* 同一入口不同发起者必须分开记: 顶栏 Print 都经 Simple 同一位置,
     * 发起者不同 (已见 0x4D002E 与其它)。以前只记第一个, 把真正的顶栏漏了。
     * 哈希冲突线性探 (16 步), 找不到空位就放弃。 */
    if (InPipeline(caller)) outer = VecFindOuter();
    e = NULL;
    for (probe = 0; probe < 16; probe++)
    {
        CallerEnt* c = &g_Callers[(idx + probe) & CALLER_MASK];
        if (c->used && c->caller == caller && c->entry == entry &&
            c->outer == outer) { c->hits++; return; }
        if (!c->used && !e) e = c;
    }
    if (!e) return;
    if (g_CallerN >= 256) return;
    s = (const wchar_t*)a3str;
    e->used = 1; e->caller = caller; e->outer = outer;
    e->entry = entry; e->hits = 1;
    e->s0 = e->s1 = e->s2 = e->s3 = 0;
    if (s && IsPlausiblePtr(s))
    {
        /* 逐字验指针, 遇到 0 即停 (字符串可能跨页) */
        e->s0 = s[0];
        if (s[0]) e->s1 = s[1];
        if (s[0] && s[1]) e->s2 = s[2];
        if (s[0] && s[1] && s[2]) e->s3 = s[3];
    }
    g_CallerN++;
    {
        char b1[16], buf[160];
        unsigned p = 0;
        p = AppendCStr(buf, p, "vcall: ", sizeof(buf));
        p = AppendCh(buf, p, entry, sizeof(buf));
        p = AppendCStr(buf, p, " caller=", sizeof(buf));
        p += HtoA(caller, buf + p);
        p = AppendCStr(buf, p, " outer=", sizeof(buf));
        p += HtoA(outer, buf + p);
        p = AppendCStr(buf, p, " s=", sizeof(buf));
        UtoA((unsigned)e->s0 & 0xFFFFu, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, ',', sizeof(buf));
        UtoA((unsigned)e->s1 & 0xFFFFu, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, ',', sizeof(buf));
        UtoA((unsigned)e->s2 & 0xFFFFu, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, ',', sizeof(buf));
        UtoA((unsigned)e->s3 & 0xFFFFu, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        LogLine(buf);
    }
}

/* VecCtxSet 已删除 (函数白名单上下文本来就会把 CreditPx 漏给
 * 同一函数画的其它文本, 如超武列表; 顶栏只认条带, 不需要上下文)。
 * 入口钩只留诊断与简报补丁。 */

/* 简报/列表 DrawText 记录: 调用者 + 10 个栈参 (+矩形 X/Y/W/H 解码)。
 * 连续相同参数是逐帧重画, 只记一次 (省 cap 给不同的行)。
 * (X,Y,W,H) = (a4,a5,a6,a7), 字符串在 a3, 已用 t= 样本实证。 */
static int g_BriefLogs = 0;
static unsigned g_BriefPrevCaller = 0;
static int g_BriefPrevA[10] = { 0 };
static unsigned g_BriefRep = 0;

static int VecIsBriefCaller(unsigned caller)
{
    if (caller >= 0x00479000u && caller < 0x00479100u) return 1;
    if (caller == 0x00621144u) return 1;
    return 0;
}

static void VecBriefLog(unsigned caller, int* a, void* t)
{
    char b1[16], buf[384];
    unsigned p, i;
    (void)t;
    /* 连续相同参数是逐帧重画, 只记一次 (省 cap 给不同的行) */
    if (caller == g_BriefPrevCaller)
    {
        for (i = 0; i < 10; i++)
            if (a[i] != g_BriefPrevA[i]) break;
        if (i == 10) { g_BriefRep++; return; }
    }
    if (g_BriefLogs >= 48) return;
    g_BriefLogs++;
    if (g_BriefRep)
    {
        char rb[16], rbuf[64];
        unsigned q = 0;
        q = AppendCStr(rbuf, q, "vbrief: rep=", sizeof(rbuf));
        UtoA(g_BriefRep, rb); q = AppendCStr(rbuf, q, rb, sizeof(rbuf));
        LogLine(rbuf);
        g_BriefRep = 0;
    }
    g_BriefPrevCaller = caller;
    for (i = 0; i < 10; i++) g_BriefPrevA[i] = a[i];
    p = 0;
    p = AppendCStr(buf, p, "vbrief: caller=", sizeof(buf));
    p += HtoA(caller, buf + p);
    p = AppendCStr(buf, p, " a=", sizeof(buf));
    for (i = 0; i < 10; i++)
    {
        UtoA((unsigned)a[i], b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCh(buf, p, (i == 9) ? ' ' : ',', sizeof(buf));
    }
    LogLine(buf);
}

/* DrawText 字形计数 (vlen 用): 入口钩在前, 定义在前, 探针段只引用 */
static unsigned g_BlitN = 0;
static int g_DTDepth = 0;
static unsigned g_DTBlit0 = 0;
static unsigned g_VlenPrevCaller = 0;
static int g_VlenPrevA[10] = { 0 };
/* 行跨度统计 (vrows 用): 最外层是简报绘制时才记 */
static int g_DTBrief = 0;
static int g_YMin = 0, g_YMax = 0, g_YOffGrid = 0;
static int g_ClipLogged = 0;

/* 简报正文盒 (0x621144 那次大 DrawText): 实测 442 字左右塞进
 * (652,313,453,432), 约 23 行 x 19 = 437px, 底边超出盒底被切,
 * 原版矮墨水蹭过去看不见, 满格墨水脚被吃 -> "最后一行不全"。
 * 修法: 只加高 432->470, 宽度保持 453 不动。
 * 教训: W 试过 453->540, 行是变长了, 但右侧被面板底面切掉
 * (文字画在一块 453 宽的面板子面上, 不是画在屏幕上;
 * 加宽文字不加宽面板 = 右边被吃, 还丢字)。
 * 真要加宽得改面板控件本身, 那是另一项大工程, 先保正确。
 * 盒底 783 < 1080, 下面全是黑边, 零风险。 */
#define BRIEF_X 652
#define BRIEF_Y 313
#define BRIEF_W 453
#define BRIEF_H 432
#define BRIEF_W2 453
#define BRIEF_H2 470

static int g_VBoxLogged = 0;

static void VecBriefBox(int* aX, int* aY, int* aW, int* aH)
{
    char b1[16], buf[128];
    unsigned p;
    if (!g_VT.BriefBox) return;
    if (*aX != BRIEF_X || *aY != BRIEF_Y || *aW != BRIEF_W || *aH != BRIEF_H)
        return;
    *aW = BRIEF_W2;
    *aH = BRIEF_H2;
    if (g_VBoxLogged) return;
    g_VBoxLogged = 1;
    p = 0;
    p = AppendCStr(buf, p, "vbox: brief body ", sizeof(buf));
    UtoA((unsigned)BRIEF_W, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, 'x', sizeof(buf));
    UtoA((unsigned)BRIEF_H, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " -> ", sizeof(buf));
    UtoA((unsigned)BRIEF_W2, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, 'x', sizeof(buf));
    UtoA((unsigned)BRIEF_H2, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

/* ---------------- 超武列表接管 (SuperPx) ----------------
 * 整体区域接管, 不分路径: 名 (Print) 与计时器 (DrawText) 都画在右侧
 * 面板区, 凡 Blit 落在这个区的一律隐藏原字形, 按 SuperPx 自定义重绘
 * (同锁同裁剪, 笔位/背景/后续全是引擎的)。
 * 区域 = 主面 (pitch 1024~4096) 且 x>=0.83 屏宽且 y>=32。
 * 顶栏 (y<32)、简报 (x~652)、菜单 (居中) 天然排除。
 * SuperPx=0 (默认) 全不碰。 */
static int VecPickPx(void* t);
static int VecRender(void* t, wchar_t wch, int adv, int h, int x, int y,
                     int colorW, int px, int yOff);
/* 原版格高 (idata Lines): FontPx/SuperPx 等比缩放字宽时的分母 (定义提前,
 * 接管渲染要用; 后面不再重复定义) */
static int g_FontOrigH = 19;
/* 超武区接管失败计数 (诊断用, 上限 8 行日志) */
static int g_SWFailN = 0;

/* 小裁剪探针 (菜单按钮等): clip 特别窄/矮的 Blit 记几何+颜色+发起者,
 * 一次定案按钮字去哪了。上限 12。 */
static int g_ClipProbeN = 0;

static void VecClipProbe(void* t, wchar_t wc, int x, int y, int adv, int h,
                         int eff, int color)
{
    int L, Tp, R, B, pitchW;
    unsigned outer;
    char b1[16], buf[192];
    unsigned p;
    if (g_ClipProbeN >= 12) return;
    if (!t || !IsPlausiblePtr(t)) return;
    L = *(int*)((char*)t + 0x30); Tp = *(int*)((char*)t + 0x34);
    R = *(int*)((char*)t + 0x38); B = *(int*)((char*)t + 0x3C);
    pitchW = *(int*)((char*)t + 0x10);
    if (B - Tp >= 20 && R - L >= 120) return;   /* 正常大面不记 */
    g_ClipProbeN++;
    outer = VecFindOuter();
    p = 0;
    p = AppendCStr(buf, p, "vgeo: wc=", sizeof(buf));
    p += HtoA((unsigned)wc & 0xFFFFu, buf + p);
    p = AppendCStr(buf, p, " xy=", sizeof(buf));
    UtoA((unsigned)x, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, ',', sizeof(buf));
    UtoA((unsigned)y, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " adv=", sizeof(buf));
    UtoA((unsigned)adv, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " h=", sizeof(buf));
    UtoA((unsigned)h, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " clip=", sizeof(buf));
    UtoA((unsigned)L, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, ',', sizeof(buf));
    UtoA((unsigned)Tp, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, ',', sizeof(buf));
    UtoA((unsigned)R, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, ',', sizeof(buf));
    UtoA((unsigned)B, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " pitch=", sizeof(buf));
    UtoA((unsigned)pitchW, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " eff=", sizeof(buf));
    p += HtoA((unsigned)eff & 0xFFFFu, buf + p);
    p = AppendCStr(buf, p, " outer=", sizeof(buf));
    p += HtoA(outer, buf + p);
    LogLine(buf);
}
/* 超武区整体接管判定 (Blit 层, 无状态, 不分路径):
 * 右侧面板区 (主面 pitch 1024~4096, x>=0.83 屏宽, y>=32) 的字一律命中。
 * 名 (Print) 与计时器 (DrawText) 都落在这个区, 到这就接管。
 * 顶栏 (y<32)、简报 (x~652)、菜单 (居中) 天然排除。
 * SuperPx=0 全不碰。 */
static int VecSWRegion(void* t, int x, int y)
{
    int pitchW;
    if (g_VT.SuperPx <= 0) return 0;
    if (!t || !IsPlausiblePtr(t)) return 0;
    pitchW = *(int*)((char*)t + 0x10);
    if (pitchW < 1024 || pitchW > 4096) return 0;
    if (y < 32) return 0;
    if (x * 100 < pitchW * 83) return 0;
    return 1;
}

/* 右侧带诊断: 落入 0.80 网即记 (命中/脱靶), 上限 12, 一次定案版式位置 */
static int g_SWBandN = 0;

static void VecSWBandLog(wchar_t wc, int x, int y, int pitchW)
{
    char b1[16], buf[128];
    unsigned p;
    int hit;
    if (g_SWBandN >= 12) return;
    g_SWBandN++;
    hit = (x * 100 >= pitchW * 83) ? 1 : 0;
    p = 0;
    p = AppendCStr(buf, p, "vswall: wc=", sizeof(buf));
    p += HtoA((unsigned)wc & 0xFFFFu, buf + p);
    p = AppendCStr(buf, p, " xy=", sizeof(buf));
    UtoA((unsigned)x, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, ',', sizeof(buf));
    UtoA((unsigned)y, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " pitch=", sizeof(buf));
    UtoA((unsigned)pitchW, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " hit=", sizeof(buf));
    UtoA((unsigned)hit, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

/* (捕获/重画/入口发现已删除: 改当场渲染+Blit 层判决, 见上) */

int __fastcall VecDrawTextEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5,
    int a6, int a7, int a8, int a9, int a10)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int brief = 0, r, k;
    int av[10];
    if (g_VT.LogAdv) VecCallerLog('D', caller, a3);
    brief = VecIsBriefCaller(caller);
    av[0] = a1; av[1] = a2; av[2] = a3; av[3] = a4; av[4] = a5;
    av[5] = a6; av[6] = a7; av[7] = a8; av[8] = a9; av[9] = a10;
    if (brief) VecBriefLog(caller, av, t);
    if (!g_OrigDrawText) return 0;
    if (brief)
    {
        /* 简报正文盒加高 (界面适配字体, 精确匹配才改) */
        if (caller == 0x00621144u) VecBriefBox(&a4, &a5, &a6, &a7);
        g_DTDepth++;
        if (g_DTDepth == 1) { g_DTBlit0 = g_BlitN; g_DTBrief = 1; }
    }
    r = g_OrigDrawText(t, NULL, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    if (brief)
    {
        unsigned n;
        g_DTDepth--;
        if (g_DTDepth != 0) return r;
        g_DTBrief = 0;
        n = g_BlitN - g_DTBlit0;
        if (g_BriefLogs < 48)
        {
            /* 本次画了几个字形 (最外层才记): 矩形 + 字数 = 换行 spill 定案。
             * 同一参数集只记一次 (与 vbrief 去重对齐, 重画不占额度)。 */
            int same = (caller == g_VlenPrevCaller);
            if (same)
                for (k = 0; k < 10; k++)
                    if (av[k] != g_VlenPrevA[k]) { same = 0; break; }
            if (!same)
            {
                char b1[16], buf[96];
                unsigned p = 0;
                g_BriefLogs++;
                g_VlenPrevCaller = caller;
                for (k = 0; k < 10; k++) g_VlenPrevA[k] = av[k];
                p = AppendCStr(buf, p, "vlen: caller=", sizeof(buf));
                p += HtoA(caller, buf + p);
                p = AppendCStr(buf, p, " n=", sizeof(buf));
                UtoA(n, b1);
                p = AppendCStr(buf, p, b1, sizeof(buf));
                LogLine(buf);
                /* 行跨度跟 vlen 一起记 (首见才记, 重画不刷屏):
                 * ymin/ymax/字数/脱格数。均匀 19 步进则
                 * 行数=(ymax-ymin)/19+1, 与盒高一比, 溢出定案。 */
                if (n > 0 && g_BriefLogs < 48)
                {
                    char c1[16], cbuf[128];
                    unsigned q = 0;
                    g_BriefLogs++;
                    q = AppendCStr(cbuf, q, "vrows: caller=", sizeof(cbuf));
                    q += HtoA(caller, cbuf + q);
                    q = AppendCStr(cbuf, q, " ymin=", sizeof(cbuf));
                    UtoA((unsigned)g_YMin, c1);
                    q = AppendCStr(cbuf, q, c1, sizeof(cbuf));
                    q = AppendCStr(cbuf, q, " ymax=", sizeof(cbuf));
                    UtoA((unsigned)g_YMax, c1);
                    q = AppendCStr(cbuf, q, c1, sizeof(cbuf));
                    q = AppendCStr(cbuf, q, " n=", sizeof(cbuf));
                    UtoA(n, c1); q = AppendCStr(cbuf, q, c1, sizeof(cbuf));
                    q = AppendCStr(cbuf, q, " offgrid=", sizeof(cbuf));
                    UtoA((unsigned)g_YOffGrid, c1);
                    q = AppendCStr(cbuf, q, c1, sizeof(cbuf));
                    LogLine(cbuf);
                }
            }
        }
    }
    return r;
}

int __fastcall VecPrintEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int r;
    if (g_VT.LogAdv) VecCallerLog('P', caller, a3);
    /* 超武名走这条路, 但 Blit 层按区域整体接管, 入口不做任何事。 */
    if (!g_OrigPrint) return 0;
    r = g_OrigPrint(t, NULL, a1, a2, a3, a4, a5, a6, a7);
    return r;
}

/* ---------------- 探针记录 ---------------- */

#define PROBE_MASK 4095
#define PROBE_CAP  1200

typedef struct {
    int seen;
    wchar_t wch;
    int adv;      /* 引擎单字宽 (探针已验证 = 原版返回值 - X) */
    int gh;       /* 引擎单字高 */
    int color;    /* 有效色 WORD */
    unsigned hits;
} ProbeEnt;

static ProbeEnt g_Probe[PROBE_MASK + 1];
static int g_ProbeLogs = 0;

/* 引擎单字度量: 成功返回宽, 高进 *ph; 失败返回 -1 */
static int VecMeasure(void* t, wchar_t wch, int* ph)
{
    wchar_t s[2];
    int w = 0, h = 0;
    if (!t || !IsPlausiblePtr(t) || !g_GetDim) return -1;
    s[0] = wch; s[1] = 0;
    if (!g_GetDim(t, NULL, s, &w, &h, 10000)) return -1;
    if (w <= 0 || w > 64 || h <= 0 || h > 64) return -1;
    if (ph) *ph = h;
    return w;
}

static void VecProbeLog(wchar_t wch, int adv, int gh, int color)
{
    unsigned idx = ((unsigned)wch) & PROBE_MASK;
    ProbeEnt* e = &g_Probe[idx];
    if (e->seen && e->wch == wch) { e->hits++; return; }
    if (g_ProbeLogs >= PROBE_CAP)
    {
        if (g_ProbeLogs == PROBE_CAP)
        {
            char b1[16], buf[128];
            unsigned p = 0;
            g_ProbeLogs++;
            p = AppendCStr(buf, p, "vadv: cap 1200 quiet totalBlit=", sizeof(buf));
            UtoA(g_BlitN, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            LogLine(buf);
        }
        return;
    }
    {
        char b1[16], b2[16], b3[16], buf[160];
        unsigned p = 0;
        e->seen = 1; e->wch = wch; e->adv = adv; e->gh = gh;
        e->color = color; e->hits = 1;
        g_ProbeLogs++;
        p = AppendCStr(buf, p, "vadv: wch=", sizeof(buf));
        p += HtoA((unsigned)wch, buf + p);
        p = AppendCStr(buf, p, " adv=", sizeof(buf));
        UtoA((unsigned)adv, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " h=", sizeof(buf));
        UtoA((unsigned)gh, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCStr(buf, p, " color=", sizeof(buf));
        p += HtoA((unsigned)color & 0xFFFFu, buf + p);
        (void)b3;
        LogLine(buf);
    }
}

/* ---------------- 字形覆盖率缓存 ----------------
 * 与颜色无关 (灰度), 按 (wch, InternalData, adv) 命中。 */

#define VC_MASK 511
#define VC_MAXW 32
#define VC_MAXH 32

typedef struct {
    int used;
    wchar_t wch;
    void* idata;    /* BitFont+4, 字体重载即失效 */
    int px;         /* 校准字号, 换字号即换键 */
    int adv;
    int h;
    int outh;         /* 自然输出高度 (基准窗全高, 1:1, 不压缩) */
    unsigned char cov[VC_MAXW * VC_MAXH];
} VCEnt;

static VCEnt g_VC[VC_MASK + 1];

/* ---------------- 基准窗校准 ----------------
 * 某套 (字号/粗体/字体) 首次使用时, 渲染 永(全高 CJK)+g/j(拉丁降部),
 * 取墨水上下界的并集为自然垂直窗。之后所有字形共用同一窗口 1:1 输出,
 * 基线全字体一致, 高度不压缩 (降部超出格高的部分如实画出,
 * 由表面裁剪收尾)。y0 与正式渲染一致 (GL_Y0),
 * 量出来即用, 不猜 GDI 内部 leading。 */
#define GL_M 8
#define GL_Y0 8

static int g_RefValid = 0;
static int g_RefPx = -1, g_RefBold = -1, g_RefT = 0, g_RefB = 0;
static wchar_t g_RefFace[FONT_FACE_MAX];

static void VecRefScan(wchar_t ch, int* pT, int* pB)
{
    int y, x, gw, scanW;
    unsigned char* bits;
    int pitch;
    if (GtRenderGlyphAt(ch, GL_M, GL_Y0, NULL) <= 0) return;
    gw = 0;
    {
        SIZE sz;
        if (g_G && g_G->dc && GetTextExtentPoint32W(g_G->dc, &ch, 1, &sz))
            gw = sz.cx;
    }
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    bits = g_G->bits;
    pitch = g_G->pitch;
    for (y = 0; y < DIB_H; y++)
    {
        unsigned char* row = bits + y * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (y < *pT) *pT = y; if (y > *pB) *pB = y; }
    }
}

static void VecRefCalibrate(int px)
{
    int T, B;
    char b1[16], b2[16], b3[16], buf[160];
    unsigned p;
    if (g_RefValid && g_RefPx == px && g_RefBold == g_VT.Bold &&
        FaceSame(g_RefFace, g_VTFace)) return;
    T = DIB_H; B = -1;
    VecRefScan(0x6C38, &T, &B);   /* 永 */
    VecRefScan(0x0067, &T, &B);   /* g */
    VecRefScan(0x006A, &T, &B);   /* j */
    if (B < T) { T = GL_Y0; B = GL_Y0 + px - 1; }   /* 量不到就退化为等同 */
    g_RefValid = 1; g_RefPx = px; g_RefBold = g_VT.Bold;
    g_RefT = T; g_RefB = B;
    CopyFace(g_RefFace, g_VTFace);
    p = 0;
    p = AppendCStr(buf, p, "vref: px=", sizeof(buf));
    UtoA((unsigned)px, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " T=", sizeof(buf));
    UtoA((unsigned)T, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
    p = AppendCStr(buf, p, " B=", sizeof(buf));
    UtoA((unsigned)B, b3); p = AppendCStr(buf, p, b3, sizeof(buf));
    LogLine(buf);
}

/* 几何补丁: 简报布局行步进跟 19 高字格。
 * 0x478D78 add ebp,0x10 / 0x478D94 sub eax,0x10: 原版 16,
 * 与 19 高 fnt 的 DrawText 换行 (BitFont+0x1C=19) 对齐, 防布局/绘制漂移。 */

#define ADDR_BRIEF_STEP1   0x00478D78u
#define ADDR_BRIEF_STEP2   0x00478D94u

static int PatchImm8(unsigned at, unsigned char op0, unsigned char op1,
                     int val, const char* tag)
{
    unsigned char* p = (unsigned char*)at;
    DWORD old;
    char b1[16], buf[128];
    unsigned q;
    if (val < 8 || val > 40) return 0;
    if (p[0] != op0 || p[1] != op1)
    {
        q = 0;
        q = AppendCStr(buf, q, "vstep: ", sizeof(buf));
        q = AppendCStr(buf, q, tag, sizeof(buf));
        q = AppendCStr(buf, q, " bytes mismatch, skip", sizeof(buf));
        LogLine(buf);
        return 0;
    }
    if (p[2] == (unsigned char)val) return 1;   /* 已是目标值 */
    if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[2] = (unsigned char)val;
    VirtualProtect(p, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    q = 0;
    q = AppendCStr(buf, q, "vstep: ", sizeof(buf));
    q = AppendCStr(buf, q, tag, sizeof(buf));
    q = AppendCStr(buf, q, "=", sizeof(buf));
    UtoA((unsigned)val, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
    LogLine(buf);
    return 1;
}

static void VecBriefSteps(int lines)
{
    static int done = 0;
    if (done) return;
    done = 1;
    /* 注意: 这里跟的是格高 Lines (布局行距), 不是 GDI 墨水字号。
     * 原版二进制是 16 (配 16 高点阵); 改版 19 高 fnt 漏改了这两处,
     * 按当前字体格高补上。BitFont+0x1C 换行=19 与 vrows 实测一致。 */
    PatchImm8(ADDR_BRIEF_STEP1, 0x83, 0xC5, lines, "brief1");
    PatchImm8(ADDR_BRIEF_STEP2, 0x83, 0xE8, lines, "brief2");
}

/* ---------------- 原版墨水盒 ----------------
 * 已删除 (改版 game.fnt 的墨水盒本身就是错位的, 照抄等于抄错)。
 * 原版哲学: 每个字在格内垂直居中, 与字体具体多高无关。
 * 摆位只用三样全自适应量: 格高 h (引擎度量)、现墨水盒 (GDI 实掃)、
 * 用户偏移。16/12/19/原版/改版/任意 FontPx, 全走同一条居中公式。 */
/* (Blit 取字形寻址结论记 notes, 代码里不再测字体文件) */

/* ---------------- 字号校准 ----------------
 * GDI 字号对格高 h (引擎度量, 自适应 12/16/19): 试渲染"永",
 * px = px0*h/inkH, 一步收敛。摆位整体垂直居中 (整块窗高格内均分,
 * VecRender), 字间相对位置不动。
 * 与字体文件内容无关 (改版字体墨水盒可能是错的, 不测不信)。
 * 几何适配 (简报行高/面板) 另做布局补丁。
 * 键=(idata,face,bold), 字体重载/换字体自动重算。 */

static int g_PxValid = 0;
static void* g_PxIdata = NULL;
static int g_PxPx = 19, g_PxBold = -1, g_PxPxSrc = 0;
static wchar_t g_PxFace[FONT_FACE_MAX];

static int VecPxCalibrate(void* t)
{
    void* idata;
    int origH, px0, T, B, inkH, px, gw, x, y, scanW;
    unsigned char* bits;
    int pitch;
    char b1[16], b2[16], b3[16], buf[160];
    unsigned p;
    if (!t || !IsPlausiblePtr(t)) return -1;
    idata = *(void**)((char*)t + 4);
    if (g_PxValid && g_PxIdata == idata && g_PxBold == g_VT.Bold &&
        g_PxPxSrc == (g_VT.FontPx > 0 ? g_VT.FontPx : 0) &&
        FaceSame(g_PxFace, g_VTFace)) return g_PxPx;
    if (!idata || !IsPlausiblePtr(idata)) return -1;
    if (g_VT.FontPx > 0)
    {
        /* 全局固定字号: 墨水与字宽调用方等比缩放, 行距/换行仍是引擎的。
         * 简报行步进同样跟字号走 (界面适配字体)。 */
        px = g_VT.FontPx;
        if (px < 8) px = 8;
        if (px > 40) px = 40;
        origH = *(int*)((char*)idata + 0x0C);
        if (origH < 8 || origH > 40) origH = 19;
        g_PxValid = 1; g_PxIdata = idata; g_PxPx = px; g_PxBold = g_VT.Bold;
        g_PxPxSrc = g_VT.FontPx;
        g_FontOrigH = origH;
        CopyFace(g_PxFace, g_VTFace);
        p = 0;
        p = AppendCStr(buf, p, "vpx: fixed px=", sizeof(buf));
        UtoA((unsigned)px, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " origH=", sizeof(buf));
        UtoA((unsigned)origH, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
        LogLine(buf);
        VecBriefSteps(origH);
        return px;
    }
    origH = *(int*)((char*)idata + 0x0C);   /* Lines: 原字形数据行数 */
    if (origH < 8 || origH > 40) return -1;
    px0 = 18;
    if (!GdiEnsure(px0, g_VT.Bold, 2, g_VTFace)) return -1;
    if (GtRenderGlyphAt(0x6C38, GL_M, GL_Y0, NULL) <= 0) return -1;
    {
        SIZE sz;
        wchar_t ch = 0x6C38;
        gw = 0;
        if (g_G && g_G->dc && GetTextExtentPoint32W(g_G->dc, &ch, 1, &sz))
            gw = sz.cx;
    }
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    bits = g_G->bits;
    pitch = g_G->pitch;
    T = DIB_H; B = -1;
    for (y = 0; y < DIB_H; y++)
    {
        unsigned char* row = bits + y * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (y < T) T = y; if (y > B) B = y; }
    }
    if (B < T) return -1;
    inkH = B - T + 1;
    px = (px0 * origH + inkH / 2) / inkH;
    if (px < 8) px = 8;
    if (px > 40) px = 40;
    g_PxValid = 1; g_PxIdata = idata; g_PxPx = px; g_PxBold = g_VT.Bold;
    g_PxPxSrc = 0;
    g_FontOrigH = origH;
    CopyFace(g_PxFace, g_VTFace);
    p = 0;
    p = AppendCStr(buf, p, "vpx: origH=", sizeof(buf));
    UtoA((unsigned)origH, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " inkH=", sizeof(buf));
    UtoA((unsigned)inkH, b2); p = AppendCStr(buf, p, b2, sizeof(buf));
    p = AppendCStr(buf, p, " px=", sizeof(buf));
    UtoA((unsigned)px, b3); p = AppendCStr(buf, p, b3, sizeof(buf));
    LogLine(buf);
    VecBriefSteps(origH);
    return px;
}

/* 字号 = 全局固定 (FontPx) 或自动校准。顶栏条带数字由 Blit 层另行覆盖。 */
static int VecPickPx(void* t)
{
    return VecPxCalibrate(t);
}

/* 横向超宽记录 (诊断用, 上限 20): 哪个字被裁边/重采样一目了然。
 * m=0 对称裁边 (≤4px), m=1 重采样 (>4px)。 */
static int g_SqzLogs = 0;

static void VecSqueezeLog(wchar_t wch, int adv, int inkW, int m)
{
    char b1[16], buf[96];
    unsigned p = 0;
    if (g_SqzLogs >= 20) return;
    g_SqzLogs++;
    p = AppendCStr(buf, p, "vsqz: wch=", sizeof(buf));
    p += HtoA((unsigned)wch & 0xFFFFu, buf + p);
    p = AppendCStr(buf, p, " adv=", sizeof(buf));
    UtoA((unsigned)adv, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " inkW=", sizeof(buf));
    UtoA((unsigned)inkW, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " m=", sizeof(buf));
    UtoA((unsigned)m, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    LogLine(buf);
}

static VCEnt* VecGlyph(void* t, wchar_t wch, int adv, int h, int px)
{
    unsigned idx = ((unsigned)wch) & VC_MASK;
    VCEnt* e = &g_VC[idx];
    void* idata = *(void**)((char*)t + 4);
    int gw, dx, y, L, R, inkW, x;
    /* 字形四周边距: GDI 墨水会超出 extent (左右 overhang), 顶边画必被裁。
     * 垂直用基准窗 1:1 映射 (见 VecRefCalibrate), 基线全字体一致,
     * 自然高度, 不压缩。 */
    int scanW;
    unsigned char* bits;
    int pitch;
    int winTop, outH;
    if (!t || !IsPlausiblePtr(t)) return NULL;
    if (px < 8 || px > 40) return NULL;
    if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
        e->adv == adv && e->h == h)
        return e;
    /* 未命中: GDI 单字光栅 (带边距), 掃出真实墨水盒再摆进格子 */
    if (!GdiEnsure(px, g_VT.Bold, 2, g_VTFace)) return NULL;
    VecRefCalibrate(px);
    gw = GtRenderGlyphAt(wch, GL_M, GL_Y0, NULL);
    if (gw <= 0 || h > VC_MAXH) return NULL;
    bits = g_G->bits;
    pitch = g_G->pitch;
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    /* 自然高度输出: 窗内行与输出行 1:1 (源行 = 窗顶 + y), 不压缩。
     * outH 由 (字号/粗体/字体) 唯一决定, 缓存命中即一致, 无需进键比较。 */
    winTop = g_RefT;
    outH = g_RefB - g_RefT + 1;
    if (outH < 1) outH = h;
    if (outH > VC_MAXH) return NULL;
    if (winTop < 0 || winTop + outH > DIB_H) return NULL;
    /* 墨水盒 (窗内全掃, 只定左右) */
    L = scanW; R = -1;
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
    }
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        unsigned char* dst = e->cov + y * VC_MAXW;
        for (dx = 0; dx < adv; dx++) dst[dx] = 0;
        if (R < L) continue;   /* 空白字 (空格): 占宽, 不画 */
        inkW = R - L + 1;
        {
            /* 摆位: 满格 (CJK/数字) 左对齐, 与原版起笔一致;
             * 窄字形 (标点等, 墨水不足格宽 2/3) 按 GDI 自然边距摆,
             * 格子比 GDI 步进宽出的部分左右均分 (如":"不再贴边)。
             * 超宽才水平挤, 起点为摆位偏移。 */
            int off = 0, avail;
            if (inkW * 3 <= adv * 2 && g_G && g_G->dc)
            {
                ABC abc;
                if (GetCharABCWidthsW(g_G->dc, wch, wch, &abc))
                {
                    int gdiAdv = abc.abcA + abc.abcB + abc.abcC;
                    off = abc.abcA + (adv - gdiAdv) / 2;
                    if (off < 0) off = 0;
                    if (off > adv - 1) off = adv - 1;
                }
            }
            avail = adv - off;
            if (avail < 1) avail = 1;
            if (inkW <= avail)
            {
                int n = inkW < avail ? inkW : avail;
                for (dx = 0; dx < n; dx++) dst[off + dx] = row[(L + dx) * 4 + 2];
            }
            else if (inkW - avail <= 4)
            {
                /* 轻微超宽 (≤4px): 对称裁边距, 不重采样。
                 * 重采样会吃掉笔画间空白 ("空白被切掉"感);
                 * 边距本来就是富余的, 切 1~2px 两边看不出。 */
                int dropL = (inkW - avail) / 2;
                int dx2;
                for (dx2 = 0; dx2 < avail; dx2++)
                    dst[off + dx2] = row[(L + dropL + dx2) * 4 + 2];
                VecSqueezeLog(wch, adv, inkW, 0);
            }
            else
            {
                for (dx = 0; dx < avail; dx++)
                {
                    int sx = L + (dx * inkW) / avail;
                    dst[off + dx] = row[sx * 4 + 2];
                }
                VecSqueezeLog(wch, adv, inkW, 1);
            }
        }
    }
    e->used = 1; e->wch = wch; e->idata = idata; e->px = px; e->adv = adv; e->h = h;
    e->outh = outH;
    return e;
}

/* 矢量替换: 成功返回 X+adv (与原版返回值口径一致), 失败返回 -1 走原版 */
static int VecRender(void* t, wchar_t wch, int adv, int h, int x, int y,
                     int colorW, int px, int yOff)
{
    unsigned short* buf;
    int pitchW, L, Tp, R, B, dx, dy;
    unsigned char fr, fg, fb;
    VCEnt* e;
    if (!t || !IsPlausiblePtr(t)) return -1;
    if (adv > VC_MAXW || h > VC_MAXH) return -1;
    buf = *(unsigned short**)((char*)t + 0x0C);
    pitchW = *(int*)((char*)t + 0x10);
    if (!buf || !IsPlausiblePtr(buf)) return -1;
    if (pitchW < 64 || pitchW > 4096) return -1;
    L = *(int*)((char*)t + 0x30);
    Tp = *(int*)((char*)t + 0x34);
    R = *(int*)((char*)t + 0x38);
    B = *(int*)((char*)t + 0x3C);
    if (R < L || B < Tp) return -1;
    if (L < 0) L = 0;
    if (Tp < 0) Tp = 0;
    if (R > 4096) R = 4096;
    if (B > 4096) B = 4096;
    Calibrate();
    e = VecGlyph(t, wch, adv, h, px);
    if (!e) return -1;
    GtUnpack16((unsigned short)(colorW & 0xFFFF), &fr, &fg, &fb);
    /* 整体垂直居中: 整块 (基准窗全高) 在格内上下均分, 所有字同一起点。
     * 字间相对位置 (基线、标点贴底、数字贴顶) 原样保留 GDI 排版,
     * 不逐字动。格高 h、窗高全是运行时量, 不认硬编码。
     * 行裁剪钳制: 小矩形 DrawText 自带紧裁剪 (如超武行 24 高),
     * 大块按格居中会探出上沿被切; 此时贴紧裁剪上沿 (与原版行首一致),
     * 裁剪宽松时钳制不生效, 与原来一样。
     * 空白字 cov 全空, 画不出东西, dy 无所谓。 */
    dy = y + (h - e->outh) / 2;
    {
        int lo = Tp, hi = B - e->outh + 1;
        if (hi >= lo)
        {
            if (dy < lo) dy = lo;
            if (dy > hi) dy = hi;
        }
    }
    dy += yOff;
    /* 输出自然高度 (outh 行, 1:1, 不压缩)。降部超出格高的行落在格外,
     * 与原版裁剪口径一致由表面裁剪 (Tp/B) 收尾, 不回落原版。 */
    for (y = 0; y < e->outh; y++)
    {
        int sy = dy + y;
        unsigned short* row;
        unsigned char* cov;
        if (sy < Tp || sy > B) continue;
        row = buf + sy * pitchW;
        cov = e->cov + y * VC_MAXW;
        for (dx = 0; dx < adv; dx++)
        {
            int sx = x + dx;
            unsigned a;
            if (sx < L || sx > R) continue;
            if (!cov[dx]) continue;
            a = cov[dx];   /* 灰度即 alpha, 全浓度 */
            row[sx] = Blend16(row[sx], fr, fg, fb, a);
        }
    }
    return x + adv;
}

/* 数字字形渲染实况 (顶栏用): 每个数字 Blit 的 px/credits/yoff、
 * 表面坐标 x,y、成败。上限 12 行。
 * 结论: 同一条顶栏被多条路径在同一坐标反复画, 位置是定案关键。 */
static int g_DigitLogs = 0;

static void VecDigitLog(wchar_t wc, int px, int yOff, int ok, int x, int y,
                        int cred)
{
    char b1[16], buf[160];
    unsigned p;
    if (g_DigitLogs >= 12) return;
    g_DigitLogs++;
    p = 0;
    p = AppendCStr(buf, p, "vdigit: wch=", sizeof(buf));
    p += HtoA((unsigned)wc, buf + p);
    p = AppendCStr(buf, p, " px=", sizeof(buf));
    UtoA((unsigned)px, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " cred=", sizeof(buf));
    UtoA((unsigned)cred, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " yoff=", sizeof(buf));
    {
        int dy = yOff;
        if (dy < 0) { p = AppendCh(buf, p, '-', sizeof(buf)); dy = -dy; }
        UtoA((unsigned)dy, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    }
    p = AppendCStr(buf, p, " xy=", sizeof(buf));
    UtoA((unsigned)x, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCh(buf, p, ',', sizeof(buf));
    UtoA((unsigned)y, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " r=", sizeof(buf));
    p = AppendCStr(buf, p, ok ? "vec" : "orig", sizeof(buf));
    LogLine(buf);
}

int __fastcall VecText_BlitHook(void* t, void* edxUnused, int wch, int x, int y, int color)
{
    wchar_t wc = (wchar_t)((unsigned)wch & 0xFFFFu);
    int eff, adv, h = 0, r, px, yOff;
    (void)edxUnused;
    if (!g_OrigBlit) return 0;   /* 不该发生, 守一下不断原版链 */
    g_BlitN++;
    if (g_DTBrief && t && IsPlausiblePtr(t))
    {
        /* 简报裁剪底边跟盒子走: DrawText 的 H 参数改了, 但真正切行的是
         * Lock 进 BitFont+0x30 的裁剪 (面板子面 745 底)。盒子加高后
         * (底 783) 必须把裁剪底同步放开, 行才画得出来。
         * 安全阀: 只放底边, 最多 +64; pitch < 1024 说明不是整屏面,
         * 不碰 (防小面越界)。 */
        int* LTRB = (int*)((char*)t + 0x30);
        int pitchW = *(int*)((char*)t + 0x10);
        if (!g_ClipLogged)
        {
            char b1[16], buf[160];
            unsigned p = 0;
            g_ClipLogged = 1;
            p = AppendCStr(buf, p, "vclip: L=", sizeof(buf));
            UtoA((unsigned)LTRB[0], b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCStr(buf, p, " T=", sizeof(buf));
            UtoA((unsigned)LTRB[1], b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCStr(buf, p, " R=", sizeof(buf));
            UtoA((unsigned)LTRB[2], b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCStr(buf, p, " B=", sizeof(buf));
            UtoA((unsigned)LTRB[3], b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCStr(buf, p, " pitch=", sizeof(buf));
            UtoA((unsigned)pitchW, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
            LogLine(buf);
        }
        if (g_VT.BriefClip && pitchW >= 1024 && LTRB[3] < 783 &&
            LTRB[3] >= 700)
            LTRB[3] = 783;
    }
    if (g_DTBrief)
    {
        /* 行跨度: 行原点 y 的最小/最大 + 脱离 ymin 网格数 (步进 19) */
        if (g_BlitN == g_DTBlit0 + 1) { g_YMin = y; g_YMax = y; g_YOffGrid = 0; }
        else
        {
            if (y < g_YMin) g_YMin = y;
            if (y > g_YMax) g_YMax = y;
            if ((y - g_YMin) % 19 != 0) g_YOffGrid++;
        }
    }
    /* TAB 走原版特殊公式, 缺字/度量失败也走原版 (保留 0x5555 兜底行为) */
    if (!t || !IsPlausiblePtr(t) || wc == 9)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    eff = (color == -1) ? (int)(*(unsigned short*)((char*)t + 0x24)) : color;
    adv = VecMeasure(t, wc, &h);
    if (adv < 0)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    if (g_VT.LogAdv) VecProbeLog(wc, adv, h, eff);
    if (g_VT.LogAdv) VecClipProbe(t, wc, x, y, adv, h, eff, color);
    if (g_VT.Render)
    {
        px = VecPickPx(t);
        if (px >= 0)
        {
            /* 右侧带诊断 (0.80 网, 含脱靶): 先记再判, 上限 12 */
            if (g_VT.LogAdv && g_VT.SuperPx > 0)
            {
                int bp = *(int*)((char*)t + 0x10);
                if (bp >= 1024 && bp <= 4096 && y >= 32 &&
                    x * 100 >= bp * 80)
                    VecSWBandLog(wc, x, y, bp);
            }
            /* 顶栏条带 (y<32) 的数字: 唯一的顶栏判据, 与发起者无关。
             * 同一条顶栏至少 5 个发起者反复画, 按位置一刀切,
             * 大小偏移永远一致, 闪烁自止。只在用户配了 CreditPx/DY 时生效,
             * 默认 0/0 全不碰。超武列表等不在条带内, 不受影响。 */
            int cred = (wc >= 0x30 && wc <= 0x39 && y >= -8 && y < 32 &&
                (g_VT.CreditPx > 0 || g_VT.CreditDY != 0));
            int effPx = g_VT.FontPx;
            int swForce = 0;
            if (cred && g_VT.CreditPx > 0)
            {
                px = g_VT.CreditPx;
                if (px < 8) px = 8;
                if (px > 40) px = 40;
            }
            /* 超武区整体隐藏+重绘: 落在区域内即按 SuperPx 画, 原字形不再画
             * (同锁同裁剪, 笔位/背景/后续全是引擎的)。
             * 列表不是顶栏, CreditDY 不掺和。 */
            swForce = VecSWRegion(t, x, y);
            if (swForce)
            {
                px = g_VT.SuperPx;
                if (px < 8) px = 8;
                if (px > 40) px = 40;
                effPx = px;
                cred = 0;
            }
            /* 字号 (全局/接管): 墨水与字宽等比缩放, 版式才对。
             * 条带数字除外 (只换墨水字宽不动, 不碰已调好的顶栏)。
             * 行距/换行仍是引擎的, 字号偏离原版太多可能溢出盒子蹭行。 */
            if (!cred && effPx > 0 && g_FontOrigH >= 8 &&
                g_FontOrigH <= 40)
            {
                /* 注意用 effPx (全局或接管), 不是 g_VT.FontPx:
                 * FontPx=0 时接管链 effPx=SuperPx, 错用 FontPx 会把 adv
                 * 压成 1 (整串字挤成细条, vsw 实证前没走到这纯属侥幸)。 */
                adv = (adv * effPx + g_FontOrigH / 2) / g_FontOrigH;
                if (adv < 1) adv = 1;
            }
            yOff = g_VT.VShift + (cred ? g_VT.CreditDY : 0);
            r = VecRender(t, wc, adv, h, x, y, eff, px, yOff);
            if (wc >= 0x30 && wc <= 0x39 && g_VT.LogAdv)
                VecDigitLog(wc, px, yOff, r >= 0, x, y, cred);
            if (swForce)
            {
                /* 接管区 VecRender 失败也不回落原版 (否则原字冒出来):
                 * 只走笔位, 记一行诊断 (上限 8)。 */
                if (r < 0 && g_SWFailN < 8)
                {
                    char f1[16], fbuf[64];
                    unsigned q = 0;
                    g_SWFailN++;
                    q = AppendCStr(fbuf, q, "vswfail: wc=", sizeof(fbuf));
                    q += HtoA((unsigned)wc & 0xFFFFu, fbuf + q);
                    q = AppendCStr(fbuf, q, " xy=", sizeof(fbuf));
                    UtoA((unsigned)x, f1);
                    q = AppendCStr(fbuf, q, f1, sizeof(fbuf));
                    LogLine(fbuf);
                }
                return x + adv;
            }
            if (r >= 0) return r;
        }
    }
    return g_OrigBlit(t, NULL, wch, x, y, color);
}

/* ---------------- 下钩 ---------------- */

static int BytesSame(const unsigned char* a, const unsigned char* b, int n)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int BuildTramp(void)
{
    unsigned char* b;
    int i;
    b = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!b) return 0;
    for (i = 0; i < 7; i++) b[i] = ((unsigned char*)ADDR_BLIT)[i];
    b[7] = 0xE9;   /* jmp 回 Blit+7 */
    *(int*)(b + 8) = (int)(ADDR_BLIT_BACK - ((unsigned)(size_t)b + 12));
    FlushInstructionCache(GetCurrentProcess(), b, 12);
    g_OrigBlit = (BlitFn)b;
    return 1;
}

/* 诊断 (一次性): 指定代码区打 hex, 找列表行距指令。 */
static void VecDumpArea(unsigned at, int len)
{
    unsigned char* p = (unsigned char*)at;
    char buf[128];
    unsigned q;
    int off, i;
    for (off = 0; off < len; off += 24)
    {
        q = 0;
        q = AppendCStr(buf, q, "vcode2: ", sizeof(buf));
        for (i = 0; i < 24 && off + i < len; i++)
        {
            /* 代码段直读 (游戏正在执行它, 必已提交;
             * 与 BuildTrampAt/LogBytes 的做法一致) */
            unsigned b = p[off + i];
            buf[q++] = "0123456789ABCDEF"[b >> 4];
            buf[q++] = "0123456789ABCDEF"[b & 15];
            buf[q++] = ' ';
        }
        buf[q] = 0;
        LogLine(buf);
    }
}

static int PatchBlit(void)
{
    unsigned char* p = (unsigned char*)ADDR_BLIT;
    DWORD old;
    unsigned target = (unsigned)(size_t)VecText_BlitHook;
    int rel = (int)(target - ((unsigned)(size_t)p + 5));
    if (!VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0xE9;
    *(int*)(p + 1) = rel;
    p[5] = 0x90; p[6] = 0x90;
    VirtualProtect(p, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 7);
    return 1;
}

/* 通用跳板/补丁 (入口钩用, 字节数按指令边界取) */
static void* BuildTrampAt(unsigned at, int n, unsigned back)
{
    unsigned char* b;
    int i;
    b = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!b) return NULL;
    for (i = 0; i < n; i++) b[i] = ((unsigned char*)at)[i];
    b[n] = 0xE9;
    *(int*)(b + n + 1) = (int)(back - ((unsigned)(size_t)b + n + 5));
    FlushInstructionCache(GetCurrentProcess(), b, (SIZE_T)(n + 5));
    return b;
}

static int PatchAt(unsigned at, void* target, int n)
{
    unsigned char* p = (unsigned char*)at;
    DWORD old;
    int rel = (int)((unsigned)(size_t)target - (at + 5));
    int i;
    if (n < 5) return 0;
    if (!VirtualProtect(p, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0xE9;
    *(int*)(p + 1) = rel;
    for (i = 5; i < n; i++) p[i] = 0x90;
    VirtualProtect(p, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, (SIZE_T)n);
    return 1;
}

static void LogBytes(const unsigned char* p)
{
    char b1[16], buf[128];
    unsigned q = 0;
    int i;
    q = AppendCStr(buf, q, "vtext: got=", sizeof(buf));
    for (i = 0; i < 7; i++)
    {
        UtoA(p[i], b1);
        q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCh(buf, q, ' ', sizeof(buf));
    }
    LogLine(buf);
}

void VecText_Install(void)
{
    char b1[16], buf[192];
    unsigned p;
    GtEnsureLog();
    if (!g_Log) return;   /* 日志都打不开就不装钩 (无意义) */
    g_VT.Enable = GetPrivateProfileIntA(VT_SECTION, "Enable", 1, g_IniPath);
    GetPrivateProfileStringA(VT_SECTION, "FontFace", "simhei.ttf",
                             g_VT.FontFace, sizeof(g_VT.FontFace), g_IniPath);
    g_VT.FontFace[sizeof(g_VT.FontFace) - 1] = 0;
    g_VT.Bold = GetPrivateProfileIntA(VT_SECTION, "Bold", 0, g_IniPath);
    g_VT.FontPx = GetPrivateProfileIntA(VT_SECTION, "FontPx", 0, g_IniPath);
    g_VT.CreditPx = GetPrivateProfileIntA(VT_SECTION, "CreditPx", 0, g_IniPath);
    g_VT.CreditDY = GetPrivateProfileIntA(VT_SECTION, "CreditDY", 0, g_IniPath);
    g_VT.SuperPx = GetPrivateProfileIntA(VT_SECTION, "SuperPx", 0, g_IniPath);
    g_VT.BriefBox = GetPrivateProfileIntA(VT_SECTION, "BriefBox", 1, g_IniPath);
    g_VT.BriefClip = GetPrivateProfileIntA(VT_SECTION, "BriefClip", 1, g_IniPath);
    g_VT.VShift = GetPrivateProfileIntA(VT_SECTION, "VShift", 0, g_IniPath);
    g_VT.LogAdv = GetPrivateProfileIntA(VT_SECTION, "LogAdv", 1, g_IniPath);
    g_VT.Render = GetPrivateProfileIntA(VT_SECTION, "Render", 0, g_IniPath);
    g_VT.Enable = g_VT.Enable ? 1 : 0;
    g_VT.Bold = g_VT.Bold ? 1 : 0;
    if (g_VT.FontPx < 0) g_VT.FontPx = 0;
    if (g_VT.FontPx > 40) g_VT.FontPx = 40;
    if (g_VT.CreditPx < 0) g_VT.CreditPx = 0;
    if (g_VT.CreditPx > 40) g_VT.CreditPx = 40;
    if (g_VT.CreditDY < -8) g_VT.CreditDY = -8;
    if (g_VT.CreditDY > 8) g_VT.CreditDY = 8;
    if (g_VT.SuperPx < 0) g_VT.SuperPx = 0;
    if (g_VT.SuperPx > 40) g_VT.SuperPx = 40;
    g_VT.BriefBox = g_VT.BriefBox ? 1 : 0;
    g_VT.BriefClip = g_VT.BriefClip ? 1 : 0;
    if (g_VT.VShift < -8) g_VT.VShift = -8;
    if (g_VT.VShift > 8) g_VT.VShift = 8;
    g_VT.LogAdv = g_VT.LogAdv ? 1 : 0;
    g_VT.Render = g_VT.Render ? 1 : 0;
    g_GetDim = (GetDimFn)ADDR_GETDIM;
    ResolveFontFace(g_VT.FontFace, "vtext", g_VTFace, L"SimHei");
    p = 0;
    p = AppendCStr(buf, p, "vtext: start en=", sizeof(buf));
    UtoA((unsigned)g_VT.Enable, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " log=", sizeof(buf));
    UtoA((unsigned)g_VT.LogAdv, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " render=", sizeof(buf));
    UtoA((unsigned)g_VT.Render, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " px=", sizeof(buf));
    UtoA((unsigned)g_VT.FontPx, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " cred=", sizeof(buf));
    UtoA((unsigned)g_VT.CreditPx, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " credy=", sizeof(buf));
    {
        int dy = g_VT.CreditDY;
        if (dy < 0) { p = AppendCh(buf, p, '-', sizeof(buf)); dy = -dy; }
        UtoA((unsigned)dy, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    }
    p = AppendCStr(buf, p, " v=5", sizeof(buf));   /* 发版自增, 日志对版本 */
    LogLine(buf);
    if (!g_VT.Enable) { LogLine("vtext: disabled"); return; }
    if (!BytesSame((unsigned char*)ADDR_BLIT, kWantBlit, 7))
    {
        LogLine("vtext: blit bytes mismatch, hook OFF");
        LogBytes((unsigned char*)ADDR_BLIT);
        return;
    }
    if (!BuildTramp()) { LogLine("vtext: tramp fail, hook OFF"); return; }
    if (!PatchBlit()) { LogLine("vtext: patch fail, hook OFF"); return; }
    LogLine(g_VT.Render ? "vtext: blit hook ON (render)" : "vtext: blit hook ON (probe)");
    /* 超武列表发起者 0x4BB7EB 附近反汇编取证: 行距指令。一次。 */
    VecDumpArea(0x004BB600u, 512);
    /* DrawText/Print 入口上下文钩 (只记调用者, 绘制原样走跳板) */
    if (BytesSame((unsigned char*)ADDR_DRAWTEXT, kWantDrawText, 6))
    {
        g_OrigDrawText = (DrawTextFn)BuildTrampAt(ADDR_DRAWTEXT, 6, ADDR_DRAWTEXT_BACK);
        if (g_OrigDrawText && PatchAt(ADDR_DRAWTEXT, (void*)(size_t)VecDrawTextEntry, 6))
            LogLine("vtext: drawtext ctx hook ON");
        else { LogLine("vtext: drawtext ctx hook OFF"); g_OrigDrawText = NULL; }
    }
    else { LogLine("vtext: drawtext bytes mismatch"); LogBytes((unsigned char*)ADDR_DRAWTEXT); }
    if (BytesSame((unsigned char*)ADDR_PRINT, kWantPrint, 10))
    {
        g_OrigPrint = (PrintFn)BuildTrampAt(ADDR_PRINT, 10, ADDR_PRINT_BACK);
        if (g_OrigPrint && PatchAt(ADDR_PRINT, (void*)(size_t)VecPrintEntry, 10))
            LogLine("vtext: print ctx hook ON");
        else { LogLine("vtext: print ctx hook OFF"); g_OrigPrint = NULL; }
    }
    else { LogLine("vtext: print bytes mismatch"); LogBytes((unsigned char*)ADDR_PRINT); }
}
