/* LineEdit.h - 单行输入框的全部公共零件 (存档名框 / 聊天框 / ESC 数值框共用)。
 * 五节: 按键轮询 / 缓冲编辑 / 剪贴板 / 逐字几何 / 呈现区与输入法。
 * 前三节只碰 windows.h, 逐字几何靠 GdiText 的字宽, 最后一节是宿主适配 (imm.h)。
 *
 * **编辑语义不在这里** —— 三个框的文字真相来源不一样:
 *   存档名框: 在引擎 NewEdit 里 (0x4B3 读 / 0x4B2 写回);
 *   聊天框:   本地缓冲 ce_Buf, 镜像给引擎;
 *   ESC 数值框: 只有数字, 无选中/无输入法, 度量是超采样后除的 (ESC_TEXT_SS)。
 * 词移动同理没并: 聊天框对非 ASCII 一次一个字符, 存档框把整段 CJK 当一个词。
 *
 * 无 CRT: 只用 Win32 与 GdiText 已导出的东西。全 static inline, 各 TU 内联一份。
 */
#pragma once

#include <windows.h>
#include <imm.h>
#include "GdiText.h"

/* ---------------- 按键轮询 ----------------
 * 按键走 GetAsyncKeyState: 边缘立刻一步, 按住 400ms 后 33ms 连发。 */

typedef struct { int vk; int prev; unsigned nextRep; int repeat; } LiKey;

static inline int LiKeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}

static inline void LiKeysReset(LiKey* keys, int n, int fromCurrent)
{
    unsigned now = GetTickCount();
    int i;
    for (i = 0; i < n; i++)
    {
        keys[i].prev = fromCurrent ? LiKeyDown(keys[i].vk) : 0;
        keys[i].nextRep = keys[i].prev ? now + 400 : 0;
    }
}

static inline int LiKeyFire(LiKey* k, unsigned now)
{
    int down = LiKeyDown(k->vk);
    if (down && !k->prev) { k->nextRep = now + 400; k->prev = 1; return 1; }
    if (down && k->repeat && (int)(now - k->nextRep) >= 0)
    { k->nextRep = now + 33; k->prev = 1; return 1; }
    k->prev = down;
    return 0;
}

/* ---------------- 缓冲编辑 ----------------
 * 不带选中区, 调用方需要时先自行删除选区。 */

static inline int LiInsert(wchar_t* buf, int cap, int* len, int* cur,
                           wchar_t ch, int maxLen)
{
    int i;
    if (maxLen > 0 && *len >= maxLen) return 0;
    if (*len >= cap - 1) return 0;
    if (*cur < 0) *cur = 0;
    if (*cur > *len) *cur = *len;
    for (i = *len; i >= *cur; i--) buf[i + 1] = buf[i];
    buf[(*cur)++] = ch;
    (*len)++;
    return 1;
}

static inline void LiDeleteRange(wchar_t* buf, int* len, int* cur,
                                 int lo, int hi)
{
    int i;
    if (lo < 0 || hi <= lo) return;
    if (hi > *len) hi = *len;
    if (hi <= lo) return;
    for (i = hi; i <= *len; i++) buf[i - (hi - lo)] = buf[i];
    *len -= hi - lo;
    *cur = lo;
}

static inline void LiBackspace(wchar_t* buf, int* len, int* cur)
{
    if (*cur > 0) LiDeleteRange(buf, len, cur, *cur - 1, *cur);
}

static inline void LiDelete(wchar_t* buf, int* len, int* cur)
{
    if (*cur < *len) LiDeleteRange(buf, len, cur, *cur, *cur + 1);
}

static inline void LiMove(int vk, int* cur, int len)
{
    if (vk == VK_LEFT && *cur > 0) (*cur)--;
    else if (vk == VK_RIGHT && *cur < len) (*cur)++;
    else if (vk == VK_HOME) *cur = 0;
    else if (vk == VK_END) *cur = len;
}

static inline int LiDigit(int vk, wchar_t* ch)
{
    if (vk >= '0' && vk <= '9') { *ch = (wchar_t)vk; return 1; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 &&
        (GetKeyState(VK_NUMLOCK) & 1))
    { *ch = (wchar_t)(L'0' + (vk - VK_NUMPAD0)); return 1; }
    return 0;
}

/* ---------------- 剪贴板 (宽字符) ---------------- */

/* 读剪贴板宽串, 返回长度 (截到 cap-1)。读不到返回 0 且 dst 置空。 */
static inline int LiClipGet(wchar_t* dst, int cap)
{
    HGLOBAL h;
    const wchar_t* p;
    SIZE_T bytes;
    int n = 0, i;

    if (!dst || cap < 2) return 0;
    dst[0] = 0;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return 0;
    if (!OpenClipboard(NULL)) return 0;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h)
    {
        p = (const wchar_t*)GlobalLock(h);
        if (p)
        {
            /* 别信 NUL 一定在: 别的程序可能塞进一段没结尾的块。 */
            bytes = GlobalSize(h);
            while (n < cap - 1 && (SIZE_T)(n + 1) * 2 < bytes && p[n]) n++;
            for (i = 0; i < n; i++) dst[i] = p[i];
            dst[n] = 0;
        }
        if (p) GlobalUnlock(h);
    }
    CloseClipboard();
    return n;
}

/* 写剪贴板宽串。所有权交出去, 失败才自己释放。 */
static inline void LiClipSet(const wchar_t* s, int n)
{
    HGLOBAL h;
    wchar_t* p;
    int i;

    if (!s || n <= 0) return;
    h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(n + 1) * sizeof(wchar_t));
    if (!h) return;
    p = (wchar_t*)GlobalLock(h);
    if (!p) { GlobalFree(h); return; }
    for (i = 0; i < n; i++) p[i] = s[i];
    p[n] = 0;
    GlobalUnlock(h);
    if (!OpenClipboard(NULL)) { GlobalFree(h); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    CloseClipboard();
}

/* ---------------- 逐字几何 ---------------- */

/* 各字符起点 xs[0..n] (像素)。调用前必须已 GdiEnsure 到该框的字号字体。
 * 宽度拿不到 (GtCharAdv < 0) 时按字号当全角算 —— 宁宽不错位。 */
static inline void LiCharXs(const wchar_t* s, int n, int px, int* xs)
{
    int i;

    xs[0] = 0;
    for (i = 0; i < n; i++)
    {
        int ga = GtCharAdv(s[i]);
        xs[i + 1] = xs[i] + ((ga < 0) ? px : ga);
    }
}

/* 文本坐标 x 命中的字符下标: 字左半算字前, 右半算字后。 */
static inline int LiHitIndex(const int* xs, int n, int mx)
{
    int i, best = 0;

    for (i = 0; i <= n; i++)
    {
        if (xs[i] <= mx) best = i;
        else break;
    }
    if (best < n && mx - xs[best] > (xs[best + 1] - xs[best]) / 2) best++;
    return best;
}

/* 让插入点留在可视区里, 返回新的横向滚动量 (调用方还要按总宽再夹一次)。 */
static inline int LiScrollFit(int curPx, int view, int fieldW)
{
    if (curPx - view > fieldW - 4) view = curPx - fieldW + 12;
    if (curPx < view) view = curPx;
    return (view < 0) ? 0 : view;
}

/* 选中区 [lo,hi); anchor < 0 或空选返回 lo = hi = -1。 */
static inline void LiSelRange(int cur, int anchor, int* lo, int* hi)
{
    *lo = *hi = -1;
    if (anchor < 0) return;
    *lo = (anchor < cur) ? anchor : cur;
    *hi = (anchor < cur) ? cur : anchor;
    if (*lo == *hi) *lo = *hi = -1;
}

/* ---------------- 呈现区与输入法 ----------------
 * 输入法坐标的基准是"主窗客户区", 而框画在渲染面上, 换算要 呈现区/渲染面。 */

/* 呈现区尺寸。全屏上采样时呈现区是**显示器**, 窗口化时是客户区; 到底在不在
 * 上采样只有 ddraw 包装层知道 (cnc-ddraw 导出 DDIsWindowed), 查不到就按窗口化
 * —— 那是唯一不会把候选窗推出屏幕的猜测。
 *   fsMode: -1 自动问包装层 / 0 强制客户区 / 1 强制显示器
 * 返回 1 = 量到了。缓存/节流/日志留给调用方 (聊天框每次开框量, 存档框 1 秒一次)。 */
static inline int LiPresent(HWND h, int fsMode, int* vw, int* vh)
{
    static int (*s_isWin)(void) = NULL;
    static int s_probed = 0;
    RECT cr;
    POINT org;
    HMONITOR mon;
    MONITORINFO mi;
    int cw, ch, w, ht, win = -1;

    if (!h || !IsWindow(h) || !GetClientRect(h, &cr)) return 0;
    cw = (int)(cr.right - cr.left);
    ch = (int)(cr.bottom - cr.top);
    if (cw < 100 || ch < 100) return 0;         /* 窗口刚建 / 正在切模式 */
    if (!s_probed)
    {
        HMODULE m;
        s_probed = 1;
        m = GetModuleHandleA("ddraw.dll");
        if (m) s_isWin = (int (*)(void))(size_t)GetProcAddress(m, "DDIsWindowed");
    }
    if (s_isWin) win = s_isWin() ? 1 : 0;
    if (fsMode == 0) win = 1;
    else if (fsMode == 1) win = 0;
    else if (win < 0) win = 1;
    w = cw; ht = ch;
    /* 全屏且客户区原点就在屏幕 (0,0): 客户区坐标 == 屏幕坐标, 基准换成显示器。 */
    if (!win)
    {
        org.x = 0; org.y = 0;
        ClientToScreen(h, &org);
        mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        mi.cbSize = sizeof(MONITORINFO);
        if (org.x == 0 && org.y == 0 && mon && GetMonitorInfo(mon, &mi))
        {
            int mw = (int)(mi.rcMonitor.right - mi.rcMonitor.left);
            int mh = (int)(mi.rcMonitor.bottom - mi.rcMonitor.top);
            if (mw > cw && mh > ch) { w = mw; ht = mh; }
        }
    }
    if (vw) *vw = w;
    if (vh) *vh = ht;
    return 1;
}

/* 组合窗与候选窗一起钉。两个坐标各按**持有上下文那个窗口**的基准给: 主窗上下文
 * 给主窗客户区坐标, 子控件上下文给它自己的客户区坐标。同一组数字推两处, 必有
 * 一处被再叠一次窗口原点 —— 那是候选窗飞出屏幕的唯一原因。 */
static inline void LiImePin(HIMC imc, int cx, int cyc, int ccx, int ccy)
{
    COMPOSITIONFORM cfc;
    CANDIDATEFORM cdc;

    if (!imc) return;
    cfc.dwStyle = CFS_FORCE_POSITION;
    cfc.ptCurrentPos.x = cx;
    cfc.ptCurrentPos.y = cyc;
    ImmSetCompositionWindow(imc, &cfc);

    cdc.dwIndex = 0;
    cdc.dwStyle = CFS_CANDIDATEPOS;
    cdc.ptCurrentPos.x = ccx;
    cdc.ptCurrentPos.y = ccy;
    cdc.rcArea.left = cdc.rcArea.top = 0;
    cdc.rcArea.right = cdc.rcArea.bottom = 0;
    ImmSetCandidateWindow(imc, &cdc);
}
