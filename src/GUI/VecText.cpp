/* VecText.cpp - 全局文本矢量接管 (超级武器列表/简报/按钮/顶栏数字等)。
 *
 * 接管点: BitFont::Blit (0x434120, 7B)。DrawText/度量/换行/对齐全部保留,
 * 版式零风险。game.fnt 格式不用管: 字宽来源是引擎自己的 GetTextDimension。
 *
 * 渲染 (Render=1): 跳过原版 1bpp 光栅, GDI 灰度覆盖率经 565/555 校准
 *   混入已 Lock 的 16 位面, advance 与原版逐字一致。
 * 任何异常 (度量失败/面信息非法/字形超大) 都走跳板调原版, 绝不花屏。
 *
 * 场景区分按路径, 不按坐标/内容 (坐标门曾误收右侧提示, 见笔记 v80 起):
 *   顶栏条带 (y<32 数字吃 CreditPx/DY); 超武收集只认侧栏发起者 0x004A6345。
 * 坐标归一 (VecNormC): 引擎笔位/裁剪只低 16 位有效; 空裁剪哨兵退主面边界。
 *
 * BitFont 对象实测布局 (Lock 0x4348F0 反汇编实证):
 *   +0x0C 锁定的 WORD 像素缓冲, +0x10 pitch(WORD 数),
 *   +0x24 默认色 WORD, +0x30..+0x3C 裁剪 LTRB (含端)。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MOExperience 同一 DLL)。
 */
#include <windows.h>
#include "GdiText.h"
#include "VecText.h"
#include "OrderUtil.h"
#include "Icon.h"

/* 后定义的前置声明 (GetDim 高度调整在后用 VecTipStep, 定义在 DrawText 入口前) */
static int VecPickPx(void* t);
/* VT_SECTION 见 VecText.h (MOExperience 读电力键共用)。 */
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
    int MenuTake;   /* 1=菜单按钮整体替换 (禁用原字形, 全墨水自定义绘制) */
    int DigitTrack; /* 数字字距微调像素 (负=收紧, 正=放松, 只影响 0-9 步进) */
    int TipBox;     /* 1=电力提示定制盒 (按列调映射补齐, 与引擎同 tone) */
    int BriefBox;   /* 1=简报正文盒加高 (界面适配字体), 0=保持原版盒子 */
    int BriefClip;  /* 1=简报裁剪底边跟盒子走 (面板子面切，需配合 BriefBox) */
    int Bold;
    int VShift;     /* 字形垂直偏移 (像素, 正=下移) */
    int LogDetail;  /* 1=记 gdi 逐字光栅日志; 0=只记关键 (默认) */
    int Render;
} VTConfig;

static int g_VtInit = 0;

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

/* 场景区分只认位置条带 (Blit 层 y<32 的数字), 不认函数:
 * 顶栏那几个函数也会画别的文本 (如按 House 遍历连超武列表一起画),
 * 函数粒度分不开, CreditPx 漏过去造成列表大小闪烁。 */

/* 简报/列表 DrawText 判定: 0x478xxx 或 0x621144 即简报绘制。 */
static int VecIsBriefCaller(unsigned caller)
{
    if (caller >= 0x00479000u && caller < 0x00479100u) return 1;
    if (caller == 0x00621144u) return 1;
    return 0;
}

/* 坐标归一 (v83): 引擎笔位/裁剪只有低 16 位有效, 高 16 位偶带脏数据
 * (右侧提示实测 Blit x=0xFF87066D/y=0xFF8C02F4, 低 16 位 1645/756 才是
 * 真坐标; 引擎自有光栅按 16 位取故不受影响, 我们按 32 位取则全灭)。
 * 符号扩展保留真负坐标 (左沿出屏字 x∈[-32768,0) 不受影响)。 */
static int VecNormC(int v)
{
    return (int)(short)(v & 0xFFFF);
}

/* DrawText 字形计数 / 行跨度 (探针已删, 只留正文盒判据) */
static int g_DTDepth = 0;
static int g_DTBrief = 0;
/* 简报正文盒内 (引擎按自有宽度换行): 步进保持原笔位, 不加宽 */
static int g_DTBody = 0;

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

/* ---------------- Blit 层超武区接管已删除 ----------------
 * SuperPx 经游戏内验证无可见作用 (置 0 对比无变化): 列表行经 Print->Simple_
 * 已被 SWPanel 整块收集+隐藏, DrawText 系叠字未观测到漏网。整机移除
 * (VecSWRegion/深度计数/swForce/失败诊断), 右侧区文字走通用 Render 路径。 */
static int VecPickPx(void* t);
static int VecRender(void* t, wchar_t wch, int adv, int h, int x, int y,
                     int colorW, int px, int yOff);
/* 原版格高 (idata Lines): FontPx 等比缩放字宽时的分母 (定义提前,
 * 接管渲染要用; 后面不再重复定义) */
static int g_FontOrigH = 19;

/* ---------------- 超武列表整体接管 ----------------
 * 收集点在 Simple_Text_Print_Wide 入口 (Print 唯一调用者, 反汇编实证):
 * 背景黑条是 Simple_ 内部按标志 (ah&4, 名行 flags=0x4618) 铺的不透明底,
 * 与字形同一调用, 入口跳过即连底+字一起消失, 不管内容细节不逐条绘制。
 * 命中 (主画布 + 落列表带 + 名/计时器形状) 即收表 (串,坐标,原前景色)
 * 并跳过; 空串只收表不跳过 (引擎底色行为与原版一致, 闪烁靠 150ms 过期)。
 * 门不全走原版; Print 入口只保留冒号计时器后备收集
 * (Simple_ 钩漏网时才跑到这, fore 不见用计时器白)。
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

/* ---------------- 悬浮提示背景盒自适应已删除 ----------------
 * ToolBox=0 关闭定案 (三条原因见 notes/VecText顶栏与简报.md), 1/2 已退役,
 * 代码整机移除 (收串/取样拓盒/自有笔位)。悬浮提示走通用 Render 路径,
 * 电力提示出界修复由 TipBox 接管。 */


/* PUA 串行步进: max(原步进, 矢量字号, 串内最大图标高), 只增不减。
 * 与 GetDim 高度调整同公式, 两处一致行盒才对上。
 * 背景: 矢量墨水/图标高于原字格时, DrawText 换行步进 (BitFont+0x1C,
 * 0x434EC2/0x435112 反汇编实证) 会挤行, 行高必须跟内容走。 */
static int VecTipStep(void* t, const wchar_t* s)
{
    int s0, px, mh = 0, q;
    if (!t || !IsPlausiblePtr(t) || !s) return -1;
    s0 = *(int*)((char*)t + 0x1C);
    if (s0 < 8 || s0 > 64) return -1;
    px = VecPickPx(t);
    if (px < 8 || px > 40) px = s0;
    for (q = 0; q < 256; q++)
    {
        wchar_t c = s[q];
        int hh;
        if (!c) break;
        if (!Icon_Is((unsigned)c & 0xFFFFu)) continue;
        hh = Icon_Height((unsigned)c & 0xFFFFu);
        if (hh > mh) mh = hh;
    }
    {
        int st = s0;
        if (px > st) st = px;
        if (mh > st) st = mh;
        if (st > 64) st = 64;
        return st;
    }
}

int __fastcall VecDrawTextEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5,
    int a6, int a7, int a8, int a9, int a10)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int brief, r, menuTake = 0;
    brief = VecIsBriefCaller(caller);
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
            g_DTBrief = 1;
            g_DTBody = (caller == 0x00621144u && a6 >= 400 && a7 >= 100);
        }
    }
    /* 注: 曾试 DrawText 入口临时抬高 BitFont+0x1C 改行步进, 实证此处的
     * t+0x1C 恒为 0 (不是行步进源, 真源在 DrawText 栈帧深处), 此路不通,
     * 相关代码已删。行高以 GetDim 实测为准 (3 行 51px=17px/行, 图标 16px
     * 装得下, 矢量墨水约 18px 仅 1px 可忽略)。 */
    r = g_OrigDrawText(t, NULL, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
    if (menuTake) g_MenuDepth--;
    if (brief)
    {
        g_DTDepth--;
        if (g_DTDepth == 0)
        {
            g_DTBrief = 0;
            g_DTBody = 0;
        }
    }
    return r;
}

/* 上一帧电力提示 Print 落笔 x (Simple 入口的 x 自校验用) */
static int g_LastTipX = -1;
/* 当帧提示串宽 (Print 入口由表查得, Blit 首字画盒用, 0=未知跳过) */
static int g_TipTot = 0;
static int g_TipOw = 0;
/* 当帧提示落笔 (Print 入口记, 首字位置门用, 与颜色无关) */
static int g_TipY = -9999;
/* 当帧提示首字是否已扫描 (Print 入口清零, Blit 首字置位) */
static int g_BlitTipSeen = 0;
/* 提示边框色粘存 (玩家色一局不变): 顶行采样成功即更新;
 * 出界裁剪后顶行全黑采不到时沿用, 不再回落纯红 (图 3 的病)。 */
static unsigned g_TipBcCache = 0;
static int g_TipBcOK = 0;
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

int __fastcall VecPrintEntry(void* t, void* e,
    int a1, int a2, int a3, int a4, int a5, int a6, int a7)
{
    unsigned caller = (unsigned)__builtin_return_address(0);
    int r;
    /* 电力提示落笔 x 记录 (Simple 盒 R 自校验用, 只读 First 字)。
     * 只认世界悬浮 Print (caller=0x4A5FF2 内层, 见定案): 菜单/列表
     * (如选择任务存档名"电力…") 走别的调用者, 在此直接排除,
     * 否则 Blit 层 TipBox 会在菜单里画额外方框 (图 4 的病)。 */
    {
        const wchar_t* ps = (const wchar_t*)a3;
        if (caller == 0x004A5FF2u && ps && IsPlausiblePtr((void*)ps) &&
            (((unsigned)ps[0] & 0xFFFFu) == 0x7535u))
        {
            int tl = 0, ci;
            g_LastTipX = a4;
            g_TipY = a5;
            g_BlitTipSeen = 0;
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
            caller == 0x004A5FF2u &&
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
                                g_TipTot = t2; g_TipOw = o2;
                                g_LastTipX = a4; g_TipY = a5;
                                g_BlitTipSeen = 0;
                            }
                        }
                        break;
                    }
                    if (g_TipTot != 0) break;
                }
            }
        }
    }
    /* 超武列表整体隐藏+收集: 只收冒号计时器 ("00:00"), 命中即收表
     * 并跳过原调用; 门不全 (面板关/异面) 走原版。
     * CJK 名串不在此收: Print 层名与悬浮提示不可区分 (同经 0x4A5FF2、
     * 同落右侧带、同 CJK 首字), 收则把靠右的建筑名提示整个吞掉
     * (Simple_ 的底已画 -> 黑盒留、字灭, 图 1 的病)。名收集只在
     * Simple_ 做 (主门, 那边认合成结构, 提示不走故不误收)。 */
    if (a4 >= 1300 && a5 >= 32)
    {
        const wchar_t* s = (const wchar_t*)a3;
        if (VecSWColon(s) && !VecSWHasEq(s) &&
            VecSWCapture((void*)a2, 1, s, a4, a5))
            return 1;
    }
    if (!g_OrigPrint) return 0;
    r = g_OrigPrint(t, NULL, a1, a2, a3, a4, a5, a6, a7);
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
     * (盒右界=a6+2, 反汇编双盒路径同源) 取自上游旧字宽; GetDim 接管已把
     * 宽字宽替换, 盒由引擎自画 (半透明+红边框原样, 一像素不碰)。 */
    /* 超武名单行只出自侧栏绘制者 0x004A6345 (vswcaller 取证:
     * 名/计时器行 c=0x004A6345, fore=0x07FF/0xFFFF, 名 x=1722 计时器 x=1749;
     * 世界悬浮提示出自扩展模块 (c=0x73AC616A, 如红字 fore=0xA92A)。
     * 同带同形但发起者不同, 在此按发起者一刀切: 非侧栏一律放行,
     * 提示串再也进不了收集表。 */
    if (g_SWPanelOn && g_SWCompW &&
        (unsigned)__builtin_return_address(0) == 0x004A6345u &&
        IsPlausiblePtr((void*)a1))
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
    return g_OrigSimple(ecxIn, edxIn, a1, a2, a3, a4, a5, a6, a7);
}

/* GetTextDimension 宽度接管: 引擎所有算字宽处 (盒子/居中/步进/换行)
 * 全走这里, 一处替换处处生效。只动单行适配宽度, 高度与换行逻辑原样:
 * 先走原版取 h (与原版同口径), w 用 GDI 自然步进和
 * (与绘制笔位同口径: ABC 全宽 + 数字 DigitTrack)。
 * 控制串 (\t \n 等)、超长 (>255 字)、触 maxW (换行逻辑) 一律走原版。
 * 非 PUA 多行串另有 grow-only 修正 (原版换行丢行宽, 超武提示窄盒溢出),
 * 只增不减, 见下。重入守卫: 嵌套调用直接走原版 (防栈爆)。
 * 未开 Render 不接管。 */
static int g_GDDepth = 0;

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
    /* 先扫 PUA (256 上限): 无图标串走老路 (控制串一律原版, 零回归)。
     * 含图标串必须自己按行度量: 原版点阵不认 PUA, 盒宽会按无图标算
     * 导致图标溢出盒外 (建造提示三行出界即此因)。 */
    {
        int hasIcon = 0, hasNL = 0, hasCtl = 0, j;
        for (j = 0; j < 256; j++)
        {
            wchar_t c = s[j];
            if (!c) break;
            if (Icon_Is((unsigned)c & 0xFFFFu)) { hasIcon = 1; break; }
            if (c == 0x0A || c == 0x0D) hasNL = 1;
            else if (c < 0x20) hasCtl = 1;
        }
        if (hasIcon)
        {
            int lineW = 0;
            tot = 0;
            for (i = 0; i < 256; i++)
            {
                wchar_t wc = s[i];
                int ga;
                if (!wc) break;
                if (wc == 0x0A)   /* 行尾: 取最大行宽 */
                {
                    if (lineW > tot) tot = lineW;
                    lineW = 0;
                    continue;
                }
                if (wc < 0x20) goto fwd;
                if (Icon_Is((unsigned)wc & 0xFFFFu))
                {
                    ga = Icon_Advance((unsigned)wc & 0xFFFFu);
                    if (ga <= 0) goto fwd;
                }
                else
                {
                    ABC abc;
                    if (!g_G || !g_G->dc ||
                        !GetCharABCWidthsW(g_G->dc, wc, wc, &abc)) goto fwd;
                    ga = abc.abcA + abc.abcB + abc.abcC;
                    if (wc >= 0x30 && wc <= 0x39) ga += g_VT.DigitTrack;
                }
                if (ga < 0) ga = 0;
                lineW += ga;
                if (lineW >= maxW) goto fwd;   /* 单行触顶 (换行逻辑): 原样 */
            }
            if (i >= 256) goto fwd;   /* 超长未见尾: 原样 */
            if (lineW > tot) tot = lineW;
            /* 精确行宽: 短串不保底 (160 保底是误诊产物, 图3大空隙即此因;
             * 当时的"短文本越界"实为超武串走原版度量, 与此分支无关)。 */
            /* 高度跟步进走: oh 是原步进总高, 按实际步进补每行差值 (只增不减)。
             * 步进公式与 DrawText 钩一致 (VecTipStep), 否则盒与行错位:
             * 度量在绘制前跑, +0x1C 此时还是原值, 正好取原步进。 */
            {
                int nlines = 1, k, v0 = -1, v1;
                for (k = 0; k < i; k++) if (s[k] == 0x0A) nlines++;
                if (t && IsPlausiblePtr(t))
                {
                    int v = *(int*)((char*)t + 0x1C);
                    if (v >= 8 && v <= 64) v0 = v;
                }
                v1 = VecTipStep(t, s);
                if (v0 > 0 && v1 > v0 && oh > 0 && oh < 4096)
                {
                    int nh = oh - nlines * v0 + nlines * v1;
                    if (nh > oh && nh < 4096) oh = nh;
                }
            }
            *w = tot; *h = oh;
            {
                DimCE* e = &g_DimC[g_DimCPos % DIMCACHE_N];
                g_DimCPos++;
                e->ptr = s; e->w0 = (unsigned)s[0] & 0xFFFFu;
                e->n = i; e->ow = ow; e->nw = tot;
            }
            g_GDDepth--;
            return r;
        }
        /* 非 PUA 多行串 grow-only 修正 (超武提示窄盒溢出修复):
         * 原版度量 (0x433CF0, 反汇编实证) 换行分支直接清零行宽不存最大,
         * maxW>0 (提示布局传视图宽, 0x478C36/0x478C83) 时盒宽常只按某行算
         * ("打击的击"/"回溯的800"溢出即此因, 与 PUA 分支是两套路径)。
         * 这里按行取 GDI 真最大, 只增不减: 真>原+6 且不超 maxW 才接管
         * (单行/他种控制符/需换行一律原样, 零回归); 高度不动 (行数不变)。 */
        else if (hasNL && !hasCtl)
        {
            int mx = 0, lw = 0, m, gdBad = 0;
            for (m = 0; m < 256; m++)
            {
                wchar_t wc = s[m];
                int ga;
                ABC abc;
                if (!wc) break;
                if (wc == 0x0A || wc == 0x0D)
                {
                    if (lw > mx) mx = lw;
                    lw = 0;
                    continue;
                }
                if (!g_G || !g_G->dc ||
                    !GetCharABCWidthsW(g_G->dc, wc, wc, &abc)) { gdBad = 1; break; }
                ga = abc.abcA + abc.abcB + abc.abcC;
                if (wc >= 0x30 && wc <= 0x39) ga += g_VT.DigitTrack;
                if (ga < 0) ga = 0;
                lw += ga;
                if (lw >= maxW) { gdBad = 1; break; }   /* 触顶: 换行意图, 原样 */
            }
            if (!gdBad && m < 256)
            {
                if (lw > mx) mx = lw;
                if (mx > ow + 6 && mx <= maxW)
                {
                    *w = mx; *h = oh;
                    {
                        DimCE* e = &g_DimC[g_DimCPos % DIMCACHE_N];
                        g_DimCPos++;
                        e->ptr = s; e->w0 = (unsigned)s[0] & 0xFFFFu;
                        e->n = m; e->ow = ow; e->nw = mx;
                    }
                    g_GDDepth--;
                    return r;
                }
            }
        }
    }
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
    {
        DimCE* e = &g_DimC[g_DimCPos % DIMCACHE_N];
        g_DimCPos++;
        e->ptr = s; e->w0 = (unsigned)s[0] & 0xFFFFu;
        e->n = i; e->ow = ow; e->nw = tot;
    }
    g_GDDepth--;
    return r;
fwd:
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
    Tp = VecNormC(*(int*)((char*)t + 0x34));
    B = VecNormC(*(int*)((char*)t + 0x3C));
    if (B < Tp)
    {
        /* 空裁剪哨兵 (0,0,-1,-1: 未设置, 非剔除): 引擎照画, 故退到主面
         * 边界 (pp 对上合成宽才认, 否则保持失败走原版, 防小面越界)。
         * 右侧提示经 DrawText(0x479041 小矩形) 进来即此形状 (v84 取证:
         * c=0,0,-1,-1 pp=1752)。 */
        int cR = VecNormC(*(int*)((char*)t + 0x38));
        if (cR >= 0 && B >= 0) { g_VecWhy = 5; return VC_FAIL; }
        if (pitchW != g_SWCompW || g_SWCompW <= 0 || g_SWCompH <= 0)
        { g_VecWhy = 5; return VC_FAIL; }
        Tp = 0; B = g_SWCompH - 1;
    }
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
    Tp = VecNormC(*(int*)((char*)t + 0x34));
    B = VecNormC(*(int*)((char*)t + 0x3C));
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
 * 整体接管改为: Print 入口隐藏+收集, 消息帧统一整块重绘 (见 MOExperience 超武面板)。
 * 逐行背景/自锁/虚表 (旧 VecSWRowDraw/VecSWBandFill/VecSWVt/VecSWDoLog) 不再需要。 */

int __fastcall VecText_BlitHook(void* t, void* edxUnused, int wch, int x, int y, int color)
{
    wchar_t wc = (wchar_t)((unsigned)wch & 0xFFFFu);
    int eff, adv, h = 0, r, px, yOff;
    (void)edxUnused;
    if (!g_OrigBlit) return 0;   /* 不该发生, 守一下不断原版链 */
    if (g_DTBrief && t && IsPlausiblePtr(t))
    {
        /* 简报裁剪底边跟盒子走: DrawText 的 H 参数改了, 但真正切行的是
         * Lock 进 BitFont+0x30 的裁剪 (面板子面 745 底)。盒子加高后
         * (底 783) 必须把裁剪底同步放开, 行才画得出来。
         * 安全阀: 只放底边, 最多 +64; pitch < 1024 说明不是整屏面,
         * 不碰 (防小面越界)。 */
        int* LTRB = (int*)((char*)t + 0x30);
        int pitchW = *(int*)((char*)t + 0x10);
        if (g_VT.BriefClip && pitchW >= 1024 && LTRB[3] < 783 &&
            LTRB[3] >= 700)
            LTRB[3] = 783;
    }
    /* TAB 走原版特殊公式, 缺字/度量失败也走原版 (保留 0x5555 兜底行为) */
    if (!t || !IsPlausiblePtr(t) || wc == 9)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    /* 坐标归一 (脏高位清掉, 见 VecNormC): 以下 inTip 跨度
     * 与 VecRender 全用真坐标。 */
    x = VecNormC(x);
    y = VecNormC(y);
    eff = (color == -1) ? (int)(*(unsigned short*)((char*)t + 0x24)) : color;
    /* PUA 自定义图标 (建造提示第三行): PNG 位图直画, 不走 GDI/原版点阵。
     * 放在 VecMeasure 之前 (原版度量不认 PUA, 必失败回落出 tofu)。 */
    if (Icon_Is((unsigned)wc & 0xFFFFu))
    {
        int ia = Icon_Advance((unsigned)wc & 0xFFFFu);
        if (ia > 0)
        {
            int rr = Icon_Blit(t, (unsigned)wc & 0xFFFFu, x, y, eff,
                               Icon_LineH(t));
            if (rr >= 0) return x + ia;
        }
        return g_OrigBlit(t, NULL, wch, x, y, color);
    }
    adv = VecMeasure(t, wc, &h);
    if (adv < 0)
        return g_OrigBlit(t, NULL, wch, x, y, color);
    int tx = x;
    int inTip = (g_LastTipX >= 0 && g_TipTot > 0 &&
                 tx >= g_LastTipX - g_TipTot - 16 &&
                 tx <= g_LastTipX + g_TipTot + 8 &&
                 y >= g_TipY - 8 && y <= g_TipY + 8);
    {
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
                int yT = y - 2, yB = y + 18;
                int xx, yy;
                /* 边缘钳制: 只画屏内部分。oldL/oldR 随 ow 自适应
                 * (118->±63 实测; 长串按 ow+8 估, 截图校准)。 */
                int L0 = newL < 0 ? 0 : newL;
                int R0 = newR >= pp ? pp - 1 : newR;
                int cL = oldL < L0 ? L0 : oldL;
                int cR = oldR > R0 ? R0 : oldR;
                /* 真裁剪 (无模式切换): 盒与字永远同宽同锚
                 * ([anchor-4, tipx+tot+4]), 作为一个整体进出屏幕;
                 * 出屏侧不画边框。
                 * v67 序号证 Print:Simple_=1:1, 无额外绘制;
                 * 此前"突然缩短"是出界回落在 tipx≈4 处跳变, 已删。 */
                if (L0 <= R0 && yT >= 0 && yB <= 4096)
                {
                    unsigned bc = 0xF800u;
                    unsigned bestR = 0;
                    /* 边框色沿用顶行采样 (引擎窄盒顶边=玩家色);
                     * 出界后顶行剩下全黑、采不到 (bestR==0) 就用粘存。 */
                    for (xx = cL; xx <= cR; xx++)
                    {
                        unsigned c = bb[yT * pp + xx];
                        unsigned R = (c >> 11) & 31u;
                        if (R > bestR)
                        {
                            bestR = R; bc = c;
                        }
                    }
                    if (bestR > 0)
                    {
                        g_TipBcCache = bc; g_TipBcOK = 1;
                    }
                    else if (g_TipBcOK) bc = g_TipBcCache;
                    /* 底一律填不透黑 (图 1 定案): 出界后映射填充会漏出
                     * 引擎半透明底 (图 2 的病); 实色填充与边框同范围,
                     * 字随后画上, 不盖字。 */
                    for (yy = yT + 1; yy <= yB - 1; yy++)
                    {
                        for (xx = L0; xx <= R0; xx++)
                            bb[yy * pp + xx] = 0;
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
                }
            }
        }
    }
    int ox = x;
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
    if (g_VT.Render)
    {
        px = VecPickPx(t);
        if (px >= 0)
        {
            /* 顶栏条带 (y<32) 的数字: 唯一的顶栏判据, 与发起者无关。
             * 同一条顶栏至少 5 个发起者反复画, 按位置一刀切,
             * 大小偏移永远一致, 闪烁自止。只在用户配了 CreditPx/DY 时生效,
             * 默认 0/0 全不碰。超武列表等不在条带内, 不受影响。 */
            int cred = (wc >= 0x30 && wc <= 0x39 && y >= -8 && y < 32 &&
                (g_VT.CreditPx > 0 || g_VT.CreditDY != 0));
            if (cred && g_VT.CreditPx > 0)
            {
                px = g_VT.CreditPx;
                if (px < 8) px = 8;
                if (px > 40) px = 40;
            }
            /* 字号 (全局): 墨水与字宽等比缩放, 版式才对。
             * 条带数字除外 (只换墨水字宽不动, 不碰已调好的顶栏)。
             * 行距/换行仍是引擎的, 字号偏离原版太多可能溢出盒子蹭行。 */
            if (!cred && g_VT.FontPx > 0 && g_FontOrigH >= 8 &&
                g_FontOrigH <= 40)
            {
                adv = (adv * g_VT.FontPx + g_FontOrigH / 2) / g_FontOrigH;
                if (adv < 1) adv = 1;
            }
            yOff = g_VT.VShift + (cred ? g_VT.CreditDY : 0);
            r = VecRender(t, wc, adv, h, (inTip ? tx : x), y, eff,
                          px, yOff);
            /* VecRender 失败时记一行 (字/坐标/失败点 why), 上限 40。 */
            if (r == VC_FAIL)
            {
                static int nF = 0;
                if (nF < 40)
                {
                    char fb1[16], ff[112];
                    unsigned fz = 0;
                    nF++;
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
                    LogLine(ff);
                }
            }
            if (r != VC_FAIL) return r - (x - ox);
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

/* 顶栏资金字号 (CreditPx, >0=用户自定义绝对像素): 底部电量对标用。 */
int VecText_CreditPx(void)
{
    return g_VT.CreditPx;
}
/* 资金解析后字体+粗体 ([VecText] FontFace/Bold): 电力同字用。 */
const wchar_t* VecText_FontFace(void)
{
    return g_VTFace;
}

int VecText_FontPx(void)
{
	return g_VT.FontPx;
}
int VecText_Bold(void)
{
	return g_VT.Bold;
}

void VecText_EnsureInit(void)
{
    char b1[16], buf[192];
    unsigned p;
    if (g_VtInit) return;
    g_VtInit = 1;
    GtEnsureLog();
    if (!g_Log) return;   /* 日志都打不开就当没配置 (后续安装会自然跳过) */
    g_VT.Enable = GetPrivateProfileIntA(VT_SECTION, "Enable", 1, g_IniPath);
    OU_ReadString(VT_SECTION, "FontFace", "simhei.ttf",
                  g_VT.FontFace, sizeof(g_VT.FontFace));
    g_VT.Bold = GetPrivateProfileIntA(VT_SECTION, "Bold", 0, g_IniPath);
    g_VT.FontPx = GetPrivateProfileIntA(VT_SECTION, "FontPx", 0, g_IniPath);
    g_VT.CreditPx = GetPrivateProfileIntA(VT_SECTION, "CreditPx", 0, g_IniPath);
    g_VT.CreditDY = GetPrivateProfileIntA(VT_SECTION, "CreditDY", 0, g_IniPath);
    g_VT.MenuTake = GetPrivateProfileIntA(VT_SECTION, "MenuTake", 1, g_IniPath);
    g_VT.DigitTrack = GetPrivateProfileIntA(VT_SECTION, "DigitTrack", 0, g_IniPath);
    g_VT.TipBox = GetPrivateProfileIntA(VT_SECTION, "TipBox", 1, g_IniPath);
    g_VT.BriefBox = GetPrivateProfileIntA(VT_SECTION, "BriefBox", 1, g_IniPath);
    g_VT.BriefClip = GetPrivateProfileIntA(VT_SECTION, "BriefClip", 1, g_IniPath);
    g_VT.VShift = GetPrivateProfileIntA(VT_SECTION, "VShift", 0, g_IniPath);
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
    g_VT.MenuTake = g_VT.MenuTake ? 1 : 0;
    if (g_VT.DigitTrack < -8) g_VT.DigitTrack = -8;
    if (g_VT.DigitTrack > 8) g_VT.DigitTrack = 8;
    g_VT.TipBox = g_VT.TipBox ? 1 : 0;
    g_VT.BriefBox = g_VT.BriefBox ? 1 : 0;
    g_VT.BriefClip = g_VT.BriefClip ? 1 : 0;
    if (g_VT.VShift < -8) g_VT.VShift = -8;
    if (g_VT.VShift > 8) g_VT.VShift = 8;
    g_VT.LogDetail = g_VT.LogDetail ? 1 : 0;
    g_LogDetail = g_VT.LogDetail;   /* 同步给 GdiText 的 gdi 日志 */
    g_VT.Render = g_VT.Render ? 1 : 0;
    ResolveFontFace(g_VT.FontFace, "vtext", g_VTFace, L"Microsoft YaHei");
    p = 0;
    p = AppendCStr(buf, p, "vtext: start en=", sizeof(buf));
    UtoA((unsigned)g_VT.Enable, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " dtail=", sizeof(buf));
    UtoA((unsigned)g_VT.LogDetail, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
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
    p = AppendCStr(buf, p, " mt=", sizeof(buf));
    UtoA((unsigned)g_VT.MenuTake, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " dt=", sizeof(buf));
    {
        int dtr = g_VT.DigitTrack;
        if (dtr < 0) { p = AppendCh(buf, p, '-', sizeof(buf)); dtr = -dtr; }
        UtoA((unsigned)dtr, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    }
    p = AppendCStr(buf, p, " bp=", sizeof(buf));
    UtoA(0u, b1); p = AppendCStr(buf, p, b1, sizeof(buf));
    p = AppendCStr(buf, p, " v=99", sizeof(buf));   /* 发版自增, 日志对版本 */
    LogLine(buf);
}

void VecText_Install(void)
{
    char b1[16], buf[192];
    unsigned p;
    (void)b1; (void)buf; (void)p;
    VecText_EnsureInit();
    if (!g_Log) return;   /* 日志都打不开就不装钩 (无意义) */
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
    /* 帧准备钩 (0x4F42F0, 首5B=A1 24 73 88 00): 每循环必跑, 底电"始终绘制"。
     * 安装见 MOExperience (MOExperience_PrepareEntry/g_PrepareTramp)。 */
    {
        static const unsigned char kWantPrepare[5] = { 0xA1, 0x24, 0x73, 0x88, 0x00 };
        if (BytesSame((unsigned char*)0x4F42F0u, kWantPrepare, 5))
        {
            g_PrepareTramp = BuildTrampAt(0x4F42F0u, 5, 0x4F42F5u);
            if (g_PrepareTramp && PatchAt(0x4F42F0u, (void*)MOExperience_PrepareEntry, 5))
                LogLine("vtext: prepare hook ON");
            else { LogLine("vtext: prepare hook OFF"); g_PrepareTramp = NULL; }
        }
        else { LogLine("vtext: prepare bytes mismatch"); LogBytes((unsigned char*)0x4F42F0u); }
    }
    /* 上屏入口钩 (0x6A70E0, 首6B=83 EC 38 53 55 56): 每次呈现前先恢复。
     * 安装见 MOExperience (MOExperience_BlitSbEntry/g_BlitSbTramp)。 */
    {
        static const unsigned char kWantBlitSb[6] = { 0x83, 0xEC, 0x38, 0x53, 0x55, 0x56 };
        if (BytesSame((unsigned char*)0x6A70E0u, kWantBlitSb, 6))
        {
            g_BlitSbTramp = BuildTrampAt(0x6A70E0u, 6, 0x6A70E6u);
            if (g_BlitSbTramp && PatchAt(0x6A70E0u, (void*)MOExperience_BlitSbEntry, 6))
                LogLine("vtext: blitsb hook ON");
            else { LogLine("vtext: blitsb hook OFF"); g_BlitSbTramp = NULL; }
        }
        else { LogLine("vtext: blitsb bytes mismatch"); LogBytes((unsigned char*)0x6A70E0u); }
    }
    /* 呈现入口钩 (0x4F4780, 首7B=83 EC 5C 8B 44 24 60): 非条带呈现路。
     * 安装见 MOExperience (MOExperience_UpdateEntry/g_UpdateTramp)。 */
    {
        static const unsigned char kWantUpdate[7] = { 0x83, 0xEC, 0x5C, 0x8B, 0x44, 0x24, 0x60 };
        if (BytesSame((unsigned char*)0x4F4780u, kWantUpdate, 7))
        {
            g_UpdateTramp = BuildTrampAt(0x4F4780u, 7, 0x4F4787u);
            if (g_UpdateTramp && PatchAt(0x4F4780u, (void*)MOExperience_UpdateEntry, 7))
                LogLine("vtext: update hook ON");
            else { LogLine("vtext: update hook OFF"); g_UpdateTramp = NULL; }
        }
        else { LogLine("vtext: update bytes mismatch"); LogBytes((unsigned char*)0x4F4780u); }
    }
}
