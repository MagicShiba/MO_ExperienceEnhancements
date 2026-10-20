/* EscMenu.cpp - custom ESC-menu page drawn into the game's owner-draw surface.
 * The game paints the dialog into a temporary 16-bit BSurface at 0x622396;
 * draw there after the original content. Display/input only, no simulation. */

#include <windows.h>
#include "GdiText.h"
#include "Lang.h"
#include "ScaleBar.h"
#include "PauseView.h"
#include "GridView.h"
#include "SWSBar.h"
#include "MsgText.h"
#include "OrderUtil.h"
#include "EscMenu.h"
#include "SidebarSkin.h"

#define ESC_SECTION          "EscMenu"
#define ESC_REG_ESI          0x0Cu          /* Syringe REGISTERS: ESI */
#define ESC_REG_ESP          0x14u          /* Syringe REGISTERS: ESP */
#define ESC_STACK_HWND       0x10u          /* OwnerDraw::Paint local: HWND */
#define ESC_PROP_OLD_PROC    L"MOE_EscOldProc"

#define ESC_ID_GAMECONTROLS  1313
#define ESC_ID_BOTTOM        1685

#define ESC_TPL_W            533
#define ESC_TPL_H            369
#define ESC_SIDEBAR_W        168

#define ESC_LEFT_W           300
#define ESC_RIGHT_W          600
#define ESC_PANEL_GAP        10
#define ESC_VMARGIN          24

#define ESC_TITLE_SLOT       10
#define ESC_BODY_SLOT        11
#define ESC_FONT_FACE        L"Microsoft YaHei"

#define ESC_TITLE_PX         18
#define ESC_BODY_PX          16
#define ESC_PAD              8
#define ESC_TEXT_INSET       6
#define ESC_TITLE_GAP        6
#define ESC_SEP_GAP          4
#define ESC_ROW_H            26
#define ESC_GROUP_H          28
#define ESC_BLOCK_GAP        12
#define ESC_WARN_H           26
#define ESC_WARN_GAP         8
#define ESC_TEXT_SS          2
#define ESC_LABEL_CAP        96
#define ESC_LEFT_ROWS        2
#define ESC_GUI_ITEMS        7
#define ESC_GUI_DISPLAY      11
#define ESC_SKIN_N           5
#define ESC_LANG_N           3
#define ESC_LANG_GAP         14

#define ESC_SW_W             44
#define ESC_SW_H             20
#define ESC_STATE_W          64
#define ESC_CTL_RIGHT        12
#define ESC_STEP_W           96
#define ESC_STEP_BTN_W       24
#define ESC_STEP_BTN_H       20
#define ESC_SKIN_GAP         10
#define ESC_ROWS_MIN         1
#define ESC_ROWS_MAX         40
#define ESC_AA_SS            8
#define ESC_AA_SCALE         (ESC_AA_SS * 2)
#define ESC_AA_COV           (ESC_AA_SS * ESC_AA_SS)

#define ESC_MISSING_ZH        L"!文本缺失!"
#define ESC_MISSING_EN        L"!Text missing!"
#define ESC_MISSING_NOTICE_ZH L"!!文本缺失，请检查多语言文件!!"
#define ESC_MISSING_NOTICE_EN L"!!Text missing - check the language file!!"

static int  s_Init = 0;
static int  s_Enable = 1;
static int  s_Sel = 0;
static int  s_GuiMissing = 0;

static const wchar_t* s_Missing = ESC_MISSING_EN;
static const wchar_t* s_MissingNotice = ESC_MISSING_NOTICE_EN;

static HWND s_Host = NULL;
static wchar_t s_LeftTitle[ESC_LABEL_CAP];
static wchar_t s_LeftItem[ESC_LEFT_ROWS][ESC_LABEL_CAP];
static wchar_t s_RightTitle[ESC_LABEL_CAP];
static wchar_t s_RightItem[2][ESC_LABEL_CAP];
static wchar_t s_GuiTitle[ESC_LABEL_CAP];
static wchar_t s_GuiGroup[4][ESC_LABEL_CAP];
static wchar_t s_GuiLabel[ESC_GUI_ITEMS][ESC_LABEL_CAP];
static wchar_t s_GuiOn[16];
static wchar_t s_GuiOff[16];
static wchar_t s_GuiFaction[32];
static wchar_t s_GuiFill[32];
static wchar_t s_GuiSkinName[ESC_SKIN_N][32];

static const wchar_t* const s_LangLabel[ESC_LANG_N] = { L"ZH", L"EN", L"AUTO" };
static const int s_LangMode[ESC_LANG_N] = {
    LANG_MODE_ZH, LANG_MODE_EN, LANG_MODE_AUTO
};

/* ---------------- init ---------------- */

static int Esc_LoadText(const char* key, wchar_t* out, int cap)
{
    return Lang_GetW(ESC_SECTION, key, s_Missing, out, cap);
}

static void Esc_GuiText(const char* key, wchar_t* out, int cap)
{
    if (!Esc_LoadText(key, out, cap)) s_GuiMissing = 1;
}

static void Esc_LoadAllText(void)
{
    if (Lang_IsChinese())
    {
        s_Missing = ESC_MISSING_ZH;
        s_MissingNotice = ESC_MISSING_NOTICE_ZH;
    }
    else
    {
        s_Missing = ESC_MISSING_EN;
        s_MissingNotice = ESC_MISSING_NOTICE_EN;
    }

    Esc_LoadText("LeftTitle", s_LeftTitle, ESC_LABEL_CAP);
    Esc_LoadText("LeftItem1", s_LeftItem[0], ESC_LABEL_CAP);
    Esc_LoadText("LeftItem2", s_LeftItem[1], ESC_LABEL_CAP);
    Esc_LoadText("RightTitle", s_RightTitle, ESC_LABEL_CAP);
    Esc_LoadText("RightItem1", s_RightItem[0], ESC_LABEL_CAP);
    Esc_LoadText("RightItem2", s_RightItem[1], ESC_LABEL_CAP);

    s_GuiMissing = 0;
    Esc_GuiText("GuiTitle", s_GuiTitle, ESC_LABEL_CAP);
    Esc_GuiText("GuiGroupGeneral", s_GuiGroup[0], ESC_LABEL_CAP);
    Esc_GuiText("GuiGroupBottom", s_GuiGroup[1], ESC_LABEL_CAP);
    Esc_GuiText("GuiScale", s_GuiLabel[0], ESC_LABEL_CAP);
    Esc_GuiText("GuiPause", s_GuiLabel[1], ESC_LABEL_CAP);
    Esc_GuiText("GuiGrid", s_GuiLabel[2], ESC_LABEL_CAP);
    Esc_GuiText("GuiGroupSws", s_GuiGroup[2], ESC_LABEL_CAP);
    Esc_GuiText("GuiGroupMisc", s_GuiGroup[3], ESC_LABEL_CAP);
    Esc_GuiText("GuiSws", s_GuiLabel[3], ESC_LABEL_CAP);
    Esc_GuiText("GuiSort", s_GuiLabel[4], ESC_LABEL_CAP);
    Esc_GuiText("GuiRows", s_GuiLabel[5], ESC_LABEL_CAP);
    Esc_GuiText("GuiSkin", s_GuiLabel[6], ESC_LABEL_CAP);
    Esc_GuiText("GuiOn", s_GuiOn, 16);
    Esc_GuiText("GuiOff", s_GuiOff, 16);
    Esc_GuiText("GuiSortFaction", s_GuiFaction, 32);
    Esc_GuiText("GuiSortFill", s_GuiFill, 32);
    Esc_GuiText("GuiSkinDefault", s_GuiSkinName[0], 32);
    Esc_GuiText("GuiSkin1", s_GuiSkinName[1], 32);
    Esc_GuiText("GuiSkin2", s_GuiSkinName[2], 32);
    Esc_GuiText("GuiSkin3", s_GuiSkinName[3], 32);
    Esc_GuiText("GuiSkin4", s_GuiSkinName[4], 32);
}

void EscMenu_EnsureInit(void)
{
    if (s_Init) return;
    s_Init = 1;
    BuildPaths();
    s_Enable = GetPrivateProfileIntA(ESC_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    Lang_EnsureInit();
    Esc_LoadAllText();
    if (!g_Calibrated) Calibrate();
    LogLine(s_Enable ? "esc: init on" : "esc: init off");
}

void EscMenu_ReloadLang(void)
{
    Esc_LoadAllText();
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
    HWND bottom;
    int W, H, l, t, r, b, m, avail, lw, rw;

    if (!GetClientRect(hDlg, &rc)) return 0;
    W = rc.right;
    H = rc.bottom;
    if (W < 200 || H < 150) return 0;

    /* Fixed panels: 300 + 10 + 600. At small resolutions the right panel
     * shrinks first so the layout never runs into the sidebar. */
    m = Esc_OuterMargin(W);
    l = m;
    t = m + ESC_VMARGIN;
    r = W - ESC_SIDEBAR_W - m;
    b = H * 355 / ESC_TPL_H - m;

    {
        bottom = GetDlgItem(hDlg, ESC_ID_BOTTOM);
        if (bottom && GetWindowRect(bottom, &x))
        {
            MapWindowPoints(NULL, hDlg, (LPPOINT)&x, 2);
            if (x.top > H / 2 && x.top < H)
                b = x.top - m;
        }
    }
    b -= ESC_VMARGIN;

    if (r <= l || b <= t) return 0;
    avail = r - l;
    if (avail < 200) return 0;

    lw = ESC_LEFT_W;
    rw = ESC_RIGHT_W;
    if (lw + ESC_PANEL_GAP + rw > avail)
    {
        rw = avail - lw - ESC_PANEL_GAP;
        if (rw < 240)
        {
            lw = avail * 34 / 100;
            if (lw < 220) lw = 220;
            rw = avail - lw - ESC_PANEL_GAP;
        }
    }
    if (rw < 120) return 0;

    if (lw + ESC_PANEL_GAP + rw < avail)
        l += (avail - (lw + ESC_PANEL_GAP + rw)) / 2;

    left->left = l;
    left->top = t;
    left->right = l + lw;
    left->bottom = b;

    right->left = left->right + ESC_PANEL_GAP;
    right->top = t;
    right->right = right->left + rw;
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

static void Esc_IntToW(wchar_t* dst, unsigned value, int cap)
{
    char b[16];
    unsigned n = UtoA(value, b), i;
    if ((int)n > cap - 1) n = (unsigned)(cap - 1);
    for (i = 0; i < n; i++) dst[i] = (wchar_t)b[i];
    dst[n] = 0;
}

enum {
    EG_GROUP = 0,
    EG_SWITCH,
    EG_SORT,
    EG_ROWS,
    EG_SKIN
};

typedef struct {
    unsigned char kind;
    unsigned char item;
} EscGuiRowInfo;

static const EscGuiRowInfo s_GuiLayout[ESC_GUI_DISPLAY] = {
    { EG_GROUP,  0 },
    { EG_GROUP,  1 },
    { EG_SWITCH, 0 },
    { EG_SWITCH, 1 },
    { EG_SWITCH, 2 },
    { EG_GROUP,  2 },
    { EG_SWITCH, 3 },
    { EG_SORT,   4 },
    { EG_ROWS,   5 },
    { EG_GROUP,  3 },
    { EG_SKIN,   6 }
};

static int Esc_GuiContentTop(const RECT* p)
{
    int y = Esc_ContentTop(p);
    if (s_GuiMissing) y += ESC_WARN_H + ESC_WARN_GAP;
    return y;
}

static int Esc_GuiRowH(int index)
{
    return (s_GuiLayout[index].kind == EG_GROUP) ? ESC_GROUP_H : ESC_ROW_H;
}

static int Esc_GuiRowTop(const RECT* p, int index)
{
    int y = Esc_GuiContentTop(p);
    int i;
    for (i = 0; i < index; i++)
    {
        if (s_GuiLayout[i].kind == EG_GROUP && s_GuiLayout[i].item != 0)
            y += ESC_BLOCK_GAP;
        y += Esc_GuiRowH(i);
    }
    if (s_GuiLayout[index].kind == EG_GROUP && s_GuiLayout[index].item != 0)
        y += ESC_BLOCK_GAP;
    return y;
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
static int Esc_Measure(const wchar_t* s, int px, int slot, int* adv)
{
    int w2;
    if (!s || !*s) return 0;
    if (!GdiEnsure(px * ESC_TEXT_SS, 0, slot, ESC_FONT_FACE)) return 0;
    w2 = RenderLine(s, lstrlenW(s));
    if (w2 <= 0) return 0;
    if (adv) *adv = g_G->adv;
    return (w2 + ESC_TEXT_SS - 1) / ESC_TEXT_SS;
}

static void Esc_Text(void* surf, const wchar_t* s, int x, int y, int maxW,
                     int px, int slot, unsigned r, unsigned g, unsigned b)
{
    unsigned char* base;
    int adv2 = 0, w, h, W, H, pitch, X, Y, xx, yy;

    w = Esc_Measure(s, px, slot, &adv2);
    if (w <= 0) return;

    h = (adv2 + ESC_TEXT_SS - 1) / ESC_TEXT_SS;
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

static int Esc_TextWidth(const wchar_t* s, int px, int slot)
{
    return Esc_Measure(s, px, slot, NULL);
}

static void Esc_RowBg(void* surf, int rowL, int rowY, int rowW)
{
    Esc_Fill(surf, rowL, rowY, rowW, ESC_ROW_H, 32, 32, 32, 120);
}

static void Esc_RowLabel(void* surf, int rowL, int rowY, int right, int bodyH,
                         const wchar_t* label)
{
    int labelW = right - 8 - (rowL + ESC_TEXT_INSET);
    if (labelW > 0)
        Esc_Text(surf, label, rowL + ESC_TEXT_INSET,
                 rowY + (ESC_ROW_H - bodyH) / 2, labelW,
                 ESC_BODY_PX, ESC_BODY_SLOT, 232, 232, 232);
}

static void Esc_Capsule(void* surf, int x, int y, int w, int h,
                        unsigned r, unsigned g, unsigned b, unsigned a)
{
    unsigned char* base;
    int W, H, pitch, rad, px, py, sx, sy;
    int x0, x1, y0, y1, radS, cyS;

    if (!surf || w <= 0 || h <= 0 || a == 0) return;
    rad = h / 2;

    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || pitch < W * 2) return;

    x0 = x; x1 = x + w;
    y0 = y; y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x1 <= x0 || y1 <= y0) return;

    base = GtLock(surf);
    if (!base) return;

    radS = rad * ESC_AA_SCALE;
    cyS = (h * ESC_AA_SCALE) / 2;
    for (py = y0; py < y1; py++)
    {
        unsigned short* row = (unsigned short*)(base + (size_t)py * pitch) + x0;
        for (px = x0; px < x1; px++)
        {
            int cov = 0;
            for (sy = 0; sy < ESC_AA_SS; sy++)
            {
                int yy = (py - y) * ESC_AA_SCALE + sy * 2 + 1;
                int dy = yy - cyS;
                for (sx = 0; sx < ESC_AA_SS; sx++)
                {
                    int xx = (px - x) * ESC_AA_SCALE + sx * 2 + 1;
                    int dx;
                    if (xx < radS)
                        dx = xx - radS;
                    else if (xx > w * ESC_AA_SCALE - radS)
                        dx = xx - (w * ESC_AA_SCALE - radS);
                    else
                    {
                        cov++;
                        continue;
                    }
                    if (dx * dx + dy * dy <= radS * radS) cov++;
                }
            }
            if (cov)
            {
                unsigned aa = (a * (unsigned)cov + ESC_AA_COV / 2) / ESC_AA_COV;
                if (aa) row[px - x0] = Blend16(row[px - x0], r, g, b, aa);
            }
        }
    }
    GtUnlock(surf);
}

static void Esc_Disc(void* surf, int cx, int cy, int rad,
                     unsigned r, unsigned g, unsigned b, unsigned a)
{
    unsigned char* base;
    int W, H, pitch, px, py, sx, sy;
    int x0, x1, y0, y1, radS, cxS, cyS;

    if (!surf || rad <= 0 || a == 0) return;

    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || pitch < W * 2) return;

    x0 = cx - rad; x1 = cx + rad;
    y0 = cy - rad; y1 = cy + rad;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x1 <= x0 || y1 <= y0) return;

    base = GtLock(surf);
    if (!base) return;

    radS = rad * ESC_AA_SCALE;
    cxS = cx * ESC_AA_SCALE;
    cyS = cy * ESC_AA_SCALE;
    for (py = y0; py < y1; py++)
    {
        unsigned short* row = (unsigned short*)(base + (size_t)py * pitch) + x0;
        for (px = x0; px < x1; px++)
        {
            int cov = 0;
            for (sy = 0; sy < ESC_AA_SS; sy++)
            {
                int dy = py * ESC_AA_SCALE + sy * 2 + 1 - cyS;
                for (sx = 0; sx < ESC_AA_SS; sx++)
                {
                    int dx = px * ESC_AA_SCALE + sx * 2 + 1 - cxS;
                    if (dx * dx + dy * dy <= radS * radS) cov++;
                }
            }
            if (cov)
            {
                unsigned aa = (a * (unsigned)cov + ESC_AA_COV / 2) / ESC_AA_COV;
                if (aa) row[px - x0] = Blend16(row[px - x0], r, g, b, aa);
            }
        }
    }
    GtUnlock(surf);
}

static void Esc_Switch(void* surf, int x, int y, int on)
{
    unsigned shade = on ? 245u : 205u;
    int cx, knob;
    Esc_Capsule(surf, x, y, ESC_SW_W, ESC_SW_H, 0x7B, 0x79, 0x7B, 255);
    Esc_Capsule(surf, x + 1, y + 1, ESC_SW_W - 2, ESC_SW_H - 2,
                on ? 123u : 70u, on ? 32u : 70u, on ? 123u : 70u, 255);

    knob = ESC_SW_H / 2 - 3;
    cx = on ? x + ESC_SW_W - ESC_SW_H / 2 : x + ESC_SW_H / 2;
    Esc_Disc(surf, cx, y + ESC_SW_H / 2, knob, shade, shade, shade, 255);
}

static int Esc_PanelFrame(void* surf, RECT* p, const wchar_t* title)
{
    int w, h, titleH, sepY;

    w = p->right - p->left;
    h = p->bottom - p->top;
    if (w < 40 || h < 40) return 0;

    titleH = Esc_TextHeight(ESC_TITLE_PX, ESC_TITLE_SLOT);

    Esc_Fill(surf, p->left, p->top, w, h, 16, 16, 16, 210);
    Esc_Outline(surf, p->left, p->top, w, h, 0x7B, 0x79, 0x7B, 255);

    Esc_Text(surf, title, p->left + ESC_PAD + ESC_TEXT_INSET, p->top + ESC_PAD,
             w - (ESC_PAD + ESC_TEXT_INSET) * 2, ESC_TITLE_PX, ESC_TITLE_SLOT,
             235, 235, 235);

    sepY = p->top + ESC_PAD + titleH + ESC_TITLE_GAP;
    Esc_Fill(surf, p->left + ESC_PAD, sepY, w - ESC_PAD * 2, 1,
             0x7B, 0x79, 0x7B, 160);
    return 1;
}

typedef struct {
    const wchar_t* text;
    int selected;
} EscRow;

static void Esc_Panel(void* surf, RECT* p, const wchar_t* title,
                      const EscRow* rows, int n)
{
    int w, bodyH, rowY, rowL, rowW, i;

    if (!Esc_PanelFrame(surf, p, title)) return;

    w = p->right - p->left;
    bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    if (bodyH > ESC_ROW_H) bodyH = ESC_ROW_H;

    rowL = p->left + ESC_PAD;
    rowW = w - ESC_PAD * 2;
    rowY = Esc_ContentTop(p);
    for (i = 0; i < n; i++, rowY += ESC_ROW_H)
    {
        if (rowY + ESC_ROW_H > p->bottom - ESC_PAD) break;

        if (rows[i].selected)
            Esc_Fill(surf, rowL, rowY, rowW, ESC_ROW_H, 0x7B, 0x20, 0x7B, 255);
        else
            Esc_RowBg(surf, rowL, rowY, rowW);

        Esc_Text(surf, rows[i].text, rowL + ESC_TEXT_INSET,
                 rowY + (ESC_ROW_H - bodyH) / 2,
                 rowW - ESC_TEXT_INSET * 2, ESC_BODY_PX, ESC_BODY_SLOT,
                 230, 230, 230);

        Esc_Fill(surf, rowL, rowY + ESC_ROW_H - 1, rowW, 1,
                 0x7B, 0x79, 0x7B, 80);
    }
}

static int Esc_RowCtlX(int rowRight)
{
    return rowRight - ESC_CTL_RIGHT - ESC_STEP_W;
}

static void Esc_SkinSegLayout(int rowL, int rowW, int* xs, int* ws)
{
    int i, total = 0, x;

    for (i = 0; i < ESC_SKIN_N; i++)
    {
        ws[i] = Esc_TextWidth(s_GuiSkinName[i], ESC_BODY_PX, ESC_BODY_SLOT);
        total += ws[i];
        if (i) total += ESC_SKIN_GAP;
    }
    x = rowL + rowW - ESC_CTL_RIGHT - total;
    for (i = 0; i < ESC_SKIN_N; i++)
    {
        xs[i] = x;
        x += ws[i] + ESC_SKIN_GAP;
    }
}

static int Esc_SkinSegmentAt(int rowL, int rowW, int mx)
{
    int xs[ESC_SKIN_N], ws[ESC_SKIN_N], i;

    Esc_SkinSegLayout(rowL, rowW, xs, ws);
    for (i = 0; i < ESC_SKIN_N; i++)
        if (mx >= xs[i] && mx < xs[i] + ws[i]) return i;
    return -1;
}

static int Esc_CtlX(const RECT* p)
{
    return Esc_RowCtlX(p->right - ESC_PAD);
}

static void Esc_StepButton(void* surf, int x, int y, const wchar_t* glyph,
                           int enabled)
{
    unsigned v = enabled ? 68u : 42u;
    unsigned fg = enabled ? 238u : 105u;
    int bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    int tw = Esc_TextWidth(glyph, ESC_BODY_PX, ESC_BODY_SLOT);
    if (bodyH > ESC_ROW_H) bodyH = ESC_ROW_H;

    Esc_Fill(surf, x, y, ESC_STEP_BTN_W, ESC_STEP_BTN_H, v, v, v, 255);
    Esc_Fill(surf, x, y, ESC_STEP_BTN_W, 1, 0x7B, 0x79, 0x7B, 180);
    Esc_Fill(surf, x, y + ESC_STEP_BTN_H - 1, ESC_STEP_BTN_W, 1,
             0x7B, 0x79, 0x7B, 180);
    Esc_Text(surf, glyph, x + (ESC_STEP_BTN_W - tw) / 2,
             y + (ESC_STEP_BTN_H - bodyH) / 2,
             ESC_STEP_BTN_W, ESC_BODY_PX, ESC_BODY_SLOT, fg, fg, fg);
}

static void Esc_DrawSwitchRow(void* surf, int rowL, int rowY, int rowW,
                              int bodyH, const wchar_t* label, int on)
{
    int switchX, stateX, tw;
    const wchar_t* state = on ? s_GuiOn : s_GuiOff;

    switchX = rowL + rowW - ESC_CTL_RIGHT - ESC_SW_W;
    stateX = switchX - 8 - ESC_STATE_W;
    Esc_RowLabel(surf, rowL, rowY, stateX, bodyH, label);

    tw = Esc_TextWidth(state, ESC_BODY_PX, ESC_BODY_SLOT);
    if (tw > ESC_STATE_W) tw = ESC_STATE_W;
    Esc_Text(surf, state, stateX + ESC_STATE_W - tw,
             rowY + (ESC_ROW_H - bodyH) / 2, tw, ESC_BODY_PX, ESC_BODY_SLOT,
             on ? 222 : 150, on ? 170 : 150, on ? 222 : 150);
    Esc_Switch(surf, switchX, rowY + (ESC_ROW_H - ESC_SW_H) / 2, on);
}

static void Esc_DrawSortRow(void* surf, int rowL, int rowY, int rowW,
                            int bodyH, const wchar_t* label)
{
    int facW, fillW, total, x, fx, active;
    int fill = SWS_FillMode();

    facW = Esc_TextWidth(s_GuiFaction, ESC_BODY_PX, ESC_BODY_SLOT);
    fillW = Esc_TextWidth(s_GuiFill, ESC_BODY_PX, ESC_BODY_SLOT);
    total = facW + 14 + fillW;
    x = rowL + rowW - ESC_CTL_RIGHT - total;
    fx = x + facW + 14;
    Esc_RowLabel(surf, rowL, rowY, x, bodyH, label);

    Esc_Text(surf, s_GuiFaction, x, rowY + (ESC_ROW_H - bodyH) / 2,
             facW + 2, ESC_BODY_PX, ESC_BODY_SLOT,
             fill ? 145 : 245, fill ? 145 : 225, fill ? 145 : 170);
    Esc_Text(surf, s_GuiFill, fx, rowY + (ESC_ROW_H - bodyH) / 2,
             fillW + 2, ESC_BODY_PX, ESC_BODY_SLOT,
             fill ? 245 : 145, fill ? 225 : 145, fill ? 170 : 145);

    active = fill ? fx : x;
    Esc_Fill(surf, active, rowY + ESC_ROW_H - 6,
             fill ? fillW : facW, 2, 208, 160, 64, 255);
}

static int Esc_LangLayout(const RECT* p, int* xs, int* ws)
{
    int i, total = 0, x;
    for (i = 0; i < ESC_LANG_N; i++)
    {
        ws[i] = Esc_TextWidth(s_LangLabel[i], ESC_BODY_PX, ESC_BODY_SLOT);
        total += ws[i];
        if (i) total += ESC_LANG_GAP;
    }
    x = p->right - ESC_PAD - ESC_TEXT_INSET - total;
    for (i = 0; i < ESC_LANG_N; i++)
    {
        xs[i] = x;
        x += ws[i] + ESC_LANG_GAP;
    }
    return total;
}

static int Esc_LangHitX(const RECT* p, int mx)
{
    int xs[ESC_LANG_N], ws[ESC_LANG_N], i;
    Esc_LangLayout(p, xs, ws);
    for (i = 0; i < ESC_LANG_N; i++)
        if (mx >= xs[i] - 4 && mx < xs[i] + ws[i] + 4)
            return i;
    return -1;
}

static int Esc_LangHit(const RECT* p, int mx, int my)
{
    int titleH = Esc_TextHeight(ESC_TITLE_PX, ESC_TITLE_SLOT);
    if (my < p->top + ESC_PAD || my >= p->top + ESC_PAD + titleH)
        return -1;
    return Esc_LangHitX(p, mx);
}

static void Esc_DrawLangTitle(void* surf, const RECT* p)
{
    int xs[ESC_LANG_N], ws[ESC_LANG_N];
    int mode = Lang_GetMode(), i;
    int titleH = Esc_TextHeight(ESC_TITLE_PX, ESC_TITLE_SLOT);
    int bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    int ty = p->top + ESC_PAD + (titleH - bodyH) / 2;

    Esc_LangLayout(p, xs, ws);
    for (i = 0; i < ESC_LANG_N; i++)
    {
        int on = (s_LangMode[i] == mode);
        Esc_Text(surf, s_LangLabel[i], xs[i], ty, ws[i] + 2,
                 ESC_BODY_PX, ESC_BODY_SLOT,
                 on ? 245 : 145, on ? 225 : 145, on ? 170 : 145);
        if (on)
            Esc_Fill(surf, xs[i], ty + bodyH + 2, ws[i], 2,
                     208, 160, 64, 255);
    }
}

static void Esc_DrawRowsRow(void* surf, int rowL, int rowY, int rowW,
                            int bodyH, const wchar_t* label)
{
    wchar_t num[16];
    int value = SWS_MaxRows();
    int ctlX, btnY, numW, tw, numX;

    ctlX = Esc_RowCtlX(rowL + rowW);
    btnY = rowY + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;
    Esc_RowLabel(surf, rowL, rowY, ctlX, bodyH, label);

    Esc_StepButton(surf, ctlX, btnY, L"-", value > ESC_ROWS_MIN);
    Esc_StepButton(surf, ctlX + ESC_STEP_W - ESC_STEP_BTN_W, btnY, L"+",
                   value < ESC_ROWS_MAX);

    Esc_IntToW(num, (unsigned)value, 16);
    tw = Esc_TextWidth(num, ESC_BODY_PX, ESC_BODY_SLOT);
    numW = ESC_STEP_W - ESC_STEP_BTN_W * 2 - 8;
    numX = ctlX + ESC_STEP_BTN_W + 4 + (numW - tw) / 2;
    Esc_Text(surf, num, numX, rowY + (ESC_ROW_H - bodyH) / 2,
             numW, ESC_BODY_PX, ESC_BODY_SLOT, 238, 238, 238);
}

static void Esc_DrawSkinRow(void* surf, int rowL, int rowY, int rowW,
                            int bodyH, const wchar_t* label)
{
    int xs[ESC_SKIN_N], ws[ESC_SKIN_N];
    int value = SidebarSkin_Get();
    int i, ty;

    if (value < 0 || value >= ESC_SKIN_N) value = 0;
    ty = rowY + (ESC_ROW_H - bodyH) / 2;

    Esc_SkinSegLayout(rowL, rowW, xs, ws);
    Esc_RowLabel(surf, rowL, rowY, xs[0], bodyH, label);

    for (i = 0; i < ESC_SKIN_N; i++)
    {
        int on = (i == value);
        Esc_Text(surf, s_GuiSkinName[i], xs[i], ty, ws[i] + 2,
                 ESC_BODY_PX, ESC_BODY_SLOT,
                 on ? 245 : 145, on ? 225 : 145, on ? 170 : 145);
    }
    Esc_Fill(surf, xs[value], rowY + ESC_ROW_H - 6, ws[value], 2,
             208, 160, 64, 255);
}

static void Esc_SettingsPanel(void* surf, RECT* p)
{
    int w, bodyH, rowL, rowW, i, y, rh, kind, item;

    if (!Esc_PanelFrame(surf, p, s_GuiTitle)) return;

    w = p->right - p->left;
    bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    if (bodyH > ESC_ROW_H) bodyH = ESC_ROW_H;

    rowL = p->left + ESC_PAD;
    rowW = w - ESC_PAD * 2;

    if (s_GuiMissing)
    {
        int wy = Esc_ContentTop(p);
        Esc_Fill(surf, rowL, wy, rowW, ESC_WARN_H, 88, 30, 30, 220);
        Esc_Text(surf, s_MissingNotice, rowL + ESC_TEXT_INSET,
                 wy + (ESC_WARN_H - bodyH) / 2,
                 rowW - ESC_TEXT_INSET * 2, ESC_BODY_PX, ESC_BODY_SLOT,
                 255, 210, 210);
    }

    for (i = 0; i < ESC_GUI_DISPLAY; i++)
    {
        y = Esc_GuiRowTop(p, i);
        rh = Esc_GuiRowH(i);
        if (y + rh > p->bottom - ESC_PAD) break;

        kind = s_GuiLayout[i].kind;
        item = s_GuiLayout[i].item;

        if (kind == EG_GROUP)
        {
            Esc_Fill(surf, rowL, y + (rh - ESC_BODY_PX) / 2, 3, ESC_BODY_PX,
                     208, 160, 64, 255);
            Esc_Text(surf, s_GuiGroup[item], rowL + ESC_TEXT_INSET,
                     y + (rh - bodyH) / 2, rowW - ESC_TEXT_INSET - 6,
                     ESC_BODY_PX, ESC_BODY_SLOT, 250, 220, 160);
            continue;
        }

        Esc_RowBg(surf, rowL, y, rowW);
        Esc_Fill(surf, rowL, y + ESC_ROW_H - 1, rowW, 1,
                 0x7B, 0x79, 0x7B, 80);

        if (kind == EG_SWITCH)
        {
            int on = 0;
            if (item == 0) on = SB_Enabled();
            else if (item == 1) on = PV_ButtonsEnabled();
            else if (item == 2) on = GV_IsOn();
            else if (item == 3) on = SWS_Enabled();
            Esc_DrawSwitchRow(surf, rowL, y, rowW, bodyH,
                              s_GuiLabel[item], on);
        }
        else if (kind == EG_SORT)
            Esc_DrawSortRow(surf, rowL, y, rowW, bodyH, s_GuiLabel[item]);
        else if (kind == EG_ROWS)
            Esc_DrawRowsRow(surf, rowL, y, rowW, bodyH, s_GuiLabel[item]);
        else if (kind == EG_SKIN)
            Esc_DrawSkinRow(surf, rowL, y, rowW, bodyH, s_GuiLabel[item]);
    }
}

static void Esc_DrawPanels(void* surf, HWND hDlg)
{
    EscRow leftRows[ESC_LEFT_ROWS];
    EscRow rightRows[2];
    RECT a, b;
    int i;

    for (i = 0; i < ESC_LEFT_ROWS; i++)
    {
        leftRows[i].text = s_LeftItem[i];
        leftRows[i].selected = (s_Sel == i);
    }
    for (i = 0; i < 2; i++)
    {
        rightRows[i].text = s_RightItem[i];
        rightRows[i].selected = 0;
    }

    if (!Esc_Layout(hDlg, &a, &b)) return;

    Esc_Panel(surf, &a, s_LeftTitle, leftRows, ESC_LEFT_ROWS);
    Esc_DrawLangTitle(surf, &a);
    if (s_Sel == 0)
        Esc_SettingsPanel(surf, &b);
    else
        Esc_Panel(surf, &b, s_RightTitle, rightRows, 2);
}

/* ---------------- input ---------------- */

static int Esc_ChLow(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int Esc_LineSection(const unsigned char* d, DWORD ls, DWORD le,
                           const char* section)
{
    DWORD p = ls, e, i;
    int n = lstrlenA(section);
    while (p < le && (d[p] == ' ' || d[p] == '\t')) p++;
    if (p >= le || d[p] != '[') return 0;
    p++;
    e = p;
    while (e < le && d[e] != ']') e++;
    while (e > p && (d[e - 1] == ' ' || d[e - 1] == '\t')) e--;
    if ((int)(e - p) != n) return 0;
    for (i = 0; i < (DWORD)n; i++)
        if (Esc_ChLow(d[p + i]) != Esc_ChLow((unsigned char)section[i])) return 0;
    return 1;
}

static int Esc_LineKey(const unsigned char* d, DWORD ls, DWORD le,
                       const char* key)
{
    DWORD p = ls;
    int i, n;
    while (p < le && (d[p] == ' ' || d[p] == '\t')) p++;
    if (p >= le || d[p] == ';' || d[p] == '[') return 0;
    n = lstrlenA(key);
    for (i = 0; i < n; i++)
    {
        if (p >= le || Esc_ChLow(d[p]) != Esc_ChLow((unsigned char)key[i]))
            return 0;
        p++;
    }
    while (p < le && (d[p] == ' ' || d[p] == '\t')) p++;
    return (p < le && d[p] == '=') ? 1 : 0;
}

static void Esc_ValueRange(const unsigned char* d, DWORD ls, DWORD le,
                           DWORD* vs, DWORD* ve)
{
    DWORD p = ls;
    while (p < le && d[p] != '=') p++;
    if (p < le) p++;
    while (p < le && (d[p] == ' ' || d[p] == '\t')) p++;
    *vs = p;
    while (p < le && d[p] != '\r' && d[p] != '\n' && d[p] != ';' &&
           d[p] != ' ' && d[p] != '\t') p++;
    *ve = p;
}

static int Esc_IniLocate(const unsigned char* d, DWORD size,
                         const char* section, const char* key,
                         DWORD* vs, DWORD* ve, DWORD* insertAt)
{
    DWORD i = 0, insert = 0;
    int target = 0, foundSec = 0;

    while (i < size)
    {
        DWORD ls = i, le;
        while (i < size && d[i] != '\n') i++;
        le = i;
        if (le > ls && d[le - 1] == '\r') le--;
        if (i < size) i++;

        if (Esc_LineSection(d, ls, le, section))
        {
            target = 1;
            foundSec = 1;
            insert = i;
            continue;
        }
        {
            DWORD p = ls;
            while (p < le && (d[p] == ' ' || d[p] == '\t')) p++;
            if (p < le && d[p] == '[')
            {
                target = 0;
                continue;
            }
        }
        if (target)
        {
            insert = i;
            if (Esc_LineKey(d, ls, le, key))
            {
                Esc_ValueRange(d, ls, le, vs, ve);
                *insertAt = insert;
                return 1;
            }
        }
    }
    *insertAt = insert;
    return foundSec ? 0 : -1;
}

static unsigned Esc_PutBytes(unsigned char* out, unsigned pos,
                             const unsigned char* src, DWORD n)
{
    while (n--) out[pos++] = *src++;
    return pos;
}

static unsigned Esc_PutText(unsigned char* out, unsigned pos, const char* s)
{
    while (*s) out[pos++] = (unsigned char)*s++;
    return pos;
}

static int Esc_WriteIniValue(const char* section, const char* key, int value)
{
    HANDLE h;
    DWORD size, got, put, need, vs = 0, ve = 0, insertAt = 0;
    unsigned char* d;
    unsigned char* out;
    char val[16];
    unsigned vlen, keyLen, pos;
    int loc;

    h = CreateFileA(g_IniPath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE)
    {
        CloseHandle(h);
        return 0;
    }
    d = (unsigned char*)LocalAlloc(LPTR, size + 1);
    if (!d)
    {
        CloseHandle(h);
        return 0;
    }
    if (!ReadFile(h, d, size, &got, NULL) || got != size)
    {
        LocalFree(d);
        CloseHandle(h);
        return 0;
    }

    val[0] = 0;
    UtoA((unsigned)value, val);
    vlen = (unsigned)lstrlenA(val);
    keyLen = (unsigned)lstrlenA(key);

    loc = Esc_IniLocate(d, size, section, key, &vs, &ve, &insertAt);
    if (loc == 1)
    {
        need = size - (ve - vs) + vlen + 1;
        out = (unsigned char*)LocalAlloc(LPTR, need);
        if (!out) { LocalFree(d); CloseHandle(h); return 0; }
        pos = Esc_PutBytes(out, 0, d, vs);
        pos = Esc_PutText(out, pos, val);
        pos = Esc_PutBytes(out, pos, d + ve, size - ve);
        need = pos;
    }
    else if (loc == 0)
    {
        need = size + keyLen + vlen + 8;
        out = (unsigned char*)LocalAlloc(LPTR, need);
        if (!out) { LocalFree(d); CloseHandle(h); return 0; }
        pos = Esc_PutBytes(out, 0, d, insertAt);
        if (pos > 0 && out[pos - 1] != '\n')
        {
            out[pos++] = '\r';
            out[pos++] = '\n';
        }
        pos = Esc_PutText(out, pos, key);
        out[pos++] = '=';
        pos = Esc_PutText(out, pos, val);
        out[pos++] = '\r';
        out[pos++] = '\n';
        pos = Esc_PutBytes(out, pos, d + insertAt, size - insertAt);
        need = pos;
    }
    else
    {
        unsigned secLen = (unsigned)lstrlenA(section);
        need = size + secLen + keyLen + vlen + 9;
        out = (unsigned char*)LocalAlloc(LPTR, need);
        if (!out) { LocalFree(d); CloseHandle(h); return 0; }
        pos = Esc_PutBytes(out, 0, d, size);
        out[pos++] = '\r';
        out[pos++] = '\n';
        out[pos++] = '[';
        pos = Esc_PutText(out, pos, section);
        out[pos++] = ']';
        out[pos++] = '\r';
        out[pos++] = '\n';
        pos = Esc_PutText(out, pos, key);
        out[pos++] = '=';
        pos = Esc_PutText(out, pos, val);
        out[pos++] = '\r';
        out[pos++] = '\n';
        need = pos;
    }

    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    if (!WriteFile(h, out, need, &put, NULL) || put != need)
    {
        LocalFree(out);
        LocalFree(d);
        CloseHandle(h);
        return 0;
    }
    SetEndOfFile(h);
    CloseHandle(h);
    LocalFree(out);
    LocalFree(d);
    return 1;
}

static int Esc_HitLeft(const RECT* a, int mx, int my, int* index)
{
    int i, rowY, rowL, rowW;
    rowL = a->left + ESC_PAD;
    rowW = (a->right - a->left) - ESC_PAD * 2;
    rowY = Esc_ContentTop(a);
    for (i = 0; i < ESC_LEFT_ROWS; i++, rowY += ESC_ROW_H)
    {
        if (rowY + ESC_ROW_H > a->bottom - ESC_PAD) break;
        if (mx >= rowL && mx < rowL + rowW &&
            my >= rowY && my < rowY + ESC_ROW_H)
        {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int Esc_HitRight(const RECT* b, int mx, int my, int* index)
{
    int i, rowY, rowL, rowW, rh;
    if (s_Sel != 0) return 0;

    rowL = b->left + ESC_PAD;
    rowW = (b->right - b->left) - ESC_PAD * 2;
    for (i = 0; i < ESC_GUI_DISPLAY; i++)
    {
        rowY = Esc_GuiRowTop(b, i);
        rh = Esc_GuiRowH(i);
        if (rowY + rh > b->bottom - ESC_PAD) break;
        if (mx >= rowL && mx < rowL + rowW &&
            my >= rowY && my < rowY + rh)
        {
            if (s_GuiLayout[i].kind == EG_GROUP) return 0;
            if (s_GuiLayout[i].kind == EG_SWITCH)
            {
                int sx = rowL + rowW - ESC_CTL_RIGHT - ESC_SW_W;
                int sy = rowY + (rh - ESC_SW_H) / 2;
                if (mx < sx || mx >= sx + ESC_SW_W ||
                    my < sy || my >= sy + ESC_SW_H)
                    return 0;
            }
            if (s_GuiLayout[i].kind == EG_SKIN &&
                Esc_SkinSegmentAt(rowL, rowW, mx) < 0)
                return 0;
            *index = s_GuiLayout[i].item;
            return 1;
        }
    }
    return 0;
}

static void Esc_ApplyGui(HWND hDlg, const RECT* right, int index, int mx)
{
    int value;

    switch (index)
    {
    case 0:
        value = SB_Enabled() ? 0 : 1;
        SB_SetEnabled(value);
        Esc_WriteIniValue("BottomBar", "ScaleEnable", value);
        break;
    case 1:
        value = PV_ButtonsEnabled() ? 0 : 1;
        PV_SetButtonsEnabled(value);
        Esc_WriteIniValue("BottomBar", "PauseEnable", value);
        break;
    case 2:
        value = GV_IsOn() ? 0 : 1;
        GV_SetOn(value);
        Esc_WriteIniValue("BottomBar", "GridEnable", value);
        break;
    case 3:
        value = SWS_Enabled() ? 0 : 1;
        SWS_SetEnabled(value);
        Esc_WriteIniValue("SWSidebar", "Enable", value);
        break;
    case 4:
        value = SWS_FillMode() ? 0 : 1;
        SWS_SetFillMode(value);
        Esc_WriteIniValue("SWSidebar", "FillMode", value);
        break;
    case 5:
    {
        int ctlX = Esc_CtlX(right);
        if (mx < ctlX || mx >= ctlX + ESC_STEP_W) return;
        value = SWS_MaxRows();
        if (mx < ctlX + ESC_STEP_BTN_W) value--;
        else if (mx >= ctlX + ESC_STEP_W - ESC_STEP_BTN_W) value++;
        else return;
        if (value < ESC_ROWS_MIN || value > ESC_ROWS_MAX) return;
        SWS_SetMaxRows(value);
        Esc_WriteIniValue("SWSidebar", "MaxRows", value);
        break;
    }
    case 6:
    {
        int rowL = right->left + ESC_PAD;
        int rowW = (right->right - right->left) - ESC_PAD * 2;
        int pick = Esc_SkinSegmentAt(rowL, rowW, mx);

        if (pick < 0 || pick == SidebarSkin_Get()) return;
        SidebarSkin_Set(pick);
        SidebarSkin_Poll();
        Esc_WriteIniValue("Sidebar", "SkinSide", pick);
        break;
    }
    default:
        return;
    }

    InvalidateRect(hDlg, NULL, FALSE);
    UpdateWindow(hDlg);
}

static void Esc_ApplyLang(HWND hDlg, int seg)
{
    if (Lang_GetMode() == s_LangMode[seg]) return;
    Lang_SetMode(s_LangMode[seg]);
    EscMenu_ReloadLang();
    MsgText_ReloadLang();
    InvalidateRect(hDlg, NULL, FALSE);
    UpdateWindow(hDlg);
}

static void Esc_Select(HWND hDlg, int index)
{
    if (index == s_Sel) return;
    s_Sel = index;
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
    WNDPROC old = (WNDPROC)GetPropW(h, ESC_PROP_OLD_PROC);
    LRESULT r;

    if (msg == WM_LBUTTONDOWN && h == s_Host)
    {
        int mx = (int)(short)LOWORD(lp);
        int my = (int)(short)HIWORD(lp);
        int index = -1;
        RECT a, b;
        if (Esc_Layout(h, &a, &b))
        {
            int seg = Esc_LangHit(&a, mx, my);
            if (seg >= 0)
            {
                Esc_ApplyLang(h, seg);
                return 0;
            }
            if (Esc_HitLeft(&a, mx, my, &index))
            {
                Esc_Select(h, index);
                return 0;   /* consume only the list click */
            }
            if (Esc_HitRight(&b, mx, my, &index))
            {
                Esc_ApplyGui(h, &b, index, mx);
                return 0;
            }
        }
    }

    r = Esc_CallOld(h, old, msg, wp, lp);

    if (msg == WM_NCDESTROY)
    {
        RemovePropW(h, ESC_PROP_OLD_PROC);
        if (h == s_Host) s_Host = NULL;
    }
    return r;
}

static void Esc_Subclass(HWND hDlg)
{
    WNDPROC old;
    if (GetPropW(hDlg, ESC_PROP_OLD_PROC)) return;
    old = (WNDPROC)SetWindowLongPtrW(hDlg, GWLP_WNDPROC, (LONG_PTR)Esc_DlgProc);
    if (!old || !SetPropW(hDlg, ESC_PROP_OLD_PROC, (HANDLE)old))
    {
        if (old) SetWindowLongPtrW(hDlg, GWLP_WNDPROC, (LONG_PTR)old);
        LogLine("esc: subclass fail");
        return;
    }
}

/* ---------------- hooks ---------------- */

static void Esc_OnShow(HWND hDlg)
{
    EscMenu_EnsureInit();
    if (!s_Enable) return;
    if (OU_IsMP()) return;
    /* Only the ESC home page has the Game Controls button; 3003 sub-pages do not. */
    if (!GetDlgItem(hDlg, ESC_ID_GAMECONTROLS)) return;

    if (s_Host != hDlg)
    {
        s_Host = hDlg;
        s_Sel = 0;
    }
    Esc_Subclass(hDlg);
}

/* 0x4F1140: dialog created (ESI=HWND), before ShowWindow.
 * Original: lea eax,[esp+4] / push eax. */
extern "C" __declspec(dllexport) DWORD __cdecl EscMenu_OnShow(void* regs)
{
    HWND hDlg = (HWND)(size_t)*(unsigned*)((char*)regs + ESC_REG_ESI);
    if (hDlg) Esc_OnShow(hDlg);
    return 0;
}

/* 0x622396: after dialog content is drawn into the temporary BSurface (ESI),
 * before it is blitted. Original: mov ecx,[0x887310] (6 bytes). */
extern "C" __declspec(dllexport) DWORD __cdecl EscMenu_OnPaintSurface(void* regs)
{
    unsigned esp, hwnd, surf;

    if (!s_Enable || !s_Host) return 0;
    if (!IsWindow(s_Host)) { s_Host = NULL; return 0; }
    if (!IsWindowVisible(s_Host)) return 0;

    esp = *(unsigned*)((char*)regs + ESC_REG_ESP);
    hwnd = esp ? *(unsigned*)(size_t)(esp + ESC_STACK_HWND) : 0;
    if (hwnd != (unsigned)(size_t)s_Host) return 0;

    surf = *(unsigned*)((char*)regs + ESC_REG_ESI);
    if (!surf) return 0;

    Esc_DrawPanels((void*)(size_t)surf, s_Host);
    return 0;
}
