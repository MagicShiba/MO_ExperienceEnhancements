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
#include "RallyPath.h"
#include "PiP.h"
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
#include "PlacementPreview.h"
#include "EnvIni.h"
#include "SWPanel.h"
#include "VecText.h"
#include "PowerDisplay.h"
#include "UnitPanel.h"
#include "Favorite.h"
#include "SmartOrder.h"
#include "SmartArea.h"
#include "FuzzyAttack.h"
#include "PatrolCircle.h"
#include "TreeChop.h"
#include "MindSelect.h"
#include "AttackMove.h"
#include "Aggressive.h"
#include "RepairAll.h"
#include "ToolTip.h"
#include "Observer.h"
#include "NoApproach.h"
#include "WeaponRange.h"
#include "LightConvFix.h"
#include "MOEVersion.h"

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
/* 组标题高与组间距: 各页共用一套, 翻页时行距才不会变。 */
#define ESC_GROUP_H          26
#define ESC_BLOCK_GAP        6
#define ESC_WARN_H           26
#define ESC_WARN_GAP         8
#define ESC_TEXT_SS          2
#define ESC_LABEL_CAP        96
#define ESC_LEFT_ROWS        5     /* 页数 = 左栏页名行数 (功能 / 文本 / 显示 / 快捷键 / 游戏控制) */
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

/* 左栏最底部: 链接行 (自上而下 bilibili / GitHub, 末行右侧还有版本号) 与
 * 语言切换。图标不用位图资源, 22x22 遮罩直接写像素 (见 kIconGithub)。 */
#define ESC_LINK_N           2    /* 底部链接行数, 也是 s_LinkText 的下标数 */
#define ESC_FOOT_H           32
#define ESC_FOOT_ICON        22   /* 字形遮罩边长 */
#define ESC_FOOT_PAD         3    /* 圆底比字形每边大这么多 (= 28 直径) */
#define ESC_FOOT_GAP         8    /* 圆底与文字之间 */
#define ESC_GH_URL           L"https://github.com/MagicShiba/MO_ExperienceEnhancements"
#define ESC_BILI_URL         L"https://space.bilibili.com/335956233"
#define ESC_TEXT_CELL_N      3   /* 文本页多格行的格数 (边距行: 左/顶/底) */

#define ESC_SW_W             44
#define ESC_SW_H             20
#define ESC_FLOW_MAX         12      /* 一行最多几个控件原子 (分段最多 8 段) */
/* 开/关状态列按实测字宽 (见 Esc_StateW), 不再留固定 64px: 中英文都是
 * 两三个字符, 定宽等于白扔四十来像素给行标题。 */
#define ESC_STATE_GAP        8
#define ESC_CTL_RIGHT        12
#define ESC_STEP_W           96
#define ESC_STEP_BTN_W       20
#define ESC_STEP_BTN_H       20   /* = W: [-]/[+] 是正方形 (数值框/文本条同高) */
#define ESC_STEP_NUM_PAD     10
/* 热键行的值框: 最长的组合键串 (Ctrl+Shift+Z+Middle) 也放得下, 再长就截断,
 * 截断只影响显示 —— 值的真身始终是 ini 里那一行。 */
#define ESC_HK_W             148
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
    GUI_GROUP_UNIT,     /* 单位面板 (左下选中面板 + 收藏标记) */
    GUI_GROUP_PIP,      /* 小窗口 (第二摄像机画中画) */
    GUI_GROUP_HOTKEY,   /* 快捷键: 下令 (中键系几套) */
    GUI_GROUP_HOTKEY_MISC,  /* 快捷键: 杂项 (选择/移动/侵略) */
    GUI_GROUP_LIGHT,
    GUI_GROUP_APPEAR,   /* 界面外观 (玩家颜色/阵营 UI), 属于显示设置页 */
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
    GUI_ITEM_PLACE_PREV,    /* 放置建筑预览浓度 ([PlacementPreview] Opacity) */
    GUI_ITEM_RALLY,         /* 折线集结点 (RallyPath, 按住 Z 追加尾巴) */
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
    GUI_ITEM_LIGHTFIX,      /* 光照修复总开关 ([Environment] LightFixEnable) */
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
    GUI_ITEM_WTH_CLOUD,     /* 云层一行: [云影|雾气|关闭] 三段互斥 */
    GUI_ITEM_WTH_STYLE,
    GUI_ITEM_CRATE,
    GUI_ITEM_UP_ENABLE,     /* 启用单位面板 ([UnitPanel] Enable) */
    GUI_ITEM_UP_BTNS,       /* 左侧按钮 行标题 */
    GUI_ITEM_UP_OFF,        /* 段 0: 关闭 (三组按钮全关, 面板贴左下角) */
    GUI_ITEM_UP_FOLD,       /* 段 1: 折叠键 */
    GUI_ITEM_UP_WASD,       /* 段 2: WASD 切换键 */
    GUI_ITEM_UP_FAV,        /* 段 3: 收藏槽按钮 */
    GUI_ITEM_FAV_STAR,      /* 收藏星标 (单位头顶的星星) */
    GUI_ITEM_FAV_ARROW,     /* 箭头指示: 不显示 / 边缘 / 中心 */
    /* 小窗口 (PiP): 总开关 / 窗内左键操作 / 自动跟随雷达事件 / 宽 / 高 / 离屏倍率 */
    GUI_ITEM_PIP_ENABLE,
    GUI_ITEM_PIP_OPERATE,
    GUI_ITEM_PIP_AUTO,
    GUI_ITEM_PIP_W,
    GUI_ITEM_PIP_H,
    GUI_ITEM_PIP_SCALE,
    /* 快捷键 (左栏第 4 页): 一行一个热键, 点一下进捕获态改键。落盘位置见
     * s_Hotkeys, 各模块的回读函数也在那张表里。 */
    GUI_ITEM_HK_SMART_ORDER,    /* 中键移动 (划战线) [SmartOrder] Hotkey */
    GUI_ITEM_HK_SMART_AREA,     /* 模糊下令 (分配目标) [SmartArea] Hotkey */
    GUI_ITEM_HK_PATROL,         /* 快速建立巡逻圈 [PatrolCircle] Hotkey */
    GUI_ITEM_HK_TREECHOP,       /* 快速砍树 [TreeChop] Hotkey */
    GUI_ITEM_HK_FUZZY,          /* 模糊攻击 [FuzzyAttack] Hotkey */
    GUI_ITEM_HK_MIND,           /* 选择被心控单位 [Features] MindSelect */
    GUI_ITEM_HK_ATTACKMOVE,     /* 移动攻击 [Features] AttackMove */
    GUI_ITEM_HK_AGGRESSIVE,     /* 侵略模式 [Aggressive] Hotkey */
    /* ---- 建筑组: 快捷维修 (升级自 RepairAll) ---- */
    GUI_ITEM_REPAIR,            /* 关闭 / 自动 / 半自动 ([RepairAll] Mode) */
    /* ---- 侧边栏组: 建造提示追加耗时/耗电 ---- */
    GUI_ITEM_TOOLTIP,           /* [Features] ToolTips */
    /* ---- 小窗口: 宽高一行 (格 = PIP_W / PIP_H) ---- */
    GUI_ITEM_PIP_SIZE,          /* 行标题 "尺寸" */
    /* ---- 游戏控制页 (左栏第 5 页) ---- */
    GUI_ITEM_SAVE_SHOT,         /* 游戏存档缩略图 ([SaveShot] Enable) */
    GUI_ITEM_OBSERVER,          /* 观战修复 ([Features] Observer) */
    GUI_ITEM_SCORE_EXIT,        /* 结算页退出按钮 ([VecText] ScoreExit) */
    GUI_ITEM_WR,                /* 显示单位范围 (Ctrl) 行标题 */
    GUI_ITEM_WR_ON,             /*   格: 启用 ([Features] WeaponRange) */
    GUI_ITEM_WR_H,              /*   格: 对空圈抬高 ([Features] WeaponRangeAAZ) */
    GUI_ITEM_NO_APPROACH,       /* 阻止无攻击力单位靠近目标 ([Features] NoApproach) */
    GUI_ITEM_COUNT
};

enum {
    /* 页首两行说明 (字体文件名 / 常用字号), 灰字不参与交互。 */
    TX_NOTE_FONT = 0,
    TX_NOTE_PX,
    TX_GROUP_CAMP,
    TX_CAMP_ENABLE,
    /* "字体"一行: 行标题 + 字号框 (字号框有自己的 item, 编辑态按 item 记) */
    TX_CAMP_FONT,
    TX_CAMP_FPXSIZE,
    TX_CAMP_LINES,
    TX_CAMP_OPACITY,
    TX_CAMP_POS,
    TX_CAMP_ALIGN,
    TX_CAMP_MARGIN,      /* 边距一行: 左/顶/底 三个数值格 (下面三个是格子 item) */
    TX_CAMP_MARGIN_L,
    TX_CAMP_MARGIN_T,
    TX_CAMP_MARGIN_B,
    TX_CAMP_WIDTH,
    TX_CAMP_ANIM,
    TX_CAMP_BAR,
    TX_GROUP_CHAT,
    TX_CHAT_EDIT,
    TX_CHAT_NAME,
    TX_CHAT_QUICK,       /* 预设文本: 每条一行, 落盘时拼成字面 \n 串 */
    TX_GROUP_HINT,
    TX_HINT_SHOW,
    TX_HINT_FONT,
    TX_HINT_FPXSIZE,
    TX_HINT_OPACITY,
    TX_HINT_COLOR,
    TX_HINT_LINES,
    TX_HINT_WIDTH,
    TX_HINT_BAR,
    TX_HINT_RADAR,
    /* 超武倒计时面板 ([SWPanel]): 开关 / 字体 / 样式 / 底板不透明度 */
    TX_GROUP_SW,
    TX_SW_ENABLE,
    TX_SW_FONT,
    TX_SW_FPXSIZE,
    TX_SW_STYLE,
    TX_SW_OPACITY,
    /* 其它文本 ([VecText]): 全局字体 / 资金 / 电力 / 简报 */
    TX_GROUP_OTHER,
    TX_VT_FONT,
    TX_VT_FPXSIZE,
    TX_SEP5,
    TX_VT_CREDIT,        /* 资金字号与偏移: 两格 */
    TX_VT_CREDIT_PX,
    TX_VT_CREDIT_DY,
    TX_SEP6,
    TX_VT_POWER,         /* 底部电力: 关闭/余量/负载·电力 */
    TX_VT_POWER_MARGIN,  /* 边距: 左 / 底 两格 */
    TX_VT_POWER_X,
    TX_VT_POWER_PAD,
    TX_SEP7,
    TX_VT_BRIEF_OWN,
    TX_VT_BRIEF_WIDTH,
    TX_VT_BRIEF_MARK,
    TX_COUNT
};

static int  s_Init = 0;
static int  s_Enable = 1;
static int  s_Sel = 0;
static int  s_GuiMissing = 0;
/* 本局是否联机 (GameMode 3/4): 面板照常显示, 会分叉的项置灰只读, 见 Esc_MpLocked。 */
static int  s_Mp = 0;
/* 左栏底部: 版本号 + 链接行。链接文案与网址被语言文件覆盖 (初值是兜底)。 */
static wchar_t s_Ver[ESC_LABEL_CAP];
static wchar_t s_LinkText[ESC_LINK_N][ESC_LABEL_CAP] = { L"bilibili", L"GitHub" };
static wchar_t s_LinkUrl[ESC_LINK_N][ESC_LABEL_CAP] = { ESC_BILI_URL, ESC_GH_URL };
static int     s_LinkHot = -1;   /* 鼠标所在链接行, -1 = 不在任何一行 */

static const wchar_t* s_Missing = ESC_MISSING_EN;
static const wchar_t* s_MissingNotice = ESC_MISSING_NOTICE_EN;

/* 菜单里的一段短文案 (段名 / 开关名 / 页名), 32 个 wchar 够放。 */
typedef wchar_t EscName[32];

static HWND s_Host = NULL;
static wchar_t s_LeftTitle[ESC_LABEL_CAP];
static wchar_t s_LeftItem[ESC_LEFT_ROWS][ESC_LABEL_CAP];
static wchar_t s_GuiTitle[ESC_LABEL_CAP];
static wchar_t s_DispTitle[ESC_LABEL_CAP];
static wchar_t s_GuiGroup[GUI_GROUP_COUNT][ESC_LABEL_CAP];
static wchar_t s_GuiLabel[GUI_ITEM_COUNT][ESC_LABEL_CAP];
static wchar_t s_GuiOn[16];
static wchar_t s_GuiOff[16];
static EscName s_GuiSort[2];
static EscName s_GuiSkinName[ESC_SKIN_N];
static EscName s_GuiScatter[ESC_SCATTER_N];
/* 放置预览浓度段名: 首段"关闭"+ 四个数字。都是固定短词, 只有"关闭"要翻译。 */
#define ESC_PLACE_PREV_N 5
static EscName s_GuiPlacePrev[ESC_PLACE_PREV_N] = {
    L"0", L"25", L"50", L"75", L"100"
};
static EscName s_GuiHide[ESC_HIDE_N];
static EscName s_GuiSwsSide[ESC_SWS_SIDE_N];
static EscName s_GuiWthMode[WEATHER_DRAW_N];
/* 云层段: 云影 / 雾气 / 关闭 —— 关也是段, 这一行不再另挂胶囊开关。
 * 段序是显示顺序, 与 WEATHER_CLOUD_* 的取值不同序, 对应关系见 kCldSegMode。 */
#define ESC_CLD_SEG_N  WEATHER_CLOUD_MODE_N
static EscName s_GuiWthLayer[ESC_CLD_SEG_N];
static EscName s_GuiWthStyle[WEATHER_CLOUD_STYLE_N];
/* 光照预设分段: 原图 + 标准/早上/下午/黄昏/晚上 + 自定义 (自定义只是状态)。 */
static EscName s_GuiLight[LIGHT_PRESET_COUNT + 1];
/* 调色行两端快捷段: 左 默认 (比例 0) / 右 触发 (比例满值)。 */
static EscName s_GuiLightMode[2];
/* 矿石光照行两端快捷段: 左 默认 (0, 夜里全亮) / 右 环境 (满值, 跟随环境光)。 */
static EscName s_GuiOreEnds[2];
/* 箭头指示 三段: 不显示 / 边缘 / 中心 (段号 = Fav_ArrowMode 的返回值)。 */
#define ESC_ARROW_N 3
static EscName s_GuiArrow[ESC_ARROW_N];
/* 小窗口离屏倍率两段: 固定写 "1x"/"2x", 两种语言通用, 不进语言文件。 */
static EscName s_GuiPipScale[2] = { L"1x", L"2x" };
/* 快捷维修三段: 关闭 / 自动 / 半自动 (段号与 [RepairAll] Mode 的映射见
 * EscChoiceRepairGet/Apply —— 自动和半自动的顺序与 Mode 值相反)。 */
#define ESC_REPAIR_N 3
static EscName s_GuiRepair[ESC_REPAIR_N];
/* 单位面板左侧按钮 四段 (段名直接取 s_GuiLabel 里连着的那四个 item, 与天气行同做法)。 */
#define ESC_UPBTN_N 4

static wchar_t s_TextTitle[ESC_LABEL_CAP];
static wchar_t s_TextLabel[TX_COUNT][ESC_LABEL_CAP];
static EscName s_TextPos[2];
static EscName s_TextAlign[2];
/* 边距一行的三个小标题 (左/顶/底), 顺序 = 格子顺序。 */
static EscName s_TCellCapMargin[ESC_TEXT_CELL_N];
/* 资金两格 / 电力边距两格 (小标题用同一套"左/顶/底"以外的专用文案)。 */
static EscName s_TCellCapCredit[2];
static EscName s_TCellCapPwr[2];
static const int s_CreditCellItems[2] = { TX_VT_CREDIT_PX, TX_VT_CREDIT_DY };
static const int s_PwrCellItems[2] = { TX_VT_POWER_X, TX_VT_POWER_PAD };
/* 超武面板样式 (统一/原本) 与底部电力模式 (关闭/余量/负载·电力)。 */
static EscName s_TextSwStyle[2];
static EscName s_TextPower[3];
static EscName s_TextBar[3];
static EscName s_TextColor[2];
/* 字体行上的两个字 ("粗体" 开关 / "字号" 小标题)。 */
static EscName s_TextBoldOn;
static EscName s_TextPxCap;
static EscName s_Cancel = L"Cancel";
static int s_TextMissing = 0;
static int s_TextScroll = 0;
static int s_GuiScroll = 0;
static int s_DispScroll = 0;
static int s_KeyScroll = 0;
static int s_CtrlScroll = 0;
static int s_KeyMissing = 0;
static wchar_t s_KeyTitle[ESC_LABEL_CAP];
static wchar_t s_CtrlTitle[ESC_LABEL_CAP];
/* 热键捕获态的一行提示 ("按任意键…"), 与语言文件里的 GuiHkPress 同义。 */
static EscName s_KeyPress;

enum {
    EDIT_PAGE_GUI = 0,
    EDIT_PAGE_TEXT = 1,
    EDIT_PAGE_DISP = 2,     /* 显示设置; 编号 = 左栏页名行下标 */
    EDIT_PAGE_KEYS = 3,     /* 快捷键 */
    EDIT_PAGE_CTRL = 4      /* 游戏控制 (游戏速度 / 存档 / 结算 / 范围圈 / 杂项) */
};

#define ESC_EDIT_CAP 8
#define ESC_EDIT_TIMER 0x4D4Fu
#define ESC_HK_TIMER   0x4D50u

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

/* ---------------- 热键捕获态 ----------------
 * 点一行进捕获: 每 16ms 扫一遍键盘/鼠标, 新按下的非修饰键按顺序攒起来,
 * **全部松开**才结算 (这样 Ctrl+Z+Middle 能等到最后一个键)。Esc 取消。
 * 结算时按 OU_HotkeyText 规范化写回 ini, 再让对应模块回读 —— 这一局立刻生效。 */
static int s_HkCap = -1;              /* 捕获中的 item, -1 = 不在捕获 */
static wchar_t s_HkCapTxt[64];        /* 框里显示的串 (提示或已攒到的组合) */
static unsigned char s_HkPrev[256];   /* 上一轮按下状态 (新按下 = cur && !prev) */
static unsigned char s_HkUsed[256];   /* 本轮已记进组合的键 (判"全松开") */
static unsigned s_HkMods;             /* 攒到的修饰键 */
static unsigned s_HkKeys[8];          /* 攒到的非修饰键 (最后一个是主键) */
static int s_HkNKey;
static unsigned s_HkT0;               /* 最近一次按下的时刻 (超时取消) */

/* ---------------- 设置项归属 ----------------
 * 同一个 opt 编号在各模块里含义不同, 表里用 mod 指明该问谁; 落盘节名也从
 * mod 推 (电力键虽属 PowerDisplay, 但配置住在 [VecText] 里)。 */
enum { OPM_MSG = 0, OPM_SW, OPM_VT, OPM_PWR };

static const char* Esc_OptSection(int mod)
{
    if (mod == OPM_SW) return SWPANEL_SECTION;
    if (mod == OPM_VT || mod == OPM_PWR) return VT_SECTION;
    return MSG_SECTION;
}

static int Esc_OptGet(int mod, int opt)
{
    switch (mod)
    {
    case OPM_SW:  return SWPanel_GetOption(opt);
    case OPM_VT:  return VecText_GetOption(opt);
    case OPM_PWR: return PowerDisplay_GetOption(opt);
    default:      return MsgText_GetOption(opt);
    }
}

static void Esc_OptSet(int mod, int opt, int v)
{
    switch (mod)
    {
    case OPM_SW:  SWPanel_SetOption(opt, v); break;
    case OPM_VT:  VecText_SetOption(opt, v); break;
    case OPM_PWR: PowerDisplay_SetOption(opt, v); break;
    default:      MsgText_SetOption(opt, v); break;
    }
}

/* ---------------- 字体行 ----------------
 * 一行 = 字体名框 + 字号框 + 粗体。四个目标各有一份配置, 选项号与落盘键都
 * 记在表里, 加一行只需往 s_TFonts 添一项。 */
enum { FT_CAMP = 0, FT_HINT, FT_SW, FT_VT, FT_COUNT };
#define ESC_FACE_CAP 64

typedef struct {
    unsigned char item;     /* 行 item (标题) */
    unsigned char px;       /* 字号框 item */
    unsigned char target;   /* FT_*: 决定字体名问哪个模块 */
    unsigned char mod;      /* OPM_*: 字号/粗体的选项归属 */
    unsigned char pxOpt;
    unsigned char boldOpt;
    const char* section;    /* 落盘节 */
    const char* faceKey;
    const char* boldKey;
} EscTextFont;

static const EscTextFont s_TFonts[] = {
    { TX_CAMP_FONT, TX_CAMP_FPXSIZE, FT_CAMP,
      OPM_MSG, MSG_OPT_CAMP_FONT_PX, MSG_OPT_CAMP_BOLD,
      MSG_SECTION, "FontFace", "Bold" },
    { TX_HINT_FONT, TX_HINT_FPXSIZE, FT_HINT,
      OPM_MSG, MSG_OPT_HINT_FONT_PX, MSG_OPT_HINT_BOLD,
      MSG_SECTION, "HintFontFace", "HintBold" },
    { TX_SW_FONT, TX_SW_FPXSIZE, FT_SW,
      OPM_SW, SWO_FONT_PX, SWO_BOLD, SWPANEL_SECTION, "FontFace", "Bold" },
    { TX_VT_FONT, TX_VT_FPXSIZE, FT_VT,
      OPM_VT, VTO_FONT_PX, VTO_BOLD, VT_SECTION, "FontFace", "Bold" }
};

static const EscTextFont* Esc_TextFontFind(int item)
{
    int i;
    for (i = 0; i < (int)(sizeof(s_TFonts) / sizeof(s_TFonts[0])); i++)
        if (s_TFonts[i].item == item) return &s_TFonts[i];
    return NULL;
}

/* 字号框 item -> 所属字体行 (数值框定位要按行几何算)。 */
static const EscTextFont* Esc_TextFontForPx(int item)
{
    int i;
    for (i = 0; i < (int)(sizeof(s_TFonts) / sizeof(s_TFonts[0])); i++)
        if (s_TFonts[i].px == item) return &s_TFonts[i];
    return NULL;
}

static const EscTextFont* Esc_TextFontByTarget(int t)
{
    int i;
    for (i = 0; i < (int)(sizeof(s_TFonts) / sizeof(s_TFonts[0])); i++)
        if (s_TFonts[i].target == t) return &s_TFonts[i];
    return NULL;
}

static void Esc_FaceGet(int t, wchar_t* out, int cap)
{
    switch (t)
    {
    case FT_HINT: MsgText_GetFace(1, out, cap); break;
    case FT_SW:   SWPanel_GetFace(out, cap);    break;
    case FT_VT:   VecText_GetFace(out, cap);    break;
    default:      MsgText_GetFace(0, out, cap); break;
    }
}

static void Esc_FacePut(int t, const wchar_t* face)
{
    switch (t)
    {
    case FT_HINT: MsgText_SetFace(1, face); break;
    case FT_SW:   SWPanel_SetFace(face);    break;
    /* 底电的覆盖位图按 [VecText] 的字面生成, 换字后要重建。 */
    case FT_VT:   VecText_SetFace(face); PowerDisplay_Invalidate(); break;
    default:      MsgText_SetFace(0, face); break;
    }
}

/* 字符串值 (字体名) 的落盘队列: 同一个键只留最后一次。
 * val 按 UTF-8 最坏情况给 (ESC_FACE_CAP 个 wchar 全落在三字节区), 名字被截断
 * 写进 ini 就等于把字体改成了一个不存在的名字。 */
typedef struct {
    const char* section;
    const char* key;
    char val[ESC_FACE_CAP * 4];
} EscPendStr;

static EscPendStr s_PendStr[8];
static int s_PendStrN = 0;

static void Esc_QueueStr(const char* section, const char* key, const char* val)
{
    int i;

    for (i = 0; i < s_PendStrN; i++)
    {
        if (lstrcmpA(s_PendStr[i].section, section) == 0 &&
            lstrcmpA(s_PendStr[i].key, key) == 0)
        {
            lstrcpynA(s_PendStr[i].val, val, (int)sizeof(s_PendStr[i].val));
            return;
        }
    }
    if (s_PendStrN >= (int)(sizeof(s_PendStr) / sizeof(s_PendStr[0]))) return;
    s_PendStr[s_PendStrN].section = section;
    s_PendStr[s_PendStrN].key = key;
    lstrcpynA(s_PendStr[s_PendStrN].val, val, (int)sizeof(s_PendStr[0].val));
    s_PendStrN++;
}

/* ---- 预设文本 (文本页 EG_TEXT 行) ----
 * ini 里仍是**一行** ([MsgText] QuickText, 条目之间写字面 \n), 但设置页按条目
 * 拆开逐行显示: 每条一行 [框][-][+], 屏幕上永远看不到 \n。[-] 删掉这条,
 * [+] 在这条后面插一条空条目并直接进入编辑。
 * 编辑是**就地**的 —— LiEdit 直接绑在该条目的缓冲上, 敲字即刻生效 (聊天框可查),
 * 字符只能从对话框消息来 (WM_CHAR/WM_IME_CHAR), 移动/删除走按键轮询。
 * 条目上限与 ChatEdit 的 CE_PRESET_N/CE_BUF_N 对齐, 串长受 ESC_QUICK_RAW 约束
 * (聊天框那边也只读这么多字节, 超了写出去读不回来)。 */
#define ESC_QUICK_N     24                  /* = ChatEdit 的 CE_PRESET_N */
#define ESC_QUICK_LINE  161                 /* = CE_BUF_N (含结尾 0) */
#define ESC_QUICK_RAW   12288               /* ini 串字节上限 (UTF-8) */
#define ESC_QUICK_GAP   6                   /* 条目框与 [-] 之间的缝 */
#define ESC_QUICK_JOIN  (ESC_QUICK_N * ESC_QUICK_LINE + 64)
static wchar_t s_QItem[ESC_QUICK_N][ESC_QUICK_LINE];    /* 已提交条目 */
static int     s_QCount = 0;                            /* 1..ESC_QUICK_N */
static wchar_t s_QItemBak[ESC_QUICK_N][ESC_QUICK_LINE]; /* 打开菜单时留底 */
static int     s_QCountBak = 1;
static int     s_QEdit = -1;               /* 正在编辑的条目下标 */
static LiEdit  s_QSt;                      /* 光标/选区 (buf = 该条目) */
static int     s_QOn = 0;
/* 同一个编辑会话兼做字体名框: 0=预设文本条目, 1=字体名 (s_FaceBuf)。 */
#define QK_PRESET 0
#define QK_FACE   1
static int     s_QKind = QK_PRESET;
static int     s_QFaceTarget = -1;
static wchar_t s_FaceBuf[ESC_FACE_CAP];
static int     s_QBakOk = 0;
static char    s_PendQuick[ESC_QUICK_RAW + 16];
static int     s_PendQuickDirty = 0;

static int Esc_EditIs(int page, int item)
{
    return s_EditOn && s_EditPage == page && s_EditItem == item;
}

/* 宽串 → UTF-8 字节串 (主 ini 是 UTF-8; 代理对不展开, 预设文本用不到)。 */
static void Esc_WideToUtf8(char* dst, int cap, const wchar_t* s)
{
    int n = 0;

    for (; s && *s && n < cap - 4; s++)
    {
        unsigned c = (unsigned)*s;

        if (c < 0x80u) dst[n++] = (char)c;
        else if (c < 0x800u)
        {
            dst[n++] = (char)(0xC0u | (c >> 6));
            dst[n++] = (char)(0x80u | (c & 0x3Fu));
        }
        else
        {
            dst[n++] = (char)(0xE0u | (c >> 12));
            dst[n++] = (char)(0x80u | ((c >> 6) & 0x3Fu));
            dst[n++] = (char)(0x80u | (c & 0x3Fu));
        }
    }
    if (cap > 0) dst[(n < cap) ? n : cap - 1] = 0;
}

/* 条目 → 一个字面 \n 分隔的串 (空条目跳过; 拆法见 Esc_QuickLoad, 两边口径一致)。 */
static int Esc_QuickJoin(wchar_t* dst, int cap)
{
    int i, n = 0, any = 0;

    for (i = 0; i < s_QCount; i++)
    {
        const wchar_t* s = s_QItem[i];

        if (!s[0]) continue;
        if (any)
        {
            if (n + 2 >= cap) break;
            dst[n++] = L'\\';
            dst[n++] = L'n';
        }
        any = 1;
        while (*s && n < cap - 1) dst[n++] = *s++;
    }
    if (cap > 0) dst[(n < cap) ? n : cap - 1] = 0;
    return n;
}

/* 条目改动 → 立刻同步给聊天框; dirty=1 时同时排队等关菜单落盘
 * (取消恢复只回灌聊天框, 不能把恢复值当改动写回去)。 */
static void Esc_QuickSync(int dirty)
{
    static wchar_t join[ESC_QUICK_JOIN];

    if (s_QKind == QK_FACE)
    {
        /* 字体名: 立刻重解析 (下一帧就换字), 落盘排进字符串队列。 */
        static char enc[ESC_FACE_CAP * 4];
        const EscTextFont* ft = Esc_TextFontByTarget(s_QFaceTarget);

        if (!ft) return;
        Esc_WideToUtf8(enc, sizeof(enc), s_FaceBuf);
        Esc_FacePut(ft->target, s_FaceBuf);
        if (dirty) Esc_QueueStr(ft->section, ft->faceKey, enc);
        return;
    }
    Esc_QuickJoin(join, ESC_QUICK_JOIN);
    if (dirty)
    {
        Esc_WideToUtf8(s_PendQuick, sizeof(s_PendQuick), join);
        s_PendQuickDirty = 1;
    }
    ChatEdit_SetQuickText(join);
}

/* 加一条之后串会有多长 (UTF-8 字节)。聊天框只读 ESC_QUICK_RAW 字节,
 * 超了就不让加 —— 写出去读不回来等于静默丢条目。
 * 计数用 volatile 指针: 普通循环会被编成 libc 的 strlen, 而本 DLL 不带 CRT。 */
static int Esc_QuickUtf8Len(void)
{
    static wchar_t join[ESC_QUICK_JOIN];
    static char enc[ESC_QUICK_RAW + 16];
    volatile const char* p = enc;

    Esc_QuickJoin(join, ESC_QUICK_JOIN);
    Esc_WideToUtf8(enc, sizeof(enc), join);
    while (*p) p++;
    return (int)(p - enc);
}

/* [MsgText] QuickText → 条目表。拆法与 ChatEdit 的 CE_PresetSplit 一致:
 * 字面 \n / 真换行 / \r 都算分隔, 两端空白去掉, 空条目跳过。
 * 一条都没有时留一个空条目当输入口 (屏幕上至少有一个框, 不然没有地方点)。
 * 主 ini 是 UTF-8; 万一被写成 ANSI 就退回 ACP (读法同 ChatEdit)。 */
static void Esc_QuickLoad(void)
{
    static char raw[ESC_QUICK_RAW];
    static wchar_t w[ESC_QUICK_RAW];
    const wchar_t* s;
    const wchar_t* p;
    int bad = 0;

    OU_ReadString(MSG_SECTION, "QuickText", "", raw, sizeof(raw));
    Lang_Utf8ToWide(w, w + ESC_QUICK_RAW, (const unsigned char*)raw);
    for (p = w; *p; p++) if (*p == (wchar_t)0xFFFD) { bad = 1; break; }
    if (bad) MultiByteToWideChar(CP_ACP, 0, raw, -1, w, ESC_QUICK_RAW);

    s_QCount = 0;
    s = w;
    while (s && *s && s_QCount < ESC_QUICK_N)
    {
        wchar_t* dst = s_QItem[s_QCount];
        int len = 0;

        while (*s && *s != L'\r' && *s != L'\n' &&
               !(*s == L'\\' && s[1] == L'n'))
        {
            if (len < ESC_QUICK_LINE - 1) dst[len++] = *s;
            s++;
        }
        if (*s == L'\\' && s[1] == L'n') s += 2;
        else if (*s) s++;
        while (len > 0 && (dst[0] == L' ' || dst[0] == L'\t'))
        {
            int i;
            for (i = 0; i < len - 1; i++) dst[i] = dst[i + 1];
            len--;
        }
        while (len > 0 && (dst[len - 1] == L' ' || dst[len - 1] == L'\t')) len--;
        dst[len] = 0;
        if (len <= 0) continue;
        s_QCount++;
    }
    if (s_QCount < 1)
    {
        s_QItem[0][0] = 0;
        s_QCount = 1;
    }
}

/* 只在第一次要画/要快照时读一次 ini。 */
static void Esc_QuickEnsure(void)
{
    static int once = 0;

    if (once) return;
    once = 1;
    Esc_QuickLoad();
}

typedef struct {
    int scale, pause, grid, shot, sws, fill, rows, skin, color;
    int bq, scatter, placePrev, sideScroll, hide, swsSide, rally;
    int chatEdit, chatName, speed, speedOn, ore;
    int wthMode, wthRain, wthSnow, wthFire, wthThunder, wthCloud, wthStyle;
    int upEnable, favStars, favArrow;
    int upBtn[3];           /* 折叠 / WASD / 收藏 (带下标的 get/set, 不进 X 宏) */
    int pipEnable, pipOperate, pipAuto, pipW, pipH, pipScale;
    int repair;             /* 快捷维修 0/1/2 原值 ([RepairAll] Mode) */
    int tooltip, saveShot, observer, noApproach;   /* 各模块总开关 */
    int wrOn, wrH;          /* 显示单位范围: 开关 + 对空圈抬高 */
    int lightFix;           /* 光照修复 ([Environment] LightFixEnable) */
    /* 结算页退出按钮走 VecText 选项 (VTO_SCORE_EXIT), 由 VecText_SaveOptions
     * 统一快照/回滚, 不进这张表。 */
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
    X(placePrev, PlacementPreview_Opacity, PlacementPreview_SetOpacity) \
    X(sideScroll, SS_Enabled, SS_SetEnabled) \
    X(hide, FM_HideState, FM_SetHideState) \
    X(swsSide, SWS_SideRight, SWS_SetSideRight) \
    X(rally, RallyPath_Enabled, RallyPath_SetEnabled) \
    X(chatEdit, ChatEdit_Enabled, ChatEdit_SetEnabled) \
    X(chatName, ChatEdit_ShowNameSP, ChatEdit_SetShowNameSP) \
    X(speed, GameSpeed_GetFps, GameSpeed_SetFps) \
    X(speedOn, GameSpeed_Enabled, GameSpeed_SetEnabled) \
    X(ore, OreLight_Factor, OreLight_SetFactor) \
    X(wthMode, Weather_DrawMode, Weather_SetDrawMode) \
    X(wthRain, Weather_Rain, Weather_SetRain) \
    X(wthSnow, Weather_Snow, Weather_SetSnow) \
    X(wthFire, Weather_Fire, Weather_SetFire) \
    X(wthThunder, Weather_Thunder, Weather_SetThunder) \
    X(wthCloud, Weather_CloudMode, Weather_SetCloudMode) \
    X(wthStyle, Weather_CloudStyle, Weather_SetCloudStyle) \
    X(upEnable, UP_Enabled, UP_SetEnabled) \
    X(favStars, Fav_StarsEnabled, Fav_SetStarsEnabled) \
    X(favArrow, Fav_ArrowMode, Fav_SetArrowMode) \
    X(pipEnable, PiP_Enabled, PiP_SetEnabled) \
    X(pipOperate, PiP_Operate, PiP_SetOperate) \
    X(pipAuto, PiP_AutoJump, PiP_SetAutoJump) \
    X(pipW, PiP_Width, PiP_SetWidth) \
    X(pipH, PiP_Height, PiP_SetHeight) \
    X(pipScale, PiP_Scale, PiP_SetScale) \
    X(repair, RepairAll_Mode, RepairAll_SetMode) \
    X(tooltip, ToolTip_Enabled, ToolTip_SetEnabled) \
    X(saveShot, SaveShot_Enabled, SaveShot_SetEnabled) \
    X(observer, Observer_Enabled, Observer_SetEnabled) \
    X(noApproach, NoApproach_Enabled, NoApproach_SetEnabled) \
    X(wrOn, WeaponRange_Enabled, WeaponRange_SetEnabled) \
    X(wrH, WeaponRange_Height, WeaponRange_SetHeight) \
    X(lightFix, LightFix_Enabled, LightFix_SetEnabled)

static EscSavedSettings s_Saved;
static int s_SavedOk = 0;
static int s_CancelDown = 0;
static int s_SlideDrag = -1;  /* 拖动条按住中的 item, -1 = 无 */
static int s_SlidePage = EDIT_PAGE_GUI; /* 拖动条所在页 (GUI / 显示 / 游戏控制) */
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
    "GuiGroupUnit", "GuiGroupPip", "GuiGroupHotkey", "GuiGroupHotkeyMisc",
    "GuiGroupLight", "GuiGroupAppear"
};
/* 快捷维修的段名 (显示顺序 = s_GuiRepair 下标) */
static const char* const kRepairKey[ESC_REPAIR_N] = {
    "GuiRepairOff", "GuiRepairAuto", "GuiRepairHalf"
};
static const char* const kLabelKey[GUI_ITEM_COUNT] = {
    "GuiScale", "GuiPause", "GuiGrid", "GuiShot", "GuiSws", "GuiSort",
    "GuiRows", "GuiSkin", "GuiColor", "GuiBuildQueue", "GuiScatter",
    "GuiPlacePrev", "GuiRally",
    "GuiSideScroll", "GuiSideHide", "GuiSwsSide", "GuiGameSpeedOn",
    "GuiGameSpeed", "GuiFps",
    "GuiLightFix",
    "GuiLightPreset", "GuiLightMode", "GuiLightParm", "GuiLightAmbient",
    "GuiLightLevel", "GuiLightGround", NULL, "GuiLightRed",
    "GuiLightGreen", "GuiLightBlue", "GuiOreLight", "GuiWthMode", "GuiWthTrio",
    "GuiWthRain", "GuiWthSnow", "GuiWthFire", "GuiWthThunder", "GuiWthCloud",
    "GuiWthStyle",
    "GuiCrate",
    "GuiUpEnable", "GuiUpBtns", "GuiUpBtnOff", "GuiUpBtnFold",
    "GuiUpBtnWasd", "GuiUpBtnFav", "GuiFavStar", "GuiFavArrow",
    "GuiPipEnable", "GuiPipOperate", "GuiPipAuto", "GuiPipW", "GuiPipH",
    "GuiPipScale",
    "GuiHkSmartOrder", "GuiHkSmartArea", "GuiHkPatrol", "GuiHkTreeChop",
    "GuiHkFuzzy", "GuiHkMind", "GuiHkAttackMove", "GuiHkAggressive",
    "GuiRepair", "GuiTooltip", "GuiPipSize", "GuiSaveShot", "GuiObserver",
    "GuiScoreExit", "GuiWeaponRange", "GuiWeaponRangeOn", "GuiWeaponRangeH",
    "GuiNoApproach"
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
static const char* const kArrowKey[ESC_ARROW_N] = {
    "GuiFavArrowNone", "GuiFavArrowEdge", "GuiFavArrowCenter"
};
static const char* const kWeatherKey[WEATHER_DRAW_N] = {
    "GuiWthImage", "GuiWthParticle"
};
/* 云层段名 (显示顺序) 与对应的模式值 */
static const char* const kCldLayerKey[ESC_CLD_SEG_N] = {
    "GuiWthCloud", "GuiWthFog", "GuiWthOff"
};
static const int kCldSegMode[ESC_CLD_SEG_N] = {
    WEATHER_CLOUD_SHADOW, WEATHER_CLOUD_FOG, WEATHER_CLOUD_OFF
};
/* 云影风格名 (与 Weather.cpp 的 kCldStyle 顺序一致) */
static const char* const kCldStyleKey[WEATHER_CLOUD_STYLE_N] = {
    "GuiWthStyle0", "GuiWthStyle1", "GuiWthStyle2", "GuiWthStyle3",
    "GuiWthStyle4", "GuiWthStyle5", "GuiWthStyle6", "GuiWthStyle7"
};
static const char* const kTextKey[TX_COUNT] = {
    /* 0 页首说明 */
    "TextFontNote1", "TextFontNote2",
    "TextGroupCamp", "TextCampEnable",
    /* 字体行: 行标题 + 字号 (无独立文案) */
    "TextFont", NULL,
    "TextCampLines", "TextCampOpacity", "TextCampPos", "TextCampAlign",
    "TextCampMargin", NULL, NULL, NULL,
    "TextCampWidth", "TextCampAnim", "TextCampBar",
    "TextGroupChat", "TextChatEdit", "TextChatName", "TextChatQuick",
    "TextGroupHint", "TextHintShow",
    "TextFont", NULL,
    "TextHintOpacity", "TextHintColor", "TextHintLines", "TextHintWidth",
    "TextHintBar", "TextHintRadar",
    "TextGroupSW", "TextSwEnable",
    "TextFont", NULL,
    "TextSwStyle", "TextSwOpacity",
    "TextGroupOther",
    "TextFont", NULL,
    NULL,
    "TextVtCredit", NULL, NULL,
    NULL,
    "TextVtPower", "TextVtPowerMargin", NULL, NULL,
    NULL,
    "TextVtBriefOwn", "TextVtBriefWidth", "TextVtBriefMark"
};
static const char* const kPosKey[2] = { "TextCampPosBottom", "TextCampPosTop" };
static const char* const kAlignKey[2] = { "TextAlignLeft", "TextAlignCenter" };
static const char* const kBarKey[3] = { "TextBarNone", "TextBarLeft", "TextBarRight" };
static const char* const kColorKey[2] = { "TextColorWhite", "TextColorPlayer" };
/* 边距一行的小标题 (左/顶/底) */
static const char* const kCellCapKey[ESC_TEXT_CELL_N] = {
    "TextMarginLeft", "TextMarginTop", "TextMarginBottom"
};
static const char* const kSwStyleKey[2] = { "TextSwStyle0", "TextSwStyle1" };
static const char* const kPowerKey[3] = {
    "TextVtPower0", "TextVtPower1", "TextVtPower2"
};
static const char* const kCreditCapKey[2] = { "TextVtCreditPx", "TextVtCreditDy" };
static const char* const kPwrCapKey[2] = { "TextVtPowerLeft", "TextVtPowerBottom" };

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

/* "v" + MOE_VERSION 转宽字符。版本号只有一份定义 (MOEVersion.h), 面板不另写。 */
static void Esc_BuildVersion(void)
{
    int i;

    s_Ver[0] = L'v';
    for (i = 0; MOE_VERSION[i] && i < ESC_LABEL_CAP - 2; i++)
        s_Ver[i + 1] = (wchar_t)MOE_VERSION[i];
    s_Ver[i + 1] = 0;
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
    {
        /* 页名键 LeftItem1..N: 页数改 ESC_LEFT_ROWS 即可, 不用逐条加语句。 */
        char key[16];
        int i;

        for (i = 0; i < ESC_LEFT_ROWS; i++)
        {
            lstrcpynA(key, "LeftItem", sizeof(key));
            UtoA((unsigned)(i + 1), key + 8);
            Esc_LoadText(key, s_LeftItem[i], ESC_LABEL_CAP);
        }
    }
    if (!Esc_LoadText("Cancel", s_Cancel, 32))
        lstrcpynW(s_Cancel, Lang_IsChinese() ? L"取消" : L"Cancel", 32);

    s_GuiMissing = s_TextMissing = s_KeyMissing = 0;
    Esc_GuiText("GuiTitle", s_GuiTitle, ESC_LABEL_CAP);
    Esc_GuiText("DispTitle", s_DispTitle, ESC_LABEL_CAP);
    Esc_GuiText("CtrlTitle", s_CtrlTitle, ESC_LABEL_CAP);
    if (!Esc_LoadText("KeyTitle", s_KeyTitle, ESC_LABEL_CAP)) s_KeyMissing = 1;
    if (!Esc_LoadText("GuiHkPress", s_KeyPress, 32))
        lstrcpynW(s_KeyPress, Lang_IsChinese() ? L"按键…" : L"Press a key", 32);
    ESC_LOAD_FAM(kGroupKey, s_GuiGroup, 0);
    ESC_LOAD_FAM(kLabelKey, s_GuiLabel, 0);
    Esc_GuiText("GuiOn", s_GuiOn, 16);
    Esc_GuiText("GuiOff", s_GuiOff, 16);
    Esc_GuiText("GuiSortFaction", s_GuiSort[0], 32);
    Esc_GuiText("GuiSortFill", s_GuiSort[1], 32);
    ESC_LOAD_FAM(kSkinKey, s_GuiSkinName, 0);
    ESC_LOAD_FAM(kScatterKey, s_GuiScatter, 0);
    ESC_LOAD_FAM(kRepairKey, s_GuiRepair, 0);
    ESC_LOAD_FAM(kHideKey, s_GuiHide, 0);
    ESC_LOAD_FAM(kSwsSideKey, s_GuiSwsSide, 0);
    ESC_LOAD_FAM(kWeatherKey, s_GuiWthMode, 0);
    ESC_LOAD_FAM(kCldLayerKey, s_GuiWthLayer, 0);
    ESC_LOAD_FAM(kCldStyleKey, s_GuiWthStyle, 0);
    ESC_LOAD_FAM(kLightKey, s_GuiLight, 0);
    ESC_LOAD_FAM(kLightModeKey, s_GuiLightMode, 0);
    ESC_LOAD_FAM(kOreEndKey, s_GuiOreEnds, 0);
    ESC_LOAD_FAM(kArrowKey, s_GuiArrow, 0);

    Esc_TextText("TextTitle", s_TextTitle, ESC_LABEL_CAP);
    ESC_LOAD_FAM(kTextKey, s_TextLabel, 1);
    ESC_LOAD_FAM(kPosKey, s_TextPos, 1);
    ESC_LOAD_FAM(kAlignKey, s_TextAlign, 1);
    ESC_LOAD_FAM(kBarKey, s_TextBar, 1);
    ESC_LOAD_FAM(kColorKey, s_TextColor, 1);
    Esc_TextText("TextBoldOn", s_TextBoldOn, 32);
    Esc_TextText("TextFontPx", s_TextPxCap, 32);
    ESC_LOAD_FAM(kCellCapKey, s_TCellCapMargin, 1);
    ESC_LOAD_FAM(kSwStyleKey, s_TextSwStyle, 1);
    ESC_LOAD_FAM(kPowerKey, s_TextPower, 1);
    ESC_LOAD_FAM(kCreditCapKey, s_TCellCapCredit, 1);
    ESC_LOAD_FAM(kPwrCapKey, s_TCellCapPwr, 1);
    /* 放置预览首段 (0) 显示"关闭", 其余是数字 */
    Esc_GuiText("GuiPlacePrevOff", s_GuiPlacePrev[0], 32);
    /* 底部链接: 语言文件缺键时保留初值 */
    Esc_LoadText("GuiBilibili", s_LinkText[0], ESC_LABEL_CAP);
    Esc_LoadText("GuiBilibiliUrl", s_LinkUrl[0], ESC_LABEL_CAP);
    Esc_LoadText("GuiGithub", s_LinkText[1], ESC_LABEL_CAP);
    Esc_LoadText("GuiGithubUrl", s_LinkUrl[1], ESC_LABEL_CAP);
    Esc_BuildVersion();
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

/* 左栏底部的纵坐标, 绘制与命中共用 (从下往上排):
 *   语言切换 (最后一行) -> 链接行 (它上面 ESC_LINK_N 行) -> 一条分隔线。
 * 页名列表到分隔线为止, 见 Esc_Panel / Esc_HitLeft。 */
static int Esc_LangY(const RECT* p)
{
    return p->bottom - ESC_PAD - ESC_ROW_H;
}

/* 链接区整块的顶 (语言行之上一条分隔线再加 ESC_LINK_N 行)。页名列表到此为止。 */
static int Esc_FootY(const RECT* p)
{
    return Esc_LangY(p) - ESC_SEP_GAP - ESC_FOOT_H * ESC_LINK_N;
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
                         int page, int item);

static void Esc_EditBegin(HWND hDlg, const RECT* right, int mx, int page,
                          int item, int value, int minValue, int maxValue)
{
    int i;

    /* 先 init (会清缓冲) 再填数字, 顺序反了数字会被 LiEdInit 清掉 */
    LiEdInit(&s_EditSt, s_EditBuf, ESC_EDIT_CAP);
    Esc_SignedToW(s_EditBuf, value, ESC_EDIT_CAP);
    s_EditSt.len = lstrlenW(s_EditBuf);
    s_EditSt.cur = Esc_EditCurAt(right, mx, s_EditBuf, s_EditSt.len, page, item);
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

/* ---------------- 预设文本编辑会话 ----------------
 * 与数字框互斥: 开一个先收另一个。字符只能从对话框消息来 (见 Esc_DlgProc),
 * 光标/选区/粘贴复用 LineEdit 的 LiEdit。 */

static int  Esc_TextWidth(const wchar_t* s, int px, int slot);
static void Esc_EditEnd(HWND hDlg);
static void Esc_EditCommit(HWND hDlg);

/* 光标可见窗口的起点: 从 start 起到光标处的宽度不超过框宽的最小 start。
 * 宽度随 start 单调不增, 二分 (每次测量都要过一遍 GDI, 不能逐字试)。 */
static int Esc_QuickStart(const wchar_t* s, int curl, int innerW, int wCur)
{
    int lo = 0, hi = curl, mid;

    if (innerW <= 0) return curl;
    if (Esc_TextWidth(s, ESC_BODY_PX, ESC_BODY_SLOT) - wCur <= innerW) return 0;
    while (lo < hi)
    {
        mid = lo + (hi - lo) / 2;
        if (Esc_TextWidth(s + mid, ESC_BODY_PX, ESC_BODY_SLOT) - wCur <= innerW)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* 点击定位: px 为文本起点起的像素 (已含滚动补偿), 返回最近的字符边界
 * (与数字框 Esc_EditCurAt 同一口径: 逐字符累加, 取最近的那个缝)。 */
static int Esc_QuickCurAt(const wchar_t* s, int len, int px)
{
    int i, acc = 0, best = 0, bestDist = 0x7FFFFFFF;

    for (i = 0; i <= len; i++)
    {
        int d = px - acc;

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

/* 当前显示窗口的起始字符 (与 Esc_QuickBox 同一算法, 供点击定位换算用)。 */
static int Esc_QuickView(const wchar_t* s, int cur, int innerW)
{
    if (cur <= 0 || innerW <= 0) return 0;
    return Esc_QuickStart(s, cur, innerW,
                          Esc_TextWidth(s + cur, ESC_BODY_PX, ESC_BODY_SLOT));
}

/* 就地编辑共用启动: 绑定已有内容, LiEdInit 会清空缓冲, 不能用于已有设置。 */
static void Esc_LineBegin(HWND hDlg, wchar_t* buf, int cap, int curAt)
{
    int len = lstrlenW(buf);

    LiEdBind(&s_QSt, buf, cap, len);
    if (curAt >= 0)               /* < 0 = 光标放末尾 (LiEdBind 已设) */
        s_QSt.cur = (curAt < len) ? curAt : len;
    LiEdTouch(&s_QSt);            /* 刚点进来光标要立刻可见 */
    s_QOn = 1;
    s_EditHost = hDlg;
    LiKeysReset(s_EditKeys, (int)(sizeof(s_EditKeys) / sizeof(s_EditKeys[0])), 1);
    SetTimer(hDlg, ESC_EDIT_TIMER, 16, NULL);
    Esc_Redraw(hDlg);
}

/* 进入第 idx 条的编辑 (就地: LiEdit 直接绑该条缓冲, 敲字即改)。 */
static void Esc_QuickBegin(HWND hDlg, int idx, int curAt)
{
    Esc_QuickEnsure();
    Esc_EditEnd(NULL);
    if (idx < 0 || idx >= s_QCount) return;
    s_QKind = QK_PRESET;
    s_QFaceTarget = -1;
    s_QEdit = idx;
    Esc_LineBegin(hDlg, s_QItem[idx], ESC_QUICK_LINE, curAt);
}

/* 进入字体名框的编辑 (就地: 绑 s_FaceBuf, 敲字立刻 SetFace + 排队落盘)。 */
static void Esc_FaceBegin(HWND hDlg, int target, int curAt)
{
    Esc_EditEnd(NULL);
    Esc_FaceGet(target, s_FaceBuf, ESC_FACE_CAP);
    s_QKind = QK_FACE;
    s_QFaceTarget = target;
    s_QEdit = -1;
    Esc_LineBegin(hDlg, s_FaceBuf, ESC_FACE_CAP, curAt);
}

/* 收掉编辑态 (提交与取消共用): 停定时器 + 重画。内容已经就地写好, 不用回拷。 */
static void Esc_QuickClose(HWND hDlg)
{
    if (s_EditHost) KillTimer(s_EditHost, ESC_EDIT_TIMER);
    s_QOn = 0;
    s_QEdit = -1;
    s_QKind = QK_PRESET;
    s_QFaceTarget = -1;
    Esc_Redraw(hDlg);
}

static void Esc_QuickCommit(HWND hDlg)
{
    if (!s_QOn) return;
    Esc_QuickSync(1);
    Esc_QuickClose(hDlg);
}

/* [+] 在第 after 条之后插一条空条目, 直接进入编辑。满员或串太长就不加。 */
static void Esc_QuickAdd(HWND hDlg, int after)
{
    int i, k;

    Esc_QuickEnsure();
    if (s_QCount >= ESC_QUICK_N) return;
    if (Esc_QuickUtf8Len() >= ESC_QUICK_RAW - 8) return;
    if (after >= s_QCount) after = s_QCount - 1;
    if (after < -1) after = -1;
    for (i = s_QCount; i > after + 1; i--)
        for (k = 0; k < ESC_QUICK_LINE; k++) s_QItem[i][k] = s_QItem[i - 1][k];
    for (k = 0; k < ESC_QUICK_LINE; k++) s_QItem[after + 1][k] = 0;
    s_QCount++;
    Esc_QuickBegin(hDlg, after + 1, -1);
}

/* [-] 删掉第 idx 条; 删空了留一个空条目当输入口 (与加载时口径一致)。 */
static void Esc_QuickDel(HWND hDlg, int idx)
{
    int i, k;

    Esc_QuickEnsure();
    if (idx < 0 || idx >= s_QCount) return;
    if (s_QOn) Esc_QuickClose(hDlg);   /* 缓冲要移动, 编辑态一律先收 */
    for (i = idx; i < s_QCount - 1; i++)
        for (k = 0; k < ESC_QUICK_LINE; k++) s_QItem[i][k] = s_QItem[i + 1][k];
    s_QCount--;
    for (k = 0; k < ESC_QUICK_LINE; k++) s_QItem[s_QCount][k] = 0;
    if (s_QCount < 1) s_QCount = 1;
    Esc_QuickSync(1);
    Esc_Redraw(hDlg);
}

static void Esc_QuickChar(wchar_t ch)
{
    if (!s_QOn) return;
    if (ch < 32 || ch == 127) return;   /* 回车/退格等控制字归轮询 */
    if (LiEdInsert(&s_QSt, &ch, 1, ESC_QUICK_LINE - 1, NULL, NULL))
        Esc_QuickSync(1);
}

/* 轮询: 只处理"到不了对话框"的键 (移动/删除/回车/Esc) 与 Ctrl+V。 */
static void Esc_QuickPoll(HWND hDlg)
{
    static int s_PrevV = 0;
    unsigned now = GetTickCount();
    int i, changed = 0, vdown;

    if (!s_QOn) return;
    for (i = 0; i < (int)(sizeof(s_EditKeys) / sizeof(s_EditKeys[0])); i++)
    {
        LiKey* k = &s_EditKeys[i];

        if (!LiKeyFire(k, now)) continue;
        if (k->vk == VK_RETURN) { Esc_QuickCommit(hDlg); return; }
        if (k->vk == VK_ESCAPE) { Esc_QuickClose(hDlg); return; }
        if (k->vk == VK_BACK || k->vk == VK_DELETE)
        {
            if (LiEdErase(&s_QSt, k->vk == VK_DELETE)) Esc_QuickSync(1);
        }
        else if (k->vk == VK_LEFT || k->vk == VK_RIGHT ||
                 k->vk == VK_HOME || k->vk == VK_END)
            LiEdMove(&s_QSt, k->vk, 0);
        else
            continue;
        changed = 1;
    }
    /* Ctrl+V: 'V' 不在按键表里 (表是数字/方向键那套), 单独做边沿。 */
    vdown = (LiKeyDown(VK_CONTROL) && LiKeyDown('V')) ? 1 : 0;
    if (vdown && !s_PrevV)
    {
        if (LiEdPaste(&s_QSt, ESC_QUICK_LINE - 1, NULL, NULL))
        {
            Esc_QuickSync(1);
            changed = 1;
        }
    }
    s_PrevV = vdown;
    if (changed) Esc_Redraw(hDlg);
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
    EG_ROWS,
    EG_SKIN,
    EG_COLOR,
    EG_CHOICE,
    EG_VALUE,
    EG_SLIDE,       /* 拖动条: 轨道 + 数值框; 调色行右端还有 "比例" 小标题 */
    EG_LIGHTPARM,   /* 光照参数行: 一行三个小格 [标题][数值框] */
    EG_TRIO,        /* 天气行: 一行四个独立开关 (雨/雪/火雨/雷电) */
    EG_UPBTN,       /* 单位面板左侧按钮: 一行四个 (关闭/折叠/WASD/收藏) */
    EG_CLOUD,       /* 云层行: 三段互斥 [云影|雾气|关] */
    EG_CELLS,       /* 多格行: 一行若干 [小标题][数值框] (文本页边距行) */
    EG_TEXT,        /* 文本行: 一个可编辑的单行文本框 (聊天预设文本) */
    EG_NOTE,        /* 说明行: 灰字, 按行宽折行, 不参与交互 */
    EG_FONT,        /* 字体行: [字体名框][字号][字号框][粗体] */
    EG_HOTKEY,      /* 热键行: 标签 + 右侧值框 (点一下进按键捕获) */
    EG_MIX,         /* 混合格行: 一行若干 [小标题][开关胶囊 / 数值框], 见 s_GuiCells */
    EG_SEP
};

typedef struct {
    unsigned char kind;
    unsigned char item;
} EscGuiRowInfo;

/* 联机只读项: 改了会让本机与其它玩家分叉, 或模块本身在联机就不接管 (点了只会
 * 让人以为生效)。明细与理由见 notes/ESC菜单.md §4 末尾"联机"。 */
static int Esc_MpLocked(int page, int item)
{
    if (!s_Mp || page == EDIT_PAGE_TEXT) return 0;
    switch (item)
    {
    case GUI_ITEM_GAMESPEED_ON:
    case GUI_ITEM_GAMESPEED:
    case GUI_ITEM_FPS:
    case GUI_ITEM_PAUSE:
    case GUI_ITEM_SKIN:
    case GUI_ITEM_COLOR:
    case GUI_ITEM_SAVE_SHOT:
        return 1;
    }
    return 0;
}

/* GUI 页 (界面功能): 按键/底栏/侧栏/游戏控制。与显示设置页共用同一套
 * item/段名表, 只是行的归属不同。 */
static const EscGuiRowInfo s_GuiLayout[] = {
    { EG_GROUP,  GUI_GROUP_GENERAL },
    { EG_SWITCH, GUI_ITEM_BUILD_QUEUE },
    { EG_CHOICE, GUI_ITEM_SCATTER },
    { EG_CHOICE, GUI_ITEM_PLACE_PREV },
    { EG_SWITCH, GUI_ITEM_RALLY },
    { EG_CHOICE, GUI_ITEM_REPAIR },
    { EG_GROUP,  GUI_GROUP_SWS },
    { EG_SWITCH, GUI_ITEM_SWS },
    { EG_CHOICE, GUI_ITEM_SORT },
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
    { EG_SWITCH, GUI_ITEM_TOOLTIP },
    { EG_GROUP,  GUI_GROUP_UNIT },
    { EG_SWITCH, GUI_ITEM_UP_ENABLE },
    { EG_UPBTN,  GUI_ITEM_UP_BTNS },
    { EG_SWITCH, GUI_ITEM_FAV_STAR },
    { EG_CHOICE, GUI_ITEM_FAV_ARROW },
    { EG_GROUP,  GUI_GROUP_PIP },
    { EG_SWITCH, GUI_ITEM_PIP_ENABLE },
    { EG_SWITCH, GUI_ITEM_PIP_OPERATE },
    { EG_SWITCH, GUI_ITEM_PIP_AUTO },
    /* 宽高并成一行 (可编辑数值格), 不再占两条拖动条 */
    { EG_MIX,    GUI_ITEM_PIP_SIZE },
    { EG_CHOICE, GUI_ITEM_PIP_SCALE }
};

/* 游戏控制页 (左栏第 5 页): 游戏速度 / 存档缩略图 / 观战修复 / 范围圈 /
 * 无攻击力单位不靠近, 原来散在 GUI 页末尾的游戏控制组与杂项组挪到这里。
 * item/控件表与前两页共用, 所以点击与编辑走同一套分派。 */
static const EscGuiRowInfo s_CtrlLayout[] = {
    { EG_GROUP,  GUI_GROUP_GAMECTRL },
    { EG_SWITCH, GUI_ITEM_GAMESPEED_ON },
    { EG_SLIDE,  GUI_ITEM_GAMESPEED },
    /* 游戏速度是本局临时参数, 与下面这些长期选项隔一条线 */
    { EG_SEP,    0 },
    { EG_SWITCH, GUI_ITEM_SAVE_SHOT },
    { EG_SWITCH, GUI_ITEM_OBSERVER },
    /* 对空高度 + 启用开关, 同一行 (数值格在前, 胶囊在后) */
    { EG_MIX,    GUI_ITEM_WR },
    { EG_SWITCH, GUI_ITEM_NO_APPROACH },
    { EG_GROUP,  GUI_GROUP_MISC },
    { EG_SWITCH, GUI_ITEM_CRATE },
    { EG_SWITCH, GUI_ITEM_FPS },
    { EG_SWITCH, GUI_ITEM_SCORE_EXIT }
};

/* 显示设置页: 环境显示 (光照/天气) + 界面外观 (玩家颜色/阵营 UI)。
 * 与 GUI 页共用 item 表与控件表, 所以点击/编辑都走同一套分派。 */
static const EscGuiRowInfo s_DispLayout[] = {
    { EG_GROUP,  GUI_GROUP_LIGHT },
    { EG_CHOICE, GUI_ITEM_LIGHT_PRESET },
    { EG_LIGHTPARM, GUI_ITEM_LIGHT_PARM },
    { EG_LIGHTPARM, GUI_ITEM_LIGHT_RGB },
    { EG_SLIDE,  GUI_ITEM_LIGHT_MODE },
    { EG_SLIDE,  GUI_ITEM_ORELIGHT },
    { EG_CHOICE, GUI_ITEM_WTH_MODE },
    { EG_TRIO,   GUI_ITEM_WTH_TRIO },
    { EG_CLOUD,  GUI_ITEM_WTH_CLOUD },
    { EG_CHOICE, GUI_ITEM_WTH_STYLE },
    /* 光照修复是上面整组参数的总开关 (改完要不要自动重建 / 读档后补刷) */
    { EG_SEP,    0 },
    { EG_SWITCH, GUI_ITEM_LIGHTFIX },
    { EG_GROUP,  GUI_GROUP_APPEAR },
    { EG_COLOR,  GUI_ITEM_COLOR },
    { EG_SKIN,   GUI_ITEM_SKIN }
};

/* 快捷键页: 两个组 (下令 / 杂项), 一行一个热键。这一页只有 EG_HOTKEY 一种控件,
 * 点行 = 进捕获态, 按下的组合键立刻写回 ini 并让对应模块回读。 */
static const EscGuiRowInfo s_KeyLayout[] = {
    { EG_GROUP,  GUI_GROUP_HOTKEY },
    { EG_HOTKEY, GUI_ITEM_HK_SMART_ORDER },
    { EG_HOTKEY, GUI_ITEM_HK_SMART_AREA },
    { EG_HOTKEY, GUI_ITEM_HK_PATROL },
    { EG_HOTKEY, GUI_ITEM_HK_TREECHOP },
    { EG_HOTKEY, GUI_ITEM_HK_FUZZY },
    { EG_GROUP,  GUI_GROUP_HOTKEY_MISC },
    { EG_HOTKEY, GUI_ITEM_HK_MIND },
    { EG_HOTKEY, GUI_ITEM_HK_ATTACKMOVE },
    { EG_HOTKEY, GUI_ITEM_HK_AGGRESSIVE }
};

static const EscGuiRowInfo s_TextLayout[] = {
    /* 页首: 字体/字号常用值 (灰字说明, 不可点) */
    { EG_NOTE,   TX_NOTE_FONT },
    { EG_NOTE,   TX_NOTE_PX },
    { EG_GROUP,  TX_GROUP_CAMP },
    { EG_SWITCH, TX_CAMP_ENABLE },
    { EG_FONT,   TX_CAMP_FONT },
    { EG_VALUE,  TX_CAMP_LINES },
    { EG_VALUE,  TX_CAMP_OPACITY },
    { EG_CHOICE, TX_CAMP_POS },
    { EG_CHOICE, TX_CAMP_ALIGN },
    { EG_CELLS,  TX_CAMP_MARGIN },
    { EG_VALUE,  TX_CAMP_WIDTH },
    { EG_SWITCH, TX_CAMP_ANIM },
    { EG_CHOICE, TX_CAMP_BAR },
    { EG_GROUP,  TX_GROUP_CHAT },
    { EG_SWITCH, TX_CHAT_EDIT },
    { EG_SWITCH, TX_CHAT_NAME },
    { EG_TEXT,   TX_CHAT_QUICK },
    { EG_GROUP,  TX_GROUP_HINT },
    { EG_SWITCH, TX_HINT_SHOW },
    { EG_FONT,   TX_HINT_FONT },
    { EG_VALUE,  TX_HINT_OPACITY },
    { EG_CHOICE, TX_HINT_COLOR },
    { EG_VALUE,  TX_HINT_LINES },
    { EG_VALUE,  TX_HINT_WIDTH },
    { EG_CHOICE, TX_HINT_BAR },
    { EG_SWITCH, TX_HINT_RADAR },
    { EG_GROUP,  TX_GROUP_SW },
    { EG_SWITCH, TX_SW_ENABLE },
    { EG_FONT,   TX_SW_FONT },
    { EG_CHOICE, TX_SW_STYLE },
    { EG_VALUE,  TX_SW_OPACITY },
    { EG_GROUP,  TX_GROUP_OTHER },
    { EG_FONT,   TX_VT_FONT },
    { EG_SEP,    TX_SEP5 },
    { EG_CELLS,  TX_VT_CREDIT },
    { EG_SEP,    TX_SEP6 },
    { EG_CHOICE, TX_VT_POWER },
    { EG_CELLS,  TX_VT_POWER_MARGIN },
    { EG_SEP,    TX_SEP7 },
    { EG_SWITCH, TX_VT_BRIEF_OWN },
    { EG_VALUE,  TX_VT_BRIEF_WIDTH },
    { EG_SWITCH, TX_VT_BRIEF_MARK }
};

/* 分段行的段名与段数。GUI 页与文本页各一套表, 而且 TX_* 与 GUI_ITEM_* 编号是重叠的,
 * 所以必须由页自己给, 不能按 item 猜是哪张表。 */
typedef struct {
    EscName* names;
    int n;
} EscSegs;

/* 一页的行表 + 全局行距参数; 绘制/命中/滚动都按它走。 */
typedef struct {
    const EscGuiRowInfo* rows;
    int n;
    const wchar_t* title;
    /* 组标题文案; NULL = 组标题就在行文案数组里 (文本页共用一个 enum) */
    const wchar_t (*groups)[ESC_LABEL_CAP];
    /* 行文案: 行高要按标题实测宽度定 (换行判定), 所以页里得带着它 */
    const wchar_t (*labels)[ESC_LABEL_CAP];
    /* 分段行取段名: GUI 页与文本页各一套表 (见 EscSegs) */
    EscSegs (*segs)(int item);
    int page;          /* EDIT_PAGE_*: item 编号必须连同页身份解释 */
    int* scroll;       /* 本页的滚动位置 (每页各一个) */
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

    if (kind == EG_GROUP) return ESC_GROUP_H;
    /* 分割线占满一行高: 线画在行中间, 与相邻行的行距一致。 */
    if (kind == EG_SEP) return ESC_ROW_H;
    if (kind == EG_COLOR)
        return ESC_ROW_H + (Esc_ColorRows(rowW) - 1) * (ESC_COLOR_SW + ESC_COLOR_GAP);

    sg.names = NULL;
    sg.n = 0;
    if (pg->segs) sg = pg->segs(item);

    Esc_RowFlow(kind, item, p->left + ESC_PAD, rowW, pg->labels[item], sg, &f);
    return ESC_ROW_H * (f.lines > 1 ? f.lines : 1);
}

/* 三种行遍历共用布局步进: 输出行顶、返回行高, 并推进到下一行。 */
static int Esc_RowStep(const RECT* p, const EscPage* pg, int i, int* next, int* y)
{
    *y = *next + (i && pg->rows[i].kind == EG_GROUP ? ESC_BLOCK_GAP : 0);
    int rh = Esc_RowH(p, pg, i);
    *next = *y + rh;
    return rh;
}

/* 走一遍某页的行表: *total 写出总高, *maxRowH 写出最高的一行 (折行行可达
 * ESC_ROW_H 的整数倍; 贴底判据要用它, 用 ESC_ROW_H 会漏判)。返回 <= scroll 的
 * 最近行顶 (滚动吸附到行顶, 避免整行被上下裁剪)。各页共用 (设置页也会长高,
 * 所以滚动不能只给文本页写死)。 */
static int Esc_PageWalk(const RECT* p, const EscPage* pg, int scroll,
                        int* total, int* maxRowH)
{
    int i, y, next = 0, best = 0, rhMax = 0;
    for (i = 0; i < pg->n; i++)
    {
        int rh = Esc_RowStep(p, pg, i, &next, &y);
        if (y <= scroll) best = y;
        if (rh > rhMax) rhMax = rh;
    }
    if (total) *total = next;
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
        if (rowY + ESC_ROW_H > Esc_FootY(p) - ESC_SEP_GAP) break;

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

/* [VecText] 里的设置项 (结算页退出按钮) 走 VecText 的选项接口: 取消菜单时的
 * 回滚由 VecText_RestoreOptions 统一负责, 这里只做取值/写值。 */
static int Esc_ScoreExitGet(void) { return VecText_GetOption(VTO_SCORE_EXIT); }
static void Esc_ScoreExitSet(int v) { VecText_SetOption(VTO_SCORE_EXIT, v); }

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
    { GUI_ITEM_GAMESPEED_ON, "GameSpeed", "Enable",       GameSpeed_Enabled, GameSpeed_SetEnabled },
    { GUI_ITEM_BUILD_QUEUE,  "Features",  "BuildQueue",   BQ_Enabled,        BQ_SetEnabled },
    { GUI_ITEM_SIDE_SCROLL,  "Sidebar",   "ScrollEnable", SS_Enabled,        SS_SetEnabled },
    { GUI_ITEM_RALLY,        "RallyPath", "Enable",       RallyPath_Enabled, RallyPath_SetEnabled },
    /* 天气: 雨/雪/火雨/雷电只在本次运行有效 (section=NULL)。 */
    { GUI_ITEM_WTH_RAIN,     NULL, NULL, Weather_Rain, Weather_SetRain },
    { GUI_ITEM_WTH_SNOW,     NULL, NULL, Weather_Snow, Weather_SetSnow },
    { GUI_ITEM_WTH_FIRE,     NULL, NULL, Weather_Fire, Weather_SetFire },
    { GUI_ITEM_WTH_THUNDER,  NULL, NULL, Weather_Thunder, Weather_SetThunder },
    { GUI_ITEM_CRATE,        "CrateIcon", "Enable",     CrateIcon_On,      CrateIcon_SetOn },
    { GUI_ITEM_UP_ENABLE,    "UnitPanel", "Enable",     UP_Enabled,        UP_SetEnabled },
    { GUI_ITEM_FAV_STAR,     "Favorite",  "StarEnable", Fav_StarsEnabled,  Fav_SetStarsEnabled },
    { GUI_ITEM_PIP_ENABLE,   "PiP", "Enable",   PiP_Enabled,  PiP_SetEnabled },
    { GUI_ITEM_PIP_OPERATE,  "PiP", "Operate",  PiP_Operate,  PiP_SetOperate },
    { GUI_ITEM_PIP_AUTO,     "PiP", "AutoJump", PiP_AutoJump, PiP_SetAutoJump },
    /* 侧边栏: 建造图标提示追加 电力 + 耗时 */
    { GUI_ITEM_TOOLTIP,      "Features", "ToolTips", ToolTip_Enabled, ToolTip_SetEnabled },
    /* 显示设置页: 光照修复 (改光照后逐格重建 / 读档后补刷) */
    { GUI_ITEM_LIGHTFIX,     ENV_SECTION, ENV_KEY_LIGHTFIX, LightFix_Enabled,
      LightFix_SetEnabled },
    /* 游戏控制页 */
    { GUI_ITEM_SAVE_SHOT,    "SaveShot", "Enable",  SaveShot_Enabled, SaveShot_SetEnabled },
    { GUI_ITEM_OBSERVER,     "Features", "Observer", Observer_Enabled, Observer_SetEnabled },
    { GUI_ITEM_SCORE_EXIT,   VT_SECTION, "ScoreExit", Esc_ScoreExitGet, Esc_ScoreExitSet },
    { GUI_ITEM_WR_ON,        "Features", "WeaponRange", WeaponRange_Enabled,
      WeaponRange_SetEnabled },
    { GUI_ITEM_NO_APPROACH,  "Features", "NoApproach", NoApproach_Enabled,
      NoApproach_SetEnabled }
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

static void EscChoiceSort(int pick)
{
    SWS_SetFillMode(pick);
    Esc_QueueValue("SWSidebar", "FillMode", pick);
}

static void EscChoiceScatter(int pick)
{
    PS_SetMode(pick);
    Esc_QueueValue("PlaceScatter", "Mode", pick);
}

/* 放置预览浓度: 0 = 关闭, 25/50/75/100 = 由淡到不透明。段号 = 百分比/25,
 * 展开虚影 (DeployPreview) 跟着同一档走, 没有独立开关。 */
static int EscChoicePlacePrevGet(void)
{
    return PlacementPreview_Opacity() / 25;
}

static void EscChoicePlacePrevApply(int pick)
{
    int pct = pick * 25;

    PlacementPreview_SetOpacity(pct);
    Esc_QueueValue("PlacementPreview", "Opacity", pct);
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

/* 箭头指示 (收藏/信标): 不显示 / 边缘 / 中心。落盘 [Favorite] ArrowMode。 */
static void EscChoiceArrow(int pick)
{
    Fav_SetArrowMode(pick);
    Esc_QueueRaw("Favorite", "ArrowMode",
                 pick <= 0 ? "none" : (pick == 1 ? "edge" : "center"));
}

/* 快捷维修 ([RepairAll] Mode): 0=关闭 1=半自动 2=自动。段序是用户习惯的
 * "关闭 / 自动 / 半自动", 与 Mode 值不同序, 所以取/存各转一道。 */
static int EscChoiceRepairGet(void)
{
    int m = RepairAll_Mode();

    return (m == 2) ? 1 : (m == 1 ? 2 : 0);
}

static void EscChoiceRepairApply(int pick)
{
    int m = (pick <= 0) ? 0 : (pick == 1 ? 2 : 1);

    RepairAll_SetMode(m);
    Esc_QueueValue("RepairAll", "Mode", m);
}

/* 小窗口离屏倍率: 段号 0/1 = 1x/2x。倍率越高画质越好, 离屏面也越大。 */
static int EscChoicePipScaleGet(void)
{
    return PiP_Scale() == 1 ? 0 : 1;
}

static void EscChoicePipScaleApply(int pick)
{
    int n = pick ? 2 : 1;

    PiP_SetScale(n);
    Esc_QueueValue("PiP", "Scale", n);
}

static const EscChoice s_Choice[] = {
    { GUI_ITEM_SORT, s_GuiSort, 2, 0, SWS_FillMode, EscChoiceSort },
    { GUI_ITEM_SCATTER,   s_GuiScatter, ESC_SCATTER_N,  0, PS_Mode,       EscChoiceScatter },
    { GUI_ITEM_PLACE_PREV, s_GuiPlacePrev, ESC_PLACE_PREV_N, 0,
      EscChoicePlacePrevGet, EscChoicePlacePrevApply },
    { GUI_ITEM_SIDE_HIDE, s_GuiHide,    ESC_HIDE_N,     0, FM_HideState,  EscChoiceHide },
    { GUI_ITEM_SWS_SIDE,  s_GuiSwsSide, ESC_SWS_SIDE_N, 1, SWS_SideRight, EscChoiceSwsSide },
    { GUI_ITEM_LIGHT_PRESET, s_GuiLight, LIGHT_PRESET_COUNT + 1, 0,
      EscChoiceLightGet, EscChoiceLightApply },
    { GUI_ITEM_WTH_MODE,  s_GuiWthMode, WEATHER_DRAW_N, 0, Weather_DrawMode,
      EscChoiceWthMode },
    { GUI_ITEM_WTH_STYLE, s_GuiWthStyle, WEATHER_CLOUD_STYLE_N, 0,
      Weather_CloudStyle, EscChoiceWthStyle },
    { GUI_ITEM_FAV_ARROW, s_GuiArrow, ESC_ARROW_N, 0, Fav_ArrowMode,
      EscChoiceArrow },
    { GUI_ITEM_PIP_SCALE, s_GuiPipScale, 2, 0, EscChoicePipScaleGet,
      EscChoicePipScaleApply },
    { GUI_ITEM_REPAIR, s_GuiRepair, ESC_REPAIR_N, 0, EscChoiceRepairGet,
      EscChoiceRepairApply }
};

ESC_FINDER(Esc_ChoiceFind, EscChoice, s_Choice)

/* ---------------- 快捷键行 ----------------
 * 每行一个 ini 热键串。def 只在 ini 缺键时用来显示, 必须与各模块自己的
 * 默认串一致 ([Features] 的三项是"单行开关+热键", 值本身就是热键串)。
 * reload 是设置页写盘后立刻回读的入口 —— 热键是模块里的运行期状态,
 * 只排队不写盘的话这一局不会生效。 */
typedef struct {
    unsigned char item;
    const char* section;
    const char* key;
    const char* def;
    void (*reload)(void);
} EscHotkey;

static const EscHotkey s_Hotkeys[] = {
    { GUI_ITEM_HK_SMART_ORDER, "SmartOrder",   "Hotkey",     "Middle",
      SmartOrder_ReloadHotkey },
    { GUI_ITEM_HK_SMART_AREA,  "SmartArea",    "Hotkey",     "Shift+Middle",
      SmartArea_ReloadHotkey },
    { GUI_ITEM_HK_PATROL,      "PatrolCircle", "Hotkey",     "Z+Middle",
      PatrolCircle_ReloadHotkey },
    { GUI_ITEM_HK_TREECHOP,    "TreeChop",     "Hotkey",     "Ctrl+Z+Middle",
      TreeChop_ReloadHotkey },
    { GUI_ITEM_HK_FUZZY,       "FuzzyAttack",  "Hotkey",     "Ctrl+Middle",
      FuzzyAttack_ReloadHotkey },
    { GUI_ITEM_HK_MIND,        "Features",     "MindSelect", "Shift+P",
      MindSelect_ReloadHotkey },
    { GUI_ITEM_HK_ATTACKMOVE,  "Features",     "AttackMove", "X2",
      AttackMove_ReloadHotkey },
    { GUI_ITEM_HK_AGGRESSIVE,  "Aggressive",   "Hotkey",     "Alt+A",
      Aggressive_ReloadHotkey }
};
#define ESC_HK_N ((int)(sizeof(s_Hotkeys) / sizeof(s_Hotkeys[0])))

ESC_FINDER(Esc_HotkeyFind, EscHotkey, s_Hotkeys)

/* 打开设置页时的原值 (取消失败回滚用; 空串 = ini 里本来没这一行)。 */
static char s_HkBak[ESC_HK_N][32];

/* 行的显示串: 读 ini -> 规范化 (Ctrl+mmb 也显示成 Ctrl+Middle)。
 * 读不到就用默认串, 与模块的兜底一致。仅 ASCII, 逐字节转宽字符即可。 */
static void Esc_HotkeyText(int item, wchar_t* out, int cap)
{
    const EscHotkey* e = Esc_HotkeyFind(item);
    char raw[48], norm[48];
    OU_Hotkey hk;
    int i;

    if (!out || cap < 2) return;
    out[0] = 0;
    if (!e) return;
    OU_ReadString(e->section, e->key, "", raw, sizeof(raw));
    if (!raw[0]) lstrcpynA(raw, e->def, sizeof(raw));
    if (OU_ParseHotkey(raw, &hk)) OU_HotkeyText(&hk, norm, sizeof(norm));
    else lstrcpynA(norm, raw, sizeof(norm));
    for (i = 0; i + 1 < cap && norm[i]; i++) out[i] = (wchar_t)(unsigned char)norm[i];
    out[i] = 0;
}

static int Esc_ChoiceValue(const EscChoice* c)
{
    return c->invert ? !c->get() : c->get();
}

typedef struct {
    unsigned char item;
    EscName* names;
    unsigned char n;
    unsigned char invert;
    unsigned char opt;      /* MSG_OPT_* / SWO_* / VTO_* / PWO_* */
    const char* key;        /* 落在 mod 对应的节里 */
    unsigned char mod;      /* OPM_* */
    /* 非空时自己取/存 (跨多个键的行, 如"底部电力"要同时改 Bottom+Mode)。 */
    int (*get)(void);
    void (*set)(int);
} EscTextChoice;

/* 底部电力: 一个选项要同时动 PowerBottom 与 PowerMode。
 * 段序 0=关闭 1=余量 2=负载/电力。 */
static int Esc_TextPowerGet(void)
{
    if (!PowerDisplay_GetOption(PWO_ENABLE)) return 0;
    return PowerDisplay_GetOption(PWO_MODE) ? 1 : 2;
}

static void Esc_TextPowerSet(int v)
{
    if (v <= 0) { PowerDisplay_SetOption(PWO_ENABLE, 0); return; }
    PowerDisplay_SetOption(PWO_ENABLE, 1);
    PowerDisplay_SetOption(PWO_MODE, (v == 1) ? 1 : 0);
}

static const EscTextChoice s_TChoice[] = {
    { TX_CAMP_POS,   s_TextPos,   2, 1, MSG_OPT_CAMP_BOTTOM,   "CampBottom" },
    { TX_CAMP_ALIGN, s_TextAlign, 2, 0, MSG_OPT_CAMP_CENTERED, "CampCentered" },
    { TX_CAMP_BAR,   s_TextBar,   3, 0, MSG_OPT_CAMP_BAR,      "CampBarSide" },
    { TX_HINT_COLOR, s_TextColor, 2, 0, MSG_OPT_HINT_COLOR,    "HintColorFollow" },
    { TX_HINT_BAR,   s_TextBar,   3, 0, MSG_OPT_HINT_BAR,      "HintBarSide" },
    /* 段序与 SWO_CLASSIC 同向 (0=统一面板, 1=原本样式), 不要加 invert。 */
    { TX_SW_STYLE,   s_TextSwStyle, 2, 0, SWO_CLASSIC, "Classic", OPM_SW },
    { TX_VT_POWER,   s_TextPower,  3, 0, 0, NULL, OPM_PWR,
      Esc_TextPowerGet, Esc_TextPowerSet }
};

ESC_FINDER(Esc_TChoiceFind, EscTextChoice, s_TChoice)

static int Esc_TChoiceValue(const EscTextChoice* c)
{
    int v = c->get ? c->get() : Esc_OptGet(c->mod, c->opt);
    return c->invert ? !v : v;
}

/* 文本页开关: opt >= 0 走 mod 对应模块的选项, 否则走自己的 get/set。 */
typedef struct {
    signed char item;
    signed char opt;
    int (*get)(void);
    void (*set)(int);
    const char* key;
    unsigned char mod;      /* OPM_* */
} EscTextToggle;

static const EscTextToggle s_TToggle[] = {
    { TX_CAMP_ENABLE, MSG_OPT_CAMP_ENABLE, NULL, NULL, "CampEnable" },
    { TX_CAMP_ANIM,   MSG_OPT_CAMP_ANIM,   NULL, NULL, "CampAnim" },
    { TX_HINT_SHOW,   MSG_OPT_HINT_SHOW,   NULL, NULL, "HintShow" },
    { TX_HINT_RADAR,  MSG_OPT_HINT_RADAR,  NULL, NULL, "HintRadarShow" },
    { TX_CHAT_EDIT,   -1, ChatEdit_Enabled,    ChatEdit_SetEnabled,    "ChatEdit" },
    { TX_CHAT_NAME,   -1, ChatEdit_ShowNameSP, ChatEdit_SetShowNameSP, "ChatNameSP" },
    /* 超武倒计时面板 / 其它文本 ([VecText]) */
    { TX_SW_ENABLE,      SWO_ENABLE,     NULL, NULL, "Enable",    OPM_SW },
    { TX_VT_BRIEF_OWN,   VTO_BRIEF_OWN,  NULL, NULL, "BriefOwn",  OPM_VT },
    { TX_VT_BRIEF_MARK,  VTO_BRIEF_MARK, NULL, NULL, "BriefMark", OPM_VT }
};

ESC_FINDER(Esc_TToggleFind, EscTextToggle, s_TToggle)

static int Esc_TToggleVal(const EscTextToggle* t)
{
    return t->opt >= 0 ? Esc_OptGet(t->mod, t->opt) : t->get();
}

static void Esc_TTogglePut(const EscTextToggle* t, int v)
{
    if (t->opt >= 0) Esc_OptSet(t->mod, t->opt, v);
    else t->set(v);
}

typedef struct {
    int item;
    int opt;
    const char* key;
    int min, max, step;
    unsigned char mod;      /* OPM_* */
} EscTextNum;

static const EscTextNum s_TextNum[] = {
    { TX_CAMP_LINES,   MSG_OPT_CAMP_LINES,   "MaxLines",     ESC_ROWS_MIN, ESC_ROWS_MAX, 1 },
    { TX_CAMP_OPACITY, MSG_OPT_CAMP_OPACITY, "BoardOpacity", 0,            100,          5 },
    { TX_CAMP_WIDTH,   MSG_OPT_CAMP_WIDTH,   "MaxWidth",     200,          1600,         50 },
    { TX_HINT_LINES,   MSG_OPT_HINT_LINES,   "HintMaxLines", ESC_ROWS_MIN, ESC_ROWS_MAX, 1 },
    { TX_HINT_WIDTH,   MSG_OPT_HINT_WIDTH,   "HintMaxWidth", 100,          1600,         20 },
    { TX_HINT_OPACITY, MSG_OPT_HINT_OPACITY, "HintOpacity",  0,            100,          5 },
    /* 边距行的三个格子: 每个格子是独立 item (编辑态按 item 记, 与光照参数行
     * 同做法), 落盘键与 [MsgText] 的键同名。 */
    { TX_CAMP_MARGIN_L, MSG_OPT_CAMP_POSX,          "PosX",             0, 999,  5 },
    { TX_CAMP_MARGIN_T, MSG_OPT_CAMP_POSY,          "PosY",             0, 999,  5 },
    { TX_CAMP_MARGIN_B, MSG_OPT_CAMP_BOTTOMMARGIN,  "CampBottomMargin", 0, 2000, 5 },
    /* 字体行的字号格 (0 = 跟点阵字高)。 */
    { TX_CAMP_FPXSIZE, MSG_OPT_CAMP_FONT_PX, "FontHeight",     0, 64, 1 },
    { TX_HINT_FPXSIZE, MSG_OPT_HINT_FONT_PX, "HintFontHeight", 0, 64, 1 },
    { TX_SW_FPXSIZE,   SWO_FONT_PX,          "FontPx",         0, 64, 1, OPM_SW },
    { TX_VT_FPXSIZE,   VTO_FONT_PX,          "FontPx",         0, 40, 1, OPM_VT },
    /* 超武面板底板不透明度 */
    { TX_SW_OPACITY,   SWO_OPACITY,          "Opacity",        0, 100, 5, OPM_SW },
    /* 资金字号与偏移 (两格) */
    { TX_VT_CREDIT_PX, VTO_CREDIT_PX,        "CreditPx",       0,  40, 1, OPM_VT },
    { TX_VT_CREDIT_DY, VTO_CREDIT_DY,        "CreditDY",      -8,   8, 1, OPM_VT },
    /* 底部电力边距 (左 / 底) */
    { TX_VT_POWER_X,   PWO_X,                "PowerX",         0, 999, 5, OPM_PWR },
    { TX_VT_POWER_PAD, PWO_PAD,              "PowerPad",       0, 200, 2, OPM_PWR },
    /* 简报宽度 (0 = 引擎默认 453) */
    { TX_VT_BRIEF_WIDTH, VTO_BRIEF_WIDTH,    "BriefWidth",     0, 4000, 20, OPM_VT }
};

ESC_FINDER(Esc_TextNumFind, EscTextNum, s_TextNum)

/* 文本页"多格行": 一行若干 [小标题][数值框], 格子值/上下限全在 s_TextNum 里,
 * 这里只记行与格子的对应关系 (绘制、命中、编辑框定位都按它查)。 */
typedef struct {
    unsigned char item;         /* 行 item (标题所在行) */
    unsigned char n;
    EscName* caps;              /* n 个小标题 */
    const int* cellItems;       /* n 个格子 item, 顺序同 caps */
} EscTextCells;

static const int s_MarginCellItems[ESC_TEXT_CELL_N] = {
    TX_CAMP_MARGIN_L, TX_CAMP_MARGIN_T, TX_CAMP_MARGIN_B
};
static const EscTextCells s_TCells[] = {
    { TX_CAMP_MARGIN, ESC_TEXT_CELL_N, s_TCellCapMargin, s_MarginCellItems },
    { TX_VT_CREDIT,      2, s_TCellCapCredit, s_CreditCellItems },
    { TX_VT_POWER_MARGIN, 2, s_TCellCapPwr,    s_PwrCellItems }
};

ESC_FINDER(Esc_TextCellsFind, EscTextCells, s_TCells)

/* 格子 item -> 所在行; *cell 写出格号。不是格子返回 NULL。 */
static const EscTextCells* Esc_TextCellOf(int item, int* cell)
{
    const EscTextCells* c;
    int r, i;

    for (r = 0; r < (int)(sizeof(s_TCells) / sizeof(s_TCells[0])); r++)
    {
        c = &s_TCells[r];
        for (i = 0; i < c->n; i++)
            if (c->cellItems[i] == item)
            {
                *cell = i;
                return c;
            }
    }
    return NULL;
}

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
    if (*value < minValue) *value = minValue;
    if (*value > maxValue) *value = maxValue;
    return 1;
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
      SWS_MaxRows, SWS_SetMaxRows, "SWSidebar", "MaxRows" },
    /* 小窗口宽高: 步进 8 与 ini 默认值 (288x192) 都是 8 的倍数, 输出来不会出现
     * 奇数尺寸; 上下限与 PiP_SetWidth/SetHeight 里的夹取一致。宽高在设置页合并成
     * 一行 (EG_MIX), 这两项只当那一行的两个数值格。 */
    { GUI_ITEM_PIP_W,      64,            1024,                8,
      PiP_Width, PiP_SetWidth, "PiP", "Width" },
    { GUI_ITEM_PIP_H,      48,            1024,                8,
      PiP_Height, PiP_SetHeight, "PiP", "Height" },
    /* 显示单位范围: 对空圈在单位所在高度上再加多少 lepton (0 = 贴地)。
     * 与 WeaponRange_SetHeight 的夹取一致; 步进 32 = 1/8 格 (常用飞行高度
     * 500~650, 步进再大就调不准)。 */
    { GUI_ITEM_WR_H,       0,             8192,                32,
      WeaponRange_Height, WeaponRange_SetHeight, "Features", "WeaponRangeAAZ" }
};

ESC_FINDER(Esc_NumFind, EscNum, s_Num)

/* ---------------- 混合格行 (EG_MIX) ----------------
 * 一行里并排放几个 [小标题][控件]: 控件是小窗口宽高这种"同族两个数值", 或者
 * "开关 + 它自己的参数" (显示单位范围: 启用 + 对空圈抬高)。格 item 落在 s_Toggle
 * 就是胶囊开关, 落在 s_Num 就是可编辑数值框; 小标题统一取 s_GuiLabel。
 * 和文本页的 EG_CELLS 一样走原子流 (Esc_RowAtoms), 折行后仍然画在哪点在哪。 */
typedef struct {
    unsigned char item;          /* 行标题 item */
    unsigned char n;
    unsigned char cells[ESC_FLOW_MAX];
} EscGuiCells;

static const EscGuiCells s_GuiCells[] = {
    { GUI_ITEM_PIP_SIZE, 2, { GUI_ITEM_PIP_W, GUI_ITEM_PIP_H } },
    /* 顺序 = 从左到右; 胶囊放最后才与普通开关行同一列 (原子流整串右对齐) */
    { GUI_ITEM_WR,       2, { GUI_ITEM_WR_H, GUI_ITEM_WR_ON } }
};

ESC_FINDER(Esc_GuiCellsFind, EscGuiCells, s_GuiCells)

/* 拖动期间只显示、松手才提交的滑条: 调色比例一次要重建 3.7 万格。其余滑条
 * (游戏速度) 拖动时直接生效。小窗口宽高改成了行内数值格, 不再有拖动条 ——
 * 那一项"必须立刻丢离屏面"的约束由 PiP_SetWidth/SetHeight 自己保证。 */
static int Esc_SlideDefer(int item)
{
    return item == GUI_ITEM_LIGHT_MODE;
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

/* 各模块的数值选项统一供绘制、编辑和滚轮使用; item 必须连同页解释。 */
struct EscNumber {
    int value, min, max, step;
};

static EscNumber Esc_Number(int page, int item)
{
    if (page == EDIT_PAGE_TEXT)
    {
        const EscTextNum* n = Esc_TextNumFind(item);
        return { Esc_OptGet(n->mod, n->opt), n->min, n->max, n->step };
    }
    const EscNum* n = Esc_NumFind(item);
    if (n) return { n->get(), n->min, n->max, n->step };
    int p = Esc_LightItemIdx(item);
    return { Lighting_GetParam(p), Esc_LightMin(p), LIGHT_PARAM_MAX, 1 };
}

/* 行 item (标题所在行) -> 这一行第一个参数号; -1 = 不是参数行。 */
static int Esc_LightRowFirst(int item)
{
    if (item == GUI_ITEM_LIGHT_PARM) return 0;
    if (item == GUI_ITEM_LIGHT_RGB) return LIGHT_ROW_CELLS;
    return -1;
}

/* 按最大显示字面确定数值格宽度, 两页保留各自的负号/四位数字预算。 */
static int Esc_BoxWidth(const wchar_t* sample, int fallback)
{
    int w = Esc_TextWidth(sample, ESC_BODY_PX, ESC_BODY_SLOT);
    w = (w > 0 ? w : fallback) + LIGHT_BOX_PAD;
    return w > LIGHT_BOX_MIN ? w : LIGHT_BOX_MIN;
}

static int Esc_LightBoxW(void) { return Esc_BoxWidth(L"-000", 24); }
static int Esc_CellBoxW(void) { return Esc_BoxWidth(L"0000", 32); }

/* 多格行一格的几何: 原子框里 [小标题][缝][数值框], 数值框贴原子右端。
 * 绘制/命中/编辑框定位三处共用, 口径不能各算各的。 */
static void Esc_CellBox(const EscFlow* f, int i, int boxW, int* capW, int* boxX)
{
    *capW = f->w[i] - LIGHT_CAP_GAP - boxW;
    *boxX = f->x[i] + *capW + LIGHT_CAP_GAP;
}

/* 某个 item 是哪一行的第几格 (-1 = 不是混合格行的格)。 */
static const EscGuiCells* Esc_GuiCellsOf(int item, int* cell)
{
    int i, k;

    for (i = 0; i < (int)(sizeof(s_GuiCells) / sizeof(s_GuiCells[0])); i++)
        for (k = 0; k < s_GuiCells[i].n; k++)
        {
            if (s_GuiCells[i].cells[k] != item) continue;
            *cell = k;
            return &s_GuiCells[i];
        }
    return NULL;
}

/* 多格行的控件描述供布局、绘制和命中共用。 */
struct EscCell {
    int item, width;
    const wchar_t* label;
    const EscToggle* toggle;
};

static EscCell Esc_Cell(int page, int row, int i)
{
    if (page == EDIT_PAGE_TEXT)
    {
        const EscTextCells* c = Esc_TextCellsFind(row);
        return { c->cellItems[i], Esc_CellBoxW(), c->caps[i], NULL };
    }
    const EscGuiCells* c = Esc_GuiCellsFind(row);
    int item = c ? c->cells[i] : s_LightItem[Esc_LightRowFirst(row) + i];
    const EscToggle* t = c ? Esc_ToggleFind(item) : NULL;
    return { item, t ? ESC_SW_W : Esc_LightBoxW(), t ? L"" : s_GuiLabel[item], t };
}

static int Esc_CellWidth(int page, int row, int i)
{
    EscCell c = Esc_Cell(page, row, i);
    if (c.toggle) return c.width;
    int w = Esc_TextWidth(c.label, ESC_BODY_PX, ESC_BODY_SLOT);
    return (w > 0 ? w : 16) + LIGHT_CAP_GAP + c.width;
}

static int Esc_CellsKind(int page, int row)
{
    if (page == EDIT_PAGE_TEXT) return Esc_TextCellsFind(row) ? EG_CELLS : 0;
    if (Esc_GuiCellsFind(row)) return EG_MIX;
    return Esc_LightRowFirst(row) >= 0 ? EG_LIGHTPARM : 0;
}

static int Esc_CellItemAt(const EscFlow* f, int page, int row, int rowY,
                          int mx, int my)
{
    for (int i = 0; i < f->n; i++)
    {
        EscCell c = Esc_Cell(page, row, i);
        int capW, x;
        Esc_CellBox(f, i, c.width, &capW, &x);
        if (EscIn(x, rowY + f->ln[i] * ESC_ROW_H, c.width, ESC_ROW_H, mx, my))
            return c.item;
    }
    return -1;
}

/* 所有滑条共用轨道、数值框与命中边界; 端点行在轨道两侧放快捷段。 */
typedef struct {
    int trkX, trkW, numX, numW, xs[2], ws[2], hitL, hitR;
    EscName* ends;
} EscSlideGeom;

static EscSlideGeom Esc_SlideGeom(int item, int rowL, int rowW,
                                   const wchar_t* label)
{
    EscSlideGeom g = {};
    int ctlX = Esc_RowCtlX(rowL + rowW), trkR, i;
    int left = rowL + ESC_TEXT_INSET
               + Esc_TextWidth(label, ESC_BODY_PX, ESC_BODY_SLOT) + ESC_LBL_GAP;

    g.ends = Esc_ItemEnds(item);
    if (g.ends)
    {
        g.numW = Esc_BoxWidth(L"100", 24);
        g.numX = rowL + rowW - ESC_CTL_RIGHT - g.numW;
        for (i = 0; i < 2; i++)
        {
            g.ws[i] = Esc_TextWidth(g.ends[i], ESC_BODY_PX, ESC_BODY_SLOT);
            if (g.ws[i] <= 0) g.ws[i] = 16;
        }
        g.xs[1] = g.numX - ESC_SLIDE_GAP - g.ws[1];
        trkR = g.xs[1] - ESC_SLIDE_GAP;
        g.xs[0] = trkR - ESC_SLIDE_W - ESC_SLIDE_GAP - g.ws[0];
        if (g.xs[0] < left) g.xs[0] = left;
        g.trkX = g.xs[0] + g.ws[0] + ESC_SLIDE_GAP;
        g.hitL = g.xs[0];
        g.hitR = g.xs[1] + g.ws[1];
    }
    else
    {
        Esc_StepBox(ctlX, &g.numX, &g.numW);
        trkR = ctlX - ESC_SLIDE_GAP;
        g.trkX = trkR - ESC_SLIDE_W;
        if (g.trkX < left) g.trkX = left;
        g.hitL = g.trkX;
        g.hitR = ctlX + ESC_STEP_W;
    }
    g.trkW = trkR - g.trkX;
    if (g.trkW < 1) g.trkW = 1;
    return g;
}

static int Esc_SlideValue(int item, const EscSlideGeom* g, int mx)
{
    const EscNum* n = Esc_NumFind(item);
    return item == GUI_ITEM_GAMESPEED ? Esc_SlideValueAt(mx, g->trkX, g->trkW)
        : Esc_TrackValueAt(mx, g->trkX, g->trkW, n->min, n->max);
}
/* 字体行几何 (定义在绘制区, 数值框定位要先问它)。 */
static void Esc_FontGeom(int rowL, int rowW, const wchar_t* label,
                         int* faceX, int* faceW, int* capX, int* capW,
                         int* pxX, int* pxW, int* bdX, int* bdW);

/* 某个数值框的几何: 光照格按控件流算, 其余行用 [+/- 数值框] 的统一版式
 * (游戏速度与普通数值行同布局)。 */
static void Esc_NumBoxFor(int page, int item, const RECT* right, int* x, int* w)
{
    int rowL = right->left + ESC_PAD;
    int rowW = right->right - right->left - ESC_PAD * 2;
    int idx = page == EDIT_PAGE_TEXT ? -1 : Esc_LightItemIdx(item);
    int cell = -1, rowItem = 0, kind = 0;
    const EscTextFont* ft = page == EDIT_PAGE_TEXT ? Esc_TextFontForPx(item) : NULL;
    const EscTextCells* tc;
    const EscGuiCells* mc;
    const wchar_t* label = NULL;
    EscFlow f;
    EscSegs none = { NULL, 0 };

    if (ft)
    {
        int fx, fw, cx, cw, bx, bw;
        Esc_FontGeom(rowL, rowW, s_TextLabel[ft->item], &fx, &fw, &cx, &cw,
                     x, w, &bx, &bw);
        return;
    }
    /* 光照、混合格、文本格共用标题后的数值框, 查找次序保持一致。 */
    if (idx >= 0)
    {
        rowItem = idx < LIGHT_ROW_CELLS ? GUI_ITEM_LIGHT_PARM : GUI_ITEM_LIGHT_RGB;
        cell = idx % LIGHT_ROW_CELLS;
        kind = EG_LIGHTPARM;
        *w = Esc_LightBoxW();
    }
    else if (page != EDIT_PAGE_TEXT && (mc = Esc_GuiCellsOf(item, &cell)))
    {
        rowItem = mc->item;
        kind = EG_MIX;
        *w = Esc_LightBoxW();
    }
    else if (page == EDIT_PAGE_TEXT && (tc = Esc_TextCellOf(item, &cell)))
    {
        rowItem = tc->item;
        kind = EG_CELLS;
        *w = Esc_CellBoxW();
        label = s_TextLabel[rowItem];
    }
    else { Esc_StepBox(Esc_CtlX(right), x, w); return; }

    Esc_RowFlow(kind, rowItem, rowL, rowW,
                label ? label : s_GuiLabel[rowItem], none, &f);
    *x = Esc_CtlX(right);
    if (cell >= 0 && cell < f.n)
    {
        int capW;
        Esc_CellBox(&f, cell, *w, &capW, x);
    }
}

static int Esc_StepMid(const RECT* right, int mx, int page, int item)
{
    int x, w;

    Esc_NumBoxFor(page, item, right, &x, &w);
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
                         int page, int item)
{
    int boxX, boxW;

    Esc_NumBoxFor(page, item, right, &boxX, &boxW);
    return Esc_QuickCurAt(s, len, mx - Esc_StepNumX(boxX, boxW, s));
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
    /* 四边全描: 只画上下两条线时两侧像缺边框 */
    Esc_Outline(surf, x, y, ESC_STEP_BTN_W, ESC_STEP_BTN_H,
                0x7B, 0x79, 0x7B, 180, 1);
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
 * 端点行/字体行的粗体共用这一份口径。 */
static void Esc_SegTextAt(void* surf, const wchar_t* name, int x, int w,
                          int textY, int ulY, int on)
{
    Esc_Text(surf, name, x, textY, w + 2, ESC_BODY_PX, ESC_BODY_SLOT,
             on ? 245 : 145, on ? 225 : 145, on ? 170 : 145);
    Esc_Fill(surf, x, ulY, w, 2,
             on ? 208 : 0x7B, on ? 160 : 0x79, on ? 64 : 0x7B,
             on ? 255 : 140);
}

/* 常规版式: 文字贴下划线基线。 */
static void Esc_SegText(void* surf, const wchar_t* name, int x, int w,
                        int rowY, int bodyH, int on)
{
    Esc_SegTextAt(surf, name, x, w, Esc_SegTextTop(rowY, bodyH),
                  Esc_SegUnderlineY(rowY), on);
}

/* 拖动条轨道的最小可用宽度: 再窄就不像滑条了, 那时宁可换行给它整行宽。 */
#define ESC_TRK_MIN 150

/* 一段文字的原子宽度 (0 宽会让它永远排得下, 给个下限)。 */
template <unsigned N>
static int Esc_NameAtoms(const wchar_t (*names)[N], int n, EscFlow* f)
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
    case EG_VALUE:
    case EG_ROWS:
        f->w[0] = ESC_STEP_W;
        return 1;
    case EG_HOTKEY:
        f->w[0] = ESC_HK_W;
        return 1;
    case EG_TRIO:
        return Esc_NameAtoms(s_GuiLabel + GUI_ITEM_WTH_RAIN, ESC_TRIO_N, f);
    case EG_UPBTN:
        return Esc_NameAtoms(s_GuiLabel + GUI_ITEM_UP_OFF, ESC_UPBTN_N, f);
    case EG_CLOUD:
        return Esc_NameAtoms(s_GuiWthLayer, ESC_CLD_SEG_N, f);
    case EG_SKIN:
        return Esc_NameAtoms(s_GuiSkinName, ESC_SKIN_N, f);
    case EG_LIGHTPARM:
    case EG_CELLS:
    case EG_MIX:
    {
        int page = kind == EG_CELLS ? EDIT_PAGE_TEXT : EDIT_PAGE_GUI;
        int n = kind == EG_CELLS ? Esc_TextCellsFind(item)->n :
                (kind == EG_MIX ? Esc_GuiCellsFind(item)->n : LIGHT_ROW_CELLS);
        for (i = 0; i < n; i++) f->w[i] = Esc_CellWidth(page, item, i);
        *gap = LIGHT_CELL_GAP;
        return n;
    }
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

/* 说明文字每次取一行, 计高与绘制共用字符数和宽度边界。 */
static int Esc_NoteChunk(const wchar_t* s, int maxW, wchar_t* buf)
{
    int n = 0, w = 0;

    while (s[n] && n < 159)
    {
        wchar_t one[2] = { s[n], 0 };
        int cw = Esc_TextWidth(one, ESC_BODY_PX, ESC_BODY_SLOT);
        if (w + cw > maxW && n > 0) break;
        buf[n++] = one[0];
        w += cw;
    }
    buf[n] = 0;
    return n;
}

static int Esc_NoteLines(const wchar_t* s, int rowW)
{
    wchar_t buf[160];
    int n = 0;

    if (!s || !s[0] || rowW <= 0) return 1;
    while (*s) { s += Esc_NoteChunk(s, rowW, buf); n++; }
    return n;
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
    if (kind == EG_NOTE)
    {   /* 说明行: 按行宽折行, 行数决定行高 (绘制读同一份) */
        f->lines = Esc_NoteLines(label, rowW - ESC_TEXT_INSET);
        f->x0line = rowL + ESC_TEXT_INSET;
        return;
    }
    if (kind == EG_FONT)
    {   /* 字体行: 标题 + 三件套固定排一行, 不参与原子流 */
        f->lines = 1;
        return;
    }
    if (kind == EG_TEXT)
    {   /* 条目块的行高只取条目数, 框与按钮位置由绘制和命中共用的几何给出。 */
        Esc_QuickEnsure();
        f->lines = ((s_QCount > 0) ? s_QCount : 1) + 1;
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

/* ---------------- 单位面板左侧按钮: 关闭 / 折叠 / WASD / 收藏 ----------------
 * 一行四段。"关闭"是状态段 (三组全关时亮), 点它 = 三组全关; 其余三段各自
 * 独立开关, 打开任一段就自动脱离"关闭"。 */

static int Esc_UpBtnOn(int seg)
{
    if (seg <= 0)
        return (!UP_BtnShown(0) && !UP_BtnShown(1) && !UP_BtnShown(2)) ? 1 : 0;
    return UP_BtnShown(seg - 1);
}

static void Esc_UpBtnApply(int seg)
{
    if (seg <= 0)
    {
        UP_SetBtnShown(0, 0);
        UP_SetBtnShown(1, 0);
        UP_SetBtnShown(2, 0);
        return;
    }
    UP_SetBtnShown(seg - 1, !UP_BtnShown(seg - 1));
}

/* 云层行的当前段: 云影 / 雾气 / 关 (关也是段)。绘制与命中共用。 */
static int Esc_CloudSeg(void)
{
    int mode = Weather_CloudMode(), i;

    for (i = 0; i < ESC_CLD_SEG_N; i++)
        if (kCldSegMode[i] == mode) return i;
    return ESC_CLD_SEG_N - 1;
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
    /* 两条分隔线: 页名 | 链接行 | 语言切换 (各自独立一条) */
    Esc_Fill(surf, p->left + ESC_PAD, Esc_FootY(p) - ESC_SEP_GAP,
             p->right - p->left - ESC_PAD * 2, 1, 0x7B, 0x79, 0x7B, 160);
    Esc_Fill(surf, p->left + ESC_PAD, y - ESC_SEP_GAP,
             p->right - p->left - ESC_PAD * 2, 1, 0x7B, 0x79, 0x7B, 160);
    Esc_LangLayout(p, xs, ws);
    Esc_DrawSegs(surf, xs, ws, s_LangLabel, ESC_LANG_N, value, y, bodyH);
}

/* ---------------- 链接行 (左栏底部, 语言切换上面) ----------------
 * 每行: 暗色圆底 + 图标 + 文字, 整块可点, 开对应主页; 末行右侧另有版本号。
 * 文案与网址来自语言文件 (GuiGithub / GuiBilibili 及两者的 *Url), 不显示网址。 */

/* 22x22 覆盖率遮罩, 由 icons/icon_a_*.png 生成 (python tools/ghicon.py):
 * 底色当透明、图案当覆盖率, 每格 0~f, 边缘自带抗锯齿。 */
static const char* const kIconGithub[ESC_FOOT_ICON] = {
    "000199620000000037a700",
    "000afffe96adb86aeffe30",
    "000cfbcfffffffffebdf50",
    "000df617cd8789ec30bf50",
    "000df8000000000001cf50",
    "003ef4000000000000afe0",
    "00afa00000000000002df4",
    "00df6000000000000009f8",
    "00ef5000000000000008fa",
    "00cf600000000000000bf7",
    "009f900000000000000ff5",
    "005fe40000000000009fe1",
    "000cfd500000000019ff60",
    "0004cffca500008adff910",
    "0ac319efff2006ffed5000",
    "1dfc302afc0002ee510000",
    "01dfb108fa0001de300000",
    "003efc7cfa0001de300000",
    "0004cffffa0001de300000",
    "000015adfa0001de300000",
    "00000006f80000ce300000",
    "0000000160000056000000",
};

static const char* const kIconBili[ESC_FOOT_ICON] = {
    "0000000000000000000000",
    "0001be8000000005dd2000",
    "0002eff90000005eff5000",
    "00007eff700005eff90000",
    "005bdffffddddefffec700",
    "09ffffffffffffffffffc2",
    "5ffffffffffffffffffff9",
    "cff6000000000000003dfd",
    "fff0000000000000000aff",
    "fff0001100000012000aff",
    "fff003dd300000be600aff",
    "fff005ff500000ffa00aff",
    "fff004ff400000ffa00aff",
    "fff001bc1000008d300aff",
    "fff0000000000000000aff",
    "fff0000000000000000aff",
    "fff0000000000000000aff",
    "cff6000000000000003dfd",
    "5ffffffffffffffffffff9",
    "09ffffffffffffffffffc2",
    "005bddddddddddddddc700",
    "0000000000000000000000",
};

/* 每行的字形与颜色, 下标同 s_LinkText。 */
static const char* const* const kLinkGlyph[ESC_LINK_N] = { kIconBili, kIconGithub };
static const unsigned char kLinkRgb[ESC_LINK_N][3] = {
    {   0, 161, 214 },   /* bilibili: 品牌蓝 */
    { 255, 255, 255 }    /* GitHub: 白 */
};

/* 链接组: 圆底左缘 / 文字左缘 / 第 i 行的顶。绘制与命中共用这几个口径。 */
static int Esc_LinkL(const RECT* p)
{
    return p->left + ESC_PAD + ESC_TEXT_INSET;
}

static int Esc_LinkTextX(const RECT* p)
{
    return Esc_LinkL(p) + ESC_FOOT_ICON + ESC_FOOT_PAD * 2 + ESC_FOOT_GAP;
}

static int Esc_LinkY(const RECT* p, int i)
{
    return Esc_FootY(p) + i * ESC_FOOT_H;
}

/* 圆底 + 字形。hot 时圆底提亮一档。 */
static void Esc_LinkIcon(void* surf, int x, int y, int link, int hot)
{
    unsigned char* base;
    const char* const* glyph = kLinkGlyph[link];
    const unsigned char* rgb = kLinkRgb[link];
    int w = ESC_FOOT_ICON, h = ESC_FOOT_ICON, pitch, i, j;
    int ox = x, oy = y, d = ESC_FOOT_ICON + ESC_FOOT_PAD * 2;

    Esc_Capsule(surf, x - ESC_FOOT_PAD, y - ESC_FOOT_PAD, d, d,
                hot ? 46u : 24u, hot ? 48u : 26u, hot ? 56u : 32u, 235);

    base = Esc_LockRect(surf, &x, &y, &w, &h, &pitch);
    if (!base) return;
    /* 一次锁定整块后逐格写像素: 逐格调 Esc_Fill 会锁/解锁近五百次。 */
    for (j = 0; j < h; j++)
    {
        const char* m = glyph[oy + j - y];
        unsigned short* row = (unsigned short*)(base + (size_t)(y + j) * pitch
                                                + (size_t)x * 2);
        for (i = 0; i < w; i++)
        {
            char c = m[ox + i - x];
            int lv = c <= '9' ? c - '0' : c - 'a' + 10;

            if (lv)
                row[i] = Blend16(row[i], rgb[0], rgb[1], rgb[2], (unsigned)lv * 17u);
        }
    }
    GtUnlock(surf);
}

static void Esc_DrawLinkRow(void* surf, const RECT* p, int link)
{
    int bodyH = Esc_TextHeight(ESC_BODY_PX, ESC_BODY_SLOT);
    int y = Esc_LinkY(p, link);
    int ty = y + (ESC_FOOT_H - bodyH) / 2;
    int hot = s_LinkHot == link;
    int tw = Esc_TextWidth(s_LinkText[link], ESC_BODY_PX, ESC_BODY_SLOT);
    int vw;

    Esc_LinkIcon(surf, Esc_LinkL(p), y + (ESC_FOOT_H - ESC_FOOT_ICON) / 2, link, hot);
    /* 平时白字, 悬停转金; 可点性靠颜色, 不画下划线 */
    Esc_Text(surf, s_LinkText[link], Esc_LinkTextX(p), ty, tw + 2,
             ESC_BODY_PX, ESC_BODY_SLOT,
             hot ? 255 : 236, hot ? 206 : 236, hot ? 110 : 236);

    if (link != ESC_LINK_N - 1) return;
    vw = Esc_TextWidth(s_Ver, ESC_BODY_PX, ESC_BODY_SLOT);
    Esc_Text(surf, s_Ver, p->right - ESC_PAD - ESC_TEXT_INSET - vw, ty,
             vw + 2, ESC_BODY_PX, ESC_BODY_SLOT, 150, 148, 150);
}

static void Esc_DrawFootLines(void* surf, const RECT* p)
{
    int i;

    for (i = 0; i < ESC_LINK_N; i++) Esc_DrawLinkRow(surf, p, i);
}

static int Esc_LinkHit(const RECT* p, int mx, int my)
{
    int i;

    if (mx < Esc_LinkL(p) - ESC_FOOT_PAD) return -1;
    for (i = 0; i < ESC_LINK_N; i++)
    {
        int y = Esc_LinkY(p, i);

        if (my >= y && my < y + ESC_FOOT_H
            && mx < Esc_LinkTextX(p) + Esc_TextWidth(s_LinkText[i], ESC_BODY_PX,
                                                     ESC_BODY_SLOT))
            return i;
    }
    return -1;
}

/* 面板不逐帧重绘, 悬停色得自己触发一次重画。 */
static void Esc_FootHover(HWND hDlg, int mx, int my)
{
    RECT a, b;
    int hot;

    if (!Esc_Layout(hDlg, &a, &b)) return;
    hot = Esc_LinkHit(&a, mx, my);
    if (hot == s_LinkHot) return;
    s_LinkHot = hot;
    Esc_Redraw(hDlg);
}

/* shell32 不在链接库里, 现取一次 ShellExecuteW。 */
static void Esc_OpenLink(int link)
{
    typedef int (WINAPI* EscShellExec)(HWND, const wchar_t*, const wchar_t*,
                                       const wchar_t*, const wchar_t*, int);
    static EscShellExec s_open = NULL;

    if (!s_open)
    {
        HMODULE lib = LoadLibraryA("shell32.dll");

        if (lib) s_open = (EscShellExec)GetProcAddress(lib, "ShellExecuteW");
    }
    if (!s_open) { LogLine("esc: no ShellExecuteW"); return; }
    s_open(NULL, L"open", s_LinkUrl[link], NULL, NULL, SW_SHOWNORMAL);
}

/* 分段行: 段落在第几行由控件流给, 文字与横条的相对位置见 Esc_SegText。 */
template <unsigned N>
static void Esc_DrawSegRow(void* surf, const EscFlow* f, int rowL, int rowW,
                           int rowY, int bodyH, const wchar_t* label,
                           const wchar_t (*names)[N], unsigned selected)
{
    int i;
    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 1);
    for (i = 0; i < f->n; i++)
        Esc_SegText(surf, names[i], f->x[i], f->w[i],
                    rowY + f->ln[i] * ESC_ROW_H, bodyH, (selected >> i) & 1u);
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

/* 数值框里的文字 + 编辑光标 (值行与拖动条共用)。
 * dim=1: 这个值当前不起作用 (深灰显示, 例如顶部模式下用不到的底边距)。 */
static void Esc_DrawNumText(void* surf, int boxX, int boxY, int boxW,
                            int textY, int value, int editing,
                            const wchar_t* text, int dim)
{
    wchar_t num[16];
    int numX;
    unsigned v = (dim && !editing) ? 110u : 238u;

    if (editing) lstrcpynW(num, s_EditBuf, 16);
    else if (text) lstrcpynW(num, text, 16);
    else Esc_SignedToW(num, value, 16);

    numX = Esc_StepNumX(boxX, boxW, num);
    Esc_Text(surf, num, numX, textY, boxW, ESC_BODY_PX, ESC_BODY_SLOT,
             v, v, v);

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

/* 数值框: 底 + 边框 (编辑中换成金色) + 数字与光标。dim = 该值当前不起作用
 * (深灰, 比正常暗一档; 编辑中仍按金色边框, 表示"能改, 只是现在不生效")。 */
static void Esc_DrawEditBox(void* surf, int x, int y, int w, int textY,
                            int value, int editing, const wchar_t* text,
                            int dim)
{
    unsigned bg = editing ? 40u : (dim ? 18u : 24u);
    unsigned r = editing ? 208u : (dim ? 0x45u : 0x7Bu);
    unsigned g = editing ? 160u : (dim ? 0x43u : 0x79u);
    unsigned b = editing ? 64u : (dim ? 0x45u : 0x7Bu);
    unsigned a = editing ? 255u : (dim ? 90u : 150u);

    Esc_Fill(surf, x, y, w, ESC_STEP_BTN_H, bg, bg, bg, dim ? 170u : 230u);
    Esc_Outline(surf, x, y, w, ESC_STEP_BTN_H, r, g, b, a, 1);
    Esc_DrawNumText(surf, x, y, w, textY, value, editing, text, dim);
}

/* 可编辑步进器: 普通数值行与速度滑条共用 [-][数值][+]。 */
static void Esc_DrawStepper(void* surf, int ctlX, int ctlY, int bodyH,
                            int value, int minValue, int maxValue,
                            int editing, const wchar_t* text)
{
    int boxX, boxW, btnY = ctlY + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;

    Esc_StepButton(surf, ctlX, btnY, L"-", value > minValue);
    Esc_StepButton(surf, ctlX + ESC_STEP_W - ESC_STEP_BTN_W, btnY, L"+",
                   value < maxValue);
    Esc_StepBox(ctlX, &boxX, &boxW);
    Esc_DrawEditBox(surf, boxX, btnY, boxW, ctlY + (ESC_ROW_H - bodyH) / 2,
                    value, editing, text, 0);
}

static void Esc_DrawValueRow(void* surf, const EscFlow* f, int rowL, int rowW,
                             int rowY, int bodyH, const wchar_t* label,
                             int value, int minValue, int maxValue,
                             int editing, const wchar_t* text)
{
    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 0);
    Esc_DrawStepper(surf, f->x[0], rowY + f->ln[0] * ESC_ROW_H, bodyH,
                    value, minValue, maxValue, editing, text);
}

/* 坐标格当前是否真的起作用 (设置页灰显的判据): 左 只在"左对齐"下生效,
 * 顶 只在"顶部"显示下, 底 只在"底部"显示下。只是显示提示, 不拦住编辑 ——
 * 先填值再切位置是常见用法。
 * 只有 [MsgText] 这三个格子会灰; 其它模块的 opt 号与它们重叠, 必须带 mod 判。 */
static int Esc_TextCellActive(int mod, int opt)
{
    if (mod != OPM_MSG) return 1;
    if (opt == MSG_OPT_CAMP_POSX)
        return !MsgText_GetOption(MSG_OPT_CAMP_CENTERED);
    if (opt == MSG_OPT_CAMP_POSY)
        return !MsgText_GetOption(MSG_OPT_CAMP_BOTTOM);
    if (opt == MSG_OPT_CAMP_BOTTOMMARGIN)
        return MsgText_GetOption(MSG_OPT_CAMP_BOTTOM);
    return 1;
}

/* 数值格和混合格共用绘制; 非生效边距灰显, 开关胶囊不带小标题。 */
static void Esc_DrawCellsRow(void* surf, const EscFlow* f, int rowL,
                             int rowW, int rowY, int bodyH,
                             const wchar_t* label, int page, int item)
{
    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 0);
    for (int i = 0; i < f->n; i++)
    {
        EscCell c = Esc_Cell(page, item, i);
        const EscTextNum* num = page == EDIT_PAGE_TEXT ? Esc_TextNumFind(c.item) : NULL;
        int dim = num && !Esc_TextCellActive(num->mod, num->opt);
        int capW, x, y = rowY + f->ln[i] * ESC_ROW_H;
        int textY = y + (ESC_ROW_H - bodyH) / 2;
        unsigned color = dim ? 105u : 230u;
        Esc_CellBox(f, i, c.width, &capW, &x);
        Esc_Text(surf, c.label, f->x[i], textY, capW,
                 ESC_BODY_PX, ESC_BODY_SLOT, color, color, color);
        if (c.toggle)
            Esc_Switch(surf, x, y + (ESC_ROW_H - ESC_SW_H) / 2, c.toggle->get());
        else
            Esc_DrawEditBox(surf, x, y + (ESC_ROW_H - ESC_STEP_BTN_H) / 2,
                            c.width, textY, Esc_Number(page, c.item).value,
                            Esc_EditIs(page, c.item), NULL, dim);
    }
}

/* 滑条绘制与点击、拖动共用几何; 端点行数值只读, 速度数值可编辑。 */
static void Esc_DrawSlideRow(void* surf, const EscFlow* f, int rowL, int rowY,
                             int rowW, int bodyH, const wchar_t* label,
                             int item, int page)
{
    const EscNum* n = Esc_NumFind(item);
    EscSlideGeom g = Esc_SlideGeom(item, rowL, rowW, label);
    int value = item == s_SlideDrag ? s_SlideVal : n->get();
    int ctlY = Esc_FlowCtlY(f, rowY);
    int knobX = Esc_TrackXFor(value, g.trkX, g.trkW, n->min, n->max);
    int ty = ctlY + (ESC_ROW_H - ESC_SLIDE_TRK_H) / 2;
    int ky = ctlY + (ESC_ROW_H - ESC_SLIDE_KNOB_H) / 2;
    int btnY = ctlY + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;

    Esc_Text(surf, label, rowL + ESC_TEXT_INSET, Esc_SegTextTop(ctlY, bodyH),
             g.hitL - ESC_LBL_GAP - (rowL + ESC_TEXT_INSET), ESC_BODY_PX,
             ESC_BODY_SLOT, 232, 232, 232);
    if (g.trkW > 8)
    {
        Esc_Capsule(surf, g.trkX, ty, g.trkW, ESC_SLIDE_TRK_H, 44, 44, 44, 235);
        if (knobX - g.trkX > 8)
            Esc_Capsule(surf, g.trkX, ty, knobX - g.trkX, ESC_SLIDE_TRK_H,
                        208, 160, 64, 235);
    }
    Esc_Fill(surf, knobX - ESC_SLIDE_KNOB_W / 2, ky,
             ESC_SLIDE_KNOB_W, ESC_SLIDE_KNOB_H, 236, 236, 236, 255);
    Esc_Outline(surf, knobX - ESC_SLIDE_KNOB_W / 2, ky,
                ESC_SLIDE_KNOB_W, ESC_SLIDE_KNOB_H, 20, 20, 20, 220, 1);
    if (g.ends)
    {
        int pick = value <= n->min ? 0 : (value >= n->max ? 1 : -1);
        Esc_DrawSegs(surf, g.xs, g.ws, g.ends, 2, pick, ctlY, bodyH);
        Esc_DrawEditBox(surf, g.numX, btnY, g.numW,
                        btnY + (ESC_STEP_BTN_H - bodyH) / 2, value, 0, NULL, 0);
    }
    else
        Esc_DrawStepper(surf, Esc_RowCtlX(rowL + rowW), ctlY, bodyH,
                        value, n->min, n->max, Esc_EditIs(page, item),
                        value > ESC_SPEED_MAX ? L"MAX" : NULL);
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

/* 预设文本块的几何: 条目框铺到按钮列左侧, 右端依次 [-] [+]。 */
static void Esc_QuickGeom(int rowL, int rowW, int* boxX, int* boxW,
                          int* minusX, int* plusX)
{
    int right = rowL + rowW - ESC_CTL_RIGHT;

    *plusX = right - ESC_STEP_BTN_W;
    *minusX = *plusX - ESC_QUICK_GAP - ESC_STEP_BTN_W;
    *boxX = rowL + ESC_TEXT_INSET;
    *boxW = *minusX - ESC_QUICK_GAP - *boxX;
    if (*boxW < 0) *boxW = 0;
}

/* 一个条目框: 未编辑时从行首截断显示; 编辑中按光标横向滚动 (超宽只显示光标附近)。
 * 空框不写占位字 —— 空框本身就是"这条没内容"。 */
/* 单行文字框 (预设文本条目 / 字体名共用): 编辑时按光标横向滚动。 */
static void Esc_DrawLineBox(void* surf, int boxX, int boxY, int boxW,
                            int textY, int bodyH, const wchar_t* s,
                            int editing, const LiEdit* st)
{
    int innerW = boxW - ESC_TEXT_INSET * 2;
    int len, curl, start = 0, x, wStart, wCur = 0;

    if (innerW < 8) return;

    Esc_Fill(surf, boxX, boxY, boxW, ESC_STEP_BTN_H,
             editing ? 44u : 24u, editing ? 44u : 24u, editing ? 44u : 24u, 230);
    Esc_Outline(surf, boxX, boxY, boxW, ESC_STEP_BTN_H,
                editing ? 208u : 0x7Bu, editing ? 160u : 0x79u,
                editing ? 64u : 0x7Bu, editing ? 255u : 150u, editing ? 2 : 1);

    len = lstrlenW(s);
    curl = (editing && st) ? st->cur : len;
    if (curl < 0) curl = 0;
    if (curl > len) curl = len;
    if (editing && curl > 0)
    {
        wCur = Esc_TextWidth(s + curl, ESC_BODY_PX, ESC_BODY_SLOT);
        start = Esc_QuickStart(s, curl, innerW, wCur);
    }
    x = boxX + ESC_TEXT_INSET;
    wStart = Esc_TextWidth(s + start, ESC_BODY_PX, ESC_BODY_SLOT);
    if (s[start])
        Esc_Text(surf, s + start, x, textY, innerW, ESC_BODY_PX, ESC_BODY_SLOT,
                 editing ? 245u : 232u, editing ? 245u : 232u,
                 editing ? 245u : 232u);
    if (editing && st && LiBlinkOn(st))
    {
        int cx = x + (wStart - wCur);

        if (cx < x) cx = x;
        if (cx > x + innerW) cx = x + innerW;
        Esc_Fill(surf, cx, textY, 1, bodyH, 245, 245, 245, 220);
    }
}

static void Esc_QuickBox(void* surf, int boxX, int boxY, int boxW,
                         int textY, int bodyH, int idx)
{
    int editing = (s_QOn && s_QKind == QK_PRESET && s_QEdit == idx);

    Esc_DrawLineBox(surf, boxX, boxY, boxW, textY, bodyH, s_QItem[idx],
                    editing, editing ? &s_QSt : NULL);
}

/* 预设文本块: 标题一行, 下面每条一行、右端 [-][+]。 */
static void Esc_DrawQuickRow(void* surf, int rowL, int rowW, int rowY,
                             int bodyH, const wchar_t* label)
{
    int boxX, boxW, minusX, plusX, i;

    Esc_QuickEnsure();
    Esc_QuickGeom(rowL, rowW, &boxX, &boxW, &minusX, &plusX);
    Esc_Text(surf, label, rowL + ESC_TEXT_INSET,
             rowY + (ESC_ROW_H - bodyH) / 2, rowW, ESC_BODY_PX, ESC_BODY_SLOT,
             230, 230, 230);
    for (i = 0; i < s_QCount; i++)
    {
        int ry = rowY + (i + 1) * ESC_ROW_H;
        int btnY = ry + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;

        Esc_QuickBox(surf, boxX, btnY, boxW, ry + (ESC_ROW_H - bodyH) / 2,
                     bodyH, i);
        Esc_StepButton(surf, minusX, btnY, L"-", 1);
        Esc_StepButton(surf, plusX, btnY, L"+",
                       (s_QCount < ESC_QUICK_N));
    }
}

/* ---------------- 字体行几何 ----------------
 * 一行四件: [字体名框][字号][字号框][粗体]。名字框封顶 (字体名都很短),
 * 否则"字号"两个字会被挤没; 整块右对齐, 多余空白落在标题与名字框之间。
 * 绘制/命中/数值框定位都读这一份。 */
#define ESC_FONT_BOX_W    34    /* 字号数值框 */
#define ESC_FONT_GAP      6
#define ESC_FONT_FACE_MAX 150   /* 字体名框上限 (再宽也只是空着) */
#define ESC_FONT_FACE_MIN 48

static void Esc_FontGeom(int rowL, int rowW, const wchar_t* label,
                         int* faceX, int* faceW, int* capX, int* capW,
                         int* pxX, int* pxW, int* bdX, int* bdW)
{
    int xIns = rowL + ESC_TEXT_INSET;
    int right = rowL + rowW - ESC_CTL_RIGHT;
    int lw = (label && label[0])
                 ? Esc_TextWidth(label, ESC_BODY_PX, ESC_BODY_SLOT) : 0;
    int rest, avail;

    *capW = Esc_TextWidth(s_TextPxCap, ESC_BODY_PX, ESC_BODY_SLOT);
    *pxW = ESC_FONT_BOX_W;
    *bdW = Esc_TextWidth(s_TextBoldOn, ESC_BODY_PX, ESC_BODY_SLOT) + 4;
    /* 字号框右边那一串 (字号 + 数值框 + 粗体) 的固定宽度; 名字框吃剩下的。 */
    rest = *bdW + ESC_FONT_GAP + *pxW + ESC_FONT_GAP + *capW + ESC_FONT_GAP;

    avail = right - rest - (xIns + lw + ESC_LBL_GAP);
    if (avail > ESC_FONT_FACE_MAX) avail = ESC_FONT_FACE_MAX;
    if (avail < ESC_FONT_FACE_MIN) avail = ESC_FONT_FACE_MIN;
    *faceX = right - rest - avail;
    *faceW = avail;
    *capX = *faceX + avail + ESC_FONT_GAP;
    *pxX = *capX + *capW + ESC_FONT_GAP;
    *bdX = *pxX + *pxW + ESC_FONT_GAP;
}

/* 字体行: 标题 + 字体名框 + "字号" + 字号框 + 粗体 (下划线式文本开关)。 */
static void Esc_DrawFontRow(void* surf, int rowL, int rowY, int rowW,
                            int bodyH, const wchar_t* label,
                            const EscTextFont* ft)
{
    int fx, fw, cx, cw, px, pw, bx, bw;
    int textY = rowY + (ESC_ROW_H - bodyH) / 2;
    int boxY = rowY + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;
    wchar_t face[ESC_FACE_CAP];
    int editingFace = (s_QOn && s_QKind == QK_FACE && s_QFaceTarget == ft->target);

    Esc_FontGeom(rowL, rowW, label, &fx, &fw, &cx, &cw, &px, &pw, &bx, &bw);
    Esc_Text(surf, label, rowL + ESC_TEXT_INSET, textY, rowW,
             ESC_BODY_PX, ESC_BODY_SLOT, 230, 230, 230);
    Esc_Text(surf, s_TextPxCap, cx, textY, cw, ESC_BODY_PX, ESC_BODY_SLOT,
             200, 200, 200);

    if (editingFace) lstrcpynW(face, s_FaceBuf, ESC_FACE_CAP);
    else Esc_FaceGet(ft->target, face, ESC_FACE_CAP);
    Esc_DrawLineBox(surf, fx, boxY, fw, textY, bodyH, face,
                    editingFace, editingFace ? &s_QSt : NULL);

    Esc_DrawEditBox(surf, px, boxY, pw, textY, Esc_OptGet(ft->mod, ft->pxOpt),
                    Esc_EditIs(EDIT_PAGE_TEXT, ft->px), NULL, 0);
    /* 粗体用下划线文本, 但文字基线取行内居中: 同行邻居都是框, 贴底基线会比
     * 它们低 3px。 */
    Esc_SegTextAt(surf, s_TextBoldOn, bx, bw, textY,
                  Esc_SegUnderlineY(rowY), Esc_OptGet(ft->mod, ft->boldOpt));
}

/* 热键行: 标题 + 右侧值框。捕获中框变黄并显示提示 (或已攒到的组合),
 * 其余时候显示规范化后的热键串。框的横向位置由控件流给出 (Esc_RowAtoms)。 */
static void Esc_DrawHotkeyRow(void* surf, const EscFlow* f, int rowL,
                              int rowW, int rowY, int bodyH,
                              const wchar_t* label, int item)
{
    int rowLine = rowY + f->ln[0] * ESC_ROW_H;
    int textY = rowLine + (ESC_ROW_H - bodyH) / 2;
    int boxY = rowLine + (ESC_ROW_H - ESC_STEP_BTN_H) / 2;
    int boxX = f->x[0], boxW = f->w[0];
    int cap = (item == s_HkCap);
    wchar_t txt[64];

    Esc_FlowLabel(surf, f, rowL, rowW, rowY, bodyH, label, 0);
    if (cap) lstrcpynW(txt, s_HkCapTxt, 64);
    else     Esc_HotkeyText(item, txt, 64);

    Esc_Fill(surf, boxX, boxY, boxW, ESC_STEP_BTN_H,
             cap ? 46u : 24u, cap ? 40u : 24u, cap ? 20u : 24u, 235);
    Esc_Outline(surf, boxX, boxY, boxW, ESC_STEP_BTN_H,
                cap ? 208u : 0x7Bu, cap ? 160u : 0x79u,
                cap ? 64u : 0x7Bu, cap ? 255u : 150u, cap ? 2 : 1);
    Esc_Text(surf, txt, boxX + ESC_TEXT_INSET, textY,
             boxW - ESC_TEXT_INSET * 2, ESC_BODY_PX, ESC_BODY_SLOT,
             cap ? 250u : 232u, cap ? 235u : 232u, cap ? 170u : 232u);
}

/* 说明行: 灰字, 按行宽折行 (与 Esc_NoteLines 同一算法)。 */
static void Esc_DrawNoteRow(void* surf, int rowL, int rowY, int rowW,
                            int bodyH, const wchar_t* text)
{
    int x = rowL + ESC_TEXT_INSET;
    int maxW = rowW - ESC_TEXT_INSET;
    int y = rowY;

    if (!text || !text[0] || maxW <= 0) return;
    while (*text)
    {
        wchar_t buf[160];

        text += Esc_NoteChunk(text, maxW, buf);
        Esc_Text(surf, buf, x, y + (ESC_ROW_H - bodyH) / 2, maxW,
                 ESC_BODY_PX, ESC_BODY_SLOT, 152, 152, 152);
        y += ESC_ROW_H;
    }
}

/* 各页共用控件分派; 页身份只决定数据来源, 布局与绘制按控件类型复用。 */
static void Esc_DrawPageRow(void* surf, int rowL, int rowY, int rowW,
                            int bodyH, int kind, int item, const EscPage* pg)
{
    int text = pg->page == EDIT_PAGE_TEXT;
    const wchar_t* label = pg->labels[item];
    EscSegs sg = pg->segs(item);
    EscFlow f;

    Esc_RowFlow(kind, item, rowL, rowW, label, sg, &f);
    switch (kind)
    {
    case EG_SWITCH:
    {
        const EscToggle* g = text ? NULL : Esc_ToggleFind(item);
        const EscTextToggle* t = text ? Esc_TToggleFind(item) : NULL;
        Esc_DrawSwitchRow(surf, &f, rowL, rowW, rowY, bodyH, label,
                          t ? Esc_TToggleVal(t) : (g ? g->get() : 0));
        break;
    }
    case EG_ROWS:
    case EG_VALUE:
    {
        const EscTextNum* n = text ? Esc_TextNumFind(item) : NULL;
        Esc_DrawValueRow(surf, &f, rowL, rowW, rowY, bodyH, label,
                         n ? Esc_OptGet(n->mod, n->opt) : SWS_MaxRows(),
                         n ? n->min : ESC_ROWS_MIN, n ? n->max : ESC_ROWS_MAX,
                         Esc_EditIs(pg->page, item), NULL);
        break;
    }
    case EG_CHOICE:
    case EG_SKIN:
    case EG_CLOUD:
    {
        const EscTextChoice* t = text ? Esc_TChoiceFind(item) : NULL;
        const EscChoice* g = text ? NULL : Esc_ChoiceFind(item);
        int value = t ? Esc_TChoiceValue(t) : (g ? Esc_ChoiceValue(g) :
                    (kind == EG_SKIN ? SidebarSkin_Get() : Esc_CloudSeg()));
        EscName* names = kind == EG_SKIN ? s_GuiSkinName :
                         (kind == EG_CLOUD ? s_GuiWthLayer : sg.names);
        if (names) Esc_DrawSegRow(surf, &f, rowL, rowW, rowY, bodyH,
                                  label, names, value >= 0 && value < f.n ? 1u << value : 0);
        break;
    }
    case EG_SLIDE:
        Esc_DrawSlideRow(surf, &f, rowL, rowY, rowW, bodyH, label, item, pg->page);
        break;
    case EG_TRIO:
    case EG_UPBTN:
    {
        unsigned selected = 0;
        int i, first = kind == EG_TRIO ? GUI_ITEM_WTH_RAIN : GUI_ITEM_UP_OFF;
        for (i = 0; i < f.n; i++)
        {
            const EscToggle* t = kind == EG_TRIO ? Esc_ToggleFind(first + i) : NULL;
            if (kind == EG_TRIO ? (t && t->get()) : Esc_UpBtnOn(i))
                selected |= 1u << i;
        }
        Esc_DrawSegRow(surf, &f, rowL, rowW, rowY, bodyH,
                       label, s_GuiLabel + first, selected);
        break;
    }
    case EG_LIGHTPARM:
    case EG_CELLS:
    case EG_MIX:
        Esc_DrawCellsRow(surf, &f, rowL, rowW, rowY, bodyH, label, pg->page, item);
        break;
    case EG_COLOR:
        Esc_DrawColorRow(surf, &f, rowL, rowW, rowY, bodyH, label);
        break;
    case EG_NOTE:
        Esc_DrawNoteRow(surf, rowL, rowY, rowW, bodyH, label);
        break;
    case EG_FONT:
        Esc_DrawFontRow(surf, rowL, rowY, rowW, bodyH, label,
                        Esc_TextFontFind(item));
        break;
    case EG_TEXT:
        Esc_DrawQuickRow(surf, rowL, rowW, rowY, bodyH, label);
        break;
    case EG_HOTKEY:
        Esc_DrawHotkeyRow(surf, &f, rowL, rowW, rowY, bodyH, label, item);
        break;
    }
}

#define ESC_ROWS_N(tab) ((int)(sizeof(tab) / sizeof((tab)[0])))

/* 页顺序与 EDIT_PAGE_* 一致; 标题、行表、滚动和文案来源集中定义。 */
static const EscPage s_Pages[] = {
    { s_GuiLayout, ESC_ROWS_N(s_GuiLayout), s_GuiTitle, s_GuiGroup,
      s_GuiLabel, EscGuiSegs, EDIT_PAGE_GUI, &s_GuiScroll, &s_GuiMissing },
    { s_TextLayout, ESC_ROWS_N(s_TextLayout), s_TextTitle, NULL,
      s_TextLabel, EscTextSegs, EDIT_PAGE_TEXT, &s_TextScroll, &s_TextMissing },
    { s_DispLayout, ESC_ROWS_N(s_DispLayout), s_DispTitle, s_GuiGroup,
      s_GuiLabel, EscGuiSegs, EDIT_PAGE_DISP, &s_DispScroll, &s_GuiMissing },
    { s_KeyLayout, ESC_ROWS_N(s_KeyLayout), s_KeyTitle, s_GuiGroup,
      s_GuiLabel, EscGuiSegs, EDIT_PAGE_KEYS, &s_KeyScroll, &s_KeyMissing },
    { s_CtrlLayout, ESC_ROWS_N(s_CtrlLayout), s_CtrlTitle, s_GuiGroup,
      s_GuiLabel, EscGuiSegs, EDIT_PAGE_CTRL, &s_CtrlScroll, &s_GuiMissing }
};

static const EscPage* Esc_PageAt(int sel)
{
    return &s_Pages[sel >= 0 && sel < ESC_ROWS_N(s_Pages) ? sel : EDIT_PAGE_GUI];
}

/* 各页共用的行循环: 组标题 / 分隔条 / 行底板在这里, 控件交给 Esc_DrawPageRow。
 * 滚出视野的行不画但仍占位 (每页都可滚, 设置页行数会随功能增加)。
 * 鼠标悬停的内容行加一层暗紫高亮 (组标题/分隔条不参与)。 */
static void Esc_RowsPanel(void* surf, RECT* p, const EscPage* pg, HWND hDlg)
{
    int w, bodyH, rowL, rowW, i, y, next, rh, kind, item;
    int clipTop, clipBottom, maxScroll;
    int hovX = -0x40000000, hovY = -0x40000000;
    POINT pt;

    if (!Esc_PanelFrame(surf, p, pg->title)) return;

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

    next = clipTop - *pg->scroll;
    s_ClipTop = clipTop;
    s_ClipBot = clipBottom;
    for (i = 0; i < pg->n; i++)
    {
        kind = pg->rows[i].kind;
        item = pg->rows[i].item;

        rh = Esc_RowStep(p, pg, i, &next, &y);
        if (y + rh <= clipTop || y >= clipBottom) continue;

        if (kind == EG_SEP)
        {   /* 只有页内分节画线 (组标题 + 组间距已足够分隔); 2px 压暗, 1px 亮灰像硬边框。 */
            Esc_Fill(surf, rowL + ESC_TEXT_INSET, y + (rh - 2) / 2,
                     rowW - ESC_TEXT_INSET * 2, 2, 0x4E, 0x4C, 0x4E, 150);
        }
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
            Esc_DrawPageRow(surf, rowL, y, rowW, bodyH, kind, item, pg);
            /* 联机只读项: 整行压一层暗色。压在这里而不是让每个行类型自己
             * 换颜色 —— 行类型会越加越多, 一处漏了就看着像"能点"。 */
            if (Esc_MpLocked(pg->page, item))
                Esc_Fill(surf, rowL, y, rowW, rh, 0, 0, 0, 120);
        }
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
    Esc_DrawFootLines(surf, &a);
    Esc_DrawLangBottom(surf, &a);
    /* 菜单暂停主循环, 这两页仍须同步原版速度控件的改动。 */
    if (s_Sel == EDIT_PAGE_GUI || s_Sel == EDIT_PAGE_CTRL) GameSpeed_Poll();
    Esc_RowsPanel(surf, &b, Esc_PageAt(s_Sel), hDlg);
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
    for (i = 0; i < s_PendStrN; i++)
        Esc_WriteIniRaw(s_PendStr[i].section, s_PendStr[i].key, s_PendStr[i].val);
    s_PendStrN = 0;
    /* 预设文本串比 EscPending.value 长得多, 单独一份缓冲, 开关是 dirty 标志。 */
    if (s_PendQuickDirty)
    {
        Esc_WriteIniRaw(MSG_SECTION, "QuickText", s_PendQuick);
        s_PendQuickDirty = 0;
    }
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

    if (page != EDIT_PAGE_TEXT)
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
    Esc_OptSet(num->mod, num->opt, value);
    Esc_QueueValue(Esc_OptSection(num->mod), num->key, value);
}

/* 步进器带按钮, 格子直接进入编辑; 数值规格来自同一入口。 */
static void Esc_NumberEdit(HWND hDlg, const RECT* right, int mx, int page,
                           int item, int stepper = 0)
{
    EscNumber n = Esc_Number(page, item);
    if (stepper && Esc_StepHit(right, mx, &n.value, n.min, n.max, n.step))
    {
        Esc_ApplyNum(page, item, n.value);
        Esc_Redraw(hDlg);
    }
    else Esc_EditBegin(hDlg, right, mx, page, item, n.value, n.min, n.max);
}

/* 快照打开设置页时的运行期值; 取消时恢复, INI 队列直接丢弃。 */
static void Esc_SaveSettings(void)
{
#define ESC_GET(n, g, s) s_Saved.n = g();
    ESC_SETTING_LIST(ESC_GET)
#undef ESC_GET
    s_Saved.upBtn[0] = UP_BtnShown(0);
    s_Saved.upBtn[1] = UP_BtnShown(1);
    s_Saved.upBtn[2] = UP_BtnShown(2);
    s_Saved.color = PlayerColor_Current();
    Lighting_SaveState();
    MsgText_SaveOptions();
    SWPanel_SaveOptions();
    VecText_SaveOptions();
    PowerDisplay_SaveOptions();
    /* 预设文本: 打开菜单时读一次 ini 并留底, 取消要能整串退回去。 */
    Esc_QuickEnsure();
    {
        int i;

        for (i = 0; i < ESC_QUICK_N; i++)
        {
            int k;
            for (k = 0; k < ESC_QUICK_LINE; k++)
                s_QItemBak[i][k] = s_QItem[i][k];
        }
        s_QCountBak = s_QCount;
        s_QBakOk = 1;
    }
    /* 热键原值 (取消回滚用; 空串 = ini 里本来没这一行)。 */
    {
        int i;

        for (i = 0; i < ESC_HK_N; i++)
            OU_ReadString(s_Hotkeys[i].section, s_Hotkeys[i].key, "",
                          s_HkBak[i], (int)sizeof(s_HkBak[i]));
    }
    s_SavedOk = 1;
}

static void Esc_RestoreSettings(void)
{
    if (!s_SavedOk) return;
    MsgText_RestoreOptions();
    SWPanel_RestoreOptions();
    VecText_RestoreOptions();
    PowerDisplay_RestoreOptions();
    PowerDisplay_Invalidate();     /* 底电位图按旧字面生成, 缓存键里没有字面 */
#define ESC_SET(n, g, s) s(s_Saved.n);
    ESC_SETTING_LIST(ESC_SET)
#undef ESC_SET
    UP_SetBtnShown(0, s_Saved.upBtn[0]);
    UP_SetBtnShown(1, s_Saved.upBtn[1]);
    UP_SetBtnShown(2, s_Saved.upBtn[2]);
    Lighting_RestoreState();
    SidebarSkin_Poll();
    if (s_Saved.color >= 0) PlayerColor_Set(s_Saved.color);
    /* 预设文本一起退回 (已经生效的那次 ChatEdit_SetQuickText 也要收回,
     * 但不排落盘 —— 恢复值不是改动)。 */
    if (s_QBakOk)
    {
        int i;

        if (s_QOn) Esc_QuickClose(NULL);
        for (i = 0; i < ESC_QUICK_N; i++)
        {
            int k;
            for (k = 0; k < ESC_QUICK_LINE; k++)
                s_QItem[i][k] = s_QItemBak[i][k];
        }
        s_QCount = s_QCountBak;
        s_PendQuickDirty = 0;
        Esc_QuickSync(0);
    }
    /* 热键: 改键是"立刻写盘 + 立刻回读"的, 取消要把 ini 与模块状态一起退回去。
     * 备份为空 = ini 里本来没这一行, 写回默认串 (行为与缺键一致)。 */
    {
        int i;

        for (i = 0; i < ESC_HK_N; i++)
        {
            const char* v = s_HkBak[i][0] ? s_HkBak[i] : s_Hotkeys[i].def;

            Esc_WriteIniRaw(s_Hotkeys[i].section, s_Hotkeys[i].key, v);
            if (s_Hotkeys[i].reload) s_Hotkeys[i].reload();
        }
    }
}

static int Esc_HitLeft(const RECT* a, int mx, int my, int* index)
{
    int i, rowY, rowL = a->left + ESC_PAD;
    int rowW = (a->right - a->left) - ESC_PAD * 2;

    for (i = 0, rowY = Esc_ContentTop(a, 0); i < ESC_LEFT_ROWS; i++, rowY += ESC_ROW_H)
    {
        if (rowY + ESC_ROW_H > Esc_FootY(a) - ESC_SEP_GAP) break;
        if (EscIn(rowL, rowY, rowW, ESC_ROW_H, mx, my)) { *index = i; return 1; }
    }
    return 0;
}

/* 行表命中: 鼠标落在哪一行 (返回行表下标, *rowY 写出该行顶)。
 * clipTop 与 top 分开传: top 是滚过之后的起点, 可见性仍按 clipTop 判。 */
static int Esc_RowAt(const RECT* b, const EscPage* pg, int top, int clipTop,
                     int mx, int my, int* row, int* rowY)
{
    int i, y, next = top;
    if (!EscIn(b->left + ESC_PAD, clipTop, b->right - b->left - ESC_PAD * 2,
               b->bottom - ESC_PAD - clipTop, mx, my)) return 0;
    for (i = 0; i < pg->n; i++)
    {
        Esc_RowStep(b, pg, i, &next, &y);
        if (my >= y && my < next)
        {
            *row = i;
            if (rowY) *rowY = y;
            return 1;
        }
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
    if (kind == EG_CELLS || kind == EG_MIX || kind == EG_LIGHTPARM)
        return Esc_CellItemAt(&f, kind == EG_CELLS ? EDIT_PAGE_TEXT : EDIT_PAGE_GUI,
                              item, rowY, mx, my) >= 0;
    if (kind == EG_NOTE)
        return 0;                       /* 说明文字不可点 */
    if (kind == EG_FONT)
    {
        int fx, fw, cx, cw, px, pw, bx, bw;

        Esc_FontGeom(rowL, rowW, label, &fx, &fw, &cx, &cw, &px, &pw, &bx, &bw);
        return mx >= fx && mx < bx + bw && my >= rowY && my < rowY + ESC_ROW_H;
    }
    if (kind == EG_TEXT)
        /* 整块 (标题行 + 每条一行) 都算控件区: 点到哪条框/哪个按钮由
         * Esc_TextApply 再细判, 这里只挡"完全在块外"的点击。 */
        return my >= rowY && my < rowY + f.lines * ESC_ROW_H;
    if (kind == EG_COLOR)
        return Esc_ColorSegmentAt(rowL, rowW, rowY, mx, my) >= 0;
    if (kind == EG_SLIDE)
    {
        EscSlideGeom g = Esc_SlideGeom(item, rowL, rowW, label);
        return EscIn(g.hitL, Esc_FlowCtlY(&f, rowY),
                     g.hitR - g.hitL, ESC_ROW_H, mx, my);
    }
    /* 分段 / 天气 / 光照格: 逐个原子框命中 */
    return Esc_FlowAtomAt(&f, rowY, mx, my) >= 0;
}

/* 当前页的行命中: 先找行再判控件。控件位置与绘制同出一源 (Esc_RowFlow), 所以
 * "画在哪就点在哪"。返回 1 并写出该行的 item 与行顶。 */
static int Esc_HitPage(const RECT* b, const EscPage* pg,
                       int mx, int my,
                       int* index, int* rowTop)
{
    int row, rowY = 0, clipTop;

    Esc_PageClampScroll(b, pg);
    clipTop = Esc_ContentTop(b, *pg->missing);
    if (!Esc_RowAt(b, pg, clipTop - *pg->scroll, clipTop, mx, my, &row, &rowY))
        return 0;
    if (!Esc_CtlHit(pg->rows[row].kind, pg->rows[row].item,
                    pg->labels[pg->rows[row].item], pg->segs(pg->rows[row].item),
                    b->left + ESC_PAD, (b->right - b->left) - ESC_PAD * 2,
                    rowY, mx, my))
        return 0;
    *index = pg->rows[row].item;
    if (rowTop) *rowTop = rowY;
    return 1;
}

static int Esc_HkRaw(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}

/* 修饰键 -> OU_MOD_*; 其余回 0 (当主键/附加键)。左右键码一起认:
 * GetAsyncKeyState(VK_CONTROL) 与 VK_LCONTROL 会同时报。 */
static unsigned Esc_HkModBit(int vk)
{
    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
        return OU_MOD_CTRL;
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)
        return OU_MOD_SHIFT;
    if (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)
        return OU_MOD_ALT;
    if (vk == VK_LWIN || vk == VK_RWIN)
        return OU_MOD_WIN;
    return 0;
}

/* 框里的实时显示: 已经攒到组合就显示组合, 否则显示"按下按键"的提示。 */
static void Esc_HkRefreshText(void)
{
    OU_Hotkey hk;
    char t[48];
    int i;

    if (s_HkNKey <= 0)
    {
        lstrcpynW(s_HkCapTxt, s_KeyPress, 64);
        return;
    }
    hk.disabled = 0;
    hk.mods = s_HkMods;
    hk.nExtra = s_HkNKey - 1;
    if (hk.nExtra > OU_HK_MAXEXTRA) hk.nExtra = OU_HK_MAXEXTRA;
    for (i = 0; i < hk.nExtra; i++) hk.extra[i] = s_HkKeys[i];
    hk.vk = s_HkKeys[s_HkNKey - 1];
    OU_HotkeyText(&hk, t, sizeof(t));
    for (i = 0; i + 1 < 64 && t[i]; i++)
        s_HkCapTxt[i] = (wchar_t)(unsigned char)t[i];
    s_HkCapTxt[i] = 0;
}

static void Esc_HkEnd(HWND h)
{
    if (s_HkCap < 0) return;
    s_HkCap = -1;
    s_HkNKey = 0;
    s_HkMods = 0;
    if (h)
    {
        KillTimer(h, ESC_HK_TIMER);
        Esc_Redraw(h);
    }
}

/* 进捕获: 先把"现在已按住的键"全记进 s_HkPrev —— 点这一行用的左键、以及
 * 切页时残留的修饰键都不该算成用户输入。 */
static void Esc_HkBegin(HWND h, int item)
{
    int vk;

    if (!Esc_HotkeyFind(item)) return;
    if (s_EditOn || s_QOn) Esc_EditCommit(h);
    s_HkCap = item;
    s_HkMods = 0;
    s_HkNKey = 0;
    s_HkT0 = GetTickCount();
    for (vk = 0; vk < 256; vk++)
    {
        s_HkPrev[vk] = (unsigned char)(Esc_HkRaw(vk) ? 1 : 0);
        s_HkUsed[vk] = 0;
    }
    s_HkPrev[VK_LBUTTON] = 1;
    Esc_HkRefreshText();
    if (h)
    {
        SetTimer(h, ESC_HK_TIMER, 16, NULL);
        Esc_Redraw(h);
    }
}

/* 结算: 规范化成 "Ctrl+Z+Middle" 直接写盘 + 让模块回读。这里不排队 ——
 * 热键是模块里的运行期状态, 排队的话这一局不生效。取消菜单时按 Esc_HkBak 回滚。 */
static void Esc_HkCommit(HWND h)
{
    const EscHotkey* e = Esc_HotkeyFind(s_HkCap);
    OU_Hotkey hk;
    char txt[48];
    int i, n;

    if (!e || s_HkNKey <= 0) { Esc_HkEnd(h); return; }
    hk.disabled = 0;
    hk.mods = s_HkMods;
    n = s_HkNKey - 1;
    if (n > OU_HK_MAXEXTRA) n = OU_HK_MAXEXTRA;
    hk.nExtra = n;
    for (i = 0; i < n; i++) hk.extra[i] = s_HkKeys[i];
    hk.vk = s_HkKeys[s_HkNKey - 1];
    OU_HotkeyText(&hk, txt, sizeof(txt));
    Esc_WriteIniRaw(e->section, e->key, txt);
    if (e->reload) e->reload();
    Esc_HkEnd(h);
}

/* 16ms 轮询: 新按下的键按顺序进组合, 全部松开才结算 (所以 Z+Middle 能等齐)。 */
static void Esc_HkPoll(HWND h)
{
    int vk, anyUsed = 0;
    unsigned now = GetTickCount();

    if (s_HkCap < 0) { if (h) KillTimer(h, ESC_HK_TIMER); return; }
    if (now - s_HkT0 > 8000u) { Esc_HkEnd(h); return; }  /* 没动静: 不改键 */

    for (vk = 1; vk < 256; vk++)
    {
        int cur = Esc_HkRaw(vk);

        if (vk == VK_LBUTTON) { s_HkPrev[vk] = (unsigned char)cur; continue; }
        if (cur && !s_HkPrev[vk])
        {
            unsigned bit = Esc_HkModBit(vk);

            if (vk == VK_ESCAPE) { Esc_HkEnd(h); return; }
            s_HkT0 = now;
            if (bit) s_HkMods |= bit;
            else if (s_HkNKey < 8) s_HkKeys[s_HkNKey++] = (unsigned)vk;
            s_HkUsed[vk] = 1;
            Esc_HkRefreshText();
            Esc_Redraw(h);
        }
        else if (!cur && s_HkPrev[vk] && s_HkNKey == 0)
        {
            /* 还没按主键就把修饰键松开了 = 改主意, 这个修饰不算 */
            s_HkMods &= ~Esc_HkModBit(vk);
        }
        s_HkPrev[vk] = (unsigned char)cur;
        if (cur && s_HkUsed[vk]) anyUsed = 1;
    }
    if (s_HkNKey > 0 && !anyUsed) Esc_HkCommit(h);
}

/* 拖动只经数值设置入口提交; 调色比例松手才应用, 避免逐帧重建地图光照。 */
static int Esc_SlideMove(int mx, const EscSlideGeom* g)
{
    int value = Esc_SlideValue(s_SlideDrag, g, mx);
    if (value == s_SlideVal) return 0;
    s_SlideVal = value;
    if (!Esc_SlideDefer(s_SlideDrag))
        Esc_ApplyNum(s_SlidePage, s_SlideDrag, value);
    return 1;
}

/* GUI 页与显示设置页共用 item 编号和控件表, 只按 page 区分编辑态归属。 */
static void Esc_ApplyGui(HWND hDlg, const RECT* right, int page, int index,
                         int mx, int my, int rowTop)
{
    const EscToggle* t = Esc_ToggleFind(index);
    const EscChoice* c = Esc_ChoiceFind(index);
    int rowL = right->left + ESC_PAD;
    int rowW = (right->right - right->left) - ESC_PAD * 2;
    const EscPage* pg = Esc_PageAt(page);
    EscFlow f;
    int i, kind = EG_GROUP, value, pick;

    for (i = 0; i < pg->n; i++)
        if (pg->rows[i].item == index && pg->rows[i].kind != EG_GROUP)
        { kind = pg->rows[i].kind; break; }
    Esc_RowFlow(kind, index, rowL, rowW, pg->labels[index], pg->segs(index), &f);
    pick = Esc_FlowAtomAt(&f, rowTop, mx, my);

    /* 快捷键行: 点一下进捕获态 (页身份只决定归属, 这里只有一种控件)。 */
    if (Esc_HotkeyFind(index)) { Esc_HkBegin(hDlg, index); return; }

    if (Esc_CellsKind(page, index))
    {
        int cell = Esc_CellItemAt(&f, page, index, rowTop, mx, my);
        if (cell < 0) return;
        const EscToggle* toggle = Esc_ToggleFind(cell);
        if (toggle) { Esc_ToggleFlip(toggle); Esc_Redraw(hDlg); }
        else Esc_NumberEdit(hDlg, right, mx, page, cell);
        return;
    }

    /* 纯开关 (s_Toggle) 与分段选择 (s_Choice) 都在表里, 这里只剩特例。
     * 云层行: 三段互斥, 点哪段就是哪档 (关 = 这一层不开)。 */
    if (index == GUI_ITEM_WTH_CLOUD)
    {
        if (pick < 0 || pick == Esc_CloudSeg()) return;
        Weather_SetCloudMode(kCldSegMode[pick]);
        Esc_QueueValue(ENV_SECTION, ENV_KEY_CLOUDMODE, kCldSegMode[pick]);
        Esc_Redraw(hDlg);
        return;
    }

    if (t)
    {
        Esc_ToggleFlip(t);
        Esc_Redraw(hDlg);
        return;
    }
    if (c)
    {
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
    case GUI_ITEM_GAMESPEED:
    {
        const EscNum* n = Esc_NumFind(index);
        EscSlideGeom g = Esc_SlideGeom(index, rowL, rowW, s_GuiLabel[index]);

        if (!g.ends && mx >= Esc_RowCtlX(rowL + rowW))
        {
            Esc_NumberEdit(hDlg, right, mx, page, index, 1);
            return;
        }
        if (g.ends && mx < g.xs[0] + g.ws[0])
            Esc_ApplyNum(page, index, n->min);
        else if (g.ends && mx >= g.xs[1])
            Esc_ApplyNum(page, index, n->max);
        else
        {
            s_SlideDrag = index;
            s_SlidePage = page;
            s_SlideVal = -1;
            Esc_SlideMove(mx, &g);
            if (hDlg) SetCapture(hDlg);
        }
        break;
    }
    case GUI_ITEM_WTH_TRIO:
    {
        /* 点的是哪个天气段切哪个 (s_Toggle 表统一取反+写回; 天气开关不落盘) */
        const EscToggle* wt;
        if (pick < 0) return;
        wt = Esc_ToggleFind(GUI_ITEM_WTH_RAIN + pick);
        if (wt) Esc_ToggleFlip(wt);
        break;
    }
    case GUI_ITEM_UP_BTNS:
    {
        /* 段 0 = 全关; 段 1..3 = 折叠 / WASD / 收藏 各自取反。三个开关都在
         * [UnitPanel] 里, 落盘键 BtnFold / BtnWasd / BtnFav。 */
        static const char* const kKey[3] = { "BtnFold", "BtnWasd", "BtnFav" };
        if (pick < 0) return;
        Esc_UpBtnApply(pick);
        if (pick <= 0)
        {
            int k;
            for (k = 0; k < 3; k++) Esc_QueueValue("UnitPanel", kKey[k], 0);
        }
        else Esc_QueueValue("UnitPanel", kKey[pick - 1], UP_BtnShown(pick - 1));
        Esc_Redraw(hDlg);
        return;
    }
    case GUI_ITEM_ROWS:
        Esc_NumberEdit(hDlg, right, mx, page, index, 1);
        return;
    case GUI_ITEM_SKIN:
    {
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
    default:
        return;
    }
    Esc_Redraw(hDlg);
}

/* 行 item 转为鼠标下的数值 item; 点击与滚轮不能各自猜字体/多格子项。 */
static int Esc_NumberItem(const RECT* b, int page, int item, int rowTop,
                          int mx, int my)
{
    int rowL = b->left + ESC_PAD;
    int rowW = b->right - b->left - ESC_PAD * 2;
    int kind = Esc_CellsKind(page, item);
    if (kind)
    {
        EscFlow f;
        const EscPage* pg = Esc_PageAt(page);
        Esc_RowFlow(kind, item, rowL, rowW, pg->labels[item], pg->segs(item), &f);
        int cell = Esc_CellItemAt(&f, page, item, rowTop, mx, my);
        return cell >= 0 && (page == EDIT_PAGE_TEXT || !Esc_ToggleFind(cell)) ? cell : -1;
    }
    if (page != EDIT_PAGE_TEXT) return Esc_NumFind(item) ? item : -1;
    const EscTextFont* ft = Esc_TextFontFind(item);
    if (ft)
    {
        int fx, fw, cx, cw, px, pw, bx, bw;
        Esc_FontGeom(rowL, rowW, s_TextLabel[item],
                     &fx, &fw, &cx, &cw, &px, &pw, &bx, &bw);
        return mx >= px && mx < bx ? ft->px : -1;
    }
    return Esc_TextNumFind(item) ? item : -1;
}

/* 文本页的点击处理: 开关/分段/数值/多格/字体/预设文本, 靠各张表分派。 */
static void Esc_TextApply(HWND hDlg, const RECT* right, int index, int mx,
                          int my, int rowTop)
{
    const EscTextToggle* t = Esc_TToggleFind(index);
    const EscTextChoice* c = Esc_TChoiceFind(index);
    const EscTextNum* num = Esc_TextNumFind(index);
    const EscTextCells* cells = Esc_TextCellsFind(index);
    const EscTextFont* ft = Esc_TextFontFind(index);
    int value, pick;

    if (ft)
    {
        /* 字体行: 点框=编辑字名 (光标落点), 点数字=编辑字号, 点按钮=切粗体。 */
        int rowL = right->left + ESC_PAD;
        int rowW = (right->right - right->left) - ESC_PAD * 2;
        int fx, fw, cx, cw, pix, piw, bx, bw;

        Esc_FontGeom(rowL, rowW, s_TextLabel[index], &fx, &fw, &cx, &cw,
                     &pix, &piw, &bx, &bw);
        if (mx < pix)
        {
            wchar_t face[ESC_FACE_CAP];
            int px;

            Esc_FaceGet(ft->target, face, ESC_FACE_CAP);
            px = mx - (fx + ESC_TEXT_INSET);
            if (px < 0) px = 0;
            Esc_FaceBegin(hDlg, ft->target,
                          Esc_QuickCurAt(face, lstrlenW(face), px));
        }
        else if (mx < bx)
        {
            Esc_NumberEdit(hDlg, right, mx, EDIT_PAGE_TEXT, ft->px);
        }
        else
        {
            int v = Esc_OptGet(ft->mod, ft->boldOpt) ? 0 : 1;

            Esc_OptSet(ft->mod, ft->boldOpt, v);
            Esc_QueueValue(ft->section, ft->boldKey, v);
            Esc_Redraw(hDlg);
        }
        return;
    }
    if (index == TX_CHAT_QUICK)
    {   /* 预设文本块: [-] 删该条, [+] 在该条后插一条, 点框=编辑该条。
         * 位置与绘制同一份几何 (Esc_QuickGeom)。 */
        int boxX, boxW, minusX, plusX, i;
        int rowL = right->left + ESC_PAD;
        int rowW = (right->right - right->left) - ESC_PAD * 2;

        Esc_QuickEnsure();
        Esc_QuickGeom(rowL, rowW, &boxX, &boxW, &minusX, &plusX);
        for (i = 0; i < s_QCount; i++)
        {
            int ry = rowTop + (i + 1) * ESC_ROW_H;

            if (EscIn(plusX, ry, ESC_STEP_BTN_W, ESC_ROW_H, mx, my))
            {
                Esc_QuickAdd(hDlg, i);
                return;
            }
            if (EscIn(minusX, ry, ESC_STEP_BTN_W, ESC_ROW_H, mx, my))
            {
                Esc_QuickDel(hDlg, i);
                return;
            }
            if (EscIn(boxX, ry, boxW, ESC_ROW_H, mx, my))
            {
                /* 点框 = 编辑该条, 且**光标落在点的地方** (与数字框一致):
                 * 先把点击 x 按当前显示起点换成文本坐标, 再找最近的字符缝。 */
                int innerW = boxW - ESC_TEXT_INSET * 2;
                const wchar_t* t = s_QItem[i];
                int start = (s_QOn && s_QEdit == i)
                                ? Esc_QuickView(t, s_QSt.cur, innerW) : 0;
                /* 显示起点在整串里的像素偏移 = 整串宽 − 从 start 起的剩余宽
                 * (不能直接加剩余宽: start=0 时那样会平白多出一整串宽)。 */
                int px = (mx - (boxX + ESC_TEXT_INSET)) +
                         (Esc_TextWidth(t, ESC_BODY_PX, ESC_BODY_SLOT) -
                          Esc_TextWidth(t + start, ESC_BODY_PX, ESC_BODY_SLOT));

                Esc_QuickBegin(hDlg, i, Esc_QuickCurAt(t, lstrlenW(t), px));
                return;
            }
        }
        return;
    }
    if (cells)
    {
        int cell = Esc_NumberItem(right, EDIT_PAGE_TEXT, index, rowTop, mx, my);
        if (cell >= 0) Esc_NumberEdit(hDlg, right, mx, EDIT_PAGE_TEXT, cell);
        return;
    }

    if (t)
    {
        value = Esc_TToggleVal(t) ? 0 : 1;
        Esc_TTogglePut(t, value);
        Esc_QueueValue(Esc_OptSection(t->mod), t->key, value);
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
        if (c->set) c->set(value);
        else Esc_OptSet(c->mod, c->opt, value);
        if (index == TX_VT_POWER)
        {   /* 一个选项动两个键 */
            Esc_QueueValue(VT_SECTION, "PowerBottom",
                           PowerDisplay_GetOption(PWO_ENABLE));
            Esc_QueueValue(VT_SECTION, "PowerMode",
                           PowerDisplay_GetOption(PWO_MODE));
        }
        else if (c->key)
            Esc_QueueValue(Esc_OptSection(c->mod), c->key, value);
    }
    else if (num)
    {
        Esc_NumberEdit(hDlg, right, mx, EDIT_PAGE_TEXT, index, 1);
        return;
    }
    else return;
    Esc_Redraw(hDlg);
}

static void Esc_EditEnd(HWND hDlg)
{
    if (!s_EditOn && !s_QOn) return;
    if (s_EditHost) KillTimer(s_EditHost, ESC_EDIT_TIMER);
    s_EditOn = 0;
    s_QOn = 0;      /* 预设文本是就地编辑, 内容已同步, 这里只收会话 */
    s_QEdit = -1;
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

    if (s_QOn) { Esc_QuickCommit(hDlg); return; }
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

    if (s_EditPage == EDIT_PAGE_TEXT || Esc_LightItemIdx(s_EditItem) < 0 || s_EditSt.len < cap)
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

    if (s_QOn) { Esc_QuickPoll(hDlg); return; }
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
    /* 离开哪页就把哪页的滚动位置复位 (每页各一个)。 */
    *Esc_PageAt(s_Sel)->scroll = 0;
    s_Sel = index;
    Esc_Redraw(hDlg);
}

/* 滚轮悬停在数值控件上时微调数值 (步长按各行自己的口径), 不滚页。
 * 返回 1 = 已处理 (吞掉滚轮)。 */
static int Esc_WheelValue(HWND h, const RECT* b, int mx, int my, int delta)
{
    int index = -1, rowTop = 0, v;
    int page = s_Sel;
    const EscPage* pg = Esc_PageAt(page);

    if (s_EditOn) return 0;
    if (!Esc_HitPage(b, pg, mx, my, &index, &rowTop)) return 0;
    if (Esc_MpLocked(page, index)) return 0;   /* 联机只读项: 滚轮也不改值 */
    index = Esc_NumberItem(b, page, index, rowTop, mx, my);
    if (index < 0) return 0;
    EscNumber n = Esc_Number(page, index);
    v = n.value + (delta > 0 ? n.step : -n.step);
    if (v < n.min) v = n.min;
    if (v > n.max) v = n.max;
    if (v != n.value) Esc_ApplyNum(page, index, v);
    Esc_Redraw(h);
    return 1;
}

/* 滚轮: 滚当前页 (设置页也可能长高, 所以每页都滚)。
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
    pg = Esc_PageAt(s_Sel);
    old = *pg->scroll;
    *pg->scroll = Esc_PageSnap(&b, pg, *pg->scroll - (delta / 120) * ESC_ROW_H * 3);
    Esc_PageClampScroll(&b, pg);
    if (*pg->scroll != old) Esc_Redraw(h);
    return 1;
}

static int Esc_EditClick(HWND hDlg, const RECT* right, int mx, int my)
{
    int index = -1, rowTop = 0;
    const EscPage* pg = Esc_PageAt(s_EditPage);

    if (!s_EditOn || !Esc_StepMid(right, mx, s_EditPage, s_EditItem)) return 0;
    if (!Esc_HitPage(right, pg, mx, my, &index, &rowTop)) return 0;
    if (Esc_NumberItem(right, s_EditPage, index, rowTop, mx, my) != s_EditItem)
        return 0;

    s_EditSt.cur = Esc_EditCurAt(right, mx, s_EditBuf, s_EditSt.len, s_EditPage, s_EditItem);
    Esc_Redraw(hDlg);
    return 1;
}

/* 拖动条收尾 (松开鼠标 / 菜单隐藏两处共用): 调色比例是松手才提交的, 这里补
 * 提交一次并红画; 其余行拖动中已实时生效。统一复位拖动状态并释放捕获。 */
static void Esc_SlideDrop(HWND h)
{
    if (Esc_SlideDefer(s_SlideDrag))
    {
        Esc_ApplyNum(s_SlidePage, s_SlideDrag, s_SlideVal);
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
        /* 捕获中点别处 = 放弃这次改键 (点同一行会重新进捕获) */
        if (s_HkCap >= 0) Esc_HkEnd(h);
        if (!Esc_Layout(h, &a, &b)) { if (s_EditOn || s_QOn) Esc_EditCommit(h); break; }
        if (Esc_CancelHit(&b, mx, my))
        {
            s_CancelDown = 1;
            SetCapture(h);
            Esc_Redraw(h);
            return 0;
        }
        if (Esc_EditClick(h, &b, mx, my)) return 0;
        if (s_EditOn || s_QOn) Esc_EditCommit(h);
        index = Esc_LinkHit(&a, mx, my);
        if (index >= 0) { Esc_OpenLink(index); return 0; }
        index = Esc_LangHit(&a, mx, my);
        if (index >= 0) { Esc_ApplyLang(h, index); return 0; }
        if (Esc_HitLeft(&a, mx, my, &index)) { Esc_Select(h, index); return 0; }
        if (Esc_HitPage(&b, Esc_PageAt(s_Sel), mx, my, &index, &rowY))
        {
            /* 联机只读项: 吞掉这次点击 (不往下走, 也不能漏给引擎的隐藏控件)。 */
            if (Esc_MpLocked(s_Sel, index)) return 0;
            if (s_Sel == EDIT_PAGE_TEXT) Esc_TextApply(h, &b, index, mx, my, rowY);
            else Esc_ApplyGui(h, &b, s_Sel, index, mx, my, rowY);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (s_SlideDrag < 0 || s_Sel == EDIT_PAGE_TEXT)
        {
            /* 不在拖动条时: 光标动了就刷悬停高亮 (行高亮 + 底部链接) */
            Esc_FootHover(h, mx, my);
            Esc_HoverNudge(h, mx, my);
            break;
        }
        if (Esc_Layout(h, &a, &b))
        {
            int rowL = b.left + ESC_PAD;
            int rowW = (b.right - b.left) - ESC_PAD * 2;
            EscSlideGeom g = Esc_SlideGeom(s_SlideDrag, rowL, rowW,
                                           s_GuiLabel[s_SlideDrag]);
            if (Esc_SlideMove(mx, &g)) Esc_Redraw(h);
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
        if (wp == ESC_HK_TIMER) { Esc_HkPoll(h); return 0; }
        break;
    case WM_CHAR:
    case WM_IME_CHAR:
        /* 预设文本框编辑中: 任意字符 (含输入法上屏的字) 进草稿。
         * 回车/退格这类控制字吞掉 —— 它们由按键轮询统一处理,
         * 漏给对话框会顺带触发菜单快捷行为。 */
        if (s_QOn)
        {
            Esc_QuickChar((wchar_t)(wp & 0xFFFFu));
            Esc_Redraw(h);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (s_EditOn || s_QOn) Esc_EditCommit(h);
        break;
    case WM_MOUSEWHEEL:
        if (Esc_PageWheel(h, wp, lp)) return 0;
        break;
    case WM_SHOWWINDOW:
        /* 隐藏 ESC 菜单 = 离开设置页, 统一落盘。 */
        if (wp) break;
        s_CancelDown = 0;
        Esc_HkEnd(h);                            /* 捕获中的那次改键作废 */
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
            Esc_HkEnd(NULL);
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

    /* 联机 (3/4) 也照常接管首页, 只把会分叉的项置灰 (Esc_MpLocked)。 */
    s_Mp = OU_IsMP();

    /* 所有 ESC 对话框 (首页 + 各级子页) 都要做两件事:
     *   1) 告诉限帧模块"菜单开着" —— 只有这时才限速, 菜单关掉立刻放行
     *   2) 子类化 —— 面板自绘/编辑框轮询
     * 面板自绘只对首页 (s_Host) 做。 */
    FrameCap_SetActiveDlg(hDlg);
    Esc_Subclass(hDlg);

    /* 只有首页带 1313 游戏控制按钮, 3003 子页没有。 */
    if (!GetDlgItem(hDlg, ESC_ID_GAMECONTROLS)) return;

    if (s_Host != hDlg)
    {
        s_CancelDown = 0;
        s_LinkHot = -1;
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
