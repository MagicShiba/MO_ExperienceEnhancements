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
#include "BuildQueue.h"
#include "PlaceScatter.h"
#include "FullMap.h"
#include "SideScroll.h"
#include "MsgText.h"
#include "ChatEdit.h"
#include "LineEdit.h"
#include "OrderUtil.h"
#include "EscMenu.h"
#include "PlayerColor.h"
#include "SidebarSkin.h"

#define ESC_SECTION          "EscMenu"
#define ESC_REG_ESI          0x0Cu          /* Syringe REGISTERS: ESI */
#define ESC_REG_ESP          0x14u          /* Syringe REGISTERS: ESP */
#define ESC_STACK_HWND       0x10u          /* OwnerDraw::Paint local: HWND */
#define ESC_PROP_OLD_PROC    L"MOE_EscOldProc"

#define ESC_ID_GAMECONTROLS  1313
#define ESC_ID_BACK          1670
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
#define ESC_GUI_DISPLAY      18
#define ESC_SEP_H            14
#define ESC_SKIN_N           5
#define ESC_SCATTER_N        3
#define ESC_HIDE_N           4
#define ESC_SWS_SIDE_N       2
#define ESC_SEG_MAX          8
#define ESC_COLOR_SW         14
#define ESC_COLOR_GAP        4
#define ESC_COLOR_LABEL_W    128
#define ESC_LANG_N           3
#define ESC_LANG_GAP         14

#define ESC_SW_W             44
#define ESC_SW_H             20
#define ESC_STATE_W          64
#define ESC_CTL_RIGHT        12
#define ESC_STEP_W           96
#define ESC_STEP_BTN_W       24
#define ESC_STEP_BTN_H       20
#define ESC_STEP_NUM_PAD     10
#define ESC_CANCEL_W         78
#define ESC_CANCEL_H         24
#define ESC_CANCEL_GAP       3
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

enum {
    GUI_GROUP_GENERAL = 0,
    GUI_GROUP_BOTTOM,
    GUI_GROUP_SWS,
    GUI_GROUP_MISC,
    GUI_GROUP_SIDEBAR,
    GUI_GROUP_COUNT
};

enum {
    GUI_ITEM_SCALE = 0,
    GUI_ITEM_PAUSE,
    GUI_ITEM_GRID,
    GUI_ITEM_SWS,
    GUI_ITEM_SORT,
    GUI_ITEM_ROWS,
    GUI_ITEM_SKIN,
    GUI_ITEM_COLOR,
    GUI_ITEM_BUILD_QUEUE,
    GUI_ITEM_SCATTER,
    GUI_ITEM_SIDE_SCROLL,
    GUI_ITEM_SIDE_HIDE,
    GUI_ITEM_SWS_SIDE,
    GUI_ITEM_COUNT
};

enum {
    TX_GROUP_CAMP = 0,
    TX_CAMP_ENABLE,
    TX_CAMP_LINES,
    TX_CAMP_OPACITY,
    TX_CAMP_POS,
    TX_CAMP_ALIGN,
    TX_CAMP_WIDTH,
    TX_CAMP_ANIM,
    TX_CAMP_BAR,
    TX_SEP1,
    TX_GROUP_CHAT,
    TX_CHAT_EDIT,
    TX_CHAT_NAME,
    TX_SEP2,
    TX_GROUP_HINT,
    TX_HINT_SHOW,
    TX_HINT_COLOR,
    TX_HINT_LINES,
    TX_HINT_WIDTH,
    TX_HINT_BAR,
    TX_HINT_RADAR,
    TX_COUNT
};

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
static wchar_t s_GuiGroup[GUI_GROUP_COUNT][ESC_LABEL_CAP];
static wchar_t s_GuiLabel[GUI_ITEM_COUNT][ESC_LABEL_CAP];
static wchar_t s_GuiOn[16];
static wchar_t s_GuiOff[16];
static wchar_t s_GuiFaction[32];
static wchar_t s_GuiFill[32];
static wchar_t s_GuiSkinName[ESC_SKIN_N][32];
static wchar_t s_GuiScatter[ESC_SCATTER_N][32];
static wchar_t s_GuiHide[ESC_HIDE_N][32];
static wchar_t s_GuiSwsSide[ESC_SWS_SIDE_N][32];

static wchar_t s_TextTitle[ESC_LABEL_CAP];
static wchar_t s_TextLabel[TX_COUNT][ESC_LABEL_CAP];
static wchar_t s_TextPos[2][32];
static wchar_t s_TextAlign[2][32];
static wchar_t s_TextBar[3][32];
static wchar_t s_TextColor[2][32];
static wchar_t s_Cancel[32] = L"Cancel";
static int s_TextMissing = 0;
static int s_TextScroll = 0;

enum {
    EDIT_PAGE_GUI = 0,
    EDIT_PAGE_TEXT = 1
};

#define ESC_EDIT_CAP 8
#define ESC_EDIT_TIMER 0x4D4Fu

/* 对话框键盘消息不落到自绘控件 (焦点常在原生子控件), 与 ChatEdit 一样轮询按键。 */
#define ES_KEY(v, rep) { v, 0, 0, rep }
static LiKey s_EditKeys[] = {
    ES_KEY(VK_LEFT, 1), ES_KEY(VK_RIGHT, 1), ES_KEY(VK_HOME, 1), ES_KEY(VK_END, 1),
    ES_KEY(VK_DELETE, 1), ES_KEY(VK_BACK, 1), ES_KEY(VK_RETURN, 0), ES_KEY(VK_ESCAPE, 0),
    ES_KEY('0', 0), ES_KEY('1', 0), ES_KEY('2', 0), ES_KEY('3', 0), ES_KEY('4', 0),
    ES_KEY('5', 0), ES_KEY('6', 0), ES_KEY('7', 0), ES_KEY('8', 0), ES_KEY('9', 0),
    ES_KEY(VK_NUMPAD0, 0), ES_KEY(VK_NUMPAD1, 0), ES_KEY(VK_NUMPAD2, 0),
    ES_KEY(VK_NUMPAD3, 0), ES_KEY(VK_NUMPAD4, 0), ES_KEY(VK_NUMPAD5, 0),
    ES_KEY(VK_NUMPAD6, 0), ES_KEY(VK_NUMPAD7, 0), ES_KEY(VK_NUMPAD8, 0),
    ES_KEY(VK_NUMPAD9, 0)
};
#undef ES_KEY

static int s_EditOn = 0;
static HWND s_EditHost = NULL;
static int s_EditPage = -1;
static int s_EditItem = -1;
static int s_EditLen = 0;
static int s_EditCur = 0;
static int s_EditMin = 0;
static int s_EditMax = 0;
static int s_EditMaxLen = 1;
static wchar_t s_EditBuf[ESC_EDIT_CAP];

static int Esc_EditIs(int page, int item)
{
    return s_EditOn && s_EditPage == page && s_EditItem == item;
}

typedef struct {
    int scale, pause, grid, sws, fill, rows, skin, color;
    int bq, scatter, sideScroll, hide, swsSide;
    int chatEdit, chatName;
} EscSavedSettings;

#define ESC_SETTING_LIST(X) \
    X(scale, SB_Enabled, SB_SetEnabled) \
    X(pause, PV_ButtonsEnabled, PV_SetButtonsEnabled) \
    X(grid, GV_IsOn, GV_SetOn) \
    X(sws, SWS_Enabled, SWS_SetEnabled) \
    X(fill, SWS_FillMode, SWS_SetFillMode) \
    X(rows, SWS_MaxRows, SWS_SetMaxRows) \
    X(skin, SidebarSkin_Get, SidebarSkin_Set) \
    X(bq, BQ_Enabled, BQ_SetEnabled) \
    X(scatter, PS_Mode, PS_SetMode) \
    X(sideScroll, SS_Enabled, SS_SetEnabled) \
    X(hide, FM_HideState, FM_SetHideState) \
    X(swsSide, SWS_SideRight, SWS_SetSideRight) \
    X(chatEdit, ChatEdit_Enabled, ChatEdit_SetEnabled) \
    X(chatName, ChatEdit_ShowNameSP, ChatEdit_SetShowNameSP)

static EscSavedSettings s_Saved;
static int s_SavedOk = 0;
static int s_CancelDown = 0;

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

static void Esc_TextText(const char* key, wchar_t* out, int cap)
{
    if (!Esc_LoadText(key, out, cap)) s_TextMissing = 1;
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
    if (!Esc_LoadText("Cancel", s_Cancel, 32))
        lstrcpynW(s_Cancel, Lang_IsChinese() ? L"取消" : L"Cancel", 32);

    s_GuiMissing = 0;
    Esc_GuiText("GuiTitle", s_GuiTitle, ESC_LABEL_CAP);
    Esc_GuiText("GuiGroupGeneral", s_GuiGroup[GUI_GROUP_GENERAL], ESC_LABEL_CAP);
    Esc_GuiText("GuiGroupBottom", s_GuiGroup[GUI_GROUP_BOTTOM], ESC_LABEL_CAP);
    Esc_GuiText("GuiGroupSws", s_GuiGroup[GUI_GROUP_SWS], ESC_LABEL_CAP);
    Esc_GuiText("GuiGroupMisc", s_GuiGroup[GUI_GROUP_MISC], ESC_LABEL_CAP);
    Esc_GuiText("GuiGroupSidebar", s_GuiGroup[GUI_GROUP_SIDEBAR], ESC_LABEL_CAP);
    Esc_GuiText("GuiScale", s_GuiLabel[GUI_ITEM_SCALE], ESC_LABEL_CAP);
    Esc_GuiText("GuiPause", s_GuiLabel[GUI_ITEM_PAUSE], ESC_LABEL_CAP);
    Esc_GuiText("GuiGrid", s_GuiLabel[GUI_ITEM_GRID], ESC_LABEL_CAP);
    Esc_GuiText("GuiSws", s_GuiLabel[GUI_ITEM_SWS], ESC_LABEL_CAP);
    Esc_GuiText("GuiSort", s_GuiLabel[GUI_ITEM_SORT], ESC_LABEL_CAP);
    Esc_GuiText("GuiRows", s_GuiLabel[GUI_ITEM_ROWS], ESC_LABEL_CAP);
    Esc_GuiText("GuiSkin", s_GuiLabel[GUI_ITEM_SKIN], ESC_LABEL_CAP);
    Esc_GuiText("GuiColor", s_GuiLabel[GUI_ITEM_COLOR], ESC_LABEL_CAP);
    Esc_GuiText("GuiBuildQueue", s_GuiLabel[GUI_ITEM_BUILD_QUEUE], ESC_LABEL_CAP);
    Esc_GuiText("GuiScatter", s_GuiLabel[GUI_ITEM_SCATTER], ESC_LABEL_CAP);
    Esc_GuiText("GuiSideScroll", s_GuiLabel[GUI_ITEM_SIDE_SCROLL], ESC_LABEL_CAP);
    Esc_GuiText("GuiSideHide", s_GuiLabel[GUI_ITEM_SIDE_HIDE], ESC_LABEL_CAP);
    Esc_GuiText("GuiSwsSide", s_GuiLabel[GUI_ITEM_SWS_SIDE], ESC_LABEL_CAP);
    Esc_GuiText("GuiOn", s_GuiOn, 16);
    Esc_GuiText("GuiOff", s_GuiOff, 16);
    Esc_GuiText("GuiSortFaction", s_GuiFaction, 32);
    Esc_GuiText("GuiSortFill", s_GuiFill, 32);
    Esc_GuiText("GuiSkinDefault", s_GuiSkinName[0], 32);
    Esc_GuiText("GuiSkin1", s_GuiSkinName[1], 32);
    Esc_GuiText("GuiSkin2", s_GuiSkinName[2], 32);
    Esc_GuiText("GuiSkin3", s_GuiSkinName[3], 32);
    Esc_GuiText("GuiSkin4", s_GuiSkinName[4], 32);
    Esc_GuiText("GuiScatterOff", s_GuiScatter[0], 32);
    Esc_GuiText("GuiScatterOnPlace", s_GuiScatter[1], 32);
    Esc_GuiText("GuiScatterAuto", s_GuiScatter[2], 32);
    Esc_GuiText("GuiSideHideOff", s_GuiHide[0], 32);
    Esc_GuiText("GuiSideHideFull", s_GuiHide[1], 32);
    Esc_GuiText("GuiSideHideRadar", s_GuiHide[2], 32);
    Esc_GuiText("GuiSideHideBare", s_GuiHide[3], 32);
    Esc_GuiText("GuiSwsSideRight", s_GuiSwsSide[0], 32);
    Esc_GuiText("GuiSwsSideLeft", s_GuiSwsSide[1], 32);

    s_TextMissing = 0;
    Esc_TextText("TextTitle", s_TextTitle, ESC_LABEL_CAP);
    Esc_TextText("TextGroupCamp", s_TextLabel[TX_GROUP_CAMP], ESC_LABEL_CAP);
    Esc_TextText("TextCampEnable", s_TextLabel[TX_CAMP_ENABLE], ESC_LABEL_CAP);
    Esc_TextText("TextCampLines", s_TextLabel[TX_CAMP_LINES], ESC_LABEL_CAP);
    Esc_TextText("TextCampOpacity", s_TextLabel[TX_CAMP_OPACITY], ESC_LABEL_CAP);
    Esc_TextText("TextCampPos", s_TextLabel[TX_CAMP_POS], ESC_LABEL_CAP);
    Esc_TextText("TextCampAlign", s_TextLabel[TX_CAMP_ALIGN], ESC_LABEL_CAP);
    Esc_TextText("TextCampWidth", s_TextLabel[TX_CAMP_WIDTH], ESC_LABEL_CAP);
    Esc_TextText("TextCampAnim", s_TextLabel[TX_CAMP_ANIM], ESC_LABEL_CAP);
    Esc_TextText("TextCampBar", s_TextLabel[TX_CAMP_BAR], ESC_LABEL_CAP);
    Esc_TextText("TextGroupChat", s_TextLabel[TX_GROUP_CHAT], ESC_LABEL_CAP);
    Esc_TextText("TextChatEdit", s_TextLabel[TX_CHAT_EDIT], ESC_LABEL_CAP);
    Esc_TextText("TextChatName", s_TextLabel[TX_CHAT_NAME], ESC_LABEL_CAP);
    Esc_TextText("TextGroupHint", s_TextLabel[TX_GROUP_HINT], ESC_LABEL_CAP);
    Esc_TextText("TextHintShow", s_TextLabel[TX_HINT_SHOW], ESC_LABEL_CAP);
    Esc_TextText("TextHintColor", s_TextLabel[TX_HINT_COLOR], ESC_LABEL_CAP);
    Esc_TextText("TextHintLines", s_TextLabel[TX_HINT_LINES], ESC_LABEL_CAP);
    Esc_TextText("TextHintWidth", s_TextLabel[TX_HINT_WIDTH], ESC_LABEL_CAP);
    Esc_TextText("TextHintBar", s_TextLabel[TX_HINT_BAR], ESC_LABEL_CAP);
    Esc_TextText("TextHintRadar", s_TextLabel[TX_HINT_RADAR], ESC_LABEL_CAP);
    Esc_TextText("TextCampPosBottom", s_TextPos[0], 32);
    Esc_TextText("TextCampPosTop", s_TextPos[1], 32);
    Esc_TextText("TextAlignLeft", s_TextAlign[0], 32);
    Esc_TextText("TextAlignCenter", s_TextAlign[1], 32);
    Esc_TextText("TextBarNone", s_TextBar[0], 32);
    Esc_TextText("TextBarLeft", s_TextBar[1], 32);
    Esc_TextText("TextBarRight", s_TextBar[2], 32);
    Esc_TextText("TextColorWhite", s_TextColor[0], 32);
    Esc_TextText("TextColorPlayer", s_TextColor[1], 32);
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

static void Esc_Redraw(HWND hDlg)
{
    if (!hDlg || !IsWindow(hDlg)) return;
    InvalidateRect(hDlg, NULL, FALSE);
    UpdateWindow(hDlg);
}

static int Esc_EditCurAt(const RECT* right, int mx, const wchar_t* s, int len);

static void Esc_EditBegin(HWND hDlg, const RECT* right, int mx, int page,
                          int item, int value, int minValue, int maxValue)
{
    int i;

    Esc_IntToW(s_EditBuf, (unsigned)value, ESC_EDIT_CAP);
    s_EditLen = lstrlenW(s_EditBuf);
    s_EditCur = Esc_EditCurAt(right, mx, s_EditBuf, s_EditLen);
    s_EditOn = 1;
    s_EditHost = hDlg;
    s_EditPage = page;
    s_EditItem = item;
    s_EditMin = minValue;
    s_EditMax = maxValue;
    s_EditMaxLen = 1;
    for (i = maxValue; i >= 10; i /= 10) s_EditMaxLen++;
    LiKeysReset(s_EditKeys,
                (int)(sizeof(s_EditKeys) / sizeof(s_EditKeys[0])), 1);
    SetTimer(hDlg, ESC_EDIT_TIMER, 16, NULL);
    Esc_Redraw(hDlg);
}

static int Esc_ColorPerRow(int rowW)
{
    int avail = rowW - ESC_COLOR_LABEL_W - ESC_CTL_RIGHT;
    int per = (avail + ESC_COLOR_GAP) / (ESC_COLOR_SW + ESC_COLOR_GAP);
    return per > 0 ? per : 1;
}

static int Esc_ColorRows(int rowW)
{
    int count = PlayerColor_Count();
    int per;
    if (count <= 0) return 1;
    per = Esc_ColorPerRow(rowW);
    return (count + per - 1) / per;
}

enum {
    EG_GROUP = 0,
    EG_SWITCH,
    EG_SORT,
    EG_ROWS,
    EG_SKIN,
    EG_COLOR,
    EG_CHOICE,
    EG_VALUE,
    EG_SEP
};

typedef struct {
    unsigned char kind;
    unsigned char item;
} EscGuiRowInfo;

static const EscGuiRowInfo s_GuiLayout[ESC_GUI_DISPLAY] = {
    { EG_GROUP,  GUI_GROUP_GENERAL },
    { EG_SWITCH, GUI_ITEM_BUILD_QUEUE },
    { EG_CHOICE, GUI_ITEM_SCATTER },
    { EG_GROUP,  GUI_GROUP_SWS },
    { EG_SWITCH, GUI_ITEM_SWS },
    { EG_SORT,   GUI_ITEM_SORT },
    { EG_ROWS,   GUI_ITEM_ROWS },
    { EG_CHOICE, GUI_ITEM_SWS_SIDE },
    { EG_GROUP,  GUI_GROUP_BOTTOM },
    { EG_SWITCH, GUI_ITEM_SCALE },
    { EG_SWITCH, GUI_ITEM_PAUSE },
    { EG_SWITCH, GUI_ITEM_GRID },
    { EG_GROUP,  GUI_GROUP_SIDEBAR },
    { EG_SWITCH, GUI_ITEM_SIDE_SCROLL },
    { EG_CHOICE, GUI_ITEM_SIDE_HIDE },
    { EG_GROUP,  GUI_GROUP_MISC },
    { EG_COLOR,  GUI_ITEM_COLOR },
    { EG_SKIN,   GUI_ITEM_SKIN }
};

static const EscGuiRowInfo s_TextLayout[TX_COUNT] = {
    { EG_GROUP,  TX_GROUP_CAMP },
    { EG_SWITCH, TX_CAMP_ENABLE },
    { EG_VALUE,  TX_CAMP_LINES },
    { EG_VALUE,  TX_CAMP_OPACITY },
    { EG_CHOICE, TX_CAMP_POS },
    { EG_CHOICE, TX_CAMP_ALIGN },
    { EG_VALUE,  TX_CAMP_WIDTH },
    { EG_SWITCH, TX_CAMP_ANIM },
    { EG_CHOICE, TX_CAMP_BAR },
    { EG_SEP,    TX_SEP1 },
    { EG_GROUP,  TX_GROUP_CHAT },
    { EG_SWITCH, TX_CHAT_EDIT },
    { EG_SWITCH, TX_CHAT_NAME },
    { EG_SEP,    TX_SEP2 },
    { EG_GROUP,  TX_GROUP_HINT },
    { EG_SWITCH, TX_HINT_SHOW },
    { EG_CHOICE, TX_HINT_COLOR },
    { EG_VALUE,  TX_HINT_LINES },
    { EG_VALUE,  TX_HINT_WIDTH },
    { EG_CHOICE, TX_HINT_BAR },
    { EG_SWITCH, TX_HINT_RADAR }
};

static int Esc_GuiContentTop(const RECT* p)
{
    int y = Esc_ContentTop(p);
    if (s_GuiMissing) y += ESC_WARN_H + ESC_WARN_GAP;
    return y;
}

static int Esc_GuiRowH(const RECT* p, int index)
{
    int rows;

    if (s_GuiLayout[index].kind == EG_GROUP) return ESC_GROUP_H;
    if (s_GuiLayout[index].kind != EG_COLOR) return ESC_ROW_H;

    rows = Esc_ColorRows((p->right - p->left) - ESC_PAD * 2);
    return ESC_ROW_H + (rows - 1) * (ESC_COLOR_SW + ESC_COLOR_GAP);
}

static int Esc_TextContentTop(const RECT* p)
{
    int y = Esc_ContentTop(p);
    if (s_TextMissing) y += ESC_WARN_H + ESC_WARN_GAP;
    return y;
}

static int Esc_TextRowH(int index)
{
    int kind = s_TextLayout[index].kind;
    if (kind == EG_GROUP) return ESC_GROUP_H;
    if (kind == EG_SEP) return ESC_SEP_H;
    return ESC_ROW_H;
}

static int Esc_TextTotalH(void)
{
    int i, h = 0;
    for (i = 0; i < TX_COUNT; i++)
    {
        if (s_TextLayout[i].kind == EG_GROUP && s_TextLayout[i].item != 0)
            h += ESC_BLOCK_GAP;
        h += Esc_TextRowH(i);
    }
    return h;
}

/* 吸附到行顶: 滚动量只取行起始位置, 避免整行被上下裁剪。 */
static int Esc_TextSnap(int scroll)
{
    int i, y = 0, best = 0;
    for (i = 0; i < TX_COUNT; i++)
    {
        if (s_TextLayout[i].kind == EG_GROUP && s_TextLayout[i].item != 0)
            y += ESC_BLOCK_GAP;
        if (y <= scroll) best = y;
        y += Esc_TextRowH(i);
    }
    return best;
}

static int Esc_TextViewH(const RECT* p)
{
    int h = p->bottom - ESC_PAD - Esc_TextContentTop(p);
    return h > 0 ? h : 0;
}

static int Esc_TextMaxScroll(const RECT* p)
{
    int max = Esc_TextTotalH() - Esc_TextViewH(p);
    return max > 0 ? max : 0;
}

static int Esc_TextClampScroll(const RECT* p)
{
    int max = Esc_TextMaxScroll(p);
    if (s_TextScroll > max) s_TextScroll = Esc_TextSnap(max);
    if (s_TextScroll < 0) s_TextScroll = 0;
    return max;
}

/* ---------------- 16-bit BSurface drawing ---------------- */

static unsigned char* Esc_LockRect(void* surf, int* x, int* y, int* w, int* h,
                                   int* pitch)
{
    int W, H, bpp;
    unsigned char* base;

    if (!surf || *w <= 0 || *h <= 0) return NULL;
    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    bpp = GtSurfGet(surf, GtVT_GET_BPP);
    *pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || bpp != 2 || *pitch < W * 2) return NULL;

    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > W) *w = W - *x;
    if (*y + *h > H) *h = H - *y;
    if (*w <= 0 || *h <= 0) return NULL;

    base = GtLock(surf);
    return base;
}

static void Esc_Fill(void* surf, int x, int y, int w, int h,
                     unsigned r, unsigned g, unsigned b, unsigned a)
{
    unsigned char* base;
    int pitch, i, j;
    unsigned short col;

    if (a == 0) return;
    base = Esc_LockRect(surf, &x, &y, &w, &h, &pitch);
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

static void Esc_Fill16(void* surf, int x, int y, int w, int h,
                       unsigned short col)
{
    unsigned char* base;
    int pitch, i, j;

    base = Esc_LockRect(surf, &x, &y, &w, &h, &pitch);
    if (!base) return;
    for (j = 0; j < h; j++)
    {
        unsigned short* row = (unsigned short*)(base + (size_t)(y + j) * pitch + (size_t)x * 2);
        for (i = 0; i < w; i++) row[i] = col;
    }
    GtUnlock(surf);
}

static void Esc_Outline(void* surf, int x, int y, int w, int h,
                        unsigned r, unsigned g, unsigned b, unsigned a,
                        int t = 2)
{
    if (w < 8 || h < 8) return;
    Esc_Fill(surf, x, y, w, t, r, g, b, a);
    Esc_Fill(surf, x, y + h - t, w, t, r, g, b, a);
    Esc_Fill(surf, x, y + t, t, h - t * 2, r, g, b, a);
    Esc_Fill(surf, x + w - t, y + t, t, h - t * 2, r, g, b, a);
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

static void Esc_DrawMissingNotice(void* surf, const RECT* p, int bodyH,
                                  int rowL, int rowW)
{
    int y = Esc_ContentTop(p);

    Esc_Fill(surf, rowL, y, rowW, ESC_WARN_H, 88, 30, 30, 220);
    Esc_Text(surf, s_MissingNotice, rowL + ESC_TEXT_INSET,
             y + (ESC_WARN_H - bodyH) / 2,
             rowW - ESC_TEXT_INSET * 2, ESC_BODY_PX, ESC_BODY_SLOT,
             255, 210, 210);
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

static void Esc_SegLayout(wchar_t names[][32], int n,
                          int rowL, int rowW, int* xs, int* ws)
{
    int i, total = 0, x;

    for (i = 0; i < n; i++)
    {
        ws[i] = Esc_TextWidth(names[i], ESC_BODY_PX, ESC_BODY_SLOT);
        total += ws[i];
        if (i) total += ESC_SKIN_GAP;
    }
    x = rowL + rowW - ESC_CTL_RIGHT - total;
    for (i = 0; i < n; i++)
    {
        xs[i] = x;
        x += ws[i] + ESC_SKIN_GAP;
    }
}

static int Esc_SegmentAt(wchar_t names[][32], int n,
                         int rowL, int rowW, int mx)
{
    int xs[ESC_SEG_MAX], ws[ESC_SEG_MAX], i;

    Esc_SegLayout(names, n, rowL, rowW, xs, ws);
    for (i = 0; i < n; i++)
        if (mx >= xs[i] && mx < xs[i] + ws[i]) return i;
    return -1;
}

static int Esc_PickSeg(wchar_t names[][32], int n, const RECT* right, int mx)
{
    return Esc_SegmentAt(names, n, right->left + ESC_PAD,
                         (right->right - right->left) - ESC_PAD * 2, mx);
}

static wchar_t (*Esc_ChoiceNames(int item))[32]
{
    if (item == GUI_ITEM_SIDE_HIDE) return s_GuiHide;
    if (item == GUI_ITEM_SWS_SIDE) return s_GuiSwsSide;
    return s_GuiScatter;
}

static int Esc_ChoiceN(int item)
{
    if (item == GUI_ITEM_SIDE_HIDE) return ESC_HIDE_N;
    if (item == GUI_ITEM_SWS_SIDE) return ESC_SWS_SIDE_N;
    return ESC_SCATTER_N;
}

static int Esc_ChoiceValue(int item)
{
    if (item == GUI_ITEM_SIDE_HIDE) return FM_HideState();
    if (item == GUI_ITEM_SWS_SIDE) return SWS_SideRight() ? 0 : 1;
    return PS_Mode();
}

static wchar_t (*Esc_TextChoiceNames(int item))[32]
{
    if (item == TX_CAMP_POS) return s_TextPos;
    if (item == TX_CAMP_ALIGN) return s_TextAlign;
    if (item == TX_HINT_COLOR) return s_TextColor;
    return s_TextBar;
}

static int Esc_TextChoiceN(int item)
{
    if (item == TX_CAMP_BAR || item == TX_HINT_BAR) return 3;
    return 2;
}

static int Esc_TextChoiceValue(int item)
{
    if (item == TX_CAMP_POS) return MsgText_GetOption(MSG_OPT_CAMP_BOTTOM) ? 0 : 1;
    if (item == TX_CAMP_ALIGN) return MsgText_GetOption(MSG_OPT_CAMP_CENTERED) ? 1 : 0;
    if (item == TX_CAMP_BAR) return MsgText_GetOption(MSG_OPT_CAMP_BAR);
    if (item == TX_HINT_COLOR) return MsgText_GetOption(MSG_OPT_HINT_COLOR);
    if (item == TX_HINT_BAR) return MsgText_GetOption(MSG_OPT_HINT_BAR);
    return 0;
}

typedef struct {
    int item;
    int opt;
    const char* key;
    int min, max, step;
} EscTextNum;

static const EscTextNum s_TextNum[] = {
    { TX_CAMP_LINES,   MSG_OPT_CAMP_LINES,   "MaxLines",     ESC_ROWS_MIN, ESC_ROWS_MAX, 1 },
    { TX_CAMP_OPACITY, MSG_OPT_CAMP_OPACITY, "BoardOpacity", 0,            100,          5 },
    { TX_CAMP_WIDTH,   MSG_OPT_CAMP_WIDTH,   "MaxWidth",     200,          1600,         50 },
    { TX_HINT_LINES,   MSG_OPT_HINT_LINES,   "HintMaxLines", ESC_ROWS_MIN, ESC_ROWS_MAX, 1 },
    { TX_HINT_WIDTH,   MSG_OPT_HINT_WIDTH,   "HintMaxWidth", 100,          1600,         20 }
};

static const EscTextNum* Esc_TextNumFind(int item)
{
    int i;
    for (i = 0; i < (int)(sizeof(s_TextNum) / sizeof(s_TextNum[0])); i++)
        if (s_TextNum[i].item == item) return &s_TextNum[i];
    return NULL;
}

static void Esc_ColorCell(int i, int per, int rowL, int* px, int* py)
{
    int step = ESC_COLOR_SW + ESC_COLOR_GAP;

    *px = rowL + ESC_COLOR_LABEL_W + (i % per) * step;
    *py = (i / per) * step;
}

static int Esc_ColorSegmentAt(int rowL, int rowW, int rowY, int mx, int my)
{
    int i, x, y, count = PlayerColor_Count();
    int per = Esc_ColorPerRow(rowW);
    int top = rowY + (ESC_ROW_H - ESC_COLOR_SW) / 2;

    for (i = 0; i < count; i++)
    {
        Esc_ColorCell(i, per, rowL, &x, &y);
        y += top;
        if (mx >= x - 2 && mx < x + ESC_COLOR_SW + 2 &&
            my >= y - 2 && my < y + ESC_COLOR_SW + 2)
            return i;
    }
    return -1;
}

static void Esc_DrawRainbowSwatch(void* surf, int x, int y, int size)
{
    int i;
    for (i = 0; i < PLAYER_COLOR_RAINBOW_SEGS; i++)
    {
        int x0 = x + (i * size) / PLAYER_COLOR_RAINBOW_SEGS;
        int x1 = x + ((i + 1) * size) / PLAYER_COLOR_RAINBOW_SEGS;
        Esc_Fill16(surf, x0, y, x1 - x0, size,
                   PlayerColor_RainbowSwatch(i));
    }
}

static int Esc_CtlX(const RECT* p)
{
    return Esc_RowCtlX(p->right - ESC_PAD);
}

static int Esc_StepHit(const RECT* right, int mx, int* value,
                       int minValue, int maxValue, int step)
{
    int ctlX = Esc_CtlX(right);

    if (mx < ctlX || mx >= ctlX + ESC_STEP_W) return 0;
    if (mx < ctlX + ESC_STEP_BTN_W) *value -= step;
    else if (mx >= ctlX + ESC_STEP_W - ESC_STEP_BTN_W) *value += step;
    else return 0;
    return *value >= minValue && *value <= maxValue;
}

static void Esc_StepBox(int ctlX, int* x, int* w)
{
    *x = ctlX + ESC_STEP_BTN_W;
    *w = ESC_STEP_W - ESC_STEP_BTN_W * 2;
}

static int Esc_StepNumX(int boxX, int boxW, const wchar_t* s)
{
    int numW = boxW - ESC_STEP_NUM_PAD;
    int tw = Esc_TextWidth(s, ESC_BODY_PX, ESC_BODY_SLOT);

    if (tw > numW) tw = numW;
    return boxX + (boxW - tw) / 2;
}

static int Esc_StepMid(const RECT* right, int mx)
{
    int x, w;

    Esc_StepBox(Esc_CtlX(right), &x, &w);
    return mx >= x && mx < x + w;
}

static void Esc_CancelRect(const RECT* p, RECT* r)
{
    r->right = p->right;
    r->left = r->right - ESC_CANCEL_W;
    r->top = p->bottom + ESC_CANCEL_GAP;
    r->bottom = r->top + ESC_CANCEL_H;
}

static int Esc_CancelHit(const RECT* p, int mx, int my)
{
    RECT r;

    Esc_CancelRect(p, &r);
    return mx >= r.left && mx < r.right && my >= r.top && my < r.bottom;
}

static int Esc_EditCurAt(const RECT* right, int mx, const wchar_t* s, int len)
{
    wchar_t pre[ESC_EDIT_CAP];
    int boxX, boxW, numX, i, best = 0, bestDist = 0x7FFFFFFF;

    Esc_StepBox(Esc_CtlX(right), &boxX, &boxW);
    numX = Esc_StepNumX(boxX, boxW, s);
    for (i = 0; i <= len; i++)
    {
        int w, d;
        lstrcpynW(pre, s, i + 1);
        w = numX + Esc_TextWidth(pre, ESC_BODY_PX, ESC_BODY_SLOT);
        d = mx - w;
        if (d < 0) d = -d;
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return best;
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

static void Esc_DrawSegRow(void* surf, int rowL, int rowY, int rowW,
                           int bodyH, const wchar_t* label,
                           wchar_t names[][32], int n, int value)
{
    int xs[ESC_SEG_MAX], ws[ESC_SEG_MAX];
    int i, ty;

    if (value < 0 || value >= n) value = 0;
    ty = rowY + (ESC_ROW_H - bodyH) / 2;

    Esc_SegLayout(names, n, rowL, rowW, xs, ws);
    Esc_RowLabel(surf, rowL, rowY, xs[0], bodyH, label);

    for (i = 0; i < n; i++)
    {
        int on = (i == value);
        Esc_Text(surf, names[i], xs[i], ty, ws[i] + 2,
                 ESC_BODY_PX, ESC_BODY_SLOT,
                 on ? 245 : 145, on ? 225 : 145, on ? 170 : 145);
    }
    Esc_Fill(surf, xs[value], rowY + ESC_ROW_H - 6, ws[value], 2,
             208, 160, 64, 255);
}

static void Esc_DrawColorRow(void* surf, int rowL, int rowY, int rowW,
                             int bodyH, const wchar_t* label)
{
    int value = PlayerColor_Current();
    int i, x, y, count = PlayerColor_Count();
    int per = Esc_ColorPerRow(rowW);
    int top = rowY + (ESC_ROW_H - ESC_COLOR_SW) / 2;

    Esc_RowLabel(surf, rowL, rowY, rowL + ESC_COLOR_LABEL_W - 8,
                 bodyH, label);

    for (i = 0; i < count; i++)
    {
        Esc_ColorCell(i, per, rowL, &x, &y);
        y += top;
        if (i == value)
            Esc_Fill(surf, x - 2, y - 2, ESC_COLOR_SW + 4,
                     ESC_COLOR_SW + 4, 208, 160, 64, 255);
        if (i == count - 1)
            Esc_DrawRainbowSwatch(surf, x, y, ESC_COLOR_SW);
        else
            Esc_Fill16(surf, x, y, ESC_COLOR_SW, ESC_COLOR_SW,
                       PlayerColor_Swatch(i));
        Esc_Outline(surf, x, y, ESC_COLOR_SW, ESC_COLOR_SW, 0, 0, 0, 200);
    }
}

static void Esc_DrawValueRow(void* surf, int rowL, int rowY, int rowW,
                             int bodyH, const wchar_t* label, int value,
                             int minValue, int maxValue, int editing)
{
    wchar_t num[16];
    int ctlX, btnY, boxX, boxW, numW, numX;
    unsigned bg = editing ? 40u : 24u;
    unsigned border = editing ? 208u : 0x7Bu;
    unsigned borderG = editing ? 160u : 0x79u;
    unsigned borderB = editing ? 64u : 0x7Bu;
    unsigned borderA = editing ? 255u : 150u;

    ctlX = Esc_RowCtlX(rowL + rowW);
    btnY = rowY + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;
    Esc_RowLabel(surf, rowL, rowY, ctlX, bodyH, label);

    Esc_StepButton(surf, ctlX, btnY, L"-", value > minValue);
    Esc_StepButton(surf, ctlX + ESC_STEP_W - ESC_STEP_BTN_W, btnY, L"+",
                   value < maxValue);

    Esc_StepBox(ctlX, &boxX, &boxW);
    Esc_Fill(surf, boxX, btnY, boxW, ESC_STEP_BTN_H, bg, bg, bg, 230);
    Esc_Outline(surf, boxX, btnY, boxW, ESC_STEP_BTN_H,
                border, borderG, borderB, borderA, 1);

    if (editing) lstrcpynW(num, s_EditBuf, 16);
    else Esc_IntToW(num, (unsigned)value, 16);
    numW = boxW - ESC_STEP_NUM_PAD;
    numX = Esc_StepNumX(boxX, boxW, num);
    Esc_Text(surf, num, numX, rowY + (ESC_ROW_H - bodyH) / 2,
             numW, ESC_BODY_PX, ESC_BODY_SLOT, 238, 238, 238);

    if (editing)
    {
        wchar_t pre[ESC_EDIT_CAP];
        int caretX;
        lstrcpynW(pre, num, s_EditCur + 1);
        caretX = numX + Esc_TextWidth(pre, ESC_BODY_PX, ESC_BODY_SLOT);
        if (caretX > boxX + boxW - 2) caretX = boxX + boxW - 2;
        Esc_Fill(surf, caretX, btnY + 3, 1, ESC_STEP_BTN_H - 6,
                 235, 235, 235, 255);
    }
}

static int Esc_TextSwitchValue(int item)
{
    switch (item)
    {
    case TX_CAMP_ENABLE: return MsgText_GetOption(MSG_OPT_CAMP_ENABLE);
    case TX_CAMP_ANIM:   return MsgText_GetOption(MSG_OPT_CAMP_ANIM);
    case TX_CHAT_EDIT:   return ChatEdit_Enabled();
    case TX_CHAT_NAME:   return ChatEdit_ShowNameSP();
    case TX_HINT_SHOW:   return MsgText_GetOption(MSG_OPT_HINT_SHOW);
    case TX_HINT_RADAR:  return MsgText_GetOption(MSG_OPT_HINT_RADAR);
    default:             return 0;
    }
}

static void Esc_DrawCancel(void* surf, const RECT* p)
{
    RECT r;
    int tw, th;
    unsigned bg = s_CancelDown ? 52u : 68u;

    Esc_CancelRect(p, &r);
    Esc_Fill(surf, r.left, r.top, ESC_CANCEL_W, ESC_CANCEL_H,
             bg, bg, bg, 255);
    Esc_Outline(surf, r.left, r.top, ESC_CANCEL_W, ESC_CANCEL_H,
                0x7B, 0x79, 0x7B, 220, 1);

    tw = Esc_TextWidth(s_Cancel, ESC_BODY_PX, ESC_BODY_SLOT);
    if (tw > ESC_CANCEL_W - 8) tw = ESC_CANCEL_W - 8;
    th = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    if (th > ESC_CANCEL_H) th = ESC_CANCEL_H;
    Esc_Text(surf, s_Cancel, r.left + (ESC_CANCEL_W - tw) / 2,
             r.top + (ESC_CANCEL_H - th) / 2, ESC_CANCEL_W - 8,
             ESC_BODY_PX, ESC_BODY_SLOT, 238, 238, 238);
}

static void Esc_TextPanel(void* surf, RECT* p)
{
    int w, bodyH, rowL, rowW, i, y, rh, kind, item;
    int clipTop, clipBottom, maxScroll;

    if (!Esc_PanelFrame(surf, p, s_TextTitle)) return;

    w = p->right - p->left;
    bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    if (bodyH > ESC_ROW_H) bodyH = ESC_ROW_H;

    rowL = p->left + ESC_PAD;
    rowW = w - ESC_PAD * 2;
    clipTop = Esc_TextContentTop(p);
    clipBottom = p->bottom - ESC_PAD;
    maxScroll = Esc_TextClampScroll(p);

    if (s_TextMissing)
        Esc_DrawMissingNotice(surf, p, bodyH, rowL, rowW);

    for (i = 0, y = clipTop - s_TextScroll; i < TX_COUNT; i++)
    {
        kind = s_TextLayout[i].kind;
        item = s_TextLayout[i].item;

        if (kind == EG_GROUP && item != 0) y += ESC_BLOCK_GAP;
        rh = Esc_TextRowH(i);

        if (y >= clipTop && y + rh <= clipBottom)
        {
            if (kind == EG_SEP)
            {
                int sy = y + rh / 2;
                Esc_Fill(surf, rowL + ESC_TEXT_INSET, sy,
                         rowW - ESC_TEXT_INSET * 2, 1, 0x7B, 0x79, 0x7B, 160);
            }
            else if (kind == EG_GROUP)
            {
                Esc_Fill(surf, rowL, y + (rh - ESC_BODY_PX) / 2, 3,
                         ESC_BODY_PX, 208, 160, 64, 255);
                Esc_Text(surf, s_TextLabel[item], rowL + ESC_TEXT_INSET,
                         y + (rh - bodyH) / 2, rowW - ESC_TEXT_INSET - 6,
                         ESC_BODY_PX, ESC_BODY_SLOT, 250, 220, 160);
            }
            else
            {
                Esc_RowBg(surf, rowL, y, rowW);
                Esc_Fill(surf, rowL, y + rh - 1, rowW, 1,
                         0x7B, 0x79, 0x7B, 80);

                if (kind == EG_SWITCH)
                    Esc_DrawSwitchRow(surf, rowL, y, rowW, bodyH,
                                      s_TextLabel[item],
                                      Esc_TextSwitchValue(item));
                else if (kind == EG_VALUE)
                {
                    const EscTextNum* num = Esc_TextNumFind(item);
                    if (num)
                        Esc_DrawValueRow(surf, rowL, y, rowW, bodyH,
                                         s_TextLabel[item],
                                         MsgText_GetOption(num->opt),
                                         num->min, num->max,
                                         Esc_EditIs(EDIT_PAGE_TEXT, item));
                }
                else if (kind == EG_CHOICE)
                    Esc_DrawSegRow(surf, rowL, y, rowW, bodyH,
                                   s_TextLabel[item],
                                   Esc_TextChoiceNames(item),
                                   Esc_TextChoiceN(item),
                                   Esc_TextChoiceValue(item));
            }
        }
        y += rh;
    }

    if (maxScroll > 0)
    {
        int trackX = p->right - 6;
        int trackH = clipBottom - clipTop;
        int total = Esc_TextTotalH();
        int thumbH, thumbY;

        if (total < 1) total = 1;
        thumbH = trackH * (trackH + maxScroll) / total;
        if (thumbH < 18) thumbH = 18;
        if (thumbH > trackH) thumbH = trackH;
        thumbY = clipTop;
        if (trackH > thumbH)
            thumbY += (trackH - thumbH) * s_TextScroll / maxScroll;
        Esc_Fill(surf, trackX, clipTop, 3, trackH, 0x7B, 0x79, 0x7B, 90);
        Esc_Fill(surf, trackX, thumbY, 3, thumbH, 208, 160, 64, 220);
    }
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
        Esc_DrawMissingNotice(surf, p, bodyH, rowL, rowW);

    for (i = 0, y = Esc_GuiContentTop(p); i < ESC_GUI_DISPLAY; i++)
    {
        kind = s_GuiLayout[i].kind;
        item = s_GuiLayout[i].item;

        if (kind == EG_GROUP && item != 0) y += ESC_BLOCK_GAP;
        rh = Esc_GuiRowH(p, i);
        if (y + rh > p->bottom - ESC_PAD) break;

        if (kind == EG_GROUP)
        {
            Esc_Fill(surf, rowL, y + (rh - ESC_BODY_PX) / 2, 3, ESC_BODY_PX,
                     208, 160, 64, 255);
            Esc_Text(surf, s_GuiGroup[item], rowL + ESC_TEXT_INSET,
                     y + (rh - bodyH) / 2, rowW - ESC_TEXT_INSET - 6,
                     ESC_BODY_PX, ESC_BODY_SLOT, 250, 220, 160);
            y += rh;
            continue;
        }

        Esc_RowBg(surf, rowL, y, rowW);
        if (rh > ESC_ROW_H)
            Esc_Fill(surf, rowL, y + ESC_ROW_H, rowW, rh - ESC_ROW_H,
                     32, 32, 32, 120);
        Esc_Fill(surf, rowL, y + rh - 1, rowW, 1,
                 0x7B, 0x79, 0x7B, 80);

        if (kind == EG_SWITCH)
        {
            int on = 0;
            switch (item)
            {
            case GUI_ITEM_SCALE:       on = SB_Enabled(); break;
            case GUI_ITEM_PAUSE:       on = PV_ButtonsEnabled(); break;
            case GUI_ITEM_GRID:        on = GV_IsOn(); break;
            case GUI_ITEM_SWS:         on = SWS_Enabled(); break;
            case GUI_ITEM_BUILD_QUEUE: on = BQ_Enabled(); break;
            case GUI_ITEM_SIDE_SCROLL: on = SS_Enabled(); break;
            }
            Esc_DrawSwitchRow(surf, rowL, y, rowW, bodyH,
                              s_GuiLabel[item], on);
        }
        else if (kind == EG_SORT)
            Esc_DrawSortRow(surf, rowL, y, rowW, bodyH, s_GuiLabel[item]);
        else if (kind == EG_ROWS)
            Esc_DrawValueRow(surf, rowL, y, rowW, bodyH, s_GuiLabel[item],
                             SWS_MaxRows(), ESC_ROWS_MIN, ESC_ROWS_MAX,
                             Esc_EditIs(EDIT_PAGE_GUI, GUI_ITEM_ROWS));
        else if (kind == EG_COLOR)
            Esc_DrawColorRow(surf, rowL, y, rowW, bodyH, s_GuiLabel[item]);
        else if (kind == EG_SKIN)
            Esc_DrawSegRow(surf, rowL, y, rowW, bodyH, s_GuiLabel[item],
                           s_GuiSkinName, ESC_SKIN_N, SidebarSkin_Get());
        else if (kind == EG_CHOICE)
            Esc_DrawSegRow(surf, rowL, y, rowW, bodyH, s_GuiLabel[item],
                           Esc_ChoiceNames(item), Esc_ChoiceN(item),
                           Esc_ChoiceValue(item));
        y += rh;
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
    else if (s_Sel == 1)
        Esc_TextPanel(surf, &b);
    else
        Esc_Panel(surf, &b, s_RightTitle, rightRows, 2);
    if (s_Sel == 0 || s_Sel == 1)
        Esc_DrawCancel(surf, &b);
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

static int Esc_WriteIniRaw(const char* section, const char* key,
                           const char* value)
{
    HANDLE h;
    DWORD size, got, put, need, vs = 0, ve = 0, insertAt = 0;
    unsigned char* d;
    unsigned char* out;
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

    vlen = (unsigned)lstrlenA(value);
    keyLen = (unsigned)lstrlenA(key);

    loc = Esc_IniLocate(d, size, section, key, &vs, &ve, &insertAt);
    if (loc == 1)
    {
        need = size - (ve - vs) + vlen + 1;
        out = (unsigned char*)LocalAlloc(LPTR, need);
        if (!out) { LocalFree(d); CloseHandle(h); return 0; }
        pos = Esc_PutBytes(out, 0, d, vs);
        pos = Esc_PutText(out, pos, value);
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
        pos = Esc_PutText(out, pos, value);
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
        pos = Esc_PutText(out, pos, value);
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

/* 延迟写入: 设置页面内点击只改内存并排队, 关闭菜单时统一落盘; 切页不写,
 * 这样取消能丢掉整次设置。同一 section+key 只保留最后一次, 避免反复重写。 */
#define ESC_PENDING_MAX 40
typedef struct {
    char section[24];
    char key[40];
    char value[24];
} EscPending;
static EscPending s_Pend[ESC_PENDING_MAX];
static int s_PendN = 0;

static int Esc_PendFind(const char* section, const char* key)
{
    int i;
    for (i = 0; i < s_PendN; i++)
        if (!lstrcmpiA(s_Pend[i].section, section) &&
            !lstrcmpiA(s_Pend[i].key, key))
            return i;
    return -1;
}

static void Esc_FlushPending(void)
{
    int i;
    for (i = 0; i < s_PendN; i++)
        Esc_WriteIniRaw(s_Pend[i].section, s_Pend[i].key, s_Pend[i].value);
    s_PendN = 0;
}

static void Esc_QueueRaw(const char* section, const char* key,
                         const char* value)
{
    int i = Esc_PendFind(section, key);
    if (i < 0)
    {
        if (s_PendN >= ESC_PENDING_MAX) Esc_FlushPending();
        i = s_PendN++;
        lstrcpynA(s_Pend[i].section, section, sizeof(s_Pend[i].section));
        lstrcpynA(s_Pend[i].key, key, sizeof(s_Pend[i].key));
    }
    lstrcpynA(s_Pend[i].value, value, sizeof(s_Pend[i].value));
}

static void Esc_QueueValue(const char* section, const char* key, int value)
{
    char val[16];
    val[0] = 0;
    UtoA((unsigned)value, val);
    Esc_QueueRaw(section, key, val);
}

static void Esc_ApplyNum(int page, int item, int value)
{
    const EscTextNum* num;

    if (page == EDIT_PAGE_GUI)
    {
        SWS_SetMaxRows(value);
        Esc_QueueValue("SWSidebar", "MaxRows", value);
    }
    else if ((num = Esc_TextNumFind(item)) != NULL)
    {
        MsgText_SetOption(num->opt, value);
        Esc_QueueValue("MsgText", num->key, value);
    }
}

/* 步进按钮直接改值; 点中间进入编辑; 其余点击仍吞掉。 */
static void Esc_StepOrEdit(HWND hDlg, const RECT* right, int mx, int page,
                           int item, int value, int minValue, int maxValue,
                           int step)
{
    if (Esc_StepHit(right, mx, &value, minValue, maxValue, step))
    {
        Esc_ApplyNum(page, item, value);
        Esc_Redraw(hDlg);
    }
    else if (Esc_StepMid(right, mx))
        Esc_EditBegin(hDlg, right, mx, page, item, value, minValue, maxValue);
}

/* 快照打开设置页时的运行期值; 取消时恢复, INI 队列直接丢弃。 */
static void Esc_SaveSettings(void)
{
#define ESC_GET(n, g, s) s_Saved.n = g();
    ESC_SETTING_LIST(ESC_GET)
#undef ESC_GET
    s_Saved.color = PlayerColor_Current();
    MsgText_SaveOptions();
    s_SavedOk = 1;
}

static void Esc_RestoreSettings(void)
{
    if (!s_SavedOk) return;
    MsgText_RestoreOptions();
#define ESC_SET(n, g, s) s(s_Saved.n);
    ESC_SETTING_LIST(ESC_SET)
#undef ESC_SET
    SidebarSkin_Poll();
    if (s_Saved.color >= 0) PlayerColor_Set(s_Saved.color);
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

static int Esc_HitRight(const RECT* b, int mx, int my, int* index,
                        int* rowTop)
{
    int i, rowY, rowL, rowW, rh;
    if (s_Sel != 0) return 0;

    rowL = b->left + ESC_PAD;
    rowW = (b->right - b->left) - ESC_PAD * 2;
    for (i = 0, rowY = Esc_GuiContentTop(b); i < ESC_GUI_DISPLAY; i++)
    {
        int kind = s_GuiLayout[i].kind;

        if (kind == EG_GROUP && s_GuiLayout[i].item != 0) rowY += ESC_BLOCK_GAP;
        rh = Esc_GuiRowH(b, i);
        if (rowY + rh > b->bottom - ESC_PAD) break;
        if (mx >= rowL && mx < rowL + rowW &&
            my >= rowY && my < rowY + rh)
        {
            if (kind == EG_GROUP) return 0;
            if (kind == EG_SWITCH)
            {
                int sx = rowL + rowW - ESC_CTL_RIGHT - ESC_SW_W;
                int sy = rowY + (rh - ESC_SW_H) / 2;
                if (mx < sx || mx >= sx + ESC_SW_W ||
                    my < sy || my >= sy + ESC_SW_H)
                    return 0;
            }
            if (kind == EG_SKIN &&
                Esc_SegmentAt(s_GuiSkinName, ESC_SKIN_N, rowL, rowW, mx) < 0)
                return 0;
            if (kind == EG_CHOICE &&
                Esc_SegmentAt(Esc_ChoiceNames(s_GuiLayout[i].item),
                              Esc_ChoiceN(s_GuiLayout[i].item),
                              rowL, rowW, mx) < 0)
                return 0;
            if (kind == EG_COLOR &&
                Esc_ColorSegmentAt(rowL, rowW, rowY, mx, my) < 0)
                return 0;
            *index = s_GuiLayout[i].item;
            if (rowTop) *rowTop = rowY;
            return 1;
        }
        rowY += rh;
    }
    return 0;
}

static void Esc_ApplyGui(HWND hDlg, const RECT* right, int index, int mx,
                         int my, int rowTop)
{
    int value;

    switch (index)
    {
    case GUI_ITEM_SCALE:
        value = SB_Enabled() ? 0 : 1;
        SB_SetEnabled(value);
        Esc_QueueValue("BottomBar", "ScaleEnable", value);
        break;
    case GUI_ITEM_PAUSE:
        value = PV_ButtonsEnabled() ? 0 : 1;
        PV_SetButtonsEnabled(value);
        Esc_QueueValue("BottomBar", "PauseEnable", value);
        break;
    case GUI_ITEM_GRID:
        value = GV_IsOn() ? 0 : 1;
        GV_SetOn(value);
        Esc_QueueValue("BottomBar", "GridEnable", value);
        break;
    case GUI_ITEM_SWS:
        value = SWS_Enabled() ? 0 : 1;
        SWS_SetEnabled(value);
        Esc_QueueValue("SWSidebar", "Enable", value);
        break;
    case GUI_ITEM_SORT:
        value = SWS_FillMode() ? 0 : 1;
        SWS_SetFillMode(value);
        Esc_QueueValue("SWSidebar", "FillMode", value);
        break;
    case GUI_ITEM_ROWS:
        Esc_StepOrEdit(hDlg, right, mx, EDIT_PAGE_GUI, GUI_ITEM_ROWS,
                       SWS_MaxRows(), ESC_ROWS_MIN, ESC_ROWS_MAX, 1);
        return;
    case GUI_ITEM_SKIN:
    {
        int pick = Esc_PickSeg(s_GuiSkinName, ESC_SKIN_N, right, mx);

        if (pick < 0 || pick == SidebarSkin_Get()) return;
        SidebarSkin_Set(pick);
        SidebarSkin_Poll();
        Esc_QueueValue("Sidebar", "SkinSide", pick);
        break;
    }
    case GUI_ITEM_COLOR:
    {
        int rowL = right->left + ESC_PAD;
        int rowW = (right->right - right->left) - ESC_PAD * 2;
        int pick = Esc_ColorSegmentAt(rowL, rowW, rowTop, mx, my);

        if (pick < 0 || pick == PlayerColor_Current()) return;
        if (!PlayerColor_Set(pick)) return;
        break;
    }
    case GUI_ITEM_BUILD_QUEUE:
        value = BQ_Enabled() ? 0 : 1;
        BQ_SetEnabled(value);
        Esc_QueueValue("Features", "BuildQueue", value);
        break;
    case GUI_ITEM_SCATTER:
    {
        int pick = Esc_PickSeg(s_GuiScatter, ESC_SCATTER_N, right, mx);

        if (pick < 0 || pick == PS_Mode()) return;
        PS_SetMode(pick);
        Esc_QueueValue("PlaceScatter", "Mode", pick);
        break;
    }
    case GUI_ITEM_SIDE_SCROLL:
        value = SS_Enabled() ? 0 : 1;
        SS_SetEnabled(value);
        Esc_QueueValue("Sidebar", "ScrollEnable", value);
        break;
    case GUI_ITEM_SIDE_HIDE:
    {
        int pick = Esc_PickSeg(s_GuiHide, ESC_HIDE_N, right, mx);

        if (pick < 0 || pick == FM_HideState()) return;
        FM_SetHideState(pick);
        Esc_QueueValue("Sidebar", "HideEnable", pick ? 1 : 0);
        if (pick) Esc_QueueValue("Sidebar", "Mode", pick - 1);
        break;
    }
    case GUI_ITEM_SWS_SIDE:
    {
        int pick = Esc_PickSeg(s_GuiSwsSide, ESC_SWS_SIDE_N, right, mx);
        int rightSide;

        if (pick < 0) return;
        rightSide = (pick == 0);
        if (rightSide == (SWS_SideRight() != 0)) return;
        SWS_SetSideRight(rightSide);
        Esc_QueueRaw("SWSidebar", "Side", rightSide ? "right" : "left");
        break;
    }
    default:
        return;
    }

    Esc_Redraw(hDlg);
}

static int Esc_TextHitRight(const RECT* b, int mx, int my, int* index)
{
    int i, y, rh, kind, item;
    int rowL = b->left + ESC_PAD;
    int rowW = (b->right - b->left) - ESC_PAD * 2;
    int clipTop = Esc_TextContentTop(b);
    int clipBottom = b->bottom - ESC_PAD;

    if (s_Sel != 1) return 0;
    Esc_TextClampScroll(b);

    y = clipTop - s_TextScroll;
    for (i = 0; i < TX_COUNT; i++)
    {
        kind = s_TextLayout[i].kind;
        item = s_TextLayout[i].item;

        if (kind == EG_GROUP && item != 0) y += ESC_BLOCK_GAP;
        rh = Esc_TextRowH(i);
        if (y >= clipTop && y + rh <= clipBottom &&
            mx >= rowL && mx < rowL + rowW &&
            my >= y && my < y + rh)
        {
            if (kind == EG_GROUP || kind == EG_SEP) return 0;
            if (kind == EG_SWITCH)
            {
                int sx = rowL + rowW - ESC_CTL_RIGHT - ESC_SW_W;
                int sy = y + (rh - ESC_SW_H) / 2;
                if (mx < sx || mx >= sx + ESC_SW_W ||
                    my < sy || my >= sy + ESC_SW_H)
                    return 0;
            }
            if (kind == EG_VALUE)
            {
                int ctlX = Esc_RowCtlX(rowL + rowW);
                if (mx < ctlX || mx >= ctlX + ESC_STEP_W) return 0;
            }
            if (kind == EG_CHOICE &&
                Esc_SegmentAt(Esc_TextChoiceNames(item),
                              Esc_TextChoiceN(item), rowL, rowW, mx) < 0)
                return 0;
            *index = item;
            return 1;
        }
        y += rh;
    }
    return 0;
}

static void Esc_TextApply(HWND hDlg, const RECT* right, int index, int mx)
{
    int value;

    switch (index)
    {
    case TX_CAMP_ENABLE:
        value = MsgText_GetOption(MSG_OPT_CAMP_ENABLE) ? 0 : 1;
        MsgText_SetOption(MSG_OPT_CAMP_ENABLE, value);
        Esc_QueueValue("MsgText", "CampEnable", value);
        break;
    case TX_CAMP_LINES:
    case TX_CAMP_OPACITY:
    case TX_CAMP_WIDTH:
    case TX_HINT_LINES:
    case TX_HINT_WIDTH:
    {
        const EscTextNum* num = Esc_TextNumFind(index);
        if (!num) return;
        Esc_StepOrEdit(hDlg, right, mx, EDIT_PAGE_TEXT, index,
                       MsgText_GetOption(num->opt), num->min, num->max,
                       num->step);
        return;
    }
    case TX_CAMP_POS:
    {
        int pick = Esc_PickSeg(s_TextPos, 2, right, mx);
        if (pick < 0) return;
        MsgText_SetOption(MSG_OPT_CAMP_BOTTOM, pick == 0 ? 1 : 0);
        Esc_QueueValue("MsgText", "CampBottom", pick == 0 ? 1 : 0);
        break;
    }
    case TX_CAMP_ALIGN:
    {
        int pick = Esc_PickSeg(s_TextAlign, 2, right, mx);
        if (pick < 0) return;
        MsgText_SetOption(MSG_OPT_CAMP_CENTERED, pick);
        Esc_QueueValue("MsgText", "CampCentered", pick);
        break;
    }
    case TX_CAMP_ANIM:
        value = MsgText_GetOption(MSG_OPT_CAMP_ANIM) ? 0 : 1;
        MsgText_SetOption(MSG_OPT_CAMP_ANIM, value);
        Esc_QueueValue("MsgText", "CampAnim", value);
        break;
    case TX_CAMP_BAR:
    {
        int pick = Esc_PickSeg(s_TextBar, 3, right, mx);
        if (pick < 0) return;
        MsgText_SetOption(MSG_OPT_CAMP_BAR, pick);
        Esc_QueueValue("MsgText", "CampBarSide", pick);
        break;
    }
    case TX_CHAT_EDIT:
        value = ChatEdit_Enabled() ? 0 : 1;
        ChatEdit_SetEnabled(value);
        Esc_QueueValue("MsgText", "ChatEdit", value);
        break;
    case TX_CHAT_NAME:
        value = ChatEdit_ShowNameSP() ? 0 : 1;
        ChatEdit_SetShowNameSP(value);
        Esc_QueueValue("MsgText", "ChatNameSP", value);
        break;
    case TX_HINT_SHOW:
        value = MsgText_GetOption(MSG_OPT_HINT_SHOW) ? 0 : 1;
        MsgText_SetOption(MSG_OPT_HINT_SHOW, value);
        Esc_QueueValue("MsgText", "HintShow", value);
        break;
    case TX_HINT_COLOR:
    {
        int pick = Esc_PickSeg(s_TextColor, 2, right, mx);
        if (pick < 0) return;
        MsgText_SetOption(MSG_OPT_HINT_COLOR, pick);
        Esc_QueueValue("MsgText", "HintColorFollow", pick);
        break;
    }
    case TX_HINT_BAR:
    {
        int pick = Esc_PickSeg(s_TextBar, 3, right, mx);
        if (pick < 0) return;
        MsgText_SetOption(MSG_OPT_HINT_BAR, pick);
        Esc_QueueValue("MsgText", "HintBarSide", pick);
        break;
    }
    case TX_HINT_RADAR:
        value = MsgText_GetOption(MSG_OPT_HINT_RADAR) ? 0 : 1;
        MsgText_SetOption(MSG_OPT_HINT_RADAR, value);
        Esc_QueueValue("MsgText", "HintRadarShow", value);
        break;
    default:
        return;
    }

    Esc_Redraw(hDlg);
}

static void Esc_EditEnd(HWND hDlg)
{
    if (!s_EditOn) return;
    if (s_EditHost) KillTimer(s_EditHost, ESC_EDIT_TIMER);
    s_EditOn = 0;
    Esc_Redraw(hDlg);
}

static void Esc_EditCommit(HWND hDlg)
{
    int value = 0, i;

    if (!s_EditOn) return;
    for (i = 0; i < s_EditLen; i++)
        value = value * 10 + (s_EditBuf[i] - L'0');
    if (s_EditLen > 0)
    {
        if (value < s_EditMin) value = s_EditMin;
        if (value > s_EditMax) value = s_EditMax;
        Esc_ApplyNum(s_EditPage, s_EditItem, value);
    }
    Esc_EditEnd(hDlg);
}

static void Esc_EditPoll(HWND hDlg)
{
    unsigned now = GetTickCount();
    DWORD fgProc = 0, hostProc = 0;
    HWND fg;
    int i, changed = 0;

    if (!s_EditOn) return;
    fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &fgProc);
    GetWindowThreadProcessId(hDlg, &hostProc);
    if (fgProc != hostProc) return;

    for (i = 0; i < (int)(sizeof(s_EditKeys) / sizeof(s_EditKeys[0])); i++)
    {
        LiKey* k = &s_EditKeys[i];
        wchar_t ch;

        if (!LiKeyFire(k, now)) continue;
        if (k->vk == VK_RETURN) { Esc_EditCommit(hDlg); return; }
        if (k->vk == VK_ESCAPE) { Esc_EditEnd(hDlg); return; }
        if (k->vk == VK_BACK)
            LiBackspace(s_EditBuf, &s_EditLen, &s_EditCur);
        else if (k->vk == VK_DELETE)
            LiDelete(s_EditBuf, &s_EditLen, &s_EditCur);
        else if (k->vk == VK_LEFT || k->vk == VK_RIGHT ||
                 k->vk == VK_HOME || k->vk == VK_END)
            LiMove(k->vk, &s_EditCur, s_EditLen);
        else if (LiDigit(k->vk, &ch))
            LiInsert(s_EditBuf, ESC_EDIT_CAP, &s_EditLen, &s_EditCur, ch,
                     s_EditMaxLen);
        else
            continue;
        changed = 1;
    }
    if (changed) Esc_Redraw(hDlg);
}

static void Esc_ApplyLang(HWND hDlg, int seg)
{
    if (Lang_GetMode() == s_LangMode[seg]) return;
    Lang_SetMode(s_LangMode[seg]);
    EscMenu_ReloadLang();
    MsgText_ReloadLang();
    Esc_Redraw(hDlg);
}

static void Esc_Select(HWND hDlg, int index)
{
    if (index == s_Sel) return;
    Esc_EditCommit(hDlg);
    s_Sel = index;
    if (index != 1) s_TextScroll = 0;
    Esc_Redraw(hDlg);
}

static int Esc_TextWheel(HWND h, WPARAM wp, LPARAM lp)
{
    POINT pt;
    RECT a, b;
    int delta, old;

    if (s_Sel != 1) return 0;
    pt.x = (int)(short)LOWORD(lp);
    pt.y = (int)(short)HIWORD(lp);
    if (!ScreenToClient(h, &pt)) return 0;
    if (!Esc_Layout(h, &a, &b)) return 0;
    if (pt.x < b.left || pt.x >= b.right ||
        pt.y < b.top || pt.y >= b.bottom)
        return 0;

    delta = (int)(short)HIWORD(wp);
    if (delta == 0) return 0;
    old = s_TextScroll;
    s_TextScroll = Esc_TextSnap(s_TextScroll - (delta / 120) * ESC_ROW_H * 3);
    Esc_TextClampScroll(&b);
    if (s_TextScroll != old) Esc_Redraw(h);
    return 1;
}

static LRESULT Esc_CallOld(HWND h, WNDPROC old, UINT msg, WPARAM wp, LPARAM lp)
{
    if (IsWindowUnicode(h))
        return old ? CallWindowProcW(old, h, msg, wp, lp)
                   : DefWindowProcW(h, msg, wp, lp);
    return old ? CallWindowProcA(old, h, msg, wp, lp)
               : DefWindowProcA(h, msg, wp, lp);
}

static int Esc_EditClick(HWND hDlg, const RECT* right, int mx, int my)
{
    int index = -1;

    if (!s_EditOn || !Esc_StepMid(right, mx)) return 0;
    if (s_EditPage == EDIT_PAGE_GUI)
    {
        if (!Esc_HitRight(right, mx, my, &index, NULL)) return 0;
    }
    else if (s_EditPage == EDIT_PAGE_TEXT)
    {
        if (!Esc_TextHitRight(right, mx, my, &index)) return 0;
    }
    else return 0;
    if (index != s_EditItem) return 0;

    s_EditCur = Esc_EditCurAt(right, mx, s_EditBuf, s_EditLen);
    Esc_Redraw(hDlg);
    return 1;
}

static void Esc_Cancel(HWND hDlg)
{
    HWND back;

    if (GetCapture() == hDlg) ReleaseCapture();
    s_CancelDown = 0;
    Esc_EditEnd(NULL);
    Esc_RestoreSettings();
    s_PendN = 0;
    Esc_Redraw(hDlg);
    back = GetDlgItem(hDlg, ESC_ID_BACK);
    if (back)
        PostMessage(hDlg, WM_COMMAND,
                    MAKEWPARAM(ESC_ID_BACK, BN_CLICKED), (LPARAM)back);
    else
        PostMessage(hDlg, WM_COMMAND, IDCANCEL, 0);
}

static LRESULT CALLBACK Esc_DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropW(h, ESC_PROP_OLD_PROC);
    LRESULT r;

    if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && h == s_Host)
    {
        int mx = (int)(short)LOWORD(lp);
        int my = (int)(short)HIWORD(lp);
        int index = -1, rowY = 0;
        RECT a, b;

        if (Esc_Layout(h, &a, &b))
        {
            int seg;

            if ((s_Sel == 0 || s_Sel == 1) &&
                Esc_CancelHit(&b, mx, my))
            {
                s_CancelDown = 1;
                SetCapture(h);
                Esc_Redraw(h);
                return 0;
            }
            if (Esc_EditClick(h, &b, mx, my)) return 0;
            if (s_EditOn) Esc_EditCommit(h);
            seg = Esc_LangHit(&a, mx, my);
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
            if (Esc_HitRight(&b, mx, my, &index, &rowY))
            {
                Esc_ApplyGui(h, &b, index, mx, my, rowY);
                return 0;
            }
            if (Esc_TextHitRight(&b, mx, my, &index))
            {
                Esc_TextApply(h, &b, index, mx);
                return 0;
            }
        }
        else if (s_EditOn) Esc_EditCommit(h);
    }

    if (msg == WM_LBUTTONUP && h == s_Host && s_CancelDown)
    {
        int mx = (int)(short)LOWORD(lp);
        int my = (int)(short)HIWORD(lp);
        RECT a, b;

        if (Esc_Layout(h, &a, &b) && (s_Sel == 0 || s_Sel == 1) &&
            Esc_CancelHit(&b, mx, my))
            Esc_Cancel(h);
        else
        {
            s_CancelDown = 0;
            if (GetCapture() == h) ReleaseCapture();
            Esc_Redraw(h);
        }
        return 0;
    }

    if (msg == WM_CAPTURECHANGED && h == s_Host && s_CancelDown)
    {
        s_CancelDown = 0;
        Esc_Redraw(h);
    }

    if (msg == WM_TIMER && wp == ESC_EDIT_TIMER && h == s_Host)
    {
        Esc_EditPoll(h);
        return 0;
    }

    if (msg == WM_KILLFOCUS && h == s_Host && s_EditOn)
        Esc_EditCommit(h);

    if (msg == WM_MOUSEWHEEL && h == s_Host && Esc_TextWheel(h, wp, lp))
        return 0;

    /* 隐藏/销毁 ESC 菜单都视为离开设置页, 统一落盘。 */
    if (msg == WM_SHOWWINDOW && !wp && h == s_Host)
    {
        s_CancelDown = 0;
        if (GetCapture() == h) ReleaseCapture();
        Esc_EditCommit(h);
        Esc_FlushPending();
        Esc_SaveSettings();
    }

    r = Esc_CallOld(h, old, msg, wp, lp);

    if (msg == WM_NCDESTROY)
    {
        RemovePropW(h, ESC_PROP_OLD_PROC);
        if (h == s_Host)
        {
            s_CancelDown = 0;
            Esc_EditCommit(NULL);
            Esc_FlushPending();
            s_Host = NULL;
        }
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
        s_CancelDown = 0;
        Esc_EditEnd(NULL);
        Esc_FlushPending();
        Esc_SaveSettings();
        s_Host = hDlg;
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
