/* SaveShot.cpp - 存/读档界面的缩略图、列表行、重命名/载入/删除按钮、存档名输入框。
 * 说明见 SaveShot.h 与 notes/存档截图.md。改动前必守:
 *   1. 面板 = MapDialogRect(模板 533x369 DLU) = 800x600 px, 引擎只把这块居中 blit;
 *      控件用客户区坐标 (= 屏幕坐标), 我们的像素也画在同一坐标系, 别放到面板外。
 *   2. 行与名字框的控件一个像素都不画: 控件的 GDI 像素晚于面板 blit (两套文字),
 *      赶在 blit 前画又会被盖掉。全部由 0x622396 画进那张整屏面。
 *   3. 字体一律跟随 [VecText]; 存档路径取自 0x67CF42 的 EDX (不是入口的 ECX)。
 *   4. 纯读渲染面 + 写本地文件, 不碰模拟, 联机安全。
 */

#include <windows.h>
#include <imm.h>
#include <objidl.h>
#include <propidl.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "Lang.h"
#include "LineEdit.h"
#include "Patch.h"
#include "ScaleBar.h"
#include "Shot.h"
#include "FrameCap.h"
#include "VecText.h"
#include "SaveShot.h"

#define SS_SECTION   "SaveShot"
#define SS_TICK_MS   1000      /* 兜底取图间隔 */
#define SS_DEF_W     320       /* 缩略图宽 (高度按视口比例) */
#define SS_MIN_W     96
#define SS_MAX_W     640
#define SS_DEF_PREV  200       /* 截图列宽 (px) */
#define SS_MIN_PREV  120

/* 模板原始尺寸 (DLU) 与换算失败时的兜底像素尺寸 */
#define SS_TPL_CX    533
#define SS_TPL_CY    369
#define SS_TPL_PX_W  800
#define SS_TPL_PX_H  600

/* FileEntryClass: Name 是 wchar_t[0x80], Filename 落在 +0x100 */
#define SS_FE_FILE   0x100
#define SS_FE_TIME   0x1AC
#define SS_FE_VALID  0x1B4
#define SS_FE_WRONG  0x1B5

/* 对话框控件 id (读档模板 183 / 存档模板 692) */
#define SS_ID_LIST_LOAD 1317
#define SS_ID_LIST_SAVE 1319
#define SS_ID_BTN_LOAD  1039
#define SS_ID_BTN_SAVE  1735
#define SS_ID_TITLE     1036

/* 我们自建的"载入"/"删除"/"重命名"按钮 (只有存档页)。必须在引擎的 WM_INITDIALOG 之前
 * 建出来, 才能被当作自绘按钮接管 —— 见 SS_SaveDlgProc。 */
#define SS_ID_BTN_LOAD_NEW 0x540
#define SS_ID_BTN_DEL_NEW  0x541
#define SS_ID_BTN_RN_NEW   0x542

/* 三个按钮排成一列时按钮之间 / 按钮列与截图列之间的缝 (px) */
#define SS_BTN_GAP  6
#define SS_COL_GAP  10
#define SS_ADDR_SAVE_DLG   0x558B90u   /* 存档页对话框过程 (stdcall, ret 0x10) */
#define SS_ADDR_MENU_TAIL  0x4F14BFu   /* ESC 菜单命令处理 0x4F1330 的公共落点 */

/* Syringe REGISTERS: EDI 8 / ESI 0xC / EBP 0x10 / ESP 0x14 / EBX 0x18 / EDX 0x1C */
#define SS_REG_EBX   0x18u
#define SS_REG_EDX   0x1Cu

#define SS_NAME_MAX  64

#define SS_DWLP_USER 8             /* 对话框状态机对象 (GetWindowLongPtr) */

/* ---------------- 状态 ---------------- */

static int  s_Init = 0;
static int  s_Enable = 1;
static int  s_W = SS_DEF_W;
static int  s_PrevW = SS_DEF_PREV;      /* 截图列宽 (px) */

static unsigned short* s_Pix = NULL;    /* 缓存: 当前局面的缩略图 */
static int  s_PixW = 0, s_PixH = 0;
static unsigned s_LastTick = 0;

static char s_SaveDir[MAX_PATH_LEN];    /* 存档目录 (= 上次存档所在目录) */
static char s_BaseName[SS_NAME_MAX];    /* 本次存档主名 (无扩展名) */
static int  s_EdHook = 1;               /* [SaveShot] EditHook: 0 = 不接管名字框 */

/* 面板与控件 (全部屏幕坐标) */
static void* s_Dlg = NULL;
static int   s_DlgList = 0;
static int   s_Px = 0, s_Py = 0, s_PanelW = 0, s_PanelH = 0;
static RECT  s_ListRect;                /* 期望的列表矩形 */
static RECT  s_BoxA, s_BoxB;            /* 两个截图框 (只含图, 不含标题) */
static int   s_CapAx = 0, s_CapAy = 0;  /* 框标题左上角 */
static int   s_CapBx = 0, s_CapBy = 0;
static int   s_CapH = 0;                /* 标题行高 (跟随 [VecText] 字号) */
static int   s_LayoutW = 0, s_LayoutH = 0;  /* 布局时的面板尺寸, 变了就重排 */
static int   s_LayoutN = 0;             /* 本实例排过几次 (防和引擎互抢位置) */
static int   s_MoverY = 0, s_MoverH = 0;/* 被推到面板底的存档名输入框 */
static HWND  s_Scroll = NULL;           /* 列表那个独立滚动条子窗口 */
static HWND  s_BoxHwnd = NULL;          /* SS_Layout 挑出来的控件 = 名字框 */
static RECT  s_BtnLoad, s_BtnDel, s_BtnRename;  /* 三个按钮位置 (空 = 不显示) */
static HWND  s_HWndLoad = NULL, s_HWndDel = NULL, s_HWndRename = NULL;
static int   s_DelArmShown = 0;         /* 删除按钮当前显示的是不是"确认删除" */
static unsigned s_Loading = 0;          /* 已请引擎读档: 期间不认/不画这个对话框 */
static HWND  s_LoadOldDlg = NULL;       /* 触发读档时的对话框, 销毁即这一程结束 */
static unsigned s_LastScan = 0;

/* 一张缩放好的 16 位图 (px 为空 = 没图) */
typedef struct { unsigned short* px; int w, h; } SS_Pic;

static SS_Pic s_PicA;                    /* 当前画面 */
static SS_Pic s_PicB;                    /* 选中存档的截图 */
static char   s_ThumbName[SS_NAME_MAX];  /* s_PicB 缓存的是哪一档 (只做比较) */

/* 前向声明 (矢量字那套定义在后面) */
static const wchar_t* SS_Face(void);
static int  SS_TextW(const wchar_t* s);
static void SS_PutTextLine(void* surf, const wchar_t* s, int x, int lineTop, int lineH,
                           unsigned r, unsigned g, unsigned b);
static INT_PTR __stdcall SS_SaveDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static void SS_DoRename(HWND dlg, HWND list);
static void SS_LoadText(void);

/* ---------------- 小工具 ---------------- */

/* 对话框的状态机对象 (DWLP_USER)。指针合法才返回, 否则 NULL。 */
static void* SS_Ctx(HWND dlg)
{
    void* ctx = (dlg && IsWindow(dlg)) ? (void*)GetWindowLongPtrA(dlg, SS_DWLP_USER) : NULL;
    return (ctx && IsPlausiblePtr(ctx)) ? ctx : NULL;
}

/* 拆目录与主名 (主名去掉扩展名)。dir/base 都可传 NULL。 */
static void SS_SplitPath(const char* path, char* dir, char* base)
{
    int n = lstrlenA(path), cut = -1, i, k;

    for (i = n - 1; i >= 0; i--)
        if (path[i] == '\\' || path[i] == '/') { cut = i; break; }
    if (dir)
    {
        for (k = 0; k < cut && k < MAX_PATH_LEN - 1; k++) dir[k] = path[k];
        dir[k] = 0;
    }
    if (base)
    {
        for (k = 0, i = cut + 1; path[i] && path[i] != '.' && k < SS_NAME_MAX - 1; )
            base[k++] = path[i++];
        base[k] = 0;
    }
}

/* dir (空则用存档目录) 下的 Thumbs\<base>.bmp。三处拼同一种路径, 一份实现。 */
static void SS_ThumbPath(char* out, unsigned cap, const char* dir, const char* base)
{
    unsigned p = AppendCStr(out, 0, dir[0] ? dir : s_SaveDir, cap);
    p = AppendCStr(out, p, "\\Thumbs\\", cap);
    p = AppendCStr(out, p, base, cap);
    AppendCStr(out, p, ".bmp", cap);
}

/* "saveshot: <a><b>" 这类短日志 (LogLine 只收一个串)。 */
static void SS_Log(const char* a, const char* b)
{
    char buf[128];
    unsigned p = AppendCStr(buf, 0, "saveshot: ", sizeof(buf));
    p = AppendCStr(buf, p, a, sizeof(buf));
    if (b) AppendCStr(buf, p, b, sizeof(buf));
    LogLine(buf);
}

/* ---------------- FileEntry 取值 ---------------- */

/* Name (wchar_t[0x80]) 即列表显示的文字。返回长度, 非法返回 0。 */
static int SS_RowName(unsigned fe, wchar_t* out, int cap)
{
    const wchar_t* nm;
    int i;

    out[0] = 0;
    if (!fe || !IsPlausiblePtr((const void*)(size_t)fe)) return 0;
    nm = (const wchar_t*)(size_t)fe;
    for (i = 0; i + 1 < cap && i < 0x7F; i++)
    {
        wchar_t c = nm[i];
        if (!c) break;
        if (c < 0x20) return 0;
        out[i] = c;
    }
    out[i] = 0;
    return i;
}

/* 定宽十进制, 返回写入的字符数。 */
static int SS_PutNum(wchar_t* o, int digits, int v)
{
    int i;

    for (i = digits - 1; i >= 0; i--) { o[i] = (wchar_t)(L'0' + v % 10); v /= 10; }
    return digits;
}

/* LastWriteTime → 本地 "YYYY-MM-DD HH:MM:SS" (定长 19 字)。无效时间返回 0。 */
static int SS_RowTime(unsigned fe, wchar_t* out)
{
    FILETIME ft, fl;
    SYSTEMTIME st;
    int n = 0;

    out[0] = 0;
    if (!fe || !IsPlausiblePtr((const void*)(size_t)fe)) return 0;
    ft.dwLowDateTime  = *(const unsigned*)(size_t)(fe + SS_FE_TIME);
    ft.dwHighDateTime = *(const unsigned*)(size_t)(fe + SS_FE_TIME + 4);
    if (!ft.dwLowDateTime && !ft.dwHighDateTime) return 0;
    if (!FileTimeToLocalFileTime(&ft, &fl)) return 0;
    if (!FileTimeToSystemTime(&fl, &st)) return 0;
    if (st.wYear < 1990 || st.wYear > 2200) return 0;   /* 偏移猜错时的保险 */

    n  = SS_PutNum(out, 4, st.wYear);      out[n++] = L'-';
    n += SS_PutNum(out + n, 2, st.wMonth); out[n++] = L'-';
    n += SS_PutNum(out + n, 2, st.wDay);   out[n++] = L' ';
    n += SS_PutNum(out + n, 2, st.wHour);  out[n++] = L':';
    n += SS_PutNum(out + n, 2, st.wMinute); out[n++] = L':';
    n += SS_PutNum(out + n, 2, st.wSecond);
    out[n] = 0;
    return n;
}

/* 档名: Name 读不出来 (比如第一行那个空白占位项) 就退回列表自己的字符串。 */
static int SS_RowText(HWND list, unsigned fe, int idx, wchar_t* out, int cap)
{
    char a[128];
    int n, i;

    if (SS_RowName(fe, out, cap) > 0) return 1;
    out[0] = 0;
    SendMessageA(list, LB_GETTEXT, (WPARAM)idx, (LPARAM)a);
    n = lstrlenA(a);
    if (n <= 0 || n >= (int)sizeof(a)) return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)a[i] < 0x20) return 0;
    if (MultiByteToWideChar(CP_ACP, 0, a, -1, out, cap) <= 1) { out[0] = 0; return 0; }
    return 1;
}

/* 选中项的 FileEntry +0x100: 引擎存的文件名 (可能是相对路径, 也可能只有名字)。 */
static int SS_SelectedFile(HWND list, char* out)
{
    int cur, k;
    unsigned fe;
    const char* nm;

    out[0] = 0;
    if (!list) return 0;
    cur = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
    if (cur < 0) return 0;
    fe = (unsigned)(size_t)SendMessageA(list, LB_GETITEMDATA, (WPARAM)cur, 0);
    if (!fe || !IsPlausiblePtr((const void*)(size_t)fe)) return 0;
    nm = (const char*)(size_t)(fe + SS_FE_FILE);
    for (k = 0; k < 31 && nm[k]; k++)
        if ((unsigned char)nm[k] < 0x20 || (unsigned char)nm[k] > 0x7E) return 0;
    if (k <= 0) return 0;
    memcpy(out, nm, (size_t)k);
    out[k] = 0;
    return 1;
}

/* ---------------- 像素 (跟随引擎位宽: 565 / 555) ---------------- */

static __inline int SS_GBits(void) { return (g_GBits == 6) ? 6 : 5; }
static __inline unsigned SS_R(unsigned short c) { return (c >> (SS_GBits() == 6 ? 11 : 10)) & 31u; }
static __inline unsigned SS_G(unsigned short c) { return (c >> 5) & ((SS_GBits() == 6) ? 63u : 31u); }
static __inline unsigned SS_B(unsigned short c) { return c & 31u; }

static __inline unsigned short SS_Pack(unsigned r, unsigned g, unsigned b)
{
    unsigned gmax = (SS_GBits() == 6) ? 63u : 31u;
    if (r > 31u) r = 31u;
    if (g > gmax) g = gmax;
    if (b > 31u) b = 31u;
    return (unsigned short)((r << (SS_GBits() == 6 ? 11 : 10)) | (g << 5) | b);
}

/* 已 Lock 的 16 位面: 填一块 (自动裁到面内) / 混一块 / 画 t 像素矩形边框。 */
static void SS_FillPx(unsigned char* base, int pitch, int W, int H,
                      int x, int y, int w, int h, unsigned short c)
{
    int i;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < h; i++)
    {
        unsigned short* r = (unsigned short*)(base + (size_t)(y + i) * pitch) + x;
        int k = w;
        while (k--) *r++ = c;
    }
}

static void SS_Frame(unsigned char* base, int pitch, int W, int H,
                     int x, int y, int w, int h, int t, unsigned short c)
{
    SS_FillPx(base, pitch, W, H, x, y, w, t, c);
    SS_FillPx(base, pitch, W, H, x, y + h - t, w, t, c);
    SS_FillPx(base, pitch, W, H, x, y, t, h, c);
    SS_FillPx(base, pitch, W, H, x + w - t, y, t, h, c);
}

static void SS_BlendPx(unsigned char* base, int pitch, int W, int H,
                       int x, int y, int w, int h,
                       unsigned r, unsigned g, unsigned b, unsigned a)
{
    int i, j;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < h; i++)
    {
        unsigned short* row = (unsigned short*)(base + (size_t)(y + i) * pitch) + x;
        for (j = 0; j < w; j++) row[j] = Blend16(row[j], r, g, b, a);
    }
}

/* ---------------- 图像 ----------------
 * 两个框里放的都是 16 位小图。尺寸一变就重分配, 不做容量复用 (设置界面, 不在乎)。 */

static void SS_PicFree(SS_Pic* p)
{
    if (p->px) VirtualFree(p->px, 0, MEM_RELEASE);
    p->px = NULL;
    p->w = p->h = 0;
}

/* 缩放 (宽高由调用方按比例算好): 缩小时对目标像素覆盖的源矩形取平均, 放大时
 * 落到最近点。src 为 16 位, srcPitch 字节/行; flip=1 表示源是自底向上 (BMP)。 */
static void SS_Shrink(unsigned short* dst, int dw, int dh,
                      const unsigned char* src, int sw, int sh, int srcPitch, int flip)
{
    int x, y;

    for (y = 0; y < dh; y++)
    {
        int sy0 = (int)((long long)y * sh / dh);
        int sy1 = (int)((long long)(y + 1) * sh / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (x = 0; x < dw; x++)
        {
            int sx0 = (int)((long long)x * sw / dw);
            int sx1 = (int)((long long)(x + 1) * sw / dw);
            unsigned ar = 0, ag = 0, ab = 0, n = 0;
            int xx, yy;

            if (sx1 <= sx0) sx1 = sx0 + 1;
            for (yy = sy0; yy < sy1; yy++)
            {
                const unsigned short* row = (const unsigned short*)
                    (src + (size_t)(flip ? (sh - 1 - yy) : yy) * srcPitch);
                for (xx = sx0; xx < sx1; xx++)
                {
                    unsigned short c = row[xx];
                    ar += SS_R(c); ag += SS_G(c); ab += SS_B(c); n++;
                }
            }
            dst[(size_t)y * dw + x] = n ? SS_Pack(ar / n, ag / n, ab / n) : 0;
        }
    }
}

/* 等比缩进 bw x bh 的框 (只填满, 不拉伸)。 */
static int SS_PicFit(SS_Pic* p, const unsigned char* src, int sw, int sh, int srcPitch,
                     int flip, int bw, int bh)
{
    int ow, oh;

    SS_PicFree(p);
    if (!src || sw <= 0 || sh <= 0 || bw < 8 || bh < 8) return 0;

    ow = bw;
    oh = (int)((long long)bw * sh / sw);
    if (oh > bh) { oh = bh; ow = (int)((long long)bh * sw / sh); }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;

    p->px = (unsigned short*)VirtualAlloc(NULL, (size_t)ow * oh * 2u,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p->px) return 0;
    SS_Shrink(p->px, ow, oh, src, sw, sh, srcPitch, flip);
    p->w = ow;
    p->h = oh;
    return 1;
}

#pragma pack(push, 1)
typedef struct {
    unsigned short magic;
    unsigned int   filesz;
    unsigned short r1, r2;
    unsigned int   off;
    unsigned int   hdrSize;
    int            w;
    int            h;
    unsigned short planes;
    unsigned short bits;
    unsigned int   compress;
    unsigned int   imgSize;
    int            hres, vres;
    unsigned int   nCol, nImp;
} SS_BmpHdr;
#pragma pack(pop)

/* 读 BMP (只认我们自己写的 16 位) 缩放进框。 */
static int SS_PicLoadBmp(SS_Pic* p, const char* path, int bw, int bh)
{
    HANDLE f;
    DWORD got = 0, sz;
    unsigned char* d;
    SS_BmpHdr* hd;
    int sh, ok = 0;

    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(f, NULL);
    d = (sz >= sizeof(SS_BmpHdr) && sz <= (16u << 20))
        ? (unsigned char*)VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
        : NULL;
    if (d && ReadFile(f, d, sz, &got, NULL) && got == sz)
    {
        hd = (SS_BmpHdr*)d;
        sh = (hd->h < 0) ? -hd->h : hd->h;
        if (hd->magic == 0x4D42 && hd->hdrSize >= 40 && hd->bits == 16 &&
            hd->w > 0 && hd->w <= 4096 && sh > 0 && sh <= 4096 &&
            hd->off < sz && (size_t)hd->off + (size_t)hd->w * 2 * (size_t)sh <= sz)
            ok = SS_PicFit(p, d + hd->off, hd->w, sh, hd->w * 2, hd->h > 0, bw, bh);
    }
    if (d) VirtualFree(d, 0, MEM_RELEASE);
    CloseHandle(f);
    return ok;
}

/* 丢掉下框缓存, 连同"缓存的是哪一档"的记认。 */
static void SS_ThumbReset(void)
{
    SS_PicFree(&s_PicB);
    s_ThumbName[0] = 0;
}

/* ---------------- 装钩小工具 ----------------
 * 跳板/补丁在 Patch.h; 装钩前跑 tools/hookaudit.py 能提前发现含 rel32 的站点。 */

/* 原版字节比对 → 建跳板 → 打补丁, 任一步失败就等于没装 (返回 NULL)。
 * 返回跳板 = 调原函数的入口。tag 只出现在日志里。 */
static void* SS_Hook(unsigned at, const unsigned char* want, int n, void* fn, const char* tag)
{
    void* orig;

    if (!PatchSame((const void*)(size_t)at, want, n))
    {
        SS_Log("site not pristine, OFF: ", tag);
        return NULL;
    }
    orig = PatchTrampAt(at, n, at + n);
    if (!orig || !PatchAt(at, fn, n))
    {
        SS_Log("hook FAILED: ", tag);
        return NULL;
    }
    SS_Log("hook ON: ", tag);
    return orig;
}

/* ---------------- 换掉存档页对话框过程 (0x558B90) ----------------
 * 目的是给对话框加**引擎原生**按钮: 只有在我们调原过程之前建出 BS_OWNERDRAW 子窗口,
 * 引擎的 WM_INITDIALOG 才会遍历到它、挂子类并注册进 0xAC1B00 表, 之后才用原版美术画。
 * 站点前 5 字节 = push ebx; mov ebx,[esp+0x10], 无 rel32。 */
static void* s_OrigSaveDlg = NULL;

/* ---------------- 自绘按钮绘制守卫 (0x6071E0) ----------------
 * 该函数一进门就假定 UI 主题资源 [0xB0FC14..0x2C] 非空 (0x607369 读 [0xB0FC20]
 * 直接解引用)。读档重建世界时这组资源被释放并置 0, 之后旧窗口的任意一次 WM_PAINT
 * 都会空指针崩溃。读档这一程按钮一个都不需要, 整函数跳过即可。 */
typedef int (__fastcall *SS_BtnDrawFn)(void* hwnd, int dl);
static SS_BtnDrawFn s_BtnDrawOrig = NULL;
static int s_BtnGuardLogged = 0;

/* UI 主题资源 [0xB0FC14..0x2C] 六个槽都还在。 */
static int SS_ThemeAlive(void)
{
    static const unsigned kTheme[6] = {
        0xB0FC14u, 0xB0FC18u, 0xB0FC1Cu, 0xB0FC20u, 0xB0FC24u, 0xB0FC28u
    };
    int i;

    for (i = 0; i < 6; i++) if (!*(void**)kTheme[i]) return 0;
    return 1;
}

static int __fastcall SS_BtnDrawGuard(void* hwnd, int dl)
{
    char buf[96];
    char cls[32];
    unsigned p;

    /* 读档这一程不需要任何按钮; 平时只在主题资源真的空了才拦 —— 那本来就是必崩前兆。 */
    if (!s_Loading && SS_ThemeAlive()) return s_BtnDrawOrig(hwnd, dl);

    if (s_BtnGuardLogged < 4)
    {
        s_BtnGuardLogged++;
        cls[0] = 0;
        if (hwnd) GetClassNameA((HWND)hwnd, cls, sizeof(cls));
        p = AppendCStr(buf, 0, "saveshot: btn draw skipped ", sizeof(buf));
        p = AppendCStr(buf, p, cls, sizeof(buf));
        p = AppendCStr(buf, p, " ret=", sizeof(buf));
        AppendInt(buf, p, sizeof(buf), (int)(size_t)__builtin_return_address(0));
        LogLine(buf);
    }
    return 0;
}

/* ---------------- 载入收尾 (0x4F14BF) ----------------
 * 菜单命令分派: 载入 (0x51E) 看返回值, 成功就把命令写进屏幕对象再 ret, 上层据此收掉
 * 菜单; 存档 (0x51F) 不看返回值, 无条件落到 0x4F14BF 把菜单 ShowWindow 回来。
 * 我们从存档页发起读档, 外层走的是存档分支 ⇒ 世界换了新档, 菜单又冒出来。所以在这里
 * 拦: s_TailLoad 为真就照载入的收尾做 (写 0x51E + ret), 别的命令原样执行。
 *
 * ebp = GetWindowLongA(hDlg, DWLP_USER) = 屏幕对象 (见 0x4F12B1)。
 * 站点 5 字节 = push 5; push esi; call edi, 无 rel32。 */
static volatile int s_TailLoad = 0;     /* 我们的读档跑完了, 等外层收尾 */
static int s_TailOn = 0;

static void SS_InstallMenuTail(const unsigned char* want)
{
    unsigned char* p;
    int i = 0, jOrig, jWrite;

    if (s_TailOn) return;
    if (!PatchSame((const void*)SS_ADDR_MENU_TAIL, want, 5))
    {
        SS_Log("menutail site not pristine, load tail OFF", NULL);
        return;
    }
    p = (unsigned char*)VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!p) return;

    /* cmp dword [s_TailLoad], 0 ; je orig */
    p[i++] = 0x83; p[i++] = 0x3D;
    *(unsigned*)(p + i) = (unsigned)(size_t)&s_TailLoad; i += 4;
    p[i++] = 0x00; p[i++] = 0x74; jOrig = i++;
    /* mov dword [s_TailLoad], 0 */
    p[i++] = 0xC7; p[i++] = 0x05;
    *(unsigned*)(p + i) = (unsigned)(size_t)&s_TailLoad; i += 4;
    *(unsigned*)(p + i) = 0; i += 4;
    /* test ebp,ebp ; jz Write */
    p[i++] = 0x85; p[i++] = 0xED; p[i++] = 0x74; jWrite = i++;
    /* mov dword [ebp], 0x51e */
    p[i++] = 0xC7; p[i++] = 0x45; p[i++] = 0x00;
    *(unsigned*)(p + i) = 0x51Eu; i += 4;
    p[jWrite] = (unsigned char)(i - (jWrite + 1));
    /* 按载入命令的收尾平衡栈并返回 */
    p[i++] = 0x33; p[i++] = 0xC0;
    p[i++] = 0x5F; p[i++] = 0x5E; p[i++] = 0x5D; p[i++] = 0x5B;
    p[i++] = 0x81; p[i++] = 0xC4; *(unsigned*)(p + i) = 0x614; i += 4;
    p[i++] = 0xC2; p[i++] = 0x10; p[i++] = 0x00;
    /* orig: 原样执行被覆盖的 5 字节 (edi 在这几条命令分派里都已 = ShowWindow) */
    p[jOrig] = (unsigned char)(i - (jOrig + 1));
    p[i++] = 0x6A; p[i++] = 0x05;
    p[i++] = 0x56;
    p[i++] = 0xFF; p[i++] = 0xD7;
    p[i++] = 0xB8; *(unsigned*)(p + i) = 0x4F14C4u; i += 4;
    p[i++] = 0xFF; p[i++] = 0xE0;

    FlushInstructionCache(GetCurrentProcess(), p, (SIZE_T)i);
    if (!PatchAt(SS_ADDR_MENU_TAIL, p, 5))
    {
        SS_Log("menutail patch FAILED", NULL);
        return;
    }
    s_TailOn = 1;
    SS_Log("load tail hook ON", NULL);
}

/* ---------------- 初始化 ---------------- */

static void SS_DefaultDir(void)
{
    unsigned p;
    const char* gd;

    if (s_SaveDir[0]) return;
    gd = GtGameDir();
    if (!gd[0]) return;
    p = AppendCStr(s_SaveDir, 0, gd, MAX_PATH_LEN);
    AppendCStr(s_SaveDir, p, "\\Saved Games", MAX_PATH_LEN);
}

void SaveShot_EnsureInit(void)
{
    static const unsigned char kDlg[5]  = { 0x53, 0x8B, 0x5C, 0x24, 0x10 };
    static const unsigned char kBtn[6]  = { 0x81, 0xEC, 0x8C, 0x01, 0x00, 0x00 };
    static const unsigned char kTail[5] = { 0x6A, 0x05, 0x56, 0xFF, 0xD7 };
    int w;

    if (s_Init) return;
    s_Init = 1;
    BuildPaths();
    GtEnsureLog();
    s_Enable  = GetPrivateProfileIntA(SS_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    w         = GetPrivateProfileIntA(SS_SECTION, "Width", SS_DEF_W, g_IniPath);
    s_PrevW   = GetPrivateProfileIntA(SS_SECTION, "PreviewWidth", SS_DEF_PREV, g_IniPath);
    s_EdHook  = GetPrivateProfileIntA(SS_SECTION, "EditHook", 1, g_IniPath) ? 1 : 0;
    if (w < SS_MIN_W) w = SS_MIN_W;
    if (w > SS_MAX_W) w = SS_MAX_W;
    if (s_PrevW < SS_MIN_PREV) s_PrevW = SS_MIN_PREV;
    if (s_PrevW > 420) s_PrevW = 420;
    s_W = w;
    SS_DefaultDir();
    if (!s_Enable) return;
    LogLine("saveshot: on");
    s_OrigSaveDlg = SS_Hook(SS_ADDR_SAVE_DLG, kDlg, 5, (void*)SS_SaveDlgProc, "savedlg");
    s_BtnDrawOrig = (SS_BtnDrawFn)SS_Hook(0x6071E0u, kBtn, 6, (void*)SS_BtnDrawGuard, "btndraw");
    SS_InstallMenuTail(kTail);
}

/* ---------------- 取图 ---------------- */

/* 盒式降采样到 s_W 宽。 */
static void SS_Capture(void)
{
    void* surf = NULL;
    int vx, vy, vw, vh, dw, dh, pitch;
    unsigned char* base;

    if (!SB_RawView(&surf, &vx, &vy, &vw, &vh) || !surf || vw < 8 || vh < 8) return;
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (pitch < GtSurfGet(surf, GtVT_GET_WIDTH) * 2) return;

    dw = s_W; if (dw > vw) dw = vw;
    dh = (int)((long long)dw * vh / vw);
    if (dh < 1) dh = 1;
    if (dh > dw * 2) dh = dw * 2;          /* 极端宽高比钳一下 */

    if (!s_Pix || s_PixW != dw || s_PixH != dh)
    {
        if (s_Pix) VirtualFree(s_Pix, 0, MEM_RELEASE);
        s_PixW = s_PixH = 0;
        s_Pix = (unsigned short*)VirtualAlloc(NULL, (size_t)dw * dh * 2u,
                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!s_Pix) return;
        s_PixW = dw;
        s_PixH = dh;
    }

    base = GtLock(surf);
    if (!base) return;
    SS_Shrink(s_Pix, dw, dh, base + (size_t)vy * pitch + (size_t)vx * 2, vw, vh, pitch, 0);
    GtUnlock(surf);
    s_LastTick = GetTickCount();
}

/* 按 ESC 那一刻的战场。调用点是 EscMenu_OnShow (0x4F1140, 引擎建好对话框、
 * ShowWindow 之前) —— 此时视口里还是刚渲染完的画面, 菜单一个像素都没画上去。
 * 之后再抓就晚了: 菜单/对话框是画在 Composite 上的, 会正好盖住视口那块。 */
void SaveShot_CaptureNow(void)
{
    SaveShot_EnsureInit();
    if (!s_Enable) return;
    if (!*(volatile unsigned*)ADDR_SCEN_STARTED) return;   /* 未开局 */
    if (s_Loading) return;                                 /* 读档中, 世界正在重建 */
    /* 已经开着对话框时又建了子对话框: 画面里已经有菜单像素, 再抓就是拿菜单当
     * "当前画面"。只在"从无到有"那次抓。 */
    if (FrameCap_ActiveDlg()) return;
    SS_Capture();
}

void SaveShot_Tick(void)
{
    SaveShot_EnsureInit();
    if (!s_Enable) return;
    if (!*(volatile unsigned*)ADDR_SCEN_STARTED) return;
    if (FrameCap_ActiveDlg()) return;                      /* ESC 菜单开着 */
    if (s_Loading) return;
    if (s_Dlg && IsWindow((HWND)s_Dlg)) return;             /* 存/读档界面开着 */
    /* 正常路径由 SaveShot_CaptureNow 抓; 这里只补一张最开始那帧兜底
     * (没有它, 在"还没弹过任何对话框"之前存档就无图可写)。 */
    if (s_Pix) return;
    if (GetTickCount() - s_LastTick < SS_TICK_MS) return;
    SS_Capture();
}

/* ---------------- 落盘 ---------------- */

/* 0x67CF42 = 存储创建调用点, EDX 是引擎拼好的完整路径。该处先做
 * MultiByteToWideChar 再交给存储, 所以正常是宽字符; 但 API 用的是 StgCreateDocfile
 * (非 W), 保险起见两种都认: 第 2 字节是 0 就当宽字符。 */
void SaveShot_OnSaveBegin(void* regs)
{
    const unsigned char* p0;
    char path[MAX_PATH_LEN], dir[MAX_PATH_LEN], base[SS_NAME_MAX], buf[MAX_PATH_LEN + 48];
    unsigned p, k;
    int wide;

    if (!regs) return;
    SaveShot_EnsureInit();
    if (!s_Enable) return;

    s_BaseName[0] = 0;
    p0 = (const unsigned char*)(size_t)*(unsigned*)((char*)regs + SS_REG_EDX);
    if (!p0 || !IsPlausiblePtr(p0) || !p0[0]) { SS_Log("bad path ptr", NULL); return; }

    wide = (p0[1] == 0 && p0[0] >= 0x20 && p0[0] < 0x7F);
    if (wide)
    {
        if (!WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)p0, -1, path, MAX_PATH_LEN, NULL, NULL))
        {
            SS_Log("wide->ansi failed", NULL);
            return;
        }
    }
    else
    {
        path[0] = 0;
        AppendCStr(path, 0, (const char*)p0, MAX_PATH_LEN);
    }

    SS_SplitPath(path, dir, base);
    for (k = 0; k + 1 < (unsigned)SS_NAME_MAX && base[k]; k++) s_BaseName[k] = base[k];
    s_BaseName[k] = 0;

    /* 引擎给的是相对路径 ("Saved Games\SAVE0001.SAV"): 补成绝对, 免得进程 CWD
     * 变了以后读 Thumbs 读不到。 */
    if (dir[0])
    {
        if (dir[0] == '\\' || dir[1] == ':') AppendCStr(s_SaveDir, 0, dir, MAX_PATH_LEN);
        else
        {
            p = AppendCStr(buf, 0, GtGameDir(), sizeof(buf));
            p = AppendCh(buf, p, '\\', sizeof(buf));
            AppendCStr(buf, p, dir, sizeof(buf));
            AppendCStr(s_SaveDir, 0, buf, MAX_PATH_LEN);
        }
    }
    SS_DefaultDir();

    SS_Log(wide ? "pathW " : "pathA ", path);
}

void SaveShot_OnSaveEnd(void* regs)
{
    char dir[MAX_PATH_LEN], path[MAX_PATH_LEN];
    unsigned p;
    int ok;

    if (!regs) return;
    if (!s_Enable) return;
    if (!(*(unsigned*)((char*)regs + SS_REG_EBX) & 0xFFu)) { SS_Log("end bl=0", NULL); return; }
    if (!s_BaseName[0]) { SS_Log("end no path", NULL); return; }
    if (!s_Pix || s_PixW <= 0 || s_PixH <= 0) { SS_Log("no frame cached, skip", NULL); return; }

    p = AppendCStr(dir, 0, s_SaveDir, MAX_PATH_LEN);
    AppendCStr(dir, p, "\\Thumbs", MAX_PATH_LEN);
    CreateDirectoryA(dir, NULL);   /* 已存在返回 ERROR_ALREADY_EXISTS, 忽略 */

    SS_ThumbPath(path, MAX_PATH_LEN, s_SaveDir, s_BaseName);
    ok = Shot_WriteBmp565(path, s_Pix, s_PixW, s_PixH);
    if (ok) SS_ThumbReset();       /* 新写的这张要能被下次读档读到 */
    SS_Log(ok ? "write " : "write FAILED ", path);
}

/* ---------------- 找对话框 ---------------- */

/* 真对话框的 DLGPROC 恒非零; 用 CreateWindowEx 建的原始 #32770 窗口附加字节为
 * 零初始化, 必为 0。MapDialogRect 对后者会直接解引用未初始化的 DialogInfo 指针
 * (崩溃实证: USER32 读 [pwnd+0x128]->+8, 值是分配残留), 所以碰窗口几何前先过这道。 */
static int SS_IsRealDialog(HWND h)
{
    return h && GetWindowLongPtrA(h, DWLP_DLGPROC) != 0;
}

/* 对话框是 #32770, 但窗口被撑成整屏, 只能靠"有没有我们的列表控件"认。 */
typedef struct { HWND hwnd; int listId; } SS_Find;

static BOOL CALLBACK SS_EnumCb(HWND h, LPARAM lp)
{
    SS_Find* f = (SS_Find*)lp;
    char cls[24];

    if (GetClassNameA(h, cls, sizeof(cls)) < 2) return TRUE;
    if (lstrcmpA(cls, "#32770") != 0) return TRUE;
    if (!SS_IsRealDialog(h)) return TRUE;
    if (GetDlgItem(h, SS_ID_LIST_LOAD)) { f->hwnd = h; f->listId = SS_ID_LIST_LOAD; return FALSE; }
    if (GetDlgItem(h, SS_ID_LIST_SAVE)) { f->hwnd = h; f->listId = SS_ID_LIST_SAVE; return FALSE; }
    return TRUE;
}

static HWND SS_FindDialog(int* listId)
{
    SS_Find f;

    f.hwnd = NULL;
    f.listId = 0;
    EnumWindows(SS_EnumCb, (LPARAM)&f);
    if (!f.hwnd) EnumChildWindows(GetDesktopWindow(), SS_EnumCb, (LPARAM)&f);
    if (listId) *listId = f.listId;
    return f.hwnd;
}

static int SS_WinRect(HWND h, RECT* out)
{
    return (h && GetWindowRect(h, out)) ? 1 : 0;
}

/* 面板尺寸: 引擎按模板换算成像素并居中, 固定 800x600。 */
static void SS_PanelRect(HWND hDlg, RECT* client)
{
    RECT rc;

    GetClientRect(hDlg, client);
    SetRect(&rc, 0, 0, SS_TPL_CX, SS_TPL_CY);
    if (!SS_IsRealDialog(hDlg) ||
        !MapDialogRect(hDlg, &rc) || rc.right < 240 || rc.bottom < 180)
        SetRect(&rc, 0, 0, SS_TPL_PX_W, SS_TPL_PX_H);

    s_PanelW = rc.right;
    s_PanelH = rc.bottom;
    s_Px = (client->right - s_PanelW) / 2;
    s_Py = (client->bottom - s_PanelH) / 2;
    if (s_Px < 0) s_Px = 0;
    if (s_Py < 0) s_Py = 0;
}

/* 引擎给列表配了一个独立滚动条子窗口 (id=0, 紧贴列表右缘、同高同顶)。
 * MoveWindow 列表不会带着它走, 不挪就会压到截图列上。 */
static HWND SS_FindScroll(HWND hDlg, HWND list, const RECT* lr)
{
    HWND c;
    RECT r;

    for (c = GetWindow(hDlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
    {
        if (c == list || GetDlgCtrlID(c) != 0 || !SS_WinRect(c, &r)) continue;
        if (r.left < lr->right - 2 || r.left > lr->right + 8) continue;
        if (r.top < lr->top - 6 || r.top > lr->top + 6) continue;
        if (r.bottom < lr->bottom - 6 || r.bottom > lr->bottom + 6) continue;
        if (r.right - r.left < 8 || r.right - r.left > 40) continue;
        return c;
    }
    return NULL;
}

/* 跟随列表加宽的子控件: 右缘不越过主按钮左缘 (bandR), 也不窄于原宽。 */
static int SS_WideW(const RECT* r, int nl, int dw, int bandR, int pad)
{
    int ow = (int)(r->right - r->left), nw = ow + dw;

    if (bandR > 0 && nl + nw > bandR - pad) nw = bandR - pad - nl;
    return (nw < ow) ? ow : nw;
}

static void SS_PlaceBtn(HWND h, const RECT* r)
{
    if (!h || !IsWindow(h)) return;
    if (r->right > r->left)
    {
        MoveWindow(h, (int)r->left, (int)r->top, (int)(r->right - r->left),
                   (int)(r->bottom - r->top), TRUE);
        ShowWindow(h, SW_SHOW);
    }
    else ShowWindow(h, SW_HIDE);
}

/* 三个自绘按钮不归引擎管, 遍历子控件时一律跳过。 */
static int SS_IsOurBtn(HWND w)
{
    return w == s_HWndRename || w == s_HWndLoad || w == s_HWndDel;
}

/* 排一次版: 列表左移到留白处、往右伸长到截图列边上; 右侧一列竖排两个截图框
 * (标题在图上, 图的顶边与列表顶边对齐); 重命名/载入/删除排在截图列下方,
 * 与储存按钮同宽同高、左缘对齐。所有坐标必须在面板内。
 * 这段判据是拿实测算出来的, 别"顺手优化"。 */
static int SS_Layout(HWND hDlg, HWND list, int listId)
{
    RECT cr, o, r, band, sr;
    HWND c, btn, title, sb;
    const int pad = 12;
    int avail, listW, listX, colW, colX, dx, dw, bw = 0, bh = 0, sbW = 0;
    int bandR = 0;      /* 输入框右缘上限: 载入/删除 占掉的地方 */

    s_MoverY = s_MoverH = 0;

    SS_LoadText();      /* 载入页的对话框不经过我们的 DlgProc, 界面文字在这里统一读 */

    SS_PanelRect(hDlg, &cr);
    if (s_PanelW < 240 || s_PanelH < 180) return 0;
    if (!SS_WinRect(list, &o)) return 0;

    sb = SS_FindScroll(hDlg, list, &o);
    if (sb && SS_WinRect(sb, &sr)) sbW = (int)(sr.right - sr.left);
    s_Scroll = sb;

    listX  = s_Px + pad;
    avail  = s_Px + s_PanelW - pad - listX;
    colW   = s_PrevW;
    if (colW > avail - sbW - 380) colW = avail - sbW - 380;   /* 列表至少留 380 */
    if (colW < SS_MIN_PREV) colW = 0;                         /* 挤不下就不开截图列 */
    listW  = avail - sbW - (colW > 0 ? colW + pad : 0);
    if (listW < o.right - o.left) {                   /* 不缩, 只长 */
        listW = (int)(o.right - o.left);
        listX = (int)o.left;
    }

    dx = listX - (int)o.left;
    dw = listW - (int)(o.right - o.left);

    SetRect(&s_ListRect, listX, (int)o.top, listX + listW, (int)o.bottom);
    MoveWindow(list, listX, s_ListRect.top, listW, s_ListRect.bottom - s_ListRect.top, TRUE);
    if (sb)
        MoveWindow(sb, listX + listW, (int)sr.top, sbW, (int)(sr.bottom - sr.top), TRUE);

    colX = listX + listW + sbW + pad;

    /* 底部那条带: [存档名输入框 ...][储存] —— 输入框右缘让到按钮左边 */
    btn = GetDlgItem(hDlg, listId == SS_ID_LIST_SAVE ? SS_ID_BTN_SAVE : SS_ID_BTN_LOAD);
    SetRect(&band, 0, 0, 0, 0);
    SetRect(&s_BtnLoad, 0, 0, 0, 0);
    SetRect(&s_BtnDel, 0, 0, 0, 0);
    SetRect(&s_BtnRename, 0, 0, 0, 0);
    if (btn && SS_WinRect(btn, &r))
    {
        bw = (int)(r.right - r.left);
        bh = (int)(r.bottom - r.top);
        band.left   = colW > 0 ? colX : s_Px + s_PanelW - bw - pad;
        band.top    = (int)o.bottom + 10;
        band.right  = band.left + bw;
        band.bottom = band.top + bh;
        bandR = band.left;
    }

    /* 高度: 列表一直拉到面板底。下面压着的可见小控件 (存档名输入框) 推到面板底,
     * 列表停在它上面 8px。 */
    {
        int bottomLimit = s_Py + s_PanelH - pad;
        int newBottom = bottomLimit;
        HWND mover = NULL;
        RECT mr;

        SetRect(&mr, 0, 0, 0, 0);
        for (c = GetWindow(hDlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        {
            if (c == list || c == sb || SS_IsOurBtn(c)) continue;
            if (!IsWindowVisible(c) || !SS_WinRect(c, &mr)) continue;
            if (mr.top < o.bottom - 6) continue;                  /* 只看列表下方 */
            if (mr.right <= listX || mr.left >= listX + listW) continue;
            if (mr.bottom - mr.top > 60) continue;                /* 大块的不动 */
            mover = c;
            break;
        }
        /* 只记录不参与布局: 名字框接管只认这一个。引擎的子控件层级跟模板 id 对不上,
         * 自己按 id/类名再扫一遍就会挑到隐藏的空壳。 */
        if (listId == SS_ID_LIST_SAVE) s_BoxHwnd = mover;
        if (mover)
        {
            int eh = (int)(mr.bottom - mr.top);
            int nl = (int)mr.left + dx;
            int nw = SS_WideW(&mr, nl, dw, bandR, pad);
            int ntop = bottomLimit - eh;

            if (ntop < (int)o.bottom + 18) ntop = (int)o.bottom + 18;
            MoveWindow(mover, nl, ntop, nw, eh, TRUE);
            s_MoverY = ntop;
            s_MoverH = eh;
            newBottom = ntop - 8;
        }
        if (newBottom > bottomLimit) newBottom = bottomLimit;
        if (newBottom > (int)o.bottom)
        {
            s_ListRect.bottom = newBottom;
            if (sb) MoveWindow(sb, listX + listW, s_ListRect.top, sbW,
                               newBottom - (int)o.top, TRUE);
            MoveWindow(list, listX, s_ListRect.top, listW, newBottom - (int)o.top, TRUE);
        }
    }

    /* 主按钮的最终位置 (与存档名输入框垂直居中) 在这里就定死: 下面截图列要让出的
     * 高度、三个按钮的排法都按它算 —— 两处各自算一次就会差几个像素。 */
    if (btn && bh > 0)
    {
        if (s_MoverH > 0) band.top = s_MoverY + (s_MoverH - bh) / 2;
        if (band.top + bh > s_Py + s_PanelH - 4) band.top = s_Py + s_PanelH - 4 - bh;
    }

    /* 与列表左缘对齐、贴着列表下沿的控件跟着挪+加宽, 但别伸进截图列 */
    if (dw || dx)
    {
        for (c = GetWindow(hDlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        {
            int nl;

            if (c == list || c == sb || SS_IsOurBtn(c)) continue;
            if (!SS_WinRect(c, &r)) continue;
            if (r.left < o.left - 8 || r.left > o.left + 8) continue;
            if (r.top < o.bottom - 6 || r.top > o.bottom + 40) continue;
            nl = (int)r.left + dx;
            MoveWindow(c, nl, (int)r.top, SS_WideW(&r, nl, dw, bandR, pad),
                       (int)(r.bottom - r.top), TRUE);
        }
    }

    /* 标题跟着列表走 (模板里两者同左缘同宽) */
    title = GetDlgItem(hDlg, SS_ID_TITLE);
    if (title && SS_WinRect(title, &r))
        MoveWindow(title, listX, (int)r.top, listW, (int)(r.bottom - r.top), TRUE);

    if (colW > 0)
    {
        /* 框高 = 图高 + 2: 图片按比例 fit 进去, 框比图高就上下留黑边 */
        int vw = s_PixW, vh = s_PixH, imgW, imgH, availH, need, colTop, stackTop;

        if (vw < 8 || vh < 8)
        {
            void* sv = NULL;
            int vx = 0, vy = 0, w2 = 0, h2 = 0;
            if (SB_RawView(&sv, &vx, &vy, &w2, &h2) && w2 > 0 && h2 > 0) { vw = w2; vh = h2; }
            else { vw = 4; vh = 3; }
        }

        s_CapH = VecText_ResolvedPx() + 6;
        if (s_CapH < 14) s_CapH = 14;

        imgW = colW - 2;
        imgH = (int)((long long)imgW * vh / vw);
        if (imgH < 24) imgH = 24;

        /* 竖向预算: 标题行在图上面 (整块比列表顶高一个标题行), 下边到三个按钮
         * 排起来的上沿再留一道缝。按钮上沿按它的实际位置算, 不用估的。 */
        stackTop = (int)s_ListRect.bottom;
        if (listId == SS_ID_LIST_SAVE && bw > 0 && bh > 0)
            stackTop = (int)band.top - SS_BTN_GAP - (3 * bh + 2 * SS_BTN_GAP);
        colTop = (int)o.top - s_CapH;
        if (colTop < s_Py + 4) colTop = s_Py + 4;   /* 面板顶上放不下就压回来 */

        availH = stackTop - SS_COL_GAP - colTop;
        if (availH < 2 * (s_CapH + 26) + pad) availH = 2 * (s_CapH + 26) + pad;
        need = 2 * (s_CapH + imgH + 2) + pad;
        if (need > availH)                       /* 塞不下就等比缩, 宁小不留黑边 */
        {
            imgH = (availH - pad - 2 * s_CapH - 4) / 2;
            if (imgH < 24) imgH = 24;
            imgW = (int)((long long)imgH * vw / vh);
            if (imgW < 48) imgW = 48;
        }

        s_CapAx = colX; s_CapAy = colTop;
        SetRect(&s_BoxA, colX, colTop + s_CapH, colX + imgW + 2, colTop + s_CapH + imgH + 2);

        s_CapBx = colX; s_CapBy = s_BoxA.bottom + pad;
        SetRect(&s_BoxB, colX, s_CapBy + s_CapH, s_BoxA.right, s_CapBy + s_CapH + imgH + 2);
    }
    else
    {
        s_CapAx = s_CapAy = s_CapBx = s_CapBy = 0;
        SetRect(&s_BoxA, 0, 0, 0, 0);
        SetRect(&s_BoxB, 0, 0, 0, 0);
    }

    /* 主按钮: 位置上面已经定死了, 这里只摆过去 */
    if (btn && bh > 0 && band.right > band.left)
        MoveWindow(btn, band.left, band.top, bw, bh, TRUE);

    /* 重命名 / 载入 / 删除: 截图列下方那块空带, 从上到下排成一列。放不下就整块不画。 */
    if (listId == SS_ID_LIST_SAVE && colW > 0 && bw > 0 && bh > 0)
    {
        int bx   = (int)band.left;
        int bot  = (int)band.top - SS_BTN_GAP;
        int topL = (int)s_BoxB.bottom + SS_COL_GAP;
        int yDel = bot - bh;
        int yLd  = yDel - SS_BTN_GAP - bh;
        int yRn  = yLd - SS_BTN_GAP - bh;

        if (yRn >= topL)
        {
            SetRect(&s_BtnRename, bx, yRn, bx + bw, yRn + bh);
            SetRect(&s_BtnLoad,   bx, yLd, bx + bw, yLd + bh);
            SetRect(&s_BtnDel,    bx, yDel, bx + bw, yDel + bh);
        }
    }

    SS_PlaceBtn(s_HWndRename, &s_BtnRename);
    SS_PlaceBtn(s_HWndLoad, &s_BtnLoad);
    SS_PlaceBtn(s_HWndDel, &s_BtnDel);

    s_LayoutW = s_PanelW;
    s_LayoutH = s_PanelH;
    s_LayoutN++;
    return 1;
}

/* 引擎偶尔会把自己的控件摆回去: 列表位置不对就重排一次。 */
static int SS_LayoutDrift(HWND list)
{
    RECT r;

    if (!s_LayoutW) return 1;
    if (s_PanelW != s_LayoutW || s_PanelH != s_LayoutH) return 1;
    if (!SS_WinRect(list, &r)) return 0;
    if (r.left == s_ListRect.left && r.top == s_ListRect.top &&
        r.right == s_ListRect.right && r.bottom == s_ListRect.bottom) return 0;
    if (r.right - r.left != s_ListRect.right - s_ListRect.left) return 1;
    if (r.bottom - r.top != s_ListRect.bottom - s_ListRect.top) return 1;
    return 0;
}

/* ---------------- 贴图 ---------------- */

static void SS_Blit(void* surf, int dx, int dy,
                    const unsigned short* src, int sw, int sh)
{
    unsigned char* base;
    int pitch, W, H, i, j;

    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W <= 0 || H <= 0 || pitch < W * 2) return;
    if (dx >= W || dy >= H || dx + sw <= 0 || dy + sh <= 0) return;
    base = GtLock(surf);
    if (!base) return;
    for (j = 0; j < sh; j++)
    {
        int sy = dy + j, x0, x1;
        unsigned short* row;
        if (sy < 0 || sy >= H) continue;
        row = (unsigned short*)(base + (size_t)sy * pitch);
        x0 = dx < 0 ? -dx : 0;
        x1 = sw; if (dx + x1 > W) x1 = W - dx;
        for (i = x0; i < x1; i++) row[dx + i] = src[(size_t)j * sw + i];
    }
    GtUnlock(surf);
}

/* ---------------- 界面文字 (语言文件 [SaveShot]) ----------------
 * 引擎自己的按钮文字来自 CSF, 但我们建的三个按钮和截图列的标题都在 DLL 里, 一律走
 * 语言文件 MOExperienceResources\<locale>.ini; 缺项由 Lang 回退到下面的默认值。
 * 只在开对话框时读一次 (GetPrivateProfileString 不进每帧的绘制路径)。 */
#define SS_TXT_CAP 48
static wchar_t s_TxtCapNow[SS_TXT_CAP], s_TxtCapShot[SS_TXT_CAP], s_TxtNoShot[SS_TXT_CAP];
static wchar_t s_TxtLoad[SS_TXT_CAP], s_TxtRename[SS_TXT_CAP], s_TxtRenameOk[SS_TXT_CAP];
static wchar_t s_TxtDelete[SS_TXT_CAP], s_TxtDeleteOk[SS_TXT_CAP];

static void SS_LoadText(void)
{
    Lang_GetW(SS_SECTION, "CaptionNow",   L"当前画面", s_TxtCapNow,   SS_TXT_CAP);
    Lang_GetW(SS_SECTION, "CaptionShot",  L"存档截图", s_TxtCapShot,  SS_TXT_CAP);
    Lang_GetW(SS_SECTION, "NoShot",       L"无截图",   s_TxtNoShot,   SS_TXT_CAP);
    Lang_GetW(SS_SECTION, "BtnRename",    L"重命名",   s_TxtRename,   SS_TXT_CAP);
    Lang_GetW(SS_SECTION, "BtnRenameOk",  L"确认改名", s_TxtRenameOk, SS_TXT_CAP);
    Lang_GetW(SS_SECTION, "BtnLoad",      L"载入",     s_TxtLoad,     SS_TXT_CAP);
    Lang_GetW(SS_SECTION, "BtnDelete",    L"删除",     s_TxtDelete,   SS_TXT_CAP);
    Lang_GetW(SS_SECTION, "BtnDeleteOk",  L"确认删除", s_TxtDeleteOk, SS_TXT_CAP);
}

/* 截图框: 深底 + 一圈亮边, 空的时候看得见框在哪。一次 Lock 画完。 */
static void SS_DrawBox(void* surf, const RECT* b, const SS_Pic* im, const wchar_t* cap,
                       int capx, int capy)
{
    int bw = b->right - b->left, bh = b->bottom - b->top;
    unsigned char* base;
    int pitch, W, H;
    unsigned short fill, edge;

    if (bw < 12 || bh < 12) return;
    if (cap && *cap) SS_PutTextLine(surf, cap, capx, capy, s_CapH, 206, 210, 220);

    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || pitch < W * 2) return;

    fill = Blend16(0, 10, 10, 14, 255);
    edge = Blend16(0, 96, 98, 108, 255);
    base = GtLock(surf);
    if (!base) return;
    SS_FillPx(base, pitch, W, H, b->left, b->top, bw, bh, fill);
    SS_Frame(base, pitch, W, H, b->left, b->top, bw, bh, 1, edge);
    GtUnlock(surf);

    if (im->px && im->w > 0 && im->h > 0)
    {
        SS_Blit(surf, b->left + (bw - im->w) / 2, b->top + (bh - im->h) / 2,
                im->px, im->w, im->h);
    }
    else
    {
        int tw = SS_TextW(s_TxtNoShot);
        if (tw > 0 && tw < bw - 8)
            SS_PutTextLine(surf, s_TxtNoShot, b->left + (bw - tw) / 2,
                           b->top + (bh - s_CapH) / 2, s_CapH, 118, 118, 128);
    }
}

/* 引擎滚动/刷新时会把滚动条摆回列表原来的右缘, 那样又压到截图列。
 * 每帧对一次, 只在真偏了才 MoveWindow (FALSE: 不触发重绘)。 */
static void SS_KeepScroll(HWND list)
{
    HWND sb = s_Scroll;
    RECT lr, sr;

    if (!sb || !IsWindow(sb)) return;
    if (!SS_WinRect(list, &lr) || !SS_WinRect(sb, &sr)) return;
    if (sr.left == lr.right && sr.top == lr.top && sr.bottom == lr.bottom) return;
    MoveWindow(sb, (int)lr.right, (int)lr.top, (int)(sr.right - sr.left),
               (int)(lr.bottom - lr.top), FALSE);
}

/* ---------------- 文字 (跟随 [VecText], 槽位用主字号 2) ---------------- */

#define SS_VEC_SLOT  2
#define SS_ROW_GAP   3      /* 行间空隙 (墨迹之外) */
#define SS_ROW_PAD   3      /* 边框到第一行之间也要留白 */
#define SS_LIST_EDGE 123    /* 存档列表的框线 (灰), 与名字框同色 */

static const wchar_t* SS_Face(void)
{
    const wchar_t* f = VecText_FontFace();
    return (f && *f) ? f : L"msyh.ttc";
}

/* 行距不能用字体 cell (g_G->adv): msyh 的 cell 含 ~6px leading, 按它排行会空出
 * 8~10px。量一次墨迹 (同字号只量一次) 用 inkTop/inkH 排。 */
static int s_InkPx = 0, s_InkTop = 0, s_InkH = 0;

static int SS_MeasureInk(int px)
{
    static const wchar_t kProbe[] = L"国Gg";   /* 汉字 + 带下降部的拉丁 */
    int x, y, w, T, B;

    if (s_InkH > 0 && s_InkPx == px) return 1;
    if (!GdiEnsure(px, VecText_Bold(), SS_VEC_SLOT, SS_Face())) return 0;
    w = RenderLine(kProbe, 3);
    if (w <= 0) return 0;
    T = DIB_H; B = -1;
    for (y = 0; y < DIB_H; y++)
    {
        unsigned char* row = g_G->bits + (size_t)y * g_G->pitch;
        for (x = 0; x < w + 2 && x < DIB_W; x++)
            if (row[x * 4 + 2]) { if (y < T) T = y; if (y > B) B = y; break; }
    }
    if (B < T) return 0;
    s_InkPx = px;
    s_InkTop = T;
    s_InkH = B - T + 1;
    return 1;
}

/* 画一段文字 (调用方已 GdiEnsure 到该槽)。maxW>0 时按宽度裁 u 到 maxW。
 * 返回画出的像素宽 (0 = 没画)。 */
static int SS_TextRun(unsigned char* base, int pitch, int W, int H,
                      const wchar_t* s, int n, int x, int y, int maxW,
                      unsigned r, unsigned g, unsigned b)
{
    int w;

    if (!s || n <= 0) return 0;
    w = RenderLine(s, n);
    if (w <= 0 || g_G->adv <= 0) return 0;
    if (maxW > 0 && w > maxW) w = maxW;
    if (x < 0 || y < 0 || x + w > W || y + g_G->adv > H) return 0;
    BlitCoverage(base, pitch, W, H, x, y, w, g_G->adv, r, g, b, 100);
    return w;
}

static int SS_TextW(const wchar_t* s)
{
    if (!s || !*s) return 0;
    if (!GdiEnsure(VecText_ResolvedPx(), VecText_Bold(), SS_VEC_SLOT, SS_Face())) return 0;
    return RenderLine(s, lstrlenW(s));
}

/* 画一行 (调用方已 Lock)。maxW>0 时按宽度截断, right=1 时 x 是右缘。 */
static void SS_PutText(unsigned char* base, int pitch, int W, int H,
                       const wchar_t* s, int x, int y, int maxW, int right,
                       unsigned r, unsigned g, unsigned b)
{
    int n;

    if (!base || !s || !*s) return;
    if (!GdiEnsure(VecText_ResolvedPx(), VecText_Bold(), SS_VEC_SLOT, SS_Face())) return;
    n = lstrlenW(s);
    if (maxW > 0)
    {
        n = CharsFit(s, n, maxW);
        if (n <= 0) return;
    }
    if (right) x -= SS_TextW(s);
    SS_TextRun(base, pitch, W, H, s, n, x, y, 0, r, g, b);
}

/* 自己 Lock 画一行: 标题/无截图这类零散文字用。墨迹在 lineH 里垂直居中。 */
static void SS_PutTextLine(void* surf, const wchar_t* s, int x, int lineTop, int lineH,
                           unsigned r, unsigned g, unsigned b)
{
    int W, H, pitch, px, y;
    unsigned char* base;

    if (!surf || !s || !*s) return;
    px = VecText_ResolvedPx();
    if (!SS_MeasureInk(px)) return;
    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || pitch < W * 2) return;
    if (lineH < 8) lineH = s_InkH + 2;      /* 排版还没跑过 (s_CapH=0) */
    y = lineTop + (lineH - s_InkH) / 2 - s_InkTop;
    base = GtLock(surf);
    if (!base) return;
    SS_PutText(base, pitch, W, H, s, x, y, 0, 0, r, g, b);
    GtUnlock(surf);
}

/* ---------------- 列表行 ----------------
 * 层级: [面板画进临时面] → [0x622396, present 前最后一跳, 我们在这画] → [上屏]
 *       → [之后 Windows 才派发子窗口的 WM_PAINT]
 * 所以控件自画会晚于我们 (两套文字), 早画又会被面板 blit 盖掉。结论: 控件一个像素
 * 都不画, 行全部由 SS_DrawRows 画进 surf。 */
static WNDPROC s_OldListProc = NULL;
static HWND    s_SubList = NULL;
static HWND    s_RhList = NULL;    /* 已按 [VecText] 字号抬过行高的列表控件 */
static int     s_DrawnCur = -2, s_DrawnTop = -2;   /* 面板上次按哪套画的 */
static unsigned s_LastInv = 0;

/* 选中/滚动和面板上那张不一样就请面板重画 (30ms 节流; 画完 s_Drawn* 更新, 自然收口)。 */
static void SS_RequestPanel(HWND list)
{
    HWND dlg = GetParent(list);
    int cur, top;
    unsigned now;

    if (!dlg || !IsWindow(dlg)) return;
    cur = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
    top = (int)SendMessageA(list, LB_GETTOPINDEX, 0, 0);
    if (cur == s_DrawnCur && top == s_DrawnTop) return;
    now = GetTickCount();
    if (now - s_LastInv < 30) return;
    s_LastInv = now;
    InvalidateRect(dlg, NULL, FALSE);
}

static LRESULT CALLBACK SS_ListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);      /* 必须配对, 否则刷新区不失效 → 反复 WM_PAINT */
        if (dc) EndPaint(h, &ps);
        SS_RequestPanel(h);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;    /* 别让类背景刷把面板面涂掉 */
    if (msg == WM_NCDESTROY)
    {
        /* 销毁通知必须送到刚恢复的那个原过程: s_OldListProc 这时已被清掉 */
        WNDPROC old = s_OldListProc;
        if (s_SubList == h) { s_SubList = NULL; s_OldListProc = NULL; }
        if (old)
        {
            SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)old);
            return CallWindowProcA(old, h, msg, wp, lp);
        }
    }
    return LiCallOld(h, s_OldListProc, msg, wp, lp);
}

/* 换一个列表窗口就重挂一次 (对话框每次打开都是新控件)。 */
static void SS_HookList(HWND list)
{
    WNDPROC old;

    if (!list || !IsWindow(list) || s_SubList == list) return;
    old = (WNDPROC)SetWindowLongPtrA(list, GWLP_WNDPROC, (LONG_PTR)SS_ListProc);
    if (!old) { SS_Log("list subclass FAILED", NULL); return; }
    s_OldListProc = old;
    s_SubList = list;
    s_RhList = NULL;                      /* 新控件: 行高要重抬 */
    s_DrawnCur = s_DrawnTop = -2;
}

/* 把列表客户区画进 surf (屏幕坐标)。左 = 档名, 右 = 完整时间 —— 引擎把日期列宽
 * 写死 (~55px, 永远 "2026..."), 加宽列表没用, 只能整张重画。 */
static void SS_DrawRows(void* surf, HWND list)
{
    RECT cr;
    POINT p;
    int cnt, top, cur, rh, i, x0, y0, x1, y1, W, H, pitch, w, px, adv, tw, maxName;
    int inkTop, inkH, want, iy0;
    unsigned char* base;
    unsigned short cBg, cSel, cEdge;

    if (!list || !surf) return;
    GetClientRect(list, &cr);
    if (cr.right < 60 || cr.bottom < 8) return;
    p.x = 0; p.y = 0;
    ClientToScreen(list, &p);
    x0 = p.x; y0 = p.y;
    x1 = p.x + (int)cr.right;
    y1 = p.y + (int)cr.bottom;

    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || pitch < W * 2) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x1 <= x0 || y1 <= y0) return;
    w = x1 - x0;
    iy0 = y0 + 2 + SS_ROW_PAD;    /* 边框 + 留白, 否则首行墨迹压在上边框上 */

    /* 字体拿不到也要照画底色/边框: 直接 return 会留下面板原像素 (控件已不画了) */
    px = VecText_ResolvedPx();
    if (!SS_MeasureInk(px)) { px = 0; adv = 0; inkTop = 0; inkH = 0; }
    else                   { adv = g_G->adv; inkTop = s_InkTop; inkH = s_InkH; }

    cnt = (int)SendMessageA(list, LB_GETCOUNT, 0, 0);
    top = (int)SendMessageA(list, LB_GETTOPINDEX, 0, 0);
    cur = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
    rh  = (int)SendMessageA(list, LB_GETITEMHEIGHT, 0, 0);

    /* 行高 = 墨迹高 + 空隙, 每个控件只设一次 (每帧设会让控件反复重排) */
    want = (inkH > 0) ? (inkH + SS_ROW_GAP) : (px + 4);
    if (want < 10) want = 10;
    if (rh != want && s_RhList != list)
    {
        SendMessageA(list, LB_SETITEMHEIGHT, 0, (LPARAM)want);
        s_RhList = list;
    }
    rh = want;
    if (top < 0 || top >= cnt) top = 0;

    cBg   = Blend16(0, 0, 0, 0, 255);
    cSel  = Blend16(0, 123, 32, 123, 255);
    cEdge = Blend16(0, SS_LIST_EDGE, SS_LIST_EDGE - 2, SS_LIST_EDGE, 255);

    base = GtLock(surf);
    if (!base) return;

    SS_FillPx(base, pitch, W, H, x0, y0, w, y1 - y0, cBg);
    if (cur >= top && cur < cnt)
    {
        int sy0 = iy0 + (cur - top) * rh, sy1 = sy0 + rh;
        if (sy1 > y1 - 2) sy1 = y1 - 2;
        if (sy0 < y0 + 2) sy0 = y0 + 2;
        SS_FillPx(base, pitch, W, H, x0 + 2, sy0, w - 4, sy1 - sy0, cSel);
    }

    /* 外框: 面板美术里那圈 2px 灰边 (与存档名输入框同色) */
    SS_Frame(base, pitch, W, H, x0, y0, w, y1 - y0, 2, cEdge);

    for (i = top; i < cnt && inkH > 0; i++)
    {
        unsigned fe;
        wchar_t nm[64], dt[28];
        /* y = 这行 DIB 的顶部 (不是墨迹顶): 墨迹在 rh 里居中, 所以回退 inkTop */
        int y = iy0 + (i - top) * rh + (rh - inkH) / 2 - inkTop;

        if (y + adv > y1 - 2) break;

        fe = (unsigned)(size_t)SendMessageA(list, LB_GETITEMDATA, (WPARAM)i, 0);
        SS_RowText(list, fe, i, nm, 64);
        SS_RowTime(fe, dt);

        tw = dt[0] ? SS_TextW(dt) : 0;
        if (tw > 0 && tw < w - 40)
            SS_PutText(base, pitch, W, H, dt, x1 - 4, y, 0, 1, 214, 210, 218);

        if (nm[0] && (maxName = w - 12 - (tw > 0 ? tw + 8 : 0)) > 12)
        {
            int dim = 0;
            if (fe && IsPlausiblePtr((const void*)(size_t)fe))
            {
                if (!*(const unsigned char*)(size_t)(fe + SS_FE_VALID) ||
                    *(const unsigned char*)(size_t)(fe + SS_FE_WRONG))
                    dim = 1;
            }
            if (dim) SS_PutText(base, pitch, W, H, nm, x0 + 5, y, maxName, 0, 128, 126, 134);
            else     SS_PutText(base, pitch, W, H, nm, x0 + 5, y, maxName, 0, 239, 235, 239);
        }
    }

    GtUnlock(surf);
}

/* ---------------- 载入 / 删除 / 重命名 ---------------- */

#define SS_BTN_PAGE_LOAD 1        /* 引擎状态机的页面号: 1=载入 2=储存 3=删除 */
#define SS_MODE_LOAD     0x40F    /* 载入页按钮 id, 也是状态机认的"载入"动作码 */
#define SS_DEL_ARM_MS    4000
#define SS_RN_ARM_MS     120000   /* 改名态: 2 分钟没确认就落回, 免得一直挂着 */

/* 删存档不可逆: 第一下只是"上膛", 4 秒内再点一下才真删 (按钮字会变) */
static int      s_DelArm = 0;
static unsigned s_DelArmTick = 0;

/* 改名态: 第一下把档名送进名字框让用户改, 第二下按框里的字写回文件。
 * s_RnFe 记下是哪一个 FileEntry —— 中间换了选中就不认, 免得改到别人。 */
static int      s_RnArm = 0;
static unsigned s_RnArmTick = 0;
static int      s_RnArmShown = 0;
static unsigned s_RnFe = 0;

static int SS_HasDir(const char* nm)
{
    int i;

    for (i = 0; nm[i]; i++)
        if (nm[i] == '\\' || nm[i] == '/' || nm[i] == ':') return 1;
    return 0;
}

/* 选中那一档的磁盘路径: .sav 与 Thumbs\.bmp。引擎存的文件名可能是相对游戏根的
 * "Saved Games\SAVE0001.SAV", 也可能只有文件名。 */
static int SS_EntryFile(HWND list, char* sav, unsigned savN, char* bmp, unsigned bmpN)
{
    char nm[40], dir[MAX_PATH_LEN], base[SS_NAME_MAX];
    unsigned p;

    if (sav && savN) sav[0] = 0;
    if (bmp && bmpN) bmp[0] = 0;
    if (!list || !sav || !bmp || !SS_SelectedFile(list, nm)) return 0;

    if (SS_HasDir(nm) && nm[0] != '\\' && nm[1] != ':')
    {
        p = AppendCStr(sav, 0, GtGameDir(), savN);
        p = AppendCh(sav, p, '\\', savN);
        AppendCStr(sav, p, nm, savN);
    }
    else if (SS_HasDir(nm)) AppendCStr(sav, 0, nm, savN);
    else
    {
        p = AppendCStr(sav, 0, s_SaveDir, savN);
        p = AppendCh(sav, p, '\\', savN);
        AppendCStr(sav, p, nm, savN);
    }

    SS_SplitPath(sav, dir, base);
    if (!base[0]) return 0;
    SS_ThumbPath(bmp, bmpN, dir, base);
    return 1;
}

/* 借一次"选中变化"让引擎重画面板。
 * 面板的重绘只由引擎自己的控件通知带出来 (外部 InvalidateRect 到不了 0x622396,
 * 而按钮文字能变是因为按钮自己重画自己), 所以改完列表就选到邻居再选回来。 */
static void SS_BumpSel(HWND list, int cur, int cnt)
{
    int other;

    if (!list || cnt <= 1) return;
    other = (cur > 0) ? (cur - 1) : (cur + 1);
    if (other < 0 || other >= cnt || other == cur) return;
    SendMessageA(list, LB_SETCURSEL, (WPARAM)other, 0);
    SendMessageA(list, LB_SETCURSEL, (WPARAM)cur, 0);
}

/* 删除: 自己动手 (.SAV + 缩略图), 再从列表摘掉那一行 —— 人留在存档页,
 * 不用像走引擎删除页那样被弹走。 */
static void SS_DoDelete(HWND dlg, HWND list)
{
    char sav[MAX_PATH_LEN], bmp[MAX_PATH_LEN];
    int cur, n0, n1;
    unsigned now;

    if (!list) return;
    cur = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
    if (cur < 0) { SS_Log("del no sel", NULL); return; }

    /* 空白档位没有文件: 连"上膛"都不做, 否则按钮会一直停在"确认删除"。 */
    if (!SS_EntryFile(list, sav, MAX_PATH_LEN, bmp, MAX_PATH_LEN))
    {
        SS_Log("del no path", NULL);
        s_DelArm = 0;
        return;
    }

    now = GetTickCount();
    if (!s_DelArm || now - s_DelArmTick > SS_DEL_ARM_MS)
    {
        s_DelArm = 1;
        s_DelArmTick = now;
        if (dlg && IsWindow(dlg)) InvalidateRect(dlg, NULL, FALSE);
        return;
    }
    s_DelArm = 0;

    SS_Log(DeleteFileA(sav) ? "del ok " : "del FAIL ", sav);
    if (bmp[0]) DeleteFileA(bmp);

    n0 = (int)SendMessageA(list, LB_GETCOUNT, 0, 0);
    SendMessageA(list, LB_DELETESTRING, (WPARAM)cur, 0);
    n1 = (int)SendMessageA(list, LB_GETCOUNT, 0, 0);
    if (cur >= n1) cur = n1 - 1;
    SendMessageA(list, LB_SETCURSEL, (WPARAM)cur, 0);
    SS_BumpSel(list, cur, n1);    /* 引擎只会因自己的控件变化才重画面板 */

    SS_ThumbReset();              /* 截图框跟着换 (没了就是"无截图") */
    s_DrawnCur = s_DrawnTop = -2;
    if (dlg && IsWindow(dlg))
    {
        InvalidateRect(dlg, NULL, FALSE);
        UpdateWindow(dlg);        /* 立刻走一次 WM_PAINT, 不等下一轮消息 */
    }
    if (n1 >= n0) SS_Log("del: row not removed", NULL);   /* 控件不认 LB_DELETESTRING */
}

static void SS_Forget(int keepLive);

/* ---------------- 重命名: 只改存档里的显示名 ----------------
 * 列表那一行显示的是 `FileEntryClass.Name`, 由 `GetFileEntry 0x559ED0` 从存档的
 * `\x05SummaryInformation` 属性集拷来, 属性 id 2 (`PIDSI_TITLE`) 就是存档时的名字;
 * 引擎同时在根存储里写了一个同内容的流 "Scenario Description", 两处都改才一致。
 * 文件名 (SAVE0001.SAV) 和缩略图都不动。
 *
 * 改的是副本再 ReplaceFile: 中途任何一步失败只多一份 .rnm, 原档不受影响。替换后
 * 还原时间戳 —— 列表按最后修改时间排, 改名不该把存档顶到最前面。 */

/* 两个 GUID 自己写: 用 libuuid 的符号会把整个 uuid.o (几千个 IID/PKEY 常量,
 * 约 64KB .rdata + 400KB 符号表) 拖进 DLL。值与系统一致 (已逐字节核对)。 */
static const GUID kIID_PSS =
    {0x0000013A, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kFMTID_Sum =
    {0xF29F85E0, 0x4FF9, 0x1068, {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}};

static int SS_RenameSave(const char* sav, const wchar_t* nm)
{
    char tmp[MAX_PATH_LEN];
    wchar_t wTmp[MAX_PATH_LEN];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    IStorage* stg = NULL;
    IPropertySetStorage* pss = NULL;
    IPropertyStorage* ps = NULL;
    IStream* st = NULL;
    unsigned p;
    int ok = 0;

    p = AppendCStr(tmp, 0, sav, MAX_PATH_LEN);
    AppendCStr(tmp, p, ".rnm", MAX_PATH_LEN);
    MultiByteToWideChar(CP_ACP, 0, tmp, -1, wTmp, MAX_PATH_LEN);
    GetFileAttributesExA(sav, GetFileExInfoStandard, &fad);

    if (!CopyFileA(sav, tmp, FALSE))
    { SS_Log("rename: copy fail", NULL); return 0; }

    if (FAILED(StgOpenStorage(wTmp, NULL, STGM_READWRITE | STGM_SHARE_EXCLUSIVE,
                              NULL, 0, &stg)))
    { SS_Log("rename: open fail", NULL); DeleteFileA(tmp); return 0; }

    /* 两个 Open 的 grfMode 都必须带 SHARE_EXCLUSIVE: 只给 STGM_READ 会返
     * STG_E_INVALIDFLAG (0x800300FF)。 */
    if (SUCCEEDED(stg->QueryInterface(kIID_PSS, (void**)&pss)))
    {
        if (SUCCEEDED(pss->Open(kFMTID_Sum,
                                STGM_READWRITE | STGM_SHARE_EXCLUSIVE, &ps)))
        {
            PROPSPEC spec;
            PROPVARIANT pv;

            spec.ulKind = PRSPEC_PROPID;
            spec.propid = 2;
            PropVariantInit(&pv);
            pv.vt = VT_LPWSTR;
            pv.pwszVal = (LPWSTR)nm;
            ok = SUCCEEDED(ps->WriteMultiple(1, &spec, &pv, 0)) &&
                 SUCCEEDED(ps->Commit(0));
            ps->Release();
        }
        pss->Release();
    }

    if (ok && SUCCEEDED(stg->OpenStream(L"Scenario Description", NULL,
                                        STGM_READWRITE | STGM_SHARE_EXCLUSIVE, 0, &st)))
    {
        ULARGE_INTEGER sz;
        LARGE_INTEGER z;
        ULONG cb = (ULONG)(((size_t)lstrlenW(nm) + 1) * sizeof(wchar_t)), wr = 0;

        sz.QuadPart = cb;
        z.QuadPart = 0;
        if (FAILED(st->SetSize(sz)) || FAILED(st->Seek(z, STREAM_SEEK_SET, NULL)) ||
            FAILED(st->Write(nm, cb, &wr)))
            ok = 0;
        st->Release();
    }

    stg->Release();                  /* 必须关掉, 否则 ReplaceFile 拿不到独占 */

    if (!ok || !ReplaceFileA(sav, tmp, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL))
    {
        SS_Log("rename: write fail", NULL);
        DeleteFileA(tmp);
        return 0;
    }

    {
        HANDLE fh = CreateFileA(sav, FILE_WRITE_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE)
        {
            SetFileTime(fh, &fad.ftCreationTime, &fad.ftLastAccessTime, &fad.ftLastWriteTime);
            CloseHandle(fh);
        }
    }
    return 1;
}

/* 载入: 不自己调 LoadGame (要接屏幕栈/载入画面, 半路调必崩), 而是把状态机的"页面/
 * 动作"两字段改成用户点载入页时的样子, 让它下一帧按自己的流程走完。
 *
 *   ctx = GetWindowLong(dlg, DWLP_USER)   对话框过程自己也从这里取
 *   ctx+0x04 = 页面 (1 载入 / 2 储存 / 3 删除)
 *   ctx+0x18 = 动作 (-1 空闲, 1 本页按钮, 0x40F 载入, 2 返回)
 *
 * 空闲值是 -1 不是 0。动作分派 (0x559030) 只按**页面**决定做什么, 动作值只过"是不是
 * 本页按钮"那道门 ⇒ 必须同时把页面改成 1, 否则就是在存档页上执行一次存档。 */
static void SS_DoLoad(HWND dlg, HWND list)
{
    void* ctx;
    char sav[MAX_PATH_LEN], bmp[MAX_PATH_LEN];

    s_DelArm = 0;
    if (!list) return;
    if ((int)SendMessageA(list, LB_GETCURSEL, 0, 0) < 0)
    { SS_Log("load no sel", NULL); return; }

    /* 空白档位 ([空白 栏位]) 没有文件名: 交给引擎会去 LoadGame 一个不存在的文件,
     * 那条失败路径把屏幕栈拆掉, 世界没了而面板还在画 = 花屏卡死。没有真文件就一步
     * 都不走。 */
    if (!SS_EntryFile(list, sav, MAX_PATH_LEN, bmp, MAX_PATH_LEN) ||
        GetFileAttributesA(sav) == INVALID_FILE_ATTRIBUTES)
    {
        SS_Log("load blank slot", NULL);
        return;
    }

    ctx = SS_Ctx(dlg);
    if (!ctx) { SS_Log("load no ctx", NULL); return; }
    if (*(int*)((char*)ctx + 4) != 2 || *(int*)((char*)ctx + 0x18) != -1)
    {
        char buf[80];
        unsigned p = AppendCStr(buf, 0, "saveshot: load skip page=", sizeof(buf));
        p = AppendInt(buf, p, sizeof(buf), *(int*)((char*)ctx + 4));
        p = AppendCStr(buf, p, " mode=", sizeof(buf));
        AppendInt(buf, p, sizeof(buf), *(int*)((char*)ctx + 0x18));
        LogLine(buf);
        return;
    }

    /* 对话框保持原样 (不隐藏): 外层收尾依赖状态机最后把它正常销毁。
     * 这期间可能撞上 UI 主题资源已被 LoadGame 释放的重绘, 由 0x6071E0 守卫兜。 */
    s_LoadOldDlg = dlg;
    SS_Forget(0);
    s_Loading = GetTickCount(); if (!s_Loading) s_Loading = 1;

    *(int*)((char*)ctx + 4)    = SS_BTN_PAGE_LOAD;
    *(int*)((char*)ctx + 0x18) = SS_MODE_LOAD;
    /* 外层是存档命令 (0x51F), 收尾会把菜单重新显示出来; 立标志让 0x4F14BF 的桩
     * 改走载入的收尾, 菜单才不会在载入完成后又冒出来。 */
    s_TailLoad = 1;
    SS_Log("load asked", NULL);
}

/* ---------------- 存档页的两个按钮 ----------------
 * 用引擎原生美术: 在对话框 WM_INITDIALOG 之前建 BS_OWNERDRAW 子窗口, 引擎会挂子类
 * 并注册进 0xAC1B00 表; 之后从"储存"按钮的 ownerdraw 元素复制 +0xB0 (底图 band)
 * 与字体。所以 0x558B90 必须被换掉 —— 只换 DWLP_DLGPROC 拿不到那个时刻。 */

/* 0xAC1B00: HWND → ownerdraw 元素表, 表项 { HWND, elem, ..., next@+0x204 }。
 * 只读查找, 找不到返回 NULL。 */
static unsigned* SS_OwnerDrawElem(HWND h)
{
    void** buckets;
    unsigned log2, n, i, guard;

    if (!h) return NULL;
    buckets = *(void***)0xAC1B00u;
    log2 = *(unsigned*)0xAC1B0Cu;
    if (!buckets || log2 > 14) return NULL;
    n = 1u << log2;
    for (i = 0; i < n; ++i)
    {
        unsigned char* e = (unsigned char*)buckets[i];
        for (guard = 0; e && guard < 4096; ++guard)
        {
            if (*(HWND*)e == h) return (unsigned*)(e + 4);
            e = *(unsigned char**)(e + 0x204);
        }
    }
    return NULL;
}

/* 按钮文字存在 ownerdraw 元素的某个字段 (引擎自己 malloc 的宽串副本), 绘制时直接
 * 读它, 不读窗口文本。偏移没有文档, 打开对话框时用文本已知的"储存"按钮探一次:
 * 扫 +0x04..+0x200 的 DWORD, 找指向 L"储存" 的那个。 */
static int s_TxtOff = -2;              /* -2 未探过, -1 探失败, 其它 = 字段偏移 */

static int SS_ProbeTextOff(void* obj, const wchar_t* want)
{
    int k;

    if (!obj || !IsPlausiblePtr(obj)) return -1;
    for (k = 0x04; k <= 0x200; k += 4)
    {
        unsigned v = *(unsigned*)((char*)obj + k);
        if (v < 0x10000u || (v & 1u) || !IsPlausiblePtr((const void*)(size_t)v)) continue;
        if (MemReadableBytes((const void*)(size_t)v, 8) >= 8 &&
            !lstrcmpW((const wchar_t*)(size_t)v, want)) return k;
    }
    return -1;
}

/* 改按钮文字。换字必须给**引擎自己 malloc 的副本** (0x7C8E17 是它用的 malloc),
 * 因为按钮销毁时引擎会 free 这个字段 —— 指到 DLL 里的静态串就会崩。旧块直接丢弃
 * 泄漏, 几十字节换零风险。探不到偏移就什么都不做 (字不变, 但不会写坏内存)。 */
static void SS_SetBtnText(HWND btn, const wchar_t* s)
{
    typedef void* (__cdecl *SS_MallocFn)(size_t);
    void* e;
    wchar_t* d;
    int n;

    if (!btn || !IsWindow(btn) || !s || s_TxtOff < 0) return;
    e = SS_OwnerDrawElem(btn);
    if (!e || !IsPlausiblePtr(e)) return;
    n = lstrlenW(s);
    d = (wchar_t*)((SS_MallocFn)0x7C8E17u)(((size_t)n + 1) * sizeof(wchar_t));
    if (!d) return;
    memcpy(d, s, ((size_t)n + 1) * sizeof(wchar_t));
    *(wchar_t**)((char*)e + s_TxtOff) = d;
    InvalidateRect(btn, NULL, TRUE);
}

/* 把字体和底图从"储存"按钮搬给新按钮, 顺便探出文字字段偏移。 */
static void SS_StyleButtons(HWND cont)
{
    HWND mine[3];
    unsigned* ec;
    HFONT f;
    int k, n = 0;

    if (!cont) return;
    if (s_HWndRename && IsWindow(s_HWndRename)) mine[n++] = s_HWndRename;
    if (s_HWndLoad && IsWindow(s_HWndLoad)) mine[n++] = s_HWndLoad;
    if (s_HWndDel  && IsWindow(s_HWndDel))  mine[n++] = s_HWndDel;
    if (!n) return;

    f = (HFONT)SendMessageW(cont, WM_GETFONT, 0, 0);
    if (f)
        for (k = 0; k < n; k++) SendMessageW(mine[k], WM_SETFONT, (WPARAM)f, TRUE);

    ec = SS_OwnerDrawElem(cont);
    if (!ec) { SS_Log("native buttons no elem (plain look)", NULL); return; }
    for (k = 0; k < n; k++)
    {
        unsigned* eb = SS_OwnerDrawElem(mine[k]);
        if (eb) *(unsigned*)((char*)eb + 0xB0) = *(unsigned*)((char*)ec + 0xB0);
        InvalidateRect(mine[k], NULL, TRUE);
    }
    SS_Log("native buttons +band", NULL);

    SS_LoadText();
    s_TxtOff = SS_ProbeTextOff((char*)ec, L"储存");
    if (s_TxtOff < 0) { SS_Log("btn text off not found (字不会变)", NULL); return; }
    /* 三个按钮一起换成语言文件的字: 只换其中一个会让两个来源混着显示。 */
    SS_SetBtnText(s_HWndRename, s_TxtRename);
    SS_SetBtnText(s_HWndLoad,   s_TxtLoad);
    SS_SetBtnText(s_HWndDel,    s_TxtDelete);
}

/* 存档页对话框过程 (0x558B90 的替身)。stdcall: 原过程结尾 ret 0x10, 我们原样转。 */
static INT_PTR __stdcall SS_SaveDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    typedef INT_PTR (__stdcall *DlgProcFn)(HWND, UINT, WPARAM, LPARAM);

    if (msg == WM_INITDIALOG)
    {
        INT_PTR r;
        HWND cont = GetDlgItem(h, SS_ID_BTN_SAVE);
        RECT rc;

        s_HWndLoad = s_HWndDel = s_HWndRename = NULL;
        s_DelArm = 0;
        s_DelArmShown = 0;
        s_RnArm = 0;
        s_RnArmShown = 0;
        /* 必须在原过程之前建: 引擎的 WM_INITDIALOG 会遍历子控件, 只有它接管的
         * 按钮才会用原版美术画。窗口文本只是给引擎初始化查 StringTable 的占位,
         * 真正的字在 SS_StyleButtons 里按语言文件设。初始坐标放到窗口外, 等
         * SS_Layout 算好再摆 (否则会临时压在"储存"上)。 */
        if (cont && !GetDlgItem(h, SS_ID_BTN_LOAD_NEW) && GetWindowRect(cont, &rc))
        {
            int w = (int)(rc.right - rc.left), hh = (int)(rc.bottom - rc.top);
            s_HWndRename = CreateWindowExW(0, L"Button", L"",
                                           WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                           -2000, -2000, w, hh,
                                           h, (HMENU)(INT_PTR)SS_ID_BTN_RN_NEW,
                                           GetModuleHandleW(NULL), NULL);
            s_HWndLoad = CreateWindowExW(0, L"Button", L"GUI:Load",
                                         WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                         -2000, -2000, w, hh,
                                         h, (HMENU)(INT_PTR)SS_ID_BTN_LOAD_NEW,
                                         GetModuleHandleW(NULL), NULL);
            s_HWndDel = CreateWindowExW(0, L"Button", L"GUI:Delete",
                                        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                        -2000, -2000, w, hh,
                                        h, (HMENU)(INT_PTR)SS_ID_BTN_DEL_NEW,
                                        GetModuleHandleW(NULL), NULL);
            SS_Log((s_HWndLoad && s_HWndDel && s_HWndRename) ? "native buttons created"
                                                             : "native buttons FAILED", NULL);
        }
        r = s_OrigSaveDlg ? ((DlgProcFn)s_OrigSaveDlg)(h, msg, wp, lp) : FALSE;
        SS_StyleButtons(cont);
        return r;
    }

    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED)
    {
        int id = LOWORD(wp);
        if (id == SS_ID_BTN_RN_NEW)
        {
            SS_DoRename(h, GetDlgItem(h, SS_ID_LIST_SAVE));
            return TRUE;
        }
        if (id == SS_ID_BTN_LOAD_NEW)
        {
            SS_DoLoad(h, GetDlgItem(h, SS_ID_LIST_SAVE));
            return TRUE;
        }
        if (id == SS_ID_BTN_DEL_NEW)
        {
            SS_DoDelete(h, GetDlgItem(h, SS_ID_LIST_SAVE));
            return TRUE;
        }
    }

    if (msg == WM_NCDESTROY)
    {
        s_DelArm = 0;
        s_DelArmShown = 0;
        s_RnArm = 0;
        s_RnArmShown = 0;
        /* 只有"按载入跑完"才保留收尾标志: 读档成功时 ctx+0x18 还是 0x40F
         * (0x5594AE 用它当返回值), 取消/失败会复位成 -1 —— 那时必须清掉,
         * 否则下一次存档或删除会被 0x4F14BF 的桩误当成载入收尾。 */
        if (s_TailLoad)
        {
            void* ctx = SS_Ctx(h);
            if (!(ctx && *(int*)((char*)ctx + 0x18) == SS_MODE_LOAD))
            {
                s_TailLoad = 0;
                SS_Log("load tail dropped", NULL);
            }
        }
    }

    if (s_OrigSaveDlg) return ((DlgProcFn)s_OrigSaveDlg)(h, msg, wp, lp);
    return FALSE;
}

/* ---------------- 存档名输入框 (自绘 + 接管编辑) ----------------
 * 引擎 NewEdit 只认 WM_CHAR: 没有光标移动/选中/Home/End/Delete, 不认 Ctrl+C/X/V/A,
 * 也不给输入法锚点。这里只补"编辑 + 绘制", 别的什么都不碰:
 *   - **文字的唯一真相在引擎控件里** (存档执行段读的就是它): 0x4B3 读、0x4B2 写回,
 *     本地只留光标/选区/滚动。0x4B2 会把待执行动作顶成 1 (存档), 写完放回 -1。
 *   - 控件一个像素都不画, 框/字/选中/光标/组字串全由 SS_EdDraw 画在 0x622396 上。
 *   - **对话框过程一行不改**: 命令/通知/焦点原样交给引擎, 替引擎做决定会把它自己的
 *     命令吞掉, 表现为界面点不动。
 */

#define SS_ED_BUF     96
#define SS_ED_PAD     5         /* 文字距框内沿 */
#define SS_ED_EDGE_R  168       /* 自绘框边框: 紫, 与引擎原生的灰边区分 */
#define SS_ED_EDGE_G  92
#define SS_ED_EDGE_B  235
#define SS_ED_GETTEXT 0x4B3u    /* 引擎控件: 取文字 (wParam = 字数, lParam = wchar_t*) */
#define SS_ED_SETTEXT 0x4B2u    /* 引擎控件: 设文字 (lParam = wchar_t*) */

static HWND    s_Ed = NULL;
static WNDPROC s_EdOld = NULL;
/* 文字真相在引擎控件; 会话状态 (光标/选区/滚动/闪烁) 在 LiEdit, 每次编辑
 * 前 SS_EdLoad 拉、改完 SS_EdFlush 写回。 */
static wchar_t s_EdText[SS_ED_BUF];
static LiEdit  s_EdSt;
static wchar_t s_EdComp[SS_ED_BUF];     /* 输入法组字串 */
static int     s_EdCompLen = 0, s_EdCompCur = 0;
static unsigned s_EdImeResT = 0;        /* 收编 GCS_RESULTSTR 的时刻 (吃掉系统回声) */
static int     s_EdBeat = 0;            /* 心跳线程已起 (光标闪 + 候选窗节拍) */
static int     s_EdSurfW = 0, s_EdSurfH = 0;   /* 面的尺寸 (输入法换算要用) */
static void SS_EdDetach(void);

/* ---- 与引擎控件之间同步文字 ---- */

/* 读引擎那份文字, 返回长度。A 版: 这是引擎注册的 ANSI 控件, 消息体里的宽串指针
 * 原样过 (不做字符集转换)。 */
static int SS_EdText(wchar_t* out)
{
    out[0] = 0;
    if (!s_Ed || !IsWindow(s_Ed)) return 0;
    SendMessageA(s_Ed, SS_ED_GETTEXT, (WPARAM)SS_ED_BUF, (LPARAM)out);
    out[SS_ED_BUF - 1] = 0;
    return lstrlenW(out);
}

/* 0x4B2 之后动作码会被引擎写成 1 (存档), 这里放回空闲 -1。
 * 只清这一个值: 载入(0x40F)/上一页(2) 都不是我们造成的, 一律不碰 ——
 * 那会取消用户刚点的东西。 */
static void SS_EdClearAct(void)
{
    void* ctx;

    if (!s_Ed || !IsWindow(s_Ed)) return;
    ctx = SS_Ctx(GetParent(s_Ed));
    if (ctx && *(int*)((char*)ctx + 0x18) == 1)
        *(int*)((char*)ctx + 0x18) = -1;
}

/* 请引擎重画对话框 (InvalidateRect 还不够, 这个对话框的 WM_PAINT 不会自己冒出来,
 * 必须跟一次 UpdateWindow, 与 Esc_Redraw 同一套)。画面上的字/光标都由 SS_EdDraw
 * 在 0x622396 里画, 对话框不重画就还停在上一帧。 */
static void SS_EdRepaint(void)
{
    HWND dlg;

    if (!s_Ed || !IsWindow(s_Ed)) return;
    dlg = GetParent(s_Ed);                  /* 直接子控件的父 = 对话框 */
    if (!dlg || !IsWindow(dlg)) return;
    InvalidateRect(dlg, NULL, FALSE);
    UpdateWindow(dlg);
}

/* 改完的文字交回引擎, 再请一次重画。 */
static void SS_EdCommit(const wchar_t* s)
{
    if (!s_Ed || !IsWindow(s_Ed)) return;
    SendMessageA(s_Ed, SS_ED_SETTEXT, 0, (LPARAM)s);
    SS_EdClearAct();
    SS_EdRepaint();
}

/* 焦点在不在名字框。跨线程不能问 GetFocus() —— 那是"调用线程"的焦点, 心跳线程里
 * 恒为 NULL; 退到前台线程的 GUITHREADINFO。 */
static int SS_EdFocused(void)
{
    GUITHREADINFO gi;

    if (!s_Ed || !IsWindow(s_Ed)) return 0;
    if (GetFocus() == s_Ed) return 1;
    ZeroMemory(&gi, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (!GetGUIThreadInfo(0, &gi)) return 0;       /* 0 = 前台线程 */
    if (gi.hwndFocus) return gi.hwndFocus == s_Ed;
    return gi.hwndActive == s_Ed;
}

/* 心跳: 这个对话框里 SetTimer 不响, 画面刷新是唯一节拍, 而画面只在我们主动置脏时
 * 才刷新 —— 光标闪烁/组字串/候选窗就都没人推了。120ms 的后台线程只做一件事:
 * 名字框有焦点时置脏。 */
static DWORD WINAPI SS_EdBeatProc(LPVOID unused)
{
    (void)unused;
    for (;;)
    {
        Sleep(120);
        if (!SS_EdFocused()) continue;
        SS_EdRepaint();
    }
    return 0;
}

static void SS_EdBeatStart(void)
{
    HANDLE h;

    if (s_EdBeat) return;
    s_EdBeat = 1;
    h = CreateThread(NULL, 0, SS_EdBeatProc, NULL, 0, NULL);
    if (h) CloseHandle(h);                  /* 线程自跑, 句柄不留 */
}

/* ---- 编辑 (插入/删除/剪贴板/几何在 LineEdit.h 的会话层) ---- */

/* 引擎那份文字拉进本地会话 (真相在引擎, 每次编辑前拉一遍)。 */
static void SS_EdLoad(void)
{
    s_EdSt.len = SS_EdText(s_EdSt.buf);
    if (s_EdSt.cur > s_EdSt.len) s_EdSt.cur = s_EdSt.len;
}

/* 改完的文字写回引擎, 组字串断掉, 请一次重画。 */
static void SS_EdFlush(void)
{
    s_EdSt.buf[s_EdSt.len] = 0;
    s_EdCompLen = 0;
    SS_EdCommit(s_EdSt.buf);
}

/* 在插入点插入 k 个字符 (选区先删掉)。整段一次写回, 免得每个字都触发一次通知。 */
static void SS_EdInsertStr(const wchar_t* src, int k)
{
    if (!s_Ed || !IsWindow(s_Ed)) return;
    SS_EdLoad();
    LiEdInsert(&s_EdSt, src, k, SS_ED_BUF - 1, NULL, NULL);
    SS_EdFlush();
}

/* 退格 / Delete: 有选中删选中, 否则删插入点前/后一个字符。 */
static void SS_EdErase(int forward)
{
    if (!s_Ed || !IsWindow(s_Ed)) return;
    SS_EdLoad();
    LiEdErase(&s_EdSt, forward);
    SS_EdFlush();
}

static void SS_EdCopy(int cut)
{
    if (!s_Ed || !IsWindow(s_Ed)) return;
    SS_EdLoad();
    if (LiEdCopy(&s_EdSt, cut) && cut) SS_EdFlush();
}

static void SS_EdPaste(void)
{
    if (!s_Ed || !IsWindow(s_Ed)) return;
    SS_EdLoad();
    if (LiEdPaste(&s_EdSt, SS_ED_BUF - 1, NULL, NULL)) SS_EdFlush();
}

/* ---- 版面: 逐字宽度 / 横向滚动 / 插入点像素 ----
 * 调用前必须已 GdiEnsure 到 [VecText] 那一槽。xs[] 至少 SS_ED_BUF+1。 */
static int SS_EdLayout(int n, int fieldW, int* xs, int* pStart)
{
    int cur;

    if (s_EdSt.cur > n) s_EdSt.cur = n;
    LiCharXs(s_EdSt.buf, n, VecText_ResolvedPx(), xs);
    cur = xs[s_EdSt.cur];
    s_EdSt.view = LiScrollFit(cur, s_EdSt.view, fieldW);
    if (s_EdSt.view > xs[n]) s_EdSt.view = xs[n];
    *pStart = LiFirstVis(xs, n, s_EdSt.view);
    return cur;
}

static int SS_EdFieldW(void)
{
    RECT r;

    if (!s_Ed || !IsWindow(s_Ed) || !GetClientRect(s_Ed, &r)) return 320;
    return ((int)(r.right - r.left) - 2 * SS_ED_PAD > 16)
        ? (int)(r.right - r.left) - 2 * SS_ED_PAD : 16;
}

static int SS_EdCaretPx(int n)
{
    int xs[SS_ED_BUF + 1], start = 0;

    if (!GdiEnsure(VecText_ResolvedPx(), VecText_Bold(), SS_VEC_SLOT, SS_Face()))
        return SS_ED_PAD;
    SS_EdLayout(n, SS_EdFieldW(), xs, &start);
    return SS_ED_PAD + LiCaretX(xs, n, s_EdSt.cur, s_EdSt.view);
}

/* 插入符位置: 宽高用的都是引擎建的那个 (宽 1px), 这里只挪位置。 */
static void SS_EdCaretSync(void)
{
    RECT cr;
    int n, x, y;

    if (!SS_EdFocused()) return;
    if (!GetClientRect(s_Ed, &cr)) return;
    n = SS_EdText(s_EdSt.buf);
    x = SS_EdCaretPx(n);
    if (x > (int)(cr.right - cr.left) - 4) x = (int)(cr.right - cr.left) - 4;
    y = ((int)(cr.bottom - cr.top) - VecText_ResolvedPx()) / 2;
    SetCaretPos((x < 0) ? 0 : x, (y < 0) ? 0 : y);
}

/* ---- IME ----
 * 位置换算: surface 坐标 * 呈现区尺寸 / surface 尺寸。两条硬约束:
 *   1) 上下文取**主窗**的, 不取名字框的 —— 子控件客户区原点在屏幕中间, 拿它当基准
 *      时输入法会把窗口原点再叠一次, 候选窗直接飞出屏幕。
 *   2) 每帧重推 (值没变就跳过): 只推一次的话, 系统下一次按键又会按插入符改回去。
 * 两份上下文各按自己的基准推 (主窗给主窗坐标, 名字框给它自己的客户区坐标)。
 */

static HWND SS_GameHwnd(void)
{
    HWND h;

    if (!IsPlausiblePtr((void*)ADDR_GAME_HWND)) return NULL;
    h = *(HWND*)ADDR_GAME_HWND;
    return (h && IsWindow(h)) ? h : NULL;
}

/* 呈现区尺寸 (0 = 还没量到) 与上次推给输入法的位置。 */
static int s_ImeVW = 0, s_ImeVH = 0;
static int s_ImeX = -1, s_ImeY = -1, s_ImeYC = -1;
static unsigned s_ImePushT = 0;

/* 量呈现区: 算法在 LineEdit.h 的 LiPresent (与聊天框同一份)。存档页无性能
 * 压力, 每帧量; 尺寸没变直接返回, 变了作废上次推的 IME 位置。 */
static void SS_MeasureView(void)
{
    int vw, vh;

    if (!LiPresent(SS_GameHwnd(), -1, &vw, &vh)) return;
    if (vw == s_ImeVW && vh == s_ImeVH) return;
    s_ImeVW = vw;
    s_ImeVH = vh;
    s_ImeX = s_ImeY = s_ImeYC = -1;      /* 基准变了, 上次推的作废 */
}

/* 取名字框的输入上下文。正常属于它自己; 万一没有 (控件没进 IMM) 退到顶层窗口 ——
 * 拿不到上下文的后果是整条组字链路静默失效, 所以要有兜底, 也要能看见。
 * *pw 回填真正持有上下文的窗口, 释放时要用它。 */
static HIMC SS_EdHimc(HWND* pw)
{
    static int warned = 0;
    HIMC imc;
    HWND root;

    *pw = NULL;
    if (!s_Ed || !IsWindow(s_Ed)) return NULL;
    imc = ImmGetContext(s_Ed);
    if (imc) { *pw = s_Ed; return imc; }
    root = GetAncestor(s_Ed, GA_ROOT);
    if (root && root != s_Ed && (imc = ImmGetContext(root)) != NULL)
    {
        *pw = root;
        return imc;
    }
    if (!warned) { warned = 1; SS_Log("ime ctx NULL", NULL); }
    return NULL;
}

/* 把候选窗钉在输入框**正下方**, 组合窗锚在框内同一行。 */
static void SS_EdImePlace(void)
{
    HWND dlg, w;
    HIMC imc;
    RECT r;
    POINT pt[2];
    int n, x0, y0, bw, bh, cx, cy, cyc, lineH, lx, ly, lyc;

    if (!s_Ed || !IsWindow(s_Ed)) return;
    if (s_EdSurfW < 100 || s_EdSurfH < 100) return;      /* 还没画过, 没有基准 */
    SS_MeasureView();
    if (s_ImeVW < 100 || s_ImeVH < 100) return;

    if (!GetWindowRect(s_Ed, &r)) return;
    dlg = GetParent(s_Ed);
    pt[0].x = r.left;  pt[0].y = r.top;
    pt[1].x = r.right; pt[1].y = r.bottom;
    if (dlg && IsWindow(dlg)) MapWindowPoints(NULL, dlg, pt, 2);
    x0 = pt[0].x;
    y0 = pt[0].y;
    bw = pt[1].x - x0;
    bh = pt[1].y - y0;
    if (bw < 8 || bh < 4) return;

    n = SS_EdText(s_EdSt.buf);
    lx = SS_EdCaretPx(n);                                /* 插入点在框内的 x */
    lineH = VecText_ResolvedPx() + 2;
    cx = (x0 + lx) * s_ImeVW / s_EdSurfW;
    cy = (y0 + bh + 2) * s_ImeVH / s_EdSurfH;            /* 框底外沿: 别盖住字 */
    cyc = cy - lineH * s_ImeVH / s_EdSurfH;
    if (cx < 0) cx = 0;
    if (cyc < 0) cyc = 0;
    if (cyc > cy) cyc = cy;
    if (cx > s_ImeVW - 40) cx = s_ImeVW - 40;
    if (cy > s_ImeVH - 40) cy = s_ImeVH - 40;

    /* 名字框自己那份上下文用它自己的客户区坐标 (原点在框左上角)。 */
    ly = bh;
    lyc = ly - lineH;
    if (lx > bw - 4) lx = bw - 4;
    if (lx < 0) lx = 0;
    if (lyc < 0) lyc = 0;
    if (lyc > ly) lyc = ly;

    /* 位置没变就不重推 (推太勤候选窗会闪), 但最多憋 400ms 也要再钉一次。 */
    if (cx == s_ImeX && cy == s_ImeY && cyc == s_ImeYC &&
        (unsigned)(GetTickCount() - s_ImePushT) < 400) return;
    s_ImeX = cx; s_ImeY = cy; s_ImeYC = cyc; s_ImePushT = GetTickCount();

    w = SS_GameHwnd();
    if (w && (imc = ImmGetContext(w)) != NULL)
    {
        LiImePin(imc, cx, cyc, cx, cy);
        ImmReleaseContext(w, imc);
    }
    imc = SS_EdHimc(&w);
    if (imc)
    {
        LiImePin(imc, lx, lyc, lx, ly);
        ImmReleaseContext(w, imc);
    }
}

static int SS_EdComposing(void)
{
    HWND w;
    HIMC imc;

    if (!s_Ed || !IsWindow(s_Ed)) return 0;
    imc = SS_EdHimc(&w);
    if (!imc) return s_EdCompLen > 0;    /* 拿不到上下文: 退到本地缓存判断 */
    return LiComposing(w);
}

static void SS_EdImePoll(void)
{
    HWND w;
    HIMC imc;

    if (!SS_EdFocused())
    {
        s_EdCompLen = 0;
        s_EdCompCur = 0;
        return;
    }
    SS_EdImePlace();                     /* 每帧钉一次候选窗 */
    imc = SS_EdHimc(&w);
    if (!imc) { s_EdCompLen = s_EdCompCur = 0; return; }
    s_EdCompLen = LiCompRead(imc, s_EdComp, SS_ED_BUF, &s_EdCompCur);
    ImmReleaseContext(w, imc);
}

/* ---- 绘制: 在 0x622396 那张整屏面上, 坐标是对话框客户区 ---- */
static void SS_EdDraw(void* surf)
{
    RECT r;
    POINT pt[2];
    HWND dlg;
    int W, H, pitch, x0, y0, bw, bh, px, inkTop, inkH, adv, fieldW, textL, textR;
    int xs[SS_ED_BUF + 1], start = 0, n, cur, lo, hi, ty, i, w;
    unsigned char* base;
    unsigned short cEdge, cFill;
    int focus, caret;

    if (!s_Ed || !IsWindow(s_Ed) || !IsWindowVisible(s_Ed)) return;
    if (!surf || !GetWindowRect(s_Ed, &r)) return;
    /* 位置直接取控件矩形 (我们的排版会把控件挪走), 转成对话框客户区坐标 */
    dlg = GetParent(s_Ed);
    pt[0].x = r.left;  pt[0].y = r.top;
    pt[1].x = r.right; pt[1].y = r.bottom;
    if (dlg && IsWindow(dlg)) MapWindowPoints(NULL, dlg, pt, 2);
    SetRect(&r, pt[0].x, pt[0].y, pt[1].x, pt[1].y);

    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || pitch < W * 2) return;
    s_EdSurfW = W;               /* 输入法换算基准: 呈现区 = 面尺寸 * 视图 / 面 */
    s_EdSurfH = H;
    x0 = (int)r.left;
    y0 = (int)r.top;
    bw = (int)(r.right - r.left);
    bh = (int)(r.bottom - r.top);
    if (bw < 16 || bh < 10) return;
    /* 边框画在控件矩形**外侧** 2px: 内容区就是控件本身, 文字与原生框同一基线 */
    if (x0 < 2 || y0 < 2 || x0 + bw + 2 > W || y0 + bh + 2 > H) return;

    fieldW = bw - 2 * SS_ED_PAD;
    if (fieldW < 16) fieldW = 16;
    textL = x0 + SS_ED_PAD;
    textR = x0 + bw - SS_ED_PAD;

    px = VecText_ResolvedPx();
    if (!SS_MeasureInk(px)) { inkTop = 0; inkH = (px > 0) ? px : 14; }
    else { inkTop = s_InkTop; inkH = s_InkH; }
    adv = inkH + 2;

    base = GtLock(surf);
    if (!base) return;

    /* 2px 紫边 + 深底: 控件自己一个像素不画, 这块整片由我们铺满 (含外扩的 2px) */
    cEdge = Blend16(0, SS_ED_EDGE_R, SS_ED_EDGE_G, SS_ED_EDGE_B, 255);
    cFill = Blend16(0, 0, 0, 0, 255);
    SS_FillPx(base, pitch, W, H, x0 - 2, y0 - 2, bw + 4, bh + 4, cEdge);
    SS_FillPx(base, pitch, W, H, x0, y0, bw, bh, cFill);

    if (!GdiEnsure(px, VecText_Bold(), SS_VEC_SLOT, SS_Face()))
    {
        GtUnlock(surf);
        return;
    }
    if (g_G->adv > 0) adv = g_G->adv;

    n = SS_EdText(s_EdSt.buf);
    s_EdSt.len = n;
    cur = SS_EdLayout(n, fieldW, xs, &start);
    focus = SS_EdFocused();
    LiEdSel(&s_EdSt, &lo, &hi);
    if (hi > n) hi = n;                 /* 引擎可能把文字截短了, 选中区跟着收 */
    ty = y0 + (bh - inkH) / 2 - inkTop;

    /* 选中底 */
    if (lo >= 0)
    {
        int sx1 = textL + xs[lo] - s_EdSt.view, sx2 = textL + xs[hi] - s_EdSt.view;
        int sy = (ty < y0) ? y0 : ty, ey = ty + adv;
        if (sx1 < x0) sx1 = x0;
        if (sx2 > x0 + bw) sx2 = x0 + bw;
        if (ey > y0 + bh) ey = y0 + bh;
        SS_BlendPx(base, pitch, W, H, sx1, sy, sx2 - sx1, ey - sy, 123, 32, 123, 150);
    }

    /* 正文: 从 start 起, 右缘由 textR 截断 */
    while (start < n && textL + xs[start] - s_EdSt.view < x0) start++;
    if (n > start)
    {
        int dx = textL + xs[start] - s_EdSt.view;
        SS_TextRun(base, pitch, W, H, s_EdSt.buf + start, CharsFit(s_EdSt.buf + start, n - start, textR - dx),
                   dx, ty, textR - dx, 239, 235, 239);
    }

    /* 插入点 */
    caret = textL + (cur - s_EdSt.view);
    if (caret < x0) caret = x0;
    if (caret > x0 + bw - 1) caret = x0 + bw - 1;

    /* 组字串: 黄字 + 下划线, 跟着插入点走 */
    if (focus && s_EdCompLen > 0)
    {
        int k = CharsFit(s_EdComp, s_EdCompLen, textR - caret);
        w = SS_TextRun(base, pitch, W, H, s_EdComp, k, caret, ty, textR - caret,
                       255, 255, 0);
        if (w > 0)
        {
            for (i = 0; i < w && caret + i < x0 + bw - 2; i++)
            {
                unsigned short* row = (unsigned short*)
                    (base + (size_t)(ty + adv - 1) * pitch) + caret + i;
                *row = Blend16(*row, 255, 255, 0, 255);
            }
            /* 组字内光标: 把插入点推到组字串里的当前位置 */
            for (i = 0; i < s_EdCompCur && i < k; i++)
            {
                int ga = GtCharAdv(s_EdComp[i]);
                caret += (ga < 0) ? px : ga;
            }
        }
    }

    /* 自绘光标: 有焦点、没有选中时才画 (系统那个 1px 另有用途: 输入法锚点) */
    if (focus && lo < 0 && LiBlinkOn(&s_EdSt))
    {
        int sy = (ty < y0 + 2) ? y0 + 2 : ty;
        int ey = ty + adv, ex = caret + 2;
        if (ey > y0 + bh - 2) ey = y0 + bh - 2;
        if (ex > x0 + bw - 2) ex = x0 + bw - 2;
        if (ey > sy && ex > caret)
            SS_FillPx(base, pitch, W, H, caret, sy, ex - caret, ey - sy,
                      Blend16(0, 255, 255, 255, 255));
    }

    GtUnlock(surf);
}

/* ---- 控件过程: 只做编辑, 绘制交回去 ---- */
static LRESULT CALLBACK SS_EdProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    int ctrl, shift;

    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);     /* 必须配对, 否则刷新区不失效 → 反复 WM_PAINT */
        if (dc) EndPaint(h, &ps);
        return 0;                        /* 一个像素都不画: 全由 SS_EdDraw 铺 */
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCPAINT:
    case WM_PRINTCLIENT:
        return 0;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        LiEdTouch(&s_EdSt);
        s_EdCompLen = s_EdCompCur = 0;
        SS_EdRepaint();
        break;                           /* 交原过程: 建/拆它自己的插入符 */

    /* 单击/双击定位。双击的第二下是 WM_LBUTTONDBLCLK 而**不是** LBUTTONDOWN, 漏了它
     * 双击就等于没点。定位完自己置脏: 画面上的光标是 SS_EdDraw 画的, 原生控件那次
     * InvalidateRect 画在下一层, 一个像素都显示不出来。 */
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
        int xs[SS_ED_BUF + 1], start = 0, n;

        if (!GdiEnsure(VecText_ResolvedPx(), VecText_Bold(), SS_VEC_SLOT, SS_Face()))
            break;
        n = SS_EdText(s_EdSt.buf);
        s_EdSt.len = n;
        SS_EdLayout(n, SS_EdFieldW(), xs, &start);  /* 顺带把横向滚动夹回合法范围 */
        LiEdClick(&s_EdSt, xs, n,
                  (int)(short)LOWORD(lp) - SS_ED_PAD + s_EdSt.view,
                  (GetKeyState(VK_SHIFT) < 0));
        s_EdCompLen = s_EdCompCur = 0;             /* 点走 = 放弃组字 */
        SS_EdRepaint();
        break;                                     /* 原过程照走 (焦点/它自己的插入符) */
    }

    case WM_CHAR:
        /* 组字中的键都属于输入法: 拼音字母/退格不进口, 否则就是 "e'shi" 或删正文 */
        if (SS_EdComposing() && (wp == 0x08 || (wp >= 0x20 && wp < 0x7F))) return 0;
        if (wp == 0x08) { SS_EdErase(0); return 0; }
        if (wp == 0x1B || wp == 0x0D || wp == 0x09) break;   /* Esc/回车/Tab 交回引擎 */
        if (wp < 0x20 || wp == 0x7F) return 0;
        { wchar_t c = (wchar_t)wp; SS_EdInsertStr(&c, 1); }
        return 0;

    case WM_KEYDOWN:
    {
        int done = 0;

        ctrl  = (GetKeyState(VK_CONTROL) < 0);
        shift = (GetKeyState(VK_SHIFT) < 0);

        if (ctrl && wp == 'A') { SS_EdLoad(); LiEdSelAll(&s_EdSt); done = 1; }
        else if (ctrl && (wp == 'C' || wp == 'X')) { SS_EdCopy(wp == 'X'); done = 1; }
        else if (ctrl && wp == 'V') { SS_EdPaste(); done = 1; }
        else if (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_HOME || wp == VK_END ||
                 wp == VK_UP || wp == VK_DOWN)
        {
            SS_EdLoad();
            if (wp == VK_UP) wp = VK_HOME;          /* 单行框: 上下 = 首尾 */
            else if (wp == VK_DOWN) wp = VK_END;
            LiEdMove(&s_EdSt, (int)wp, shift);
            done = 1;
        }
        else if (wp == VK_DELETE) { SS_EdErase(1); done = 1; }
        else if (wp == VK_BACK) return 0;   /* 退格只认 WM_CHAR: 原过程也会删一次 */

        if (done) { SS_EdRepaint(); return 0; }
        break;
    }

    /* ---- IME: 系统组合窗不用, 组字串自己画 ---- */
    case WM_IME_SETCONTEXT:
        lp &= (LPARAM)(~(DWORD)ISC_SHOWUICOMPOSITIONWINDOW);
        break;
    case WM_IME_STARTCOMPOSITION:
        SS_EdImePlace();                 /* 组字一开始就把候选窗钉到框下 */
        return 0;
    case WM_IME_COMPOSITION:
    {
        HWND w;
        HIMC imc = SS_EdHimc(&w);
        wchar_t res[SS_ED_BUF];
        LONG nr = 0;
        int kr;

        res[0] = 0;
        /* 上下文里只读不写: 读完立刻释放, 插入/通知不在 IMM 锁内做 */
        if (imc)
        {
            if (lp & GCS_RESULTSTR)
                nr = ImmGetCompositionStringW(imc, GCS_RESULTSTR, res,
                                              (DWORD)(SS_ED_BUF - 1) * 2);
            if (lp & (GCS_COMPSTR | GCS_CURSORPOS))
                s_EdCompLen = LiCompRead(imc, s_EdComp, SS_ED_BUF, &s_EdCompCur);
            ImmReleaseContext(w, imc);
        }

        if (nr > 0)
        {
            kr = (int)(nr / 2);
            if (kr > SS_ED_BUF - 1) kr = SS_ED_BUF - 1;
            SS_EdInsertStr(res, kr);
            s_EdImeResT = GetTickCount();      /* 收编时刻: 用来吃掉系统回声 */
        }
        if (lp & (GCS_COMPSTR | GCS_CURSORPOS | GCS_RESULTSTR))
        {
            SS_EdImePlace();
            SS_EdRepaint();              /* 改了字/组字串都要靠重画才出现 */
        }
        return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        s_EdCompLen = 0;
        s_EdCompCur = 0;
        SS_EdRepaint();
        return 0;
    case WM_IME_CHAR:
        /* 刚落字 200ms 内的 WM_IME_CHAR 是系统的回声 (GCS_RESULTSTR 已经收编过),
         * 更晚的是"直接输入" (全角这类不发 composition), 必须收; 半角走 WM_CHAR。 */
        if (s_EdImeResT && (unsigned)(GetTickCount() - s_EdImeResT) < 200) return 0;
        { wchar_t c = (wchar_t)wp; if (c >= 0x80) SS_EdInsertStr(&c, 1); }
        return 0;

    case WM_NCDESTROY:
    {
        WNDPROC old = s_EdOld;
        if (s_Ed == h) { s_Ed = NULL; s_EdOld = NULL; s_EdCompLen = s_EdCompCur = 0; }
        if (old)
        {
            SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)old);
            return CallWindowProcA(old, h, msg, wp, lp);
        }
        break;
    }

    default:
        break;
    }

    r = LiCallOld(h, s_EdOld, msg, wp, lp);

    /* 原过程在获得焦点/按键时会按它自己的排版摆插入符。位置统一改成我们这版算出来的
     * 插入点 —— 只挪位置, 不销毁: 那是输入法的锚点。 */
    if (msg == WM_SETFOCUS || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ||
        msg == WM_KEYDOWN || msg == WM_CHAR)
        SS_EdCaretSync();
    return r;
}

/* ---- 装/拆 ---- */
static void SS_EdDetach(void)
{
    if (s_Ed && IsWindow(s_Ed) && s_EdOld)
        SetWindowLongPtrA(s_Ed, GWLP_WNDPROC, (LONG_PTR)s_EdOld);
    s_Ed = NULL;
    s_EdOld = NULL;
    LiEdInit(&s_EdSt, s_EdText, SS_ED_BUF);
    s_EdCompLen = s_EdCompCur = 0;
    s_EdImeResT = 0;
    s_EdSurfW = s_EdSurfH = 0;
    s_ImeX = s_ImeY = s_ImeYC = -1;
}

/* 认领名字框: 只认 SS_Layout 挑出来的那一个。文字读不到 (0x4B3 返 0) 就不接管,
 * 说明字不在这个窗口里, 硬画等于抹掉它。[SaveShot] EditHook=0 时整块不装。 */
static void SS_EdAttach(void)
{
    HWND ed;
    WNDPROC old;

    if (!s_EdHook || !s_BoxHwnd || !IsWindow(s_BoxHwnd)) return;
    ed = s_BoxHwnd;
    if (s_Ed == ed) return;
    if (s_Ed) SS_EdDetach();

    /* 先 init (清会话状态) 再拉文字进 buf, 顺序反了文字会被 LiEdInit 清掉 */
    s_Ed = ed;
    LiEdInit(&s_EdSt, s_EdText, SS_ED_BUF);
    if (SS_EdText(s_EdSt.buf) <= 0) /* 拉不到文字 = 字不在这个窗口里, 硬画等于抹掉它 */
    {
        s_Ed = NULL;
        SS_Log("namebox empty, keep native", NULL);
        return;
    }
    old = (WNDPROC)SetWindowLongPtrA(ed, GWLP_WNDPROC, (LONG_PTR)SS_EdProc);
    if (!old)
    {
        SS_Log("namebox subclass FAILED", NULL);
        s_Ed = NULL;
        return;
    }
    s_EdOld = old;
    s_EdSt.len = lstrlenW(s_EdSt.buf);
    s_EdSt.cur = s_EdSt.len;
    LiEdTouch(&s_EdSt);

    SS_EdBeatStart();       /* 光标闪 / 组字 / 候选窗要有节拍 (SetTimer 在这不响) */
    SS_EdRepaint();         /* 立刻把这一块交给我们画 */
}

/* 每帧一次的轻量轮询: 组字串 + 候选窗位置 + 插入符位置。 */
static void SS_EdPoll(void)
{
    if (!s_Ed || !IsWindow(s_Ed)) return;
    SS_EdImePoll();
    SS_EdCaretSync();
}

/* ---------------- 重命名按钮 ----------------
 * 两下: 第一下进改名态 (按钮变"确认改名"), 用户在名字框里改; 第二下按框里的字写回
 * 文件。名字框就是"储存"用的那个 —— 引擎里存档名与显示名是同一个字段, 而换选中时
 * 引擎自己会把该档名字填进去, 所以第一下不碰框里的字。 */
static void SS_DoRename(HWND dlg, HWND list)
{
    char sav[MAX_PATH_LEN], bmp[MAX_PATH_LEN];
    wchar_t cur[0x80];
    unsigned fe;
    int sel, cnt;

    if (!list || !s_Ed || !IsWindow(s_Ed)) return;
    sel = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
    if (sel < 0) { s_RnArm = 0; return; }

    /* 空白档位 ([空白 栏位]) 没有文件: 没有可改的对象 */
    if (!SS_EntryFile(list, sav, MAX_PATH_LEN, bmp, MAX_PATH_LEN) ||
        GetFileAttributesA(sav) == INVALID_FILE_ATTRIBUTES)
    { s_RnArm = 0; return; }

    fe = (unsigned)(size_t)SendMessageA(list, LB_GETITEMDATA, (WPARAM)sel, 0);
    if (!fe || !IsPlausiblePtr((const void*)(size_t)fe)) { s_RnArm = 0; return; }

    if (!s_RnArm)
    {
        s_RnArm = 1;
        s_RnArmTick = GetTickCount();
        s_RnFe = fe;
        SetFocus(s_Ed);
        return;
    }

    if (fe != s_RnFe) { s_RnArm = 0; return; }   /* 中间换了选中: 不认, 免得改到别人 */

    s_RnArm = 0;
    SS_EdLoad();
    SS_RowName(fe, cur, 0x80);
    if (!s_EdSt.buf[0] || !lstrcmpW(s_EdSt.buf, cur)) return;   /* 空名字 / 没改 */

    if (!SS_RenameSave(sav, s_EdSt.buf)) { SS_Log("rename FAIL ", sav); return; }

    /* 行是按 FileEntry 画的, 改这里就够; 列表控件里那份旧串要等引擎重扫 */
    lstrcpynW((wchar_t*)(size_t)fe, s_EdSt.buf, 0x80);
    cnt = (int)SendMessageA(list, LB_GETCOUNT, 0, 0);
    SS_BumpSel(list, sel, cnt);
    s_DrawnCur = s_DrawnTop = -2;
    if (dlg && IsWindow(dlg))
    {
        InvalidateRect(dlg, NULL, FALSE);
        UpdateWindow(dlg);
    }
    SS_Log("rename ok ", sav);
}

/* ---------------- 显示 ---------------- */

static int SS_DlgAlive(void)
{
    if (!s_Dlg || !s_DlgList) return 0;
    if (!IsWindow((HWND)s_Dlg)) return 0;
    return GetDlgItem((HWND)s_Dlg, s_DlgList) != NULL;
}

/* 忘掉这个对话框。keepLive=0 (读档前) 连活着的控件一起交还 —— 世界马上重建, 它们
 * 都会作废; keepLive=1 时对话框可能只是重新认一次, 活着的句柄要留着 (丢了会重挂
 * 子类化、重认名字框, 光标位置也丢)。 */
static void SS_Forget(int keepLive)
{
    if (!keepLive)
    {
        if (s_SubList && s_OldListProc)
            SetWindowLongPtrA(s_SubList, GWLP_WNDPROC, (LONG_PTR)s_OldListProc);
        s_SubList = NULL;
        s_OldListProc = NULL;
        s_HWndLoad = s_HWndDel = s_HWndRename = s_BoxHwnd = NULL;
        SS_EdDetach();
    }
    else
    {
        if (s_SubList && !IsWindow(s_SubList)) { s_SubList = NULL; s_OldListProc = NULL; }
        if (s_HWndLoad && !IsWindow(s_HWndLoad)) s_HWndLoad = NULL;
        if (s_HWndDel && !IsWindow(s_HWndDel)) s_HWndDel = NULL;
        if (s_HWndRename && !IsWindow(s_HWndRename)) s_HWndRename = NULL;
        if (s_BoxHwnd && !IsWindow(s_BoxHwnd)) s_BoxHwnd = NULL;
        if (s_Ed && !IsWindow(s_Ed)) SS_EdDetach();
    }
    s_Dlg = NULL;
    s_DlgList = 0;
    s_Scroll = NULL;
    s_LayoutW = s_LayoutH = 0;
    s_LayoutN = 0;
    s_DrawnCur = s_DrawnTop = -2;
    SetRect(&s_BtnLoad, 0, 0, 0, 0);
    SetRect(&s_BtnDel, 0, 0, 0, 0);
    SetRect(&s_BtnRename, 0, 0, 0, 0);
    SS_PicFree(&s_PicA);
    SS_ThumbReset();
}

void SaveShot_PaintDialog(void* regs, void* surf)
{
    HWND hwnd, list;
    int listId = 0, aw, ah, bw, bh;
    char name[SS_NAME_MAX];
    RECT cr;

    (void)regs;
    SaveShot_EnsureInit();
    if (!s_Enable || !surf || !IsPlausiblePtr(surf)) return;

    /* 已请引擎读档: 旧对话框还在 (只是即将被销毁), 世界正在重建 —— 那一刻再去认它、
     * 拿它的 FileEntry 和控件指针都可能已作废。等它真的没了再恢复。 */
    if (s_Loading)
    {
        if (s_LoadOldDlg && !IsWindow(s_LoadOldDlg))
        {
            s_Loading = 0;
            s_LoadOldDlg = NULL;
            SS_Forget(1);
        }
        else if (GetTickCount() - s_Loading < 90000) return;   /* 看门狗 */
        else { s_Loading = 0; s_LoadOldDlg = NULL; }
    }

    if (SS_DlgAlive())
    {
        hwnd = (HWND)s_Dlg;
        listId = s_DlgList;
    }
    else
    {
        /* 没对话框时这个钩点仍然每帧进, 枚举窗口要节流 */
        if (GetTickCount() - s_LastScan < 200) return;
        s_LastScan = GetTickCount();
        hwnd = SS_FindDialog(&listId);
        if (!hwnd) { SS_Forget(1); return; }
        SS_Forget(1);
        s_Dlg = (void*)hwnd;
        s_DlgList = listId;
    }

    list = GetDlgItem(hwnd, listId);
    if (!list) { SS_Forget(1); return; }

    SS_PanelRect(hwnd, &cr);
    if ((!s_LayoutW || s_PanelW != s_LayoutW || s_PanelH != s_LayoutH ||
         SS_LayoutDrift(list)) && s_LayoutN < 3)
    {
        SS_PicFree(&s_PicA);
        SS_ThumbReset();
        if (!SS_Layout(hwnd, list, listId)) return;
    }

    SS_KeepScroll(list);
    SS_HookList(list);
    SS_DrawRows(surf, list);

    /* 存档页的存档名输入框: 认领 + 每帧重画。原生控件一个像素都不画, 所以这块
     * 屏幕上只剩这里画的紫框。 */
    if (listId == SS_ID_LIST_SAVE)
    {
        SS_EdAttach();
        SS_EdPoll();
        SS_EdDraw(surf);
    }

    /* 删除按钮上膛: 直接改按钮对象里的文字字段 (引擎绘制只读那份副本) */
    if (listId == SS_ID_LIST_SAVE && s_HWndDel && IsWindow(s_HWndDel))
    {
        int arm = (s_DelArm && GetTickCount() - s_DelArmTick <= SS_DEL_ARM_MS) ? 1 : 0;
        if (s_DelArm && !arm) s_DelArm = 0;          /* 4 秒没点, 落回"删除" */
        if (arm != s_DelArmShown)
        {
            s_DelArmShown = arm;
            SS_SetBtnText(s_HWndDel, arm ? s_TxtDeleteOk : s_TxtDelete);
        }
    }

    /* 重命名: 编辑态时按钮变"确认改名", 超时或改完落回 */
    if (listId == SS_ID_LIST_SAVE && s_HWndRename && IsWindow(s_HWndRename))
    {
        int arm = (s_RnArm && GetTickCount() - s_RnArmTick <= SS_RN_ARM_MS) ? 1 : 0;
        if (s_RnArm && !arm) s_RnArm = 0;
        if (arm != s_RnArmShown)
        {
            s_RnArmShown = arm;
            SS_SetBtnText(s_HWndRename, arm ? s_TxtRenameOk : s_TxtRename);
        }
    }

    if (!s_BoxA.right || s_BoxA.right <= s_BoxA.left) return;

    /* 上框: 当前画面 (滚动缓存, 对话框开着时不刷新) */
    aw = s_BoxA.right - s_BoxA.left - 2;   /* 框内留 1px 边框 */
    ah = s_BoxA.bottom - s_BoxA.top - 2;
    if (!s_PicA.px && s_Pix && s_PixW > 0 && s_PixH > 0)
        SS_PicFit(&s_PicA, (const unsigned char*)s_Pix, s_PixW, s_PixH,
                  s_PixW * 2, 0, aw, ah);

    /* 下框: 列表选中那一档已存的截图 (主名, 去掉目录与扩展名) */
    bw = s_BoxB.right - s_BoxB.left - 2;
    bh = s_BoxB.bottom - s_BoxB.top - 2;
    {
        char nm[40];
        SS_SplitPath(SS_SelectedFile(list, nm) ? nm : "", NULL, name);
    }
    if (lstrcmpA(name, s_ThumbName) != 0)
    {
        SS_PicFree(&s_PicB);
        lstrcpynA(s_ThumbName, name, SS_NAME_MAX);   /* 加载失败也记下, 免得每帧重读磁盘 */
        if (name[0])
        {
            char path[MAX_PATH_LEN];
            SS_ThumbPath(path, MAX_PATH_LEN, s_SaveDir, name);
            if (!SS_PicLoadBmp(&s_PicB, path, bw, bh))
                SS_Log("no thumb ", path);
        }
    }
    else if (s_PicB.px && (s_PicB.w > bw + 2 || s_PicB.h > bh + 2))
    {
        SS_ThumbReset();        /* 换排版后框变小了 */
    }

    SS_DrawBox(surf, &s_BoxA, &s_PicA, s_TxtCapNow,  s_CapAx, s_CapAy);
    SS_DrawBox(surf, &s_BoxB, &s_PicB, s_TxtCapShot, s_CapBx, s_CapBy);

    /* 记下这次面板按哪套 "选中/顶行" 画的: 列表的 WM_PAINT 靠它判断要不要请
     * 面板重画, 不记就会互相请求重画。 */
    s_DrawnCur = (int)SendMessageA(list, LB_GETCURSEL, 0, 0);
    s_DrawnTop = (int)SendMessageA(list, LB_GETTOPINDEX, 0, 0);
}

/* ---------------- Syringe 导出 ---------------- */

/* 0x67CF42: StgCreateDocfileW 调用点 (EDX = 宽字符完整路径)。
 * 原指令 FF 15 C8 15 7E 00 (call dword ptr [0x7e15c8]), 返回 0 照原样重放。 */
extern "C" __declspec(dllexport) DWORD __cdecl SaveShot_SaveBegin(void* regs)
{
    SaveShot_OnSaveBegin(regs);
    return 0;
}

/* 0x67D2F1: 存档尾部 (存储已 Commit+Release, BL = 1 成功)。
 * 原指令 5F 5E 8A C3 5D (pop edi / pop esi / mov al,bl / pop ebp)。 */
extern "C" __declspec(dllexport) DWORD __cdecl SaveShot_SaveEnd(void* regs)
{
    SaveShot_OnSaveEnd(regs);
    return 0;
}
