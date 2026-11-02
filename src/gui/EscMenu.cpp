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
#include "SaveShot.h"
#include "PlayerColor.h"
#include "SidebarSkin.h"
#include "FrameCap.h"
#include "GameSpeed.h"
#include "LightingPreset.h"
#include "OreLight.h"
#include "CrateIcon.h"
#include "EnvIni.h"

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
/* 左栏只有两行页名, 让位优先级最低: 右侧内容栏差多少先从它身上扣。 */
#define ESC_LEFT_MIN         176
#define ESC_PANEL_GAP        10
#define ESC_VMARGIN          24

#define ESC_TITLE_SLOT       10
#define ESC_BODY_SLOT        11
#define ESC_FONT_FACE        L"Microsoft YaHei"

#define ESC_TITLE_PX         18
#define ESC_BODY_PX          16
#define ESC_PAD              8
#define ESC_TEXT_INSET       6
#define ESC_LBL_GAP          8    /* 行标题与控件之间的最小空隙 */
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
#define ESC_GUI_DISPLAY      34
#define ESC_SEP_H            14
#define ESC_SKIN_N           5
#define ESC_STYLE_PER        4      /* 云影风格每行段数 (8 个预设 = 两行) */
#define ESC_SCATTER_N        3
#define ESC_HIDE_N           4
#define ESC_SWS_SIDE_N       2
#define ESC_COLOR_SW         14
#define ESC_COLOR_GAP        4
#define ESC_COLOR_LABEL_W    128
#define ESC_LANG_N           3
#define ESC_LANG_GAP         14

#define ESC_SW_W             44
#define ESC_SW_H             20
#define ESC_SORT_GAP         14      /* 侧栏排序行两段之间的缝 */
#define ESC_FLOW_MAX         12      /* 一行最多几个控件原子 (分段最多 8 段) */
/* 开/关状态列按实测字宽 (见 Esc_StateW), 不再留固定 64px: 中英文都是
 * 两三个字符, 定宽等于白扔四十来像素给行标题。 */
#define ESC_STATE_GAP        8
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
    /* 光照: 预设行 + 参数两行 + 调色行 (拖动条 + 两端快捷段 + 数值显示) +
     * 矿石光照行。调色行/矿石光控行无数值输入, 数值只显示。参数分两行:
     * 第一行 环境/等级/地面, 第二行 红/绿/蓝。每个格子各有自己的 item
     * (数值编辑框按 item 记状态), 行 item 只当这一行的标识 (第二行没有
     * 行标题)。 */
    GUI_ITEM_LIGHT_PRESET,
    GUI_ITEM_LIGHT_MODE,
    GUI_ITEM_LIGHT_PARM,
    GUI_ITEM_LIGHT_AMBIENT,
    GUI_ITEM_LIGHT_LEVEL,
    GUI_ITEM_LIGHT_GROUND,
    GUI_ITEM_LIGHT_RGB,
    GUI_ITEM_LIGHT_RED,
    GUI_ITEM_LIGHT_GREEN,
    GUI_ITEM_LIGHT_BLUE,
    GUI_ITEM_ORELIGHT,      /* 矿石/宝石光照比例 ([Environment] OreFactor) */
    GUI_ITEM_WTH_MODE,
    GUI_ITEM_WTH_TRIO,      /* 天气一行: 雨/雪/火雨/雷电 四个独立开关 */
    GUI_ITEM_WTH_RAIN,
    GUI_ITEM_WTH_SNOW,
    GUI_ITEM_WTH_FIRE,
    GUI_ITEM_WTH_THUNDER,   /* 雷电 (闪电+打雷), 与雨/雪/火雨同一行的第四段 */
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

/* 菜单里的一段短文案 (段名 / 开关名 / 页名), 32 个 wchar 够放。 */
typedef wchar_t EscName[32];

static HWND s_Host = NULL;
static wchar_t s_LeftTitle[ESC_LABEL_CAP];
static wchar_t s_LeftItem[ESC_LEFT_ROWS][ESC_LABEL_CAP];
static wchar_t s_GuiTitle[ESC_LABEL_CAP];
static wchar_t s_GuiGroup[GUI_GROUP_COUNT][ESC_LABEL_CAP];
static wchar_t s_GuiLabel[GUI_ITEM_COUNT][ESC_LABEL_CAP];
static wchar_t s_GuiOn[16];
static wchar_t s_GuiOff[16];
static EscName s_GuiFaction;
static EscName s_GuiFill;
static EscName s_GuiSkinName[ESC_SKIN_N];
static EscName s_GuiScatter[ESC_SCATTER_N];
static EscName s_GuiHide[ESC_HIDE_N];
static EscName s_GuiSwsSide[ESC_SWS_SIDE_N];
static EscName s_GuiWthMode[WEATHER_DRAW_N];
static EscName s_GuiWthStyle[WEATHER_CLOUD_STYLE_N];
/* 光照预设分段: 原图 + 标准/早上/下午/黄昏/晚上 + 自定义 (自定义只是状态)。 */
static EscName s_GuiLight[LIGHT_PRESET_COUNT + 1];
/* 调色行两端快捷段: 左 默认 (比例 0) / 右 触发 (比例满值)。 */
static EscName s_GuiLightMode[2];
/* 矿石光照行两端快捷段: 左 默认 (0, 夜里全亮) / 右 环境 (满值, 跟随环境光)。 */
static EscName s_GuiOreEnds[2];

static wchar_t s_TextTitle[ESC_LABEL_CAP];
static wchar_t s_TextLabel[TX_COUNT][ESC_LABEL_CAP];
static EscName s_TextPos[2];
static EscName s_TextAlign[2];
static EscName s_TextBar[3];
static EscName s_TextColor[2];
static EscName s_Cancel = L"Cancel";
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
    ES_KEY(VK_OEM_MINUS, 0), ES_KEY(VK_SUBTRACT, 0),
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
static int s_EditMin = 0;
static int s_EditMax = 0;
static int s_EditMaxLen = 1;
static wchar_t s_EditBuf[ESC_EDIT_CAP];
static LiEdit s_EditSt;   /* 光标/选区 (buf = s_EditBuf, Esc_EditBegin 接上) */

static int Esc_EditIs(int page, int item)
{
    return s_EditOn && s_EditPage == page && s_EditItem == item;
}

typedef struct {
    int scale, pause, grid, shot, sws, fill, rows, skin, color;
    int bq, scatter, sideScroll, hide, swsSide;
    int chatEdit, chatName, speed, speedOn, ore;
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
    X(ore, OreLight_Factor, OreLight_SetFactor) \
    X(wthMode, Weather_DrawMode, Weather_SetDrawMode) \
    X(wthRain, Weather_Rain, Weather_SetRain) \
    X(wthSnow, Weather_Snow, Weather_SetSnow) \
    X(wthFire, Weather_Fire, Weather_SetFire) \
    X(wthCloud, Weather_Cloud, Weather_SetCloud) \
    X(wthStyle, Weather_CloudStyle, Weather_SetCloudStyle)

static EscSavedSettings s_Saved;
static int s_SavedOk = 0;
static int s_CancelDown = 0;
static int s_SlideDrag = -1;  /* 拖动条按住中的 item, -1 = 无 */
static int s_SlideVal;        /* 拖动中的待生效值 (调色比例松开才提交) */

static EscName s_LangLabel[ESC_LANG_N] = { L"ZH", L"EN", L"AUTO" };
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
 * NULL = 这一项没有文案 (光照参数第二行没有标题, 文本页有两个分隔条)。 */
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
    "GuiLightPreset", "GuiLightMode", "GuiLightParm", "GuiLightAmbient",
    "GuiLightLevel", "GuiLightGround", NULL, "GuiLightRed",
    "GuiLightGreen", "GuiLightBlue", "GuiOreLight", "GuiWthMode", "GuiWthTrio",
    "GuiWthRain", "GuiWthSnow", "GuiWthFire", "GuiWthThunder", "GuiWthCloud",
    "GuiWthStyle",
    "GuiCrate"
};
static const char* const kLightKey[LIGHT_PRESET_COUNT + 1] = {
    "GuiLightOrig", "GuiLightStandard", "GuiLightMorning", "GuiLightAfternoon",
    "GuiLightDusk", "GuiLightNight", "GuiLightCustom"
};
/* 调色行两端快捷段 (左 默认 / 右 触发) */
static const char* const kLightModeKey[2] = {
    "GuiLightModePerCell", "GuiLightModeUniform"
};
/* 矿石光照行两端快捷段: 左 = 原版默认, 右 = 完全跟随环境光 (与
 * GuiLightAmbient 的"环境"同键。en 侧 Default/Ambient 同样两行通用)。 */
static const char* const kOreEndKey[2] = {
    "GuiLightModePerCell", "GuiLightAmbient"
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
    "GuiWthStyle4", "GuiWthStyle5", "GuiWthStyle6", "GuiWthStyle7"
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
    ESC_LOAD_FAM(kLightModeKey, s_GuiLightMode, 0);
    ESC_LOAD_FAM(kOreEndKey, s_GuiOreEnds, 0);

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
    /* 右栏是内容栏 (设置项都在这儿), 宽度先保它: 差多少全从左栏扣, 左栏扣到
     * ESC_LEFT_MIN 为止, 剩下的差额才由右栏承担 (那时行内控件开始换行,
     * 见 Esc_RowLines)。反过来"先把右栏压到 240 再动左栏"在 1024x768 就
     * 白白让右栏少了 70px, 而左栏只有两行页名。 */
    if (lw + ESC_PANEL_GAP + rw > avail)
    {
        int over = lw + ESC_PANEL_GAP + rw - avail;
        int room = lw - ESC_LEFT_MIN;

        lw -= (over < room) ? over : room;
        rw = avail - lw - ESC_PANEL_GAP;
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

/* 左栏最底下那一条是语言切换: 页名列表到这条线为止。绘制与命中共用。 */
static int Esc_LangY(const RECT* p)
{
    return p->bottom - ESC_PAD - ESC_ROW_H;
}

static void Esc_IntToW(wchar_t* dst, unsigned value, int cap)
{
    char b[16];
    unsigned n = UtoA(value, b), i;
    if ((int)n > cap - 1) n = (unsigned)(cap - 1);
    for (i = 0; i < n; i++) dst[i] = (wchar_t)b[i];
    dst[n] = 0;
}

/* 有符号版 (光照等级可以是负数)。 */
static void Esc_SignedToW(wchar_t* dst, int value, int cap)
{
    if (value >= 0) { Esc_IntToW(dst, (unsigned)value, cap); return; }
    dst[0] = L'-';
    Esc_IntToW(dst + 1, (unsigned)(-value), cap - 1);
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

    /* 先 init (会清缓冲) 再填数字, 顺序反了数字会被 LiEdInit 清掉 */
    LiEdInit(&s_EditSt, s_EditBuf, ESC_EDIT_CAP);
    Esc_SignedToW(s_EditBuf, value, ESC_EDIT_CAP);
    s_EditSt.len = lstrlenW(s_EditBuf);
    s_EditSt.cur = Esc_EditCurAt(right, mx, s_EditBuf, s_EditSt.len, item);
    s_EditOn = 1;
    s_EditHost = hDlg;
    s_EditPage = page;
    s_EditItem = item;
    s_EditMin = minValue;
    s_EditMax = maxValue;
    s_EditMaxLen = 1;
    for (i = (maxValue > -minValue) ? maxValue : -minValue; i >= 10; i /= 10)
        s_EditMaxLen++;
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
    EG_SLIDE,       /* 拖动条: 轨道 + 数值框; 调色行右端还有 "比例" 小标题 */
    EG_LIGHTPARM,   /* 光照参数行: 一行三个小格 [标题][数值框] */
    EG_TRIO,        /* 天气行: 一行四个独立开关 (雨/雪/火雨/雷电) */
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
    { EG_LIGHTPARM, GUI_ITEM_LIGHT_RGB },
    { EG_SLIDE,  GUI_ITEM_LIGHT_MODE },
    { EG_SLIDE,  GUI_ITEM_ORELIGHT },
    { EG_CHOICE, GUI_ITEM_WTH_MODE },
    { EG_TRIO,   GUI_ITEM_WTH_TRIO },
    { EG_SWITCH, GUI_ITEM_WTH_CLOUD },
    { EG_CHOICE, GUI_ITEM_WTH_STYLE },
    { EG_GROUP,  GUI_GROUP_MISC },
    { EG_COLOR,  GUI_ITEM_COLOR },
    { EG_SKIN,   GUI_ITEM_SKIN },
    { EG_SWITCH, GUI_ITEM_CRATE },
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

/* 分段行的段名与段数。两页各一套表, 而且 TX_* 与 GUI_ITEM_* 编号是重叠的,
 * 所以必须由页自己给, 不能按 item 猜是哪张表。 */
typedef struct {
    EscName* names;
    int n;
} EscSegs;

/* 一页的行表 + 全局行距参数; 绘制/命中/滚动都按它走。 */
typedef struct {
    const EscGuiRowInfo* rows;
    int n;
    int groupH;
    int gap;
    /* 组标题文案; NULL = 组标题就在行文案数组里 (文本页共用一个 enum) */
    const wchar_t (*groups)[ESC_LABEL_CAP];
    /* 行文案: 行高要按标题实测宽度定 (换行判定), 所以页里得带着它 */
    const wchar_t (*labels)[ESC_LABEL_CAP];
    /* 分段行取段名: 两页各一套表 (见 EscSegs) */
    EscSegs (*segs)(int item);
    void (*draw)(void* surf, int rowL, int rowY, int rowW, int bodyH,
                 int kind, int item);
    int* scroll;       /* 本页的滚动位置 (两页各一个) */
    const int* missing;/* 本页缺文案计数 (内容区上移用) */
} EscPage;

/* ---------------- 控件流: 行内自动换行 ----------------
 * 一行 = [行标题][控件原子...]。原子 (一段文字 / 一个数值格 / 一个开关) 的宽度
 * 各不相同, 由 Esc_RowAtoms 按实测字宽给出, 位置由 Esc_FlowPlace 统一排布。
 * 行高、绘制、命中都读这一份坐标, 所以加一种行类型只需在 Esc_RowAtoms 里给它
 * 原子宽度, 不许哪一行自己判断放不放得下 (那样每行行为都会不一样)。 */

typedef struct {
    int n;                          /* 原子数 */
    int lines;                      /* 占几行 (>= 1) */
    int x0line;                     /* 第 0 行第一个原子的 x; 0 = 该行没有原子 */
    int w[ESC_FLOW_MAX];            /* 原子宽度 */
    int x[ESC_FLOW_MAX];            /* 原子左缘 */
    int ln[ESC_FLOW_MAX];           /* 原子所在行 (0 = 与行标题同排) */
} EscFlow;

/* 定义在绘制区之后 (要用分段/光照那一堆实测宽度), 行高这边先问它。 */
static void Esc_RowFlow(int kind, int item, int rowL, int rowW,
                        const wchar_t* label, EscSegs sg, EscFlow* f);
/* 设置页的段名表 (定义在行控件区, 数值框几何要按同一份流算)。 */
static EscSegs EscGuiSegs(int item);
/* 行标题的绘制口径 (见 Esc_FlowLabel): 控件流的定位要给标题让位。 */
static void Esc_FlowLabel(void* surf, const EscFlow* f, int rowL, int rowW,
                          int rowY, int bodyH, const wchar_t* label, int seg);

/* 行高 = ESC_ROW_H × 控件流的行数。色块栅格自己往下排 (Esc_ColorRows), 一排
 * 色块比一行文字矮, 单独算。 */
static int Esc_RowH(const RECT* p, const EscPage* pg, int i)
{
    int kind = pg->rows[i].kind;
    int item = pg->rows[i].item;
    int rowW = (p->right - p->left) - ESC_PAD * 2;
    EscFlow f;
    EscSegs sg;

    if (kind == EG_GROUP) return pg->groupH;
    if (kind == EG_SEP) return ESC_SEP_H;
    if (kind == EG_COLOR)
        return ESC_ROW_H + (Esc_ColorRows(rowW) - 1) * (ESC_COLOR_SW + ESC_COLOR_GAP);

    sg.names = NULL;
    sg.n = 0;
    if (pg->segs) sg = pg->segs(item);

    Esc_RowFlow(kind, item, p->left + ESC_PAD, rowW, pg->labels[item], sg, &f);
    return ESC_ROW_H * (f.lines > 1 ? f.lines : 1);
}

/* 走一遍某页的行表: *total 写出总高, *maxRowH 写出最高的一行 (折行行可达
 * ESC_ROW_H 的整数倍; 贴底判据要用它, 用 ESC_ROW_H 会漏判)。返回 <= scroll 的
 * 最近行顶 (滚动吸附到行顶, 避免整行被上下裁剪)。两页共用 (设置页也会长高,
 * 所以滚动不能只给文本页写死)。 */
static int Esc_PageWalk(const RECT* p, const EscPage* pg, int scroll,
                        int* total, int* maxRowH)
{
    int i, y = 0, best = 0, rhMax = 0;

    for (i = 0; i < pg->n; i++)
    {
        int rh;

        if (pg->rows[i].kind == EG_GROUP && pg->rows[i].item != 0)
            y += pg->gap;
        if (y <= scroll) best = y;
        rh = Esc_RowH(p, pg, i);
        if (rh > rhMax) rhMax = rh;
        y += rh;
    }
    if (total) *total = y;
    if (maxRowH) *maxRowH = rhMax;
    return best;
}

static int Esc_PageTotalH(const RECT* p, const EscPage* pg)
{
    int total = 0;
    Esc_PageWalk(p, pg, -1, &total, NULL);
    return total;
}

static int Esc_PageSnap(const RECT* p, const EscPage* pg, int scroll)
{
    return Esc_PageWalk(p, pg, scroll, NULL, NULL);
}

/* 本页最大滚动量 (总高 - 可视高度)。 */
static int Esc_PageMaxScroll(const RECT* p, const EscPage* pg, int* maxRowH)
{
    int top = Esc_ContentTop(p, *pg->missing);
    int view = p->bottom - ESC_PAD - top;
    int total = 0, rhMax = 0, max;

    if (view < 0) view = 0;
    Esc_PageWalk(p, pg, -1, &total, &rhMax);
    if (maxRowH) *maxRowH = rhMax;
    max = total - view;
    return max > 0 ? max : 0;
}

/* 把滚动位置夹到合法范围, 返回最大滚动量 (0 = 无需滚动)。
 * 越界一律直接贴底, **不吸附行顶**: 底部位置一般不是行顶的整数倍, 吸附会少滚
 * 一截; 而折行行 (两行高) 的那一截正好等于一整行时, 光靠"距底不足一行"判不出
 * 来, 于是最后那一两行永远露不出来。贴底阈值也按本页最高行算, 同理。
 * 溢出不出一行 (max 比最高行还小) 时吸附只会把位置按回 0, 退化成逐像素滚。 */
static int Esc_PageClampScroll(const RECT* p, const EscPage* pg)
{
    int rhMax = 0;
    int max = Esc_PageMaxScroll(p, pg, &rhMax);
    int* sc = pg->scroll;

    if (max <= 0)
    {
        *sc = 0;
        return 0;
    }
    if (max < rhMax)
    {
        if (*sc > max) *sc = max;
        if (*sc < 0) *sc = 0;
        return max;
    }
    if (*sc > max) *sc = max;
    if (*sc > 0 && max - *sc < rhMax) *sc = max;
    if (*sc < 0) *sc = 0;
    return max;
}

/* ---------------- 16-bit BSurface drawing ---------------- */

/* 行区裁剪: 滚动时首尾行只露半行, 所有绘制统一裁到面板行区内。
 * s_ClipBot <= s_ClipTop = 不裁 (只在行循环期间打开)。 */
static int s_ClipTop, s_ClipBot;

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
    if (s_ClipBot > s_ClipTop)
    {
        if (*y < s_ClipTop) { *h -= s_ClipTop - *y; *y = s_ClipTop; }
        if (*y + *h > s_ClipBot) *h = s_ClipBot - *y;
    }
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
    int adv2 = 0, w, h, W, H, pitch, X, Y, Y0 = 0, xx, yy;

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
    if (s_ClipBot > s_ClipTop)
    {
        /* 裁掉的行从覆盖图对应位置开始读 (Y0), 不能只缩 h。 */
        if (y < s_ClipTop) { Y0 = s_ClipTop - y; h -= Y0; y = s_ClipTop; }
        if (y + h > s_ClipBot) h = s_ClipBot - y;
        if (h <= 0) return;
    }

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
                    + (size_t)((Y0 + Y) * ESC_TEXT_SS + yy) * g_G->pitch
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

/* 开关行的"开/关"状态列宽: 取两种状态里较宽的那个, 各行的列就还是对齐的。
 * 中英文都只有两三个字符, 按实测给比固定值省下四十来像素 (留给行标题)。 */
static int Esc_StateW(void)
{
    int a = Esc_TextWidth(s_GuiOn, ESC_BODY_PX, ESC_BODY_SLOT);
    int b = Esc_TextWidth(s_GuiOff, ESC_BODY_PX, ESC_BODY_SLOT);

    if (a < b) a = b;
    return a > 0 ? a : 24;
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
    int labelW = right - ESC_LBL_GAP - (rowL + ESC_TEXT_INSET);
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
    if (s_ClipBot > s_ClipTop)
    {
        /* 开关是胶囊画的 (不走 Esc_Fill), 裁剪得自己再来一遍 */
        if (y0 < s_ClipTop) y0 = s_ClipTop;
        if (y1 > s_ClipBot) y1 = s_ClipBot;
    }
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
        /* 底部那条是语言切换, 页名不许压上去 */
        if (rowY + ESC_ROW_H > Esc_LangY(p) - ESC_SEP_GAP) break;

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
    /* 天气: 雨/雪/火雨/雷电只在本次运行有效 (section=NULL), 云影是长期开关, 落盘。 */
    { GUI_ITEM_WTH_RAIN,     NULL, NULL, Weather_Rain, Weather_SetRain },
    { GUI_ITEM_WTH_SNOW,     NULL, NULL, Weather_Snow, Weather_SetSnow },
    { GUI_ITEM_WTH_FIRE,     NULL, NULL, Weather_Fire, Weather_SetFire },
    { GUI_ITEM_WTH_THUNDER,  NULL, NULL, Weather_Thunder, Weather_SetThunder },
    { GUI_ITEM_WTH_CLOUD,    ENV_SECTION, ENV_KEY_CLOUD, Weather_Cloud, Weather_SetCloud },
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

/* 开关的点击动作: 取反 -> 写回 -> 需要落盘的排队。 */
static void Esc_ToggleFlip(const EscToggle* t)
{
    int value = t->get() ? 0 : 1;

    t->set(value);
    if (t->section) Esc_QueueValue(t->section, t->key, value);
}

/* 分段选择行: 显示值可能与底层开关反相 (右/左、下/上)。 */
typedef struct {
    unsigned char item;
    EscName* names;
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

/* 天气画法: 0=图像 (引擎 SHP) 1=粒子。这是**长期设置**, 落盘到 [Environment] DrawMode;
 * 雨/雪/火雨各自的开关仍然只在本次运行有效 (不在配置里留"恒定天气")。 */
static void EscChoiceWthMode(int pick)
{
    Weather_SetDrawMode(pick);
    Esc_QueueRaw(ENV_SECTION, ENV_KEY_DRAWMODE, pick ? "particle" : "image");
}

/* 云影风格: 换一种云图预设。也是长期设置, 落盘到 [Environment] CloudStyle (写序号)。 */
static void EscChoiceWthStyle(int pick)
{
    Weather_SetCloudStyle(pick);
    Esc_QueueValue(ENV_SECTION, ENV_KEY_CLOUDSTYLE, pick);
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
    EscName* names;
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

/* 一个色块的位置: 每行列数按行宽算, 行内竖直居中 (色块比行矮)。绘制与命中
 * 共用这一份, 不然点到的和看到的会差几像素。 */
static void Esc_ColorCellRect(int i, int rowL, int rowW, int rowY,
                              int* px, int* py)
{
    int step = ESC_COLOR_SW + ESC_COLOR_GAP;
    int per = Esc_ColorPerRow(rowW);

    *px = rowL + ESC_COLOR_LABEL_W + (i % per) * step;
    *py = rowY + (ESC_ROW_H - ESC_COLOR_SW) / 2 + (i / per) * step;
}

static int Esc_ColorSegmentAt(int rowL, int rowW, int rowY, int mx, int my)
{
    int i, x, y, count = PlayerColor_Count();

    for (i = 0; i < count; i++)
    {
        Esc_ColorCellRect(i, rowL, rowW, rowY, &x, &y);
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
 * 布局:  [标签]  [=====O=====]  [- 数值 +]
 * 轨道右缘贴控件区左缘 (留滑条间距), 数值框与普通数值行同宽 (两侧
 * +/- 按钮), 点按钮一次 +-5, 点框直接输数字。
 * 轨道左端按标题实测宽度让位 (与端点行同一口径), 英文长标题不会被截。 */
static void Esc_SlideGeom(int rowL, int rowW, const wchar_t* label,
                          int* trkX, int* trkW, int* numX, int* numW)
{
    int trkR, lblMin;

    Esc_StepBox(Esc_RowCtlX(rowL + rowW), numX, numW);
    trkR = Esc_RowCtlX(rowL + rowW) - ESC_SLIDE_GAP;
    lblMin = rowL + ESC_TEXT_INSET
             + Esc_TextWidth(label, ESC_BODY_PX, ESC_BODY_SLOT) + ESC_LBL_GAP;
    *trkX = trkR - ESC_SLIDE_W;
    if (*trkX < lblMin) *trkX = lblMin;
    *trkW = trkR - *trkX;
    if (*trkW < 1) *trkW = 1;
}

/* 前向声明: 速度条的吸附映射要先用连续映射。 */
static int Esc_TrackValueAt(int mx, int trkX, int trkW, int minv, int maxv);

/* 鼠标横坐标 -> 速度值 (在连续映射上按 ESC_SPEED_STEP 吸附, 最右端单独一档 = MAX) */
static int Esc_SlideValueAt(int mx, int trkX, int trkW)
{
    int v = Esc_TrackValueAt(mx, trkX, trkW, ESC_SPEED_MIN, ESC_SPEED_TOP);

    if (v > ESC_SPEED_MAX) return ESC_SPEED_TOP;
    return ((v + ESC_SPEED_STEP / 2) / ESC_SPEED_STEP) * ESC_SPEED_STEP;
}

/* 连续拖动条 (调色比例 / 矿石光照) 的映射, 不吸附; 速度条在其上吸附。 */
static int Esc_TrackValueAt(int mx, int trkX, int trkW, int minv, int maxv)
{
    int rel, span = maxv - minv;

    rel = mx - trkX;
    if (rel < 0) rel = 0;
    if (rel > trkW) rel = trkW;
    return minv + (rel * span + trkW / 2) / trkW;
}

static int Esc_TrackXFor(int value, int trkX, int trkW, int minv, int maxv)
{
    return trkX + ((value - minv) * trkW + (maxv - minv) / 2) / (maxv - minv);
}

/* 带 +/- 的数值行 (游戏速度 / 调色比例 / 矿石光照 / 侧栏行数): 范围、步进、
 * 取值与落盘都在一张表里。落盘写回的是 set 之后 get 到的值 (set 会夹取),
 * section = NULL 表示只改内存。 */
typedef struct {
    unsigned char item;
    int min, max, step;
    int (*get)(void);
    void (*set)(int);
    const char* section;
    const char* key;
} EscNum;

static const EscNum s_Num[] = {
    { GUI_ITEM_GAMESPEED,  ESC_SPEED_MIN, ESC_SPEED_TOP,       ESC_SPEED_STEP,
      GameSpeed_GetFps, GameSpeed_SetFps, "GameSpeed", "FPS" },
    { GUI_ITEM_LIGHT_MODE, 0,             LIGHT_MIX_MAX,       1,
      Lighting_Mix, Lighting_SetMix, ENV_SECTION, ENV_KEY_LIGHTMIX },
    { GUI_ITEM_ORELIGHT,   0,             ORELIGHT_FACTOR_MAX, 1,
      OreLight_Factor, OreLight_SetFactor, ENV_SECTION, ENV_KEY_OREFACTOR },
    { GUI_ITEM_ROWS,       ESC_ROWS_MIN,  ESC_ROWS_MAX,        1,
      SWS_MaxRows, SWS_SetMaxRows, "SWSidebar", "MaxRows" }
};

ESC_FINDER(Esc_NumFind, EscNum, s_Num)

/* 拖动条行 (速度 / 调色比例 / 矿石光照) 的规格: min / max / 步进与当前值。 */
static int Esc_SlideSpec(int item, int* minv, int* maxv, int* step)
{
    const EscNum* n = Esc_NumFind(item);

    *minv = n ? n->min : 0;
    *maxv = n ? n->max : 0;
    *step = n ? n->step : 1;
    return n ? n->get() : 0;
}

/* 端点行 (调色比例 / 矿石光照) 的两端快捷段文案, NULL = 不是端点行 (游戏速度)。 */
static EscName* Esc_ItemEnds(int item)
{
    if (item == GUI_ITEM_LIGHT_MODE) return s_GuiLightMode;
    if (item == GUI_ITEM_ORELIGHT) return s_GuiOreEnds;
    return NULL;
}

/* ---------------- 光照参数行 ----------------
 * 两行各三个小格: [标题][数值框]。第一行 环境/等级/地面, 第二行 红/绿/蓝
 * (第二行没有行标题, 两行之间也不画分隔线, 视觉上是一组)。
 * 没有 +/- 按钮 (点数值框直接输数字), 数值框按三位数定宽。每行内的小格按各自
 * 标题的实际文字宽度排, 整组贴行右端 —— 与其它选项行 (标签在左、控件在右) 同版式。 */
#define LIGHT_CAP_GAP    4       /* 小标题与数值框之间的缝 */
#define LIGHT_BOX_MIN    32      /* 数值框最小宽 (三位数 + 留白) */
#define LIGHT_BOX_PAD    12      /* 数值框左右留白 */
#define LIGHT_CELL_GAP   14      /* 两格之间的缝 */
#define LIGHT_ROW_CELLS  3       /* 每行三格 */

/* 参数号 -> 菜单 item (顺序 = 显示顺序, 与 LIGHT_P_* 一致) */
static const int s_LightItem[LIGHT_PARAM_N] = {
    GUI_ITEM_LIGHT_AMBIENT, GUI_ITEM_LIGHT_LEVEL, GUI_ITEM_LIGHT_GROUND,
    GUI_ITEM_LIGHT_RED, GUI_ITEM_LIGHT_GREEN, GUI_ITEM_LIGHT_BLUE
};

static int Esc_LightItemIdx(int item)
{
    int i;

    for (i = 0; i < LIGHT_PARAM_N; i++)
        if (s_LightItem[i] == item) return i;
    return -1;
}

/* 下限: 等级可以为负 (高度对亮度的影响反过来), 其余分量负值没有意义。 */
static int Esc_LightMin(int param)
{
    return (param == LIGHT_P_LEVEL) ? -LIGHT_PARAM_MAX : 0;
}

/* 行 item (标题所在行) -> 这一行第一个参数号; -1 = 不是参数行。 */
static int Esc_LightRowFirst(int item)
{
    if (item == GUI_ITEM_LIGHT_PARM) return 0;
    if (item == GUI_ITEM_LIGHT_RGB) return LIGHT_ROW_CELLS;
    return -1;
}

/* 数值框宽度: 恰好放三位数字 (等级可为负, 按 "-000" 算), 不留空位。 */
static int Esc_LightBoxW(void)
{
    int w = Esc_TextWidth(L"-000", ESC_BODY_PX, ESC_BODY_SLOT);

    if (w <= 0) w = 24;
    w += LIGHT_BOX_PAD;
    return (w > LIGHT_BOX_MIN) ? w : LIGHT_BOX_MIN;
}

/* 光照参数一格的几何: 原子框里 [小标题][缝][数值框], 数值框贴原子右端。
 * 绘制/命中/滚轮三处共用, 口径不能各算各的。 */
static void Esc_LightCell(const EscFlow* f, int i, int* capW, int* boxX)
{
    int boxW = Esc_LightBoxW();

    *capW = f->w[i] - LIGHT_CAP_GAP - boxW;
    *boxX = f->x[i] + *capW + LIGHT_CAP_GAP;
}

/* 光照参数行: 光标落在哪一格的数值框上 (返回参数号, -1 = 没落在格上)。
 * 点编辑与滚轮微调共用, 格子位置与绘制同出一源。 */
static int Esc_LightCellAt(const RECT* b, int item, int rowTop, int mx, int my)
{
    EscFlow f;
    int rowL = b->left + ESC_PAD;
    int rowW = (b->right - b->left) - ESC_PAD * 2;
    int first = Esc_LightRowFirst(item);
    int boxW = Esc_LightBoxW();
    int i;

    if (first < 0) return -1;
    Esc_RowFlow(EG_LIGHTPARM, item, rowL, rowW, s_GuiLabel[item],
                EscGuiSegs(item), &f);
    for (i = 0; i < f.n && i < LIGHT_ROW_CELLS; i++)
    {
        int capW, boxX;

        Esc_LightCell(&f, i, &capW, &boxX);
        if (f.ln[i] != (my - rowTop) / ESC_ROW_H) continue;
        if (mx >= boxX && mx < boxX + boxW) break;
    }
    return (i >= LIGHT_ROW_CELLS || i >= f.n) ? -1 : first + i;
}

/* 端点行几何 (调色比例 / 矿石光照): 右端 [数值], 往左 [右段][轨道][左段],
 * 行标题在最左。两端段是 0 / 满值 的快捷跳值; 行标题按实测宽度让位,
 * 不够放时轨道左端右移。 */
static void Esc_EndBarGeom(int rowL, int rowW, const wchar_t* label,
                           EscName* names, int* trkX, int* trkW,
                           int xs[], int ws[], int* valX, int* valW)
{
    int right = rowL + rowW - ESC_CTL_RIGHT;
    int lblMin, x, i;

    *valW = Esc_TextWidth(L"100", ESC_BODY_PX, ESC_BODY_SLOT);
    if (*valW <= 0) *valW = 24;
    *valW += LIGHT_BOX_PAD;
    if (*valW < LIGHT_BOX_MIN) *valW = LIGHT_BOX_MIN;
    *valX = right - *valW;

    for (i = 0; i < 2; i++)     /* 右段从右往左摆, 再放轨道, 再放左段 */
    {
        /* 段宽 = 纯文字宽 (横线贴文字, 与其它分段行同一口径); 段与轨道、
         * 段与数值框之间都留 ESC_SLIDE_GAP。 */
        ws[i] = Esc_TextWidth(names[i], ESC_BODY_PX, ESC_BODY_SLOT);
        if (ws[i] <= 0) ws[i] = 16;
    }
    x = *valX - ESC_SLIDE_GAP - ws[1];
    x -= ESC_SLIDE_GAP + ESC_SLIDE_W;   /* 轨道左端理想位置 */
    xs[0] = x - ESC_SLIDE_GAP - ws[0];

    lblMin = rowL + ESC_TEXT_INSET;
    lblMin += Esc_TextWidth(label, ESC_BODY_PX, ESC_BODY_SLOT) + 8;
    if (xs[0] < lblMin) xs[0] = lblMin;
    xs[1] = *valX - ESC_SLIDE_GAP - ws[1];
    *trkX = xs[0] + ws[0] + ESC_SLIDE_GAP;
    *trkW = xs[1] - ESC_SLIDE_GAP - *trkX;
    if (*trkW < 1) *trkW = 1;
}

/* 某个数值框的几何: 光照格按控件流算, 其余行用 [+/- 数值框] 的统一版式
 * (游戏速度与普通数值行同布局)。 */
static void Esc_NumBoxFor(int item, const RECT* right, int* x, int* w)
{
    int idx = Esc_LightItemIdx(item);

    if (idx >= 0)
    {
        int first = (idx / LIGHT_ROW_CELLS) * LIGHT_ROW_CELLS;
        int rowItem = (first == 0) ? GUI_ITEM_LIGHT_PARM : GUI_ITEM_LIGHT_RGB;
        int cell = idx - first;
        int rowL = right->left + ESC_PAD;
        int rowW = (right->right - right->left) - ESC_PAD * 2;
        int boxW = Esc_LightBoxW();
        EscFlow f;

        Esc_RowFlow(EG_LIGHTPARM, rowItem, rowL, rowW, s_GuiLabel[rowItem],
                    EscGuiSegs(rowItem), &f);
        *w = boxW;
        *x = Esc_CtlX(right);           /* 兜底: 行比流短时别越界 */
        if (cell < f.n)
            *x = f.x[cell] + (f.w[cell] - LIGHT_CAP_GAP - boxW) + LIGHT_CAP_GAP;
        return;
    }
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
    int boxX, boxW, numX, i, acc = 0, best = 0, bestDist = 0x7FFFFFFF;

    Esc_NumBoxFor(item, right, &boxX, &boxW);
    numX = Esc_StepNumX(boxX, boxW, s);
    /* 插入点 i 的横坐标 = 文本起点 + 前 i 个字符的宽度 (逐字符累加;
     * 等宽数字下与整串渲染逐位一致)。 */
    for (i = 0; i <= len; i++)
    {
        int d = mx - (numX + acc);
        if (d < 0) d = -d;
        if (d < bestDist) { bestDist = d; best = i; }
        if (i < len)
        {
            wchar_t one[2];

            one[0] = s[i];
            one[1] = 0;
            acc += Esc_TextWidth(one, ESC_BODY_PX, ESC_BODY_SLOT);
        }
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

static void Esc_DrawSwitchRow(void* surf, const EscFlow* f, int rowL, int rowW,
                              int rowY, int bodyH, const wchar_t* label, int on)
{
    const wchar_t* state = on ? s_GuiOn : s_GuiOff;
    int stateW = Esc_StateW();
    int tw, y;

    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 0);

    /* 原子 0 = 开/关状态 (在原子框里右对齐), 原子 1 = 胶囊开关 */
    y = rowY + f->ln[0] * ESC_ROW_H;
    tw = Esc_TextWidth(state, ESC_BODY_PX, ESC_BODY_SLOT);
    if (tw > stateW) tw = stateW;
    Esc_Text(surf, state, f->x[0] + stateW - tw, y + (ESC_ROW_H - bodyH) / 2,
             tw, ESC_BODY_PX, ESC_BODY_SLOT,
             on ? 222 : 150, on ? 170 : 150, on ? 222 : 150);
    Esc_Switch(surf, f->x[1],
               rowY + f->ln[1] * ESC_ROW_H + (ESC_ROW_H - ESC_SW_H) / 2, on);
}

/* ---------------- 天气行: 雨/雪/火雨/雷电 一行四个独立开关 ----------------
 * 样式与分段选择行一致 (开 = 金色 + 金线, 关 = 灰字 + 灰线), 点文字切换, 可多个
 * 同时开。以后加天气种类只需扩 s_Toggle 表、ESC_TRIO_N 和这段的枚举。 */

#define ESC_TRIO_N 4        /* = GUI_ITEM_WTH_RAIN 起连续 4 个段 */

/* 分段文字下的横条 y: 所有"文字 + 下划线"的控件共用这一口径 (贴行底 2px),
 * 文字基线由 ulY 反推 (ulY + 1 - bodyH)。新加分段控件必须走这里, 别各算各的。 */
static int Esc_SegUnderlineY(int rowY)
{
    return rowY + ESC_ROW_H - 2;
}

/* 段文字顶: 与横条同基线, 太高时压回行顶。 */
static int Esc_SegTextTop(int rowY, int bodyH)
{
    int ty = Esc_SegUnderlineY(rowY) + 1 - bodyH;

    return ty < rowY ? rowY : ty;
}

/* 一段可选标签: 选中金色 + 金线, 未选中灰字 + 灰线。分段行/天气行/排序行/
 * 端点行共用这一份口径。 */
static void Esc_SegText(void* surf, const wchar_t* name, int x, int w,
                        int rowY, int bodyH, int on)
{
    int ulY = Esc_SegUnderlineY(rowY);
    int ty = Esc_SegTextTop(rowY, bodyH);

    Esc_Text(surf, name, x, ty, w + 2, ESC_BODY_PX, ESC_BODY_SLOT,
             on ? 245 : 145, on ? 225 : 145, on ? 170 : 145);
    Esc_Fill(surf, x, ulY, w, 2,
             on ? 208 : 0x7B, on ? 160 : 0x79, on ? 64 : 0x7B,
             on ? 255 : 140);
}

/* 拖动条轨道的最小可用宽度: 再窄就不像滑条了, 那时宁可换行给它整行宽。 */
#define ESC_TRK_MIN 150

/* 光照参数一格 = [小标题][缝][数值框] (与 Esc_DrawLightParamRow 同一口径)。 */
static int Esc_LightCellW(int param)
{
    int cw = Esc_TextWidth(s_GuiLabel[s_LightItem[param]],
                           ESC_BODY_PX, ESC_BODY_SLOT);

    if (cw <= 0) cw = 16;
    return cw + LIGHT_CAP_GAP + Esc_LightBoxW();
}

/* 一段文字的原子宽度 (0 宽会让它永远排得下, 给个下限)。 */
static int Esc_NameAtoms(EscName* names, int n, EscFlow* f)
{
    int i;

    if (n > ESC_FLOW_MAX) n = ESC_FLOW_MAX;
    for (i = 0; i < n; i++)
    {
        f->w[i] = Esc_TextWidth(names[i], ESC_BODY_PX, ESC_BODY_SLOT);
        if (f->w[i] <= 0) f->w[i] = 8;
    }
    return n;
}

/* 这一行的原子宽度 (不含原子间距, 间距写回 *gap)。返回原子数。 */
static int Esc_RowAtoms(int kind, int item, EscSegs sg, EscFlow* f, int* gap)
{
    int i;

    *gap = ESC_SKIN_GAP;
    switch (kind)
    {
    case EG_SWITCH:
        f->w[0] = Esc_StateW();
        f->w[1] = ESC_SW_W;
        *gap = ESC_STATE_GAP;
        return 2;
    case EG_SORT:
        f->w[0] = Esc_TextWidth(s_GuiFaction, ESC_BODY_PX, ESC_BODY_SLOT);
        f->w[1] = Esc_TextWidth(s_GuiFill, ESC_BODY_PX, ESC_BODY_SLOT);
        *gap = ESC_SORT_GAP;
        return 2;
    case EG_VALUE:
    case EG_ROWS:
        f->w[0] = ESC_STEP_W;
        return 1;
    case EG_LIGHTPARM:
    {
        int first = Esc_LightRowFirst(item);

        if (first < 0) return 0;
        for (i = 0; i < LIGHT_ROW_CELLS; i++)
            f->w[i] = Esc_LightCellW(first + i);
        *gap = LIGHT_CELL_GAP;
        return LIGHT_ROW_CELLS;
    }
    case EG_TRIO:
        for (i = 0; i < ESC_TRIO_N; i++)
        {
            f->w[i] = Esc_TextWidth(s_GuiLabel[GUI_ITEM_WTH_RAIN + i],
                                    ESC_BODY_PX, ESC_BODY_SLOT);
            if (f->w[i] <= 0) f->w[i] = 8;
        }
        return ESC_TRIO_N;
    case EG_SKIN:
        return Esc_NameAtoms(s_GuiSkinName, ESC_SKIN_N, f);
    default:
        break;
    }
    return sg.names ? Esc_NameAtoms(sg.names, sg.n, f) : 0;
}

/* 原子排布: 整串放得下就整串右对齐; 放不下就先按 [控件列首, 右界] 的容量 (和
 * per 给的每行列数) 分行, 再每行各自右对齐。行宽是按容量分出来的, 右对齐后起点
 * 必然 >= 列首, 参数不会折到行标题底下。列首连第一个原子都放不下时 (超长标题 +
 * 窄栏) 标题独占第一行、整串从行首排, 那时 x0line 留 0。 */
static void Esc_FlowPlace(EscFlow* f, int rowL, int rowW, const wchar_t* label,
                          int gap, int per)
{
    int xIns = rowL + ESC_TEXT_INSET;
    int right = rowL + rowW - ESC_CTL_RIGHT;
    int lblW = (label && label[0])
                   ? Esc_TextWidth(label, ESC_BODY_PX, ESC_BODY_SLOT) : 0;
    int total = 0, i, line, cnt, used, base;
    int xCtl = xIns + lblW + ESC_LBL_GAP;   /* 控件列首 = 标题右缘 + 缝 */
    int lineW[ESC_FLOW_MAX];
    int n = f->n;

    if (n <= 0) { f->lines = 1; f->x0line = 0; return; }
    for (i = 0; i < n; i++) total += f->w[i] + (i ? gap : 0);

    if (lblW + ESC_LBL_GAP + total <= rowW - ESC_TEXT_INSET - ESC_CTL_RIGHT)
    {   /* 一行放得下: 整串右对齐 */
        int x = right - total;

        for (i = 0; i < n; i++)
        {
            f->x[i] = x;
            f->ln[i] = 0;
            x += f->w[i] + gap;
        }
        f->lines = 1;
        f->x0line = right - total;
        return;
    }

    base = xCtl;
    if (f->w[0] > right - xCtl) base = xIns;   /* 列首放不下第一个原子 */

    /* 第一遍: 只分组 (每行放了哪几个, 各多宽), 位置第二遍再定。 */
    line = 0;
    cnt = 0;
    used = 0;
    for (i = 0; i < n; i++)
    {
        int need = (cnt ? gap : 0) + f->w[i];

        if (cnt > 0 && (used + need > right - base || (per > 0 && cnt >= per)))
        {   /* 这一行满了: 收行, 当前原子另起一行 */
            lineW[line] = used;
            line++;
            cnt = 0;
            used = 0;
            need = f->w[i];
        }
        f->ln[i] = line;
        used += need;
        cnt++;
    }
    lineW[line] = used;
    line++;

    /* 第二遍: 每行右对齐到右界。 */
    for (i = 0; i < line; i++)
    {
        int x = right - lineW[i];
        int k;

        for (k = 0; k < n; k++)
            if (f->ln[k] == i)
            {
                f->x[k] = x;
                x += f->w[k] + gap;
            }
    }
    f->lines = line;
    f->x0line = (base == xIns) ? 0 : right - lineW[0];
}

/* 一行的控件流。EG_SLIDE 不是一堆原子而是一条可长可短的轨道: 放得下时轨道自己
 * 缩, 放不下整条挪到第二行 (那时才用满整行宽), 所以只出行数不出原子。 */
static void Esc_RowFlow(int kind, int item, int rowL, int rowW,
                        const wchar_t* label, EscSegs sg, EscFlow* f)
{
    int gap = 0;

    f->n = 0;
    f->lines = 1;
    f->x0line = 0;

    if (kind == EG_SLIDE)
    {
        int lw = (label && label[0])
                     ? Esc_TextWidth(label, ESC_BODY_PX, ESC_BODY_SLOT) : 0;

        if (!Esc_ItemEnds(item) &&
            lw + ESC_LBL_GAP + ESC_SLIDE_GAP + ESC_TRK_MIN + ESC_STEP_W
               + ESC_CTL_RIGHT + ESC_TEXT_INSET > rowW)
            f->lines = 2;
        return;
    }
    if (kind == EG_GROUP || kind == EG_SEP) return;
    if (kind == EG_COLOR)
    {   /* 色块栅格: 行高另算 (Esc_RowH), 这里只给行标题让出固定的色块列首 */
        f->x0line = rowL + ESC_COLOR_LABEL_W;
        return;
    }

    f->n = Esc_RowAtoms(kind, item, sg, f, &gap);
    /* 云影风格 8 个预设名都长, 无论栏多宽都按每行四个排 (两行), 免得一条
     * 挤成五六段、另一条只剩两段。 */
    if (f->n > 0)
        Esc_FlowPlace(f, rowL, rowW, label, gap,
                      item == GUI_ITEM_WTH_STYLE ? ESC_STYLE_PER : 0);
}

/* 行标题: 永远在最上面一行, 宽度到第 0 行第一个原子为止 (该行没有原子就占满
 * 控件区)。seg = 1 时与分段文字同基线 (贴横条上缘), 0 = 行内垂直居中。 */
static void Esc_FlowLabel(void* surf, const EscFlow* f, int rowL, int rowW,
                          int rowY, int bodyH, const wchar_t* label, int seg)
{
    int right = f->x0line ? f->x0line - ESC_LBL_GAP
                          : rowL + rowW - ESC_CTL_RIGHT;
    int w = right - (rowL + ESC_TEXT_INSET);
    int y = seg ? (Esc_SegUnderlineY(rowY) + 1 - bodyH)
                : (rowY + (ESC_ROW_H - bodyH) / 2);

    if (!label || !label[0] || w <= 0) return;
    if (y < rowY) y = rowY;
    Esc_Text(surf, label, rowL + ESC_TEXT_INSET, y, w, ESC_BODY_PX,
             ESC_BODY_SLOT, 232, 232, 232);
}

/* 命中第几个原子 (-1 = 没点中)。行高按 ESC_ROW_H, 左右各放宽 2px。 */
static int Esc_FlowAtomAt(const EscFlow* f, int rowY, int mx, int my)
{
    int i;

    for (i = 0; i < f->n; i++)
    {
        int y = rowY + f->ln[i] * ESC_ROW_H;

        if (mx >= f->x[i] - 2 && mx < f->x[i] + f->w[i] + 2 &&
            my >= y && my < y + ESC_ROW_H)
            return i;
    }
    return -1;
}

/* 拖动条那一行的控件 y (整条落在第二行时下移一格)。 */
static int Esc_FlowCtlY(const EscFlow* f, int rowY)
{
    return rowY + (f->lines > 1 ? ESC_ROW_H : 0);
}

static void Esc_DrawTrioRow(void* surf, const EscFlow* f, int rowL, int rowW,
                            int rowY, int bodyH)
{
    int i;

    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH,
                  s_GuiLabel[GUI_ITEM_WTH_TRIO], 1);
    for (i = 0; i < f->n && i < ESC_TRIO_N; i++)
    {
        const EscToggle* t = Esc_ToggleFind(GUI_ITEM_WTH_RAIN + i);

        Esc_SegText(surf, s_GuiLabel[GUI_ITEM_WTH_RAIN + i], f->x[i], f->w[i],
                    rowY + f->ln[i] * ESC_ROW_H, bodyH, t && t->get());
    }
}

/* 侧栏排序行: 两段 (兵种/填充) 二选一, 段绘制与分段行同源。 */
static void Esc_DrawSortRow(void* surf, const EscFlow* f, int rowL, int rowW,
                            int rowY, int bodyH, const wchar_t* label)
{
    int fill = SWS_FillMode();
    int i;

    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 1);
    for (i = 0; i < f->n && i < 2; i++)
        Esc_SegText(surf, i ? s_GuiFill : s_GuiFaction, f->x[i], f->w[i],
                    rowY + f->ln[i] * ESC_ROW_H, bodyH, i ? fill : !fill);
}

/* 语言切换在左栏最底下: 与页名列表隔开 (中间一条分隔线), 不跟标题抢位置。
 * 三个段名右对齐到面板右缘, 段宽按实测字宽。 */
static void Esc_LangLayout(const RECT* p, int* xs, int* ws)
{
    int right = p->right - ESC_PAD - ESC_TEXT_INSET;
    int i, total = 0, x;

    for (i = 0; i < ESC_LANG_N; i++)
    {
        ws[i] = Esc_TextWidth(s_LangLabel[i], ESC_BODY_PX, ESC_BODY_SLOT);
        total += ws[i] + (i ? ESC_LANG_GAP : 0);
    }
    x = right - total;
    for (i = 0; i < ESC_LANG_N; i++)
    {
        xs[i] = x;
        x += ws[i] + ESC_LANG_GAP;
    }
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
    int y = Esc_LangY(p);

    if (my < y || my >= y + ESC_ROW_H) return -1;
    return Esc_LangHitX(p, mx);
}

/* 一排等间距的可选标签 (标题栏语言切换 / 端点行两段)。 */
static void Esc_DrawSegs(void* surf, const int* xs, const int* ws,
                         EscName* names, int n, int value, int rowY,
                         int bodyH)
{
    int i;

    for (i = 0; i < n; i++)
        Esc_SegText(surf, names[i], xs[i], ws[i], rowY, bodyH, i == value);
}

/* 语言切换在左栏最底下: 与页名列表隔开 (中间一条分隔线), 不跟标题抢位置。 */
static void Esc_DrawLangBottom(void* surf, const RECT* p)
{
    int xs[ESC_LANG_N], ws[ESC_LANG_N];
    int mode = Lang_GetMode(), i, value = -1;
    int bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    int y = Esc_LangY(p);

    if (bodyH > ESC_ROW_H) bodyH = ESC_ROW_H;
    for (i = 0; i < ESC_LANG_N; i++)
        if (s_LangMode[i] == mode) value = i;
    Esc_Fill(surf, p->left + ESC_PAD, y - ESC_SEP_GAP, p->right - p->left - ESC_PAD * 2,
             1, 0x7B, 0x79, 0x7B, 160);
    Esc_LangLayout(p, xs, ws);
    Esc_DrawSegs(surf, xs, ws, s_LangLabel, ESC_LANG_N, value, y, bodyH);
}

/* 分段行: 段落在第几行由控件流给, 文字与横条的相对位置见 Esc_SegText。 */
static void Esc_DrawSegRow(void* surf, const EscFlow* f, int rowL, int rowW,
                           int rowY, int bodyH, const wchar_t* label,
                           EscName* names, int value)
{
    int i;

    if (value < 0 || value >= f->n) value = -1;   /* -1 = 全都没选中 */
    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 1);
    for (i = 0; i < f->n; i++)
        Esc_SegText(surf, names[i], f->x[i], f->w[i],
                    rowY + f->ln[i] * ESC_ROW_H, bodyH, i == value);
}

static void Esc_DrawColorRow(void* surf, const EscFlow* f, int rowL, int rowW,
                             int rowY, int bodyH, const wchar_t* label)
{
    int value = PlayerColor_Current();
    int i, x, y, count = PlayerColor_Count();

    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 0);

    for (i = 0; i < count; i++)
    {
        Esc_ColorCellRect(i, rowL, rowW, rowY, &x, &y);
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
    else Esc_SignedToW(num, value, 16);

    numX = Esc_StepNumX(boxX, boxW, num);
    Esc_Text(surf, num, numX, textY, boxW, ESC_BODY_PX, ESC_BODY_SLOT,
             238, 238, 238);

    if (editing)
    {
        wchar_t pre[ESC_EDIT_CAP];
        int caretX;

        lstrcpynW(pre, num, s_EditSt.cur + 1);
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

/* 光照参数行: 一格 = [小标题][数值框], 三个格是一行的三个原子, 放不下就按格
 * 折行。数值单位见 LightingPreset.h (环境与 R/G/B = ini ×100, 等级/地面 ×1000)。
 * item = 该行的行 item; 第二行 (R/G/B) 没有行标题。 */
static void Esc_DrawLightParamRow(void* surf, const EscFlow* f, int rowL,
                                  int rowW, int rowY, int bodyH,
                                  const wchar_t* label, int item)
{
    int first = Esc_LightRowFirst(item);
    int boxW = Esc_LightBoxW();
    int i, y, capY, boxY;

    if (first < 0) return;
    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 0);

    for (i = 0; i < f->n && i < LIGHT_ROW_CELLS; i++)
    {
        int p = first + i;
        int capW, boxX;

        Esc_LightCell(f, i, &capW, &boxX);
        y = rowY + f->ln[i] * ESC_ROW_H;
        capY = y + (ESC_ROW_H - bodyH) / 2;
        boxY = y + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;
        Esc_Text(surf, s_GuiLabel[s_LightItem[p]], f->x[i], capY, capW,
                 ESC_BODY_PX, ESC_BODY_SLOT, 230, 230, 230);
        Esc_DrawEditBox(surf, boxX, boxY, boxW, capY, Lighting_GetParam(p),
                        Esc_EditIs(EDIT_PAGE_GUI, s_LightItem[p]), NULL);
    }
}

static void Esc_DrawValueRow(void* surf, const EscFlow* f, int rowL, int rowW,
                             int rowY, int bodyH, const wchar_t* label,
                             int value, int minValue, int maxValue,
                             int editing, const wchar_t* text)
{
    int ctlX = f->x[0], btnY, boxX, boxW;

    btnY = rowY + f->ln[0] * ESC_ROW_H + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;
    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 0);

    Esc_StepButton(surf, ctlX, btnY, L"-", value > minValue);
    Esc_StepButton(surf, ctlX + ESC_STEP_W - ESC_STEP_BTN_W, btnY, L"+",
                   value < maxValue);

    Esc_StepBox(ctlX, &boxX, &boxW);
    Esc_DrawEditBox(surf, boxX, btnY, boxW,
                    rowY + f->ln[0] * ESC_ROW_H + (ESC_ROW_H - bodyH) / 2,
                    value, editing, text);
}

/* 拖动条行。ends 非空 = 端点行 (调色比例 / 矿石光照): [左段][轨道][右段][数值],
 * 两端段是 0 / 满值 的快捷, 数值只显示不能输入; 否则是游戏速度行的
 * [- 数值 +] (数值框可编辑)。 */
static void Esc_DrawSlideRow(void* surf, const EscFlow* f, int rowL, int rowY,
                             int rowW, int bodyH, const wchar_t* label,
                             int value, int minv, int maxv, int editing,
                             const wchar_t* text, EscName* ends)
{
    int ctlY = Esc_FlowCtlY(f, rowY);
    int trkX, trkW, numX = 0, numW = 0, knobX, ty, ky, btnY;
    int xs[2] = { 0 }, ws[2] = { 0 }, valX = 0, valW = 0, pick;

    if (ends)
    {
        /* 端点行: 段文字与行标题同基线, 数值文字在框内垂直居中。行标题宽度
         * 卡到轨道左缘, 免得长标题压到轨道上。 */
        Esc_EndBarGeom(rowL, rowW, label, ends, &trkX, &trkW, xs, ws,
                       &valX, &valW);
        Esc_Text(surf, label, rowL + ESC_TEXT_INSET, Esc_SegTextTop(ctlY, bodyH),
                 trkX - ESC_LBL_GAP - (rowL + ESC_TEXT_INSET), ESC_BODY_PX,
                 ESC_BODY_SLOT, 232, 232, 232);
    }
    else
    {
        Esc_SlideGeom(rowL, rowW, label, &trkX, &trkW, &numX, &numW);
        Esc_RowLabel(surf, rowL, rowY, trkX, bodyH, label);
    }

    /* 轨道 + 选中段 */
    ty = ctlY + (ESC_ROW_H - ESC_SLIDE_TRK_H) / 2;
    if (trkW > 8)
    {
        Esc_Capsule(surf, trkX, ty, trkW, ESC_SLIDE_TRK_H, 44, 44, 44, 235);
        knobX = Esc_TrackXFor(value, trkX, trkW, minv, maxv);
        if (knobX - trkX > 8)
            Esc_Capsule(surf, trkX, ty, knobX - trkX, ESC_SLIDE_TRK_H,
                        208, 160, 64, 235);
    }
    else
        knobX = trkX;

    /* 把手 */
    ky = ctlY + (ESC_ROW_H - ESC_SLIDE_KNOB_H) / 2;
    Esc_Fill(surf, knobX - ESC_SLIDE_KNOB_W / 2, ky,
             ESC_SLIDE_KNOB_W, ESC_SLIDE_KNOB_H, 236, 236, 236, 255);
    Esc_Outline(surf, knobX - ESC_SLIDE_KNOB_W / 2, ky,
                ESC_SLIDE_KNOB_W, ESC_SLIDE_KNOB_H, 20, 20, 20, 220, 1);
    btnY = ctlY + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;

    if (!ends)
    {
        Esc_StepButton(surf, Esc_RowCtlX(rowL + rowW), btnY, L"-",
                       value > minv);
        Esc_StepButton(surf, Esc_RowCtlX(rowL + rowW) + ESC_STEP_W -
                       ESC_STEP_BTN_W, btnY, L"+", value < maxv);
        Esc_DrawEditBox(surf, numX, btnY, numW,
                        ctlY + (ESC_ROW_H - bodyH) / 2, value, editing, text);
        return;
    }

    /* 两端快捷段 + 只显数值的小框 */
    pick = (value <= minv) ? 0 : (value >= maxv ? 1 : -1);
    Esc_DrawSegs(surf, xs, ws, ends, 2, pick, ctlY, bodyH);
    Esc_DrawEditBox(surf, valX, btnY, valW,
                    btnY + (ESC_STEP_BTN_H - bodyH) / 2, value, 0, NULL);
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
static void Esc_RowPlate(void* surf, int rowL, int rowW, int y, int rh,
                         int line)
{
    Esc_RowBg(surf, rowL, y, rowW);
    if (rh > ESC_ROW_H)
        Esc_Fill(surf, rowL, y + ESC_ROW_H, rowW, rh - ESC_ROW_H, 32, 32, 32, 120);
    if (line) Esc_Fill(surf, rowL, y + rh - 1, rowW, 1, 0x7B, 0x79, 0x7B, 80);
}

/* 分段行的段名表 (见 EscSegs): 两种选择项表取法一样, 找不到 = 空表 (该行不当
 * 分段行画)。 */
#define ESC_SEGS(fn, T, find)                                                \
    static EscSegs fn(int item)                                              \
    {                                                                        \
        const T* c = find(item);                                             \
        EscSegs s;                                                           \
        s.names = c ? c->names : NULL;                                       \
        s.n = c ? c->n : 0;                                                  \
        return s;                                                            \
    }

ESC_SEGS(EscGuiSegs, EscChoice, Esc_ChoiceFind)
ESC_SEGS(EscTextSegs, EscTextChoice, Esc_TChoiceFind)

/* 设置页一行的控件。开关项查 s_Toggle 拿当前值, 分段项查 s_Choice.
 * 控件版式统一由 Esc_RowFlow 算 (见"控件流"), 这里只按行类型分派。 */
static void Esc_DrawGuiRow(void* surf, int rowL, int rowY, int rowW,
                           int bodyH, int kind, int item)
{
    const EscToggle* t;
    const EscChoice* c = Esc_ChoiceFind(item);
    const wchar_t* label = s_GuiLabel[item];
    EscSegs sg = EscGuiSegs(item);
    EscFlow f;
    int cur;

    Esc_RowFlow(kind, item, rowL, rowW, label, sg, &f);

    if (kind == EG_SWITCH)
    {
        t = Esc_ToggleFind(item);
        Esc_DrawSwitchRow(surf, &f, rowL, rowW, rowY, bodyH, label,
                          t ? t->get() : 0);
    }
    else if (kind == EG_SORT)
        Esc_DrawSortRow(surf, &f, rowL, rowW, rowY, bodyH, label);
    else if (kind == EG_ROWS)
        Esc_DrawValueRow(surf, &f, rowL, rowW, rowY, bodyH, label,
                         SWS_MaxRows(), ESC_ROWS_MIN, ESC_ROWS_MAX,
                         Esc_EditIs(EDIT_PAGE_GUI, GUI_ITEM_ROWS), NULL);
    else if (kind == EG_SLIDE)
    {
        int minv, maxv, step;

        cur = Esc_SlideSpec(item, &minv, &maxv, &step);
        if (item == s_SlideDrag) cur = s_SlideVal;    /* 拖动中显示未生效的值 */
        Esc_DrawSlideRow(surf, &f, rowL, rowY, rowW, bodyH, label, cur,
                         minv, maxv,
                         item == GUI_ITEM_GAMESPEED
                             ? Esc_EditIs(EDIT_PAGE_GUI, item) : 0,
                         (item == GUI_ITEM_GAMESPEED && cur > ESC_SPEED_MAX)
                             ? L"MAX" : NULL,
                         Esc_ItemEnds(item));
    }
    else if (kind == EG_LIGHTPARM)
        Esc_DrawLightParamRow(surf, &f, rowL, rowW, rowY, bodyH, label, item);
    else if (kind == EG_TRIO)
        Esc_DrawTrioRow(surf, &f, rowL, rowW, rowY, bodyH);
    else if (kind == EG_COLOR)
        Esc_DrawColorRow(surf, &f, rowL, rowW, rowY, bodyH, label);
    else if (kind == EG_SKIN)
        Esc_DrawSegRow(surf, &f, rowL, rowW, rowY, bodyH, label,
                       s_GuiSkinName, SidebarSkin_Get());
    else if (sg.names)
        Esc_DrawSegRow(surf, &f, rowL, rowW, rowY, bodyH, label,
                       sg.names, Esc_ChoiceValue(c));
}

/* 文本页一行的控件。 */
static void Esc_DrawTextRow(void* surf, int rowL, int rowY, int rowW,
                            int bodyH, int kind, int item)
{
    const EscTextNum* num;
    const EscTextChoice* c;
    const EscTextToggle* t;
    const wchar_t* label = s_TextLabel[item];
    EscFlow f;
    EscSegs sg = EscTextSegs(item);

    Esc_RowFlow(kind, item, rowL, rowW, label, sg, &f);

    if (kind == EG_SWITCH)
    {
        t = Esc_TToggleFind(item);
        Esc_DrawSwitchRow(surf, &f, rowL, rowW, rowY, bodyH, label,
                          t ? Esc_TToggleVal(t) : 0);
    }
    else if (kind == EG_VALUE)
    {
        num = Esc_TextNumFind(item);
        if (num)
            Esc_DrawValueRow(surf, &f, rowL, rowW, rowY, bodyH, label,
                             MsgText_GetOption(num->opt), num->min, num->max,
                             Esc_EditIs(EDIT_PAGE_TEXT, item), NULL);
    }
    else if ((c = Esc_TChoiceFind(item)) != NULL)
        Esc_DrawSegRow(surf, &f, rowL, rowW, rowY, bodyH, label,
                       c->names, Esc_TChoiceValue(c));
}

static const EscPage kGuiPage = { s_GuiLayout, ESC_GUI_DISPLAY, ESC_GUI_GROUP_H,
                                  ESC_GUI_GAP, s_GuiGroup, s_GuiLabel,
                                  EscGuiSegs, Esc_DrawGuiRow, &s_GuiScroll,
                                  &s_GuiMissing };
static const EscPage kTextPage = { s_TextLayout, TX_COUNT, ESC_GROUP_H,
                                   ESC_BLOCK_GAP, NULL, s_TextLabel,
                                   EscTextSegs, Esc_DrawTextRow, &s_TextScroll,
                                   &s_TextMissing };

/* 两页共用的行循环: 组标题 / 分隔条 / 行底板在这里, 控件交给 page->draw。
 * 滚出视野的行不画但仍占位 (两页都可滚, 设置页行数会随功能增加)。
 * 鼠标悬停的内容行加一层暗紫高亮 (组标题/分隔条不参与)。 */
static void Esc_RowsPanel(void* surf, RECT* p, const wchar_t* title,
                          const EscPage* pg, HWND hDlg)
{
    int w, bodyH, rowL, rowW, i, y, rh, kind, item;
    int clipTop, clipBottom, maxScroll;
    int hovX = -0x40000000, hovY = -0x40000000;
    POINT pt;

    if (!Esc_PanelFrame(surf, p, title)) return;

    /* 光标在面板内才高亮; lp 坐标不可靠 (见 Esc_PageWheel), 用 GetCursorPos */
    if (hDlg && GetCursorPos(&pt) && ScreenToClient(hDlg, &pt))
    {
        hovX = pt.x;
        hovY = pt.y;
    }

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
    s_ClipTop = clipTop;
    s_ClipBot = clipBottom;
    for (i = 0; i < pg->n; i++)
    {
        kind = pg->rows[i].kind;
        item = pg->rows[i].item;

        if (kind == EG_GROUP && item != 0) y += pg->gap;
        rh = Esc_RowH(p, pg, i);

        if (y + rh <= clipTop || y >= clipBottom) { y += rh; continue; }

        if (kind == EG_SEP)
            Esc_Fill(surf, rowL + ESC_TEXT_INSET, y + rh / 2,
                     rowW - ESC_TEXT_INSET * 2, 1, 0x7B, 0x79, 0x7B, 160);
        else if (kind == EG_GROUP)
            Esc_GroupRow(surf, rowL, rowW, y, rh, bodyH,
                         pg->groups ? pg->groups[item] : pg->labels[item]);
        else
        {
            /* 光照参数两行是一组: 第一行不画底部分隔线。 */
            Esc_RowPlate(surf, rowL, rowW, y, rh,
                         !(kind == EG_LIGHTPARM && item == GUI_ITEM_LIGHT_PARM));
            if (EscIn(rowL, y, rowW, rh, hovX, hovY))
                Esc_Fill(surf, rowL, y, rowW, rh, 123, 32, 123, 70);
            pg->draw(surf, rowL, y, rowW, bodyH, kind, item);
        }
        y += rh;
    }
    s_ClipBot = 0;

    if (maxScroll > 0)
    {
        int trackX = p->right - 6;
        int trackH = clipBottom - clipTop;
        int total = Esc_PageTotalH(p, pg);
        int thumbH, thumbY;

        if (total < 1) total = 1;
        /* thumb/track = 可视/总高 = trackH/total。maxScroll = total - trackH,
         * 别把 (trackH + maxScroll) 当可视比例 —— 那正好等于 total, thumb 会
         * 恒占满轨道, 看不出还有内容可滚。 */
        thumbH = trackH * trackH / total;
        if (thumbH < 18) thumbH = 18;
        if (thumbH > trackH) thumbH = trackH;
        thumbY = clipTop;
        if (trackH > thumbH)
            thumbY += (trackH - thumbH) * *pg->scroll / maxScroll;
        Esc_Fill(surf, trackX, clipTop, 3, trackH, 0x7B, 0x79, 0x7B, 90);
        Esc_Fill(surf, trackX, thumbY, 3, thumbH, 208, 160, 64, 220);
    }
}

static void Esc_TextPanel(void* surf, RECT* p, HWND hDlg)
{
    Esc_RowsPanel(surf, p, s_TextTitle, &kTextPage, hDlg);
}

static void Esc_SettingsPanel(void* surf, RECT* p, HWND hDlg)
{
    /* 菜单开着时主循环停摆, GameSpeed_Frame 不会被调; 在这里补一次同步,
     * 否则拖过原版速度页的滑块再回来, 这一行显示的还是旧值。 */
    GameSpeed_Poll();
    Esc_RowsPanel(surf, p, s_GuiTitle, &kGuiPage, hDlg);
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
    Esc_DrawLangBottom(surf, &a);
    if (s_Sel == 0) Esc_SettingsPanel(surf, &b, hDlg);
    else            Esc_TextPanel(surf, &b, hDlg);
    Esc_DrawCancel(surf, &b);
}

/* ---------------- input ---------------- */

/* 行内的名字比较都是"内存里一段 [ls,le) 对 ini 键名"。lstrcmpiA 要 NUL 结尾,
 * 所以先拷到栈上 (section/key 都是短名字); 不区分大小写, 也不碰区间外的字节。 */
static int Esc_NameEq(const unsigned char* d, int n, const char* s)
{
    char buf[40];

    if (n >= (int)sizeof(buf)) return 0;
    lstrcpynA(buf, (const char*)d, n + 1);
    return !lstrcmpiA(buf, s);
}

static int Esc_LineSection(const unsigned char* d, DWORD ls, DWORD le,
                           const char* section)
{
    DWORD p = ls, e;
    int n = lstrlenA(section);

    while (p < le && (d[p] == ' ' || d[p] == '\t')) p++;
    if (p >= le || d[p] != '[') return 0;
    p++;
    e = p;
    while (e < le && d[e] != ']') e++;
    while (e > p && (d[e - 1] == ' ' || d[e - 1] == '\t')) e--;
    return ((int)(e - p) == n) && Esc_NameEq(d + p, n, section);
}

static int Esc_LineKey(const unsigned char* d, DWORD ls, DWORD le,
                       const char* key)
{
    DWORD p = ls;
    int n;

    while (p < le && (d[p] == ' ' || d[p] == '\t')) p++;
    if (p >= le || d[p] == ';' || d[p] == '[') return 0;
    n = lstrlenA(key);
    if ((DWORD)n > le - p) return 0;
    if (!Esc_NameEq(d + p, n, key)) return 0;
    p += n;
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
    memcpy(out + pos, src, (size_t)n);
    return pos + n;
}

static unsigned Esc_PutText(unsigned char* out, unsigned pos, const char* s)
{
    return Esc_PutBytes(out, pos, (const unsigned char*)s, (DWORD)lstrlenA(s));
}

static unsigned Esc_PutEol(unsigned char* out, unsigned pos)
{
    out[pos] = '\r';
    out[pos + 1] = '\n';
    return pos + 2;
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
    const EscNum* n;
    int idx;

    if (page == EDIT_PAGE_GUI)
    {
        /* 光照参数: 临时设置, 不落盘 (换局/取消菜单后按地图原值重置)。 */
        idx = Esc_LightItemIdx(item);
        if (idx >= 0) { Lighting_SetParam(idx, value); return; }
        n = Esc_NumFind(item);
        if (!n) return;
        n->set(value);
        if (n->section) Esc_QueueValue(n->section, n->key, n->get());
        return;
    }
    num = Esc_TextNumFind(item);
    if (!num) return;
    MsgText_SetOption(num->opt, value);
    Esc_QueueValue("MsgText", num->key, value);
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
        if (rowY + ESC_ROW_H > Esc_LangY(a) - ESC_SEP_GAP) break;
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
        rh = Esc_RowH(b, pg, i);
        if (y + rh <= clipTop || y >= clipBottom) { y += rh; continue; }
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

/* 行内控件的精确命中: 只有真点在控件上才算。控件位置与绘制同出一源
 * (Esc_RowFlow), 所以"画在哪就点在哪", 折行之后也自动对得上。 */
static int Esc_CtlHit(int kind, int item, const wchar_t* label, EscSegs sg,
                      int rowL, int rowW, int rowY, int mx, int my)
{
    EscFlow f;

    if (kind == EG_GROUP || kind == EG_SEP) return 0;
    Esc_RowFlow(kind, item, rowL, rowW, label, sg, &f);

    if (kind == EG_SWITCH)
        return EscIn(f.x[1], rowY + f.ln[1] * ESC_ROW_H
                     + (ESC_ROW_H - ESC_SW_H) / 2,
                     ESC_SW_W, ESC_SW_H, mx, my);
    if (kind == EG_VALUE || kind == EG_ROWS)
        return EscIn(f.x[0], rowY, ESC_STEP_W, ESC_ROW_H, mx, my);
    if (kind == EG_COLOR)
        return Esc_ColorSegmentAt(rowL, rowW, rowY, mx, my) >= 0;
    if (kind == EG_SLIDE)
    {
        int ctlY = Esc_FlowCtlY(&f, rowY);
        int trkX, trkW, numX, numW, xs[2], ws[2], valX, valW;
        EscName* ends = Esc_ItemEnds(item);

        if (ends)   /* 端点行: 两段 + 轨道都算控件区 (右侧数值框不可点) */
        {
            Esc_EndBarGeom(rowL, rowW, label, ends, &trkX, &trkW,
                           xs, ws, &valX, &valW);
            return mx >= xs[0] && mx < xs[1] + ws[1] &&
                   my >= ctlY && my < ctlY + ESC_ROW_H;
        }
        Esc_SlideGeom(rowL, rowW, label, &trkX, &trkW, &numX, &numW);
        /* 右界含 + 按钮 (数字框右缘 + 按钮宽 = 控件区右缘), 否则 + 点不到 */
        return mx >= trkX && mx < numX + numW + ESC_STEP_BTN_W &&
               my >= ctlY && my < ctlY + ESC_ROW_H;
    }
    /* 分段 / 天气 / 光照格: 逐个原子框命中 */
    return Esc_FlowAtomAt(&f, rowY, mx, my) >= 0;
}

/* 当前页的行命中: 先找行再判控件。控件位置与绘制同出一源 (Esc_RowFlow), 所以
 * "画在哪就点在哪"。返回 1 并写出该行的 item 与行顶。 */
static int Esc_HitPage(const RECT* b, const EscPage* pg,
                       const wchar_t (*labels)[ESC_LABEL_CAP], int mx, int my,
                       int* index, int* rowTop)
{
    int row, rowY = 0, clipTop;

    Esc_PageClampScroll(b, pg);
    clipTop = Esc_ContentTop(b, *pg->missing);
    if (!Esc_RowAt(b, pg, clipTop - *pg->scroll, clipTop, mx, my, &row, &rowY))
        return 0;
    if (!Esc_CtlHit(pg->rows[row].kind, pg->rows[row].item,
                    labels[pg->rows[row].item], pg->segs(pg->rows[row].item),
                    b->left + ESC_PAD, (b->right - b->left) - ESC_PAD * 2,
                    rowY, mx, my))
        return 0;
    *index = pg->rows[row].item;
    if (rowTop) *rowTop = rowY;
    return 1;
}

static int Esc_HitRight(const RECT* b, int mx, int my, int* index, int* rowTop)
{
    if (s_Sel != 0) return 0;
    return Esc_HitPage(b, &kGuiPage, s_GuiLabel, mx, my, index, rowTop);
}

static void Esc_ApplyGui(HWND hDlg, const RECT* right, int index, int mx,
                         int my, int rowTop)
{
    const EscToggle* t = Esc_ToggleFind(index);
    const EscChoice* c = Esc_ChoiceFind(index);
    int rowL = right->left + ESC_PAD;
    int rowW = (right->right - right->left) - ESC_PAD * 2;
    int value, pick;

    /* 纯开关 (s_Toggle) 与分段选择 (s_Choice) 都在表里, 这里只剩特例。 */
    if (t)
    {
        Esc_ToggleFlip(t);
        Esc_Redraw(hDlg);
        return;
    }
    if (c)
    {
        EscFlow f;

        Esc_RowFlow(EG_CHOICE, index, rowL, rowW, s_GuiLabel[index],
                    EscGuiSegs(index), &f);
        pick = Esc_FlowAtomAt(&f, rowTop, mx, my);
        if (pick < 0) return;
        value = c->invert ? !pick : pick;
        if (value == c->get()) return;
        c->apply(pick);
        Esc_Redraw(hDlg);
        return;
    }

    switch (index)
    {
    case GUI_ITEM_LIGHT_MODE:
    case GUI_ITEM_ORELIGHT:
    {
        /* 端点行: 点左段跳最小值, 点右段跳满值, 点轨道跳值并进入拖动。 */
        int trkX, trkW, xs[2], ws[2], valX, valW;
        int minv, maxv, step;

        Esc_SlideSpec(index, &minv, &maxv, &step);
        Esc_EndBarGeom(rowL, rowW, s_GuiLabel[index], Esc_ItemEnds(index),
                       &trkX, &trkW, xs, ws, &valX, &valW);
        if (mx >= xs[0] && mx < xs[0] + ws[0])
            value = minv;
        else if (mx >= xs[1] && mx < xs[1] + ws[1])
            value = maxv;
        else
        {
            /* 点轨道: 跳到该点并进入拖动。调色比例一次提交要重建全部约
             * 3.7 万格, 拖动期间只显示、松手才提交; 矿石光照值轻, 直接生效。 */
            value = Esc_TrackValueAt(mx, trkX, trkW, minv, maxv);
            s_SlideVal = value;
            s_SlideDrag = index;
            if (hDlg) SetCapture(hDlg);
            if (index != GUI_ITEM_LIGHT_MODE)
                Esc_ApplyNum(EDIT_PAGE_GUI, index, value);
            break;
        }
        Esc_ApplyNum(EDIT_PAGE_GUI, index, value);
        break;
    }
    case GUI_ITEM_WTH_TRIO:
    {
        /* 点的是哪个天气段切哪个 (s_Toggle 表统一取反+写回; 天气开关不落盘) */
        EscFlow f;
        const EscToggle* wt;
        int seg;

        Esc_RowFlow(EG_TRIO, GUI_ITEM_WTH_TRIO, rowL, rowW,
                    s_GuiLabel[GUI_ITEM_WTH_TRIO], EscGuiSegs(GUI_ITEM_WTH_TRIO),
                    &f);
        seg = Esc_FlowAtomAt(&f, rowTop, mx, my);
        if (seg < 0) return;
        wt = Esc_ToggleFind(GUI_ITEM_WTH_RAIN + seg);
        if (wt) Esc_ToggleFlip(wt);
        break;
    }
    case GUI_ITEM_ROWS:
        Esc_StepOrEdit(hDlg, right, mx, EDIT_PAGE_GUI, GUI_ITEM_ROWS,
                       SWS_MaxRows(), ESC_ROWS_MIN, ESC_ROWS_MAX, 1);
        return;
    case GUI_ITEM_GAMESPEED:
    {
        int trkX, trkW, numX, numW;
        int minv, maxv, step;
        int cur = Esc_SlideSpec(index, &minv, &maxv, &step);
        int v = cur;

        Esc_SlideGeom(rowL, rowW, s_GuiLabel[index], &trkX, &trkW, &numX, &numW);
        /* +/- 按钮: 一次 +-5 (与滑条吸附档一致); 没点上按钮再看数字框 */
        if (Esc_StepHit(right, mx, &v, minv, maxv, step))
        {
            Esc_ApplyNum(EDIT_PAGE_GUI, index, v);
            break;
        }
        if (mx >= numX)          /* 点数值框: 直接输数字 (可输非 5 的倍数) */
        {
            Esc_EditBegin(hDlg, right, mx, EDIT_PAGE_GUI, index, cur, minv,
                          maxv);
            return;
        }
        /* 点轨道: 跳到该点并进入拖动 */
        s_SlideVal = Esc_SlideValueAt(mx, trkX, trkW);
        Esc_ApplyNum(EDIT_PAGE_GUI, index, s_SlideVal);
        s_SlideDrag = index;
        if (hDlg) SetCapture(hDlg);
        break;
    }
    case GUI_ITEM_SKIN:
    {
        EscFlow f;

        Esc_RowFlow(EG_SKIN, index, rowL, rowW, s_GuiLabel[index],
                    EscGuiSegs(index), &f);
        pick = Esc_FlowAtomAt(&f, rowTop, mx, my);
        if (pick < 0 || pick == SidebarSkin_Get()) return;
        SidebarSkin_Set(pick);
        SidebarSkin_Poll();
        Esc_QueueValue("Sidebar", "SkinSide", pick);
        break;
    }
    case GUI_ITEM_COLOR:
        pick = Esc_ColorSegmentAt(rowL, rowW, rowTop, mx, my);
        if (pick < 0 || pick == PlayerColor_Current()) return;
        if (!PlayerColor_Set(pick)) return;
        break;
    case GUI_ITEM_LIGHT_PARM:
    case GUI_ITEM_LIGHT_RGB:
    {
        /* 点的是哪一格的数值框就编辑哪个参数 (格子位置与绘制同一口径) */
        int p = Esc_LightCellAt(right, index, rowTop, mx, my);

        if (p < 0) return;
        Esc_EditBegin(hDlg, right, mx, EDIT_PAGE_GUI, s_LightItem[p],
                      Lighting_GetParam(p), Esc_LightMin(p), LIGHT_PARAM_MAX);
        return;
    }
    default:
        return;
    }
    Esc_Redraw(hDlg);
}

static int Esc_TextHitRight(const RECT* b, int mx, int my, int* index,
                            int* rowTop)
{
    if (s_Sel != 1) return 0;
    return Esc_HitPage(b, &kTextPage, s_TextLabel, mx, my, index, rowTop);
}

/* 文本页的点击处理: 开关/分段/数值三类, 全靠 s_TToggle / s_TChoice / s_TextNum。 */
static void Esc_TextApply(HWND hDlg, const RECT* right, int index, int mx,
                          int my, int rowTop)
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
        EscFlow f;
        int rowL = right->left + ESC_PAD;
        int rowW = (right->right - right->left) - ESC_PAD * 2;

        Esc_RowFlow(EG_CHOICE, index, rowL, rowW, s_TextLabel[index],
                    EscTextSegs(index), &f);
        pick = Esc_FlowAtomAt(&f, rowTop, mx, my);
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

/* 当前编辑框里的数值 (可带前导负号)。 */
static int Esc_EditValue(void)
{
    int v = 0, i = 0, neg = 0;

    if (s_EditSt.len > 0 && s_EditBuf[0] == L'-') { neg = 1; i = 1; }
    for (; i < s_EditSt.len; i++)
        v = v * 10 + (s_EditBuf[i] - L'0');
    return neg ? -v : v;
}

/* 上下键 = ±1 (数值框没有 +/- 按钮时的微调手段)。 */
static void Esc_EditStep(int delta)
{
    int v = Esc_EditValue() + delta;

    if (v < s_EditMin) v = s_EditMin;
    if (v > s_EditMax) v = s_EditMax;
    Esc_SignedToW(s_EditBuf, v, ESC_EDIT_CAP);
    s_EditSt.len = lstrlenW(s_EditBuf);
    s_EditSt.cur = s_EditSt.len;
}

static void Esc_EditCommit(HWND hDlg)
{
    int value;

    if (!s_EditOn) return;
    value = Esc_EditValue();
    if (s_EditSt.len > 0)
    {
        if (value < s_EditMin) value = s_EditMin;
        if (value > s_EditMax) value = s_EditMax;
        Esc_ApplyNum(s_EditPage, s_EditItem, value);
    }
    Esc_EditEnd(hDlg);
}

/* 负号: 只有下限为负的参数 (光照等级) 能按 '-' 切换符号。 */
static void Esc_EditToggleSign(void)
{
    int n = s_EditSt.len, i;

    if (s_EditMin >= 0) return;
    if (n > 0 && s_EditBuf[0] == L'-')
    {
        for (i = 0; i < n; i++) s_EditBuf[i] = s_EditBuf[i + 1];
        s_EditSt.len = n - 1;
    }
    else
    {
        if (n + 1 >= ESC_EDIT_CAP) return;
        for (i = n; i > 0; i--) s_EditBuf[i] = s_EditBuf[i - 1];
        s_EditBuf[0] = L'-';
        s_EditSt.len = n + 1;
    }
    s_EditSt.cur = s_EditSt.len;
    s_EditSt.anchor = -1;
}

/* 数字框输入: 光照参数用计算器式移位 (上限三位数, 换数频繁, 位数打满后再按
 * 数字就丢最左一位、新数字接在末尾); 其余数值框保持满位拒绝的原输入习惯。
 * 负号占一格, 移位时保留。 */
static void Esc_EditInsertDigit(wchar_t ch)
{
    int sign = (s_EditSt.len > 0 && s_EditBuf[0] == L'-') ? 1 : 0;
    int cap = s_EditMaxLen + sign;
    int i;

    if (Esc_LightItemIdx(s_EditItem) < 0 || s_EditSt.len < cap)
    {
        LiEdInsert(&s_EditSt, &ch, 1, cap, NULL, NULL);
        return;
    }
    for (i = sign; i < s_EditSt.len - 1; i++)
        s_EditSt.buf[i] = s_EditSt.buf[i + 1];
    s_EditSt.buf[s_EditSt.len - 1] = ch;
    s_EditSt.cur = s_EditSt.len;
    s_EditSt.anchor = -1;
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
            LiEdErase(&s_EditSt, 0);
        else if (k->vk == VK_DELETE)
            LiEdErase(&s_EditSt, 1);
        else if (k->vk == VK_LEFT || k->vk == VK_RIGHT ||
                 k->vk == VK_HOME || k->vk == VK_END)
            LiEdMove(&s_EditSt, k->vk, 0);
        else if (LiDigit(k->vk, &ch))
            Esc_EditInsertDigit(ch);
        else if (k->vk == VK_OEM_MINUS || k->vk == VK_SUBTRACT)
            Esc_EditToggleSign();
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

/* 滚轮悬停在数值控件上时微调数值 (步长按各行自己的口径), 不滚页。
 * 返回 1 = 已处理 (吞掉滚轮)。 */
static int Esc_WheelValue(HWND h, const RECT* b, int mx, int my, int delta)
{
    int index = -1, rowTop = 0, p, v, old, minv, maxv, step, page = EDIT_PAGE_GUI;

    if (s_EditOn) return 0;             /* 编辑中滚轮不动数值 */
    if (s_Sel == 0)
    {
        const EscNum* n;

        if (!Esc_HitRight(b, mx, my, &index, &rowTop)) return 0;
        n = Esc_NumFind(index);
        if (n)
        {
            v = n->get();
            minv = n->min;
            maxv = n->max;
            step = n->step;
        }
        else if ((p = Esc_LightCellAt(b, index, rowTop, mx, my)) >= 0)
        {
            index = s_LightItem[p];
            v = Lighting_GetParam(p);
            minv = 0;
            maxv = LIGHT_PARAM_MAX;
            step = 1;
        }
        else return 0;
    }
    else if (s_Sel == 1)
    {
        const EscTextNum* num;

        if (!Esc_TextHitRight(b, mx, my, &index, NULL)) return 0;
        num = Esc_TextNumFind(index);
        if (!num) return 0;
        v = MsgText_GetOption(num->opt);
        minv = num->min;
        maxv = num->max;
        step = num->step;
        page = EDIT_PAGE_TEXT;
    }
    else return 0;

    old = v;
    v += (delta > 0) ? step : -step;
    if (v < minv) v = minv;
    if (v > maxv) v = maxv;
    if (v == old) return 0;
    Esc_ApplyNum(page, index, v);
    Esc_Redraw(h);
    return 1;
}

/* 滚轮: 滚当前页 (设置页也可能长高, 所以两页都滚)。
 * 光标位置用 GetCursorPos 取, 不用 lp —— 滚轮消息可能是引擎转发来的,
 * lp 的坐标系不保证是屏幕坐标, 拿它 ScreenToClient 会出现"点在右面板却判到
 * 左面板"的错位。作用域 = 整个对话框客户区: 无论光标在左右哪个面板上,
 * 滚的都是当前选中的页。 */
static int Esc_PageWheel(HWND h, WPARAM wp, LPARAM lp)
{
    POINT pt;
    RECT rc, a, b;
    const EscPage* pg;
    int delta, old;

    (void)lp;
    if (!GetCursorPos(&pt)) return 0;
    if (!ScreenToClient(h, &pt)) return 0;
    if (!GetClientRect(h, &rc)) return 0;
    if (!EscIn(0, 0, rc.right, rc.bottom, pt.x, pt.y)) return 0;
    if (!Esc_Layout(h, &a, &b)) return 0;

    delta = (int)(short)HIWORD(wp);
    if (delta == 0) return 0;
    /* 光标在数值控件上: 滚轮微调数值, 不滚页 */
    if (Esc_WheelValue(h, &b, pt.x, pt.y, delta)) return 1;
    pg = (s_Sel == 0) ? &kGuiPage : &kTextPage;
    old = *pg->scroll;
    *pg->scroll = Esc_PageSnap(&b, pg, *pg->scroll - (delta / 120) * ESC_ROW_H * 3);
    Esc_PageClampScroll(&b, pg);
    if (*pg->scroll != old) Esc_Redraw(h);
    return 1;
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
        if (!Esc_TextHitRight(right, mx, my, &index, NULL)) return 0;
    }
    else return 0;
    if (index != s_EditItem) return 0;

    s_EditSt.cur = Esc_EditCurAt(right, mx, s_EditBuf, s_EditSt.len, s_EditItem);
    Esc_Redraw(hDlg);
    return 1;
}

/* 拖动条收尾 (松开鼠标 / 菜单隐藏两处共用): 调色比例是松手才提交的, 这里补
 * 提交一次并红画; 其余行拖动中已实时生效。统一复位拖动状态并释放捕获。 */
static void Esc_SlideDrop(HWND h)
{
    if (s_SlideDrag == GUI_ITEM_LIGHT_MODE)
    {
        Esc_ApplyNum(EDIT_PAGE_GUI, s_SlideDrag, s_SlideVal);
        Esc_Redraw(h);
    }
    s_SlideDrag = -1;
    if (GetCapture() == h) ReleaseCapture();
}

static void Esc_Cancel(HWND hDlg)
{
    HWND back;

    if (GetCapture() == hDlg) ReleaseCapture();
    s_CancelDown = 0;
    s_SlideDrag = -1;
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

/* 悬停高亮的重绘触发: 光标在右面板行区内移动 / 进出该区时重绘一次,
 * 让行背景跟随光标 (WM_MOUSEMOVE 的 lp 是标准客户区坐标, 可直接用)。 */
static void Esc_HoverNudge(HWND h, int mx, int my)
{
    RECT a, b;
    static int s_WasIn = 0;
    int in;

    if (!Esc_Layout(h, &a, &b)) return;
    in = EscIn(b.left, b.top, b.right - b.left, b.bottom - b.top, mx, my);
    if (!in && !s_WasIn) return;
    s_WasIn = in;
    Esc_Redraw(h);
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
        if (Esc_TextHitRight(&b, mx, my, &index, &rowY))
        { Esc_TextApply(h, &b, index, mx, my, rowY); return 0; }
        break;
    case WM_MOUSEMOVE:
        if (s_SlideDrag < 0 || s_Sel != 0)
        {
            /* 不在拖动条时: 光标动了就刷悬停高亮 */
            Esc_HoverNudge(h, mx, my);
            break;
        }
        if (Esc_Layout(h, &a, &b))
        {
            int rowL = b.left + ESC_PAD;
            int rowW = (b.right - b.left) - ESC_PAD * 2;
            int minv, maxv, step, trkX, trkW, numX, numW, capX, capW, v;
            int xs[2], ws[2];

            Esc_SlideSpec(s_SlideDrag, &minv, &maxv, &step);
            if (!Esc_ItemEnds(s_SlideDrag))
                Esc_SlideGeom(rowL, rowW, s_GuiLabel[s_SlideDrag], &trkX, &trkW, &numX, &numW);
            else
                Esc_EndBarGeom(rowL, rowW, s_GuiLabel[s_SlideDrag],
                               Esc_ItemEnds(s_SlideDrag), &trkX, &trkW,
                               xs, ws, &capX, &capW);
            v = (s_SlideDrag == GUI_ITEM_GAMESPEED)
                ? Esc_SlideValueAt(mx, trkX, trkW)
                : Esc_TrackValueAt(mx, trkX, trkW, minv, maxv);
            if (v != s_SlideVal)
            {
                s_SlideVal = v;
                /* 调色比例一次提交要重建 3.7 万格, 松开才应用; 其余直接生效 */
                if (s_SlideDrag != GUI_ITEM_LIGHT_MODE)
                    Esc_ApplyNum(EDIT_PAGE_GUI, s_SlideDrag, v);
                Esc_Redraw(h);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (s_SlideDrag >= 0)
        {
            Esc_SlideDrop(h);
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
        s_SlideDrag = -1;
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
        if (s_SlideDrag >= 0) Esc_SlideDrop(h);   /* 拖动中的值照常提交 */
        if (GetCapture() == h) ReleaseCapture();
        Esc_EditCommit(h);
        Esc_FlushPending();
        Esc_SaveSettings();
        break;
    default:
        break;
    }

    r = LiCallOld(h, old, msg, wp, lp);

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
    /* 存档界面的"当前画面"要的是按键那一刻: 这里 (0x4F1140) 对话框刚建好、还没
     * ShowWindow, 视口里仍是刚渲染完的战场。晚一步抓到的就是菜单/对话框。 */
    SaveShot_CaptureNow();

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

    /* 同一个钩点: 存/读档对话框的缩略图 (自己判 HWND, ESC 关掉也照画)。 */
    SaveShot_PaintDialog(regs, (void*)(size_t)*(unsigned*)((char*)regs + ESC_REG_ESI));

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
