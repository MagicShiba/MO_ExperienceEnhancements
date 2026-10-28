/* EscMenu.cpp - custom ESC-menu page drawn into the game's owner-draw surface.
 * The game paints the dialog into a temporary 16-bit BSurface at 0x622396;
 * draw there after the original content. Display/input only, no simulation. */

#include <windows.h>
#include "GdiText.h"
#include "Lang.h"
#include "ScaleBar.h"
#include "PauseView.h"
#include "GridView.h"
#include "Shot.h"
#include "Weather.h"
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
#include "FrameCap.h"
#include "GameSpeed.h"
#include "LightingPreset.h"
#include "CrateIcon.h"

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
/* GUI 功能页行数随功能增加, 用略紧的组标题/组间距尽量多放几行, 1024x768 放
 * 不下的部分靠滚轮 (设置页也可滚, 见 Esc_PageWalk)。只作用于这一页,
 * 文本页仍用上面的值。 */
#define ESC_GUI_GROUP_H      26
#define ESC_GUI_GAP          6
#define ESC_BLOCK_GAP        12
#define ESC_WARN_H           26
#define ESC_WARN_GAP         8
#define ESC_TEXT_SS          2
#define ESC_LABEL_CAP        96
#define ESC_LEFT_ROWS        2
#define ESC_GUI_DISPLAY      33
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
#define ESC_SLIDE_W          300
#define ESC_SLIDE_GAP        10
#define ESC_SLIDE_TRK_H      8
#define ESC_SLIDE_KNOB_W     10
#define ESC_SLIDE_KNOB_H     16
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
    GUI_GROUP_GAMECTRL,
    GUI_GROUP_MISC,
    GUI_GROUP_SIDEBAR,
    GUI_GROUP_LIGHT,
    GUI_GROUP_COUNT
};

enum {
    GUI_ITEM_SCALE = 0,
    GUI_ITEM_PAUSE,
    GUI_ITEM_GRID,
    GUI_ITEM_SHOT,
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
    GUI_ITEM_GAMESPEED_ON,
    GUI_ITEM_GAMESPEED,
    GUI_ITEM_FPS,
    /* 光照: 预设行 + 参数行。参数行的四个格子各有自己的 item,
     * 因为数值编辑框按 item 记状态 (格子顺序 = 环境光/红/绿/蓝)。 */
    GUI_ITEM_LIGHT_PRESET,
    GUI_ITEM_LIGHT_PARM,
    GUI_ITEM_LIGHT_AMBIENT,
    GUI_ITEM_LIGHT_RED,
    GUI_ITEM_LIGHT_GREEN,
    GUI_ITEM_LIGHT_BLUE,
    GUI_ITEM_WTH_MODE,
    GUI_ITEM_WTH_RAIN,
    GUI_ITEM_WTH_SNOW,
    GUI_ITEM_WTH_FIRE,
    GUI_ITEM_WTH_CLOUD,
    GUI_ITEM_WTH_STYLE,
    GUI_ITEM_CRATE,
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
static wchar_t s_GuiWthMode[WEATHER_DRAW_N][32];
static wchar_t s_GuiWthStyle[WEATHER_CLOUD_STYLE_N][32];
/* 光照预设分段: 原图 + 标准/早上/下午/黄昏/晚上 + 自定义 (自定义只是状态)。 */
static wchar_t s_GuiLight[LIGHT_PRESET_COUNT + 1][32];

static wchar_t s_TextTitle[ESC_LABEL_CAP];
static wchar_t s_TextLabel[TX_COUNT][ESC_LABEL_CAP];
static wchar_t s_TextPos[2][32];
static wchar_t s_TextAlign[2][32];
static wchar_t s_TextBar[3][32];
static wchar_t s_TextColor[2][32];
static wchar_t s_Cancel[32] = L"Cancel";
static int s_TextMissing = 0;
static int s_TextScroll = 0;
static int s_GuiScroll = 0;

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
    ES_KEY(VK_UP, 1), ES_KEY(VK_DOWN, 1),
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
    int scale, pause, grid, shot, sws, fill, rows, skin, color;
    int bq, scatter, sideScroll, hide, swsSide;
    int chatEdit, chatName, speed, speedOn;
    int wthMode, wthRain, wthSnow, wthFire, wthCloud, wthStyle;
} EscSavedSettings;

#define ESC_SETTING_LIST(X) \
    X(scale, SB_Enabled, SB_SetEnabled) \
    X(pause, PV_ButtonsEnabled, PV_SetButtonsEnabled) \
    X(grid, GV_IsOn, GV_SetOn) \
    X(shot, Shot_Visible, Shot_SetVisible) \
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
    X(chatName, ChatEdit_ShowNameSP, ChatEdit_SetShowNameSP) \
    X(speed, GameSpeed_GetFps, GameSpeed_SetFps) \
    X(speedOn, GameSpeed_Enabled, GameSpeed_SetEnabled) \
    X(wthMode, Weather_DrawMode, Weather_SetDrawMode) \
    X(wthRain, Weather_Rain, Weather_SetRain) \
    X(wthSnow, Weather_Snow, Weather_SetSnow) \
    X(wthFire, Weather_Fire, Weather_SetFire) \
    X(wthCloud, Weather_Cloud, Weather_SetCloud) \
    X(wthStyle, Weather_CloudStyle, Weather_SetCloudStyle)

static EscSavedSettings s_Saved;
static int s_SavedOk = 0;
static int s_CancelDown = 0;
static int s_SlideDrag = 0;   /* 拖动条按住中 (游戏速度) */

static wchar_t s_LangLabel[ESC_LANG_N][32] = { L"ZH", L"EN", L"AUTO" };
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

/* 一组标签的键名表: 下标与对应 enum / 槽位同序, 增删要两处一起改。
 * NULL = 这一项没有文案 (文本页的两个分隔条)。 */
static const char* const kGroupKey[GUI_GROUP_COUNT] = {
    "GuiGroupGeneral", "GuiGroupBottom", "GuiGroupSws",
    "GuiGroupGameCtrl", "GuiGroupMisc", "GuiGroupSidebar",
    "GuiGroupLight"
};
static const char* const kLabelKey[GUI_ITEM_COUNT] = {
    "GuiScale", "GuiPause", "GuiGrid", "GuiShot", "GuiSws", "GuiSort",
    "GuiRows", "GuiSkin", "GuiColor", "GuiBuildQueue", "GuiScatter",
    "GuiSideScroll", "GuiSideHide", "GuiSwsSide", "GuiGameSpeedOn",
    "GuiGameSpeed", "GuiFps",
    "GuiLightPreset", "GuiLightParm", "GuiLightAmbient", "GuiLightRed",
    "GuiLightGreen", "GuiLightBlue", "GuiWthMode", "GuiWthRain",
    "GuiWthSnow", "GuiWthFire", "GuiWthCloud", "GuiWthStyle", "GuiCrate"
};
static const char* const kLightKey[LIGHT_PRESET_COUNT + 1] = {
    "GuiLightOrig", "GuiLightStandard", "GuiLightMorning", "GuiLightAfternoon",
    "GuiLightDusk", "GuiLightNight", "GuiLightCustom"
};
static const char* const kSkinKey[ESC_SKIN_N] = {
    "GuiSkinDefault", "GuiSkin1", "GuiSkin2", "GuiSkin3", "GuiSkin4"
};
static const char* const kScatterKey[ESC_SCATTER_N] = {
    "GuiScatterOff", "GuiScatterOnPlace", "GuiScatterAuto"
};
static const char* const kHideKey[ESC_HIDE_N] = {
    "GuiSideHideOff", "GuiSideHideFull", "GuiSideHideRadar", "GuiSideHideBare"
};
static const char* const kSwsSideKey[ESC_SWS_SIDE_N] = {
    "GuiSwsSideRight", "GuiSwsSideLeft"
};
static const char* const kWeatherKey[WEATHER_DRAW_N] = {
    "GuiWthImage", "GuiWthParticle"
};
/* 云影风格名 (与 Weather.cpp 的 kCldStyle 顺序一致) */
static const char* const kCldStyleKey[WEATHER_CLOUD_STYLE_N] = {
    "GuiWthStyle0", "GuiWthStyle1", "GuiWthStyle2", "GuiWthStyle3",
    "GuiWthStyle4", "GuiWthStyle5", "GuiWthStyle6"
};
static const char* const kTextKey[TX_COUNT] = {
    "TextGroupCamp", "TextCampEnable", "TextCampLines", "TextCampOpacity",
    "TextCampPos", "TextCampAlign", "TextCampWidth", "TextCampAnim",
    "TextCampBar", NULL, "TextGroupChat", "TextChatEdit", "TextChatName",
    NULL, "TextGroupHint", "TextHintShow", "TextHintColor", "TextHintLines",
    "TextHintWidth", "TextHintBar", "TextHintRadar"
};
static const char* const kPosKey[2] = { "TextCampPosBottom", "TextCampPosTop" };
static const char* const kAlignKey[2] = { "TextAlignLeft", "TextAlignCenter" };
static const char* const kBarKey[3] = { "TextBarNone", "TextBarLeft", "TextBarRight" };
static const char* const kColorKey[2] = { "TextColorWhite", "TextColorPlayer" };

/* 按表读一组标签; 缺文案时按 text 落到对应页的缺失标志。 */
static void Esc_LoadFam(const char* const* keys, wchar_t* dst, int cap, int n,
                        int text)
{
    int i;
    for (i = 0; i < n; i++, dst += cap)
    {
        if (!keys[i]) continue;
        if (text) Esc_TextText(keys[i], dst, cap);
        else      Esc_GuiText(keys[i], dst, cap);
    }
}

/* 项数/每项宽度都从数组本身算 (标签恒为 wchar_t[N] 的二维数组) */
#define ESC_LOAD_FAM(keys, arr, text)                                        \
    Esc_LoadFam((keys), &(arr)[0][0],                                        \
                (int)(sizeof((arr)[0]) / sizeof(wchar_t)),                    \
                (int)(sizeof(arr) / sizeof((arr)[0])), (text))

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
    if (!Esc_LoadText("Cancel", s_Cancel, 32))
        lstrcpynW(s_Cancel, Lang_IsChinese() ? L"取消" : L"Cancel", 32);

    s_GuiMissing = s_TextMissing = 0;
    Esc_GuiText("GuiTitle", s_GuiTitle, ESC_LABEL_CAP);
    ESC_LOAD_FAM(kGroupKey, s_GuiGroup, 0);
    ESC_LOAD_FAM(kLabelKey, s_GuiLabel, 0);
    Esc_GuiText("GuiOn", s_GuiOn, 16);
    Esc_GuiText("GuiOff", s_GuiOff, 16);
    Esc_GuiText("GuiSortFaction", s_GuiFaction, 32);
    Esc_GuiText("GuiSortFill", s_GuiFill, 32);
    ESC_LOAD_FAM(kSkinKey, s_GuiSkinName, 0);
    ESC_LOAD_FAM(kScatterKey, s_GuiScatter, 0);
    ESC_LOAD_FAM(kHideKey, s_GuiHide, 0);
    ESC_LOAD_FAM(kSwsSideKey, s_GuiSwsSide, 0);
    ESC_LOAD_FAM(kWeatherKey, s_GuiWthMode, 0);
    ESC_LOAD_FAM(kCldStyleKey, s_GuiWthStyle, 0);
    ESC_LOAD_FAM(kLightKey, s_GuiLight, 0);

    Esc_TextText("TextTitle", s_TextTitle, ESC_LABEL_CAP);
    ESC_LOAD_FAM(kTextKey, s_TextLabel, 1);
    ESC_LOAD_FAM(kPosKey, s_TextPos, 1);
    ESC_LOAD_FAM(kAlignKey, s_TextAlign, 1);
    ESC_LOAD_FAM(kBarKey, s_TextBar, 1);
    ESC_LOAD_FAM(kColorKey, s_TextColor, 1);
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

/* 行区起点 (标题 + 分隔线之后); 该页缺文案时再让开一条提示的高度。 */
static int Esc_ContentTop(const RECT* p, int missing)
{
    int y = p->top + ESC_PAD + Esc_TextHeight(ESC_TITLE_PX, ESC_TITLE_SLOT)
            + ESC_TITLE_GAP + ESC_SEP_GAP;

    return missing ? y + ESC_WARN_H + ESC_WARN_GAP : y;
}

static void Esc_IntToW(wchar_t* dst, unsigned value, int cap)
{
    char b[16];
    unsigned n = UtoA(value, b), i;
    if ((int)n > cap - 1) n = (unsigned)(cap - 1);
    for (i = 0; i < n; i++) dst[i] = (wchar_t)b[i];
    dst[n] = 0;
}

/* 点是否落在矩形内 (右/下开区间, 各处命中判定统一口径) */
static int EscIn(int x, int y, int w, int h, int mx, int my)
{
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

static void Esc_Redraw(HWND hDlg)
{
    if (!hDlg || !IsWindow(hDlg)) return;
    InvalidateRect(hDlg, NULL, FALSE);
    UpdateWindow(hDlg);
}

static int Esc_EditCurAt(const RECT* right, int mx, const wchar_t* s, int len,
                         int item);

static void Esc_EditBegin(HWND hDlg, const RECT* right, int mx, int page,
                          int item, int value, int minValue, int maxValue)
{
    int i;

    Esc_IntToW(s_EditBuf, (unsigned)value, ESC_EDIT_CAP);
    s_EditLen = lstrlenW(s_EditBuf);
    s_EditCur = Esc_EditCurAt(right, mx, s_EditBuf, s_EditLen, item);
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
    EG_SLIDE,       /* 拖动条: 轨道 + 数值框 */
    EG_LIGHTPARM,   /* 光照参数行: 一行四个小格 [标题][数值框] */
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
    { EG_SWITCH, GUI_ITEM_SHOT },
    { EG_GROUP,  GUI_GROUP_SIDEBAR },
    { EG_SWITCH, GUI_ITEM_SIDE_SCROLL },
    { EG_CHOICE, GUI_ITEM_SIDE_HIDE },
    { EG_GROUP,  GUI_GROUP_GAMECTRL },
    { EG_SWITCH, GUI_ITEM_GAMESPEED_ON },
    { EG_SLIDE,  GUI_ITEM_GAMESPEED },
    { EG_GROUP,  GUI_GROUP_LIGHT },
    { EG_CHOICE, GUI_ITEM_LIGHT_PRESET },
    { EG_LIGHTPARM, GUI_ITEM_LIGHT_PARM },
    { EG_COLOR,  GUI_ITEM_COLOR },
    { EG_SKIN,   GUI_ITEM_SKIN },
    { EG_GROUP,  GUI_GROUP_MISC },
    { EG_SWITCH, GUI_ITEM_CRATE },
    { EG_CHOICE, GUI_ITEM_WTH_MODE },
    { EG_SWITCH, GUI_ITEM_WTH_RAIN },
    { EG_SWITCH, GUI_ITEM_WTH_SNOW },
    { EG_SWITCH, GUI_ITEM_WTH_FIRE },
    { EG_SWITCH, GUI_ITEM_WTH_CLOUD },
    { EG_CHOICE, GUI_ITEM_WTH_STYLE },
    { EG_SWITCH, GUI_ITEM_FPS }
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

/* 一页的行表 + 全局行距参数; 绘制/命中/滚动都按它走。 */
typedef struct {
    const EscGuiRowInfo* rows;
    int n;
    int groupH;
    int gap;
    /* 组标题文案; NULL = 组标题就在行文案数组里 (文本页共用一个 enum) */
    const wchar_t (*groups)[ESC_LABEL_CAP];
    void (*draw)(void* surf, int rowL, int rowY, int rowW, int rh, int bodyH,
                 int kind, int item);
    int* scroll;       /* 本页的滚动位置 (两页各一个) */
    const int* missing;/* 本页缺文案计数 (内容区上移用) */
} EscPage;

static int Esc_RowH(const RECT* p, const EscGuiRowInfo* rows, int i, int groupH)
{
    int kind = rows[i].kind;

    if (kind == EG_GROUP) return groupH;
    if (kind == EG_SEP) return ESC_SEP_H;
    if (kind == EG_COLOR)
        return ESC_ROW_H + (Esc_ColorRows((p->right - p->left) - ESC_PAD * 2) - 1)
                            * (ESC_COLOR_SW + ESC_COLOR_GAP);
    return ESC_ROW_H;
}

/* 走一遍某页的行表: *total 写出总高, 返回 <= scroll 的最近行顶
 * (滚动吸附到行顶, 避免整行被上下裁剪)。两页共用 (设置页也会长高,
 * 所以滚动不能只给文本页写死)。 */
static int Esc_PageWalk(const RECT* p, const EscPage* pg, int scroll,
                        int* total)
{
    int i, y = 0, best = 0;

    for (i = 0; i < pg->n; i++)
    {
        if (pg->rows[i].kind == EG_GROUP && pg->rows[i].item != 0)
            y += pg->gap;
        if (y <= scroll) best = y;
        y += Esc_RowH(p, pg->rows, i, pg->groupH);
    }
    if (total) *total = y;
    return best;
}

static int Esc_PageTotalH(const RECT* p, const EscPage* pg)
{
    int total = 0;
    Esc_PageWalk(p, pg, -1, &total);
    return total;
}

static int Esc_PageSnap(const RECT* p, const EscPage* pg, int scroll)
{
    return Esc_PageWalk(p, pg, scroll, NULL);
}

/* 本页最大滚动量 (总高 - 可视高度)。 */
static int Esc_PageMaxScroll(const RECT* p, const EscPage* pg)
{
    int top = Esc_ContentTop(p, *pg->missing);
    int view = p->bottom - ESC_PAD - top;
    int max;

    if (view < 0) view = 0;
    max = Esc_PageTotalH(p, pg) - view;
    return max > 0 ? max : 0;
}

/* 把滚动位置夹到合法范围, 返回最大滚动量 (0 = 无需滚动)。 */
static int Esc_PageClampScroll(const RECT* p, const EscPage* pg)
{
    int max = Esc_PageMaxScroll(p, pg);
    int* sc = pg->scroll;

    if (*sc > max) *sc = Esc_PageSnap(p, pg, max);
    if (*sc < 0) *sc = 0;
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
    col = Blend16(0, r, g, b, 255);
    for (j = 0; j < h; j++)
    {
        unsigned short* row = (unsigned short*)(base + (size_t)(y + j) * pitch
                                                + (size_t)x * 2);
        if (a >= 255) for (i = 0; i < w; i++) row[i] = col;
        else          for (i = 0; i < w; i++) row[i] = Blend16(row[i], r, g, b, a);
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
    int y = Esc_ContentTop(p, 0);

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
    int x0, x1, y0, y1, radS, cyS, wS;

    if (!surf || w <= 0 || h <= 0 || a == 0) return;
    rad = h / 2;

    W = GtSurfGet(surf, GtVT_GET_WIDTH);
    H = GtSurfGet(surf, GtVT_GET_HEIGHT);
    pitch = GtSurfGet(surf, GtVT_GET_PITCH);
    if (W < 1 || H < 1 || pitch < W * 2) return;

    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > W ? W : x + w;
    y1 = y + h > H ? H : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    base = GtLock(surf);
    if (!base) return;

    /* 8x8 子采样求覆盖率: 中间直段整格算, 两端按到圆心距判 */
    radS = rad * ESC_AA_SCALE;
    wS = w * ESC_AA_SCALE;
    cyS = (h * ESC_AA_SCALE) / 2;
    for (py = y0; py < y1; py++)
    {
        unsigned short* row = (unsigned short*)(base + (size_t)py * pitch) + x0;
        for (px = x0; px < x1; px++)
        {
            int cov = 0;
            for (sy = 0; sy < ESC_AA_SS; sy++)
            {
                int dy = (py - y) * ESC_AA_SCALE + sy * 2 + 1 - cyS;
                for (sx = 0; sx < ESC_AA_SS; sx++)
                {
                    int dx = (px - x) * ESC_AA_SCALE + sx * 2 + 1;
                    if (dx < radS) dx -= radS;
                    else if (dx > wS - radS) dx -= wS - radS;
                    else { cov++; continue; }
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

    /* 圆把手 = 两端各半圆、直段为空的胶囊 (宽 = 高 = 2*半径) */
    knob = ESC_SW_H / 2 - 3;
    cx = on ? x + ESC_SW_W - ESC_SW_H / 2 : x + ESC_SW_H / 2;
    Esc_Capsule(surf, cx - knob, y + ESC_SW_H / 2 - knob, knob * 2, knob * 2,
                shade, shade, shade, 255);
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
    rowY = Esc_ContentTop(p, 0);
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

/* 一排等间距标签的版式: 右端对齐 right, 返回总宽。
 * 分段选择行 (Esc_DrawSegRow / Esc_SegmentAt) 与标题栏语言切换共用。 */
static int Esc_SegLayoutAt(wchar_t (*names)[32], int n, int right, int gap,
                           int* xs, int* ws)
{
    int i, total = 0, x;

    for (i = 0; i < n; i++)
    {
        ws[i] = Esc_TextWidth(names[i], ESC_BODY_PX, ESC_BODY_SLOT);
        total += ws[i] + (i ? gap : 0);
    }
    x = right - total;
    for (i = 0; i < n; i++)
    {
        xs[i] = x;
        x += ws[i] + gap;
    }
    return total;
}

static void Esc_SegLayout(wchar_t names[][32], int n,
                          int rowL, int rowW, int* xs, int* ws)
{
    Esc_SegLayoutAt(names, n, rowL + rowW - ESC_CTL_RIGHT, ESC_SKIN_GAP, xs, ws);
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

/* ---- 表驱动的行控件 ----
 * 三张表 (GUI 开关 / GUI 分段 / 文本页开关与分段) 把"取反 -> 写回 -> 排队落盘"
 * 这套重复动作收成一份; 新加一个开关只需加一行, 不用改 apply 的 switch。 */
static void Esc_QueueRaw(const char* section, const char* key, const char* value);
static void Esc_QueueValue(const char* section, const char* key, int value);

typedef struct {
    unsigned char item;
    const char* section;
    const char* key;
    int (*get)(void);
    void (*set)(int);
} EscToggle;

static const EscToggle s_Toggle[] = {
    { GUI_ITEM_SCALE,        "BottomBar", "ScaleEnable",  SB_Enabled,        SB_SetEnabled },
    { GUI_ITEM_PAUSE,        "BottomBar", "PauseEnable",  PV_ButtonsEnabled, PV_SetButtonsEnabled },
    { GUI_ITEM_GRID,         "BottomBar", "GridEnable",   GV_IsOn,           GV_SetOn },
    { GUI_ITEM_SHOT,         "BottomBar", "ShotEnable",   Shot_Visible,      Shot_SetVisible },
    { GUI_ITEM_FPS,          "FrameCap",  "Enable",       FrameCap_Enabled,  FrameCap_SetEnabled },
    { GUI_ITEM_SWS,          "SWSidebar", "Enable",       SWS_Enabled,       SWS_SetEnabled },
    { GUI_ITEM_SORT,         "SWSidebar", "FillMode",     SWS_FillMode,      SWS_SetFillMode },
    { GUI_ITEM_GAMESPEED_ON, "GameSpeed", "Enable",       GameSpeed_Enabled, GameSpeed_SetEnabled },
    { GUI_ITEM_BUILD_QUEUE,  "Features",  "BuildQueue",   BQ_Enabled,        BQ_SetEnabled },
    { GUI_ITEM_SIDE_SCROLL,  "Sidebar",   "ScrollEnable", SS_Enabled,        SS_SetEnabled },
    /* 天气: 雨/雪/火雨只在本次运行有效 (section=NULL), 云影是长期开关, 落盘。 */
    { GUI_ITEM_WTH_RAIN,     NULL, NULL, Weather_Rain, Weather_SetRain },
    { GUI_ITEM_WTH_SNOW,     NULL, NULL, Weather_Snow, Weather_SetSnow },
    { GUI_ITEM_WTH_FIRE,     NULL, NULL, Weather_Fire, Weather_SetFire },
    { GUI_ITEM_WTH_CLOUD,    "Weather", "CloudEnable", Weather_Cloud, Weather_SetCloud },
    { GUI_ITEM_CRATE,        "CrateIcon", "Enable",     CrateIcon_On,      CrateIcon_SetOn }
};

/* 行控件表的 item 都是首字段, 五张表的查找长得一样, 用宏生成。 */
#define ESC_FINDER(fn, T, tab)                                               \
    static const T* fn(int item)                                             \
    {                                                                        \
        int i;                                                               \
        for (i = 0; i < (int)(sizeof(tab) / sizeof((tab)[0])); i++)           \
            if ((tab)[i].item == item) return &(tab)[i];                      \
        return NULL;                                                         \
    }

ESC_FINDER(Esc_ToggleFind, EscToggle, s_Toggle)

/* 分段选择行: 显示值可能与底层开关反相 (右/左、下/上)。 */
typedef struct {
    unsigned char item;
    wchar_t (*names)[32];
    unsigned char n;
    unsigned char invert;
    int (*get)(void);
    void (*apply)(int pick);
} EscChoice;

static void EscChoiceScatter(int pick)
{
    PS_SetMode(pick);
    Esc_QueueValue("PlaceScatter", "Mode", pick);
}

static void EscChoiceHide(int pick)
{
    FM_SetHideState(pick);
    Esc_QueueValue("Sidebar", "HideEnable", pick ? 1 : 0);
    if (pick) Esc_QueueValue("Sidebar", "Mode", pick - 1);
}

static void EscChoiceSwsSide(int pick)
{
    SWS_SetSideRight(pick == 0);
    Esc_QueueRaw("SWSidebar", "Side", pick ? "left" : "right");
}

/* 天气画法: 0=图像 (引擎 SHP) 1=粒子。这是**长期设置**, 落盘到 [Weather] DrawMode;
 * 雨/雪/火雨各自的开关仍然只在本次运行有效 (不在配置里留"恒定天气")。 */
static void EscChoiceWthMode(int pick)
{
    Weather_SetDrawMode(pick);
    Esc_QueueRaw("Weather", "DrawMode", pick ? "particle" : "image");
}

/* 云影风格: 换一种云图预设。也是长期设置, 落盘到 [Weather] CloudStyle (写序号)。 */
static void EscChoiceWthStyle(int pick)
{
    Weather_SetCloudStyle(pick);
    Esc_QueueValue("Weather", "CloudStyle", pick);
}

/* 光照预设: 当前是"手动调过"时高亮最后一段 (自定义), 那一段不可选,
 * 只是状态显示。 */
static int EscChoiceLightGet(void)
{
    int p = Lighting_Preset();

    return (p < 0) ? LIGHT_PRESET_COUNT : p;
}

static void EscChoiceLightApply(int pick)
{
    if (pick >= LIGHT_PRESET_COUNT) return;
    Lighting_ApplyPreset(pick);
}

static const EscChoice s_Choice[] = {
    { GUI_ITEM_SCATTER,   s_GuiScatter, ESC_SCATTER_N,  0, PS_Mode,       EscChoiceScatter },
    { GUI_ITEM_SIDE_HIDE, s_GuiHide,    ESC_HIDE_N,     0, FM_HideState,  EscChoiceHide },
    { GUI_ITEM_SWS_SIDE,  s_GuiSwsSide, ESC_SWS_SIDE_N, 1, SWS_SideRight, EscChoiceSwsSide },
    { GUI_ITEM_LIGHT_PRESET, s_GuiLight, LIGHT_PRESET_COUNT + 1, 0,
      EscChoiceLightGet, EscChoiceLightApply },
    { GUI_ITEM_WTH_MODE,  s_GuiWthMode, WEATHER_DRAW_N, 0, Weather_DrawMode,
      EscChoiceWthMode },
    { GUI_ITEM_WTH_STYLE, s_GuiWthStyle, WEATHER_CLOUD_STYLE_N, 0,
      Weather_CloudStyle, EscChoiceWthStyle }
};

ESC_FINDER(Esc_ChoiceFind, EscChoice, s_Choice)

static int Esc_ChoiceValue(const EscChoice* c)
{
    return c->invert ? !c->get() : c->get();
}

typedef struct {
    unsigned char item;
    wchar_t (*names)[32];
    unsigned char n;
    unsigned char invert;
    unsigned char opt;      /* MSG_OPT_* */
    const char* key;        /* [MsgText] 键 */
} EscTextChoice;

static const EscTextChoice s_TChoice[] = {
    { TX_CAMP_POS,   s_TextPos,   2, 1, MSG_OPT_CAMP_BOTTOM,   "CampBottom" },
    { TX_CAMP_ALIGN, s_TextAlign, 2, 0, MSG_OPT_CAMP_CENTERED, "CampCentered" },
    { TX_CAMP_BAR,   s_TextBar,   3, 0, MSG_OPT_CAMP_BAR,      "CampBarSide" },
    { TX_HINT_COLOR, s_TextColor, 2, 0, MSG_OPT_HINT_COLOR,    "HintColorFollow" },
    { TX_HINT_BAR,   s_TextBar,   3, 0, MSG_OPT_HINT_BAR,      "HintBarSide" }
};

ESC_FINDER(Esc_TChoiceFind, EscTextChoice, s_TChoice)

static int Esc_TChoiceValue(const EscTextChoice* c)
{
    int v = MsgText_GetOption(c->opt);
    return c->invert ? !v : v;
}

/* 文本页开关: opt >= 0 走 MsgText 选项, 否则走自己的 get/set。 */
typedef struct {
    signed char item;
    signed char opt;
    int (*get)(void);
    void (*set)(int);
    const char* key;
} EscTextToggle;

static const EscTextToggle s_TToggle[] = {
    { TX_CAMP_ENABLE, MSG_OPT_CAMP_ENABLE, NULL, NULL, "CampEnable" },
    { TX_CAMP_ANIM,   MSG_OPT_CAMP_ANIM,   NULL, NULL, "CampAnim" },
    { TX_HINT_SHOW,   MSG_OPT_HINT_SHOW,   NULL, NULL, "HintShow" },
    { TX_HINT_RADAR,  MSG_OPT_HINT_RADAR,  NULL, NULL, "HintRadarShow" },
    { TX_CHAT_EDIT,   -1, ChatEdit_Enabled,    ChatEdit_SetEnabled,    "ChatEdit" },
    { TX_CHAT_NAME,   -1, ChatEdit_ShowNameSP, ChatEdit_SetShowNameSP, "ChatNameSP" }
};

ESC_FINDER(Esc_TToggleFind, EscTextToggle, s_TToggle)

static int Esc_TToggleVal(const EscTextToggle* t)
{
    return t->opt >= 0 ? MsgText_GetOption(t->opt) : t->get();
}

static void Esc_TTogglePut(const EscTextToggle* t, int v)
{
    if (t->opt >= 0) MsgText_SetOption(t->opt, v);
    else t->set(v);
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

ESC_FINDER(Esc_TextNumFind, EscTextNum, s_TextNum)

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

/* ---------------- 拖动条 (游戏速度) ----------------
 * 布局:  [标签]  [=====O=====]  [数值]
 * 数值框没有 +/- 按钮, 直接占满右侧控件区, 否则右边会空出加号那一块。 */
static void Esc_SlideNumBox(int rowRight, int* x, int* w)
{
    *x = Esc_RowCtlX(rowRight);
    *w = ESC_STEP_W;
}

static void Esc_SlideGeom(int rowL, int rowW, int* trkX, int* trkW,
                          int* numX, int* numW)
{
    int trkR, lblMin;

    Esc_SlideNumBox(rowL + rowW, numX, numW);
    trkR = *numX - ESC_SLIDE_GAP;
    lblMin = rowL + ESC_TEXT_INSET + 84;
    *trkX = trkR - ESC_SLIDE_W;
    if (*trkX < lblMin) *trkX = lblMin;
    *trkW = trkR - *trkX;
    if (*trkW < 1) *trkW = 1;
}

/* 鼠标横坐标 -> 速度值 (按 ESC_SPEED_STEP 吸附, 最右端单独一档 = MAX) */
static int Esc_SlideValueAt(int mx, int trkX, int trkW)
{
    int span = ESC_SPEED_TOP - ESC_SPEED_MIN;
    int rel, v;

    rel = mx - trkX;
    if (rel < 0) rel = 0;
    if (rel > trkW) rel = trkW;

    v = ESC_SPEED_MIN + (rel * span + trkW / 2) / trkW;
    if (v > ESC_SPEED_MAX) return ESC_SPEED_TOP;
    return ((v + ESC_SPEED_STEP / 2) / ESC_SPEED_STEP) * ESC_SPEED_STEP;
}

static int Esc_SlideXFor(int value, int trkX, int trkW)
{
    int span = ESC_SPEED_TOP - ESC_SPEED_MIN;

    return trkX + ((value - ESC_SPEED_MIN) * trkW + span / 2) / span;
}

/* ---------------- 光照参数行 ----------------
 * 一行四个小格: [标题][数值框]。没有 +/- 按钮 (点数值框直接输数字),
 * 数值框按三位数定宽。四个小格按各自标题的实际文字宽度排 (英文
 * "Ambient" 不会被截断), 格与格之间是固定间距, 整组贴行右端。 */
#define LIGHT_CAP_GAP    4       /* 小标题与数值框之间的缝 */
#define LIGHT_BOX_MIN    32      /* 数值框最小宽 (三位数 + 留白) */
#define LIGHT_BOX_PAD    12      /* 数值框左右留白 */
#define LIGHT_CELL_GAP   14      /* 两格之间的缝 */

static int Esc_LightItemIdx(int item)
{
    if (item < GUI_ITEM_LIGHT_AMBIENT || item > GUI_ITEM_LIGHT_BLUE) return -1;
    return item - GUI_ITEM_LIGHT_AMBIENT;
}

/* 四个格子的几何。每格 = [小标题][数值框], 组内紧凑, 整组贴行右端 ——
 * 与其它选项行 (标签在左、控件在右) 的版式一致。 */
typedef struct {
    int capX[LIGHT_PARAM_N];   /* 小标题 x */
    int capW[LIGHT_PARAM_N];   /* 小标题宽 (按实测文字宽度) */
    int boxX[LIGHT_PARAM_N];   /* 数值框 x */
    int boxW;                  /* 数值框宽 (四格相同) */
} LightGeom;

/* 数值框宽度: 恰好放三位数字 (上限 150), 不留空位。 */
static int Esc_LightBoxW(void)
{
    int w = Esc_TextWidth(L"000", ESC_BODY_PX, ESC_BODY_SLOT);

    if (w <= 0) w = 24;
    w += LIGHT_BOX_PAD;
    return (w > LIGHT_BOX_MIN) ? w : LIGHT_BOX_MIN;
}

static void Esc_LightGeom(int rowL, int rowW, LightGeom* g)
{
    int right = rowL + rowW - ESC_CTL_RIGHT;
    int total = 0, x, i;

    g->boxW = Esc_LightBoxW();
    for (i = 0; i < LIGHT_PARAM_N; i++)
    {
        /* 宽度按实测来, 不能用固定值 —— 固定值会把 "Ambient" 截成 "Amt"。 */
        g->capW[i] = Esc_TextWidth(s_GuiLabel[GUI_ITEM_LIGHT_AMBIENT + i],
                                   ESC_BODY_PX, ESC_BODY_SLOT);
        if (g->capW[i] <= 0) g->capW[i] = 16;
        total += g->capW[i] + LIGHT_CAP_GAP + g->boxW;
    }
    total += LIGHT_CELL_GAP * (LIGHT_PARAM_N - 1);

    x = right - total;
    if (x < rowL + 4) x = rowL + 4;
    for (i = 0; i < LIGHT_PARAM_N; i++)
    {
        g->capX[i] = x;
        g->boxX[i] = x + g->capW[i] + LIGHT_CAP_GAP;
        x += g->capW[i] + LIGHT_CAP_GAP + g->boxW + LIGHT_CELL_GAP;
    }
}

/* 某个数值框的几何: 拖动条没有 +/- 按钮, 框占满整个控件区 */
static void Esc_NumBoxFor(int item, const RECT* right, int* x, int* w)
{
    int idx = Esc_LightItemIdx(item);

    if (idx >= 0)
    {
        LightGeom g;

        Esc_LightGeom(right->left + ESC_PAD,
                      (right->right - right->left) - ESC_PAD * 2, &g);
        *x = g.boxX[idx];
        *w = g.boxW;
        return;
    }
    if (item == GUI_ITEM_GAMESPEED)
        Esc_SlideNumBox(right->right - ESC_PAD, x, w);
    else
        Esc_StepBox(Esc_CtlX(right), x, w);
}

static int Esc_StepMid(const RECT* right, int mx, int item)
{
    int x, w;

    Esc_NumBoxFor(item, right, &x, &w);
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
    return EscIn(r.left, r.top, ESC_CANCEL_W, ESC_CANCEL_H, mx, my);
}

static int Esc_EditCurAt(const RECT* right, int mx, const wchar_t* s, int len,
                         int item)
{
    wchar_t pre[ESC_EDIT_CAP];
    int boxX, boxW, numX, i, best = 0, bestDist = 0x7FFFFFFF;

    Esc_NumBoxFor(item, right, &boxX, &boxW);
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
    return Esc_SegLayoutAt(s_LangLabel, ESC_LANG_N,
                           p->right - ESC_PAD - ESC_TEXT_INSET, ESC_LANG_GAP, xs, ws);
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

/* 一排可选标签: 选中的提亮成金色并在 ulY 画下划线 (value<0 = 都没选中)。 */
static void Esc_DrawSegs(void* surf, const int* xs, const int* ws,
                         wchar_t (*names)[32], int n, int value, int ty, int ulY)
{
    int i, on;

    for (i = 0; i < n; i++)
    {
        on = (i == value);
        Esc_Text(surf, names[i], xs[i], ty, ws[i] + 2,
                 ESC_BODY_PX, ESC_BODY_SLOT,
                 on ? 245 : 145, on ? 225 : 145, on ? 170 : 145);
    }
    if (value >= 0 && value < n)
        Esc_Fill(surf, xs[value], ulY, ws[value], 2, 208, 160, 64, 255);
}

static void Esc_DrawLangTitle(void* surf, const RECT* p)
{
    int xs[ESC_LANG_N], ws[ESC_LANG_N];
    int mode = Lang_GetMode(), i, value = -1;
    int titleH = Esc_TextHeight(ESC_TITLE_PX, ESC_TITLE_SLOT);
    int bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    int ty = p->top + ESC_PAD + (titleH - bodyH) / 2;

    for (i = 0; i < ESC_LANG_N; i++)
        if (s_LangMode[i] == mode) value = i;
    Esc_LangLayout(p, xs, ws);
    Esc_DrawSegs(surf, xs, ws, s_LangLabel, ESC_LANG_N, value, ty, ty + bodyH + 2);
}

static void Esc_DrawSegRow(void* surf, int rowL, int rowY, int rowW,
                           int bodyH, const wchar_t* label,
                           wchar_t names[][32], int n, int value)
{
    int xs[ESC_SEG_MAX], ws[ESC_SEG_MAX];

    if (value < 0 || value >= n) value = 0;
    Esc_SegLayout(names, n, rowL, rowW, xs, ws);
    Esc_RowLabel(surf, rowL, rowY, xs[0], bodyH, label);
    Esc_DrawSegs(surf, xs, ws, names, n, value,
                 rowY + (ESC_ROW_H - bodyH) / 2, rowY + ESC_ROW_H - 6);
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

/* 数值框里的文字 + 编辑光标 (值行与拖动条共用) */
static void Esc_DrawNumText(void* surf, int boxX, int boxY, int boxW,
                            int textY, int value, int editing,
                            const wchar_t* text)
{
    wchar_t num[16];
    int numX;

    if (editing) lstrcpynW(num, s_EditBuf, 16);
    else if (text) lstrcpynW(num, text, 16);
    else Esc_IntToW(num, (unsigned)value, 16);

    numX = Esc_StepNumX(boxX, boxW, num);
    Esc_Text(surf, num, numX, textY, boxW, ESC_BODY_PX, ESC_BODY_SLOT,
             238, 238, 238);

    if (editing)
    {
        wchar_t pre[ESC_EDIT_CAP];
        int caretX;

        lstrcpynW(pre, num, s_EditCur + 1);
        caretX = numX + Esc_TextWidth(pre, ESC_BODY_PX, ESC_BODY_SLOT);
        if (caretX > boxX + boxW - 2) caretX = boxX + boxW - 2;
        Esc_Fill(surf, caretX, boxY + 3, 1, ESC_STEP_BTN_H - 6,
                 235, 235, 235, 255);
    }
}

/* 数值框: 底 + 边框 (编辑中换成金色) + 数字与光标。 */
static void Esc_DrawEditBox(void* surf, int x, int y, int w, int textY,
                            int value, int editing, const wchar_t* text)
{
    unsigned bg = editing ? 40u : 24u;
    unsigned r = editing ? 208u : 0x7Bu;
    unsigned g = editing ? 160u : 0x79u;
    unsigned b = editing ? 64u : 0x7Bu;
    unsigned a = editing ? 255u : 150u;

    Esc_Fill(surf, x, y, w, ESC_STEP_BTN_H, bg, bg, bg, 230);
    Esc_Outline(surf, x, y, w, ESC_STEP_BTN_H, r, g, b, a, 1);
    Esc_DrawNumText(surf, x, y, w, textY, value, editing, text);
}

/* 光照参数行: 环境 / 红 / 绿 / 蓝 四个格子, 数值单位 = 地图 ini 的 ×100。
 * 没有 +/- 按钮 —— 点数值框直接输数字 (三位数)。 */
static void Esc_DrawLightParamRow(void* surf, int rowL, int rowY, int rowW,
                                  int bodyH, const wchar_t* label)
{
    LightGeom g;
    int i, boxY;

    Esc_LightGeom(rowL, rowW, &g);
    Esc_RowLabel(surf, rowL, rowY, g.capX[0], bodyH, label);

    boxY = rowY + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;
    for (i = 0; i < LIGHT_PARAM_N; i++)
    {
        Esc_Text(surf, s_GuiLabel[GUI_ITEM_LIGHT_AMBIENT + i], g.capX[i],
                 rowY + (ESC_ROW_H - bodyH) / 2, g.capW[i],
                 ESC_BODY_PX, ESC_BODY_SLOT, 230, 230, 230);
        Esc_DrawEditBox(surf, g.boxX[i], boxY, g.boxW,
                        rowY + (ESC_ROW_H - bodyH) / 2,
                        Lighting_GetParam(i),
                        Esc_EditIs(EDIT_PAGE_GUI, GUI_ITEM_LIGHT_AMBIENT + i),
                        NULL);
    }
}

static void Esc_DrawValueRow(void* surf, int rowL, int rowY, int rowW,
                             int bodyH, const wchar_t* label, int value,
                             int minValue, int maxValue, int editing,
                             const wchar_t* text)
{
    int ctlX, btnY, boxX, boxW;

    ctlX = Esc_RowCtlX(rowL + rowW);
    btnY = rowY + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;
    Esc_RowLabel(surf, rowL, rowY, ctlX, bodyH, label);

    Esc_StepButton(surf, ctlX, btnY, L"-", value > minValue);
    Esc_StepButton(surf, ctlX + ESC_STEP_W - ESC_STEP_BTN_W, btnY, L"+",
                   value < maxValue);

    Esc_StepBox(ctlX, &boxX, &boxW);
    Esc_DrawEditBox(surf, boxX, btnY, boxW, rowY + (ESC_ROW_H - bodyH) / 2,
                    value, editing, text);
}

/* 拖动条行: 轨道 + 把手 + 数值框(可点进去直接输数字) */
static void Esc_DrawSlideRow(void* surf, int rowL, int rowY, int rowW,
                             int bodyH, const wchar_t* label, int value,
                             int editing, const wchar_t* text)
{
    int trkX, trkW, numX, numW;
    int knobX, ty, ky, btnY;

    Esc_SlideGeom(rowL, rowW, &trkX, &trkW, &numX, &numW);
    Esc_RowLabel(surf, rowL, rowY, trkX, bodyH, label);

    /* 轨道 + 已选中段 */
    ty = rowY + (ESC_ROW_H - ESC_SLIDE_TRK_H) / 2;
    if (trkW > 8)
    {
        Esc_Capsule(surf, trkX, ty, trkW, ESC_SLIDE_TRK_H, 44, 44, 44, 235);
        knobX = Esc_SlideXFor(value, trkX, trkW);
        if (knobX - trkX > 8)
            Esc_Capsule(surf, trkX, ty, knobX - trkX, ESC_SLIDE_TRK_H,
                        208, 160, 64, 235);
    }
    else
        knobX = trkX;

    /* 把手 */
    ky = rowY + (ESC_ROW_H - ESC_SLIDE_KNOB_H) / 2;
    Esc_Fill(surf, knobX - ESC_SLIDE_KNOB_W / 2, ky,
             ESC_SLIDE_KNOB_W, ESC_SLIDE_KNOB_H, 236, 236, 236, 255);
    Esc_Outline(surf, knobX - ESC_SLIDE_KNOB_W / 2, ky,
                ESC_SLIDE_KNOB_W, ESC_SLIDE_KNOB_H, 20, 20, 20, 220, 1);

    btnY = rowY + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;
    Esc_DrawEditBox(surf, numX, btnY, numW, rowY + (ESC_ROW_H - bodyH) / 2,
                    value, editing, text);
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

static void Esc_GroupRow(void* surf, int rowL, int rowW, int y, int rh,
                         int bodyH, const wchar_t* label)
{
    Esc_Fill(surf, rowL, y + (rh - ESC_BODY_PX) / 2, 3, ESC_BODY_PX,
             208, 160, 64, 255);
    Esc_Text(surf, label, rowL + ESC_TEXT_INSET, y + (rh - bodyH) / 2,
             rowW - ESC_TEXT_INSET - 6, ESC_BODY_PX, ESC_BODY_SLOT, 250, 220, 160);
}

/* 行底板: 颜色行比普通行高 (色块换行), 多出来的部分也要铺底, 再压一条行线。 */
static void Esc_RowPlate(void* surf, int rowL, int rowW, int y, int rh)
{
    Esc_RowBg(surf, rowL, y, rowW);
    if (rh > ESC_ROW_H)
        Esc_Fill(surf, rowL, y + ESC_ROW_H, rowW, rh - ESC_ROW_H, 32, 32, 32, 120);
    Esc_Fill(surf, rowL, y + rh - 1, rowW, 1, 0x7B, 0x79, 0x7B, 80);
}

/* 设置页一行的控件。开关项查 s_Toggle 拿当前值, 分段项查 s_Choice. */
static void Esc_DrawGuiRow(void* surf, int rowL, int rowY, int rowW, int rh,
                           int bodyH, int kind, int item)
{
    const EscToggle* t;
    const EscChoice* c;
    int cur;

    if (kind == EG_SWITCH)
    {
        t = Esc_ToggleFind(item);
        Esc_DrawSwitchRow(surf, rowL, rowY, rowW, bodyH, s_GuiLabel[item],
                          t ? t->get() : 0);
    }
    else if (kind == EG_SORT)
        Esc_DrawSortRow(surf, rowL, rowY, rowW, bodyH, s_GuiLabel[item]);
    else if (kind == EG_ROWS)
        Esc_DrawValueRow(surf, rowL, rowY, rowW, bodyH, s_GuiLabel[item],
                         SWS_MaxRows(), ESC_ROWS_MIN, ESC_ROWS_MAX,
                         Esc_EditIs(EDIT_PAGE_GUI, GUI_ITEM_ROWS), NULL);
    else if (kind == EG_SLIDE)
    {
        cur = GameSpeed_GetFps();
        Esc_DrawSlideRow(surf, rowL, rowY, rowW, bodyH, s_GuiLabel[item], cur,
                         Esc_EditIs(EDIT_PAGE_GUI, item),
                         cur > ESC_SPEED_MAX ? L"MAX" : NULL);
    }
    else if (kind == EG_LIGHTPARM)
        Esc_DrawLightParamRow(surf, rowL, rowY, rowW, bodyH, s_GuiLabel[item]);
    else if (kind == EG_COLOR)
        Esc_DrawColorRow(surf, rowL, rowY, rowW, bodyH, s_GuiLabel[item]);
    else if (kind == EG_SKIN)
        Esc_DrawSegRow(surf, rowL, rowY, rowW, bodyH, s_GuiLabel[item],
                       s_GuiSkinName, ESC_SKIN_N, SidebarSkin_Get());
    else if ((c = Esc_ChoiceFind(item)) != NULL)
        Esc_DrawSegRow(surf, rowL, rowY, rowW, bodyH, s_GuiLabel[item],
                       c->names, c->n, Esc_ChoiceValue(c));
}

/* 文本页一行的控件。 */
static void Esc_DrawTextRow(void* surf, int rowL, int rowY, int rowW, int rh,
                            int bodyH, int kind, int item)
{
    const EscTextNum* num;
    const EscTextChoice* c;
    const EscTextToggle* t;

    if (kind == EG_SWITCH)
    {
        t = Esc_TToggleFind(item);
        Esc_DrawSwitchRow(surf, rowL, rowY, rowW, bodyH, s_TextLabel[item],
                          t ? Esc_TToggleVal(t) : 0);
    }
    else if (kind == EG_VALUE)
    {
        num = Esc_TextNumFind(item);
        if (num)
            Esc_DrawValueRow(surf, rowL, rowY, rowW, bodyH, s_TextLabel[item],
                             MsgText_GetOption(num->opt), num->min, num->max,
                             Esc_EditIs(EDIT_PAGE_TEXT, item), NULL);
    }
    else if ((c = Esc_TChoiceFind(item)) != NULL)
        Esc_DrawSegRow(surf, rowL, rowY, rowW, bodyH, s_TextLabel[item],
                       c->names, c->n, Esc_TChoiceValue(c));
}

static const EscPage kGuiPage = { s_GuiLayout, ESC_GUI_DISPLAY, ESC_GUI_GROUP_H,
                                  ESC_GUI_GAP, s_GuiGroup, Esc_DrawGuiRow,
                                  &s_GuiScroll, &s_GuiMissing };
static const EscPage kTextPage = { s_TextLayout, TX_COUNT, ESC_GROUP_H,
                                   ESC_BLOCK_GAP, NULL, Esc_DrawTextRow,
                                   &s_TextScroll, &s_TextMissing };

/* 两页共用的行循环: 组标题 / 分隔条 / 行底板在这里, 控件交给 page->draw。
 * 滚出视野的行不画但仍占位 (两页都可滚, 设置页行数会随功能增加)。 */
static void Esc_RowsPanel(void* surf, RECT* p, const wchar_t* title,
                          const EscPage* pg, const wchar_t (*labels)[ESC_LABEL_CAP])
{
    int w, bodyH, rowL, rowW, i, y, rh, kind, item;
    int clipTop, clipBottom, maxScroll;

    if (!Esc_PanelFrame(surf, p, title)) return;

    w = p->right - p->left;
    bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    if (bodyH > ESC_ROW_H) bodyH = ESC_ROW_H;
    rowL = p->left + ESC_PAD;
    rowW = w - ESC_PAD * 2;
    clipTop = Esc_ContentTop(p, *pg->missing);
    clipBottom = p->bottom - ESC_PAD;
    maxScroll = Esc_PageClampScroll(p, pg);

    if (*pg->missing)
        Esc_DrawMissingNotice(surf, p, bodyH, rowL, rowW);

    y = clipTop - *pg->scroll;
    for (i = 0; i < pg->n; i++)
    {
        kind = pg->rows[i].kind;
        item = pg->rows[i].item;

        if (kind == EG_GROUP && item != 0) y += pg->gap;
        rh = Esc_RowH(p, pg->rows, i, pg->groupH);

        if (y < clipTop || y + rh > clipBottom) { y += rh; continue; }

        if (kind == EG_SEP)
            Esc_Fill(surf, rowL + ESC_TEXT_INSET, y + rh / 2,
                     rowW - ESC_TEXT_INSET * 2, 1, 0x7B, 0x79, 0x7B, 160);
        else if (kind == EG_GROUP)
            Esc_GroupRow(surf, rowL, rowW, y, rh, bodyH,
                         pg->groups ? pg->groups[item] : labels[item]);
        else
        {
            Esc_RowPlate(surf, rowL, rowW, y, rh);
            pg->draw(surf, rowL, y, rowW, rh, bodyH, kind, item);
        }
        y += rh;
    }

    if (maxScroll > 0)
    {
        int trackX = p->right - 6;
        int trackH = clipBottom - clipTop;
        int total = Esc_PageTotalH(p, pg);
        int thumbH, thumbY;

        if (total < 1) total = 1;
        thumbH = trackH * (trackH + maxScroll) / total;
        if (thumbH < 18) thumbH = 18;
        if (thumbH > trackH) thumbH = trackH;
        thumbY = clipTop;
        if (trackH > thumbH)
            thumbY += (trackH - thumbH) * *pg->scroll / maxScroll;
        Esc_Fill(surf, trackX, clipTop, 3, trackH, 0x7B, 0x79, 0x7B, 90);
        Esc_Fill(surf, trackX, thumbY, 3, thumbH, 208, 160, 64, 220);
    }
}

static void Esc_TextPanel(void* surf, RECT* p)
{
    Esc_RowsPanel(surf, p, s_TextTitle, &kTextPage, s_TextLabel);
}

static void Esc_SettingsPanel(void* surf, RECT* p)
{
    /* 菜单开着时主循环停摆, GameSpeed_Frame 不会被调; 在这里补一次同步,
     * 否则拖过原版速度页的滑块再回来, 这一行显示的还是旧值。 */
    GameSpeed_Poll();
    Esc_RowsPanel(surf, p, s_GuiTitle, &kGuiPage, s_GuiLabel);
}

static void Esc_DrawPanels(void* surf, HWND hDlg)
{
    EscRow leftRows[ESC_LEFT_ROWS];
    RECT a, b;
    int i;

    for (i = 0; i < ESC_LEFT_ROWS; i++)
    {
        leftRows[i].text = s_LeftItem[i];
        leftRows[i].selected = (s_Sel == i);
    }
    if (!Esc_Layout(hDlg, &a, &b)) return;

    Esc_Panel(surf, &a, s_LeftTitle, leftRows, ESC_LEFT_ROWS);
    Esc_DrawLangTitle(surf, &a);
    if (s_Sel == 0) Esc_SettingsPanel(surf, &b);
    else            Esc_TextPanel(surf, &b);
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
        DWORD ls = i, le, p;
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
        p = ls;
        while (p < le && (d[p] == ' ' || d[p] == '\t')) p++;
        if (p < le && d[p] == '[') { target = 0; continue; }
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

static unsigned Esc_PutEol(unsigned char* out, unsigned pos)
{
    out[pos++] = '\r';
    out[pos++] = '\n';
    return pos;
}

/* 写一行 `key=value` + 换行 */
static unsigned Esc_PutKeyVal(unsigned char* out, unsigned pos, const char* key,
                              const char* value)
{
    pos = Esc_PutText(out, pos, key);
    out[pos++] = '=';
    pos = Esc_PutText(out, pos, value);
    return Esc_PutEol(out, pos);
}

/* 原样改写 ini 的一行 (保留文件其余字节, 不改 ini 写入器的键序)。
 * Esc_IniLocate 给三种情况: 1=换值, 0=插到该节末尾, -1=连节都不存在。 */
static int Esc_WriteIniRaw(const char* section, const char* key,
                           const char* value)
{
    HANDLE h;
    DWORD size, got, put, vs = 0, ve = 0, insertAt = 0, tail;
    unsigned char* d = NULL;
    unsigned char* out = NULL;
    unsigned vlen, keyLen, secLen, pos;
    int loc, ok = 0;

    h = CreateFileA(g_IniPath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) goto done;
    d = (unsigned char*)LocalAlloc(LPTR, size + 1);
    if (!d || !ReadFile(h, d, size, &got, NULL) || got != size) goto done;

    vlen = (unsigned)lstrlenA(value);
    keyLen = (unsigned)lstrlenA(key);
    secLen = (unsigned)lstrlenA(section);

    loc = Esc_IniLocate(d, size, section, key, &vs, &ve, &insertAt);
    out = (unsigned char*)LocalAlloc(LPTR, size + keyLen + vlen + secLen + 16);
    if (!out) goto done;

    /* 三段只有"从哪里分叉"不同, 尾巴都是把 d 剩下的字节原样搬过去 */
    if (loc == 1)
    {
        pos = Esc_PutBytes(out, 0, d, vs);
        pos = Esc_PutText(out, pos, value);
        tail = ve;
    }
    else if (loc == 0)
    {
        pos = Esc_PutBytes(out, 0, d, insertAt);
        if (pos > 0 && out[pos - 1] != '\n') pos = Esc_PutEol(out, pos);
        pos = Esc_PutKeyVal(out, pos, key, value);
        tail = insertAt;
    }
    else
    {
        pos = Esc_PutBytes(out, 0, d, size);
        pos = Esc_PutEol(out, pos);
        out[pos++] = '[';
        pos = Esc_PutText(out, pos, section);
        out[pos++] = ']';
        pos = Esc_PutEol(out, pos);
        pos = Esc_PutKeyVal(out, pos, key, value);
        tail = size;
    }
    pos = Esc_PutBytes(out, pos, d + tail, size - tail);

    SetFilePointer(h, 0, NULL, FILE_BEGIN);
    ok = WriteFile(h, out, pos, &put, NULL) && put == pos;
    if (ok) SetEndOfFile(h);
done:
    if (out) LocalFree(out);
    if (d) LocalFree(d);
    CloseHandle(h);
    return ok;
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
        int idx = Esc_LightItemIdx(item);

        /* 光照参数: 临时设置, 不落盘 (换局/取消菜单后按地图原值重置)。 */
        if (idx >= 0)
        {
            Lighting_SetParam(idx, value);
            return;
        }
        if (item == GUI_ITEM_GAMESPEED)
        {
            GameSpeed_SetFps(value);
            Esc_QueueValue("GameSpeed", "FPS", GameSpeed_GetFps());
        }
        else
        {
            SWS_SetMaxRows(value);
            Esc_QueueValue("SWSidebar", "MaxRows", value);
        }
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
    else if (Esc_StepMid(right, mx, item))
        Esc_EditBegin(hDlg, right, mx, page, item, value, minValue, maxValue);
}

/* 快照打开设置页时的运行期值; 取消时恢复, INI 队列直接丢弃。 */
static void Esc_SaveSettings(void)
{
#define ESC_GET(n, g, s) s_Saved.n = g();
    ESC_SETTING_LIST(ESC_GET)
#undef ESC_GET
    s_Saved.color = PlayerColor_Current();
    Lighting_SaveState();
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
    Lighting_RestoreState();
    SidebarSkin_Poll();
    if (s_Saved.color >= 0) PlayerColor_Set(s_Saved.color);
}

static int Esc_HitLeft(const RECT* a, int mx, int my, int* index)
{
    int i, rowY, rowL = a->left + ESC_PAD;
    int rowW = (a->right - a->left) - ESC_PAD * 2;

    for (i = 0, rowY = Esc_ContentTop(a, 0); i < ESC_LEFT_ROWS; i++, rowY += ESC_ROW_H)
    {
        if (rowY + ESC_ROW_H > a->bottom - ESC_PAD) break;
        if (EscIn(rowL, rowY, rowW, ESC_ROW_H, mx, my)) { *index = i; return 1; }
    }
    return 0;
}

/* 行表命中: 鼠标落在哪一行 (返回行表下标, *rowY 写出该行顶)。
 * clipTop 与 top 分开传: top 是滚过之后的起点, 可见性仍按 clipTop 判。 */
static int Esc_RowAt(const RECT* b, const EscPage* pg, int top, int clipTop,
                     int mx, int my, int* row, int* rowY)
{
    int i, y, rh, kind;
    int rowL = b->left + ESC_PAD;
    int rowW = (b->right - b->left) - ESC_PAD * 2;
    int clipBottom = b->bottom - ESC_PAD;

    for (i = 0, y = top; i < pg->n; i++)
    {
        kind = pg->rows[i].kind;
        if (kind == EG_GROUP && pg->rows[i].item != 0) y += pg->gap;
        rh = Esc_RowH(b, pg->rows, i, pg->groupH);
        if (y < clipTop || y + rh > clipBottom) { y += rh; continue; }
        if (EscIn(rowL, y, rowW, rh, mx, my))
        {
            *row = i;
            if (rowY) *rowY = y;
            return 1;
        }
        y += rh;
    }
    return 0;
}

/* 行内控件的精确命中: 只有真点在控件上才算 (names 非空 = 这一行是分段选择)。 */
static int Esc_CtlHit(int kind, int rowL, int rowW, int rowY, int rh,
                      int mx, int my, wchar_t (*names)[32], int n)
{
    if (kind == EG_GROUP || kind == EG_SEP) return 0;
    if (kind == EG_SWITCH)
    {
        int sx = rowL + rowW - ESC_CTL_RIGHT - ESC_SW_W;
        return EscIn(sx, rowY + (rh - ESC_SW_H) / 2, ESC_SW_W, ESC_SW_H, mx, my);
    }
    if (kind == EG_VALUE)
    {
        int ctlX = Esc_RowCtlX(rowL + rowW);
        return mx >= ctlX && mx < ctlX + ESC_STEP_W;
    }
    if (names) return Esc_SegmentAt(names, n, rowL, rowW, mx) >= 0;
    if (kind == EG_SLIDE)
    {
        int trkX, trkW, numX, numW;
        Esc_SlideGeom(rowL, rowW, &trkX, &trkW, &numX, &numW);
        return mx >= trkX && mx < numX + numW;
    }
    if (kind == EG_COLOR)
        return Esc_ColorSegmentAt(rowL, rowW, rowY, mx, my) >= 0;
    return 1;
}

static int Esc_HitRight(const RECT* b, int mx, int my, int* index, int* rowTop)
{
    const EscChoice* c;
    int row, rowY = 0, kind, rowL, rowW, top, clipTop;

    if (s_Sel != 0) return 0;
    rowL = b->left + ESC_PAD;
    rowW = (b->right - b->left) - ESC_PAD * 2;
    clipTop = Esc_ContentTop(b, s_GuiMissing);
    top = clipTop - s_GuiScroll;
    if (!Esc_RowAt(b, &kGuiPage, top, clipTop, mx, my, &row, &rowY)) return 0;

    kind = s_GuiLayout[row].kind;
    c = Esc_ChoiceFind(s_GuiLayout[row].item);
    if (!Esc_CtlHit(kind, rowL, rowW, rowY,
                    Esc_RowH(b, s_GuiLayout, row, ESC_GUI_GROUP_H), mx, my,
                    kind == EG_SKIN ? s_GuiSkinName : (c ? c->names : NULL),
                    kind == EG_SKIN ? ESC_SKIN_N : (c ? c->n : 0)))
        return 0;
    *index = s_GuiLayout[row].item;
    if (rowTop) *rowTop = rowY;
    return 1;
}

static void Esc_ApplyGui(HWND hDlg, const RECT* right, int index, int mx,
                         int my, int rowTop)
{
    const EscToggle* t = Esc_ToggleFind(index);
    const EscChoice* c = Esc_ChoiceFind(index);
    int value, rowL, rowW, pick;

    /* 纯开关 (s_Toggle) 与分段选择 (s_Choice) 都在表里, 这里只剩特例。 */
    if (t)
    {
        value = t->get() ? 0 : 1;
        t->set(value);
        if (t->section) Esc_QueueValue(t->section, t->key, value);
        Esc_Redraw(hDlg);
        return;
    }
    if (c)
    {
        pick = Esc_PickSeg(c->names, c->n, right, mx);
        if (pick < 0) return;
        value = c->invert ? !pick : pick;
        if (value == c->get()) return;
        c->apply(pick);
        Esc_Redraw(hDlg);
        return;
    }

    switch (index)
    {
    case GUI_ITEM_ROWS:
        Esc_StepOrEdit(hDlg, right, mx, EDIT_PAGE_GUI, GUI_ITEM_ROWS,
                       SWS_MaxRows(), ESC_ROWS_MIN, ESC_ROWS_MAX, 1);
        return;
    case GUI_ITEM_GAMESPEED:
    {
        int trkX, trkW, numX, numW;
        int cur = GameSpeed_GetFps();

        rowL = right->left + ESC_PAD;
        rowW = (right->right - right->left) - ESC_PAD * 2;
        Esc_SlideGeom(rowL, rowW, &trkX, &trkW, &numX, &numW);
        if (mx >= numX)          /* 点数值框: 直接输数字 (可输非 5 的倍数) */
        {
            Esc_EditBegin(hDlg, right, mx, EDIT_PAGE_GUI, GUI_ITEM_GAMESPEED,
                          cur, ESC_SPEED_MIN, ESC_SPEED_TOP);
            return;
        }
        /* 点轨道: 跳到该点并进入拖动 */
        Esc_ApplyNum(EDIT_PAGE_GUI, GUI_ITEM_GAMESPEED,
                     Esc_SlideValueAt(mx, trkX, trkW));
        s_SlideDrag = 1;
        if (hDlg) SetCapture(hDlg);
        break;
    }
    case GUI_ITEM_SKIN:
        pick = Esc_PickSeg(s_GuiSkinName, ESC_SKIN_N, right, mx);
        if (pick < 0 || pick == SidebarSkin_Get()) return;
        SidebarSkin_Set(pick);
        SidebarSkin_Poll();
        Esc_QueueValue("Sidebar", "SkinSide", pick);
        break;
    case GUI_ITEM_COLOR:
        rowL = right->left + ESC_PAD;
        rowW = (right->right - right->left) - ESC_PAD * 2;
        pick = Esc_ColorSegmentAt(rowL, rowW, rowTop, mx, my);
        if (pick < 0 || pick == PlayerColor_Current()) return;
        if (!PlayerColor_Set(pick)) return;
        break;
    case GUI_ITEM_LIGHT_PARM:
    {
        LightGeom g;
        int i;

        Esc_LightGeom(right->left + ESC_PAD,
                      (right->right - right->left) - ESC_PAD * 2, &g);
        for (i = 0; i < LIGHT_PARAM_N; i++)
        {
            if (mx >= g.boxX[i] && mx < g.boxX[i] + g.boxW)
            {
                Esc_EditBegin(hDlg, right, mx, EDIT_PAGE_GUI,
                              GUI_ITEM_LIGHT_AMBIENT + i,
                              Lighting_GetParam(i), 0, LIGHT_PARAM_MAX);
                return;
            }
        }
        return;
    }
    default:
        return;
    }
    Esc_Redraw(hDlg);
}

static int Esc_TextHitRight(const RECT* b, int mx, int my, int* index)
{
    const EscTextChoice* c;
    int row, rowY = 0, clipTop;

    if (s_Sel != 1) return 0;
    Esc_PageClampScroll(b, &kTextPage);
    clipTop = Esc_ContentTop(b, s_TextMissing);
    if (!Esc_RowAt(b, &kTextPage, clipTop - s_TextScroll, clipTop, mx, my,
                   &row, &rowY))
        return 0;

    c = Esc_TChoiceFind(s_TextLayout[row].item);
    if (!Esc_CtlHit(s_TextLayout[row].kind, b->left + ESC_PAD,
                    (b->right - b->left) - ESC_PAD * 2, rowY,
                    Esc_RowH(b, s_TextLayout, row, ESC_GROUP_H), mx, my,
                    c ? c->names : NULL, c ? c->n : 0))
        return 0;
    *index = s_TextLayout[row].item;
    return 1;
}

/* 文本页的点击处理: 开关/分段/数值三类, 全靠 s_TToggle / s_TChoice / s_TextNum。 */
static void Esc_TextApply(HWND hDlg, const RECT* right, int index, int mx)
{
    const EscTextToggle* t = Esc_TToggleFind(index);
    const EscTextChoice* c = Esc_TChoiceFind(index);
    const EscTextNum* num = Esc_TextNumFind(index);
    int value, pick;

    if (t)
    {
        value = Esc_TToggleVal(t) ? 0 : 1;
        Esc_TTogglePut(t, value);
        Esc_QueueValue("MsgText", t->key, value);
    }
    else if (c)
    {
        pick = Esc_PickSeg(c->names, c->n, right, mx);
        if (pick < 0) return;
        value = c->invert ? !pick : pick;
        MsgText_SetOption(c->opt, value);
        Esc_QueueValue("MsgText", c->key, value);
    }
    else if (num)
    {
        Esc_StepOrEdit(hDlg, right, mx, EDIT_PAGE_TEXT, index,
                       MsgText_GetOption(num->opt), num->min, num->max, num->step);
        return;
    }
    else return;
    Esc_Redraw(hDlg);
}

static void Esc_EditEnd(HWND hDlg)
{
    if (!s_EditOn) return;
    if (s_EditHost) KillTimer(s_EditHost, ESC_EDIT_TIMER);
    s_EditOn = 0;
    Esc_Redraw(hDlg);
}

/* 当前编辑框里的数值 (非数字字符按 0 算)。 */
static int Esc_EditValue(void)
{
    int v = 0, i;

    for (i = 0; i < s_EditLen; i++)
        v = v * 10 + (s_EditBuf[i] - L'0');
    return v;
}

/* 上下键 = ±1 (数值框没有 +/- 按钮时的微调手段)。 */
static void Esc_EditStep(int delta)
{
    int v = Esc_EditValue() + delta;

    if (v < s_EditMin) v = s_EditMin;
    if (v > s_EditMax) v = s_EditMax;
    Esc_IntToW(s_EditBuf, (unsigned)v, ESC_EDIT_CAP);
    s_EditLen = lstrlenW(s_EditBuf);
    s_EditCur = s_EditLen;
}

static void Esc_EditCommit(HWND hDlg)
{
    int value;

    if (!s_EditOn) return;
    value = Esc_EditValue();
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
        if (k->vk == VK_UP) { Esc_EditStep(1); }
        else if (k->vk == VK_DOWN) { Esc_EditStep(-1); }
        else if (k->vk == VK_BACK)
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
    /* 离开哪页就把哪页的滚动位置复位 (两页各一个)。 */
    if (index != 1) s_TextScroll = 0;
    if (index != 0) s_GuiScroll = 0;
    Esc_Redraw(hDlg);
}

/* 滚轮: 滚当前页 (设置页也可能长高, 所以两页都滚)。 */
static int Esc_PageWheel(HWND h, WPARAM wp, LPARAM lp)
{
    POINT pt;
    RECT a, b;
    const EscPage* pg;
    int delta, old;

    pt.x = (int)(short)LOWORD(lp);
    pt.y = (int)(short)HIWORD(lp);
    if (!ScreenToClient(h, &pt)) return 0;
    if (!Esc_Layout(h, &a, &b)) return 0;
    if (!EscIn(b.left, b.top, b.right - b.left, b.bottom - b.top, pt.x, pt.y))
        return 0;

    delta = (int)(short)HIWORD(wp);
    if (delta == 0) return 0;
    pg = (s_Sel == 0) ? &kGuiPage : &kTextPage;
    old = *pg->scroll;
    *pg->scroll = Esc_PageSnap(&b, pg, *pg->scroll - (delta / 120) * ESC_ROW_H * 3);
    Esc_PageClampScroll(&b, pg);
    if (*pg->scroll != old) Esc_Redraw(h);
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

    if (!s_EditOn || !Esc_StepMid(right, mx, s_EditItem)) return 0;
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

    s_EditCur = Esc_EditCurAt(right, mx, s_EditBuf, s_EditLen, s_EditItem);
    Esc_Redraw(hDlg);
    return 1;
}

static void Esc_Cancel(HWND hDlg)
{
    HWND back;

    if (GetCapture() == hDlg) ReleaseCapture();
    s_CancelDown = 0;
    s_SlideDrag = 0;
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
    int mx = (int)(short)LOWORD(lp);
    int my = (int)(short)HIWORD(lp);
    int index = -1, rowY = 0;
    RECT a, b;
    LRESULT r;

    /* 面板自绘只对首页 (s_Host) 做; 其余消息一律原样交给原过程。 */
    if (h == s_Host) switch (msg)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (!Esc_Layout(h, &a, &b)) { if (s_EditOn) Esc_EditCommit(h); break; }
        if (Esc_CancelHit(&b, mx, my))
        {
            s_CancelDown = 1;
            SetCapture(h);
            Esc_Redraw(h);
            return 0;
        }
        if (Esc_EditClick(h, &b, mx, my)) return 0;
        if (s_EditOn) Esc_EditCommit(h);
        index = Esc_LangHit(&a, mx, my);
        if (index >= 0) { Esc_ApplyLang(h, index); return 0; }
        if (Esc_HitLeft(&a, mx, my, &index)) { Esc_Select(h, index); return 0; }
        if (Esc_HitRight(&b, mx, my, &index, &rowY))
        { Esc_ApplyGui(h, &b, index, mx, my, rowY); return 0; }
        if (Esc_TextHitRight(&b, mx, my, &index))
        { Esc_TextApply(h, &b, index, mx); return 0; }
        break;
    case WM_MOUSEMOVE:
        if (!s_SlideDrag || s_Sel != 0) break;      /* 按住拖动速度条 */
        if (Esc_Layout(h, &a, &b))
        {
            int rowL = b.left + ESC_PAD;
            int rowW = (b.right - b.left) - ESC_PAD * 2;
            int trkX, trkW, numX, numW, v;

            Esc_SlideGeom(rowL, rowW, &trkX, &trkW, &numX, &numW);
            v = Esc_SlideValueAt(mx, trkX, trkW);
            if (v != GameSpeed_GetFps())
            {
                Esc_ApplyNum(EDIT_PAGE_GUI, GUI_ITEM_GAMESPEED, v);
                Esc_Redraw(h);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (s_SlideDrag)
        {
            s_SlideDrag = 0;
            if (GetCapture() == h) ReleaseCapture();
            return 0;
        }
        if (!s_CancelDown) break;
        if (Esc_Layout(h, &a, &b) && Esc_CancelHit(&b, mx, my))
            Esc_Cancel(h);
        else
        {
            s_CancelDown = 0;
            if (GetCapture() == h) ReleaseCapture();
            Esc_Redraw(h);
        }
        return 0;
    case WM_CAPTURECHANGED:
        s_SlideDrag = 0;
        if (s_CancelDown) { s_CancelDown = 0; Esc_Redraw(h); }
        break;
    case WM_TIMER:
        if (wp == ESC_EDIT_TIMER) { Esc_EditPoll(h); return 0; }
        break;
    case WM_KILLFOCUS:
        if (s_EditOn) Esc_EditCommit(h);
        break;
    case WM_MOUSEWHEEL:
        if (Esc_PageWheel(h, wp, lp)) return 0;
        break;
    case WM_SHOWWINDOW:
        /* 隐藏 ESC 菜单 = 离开设置页, 统一落盘。 */
        if (wp) break;
        s_CancelDown = 0;
        s_SlideDrag = 0;
        if (GetCapture() == h) ReleaseCapture();
        Esc_EditCommit(h);
        Esc_FlushPending();
        Esc_SaveSettings();
        break;
    default:
        break;
    }

    r = Esc_CallOld(h, old, msg, wp, lp);

    if (msg == WM_NCDESTROY)
    {
        RemovePropW(h, ESC_PROP_OLD_PROC);
        /* 子页销毁时回退到首页 (首页可能还开着), 否则菜单会被误判为关闭 */
        if (FrameCap_ActiveDlg() == (void*)(size_t)h)
            FrameCap_SetActiveDlg((s_Host && s_Host != h && IsWindow(s_Host))
                                  ? (void*)s_Host : NULL);
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

    /* 所有 ESC 对话框 (首页 + 各级子页) 都要做两件事:
     *   1) 告诉限帧模块"菜单开着" —— 只有这时才限速, 菜单关掉立刻放行
     *   2) 子类化 —— 面板自绘/编辑框轮询
     * 面板自绘只对首页 (s_Host) 做。 */
    FrameCap_SetActiveDlg(hDlg);
    Esc_Subclass(hDlg);

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
