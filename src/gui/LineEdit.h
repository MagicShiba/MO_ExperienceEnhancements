/* LineEdit.h - 单行输入框的全部公共零件 (存档名框 / 聊天框 / ESC 数值框共用)。
 * 六节: 按键轮询 / 缓冲编辑 / 剪贴板 / 逐字几何 / 呈现区与输入法 / 编辑会话。
 * 前三节只碰 windows.h, 逐字几何靠 GdiText 的字宽, 后两节是宿主适配 (imm.h)。
 *
 * 编辑会话 (LiEdit) 是三框共用的编辑底子: 光标/选区/删除/复制剪切/粘贴/点击
 * 定位/光标闪烁/组字串读取一份实现。**文字真相来源不在这里** ——
 *   存档名框: 在引擎 NewEdit 里 (0x4B3 读 / 0x4B2 写回), 宿主每次操作前拉、
 *             改完写回;
 *   聊天框:   本地缓冲 ce_Buf, 改完镜像给引擎;
 *   ESC 数值框: 只有数字, 无选中/无输入法, 度量是超采样后除的 (ESC_TEXT_SS)。
 * 类型差异 (文本/数字/宽度预算) 走 LiAcceptFn 钩; 词移动不并: 聊天框对非
 * ASCII 一次一个字符, 存档框没有词跳。
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

/* 滚动量 view 下第一个可见字符的下标 (xs 单调不减)。 */
static inline int LiFirstVis(const int* xs, int n, int view)
{
    int i = 0;
    while (i < n && xs[i + 1] <= view) i++;
    return i;
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

/* ---------------- 编辑会话 (三框共用的编辑底子) ----------------
 * 光标/选区/删除/复制剪切/粘贴/点击定位/闪烁一份实现; 文字真相来源与
 * 绘制在宿主 (见文件头)。本节依赖上面的缓冲编辑/剪贴板/逐字几何/IMM。 */

#define LI_BLINK_MS  530        /* 光标闪烁半周期 (亮/暗各一拍) */
#define LI_PASTE_CAP 162        /* 粘贴读剪贴板的统一容量 */

typedef struct {
    wchar_t* buf;               /* 存储由宿主提供 */
    int cap;
    int len, cur, anchor;       /* anchor < 0 = 无选中, 选中 = [min,max) */
    int view;                   /* 横向滚动 (像素), 宿主在绘制时调整 */
    unsigned blinkT;
} LiEdit;

/* 宿主过滤钩: 在刚插入后调用, 返回 0 拒收该字符 (数字框收数字 /
 * 聊天框宽度预算), 会立即回退。NULL = 全收。 */
typedef int (*LiAcceptFn)(void* ctx, wchar_t ch, int newLen);

static inline void LiEdInit(LiEdit* e, wchar_t* buf, int cap)
{
    e->buf = buf;
    e->cap = cap;
    e->len = 0;
    e->cur = 0;
    e->anchor = -1;
    e->view = 0;
    e->blinkT = 0;
    buf[0] = 0;
}

/* 绑到**已有内容**的缓冲上, 不清字 (就地编辑一条已存在的数据时用;
 * 用 LiEdInit 会先把 buf[0] 清零 —— 输入框点一下就变空了)。 */
static inline void LiEdBind(LiEdit* e, wchar_t* buf, int cap, int len)
{
    e->buf = buf;
    e->cap = cap;
    e->len = (len < cap) ? len : cap - 1;
    e->cur = e->len;
    e->anchor = -1;
    e->view = 0;
    e->blinkT = 0;
}

/* 光标闪烁复位 (按键/点击后立刻亮着)。 */
static inline void LiEdTouch(LiEdit* e)
{
    e->blinkT = GetTickCount();
}

/* 光标亮暗相位 (1 = 亮)。有选中时藏光标是宿主的事。 */
static inline int LiBlinkOn(const LiEdit* e)
{
    return ((GetTickCount() - e->blinkT) / LI_BLINK_MS) % 2u == 0u;
}

/* 选中区 [lo,hi), 无选中 lo = hi = -1。 */
static inline void LiEdSel(const LiEdit* e, int* lo, int* hi)
{
    LiSelRange(e->cur, e->anchor, lo, hi);
}

/* 插入一段: 先删选区, 再逐字过 (控制符转空格 → 容量/maxLen → 宿主过滤)。
 * 返回实际插入的字符数。 */
static inline int LiEdInsert(LiEdit* e, const wchar_t* s, int n, int maxLen,
                             LiAcceptFn accept, void* ctx)
{
    int i, lo, hi, put = 0;

    LiSelRange(e->cur, e->anchor, &lo, &hi);
    if (lo >= 0)
    { LiDeleteRange(e->buf, &e->len, &e->cur, lo, hi); e->anchor = -1; }
    for (i = 0; i < n; i++)
    {
        wchar_t c = s[i];
        if (c < 0x20 || c == 0x7F) c = L' ';   /* 单行: 控制符转空格 */
        if (!LiInsert(e->buf, e->cap, &e->len, &e->cur, c, maxLen)) break;
        if (accept && !accept(ctx, c, e->len))
        {
            LiDeleteRange(e->buf, &e->len, &e->cur, e->cur - 1, e->cur);
            continue;
        }
        put++;
    }
    return put;
}

/* 退格/删除: 有选中删选中, 否则删插入点前/后一个。返回是否删了。 */
static inline int LiEdErase(LiEdit* e, int forward)
{
    int lo, hi, did = 0;

    LiSelRange(e->cur, e->anchor, &lo, &hi);
    if (lo >= 0)
    {
        LiDeleteRange(e->buf, &e->len, &e->cur, lo, hi);
        e->anchor = -1;
        did = 1;
    }
    else if (forward)
    {
        if (e->cur < e->len)
        { LiDeleteRange(e->buf, &e->len, &e->cur, e->cur, e->cur + 1); did = 1; }
    }
    else
    {
        if (e->cur > 0)
        { LiDeleteRange(e->buf, &e->len, &e->cur, e->cur - 1, e->cur); did = 1; }
    }
    if (did) LiEdTouch(e);
    return did;
}

/* 复制/剪切选区。返回是否复制了东西 (cut 时顺带删选区)。 */
static inline int LiEdCopy(LiEdit* e, int cut)
{
    int lo, hi;

    LiEdSel(e, &lo, &hi);
    if (lo < 0) return 0;
    if (hi > e->len) hi = e->len;
    LiClipSet(e->buf + lo, hi - lo);
    if (cut)
    {
        LiDeleteRange(e->buf, &e->len, &e->cur, lo, hi);
        e->anchor = -1;
        LiEdTouch(e);
    }
    return 1;
}

/* 粘贴: 剪贴板内容按 LiEdInsert 语义进框。返回是否插了。 */
static inline int LiEdPaste(LiEdit* e, int maxLen, LiAcceptFn accept, void* ctx)
{
    wchar_t tmp[LI_PASTE_CAP];
    int n = LiClipGet(tmp, LI_PASTE_CAP);

    return (n > 0) ? LiEdInsert(e, tmp, n, maxLen, accept, ctx) : 0;
}

/* ←→/Home/End 移动 (含 Shift 选区)。VK_UP/DOWN (历史/微调) 归宿主。 */
static inline void LiEdMove(LiEdit* e, int vk, int shift)
{
    if (!shift) e->anchor = -1;
    else if (e->anchor < 0) e->anchor = e->cur;
    LiMove(vk, &e->cur, e->len);
    if (shift && e->anchor == e->cur) e->anchor = -1;
    LiEdTouch(e);
}

/* 全选 (Ctrl+A)。 */
static inline void LiEdSelAll(LiEdit* e)
{
    e->anchor = 0;
    e->cur = e->len;
    LiEdTouch(e);
}

/* 交回原窗口过程 (没挂上时走默认过程)。 */
static inline LRESULT LiCallOld(HWND h, WNDPROC old, UINT msg,
                                WPARAM wp, LPARAM lp)
{
    if (!old) return IsWindowUnicode(h) ? DefWindowProcW(h, msg, wp, lp)
                                        : DefWindowProcA(h, msg, wp, lp);
    return IsWindowUnicode(h) ? CallWindowProcW(old, h, msg, wp, lp)
                              : CallWindowProcA(old, h, msg, wp, lp);
}

/* 点击定位: mx 为文本区起点起的像素 (已含滚动补偿), xs[0..n] 由 LiCharXs
 * 算好。shift = 1 时从原插入点选到我。返回新插入点。 */
static inline int LiEdClick(LiEdit* e, const int* xs, int n, int mx, int shift)
{
    int old = e->cur;

    e->cur = LiHitIndex(xs, n, mx);
    if (shift) { if (e->anchor < 0) e->anchor = old; }
    else e->anchor = -1;
    LiEdTouch(e);
    return e->cur;
}

/* 插入点像素 (相对文本区起点, 已扣滚动, 不为负)。 */
static inline int LiCaretX(const int* xs, int n, int cur, int view)
{
    int x;

    if (cur > n) cur = n;
    x = xs[cur] - view;
    return (x < 0) ? 0 : x;
}

/* 读组字串 + 组字游标。返回串长 (0 = 无组字, comp 置空)。 */
static inline int LiCompRead(HIMC imc, wchar_t* comp, int cap, int* cur)
{
    LONG r;
    int n = 0;

    if (cur) *cur = 0;
    if (!imc || !comp || cap < 2) { if (comp) comp[0] = 0; return 0; }
    r = ImmGetCompositionStringW(imc, GCS_COMPSTR, comp, (DWORD)(cap - 1) * 2);
    if (r > 0)
    {
        LONG c;
        n = (int)(r / 2);
        if (n > cap - 1) n = cap - 1;
        comp[n] = 0;
        if (cur)
        {
            c = ImmGetCompositionStringW(imc, GCS_CURSORPOS, NULL, 0);
            if (c < 0) c = 0;
            if (c > (LONG)n) c = (LONG)n;
            *cur = (int)c;
        }
    }
    else comp[0] = 0;
    return n;
}

/* 组字中? (按键钩子里现查, 与帧轮询独立)。 */
static inline int LiComposing(HWND w)
{
    HIMC imc;
    LONG r;
    int on = 0;

    if (!w) return 0;
    imc = ImmGetContext(w);
    if (imc)
    {
        r = ImmGetCompositionStringW(imc, GCS_COMPSTR, NULL, 0);
        ImmReleaseContext(w, imc);
        on = (r > 0) ? 1 : 0;
    }
    return on;
}
