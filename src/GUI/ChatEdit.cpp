/* ChatEdit.cpp - 自绘聊天输入框 (见 ChatEdit.h)。
 * 原生 MessageListClass::Input 显示差、无光标移动/选中/粘贴, 且单机不建框。
 *
 * 做法 (自绘 + GDI, 只读查询 + 渲染面绘制, 联机安全):
 *   聊天框: 对话框四处 session 检查各下一钩, 跳过单人判定 (Phobos
 *     AllowChatBoxInSinglePlayer 同原理, 见 notes/ChatBox参考.md)。
 *   输入: 运行时补丁 MessageListClass::Input (0x5D4510, 5B, 跳回 0x5D4515),
 *     同签名 __fastcall 替换; 字符/退格/回车/Esc/Ctrl 自己处理后消费
 *     (*keySlot=0, return 0), 其余走跳板跑原生。英文走 WM_CHAR, 由对话框层
 *     直接写行缓冲, 另有 ChatEdit_CharPost 摘除钩收编 (两钩互补才是全量)。
 *     导航键 (箭头/Home/End/Delete) 到不了 Input, 每帧 GetAsyncKeyState 轮询
 *     (边缘立刻一步, 按住 400ms 后 33ms 连发), 只在游戏主窗当前台时动手,
 *     组字中放行给 IME。不装全局键盘钩 (会卡住后台程序取键)。
 *   历史: 回车/提交时记行 (Esc 取消不记, 相邻重复不记, 32 行, 纯输入框本地);
 *     开框时上下键翻 (Up 更旧/Down 更新, 到底恢复翻前行), 改字即离翻。
 *   文本全程镜像进原生 EditBuffer, 提交/取消走原生 Input, 发包与手打一致,
 *     不碰模拟/网络函数 (单机+联机同路)。
 *   显示: 原生编辑行由 ChatEdit_HideNative 隐藏 (0x5D49B4, 3B, 永不返回 0,
 *     两向显式落点, 详见该函数注释); 自绘盒子画在 MessageDrawHook 尾部,
 *     压住战役框。底部模式贴战役框下沿, 顶部模式留原生行原位;
 *     底板只包画出内容, 字体跟战役框 (MsgText_CampStyle)。
 *   粘贴 CF_UNICODETEXT, 复制/剪切写剪贴板; 组字串自画黄色+下划线。
 *   输入法: 组合窗 (隐藏) 指盒底上一行做堆叠锚点 + 候选窗 CANDIDATEPOS 指盒底
 *     (surface→客户区换算); 系统组合窗由主窗子类化隐藏 (WM_IME_SETCONTEXT 清位
 *     + 吃掉 STARTCOMPOSITION), 候选条仍由系统画。
 *   代价: 打字时方向键镜头可能动一下 (不再吞键)。
 * 构建见 build-fast.bat (32 位 mingw, 无 CRT, -limm32)。
 */

#include <windows.h>
#include <imm.h>
#include "GdiText.h"
#include "MsgText.h"
#include "ChatEdit.h"

#define CE_MSGLIST      0xA8BC60u
#define CE_OFF_ISEDIT   0x19u
#define CE_OFF_EDITBUF  0x28u
#define CE_OFF_EDITLABEL 0x24u
#define CE_OFF_EDITPOS  0x1Cu
#define CE_OFF_CURPOS   0x2B0u   /* EditCurrentPos  */
#define CE_OFF_INITPOS  0x2B4u   /* EditInitialPos  */
#define CE_OFF_FIELDW   0x2C4u
#define CE_BUF_N        162

#define CE_INPUT        0x005D4510u
#define CE_INPUT_BACK   0x005D4515u

/* MessageListClass::Draw: 原生编辑行绘制段的落点, 也是引擎自己
 * "不在编辑" 分支的落点 (跳过该段栈零失衡, 见 ChatEdit_HideNative)。 */
#define CE_NATIVE_SKIP  0x005D4A8Du
#define CE_NATIVE_ENTER 0x005D49BFu   /* 紧接被覆盖的 3 字节之后, 引擎原生进入点 */
/* 越过引擎的"无文本就不画"判据, 落点=判据之后第一条指令。 */

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

#define CE_BITFONT      0x0089C4D0u
#define CE_OFF_FONT_H   0x1Cu

#define CE_GDI_SLOT     4
#define CE_VT_LOCK      0x5Cu
#define CE_VT_UNLOCK    0x60u
#define CE_VT_FILLRECT  0x1Cu

/* Syringe 传进来的寄存器块 (同 MsgText.cpp 的用法) */
#define CE_OFF_REG_ECX  0x20u

/* 文字样式跟战役框 (同 [MsgText] FontFace/FontHeight/Bold, 经 MsgText_CampStyle
 * 取已解析结果; 直接把 "msyh.ttc" 当字体名传 GDI 会回退到无 CJK 字形, 中文空白)。
 * ce_Px=0 表示跟游戏点阵字高。配置只读一次, 存一份给按键级度量用。 */
static const wchar_t* ce_Face = L"Microsoft YaHei";
static int ce_Px = 0;
static int ce_Bold = 0;

/* ---------------- 状态 ---------------- */

static int ce_Init = 0;
static int ce_Enable = 1;
static int ce_ChatBoxSP = 1;   /* 单人也建原生聊天框 */
static int ce_ChatNameSP = 0;  /* 单人消息也显示 "名字: " */
static int ce_Active = 0;
/* 本帧盒子是用战役框锚点 (0) 还是留在原生行原位 (1)。留在原位时不允许
 * 钳屏内位移, 否则和原生行分家, 合并黑板失效。 */
static int ce_AtNative = 1;

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
static int ce_CompH = 0;      /* IME 组合行高缓存 (>0=已测到, 开框清零) */

/* 输入历史: 发过的行 (回车/提交时记, Esc 取消不记, 相邻重复不记)。
 * ce_HistIdx<0=不在翻, 否则 0=最新。起翻先 stash 当前行, Down 到底恢复它。 */
#define CE_HIST_N 32
static wchar_t ce_Hist[CE_HIST_N][CE_BUF_N];
static int ce_HistLen[CE_HIST_N];
static int ce_HistCount = 0;
static int ce_HistIdx = -1;
static wchar_t ce_HistStash[CE_BUF_N];
static int ce_HistStashLen = 0;
static int ce_Esc = 0;   /* 本次开框按过 Esc (取消语义, 关框不记历史) */

static unsigned ce_CmdTick = 0; /* Ctrl 组合键上次处理时刻 (Input 钩/轮询握手) */
static unsigned ce_NavTick = 0; /* 退格/Delete 上次处理时刻 (Input 钩/轮询握手) */
/* WM_CHAR 摘除钩 (定义在后, 开框时懒安装): 行缓冲游标快照 + 安装前置声明 */
static unsigned ce_LastCur = 0xFFFFFFFFu;
static void CE_CharPostInstall(void);
static HWND ce_FgWnd = NULL;

typedef struct { int vk; int prev; unsigned nextRep; } CENavKey;
static CENavKey ce_NavKeys[] = {
    { VK_LEFT, 0, 0 }, { VK_RIGHT, 0, 0 },
    { VK_HOME, 0, 0 }, { VK_END, 0, 0 }, { VK_DELETE, 0, 0 },
    { VK_UP, 0, 0 }, { VK_DOWN, 0, 0 },   /* 上下翻输入历史 */
    { VK_BACK, 0, 0 },   /* 退格: 对话框上游直改原生时 Input 钩收不到, 轮询后备 */
};
static int ce_CtrlPrev[4] = { 0, 0, 0, 0 };   /* C/X/V/A 边沿 */

/* ---------------- 小工具 (无 CRT) ---------------- */

static int CE_KeyDown(unsigned vk)
{
    return (GetAsyncKeyState((int)vk) & 0x8000) ? 1 : 0;
}

/* 区间可读判定 (IsPlausiblePtr 只判单指针 4 对齐, 不能直接判区间尾地址)。
 * 只看起点对齐 + 末字节合法。 */
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
    if (ce_Init) return;
    ce_Init = 1;
    BuildPaths();
    GtEnsureLog();
    ce_Enable = GetPrivateProfileIntA(MSG_SECTION, "ChatEdit", 1, g_IniPath) ? 1 : 0;
    ce_ChatBoxSP = GetPrivateProfileIntA(MSG_SECTION, "ChatBoxSP", 1, g_IniPath) ? 1 : 0;
    ce_ChatNameSP = GetPrivateProfileIntA(MSG_SECTION, "ChatNameSP", 0, g_IniPath) ? 1 : 0;
    MsgText_CampStyle(&ce_Face, &ce_Px, &ce_Bold);
    {
        char sb[128], t1[16];
        unsigned q = 0;
        q = AppendCStr(sb, q, "chatedit: ", sizeof(sb));
        q = AppendCStr(sb, q, ce_Enable ? "box ON" : "box OFF", sizeof(sb));
        q = AppendCStr(sb, q, " ChatBoxSP=", sizeof(sb));
        UtoA((unsigned)ce_ChatBoxSP, t1);
        q = AppendCStr(sb, q, t1, sizeof(sb));
        q = AppendCStr(sb, q, " ChatNameSP=", sizeof(sb));
        UtoA((unsigned)ce_ChatNameSP, t1);
        AppendCStr(sb, q, t1, sizeof(sb));
        LogLine(sb);
    }
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

/* 输入域宽 (原生值, 过窄回退 640)。 */
static int CE_FieldW(void)
{
    int w = CE_NativeFieldW();
    return (w < 80) ? 640 : w;
}

static void CE_SelRange(int* lo, int* hi)
{
    if (ce_Anchor < 0 || ce_Anchor == ce_Cur) { *lo = *hi = -1; return; }
    *lo = (ce_Anchor < ce_Cur) ? ce_Anchor : ce_Cur;
    *hi = (ce_Anchor < ce_Cur) ? ce_Cur : ce_Anchor;
}

/* GDI 自然宽度 (ABC 全宽, 无 DigitTrack)。须先 GdiEnsure 成功。 */
static int CE_TextWidth(const wchar_t* s, int n)
{
    int tot = 0, i;
    if (!g_G || !g_G->dc || !s || n <= 0) return 0;
    for (i = 0; i < n; i++)
    {
        int ga = GtCharAdv(s[i]);
        if (ga < 0) return -1;
        tot += ga;
    }
    return tot;
}

/* 文本镜像进原生缓冲 (提交/原生路径永远看到有效文本)。
 * 光标置尾, 起点置 0 (空=取消, 非空=发送, 与原生语义一致)。
 * 对话框层也会直接改这块 (Delete/方向键不走 Input 钩), 帧级覆盖回去。 */
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

/* 有效字高: 战役框 FontHeight>0 用它, 否则跟游戏点阵字高 (与战役框同口径)。 */
static int CE_FontPx(void)
{
    return ce_Px ? ce_Px : CE_LivePx();
}

/* 度量前切到输入槽 (Input 上下文里 g_G 可能指向别的槽, 用错槽算错预算) */
static int CE_EnsureFont(void)
{
    return GdiEnsure(CE_FontPx(), ce_Bold, CE_GDI_SLOT, ce_Face);
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
    ce_HistIdx = -1;   /* 改字即离翻历史 */
    if (lo < 0 || hi <= lo) return;
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
    ce_HistIdx = -1;   /* 改字即离翻历史 */
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

/* ---------------- 输入历史 (上下键翻, 只挂输入框, 与联机无关) ----------------
 * 记: 回车分支记 + 关框补记 (单机对话框提交不走 Input, Esc 取消不记)。
 * 两处双记由"相邻重复不记"合并。翻: Up=更旧, Down=更新, 到底恢复翻前行。
 * 改字 (Insert/Delete) 即离翻; 光标键移动不离翻, Up 从当前位继续。 */

static void CE_HistPush(void)
{
    int i, k;
    if (ce_Len <= 0 || ce_Len >= CE_BUF_N) return;
    if (ce_HistCount > 0 && ce_HistLen[0] == ce_Len)
    {
        for (k = 0; k < ce_Len; k++)
            if (ce_Hist[0][k] != ce_Buf[k]) break;
        if (k >= ce_Len) return;   /* 相邻重复不记 (兼并回车+关框双记) */
    }
    if (ce_HistCount < CE_HIST_N) ce_HistCount++;
    for (i = ce_HistCount - 1; i > 0; i--)
    {
        int l = ce_HistLen[i - 1];
        ce_HistLen[i] = l;
        for (k = 0; k <= l; k++) ce_Hist[i][k] = ce_Hist[i - 1][k];
    }
    ce_HistLen[0] = ce_Len;
    for (k = 0; k <= ce_Len; k++) ce_Hist[0][k] = ce_Buf[k];
}

static void CE_HistApply(int idx)
{
    int k;
    for (k = 0; k <= ce_HistLen[idx]; k++) ce_Buf[k] = ce_Hist[idx][k];
    ce_Len = ce_HistLen[idx];
    ce_Cur = ce_Len;
    ce_Anchor = -1;
    ce_ViewX = 0;   /* Render 按光标重滚 */
    CE_Mirror();
    CE_BlinkReset();
}

static void CE_HistMove(int dir)
{
    int k;
    if (dir < 0)   /* Up: 更旧 */
    {
        if (ce_HistCount <= 0) return;
        if (ce_HistIdx < 0)
        {
            int n = (ce_Len < CE_BUF_N) ? ce_Len : CE_BUF_N - 1;
            for (k = 0; k < n; k++) ce_HistStash[k] = ce_Buf[k];
            ce_HistStash[n] = 0;
            ce_HistStashLen = n;
            ce_HistIdx = 0;
        }
        else if (ce_HistIdx < ce_HistCount - 1) ce_HistIdx++;
        else return;   /* 已到最旧 */
        CE_HistApply(ce_HistIdx);
    }
    else   /* Down: 更新, 到底恢复翻前行 */
    {
        if (ce_HistIdx < 0) return;
        if (ce_HistIdx > 0) { ce_HistIdx--; CE_HistApply(ce_HistIdx); }
        else
        {
            for (k = 0; k <= ce_HistStashLen; k++) ce_Buf[k] = ce_HistStash[k];
            ce_Len = ce_HistStashLen;
            ce_Cur = ce_Len;
            ce_Anchor = -1;
            ce_ViewX = 0;
            ce_HistIdx = -1;
            CE_Mirror();
            CE_BlinkReset();
        }
    }
}

/* ---------------- 组字轮询 (Imm, 无钩) ---------------- */

/* 本帧 IME 上下文: PollIME 取得, PositionIME 复用, 帧末释放。 */
static HIMC ce_FrameImc = NULL;
static HWND  ce_FrameImcWnd = NULL;

static void CE_ReleaseFrameImc(void)
{
    if (ce_FrameImc)
    {
        ImmReleaseContext(ce_FrameImcWnd, ce_FrameImc);
        ce_FrameImc = NULL;
        ce_FrameImcWnd = NULL;
    }
}

static void CE_PollIME(void)
{
    HWND fg;
    HIMC himc;
    LONG r;
    ce_CompLen = 0;
    ce_CompCur = 0;
    CE_ReleaseFrameImc();
    fg = GetForegroundWindow();
    if (fg && fg == ce_FgWnd)
    {
        himc = ImmGetContext(fg);
        if (himc)
        {
            ce_FrameImc = himc;
            ce_FrameImcWnd = fg;
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
        }
    }
}

/* ---------------- IME 组合/候选窗 ----------------
 * 组合窗隐藏, 指盒底上一行做堆叠锚点; 候选窗指盒底。系统组合窗由主窗子类化隐藏。 */

#define CE_GAME_HWND_ADDR 0x00B73550u

/* 游戏主窗句柄 (引擎存的 0xB73550, 同 FullMap.cpp) */
static HWND CE_GameHwnd(void)
{
    if (IsPlausiblePtr((void*)CE_GAME_HWND_ADDR))
    {
        HWND hg = *(HWND*)CE_GAME_HWND_ADDR;
        if (hg && IsWindow(hg)) return hg;
    }
    return NULL;
}

/* 系统组合窗隐藏 (主窗子类化): 自画组字串, 候选与输入原样转交。 */

static WNDPROC ce_OrigWndProc = NULL;

static LRESULT WINAPI CE_WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC orig = ce_OrigWndProc;
    if (m == WM_IME_SETCONTEXT)
        l &= (LPARAM)(~(DWORD)ISC_SHOWUICOMPOSITIONWINDOW);
    if (m == WM_IME_STARTCOMPOSITION)
    {
        /* 自画组字串, 不开系统组合窗。 */
        (void)w; (void)l;
        return 0;
    }
    if (IsWindowUnicode(h))
        return orig ? CallWindowProcW(orig, h, m, w, l)
                    : DefWindowProcW(h, m, w, l);
    return orig ? CallWindowProcA(orig, h, m, w, l)
                : DefWindowProcA(h, m, w, l);
}

static void CE_SubclassEnsure(HWND h)
{
    WNDPROC cur, want;
    int uni;
    if (!h || !IsWindow(h)) return;
    want = (WNDPROC)CE_WndProc;
    uni = IsWindowUnicode(h) ? 1 : 0;
    cur = uni ? (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC)
              : (WNDPROC)GetWindowLongPtrA(h, GWLP_WNDPROC);
    if (cur == want) return;
    if (!ce_OrigWndProc) ce_OrigWndProc = cur;
    else if (cur != ce_OrigWndProc) return;
    if (uni) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)want);
    else SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)want);
}

/* IME 组合行高: 读 IME 组合字体实测, 失败回退换算, 均缓存。 */
static int CE_CompLineH(HIMC himc, int lineH, int ch, int surfH)
{
    LOGFONTW lf;
    HDC hdc;
    HFONT hf, ho;
    TEXTMETRICW tm;
    int h = 0, got = 0;
    /* 回退值也缓存 (常态无组字, 否则每帧重建 GDI 字体)。变量名须避开 Windows 宏。 */
    static int s_clh = -1, s_cch = -1, s_csh = -1, s_cval = 0;
    if (ce_CompH > 0) return ce_CompH;
    if (s_clh == lineH && s_cch == ch && s_csh == surfH) return s_cval;
    if (himc && ImmGetCompositionFontW(himc, &lf))
    {
        hdc = GetDC(NULL);
        if (hdc)
        {
            hf = CreateFontIndirectW(&lf);
            if (hf)
            {
                ho = (HFONT)SelectObject(hdc, hf);
                if (GetTextMetricsW(hdc, &tm) && tm.tmHeight > 0 && tm.tmHeight < 256)
                { h = tm.tmHeight; got = 1; }
                SelectObject(hdc, ho);
                DeleteObject(hf);
            }
            ReleaseDC(NULL, hdc);
        }
    }
    if (!got && lineH > 0 && ch > 0 && surfH > 0)
        h = lineH * ch / surfH;
    if (h < 8) h = 8;
    if (h > 128) h = 128;
    if (got) ce_CompH = h;
    else { s_clh = lineH; s_cch = ch; s_csh = surfH; s_cval = h; }
    return h;
}

/* IME 定位: 组合窗指盒底上一行, 候选窗指盒底。 */
static void CE_PositionIME(int imeX, int boxY, int bh,
                           int lineH, int surfW, int surfH)
{
    /* 位置不变则跳过: IMM 调用贵, 光标/盒大多时间不动。 */
    static int s_cx = -1, s_cy = -1, s_cyc = -1;
    HWND h = CE_GameHwnd();
    RECT rc;
    COMPOSITIONFORM cfc = { 0 };
    CANDIDATEFORM cdc = { 0 };
    HIMC himc;
    int own;
    int cw, ch, cx, cy, cyc;
    if (!h || !IsWindow(h)) return;
    if (GetForegroundWindow() != h) return;
    if (!GetClientRect(h, &rc)) return;
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw < 100 || ch < 100) return;
    cx = imeX * cw / surfW;
    cy = (boxY + bh) * ch / surfH;
    if (cx > cw - 40) cx = cw - 40;
    if (cy > ch - 40) cy = ch - 40;
    /* 复用本帧已取的上下文; 候选窗位置依赖真行高, 不用回退值。 */
    himc = (ce_FrameImc && ce_FrameImcWnd == h) ? ce_FrameImc : NULL;
    own = 0;
    if (!himc)
    {
        CE_ReleaseFrameImc();
        himc = ImmGetContext(h);
        own = 1;
        if (!himc) return;
    }
    cyc = cy - CE_CompLineH(himc, lineH, ch, surfH);
    if (cyc < 0) cyc = 0;
    if (cx == s_cx && cy == s_cy && cyc == s_cyc)
    {
        if (own) ImmReleaseContext(h, himc);
        return;
    }
    s_cx = cx; s_cy = cy; s_cyc = cyc;
    CE_SubclassEnsure(h);
    cfc.dwStyle = CFS_FORCE_POSITION;
    cfc.ptCurrentPos.x = cx;
    cfc.ptCurrentPos.y = cyc;
    ImmSetCompositionWindow(himc, &cfc);
    cdc.dwStyle = CFS_CANDIDATEPOS;
    cdc.ptCurrentPos.x = cx;
    cdc.ptCurrentPos.y = cy;
    ImmSetCandidateWindow(himc, &cdc);
    if (own) ImmReleaseContext(h, himc);
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
    if (shift && ce_Anchor == ce_Cur) ce_Anchor = -1;
    CE_BlinkReset();
}

/* 游戏主窗是否就是当前前台窗口。轮询与泵都只在这里为真时动手 ——
 * GetAsyncKeyState 是全局按键状态, 用户切到别的程序删字时它照样为 1。 */
static int CE_GameForeground(void)
{
    if (!ce_FgWnd) return 0;
    return (GetForegroundWindow() == ce_FgWnd);
}

/* 轮询按帧处理移动/删除 (唯一的按键来源) */
static void CE_ApplyNav(int vk)
{
    int shift = CE_KeyDown(VK_SHIFT);
    int ctrl = CE_KeyDown(VK_CONTROL);
    if (vk == VK_DELETE) { CE_DoDelete(); CE_Mirror(); }
    else if (vk == VK_BACK) { CE_DoBackspace(); CE_Mirror(); }
    else if (vk == VK_UP) CE_HistMove(-1);
    else if (vk == VK_DOWN) CE_HistMove(1);
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
        /* 边缘立刻走一步, 按住 400ms 后起 33ms 连发; 只读按键状态, 不装全局钩。 */
        if (down && !ce_NavKeys[i].prev)
        {
            CE_ApplyNav(ce_NavKeys[i].vk);
            ce_NavKeys[i].nextRep = now + 400;
        }
        else if (down && (int)(now - ce_NavKeys[i].nextRep) >= 0)
        {
            CE_ApplyNav(ce_NavKeys[i].vk);
            ce_NavKeys[i].nextRep = now + 33;
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

/* 泵线程: 框开着时 ~60Hz 投 WM_NULL 保主循环不断流 (方向键吃掉后无消息即饿死);
 * 只在游戏主窗当前台时投递。 */

static DWORD WINAPI CE_Watchdog(LPVOID unused)
{
    (void)unused;
    for (;;)
    {
        if (ce_Active && CE_GameForeground())
            PostMessageA(ce_FgWnd, WM_NULL, 0, 0);
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

/* ---------------- 激活/关闭 ---------------- */

static void CE_Activate(void)
{
    wchar_t* nb = (wchar_t*)(CE_MSGLIST + CE_OFF_EDITBUF);
    int n = 0;
    unsigned k;
    ce_CompLen = 0; ce_CompCur = 0; ce_CompH = 0; ce_HistIdx = -1; ce_Esc = 0;
    /* 同步原生缓冲: 只认领, 不显示。AddEdit 预填的发送模板原样未动则发送时剥离,
     * 自己和原生缓冲都归零 (镜像覆盖模板)。 */
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
    CE_BlinkReset();
}

static void CE_Deactivate(void)
{
    if (ce_Len > 0 && !ce_Esc) CE_HistPush();   /* 关框补记 (单机对话框提交不走 Input; 回车已记过, 相邻去重合并) */
    ce_Esc = 0;
    ce_Active = 0;
    ce_Anchor = -1;
    ce_CompLen = 0;
}

/* ---------------- 绘制 ---------------- */

typedef void* (__fastcall *CE_LockFn)(void*, void*, int, int);
typedef int (__fastcall *CE_UnlockFn)(void*, void*);
typedef int (__fastcall *CE_FillFn)(void*, void*, RectangleStruct*,
                                    ColorStruct*, int);

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
    int compX, imeX, compWsum, compFit, th, boxY;
    void* label;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    (void)bitLineH;
    lineH = CE_FontPx();
    if (lineH < 8 || lineH > 64) lineH = 16;
    fieldW = CE_NativeFieldW();
    if (fieldW < 80 || fieldW > surfW) fieldW = (surfW - 16 > 80) ? surfW - 16 : 640;
    /* 落位: 底部模式贴战役框下沿, 顶部模式贴战役框顶边 (MsgText_ChatAnchor);
     * 兜底: 对话框 EditPos (+0x1C) → label 自身 X/Y → 底部条带。 */
    lx = 8; ly = surfH - lineH - 12;
    {
        int ax = 0, ay = 0;
        ce_AtNative = 1;
        if (MsgText_ChatAnchor(surfW, surfH, lineH, &ax, &ay))
        {
            lx = ax; ly = ay;
            ce_AtNative = 0;
        }
        else
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
    }
    if (!GdiEnsure(lineH, ce_Bold, CE_GDI_SLOT, ce_Face)) return 0;
    /* 逐字 x 偏移 (与 RenderLine 同口径)。 */
    xs[0] = 0;
    for (i = 0; i < ce_Len; i++)
    {
        int ga = GtCharAdv(ce_Buf[i]);
        if (ga < 0) ga = lineH;
        xs[i + 1] = xs[i] + ga;
    }
    /* 滚动保证光标可见 */
    curX = xs[ce_Cur];
    if (curX - ce_ViewX > fieldW - 8) ce_ViewX = curX - fieldW + 24;
    if (curX < ce_ViewX) ce_ViewX = curX;
    if (ce_ViewX < 0) ce_ViewX = 0;
    startIdx = 0;
    while (startIdx < ce_Len && xs[startIdx + 1] <= ce_ViewX) startIdx++;
    /* 可见窗口三重截断: 像素(fieldW/1000 滚窗) + DIB 容量 (RenderLine 单次上限
     * DIB_W-8)。盒只包画出部分, 不留黑尾。 */
    endIdx = startIdx;
    while (endIdx < ce_Len &&
           xs[endIdx + 1] - xs[startIdx] <= fieldW &&
           xs[endIdx + 1] - xs[startIdx] <= 1000) endIdx++;
    {
        int fit = CharsFit(ce_Buf + startIdx, endIdx - startIdx, DIB_W - 8);
        if (fit < endIdx - startIdx) endIdx = startIdx + fit;
    }
    visLen = endIdx - startIdx;
    /* 组字宽 (盒跟宽用; 同样按 DIB 容量截, 与画出的一致) */
    compWsum = 0;
    compFit = 0;
    if (ce_CompLen > 0)
    {
        compFit = CharsFit(ce_Comp, ce_CompLen, DIB_W - 8);
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
    /* 钳屏内只在战役框锚点下做; 留原生位时不动 (否则和原生行分家)。 */
    if (!ce_AtNative)
    {
        if (tx + bw + 4 > surfW) tx = surfW - bw - 4;
        if (tx < 0) tx = 0;
        if (boxY + bh + 4 > surfH) boxY = surfH - bh - 4;
        if (boxY < 0) boxY = 0;
    }
    /* 墨水在盒内垂直居中。 */
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

    /* 底板只按画出内容宽 (原生输入域宽=整屏宽, 铺满即顶上黑杠; 原生行已隐藏)。 */
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
    /* 组字串 (黄色+下划线, 光标跟组字光标; 宽钳到盒内).
     * 候选框锚点另算 (imeX = 插入点, 见下), 不跟组字串走。 */
    compX = dx + (xs[ce_Cur] - xs[startIdx]);
    imeX = compX;
    if (ce_CompLen > 0 && compFit > 0)
    {
        int w = RenderLine(ce_Comp, compFit);
        if (w > 0)
        {
            int k;
            int acc = 0;
            int showW = w + 2;
            int maxW = tx + bw - 2 - compX;
            if (showW > maxW) showW = maxW;
            for (k = 0; k < ce_CompCur && k < compFit; k++)
            {
                int ga = GtCharAdv(ce_Comp[k]);
                if (ga < 0) ga = lineH;
                acc += ga;
            }
            if (showW > 0)
            {
                BlitCoverage(locked, surfPitch, surfW, surfH, compX, ty, showW, th,
                             255, 255, 0, 100);
                CE_FillRect(surf, compX, ty + th - 1, showW, 1, 255, 255, 0, 100);
            }
            compX += acc;
        }
    }
    /* 光标 (有选中则隐藏, 530ms 闪烁; 钳到盒内) */
    if (compX < tx) compX = tx;
    if (compX > tx + bw - 2) compX = tx + bw - 2;
    if (imeX < tx) imeX = tx;
    if (imeX > tx + bw - 2) imeX = tx + bw - 2;
    if (lo < 0 && ((GetTickCount() - ce_BlinkT) / 530u) % 2u == 0u)
        CE_FillRect(surf, compX, ty, 2, th, 255, 255, 255, 100);

    unlockFn(surf, NULL);
    /* 候选指盒底, 每帧设一次。 */
    CE_PositionIME(imeX, boxY, bh, lineH, surfW, surfH);
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

/* 组字中? 钩子内现查。组字中 ASCII 已被 IME 收编, 钩子照打会留英文残渣。 */
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

/* ---------------- WM_CHAR 摘除钩 (英文唯一入口) ----------------
 * WM_CHAR 不进 Input: 对话框层 0x54F790 的 WM_CHAR 分支先 0x54F200 写标志字
 * 0x000A, 再 0x54F1C0 写原文, 两格成对进环; Input 钩见不到英文, 不收编就是黑洞
 * (中文走 WM_IME_CHAR→队列→Input, 不经此处)。
 * 站点 0x54F8B4 (原生 call 之后 `jmp 0x54fb18`, 无他处跳入): 原生照写环,
 * 我方随后把刚 push 的两格摘走 (收编 + 游标回退 + 清格), 跳回原目标。
 * 首帧开框懒安装。裸函数 pusha/call C/popa/push imm32+ret 净零。 */
#define CE_CHARPOST_SITE 0x0054F8B4u
#define CE_CHARPOST_BACK 0x0054FB18u

static int ce_PostOn = 0;          /* 补丁已下 (只试一次) */

/* 非 static (裸汇编按 C 符号引用)。
 * this_ 即摘除点 ecx (0x54F1C0 不碰 ecx, 入口原样保留); 优先用它,
 * 不可信回退全局。行环是真队列 (Pop 推进 0x314), 判据按"该格是否刚 push",
 * 不能只看游标。 */
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
    if (w < 0x20 || w == 0x7F) return;  /* 控制符走原生 */
    fieldW = CE_FieldW();
    CE_InsertOne((wchar_t)w, fieldW);
    CE_Mirror();
    CE_BlinkReset();
    /* 摘除 (A)(B) 两格: 倒数第二格是本次 (A)(0x000A) 才退 2, 否则退 1。 */
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
    /* DLL 会被重定基址, 跳回游戏固定地址用基址无关写法 push imm32+ret。 */
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
    ce_PostOn = 1;   /* 只试一次 */
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
    /* 组字键同样会到这里; 组字中 ASCII (除空格) 吞掉 (IME 已收编, 黄字照显),
     * CJK 提交字照收; 回车不吞 (否则组字永不清, 后续全被吞死锁)。 */
    ukey = (unsigned)key & 0xFFFFu;
    if (CE_CompActiveNow())
    {
        if (ukey > 0x20u && ukey < 0x80u)
        { *keySlot = 0; CE_BlinkReset(); return 0; }   /* 组字键吞掉, 不照打 */
    }
    if (ukey == 0x0Du)
    {
        /* 回车: 先剥未动预填 (空=原生取消, 非空=原生提交, 发包同手打) */
        CE_StripPrefix();
        /* 发送指纹存档 (单机回声跟玩家色用, 见上) */
        if (ce_Len > 0) CE_NoteSent();
        CE_HistPush();   /* 记历史 (空行不记, 见内判) */
        return ce_OrigInput(t, edxUnused, keySlot, key);
    }
    if (ukey == 0x1Bu)
    { ce_Esc = 1; return ce_OrigInput(t, edxUnused, keySlot, key); }   /* Esc 原生关闭, 不记历史 */
    if (ukey == 0x08u)
    {
        /* 退格: 轮询优先, 这里只做后备 (150ms 握手防双删) */
        if ((int)(GetTickCount() - ce_NavTick) > 150)
        {
            CE_DoBackspace();
            CE_Mirror();
        }
        ce_NavTick = GetTickCount();
        CE_BlinkReset();
        *keySlot = 0;
        return 0;
    }
    if (ukey == 0x7Fu)
    {
        /* Delete: 同上, 只做后备 */
        if ((int)(GetTickCount() - ce_NavTick) > 150)
        {
            CE_DoDelete();
            CE_Mirror();
        }
        ce_NavTick = GetTickCount();
        CE_BlinkReset();
        *keySlot = 0;
        return 0;
    }
    ctrl = CE_KeyDown(VK_CONTROL);
    if (ctrl)
    {
        wchar_t c = (wchar_t)ukey;
        int fieldW = CE_FieldW();
        if (c == L'C' || c == L'c' || ukey == 0x03u) CE_DoCopy(0);
        else if (c == L'X' || c == L'x' || ukey == 0x18u) CE_DoCopy(1);
        else if (c == L'V' || c == L'v' || ukey == 0x16u) CE_DoPaste(fieldW);
        else if (c == L'A' || c == L'a' || ukey == 0x01u)
        { ce_Anchor = 0; ce_Cur = ce_Len; CE_BlinkReset(); }
        else if (ukey < 0x20u) { *keySlot = 0; return 0; }   /* 原生拒收 */
        else CE_InsertOne(c, fieldW);   /* 与原生一致: 照打 */
        if (c != L'V' && c != L'v' && ukey != 0x16u) CE_Mirror();
        ce_CmdTick = GetTickCount();
        *keySlot = 0;
        return 0;
    }
    if (ukey >= 0x20u && ukey < 0x10000u)
    {
        int fieldW = CE_FieldW();
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
    /* 非 gamemd 宿主由 DllMain 宿主守卫拦截 (见 MOExperience.cpp),
     * 到这里必是游戏本体, 0x5D4510 恒在镜像内, 直接版本校验即可。 */
    /* [MsgText] ChatEdit=0 = 全走原生: Input 补丁永久有效 (无还原路径),
     * 装了就等于接管, 必须提前返回。 */
    if (!ce_Enable)
    {
        LogLine("chatedit: OFF, input hook not installed");
        return;
    }
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


/* ---------------- 原生编辑行隐藏 (0x5D49B4, 3B) ----------------
 * MessageListClass::Draw (0x5D49A0): 0x5D49B4 取 [esi+0x19](IsEdit),
 * 0x5D49B9 为 0 则跳 0x5D4A8D (整段跳过), 否则 0x5D49BF 起画原生行。
 * 本钩覆盖 0x5D49B4..0x5D49B6, 落点只给两处引擎原生走法:
 *   跳过 → 0x5D4A8D (栈零失衡); 进入 → 0x5D49BF (紧接被覆盖字节之后)。
 * 永不返回 0: 3B 装不下 Syringe 5B 跳转, 回放落到跳转位移字节上歪跳,
 * 导致 ChatEdit=0 也看不到原生行。自己读 [this+0x19], 两向显式落点。
 * 勿碰 0x5D4A22 打印段: 单机 CursorCharacter==0, 引擎在 0x5D49FA 即跳走,
 * 那段上下文未初始化, 硬闯必崩。 */
extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_HideNative(void* regs)
{
    unsigned char* ecx;
    int isEdit;
    (void)regs;
    CE_EnsureInit();
    ecx = *(unsigned char**)((char*)regs + CE_OFF_REG_ECX);
    isEdit = (ecx && IsPlausiblePtr(ecx) && ecx[0x19]) ? 1 : 0;
    /* 判 [this+0x19] 而不是 ce_Active (后者在 DrawHook 才置位, 晚一帧)。
     * 同一帧 ChatEdit_Frame 见 IsEdit 即激活并画盒, 不空框。 */
    if (!isEdit) return CE_NATIVE_SKIP;   /* 0x5D4A8D */
    if (!ce_Enable) return CE_NATIVE_ENTER;/* 0x5D49BF: 引擎原生路径, 画原生行 */
    return CE_NATIVE_SKIP;   /* 0x5D4A8D */
}

/* ---------------- 单人聊天框 + 名字前缀 ---------------- */

/* 单人聊天框 (Phobos AllowChatBoxInSinglePlayer 同原理, 见 notes/ChatBox参考.md):
 * 对话框函数按 [SessionClass 0xA8B238]==0/5 判定单人并跳过聊天框四处逻辑。
 * 开启时跳过 session 检查 (保留其余旗标检查), 返回 0 走原版。
 *   0x55E477 (5B, mov eax,[sess]) → 0x55E48D (绕过 je, 进 0xA8BC79 检查)
 *   0x55E62F (6B, cmp [sess],ebx) → 0x55E637 (跳过 je, 进 push/call)
 *   0x55E693 (6B, cmp [sess],ebx) → 0x55E69B (跳过 je, 进 cmp esi,0x1b)
 *   0x55E746 (5B, mov eax,[sess]) → 0x55E77B (绕过 je, 进 CurrentPlayer 分支) */
#define ADDR_CHATSP_1      0x0055E48Du
#define ADDR_CHATSP_2      0x0055E637u
#define ADDR_CHATSP_3      0x0055E69Bu
#define ADDR_CHATSP_4      0x0055E77Bu

extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_ChatSp1(void* regs)
{
    static int s_logged = 0;
    (void)regs;
    CE_EnsureInit();
    if (!ce_ChatBoxSP) return 0;
    if (!s_logged) { s_logged = 1; LogLine("chatsp: singleplayer chatbox on"); }
    return ADDR_CHATSP_1;
}

extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_ChatSp2(void* regs)
{
    (void)regs;
    CE_EnsureInit();
    if (!ce_ChatBoxSP) return 0;
    return ADDR_CHATSP_2;
}

extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_ChatSp3(void* regs)
{
    (void)regs;
    CE_EnsureInit();
    if (!ce_ChatBoxSP) return 0;
    return ADDR_CHATSP_3;
}

extern "C" __declspec(dllexport) DWORD __cdecl ChatEdit_ChatSp4(void* regs)
{
    (void)regs;
    CE_EnsureInit();
    if (!ce_ChatBoxSP) return 0;
    return ADDR_CHATSP_4;
}
/* 单人消息是否显示 "名字: " 前缀 (消息排版侧问, 见 MsgText.cpp) */
int ChatEdit_ShowNameSP(void)
{
    CE_EnsureInit();
    return ce_ChatNameSP;
}

/* ---------------- 帧入口 ---------------- */

int ChatEdit_Frame(void* surf, int bitLineH, int screenW,
                   int surfW, int surfH, int surfPitch)
{
    int isEdit, fieldW;
    (void)screenW;
    CE_EnsureInit();
    CE_PumpEnsure();
    if (!surf || surfW <= 0 || surfH <= 0 || surfPitch <= 0) return 0;
    isEdit = CE_NativeIsEdit();
    if (!ce_Enable)
    {
        if (ce_Active) CE_Deactivate();
        return 0;
    }
    if (isEdit && !ce_Active) CE_Activate();
    else if (!isEdit && ce_Active) CE_Deactivate();
    if (!ce_Active) return 0;
    CE_PollIME();
    if (ce_CompLen <= 0)
    {
        fieldW = CE_FieldW();
        if (fieldW > surfW) fieldW = 640;
        /* 轮询只在游戏自己当前台时动手 (GetAsyncKeyState 是全局状态)。 */
        if (CE_GameForeground()) CE_PollKeys(fieldW);
    }
    /* 单机发送不走 Input 回车分支, 缓冲非空逐帧刷新指纹, 回声何时到都命中。 */
    if (ce_Len > 0) CE_NoteSent();
    /* 我方缓冲为准权威回写: VK_DELETE 由对话框层直接改原生缓冲 (不走 Input 钩),
     * 不覆盖回去就是"显示没删、发送少字"。 */
    CE_Mirror();
    /* 绘制放 MessageDrawHook 尾部 (战役框/提示框/超武面板之后), 输入框压最上层。
     * 原生行已隐藏, 无需再盖。 */
    CE_Render(surf, bitLineH, surfW, surfH, surfPitch);
    CE_ReleaseFrameImc();   /* 本帧没人用上的 IME 上下文 (上面几条提前 return 的路) */
    return 1;   /* 激活中 */
}
