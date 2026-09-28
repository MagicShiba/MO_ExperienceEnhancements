/* ChatEdit.cpp - 自定义聊天输入框 (原生 MessageListClass::Input 显示差、
 * 无光标移动/选中/粘贴)。
 *
 * 做法 (战役框同管线: 自绘 + GDI, 只读查询 + 渲染面绘制, 联机安全):
 *   输入: 运行时补丁 MessageListClass::Input (0x5D4510, 5B 无 rel,
 *     跳回 0x5D4515), 同签名 __fastcall 替换。字符/退格/回车/Esc/Ctrl 组合
 *     自己处理后消费 (*keySlot=0, return 0), 其余走跳板跑原生。
 *     导航键 (箭头/Home/End/Delete) 到不了 Input (对话框上游吃掉, 反汇编实证):
 *     低级键盘钩消息级直接移动 (Left/Right) + 每帧轮询后备 (LL/轮询 150ms 握手),
 *     组字中方向键放行给 IME。
 *   文本全程镜像进原生 EditBuffer (0xA8BC60+0x28, 光标 +0x2B0, 始终有效):
 *     提交/取消直接调原生 Input(Enter/Esc), 发包字节与手打完全一致
 *     (单机+联机同路, 不碰任何模拟/网络函数)。
 *   显示: MessageDrawHook 尾部调 ChatEdit_Frame (状态/轮询/组字),
 *     绘制在 DrawTail (0x5D4A8D, 原生编辑块之后) 做, 不透明黑底盖住原生行。
 *     GDI 槽 4 (字体与战役框同源: [MOExperience] FontFace/Bold 同解析)。
 *   粘贴: CF_UNICODETEXT; 复制/剪切写剪贴板 (全本地 Win32)。
 *   组字: 每帧 Imm 轮询组字串画出来+下划线 (全屏下系统组字窗不可见),
 *     组字中暂停导航/编辑轮询, 回车等转原生。
 *   防滚图: 低级键盘钩吞 Left/Right 消息并直接移动光标 (开框装、关框卸,
 *     严格校验前台窗口; 组字中放行给 IME)。
 * 开关 [MOExperience] ChatEdit=1 (0=全走原生)。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, -limm32)。
 */

#include <windows.h>
#include <imm.h>
#include "GdiText.h"
#include "ChatEdit.h"

#define CE_MSGLIST      0xA8BC60u
#define CE_OFF_ISEDIT   0x19u
#define CE_OFF_EDITBUF  0x28u
#define CE_OFF_EDITLABEL 0x24u
#define CE_OFF_EDITPOS  0x1Cu
#define CE_OFF_MAXCHARS 0x10u
#define CE_OFF_CURPOS   0x2B0u
#define CE_OFF_INITPOS  0x2B4u
#define CE_OFF_FIELDW   0x2C4u
#define CE_BUF_N        162

#define CE_INPUT        0x005D4510u
#define CE_INPUT_BACK   0x005D4515u

/* 原生行缓冲协议 (0x54F1C0/0x54F200 反汇编): 对象=*(0x87F770),
 * 环首+0x114 (256 槽), 环尾+0x314, 游标(写头)+0x318。
 * WM_CHAR 分支 (0x54F85C) 先 (A) 0x54F200(0x0A,0) 写标志字 0x000A,
 * 再 (B) 0x54F1C0(ch) 写原文 (SBCS, <0x100), 两格成对。摘除钩用。 */
#define CE_LINEOBJ_ADDR  0x0087F770u
#define CE_LINEBUF_OFF   0x114u
#define CE_LINECUR_OFF   0x318u
#define CE_RING_TAIL     (CE_LINECUR_OFF - 4u)   /* 0x314: 环尾 (消费位) */
#define CE_RING_N        0x100u                  /* 环长 (0x114..0x313) */
#define CE_CHAR_FLAG     0x000Au                 /* WM_CHAR 的 (A) 标志字 */

#define CE_COMPOSITE    0x0088731Cu
#define CE_VIEWBOUNDS   0x00886FA0u
#define CE_BITFONT      0x0089C4D0u
#define CE_OFF_FONT_H   0x1Cu

#define CE_GDI_SLOT     4
#define CE_VT_LOCK      0x5Cu
#define CE_VT_UNLOCK    0x60u
#define CE_VT_FILLRECT  0x1Cu
#define CE_VT_GET_PITCH 0x74u
#define CE_VT_GET_WIDTH 0x7Cu
#define CE_VT_GET_HEIGHT 0x80u

#define CE_SECTION      "MOExperience"

/* 字体跟战役框 (同 [MOExperience] FontFace/Bold, 经 ResolveFontFace 解析;
 * 直接把 "msyh.ttc" 当字体名传 GDI 会回退到无 CJK 字形, 中文空白) */
static wchar_t ce_Face[FONT_FACE_MAX];
static int ce_Bold = 0;

/* ---------------- 状态 ---------------- */

static int ce_Init = 0;
static int ce_Enable = 1;
static int ce_Active = 0;

static wchar_t ce_Buf[CE_BUF_N];
static int ce_Len = 0;
static int ce_Cur = 0;
static int ce_Anchor = -1;      /* -1=无选中, 否则选中 [min(Anchor,Cur), max) */
/* 开框时认领的预填模板 (AddEdit 写入的"从Name："等): 原样未动则发送时剥离,
 * 用户只发自己打的字; 动过则尊重原文发送。 */
static wchar_t ce_Pre[CE_BUF_N];
static int ce_PreLen = 0;
static int ce_ViewX = 0;        /* 横向滚动 (像素) */
static unsigned ce_BlinkT = 0;

static wchar_t ce_Comp[CE_BUF_N];
static int ce_CompLen = 0;
static int ce_CompCur = 0;

static unsigned ce_CmdTick = 0; /* Ctrl 组合键上次处理时刻 (Input/轮询握手) */
static unsigned ce_NavTick = 0; /* 方向键上次处理时刻 (LL/轮询/钩握手) */
static unsigned ce_LLCnt = 0;   /* LL 事件计数 (轮询重锚用) */
/* WM_CHAR 摘除钩 (定义在后, 开框时懒安装): 行缓冲游标快照 + 安装前置声明 */
static unsigned ce_LastCur = 0xFFFFFFFFu;
static void CE_CharPostInstall(void);
static unsigned ce_LLSeen = 0;  /* 轮询已消化的 LL 计数 */
static unsigned ce_LLLastTick = 0; /* LL 末事件时刻 (边缘退让判据) */
static HWND ce_FgWnd = NULL;

typedef struct { int vk; int prev; unsigned nextRep; } CENavKey;
static CENavKey ce_NavKeys[] = {
    { VK_LEFT, 0, 0 }, { VK_RIGHT, 0, 0 },
    { VK_HOME, 0, 0 }, { VK_END, 0, 0 }, { VK_DELETE, 0, 0 },
    { VK_BACK, 0, 0 },   /* 退格: 对话框上游直改原生时 Input 钩收不到, 轮询后备 */
};
static int ce_CtrlPrev[4] = { 0, 0, 0, 0 };   /* C/X/V/A 边沿 */

/* ---------------- 小工具 (无 CRT) ---------------- */

static int CE_KeyDown(unsigned vk)
{
    return (GetAsyncKeyState((int)vk) & 0x8000) ? 1 : 0;
}

/* 区间可读判定。IsPlausiblePtr 是"单指针 4 对齐"判定, 拿它判 [p,p+n) 会
 * 因尾地址不 4 对齐而恒假: +0x31A (0x318+2) 恒 mod4=2、环内奇数游标恒
 * 不对齐 —— 英文收编全灭就是这么来的。此处只看起点对齐 + 末字节合法。 */
static int CE_SafeRange(const void* p, unsigned n)
{
    unsigned a = (unsigned)(size_t)p, e;
    if (!n) return 0;
    e = a + n - 1u;
    if (e < a) return 0;                       /* 回绕 */
    if (a < 0x00010000u || e >= 0x7FF00000u) return 0;
    if (a & 3u) return 0;
    return 1;
}

static void CE_EnsureInit(void)
{
    char faceCfg[64];
    if (ce_Init) return;
    ce_Init = 1;
    BuildPaths();
    GtEnsureLog();
    ce_Enable = GetPrivateProfileIntA(CE_SECTION, "ChatEdit", 1, g_IniPath) ? 1 : 0;
    /* 字体与战役框同源 (同键同解析, 显示天然一致) */
    GetPrivateProfileStringA(CE_SECTION, "FontFace", "msyh.ttc",
                             faceCfg, sizeof(faceCfg), g_IniPath);
    faceCfg[sizeof(faceCfg) - 1] = 0;
    ResolveFontFace(faceCfg, "chatedit", ce_Face, L"Microsoft YaHei");
    ce_Bold = GetPrivateProfileIntA(CE_SECTION, "Bold", 0, g_IniPath) ? 1 : 0;
    LogLine(ce_Enable ? "chatedit: custom input ON" : "chatedit: custom input OFF");
}

static int CE_NativeIsEdit(void)
{
    return (*(unsigned char*)(CE_MSGLIST + CE_OFF_ISEDIT)) ? 1 : 0;
}

static int CE_NativeFieldW(void)
{
    int w = *(int*)(CE_MSGLIST + CE_OFF_FIELDW);
    return w;
}

static void CE_SelRange(int* lo, int* hi)
{
    if (ce_Anchor < 0 || ce_Anchor == ce_Cur) { *lo = *hi = -1; return; }
    *lo = (ce_Anchor < ce_Cur) ? ce_Anchor : ce_Cur;
    *hi = (ce_Anchor < ce_Cur) ? ce_Cur : ce_Anchor;
}

/* GDI 自然宽度 (与 RenderLine 同口径: ABC 全宽, 无 DigitTrack)。
 * 调用前须 GdiEnsure 成功 (g_G->dc 有效)。 */
static int CE_TextWidth(const wchar_t* s, int n)
{
    int tot = 0, i;
    if (!g_G || !g_G->dc || !s || n <= 0) return 0;
    for (i = 0; i < n; i++)
    {
        ABC abc;
        int ga;
        if (!GetCharABCWidthsW(g_G->dc, s[i], s[i], &abc)) return -1;
        ga = abc.abcA + abc.abcB + abc.abcC;
        if (ga < 0) ga = 0;
        tot += ga;
    }
    return tot;
}

/* 文本镜像进原生缓冲 (提交/原生路径永远看到有效文本)。
 * 光标置尾 (有效位内), 起点置 0 (空=取消, 非空=发送, 与原生语义一致)。 */
static void CE_Mirror(void)
{
    wchar_t* nb = (wchar_t*)(CE_MSGLIST + CE_OFF_EDITBUF);
    int i;
    if (!IsPlausiblePtr(nb)) return;
    if (ce_Len > CE_BUF_N - 1) ce_Len = CE_BUF_N - 1;
    for (i = 0; i < ce_Len; i++) nb[i] = ce_Buf[i];
    nb[ce_Len] = 0;
    *(int*)(CE_MSGLIST + CE_OFF_CURPOS) = ce_Len;
    *(int*)(CE_MSGLIST + CE_OFF_INITPOS) = 0;
}

/* ---------------- 编辑操作 (调用后须 CE_Mirror + 闪烁复位) ---------------- */

static void CE_BlinkReset(void)
{
    ce_BlinkT = GetTickCount();
}

static int CE_LivePx(void)
{
    void* font = *(void**)CE_BITFONT;
    int px = font ? *(int*)((char*)font + CE_OFF_FONT_H) : 16;
    if (px < 8 || px > 64) px = 16;
    return px;
}

/* 度量前固定切到输入槽 (Input 上下文里 g_G 可能指向战役/提示槽,
 * 用错槽预算就按错字体算; GdiEnsure 命中缓存时只是几次比较, 按键级无压力) */
static int CE_EnsureFont(void)
{
    return GdiEnsure(CE_LivePx(), ce_Bold, CE_GDI_SLOT, ce_Face);
}

static int CE_BudgetOk(int newLen, int fieldW)
{
    int w;
    if (newLen >= CE_BUF_N) return 0;
    if (!CE_EnsureFont()) return 1;   /* 字体就绪失败不卡 (只剩字数上限) */
    w = CE_TextWidth(ce_Buf, newLen);
    if (w < 0) return 1;   /* 度量失败不卡 (只剩字数上限) */
    return (w <= fieldW - 10) ? 1 : 0;
}

static void CE_DeleteRange(int lo, int hi)
{
    int i;
    if (lo < 0 || hi <= lo) return;
    if (lo < 0) lo = 0;
    if (hi > ce_Len) hi = ce_Len;
    for (i = hi; i <= ce_Len; i++) ce_Buf[i - (hi - lo)] = ce_Buf[i];
    ce_Len -= (hi - lo);
    ce_Cur = lo;
    ce_Anchor = -1;
}

static void CE_InsertOne(wchar_t c, int fieldW)
{
    int lo, hi, i;
    int keep;
    CE_SelRange(&lo, &hi);
    if (lo >= 0) CE_DeleteRange(lo, hi);
    if (ce_Len >= CE_BUF_N - 1) return;
    for (i = ce_Len; i >= ce_Cur; i--) ce_Buf[i + 1] = ce_Buf[i];
    ce_Buf[ce_Cur] = c;
    keep = ce_Len + 1;
    ce_Len++;
    ce_Cur++;
    ce_Anchor = -1;
    if (!CE_BudgetOk(keep, fieldW))
    {
        /* 超宽回退 (与原生拒收同效果) */
        for (i = ce_Cur; i <= ce_Len; i++) ce_Buf[i - 1] = ce_Buf[i];
        ce_Len--;
        ce_Cur--;
    }
}

static void CE_DoBackspace(void)
{
    int lo, hi;
    CE_SelRange(&lo, &hi);
    if (lo >= 0) { CE_DeleteRange(lo, hi); return; }
    if (ce_Cur <= 0) return;
    CE_DeleteRange(ce_Cur - 1, ce_Cur);
}

static void CE_DoDelete(void)
{
    int lo, hi;
    CE_SelRange(&lo, &hi);
    if (lo >= 0) { CE_DeleteRange(lo, hi); return; }
    if (ce_Cur >= ce_Len) return;
    CE_DeleteRange(ce_Cur, ce_Cur + 1);
}

static int CE_IsWord(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
           (c >= L'0' && c <= L'9') || c == L'_';
}

static void CE_MoveWord(int dir)
{
    int i = ce_Cur;
    if (dir < 0)
    {
        if (i <= 0) return;
        i--;
        if (ce_Buf[i] < 0x80)
        {
            int cls = (ce_Buf[i] == L' ') ? 0 : (CE_IsWord(ce_Buf[i]) ? 1 : 2);
            if (cls != 2)
            {
                while (i > 0)
                {
                    int c2 = (ce_Buf[i - 1] == L' ') ? 0
                        : (CE_IsWord(ce_Buf[i - 1]) ? 1 : 2);
                    if (c2 != cls) break;
                    i--;
                }
            }
        }
        ce_Cur = i;
    }
    else
    {
        if (i >= ce_Len) return;
        if (ce_Buf[i] < 0x80)
        {
            int cls = (ce_Buf[i] == L' ') ? 0 : (CE_IsWord(ce_Buf[i]) ? 1 : 2);
            if (cls != 2)
            {
                while (i < ce_Len)
                {
                    int c2 = (ce_Buf[i] == L' ') ? 0
                        : (CE_IsWord(ce_Buf[i]) ? 1 : 2);
                    if (c2 != cls) break;
                    i++;
                }
            }
            else i++;
        }
        else i++;
        ce_Cur = i;
    }
}

/* ---------------- 剪贴板 (全本地 Win32) ---------------- */

static int CE_ClipboardGet(wchar_t* dst, int cap)
{
    int n = 0;
    HGLOBAL h;
    const wchar_t* p;
    SIZE_T bytes;
    if (!OpenClipboard(NULL)) return 0;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h)
    {
        p = (const wchar_t*)GlobalLock(h);
        if (p && IsPlausiblePtr(p))
        {
            bytes = GlobalSize(h);
            while (n < cap - 1 && (SIZE_T)(n + 1) * 2 < bytes && p[n]) n++;
            {
                int i;
                for (i = 0; i < n; i++) dst[i] = p[i];
            }
        }
        if (p) GlobalUnlock(h);
    }
    CloseClipboard();
    return n;
}

static void CE_ClipboardSet(const wchar_t* s, int n)
{
    HGLOBAL h;
    wchar_t* p;
    int i;
    if (n <= 0 || !s) return;
    if (n > CE_BUF_N - 1) n = CE_BUF_N - 1;
    h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(n + 1) * 2);
    if (!h) return;
    p = (wchar_t*)GlobalLock(h);
    if (!p) { GlobalFree(h); return; }
    for (i = 0; i < n; i++) p[i] = s[i];
    p[n] = 0;
    GlobalUnlock(h);
    if (!OpenClipboard(NULL)) { GlobalFree(h); return; }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, h);   /* 所有权转交剪贴板, 不再释放 */
    CloseClipboard();
}

static void CE_DoCopy(int cut)
{
    int lo, hi;
    CE_SelRange(&lo, &hi);
    if (lo < 0) return;
    CE_ClipboardSet(ce_Buf + lo, hi - lo);
    if (cut) { CE_DeleteRange(lo, hi); CE_Mirror(); CE_BlinkReset(); }
}

static void CE_DoPaste(int fieldW)
{
    wchar_t tmp[CE_BUF_N];
    int n, i;
    int lo, hi;
    n = CE_ClipboardGet(tmp, CE_BUF_N);
    if (n <= 0) return;
    CE_SelRange(&lo, &hi);
    if (lo >= 0) { CE_DeleteRange(lo, hi); }
    for (i = 0; i < n; i++)
    {
        wchar_t c = tmp[i];
        if (c < 0x20) c = L' ';   /* 单行: 控制符转空格 */
        CE_InsertOne(c, fieldW);
    }
    CE_Mirror();
    CE_BlinkReset();
}

/* ---------------- 组字轮询 (Imm, 无钩) ---------------- */

static void CE_PollIME(void)
{
    HWND fg;
    HIMC himc;
    LONG r;
    ce_CompLen = 0;
    ce_CompCur = 0;
    fg = GetForegroundWindow();
    himc = NULL;
    r = -999;
    if (fg && fg == ce_FgWnd)
    {
        himc = ImmGetContext(fg);
        if (himc)
        {
            r = ImmGetCompositionStringW(himc, GCS_COMPSTR, ce_Comp,
                                        (DWORD)(CE_BUF_N - 1) * 2);
            if (r > 0)
            {
                int n = (int)(r / 2);
                LONG cur;
                if (n > CE_BUF_N - 1) n = CE_BUF_N - 1;
                ce_Comp[n] = 0;
                ce_CompLen = n;
                cur = ImmGetCompositionStringW(himc, GCS_CURSORPOS, NULL, 0);
                if (cur < 0) cur = 0;
                if (cur > n) cur = n;
                ce_CompCur = (int)cur;
            }
            ImmReleaseContext(fg, himc);
        }
    }
}

/* ---------------- 导航/命令轮询 (箭头等到不了 Input, 在此处理) ---------------- */

static void CE_NavMove(int vk, int shift, int ctrl)
{
    int old = ce_Cur;
    if (!shift) ce_Anchor = -1;
    else if (ce_Anchor < 0) ce_Anchor = old;
    switch (vk)
    {
    case VK_LEFT:  if (ctrl) CE_MoveWord(-1); else if (ce_Cur > 0) ce_Cur--; break;
    case VK_RIGHT: if (ctrl) CE_MoveWord(1);  else if (ce_Cur < ce_Len) ce_Cur++; break;
    case VK_HOME:  ce_Cur = 0; break;
    case VK_END:   ce_Cur = ce_Len; break;
    default: break;
    }
    if (!shift) ce_Anchor = -1;
    else if (ce_Anchor == ce_Cur) ce_Anchor = -1;
    if (ce_ViewX < 0) ce_ViewX = 0;
    CE_BlinkReset();
}

/* LL 与轮询共用: 移动/删除 + 握手时刻 */
static void CE_ApplyNav(int vk)
{
    int shift = CE_KeyDown(VK_SHIFT);
    int ctrl = CE_KeyDown(VK_CONTROL);
    if (vk == VK_DELETE) { CE_DoDelete(); CE_Mirror(); }
    else if (vk == VK_BACK) { CE_DoBackspace(); CE_Mirror(); }
    else CE_NavMove(vk, shift, ctrl);
    ce_NavTick = GetTickCount();
}

static void CE_PollKeys(int fieldW)
{
    unsigned now = GetTickCount();
    unsigned i;
    int ctrl = CE_KeyDown(VK_CONTROL);
    for (i = 0; i < sizeof(ce_NavKeys) / sizeof(ce_NavKeys[0]); i++)
    {
        int down = CE_KeyDown((unsigned)ce_NavKeys[i].vk);
        /* 边缘: 只在 LL 长期静默 (>2s, 基本等于 LL 已死) 才接管;
         * LL 活着时边缘一律让路 (LL 那步已走)。慢点按 (>150ms 间隔)
         * 不再被 LL+轮询各走一步 (双步跳字, 体感"卡住")。 */
        if (down && !ce_NavKeys[i].prev && (int)(now - ce_LLLastTick) > 2000)
        {
            CE_ApplyNav(ce_NavKeys[i].vk);
            ce_NavKeys[i].nextRep = now + 400;
        }
        /* 连发: 轮询按 33ms 自走; 若自上次轮询步进后有新 LL 事件,
         * 本拍跳过并重锚 (LL 那步已走, 防双步; 锚后下拍继续, 不饿死)。
         * LL 30Hz 时轮询基本全跳过 (LL 全包); LL 6Hz/死亡时轮询补满。 */
        else if (down && (int)(now - ce_NavKeys[i].nextRep) >= 0)
        {
            if (ce_LLCnt != ce_LLSeen)
            { ce_LLSeen = ce_LLCnt; ce_NavKeys[i].nextRep = now + 33; }
            else
            { CE_ApplyNav(ce_NavKeys[i].vk); ce_NavKeys[i].nextRep = now + 33; }
        }
        ce_NavKeys[i].prev = down;
    }
    /* Ctrl+字母: Input 钩优先 (同帧先跑), 轮询只做后备 (150ms 握手防双触发) */
    if (ctrl && (int)(now - ce_CmdTick) > 150)
    {
        static const int vks[4] = { 'C', 'X', 'V', 'A' };
        for (i = 0; i < 4; i++)
        {
            int down = CE_KeyDown((unsigned)vks[i]);
            if (down && !ce_CtrlPrev[i])
            {
                if (i == 0) CE_DoCopy(0);
                else if (i == 1) CE_DoCopy(1);
                else if (i == 2) CE_DoPaste(fieldW);
                else { ce_Anchor = 0; ce_Cur = ce_Len; CE_BlinkReset(); }
                ce_CmdTick = now;
            }
            ce_CtrlPrev[i] = down;
        }
    }
    else if (!ctrl)
    {
        for (i = 0; i < 4; i++) ce_CtrlPrev[i] = 0;
    }
}

/* 泵线程 (独立线程, 与主循环解耦): 框开着时以 ~60Hz 投 WM_NULL,
 * 主循环不断流。无锁、无阻塞调用, 联机安全。 */
static DWORD WINAPI CE_Watchdog(LPVOID unused)
{
    (void)unused;
    for (;;)
    {
        /* 喂泵: 框开着时以 ~60Hz 投 WM_NULL, 主循环不断流。
         * 主循环一消息一帧, 吞掉方向键后无其它消息即饿死
         * (冻住、动一次走一帧); 线程投递与主循环解耦, 联机安全。 */
        if (ce_Active && ce_FgWnd) PostMessageA(ce_FgWnd, WM_NULL, 0, 0);
        Sleep(16);
    }
    return 0;
}

static int ce_PumpOn = 0;
static void CE_PumpEnsure(void)
{
    if (ce_PumpOn) return;
    ce_PumpOn = 1;
    {
        HANDLE h = CreateThread(NULL, 0, CE_Watchdog, NULL, 0, NULL);
        if (h) CloseHandle(h);   /* 泵线程自跑, 句柄不留 */
    }
}

/* ---------------- 低级键盘钩 (导航/编辑键消息级处理+吞掉, 防滚地图/旁路) ---------------- */

static HHOOK ce_LLHook = NULL;

static LRESULT CALLBACK CE_LLProc(int nCode, WPARAM wp, LPARAM lp)
{
    if (nCode == HC_ACTION && ce_Active &&
        (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN))
    {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lp;
        unsigned vku = k->vkCode;
        if (!(k->flags & LLKHF_INJECTED) &&
            GetForegroundWindow() == ce_FgWnd && ce_FgWnd &&
            (vku == VK_LEFT || vku == VK_RIGHT || vku == VK_HOME ||
             vku == VK_END || vku == VK_DELETE || vku == VK_BACK) &&
            ce_CompLen <= 0)
        {
            /* 消息级直接处理 (不依赖轮询/重绘率) + 吞掉:
             * 对话框层看不到这些键, 不再旁路改原生缓冲。
             * 组字中放行 (组字光标归 IME, 与原生一致) */
            CE_ApplyNav((int)vku);
            ce_LLCnt++;   /* 轮询重锚判据 (计次, 不计时: LL 再稀疏也不饿死轮询) */
            ce_LLLastTick = GetTickCount();   /* 边缘退让判据 */
            /* 喂泵: 主循环是消息驱动的, 吞掉方向键后若鼠标不动又无其它消息,
             * 队列 starved → 主循环饿死 → 整局冻住、松开 (keyup 照常投递)
             * 才恢复。补一个无副作用的 WM_NULL 让循环继续转; 游戏看不到
             * 方向键 (照样吞), 地图不滚、旁路不写。 */
            if (ce_FgWnd) PostMessageA(ce_FgWnd, WM_NULL, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(ce_LLHook, nCode, wp, lp);
}

static void CE_LLOn(void)
{
    HMODULE hm = NULL;
    if (ce_LLHook) return;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)CE_LLProc, &hm);
    ce_LLHook = SetWindowsHookExA(WH_KEYBOARD_LL, CE_LLProc, hm, 0);
}

static void CE_LLOff(void)
{
    if (!ce_LLHook) return;
    UnhookWindowsHookEx(ce_LLHook);
    ce_LLHook = NULL;
}

/* ---------------- 激活/关闭 ---------------- */

static void CE_Activate(void)
{
    wchar_t* nb = (wchar_t*)(CE_MSGLIST + CE_OFF_EDITBUF);
    int n = 0;
    unsigned k;
    ce_Len = 0; ce_Cur = 0; ce_Anchor = -1; ce_ViewX = 0;
    ce_CompLen = 0; ce_CompCur = 0;
    /* 同步原生缓冲: 只认领, 不显示。
     * AddEdit 预填的发送模板 ("从Name："等) 在联机中会自动加用户名,
     * 显示是噪音、发送是事故: 存档 (发送时兜底剥离) 后直接清空,
     * 自己和原生缓冲都归零 (镜像覆盖原生模板)。 */
    if (IsPlausiblePtr(nb))
    {
        while (n < CE_BUF_N - 1 && nb[n]) { ce_Buf[n] = nb[n]; n++; }
        ce_Len = n;
    }
    ce_Buf[ce_Len] = 0;
    ce_Cur = ce_Len;
    ce_PreLen = (ce_Len < CE_BUF_N) ? ce_Len : CE_BUF_N - 1;
    {
        int k;
        for (k = 0; k <= ce_PreLen; k++) ce_Pre[k] = ce_Buf[k];
    }
    ce_Len = 0; ce_Buf[0] = 0; ce_Cur = 0; ce_Anchor = -1; ce_ViewX = 0;
    CE_Mirror();
    ce_FgWnd = GetForegroundWindow();
    if (!ce_FgWnd) ce_FgWnd = GetActiveWindow();
    for (k = 0; k < sizeof(ce_NavKeys) / sizeof(ce_NavKeys[0]); k++)
    { ce_NavKeys[k].prev = 0; ce_NavKeys[k].nextRep = 0; }
    ce_LLSeen = ce_LLCnt;
    ce_LLLastTick = GetTickCount();   /* 先假设 LL 活着 (2s 内纠正) */
    for (k = 0; k < 4; k++) ce_CtrlPrev[k] = 0;
    /* 行缓冲游标快照 (摘除判据: 只收编开框后新追加的字, 不碰旧残留) */
    ce_LastCur = 0xFFFFFFFFu;
    {
        void* ln = NULL;
        if (IsPlausiblePtr((void*)CE_LINEOBJ_ADDR))
            ln = *(void**)CE_LINEOBJ_ADDR;
        if (ln && CE_SafeRange((char*)ln + CE_LINECUR_OFF, 4))
            ce_LastCur = *(unsigned*)((char*)ln + CE_LINECUR_OFF);
    }
    CE_CharPostInstall();   /* 懒安装 (Ares 系钩子早下完, 此时下最稳) */
    ce_Active = 1;
    CE_LLOn();
    CE_BlinkReset();
}

static void CE_Deactivate(void)
{
    ce_Active = 0;
    ce_Anchor = -1;
    ce_CompLen = 0;
    CE_LLOff();
}

/* ---------------- 绘制 ---------------- */

typedef void* (__fastcall *CE_LockFn)(void*, void*, int, int);
typedef int (__fastcall *CE_UnlockFn)(void*, void*);
typedef int (__fastcall *CE_FillFn)(void*, void*, RectangleStruct*,
                                   ColorStruct*, int);
typedef int (__fastcall *CE_GetIntFn)(void* t, void* edxUnused);

static int CE_SurfGet(void* surf, unsigned vtOff)
{
    unsigned char* vt;
    CE_GetIntFn fn;
    if (!surf || !IsPlausiblePtr(surf)) return -1;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return -2;
    fn = (CE_GetIntFn)(*(void**)(vt + vtOff));
    if (!fn) return -3;
    return fn(surf, NULL);
}

static void CE_FillRect(void* surf, int x, int y, int w, int h,
                        unsigned char r, unsigned char g, unsigned char b,
                        int op)
{
    unsigned char* vt;
    CE_FillFn fn;
    RectangleStruct rc;
    ColorStruct c;
    if (!surf || !IsPlausiblePtr(surf) || w <= 0 || h <= 0) return;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    fn = (CE_FillFn)(*(void**)(vt + CE_VT_FILLRECT));
    if (!fn) return;
    rc.X = x; rc.Y = y; rc.Width = w; rc.Height = h;
    c.R = r; c.G = g; c.B = b;
    fn(surf, NULL, &rc, &c, op);
}

static int CE_Render(void* surf, int bitLineH, int surfW, int surfH,
                     int surfPitch)
{
    unsigned char* vt;
    CE_LockFn lockFn;
    CE_UnlockFn unlockFn;
    unsigned char* locked;
    int fieldW, lx, ly, lineH;
    int xs[CE_BUF_N + 1];
    int i, tx, ty, bw, bh;
    int startIdx, endIdx, visLen, dx, curX, lo, hi;
    int compX, compWsum, compFit, th, boxY;
    void* label;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    lineH = bitLineH;
    if (lineH < 8 || lineH > 64) lineH = 16;
    fieldW = CE_NativeFieldW();
    if (fieldW < 80 || fieldW > surfW) fieldW = (surfW - 16 > 80) ? surfW - 16 : 640;
    /* 位置跟原生编辑行: 首选对话框维护的 EditPos (+0x1C, 屏幕坐标);
     * label 自身 X/Y 是创建占位 (实测常落 (3,0) 左上, 不是绘制位)。
     * 都非法才回退底部条带。 */
    lx = 8; ly = surfH - lineH - 12;
    {
        int ex = *(int*)(CE_MSGLIST + CE_OFF_EDITPOS);
        int ey = *(int*)(CE_MSGLIST + CE_OFF_EDITPOS + 4);
        if (ex >= 0 && ex < surfW && ey >= 0 && ey < surfH) { lx = ex; ly = ey; }
        else
        {
            label = *(void**)(CE_MSGLIST + CE_OFF_EDITLABEL);
            if (label && IsPlausiblePtr(label))
            {
                int qx = *(int*)((char*)label + 12);
                int qy = *(int*)((char*)label + 16);
                if (qx >= 0 && qx < surfW && qy >= 0 && qy < surfH) { lx = qx; ly = qy; }
            }
        }
    }
    if (!GdiEnsure(lineH, ce_Bold, CE_GDI_SLOT, ce_Face)) return 0;
    /* 逐字 x 偏移 (与 RenderLine 同口径) */
    xs[0] = 0;
    for (i = 0; i < ce_Len; i++)
    {
        ABC abc;
        int ga = lineH;
        if (g_G && g_G->dc && GetCharABCWidthsW(g_G->dc, ce_Buf[i], ce_Buf[i], &abc))
        {
            ga = abc.abcA + abc.abcB + abc.abcC;
            if (ga < 0) ga = 0;
        }
        xs[i + 1] = xs[i] + ga;
    }
    /* 滚动保证光标可见 */
    curX = xs[ce_Cur];
    if (curX - ce_ViewX > fieldW - 8) ce_ViewX = curX - fieldW + 24;
    if (curX < ce_ViewX) ce_ViewX = curX;
    if (ce_ViewX < 0) ce_ViewX = 0;
    startIdx = 0;
    while (startIdx < ce_Len && xs[startIdx + 1] <= ce_ViewX) startIdx++;
    /* 可见窗口三重截断: 像素(fieldW/1000 滚窗) + DIB 容量
     * (RenderLine 一次最多画 DIB_W-8 像素, 超的 RenderLine 会吞;
     * 之前只截像素不断 DIB, 超 42 字就画一半留黑尾)。
     * 盒只包画出的部分: 哪边卡住都不可能再有黑尾。 */
    endIdx = startIdx;
    while (endIdx < ce_Len &&
           xs[endIdx + 1] - xs[startIdx] <= fieldW &&
           xs[endIdx + 1] - xs[startIdx] <= 1000) endIdx++;
    {
        int fit = CharsFit(ce_Buf + startIdx, endIdx - startIdx, DIB_W - 8);
        if (fit < 0) fit = 0;
        if (fit < endIdx - startIdx) endIdx = startIdx + fit;
    }
    visLen = endIdx - startIdx;
    /* 组字宽 (盒跟宽用; 同样按 DIB 容量截, 与画出的一致) */
    compWsum = 0;
    compFit = 0;
    if (ce_CompLen > 0)
    {
        compFit = CharsFit(ce_Comp, ce_CompLen, DIB_W - 8);
        if (compFit < 0) compFit = 0;
        if (compFit > ce_CompLen) compFit = ce_CompLen;
        compWsum = CE_TextWidth(ce_Comp, compFit);
        if (compWsum < 0) compWsum = 0;
    }
    /* 盒跟画出的走 (左右 6 内边距; 高按真实行步进, 上下 3) */
    th = (g_G && g_G->adv > 0) ? g_G->adv : lineH;
    {
        int drawnW = (xs[endIdx] - xs[startIdx]) + compWsum;
        bw = drawnW + 12;
        if (bw < 96) bw = 96;
        if (bw > fieldW + 8) bw = fieldW + 8;
    }
    bh = th + 6;
    tx = lx; ty = ly;
    boxY = ty - 3;
    /* 墨水在盒内垂直居中 (数字/字母与汉字同基线是 GDI 天然行为;
     * 之前字贴盒顶是因为盒矮+笔位顶对齐)。 */
    {
        int textCy = th;
        if (g_G && g_G->dc && visLen > 0)
        {
            SIZE sz;
            if (GetTextExtentPoint32W(g_G->dc, ce_Buf + startIdx, visLen, &sz) &&
                sz.cy > 0 && sz.cy <= 64)
                textCy = sz.cy;
        }
        ty = boxY + (bh - textCy) / 2;
    }
    dx = tx - (xs[startIdx] - ce_ViewX);
    CE_SelRange(&lo, &hi);

    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    lockFn = (CE_LockFn)(*(void**)(vt + CE_VT_LOCK));
    unlockFn = (CE_UnlockFn)(*(void**)(vt + CE_VT_UNLOCK));
    if (!lockFn || !unlockFn) return 0;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return 0;

    /* 底板 (不透明, 盖住原生行) */
    CE_FillRect(surf, tx - 4, boxY, bw, bh, 0, 0, 0, 100);
    /* 选中底 (钳到盒内) */
    if (lo >= 0)
    {
        int sx1 = dx + (xs[lo] - xs[startIdx]);
        int sx2 = dx + (xs[hi] - xs[startIdx]);
        if (sx1 < tx) sx1 = tx;
        if (sx2 > tx + bw - 4) sx2 = tx + bw - 4;
        if (sx2 > sx1)
            CE_FillRect(surf, sx1, ty, sx2 - sx1, th, 51, 102, 204, 70);
    }
    /* 文本 (只画可见窗) */
    if (visLen > 0)
    {
        int w = RenderLine(ce_Buf + startIdx, visLen);
        if (w > 0)
            BlitCoverage(locked, surfPitch, surfW, surfH, dx, ty, w + 2, th,
                         255, 255, 255, 100);
    }
    /* 组字串 (黄色+下划线, 光标跟组字光标; 宽钳到盒内) */
    compX = dx + (xs[ce_Cur] - xs[startIdx]);
    if (ce_CompLen > 0 && compFit > 0)
    {
        int w = RenderLine(ce_Comp, compFit);
        if (w > 0)
        {
            int ccx = compX, k;
            int acc = 0;
            int showW = w + 2;
            int maxW = tx + bw - 2 - compX;
            if (showW > maxW) showW = maxW;
            for (k = 0; k < ce_CompCur && k < compFit; k++)
            {
                ABC abc;
                int ga = lineH;
                if (g_G && g_G->dc &&
                    GetCharABCWidthsW(g_G->dc, ce_Comp[k], ce_Comp[k], &abc))
                {
                    ga = abc.abcA + abc.abcB + abc.abcC;
                    if (ga < 0) ga = 0;
                }
                acc += ga;
            }
            if (showW > 0)
            {
                BlitCoverage(locked, surfPitch, surfW, surfH, compX, ty, showW, th,
                             255, 255, 0, 100);
                CE_FillRect(surf, compX, ty + th - 1, showW, 1, 255, 255, 0, 100);
            }
            ccx = compX + acc;
            compX = ccx;
        }
    }
    /* 光标 (有选中则隐藏, 530ms 闪烁; 钳到盒内) */
    if (compX < tx) compX = tx;
    if (compX > tx + bw - 2) compX = tx + bw - 2;
    if (lo < 0 && ((GetTickCount() - ce_BlinkT) / 530u) % 2u == 0u)
        CE_FillRect(surf, compX, ty, 2, th, 255, 255, 255, 100);

    unlockFn(surf, NULL);
    (void)i;
    return 1;
}

/* 发送前剥离未动过的预填前缀 ("从Name："不应被发送)。
 * 有改动: 前缀区被编辑过则尊重原文。调用后已镜像。 */
static void CE_StripPrefix(void){
    int k, i;
    if (ce_PreLen <= 0 || ce_Len < ce_PreLen) return;
    for (k = 0; k < ce_PreLen; k++)
        if (ce_Buf[k] != ce_Pre[k]) return;
    for (i = ce_PreLen; i <= ce_Len; i++) ce_Buf[i - ce_PreLen] = ce_Buf[i];
    ce_Len -= ce_PreLen;
    ce_Cur = (ce_Cur > ce_PreLen) ? ce_Cur - ce_PreLen : 0;
    ce_Anchor = -1;
    CE_Mirror();
    CE_BlinkReset();
}

/* ---------------- 发送指纹 (单机跟色用) ----------------
 * 单机回声 AddMessage 无名 (text 裸奔+默认色), 名字判别失效:
 * 回车瞬间把发出的文本存档, AddMessage 侧文本命中即"自己的话"。 */

#define CE_SENT_N 4
typedef struct { wchar_t s[CE_BUF_N]; int len; unsigned tick; } CESent;
static CESent ce_Sent[CE_SENT_N];
static int ce_SentHead = 0;

static void CE_NoteSent(void)
{
    CESent* e = &ce_Sent[ce_SentHead];
    int i;
    ce_SentHead = (ce_SentHead + 1) % CE_SENT_N;
    e->len = (ce_Len < CE_BUF_N) ? ce_Len : CE_BUF_N - 1;
    for (i = 0; i < e->len; i++) e->s[i] = ce_Buf[i];
    e->s[e->len] = 0;
    e->tick = GetTickCount();
}

/* 命中最近发出的文本? (回声带原生追加的尾空格, 容忍尾空白; 5s 有效) */
int ChatEdit_MatchSent(const wchar_t* msg)
{
    int si, ml, k;
    unsigned now;
    if (!msg || !IsPlausiblePtr(msg)) return 0;
    ml = 0; while (ml < CE_BUF_N * 2 && msg[ml]) ml++;
    now = GetTickCount();
    for (si = 0; si < CE_SENT_N; si++)
    {
        CESent* e = &ce_Sent[si];
        if (e->len <= 0) continue;
        if (now - e->tick > 5000u) continue;
        if (ml < e->len) continue;
        for (k = 0; k < e->len; k++)
            if (msg[k] != e->s[k]) break;
        if (k < e->len) continue;
        for (; k < ml; k++)
            if (msg[k] != L' ' && msg[k] != 0x3000u) break;
        if (k >= ml) return 1;
    }
    return 0;
}

/* 当前真有组字? (钩子内现查, 不用轮询 stale 值; Imm 三连 µs 级。
 * 有组字时 ASCII 键 IME 已收编 (原生凭 IMEBuffer 标志位同样跳过),
 * 钩子照打只会留下英文残渣随消息发出。) */
static int CE_CompActiveNow(void)
{
    HWND fg = GetForegroundWindow();
    HIMC himc;
    LONG r;
    if (!fg || fg != ce_FgWnd) return 0;
    himc = ImmGetContext(fg);
    if (!himc) return 0;
    r = ImmGetCompositionStringW(himc, GCS_COMPSTR, NULL, 0);
    ImmReleaseContext(fg, himc);
    return (r > 0) ? 1 : 0;
}

/* ---------------- WM_CHAR 摘除钩 (英文输入的唯一入口) ----------------
 * 反汇编实证: WM_CHAR (0x102) 不进 0xB730EC 队列/Input, 对话框层 0x54F790
 * 的 WM_CHAR 分支 (跳表项 0x54F85C, isEdit 门控) 先 0x54F200(0x0A,0) 往
 * 行环写标志字, 再 0x54F1C0(ch) 写原文 —— Input 钩永远见不到英文, 而每帧
 * CE_Mirror 以我方缓冲为准覆盖原生 ⇒ 不收编就是英文黑洞。
 * (中文走 WM_IME_CHAR→队列→Input, 不经此处。)
 * 站点取 0x54F8B4 (原生 call 之后那条 `jmp 0x54fb18`, 5B 无 rel, 除
 * fall-through 外无别的分支跳进来): 原生照写环, 我方随后把刚 push 的那两格
 * 摘走 (收编进 ce_Buf + 游标回退 + 清格, 脏字不留给原生消费), 再跳回原目标。
 * 懒安装 (首帧开框时下, Ares 系钩子那时早下完了)。栈: 裸函数
 * pusha/call C/popa/push imm32+ret 净零; 跳回游戏地址走 push imm32+ret
 * (本 DLL 会被重定基址, E9 rel 不可用)。状态全从全局/游戏读, 联机安全。 */
#define CE_CHARPOST_SITE 0x0054F8B4u
#define CE_CHARPOST_BACK 0x0054FB18u

static int ce_PostOn = 0;          /* 补丁已下 (只试一次) */

/* 非 static (裸汇编按 C 符号引用, C++ 粉碎后链接不到)。
 * this_: 摘除点 ecx 即对话框层传进来的 this (0x54F1C0 不碰 ecx, 入口
 * jmp 时原样保留)。优先用它, 不可信才回退全局 (Ares 等可能自有对象)。
 * 行环是真队列 (0x54F610 Pop 推进 0x314, 0x54F670 Push 即 0x54F1C0),
 * 游戏在持续消费, 所以判据要按"这一格是不是刚 push 的"来, 不能只看游标。 */
extern "C" void CE_CharAdopt2(void* this_)
{
    void* line = NULL;
    unsigned short* ring;
    unsigned cur, tail, used, idx, back, w;
    int fieldW;

    CE_EnsureInit();
    /* 对象选择: 优先入口 this_, 回退全局。判据必须是"区间可读"而不是
     * IsPlausiblePtr (后者要求 4 对齐, base+0x318+2 恒 mod4=2 必假)。 */
    if (this_ && CE_SafeRange((char*)this_ + CE_LINECUR_OFF, 4))
        line = this_;
    else if (IsPlausiblePtr((void*)CE_LINEOBJ_ADDR))
    {
        void* g = *(void**)CE_LINEOBJ_ADDR;
        if (g && CE_SafeRange((char*)g + CE_LINECUR_OFF, 4)) line = g;
    }
    if (!line || !CE_SafeRange((char*)line + CE_LINEBUF_OFF,
                               CE_RING_N * 2 + 8))
        return;                     /* 形状不对: passthrough, 不碰原生 */
    cur = *(unsigned*)((char*)line + CE_LINECUR_OFF);
    tail = *(unsigned*)((char*)line + CE_RING_TAIL);
    if (cur >= CE_RING_N || tail >= CE_RING_N) return;   /* 未初始化 (-1) */
    used = (cur - tail) & (CE_RING_N - 1);
    if (cur == ce_LastCur) return;   /* 游标没动: (B) 没写, 别碰 */
    ce_LastCur = cur;
    if (!ce_Enable || !ce_Active) return;
    if (used == 0) return;           /* 队列空 (消费方抢在前面) */
    ring = (unsigned short*)((char*)line + CE_LINEBUF_OFF);
    idx = (cur - 1) & (CE_RING_N - 1);
    w = ring[idx];
    if (w >= 0x100) return;             /* 非原文 (标志字/其它), 留给原生 */
    if (w < 0x20 || w == 0x7F) return;  /* 控制符走原生 (与今日一致) */
    fieldW = CE_NativeFieldW();
    if (fieldW < 80) fieldW = 640;
    CE_InsertOne((wchar_t)w, fieldW);
    CE_Mirror();
    CE_BlinkReset();
    /* 摘除 (A)(B) 两格: 倒数第二格是本次 (A) 才退 2, 否则只退 1。
     * WM_CHAR 的 (A) 恒为 0x000A —— 0x54F899 传 (0x0A,0), 0x54F200 跳过
     * 修饰位轮询原样入环 (高位为 0), 只按 0xFF00 判会漏掉它。 */
    back = 1;
    if (used >= 2 && ring[(cur - 2) & (CE_RING_N - 1)] == CE_CHAR_FLAG)
        back = 2;
    ring[idx] = 0;
    if (back == 2) ring[(cur - 2) & (CE_RING_N - 1)] = 0;
    *(unsigned*)((char*)line + CE_LINECUR_OFF) = (cur - back) & (CE_RING_N - 1);
    ce_LastCur = (cur - back) & (CE_RING_N - 1);
}

__attribute__((naked)) void ChatEdit_CharPost(void)
{
    /* 注意: 本 DLL 会被系统重定基址 (实证 delta=0x8600000, 见 crash EIP),
     * E8/E9 的链接期 rel 只对 DLL 内部目标有效 (两端同漂)。
     * 跳回游戏固定地址必须用基址无关写法: push imm32(数值本身恒对)+ret,
     * 绝不能 jmp 0x54fb18 (链接期 rel, 重定基址后偏 delta)。 */
    __asm__ __volatile__(
        "pusha\n\t"
        "push %ecx\n\t"          /* this_ (入口 ecx, (B) 不碰它) */
        "call _CE_CharAdopt2\n\t"
        "add $4, %esp\n\t"
        "popa\n\t"
        "pushl $0x54fb18\n\t"    /* 基址无关跳回 (见上注释) */
        "ret\n\t"
    );
}

static void CE_CharPostInstall(void)
{
    unsigned char* p = (unsigned char*)CE_CHARPOST_SITE;
    unsigned target;
    int want, rel;
    DWORD old;
    if (ce_PostOn) return;
    ce_PostOn = 1;   /* 只试一次 (失败即认, 不刷屏不重复 VirtualProtect) */
    want = (int)(CE_CHARPOST_BACK - (CE_CHARPOST_SITE + 5));
    if (p[0] != 0xE9 || *(int*)(p + 1) != want)
    { LogLine("chatedit: charpost bytes mismatch, hook OFF"); return; }
    target = (unsigned)(size_t)ChatEdit_CharPost;
    rel = (int)(target - (CE_CHARPOST_SITE + 5));
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old))
    { LogLine("chatedit: charpost protect fail, hook OFF"); return; }
    p[0] = 0xE9;
    *(int*)(p + 1) = rel;
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    LogLine("chatedit: charpost hook ON");
}

/* ---------------- Input 运行时补丁 ----------------
 * 注: Input 只收 WM_IME_CHAR 队列 (中文/拼音)。英文走 WM_CHAR,
 * 由对话框层 0x54F790 直接 call 0x54F1C0 写原生行缓冲, 不过 Input;
 * 见上 ChatEdit_CharPost (0x54F8B4 后置摘除), 两钩互补才是全量输入。 */

typedef int (__fastcall *CE_InputFn)(void* t, void* edxUnused,
                                    int* keySlot, int key);
static CE_InputFn ce_OrigInput = NULL;

int __fastcall ChatEdit_InputHook(void* t, void* edxUnused,
                                 int* keySlot, int key)
{
    unsigned ukey;
    int ctrl;
    (void)edxUnused;
    CE_EnsureInit();
    if (!ce_OrigInput) return 0;
    if (!ce_Enable || !ce_Active)
        return ce_OrigInput(t, edxUnused, keySlot, key);
    if (!keySlot || !IsPlausiblePtr(keySlot) || key == 0)
        return ce_OrigInput(t, edxUnused, keySlot, key);
    /* 注意: 组字键同样会到这里 (ckey 实证: 拼音每个字母都进来;
     * 原生凭 IMEBuffer 标志位跳过它们, 我们之前照打, 英文残渣随消息
     * 发出就是这么来的)。组字中 ASCII (除空格) 一律吞掉 (IME 已收编,
     * 黄字照显); CJK (提交字) 永远收编。回车不吞 (吞回车会导致组字
     * 永不清、之后所有字母全被吞的死锁; 回车即发送, 与原生一致)。 */
    ukey = (unsigned)key & 0xFFFFu;
    if (CE_CompActiveNow())
    {
        if (ukey >= 0x20u && ukey < 0x80u && ukey != 0x20u)
        { *keySlot = 0; CE_BlinkReset(); return 0; }   /* 组字键吞掉, 不照打 */
    }
    if (ukey == 0x0Du)
    {
        /* 回车: 先剥未动预填 (空=原生取消, 非空=原生提交, 发包同手打) */
        CE_StripPrefix();
        /* 发送指纹存档 (单机回声跟玩家色用, 见上) */
        if (ce_Len > 0) CE_NoteSent();
        return ce_OrigInput(t, edxUnused, keySlot, key);
    }
    if (ukey == 0x1Bu)
        return ce_OrigInput(t, edxUnused, keySlot, key);   /* Esc 原生关闭 */
    if (ukey == 0x08u)
    {
        /* 退格: LL 钩优先 (同按键先跑), 这里只做后备 (150ms 握手防双删);
         * 对话框上游直改原生时 0x08 到不了这里, 靠 LL/轮询。 */
        if ((int)(GetTickCount() - ce_NavTick) > 150)
        {
            CE_DoBackspace();
            CE_Mirror();
        }
        ce_NavTick = GetTickCount();   /* 删没删都握手: 轮询不再补刀 */
        CE_BlinkReset();
        *keySlot = 0;
        return 0;
    }
    if (ukey == 0x7Fu)
    {
        /* Delete: 同上握手 (LL/轮询优先), 只做后备, 不双删 */
        if ((int)(GetTickCount() - ce_NavTick) > 150)
        {
            CE_DoDelete();
            CE_Mirror();
        }
        ce_NavTick = GetTickCount();   /* 删没删都握手: 轮询不再补刀 */
        CE_BlinkReset();
        *keySlot = 0;
        return 0;
    }
    ctrl = CE_KeyDown(VK_CONTROL);
    if (ctrl)
    {
        wchar_t c = (wchar_t)ukey;
        int fieldW = CE_NativeFieldW();
        if (fieldW < 80) fieldW = 640;
        if (c == L'C' || c == L'c' || ukey == 0x03u) CE_DoCopy(0);
        else if (c == L'X' || c == L'x' || ukey == 0x18u) CE_DoCopy(1);
        else if (c == L'V' || c == L'v' || ukey == 0x16u) CE_DoPaste(fieldW);
        else if (c == L'A' || c == L'a' || ukey == 0x01u)
        { ce_Anchor = 0; ce_Cur = ce_Len; CE_BlinkReset(); }
        else if (ukey < 0x20u) { *keySlot = 0; return 0; }   /* 原生拒收 */
        else CE_InsertOne(c, fieldW);   /* 与原生一致: 照打 */
        if (c == L'V' || c == L'v' || ukey == 0x16u) { /* paste 内已 mirror */ }
        else CE_Mirror();
        ce_CmdTick = GetTickCount();
        *keySlot = 0;
        return 0;
    }
    if (ukey >= 0x20u && ukey < 0x10000u)
    {
        int fieldW = CE_NativeFieldW();
        if (fieldW < 80) fieldW = 640;
        CE_InsertOne((wchar_t)ukey, fieldW);
        CE_Mirror();
        CE_BlinkReset();
        *keySlot = 0;
        return 0;
    }
    return ce_OrigInput(t, edxUnused, keySlot, key);
}

static const unsigned char CE_WANT_INPUT[5] = { 0x53, 0x8B, 0x5C, 0x24, 0x0C };

static void* CE_TrampAt(unsigned at, int n, unsigned back)
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

void ChatEdit_Install(void)
{
    unsigned char* p = (unsigned char*)CE_INPUT;
    unsigned target;
    int rel, i;
    DWORD old;
    CE_EnsureInit();
    for (i = 0; i < 5; i++)
        if (p[i] != CE_WANT_INPUT[i])
        { LogLine("chatedit: input bytes mismatch, hook OFF"); return; }
    ce_OrigInput = (CE_InputFn)CE_TrampAt(CE_INPUT, 5, CE_INPUT_BACK);
    if (!ce_OrigInput) { LogLine("chatedit: tramp fail, hook OFF"); return; }
    target = (unsigned)(size_t)ChatEdit_InputHook;
    rel = (int)(target - (CE_INPUT + 5));
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return;
    p[0] = 0xE9;
    *(int*)(p + 1) = rel;
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    LogLine("chatedit: input hook ON");
}

/* ---------------- 原生编辑行之后绘制 (0x5D4A8D, 5B) ----------------
 * Draw 的编辑块之后、消息列表之前 (mov ecx,[esi] + pop esi + test, 无 rel):
 * 我方激活即在原生行之上画不透明盒子 (原生照画, 被盖住), 只加绘制、
 * 不改原生一字节控制流 (SkipNative 跳过方案导致二次回车崩溃, 已撤)。
 * 未激活直接返回 0 跑原指令。
 * 钩子经 dist/MOExperience.dll.inj: 0x5D4A8D = ChatEdit_DrawTail, 5。 */
extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_DrawTail(void* regs)
{
    void* surf;
    int surfW, surfH, surfPitch, bitLineH;
    (void)regs;
    CE_EnsureInit();
    if (!ce_Enable || !ce_Active) return 0;
    surf = *(void**)CE_COMPOSITE;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    surfW = CE_SurfGet(surf, CE_VT_GET_WIDTH);
    surfH = CE_SurfGet(surf, CE_VT_GET_HEIGHT);
    surfPitch = CE_SurfGet(surf, CE_VT_GET_PITCH);
    if (surfW <= 0 || surfH <= 0 || surfPitch <= 0) return 0;
    bitLineH = CE_LivePx();
    CE_Render(surf, bitLineH, surfW, surfH, surfPitch);
    return 0;
}

/* ---------------- 帧入口 ---------------- */

int ChatEdit_Frame(void* surf, int bitLineH, int screenW,
                   int surfW, int surfH, int surfPitch)
{
    int isEdit, fieldW;
    (void)screenW;
    CE_EnsureInit();
    CE_PumpEnsure();
    if (!ce_Enable)
    {
        if (ce_Active) CE_Deactivate();
        return 0;
    }
    if (!surf || surfW <= 0 || surfH <= 0 || surfPitch <= 0) return 0;
    isEdit = CE_NativeIsEdit();
    if (isEdit && !ce_Active) CE_Activate();
    else if (!isEdit && ce_Active) CE_Deactivate();
    if (!ce_Active) return 0;
    CE_PollIME();
    if (ce_CompLen <= 0)
    {
        fieldW = CE_NativeFieldW();
        if (fieldW < 80 || fieldW > surfW) fieldW = 640;
        CE_PollKeys(fieldW);
    }
    /* 逐帧指纹: 单机发送不走 Input 回车分支 (对话框直接发),
     * 回车点存档会 miss; 缓冲非空就每帧刷新, 回声何时到都命中。 */
    if (ce_Len > 0) CE_NoteSent();
    /* 每帧权威回写: 反汇编实证 Input 只处理 0x8/0xD/0x1B,
     * VK_DELETE 由对话框层直接改原生缓冲 (不走 Input 钩):
     * 不修的话"显示没删、发送少字"。我方缓冲为准 (原生只在发送瞬间读),
     * 帧级覆盖后旁路修改永远到不了发送; 我方轮询 Delete 照常生效。 */
    CE_Mirror();
    (void)surf; (void)bitLineH; (void)screenW;
    (void)surfW; (void)surfH; (void)surfPitch;
    return 1;   /* 激活中 (绘制在 DrawTail 做, 与原生行叠序正确) */
}
