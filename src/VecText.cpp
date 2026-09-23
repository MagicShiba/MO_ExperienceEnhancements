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
#define ADDR_GETDIM 0x00433CF0u
#define ADDR_GETDIM_BACK 0x00433CF6u
#define ADDR_DRAWTEXT    0x00434CD0u
#define ADDR_DRAWTEXT_BACK 0x00434CD6u
#define ADDR_PRINT       0x00434B90u
#define ADDR_PRINT_BACK  0x00434B9Au
#define ADDR_SIMPLE      0x004A5EB0u
#define ADDR_SIMPLE_BACK 0x004A5EB7u

/* Blit 头 7 字节 (sub esp,0x30 + push ebx/ebp/esi/edi), 下钩前校验 */
static const unsigned char kWantBlit[7] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56, 0x57 };
/* DrawText 头 6 字节 (sub esp,0x30 + push ebx/ebp/esi) */
static const unsigned char kWantDrawText[6] = { 0x83, 0xEC, 0x30, 0x53, 0x55, 0x56 };
/* Print 头 10 字节 (push esi + mov esi,[esp+8] + push edi + mov edi,[esp+0x18]) */
static const unsigned char kWantPrint[10] = { 0x56, 0x8B, 0x74, 0x24, 0x08,
                                              0x57, 0x8B, 0x7C, 0x24, 0x18 };
/* Simple_ 头 7 字节 (sub esp,0x28 + mov eax,[esp+0x34]) */
static const unsigned char kWantSimple[7] = { 0x83, 0xEC, 0x28,
                                              0x8B, 0x44, 0x24, 0x34 };
/* GetDim 头 6 字节 (sub esp,0x1C + mov eax,[ecx+4], gamemd 静态对过) */
static const unsigned char kWantGetDim[6] = { 0x83, 0xEC, 0x1C,
                                              0x8B, 0x41, 0x04 };

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
    int SuperPx;    /* Blit 层漏网重绘字号 (0=关闭; 列表整体接管另由 [SWPanel] 开关) */
    int MenuTake;   /* 1=菜单按钮整体替换 (禁用原字形, 全墨水自定义绘制) */
    int DigitTrack; /* 数字字距微调像素 (负=收紧, 正=放松, 只影响 0-9 步进) */
    int ToolBox;    /* 0=关, 1=悬浮提示背景盒跟字走 (取样实色底才拓盒), 2=探测 */
    int TipBox;     /* 1=电力提示定制盒 (按列调映射补齐, 与引擎同 tone) */
    int BriefBox;   /* 1=简报正文盒加高 (界面适配字体), 0=保持原版盒子 */
    int BriefClip;  /* 1=简报裁剪底边跟盒子走 (面板子面切，需配合 BriefBox) */
    int Bold;
    int VShift;     /* 字形垂直偏移 (像素, 正=下移) */
    int LogAdv;
    int LogDetail;  /* 1=逐字/逐帧普查日志 (vhead/vren/vss/vbuf/vtip 等), 0=只记关键 */
    int Render;
} VTConfig;

static VTConfig g_VT;
static wchar_t g_VTFace[FONT_FACE_MAX];

/* VecRender/VecGlyph 最近一次失败点 (0=成功), 只给失败日志用:
 * 1=!t 2=adv/h 3=!buf 4=pitch 5=B<Tp 6=VecGlyph返回NULL;
 * 10=!t 11=px越界 12=!GdiEnsure 13=gw/h 14=outH 15=winTop 16=off+inkW */
static int g_VecWhy = 0;

/* thiscall (ECX=this): __fastcall 占位 (this, edxUnused) + 栈上 4 参数,
 * 编译器生成 ret 0x10, 与原 Blit 尾完全一致, JMP 钩可直接跳入。 */
typedef int (__fastcall *BlitFn)(void* t, void* edxUnused,
                                 int wch, int x, int y, int color);
static BlitFn g_OrigBlit = NULL;

typedef int (__fastcall *GetDimFn)(void* t, void* edxUnused,
                                   const wchar_t* s, int* w, int* h, int maxW);
static GetDimFn g_OrigGetDim = NULL;   /* 度量原版 (跳板); 引擎算字宽唯一入口,
                                        * 由 VecGetDimEntry 接管改宽 */

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

/* Simple_Text_Print_Wide (0x4A5EB0): 全文本必经之路 (Print 唯一调用者)。
 * fastcall 形 (ECX=pRetVal, EDX=输入) + 7 个栈参 (ret 0x1c, 尾部反汇编实证)。
 * 诊断钩只观察不跳过: 转发必须原样 (ECX/EDX 原值进跳板)。 */
typedef int (__fastcall *SimpleFn)(int ecxIn, int edxIn,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7);
static SimpleFn g_OrigSimple = NULL;

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

/* 栈上找直接上游: 第一个落在游戏代码段且通过返回地址校验的
 * (不管线内外之分)。-O2 无帧指针, __builtin_return_address 不可用,
 * 用它抓 Simple_Wide 的直接调用点 (反汇编定盒宽来源)。 */
static unsigned VecFindDirect(void)
{
    unsigned esp, i;
    __asm__ volatile ("movl %%esp, %0" : "=r"(esp));
    for (i = 0; i < 256; i++)
    {
        unsigned v;
        if (!IsPlausiblePtr((void*)esp)) break;
        v = *(unsigned*)esp;
        if (v >= 0x00400000u && v < 0x00900000u && VecIsRetAddr(v))
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
/* 简报正文盒内 (引擎按自有宽度换行): 步进保持原笔位, 不加宽 */
static int g_DTBody = 0;
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

/* ---------------- 超武列表整体接管 ----------------
 * 收集点在 Simple_Text_Print_Wide 入口 (Print 唯一调用者, 反汇编实证):
 * 背景黑条是 Simple_ 内部按标志 (ah&4, 名行 flags=0x4618) 铺的不透明底,
 * 与字形同一调用, 入口跳过即连底+字一起消失, 不管内容细节不逐条绘制。
 * 命中 (主画布 + 落列表带 + 名/计时器形状) 即收表 (串,坐标,原前景色)
 * 并跳过; 空串只收表不跳过 (引擎底色行为与原版一致, 闪烁靠 150ms 过期)。
 * 门不全走原版; Print 入口保留同带收集做后备 (fore 不见时用名青/计时器白)。
 * DrawText 计时器 (侧栏图标叠字, 非列表) 与 Blit 层漏网 backstop 不动。 */

/* 提示串排除 ("电力 = 24660" 会飘进右侧带, 误收进行列表污染超武面板):
 * 前 16 字含 '=' 即非列表串。超武名/计时器从不含 '='。 */
static int VecSWHasEq(const wchar_t* s)
{
    int k;
    if (!s || !IsPlausiblePtr((void*)s)) return 0;
    for (k = 0; k < 16; k++)
    {
        unsigned c = (unsigned)s[k] & 0xFFFFu;
        if (!c) break;
        if (c == 0x3Du) return 1;
    }
    return 0;
}
/* 冒号嗅探 ("00:00" 经任何 Print 路径; 顶栏纯数字天然区分) */
static int VecSWColon(const wchar_t* s)
{
    int k;
    if (!s || !IsPlausiblePtr(s)) return 0;
    for (k = 0; k < 16; k++)
    {
        unsigned c = (unsigned)s[k] & 0xFFFFu;
        if (!c) break;
        if (c == 0x3Au) return 1;
    }
    return 0;
}

SWCapRow g_SWCap[SWCAP_MAX];
int g_SWCapN = 0;
int g_SWCompW = 0, g_SWCompH = 0;
int g_SWPanelOn = 0;

/* 收表 (同坐标覆盖, 空串也收: 杀过期旧文, 面板画空=闪烁灭态)。
 * src: 0=Simple_ (带原色), 1=Print 后备 (无色)。
 * 后备不许覆盖新鲜的 Simple_ 行 (防碎片冲掉正文)。 */
static void VecSWStore(int kind, const wchar_t* s, int n, int fore, int x,
                       int y, int src)
{
    int i;
    SWCapRow* r;
    unsigned now = GetTickCount();
    /* 同行归并 (x 容差 ±6): 引擎同行落笔 x 逐帧抖动 (1722~1725 实证),
     * 精确匹配会涨满 32 项导致丢弃+过期闪烁。名/计时器同 y 不同 x
     * (差 27) 天然分开, 不会误并。 */
    for (i = 0; i < g_SWCapN; i++)
    {
        int dx = g_SWCap[i].x - x;
        if (g_SWCap[i].y == y && dx >= -6 && dx <= 6) break;
    }
    if (i >= g_SWCapN)
    {
        if (g_SWCapN >= SWCAP_MAX) return;
        r = &g_SWCap[g_SWCapN++];
        r->x = x; r->y = y;
        r->fore = -1;
    }
    else
    {
        r = &g_SWCap[i];
        r->x = x;   /* 跟随漂移 */
        if (src == 1 && fore < 0 && r->fore >= 0 &&
            now - r->tick <= SW_FRESH_MS) return;   /* 新鲜正文, 后备碎片靠边 */
    }
    if (n > 63) n = 63;
    for (i = 0; i <= n; i++) r->s[i] = s[i];
    r->s[63] = 0;
    r->kind = kind;
    if (fore >= 0) r->fore = fore;
    else if (r->fore < 0) r->fore = (kind == 0) ? 0x07FF : 0xFFFF;
    r->tick = now;
}

/* Print 层后备收集 (Simple_ 钩漏网时才跑到这): 同带同面即收并跳过。
 * fore 在 Print 层不可见 (a6=a7=0 实证), 记 -1 用名青/计时器白。 */
static int VecSWCapture(void* surf, int kind, const wchar_t* s, int x, int y)
{
    int sw = 0, sh = 0, n = 0;
    if (!g_SWPanelOn) return 0;
    if (!s || !IsPlausiblePtr(s)) return 0;
    if (x < 1300 || x > 1752 || y < 32 || y > 1080) return 0;
    if (surf && IsPlausiblePtr(surf))
    {
        int* b = (int*)surf;
        sw = b[1]; sh = b[2];
    }
    if (sw <= 0 || sh <= 0 || sw != g_SWCompW || sh != g_SWCompH) return 0;
    while (s[n] && n < 63) n++;
    if (g_SWCapN >= SWCAP_MAX && n > 0)
    {
        int i, found = 0;
        for (i = 0; i < g_SWCapN; i++)
        {
            int dx = g_SWCap[i].x - x;
            if (g_SWCap[i].y == y && dx >= -6 && dx <= 6) { found = 1; break; }
        }
        if (!found) return 1;   /* 满了: 照跳, 不收 */
    }
    VecSWStore(kind, s, n, -1, x, y, 1);
    return n > 0 ? 1 : 0;   /* 空串不跳过 (底色行为与原版一致) */
}

/* (旧捕获/重画/探针已删除: 定案为 Simple_ 当场收集 + 整块重绘) */

/* ---------------- 菜单按钮整体替换 ----------------
 * 路径定案 (vmenu 探针, 已删): DrawText caller=0x00621144 (与简报正文
 * 同一调用者, 再用标志+尺寸区分), flags=5, 123x24 (正文, 如 1773,231
 * 游戏控制) / 121x20 (阴影, +2,+2 偏移)。
 * 入口时表面尚未锁定 (t+0x0C 无效), 故入口只收串+矩形 (t 在此不可靠,
 * 不建字形); 首个 Blit 建全串字形 (Blit 的 t 才是真 BitFont)、算总宽、
 * 矩形内居中起笔, 之后按实际墨水宽度走笔, 引擎 x 不用。
 * 原字形一次不画 (灾难失败才回落原版保底可读)。 */
static int g_MenuDepth = 0;
#define MENU_MAXCH 64
static wchar_t g_MenuStr[MENU_MAXCH + 1];
static int g_MenuAdv[MENU_MAXCH];
static int g_MenuN = 0;
static int g_MenuRX = 0, g_MenuRW = 0;
static int g_MenuIdx = 0;      /* 已画字数 */
static int g_MenuPenX = 0;     /* 自有笔位 */
static int g_MenuBuilt = 0;

static int VecMenuMatch(unsigned caller, int w, int h, int flags)
{
    if (!g_VT.MenuTake) return 0;
    if (caller != 0x00621144u) return 0;
    if (flags != 5) return 0;
    if ((w == 123 && h == 24) || (w == 121 && h == 20)) return 1;
    return 0;
}

/* ---------------- 悬浮提示背景盒自适应 ----------------
 * 定案: 世界悬浮框经 Print (caller=0x4A5FF2 内层), 如 (776,571) "电力 = …"、
 * 建筑名标签。引擎盒要么固定尺寸、要么按自有窄字宽算, 我们的宽字撑出去,
 * 故盒跟字走 (GetDim 钩够不着固定盒)。
 * Print 入口收串+坐标 (超武带除外; 不认 outer, 发起者链不稳定),
 * 首个 Blit 建全串字形算总宽 (GDI 自然步进),
 * 取样确认是实色暗底才拓盒 (花面无盒不碰), 再自有笔位画字。
 * 原字形一次不画。分段多色 (如红字负载) 各段独立收/拓/画,
 * 同行相邻段无缝衔接 (500ms 内同行即连续, 左垫省略防吃掉上段尾巴)。 */
static int g_ToolDepth = 0;
#define TOOL_MAXCH 64
static wchar_t g_ToolStr[TOOL_MAXCH + 1];
static int g_ToolAdv[TOOL_MAXCH];
static int g_ToolN = 0;
static int g_ToolX = 0, g_ToolY = 0;
static int g_ToolIdx = 0;
static int g_ToolPenX = 0;
static int g_ToolBuilt = 0;   /* 0=未建, 1=已建, -1=失败走引擎笔位 */
static int g_ToolLastX1 = -9999, g_ToolLastY = -9999;
static unsigned g_ToolLastTick = 0;
static int g_ToolStepA[TOOL_MAXCH];
static int g_ToolInk0 = 0, g_ToolInkEnd = 0;   /* 首/尾墨水相对 x 偏移 */
static unsigned g_ToolGateTick = 0;   /* 取样门上次判决时刻 */
static int g_ToolGateOK = 0;
static unsigned g_ToolGateColor = 0;

static int VecToolMatch(unsigned caller, const wchar_t* s, int x, int y)
{
    unsigned c0;
    if (!g_VT.ToolBox) return 0;
    if (caller != 0x004A5FF2u) return 0;
    /* 超武带归 SW (那边收表+跳过, 不抢) */
    if (x >= 1300 && x <= 1752 && y >= 32 && y <= 1080) return 0;
    if (!s || !IsPlausiblePtr((void*)s)) return 0;
    c0 = (unsigned)s[0] & 0xFFFFu;
    if (!((c0 >= 0x2E80u && c0 < 0xA000u) ||
          (c0 >= 0xFF00u && c0 < 0xFFF0u))) return 0;
    /* 不认 outer: 发起者链不稳定 (同串跨局 outer 会变), 认"盒"不认"路":
     * 取样门 (实色暗底) 自己会排除无盒绘制, 花面一笔不碰。 */
    return 1;
}


/* 面内实色填充 (调用方保证 y1<=B; x 钳到行宽内) */
static void VecFillBox(unsigned short* buf, int pitchW,
                       int x0, int y0, int x1, int y1, unsigned color)
{
    int yy, xx;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= pitchW) x1 = pitchW - 1;
    if (x1 < x0 || y1 < y0) return;
    for (yy = y0; yy <= y1; yy++)
    {
        unsigned short* row = buf + yy * pitchW;
        for (xx = x0; xx <= x1; xx++) row[xx] = (unsigned short)color;
    }
}

int __fastcall VecDrawTextEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5,
    int a6, int a7, int a8, int a9, int a10)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int brief = 0, r, k, menuTake = 0;
    int av[10];
    if (g_VT.LogAdv) VecCallerLog('D', caller, a3);
    /* 出界: DrawText 矩形普查 (定案即删)。盒若经 DrawText 画,
     * 矩形必含提示星座数 (393/569/480/384/329/118 附近)。
     * 命中任一或前 4 次无条件记, 上限 16 行。 */
    {
        static int n = 0;
        static unsigned seen[16];
        int av[10];
        int hit = 0, si, ai;
        av[0] = a1; av[1] = a2; av[2] = a3; av[3] = a4; av[4] = a5;
        av[5] = a6; av[6] = a7; av[7] = a8; av[8] = a9; av[9] = a10;
        for (ai = 0; ai < 10 && !hit; ai++)
        {
            int v = av[ai];
            if (v == 393 || v == 569 || v == 480 || v == 384 ||
                v == 329 || (v >= 100 && v <= 200)) hit = 1;
        }
        for (si = 0; si < 16; si++)
            if (seen[si] == caller) break;
        if (n < 16 && (hit || n < 4) && si >= 16)
        {
            char b1[16], fbuf[192];
            unsigned z = 0;
            seen[n] = caller; n++;
            z = AppendCStr(fbuf, z, "vdt: c=", sizeof(fbuf));
            z += HtoA(caller, fbuf + z);
            for (ai = 0; ai < 10; ai++)
            {
                z = AppendCStr(fbuf, z, " a", sizeof(fbuf));
                UtoA((unsigned)(ai + 1), b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                z = AppendCh(fbuf, z, '=', sizeof(fbuf));
                UtoA((unsigned)av[ai], b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            }
            LogLine(fbuf);
        }
    }
    brief = VecIsBriefCaller(caller);
    av[0] = a1; av[1] = a2; av[2] = a3; av[3] = a4; av[4] = a5;
    av[5] = a6; av[6] = a7; av[7] = a8; av[8] = a9; av[9] = a10;
    if (brief) VecBriefLog(caller, av, t);
    menuTake = VecMenuMatch(caller, a6, a7, a8);
    if (menuTake)
    {
        /* 收串+矩形 (只收首层): 给 Blit 层自有版式用 */
        if (g_MenuDepth == 0)
        {
            const wchar_t* ms = (const wchar_t*)a3;
            int mn = 0;
            if (ms && IsPlausiblePtr((void*)ms))
            {
                while (mn < MENU_MAXCH && ms[mn] && ms[mn] != 0x0Au)
                { g_MenuStr[mn] = ms[mn]; mn++; }
            }
            g_MenuStr[mn] = 0;
            g_MenuN = mn;
            g_MenuRX = a4; g_MenuRW = a6;
            g_MenuIdx = 0; g_MenuBuilt = 0;
        }
        g_MenuDepth++;
    }
    /* DrawText 不碰 (计时器侧栏叠字非列表 + 简报正文, 全部走原版;
     * 列表的隐藏/收集只在 Print 入口做, 高层级一次干掉, 不逐条;
     * 菜单按钮只立深度标志, 字形替换在 Blit 层做) */
    if (!g_OrigDrawText) { if (menuTake) g_MenuDepth--; return 0; }
    if (brief)
    {
        /* 简报正文盒加高 (界面适配字体, 精确匹配才改) */
        if (caller == 0x00621144u) VecBriefBox(&a4, &a5, &a6, &a7);
        g_DTDepth++;
        if (g_DTDepth == 1)
        {
            g_DTBlit0 = g_BlitN; g_DTBrief = 1;
            g_DTBody = (caller == 0x00621144u && a6 >= 400 && a7 >= 100);
        }
    }
    r = g_OrigDrawText(t, NULL, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    if (menuTake) g_MenuDepth--;
    if (brief)
    {
        unsigned n;
        g_DTDepth--;
        if (g_DTDepth != 0) return r;
        g_DTBrief = 0;
        g_DTBody = 0;
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

/* 盒拓宽缓存: GetDim 成功替换后记 (串指针/首字/旧宽/新宽),
 * Simple 入口核对后给盒 R 参数 (a6) 加 nw-ow。声明在 Print/Simple 之前。 */
static const wchar_t* g_LastDimPtr = NULL;
static unsigned g_LastDimW0 = 0;
static int g_LastDimOw = 0, g_LastDimNw = 0;
/* 上一帧电力提示 Print 落笔 x (Simple 入口的 x 自校验用) */
static int g_LastTipX = -1;
/* 当帧提示串宽 (Print 入口由表查得, Blit 首字画盒用, 0=未知跳过) */
static int g_TipTot = 0;
static int g_TipOw = 0;
/* 边缘位移 (Print 入口算, Blit 逐字消耗, 非提示串清零, 无泄漏) */
static int g_TipShift = 0;
static int g_TipShiftN = 0;
/* 深边缘推进 (v62 实验): tipx<4 时转交引擎的 a4 右推至 4,
 * 引擎锚点回屏内即解 cull (整串跳过), Blit/盒全活。
 * 只影响转发值与记录值, 中间 (a4>=4) 恒零零回归。 */
static int g_TipPush = 0;
/* 当帧提示落笔 (Print 入口记, 首字位置门用, 与颜色无关) */
static int g_TipY = -9999;
/* 当帧提示首字是否已扫描 (Print 入口清零, Blit 首字置位) */
static int g_BlitTipSeen = 0;
/* 提示 Print 序号 (精确+子串分支都加): vbox2 凭 ps= 对齐盒与 Print,
 * 数 Simple_/Print/盒三者个数, 找盖盒子的额外绘制 */
static unsigned g_TipSeq = 0;
/* 当 Print 已收几个落点内 Blit (vhead 取证用, 入口清零) */
static int g_HeadN = 0;
/* 边缘诊断 (定案即删): 当帧提示色 Blit 最小 x, Print 入口转存上一帧。
 * 若引擎在边缘滚字 (minx 远小于 tipx 甚至为负), 盒 (跟 tipx) 与字
 * (跟 Blit x) 必然错位。 */
static int g_BlitMinX = 9999;
static int g_PrevMinX = 9999;
static int g_BlitMaxX = -9999;
static int g_PrevMaxX = -9999;
static int g_BlitMinY = 9999;
static int g_PrevMinY = 9999;
static int g_BlitMaxY = -9999;
static int g_PrevMaxY = -9999;
static int g_MutModeLog = 0;
/* 世界悬浮提示活跨 (任意 0x4A5FF2 CJK 浮框, 电力/建筑名): Blit 层用它把
 * 悬浮文本排除出超武区接管。否则提示飘到右侧带时被 SuperPx 重绘且按引擎
 * 窄 adv 步进, 字被压窄左移、整串变短 -> 右侧留大空隙 (偏移与压缩根因)。 */
static int g_WtActive = 0;
static int g_WtX = 0, g_WtY = -9999, g_WtTot = 0;
/* 多槽宽度表 (单槽会被同帧其它串冲掉, 见 vfixq ow=30 教训):
 * ring 32 项, 按指针/首字+长度命中, 跨帧也有效。 */
#define DIMCACHE_N 32
typedef struct {
    const wchar_t* ptr;
    unsigned w0;
    int n, ow, nw;
} DimCE;
static DimCE g_DimC[DIMCACHE_N];
static int g_DimCPos = 0;
/* 防冲 sticky (边缘修复): 主表是 ring, 出屏后 GetDim 不再刷新,
 * 槽位被其它串冲掉致 Print 查表 miss (盒跳过、宽字溢窄盒)。
 * 此处按 (首字,长度) 常驻 16 槽, 同槽只认同 key 覆盖,
 * 取用时全串比对 (内容不同不用, 与 v49 同口径), 永不丢。 */
#define STICKY_N 16
typedef struct {
    int used;
    unsigned w0;
    int n, ow, nw;
    wchar_t tx[64];
} StkCE;
static StkCE g_StkC[STICKY_N];
static int g_StkRR = 0;

/* 全串宽度查表 (主 ring + sticky), 逐字内容全比对, 找不到返回 0。
 * 供世界悬浮提示排除超武区渲染用 (任意 0x4A5FF2 文本, 不限电力串)。 */
static int VecDimLookup(const wchar_t* s, int len)
{
    int ci, si, k;
    if (!s || len <= 0) return 0;
    for (ci = 0; ci < DIMCACHE_N; ci++)
    {
        DimCE* e = &g_DimC[ci];
        if (!e->ptr || e->w0 != ((unsigned)s[0] & 0xFFFFu) || e->n != len)
            continue;
        if (!IsPlausiblePtr((void*)e->ptr)) continue;
        for (k = 0; k < len; k++)
            if (e->ptr[k] != s[k]) break;
        if (k == len) return e->nw;
    }
    for (si = 0; si < STICKY_N; si++)
    {
        StkCE* st = &g_StkC[si];
        if (!st->used || st->w0 != ((unsigned)s[0] & 0xFFFFu) || st->n != len)
            continue;
        for (k = 0; k < len; k++)
            if (st->tx[k] != s[k]) break;
        if (k == len) return st->nw;
    }
    return 0;
}

/* 盒几何实测 (定案即删): 沿 y-4..y+28 找长红边 (>=40px 连续)。
 * ph=0: Print 入口 (盒已画字未画); ph=1: Simple 返回后 (全画完)。
 * 两相对照定边框绘制时机; 与帧轮转联动定宽度杠杆。上限 16 行。 */
static int g_ScanN = 0;
static void VecScanBoxBuf(unsigned short* bb, int pp, int x, int y,
                          int ph)
{
    char b1[16], fbuf[160];
    unsigned z = 0;
    int yy, xx, top = 0, bot = 0, bL = 0, bR = 0;
    int loose = 0, minS = 99999;
    unsigned minC = 0;
    if (g_ScanN >= 20) return;
    g_ScanN++;
    if (bb && IsPlausiblePtr((void*)bb) && pp >= 64 && pp <= 4096)
    {
        for (yy = y - 4; yy <= y + 28; yy++)
        {
            int runL = -1, runR = -1;
            if (yy < 0 || yy > 4096) continue;
            for (xx = x - 200; xx <= x + 400; xx++)
            {
                unsigned c, R, G, B;
                int isRed;
                if (xx < 0 || xx >= pp) continue;
                c = bb[yy * pp + xx];
                R = (c >> 11) & 31u; G = (c >> 5) & 63u; B = c & 31u;
                isRed = (R >= 12 && R > G + 6 && R > B + 6);
                if (R >= 8 && R > G + 2 && R > B + 2) loose++;
                {
                    int s2 = (int)R + (int)G + (int)B;
                    if (s2 < minS) { minS = s2; minC = c; }
                }
                if (isRed)
                {
                    if (runL < 0) runL = xx;
                    runR = xx;
                }
                else if (runL >= 0)
                {
                    if (runR - runL + 1 >= 40)
                    {
                        if (!top) { top = yy; bL = runL; bR = runR; }
                        else bot = yy;
                    }
                    runL = -1;
                }
            }
            if (runL >= 0 && runR - runL + 1 >= 40)
            {
                if (!top) { top = yy; bL = runL; bR = runR; }
                else bot = yy;
            }
        }
    }
    z = AppendCStr(fbuf, z, "vscan: m=", sizeof(fbuf));
    UtoA((unsigned)g_MutModeLog, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " ph=", sizeof(fbuf));
    UtoA((unsigned)ph, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " top=", sizeof(fbuf));
    UtoA((unsigned)top, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " bot=", sizeof(fbuf));
    UtoA((unsigned)bot, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " L=", sizeof(fbuf));
    UtoA((unsigned)bL, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " R=", sizeof(fbuf));
    UtoA((unsigned)bR, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " lr=", sizeof(fbuf));
    UtoA((unsigned)loose, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " dk=", sizeof(fbuf));
    z += HtoA(minC, fbuf + z);
    LogLine(fbuf);
}

int __fastcall VecPrintEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int r, toolTake = 0;
    int pushK = 0;
    if (g_VT.LogAdv) VecCallerLog('P', caller, a3);
    /* 电力提示落笔 x 记录 (Simple 盒 R 自校验用, 只读 First 字) */
    {
        const wchar_t* ps = (const wchar_t*)a3;
        if (ps && IsPlausiblePtr((void*)ps) &&
            (((unsigned)ps[0] & 0xFFFFu) == 0x7535u))
        {
            int tl = 0, ci;
            g_LastTipX = a4;
            g_TipY = a5;
            g_BlitTipSeen = 0;
            g_TipSeq++;
            g_HeadN = 0;
            g_PrevMinX = g_BlitMinX;
            g_BlitMinX = 9999;
            g_PrevMaxX = g_BlitMaxX;
            g_BlitMaxX = -9999;
            g_PrevMinY = g_BlitMinY;
            g_BlitMinY = 9999;
            g_PrevMaxY = g_BlitMaxY;
            g_BlitMaxY = -9999;
            /* 取本串宽度 (表对齐缓存): Blit 首字画盒用, 查不到=0跳过 */
            g_TipTot = 0;
            g_TipOw = 0;
            while (tl < 64 && ps[tl]) tl++;
            /* 精确内容匹配 (指针复用会 stale, 只认全串相同):
             * 首字+n 预滤, 再逐字比对, 对上才取宽。 */
            for (ci = 0; ci < DIMCACHE_N; ci++)
            {
                DimCE* e = &g_DimC[ci];
                int k, same;
                if (!e->ptr ||
                    e->w0 != ((unsigned)ps[0] & 0xFFFFu) ||
                    e->n != tl)
                    continue;
                if (!IsPlausiblePtr((void*)e->ptr)) continue;
                same = 1;
                for (k = 0; k < tl; k++)
                {
                    if (e->ptr[k] != ps[k]) { same = 0; break; }
                }
                if (same)
                {
                    int si, done = 0;
                    g_TipTot = e->nw; g_TipOw = e->ow;
                    /* 写透 sticky: 同 key 覆盖, 否则轮转新槽 */
                    for (si = 0; si < STICKY_N; si++)
                    {
                        StkCE* s = &g_StkC[si];
                        if (s->used && s->w0 == e->w0 && s->n == tl)
                        {
                            s->nw = e->nw; s->ow = e->ow;
                            for (k = 0; k < tl; k++) s->tx[k] = ps[k];
                            s->tx[tl] = 0;
                            done = 1;
                            break;
                        }
                    }
                    if (!done)
                    {
                        StkCE* s = &g_StkC[g_StkRR % STICKY_N];
                        g_StkRR++;
                        s->used = 1; s->w0 = e->w0; s->n = tl;
                        s->nw = e->nw; s->ow = e->ow;
                        for (k = 0; k < tl; k++) s->tx[k] = ps[k];
                        s->tx[tl] = 0;
                    }
                    break;
                }
            }
            /* 主表 miss 回 sticky (边缘 GetDim 停量后槽位被冲):
             * 全串比对, 对上才取宽。 */
            if (g_TipTot == 0 && tl >= 10 && tl < 64)
            {
                int si;
                for (si = 0; si < STICKY_N; si++)
                {
                    StkCE* s = &g_StkC[si];
                    int k2, same2;
                    if (!s->used ||
                        s->w0 != ((unsigned)ps[0] & 0xFFFFu) ||
                        s->n != tl)
                        continue;
                    same2 = 1;
                    for (k2 = 0; k2 < tl; k2++)
                    {
                        if (s->tx[k2] != ps[k2]) { same2 = 0; break; }
                    }
                    if (same2) { g_TipTot = s->nw; g_TipOw = s->ow; break; }
                }
            }
            /* vtip (边缘诊断, 定案即删): tipx/tot/minx/maxx 同行,
             * 变化触发, 上限 40, 长短串全记 (看 Print 是否批量交错;
             * minx/maxx=上一帧带内提示色 Blit 的实际起笔/终笔,
             * 与盒 [L0,R0] 对照即见错位)。
             * tipx/minx/maxx 偏置 +2000 (屏内恒正); minx=11999
             * 表示上一帧带内无提示色 Blit; tot=0 表示查表 miss (盒跳过)。 */
            {
                static int nV = 0;
                static int vX = -99999, vT = -1, vM = -99999, vA = -99999;
                static int vNY = -99999, vAY = -99999;
                {
                    /* Deep-zone line: first 8 always, then tipx<120 only.
                     * Cap 400. */
                    if (g_VT.LogDetail &&
                        (a4 != vX || g_TipTot != vT ||
                         g_PrevMinX != vM || g_PrevMaxX != vA ||
                         g_PrevMinY != vNY || g_PrevMaxY != vAY) &&
                        nV < 400 && (nV < 8 || a4 < 2120))
                {
                    char vb1[16], vf[192];
                    unsigned vz = 0;
                    nV++;
                    vX = a4; vT = g_TipTot;
                    vM = g_PrevMinX; vA = g_PrevMaxX;
                    vNY = g_PrevMinY; vAY = g_PrevMaxY;
                    vz = AppendCStr(vf, vz, "vtip: n=", sizeof(vf));
                    UtoA((unsigned)tl, vb1);
                    vz = AppendCStr(vf, vz, vb1, sizeof(vf));
                    vz = AppendCStr(vf, vz, " tipx=", sizeof(vf));
                    UtoA((unsigned)(a4 + 2000), vb1);
                    vz = AppendCStr(vf, vz, vb1, sizeof(vf));
                    vz = AppendCStr(vf, vz, " tot=", sizeof(vf));
                    UtoA((unsigned)g_TipTot, vb1);
                    vz = AppendCStr(vf, vz, vb1, sizeof(vf));
                    vz = AppendCStr(vf, vz, " minx=", sizeof(vf));
                    UtoA((unsigned)(g_PrevMinX + 2000), vb1);
                    vz = AppendCStr(vf, vz, vb1, sizeof(vf));
                    vz = AppendCStr(vf, vz, " maxx=", sizeof(vf));
                    UtoA((unsigned)(g_PrevMaxX + 2000), vb1);
                    vz = AppendCStr(vf, vz, vb1, sizeof(vf));
                    vz = AppendCStr(vf, vz, " miny=", sizeof(vf));
                    UtoA((unsigned)(g_PrevMinY + 2000), vb1);
                    vz = AppendCStr(vf, vz, vb1, sizeof(vf));
                    vz = AppendCStr(vf, vz, " maxy=", sizeof(vf));
                    UtoA((unsigned)(g_PrevMaxY + 2000), vb1);
                    vz = AppendCStr(vf, vz, vb1, sizeof(vf));
                    LogLine(vf);
                    }
                }
            }
            /* 边缘位移已退役: 位移恒零, 出界即真裁剪 (盒字同宽同锚,
             * 整体出屏, 无模式切换)。 */
            g_TipShift = 0;
            g_TipShiftN = 0;
            /* 推进已删除: 锚点出屏也不右推, 原样转交,
             * Blit 层盒字同几何真裁剪。 */
            pushK = 0;
            g_TipPush = 0;
        }
        else
        {
            g_TipShift = 0;
            g_TipShiftN = 0;
        }
    }
    /* Substring fallback (deep edge): engine passes only the visible
     * substring (first char != tip leader) -> exact branch ignores it,
     * seen never resets, box stalls while wide text keeps drawing.
     * Match qs against sticky full texts; on hit at offset k:
     * tipx = substring pen a4 (== visible start), tot = nw - k*avg
     * (avg = nw/n, few-px error), ow scaled the same.
     * Gates (anti-misfire): tl in [8,64), y within +-24 of last tip,
     * a4 within [tipx-64, tipx+400]. tot < 40 -> miss.
     * 注意: 不能再用 g_TipTot==0 当门 -- 它是全局量, 上一帧整串的宽会
     * 留在这里, 子串帧永远等不到 0, 于是盒用旧整串宽 -> 右侧留大空隙。
     * 改为: 非电字开头 (即不是精确整串) 或 表未命中时, 一律走子串匹配。 */
    {
        const wchar_t* qs = (const wchar_t*)a3;
        int ql = 0;
        if (qs && IsPlausiblePtr((void*)qs) &&
            (g_TipTot == 0 || ((unsigned)qs[0] & 0xFFFFu) != 0x7535u) &&
            g_TipY > -9000 && a5 >= g_TipY - 24 && a5 <= g_TipY + 24 &&
            a4 >= g_LastTipX - 64 && a4 <= g_LastTipX + 400)
        {
            int si;
            while (ql < 64 && qs[ql]) ql++;
            if (ql >= 4 && ql < 64)
            {
                for (si = 0; si < STICKY_N; si++)
                {
                    StkCE* s = &g_StkC[si];
                    int kk;
                    if (!s->used || s->n < ql) continue;
                    for (kk = 0; kk + ql <= s->n; kk++)
                    {
                        int m2, ok = 1;
                        for (m2 = 0; m2 < ql; m2++)
                        {
                            if (s->tx[kk + m2] != qs[m2])
                            {
                                ok = 0;
                                break;
                            }
                        }
                        if (!ok) continue;
                        {
                            /* 子串宽优先用表 (引擎若为子串单独度量过就有);
                             * 没有才按 avg 估。这样盒宽与可见字严格一致。 */
                            int ew = VecDimLookup(qs, ql);
                            int t2 = (ew > 0) ? ew : (s->nw - kk * s->nw / s->n);
                            int o2 = s->ow - kk * s->ow / s->n;
                            if (o2 < 40) o2 = t2;
                            if (t2 >= 40)
                            {
                                static int nS = 0;
                                 g_TipTot = t2; g_TipOw = o2;
                                 g_LastTipX = a4; g_TipY = a5;
                                 g_BlitTipSeen = 0;
                                 g_TipSeq++;
                                 g_HeadN = 0;
                                 /* 推进已删除 (同上, 出界整串走原版, 不左对齐) */
                                 pushK = 0;
                                 g_TipPush = 0;
                                if (nS < 20)
                                {
                                    char sb1[16], sf[128];
                                    unsigned sz = 0;
                                    nS++;
                                    sz = AppendCStr(sf, sz, "vsub: k=",
                                                    sizeof(sf));
                                    UtoA((unsigned)kk, sb1);
                                    sz = AppendCStr(sf, sz, sb1,
                                                    sizeof(sf));
                                    sz = AppendCStr(sf, sz, " tipx=",
                                                    sizeof(sf));
                                    UtoA((unsigned)(a4 + 2000), sb1);
                                    sz = AppendCStr(sf, sz, sb1,
                                                    sizeof(sf));
                                    sz = AppendCStr(sf, sz, " tot=",
                                                    sizeof(sf));
                                    UtoA((unsigned)t2, sb1);
                                    sz = AppendCStr(sf, sz, sb1,
                                                    sizeof(sf));
                                    sz = AppendCStr(sf, sz, " n=",
                                                    sizeof(sf));
                                    UtoA((unsigned)ql, sb1);
                                    sz = AppendCStr(sf, sz, sb1,
                                                    sizeof(sf));
                                    LogLine(sf);
                                }
                            }
                        }
                        break;
                    }
                    if (g_TipTot != 0) break;
                }
            }
        }
    }
    /* 出界: Print 收到的串 (m=6 若传子串, 即盒交集, 边界反推盒宽)。
     * 每种模式记一次, 定案即删。 */
    {
        static int logged = 0;
        int mode = g_MutModeLog;
        const wchar_t* ps = (const wchar_t*)a3;
        if (!(logged & (1 << mode)) && ps && IsPlausiblePtr((void*)ps) &&
            (((unsigned)ps[0] & 0xFFFFu) == 0x7535u) &&
            mode >= 0 && mode <= 6)
        {
            char b1[16], fbuf[96];
            unsigned z = 0;
            int nl = 0;
            logged |= (1 << mode);
            while (nl < 64 && ps[nl]) nl++;
            z = AppendCStr(fbuf, z, "vstr: m=", sizeof(fbuf));
            UtoA((unsigned)mode, b1);
            z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            z = AppendCStr(fbuf, z, " n=", sizeof(fbuf));
            UtoA((unsigned)nl, b1);
            z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            z = AppendCStr(fbuf, z, " s0=", sizeof(fbuf));
            z += HtoA((unsigned)ps[0] & 0xFFFFu, fbuf + z);
            z = AppendCStr(fbuf, z, " s1=", sizeof(fbuf));
            z += HtoA(nl >= 2 ? ((unsigned)ps[1] & 0xFFFFu) : 0,
                      fbuf + z);
            LogLine(fbuf);
        }
    }
    /* 盒几何实测 ph=0 (Print 入口): 见 VecScanBox (老内联版已退役)。
     * ph=3: 同扫 a2 surface 像素 (若画布与 surface 不是同一块内存,
     * 盒可能画在 surface 上)。vbuf 对比两块地址。 */
    {
        static int n = 0;
        const wchar_t* ps = (const wchar_t*)a3;
        if (ps && IsPlausiblePtr((void*)ps) &&
            (((unsigned)ps[0] & 0xFFFFu) == 0x7535u))
        {
            /* ph=0 (画布) 已证无效 (cbb=NULL), 退役省行数, 只扫 surface */
            if (g_VT.LogDetail && n < 300 && IsPlausiblePtr((void*)a2))
            {
                int* sd = (int*)a2;
                char b1[16], fbuf[128];
                unsigned z = 0;
                unsigned short* cbb = NULL;
                n++;
                if (IsPlausiblePtr((void*)a1))
                    cbb = *(unsigned short**)((char*)a1 + 0x0C);
                z = AppendCStr(fbuf, z, "vbuf: cbb=", sizeof(fbuf));
                z += HtoA((unsigned)cbb, fbuf + z);
                z = AppendCStr(fbuf, z, " spx=", sizeof(fbuf));
                z += HtoA((unsigned)sd[0], fbuf + z);
                z = AppendCStr(fbuf, z, " pp=", sizeof(fbuf));
                UtoA((unsigned)sd[1], b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                z = AppendCStr(fbuf, z, " xy=", sizeof(fbuf));
                UtoA((unsigned)a4, b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                z = AppendCh(fbuf, z, ',', sizeof(fbuf));
                UtoA((unsigned)a5, b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                LogLine(fbuf);
                VecScanBoxBuf((unsigned short*)sd[0], sd[1], a4, a5, 3);
            }
        }
    }
    /* 悬浮提示收串: CJK Print (超武带除外), 首层才收; 不认 outer */
    if (g_ToolDepth == 0 &&
        VecToolMatch(caller, (const wchar_t*)a3, a4, a5))
    {
        const wchar_t* ms = (const wchar_t*)a3;
        int mn = 0;
        while (mn < TOOL_MAXCH && ms[mn]) { g_ToolStr[mn] = ms[mn]; mn++; }
        g_ToolStr[mn] = 0;
        g_ToolN = mn;
        g_ToolX = a4; g_ToolY = a5;
        g_ToolIdx = 0; g_ToolBuilt = 0;
        toolTake = 1;
        g_ToolDepth++;
    }
    /* 超武列表整体隐藏+收集: 右侧面板区 CJK/冒号串, 命中即收表并跳过
     * 原调用 (字+黑底一起消失); 门不全 (面板关/异面/形状不符) 走原版。 */
    if (a4 >= 1300 && a5 >= 32)
    {
        const wchar_t* s = (const wchar_t*)a3;
        unsigned c0 = 0;
        int kind = -1;
        if (s && IsPlausiblePtr(s)) c0 = (unsigned)s[0] & 0xFFFFu;
        if ((c0 >= 0x2E80u && c0 < 0xA000u) ||
            (c0 >= 0xFF00u && c0 < 0xFFF0u))
            kind = 0;
        else if (VecSWColon(s))
            kind = 1;
        if (kind >= 0 && !VecSWHasEq(s) &&
            VecSWCapture((void*)a2, kind, s, a4, a5))
            return 1;
    }
    if (!g_OrigPrint) { if (toolTake) g_ToolDepth--; return 0; }
    /* 世界悬浮提示活跨 (CJK 浮框): 记落笔+全串宽, Blit 层据此把提示排
     * 除出超武区接管 (见 g_Wt 注释)。只在本 Print 的同步 Blit 期间有效,
     * 转发返回即清 (无泄漏)。表宽查不到 (首帧) 不激活, 下一帧起生效。 */
    g_WtActive = 0;
    {
        const wchar_t* ws = (const wchar_t*)a3;
        if (caller == 0x004A5FF2u && ws && IsPlausiblePtr((void*)ws))
        {
            unsigned c0 = (unsigned)ws[0] & 0xFFFFu;
            if ((c0 >= 0x2E80u && c0 < 0xA000u) ||
                (c0 >= 0xFF00u && c0 < 0xFFF0u))
            {
                int wl = 0, nw;
                while (wl < 64 && ws[wl]) wl++;
                nw = VecDimLookup(ws, wl);
                if (nw > 0)
                {
                    g_WtActive = 1;
                    g_WtX = a4; g_WtY = a5; g_WtTot = nw;
                }
            }
        }
    }
    r = g_OrigPrint(t, NULL, a1, a2, a3, a4 + pushK, a5, a6, a7);
    g_WtActive = 0;
    if (toolTake) g_ToolDepth--;
    return r;
}

/* 文本槽判定: CJK 首字=0 (名), 前 8 字含冒号=1 (计时器), 否则 -1。
 * 只认 EDX: ECX 是调用方暂存 (内容不定, 首字偶落 CJK 区);
 * 栈槽整数 (如 flags=0x4618) 也恰落 CJK 区, 碰不得。 */
static int VecSWTextKind(int slot, const wchar_t** out)
{
    const wchar_t* s;
    unsigned c;
    int k;
    if (!IsPlausiblePtr((void*)slot)) return -1;
    s = (const wchar_t*)slot;
    c = (unsigned)s[0] & 0xFFFFu;
    if ((c >= 0x2E80u && c < 0xA000u) ||
        (c >= 0xFF00u && c < 0xFFF0u)) { *out = s; return 0; }
    for (k = 0; k < 8; k++)
    {
        c = (unsigned)s[k] & 0xFFFFu;
        if (!c) break;
        if (c == 0x3Au) { *out = s; return 1; }
    }
    return -1;
}

/* Simple_ 入口: 命中 (主画布 + 落列表带 + 名/计时器) 即收表并跳过
 * (底+字同死); 空串只收表不跳过; 其余原样转发 (ECX/EDX 原值进跳板)。 */
int __fastcall VecSimpleEntry(int ecxIn, int edxIn,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    if (!g_OrigSimple) return 0;
    /* 提示盒跟字走 (电力提示出界修复): Simple_Wide 的盒 R 参数 a6
     * (盒右界=a6+2, 反汇编双盒路径同源) 取自上游旧字宽; 用 GetDim 缓存的
     * nw-ow 加宽后转发, 盒由引擎自画 (半透明+红边框原样, 一像素不碰)。
     * 三重门: 电力串 + 调用签名 (a4/a5/a7) + x 自校验 (a6≈x+ow-2)。
     * 首帧无缓存静默跳过 (视觉无差)。 */
    {
        static int nq = 0;
        const wchar_t* ts = (const wchar_t*)edxIn;
        /* 结构探测: a2/a3 解引用, 盒几何定位。
         * SW 已证 p2[0]+p3[0]=落笔x; 看 R/B 藏在哪个槽。
         * 上限 300 行: 数每 Print 对几个 Simple_, 找盖盒子的额外绘制。 */
        if (g_VT.LogDetail && nq < 300 && ts && IsPlausiblePtr((void*)ts) &&
            (((unsigned)ts[0] & 0xFFFFu) == 0x7535u))
        {
            char b1[16], fbuf[288];
            unsigned z = 0;
            int ci;
            nq++;
            z = AppendCStr(fbuf, z, "vss: q=", sizeof(fbuf));
            UtoA((unsigned)nq, b1);
            z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            z = AppendCStr(fbuf, z, " ps=", sizeof(fbuf));
            UtoA(g_TipSeq, b1);
            z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            z = AppendCStr(fbuf, z, " p2=", sizeof(fbuf));
            if (IsPlausiblePtr((void*)a2))
            {
                int* p2 = (int*)a2;
                for (ci = 0; ci < 4; ci++)
                {
                    if (ci) z = AppendCh(fbuf, z, ',', sizeof(fbuf));
                    UtoA((unsigned)p2[ci], b1);
                    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                }
            }
            else z = AppendCh(fbuf, z, 'X', sizeof(fbuf));
            z = AppendCStr(fbuf, z, " p3=", sizeof(fbuf));
            if (IsPlausiblePtr((void*)a3))
            {
                int* p3 = (int*)a3;
                for (ci = 0; ci < 4; ci++)
                {
                    if (ci) z = AppendCh(fbuf, z, ',', sizeof(fbuf));
                    UtoA((unsigned)p3[ci], b1);
                    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                }
            }
            else z = AppendCh(fbuf, z, 'X', sizeof(fbuf));
            z = AppendCStr(fbuf, z, " a6=", sizeof(fbuf));
            UtoA((unsigned)a6, b1);
            z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            z = AppendCStr(fbuf, z, " tipx=", sizeof(fbuf));
            UtoA((unsigned)(g_LastTipX + 1), b1);
            z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            z = AppendCStr(fbuf, z, " vr=", sizeof(fbuf));
            z += HtoA(VecFindDirect(), fbuf + z);
            z = AppendCStr(fbuf, z, " pp=", sizeof(fbuf));
            {
                int* p33 = NULL;
                int q = 0;
                if (IsPlausiblePtr((void*)a3))
                    q = ((int*)a3)[3];
                if (q && IsPlausiblePtr((void*)q)) p33 = (int*)q;
                if (p33)
                {
                    for (ci = 0; ci < 4; ci++)
                    {
                        int q2 = p33[ci];
                        if (ci) z = AppendCh(fbuf, z, ',',
                                            sizeof(fbuf));
                        UtoA((unsigned)q2, b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCh(fbuf, z, ':', sizeof(fbuf));
                        if (q2 && IsPlausiblePtr((void*)q2))
                            z += HtoA((unsigned)(*(wchar_t*)q2) &
                                      0xFFFFu, fbuf + z);
                        else
                            z = AppendCh(fbuf, z, 'X', sizeof(fbuf));
                    }
                }
                else z = AppendCh(fbuf, z, 'X', sizeof(fbuf));
            }
            LogLine(fbuf);
        }
    }
    /* 入口盒预检 (定案即删): Simple 运行前画布上已有盒 => 上游预画。
     * 鼠标不动时是上一帧残留 (fresh=0, 无判决力); 刚悬停的新位置
     * (fresh=1) 才定 Mis 时机: 有盒=上游画, 无盒=Simple 内或更晚画。
     * ph=2, 上限 4 组。 */
    {
        static int n = 0;
        static int lx = -9999, ly = -9999;
        const wchar_t* ts0 = (const wchar_t*)edxIn;
        if (n < 4 && ts0 && IsPlausiblePtr((void*)ts0) &&
            (((unsigned)ts0[0] & 0xFFFFu) == 0x7535u) &&
            IsPlausiblePtr((void*)a3))
        {
            int* p30 = (int*)a3;
            int fresh = (p30[0] != lx || p30[1] != ly);
            char b1[16], fbuf[64];
            unsigned z = 0;
            n++;
            lx = p30[0]; ly = p30[1];
            /* 入口扫 surface 真内存 (画布未锁定, ph=0/1/2 的 a1 扫描无效) */
            if (IsPlausiblePtr((void*)a1))
            {
                int* sd0 = (int*)a1;
                VecScanBoxBuf((unsigned short*)sd0[0], sd0[1],
                              p30[0], p30[1], 5);
            }
            z = AppendCStr(fbuf, z, "vfresh: f=", sizeof(fbuf));
            UtoA((unsigned)(fresh ? 1 : 0), b1);
            z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            LogLine(fbuf);
        }
    }
    if (g_SWPanelOn && g_SWCompW && IsPlausiblePtr((void*)a1))
    {
        int* b = (int*)a1;
        if (b[1] == g_SWCompW && b[2] == g_SWCompH &&
            IsPlausiblePtr((void*)a2) && IsPlausiblePtr((void*)a3))
        {
            int* p2 = (int*)a2;
            int* p3 = (int*)a3;
            const wchar_t* text = NULL;
            int kind = VecSWTextKind(edxIn, &text);   /* 文本只认 EDX */
            int n = 0, sx, sy;
            int inband;
            sx = p2[0] + p3[0]; sy = p2[1] + p3[1];
            inband = (sx >= 1300 && sx <= 1752 && sy >= 32 && sy <= 1080);
            if (text) { while (text[n] && n < 64) n++; }
            if (text && inband && !VecSWHasEq(text))
            {
                VecSWStore(kind, text, n, a4, sx, sy, 0);
                if (n > 0)
                {
                    if (IsPlausiblePtr((void*)ecxIn))
                    {
                        ((int*)ecxIn)[0] = sx;
                        ((int*)ecxIn)[1] = sy;
                    }
                    return ecxIn;   /* 与原版同口径 (pRetVal 非空) */
                }
            }
        }
    }
    {
        int r;
        r = g_OrigSimple(ecxIn, edxIn, a1, a2, a3, a4, a5, a6, a7);
        {
            /* ph=1: Simple 全画完后实测 (与 ph=0 对照定边框绘制时机) */
            const wchar_t* ts2 = (const wchar_t*)edxIn;
            if (ts2 && IsPlausiblePtr((void*)ts2) &&
                (((unsigned)ts2[0] & 0xFFFFu) == 0x7535u) &&
                IsPlausiblePtr((void*)a3))
            {
                int* p3 = (int*)a3;
                /* ph=1 (画布壳) 已证无效 (Simple 上下文 a1 是 surface,
                 * 按画布解读全是零), 退役。只扫 surface 真内存。 */
                if (IsPlausiblePtr((void*)a1))
                {
                    int* sd1 = (int*)a1;
                    VecScanBoxBuf((unsigned short*)sd1[0], sd1[1],
                                  p3[0], p3[1], 4);
                }
            }
        }
        return r;
    }
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

/* GetTextDimension 宽度接管: 引擎所有算字宽处 (盒子/居中/步进/换行)
 * 全走这里, 一处替换处处生效。只动单行适配宽度, 高度与换行逻辑原样:
 * 先走原版取 h (与原版同口径), w 用 GDI 自然步进和
 * (与绘制笔位同口径: ABC 全宽 + 数字 DigitTrack)。
 * 控制串 (\t \n 等)、超长 (>255 字)、触 maxW (换行逻辑) 一律走原版。
 * 重入守卫: 嵌套调用直接走原版 (防栈爆)。未开 Render 不接管。 */
static int g_GDDepth = 0;
/* 临时计数 (定案即删): 钩子改了谁的宽 */
static int g_DimLogs = 0;

/* GetDim 决策记录 (定案即删): 电力串每次度量的 maxW/原宽/替换与否。
 * 若上游用小 maxW 调 GetDim (ow>=maxW 走原版 fwd), 盒宽即从此漏出。
 * 上限 8 行。 */
static int g_Dim2N = 0;
static void VecDim2Log(const wchar_t* s, int maxW, int ow, int rep)
{
    char b1[16], fbuf[128];
    unsigned z = 0;
    if (g_Dim2N >= 64) return;
    if (!g_VT.LogDetail) return;
    if (!s || !IsPlausiblePtr((void*)s)) return;
    g_Dim2N++;
    z = AppendCStr(fbuf, z, "vdim2: maxW=", sizeof(fbuf));
    UtoA((unsigned)maxW, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " ow=", sizeof(fbuf));
    UtoA((unsigned)ow, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " rep=", sizeof(fbuf));
    UtoA((unsigned)rep, b1);
    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
    z = AppendCStr(fbuf, z, " s=", sizeof(fbuf));
    z += HtoA((unsigned)s[0] & 0xFFFFu, fbuf + z);
    LogLine(fbuf);
}

int __fastcall VecGetDimEntry(void* t, void* e, const wchar_t* s,
                              int* w, int* h, int maxW)
{
    int ow = 0, oh = 0, r;
    int px, i, tot;
    (void)e;
    if (!g_OrigGetDim) return 0;
    if (g_GDDepth > 0 || !g_VT.Render)
        return g_OrigGetDim(t, e, s, w, h, maxW);
    if (!t || !IsPlausiblePtr(t) || !s || !IsPlausiblePtr((void*)s) ||
        !w || !h || maxW <= 0)
        return g_OrigGetDim(t, e, s, w, h, maxW);
    g_GDDepth++;
    r = g_OrigGetDim(t, e, s, &ow, &oh, maxW);
    if (!r || ow >= maxW) goto fwd;   /* 失败/触顶: 原样 */
    px = VecPickPx(t);
    if (px < 8 || px > 40 || !GdiEnsure(px, g_VT.Bold, 2, g_VTFace))
        goto fwd;
    tot = 0;
    for (i = 0; i < 256; i++)
    {
        wchar_t wc = s[i];
        ABC abc;
        int ga;
        if (!wc) break;
        if (wc < 0x20) goto fwd;   /* 控制串: 原样 */
        if (!g_G || !g_G->dc ||
            !GetCharABCWidthsW(g_G->dc, wc, wc, &abc)) goto fwd;
        ga = abc.abcA + abc.abcB + abc.abcC;
        if (wc >= 0x30 && wc <= 0x39) ga += g_VT.DigitTrack;
        if (ga < 0) ga = 0;
        tot += ga;
        if (tot >= maxW) goto fwd;   /* 触顶 (换行逻辑): 原样 */
    }
    if (i >= 256) goto fwd;   /* 超长未见尾: 原样 */
    *w = tot; *h = oh;
    g_LastDimPtr = s; g_LastDimW0 = (unsigned)s[0] & 0xFFFFu;
    g_LastDimOw = ow; g_LastDimNw = tot;
    /* 字体解剖 (定案即删): 电力串上下文的字体对象字段 + 候选表探测。
     * 找内联求和读的那张表 (WORD[字] / BYTE[字]), 下版按 GDI 步进对齐。 */
    {
        static int n = 0;
        if (n < 2 && ((unsigned)s[0] & 0xFFFFu) == 0x7535u &&
            t && IsPlausiblePtr(t))
        {
            char b1[16], fbuf[256];
            unsigned z = 0;
            int fi;
            int* fo = (int*)t;
            n++;
            z = AppendCStr(fbuf, z, "vfont: t=", sizeof(fbuf));
            z += HtoA((unsigned)t, fbuf + z);
            z = AppendCStr(fbuf, z, " o=", sizeof(fbuf));
            for (fi = 0; fi < 12; fi++)
            {
                if (fi) z = AppendCh(fbuf, z, ',', sizeof(fbuf));
                UtoA((unsigned)fo[fi], b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            }
            LogLine(fbuf);
            z = 0;
            z = AppendCStr(fbuf, z, "vftab:", sizeof(fbuf));
            for (fi = 0; fi < 12; fi++)
            {
                int v = fo[fi];
                if (v && IsPlausiblePtr((void*)v) && (v & 3) == 0)
                {
                    unsigned short* tw = (unsigned short*)v;
                    unsigned char* tb = (unsigned char*)v;
                    z = AppendCStr(fbuf, z, " [", sizeof(fbuf));
                    UtoA((unsigned)fi, b1);
                    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                    z = AppendCh(fbuf, z, ']', sizeof(fbuf));
                    z += HtoA(v, fbuf + z);
                    z = AppendCStr(fbuf, z, " w=", sizeof(fbuf));
                    UtoA((unsigned)tw[0x7535], b1);
                    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                    z = AppendCStr(fbuf, z, " b=", sizeof(fbuf));
                    UtoA((unsigned)tb[0x7535], b1);
                    z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                }
            }
            LogLine(fbuf);
        }
    }
    {
        DimCE* e = &g_DimC[g_DimCPos % DIMCACHE_N];
        g_DimCPos++;
        e->ptr = s; e->w0 = (unsigned)s[0] & 0xFFFFu;
        e->n = i; e->ow = ow; e->nw = tot;
    }
    if (((unsigned)s[0] & 0xFFFFu) == 0x7535u)
        VecDim2Log(s, maxW, ow, 1);
    /* Change-triggered: 每帧同值烧 cap (120 行只够约 2 秒),
     * 只记 (px,ow,nw,n) 变化。 */
    if (g_VT.LogDetail && i >= 10)
    {
        static int dPx = -1, dOw = -1, dNw = -1, dN = -1;
        int moved = (px != dPx || ow != dOw || tot != dNw || i != dN);
        if (moved && g_DimLogs < 120)
        {
        char b1[16], dbuf[160];
        unsigned q = 0;
        g_DimLogs++;
        dPx = px; dOw = ow; dNw = tot; dN = i;
        q = AppendCStr(dbuf, q, "vdim: px=", sizeof(dbuf));
        UtoA((unsigned)px, b1); q = AppendCStr(dbuf, q, b1, sizeof(dbuf));
        q = AppendCStr(dbuf, q, " ow=", sizeof(dbuf));
        UtoA((unsigned)ow, b1); q = AppendCStr(dbuf, q, b1, sizeof(dbuf));
        q = AppendCStr(dbuf, q, " nw=", sizeof(dbuf));
        UtoA((unsigned)tot, b1); q = AppendCStr(dbuf, q, b1, sizeof(dbuf));
        q = AppendCStr(dbuf, q, " n=", sizeof(dbuf));
        UtoA((unsigned)i, b1); q = AppendCStr(dbuf, q, b1, sizeof(dbuf));
        q = AppendCStr(dbuf, q, " s=", sizeof(dbuf));
        q += HtoA((unsigned)s[0] & 0xFFFFu, dbuf + q);
        q = AppendCh(dbuf, q, ',', sizeof(dbuf));
        q += HtoA((unsigned)s[1] & 0xFFFFu, dbuf + q);
        LogLine(dbuf);
        }
    }
    g_GDDepth--;
    return r;
fwd:
    if (s && IsPlausiblePtr((void*)s) &&
        (((unsigned)s[0] & 0xFFFFu) == 0x7535u))
        VecDim2Log(s, maxW, ow, 0);
    if (r) { *w = ow; *h = oh; }
    g_GDDepth--;
    return r;
}

/* 引擎单字度量: 成功返回宽, 高进 *ph; 失败返回 -1。
 * 走原版 (笔位基准不动; 布局修正由 GetDim 钩做)。 */
static int VecMeasure(void* t, wchar_t wch, int* ph)
{
    wchar_t s[2];
    int w = 0, h = 0;
    if (!t || !IsPlausiblePtr(t) || !g_OrigGetDim) return -1;
    s[0] = wch; s[1] = 0;
    if (!g_OrigGetDim(t, NULL, s, &w, &h, 10000)) return -1;
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
/* VecRender 失败哨兵: 不能用负 x 判失败 (离屏字 x+step 本就为负)。 */
#define VC_FAIL (-1000000)

typedef struct {
    int used;
    wchar_t wch;
    void* idata;    /* BitFont+4, 字体重载即失效 */
    int px;         /* 校准字号, 换字号即换键 */
    int adv;
    int h;
    int outh;         /* 自然输出高度 (基准窗全高, 1:1, 不压缩) */
    int fullW;        /* 墨水右缘 (off+inkW): 数字按实际宽度回笔用 */
    int gdiW;         /* GDI 自然步进 (ABC 全宽, 含边距): 字间距用它才有缝 */
    int off, inkW;    /* 墨水在格内的摆位与宽度: 填充只包墨水用 */
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
    if (!g_LogDetail) return;
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

static VCEnt* VecGlyph(void* t, wchar_t wch, int adv, int h, int px)
{
    unsigned idx = ((unsigned)wch) & VC_MASK;
    VCEnt* e = &g_VC[idx];
    void* idata = *(void**)((char*)t + 4);
    int gw, dx, y, L, R, inkW, x, off, gdiW;
    /* 字形四周边距: GDI 墨水会超出 extent (左右 overhang), 顶边画必被裁。
     * 垂直用基准窗 1:1 映射 (见 VecRefCalibrate), 基线全字体一致,
     * 自然高度, 不压缩。 */
    int scanW;
    unsigned char* bits;
    int pitch;
    int winTop, outH;
    if (!t || !IsPlausiblePtr(t)) { g_VecWhy = 10; return NULL; }
    if (px < 8 || px > 40) { g_VecWhy = 11; return NULL; }
    if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
        e->adv == adv && e->h == h)
        return e;
    /* 未命中: GDI 单字光栅 (带边距), 掃出真实墨水盒再摆进格子 */
    if (!GdiEnsure(px, g_VT.Bold, 2, g_VTFace)) { g_VecWhy = 12; return NULL; }
    VecRefCalibrate(px);
    gw = GtRenderGlyphAt(wch, GL_M, GL_Y0, NULL);
    if (gw <= 0 || h > VC_MAXH) { g_VecWhy = 13; return NULL; }
    bits = g_G->bits;
    pitch = g_G->pitch;
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    /* 自然高度输出: 窗内行与输出行 1:1 (源行 = 窗顶 + y), 不压缩。
     * outH 由 (字号/粗体/字体) 唯一决定, 缓存命中即一致, 无需进键比较。 */
    winTop = g_RefT;
    outH = g_RefB - g_RefT + 1;
    if (outH < 1) outH = h;
    if (outH > VC_MAXH) { g_VecWhy = 14; return NULL; }
    if (winTop < 0 || winTop + outH > DIB_H) { g_VecWhy = 15; return NULL; }
    /* 墨水盒 (窗内全掃, 只定左右) */
    L = scanW; R = -1;
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
    }
    /* 摆位 (与行无关, 算一次): 满格 (CJK/数字) 左对齐, 与原版起笔一致;
     * 窄字形 (标点等, 墨水不足格宽 2/3) 按 GDI 自然边距摆,
     * 格子比 GDI 步进宽出的部分左右均分 (如":"不再贴边)。
     * 全墨水不压缩: 紧墨水盒原样端上, 溢出格子由面界收尾
     * (与 GDI 自然 overhang 一致)。任何挤压 (裁边/重采样/
     * 滤波) 都会窄化或切掉笔画, 已全部删除。
     * gdiW = GDI 自然步进 (ABC 全宽, 含边距): 字间距走它才有缝。 */
    inkW = (R >= L) ? R - L + 1 : 0;
    off = 0;
    gdiW = 0;
    if (g_G && g_G->dc)
    {
        ABC abc;
        if (GetCharABCWidthsW(g_G->dc, wch, wch, &abc))
        {
            gdiW = abc.abcA + abc.abcB + abc.abcC;
            if (inkW > 0 && inkW * 3 <= adv * 2)
            {
                off = abc.abcA + (adv - gdiW) / 2;
                if (off < 0) off = 0;
                if (off > adv - 1) off = adv - 1;
            }
        }
    }
    if (off + inkW > VC_MAXW) { g_VecWhy = 16; return NULL; }  /* 巨墨水走原版 */
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        unsigned char* dst = e->cov + y * VC_MAXW;
        for (dx = 0; dx < VC_MAXW; dx++) dst[dx] = 0;
        if (!inkW) continue;   /* 空白字 (空格): 占宽, 不画 */
        for (dx = 0; dx < inkW; dx++)
            dst[off + dx] = row[(L + dx) * 4 + 2];
    }
    e->used = 1; e->wch = wch; e->idata = idata; e->px = px; e->adv = adv; e->h = h;
    e->outh = outH;
    e->fullW = off + inkW;
    e->gdiW = gdiW;
    e->off = off; e->inkW = inkW;
    return e;
}

/* 矢量替换: 成功返回 X+实际笔位 (数字按 GDI 自然步进, 其它按原版 adv),
 * 与原版返回值口径一致 (调用方用它链下一笔), 失败返回 -1 走原版 */
static int VecRender(void* t, wchar_t wch, int adv, int h, int x, int y,
                     int colorW, int px, int yOff)
{
    unsigned short* buf;
    int pitchW, Tp, B, dx, dy;
    unsigned char fr, fg, fb;
    VCEnt* e;
    g_VecWhy = 0;
    if (!t || !IsPlausiblePtr(t)) { g_VecWhy = 1; return VC_FAIL; }
    if (adv > VC_MAXW || h > VC_MAXH) { g_VecWhy = 2; return VC_FAIL; }
    buf = *(unsigned short**)((char*)t + 0x0C);
    pitchW = *(int*)((char*)t + 0x10);
    if (!buf || !IsPlausiblePtr(buf)) { g_VecWhy = 3; return VC_FAIL; }
    if (pitchW < 64 || pitchW > 4096) { g_VecWhy = 4; return VC_FAIL; }
    Tp = *(int*)((char*)t + 0x34);
    B = *(int*)((char*)t + 0x3C);
    if (B < Tp) { g_VecWhy = 5; return VC_FAIL; }
    if (Tp < 0) Tp = 0;
    if (B > 4096) B = 4096;
    Calibrate();
    e = VecGlyph(t, wch, adv, h, px);
    if (!e) { if (g_VecWhy == 0) g_VecWhy = 6; return VC_FAIL; }
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
        /* 横向只认面界 (0..pitchW), 不认引擎裁剪: 全墨水溢出格子是
         * 正常的 GDI overhang, 切掉才是 bug。 */
        for (dx = 0; dx < VC_MAXW; dx++)
        {
            int sx = x + dx;
            unsigned a;
            if (sx < 0 || sx >= pitchW) continue;
            if (!cov[dx]) continue;
            a = cov[dx];   /* 灰度即 alpha, 全浓度 */
            row[sx] = Blend16(row[sx], fr, fg, fb, a);
        }
    }
    /* 步进: GDI 自然步进优先 (含边距, 字间自带缝), 只加宽不收窄。
     * 简报正文盒内 (引擎按自有宽度换行, 加宽会撑出盒子) 保持原笔位,
     * 数字除外 (正文里极少, 顶栏/计时器数字必须散开)。
     * 数字另吃 DigitTrack 微调 (负=收紧)。 */
    {
        int digit = (wch >= 0x30 && wch <= 0x39);
        int st = (e->gdiW > 0) ? e->gdiW + (digit ? g_VT.DigitTrack : 0)
                               : e->fullW;
        if (st > adv && (digit || !g_DTBody)) return x + st;
    }
    return x + adv;
}

/* 菜单字形缓存: 全墨水不挤压, 键 (wch,idata,px,adv,h), 存 off/inkW。
 * 巨墨水 (off+inkW > 32) 不接, 返回 NULL 走原版 (保底可读)。 */
#define MV_MASK 127
typedef struct {
    int used;
    wchar_t wch;
    void* idata;
    int px, adv, h;
    int off, inkW, outh;
    int gdiW;         /* GDI 自然步进 (ABC 全宽, 含边距): 菜单字间距用它 */
    unsigned char cov[VC_MAXW * VC_MAXH];
} MVEnt;
static MVEnt g_MV[MV_MASK + 1];

static MVEnt* VecMenuGlyph(void* t, wchar_t wch, int adv, int h, int px)
{
    unsigned idx = ((unsigned)wch) & MV_MASK;
    MVEnt* e = &g_MV[idx];
    void* idata = *(void**)((char*)t + 4);
    int gw, dx, y, L, R, inkW, x, off, gdiW;
    int scanW;
    unsigned char* bits;
    int pitch;
    int winTop, outH;
    if (!t || !IsPlausiblePtr(t)) return NULL;
    if (px < 8 || px > 40) return NULL;
    if (e->used && e->wch == wch && e->idata == idata && e->px == px &&
        e->adv == adv && e->h == h)
        return e;
    if (!GdiEnsure(px, g_VT.Bold, 2, g_VTFace)) return NULL;
    VecRefCalibrate(px);
    gw = GtRenderGlyphAt(wch, GL_M, GL_Y0, NULL);
    if (gw <= 0 || h > VC_MAXH) return NULL;
    bits = g_G->bits;
    pitch = g_G->pitch;
    scanW = GL_M + gw + GL_M;
    if (scanW > DIB_W) scanW = DIB_W;
    winTop = g_RefT;
    outH = g_RefB - g_RefT + 1;
    if (outH < 1) outH = h;
    if (outH > VC_MAXH) return NULL;
    if (winTop < 0 || winTop + outH > DIB_H) return NULL;
    /* 紧墨水盒 (只定左右, 全要, 不裁) */
    L = scanW; R = -1;
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        for (x = 0; x < scanW; x++)
            if (row[x * 4 + 2]) { if (x < L) L = x; if (x > R) R = x; }
    }
    inkW = (R >= L) ? R - L + 1 : 0;
    off = 0;
    gdiW = 0;
    if (g_G && g_G->dc)
    {
        ABC abc;
        if (GetCharABCWidthsW(g_G->dc, wch, wch, &abc))
        {
            gdiW = abc.abcA + abc.abcB + abc.abcC;
            if (inkW > 0 && inkW * 3 <= adv * 2)
            {
                off = abc.abcA + (adv - gdiW) / 2;
                if (off < 0) off = 0;
                if (off > adv - 1) off = adv - 1;
            }
        }
    }
    if (off + inkW > VC_MAXW) return NULL;
    for (y = 0; y < outH; y++)
    {
        unsigned char* row = bits + (winTop + y) * pitch;
        unsigned char* dst = e->cov + y * VC_MAXW;
        for (dx = 0; dx < VC_MAXW; dx++) dst[dx] = 0;
        if (!inkW) continue;   /* 空白字: 占宽, 不画 */
        for (dx = 0; dx < inkW; dx++)
            dst[off + dx] = row[(L + dx) * 4 + 2];
    }
    e->used = 1; e->wch = wch; e->idata = idata; e->px = px; e->adv = adv;
    e->h = h; e->off = off; e->inkW = inkW;
    e->outh = outH;
    e->gdiW = gdiW;
    return e;
}

/* 菜单步进: GDI 自然步进优先 (含边距, 字间自带缝), 无则墨水右缘,
 * 空白按引擎格。数字另吃 DigitTrack 微调。 */
static int VecMenuStep(MVEnt* me, int fallback)
{
    int st;
    if (!me) return fallback;
    if (me->gdiW <= 0)
        return (me->inkW > 0) ? me->off + me->inkW : fallback;
    st = me->gdiW;
    if (me->wch >= 0x30 && me->wch <= 0x39) st += g_VT.DigitTrack;
    if (st < 1) st = 1;
    return st;
}

/* 菜单字形绘制: 成功返回 X+adv (与原版笔位口径一致), 失败 -1 走原版。
 * 横向全墨水不裁 (只认面界 0..pitchW, 溢出格子由面界收尾, 与 GDI 自然
 * overhang 一致); 纵向摆位/裁剪与 VecRender 一致; 笔位/颜色走引擎。 */
static int VecMenuRender(void* t, wchar_t wch, int adv, int h, int x, int y,
                         int colorW, int px, int yOff)
{
    unsigned short* buf;
    int pitchW, Tp, B, dx, dy;
    unsigned char fr, fg, fb;
    MVEnt* e;
    if (!t || !IsPlausiblePtr(t)) return -1;
    if (adv > VC_MAXW || h > VC_MAXH) return -1;
    buf = *(unsigned short**)((char*)t + 0x0C);
    pitchW = *(int*)((char*)t + 0x10);
    if (!buf || !IsPlausiblePtr(buf)) return -1;
    if (pitchW < 64 || pitchW > 4096) return -1;
    Tp = *(int*)((char*)t + 0x34);
    B = *(int*)((char*)t + 0x3C);
    if (B < Tp) return -1;
    if (Tp < 0) Tp = 0;
    if (B > 4096) B = 4096;
    Calibrate();
    e = VecMenuGlyph(t, wch, adv, h, px);
    if (!e) return -1;
    GtUnpack16((unsigned short)(colorW & 0xFFFF), &fr, &fg, &fb);
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
    for (y = 0; y < e->outh; y++)
    {
        int sy = dy + y;
        unsigned short* row;
        unsigned char* cov;
        if (sy < 0 || sy > B) continue;
        row = buf + sy * pitchW;
        cov = e->cov + y * VC_MAXW;
        for (dx = 0; dx < e->inkW; dx++)
        {
            int sx = x + e->off + dx;
            unsigned a;
            if (sx < 0 || sx >= pitchW) continue;
            if (!cov[e->off + dx]) continue;
            a = cov[e->off + dx];
            row[sx] = Blend16(row[sx], fr, fg, fb, a);
        }
    }
    return x + adv;
}

/* ---------------- 超武逐行绘制已删除 ----------------
 * 整体接管改为: Print 入口隐藏+收集, 消息帧统一整块重绘 (见 MsgFont 超武面板)。
 * 逐行背景/自锁/虚表 (旧 VecSWRowDraw/VecSWBandFill/VecSWVt/VecSWDoLog) 不再需要。 */

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

/* 提示步进: GDI 自然步进 (含边距), 无则墨水右缘, 空白按引擎格。
 * 数字另吃 DigitTrack。自有版式无引擎约束, 不取 max。 */
static int VecToolStep(VCEnt* e, int adv)
{
    int st;
    if (!e) return (adv > 0) ? adv : 8;
    st = (e->gdiW > 0) ? e->gdiW : e->fullW;
    if (!st) return (adv > 0) ? adv : 8;
    if (e->wch >= 0x30 && e->wch <= 0x39) st += g_VT.DigitTrack;
    if (st < 1) st = 1;
    return st;
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
    /* 落笔即引擎坐标 (推进已删除, tx==x)。 */
    int tx = x - g_TipPush;
    int inTip = (g_LastTipX >= 0 && g_TipTot > 0 &&
                 tx >= g_LastTipX - g_TipTot - 16 &&
                 tx <= g_LastTipX + g_TipTot + 8 &&
                 y >= g_TipY - 8 && y <= g_TipY + 8);
    /* 世界悬浮提示 (电力/建筑名) 跨度: 提示字必须排除超武区接管。
     * 否则提示飘到右侧带时被换成 SuperPx 字体, 且返回引擎窄 adv,
     * 字被压窄左移、整串变短, 盒 (按全宽画) 右侧留大空隙。 */
    int inWt = (g_WtActive && g_WtTot > 0 &&
                tx >= g_WtX - 16 && tx <= g_WtX + g_WtTot + 8 &&
                y >= g_WtY - 8 && y <= g_WtY + 8);
    /* 出界: 提示首字 Blit 时扫有效画布 (盒若上游预画, 此时无字污染
     * 最干净; bb 与渲染同源, 必有效)。上限 4 帧, 定案即删。 */
    {
        static int n = 0;
        /* 帧跨度 (边缘诊断, 定案即删): 位置门 (颜色盲, y +-64),
         * 看引擎在边缘到底把字画在哪 (颜色门在深边缘已证盲)。
         * 邻串可能混入, 看随 tipx 联动的趋势, 不看单行。 */
        if (g_LastTipX >= 0 && g_TipTot > 0 &&
            y >= g_TipY - 64 && y <= g_TipY + 64 &&
            tx >= g_LastTipX - 350 &&
            tx <= g_LastTipX + g_TipTot + 64 &&
            t && IsPlausiblePtr(t))
        {
            if (tx < g_BlitMinX) g_BlitMinX = tx;
            if (tx > g_BlitMaxX) g_BlitMaxX = tx;
            if (y < g_BlitMinY) g_BlitMinY = y;
            if (y > g_BlitMaxY) g_BlitMaxY = y;
        }
    /* 首字取证 (vhead): 只抓近边缘批次 (tipx<40), 每批全部 Blit
     * (15 字上限) 的字/坐标/颜色, 定深边缘哪些字没发出来。
     * 上限 900 行 (屏内批次不占额度)。
     * (不用 inTip: 它含 tipx>=0, 深边缘恒假, 会漏掉正要抓的批次。) */
    if (g_VT.LogDetail && g_HeadN < 15 && g_TipTot > 0 && g_LastTipX < 40 &&
        tx >= g_LastTipX - g_TipTot - 16 &&
        tx <= g_LastTipX + g_TipTot + 8 &&
        y >= g_TipY - 8 && y <= g_TipY + 8 &&
        t && IsPlausiblePtr(t))
    {
        static int nH = 0;
        if (nH < 900)
        {
            char hb1[16], hf[144];
            unsigned hz = 0;
            nH++; g_HeadN++;
            hz = AppendCStr(hf, hz, "vhead: wch=", sizeof(hf));
            hz += HtoA((unsigned)wc & 0xFFFFu, hf + hz);
            hz = AppendCStr(hf, hz, " x=", sizeof(hf));
            UtoA((unsigned)(x + 2000), hb1);
            hz = AppendCStr(hf, hz, hb1, sizeof(hf));
            hz = AppendCStr(hf, hz, " y=", sizeof(hf));
            UtoA((unsigned)(y + 2000), hb1);
            hz = AppendCStr(hf, hz, hb1, sizeof(hf));
            hz = AppendCStr(hf, hz, " eff=", sizeof(hf));
            hz += HtoA((unsigned)eff & 0xFFFFu, hf + hz);
            hz = AppendCStr(hf, hz, " t=", sizeof(hf));
            hz += HtoA((unsigned)(size_t)t, hf + hz);
            hz = AppendCStr(hf, hz, " ps=", sizeof(hf));
            UtoA(g_TipSeq, hb1);
            hz = AppendCStr(hf, hz, hb1, sizeof(hf));
            LogLine(hf);
        }
    }
        /* 位置门 (颜色盲): 本 Print 的首个 Blit 落在当帧提示跨度内。
         * 边缘出屏字会被引擎跳过 (不调 Blit), 首 Blit 未必是串首,
         * 故按跨度 [tipx-tot-16, tipx+tot+8] 认; 不按原点 ±8。
         * 右界 tot+8: 本串字 ENGINE x 不会超 a4+tot (账本即此)。
         * 每批只进一次 (seen 守卫): 串内后续字的 tx 天然递增,
         * 绝不能按"漂移"重画盒, 否则盒跟着字走、越走越偏 (v65 教训)。
         * 单线程 Print→Blits 同步, 陈旧跨 Print 不可能, 无需漂移。 */
        if (!g_BlitTipSeen &&
            tx >= g_LastTipX - g_TipTot - 16 &&
            tx <= g_LastTipX + g_TipTot + 8 &&
            y >= g_TipY - 8 && y <= g_TipY + 8 &&
            t && IsPlausiblePtr(t))
        {
            unsigned short* bb = *(unsigned short**)((char*)t + 0x0C);
            int pp = *(int*)((char*)t + 0x10);
            g_BlitTipSeen = 1;
            if (n < 4)
            {
                n++;
                VecScanBoxBuf(bb, pp, x, y, 6);
            }
            /* 出界修复 (TipBox): 定制盒。引擎窄盒 [cx-63,cx+62] 已在,
             * 按列调映射 r=盒内/盒旁 (逐通道钳 0.15~0.95) 补两侧条带
             * (纹理保留, 同 tone 无缝), 采样红画新边框, 字随后画上。
             * 坐标全落在已扫描安全区内; 关游戏即无残留 (世界每帧重画)。
             * TipBox=0 关闭。 */
            /* 定制盒: 每批首个落点 Blit 进一次 (seen 守卫),
             * 串内后续字不进; 锚点/几何见下, 出屏即真裁剪。 */
            if (g_VT.TipBox && g_TipTot >= 50 && g_TipTot <= 400 &&
                bb && IsPlausiblePtr((void*)bb) &&
                pp >= 64 && pp <= 4096)
            {
                /* 盒锚点取 max(Print 落笔, 首 Blit x):
                 * 中间两者一致; 首字出屏被跳过时首 Blit 偏右 (前面
                 * 几个字已在屏外), 跟 Blit x 左沿才与可见字
                 * 对齐, 右沿仍用 Print 真右沿 (tipx+tot, 不多占);
                 * 取 min 会把盒钉在旧落笔、字溢出右沿 (图 2 的病)。
                 * cx (引擎窄盒中心) 仍按 Print 版式取, 与裁剪无关。 */
                int anchor = (tx > g_LastTipX) ? tx : g_LastTipX;
                int cx = g_LastTipX + g_TipTot / 2;
                int oldHW = (g_TipOw > 0 ? g_TipOw : 118) / 2 + 4;
                int oldL = cx - oldHW, oldR = cx + oldHW - 1;
                int newL = anchor - 4, newR = g_LastTipX + g_TipTot + 4;
                int yT = y - 2, yB = y + 18, ym = y + 9;
                int xx, yy;
                /* 边缘钳制: 只画屏内部分。oldL/oldR 随 ow 自适应
                 * (118->±63 实测; 长串按 ow+8 估, 截图校准)。 */
                int L0 = newL < 0 ? 0 : newL;
                int R0 = newR >= pp ? pp - 1 : newR;
                int mL = oldL + 2 < L0 ? L0 : oldL + 2;
                int mRb = oldR - 2 > R0 ? R0 : oldR - 2;
                int sL1 = L0, sL2 = oldL - 1 > R0 ? R0 : oldL - 1;
                int sR1 = oldR + 1 < L0 ? L0 : oldR + 1, sR2 = R0;
                int fL1 = L0, fL2 = oldL > R0 ? R0 : oldL;
                int fR1 = oldR < L0 ? L0 : oldR, fR2 = R0;
                int cL = oldL < L0 ? L0 : oldL;
                int cR = oldR > R0 ? R0 : oldR;
                /* 真裁剪 (无模式切换): 盒与字永远同宽同锚
                 * ([anchor-4, tipx+tot+4]), 作为一个整体进出屏幕;
                 * 填充/采样天然钳在屏内 (L0/R0), 出屏侧不画边框。
                 * v67 序号证 Print:Simple_=1:1, 无额外绘制;
                 * 此前"突然缩短"是出界回落在 tipx≈4 处跳变, 已删。 */
                if (L0 <= R0 && yT >= 0 && yB <= 4096)
                {
                    long mR = 0, mG = 0, mB = 0, bR = 0, bG = 0, bB = 0;
                    long mN = 0, bN = 0;
                    long rR, rG, rB;
                    unsigned bc = 0xF800u;
                    unsigned bestR = 0;
                    char b1[16], fbuf[96];
                    unsigned z = 0;
                    static int n2 = 0;
                    static int lastL = -9999, lastR = -9999;
                    for (xx = mL; xx <= mRb; xx++)
                    {
                        unsigned c = bb[ym * pp + xx];
                        mR += (c >> 11) & 31u; mG += (c >> 5) & 63u;
                        mB += c & 31u; mN++;
                    }
                    /* 地面采样只取真盒外像素: 出屏侧钳入段落在真盒内
                     * (盒 tone), 会把比例拉偏、接缝露馅, 故跳过,
                     * 用另一侧邻地 ( tones 一致, 同一材质)。 */
                    if (newL >= 0)
                    for (xx = sL1; xx <= sL2; xx++)
                    {
                        unsigned c = bb[ym * pp + xx];
                        bR += (c >> 11) & 31u; bG += (c >> 5) & 63u;
                        bB += c & 31u; bN++;
                    }
                    if (newR < pp)
                    for (xx = sR1; xx <= sR2; xx++)
                    {
                        unsigned c = bb[ym * pp + xx];
                        bR += (c >> 11) & 31u; bG += (c >> 5) & 63u;
                        bB += c & 31u; bN++;
                    }
                    rR = (bR > 0 && mN > 0 && bN > 0) ?
                        (mR * bN * 1000 / (bR * mN)) : 600;
                    rG = (bG > 0 && mN > 0 && bN > 0) ?
                        (mG * bN * 1000 / (bG * mN)) : 600;
                    rB = (bB > 0 && mN > 0 && bN > 0) ?
                        (mB * bN * 1000 / (bB * mN)) : 600;
                    if (rR < 150) rR = 150;
                    if (rR > 950) rR = 950;
                    if (rG < 150) rG = 150;
                    if (rG > 950) rG = 950;
                    if (rB < 150) rB = 150;
                    if (rB > 950) rB = 950;
                    for (xx = cL; xx <= cR; xx++)
                    {
                        unsigned c = bb[yT * pp + xx];
                        unsigned R = (c >> 11) & 31u;
                        if (R > bestR)
                        {
                            bestR = R; bc = c;
                        }
                    }
                    for (yy = yT + 1; yy <= yB - 1; yy++)
                    {
                        for (xx = fL1 + 1; xx <= fL2; xx++)
                        {
                            unsigned c = bb[yy * pp + xx];
                            unsigned R = ((c >> 11) & 31u) * (unsigned)rR
                                / 1000u;
                            unsigned G = ((c >> 5) & 63u) * (unsigned)rG
                                / 1000u;
                            unsigned B = (c & 31u) * (unsigned)rB / 1000u;
                            if (R > 31u) R = 31u;
                            if (G > 63u) G = 63u;
                            if (B > 31u) B = 31u;
                            bb[yy * pp + xx] = (unsigned short)
                                ((R << 11) | (G << 5) | B);
                        }
                        for (xx = fR1; xx < fR2; xx++)
                        {
                            unsigned c = bb[yy * pp + xx];
                            unsigned R = ((c >> 11) & 31u) * (unsigned)rR
                                / 1000u;
                            unsigned G = ((c >> 5) & 63u) * (unsigned)rG
                                / 1000u;
                            unsigned B = (c & 31u) * (unsigned)rB / 1000u;
                            if (R > 31u) R = 31u;
                            if (G > 63u) G = 63u;
                            if (B > 31u) B = 31u;
                            bb[yy * pp + xx] = (unsigned short)
                                ((R << 11) | (G << 5) | B);
                        }
                    }
                    for (xx = L0; xx <= R0; xx++)
                    {
                        bb[yT * pp + xx] = (unsigned short)bc;
                        bb[yB * pp + xx] = (unsigned short)bc;
                    }
                    /* 竖边框只画屏内段: 出屏侧顶着屏沿画会造出一条
                     * 本不存在的假边框 (左对齐即此), 直接跳过。 */
                    for (yy = yT + 1; yy <= yB - 1; yy++)
                    {
                        if (newL >= 0)
                            bb[yy * pp + L0] = (unsigned short)bc;
                        if (newR < pp)
                            bb[yy * pp + R0] = (unsigned short)bc;
                    }
                    /* 深水区专线 (长 session 按位移记必烧穿): 前 8 行必记,
                     * 之后只记 L0<120。上限 400。定案即删。 */
                    {
                        if (g_VT.LogDetail &&
                            (L0 != lastL || R0 != lastR) && n2 < 400 &&
                            (n2 < 8 || L0 < 120))
                    {
                        n2++;
                        lastL = L0; lastR = R0;
                        z = AppendCStr(fbuf, z, "vbox2: box=",
                                       sizeof(fbuf));
                        UtoA((unsigned)L0, b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCh(fbuf, z, ',', sizeof(fbuf));
                        UtoA((unsigned)R0, b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCStr(fbuf, z, " tot=", sizeof(fbuf));
                        UtoA((unsigned)g_TipTot, b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCStr(fbuf, z, " r=", sizeof(fbuf));
                        UtoA((unsigned)rR, b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCh(fbuf, z, ',', sizeof(fbuf));
                        UtoA((unsigned)rG, b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCh(fbuf, z, ',', sizeof(fbuf));
                        UtoA((unsigned)rB, b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCStr(fbuf, z, " sh=", sizeof(fbuf));
                        UtoA((unsigned)(g_TipShift + 1000), b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCStr(fbuf, z, " ps=", sizeof(fbuf));
                        UtoA(g_TipSeq, b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCStr(fbuf, z, " tx=", sizeof(fbuf));
                        UtoA((unsigned)(tx + 2000), b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        z = AppendCStr(fbuf, z, " tipx=", sizeof(fbuf));
                        UtoA((unsigned)(g_LastTipX + 2000), b1);
                        z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                        LogLine(fbuf);
                        }
                    }
                }
            }
        }
    }
    /* 边缘位移: 本 Print 剩余字统一加 g_TipShift (边缘提示回屏)。
     * Print 入口逐次重置 (else 清零), 无泄漏; 菜单/工具分支内不加
     * (防串扰); 只影响本串像素, 引擎账本用原 x (返回值去位移,
     * 防引擎拿返回值步进造成累加拉长)。 */
    int ox = x;
    if (g_TipShiftN > 0 && g_MenuDepth == 0 && g_ToolDepth == 0)
    {
        x += g_TipShift;
        g_TipShiftN--;
    }
    /* 出界: 读画布裁剪 (盒矩形的信使)。提示色 (0xA92A/m5 变体) 才记,
     * 宽度变化才记, 上限 8 行。m=6 (a7=0) 的裁剪宽度即盒宽。 */
    {
        static int n = 0;
        static int lastW = -1;
        if (n < 8 && (eff == 0xA92Au || eff == 0x292Au) &&
            t && IsPlausiblePtr(t))
        {
            int* cp = (int*)((char*)t + 0x30);
            int w = cp[2] - cp[0];
            if (w != lastW && w > 0 && w < 2000)
            {
                char b1[16], fbuf[128];
                unsigned z = 0;
                lastW = w; n++;
                z = AppendCStr(fbuf, z, "vclip: m=", sizeof(fbuf));
                UtoA((unsigned)g_MutModeLog, b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                z = AppendCStr(fbuf, z, " L=", sizeof(fbuf));
                UtoA((unsigned)cp[0], b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                z = AppendCStr(fbuf, z, " T=", sizeof(fbuf));
                UtoA((unsigned)cp[1], b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                z = AppendCStr(fbuf, z, " R=", sizeof(fbuf));
                UtoA((unsigned)cp[2], b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                z = AppendCStr(fbuf, z, " B=", sizeof(fbuf));
                UtoA((unsigned)cp[3], b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
                LogLine(fbuf);
            }
        }
    }
    /* 出界: BitFont 对象状态快照 (盒边界的藏身处)。
     * 提示色门控, 每种模式记首字 (m=0/5/6 各一行), 对比找差异字段。
     * 定案即删。 */
    {
        static int logged = 0;
        int mode = g_MutModeLog;
        if (!(logged & (1 << mode)) &&
            (eff == 0xA92Au || eff == 0x292Au) &&
            t && IsPlausiblePtr(t) && mode >= 0 && mode <= 6)
        {
            char b1[16], fbuf[224];
            unsigned z = 0;
            int fi;
            int* fo = (int*)t;
            logged |= (1 << mode);
            z = AppendCStr(fbuf, z, "vdump: m=", sizeof(fbuf));
            UtoA((unsigned)mode, b1);
            z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            z = AppendCStr(fbuf, z, " o=", sizeof(fbuf));
            for (fi = 0; fi < 16; fi++)
            {
                if (fi) z = AppendCh(fbuf, z, ',', sizeof(fbuf));
                UtoA((unsigned)fo[fi], b1);
                z = AppendCStr(fbuf, z, b1, sizeof(fbuf));
            }
            LogLine(fbuf);
        }
    }
    if (g_ToolDepth > 0)
    {
        /* 悬浮提示自有版式+背景盒自适应: 首 Blit 建全串字形算总宽,
         * 取样是实色暗底才拓盒 (花面无盒不碰), 之后自有笔位画字,
         * 原字形一次不画。串序对不上/建失败回落引擎笔位画自定义字形,
         * 再失败走原版 (保底可读)。 */
        px = VecPickPx(t);
        if (px >= 0 && g_ToolN > 0 && g_ToolBuilt == 0)
        {
            int i, tot = 0, ok = 1, oh = 0;
            int ink0 = 0, inkEnd = 0, seenInk = 0;
            for (i = 0; i < g_ToolN; i++)
            {
                int ca, chh = 0, st;
                VCEnt* ge;
                ca = VecMeasure(t, g_ToolStr[i], &chh);
                if (ca < 0) ca = 8;
                g_ToolAdv[i] = ca;
                ge = VecGlyph(t, g_ToolStr[i], ca, h, px);
                if (!ge) { ok = 0; break; }
                st = VecToolStep(ge, ca);
                g_ToolStepA[i] = st;
                if (ge->inkW > 0)
                {
                    if (!seenInk) { ink0 = tot; seenInk = 1; }
                    inkEnd = tot + st;
                }
                tot += st;
                if (i == 0) oh = ge->outh;
            }
            g_ToolInk0 = ink0;
            g_ToolInkEnd = seenInk ? inkEnd : 0;
            if (ok && oh > 0)
            {
                unsigned short* bb = *(unsigned short**)((char*)t + 0x0C);
                int pp = *(int*)((char*)t + 0x10);
                int Tp = *(int*)((char*)t + 0x34);
                int B = *(int*)((char*)t + 0x3C);
                int dy = y + (h - oh) / 2;
                int lo = Tp, hi = B - oh + 1, ym, k, flat = 1;
                unsigned first = 0;
                if (B > 4096) B = 4096;
                if (hi >= lo)
                {
                    if (dy < lo) dy = lo;
                    if (dy > hi) dy = hi;
                }
                ym = dy + oh / 2;
                {
                    unsigned nowG = GetTickCount();
                    int dytG = (int)nowG - (int)g_ToolLastTick;
                    int contig = (dytG >= 0 && dytG < 500 &&
                        y - g_ToolLastY >= -4 && y - g_ToolLastY <= 4 &&
                        g_ToolX - g_ToolLastX1 >= -8 &&
                        g_ToolX - g_ToolLastX1 <= 64);
                    int inkBytes = g_ToolInkEnd - g_ToolInk0;
                    int surfOK = (bb && IsPlausiblePtr(bb) && pp >= 64 &&
                                  pp <= 4096 && ym >= 0 && ym <= B);
                    /* 相邻段沿用门判决: 后帧取样会撞上自己上一帧的字
                     * (自盲, 门永远不过); 8 秒以上重取样,
                     * 防提示挪到花面还照填 */
                    if (contig && surfOK && g_ToolGateOK && inkBytes > 0 &&
                        nowG - g_ToolGateTick < 8000u)
                    {
                        flat = 1;
                        first = g_ToolGateColor;
                    }
                    else if (surfOK && inkBytes > 0)
                    {
                        for (k = 0; k < 4; k++)
                        {
                            int sx = g_ToolX + g_ToolInk0 + 2 + k * 4;
                            unsigned c;
                            if (sx < 0 || sx >= pp) { flat = 0; break; }
                            c = bb[ym * pp + sx];
                            if (k == 0) first = c;
                            else if (c != first) { flat = 0; break; }
                        }
                        if (flat)
                        {
                            unsigned r5 = (first >> 11) & 31u;
                            unsigned g6 = (first >> 5) & 63u;
                            unsigned b5 = first & 31u;
                            if (r5 > 1u || g6 > 2u || b5 > 1u) flat = 0;
                        }
                        g_ToolGateOK = flat;
                        g_ToolGateColor = first;
                        g_ToolGateTick = nowG;
                    }
                    else flat = 0;
                    if (flat)
                    {
                        /* 填充只包墨水 (前导/尾随空格不算,
                         * 防盖住行首图标); 相邻段从上段尾巴起笔, 无缝 */
                        int x0 = g_ToolX + g_ToolInk0 - 4;
                        int x1 = g_ToolX + g_ToolInkEnd + 4;
                        int y0 = dy - 2, y1 = dy + oh + 2;
                        if (contig && x0 < g_ToolLastX1) x0 = g_ToolLastX1;
                        if (y1 > B) y1 = B;
                        if (g_VT.ToolBox == 1)
                        {
                            VecFillBox(bb, pp, x0, y0, x1, y1,
                                       (unsigned short)first);
                        }
                        else
                        {
                            /* 探测模式 (ToolBox=2): 只量数不落笔。
                             * 沿中线向两侧扫实色连通域, 得盒宽假设 run;
                             * 与字总宽 tot 对比, 看字是否溢出盒。 */
                            int sxL = g_ToolX + g_ToolInk0 + 2;
                            int sxR = g_ToolX + g_ToolInkEnd + 2;
                            int lx = sxL, rx;
                            while (lx > 0 && sxL - lx < 600 &&
                                bb[ym * pp + lx - 1] ==
                                (unsigned short)first) lx--;
                            if (sxR < 0 || sxR >= pp ||
                                bb[ym * pp + sxR] !=
                                (unsigned short)first)
                                rx = -1;
                            else
                            {
                                rx = sxR;
                                while (rx < pp - 1 && rx - sxR < 600 &&
                                    bb[ym * pp + rx + 1] ==
                                    (unsigned short)first) rx++;
                            }
                        }
                    }
                }
                g_ToolPenX = g_ToolX;
                /* 探测模式 (ToolBox=2) 只量数: 不自有笔位,
                 * 全串引擎笔位正常渲染, 视觉与平时无异。 */
                g_ToolBuilt = (g_VT.ToolBox == 2) ? -1 : 1;
            }
            else g_ToolBuilt = -1;
        }
        if (g_ToolBuilt == 1 && g_ToolIdx < g_ToolN &&
            wc == g_ToolStr[g_ToolIdx])
        {
            VCEnt* te;
            int step;
            yOff = g_VT.VShift;
            r = VecRender(t, wc, g_ToolAdv[g_ToolIdx], h, g_ToolPenX,
                          y, eff, px, yOff);
            if (r != VC_FAIL)
            {
                te = VecGlyph(t, wc, g_ToolAdv[g_ToolIdx], h, px);
                step = VecToolStep(te, g_ToolAdv[g_ToolIdx]);
                g_ToolPenX += step;
                g_ToolIdx++;
                g_ToolLastX1 = g_ToolPenX;
                g_ToolLastY = y;
                g_ToolLastTick = GetTickCount();
                return x + adv;   /* 引擎账本照旧, 笔位我们自己走 */
            }
        }
        if (px >= 0)
        {
            yOff = g_VT.VShift;
            r = VecRender(t, wc, adv, h, x, y, eff, px, yOff);
            if (r != VC_FAIL) return r - (x - ox);
        }
        return g_OrigBlit(t, NULL, wch, ox, y, color);
    }
    if (g_MenuDepth > 0)
    {
        /* 菜单整行自有版式: 首 Blit 建全串字形、算总宽、矩形内居中起笔,
         * 之后按实际墨水宽度走笔 (空格按引擎 adv, 只加宽不收窄)。
         * 串序对上才用自有笔位, 否则回落引擎笔位 (保底不错位);
         * 原字形一次不画, 双双失败才走原版 (保底可读)。 */
        int built = g_MenuBuilt;
        px = VecPickPx(t);
        if (px >= 0 && g_MenuN > 0 && !built)
        {
            int i, tot = 0, ok = 1;
            for (i = 0; i < g_MenuN; i++)
            {
                int ca, chh = 0;
                MVEnt* gi;
                ca = VecMeasure(t, g_MenuStr[i], &chh);
                if (ca < 0) ca = 8;
                g_MenuAdv[i] = ca;
                gi = VecMenuGlyph(t, g_MenuStr[i], ca, h, px);
                if (!gi) { ok = 0; break; }
                tot += VecMenuStep(gi, ca);
            }
            if (ok)
            {
                g_MenuPenX = g_MenuRX + (g_MenuRW - tot) / 2;
                if (g_MenuPenX < g_MenuRX) g_MenuPenX = g_MenuRX;
                g_MenuBuilt = 1;
                built = 1;
            }
        }
        if (built && g_MenuIdx < g_MenuN && wc == g_MenuStr[g_MenuIdx])
        {
            MVEnt* me;
            int step;
            yOff = g_VT.VShift;
            r = VecMenuRender(t, wc, g_MenuAdv[g_MenuIdx], h, g_MenuPenX,
                              y, eff, px, yOff);
            if (r >= 0)
            {
                me = VecMenuGlyph(t, wc, g_MenuAdv[g_MenuIdx], h, px);
                step = VecMenuStep(me, g_MenuAdv[g_MenuIdx]);
                if (step < 1) step = 1;
                g_MenuPenX += step;
                g_MenuIdx++;
                return x + adv;   /* 引擎账本照旧, 笔位我们自己走 */
            }
        }
        if (px >= 0)
        {
            yOff = g_VT.VShift;
            r = VecMenuRender(t, wc, adv, h, x, y, eff, px, yOff);
            if (r >= 0) return r;
        }
        return g_OrigBlit(t, NULL, wch, x, y, color);
    }
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
            /* 悬浮提示不属于超武列表: 即使落进右侧带也不许 SuperPx 接管,
             * 否则窄 adv 步进会把整串压短 (偏移/空隙的根因)。 */
            if (swForce && (inTip || inWt)) swForce = 0;
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
                 * 压成 1 (整串字挤成细条)。 */
                adv = (adv * effPx + g_FontOrigH / 2) / g_FontOrigH;
                if (adv < 1) adv = 1;
            }
            yOff = g_VT.VShift + (cred ? g_VT.CreditDY : 0);
            r = VecRender(t, wc, adv, h, (inTip ? tx : x), y, eff,
                          px, yOff);
            /* 失败取证 (vfb): VecRender 掉回原版时记一行 (字/坐标/失败点),
             * 只记失败, 上限 60。这是"字被压窄左移"的直接证据。 */
            if (r == VC_FAIL && g_TipTot > 0 && g_TipY > -9000)
            {
                static int nF = 0;
                static unsigned lastF = 0xFFFFFFFFu;
                unsigned sig = ((unsigned)wc << 8) ^ (unsigned)g_VecWhy;
                if (nF < 60 && sig != lastF)
                {
                    char fb1[16], ff[128];
                    unsigned fz = 0;
                    lastF = sig; nF++;
                    fz = AppendCStr(ff, fz, "vfb: wch=", sizeof(ff));
                    fz += HtoA((unsigned)wc & 0xFFFFu, ff + fz);
                    fz = AppendCStr(ff, fz, " x=", sizeof(ff));
                    UtoA((unsigned)(tx + 2000), fb1);
                    fz = AppendCStr(ff, fz, fb1, sizeof(ff));
                    fz = AppendCStr(ff, fz, " y=", sizeof(ff));
                    UtoA((unsigned)y, fb1);
                    fz = AppendCStr(ff, fz, fb1, sizeof(ff));
                    fz = AppendCStr(ff, fz, " adv=", sizeof(ff));
                    UtoA((unsigned)adv, fb1);
                    fz = AppendCStr(ff, fz, fb1, sizeof(ff));
                    fz = AppendCStr(ff, fz, " why=", sizeof(ff));
                    UtoA((unsigned)g_VecWhy, fb1);
                    fz = AppendCStr(ff, fz, fb1, sizeof(ff));
                    fz = AppendCStr(ff, fz, " ps=", sizeof(ff));
                    UtoA(g_TipSeq, fb1);
                    fz = AppendCStr(ff, fz, fb1, sizeof(ff));
                    LogLine(ff);
                }
            }
            if (g_VT.LogDetail)
            {
                static int nR = 0;
                if (nR < 400 && g_TipTot > 0 && g_LastTipX < 40 &&
                    tx >= g_LastTipX - g_TipTot - 16 &&
                    tx <= g_LastTipX + g_TipTot + 8 &&
                    y >= g_TipY - 8 && y <= g_TipY + 8)
                {
                    char rb1[16], rf[160];
                    unsigned rz = 0;
                    nR++;
                    rz = AppendCStr(rf, rz, "vren: wch=", sizeof(rf));
                    rz += HtoA((unsigned)wc & 0xFFFFu, rf + rz);
                    rz = AppendCStr(rf, rz, " adv=", sizeof(rf));
                    UtoA((unsigned)adv, rb1);
                    rz = AppendCStr(rf, rz, rb1, sizeof(rf));
                    rz = AppendCStr(rf, rz, " r=", sizeof(rf));
                    if (r == VC_FAIL)
                        rz = AppendCStr(rf, rz, "orig", sizeof(rf));
                    else
                    {
                        int rr = r - x;
                        if (rr < 0)
                        {
                            rz = AppendCh(rf, rz, '-', sizeof(rf));
                            rr = -rr;
                        }
                        UtoA((unsigned)rr, rb1);
                        rz = AppendCStr(rf, rz, rb1, sizeof(rf));
                    }
                    rz = AppendCStr(rf, rz, " ps=", sizeof(rf));
                    UtoA(g_TipSeq, rb1);
                    rz = AppendCStr(rf, rz, rb1, sizeof(rf));
                    LogLine(rf);
                }
            }
            if (wc >= 0x30 && wc <= 0x39 && g_VT.LogAdv)
                VecDigitLog(wc, px, yOff, r != VC_FAIL, x, y, cred);
            if (swForce)
            {
                /* 接管区 VecRender 失败也不回落原版 (否则原字冒出来):
                 * 只走笔位, 记一行诊断 (上限 8)。 */
                if (r == VC_FAIL && g_SWFailN < 8)
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
            if (r != VC_FAIL) return r + (inTip ? g_TipPush : 0) - (x - ox);
        }
    }
    return g_OrigBlit(t, NULL, wch, ox, y, color);
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
    g_VT.MenuTake = GetPrivateProfileIntA(VT_SECTION, "MenuTake", 1, g_IniPath);
    g_VT.DigitTrack = GetPrivateProfileIntA(VT_SECTION, "DigitTrack", 0, g_IniPath);
    g_VT.ToolBox = GetPrivateProfileIntA(VT_SECTION, "ToolBox", 1, g_IniPath);
    g_VT.TipBox = GetPrivateProfileIntA(VT_SECTION, "TipBox", 1, g_IniPath);
    g_VT.BriefBox = GetPrivateProfileIntA(VT_SECTION, "BriefBox", 1, g_IniPath);
    g_VT.BriefClip = GetPrivateProfileIntA(VT_SECTION, "BriefClip", 1, g_IniPath);
    g_VT.VShift = GetPrivateProfileIntA(VT_SECTION, "VShift", 0, g_IniPath);
    g_VT.LogAdv = GetPrivateProfileIntA(VT_SECTION, "LogAdv", 1, g_IniPath);
    g_VT.LogDetail = GetPrivateProfileIntA(VT_SECTION, "LogDetail", 0, g_IniPath);
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
    g_VT.MenuTake = g_VT.MenuTake ? 1 : 0;
    if (g_VT.DigitTrack < -8) g_VT.DigitTrack = -8;
    if (g_VT.DigitTrack > 8) g_VT.DigitTrack = 8;
    if (g_VT.ToolBox < 0) g_VT.ToolBox = 0;
    if (g_VT.ToolBox > 2) g_VT.ToolBox = 2;
    g_VT.TipBox = g_VT.TipBox ? 1 : 0;
    g_VT.BriefBox = g_VT.BriefBox ? 1 : 0;
    g_VT.BriefClip = g_VT.BriefClip ? 1 : 0;
    if (g_VT.VShift < -8) g_VT.VShift = -8;
    if (g_VT.VShift > 8) g_VT.VShift = 8;
    g_VT.LogAdv = g_VT.LogAdv ? 1 : 0;
    g_VT.LogDetail = g_VT.LogDetail ? 1 : 0;
    g_LogDetail = g_VT.LogDetail;   /* 同步给 GdiText 的 gdi 日志 */
    g_VT.Render = g_VT.Render ? 1 : 0;
    ResolveFontFace(g_VT.FontFace, "vtext", g_VTFace, L"Microsoft YaHei");
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
    p = AppendCStr(buf, p, " swpx=", sizeof(buf));
    UtoA((unsigned)g_VT.SuperPx, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " mt=", sizeof(buf));
    UtoA((unsigned)g_VT.MenuTake, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " dt=", sizeof(buf));
    {
        int dtr = g_VT.DigitTrack;
        if (dtr < 0) { p = AppendCh(buf, p, '-', sizeof(buf)); dtr = -dtr; }
        UtoA((unsigned)dtr, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    }
    p = AppendCStr(buf, p, " tb=", sizeof(buf));
    UtoA((unsigned)g_VT.ToolBox, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " bp=", sizeof(buf));
    UtoA(0u, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " v=78", sizeof(buf));   /* 发版自增, 日志对版本 */
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
    /* Simple_ 收集钩 (全文本必经, 命中跳过): 字节对不上就不装 */
    if (BytesSame((unsigned char*)ADDR_SIMPLE, kWantSimple, 7))
    {
        g_OrigSimple = (SimpleFn)BuildTrampAt(ADDR_SIMPLE, 7, ADDR_SIMPLE_BACK);
        if (g_OrigSimple && PatchAt(ADDR_SIMPLE, (void*)(size_t)VecSimpleEntry, 7))
            LogLine("vtext: simple hook ON");
        else { LogLine("vtext: simple hook OFF"); g_OrigSimple = NULL; }
    }
    else { LogLine("vtext: simple bytes mismatch"); LogBytes((unsigned char*)ADDR_SIMPLE); }
    /* GetDim 宽度接管钩 (引擎算字宽唯一入口, 一处替换处处生效) */
    if (BytesSame((unsigned char*)ADDR_GETDIM, kWantGetDim, 6))
    {
        g_OrigGetDim = (GetDimFn)BuildTrampAt(ADDR_GETDIM, 6, ADDR_GETDIM_BACK);
        if (g_OrigGetDim && PatchAt(ADDR_GETDIM, (void*)(size_t)VecGetDimEntry, 6))
            LogLine("vtext: getdim hook ON");
        else { LogLine("vtext: getdim hook OFF"); g_OrigGetDim = NULL; }
    }
    else { LogLine("vtext: getdim bytes mismatch"); LogBytes((unsigned char*)ADDR_GETDIM); }
}
