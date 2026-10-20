/* EscMenu.cpp - custom ESC-menu page drawn into the game's owner-draw surface.
 * The game paints the dialog into a temporary 16-bit BSurface at 0x622396;
 * draw there after the original content. Display/input only, no simulation. */

#include <windows.h>
#include "GdiText.h"
#include "EscMenu.h"

#define ESC_SECTION          "EscMenu"
#define ESC_REG_ESI          0x0Cu          /* Syringe REGISTERS: ESI */
#define ESC_REG_ESP          0x14u          /* Syringe REGISTERS: ESP */
#define ESC_STACK_HWND       0x10u          /* OwnerDraw::Paint local: HWND */
#define ESC_ADDR_SESSION_MODE 0x00A8B238u   /* SessionClass::GameMode */
#define ESC_GAME_LAN         3
#define ESC_GAME_INTERNET    4

#define ESC_ID_GAMECONTROLS  1313
#define ESC_ID_BOTTOM        1685

#define ESC_TPL_W            533
#define ESC_TPL_H            369
#define ESC_SIDEBAR_W        168

#define ESC_TITLE_SLOT       10
#define ESC_BODY_SLOT        11
#define ESC_FONT_FACE        L"Microsoft YaHei"

#define ESC_TITLE_PX         18
#define ESC_BODY_PX          16
#define ESC_PAD              8
#define ESC_TEXT_INSET       6
#define ESC_TITLE_GAP        6
#define ESC_SEP_GAP          4
#define ESC_ROW_H            24
#define ESC_TEXT_SS          2

static int  s_Init = 0;
static int  s_Enable = 1;
static int  s_Sel = 0;

static HWND s_Host = NULL;

/* ---------------- init ---------------- */

void EscMenu_EnsureInit(void)
{
    if (s_Init) return;
    s_Init = 1;
    BuildPaths();
    GtEnsureLog();
    s_Enable = GetPrivateProfileIntA(ESC_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    if (!g_Calibrated) Calibrate();
    if (!g_ExpReady) GtBuildExpand();
    if (g_Log) LogLine(s_Enable ? "esc: init on" : "esc: init off");
}

static int Esc_IsMP(void)
{
    int gm;
    if (!IsPlausiblePtr((void*)ESC_ADDR_SESSION_MODE)) return 1;
    gm = *(volatile int*)(size_t)ESC_ADDR_SESSION_MODE;
    return (gm == ESC_GAME_LAN || gm == ESC_GAME_INTERNET) ? 1 : 0;
}

/* ---------------- layout ---------------- */

static int Esc_OuterMargin(int W)
{
    int m = W * 6 / ESC_TPL_W;
    return m < 8 ? 8 : m;
}

static int Esc_Layout(HWND hDlg, RECT* left, RECT* right)
{
    RECT rc, x;
    int W, H, l, t, r, b, m, avail, lw;

    if (!GetClientRect(hDlg, &rc)) return 0;
    W = rc.right;
    H = rc.bottom;
    if (W < 200 || H < 150) return 0;

    /* Uniform margin inside the content box. The sidebar is 168px wide
     * (Ares WorkAreaWidth=632 at 800x600); the status bar marks the bottom. */
    m = Esc_OuterMargin(W);
    l = m;
    t = m;
    r = W - ESC_SIDEBAR_W - m;
    b = H * 355 / ESC_TPL_H - m;

    if (GetDlgItem(hDlg, ESC_ID_BOTTOM) &&
        GetWindowRect(GetDlgItem(hDlg, ESC_ID_BOTTOM), &x))
    {
        MapWindowPoints(NULL, hDlg, (LPPOINT)&x, 2);
        if (x.top > H / 2 && x.top < H)
            b = x.top - m;
    }

    if (r <= l || b <= t) return 0;
    avail = r - l - m;
    if (avail < 100) return 0;
    lw = avail * 40 / 100;
    if (lw < 50) lw = 50;

    left->left = l;
    left->top = t;
    left->right = l + lw;
    left->bottom = b;

    right->left = l + lw + m;
    right->top = t;
    right->right = r;
    right->bottom = b;
    return 1;
}

static int Esc_TextHeight(int px, int slot)
{
    if (!GdiEnsure(px * ESC_TEXT_SS, 0, slot, ESC_FONT_FACE)) return px + 2;
    return (g_G->adv + ESC_TEXT_SS - 1) / ESC_TEXT_SS;
}

static int Esc_ContentTop(const RECT* p)
{
    return p->top + ESC_PAD
           + Esc_TextHeight(ESC_TITLE_PX, ESC_TITLE_SLOT)
           + ESC_TITLE_GAP + ESC_SEP_GAP;
}

static int Esc_RowTop(const RECT* p, int index)
{
    return Esc_ContentTop(p) + index * ESC_ROW_H;
}

/* ---------------- 16-bit BSurface drawing ---------------- */

static void Esc_Fill(void* surf, int x, int y, int w, int h,
                     unsigned r, unsigned g, unsigned b, unsigned a)
{
    unsigned char* base;
    int W, H, bpp, pitch, i, j;
    unsigned short col;

    if (!surf || w <= 0 || h <= 0 || a == 0) return;
    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    bpp = GtSurfGet(surf, GtVT_GET_BPP);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || bpp != 2 || pitch < W * 2) return;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;

    base = GtLock(surf);
    if (!base) return;

    if (a >= 255)
    {
        col = Blend16(0, r, g, b, 255);
        for (j = 0; j < h; j++)
        {
            unsigned short* row = (unsigned short*)(base + (size_t)(y + j) * pitch + (size_t)x * 2);
            for (i = 0; i < w; i++) row[i] = col;
        }
    }
    else
    {
        for (j = 0; j < h; j++)
        {
            unsigned short* row = (unsigned short*)(base + (size_t)(y + j) * pitch + (size_t)x * 2);
            for (i = 0; i < w; i++) row[i] = Blend16(row[i], r, g, b, a);
        }
    }
    GtUnlock(surf);
}

static void Esc_Outline(void* surf, int x, int y, int w, int h,
                        unsigned r, unsigned g, unsigned b, unsigned a)
{
    if (w < 8 || h < 8) return;
    Esc_Fill(surf, x, y, w, 2, r, g, b, a);
    Esc_Fill(surf, x, y + h - 2, w, 2, r, g, b, a);
    Esc_Fill(surf, x, y + 2, 2, h - 4, r, g, b, a);
    Esc_Fill(surf, x + w - 2, y + 2, 2, h - 4, r, g, b, a);
}

/* Supersampled text: render at 2x, box-filter the red channel as coverage.
 * ANTIALIASED_QUALITY produces equal RGB, and BlitCoverage also reads red. */
static void Esc_Text(void* surf, const wchar_t* s, int x, int y, int maxW,
                     int px, int slot, unsigned r, unsigned g, unsigned b)
{
    unsigned char* base;
    int len = 0, w2, adv2, w, h, W, H, pitch, X, Y, xx, yy;

    if (!s || !*s) return;
    while (s[len] && len < 256) len++;
    if (!GdiEnsure(px * ESC_TEXT_SS, 0, slot, ESC_FONT_FACE)) return;
    w2 = RenderLine(s, len);
    if (w2 <= 0 || !g_G || !g_G->bits) return;

    adv2 = g_G->adv;
    w = (w2 + ESC_TEXT_SS - 1) / ESC_TEXT_SS;
    h = (adv2 + ESC_TEXT_SS - 1) / ESC_TEXT_SS;
    if (w > DIB_W / ESC_TEXT_SS) w = DIB_W / ESC_TEXT_SS;
    if (h > DIB_H / ESC_TEXT_SS) h = DIB_H / ESC_TEXT_SS;
    if (maxW > 0 && w > maxW) w = maxW;
    if (w <= 0 || h <= 0) return;

    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || pitch < W * 2) return;
    if (x + w > W) x = W - w;
    if (y + h > H) h = H - y;
    if (x < 0 || y < 0 || h <= 0) return;

    base = GtLock(surf);
    if (!base) return;

    for (Y = 0; Y < h; Y++)
    {
        unsigned short* dstRow = (unsigned short*)(base + (size_t)(y + Y) * pitch) + x;
        for (X = 0; X < w; X++)
        {
            unsigned sum = 0, a;
            for (yy = 0; yy < ESC_TEXT_SS; yy++)
            {
                const unsigned char* src = g_G->bits
                    + (size_t)(Y * ESC_TEXT_SS + yy) * g_G->pitch
                    + (size_t)(X * ESC_TEXT_SS) * 4;
                for (xx = 0; xx < ESC_TEXT_SS; xx++)
                    sum += src[xx * 4 + 2];
            }
            a = sum / (ESC_TEXT_SS * ESC_TEXT_SS);
            if (a) dstRow[X] = Blend16(dstRow[X], r, g, b, a);
        }
    }
    GtUnlock(surf);
}

typedef struct {
    const wchar_t* text;
    int selected;
} EscRow;

static void Esc_Panel(void* surf, RECT* p, const wchar_t* title,
                      const EscRow* rows, int n)
{
    int w, h, titleH, bodyH, sepY, rowY, rowL, rowW, i;

    w = p->right - p->left;
    h = p->bottom - p->top;
    if (w < 40 || h < 40) return;

    titleH = Esc_TextHeight(ESC_TITLE_PX, ESC_TITLE_SLOT);
    bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    if (bodyH > ESC_ROW_H) bodyH = ESC_ROW_H;

    Esc_Fill(surf, p->left, p->top, w, h, 16, 16, 16, 210);
    Esc_Outline(surf, p->left, p->top, w, h, 0x7B, 0x79, 0x7B, 255);

    Esc_Text(surf, title, p->left + ESC_PAD + ESC_TEXT_INSET, p->top + ESC_PAD,
             w - (ESC_PAD + ESC_TEXT_INSET) * 2, ESC_TITLE_PX, ESC_TITLE_SLOT,
             235, 235, 235);

    sepY = p->top + ESC_PAD + titleH + ESC_TITLE_GAP;
    Esc_Fill(surf, p->left + ESC_PAD, sepY, w - ESC_PAD * 2, 1,
             0x7B, 0x79, 0x7B, 160);

    rowL = p->left + ESC_PAD;
    rowW = w - ESC_PAD * 2;
    for (i = 0; i < n; i++)
    {
        rowY = Esc_RowTop(p, i);
        if (rowY + ESC_ROW_H > p->bottom - ESC_PAD) break;

        if (rows[i].selected)
            Esc_Fill(surf, rowL, rowY, rowW, ESC_ROW_H, 0x7B, 0x20, 0x7B, 255);
        else
            Esc_Fill(surf, rowL, rowY, rowW, ESC_ROW_H, 32, 32, 32, 120);

        Esc_Text(surf, rows[i].text, rowL + ESC_TEXT_INSET,
                 rowY + (ESC_ROW_H - bodyH) / 2,
                 rowW - ESC_TEXT_INSET * 2, ESC_BODY_PX, ESC_BODY_SLOT,
                 230, 230, 230);

        Esc_Fill(surf, rowL, rowY + ESC_ROW_H - 1, rowW, 1,
                 0x7B, 0x79, 0x7B, 80);
    }
}

static void Esc_DrawPanels(void* surf, HWND hDlg)
{
    EscRow leftRows[2] = {
        { L"\u5360\u4F4D\u9879 1", s_Sel == 0 },
        { L"\u5360\u4F4D\u9879 2", s_Sel == 1 },
    };
    EscRow rightRows[2] = {
        { L"\u5360\u4F4D\u8BBE\u7F6E A", 0 },
        { L"\u5360\u4F4D\u8BBE\u7F6E B", 0 },
    };
    RECT a, b;

    if (!g_Calibrated) Calibrate();
    if (!g_ExpReady) GtBuildExpand();
    if (!Esc_Layout(hDlg, &a, &b)) return;

    Esc_Panel(surf, &a, L"\u529F\u80FD\u83DC\u5355", leftRows, 2);
    Esc_Panel(surf, &b, L"\u8BE6\u7EC6\u8BBE\u7F6E", rightRows, 2);
}

/* ---------------- input ---------------- */

static int Esc_HitLeft(HWND hDlg, int mx, int my, int* index)
{
    RECT a, b;
    int i, rowY, rowL, rowW;
    if (!Esc_Layout(hDlg, &a, &b)) return 0;

    rowL = a.left + ESC_PAD;
    rowW = (a.right - a.left) - ESC_PAD * 2;
    for (i = 0; i < 2; i++)
    {
        rowY = Esc_RowTop(&a, i);
        if (rowY + ESC_ROW_H > a.bottom - ESC_PAD) break;
        if (mx >= rowL && mx < rowL + rowW &&
            my >= rowY && my < rowY + ESC_ROW_H)
        {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static void Esc_Select(HWND hDlg, int index)
{
    char buf[64];
    unsigned p;
    if (index < 0 || index > 1 || index == s_Sel) return;
    s_Sel = index;
    if (g_Log)
    {
        p = AppendCStr(buf, 0, "esc: select ", sizeof(buf));
        UtoA((unsigned)(index + 1), buf + p);
        LogLine(buf);
    }
    InvalidateRect(hDlg, NULL, FALSE);
    UpdateWindow(hDlg);
}

static LRESULT Esc_CallOld(HWND h, WNDPROC old, UINT msg, WPARAM wp, LPARAM lp)
{
    if (IsWindowUnicode(h))
        return old ? CallWindowProcW(old, h, msg, wp, lp)
                   : DefWindowProcW(h, msg, wp, lp);
    return old ? CallWindowProcA(old, h, msg, wp, lp)
               : DefWindowProcA(h, msg, wp, lp);
}

static LRESULT CALLBACK Esc_DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropW(h, L"MOE_EscOldProc");
    LRESULT r;

    if (msg == WM_LBUTTONDOWN && h == s_Host)
    {
        int mx = (int)(short)LOWORD(lp);
        int my = (int)(short)HIWORD(lp);
        int index = -1;
        if (Esc_HitLeft(h, mx, my, &index))
        {
            Esc_Select(h, index);
            return 0;   /* consume only the list click */
        }
    }

    r = Esc_CallOld(h, old, msg, wp, lp);

    if (msg == WM_NCDESTROY)
    {
        RemovePropW(h, L"MOE_EscOldProc");
        if (h == s_Host) s_Host = NULL;
    }
    return r;
}

static void Esc_Subclass(HWND hDlg)
{
    WNDPROC old;
    if (GetPropW(hDlg, L"MOE_EscOldProc")) return;
    old = (WNDPROC)SetWindowLongPtrW(hDlg, GWLP_WNDPROC, (LONG_PTR)Esc_DlgProc);
    if (!old || !SetPropW(hDlg, L"MOE_EscOldProc", (HANDLE)old))
    {
        if (old) SetWindowLongPtrW(hDlg, GWLP_WNDPROC, (LONG_PTR)old);
        if (g_Log) LogLine("esc: subclass fail");
        return;
    }
    if (g_Log) LogLine("esc: subclass ok");
}

/* ---------------- hooks ---------------- */

static void Esc_OnShow(HWND hDlg)
{
    EscMenu_EnsureInit();
    if (!s_Enable) return;
    if (Esc_IsMP()) return;
    /* Only the ESC home page has the Game Controls button; 3003 sub-pages do not. */
    if (!GetDlgItem(hDlg, ESC_ID_GAMECONTROLS)) return;

    if (s_Host != hDlg)
    {
        s_Host = hDlg;
        s_Sel = 0;
        if (g_Log) LogLine("esc: host set");
    }
    Esc_Subclass(hDlg);
}

/* 0x4F1140: dialog created (ESI=HWND), before ShowWindow.
 * Original: lea eax,[esp+4] / push eax. */
extern "C" __declspec(dllexport) DWORD __cdecl EscMenu_OnShow(void* regs)
{
    HWND hDlg;
    if (!regs) return 0;
    hDlg = (HWND)(size_t)*(unsigned*)((char*)regs + ESC_REG_ESI);
    if (hDlg && IsWindow(hDlg)) Esc_OnShow(hDlg);
    return 0;
}

/* 0x622396: after dialog content is drawn into the temporary BSurface (ESI),
 * before it is blitted. Original: mov ecx,[0x887310] (6 bytes). */
extern "C" __declspec(dllexport) DWORD __cdecl EscMenu_OnPaintSurface(void* regs)
{
    unsigned esp, hwnd, surf;
    int W, H;

    if (!regs || !s_Enable || !s_Host) return 0;
    if (!IsWindow(s_Host)) { s_Host = NULL; return 0; }
    if (!IsWindowVisible(s_Host)) return 0;
    if (!GetDlgItem(s_Host, ESC_ID_GAMECONTROLS)) return 0;

    esp = *(unsigned*)((char*)regs + ESC_REG_ESP);
    hwnd = esp ? *(unsigned*)(size_t)(esp + ESC_STACK_HWND) : 0;
    if (hwnd != (unsigned)(size_t)s_Host) return 0;

    surf = *(unsigned*)((char*)regs + ESC_REG_ESI);
    if (!surf || !IsPlausiblePtr((void*)(size_t)surf)) return 0;

    W = GtSurfGet((void*)(size_t)surf, GtVT_GET_WIDTH);
    H = GtSurfGet((void*)(size_t)surf, GtVT_GET_HEIGHT);
    if (W < 200 || H < 150) return 0;

    Esc_DrawPanels((void*)(size_t)surf, s_Host);
    return 0;
}
