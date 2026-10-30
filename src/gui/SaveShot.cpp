/* SaveShot.cpp - 存/读档界面的缩略图与存档页按钮, 见 SaveShot.h 与 notes/存档截图.md。
 *
 * 先看这几条约束, 改动前务必守住:
 *   1. 对话框窗口是整屏的, 引擎只把"面板"那么大一块画进自己的临时面再居中 blit。
 *      面板 = MapDialogRect(模板 533x369 DLU) = 800x600 px; 控件用客户区坐标
 *      (= 屏幕坐标), 我们的像素也画在这个面的同一坐标系里。
 *   2. 面板尺寸改不了 (改模板 cx/cy 无效, 引擎按自己的尺寸摆控件), 界面"变大"
 *      只能靠在面板内重排。控件别放到面板外, 超出部分会被裁掉。
 *   3. 列表行只能"控件一像素不画 + 我们在 0x622396 画": 控件自画的 GDI 像素落在
 *      面板 blit 之后 (两套文字), 让控件赶在 blit 之前画又会被盖掉 (只闪一瞬)。
 *   4. 行文字、截图框标题的字体/字号/粗体一律跟随 [VecText], 不另开 ini 开关。
 *   5. 存档路径取自 0x67CF42 的 EDX, 不是存档函数入口的 ECX (那是日志用名字串)。
 *
 * 流程: SaveShot_Tick 每秒从 SB_RawView 缓存一张无 UI 的缩略图; 存档结束 (0x67D2F1)
 * 写成 <存档目录>\Thumbs\<主名>.bmp; 对话框打开后把当前画面与该档截图分别画进右侧
 * 两框, 列表行和"载入/删除"两个按钮由我们接管。不碰模拟状态, 联机安全。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "ScaleBar.h"
#include "Shot.h"
#include "FrameCap.h"
#include "EscMenu.h"
#include "VecText.h"
#include "SaveShot.h"

#define SS_SECTION   "SaveShot"
#define SS_TICK_MS   1000      /* 缓存刷新间隔 */
#define SS_DEF_W     320       /* 缩略图宽 (高度按视口比例) */
#define SS_MIN_W     96
#define SS_MAX_W     640
#define SS_DEF_PREV  200       /* 截图列宽 (px) */
#define SS_MIN_PREV  120

/* 对话框模板原始尺寸 (DLU): 引擎按 8pt Sans Serif 换算, 横向 6/4、纵向 13/8 */
#define SS_TPL_CX    533
#define SS_TPL_CY    369
#define SS_TPL_PX_W  800
#define SS_TPL_PX_H  600

/* FileEntryClass: Name 是 wchar_t[0x80], 所以 Filename 落在 +0x100 */
#define SS_FE_NAME   0x000
#define SS_FE_FILE   0x100
#define SS_FE_TIME   0x1AC
#define SS_FE_VALID  0x1B4
#define SS_FE_WRONG  0x1B5

/* ListBox 消息 */
#define SS_LB_GETCOUNT      0x018B
#define SS_LB_GETCURSEL     0x0188
#define SS_LB_GETTOPINDEX   0x018E
#define SS_LB_GETITEMHEIGHT 0x01A1
#define SS_LB_SETITEMHEIGHT 0x01A0
#define SS_LB_GETITEMDATA   0x0199
#define SS_LB_GETTEXT       0x0189
#define SS_LB_SETCURSEL     0x0186
#define SS_LB_DELETESTRING  0x0182

/* 对话框控件 id (读档模板 183 / 存档模板 692) */
#define SS_ID_LIST_LOAD 1317
#define SS_ID_LIST_SAVE 1319
#define SS_ID_BTN_LOAD  1039
#define SS_ID_BTN_SAVE  1735
#define SS_ID_TITLE     1036

/* 我们自建的"载入"/"删除"按钮 (只有存档页)。它们必须在引擎的 WM_INITDIALOG 之前
 * 建出来, 才能被当作自绘按钮接管 —— 见 SS_SaveDlgProc。 */
#define SS_ID_BTN_LOAD_NEW 0x540
#define SS_ID_BTN_DEL_NEW  0x541
#define SS_ADDR_SAVE_DLG   0x558B90u   /* 存档页对话框过程 (stdcall, ret 0x10) */
#define SS_ADDR_MENU_TAIL  0x4F14BFu   /* ESC 菜单命令处理 0x4F1330 的公共落点 */

/* Syringe REGISTERS: EDI 8 / ESI 0xC / EBP 0x10 / ESP 0x14 / EBX 0x18 / EDX 0x1C */
#define SS_REG_EBX   0x18u
#define SS_REG_EDX   0x1Cu

#define SS_NAME_MAX  64

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
static RECT  s_BtnLoad, s_BtnDel;       /* 两个按钮位置 (空 = 不显示) */
static HWND  s_HWndLoad = NULL;
static HWND  s_HWndDel  = NULL;
static int   s_DelArmShown = 0;         /* 删除按钮当前显示的是不是"确认删除" */
static unsigned s_Loading = 0;          /* 已请引擎读档: 期间不认/不画这个对话框 */
static HWND  s_LoadOldDlg = NULL;       /* 触发读档时的对话框, 销毁即这一程结束 */
static unsigned s_LastScan = 0;

/* 一张可以缩放显示的 16 位图 */
typedef struct {
    unsigned short* px;
    int w, h;          /* 内容尺寸 */
    int capW, capH;    /* 已分配上限 */
    int ok;
    char name[SS_NAME_MAX];
} SS_Img;

static SS_Img s_ImgA;   /* 当前画面 */
static SS_Img s_ImgB;   /* 选中存档的截图 */

/* SS_DrawBox 在矢量字定义之前就用到, 先声明 */
static const wchar_t* SS_Face(void);
static int  SS_TextW(const wchar_t* s);
static void SS_PutTextLine(void* surf, const wchar_t* s, int x, int lineTop, int lineH,
                           unsigned r, unsigned g, unsigned b);
static INT_PTR __stdcall SS_SaveDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

/* ---------------- 小工具 ---------------- */

/* 用 lstrlenA: 手写 while 循环会被 GCC 还原成 strlen 调用 (-nostdlib 下链接失败) */
static int SS_StrLen(const char* s) { return (int)lstrlenA(s); }

static int SS_StrEq(const char* a, const char* b)
{
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

/* 拆目录与主名 (主名去掉扩展名)。dir/base 都可传 NULL。 */
static void SS_SplitPath(const char* path, char* dir, char* base)
{
    int n = SS_StrLen(path), cut = -1, i, k;
    for (i = n - 1; i >= 0; i--)
        if (path[i] == '\\' || path[i] == '/') { cut = i; break; }
    if (dir)
    {
        for (k = 0; k < cut && k < MAX_PATH_LEN - 1; k++) dir[k] = path[k];
        dir[k] = 0;
    }
    if (base)
    {
        int p = cut + 1;
        k = 0;
        while (path[p] && path[p] != '.' && k < SS_NAME_MAX - 1) base[k++] = path[p++];
        base[k] = 0;
    }
}

/* 十进制追加 (无 CRT), 负数按 "-123"。返回新位置。 */
static unsigned SS_AppendInt(char* buf, unsigned pos, int v, unsigned cap)
{
    char tmp[12];
    int n = 0, i, neg = 0;
    unsigned u;

    if (v < 0) { neg = 1; u = (unsigned)(-v); }
    else       { u = (unsigned)v; }
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg && pos + 1 < cap) buf[pos++] = '-';
    for (i = n - 1; i >= 0 && pos + 1 < cap; i--) buf[pos++] = tmp[i];
    buf[pos] = 0;
    return pos;
}

/* ---------------- FileEntry 取值 ---------------- */

/* Name (wchar_t[0x80]) 即列表显示的文字。返回长度, 非法返回 0。 */
static int SS_RowName(unsigned fe, wchar_t* out, int cap)
{
    const wchar_t* nm;
    int i;

    out[0] = 0;
    if (!fe || !IsPlausiblePtr((const void*)(size_t)fe)) return 0;
    nm = (const wchar_t*)(size_t)(fe + SS_FE_NAME);
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

static int SS_PutDigits(wchar_t* o, int v, int digits)
{
    int i;
    if (v < 0) v = 0;
    for (i = digits - 1; i >= 0; i--) { o[i] = (wchar_t)(L'0' + (v % 10)); v /= 10; }
    return digits;
}

/* LastWriteTime → 本地 "YYYY-MM-DD HH:MM:SS" (定长 19 字)。无效时间返回 0。 */
static int SS_RowTime(unsigned fe, wchar_t* out)
{
    FILETIME ft, fl;
    SYSTEMTIME st;
    int n;

    out[0] = 0;
    if (!fe || !IsPlausiblePtr((const void*)(size_t)fe)) return 0;
    ft.dwLowDateTime  = *(const unsigned*)(size_t)(fe + SS_FE_TIME);
    ft.dwHighDateTime = *(const unsigned*)(size_t)(fe + SS_FE_TIME + 4);
    if (!ft.dwLowDateTime && !ft.dwHighDateTime) return 0;
    if (!FileTimeToLocalFileTime(&ft, &fl)) return 0;
    if (!FileTimeToSystemTime(&fl, &st)) return 0;
    if (st.wYear < 1990 || st.wYear > 2200) return 0;   /* 偏移猜错时的保险 */

    n = SS_PutDigits(out, st.wYear, 4);
    out[n++] = L'-'; n += SS_PutDigits(out + n, st.wMonth, 2);
    out[n++] = L'-'; n += SS_PutDigits(out + n, st.wDay, 2);
    out[n++] = L' '; n += SS_PutDigits(out + n, st.wHour, 2);
    out[n++] = L':'; n += SS_PutDigits(out + n, st.wMinute, 2);
    out[n++] = L':'; n += SS_PutDigits(out + n, st.wSecond, 2);
    out[n] = 0;
    return n;
}

/* 档名: Name 读不出来 (比如第一行那个空白占位项) 就退回列表自己的字符串。 */
static int SS_RowText(HWND list, unsigned fe, int idx, wchar_t* out, int cap)
{
    char a[128];
    int n, i, wn;

    if (SS_RowName(fe, out, cap) > 0) return 1;
    out[0] = 0;
    a[0] = 0;
    SendMessageA(list, SS_LB_GETTEXT, (WPARAM)idx, (LPARAM)a);
    n = SS_StrLen(a);
    if (n <= 0 || n >= (int)sizeof(a)) return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)a[i] < 0x20) return 0;
    wn = MultiByteToWideChar(CP_ACP, 0, a, -1, out, cap);
    if (wn <= 1) { out[0] = 0; return 0; }
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
    cur = (int)SendMessageA(list, SS_LB_GETCURSEL, 0, 0);
    if (cur < 0) return 0;
    fe = (unsigned)(size_t)SendMessageA(list, SS_LB_GETITEMDATA, (WPARAM)cur, 0);
    if (!fe || !IsPlausiblePtr((const void*)(size_t)fe)) return 0;
    nm = (const char*)(size_t)(fe + SS_FE_FILE);
    for (k = 0; k < 31 && nm[k]; k++)
        if ((unsigned char)nm[k] < 0x20 || (unsigned char)nm[k] > 0x7E) break;
    if (k <= 0) return 0;
    for (k = 0; nm[k] && k < 31; k++) out[k] = nm[k];
    out[k] = 0;
    return 1;
}

/* ---------------- 像素打包 (跟随引擎位宽) ---------------- */
static __inline unsigned SS_R(unsigned short c) { return (g_GBits == 6) ? ((c >> 11) & 31u) : ((c >> 10) & 31u); }
static __inline unsigned SS_G(unsigned short c) { return (g_GBits == 6) ? ((c >> 5) & 63u) : ((c >> 5) & 31u); }
static __inline unsigned SS_B(unsigned short c) { return c & 31u; }

static __inline unsigned short SS_Pack(unsigned r, unsigned g, unsigned b)
{
    if (g_GBits == 6)
    {
        if (r > 31u) r = 31u;
        if (g > 63u) g = 63u;
        if (b > 31u) b = 31u;
        return (unsigned short)((r << 11) | (g << 5) | b);
    }
    if (r > 31u) r = 31u;
    if (g > 31u) g = 31u;
    if (b > 31u) b = 31u;
    return (unsigned short)((r << 10) | (g << 5) | b);
}

static __inline unsigned SS_E5(unsigned v) { return (v << 3) | (v >> 2); }
static __inline unsigned SS_E6(unsigned v) { return (v << 2) | (v >> 4); }

/* ---------------- 图像缓冲 ---------------- */

static void SS_ImgFree(SS_Img* im)
{
    if (im->px) VirtualFree(im->px, 0, MEM_RELEASE);
    im->px = NULL;
    im->w = im->h = im->capW = im->capH = 0;
    im->ok = 0;
    im->name[0] = 0;
}

static int SS_ImgAlloc(SS_Img* im, int w, int h)
{
    if (im->px && im->capW >= w && im->capH >= h) return 1;
    SS_ImgFree(im);
    im->px = (unsigned short*)VirtualAlloc(NULL, (size_t)w * h * 2u,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!im->px) return 0;
    im->capW = w;
    im->capH = h;
    return 1;
}

/* 双线性缩放到框内 (等比, 只缩不放)。src 为 16 位, srcPitch 字节/行。 */
static int SS_ImgResample(SS_Img* im, const unsigned char* src, int srcW, int srcH,
                          int srcPitch, int topDown, int bw, int bh)
{
    int ow, oh, x, y;

    im->ok = 0;
    im->w = im->h = 0;
    if (!src || srcW <= 0 || srcH <= 0 || bw < 8 || bh < 8) return 0;

    ow = bw;
    oh = (int)((long long)bw * srcH / srcW);
    if (oh > bh) { oh = bh; ow = (int)((long long)bh * srcW / srcH); }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;

    if (!SS_ImgAlloc(im, ow, oh)) return 0;

    for (y = 0; y < oh; y++)
    {
        int iy0, iy1, fy;
        long long sy = ((long long)(2 * y + 1) * srcH) / (2 * oh) - 1;
        const unsigned char* rowA;
        const unsigned char* rowB;
        if (sy < 0) sy = 0;
        iy0 = (int)sy;
        fy = (int)((sy - iy0) * 256);
        iy1 = (iy0 + 1 < srcH) ? iy0 + 1 : iy0;
        rowA = src + (size_t)(topDown ? iy0 : (srcH - 1 - iy0)) * srcPitch;
        rowB = src + (size_t)(topDown ? iy1 : (srcH - 1 - iy1)) * srcPitch;
        for (x = 0; x < ow; x++)
        {
            int ix0, ix1, fx;
            long long sx = ((long long)(2 * x + 1) * srcW) / (2 * ow) - 1;
            unsigned short c00, c01, c10, c11;
            unsigned r0, g0, b0, r1b, g1b, b1b, r, g, b;
            if (sx < 0) sx = 0;
            ix0 = (int)sx;
            fx = (int)((sx - ix0) * 256);
            ix1 = (ix0 + 1 < srcW) ? ix0 + 1 : ix0;
            c00 = ((const unsigned short*)rowA)[ix0];
            c01 = ((const unsigned short*)rowA)[ix1];
            c10 = ((const unsigned short*)rowB)[ix0];
            c11 = ((const unsigned short*)rowB)[ix1];

            r0  = (SS_E5(SS_R(c00)) * (256 - fx) + SS_E5(SS_R(c01)) * fx) >> 8;
            b0  = (SS_E5(SS_B(c00)) * (256 - fx) + SS_E5(SS_B(c01)) * fx) >> 8;
            r1b = (SS_E5(SS_R(c10)) * (256 - fx) + SS_E5(SS_R(c11)) * fx) >> 8;
            b1b = (SS_E5(SS_B(c10)) * (256 - fx) + SS_E5(SS_B(c11)) * fx) >> 8;
            if (g_GBits == 6)
            {
                g0  = (SS_E6(SS_G(c00)) * (256 - fx) + SS_E6(SS_G(c01)) * fx) >> 8;
                g1b = (SS_E6(SS_G(c10)) * (256 - fx) + SS_E6(SS_G(c11)) * fx) >> 8;
            }
            else
            {
                g0  = (SS_E5(SS_G(c00)) * (256 - fx) + SS_E5(SS_G(c01)) * fx) >> 8;
                g1b = (SS_E5(SS_G(c10)) * (256 - fx) + SS_E5(SS_G(c11)) * fx) >> 8;
            }
            r = (r0 * (256 - fy) + r1b * fy) >> 8;
            g = (g0 * (256 - fy) + g1b * fy) >> 8;
            b = (b0 * (256 - fy) + b1b * fy) >> 8;

            if (g_GBits == 6) im->px[(size_t)y * ow + x] = SS_Pack(r >> 3, g >> 2, b >> 3);
            else              im->px[(size_t)y * ow + x] = SS_Pack(r >> 3, g >> 3, b >> 3);
        }
    }
    im->w = ow;
    im->h = oh;
    im->ok = 1;
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

/* 读 BMP (只认 16 位) 缩放进框。 */
static int SS_ImgLoadBmp(SS_Img* im, const char* path, int bw, int bh)
{
    HANDLE f;
    DWORD got = 0, sz;
    unsigned char* d;
    SS_BmpHdr* hd;
    int srcW, srcH, topDown, ok;
    const unsigned char* pix;

    im->ok = 0;
    im->w = im->h = 0;
    if (bw < 8 || bh < 8) return 0;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(f, NULL);
    if (sz < sizeof(SS_BmpHdr) || sz > (16u << 20))
    {
        CloseHandle(f);
        return 0;
    }
    d = (unsigned char*)VirtualAlloc(NULL, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!d) { CloseHandle(f); return 0; }
    if (!ReadFile(f, d, sz, &got, NULL) || got != sz)
    {
        CloseHandle(f); VirtualFree(d, 0, MEM_RELEASE); return 0;
    }
    CloseHandle(f);

    hd = (SS_BmpHdr*)d;
    if (hd->magic != 0x4D42 || hd->hdrSize < 40 || hd->bits != 16 ||
        hd->w <= 0 || hd->h == 0 || hd->off >= sz)
    {
        VirtualFree(d, 0, MEM_RELEASE);
        return 0;
    }
    srcW = hd->w;
    srcH = (hd->h < 0) ? -hd->h : hd->h;
    topDown = (hd->h < 0);
    if (srcW > 4096 || srcH > 4096 ||
        (size_t)hd->off + (size_t)srcW * 2 * (size_t)srcH > sz)
    {
        VirtualFree(d, 0, MEM_RELEASE);
        return 0;
    }

    pix = d + hd->off;
    ok = SS_ImgResample(im, pix, srcW, srcH, srcW * 2, topDown, bw, bh);
    VirtualFree(d, 0, MEM_RELEASE);
    return ok;
}

/* ---------------- 钩子小工具 ---------------- */

static int SS_BytesSame(const unsigned char* a, const unsigned char* b, int n)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* 把 site 前 n 字节搬进可执行内存, 末尾 jmp 回 back。站点含 rel32 就拒绝。 */
static void* SS_BuildTrampAt(unsigned at, int n, unsigned back)
{
    unsigned char* b;
    int i;
    if (n < 1 || n > 16) return NULL;
    for (i = 0; i < n; i++)
    {
        unsigned char c = ((unsigned char*)at)[i];
        if (c == 0xE8 || c == 0xE9) return NULL;
    }
    b = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!b) return NULL;
    for (i = 0; i < n; i++) b[i] = ((unsigned char*)at)[i];
    b[n] = 0xE9;
    *(int*)(b + n + 1) = (int)(back - ((unsigned)(size_t)b + n + 5));
    FlushInstructionCache(GetCurrentProcess(), b, (SIZE_T)(n + 5));
    return b;
}

static int SS_PatchAt(unsigned at, void* target, int n)
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

/* ---------------- 换掉存档页对话框过程 (0x558B90) ----------------
 * 目的是给对话框加**引擎原生**按钮: 只有在我们调原过程之前建出 BS_OWNERDRAW 子窗口,
 * 引擎的 WM_INITDIALOG 才会遍历到它、挂子类并注册进 0xAC1B00 表, 之后才用原版美术画。
 * 站点前 5 字节 = push ebx; mov ebx,[esp+0x10], 无 rel32。 */
static void* s_OrigSaveDlg = NULL;

static void SS_InstallDlgHook(void)
{
    static const unsigned char kWant[5] = { 0x53, 0x8B, 0x5C, 0x24, 0x10 };
    if (s_OrigSaveDlg) return;
    if (!SS_BytesSame((unsigned char*)SS_ADDR_SAVE_DLG, kWant, 5))
    {
        LogLine("saveshot: savedlg site not pristine - native buttons OFF");
        return;
    }
    s_OrigSaveDlg = SS_BuildTrampAt(SS_ADDR_SAVE_DLG, 5, SS_ADDR_SAVE_DLG + 5);
    if (!s_OrigSaveDlg || !SS_PatchAt(SS_ADDR_SAVE_DLG, (void*)SS_SaveDlgProc, 5))
    {
        s_OrigSaveDlg = NULL;
        LogLine("saveshot: savedlg hook FAILED");
        return;
    }
    LogLine("saveshot: savedlg hook ON");
}

/* ---------------- 自绘按钮绘制守卫 (0x6071E0) ----------------
 * 该函数一进门就假定 UI 主题资源 [0xB0FC14..0x2C] 非空 (0x607369 读 [0xB0FC20]
 * 直接解引用)。读档重建世界时这组资源被释放并置 0, 之后旧窗口的任意一次 WM_PAINT
 * 都会空指针崩溃。读档这一程按钮一个都不需要, 整函数跳过即可。
 * 站点前 6 字节 = sub esp, 0x18c, 无 rel32。 */
typedef int (__fastcall *SS_BtnDrawFn)(void* hwnd, int dl);
static SS_BtnDrawFn s_BtnDrawOrig = NULL;
static int s_BtnGuardLogged = 0;

static int __fastcall SS_BtnDrawGuard(void* hwnd, int dl)
{
    static const unsigned kTheme[6] = {
        0xB0FC14u, 0xB0FC18u, 0xB0FC1Cu, 0xB0FC20u, 0xB0FC24u, 0xB0FC28u
    };
    int i;

    if (!s_Loading)
    {
        /* 平时只看资源是不是真的空了 —— 空了一定是必崩的前兆 */
        for (i = 0; i < 6; i++)
            if (!*(void**)kTheme[i]) break;
        if (i == 6) return s_BtnDrawOrig ? s_BtnDrawOrig(hwnd, dl) : 0;
    }

    if (s_BtnGuardLogged < 4)
    {
        char cls[32];
        char buf[96];
        unsigned p;
        s_BtnGuardLogged++;
        cls[0] = 0;
        if (hwnd) GetClassNameA((HWND)hwnd, cls, sizeof(cls));
        p = AppendCStr(buf, 0, "saveshot: btn draw skipped ", sizeof(buf));
        p = AppendCStr(buf, p, cls, sizeof(buf));
        p = AppendCStr(buf, p, " ret=", sizeof(buf));
        SS_AppendInt(buf, p, (int)(size_t)__builtin_return_address(0), sizeof(buf));
        LogLine(buf);
    }
    return 0;
}

static void SS_InstallBtnGuard(void)
{
    static const unsigned char kWant[6] = { 0x81, 0xEC, 0x8C, 0x01, 0x00, 0x00 };
    if (s_BtnDrawOrig) return;
    if (!SS_BytesSame((unsigned char*)0x6071E0u, kWant, 6))
    {
        LogLine("saveshot: btndraw site not pristine - guard OFF");
        return;
    }
    s_BtnDrawOrig = (SS_BtnDrawFn)SS_BuildTrampAt(0x6071E0u, 6, 0x6071E6u);
    if (!s_BtnDrawOrig || !SS_PatchAt(0x6071E0u, (void*)SS_BtnDrawGuard, 6))
    {
        s_BtnDrawOrig = NULL;
        LogLine("saveshot: btndraw guard FAILED");
        return;
    }
    LogLine("saveshot: btndraw guard ON");
}

/* ---------------- 载入收尾 (0x4F14BF) ----------------
 * ESC 菜单的命令分派把两条路分得很开: 载入 (0x51E) 看状态机返回值, 成功则把命令写进
 * 屏幕对象 (ctx 的第一个 DWORD) 后直接 ret, 上层据此收掉整个菜单、直接进游戏; 存档
 * (0x51F) 不看返回值, 无条件落到 0x4F14BF 把菜单 ShowWindow + UpdateWindow 回来。
 *
 * 我们是从存档页发起读档的, 于是世界换新了、菜单却又被显示出来 —— 表现为"停在设置页,
 * 鼠标消失, 要盲点一下才回游戏"。所以在 0x4F14BF 判断: 这次是按钮发起的读档就照载入
 * 的收尾做 (写 0x51E + ret), 别的命令一律原样执行。
 *
 * ebp = GetWindowLongA(hDlg, DWLP_USER), 即屏幕对象 (见 0x4F12B1)。
 * 站点 5 字节 = push 5; push esi; call edi, 无 rel32。 */
static volatile int s_TailLoad = 0;     /* 我们的读档跑完了, 等外层收尾 */

static void SS_InstallMenuTail(void)
{
    static const unsigned char kWant[5] = { 0x6A, 0x05, 0x56, 0xFF, 0xD7 };
    static int s_On = 0;
    unsigned char* c;
    int i = 0, jOrig = 0, jWrite = 0;

    if (s_On) return;
    if (!SS_BytesSame((unsigned char*)SS_ADDR_MENU_TAIL, kWant, 5))
    {
        LogLine("saveshot: menutail site not pristine - load tail OFF");
        return;
    }
    c = (unsigned char*)VirtualAlloc(NULL, 96, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!c) return;

    /* cmp dword [s_TailLoad], 0 ; je orig */
    c[i++] = 0x83; c[i++] = 0x3D;
    *(unsigned*)(c + i) = (unsigned)(size_t)&s_TailLoad; i += 4;
    c[i++] = 0x00;
    c[i++] = 0x74; jOrig = i++;
    /* mov dword [s_TailLoad], 0 */
    c[i++] = 0xC7; c[i++] = 0x05;
    *(unsigned*)(c + i) = (unsigned)(size_t)&s_TailLoad; i += 4;
    *(unsigned*)(c + i) = 0; i += 4;
    /* test ebp,ebp ; jz tail */
    c[i++] = 0x85; c[i++] = 0xED;
    c[i++] = 0x74; jWrite = i++;
    c[i++] = 0xC7; c[i++] = 0x45; c[i++] = 0x00;        /* mov dword [ebp], 0x51e */
    *(unsigned*)(c + i) = 0x51Eu; i += 4;
    /* tail: 按载入命令的收尾平衡栈并返回 */
    c[jWrite] = (unsigned char)(i - (jWrite + 1));
    c[i++] = 0x33; c[i++] = 0xC0;
    c[i++] = 0x5F; c[i++] = 0x5E; c[i++] = 0x5D; c[i++] = 0x5B;
    c[i++] = 0x81; c[i++] = 0xC4; *(unsigned*)(c + i) = 0x614; i += 4;
    c[i++] = 0xC2; c[i++] = 0x10; c[i++] = 0x00;
    /* orig: 原样执行被覆盖的 5 字节 (edi 在这几条命令分派里都已 = ShowWindow) */
    c[jOrig] = (unsigned char)(i - (jOrig + 1));
    c[i++] = 0x6A; c[i++] = 0x05;
    c[i++] = 0x56;
    c[i++] = 0xFF; c[i++] = 0xD7;
    c[i++] = 0xB8; *(unsigned*)(c + i) = 0x4F14C4u; i += 4;
    c[i++] = 0xFF; c[i++] = 0xE0;

    FlushInstructionCache(GetCurrentProcess(), c, (SIZE_T)i);
    if (!SS_PatchAt(SS_ADDR_MENU_TAIL, (void*)c, 5))
    {
        LogLine("saveshot: menutail patch FAILED");
        return;
    }
    s_On = 1;
    LogLine("saveshot: load tail hook ON");
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
    int w;
    if (s_Init) return;
    s_Init = 1;
    BuildPaths();
    GtEnsureLog();
    s_Enable  = GetPrivateProfileIntA(SS_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    w         = GetPrivateProfileIntA(SS_SECTION, "Width", SS_DEF_W, g_IniPath);
    s_PrevW   = GetPrivateProfileIntA(SS_SECTION, "PreviewWidth", SS_DEF_PREV, g_IniPath);
    if (w < SS_MIN_W) w = SS_MIN_W;
    if (w > SS_MAX_W) w = SS_MAX_W;
    if (s_PrevW < SS_MIN_PREV) s_PrevW = SS_MIN_PREV;
    if (s_PrevW > 420) s_PrevW = 420;
    s_W = w;
    SS_DefaultDir();
    if (!s_Enable) return;
    LogLine("saveshot: on");
    SS_InstallDlgHook();
    SS_InstallBtnGuard();
    SS_InstallMenuTail();
}

/* ---------------- 取图 ---------------- */

/* 盒式降采样: 每个目标像素在源矩形里每轴最多取 4 个点, 按通道求平均。 */
static void SS_Capture(void)
{
    void* surf = NULL;
    unsigned char* base;
    int vx, vy, vw, vh, pitch, dw, dh, i, j;
    int is565;

    if (!SB_RawView(&surf, &vx, &vy, &vw, &vh)) return;
    if (!surf || vw < 8 || vh < 8) return;
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (pitch < GtSurfGet(surf, GtVT_GET_WIDTH) * 2) return;

    dw = s_W; if (dw > vw) dw = vw;
    dh = (int)((long long)dw * vh / vw);
    if (dh < 1) dh = 1;
    if (dh > dw * 2) dh = dw * 2;          /* 极端宽高比钳一下 */

    if (!s_Pix || s_PixW != dw || s_PixH != dh)
    {
        if (s_Pix) VirtualFree(s_Pix, 0, MEM_RELEASE);
        s_Pix = (unsigned short*)VirtualAlloc(NULL, (size_t)dw * dh * 2u,
                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        s_PixW = s_Pix ? dw : 0;
        s_PixH = s_Pix ? dh : 0;
        if (!s_Pix) return;
    }

    is565 = (g_GBits == 6);
    base = GtLock(surf);
    if (!base) return;
    for (j = 0; j < dh; j++)
    {
        int sy0 = (int)((long long)j * vh / dh);
        int sy1 = (int)((long long)(j + 1) * vh / dh);
        int stepy;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        stepy = (sy1 - sy0 + 3) >> 2; if (stepy < 1) stepy = 1;
        for (i = 0; i < dw; i++)
        {
            int sx0 = (int)((long long)i * vw / dw);
            int sx1 = (int)((long long)(i + 1) * vw / dw);
            int stepx, xx, yy;
            unsigned ar = 0, ag = 0, ab = 0, n = 0;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            stepx = (sx1 - sx0 + 3) >> 2; if (stepx < 1) stepx = 1;
            for (yy = sy0; yy < sy1; yy += stepy)
            {
                const unsigned short* row = (const unsigned short*)
                    (base + (size_t)(vy + yy) * pitch + (size_t)vx * 2);
                for (xx = sx0; xx < sx1; xx += stepx)
                {
                    unsigned short c = row[xx];
                    if (is565) { ar += (c >> 11) & 31u; ag += (c >> 5) & 63u; }
                    else       { ar += (c >> 10) & 31u; ag += (c >> 5) & 31u; }
                    ab += c & 31u;
                    n++;
                }
            }
            if (!n) n = 1;
            s_Pix[(size_t)j * dw + i] = SS_Pack(ar / n, ag / n, ab / n);
        }
    }
    GtUnlock(surf);
    s_LastTick = GetTickCount();
}

void SaveShot_Tick(void)
{
    SaveShot_EnsureInit();
    if (!s_Enable) return;
    if (!*(volatile unsigned*)ADDR_SCEN_STARTED) return;   /* 未开局 */
    if (FrameCap_ActiveDlg()) return;                      /* ESC 菜单开着 */
    if (s_Loading) return;                                 /* 读档中, 世界正在重建 */
    if (s_Dlg && IsWindow((HWND)s_Dlg)) return;             /* 存/读档界面开着 */
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
    if (!p0 || !IsPlausiblePtr(p0) || !p0[0]) { LogLine("saveshot: bad path ptr"); return; }

    wide = (p0[1] == 0 && p0[0] >= 0x20 && p0[0] < 0x7F) ? 1 : 0;
    if (wide)
    {
        if (!WideCharToMultiByte(CP_ACP, 0, (const wchar_t*)p0, -1, path,
                                 MAX_PATH_LEN, NULL, NULL) || !path[0])
        {
            LogLine("saveshot: wide->ansi failed");
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

    if (dir[0])
    {
        /* 引擎给的是相对路径 ("Saved Games\SAVE0001.SAV"): 补成绝对,
         * 免得进程 CWD 变了以后读 Thumbs 读不到。 */
        if (dir[0] != '\\' && dir[1] != ':')
        {
            p = AppendCStr(buf, 0, GtGameDir(), sizeof(buf));
            p = AppendCh(buf, p, '\\', sizeof(buf));
            AppendCStr(buf, p, dir, sizeof(buf));
        }
        else AppendCStr(buf, 0, dir, sizeof(buf));
        for (k = 0; k + 1 < (unsigned)MAX_PATH_LEN && buf[k]; k++) s_SaveDir[k] = buf[k];
        s_SaveDir[k] = 0;
    }
    SS_DefaultDir();

    p = AppendCStr(buf, 0, "saveshot: path", sizeof(buf));
    p = AppendCh(buf, p, wide ? 'W' : 'A', sizeof(buf));
    p = AppendCh(buf, p, ' ', sizeof(buf));
    AppendCStr(buf, p, path, sizeof(buf));
    LogLine(buf);
}

void SaveShot_OnSaveEnd(void* regs)
{
    char dir[MAX_PATH_LEN], path[MAX_PATH_LEN], buf[MAX_PATH_LEN + 48];
    unsigned p;

    if (!regs) return;
    if (!s_Enable) return;
    if (!(*(unsigned*)((char*)regs + SS_REG_EBX) & 0xFFu)) { LogLine("saveshot: end bl=0"); return; }
    if (!s_BaseName[0]) { LogLine("saveshot: end no path"); return; }
    if (!s_Pix || s_PixW <= 0 || s_PixH <= 0) { LogLine("saveshot: no frame cached, skip"); return; }

    p = AppendCStr(dir, 0, s_SaveDir, MAX_PATH_LEN);
    p = AppendCStr(dir, p, "\\Thumbs", MAX_PATH_LEN);
    CreateDirectoryA(dir, NULL);   /* 已存在返回 ERROR_ALREADY_EXISTS, 忽略 */

    p = AppendCStr(path, 0, dir, MAX_PATH_LEN);
    p = AppendCStr(path, p, "\\", MAX_PATH_LEN);
    p = AppendCStr(path, p, s_BaseName, MAX_PATH_LEN);
    AppendCStr(path, p, ".bmp", MAX_PATH_LEN);

    p = AppendCStr(buf, 0, "saveshot: ", sizeof(buf));
    if (Shot_WriteBmp565(path, s_Pix, s_PixW, s_PixH))
    {
        p = AppendCStr(buf, p, "write ", sizeof(buf));
        SS_ImgFree(&s_ImgB);        /* 新写的这张要能被下次读档读到 */
    }
    else
        p = AppendCStr(buf, p, "write FAILED ", sizeof(buf));
    AppendCStr(buf, p, path, sizeof(buf));
    LogLine(buf);
}

/* ---------------- 找对话框 ---------------- */

/* 对话框是 #32770, 但窗口被撑成整屏, 只能靠"有没有我们的列表控件"认。 */
typedef struct { HWND hwnd; int listId; } SS_Find;

static BOOL CALLBACK SS_EnumCb(HWND h, LPARAM lp)
{
    SS_Find* f = (SS_Find*)lp;
    char cls[24];
    int n = GetClassNameA(h, cls, sizeof(cls));
    if (n < 2 || cls[0] != '#' || cls[1] != '3') return TRUE;
    if (!SS_StrEq(cls, "#32770")) return TRUE;
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
    if (!h || !GetWindowRect(h, out)) return 0;
    return 1;
}

/* 面板尺寸: 引擎按模板换算成像素并居中, 固定 800x600。 */
static void SS_PanelRect(HWND hDlg, RECT* client)
{
    RECT rc;
    GetClientRect(hDlg, client);
    rc.left = 0; rc.top = 0;
    rc.right  = SS_TPL_CX;
    rc.bottom = SS_TPL_CY;
    if (!MapDialogRect(hDlg, &rc) || rc.right < 240 || rc.bottom < 180)
    {
        rc.right  = SS_TPL_PX_W;
        rc.bottom = SS_TPL_PX_H;
    }
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
        if (c == list) continue;
        if (GetDlgCtrlID(c) != 0) continue;
        if (!SS_WinRect(c, &r)) continue;
        if (r.left < lr->right - 2 || r.left > lr->right + 8) continue;
        if (r.top < lr->top - 6 || r.top > lr->top + 6) continue;
        if (r.bottom < lr->bottom - 6 || r.bottom > lr->bottom + 6) continue;
        if (r.right - r.left < 8 || r.right - r.left > 40) continue;
        return c;
    }
    return NULL;
}

/* 排一次版: 列表左移到留白处、往右伸长到截图列边上; 右侧一列竖排两个截图框;
 * 载入/删除排在截图列下方, 与储存按钮同宽同高、左缘对齐。所有坐标必须在面板内。 */
static int SS_Layout(HWND hDlg, HWND list, int listId)
{
    RECT cr, o, r, band, sr;
    HWND c, btn, title, sb;
    const int pad = 12;
    int avail, listW, listX, colW, colX, dx, dw, bw = 0, bh = 0, sbW = 0;
    int bandR = 0;      /* 输入框右缘上限: 载入/删除 占掉的地方 */

    s_MoverY = s_MoverH = 0;

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

    s_ListRect.left   = listX;
    s_ListRect.top    = o.top;
    s_ListRect.right  = listX + listW;
    s_ListRect.bottom = o.bottom;
    MoveWindow(list, listX, s_ListRect.top, listW, s_ListRect.bottom - s_ListRect.top, TRUE);

    if (sb)
        MoveWindow(sb, listX + listW, (int)sr.top, sbW, (int)(sr.bottom - sr.top), TRUE);

    colX = listX + listW + sbW + pad;

    /* 底部那条带: [存档名输入框 ...][储存] —— 输入框右缘让到按钮左边 */
    btn = GetDlgItem(hDlg, listId == SS_ID_LIST_SAVE ? SS_ID_BTN_SAVE : SS_ID_BTN_LOAD);
    SetRect(&band, 0, 0, 0, 0);
    SetRect(&s_BtnLoad, 0, 0, 0, 0);
    SetRect(&s_BtnDel, 0, 0, 0, 0);
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
            if (c == list || c == sb) continue;
            if (c == s_HWndLoad || c == s_HWndDel) continue;
            if (!IsWindowVisible(c)) continue;
            if (!SS_WinRect(c, &mr)) continue;
            if (mr.top < o.bottom - 6) continue;                  /* 只看列表下方 */
            if (mr.right <= listX || mr.left >= listX + listW) continue;
            if (mr.bottom - mr.top > 60) continue;                /* 大块的不动 */
            mover = c;
            break;
        }
        if (mover)
        {
            int eh = (int)(mr.bottom - mr.top);
            int nl = (int)mr.left + dx;
            int nw = (int)(mr.right - mr.left) + dw;
            int ntop = bottomLimit - eh;

            if (bandR > 0 && nl + nw > bandR - pad) nw = bandR - pad - nl;
            if (nw < (int)(mr.right - mr.left)) nw = (int)(mr.right - mr.left);
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
            MoveWindow(list, listX, s_ListRect.top, listW,
                       newBottom - (int)o.top, TRUE);
        }
    }

    /* 与列表左缘对齐、贴着列表下沿的控件跟着挪+加宽, 但别伸进截图列 */
    if (dw || dx)
    {
        for (c = GetWindow(hDlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        {
            int nl, nw;
            if (c == list || c == sb) continue;
            if (c == s_HWndLoad || c == s_HWndDel) continue;
            if (!SS_WinRect(c, &r)) continue;
            if (r.left < o.left - 8 || r.left > o.left + 8) continue;
            if (r.top < o.bottom - 6 || r.top > o.bottom + 40) continue;
            nl = (int)r.left + dx;
            nw = (int)(r.right - r.left) + dw;
            if (bandR > 0 && nl + nw > bandR - pad) nw = bandR - pad - nl;
            if (nw < (int)(r.right - r.left)) nw = (int)(r.right - r.left);
            MoveWindow(c, nl, (int)r.top, nw, (int)(r.bottom - r.top), TRUE);
        }
    }

    /* 标题跟着列表走 (模板里两者同左缘同宽) */
    title = GetDlgItem(hDlg, SS_ID_TITLE);
    if (title && SS_WinRect(title, &r))
        MoveWindow(title, listX, (int)r.top, listW, (int)(r.bottom - r.top), TRUE);

    if (colW > 0)
    {
        /* 框高 = 图高 + 2: 图片按比例 fit 进去, 框比图高就上下留黑边 */
        int vw = s_PixW, vh = s_PixH, imgW, imgH, availH, need;

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

        availH = (int)(s_ListRect.bottom - s_ListRect.top);
        need = 2 * (s_CapH + imgH + 2) + pad;
        if (need > availH)                       /* 塞不下就等比缩, 宁小不留黑边 */
        {
            imgH = (availH - pad - 2 * s_CapH - 4) / 2;
            if (imgH < 24) imgH = 24;
            imgW = (int)((long long)imgH * vw / vh);
            if (imgW < 48) imgW = 48;
        }

        s_CapAx = colX; s_CapAy = o.top;
        s_BoxA.left   = colX;
        s_BoxA.top    = o.top + s_CapH;
        s_BoxA.right  = colX + imgW + 2;
        s_BoxA.bottom = s_BoxA.top + imgH + 2;

        s_CapBx = colX; s_CapBy = s_BoxA.bottom + pad;
        s_BoxB.left   = colX;
        s_BoxB.top    = s_CapBy + s_CapH;
        s_BoxB.right  = s_BoxA.right;
        s_BoxB.bottom = s_BoxB.top + imgH + 2;
    }
    else
    {
        s_CapAx = s_CapAy = s_CapBx = s_CapBy = 0;
        SetRect(&s_BoxA, 0, 0, 0, 0);
        SetRect(&s_BoxB, 0, 0, 0, 0);
    }

    /* 主按钮: 挪到面板底部那条带, 与存档名输入框垂直居中 */
    if (btn && bh > 0 && band.right > band.left)
    {
        if (s_MoverH > 0)
            band.top = s_MoverY + (s_MoverH - bh) / 2;
        if (band.top + bh > s_Py + s_PanelH - 4) band.top = s_Py + s_PanelH - 4 - bh;
        if (s_BtnLoad.right > s_BtnLoad.left)
        {
            int dy = band.top - s_BtnLoad.top;
            OffsetRect(&s_BtnLoad, 0, dy);
            OffsetRect(&s_BtnDel, 0, dy);
        }
        MoveWindow(btn, band.left, band.top, bw, bh, TRUE);
    }

    /* 载入 / 删除: 截图列下方那块空带, 上下排成一列。放不下就整块不画。 */
    if (listId == SS_ID_LIST_SAVE && colW > 0 && bw > 0 && bh > 0)
    {
        int bx   = (int)band.left;
        int bot  = (int)band.top - 6;
        int topL = (int)s_BoxB.bottom + 6;
        int yDel = bot - bh;
        int yLd  = yDel - 6 - bh;

        if (yLd >= topL)
        {
            SetRect(&s_BtnLoad, bx, yLd,  bx + bw, yLd + bh);
            SetRect(&s_BtnDel,  bx, yDel, bx + bw, yDel + bh);
        }
    }

    if (s_HWndLoad && IsWindow(s_HWndLoad))
    {
        if (s_BtnLoad.right > s_BtnLoad.left)
        {
            MoveWindow(s_HWndLoad, (int)s_BtnLoad.left, (int)s_BtnLoad.top,
                       (int)(s_BtnLoad.right - s_BtnLoad.left),
                       (int)(s_BtnLoad.bottom - s_BtnLoad.top), TRUE);
            ShowWindow(s_HWndLoad, SW_SHOW);
        }
        else ShowWindow(s_HWndLoad, SW_HIDE);
    }
    if (s_HWndDel && IsWindow(s_HWndDel))
    {
        if (s_BtnDel.right > s_BtnDel.left)
        {
            MoveWindow(s_HWndDel, (int)s_BtnDel.left, (int)s_BtnDel.top,
                       (int)(s_BtnDel.right - s_BtnDel.left),
                       (int)(s_BtnDel.bottom - s_BtnDel.top), TRUE);
            ShowWindow(s_HWndDel, SW_SHOW);
        }
        else ShowWindow(s_HWndDel, SW_HIDE);
    }

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

/* 截图框: 深底 + 一圈亮边, 空的时候看得见框在哪。一次 Lock 画完。 */
static void SS_DrawBox(void* surf, const RECT* b, const SS_Img* im, const wchar_t* cap,
                       int capx, int capy)
{
    int bw = b->right - b->left, bh = b->bottom - b->top;
    unsigned char* base;
    int pitch, W, H, x, y;
    unsigned short fill, edge;

    if (bw < 12 || bh < 12) return;

    if (cap && *cap)
        SS_PutTextLine(surf, cap, capx, capy, s_CapH, 206, 210, 220);

    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || pitch < W * 2) return;

    fill = Blend16(0, 10, 10, 14, 255);
    edge = Blend16(0, 96, 98, 108, 255);
    base = GtLock(surf);
    if (!base) return;
    for (y = 0; y < bh; y++)
    {
        int sy = b->top + y;
        unsigned short* row;
        if (sy < 0 || sy >= H) continue;
        row = (unsigned short*)(base + (size_t)sy * pitch);
        for (x = 0; x < bw; x++)
        {
            int sx = b->left + x;
            if (sx < 0 || sx >= W) continue;
            row[sx] = (x == 0 || y == 0 || x == bw - 1 || y == bh - 1) ? edge : fill;
        }
    }
    GtUnlock(surf);

    if (im->ok && im->px && im->w > 0 && im->h > 0)
    {
        SS_Blit(surf, b->left + (bw - im->w) / 2, b->top + (bh - im->h) / 2,
                im->px, im->w, im->h);
    }
    else
    {
        static const wchar_t* kNone = L"无截图";
        int tw = SS_TextW(kNone);
        if (tw > 0 && tw < bw - 8)
            SS_PutTextLine(surf, kNone, b->left + (bw - tw) / 2,
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

/* ---------------- 列表行 ----------------
 * 层级: [面板画进临时面] → [0x622396, present 前最后一跳, 我们在这画] → [上屏]
 *       → [之后 Windows 才派发子窗口的 WM_PAINT]
 * 所以控件自画会晚于我们 (两套文字), 早画又会被面板 blit 盖掉。结论: 控件一个像素
 * 都不画, 行全部由 SS_DrawRows 画进 surf, 上面不会再有任何东西盖它。 */
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
    cur = (int)SendMessageA(list, SS_LB_GETCURSEL, 0, 0);
    top = (int)SendMessageA(list, SS_LB_GETTOPINDEX, 0, 0);
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
        WNDPROC old = s_OldListProc;
        if (s_SubList == h) { s_SubList = NULL; s_OldListProc = NULL; }
        if (old) SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)old);
        if (old) return CallWindowProcA(old, h, msg, wp, lp);
    }
    if (s_OldListProc) return CallWindowProcA(s_OldListProc, h, msg, wp, lp);
    return DefWindowProcA(h, msg, wp, lp);
}

/* 换一个列表窗口就重挂一次 (对话框每次打开都是新控件)。 */
static void SS_HookList(HWND list)
{
    WNDPROC old;

    if (!list || !IsWindow(list)) return;
    if (s_SubList == list) return;
    old = (WNDPROC)SetWindowLongPtrA(list, GWLP_WNDPROC, (LONG_PTR)SS_ListProc);
    if (!old) { LogLine("saveshot: list subclass FAILED"); return; }
    s_OldListProc = old;
    s_SubList = list;
    s_RhList = NULL;                      /* 新控件: 行高要重抬 */
    s_DrawnCur = s_DrawnTop = -2;
}

/* 行文字完全跟随 [VecText] 的字形/字号/粗体, 与标题、按钮同源。
 * 槽位用主字号槽 2: GdiEnsure 参数与 VecText 完全同参, 不会互拆重建。 */
#define SS_VEC_SLOT  2
#define SS_ROW_GAP   3      /* 行间空隙 (墨迹之外) */
#define SS_ROW_PAD   3      /* 边框到第一行之间也要留白 */
#define SS_EDGE_R 123
#define SS_EDGE_G 121
#define SS_EDGE_B 123

static const wchar_t* SS_Face(void)
{
    const wchar_t* f = VecText_FontFace();
    return (f && *f) ? f : L"msyh.ttc";
}

/* 行距不能用字体 cell (g_G->adv): msyh 的 cell 含 ~6px leading, 按它排行会空出
 * 8~10px。量一次墨迹 (同字号只量一次) 用 inkH/inkTop 排。 */
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

/* 画一行 (调用方已 Lock)。maxW>0 时按宽度截断, right=1 时 x 是右缘。 */
static void SS_PutText(unsigned char* base, int pitch, int W, int H,
                       const wchar_t* s, int x, int y, int maxW, int right,
                       unsigned r, unsigned g, unsigned b)
{
    int px, n, w;

    if (!base || !s || !*s) return;
    px = VecText_ResolvedPx();
    if (!GdiEnsure(px, VecText_Bold(), SS_VEC_SLOT, SS_Face())) return;
    n = lstrlenW(s);
    if (n <= 0) return;
    if (maxW > 0)
    {
        int fit = CharsFit(s, n, maxW);
        if (fit <= 0) return;
        n = fit;
    }
    w = RenderLine(s, n);
    if (w <= 0 || g_G->adv <= 0) return;
    if (right) x -= w;
    if (x < 0 || y < 0 || x + w > W || y + g_G->adv > H) return;
    BlitCoverage(base, pitch, W, H, x, y, w, g_G->adv, r, g, b, 100);
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

static int SS_TextW(const wchar_t* s)
{
    if (!s || !*s) return 0;
    if (!GdiEnsure(VecText_ResolvedPx(), VecText_Bold(), SS_VEC_SLOT, SS_Face())) return 0;
    return RenderLine(s, lstrlenW(s));
}

/* 把列表客户区画进 surf (屏幕坐标)。左 = 档名, 右 = 完整时间 —— 引擎把日期列宽
 * 写死 (~55px, 永远 "2026..."), 加宽列表没用, 只能整张重画。 */
static void SS_DrawRows(void* surf, HWND list)
{
    RECT cr;
    POINT p;
    int cnt, top, cur, rh, i, j, x0, y0, x1, y1, W, H, pitch, w, px, adv, tw, maxName;
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

    cnt = (int)SendMessageA(list, SS_LB_GETCOUNT, 0, 0);
    top = (int)SendMessageA(list, SS_LB_GETTOPINDEX, 0, 0);
    cur = (int)SendMessageA(list, SS_LB_GETCURSEL, 0, 0);
    rh  = (int)SendMessageA(list, SS_LB_GETITEMHEIGHT, 0, 0);

    /* 行高 = 墨迹高 + 空隙, 每个控件只设一次 (每帧设会让控件反复重排) */
    want = (inkH > 0) ? (inkH + SS_ROW_GAP) : (px + 4);
    if (want < 10) want = 10;
    if (rh != want && s_RhList != list)
    {
        SendMessageA(list, SS_LB_SETITEMHEIGHT, 0, (LPARAM)want);
        s_RhList = list;
    }
    rh = want;
    if (top < 0 || top >= cnt) top = 0;

    cBg   = Blend16(0, 0, 0, 0, 255);
    cSel  = Blend16(0, 123, 32, 123, 255);
    cEdge = Blend16(0, SS_EDGE_R, SS_EDGE_G, SS_EDGE_B, 255);

    base = GtLock(surf);
    if (!base) return;

    for (j = y0; j < y1; j++)
    {
        unsigned short* row = (unsigned short*)(base + (size_t)j * pitch);
        for (i = x0; i < x1; i++) row[i] = cBg;
    }
    if (cur >= top && cur < cnt)
    {
        int sy0 = iy0 + (cur - top) * rh, sy1 = sy0 + rh;
        if (sy1 > y1 - 2) sy1 = y1 - 2;
        if (sy0 < y0 + 2) sy0 = y0 + 2;
        for (j = sy0; j < sy1; j++)
        {
            unsigned short* row = (unsigned short*)(base + (size_t)j * pitch);
            for (i = x0 + 2; i < x1 - 2; i++) row[i] = cSel;
        }
    }

    /* 外框: 面板美术里那圈 2px 灰边 (与存档名输入框同色) */
    for (j = y0; j < y1; j++)
    {
        unsigned short* row = (unsigned short*)(base + (size_t)j * pitch);
        row[x0] = cEdge; row[x0 + 1] = cEdge;
        row[x1 - 1] = cEdge; row[x1 - 2] = cEdge;
    }
    for (i = x0; i < x1; i++)
    {
        ((unsigned short*)(base + (size_t)y0 * pitch))[i] = cEdge;
        ((unsigned short*)(base + (size_t)(y0 + 1) * pitch))[i] = cEdge;
        ((unsigned short*)(base + (size_t)(y1 - 1) * pitch))[i] = cEdge;
        ((unsigned short*)(base + (size_t)(y1 - 2) * pitch))[i] = cEdge;
    }

    for (i = top; i < cnt && inkH > 0; i++)
    {
        unsigned fe;
        wchar_t nm[64], dt[28];
        /* y = 这行 DIB 的顶部 (不是墨迹顶): 墨迹在 rh 里居中, 所以回退 inkTop */
        int y = iy0 + (i - top) * rh + (rh - inkH) / 2 - inkTop;

        if (y + adv > y1 - 2) break;

        fe = (unsigned)(size_t)SendMessageA(list, SS_LB_GETITEMDATA, (WPARAM)i, 0);
        SS_RowText(list, fe, i, nm, 64);
        SS_RowTime(fe, dt);

        tw = 0;
        if (dt[0])
        {
            int dw = RenderLine(dt, lstrlenW(dt));
            if (dw > 0) tw = dw;
        }
        if (tw > 0 && tw < w - 40)
            SS_PutText(base, pitch, W, H, dt, x1 - 4, y, 0, 1, 214, 210, 218);

        if (nm[0])
        {
            int dim = 0;
            maxName = w - 12 - (tw > 0 ? tw + 8 : 0);
            if (fe && IsPlausiblePtr((const void*)(size_t)fe))
            {
                if (!*(const unsigned char*)(size_t)(fe + SS_FE_VALID) ||
                    *(const unsigned char*)(size_t)(fe + SS_FE_WRONG))
                    dim = 1;
            }
            if (maxName > 12)
            {
                if (dim) SS_PutText(base, pitch, W, H, nm, x0 + 5, y, maxName, 0, 128, 126, 134);
                else     SS_PutText(base, pitch, W, H, nm, x0 + 5, y, maxName, 0, 239, 235, 239);
            }
        }
    }

    GtUnlock(surf);
}

/* ---------------- 载入 / 删除 ---------------- */

#define SS_DWLP_USER     8
#define SS_BTN_PAGE_LOAD 1        /* 引擎状态机的页面号: 1=载入 2=储存 3=删除 */
#define SS_MODE_LOAD     0x40F    /* 载入页按钮 id, 也是状态机认的"载入"动作码 */

/* 删存档不可逆: 第一下只是"上膛", 4 秒内再点一下才真删 (按钮字会变) */
static int      s_DelArm = 0;
static unsigned s_DelArmTick = 0;
#define SS_DEL_ARM_MS 4000

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
    if (!list || !sav || !bmp) return 0;
    if (!SS_SelectedFile(list, nm)) return 0;

    if (SS_HasDir(nm))
    {
        if (nm[0] != '\\' && !(nm[0] && nm[1] == ':'))
        {
            p = AppendCStr(sav, 0, GtGameDir(), savN);
            p = AppendCh(sav, p, '\\', savN);
            AppendCStr(sav, p, nm, savN);
        }
        else AppendCStr(sav, 0, nm, savN);
    }
    else
    {
        p = AppendCStr(sav, 0, s_SaveDir, savN);
        p = AppendCh(sav, p, '\\', savN);
        AppendCStr(sav, p, nm, savN);
    }

    SS_SplitPath(sav, dir, base);
    if (!base[0]) return 0;
    p = AppendCStr(bmp, 0, dir[0] ? dir : s_SaveDir, bmpN);
    p = AppendCStr(bmp, p, "\\Thumbs\\", bmpN);
    p = AppendCStr(bmp, p, base, bmpN);
    AppendCStr(bmp, p, ".bmp", bmpN);
    return 1;
}

/* 删除: 自己动手 (.SAV + 缩略图), 再从列表摘掉那一行 —— 人留在存档页,
 * 不用像走引擎删除页那样被弹走。 */
static void SS_DoDelete(HWND dlg, HWND list)
{
    char sav[MAX_PATH_LEN], bmp[MAX_PATH_LEN], msg[MAX_PATH_LEN + 48];
    int cur, n;
    unsigned p, now;

    if (!list) return;
    cur = (int)SendMessageA(list, SS_LB_GETCURSEL, 0, 0);
    if (cur < 0) { LogLine("saveshot: del no sel"); return; }

    now = GetTickCount();
    if (!s_DelArm || now - s_DelArmTick > SS_DEL_ARM_MS)
    {
        s_DelArm = 1;
        s_DelArmTick = now;
        if (dlg && IsWindow(dlg)) InvalidateRect(dlg, NULL, FALSE);
        return;
    }
    s_DelArm = 0;

    if (!SS_EntryFile(list, sav, MAX_PATH_LEN, bmp, MAX_PATH_LEN))
    { LogLine("saveshot: del no path"); return; }

    p = AppendCStr(msg, 0, DeleteFileA(sav) ? "saveshot: del ok " : "saveshot: del FAIL ",
                   sizeof(msg));
    AppendCStr(msg, p, sav, sizeof(msg));
    LogLine(msg);
    if (bmp[0]) DeleteFileA(bmp);

    SendMessageA(list, SS_LB_DELETESTRING, (WPARAM)cur, 0);
    n = (int)SendMessageA(list, SS_LB_GETCOUNT, 0, 0);
    if (cur >= n) cur = n - 1;
    SendMessageA(list, SS_LB_SETCURSEL, (WPARAM)cur, 0);

    SS_ImgFree(&s_ImgB);          /* 截图框跟着换 (没了就是"无截图") */
    s_DrawnCur = -2;
    if (dlg && IsWindow(dlg)) InvalidateRect(dlg, NULL, FALSE);
}

/* 载入前把界面交还给引擎: 还原列表子类化、忘掉对话框与截图缓存。
 * 读档会重建整个世界 (surface/FileEntry/控件全部作废), 之后碰它们都是野指针。
 * 两个按钮句柄不在这丢 —— 它们随对话框一起销毁, 由 SS_ResetDlg 按 IsWindow 判。 */
static void SS_Detach(HWND list)
{
    if (list && s_SubList == list && s_OldListProc)
    {
        SetWindowLongPtrA(list, GWLP_WNDPROC, (LONG_PTR)s_OldListProc);
        s_SubList = NULL;
        s_OldListProc = NULL;
    }
    s_Dlg = NULL;
    s_DlgList = 0;
    s_Scroll = NULL;
    s_LayoutW = s_LayoutH = 0;
    s_LayoutN = 0;
    SetRect(&s_BtnLoad, 0, 0, 0, 0);
    SetRect(&s_BtnDel, 0, 0, 0, 0);
    SS_ImgFree(&s_ImgA);
    SS_ImgFree(&s_ImgB);
}

/* 载入: 不自己调 LoadGame (那要接屏幕栈/载入画面, 半路调用必崩), 而是把状态机
 * (0x558DD0) 的"页面/动作"两字段改成用户点载入页时的样子, 让它下一帧按自己的流程
 * 走完 —— 用列表里当前选中项。
 *
 *   ctx = GetWindowLong(dlg, DWLP_USER)   对话框过程自己也从这里取
 *   ctx+0x04 = 页面 (1 载入 / 2 储存 / 3 删除)
 *   ctx+0x18 = 动作 (-1 空闲, 1 本页按钮, 0x40F 载入, 2 返回)
 *
 * 空闲值是 -1 不是 0。动作分派 (0x559030) 只按**页面**决定做什么, 动作值只过
 * "是不是本页按钮"那道门 —— 所以必须同时把页面改成 1, 否则就是在存档页上执行一次
 * 存档, 覆盖用户档。判据对不上就当没点。 */
static void SS_DoLoad(HWND dlg, HWND list)
{
    void* ctx;
    int page, mode;

    s_DelArm = 0;
    if (!list) return;
    if ((int)SendMessageA(list, SS_LB_GETCURSEL, 0, 0) < 0)
    { LogLine("saveshot: load no sel"); return; }

    ctx = (void*)GetWindowLongPtrA(dlg, SS_DWLP_USER);
    if (!ctx || !IsPlausiblePtr(ctx)) { LogLine("saveshot: load no ctx"); return; }
    page = *(int*)((char*)ctx + 4);
    mode = *(int*)((char*)ctx + 0x18);

    if (page != 2 || mode != -1)
    {
        char buf[80];
        unsigned p = AppendCStr(buf, 0, "saveshot: load skip page=", sizeof(buf));
        p = SS_AppendInt(buf, p, page, sizeof(buf));
        p = AppendCStr(buf, p, " mode=", sizeof(buf));
        SS_AppendInt(buf, p, mode, sizeof(buf));
        LogLine(buf);
        return;
    }

    /* 对话框保持原样 (不隐藏): 外层收尾依赖状态机最后把它正常销毁。
     * 这期间可能撞上 UI 主题资源已被 LoadGame 释放的重绘, 由 0x6071E0 守卫兜。 */
    s_LoadOldDlg = dlg;
    SS_Detach(list);
    s_Loading = GetTickCount(); if (!s_Loading) s_Loading = 1;

    *(int*)((char*)ctx + 4)    = SS_BTN_PAGE_LOAD;
    *(int*)((char*)ctx + 0x18) = SS_MODE_LOAD;
    /* 外层是存档命令 (0x51F), 收尾会把菜单重新显示出来; 立标志让 0x4F14BF 的桩
     * 改走载入的收尾, 菜单才不会在载入完成后又冒出来。 */
    s_TailLoad = 1;
    LogLine("saveshot: load asked");
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

/* 按钮文字存在按钮对象的某个字段里 (引擎自己 malloc 的宽串副本), 绘制时直接读它,
 * 不读窗口文本。字段偏移没有文档, 打开对话框时用文本已知的"储存"按钮探一次:
 * 扫 +0x04..+0x200 的 DWORD, 找指向 L"储存" 的那个。 */
static int s_TxtOff = -2;              /* -2 未探过, -1 探失败, 其它 = 字段偏移 */

static int SS_WStrEq(const wchar_t* a, const wchar_t* b)
{
    int i;
    if (!a || !b) return 0;
    for (i = 0; i < 24; i++)
    {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 0;
}

static int SS_ProbeTextOff(void* obj, const wchar_t* want)
{
    int k;
    if (!obj || !IsPlausiblePtr(obj) || !want) return -1;
    for (k = 0x04; k <= 0x200; k += 4)
    {
        unsigned v = *(unsigned*)((char*)obj + k);
        if (v < 0x10000u || (v & 1u)) continue;
        if (!IsPlausiblePtr((const void*)(size_t)v)) continue;
        if (SS_WStrEq((const wchar_t*)(size_t)v, want)) return k;
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
    int n = 0;

    if (!btn || !IsWindow(btn) || !s || s_TxtOff < 0) return;
    e = SS_OwnerDrawElem(btn);
    if (!e || !IsPlausiblePtr(e)) return;
    while (s[n]) n++;
    d = (wchar_t*)((SS_MallocFn)0x7C8E17u)(((size_t)n + 1) * sizeof(wchar_t));
    if (!d) return;
    memcpy(d, s, ((size_t)n + 1) * sizeof(wchar_t));
    *(wchar_t**)((char*)e + s_TxtOff) = d;
    InvalidateRect(btn, NULL, TRUE);
}

/* 把字体和底图从"储存"按钮搬给新按钮, 顺便探出文字字段偏移。 */
static void SS_StyleButtons(HWND cont)
{
    HFONT f;
    unsigned* ec;
    HWND mine[2];
    int k, n = 0;

    if (!cont) return;
    if (s_HWndLoad && IsWindow(s_HWndLoad)) mine[n++] = s_HWndLoad;
    if (s_HWndDel  && IsWindow(s_HWndDel))  mine[n++] = s_HWndDel;
    if (!n) return;

    f = (HFONT)SendMessageW(cont, WM_GETFONT, 0, 0);
    if (f) for (k = 0; k < n; k++) SendMessageW(mine[k], WM_SETFONT, (WPARAM)f, TRUE);

    ec = SS_OwnerDrawElem(cont);
    if (!ec) { LogLine("saveshot: native buttons no elem (plain look)"); return; }
    for (k = 0; k < n; k++)
    {
        unsigned* eb = SS_OwnerDrawElem(mine[k]);
        if (eb) *(unsigned*)((char*)eb + 0xB0) = *(unsigned*)((char*)ec + 0xB0);
        InvalidateRect(mine[k], NULL, TRUE);
    }
    LogLine("saveshot: native buttons +band");

    s_TxtOff = SS_ProbeTextOff((char*)ec, L"储存");
    if (s_TxtOff < 0) LogLine("saveshot: btn text off not found (字不会变)");
}

/* 存档页对话框过程 (0x558B90 的替身)。stdcall: 原过程结尾 ret 0x10, 我们原样转。 */
static INT_PTR __stdcall SS_SaveDlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    typedef INT_PTR (__stdcall *DlgProcFn)(HWND, UINT, WPARAM, LPARAM);

    if (msg == WM_INITDIALOG)
    {
        INT_PTR r;
        HWND cont = GetDlgItem(h, SS_ID_BTN_SAVE);
        HWND gotLoad = GetDlgItem(h, SS_ID_BTN_LOAD_NEW);
        RECT rc;

        s_HWndLoad = s_HWndDel = NULL;
        s_DelArm = 0;
        s_DelArmShown = 0;
        /* 必须在原过程之前建: 引擎的 WM_INITDIALOG 会遍历子控件, 只有它接管的
         * 按钮才会用原版美术画。文本给 CSF 键 (GUI:Load / GUI:Delete) —— 引擎把
         * 窗口文本当键去 StringTable 查, 查不到就显示 MISSING。初始坐标放到窗口
         * 外, 等 SS_Layout 算好再摆 (否则会临时压在"储存"上)。 */
        if (cont && !gotLoad && GetWindowRect(cont, &rc))
        {
            int w = (int)(rc.right - rc.left), hh = (int)(rc.bottom - rc.top);
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
            LogLine((s_HWndLoad && s_HWndDel) ? "saveshot: native buttons created"
                                              : "saveshot: native buttons FAILED");
        }
        r = s_OrigSaveDlg
            ? ((DlgProcFn)s_OrigSaveDlg)(h, msg, wp, lp) : FALSE;
        SS_StyleButtons(cont);
        return r;
    }

    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED)
    {
        int id = LOWORD(wp);
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
        /* 只有"按载入跑完"才保留收尾标志: 读档成功时 ctx+0x18 还是 0x40F
         * (0x5594AE 用它当返回值), 取消/失败会复位成 -1 —— 那时必须清掉,
         * 否则下一次存档或删除会被 0x4F14BF 的桩误当成载入收尾。 */
        if (s_TailLoad)
        {
            void* ctx = (void*)GetWindowLongPtrA(h, SS_DWLP_USER);
            if (!(ctx && IsPlausiblePtr(ctx) &&
                  *(int*)((char*)ctx + 0x18) == SS_MODE_LOAD))
            {
                s_TailLoad = 0;
                LogLine("saveshot: load tail dropped");
            }
        }
    }

    if (s_OrigSaveDlg)
        return ((DlgProcFn)s_OrigSaveDlg)(h, msg, wp, lp);
    return FALSE;
}

/* ---------------- 显示 ---------------- */

static int SS_DlgAlive(void)
{
    if (!s_Dlg || !s_DlgList) return 0;
    if (!IsWindow((HWND)s_Dlg)) return 0;
    return GetDlgItem((HWND)s_Dlg, s_DlgList) != NULL;
}

static void SS_ResetDlg(void)
{
    s_Dlg = NULL;
    s_DlgList = 0;
    s_Scroll = NULL;
    s_LayoutW = s_LayoutH = 0;
    s_LayoutN = 0;
    s_DrawnCur = s_DrawnTop = -2;
    /* 控件没了才丢 WNDPROC 记录; 还在的话 (只是重新认对话框) 别丢, 否则会重挂 */
    if (s_SubList && !IsWindow(s_SubList)) { s_SubList = NULL; s_OldListProc = NULL; }
    if (s_HWndLoad && !IsWindow(s_HWndLoad)) s_HWndLoad = NULL;
    if (s_HWndDel && !IsWindow(s_HWndDel)) s_HWndDel = NULL;
    SetRect(&s_BtnLoad, 0, 0, 0, 0);
    SetRect(&s_BtnDel, 0, 0, 0, 0);
    SS_ImgFree(&s_ImgA);
    SS_ImgFree(&s_ImgB);
}

void SaveShot_PaintDialog(void* regs, void* surf)
{
    HWND hwnd, list;
    int listId = 0, aw, ah, bw, bh;
    char name[SS_NAME_MAX];
    RECT cr;

    (void)regs;
    SaveShot_EnsureInit();
    if (!s_Enable || !surf) return;
    if (!IsPlausiblePtr(surf)) return;

    /* 已请引擎读档: 旧对话框还在 (只是即将被销毁), 世界正在重建 —— 那一刻再去认它、
     * 拿它的 FileEntry 和控件指针都可能已作废。等它真的没了再恢复。 */
    if (s_Loading)
    {
        if (s_LoadOldDlg && !IsWindow(s_LoadOldDlg))
        {
            s_Loading = 0;
            s_LoadOldDlg = NULL;
            SS_ResetDlg();
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
        if (!hwnd) { SS_ResetDlg(); return; }
        SS_ResetDlg();
        s_Dlg = (void*)hwnd;
        s_DlgList = listId;
    }

    list = GetDlgItem(hwnd, listId);
    if (!list) { SS_ResetDlg(); return; }

    SS_PanelRect(hwnd, &cr);
    if ((!s_LayoutW || s_PanelW != s_LayoutW || s_PanelH != s_LayoutH ||
         SS_LayoutDrift(list)) && s_LayoutN < 3)
    {
        SS_ImgFree(&s_ImgA);
        SS_ImgFree(&s_ImgB);
        if (!SS_Layout(hwnd, list, listId)) return;
    }

    SS_KeepScroll(list);
    SS_HookList(list);
    SS_DrawRows(surf, list);

    /* 删除按钮上膛: 直接改按钮对象里的文字字段 (引擎绘制只读那份副本) */
    if (listId == SS_ID_LIST_SAVE && s_HWndDel && IsWindow(s_HWndDel))
    {
        int arm = (s_DelArm && GetTickCount() - s_DelArmTick <= SS_DEL_ARM_MS) ? 1 : 0;
        if (s_DelArm && !arm) s_DelArm = 0;          /* 4 秒没点, 落回"删除" */
        if (arm != s_DelArmShown)
        {
            s_DelArmShown = arm;
            SS_SetBtnText(s_HWndDel, arm ? L"确认删除" : L"删除");
        }
    }

    if (!s_BoxA.right || s_BoxA.right <= s_BoxA.left) return;

    /* 上框: 当前画面 (滚动缓存, 对话框开着时不刷新) */
    aw = s_BoxA.right - s_BoxA.left - 2;   /* 框内留 1px 边框 */
    ah = s_BoxA.bottom - s_BoxA.top - 2;
    if (!s_ImgA.ok && s_Pix && s_PixW > 0 && s_PixH > 0)
        SS_ImgResample(&s_ImgA, (const unsigned char*)s_Pix, s_PixW, s_PixH,
                       s_PixW * 2, 1, aw, ah);

    /* 下框: 列表选中那一档已存的截图 (主名, 去掉目录与扩展名) */
    bw = s_BoxB.right - s_BoxB.left - 2;
    bh = s_BoxB.bottom - s_BoxB.top - 2;
    {
        char nm[40];
        if (!SS_SelectedFile(list, nm)) nm[0] = 0;
        SS_SplitPath(nm, NULL, name);
    }
    if (!SS_StrEq(name, s_ImgB.name))
    {
        SS_ImgFree(&s_ImgB);
        if (name[0])
        {
            char path[MAX_PATH_LEN], msg[MAX_PATH_LEN + 32];
            unsigned p, k;
            for (k = 0; k + 1 < (unsigned)SS_NAME_MAX && name[k]; k++) s_ImgB.name[k] = name[k];
            s_ImgB.name[k] = 0;
            p = AppendCStr(path, 0, s_SaveDir, MAX_PATH_LEN);
            p = AppendCStr(path, p, "\\Thumbs\\", MAX_PATH_LEN);
            p = AppendCStr(path, p, name, MAX_PATH_LEN);
            AppendCStr(path, p, ".bmp", MAX_PATH_LEN);
            p = AppendCStr(msg, 0,
                           SS_ImgLoadBmp(&s_ImgB, path, bw, bh) ? "saveshot: thumb " : "saveshot: no thumb ",
                           sizeof(msg));
            AppendCStr(msg, p, path, sizeof(msg));
            LogLine(msg);
        }
    }
    else if (s_ImgB.ok && (s_ImgB.w > bw + 2 || s_ImgB.h > bh + 2))
    {
        SS_ImgFree(&s_ImgB);    /* 换排版后框变小了 */
    }

    SS_DrawBox(surf, &s_BoxA, &s_ImgA, L"当前画面", s_CapAx, s_CapAy);
    SS_DrawBox(surf, &s_BoxB, &s_ImgB, L"存档截图", s_CapBx, s_CapBy);

    /* 记下这次面板按哪套 "选中/顶行" 画的: 列表的 WM_PAINT 靠它判断要不要请
     * 面板重画, 不记就会互相请求重画。 */
    s_DrawnCur = (int)SendMessageA(list, SS_LB_GETCURSEL, 0, 0);
    s_DrawnTop = (int)SendMessageA(list, SS_LB_GETTOPINDEX, 0, 0);
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
