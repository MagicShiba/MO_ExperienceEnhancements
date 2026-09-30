/* SWSBar.cpp - 超武侧栏 (Phobos SuperWeaponSidebar 移植简化版)。
 *
 * 原理、设计取舍、踩过的坑与地址推导见 notes/超武侧栏.md。这里只留
 * "读/改这份代码时必须知道"的约束。
 *
 * 不建 Gadget、不碰模拟 (Ares 不动):
 *   - 数据: 只读扫 SuperClass::Array (Owner==本家 && IsPresent), 纯本地过滤
 *     +排序, 不写任何引擎状态 —— 联机安全。
 *   - 分组: 右侧 4 列 (盟/苏/厄/焚风), 空列隐藏; 归属查内建表 (96 个, 由
 *     rulesmo.ini 授予建筑 Owner/Prerequisite 链离线推导), 查不到落玩家阵营。
 *   - 排序/隐藏: `[SWSidebar]` 单节, 以超武 ID 为键: 负数=隐藏, 0/缺键=
 *     默认 (优先级 1000), 正数=优先级 (越小越靠前, 同值按 ArrayIndex)。
 *   - 图标: 走 PCX (rulesmo.ini 的 `SidebarPCX=`) 逐像素贴图。**不用 SHP** ——
 *     `SidebarImage` 指向引擎 XXICON.SHP 占位引用, 画出来是一堆紫球。
 *   - 绘制: 图标盒 60x48 + 阵营色边框 + 充能百分比 + 悬停名提示, 整体
 *     `[SWSidebar] Alpha` 可调; 画进**自有离屏缓存**, 15 个游戏逻辑帧重建
 *     一次 (交互时当帧重建), 每渲染帧只做一次带 alpha 的合成。
 *   - 交互: ToggleKey 切换显隐 (隐藏即不占空间: 不画+不吞点击); 左键图标=
 *     选中打靶 (只写 CurrentSWTypeIndex, 非模拟), 右键栏内=取消; 点击吞没
 *     经 0x692419 门防误下单。
 *   - 防御支援页去重: 侧栏有的超武不在引擎自带的防御页 (页签 1) 重复出现,
 *     显隐箭头左边的"禁入牌"按钮切换。见"隐藏防御支援页里的重复超武"。
 *
 * 地址 (只读为主, 首帧探针日志自证)。**本机 Ares 3.0 的结构体字段比 YRpp
 * 头文件大 0x0C**: 头文件 Super/SWType 的字段值一律 +0x0C, 见下面各宏注释。
 * 头文件地址不能直接抄, 详见笔记的可信度表。
 *   SuperClass::Array 0xA83CB8 / HouseClass::Array 0xA80228 (含虚表);
 *   SuperWeaponTypeClass::Array 0xA8E330;
 *   CurrentPlayer *(0xA83D4C); CurrentFrame *(0xA8ED84);
 *   Scenario PlayerSideIndex *(*(0xA8B230)+0x34B8);
 *   Display 对象 0x87F7E8, CurrentSWTypeIndex +0x11B8 (=0x8809A0)。
 * 任一校验失败即 fail-closed (不画/不点, 记日志), 绝不花屏误触。
 *
 * 构建见 构建.bat (32 位 mingw, 无 CRT, 与 MOExperience 同一 DLL)。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "VecText.h"
#include "SWSBar.h"

#define SWS_SECTION "SWSidebar"

#define ADDR_SUPER_ARR   0xA83CB8u
#define ADDR_SWTYPE_ARR  0xA8E330u
#define ADDR_HOUSE_ARR   0xA80228u
#define ADDR_CURPLAYER   0xA83D4Cu
/* 容器布局 (YRpp ArrayClasses.h 实证, 带虚表):
 *   +0 vtable / +4 Items / +8 Capacity / +12 IsInit+IsAlloc /
 *   +16 Count / +20 CapInc。
 * 传的是**对象基址**, 不是字段地址。 */
#define ARR_ITEMS(base) (*(void***)((base) + 4u))
#define ARR_COUNT(base) (*(int*)((base) + 16u))
#define ADDR_FRAME       0xA8ED84u
#define ADDR_SCENARIO    0xA8B230u
#define OFF_SCEN_SIDE    0x34B8u
#define ADDR_CURSW       0x8809A0u   /* Display(0x87F7E8)+0x11B8 */

/* Super 字段 (括号里是 YRpp 头文件值, 本机实测 = 头文件 + 0x0C) */
#define OFF_SUP_TYPE     0x28u   /* (0x1C) T405 全中 */
#define OFF_SUP_OWNER    0x2Cu   /* (0x20) H405 全中 */
#define OFF_SUP_RTSTART  0x30u   /* (0x24) RechargeTimer.Start */
#define OFF_SUP_RTLEFT   0x38u   /* (0x2C) RechargeTimer.Left  */
#define OFF_SUP_PRESENT  0x6Du   /* (0x61) IsPresent  */
#define OFF_SUP_READY    0x6Fu   /* (0x63) IsReady    */
#define OFF_SUP_SUSPEND  0x70u   /* (0x64) IsSuspended */

/* SuperWeaponType 字段 (同样 +0x0C 漂移; ID 与 Super 同源, 计数已验证) */
#define OFF_SWT_ID       0x24u   /* (0x18) */
#define OFF_SWT_UINAME   0x60u   /* (0x54) */
#define OFF_SWT_ARRIDX   0x98u   /* (0x8C) */
#define OFF_SWT_RECH     0xB0u   /* (0xA4) RechargeTime */

#define VT_LOCK            0x5C
#define VT_UNLOCK          0x60

#define ADDR_CLICKGATE_SKIP 0x006925FCu

#define SWS_CAP 160
#define SWS_INI_CAP 160

/* ---------------- 配置 ----------------
 * 铁律: 每帧路径上**不允许**出现 GetPrivateProfileIntA —— 那等于每帧一次 ini
 * 文件 I/O, 而且落在 perf 计时之外, 帧率掉了日志还看不出来。全部读一次。 */

static int s_Init = 0;
static int s_Enable = 1;
/* 侧栏**唯一**的透明度旋钮 0~100。作用于底框/边框/滚动条/按钮/文字/图标。
 * 例外: 悬停提示框、悬停图标 (见"绘制: 离屏缓存")。 */
static int s_Alpha = 100;
/* 图标盒固定 60x48 (Phobos 原版 cameo 尺寸)。**不要**做成配置项: PCX 是固定
 * 尺寸源资源, 缩放后目标矩形只按屏幕边界裁剪、不按图标盒裁剪, 非默认尺寸会
 * 溢出格子与底框错位。 */
#define SWS_ICON_W 60
#define SWS_ICON_H 48
/* 整块底框自身的不透明度。图标不吃逐元素不透明度, 所以"整块画一次黑底"与
 * "逐列各画一次"效果一样, 不需要第二个旋钮。 */
#define SWS_BACKDROP_OP 30

static int s_Gap = 2, s_ColGap = 6;
static int s_RightMargin = 2;
/* 纵向定位: 一个键同时表达"按屏幕比例"和"按像素"。
 *   0 ~ 1 = 比例 (x100000 定点), 0.5 = 屏幕高 50% 处对齐;
 *   > 1    = 像素 Y 坐标 (80 = 距顶 80px)。
 * 范围取 0~1 而不是 0~100, 是为了和 ini 里其它归一到 0~1 的观感一致。 */
static int s_TopIsPct = 0;
static int s_TopMargin = 50000;  /* 比例时 x100000 (0.5 -> 50000); 像素时是 px */
static int s_FontPx = 12;        /* 只管充能百分比; 悬停名走 [VecText] */
static OU_Hotkey s_ToggleHk;
static int s_Visible = 1;
static int s_PrevTog = 0, s_PrevLB = 0, s_PrevRB = 0;

/* 待上膛的超武, **等左键松开才写**。
 *
 * 引擎放置超武的判据是 0x692B06 读 Display+0x11B8 (CurrentSWTypeIndex):
 * 一旦非 -1 且**当帧**按键按着, 就直接在鼠标处释放 —— 延一帧写躲不掉, 那一帧
 * 按键还按着。所以"按下只记录, 松开才写": 整段按住期由 s_HoldSkip 吞掉引擎的
 * 地图点击 (等价于 Gadget 消费掉这次完整点击), 真正写 CurrentSWTypeIndex 时
 * 按键已松开, 引擎那个条件不可能成立。
 * Phobos 不需要这套: 它的 SWButtonClass 是真 Gadget, GadgetClass::Action 消费
 * 点击时会顺手清掉 MouseClass 的按键态, ProcessClickCoords 看不到"按住"。 */
static int s_PendSel = -2;   /* -2 none, -1 cancel, >=0 SW ArrayIndex */
static int s_SelDown = 0;    /* 选中的那一下左键是否还按着 (等松开才上膛) */
static int s_ArmIdx = -1;    /* s_SelDown 期间记住的 swIdx */
/* 物理左键按住期间一直吞掉引擎的地图点击。关键: 引擎判断的是"左键**按住**"
 * 而不是边沿, 所以只要"已选中 + 按住"同时成立就会当场释放。按下即吞, 一直
 * 吞到松开。 */
static int s_HoldSkip = 0;
/* 吞没门/滚轮用的"栏区已知"标志 = 缓存里有一帧可合成的画面。只由重建路径置位,
 * 无条目/收起时清零 —— 两者都对应"屏上确实没有侧栏可点"。 */
static int s_BoundsKnown = 0;

/* 即时超武: 走 SpecialPlace 事件 (定义在下方)。 */
static int SwsPushSpecialPlace(int swIdx);
static int s_ShowDrain = 0;   /* [SWSidebar] DrainSupers: 1=显示火风暴类 (默认 0=过滤掉) */
static int s_ForceScan = 0;   /* 点击时置 1: 强制立刻重扫, 不等 15 帧 */
static int s_ScrollStep = 1;  /* [SWSidebar] ScrollStep */

/* ---------------- 隐藏"防御支援页"里的重复超武 ----------------
 * 目标态 s_DedupHide (按钮切换 / [SWSidebar] DedupDefensePage 初值) 与已落到
 * 防御页的实际态 s_DedupDone 分开存: 两者不同、或 s_DedupDirty 被置 1 时,
 * 轮询里补做一次"删"或"补"。 */
static int s_DedupHide = 1;     /* 1 = 隐藏重复, 0 = 原样 */
static int s_DedupDone = 0;     /* 已经落到防御页的实际状态 */
static int s_DedupDirty = 0;    /* 需要 (重新) 应用一次 */
static int s_HidX = 0, s_HidY = 0;   /* 隐藏重复按钮几何 (绘制期发布) */
static int s_HidValid = 0;      /* 该按钮这一帧是否真画出来了 (重建早退时要清) */
static unsigned char* s_TabCam = NULL;  /* Ares RulesExt::TabCameos[4] 基址 */

/* 引擎事件队列 (OutList): SwsPushSpecialPlace 追加 SpecialPlace 事件用。 */
#define SWS_ADDR_OUT_COUNT  0xA802C8u
#define SWS_ADDR_OUT_NEXT   0xA802D0u
#define SWS_ADDR_OUT_BUF    0xA802D4u
#define SWS_ADDR_OUT_PAR    0xA83A54u
#define SWS_EVT_SIZE        111u
#define SWS_EVT_CAP         0x80u
#define SWS_EVT_SLOTS_N     128u   /* 环形容量; 槽位掩码 = SWS_EVT_SLOTS_N-1 */


typedef struct { char id[32]; int prio; } SwsIniEnt;
static SwsIniEnt s_Ini[SWS_INI_CAP];
static int s_IniN = 0;

/* ---------------- 内建阵营表 (rulesmo.ini 离线推导, 见 notes/超武侧栏.md) ----------------
 * 0=盟军 1=苏联 2=厄普西隆 3=焚风。查不到的 ID 落玩家阵营列。 */

typedef struct { const char* id; int side; } SwsSideEnt;
static const SwsSideEnt kSides[] = {
    { "AmericanParaDropSpecial", 0 }, { "BackwarpSpecial", 0 },
    { "BlackWidowAlphaSpecial", 0 }, { "BlackWidowSpecial", 0 },
    { "BloodhoundsSpecial", 0 }, { "ChronoboostSpecial", 0 },
    { "ChronoliftSpecial", 0 }, { "ChronoSphereSpecial", 0 },
    { "CryomineSpawn", 0 }, { "CryoshotSpecial", 0 },
    { "CryospearSpecial", 0 }, { "ForceShieldSpecial", 0 },
    { "GlacialScreenSpecial", 0 }, { "HunterSeekerSpecial", 0 },
    { "KingsnakesSpecial", 0 }, { "LightningRodSpecial", 0 },
    { "LightningStormSpecial", 0 }, { "MercurySpecial", 0 },
    { "PaladinAidSpecial", 0 }, { "SonarPulseSpecial", 0 },
    { "SpySatSpecial", 0 }, { "TargetPainterSpecial", 0 },
    { "WarpMinersSpecial", 0 }, { "ZephyrBeaconSpecial", 0 },
    { "CraneSpecial", 1 }, { "DisruptorSpecial", 1 },
    { "DrakuvSpecial", 1 }, { "EliteReservesSpecial", 1 },
    { "EMPMineSpawn", 1 }, { "EMPulsSpecial", 1 },
    { "FlameTowerSpecial", 1 }, { "GearChangeSpecial", 1 },
    { "InstantShelterSpecial", 1 }, { "IronCurtainSpecial", 1 },
    { "IronGuardSpecial", 1 }, { "IrradiateBetaSpecial", 1 },
    { "IrradiateSpecial", 1 }, { "MotorAmbushSpecial", 1 },
    { "NavalMineSpecial", 1 }, { "NuclearPathSpecial", 1 },
    { "NukeSpecial", 1 }, { "OverchargeSpecial", 1 },
    { "PackAttackSpecial", 1 }, { "RadAttackSpecial", 1 },
    { "RepairDroneSpecial", 1 }, { "RepairDronesSpecial", 1 },
    { "SmokebombsSpecial", 1 }, { "SpyPlaneSpecial", 1 },
    { "TankDropSpecial", 1 }, { "TerrorDropSpecial", 1 },
    { "WallbusterSpecial", 1 },
    { "EliteReservesMessage", 2 }, { "FakeChemplugSpawn", 2 },
    { "FakeDominatorSpecial", 2 }, { "GenomineSpawn", 2 },
    { "HijackersSpecial", 2 }, { "IllusionSpecial", 2 },
    { "KineticBarrierSpecial", 2 }, { "LibraCloneSpecial", 2 },
    { "MagnetShiftSpecial", 2 }, { "MutationSpecial", 2 },
    { "PsychicDominatorSpecial", 2 }, { "PsychicFlashSpecial", 2 },
    { "QuickFortSpecial", 2 }, { "RageInductorSpecial", 2 },
    { "RavenSpecial", 2 }, { "RegenDrugsSpecial", 2 },
    { "RisenMonolithSpecial", 2 }, { "RuinerSpecial", 2 },
    { "TickTrapSpecial", 2 }, { "ToxicStrikeSpecial", 2 },
    { "VisionSpecial", 2 }, { "WonderDrugsSpecial", 2 },
    { "BlackoutMissileSpecial", 3 }, { "BlasticadeSpecial", 3 },
    { "BoidBlitzSpecial", 3 }, { "ChaosTouchSpecial", 3 },
    { "ConfusionGridSpawn", 3 }, { "DecoySquadronSpecial", 3 },
    { "DecoyTeamSpecial", 3 }, { "DevourerSpecial", 3 },
    { "GoldenWindSpecial", 3 }, { "GreatTempestSpecial", 3 },
    { "HarbingerSpecial", 3 }, { "KnightfallSpawn", 3 },
    { "KnightfallSpecial", 3 }, { "MADMineSpecial", 3 },
    { "MegaarenaSpecial", 3 }, { "NanofiberSyncSpecial", 3 },
    { "NanochargeSpecial", 3 }, { "ReconSortieSpecial", 3 },
    { "ReproCashFix", 3 }, { "SignalJammerSpecial", 3 },
    { "SpinbladeSpecial", 3 }, { "StasisGridSpawn", 3 },
    { "SweeperDropSpecial", 3 },
};
#define K_SIDES_N (sizeof(kSides) / sizeof(kSides[0]))

/* 阵营边框色: 0=盟军 1=苏联 2=厄普西隆 3=焚风(墨绿) */
static const unsigned char kSideCol[4][3] = {
    { 70, 150, 255 }, { 255, 70, 70 },
    { 170, 80, 255 }, { 0, 110, 60 },
};

/* ---------------- 扫描结果 ---------------- */

typedef struct {
    void* type;
    int swIdx;
    int side;
    int prio;
    int ready;
    int susp;
    int remain;   /* 剩余帧 */
    int total;    /* 总帧 (<=0=未知) */
    int x, y;
    const char* pcxFile;  /* SidebarPCX= 文件名, 指向我们自己的表 */
    int instant;          /* 1 = 点击即生效 (Action=None 或 SW.UseAITargeting) */
    wchar_t name[24];     /* 悬停提示用; 每类型填一次 */
} SwsItem;

static SwsItem s_Items[SWS_CAP];
static int s_ItemN = 0;
/* 排序结果缓存。排序键 (阵营, ini 优先级, ArrayIndex) 全来自 s_Items, 而
 * s_Items 只有 SwsScan 会改 —— 顺序**与帧无关**, 所以只在缓存重建时排一次
 * (见 SwsRebuild), SwsScan 结束时把 s_OrderN 置 -1 作废。
 *
 * ⚠ s_Items (原序 = SuperClass 数组序) 与 s_Order (排序后) 是**两套下标**。
 *   遍历排序结果一律写 s_Items[s_Order[i]], 拿排序下标去直接索引 s_Items
 *   会取到另一个超武 (见 SwsHitItem)。 */
static int s_Order[SWS_CAP];
static int s_OrderN = -1;           /* -1 = 还没排过 */
static int s_OrderColCount[4];

/* 侧栏主题色 (淡蓝): 箭头、箭头边框、文字描边统一用它, 方便一处改色。 */
#define SWS_ACCENT_R 0xA5
#define SWS_ACCENT_G 0xD3
#define SWS_ACCENT_B 0xFF
static int s_BoundX = 0, s_BoundY = 0, s_BoundW = 0, s_BoundH = 0;
/* 面板级滚动偏移 (所有列一起上下滚) + 显示行数上限 (超出用滚动条)。
 * 逐列偏移在列间距为 0 时没法各画各的滚动条 (4px 塞不进 0 间距里), 而
 * 战役框那套也是整框一个偏移, 所以统一成一个。 */
static int s_Scroll = 0;
static int s_MaxRows = 10;         /* [SWSidebar] MaxRows, 0 = 不限 */
static int s_BtnX = 0, s_BtnY = 0, s_BtnW = 0, s_BtnH = 0;  /* 显隐按钮 */
/* 展开态那一帧记下的按钮坐标: 收起时原样沿用, 折叠/展开不许跳位。
 * 见 SwsCollapsedBtnGeom。 -1 = 还没展开过。 */
static int s_BtnXKeep = -1, s_BtnYKeep = -1;
/* SWS_Draw 算好的布局, 供点击/滚动判定使用 (现在由缓存重建路径发布) */
static int s_YStart = 0, s_RowH = 50, s_VisRows = 0;
static int s_LastX = 0, s_MaxSk = 0;

static int s_ScanLogged = 0;

/* ---------------- 小工具 (无 CRT) ---------------- */

static int SwsStrEq(const char* a, const char* b)
{
    int i = 0;
    while (a[i] && b[i])
    {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

/* copy an ID string (bounded) */
static void SwsCopyId(char* dst, const char* src)
{
    int i = 0;
    while (src[i] && i < 31) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* an ID must look like a rules ID: alnum/underscore, non-empty */
static int SwsIdOk(const char* s)
{
    int i = 0;
    if (!s) return 0;
    for (i = 0; i < 31; i++)
    {
        char c = s[i];
        if (!c) break;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return i > 0;
}

static int SwsPlayerSide(void)
{
    void* scen;
    int side;
    if (!IsPlausiblePtr((void*)ADDR_SCENARIO)) return 0;
    scen = *(void**)ADDR_SCENARIO;
    if (!scen || !IsPlausiblePtr(scen)) return 0;
    if (!IsPlausiblePtr((char*)scen + OFF_SCEN_SIDE + 4)) return 0;
    side = *(int*)((char*)scen + OFF_SCEN_SIDE);
    if (side < 0 || side > 3) return 0;
    return side;
}

static int SwsSideOf(const char* id)
{
    unsigned i;
    for (i = 0; i < K_SIDES_N; i++)
        if (SwsStrEq(kSides[i].id, id)) return kSides[i].side;
    return -1;
}

static int SwsPrioOf(const char* id, int* hide)
{
    int i;
    *hide = 0;
    for (i = 0; i < s_IniN; i++)
    {
        if (SwsStrEq(s_Ini[i].id, id))
        {
            if (s_Ini[i].prio < 0) *hide = 1;
            return s_Ini[i].prio;
        }
    }
    return 1000;
}

typedef void* (__fastcall *SwsLockFn)(void*, void*, int, int);
typedef int (__fastcall *SwsUnlockFn)(void*, void*);

/* wide-string copy (bounded) */
static void SwsCopyName(wchar_t* dst, const wchar_t* src, int cap)
{
    int i = 0;
    if (!dst || cap <= 0) return;
    if (src) { while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; } }
    dst[i] = 0;
}

/* 日志样板: "前缀 + 十进制" / "前缀 + 字符串"。省掉每处都开 buf/q 局部变量。 */
static void LogI(const char* tag, int v)
{
    char b[80], n[16];
    unsigned q;
    UtoA((unsigned)v, n);
    q = AppendCStr(b, 0, tag, sizeof(b));
    AppendCStr(b, q, n, sizeof(b));
    LogLine(b);
}
static void LogS(const char* tag, const char* s)
{
    char b[96];
    unsigned q = AppendCStr(b, 0, tag, sizeof(b));
    AppendCStr(b, q, s, sizeof(b));
    LogLine(b);
}

/* ---------------- 配置加载 (一次, 重启生效) ---------------- */

/* 键长 (volatile 逐字节: 防编译器把循环认成 strlen 习语,
 * 无 CRT 链接会缺符号, 见构建记录)。 */
static unsigned SwsKeyLen(const char* k)
{
    unsigned L = 0;
    while (*(volatile const char*)(k + L)) L++;
    return L;
}

static void SwsLoadIniList(void)
{
    /* 列出节内全部键: 已知全局键跳过, 其余按超武 ID 收表。 */
    static char keys[8192];
    DWORD n;
    unsigned pos = 0;
    s_IniN = 0;
    n = GetPrivateProfileStringA(SWS_SECTION, NULL, "", keys, sizeof(keys),
                                 g_IniPath);
    if (n <= 1) return;
    while (pos < n && s_IniN < SWS_INI_CAP)
    {
        const char* k = keys + pos;
        unsigned L = SwsKeyLen(k);
        int isGlobal = 0;
        if (L == 0) break;
        isGlobal =
            SwsStrEq(k, "Enable") ||
            SwsStrEq(k, "ToggleKey") ||
            SwsStrEq(k, "Gap") || SwsStrEq(k, "ColGap") ||
            SwsStrEq(k, "RightMargin") ||
            SwsStrEq(k, "TopMargin") || SwsStrEq(k, "Alpha") ||
            SwsStrEq(k, "FontPx") ||
            SwsStrEq(k, "MaxRows") ||
            SwsStrEq(k, "ScrollStep") || SwsStrEq(k, "DrainSupers") ||
            SwsStrEq(k, "DedupDefensePage");
        /* 漏一个全局键的后果: 它会被当成"超武 ID"收进 s_Ini, 显示成一个
         * 永远不亮的空条目。新增配置项时**必须**同时加进这一串, 并与
         * SwsEnsureInit 里真正读取的那份保持一致 (曾经两边不一致, 留下一堆
         * 早已删除的键, 排查时以为它们还生效)。 */
        if (!isGlobal && L < 31)
        {
            int v = GetPrivateProfileIntA(SWS_SECTION, k, 0, g_IniPath);
            SwsCopyId(s_Ini[s_IniN].id, k);
            s_Ini[s_IniN].prio = v;
            s_IniN++;
        }
        pos += L + 1;
    }
}

static void SwsEnsureInit(void)
{
    char buf[128];
    unsigned p;
    if (s_Init) return;
    s_Init = 1;
    BuildPaths();
    GtEnsureLog();
    s_Enable = GetPrivateProfileIntA(SWS_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    s_Gap = GetPrivateProfileIntA(SWS_SECTION, "Gap", 2, g_IniPath);
    s_ColGap = GetPrivateProfileIntA(SWS_SECTION, "ColGap", 0, g_IniPath);
    s_RightMargin = GetPrivateProfileIntA(SWS_SECTION, "RightMargin", 2, g_IniPath);
    /* TopMargin 一个键两用: 0~1 = 比例(屏幕高), >1 = 像素。默认 0.5 = 居中。
     * 全整数定点不用浮点。**判定和换算必须分开**: 先由字面值(含小数)决定
     * 语义 (0~1 比例 / >1 像素), 再按语义换算 —— 无条件先乘 100000 再判会让
     * "80" 变成 1000000 像素, 侧栏直接顶出屏幕。 */
    {
        char tb[32];
        OU_ReadString(SWS_SECTION, "TopMargin", "0.5", tb, sizeof(tb));
        {
            int neg = 0, seenDot = 0;
            unsigned i2 = 0, ip = 0, frac = 0, scale = 1;
            long v;
            if (tb[0] == '-') { neg = 1; i2 = 1; }
            else if (tb[0] == '+') i2 = 1;
            for (; tb[i2]; i2++)
            {
                if (tb[i2] == '.') { if (seenDot) break; seenDot = 1; continue; }
                if (tb[i2] < '0' || tb[i2] > '9') break;
                if (!seenDot) ip = ip * 10 + (unsigned)(tb[i2] - '0');
                else if (scale < 100000) { frac = frac * 10 + (unsigned)(tb[i2] - '0'); scale *= 10; }
            }
            {   /* fp4 = 原值 x10000 (纯整数, 避免 long 溢出与浮点) */
                long fp4 = (long)ip * 10000L;
                if (seenDot)
                    fp4 += (long)((frac * 10000L) / (long)(scale ? scale : 1));
                if (neg) fp4 = -fp4;
                if (fp4 < 0) fp4 = 0;
                if (fp4 <= 10000L)                       /* 0 ~ 1  -> 比例 */
                {
                    s_TopIsPct = 1;
                    v = fp4 * 10L;                      /* x100000 定点 */
                    if (v > 100000L) v = 100000L;        /* 比例上限 100.0% */
                }
                else                                      /* > 1  -> 像素 */
                {
                    s_TopIsPct = 0;
                    v = (fp4 + 5000L) / 10000L;          /* 四舍五入回像素 */
                    if (v > 1000000L) v = 1000000L;     /* 防手滑 */
                }
            }
            s_TopMargin = (int)v;
        }
    }
    /* 整体透明度: 0=完全透明, 100=不透明。作用于侧栏画出的绝大多数像素
     * (底框/图标/边框/滚动条/箭头/文字)。 */
    s_Alpha = GetPrivateProfileIntA(SWS_SECTION, "Alpha", 100, g_IniPath);
    if (s_Alpha < 0) s_Alpha = 0;
    if (s_Alpha > 100) s_Alpha = 100;
    /* 充能百分比字号。悬停名走全局 [VecText] 的字体设置, 不在此处配。 */
    s_FontPx = GetPrivateProfileIntA(SWS_SECTION, "FontPx", 12, g_IniPath);
    s_MaxRows = GetPrivateProfileIntA(SWS_SECTION, "MaxRows", 10, g_IniPath);
    s_ShowDrain = GetPrivateProfileIntA(SWS_SECTION, "DrainSupers", 0, g_IniPath);
    /* 原版"防御支援"页去重: 按钮的初始状态 (之后由点击切换)。 */
    s_DedupHide = GetPrivateProfileIntA(SWS_SECTION, "DedupDefensePage", 1,
                                        g_IniPath) ? 1 : 0;
    s_DedupDone = !s_DedupHide;
    s_ScrollStep = GetPrivateProfileIntA(SWS_SECTION, "ScrollStep", 1, g_IniPath);
    if (s_ScrollStep < 1) s_ScrollStep = 1;
    if (s_ScrollStep > 10) s_ScrollStep = 10;
    if (s_MaxRows < 1) s_MaxRows = 1;
    if (s_MaxRows > 40) s_MaxRows = 40;
    OU_ParseHotkey("F4", &s_ToggleHk);
    OU_ReadHotkey(SWS_SECTION, "ToggleKey", &s_ToggleHk);
    if (s_Gap < 0) s_Gap = 0;
    if (s_Gap > 16) s_Gap = 16;
    if (s_ColGap < 0) s_ColGap = 0;
    if (s_ColGap > 32) s_ColGap = 32;
    if (s_FontPx < 8) s_FontPx = 8;
    if (s_FontPx > 32) s_FontPx = 32;
    SwsLoadIniList();
    p = 0;
    p = AppendCStr(buf, p, "sws: en=", sizeof(buf));
    p += UtoA((unsigned)s_Enable, buf + p);
    p = AppendCStr(buf, p, " a=", sizeof(buf));
    p += UtoA((unsigned)s_Alpha, buf + p);
    p = AppendCStr(buf, p, " iniN=", sizeof(buf));
    p += UtoA((unsigned)s_IniN, buf + p);
    p = AppendCStr(buf, p, " rows=", sizeof(buf));
    p += UtoA((unsigned)s_MaxRows, buf + p);
    LogLine(buf);
}

/* ---------------- 扫描 (只读, 联机安全) ----------------
 * 组合门控 (启动期半初始化垃圾曾致 fault, 见 notes/超武侧栏.md):
 *   先纯安全读 (固定地址字 + 三元组比对, 从不碰数组内容);
 *   超武/House/本家五元组与上一轮任一不同 → 只缓存、本轮返回;
 *   五元组稳定 → 数组已落定, 再做入局检查 (本家须在 House 内)
 *     与逐项全校验 (IsPresent/ID 可打印/序号范围/标志位 0/1)。
 * 瞬态垃圾至多存活一轮, 进不了任何内容解引用。 */

static void* s_PrevSupItems = NULL;
static int s_PrevSupCount = 0;
static void* s_PrevHouseItems = NULL;
static int s_PrevHouseCount = 0;
static void* s_PrevCur = NULL;
static int s_StableInit = 0;

/* ArrayIndex by ID match against the SuperWeaponType array.
 * Read-only walk of a validated container; no writes, no engine calls. */
static int SwsArrayIndexOf(void* type, const char* id)
{
    void** items;
    int count, i;
    if (IsPlausiblePtr((void*)ADDR_SWTYPE_ARR))
    {
        items = ARR_ITEMS(ADDR_SWTYPE_ARR);
        count = ARR_COUNT(ADDR_SWTYPE_ARR);
    }
    else
    {
        items = NULL;
        count = 0;
    }
    if (items && IsPlausiblePtr(items) && count > 0 && count <= 4096)
    {
        for (i = 0; i < count; i++)
        {
            void* t = items[i];
            if (t == type) return i;
            if (!t || !IsPlausiblePtr(t)) continue;
            if (SwsStrEq((const char*)t + OFF_SWT_ID, id)) return i;
        }
    }
    return -1;
}


/* ---------------- 超武图标: 全部走引擎现成设施 ----------------
 * 图标是 PCX (rulesmo.ini 里 96 节用 SidebarPCX=xxx.pcx, 磁盘上没有, 全在
 * MIX 里)。`SidebarImage` 通常是 Type==0xFFFF 的 SHPReference, 指向
 * XXICON.SHP 时就是引擎占位图 (紫球) —— **不要**再写 SHP 兜底, 见笔记。
 *   读键: CCINIClass::INI_Rules (0x887048) -> INIClass::ReadString (0x528A10)
 *   载图: PCX::Instance (0xAC4848) -> GetSurface / ForceLoadFile
 * 两条命门: GetSurface 未加载时给的是占位图 (必须先 ForceLoadFile);
 * BSurface* **不可跨帧缓存** (切页签/重载美术会失效), 所以 surface 每次
 * 重建现取。 */

/* Ares 3.0 在 0x528A10 上挂了 INIClass_GetString (inline 5B JMP), 那是 drop-in
 * 替换 —— Ares 自己读 SidebarPCX= 也走这一个对象同一个函数, 语义一致。 */
typedef int (__thiscall *SwsIniReadStringFn)(void* ini, const char* section,
    const char* key, const char* def, char* buf, unsigned size);
#define FN_INIREADSTR ((SwsIniReadStringFn)(void*)0x528A10u)
#define ADDR_INI_RULES 0x887048u

typedef void* (__thiscall *SwsPcxGetSurfFn)(void* pcx, const char* name, void* pal);
#define FN_PCXGETSURF ((SwsPcxGetSurfFn)(void*)0x6BA140u)
typedef int (__thiscall *SwsPcxLoadFn)(void* pcx, const char* name, int f1, int f2);
#define FN_PCXLOAD ((SwsPcxLoadFn)(void*)0x6B9D00u)
#define ADDR_PCX_INSTANCE 0xAC4848u
#define PCX_TRANS_PARENT 0xF81Fu   /* COLOR_PURPLE = COLOR_RED|COLOR_BLUE */

#define PCX_CACHE 320

typedef struct {
    char id[32];
    char file[64];
    int  tried;
} SwsPcxEnt;

static SwsPcxEnt s_Pcx[PCX_CACHE];
static int s_PcxN = 0;

/* ---- 每类型缓存 ----
 * ArrayIndex (线性扫 135 项类型数组做字符串比较)、阵营、ini 优先级/隐藏
 * 都是**类型**的固有属性, 类型对象静态, 按 type 指针缓存一次即可, 之后每
 * 帧只剩指针比较。 */
#define SWS_TCACHE 320
typedef struct {
    void* type;
    int   swIdx;
    int   side;      /* -2 = 查表未命中, 要用玩家阵营 */
    int   prio;      /* -1 = 尚未填充 (SwsTypeEntry 用它当"已填"标志) */
    int   hide;
    int   manual;
    int   instant;   /* 1 = 点击即发射 */
    const char* pcxFile;
    wchar_t name[24];   /* 悬停提示, 属于**类型**而非条目下标 */
} SwsTypeCache;
static SwsTypeCache s_TC[SWS_TCACHE];
static int s_TCN = 0;

static SwsTypeCache* SwsTypeEntry(void* type, const char* id)
{
    int i;
    for (i = 0; i < s_TCN; i++)
        if (s_TC[i].type == type) return &s_TC[i];
    if (s_TCN >= SWS_TCACHE) return NULL;
    i = s_TCN++;
    s_TC[i].type = type;
    s_TC[i].swIdx = SwsArrayIndexOf(type, id);
    s_TC[i].side = -2;
    s_TC[i].prio = -1;              /* -1 = 尚未填充 */
    s_TC[i].hide = 0;
    s_TC[i].manual = 1;
    s_TC[i].instant = 0;
    s_TC[i].pcxFile = NULL;
    s_TC[i].name[0] = 0;
    return &s_TC[i];
}

/* 读某个超武节的 ini 键 (每 id 每键只查一次)。rules 在 MIX 里, 磁盘上
 * 没有 ini, 所以必须用游戏已加载好的 CCINIClass::INI_Rules 对象。 */
typedef struct { char id[32]; int manual; } SwsMfEnt;
static SwsMfEnt s_Mf[PCX_CACHE];
static int s_MfN = 0;

/* SW.ManualFire=no 的超武不进侧栏 (自动触发的技能没有手动打靶这一步);
 * 缺键默认显示。支援技能与超武同属 [SuperWeaponType] 分类, 一并显示。 */
static int SwsManualFireOk(const char* id)
{
    int i, slot = -1;
    void* rules;
    char buf[32];
    for (i = 0; i < s_MfN; i++)
        if (SwsStrEq(s_Mf[i].id, id)) { slot = i; break; }
    if (slot >= 0) return s_Mf[slot].manual;
    if (s_MfN >= PCX_CACHE) return 1;
    slot = s_MfN++;
    SwsCopyId(s_Mf[slot].id, id);
    s_Mf[slot].manual = 1;
    rules = *(void**)ADDR_INI_RULES;
    if (rules && IsPlausiblePtr(rules))
    {
        buf[0] = 0;
        FN_INIREADSTR(rules, id, "SW.ManualFire", "", buf, (unsigned)sizeof(buf));
        if (SwsStrEq(buf, "no") || SwsStrEq(buf, "false"))
            s_Mf[slot].manual = 0;
    }
    return s_Mf[slot].manual;
}

/* 取某超武的 SidebarPCX= 文件名 (读一次, 缓存在我们自己的表里)。
 * 注意: 这里只缓存"文件名", 绝不缓存引擎的 BSurface* —— 引擎会在切页签/
 * 重载美术时释放并重建 PCX surface, 跨帧持有它的指针 = 野指针 (表现为
 * 图画成紫球, 切页时崩溃)。surface 每帧现取。 */
/* ---------------- 行为旗标 (从 rules INI 读, 不猜结构偏移) ----------------
 * "点击即生效" 的判据**必须**读 ini, 不能看 `Type->Action == Action::None`:
 * Ares 把 `Action=Custom` 改写成 `Actions::SuperWeaponAllowed` (0x7F) 而不是
 * None, 而 MO 六个超武全是 Custom —— 按 Action==0 判会判错对象。
 * 真正成立的两个键: `Action=None` 与 `SW.UseAITargeting=yes` (手册: 点击图标
 * 自动发射, 无需选目标)。Ares 的 `Type=<NewSWType>` 另有两类把 Action 写成
 * None (HunterSeeker / Battery), 也算。推导过程见笔记。
 * `UseChargeDrain=yes` 是长期生效类, 只用来把它们从侧栏过滤掉。 */
typedef struct {
    char id[32];
    int instant;    /* 1 = 点击即发射, 无需选格 */
    int drain;      /* 1 = UseChargeDrain (长期生效, 点击是开关) */
    int tried;
} SwsBehavior;

#define SWS_BEH_CAP 320
static SwsBehavior s_Beh[SWS_BEH_CAP];
static int s_BehN = 0;

static int SwsIniYes(void* rules, const char* sec, const char* key)
{
    char buf[32];
    buf[0] = 0;
    if (!rules || !IsPlausiblePtr(rules)) return 0;
    FN_INIREADSTR(rules, sec, key, "", buf, (unsigned)sizeof(buf));
    if (SwsStrEq(buf, "yes") || SwsStrEq(buf, "true") ||
        SwsStrEq(buf, "YES") || SwsStrEq(buf, "TRUE"))
        return 1;
    return 0;
}

static SwsBehavior* SwsBehaviorOf(const char* id)
{
    int i, slot = -1;
    void* rules;
    char act[32];
    char typ[32];
    for (i = 0; i < s_BehN; i++)
        if (SwsStrEq(s_Beh[i].id, id)) { slot = i; break; }
    if (slot >= 0 && s_Beh[slot].tried) return &s_Beh[slot];
    if (slot < 0)
    {
        if (s_BehN >= SWS_BEH_CAP) return NULL;
        slot = s_BehN++;
        SwsCopyId(s_Beh[slot].id, id);
        s_Beh[slot].tried = 0;
    }
    s_Beh[slot].tried = 1;
    s_Beh[slot].instant = 0;
    s_Beh[slot].drain = 0;
    rules = *(void**)ADDR_INI_RULES;
    if (rules && IsPlausiblePtr(rules))
    {
        /* Action=None 才是"引擎不处理格子", 无需玩家选目标 */
        act[0] = 0;
        FN_INIREADSTR(rules, id, "Action", "", act, (unsigned)sizeof(act));
        if (SwsStrEq(act, "None")) s_Beh[slot].instant = 1;
        /* 手册: SW.UseAITargeting=yes -> 点击图标自动发射, 不需选目标 */
        if (SwsIniYes(rules, id, "SW.UseAITargeting")) s_Beh[slot].instant = 1;
        /* Type=<Ares NewSWType>: HunterSeeker 与 Battery 在各自的
         * src/Misc/SWTypes 下把 `pSW->Action = Action::None` 写死了。
         * (Firestorm 没有, 所以爆裂屏障确实要选格, 点两次才对。) */
        typ[0] = 0;
        FN_INIREADSTR(rules, id, "Type", "", typ, (unsigned)sizeof(typ));
        if (SwsStrEq(typ, "HunterSeeker") || SwsStrEq(typ, "Battery"))
            s_Beh[slot].instant = 1;
        /* 长期生效类 (爆裂屏障/心灵之火): 点击是开关 (Ready<->Draining),
         * 同样**一次点击**就完成 —— Ares 的 SW_Firewall::Activate 收下 cell
         * 参数却根本不用, 直接置 house 的 firestorm 状态。 */
        if (SwsIniYes(rules, id, "UseChargeDrain"))
        {
            s_Beh[slot].drain = 1;
            s_Beh[slot].instant = 1;
        }
    }
    return &s_Beh[slot];
}

/* 取某超武的 SidebarPCX= 文件名。只缓存**文件名**, 绝不缓存引擎的
 * BSurface* —— 引擎切页签/重载美术时会释放重建, 跨帧持有就是野指针。 */
static const char* SwsPcxFileOf(const char* id)
{
    int i, slot = -1;
    void* rules;
    char buf[64];
    for (i = 0; i < s_PcxN; i++)
        if (SwsStrEq(s_Pcx[i].id, id)) { slot = i; break; }
    if (slot >= 0 && s_Pcx[slot].tried)
        return s_Pcx[slot].file[0] ? s_Pcx[slot].file : NULL;
    if (slot < 0)
    {
        if (s_PcxN >= PCX_CACHE) return NULL;
        slot = s_PcxN++;
        SwsCopyId(s_Pcx[slot].id, id);   /* 必须记 id, 否则下次查不到 */
        s_Pcx[slot].file[0] = 0;
        s_Pcx[slot].tried = 0;
    }
    s_Pcx[slot].tried = 1;
    rules = *(void**)ADDR_INI_RULES;
    buf[0] = 0;
    if (rules && IsPlausiblePtr(rules))
        FN_INIREADSTR(rules, id, "SidebarPCX", "", buf, (unsigned)sizeof(buf));
    {
        unsigned k = 0;
        int j = 0;
        while (buf[j] && j < 63) s_Pcx[slot].file[k++] = buf[j++];
        s_Pcx[slot].file[k] = 0;
    }
    return s_Pcx[slot].file[0] ? s_Pcx[slot].file : NULL;
}


/* 现取 surface: GetSurface 命中缓存直接用, 未命中则 ForceLoadFile 再取。
 * 这几个地址均未被 Ares hook。 */
static void* SwsPcxLiveSurf(const char* file)
{
    void* pcx, *surf;
    if (!file || !file[0]) return NULL;
    pcx = (void*)ADDR_PCX_INSTANCE;
    if (!pcx || !IsPlausiblePtr(pcx)) return NULL;
    surf = FN_PCXGETSURF(pcx, file, NULL);
    if (!surf)
    {
        FN_PCXLOAD(pcx, file, 2, 0);
        surf = FN_PCXGETSURF(pcx, file, NULL);
    }
    if (surf && !IsPlausiblePtr(surf)) return NULL;
    return surf;
}

/* 首次遇到这个超武类型时把它算一遍 (阵营/优先级/隐藏/ManualFire/图标/悬停名/
 * 即时类)。类型对象静态, 只算一次 —— 抽成独立函数是因为"这个超武在侧栏上显不
 * 显示"还要被防御页去重 (SwsDedupRemove / SwsDedupRestore) 问, 而那两条路
 * 不一定先跑过一次 SwsScan。填完 tc->prio 就成了"已填"标志。 */
static void SwsFillType(SwsTypeCache* tc, void* type, const char* id)
{
    int h = 0;
    SwsBehavior* bh;
    tc->side = SwsSideOf(id);
    tc->prio = SwsPrioOf(id, &h);
    tc->hide = h;
    tc->manual = SwsManualFireOk(id);
    tc->pcxFile = SwsPcxFileOf(id);
    {
        /* 悬停提示文本: 优先 UIName, 空则退回 ID。存进**类型缓存**而不是
         * s_Items[n] —— 条目下标会随建筑增减重排, 存那里必然错位。 */
        const wchar_t* uin = *(const wchar_t**)((char*)type + OFF_SWT_UINAME);
        SwsCopyName(tc->name, uin, 24);
        if (!tc->name[0])
        {
            int k = 0;
            while (k < 23 && id[k]) {
                tc->name[k] = (wchar_t)(unsigned char)id[k];
                k++;
            }
            tc->name[k] = 0;
        }
        /* 缺 SidebarPCX 的超武: 一次性记下来, 便于查它们的图标 */
        if (!tc->pcxFile) LogS("sws: nopcx ", id);
    }
    /* "点击即生效" 读 rules INI, 理由见 SwsBehaviorOf 上方注释 */
    bh = SwsBehaviorOf(id);
    tc->instant = bh ? bh->instant : 0;
    /* 火风暴类长期生效超武 (UseChargeDrain=yes) 决定放弃显示: Ares 的
     * ChargeDrain 状态机只接在 SuperClass::ClickFire, 而我们走 SpecialPlace
     * 事件 (Fire_SW->Launch->Activate), 两条入口不相交 —— 效果能打出去但
     * ChargeDrainState 永远停在 Ready, 显示不出"生效中"。Phobos 也没接这条。
     * 推导见笔记。[SWSidebar] DrainSupers=1 可临时放回来试。 */
    if (bh && bh->drain && !s_ShowDrain)
    {
        static int s_DrainHidden = 0;
        tc->hide = 1;
        if (s_DrainHidden < 16)
        {
            LogS("sws: hide drain ", id);
            s_DrainHidden++;
        }
    }
}

/* 这个超武 (按 SuperWeaponType 数组下标) 到底显不显示在超武侧栏上。
  * 判据 = SwsScan 的显示门: !hide && ManualFire (本家/IsPresent 由调用方在
  * SuperClass 数组上自己判)。
  *
  * ⚠ 这里**刻意不判** `s_Enable` / `s_DedupHide`: 它是个纯谓词, 被"搬出"与
  * "搬回"共用。而"切回显示"时 `s_DedupHide` 恰好是 0, 一旦在这里判掉, 搬回
  * 循环会把所有超武都 `continue` 掉, 按钮就永远没反应 (日志表现为
  * `dedup show n=0`)。要判"侧栏开没开 / 要不要去重"由**调用方**自己判。 */
static int SwsSidebarShows(int swIdx)
{
    void** items;
    void* type;
    const char* id;
    int count;
    SwsTypeCache* tc;
    if (swIdx < 0 || swIdx > 1000) return 0;
    if (!IsPlausiblePtr((void*)ADDR_SWTYPE_ARR)) return 0;
    items = ARR_ITEMS(ADDR_SWTYPE_ARR);
    count = ARR_COUNT(ADDR_SWTYPE_ARR);
    if (!items || !IsPlausiblePtr(items) || count <= 0 || count > 4096) return 0;
    if (swIdx >= count) return 0;
    type = items[swIdx];
    if (!type || !IsPlausiblePtr(type)) return 0;
    id = (const char*)type + OFF_SWT_ID;
    if (!SwsIdOk(id)) return 0;
    tc = SwsTypeEntry(type, id);
    if (!tc) return 0;
    if (tc->prio < 0) SwsFillType(tc, type, id);
    if (tc->swIdx < 0) return 0;
    return (tc->hide || !tc->manual) ? 0 : 1;
}

static void SwsScan(void)
{
    void* cur;
    void** items;
    void** hItems;
    int count, hCount, i, n = 0;
    int pside, frame;
    s_ItemN = 0;
    /* 以下全是固定地址字读取, 不碰数组内容, 任何状态下安全。 */
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return;
    if (!IsPlausiblePtr((void*)ADDR_SUPER_ARR)) return;
    items = ARR_ITEMS(ADDR_SUPER_ARR);
    count = ARR_COUNT(ADDR_SUPER_ARR);
    if (!items || !IsPlausiblePtr(items) || count <= 0 || count > 4096) return;
    if (!IsPlausiblePtr((void*)ADDR_HOUSE_ARR)) return;
    hItems = ARR_ITEMS(ADDR_HOUSE_ARR);
    hCount = ARR_COUNT(ADDR_HOUSE_ARR);
    if (!hItems || !IsPlausiblePtr(hItems) || hCount <= 0 || hCount > 64)
        return;
    /* 数组换了 (开局/重开) 的那一轮只缓存、本轮不扫, 下一轮才真扫。 */
    if (!s_StableInit || items != (void**)s_PrevSupItems ||
        count != s_PrevSupCount || hItems != (void**)s_PrevHouseItems ||
        hCount != s_PrevHouseCount || cur != s_PrevCur)
    {
        s_PrevSupItems = items;
        s_PrevSupCount = count;
        s_PrevHouseItems = hItems;
        s_PrevHouseCount = hCount;
        s_PrevCur = cur;
        s_StableInit = 1;
        return;
    }
    /* 入局检查: 只做指针比对, 不解引用对象 */
    for (i = 0; i < hCount; i++)
        if (hItems[i] == cur) break;
    if (i >= hCount) return;
    pside = SwsPlayerSide();
    frame = *(int*)ADDR_FRAME;
    for (i = 0; i < count && n < SWS_CAP; i++)
    {
        void* sup = items[i];
        void* type;
        void* owner;
        const char* id;
        unsigned char present, ready, susp;
        int swIdx, total, start, left, remain, side, prio;
        const char* pcxFile = NULL;
        int instant = 0;
        SwsTypeCache* tc;
        if (!sup || !IsPlausiblePtr(sup)) continue;
        type = *(void**)((char*)sup + OFF_SUP_TYPE);
        owner = *(void**)((char*)sup + OFF_SUP_OWNER);
        if (!type || !IsPlausiblePtr(type)) continue;
        if (!owner || !IsPlausiblePtr(owner) || owner != cur) continue;
        present = *(unsigned char*)((char*)sup + OFF_SUP_PRESENT);
        if (present != 1) continue;
        ready = *(unsigned char*)((char*)sup + OFF_SUP_READY);
        susp = *(unsigned char*)((char*)sup + OFF_SUP_SUSPEND);
        if (ready > 1 || susp > 1) continue;
        id = (const char*)type + OFF_SWT_ID;
        if (!SwsIdOk(id)) continue;
        /* 按类型指针缓存 (ArrayIndex 按 ID 反查类型数组, 不能用猜的字段
         * 偏移)、阵营、ini 优先级/隐藏、SW.ManualFire、SidebarPCX、悬停名。 */
        tc = SwsTypeEntry(type, id);
        if (!tc) continue;
        if (tc->swIdx < 0) continue;
        if (tc->prio < 0) SwsFillType(tc, type, id);
        /* 支援技能与超武同分类, 一并显示; 只排除 SW.ManualFire=no。
         * 不能拿引擎 ShowTimers 集合当显示清单 —— 它只有 11 个, 是
         * "正在计时"的子集, 不是侧栏该有的全量。 */
        if (tc->hide || !tc->manual) continue;
        swIdx = tc->swIdx;
        side = tc->side;
        prio = tc->prio;
        pcxFile = tc->pcxFile;
        instant = tc->instant;
        total = *(int*)((char*)type + OFF_SWT_RECH);
        if (total < 0 || total > 1000000) total = 0;
        start = *(int*)((char*)sup + OFF_SUP_RTSTART);
        left = *(int*)((char*)sup + OFF_SUP_RTLEFT);
        if (start == -1) remain = left;
        else remain = left - (frame - start);
        if (remain < 0) remain = 0;
        if (total > 0 && remain > total) remain = total;
        s_Items[n].type = type;
        s_Items[n].pcxFile = pcxFile;
        s_Items[n].swIdx = swIdx;
        s_Items[n].side = (side >= 0) ? side : pside;
        s_Items[n].prio = prio;
        /* 名字属于类型, 存进类型缓存; 搬进条目时一并带走, 保证悬停提示和
         * 图标永远对应同一个超武 (直接拷 tc->name, 不再经过中转缓冲区)。 */
        SwsCopyName(s_Items[n].name, tc->name, 24);
        s_Items[n].ready = (ready == 1 && susp == 0) ? 1 : 0;
        s_Items[n].susp = (susp == 1) ? 1 : 0;
        s_Items[n].remain = remain;
        s_Items[n].total = total;
        s_Items[n].instant = instant;
        n++;
    }
    s_ItemN = n;
    s_OrderN = -1;      /* 条目变了, 排序缓存作废 (见 s_Order 声明处) */
    if (!s_ScanLogged && n > 0)
    {
        s_ScanLogged = 1;
        LogI("sws: scan ok n=", n);
    }
}

/* ================= 隐藏"防御支援页"里的重复超武 =================
 *
 * 目标: 侧栏上有的超武, 防御支援页 (4 页签条第 1 页) 里**不再**出现; 侧栏上
 * 没有的 (ini 隐藏 / SW.ManualFire=no / 火风暴类), 防御页照旧。
 *
 * 为什么要动游戏内部: 引擎只往页签**加** cameo, 从不因为"它已经在超武侧栏上
 * 显示了"而拒绝; 唯一的移除路径 RecheckCameo(0x6AA600) 只删 IsPresent==0 的
 * 条目, 而我们的超武都还在。Ares 3.0 把容器换成 .data 段的
 * DynamicVectorClass<BuildType> RulesExt::TabCameos[4], 没导出任何"删某个
 * cameo"的接口 —— 只能直接改这个数组。这也是本文件唯一有模拟风险的写操作,
 * 但全是本地 UI 数据, 不碰 SuperClass/EventClass。
 *
 * 两个方向刻意不对称: **搬出**改 Ares 那个数组 (要先反查地址 + 逐次校验
 * 容器); **搬回**只调引擎自己的 AddCameo (自带查重, 对已在页里的重复调用是
 * 无害空操作) —— 于是"切回显示"不依赖任何反查出来的地址, 不会因为容器校验
 * 不过而**静默失败**。复查由 SwsDedupSync 负责 (推导见笔记)。 */

/* SidebarClass 侧栏几何 (gamemd 反汇编实证; 与 YRpp 头文件差 4 字节: 头文件说
 * Tabs[0] 是首字段/CameoCount +0x50, 实际是 +0x1544/+0x54。以 Ares.dll 代码
 * 为准, 见笔记"偏移实证"。) */
#define ADDR_SIDEBAR          0x0087F7E8u   /* 侧栏对象就在这个地址上 */
#define ADDR_ADDCAMEO         0x006A6300u   /* SidebarClass::AddCameo 本体 */
#define ADDR_REPAINTSIDEBAR   0x006A60A0u   /* SidebarClass::RepaintSidebar */
#define ADDR_UPDSCROLLBTN     0x006A6610u   /* SidebarClass::UpdateScrollButtons */
#define ADDR_SETTAB           0x006A7590u   /* SidebarClass::SetTab */
#define ADDR_GETVISCNT        0x006AC430u   /* SidebarClass::GetVisibleCameoCount */
#define OFF_STRIP_BASE        0x1544u
#define OFF_STRIP_STRIDE      0x0F94u
#define OFF_STRIP_CAMCOUNT    0x54u
#define OFF_STRIP_TOPROW      0x44u
#define OFF_STRIP_INDEX       0x38u
#define OFF_SIDE_ACTIVETAB    0x539Cu
#define OFF_SIDE_NEEDREDRAW   0x53A6u
#define OFF_SIDE_NEEDBGREDRAW 0x53A7u
#define ABS_SPECIAL           0x1F   /* 引擎实际用的超武 cameo 类型 */
#define TAB_DEFENSE           1      /* 0 建筑 / 1 防御 / 2 步兵 / 3 载具 */
#define BT_SIZE               0x34u  /* sizeof(BuildType) */

/* DynamicVectorClass<BuildType> (YRpp ArrayClasses.h, 带虚表):
 *   +0x00 void* vtable
 *   +0x04 BuildType* Items
 *   +0x08 int    Capacity
 *   +0x0C BYTE   IsInitialized / BYTE IsAllocated (+2 pad)
 *   +0x10 int    Count
 *   +0x14 int    CapacityIncrement
 *   sizeof = 0x18, 4 个 = 0x60 */
#define DVEC_OFF_ITEMS  0x04u
#define DVEC_OFF_CAP    0x08u
#define DVEC_OFF_COUNT  0x10u
#define DVEC_OFF_INC    0x14u
#define DVEC_SIZE       0x18u
#define TABCAM_N        4
#define TABCAM_BYTES    0x60u
/* Ares 的 RulesExt::ClearCameos() 给四个容器统一写死的 CapacityIncrement,
 * 拿来当定位指纹 (终身不变, 见下面 SwsAresFindTabCameos 的判据 2)。 */
#define TABCAM_INC      100

/* 页签 i 的 CameoCount 实时值。
 *
 *   MouseClass / SidebarClass / DisplayClass —— **同一个静态对象, 地址就是
 *   0x87F7E8 本身** (原版直接 `mov ecx,0x87f7e8` 当 this 用)。所以这里
 *   **不能**写 `*(void**)ADDR_SIDEBAR` —— 那样读到的是虚表指针 (一个 .text
 *   代码地址), 偏移全算错, 拿它当 `this` 调引擎函数直接崩。
 *
 *   StripClass::Index +0x38 / TopRowIndex +0x44 / CameoCount +0x54
 *   SidebarClass::Tabs = pThis + 0x1544, 步长 0xF94
 *
 * 偏移**从 Ares.dll 自己的代码里读出来**: SkipSizeCheck 里有
 * `imul eax,ebx,0xf94` / `add eax,0x880d2c`, 而 0x880d2c = 0x87f7e8+0x1544;
 * RecheckCameos 里是 `cmp [edi+0x54],0` / `mov eax,[edi+0x38]`。
 * YRpp 头文件里这几个字段都比实际值小 4 —— **以 Ares.dll 代码为准**。 */
static int SwsStripCameoCount(int tab)
{
    unsigned addr;
    if (tab < 0 || tab > 3) return -1;
    if (!IsPlausiblePtr((void*)ADDR_SIDEBAR)) return -1;
    addr = (unsigned)ADDR_SIDEBAR + OFF_STRIP_BASE +
           (unsigned)tab * OFF_STRIP_STRIDE + OFF_STRIP_CAMCOUNT;
    if (!IsPlausiblePtr((void*)(size_t)addr)) return -1;
    return *(int*)(size_t)addr;
}

/* 取 Ares.dll 的映像范围 [lo, hi)。解析 PE 头拿 SizeOfImage, 只为两件事:
 * 校验我们反查出来的地址真的落在 Ares 的 .data 里, 以及校验容器的 vtable 指针
 * 是 Ares 的函数而不是别处的野指针。 */
static int SwsAresImage(unsigned char** ppLo, unsigned char** ppHi)
{
    HMODULE h;
    unsigned char *base;
    DWORD pe, osz, imgSz;
    h = GetModuleHandleA("Ares.dll");
    if (!h || !IsPlausiblePtr((const void*)h)) return 0;
    base = (unsigned char*)h;
    pe = *(DWORD*)(base + 0x3C);
    if (pe < 0x40 || pe > 0x1000 || !IsPlausiblePtr(base + pe)) return 0;
    if (base[pe] != 'P' || base[pe + 1] != 'E' || base[pe + 2] != 0 ||
        base[pe + 3] != 0) return 0;
    osz = *(WORD*)(base + pe + 20);
    if (osz <= 0 || osz > 0x400) return 0;
    if (!IsPlausiblePtr(base + pe + 24 + 56 + 4)) return 0;
    imgSz = *(DWORD*)(base + pe + 24 + 56);
    if (imgSz == 0 || imgSz > 0x4000000) return 0;
    *ppLo = base;
    *ppHi = base + imgSz;
    return 1;
}

/* 一个 DynamicVectorClass<BuildType> 自洽吗 (只判它自己, 不牵扯 Tabs) */
static int SwsDVecOk(const unsigned char* v, const unsigned char* lo,
                     const unsigned char* hi)
{
    void* vt = *(void**)v;
    int cap = *(int*)(v + DVEC_OFF_CAP);
    int cnt = *(int*)(v + DVEC_OFF_COUNT);
    int inc = *(int*)(v + DVEC_OFF_INC);
    if (!vt || (const unsigned char*)vt < lo || (const unsigned char*)vt >= hi)
        return 0;
    if (cnt < 0 || cnt > 4096) return 0;
    if (cap < cnt || cap > 1000000) return 0;
    if (inc != TABCAM_INC) return 0;
    if (cnt > 0)
    {
        void* it = *(void**)(v + DVEC_OFF_ITEMS);
        if (!it || !IsPlausiblePtr(it)) return 0;
    }
    return 1;
}

/* 定位 Ares 的 RulesExt::TabCameos[4] —— **从 Ares 自己的代码里读地址**。
 *
 * 不用"扫 .data 段找 4 连的 DynamicVectorClass": 那条路依赖
 * `StripClass::CameoCount` 互证, 而这条互证在本机对不上, 于是永远命中 0 个。
 * 改从 Ares 导出的 `SidebarClass_AddCameo_SkipSizeCheck` (钩 0x6A63B7)
 * 开头几十字节里抓这两条 (实测反汇编):
 *     8B 04 CD <disp32>     mov eax, [ecx*8 + TabCameos[0].Items]
 *     6B 0C CD <disp32> 34  imul ecx, [ecx*8 + TabCameos[0].Count], 0x34
 * 两个 disp32 都是编译期写死的绝对地址, 于是
 *     TabCameos = (Items 的 disp32) - 4, 两条互证 (Count 的 disp32) - 0x10
 * 定位跟着 Ares 的代码走: 不扫数据段、不会误命中别的容器、ASLR 后依然对。
 * 找不到 / 两条对不上 / 容器不自洽一律 fail-closed。
 *
 * 只在确实在跑一局时才找: CurrentPlayer 有效 + 帧号走过 60 (主菜单里容器
 * 还没建好)。试一次就够 —— 上面两个前置条件已保证时机, 而那条指令永远在。
 *
 * > ⚠ **不要用 `Unsorted::ScenarioInit` (0xA8E7AC) 做"在跑一局"判断**。名字
 * > 像是"场景初始化中", 实际是计数器 (gamemd 里有 `inc dword [0xA8E7AC]`)
 * > 且实战中长期为 0, 两种判法都让定位器一次都没跑过。 */
#define ARES_SKIPCHECK "SidebarClass_AddCameo_SkipSizeCheck"
#define ARES_SCAN_SPAN 0x60u
static unsigned char* SwsAresFindTabCameos(void)
{
    static int s_Failed = 0;
    FARPROC fn;
    unsigned char *base, *imgHi, *p, *found = NULL;
    unsigned itemsAddr = 0, cntAddr = 0;
    int gotItems = 0, gotCnt = 0, t;
    void* cur;
    if (s_TabCam) return s_TabCam;
    if (s_Failed) return NULL;
    /* "在跑一局" 的判据: 本家指针有效 + 帧号已经走过 60 */
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return NULL;
    if (*(int*)ADDR_FRAME < 60) return NULL;
    if (!SwsAresImage(&base, &imgHi)) { s_Failed = 1; return NULL; }
    fn = GetProcAddress((HMODULE)base, ARES_SKIPCHECK);
    if (!fn || !IsPlausiblePtr((const void*)fn)) { s_Failed = 1; return NULL; }
    /* 扫那几十字节, 抓两条绝对地址 (mov eax,[ecx*8+d] / imul ecx,[ecx*8+d],0x34) */
    for (p = (unsigned char*)fn; p + 8 <= (unsigned char*)fn + ARES_SCAN_SPAN; p++)
    {
        if (p[0] == 0x8B && p[1] == 0x04 && p[2] == 0xCD)
        {
            memcpy(&itemsAddr, p + 3, 4);
            gotItems = 1;
        }
        else if (p[0] == 0x6B && p[1] == 0x0C && p[2] == 0xCD && p[7] == 0x34)
        {
            memcpy(&cntAddr, p + 3, 4);
            gotCnt = 1;
        }
    }
    if (!gotItems || !gotCnt ||
        !IsPlausiblePtr((void*)(size_t)itemsAddr) ||
        !IsPlausiblePtr((void*)(size_t)cntAddr) ||
        (unsigned char*)(size_t)itemsAddr <= base ||
        (unsigned char*)(size_t)itemsAddr >= imgHi ||
        (unsigned char*)(size_t)cntAddr <= base ||
        (unsigned char*)(size_t)cntAddr >= imgHi)
        goto fail;
    /* Items 在容器 +0x04, Count 在 +0x10, 两条必须指向同一个容器的两个字段 */
    if ((unsigned char*)(size_t)itemsAddr - 4 !=
        (unsigned char*)(size_t)cntAddr - 0x10)
        goto fail;
    found = (unsigned char*)(size_t)itemsAddr - 4;
    /* 四个容器逐个自洽 + vtable 相同 (确实是同一个类的连续数组) */
    {
        void* vt0 = *(void**)found;
        for (t = 0; t < TABCAM_N; t++)
        {
            if (*(void**)(found + (unsigned)t * DVEC_SIZE) != vt0) goto fail;
            if (!SwsDVecOk(found + (unsigned)t * DVEC_SIZE, base, imgHi)) goto fail;
        }
    }
    s_TabCam = found;
    {
        char sb[96], b1[16];
        unsigned q = AppendCStr(sb, 0, "sws: dd tabcameo=", sizeof(sb));
        HtoA((unsigned)(size_t)found, b1);
        q = AppendCStr(sb, q, b1, sizeof(sb));
        q = AppendCStr(sb, q, " tc=", sizeof(sb));
        for (t = 0; t < TABCAM_N; t++)
        {
            UtoA((unsigned)*(int*)(found + (unsigned)t * DVEC_SIZE +
                                   DVEC_OFF_COUNT), b1);
            q = AppendCStr(sb, q, t ? "/" : "", sizeof(sb));
            q = AppendCStr(sb, q, b1, sizeof(sb));
        }
        AppendCStr(sb, q, "", sizeof(sb));
        LogLine(sb);
    }
    return s_TabCam;
fail:
    LogS("sws: dd off got=", gotItems ? (gotCnt ? "1/1" : "1/0") : "0/0");
    s_Failed = 1;
    return NULL;
}

/* 每次用之前重新校验一遍容器 (Items 指针扩容时会变, 必须现读; 定位出来的
 * **容器头地址**本身终身不变 —— Ares 只在 ClearCameos 里清内容, 从不搬容器)。
 *
 * 刻意**不**拿 `StripClass::CameoCount` 互证: 它在本机既会与 TabCameos[i].Count
 * 不同步, 拿它当闸门等于把整条路堵死。容器地址是从 **Ares 自己的代码**里读出来
 * 的, 本身就可信 (定位法见上)。 */
static int SwsTabCameoOk(void)
{
    unsigned char *base, *imgHi, *vt0;
    int t;
    if (!s_TabCam) return 0;
    if (!SwsAresImage(&base, &imgHi)) return 0;
    if (s_TabCam < base || s_TabCam + TABCAM_BYTES > imgHi) return 0;
    vt0 = (unsigned char*)*(void**)s_TabCam;
    for (t = 0; t < TABCAM_N; t++)
    {
        if (*(void**)(s_TabCam + (unsigned)t * DVEC_SIZE) != vt0) return 0;
        if (!SwsDVecOk(s_TabCam + (unsigned)t * DVEC_SIZE, base, imgHi)) return 0;
    }
    return 1;
}

/* 防御页条目变动之后把页签状态补齐 (抄 Ares StripClass_RecheckCameos 的收尾)。
 *
 * ⚠ 侧栏对象**就在** ADDR_SIDEBAR 那个地址上, 不要解引用 —— 那样读到的是对象
 * 的**虚表指针** (一个 .text 代码地址), 后面拿它当 `this` 调
 * `UpdateScrollButtons` / `SetTab` 会直接崩。 */
static void SwsDefenseRepair(void)
{
    void* sb;
    unsigned char* strip;
    int cnt, top, exc;
    if (!SwsTabCameoOk()) return;
    sb = (void*)ADDR_SIDEBAR;
    strip = (unsigned char*)sb + OFF_STRIP_BASE +
            (unsigned)TAB_DEFENSE * OFF_STRIP_STRIDE;
    cnt = *(int*)(s_TabCam + (unsigned)TAB_DEFENSE * DVEC_SIZE + DVEC_OFF_COUNT);
    /* Ares 维护两份计数: TabCameos[i].Count (我们只改这份) 和
     * StripClass::CameoCount (页签布局按它算)。两份必须跟上, 否则页签会按
     * 旧计数去画、点到已删掉的位置。所以这个写入是**必须的**; 写之前先确认
     * 读出来的确实是个正常计数, 万一偏移又对不上宁可跳过 (宁可不修也不能
     * 写坏别人的内存)。 */
    if (SwsStripCameoCount(TAB_DEFENSE) >= 0)
        *(int*)(strip + OFF_STRIP_CAMCOUNT) = cnt;
    if (cnt > 0)
    {
        ((int (__thiscall *)(void*))(void*)ADDR_UPDSCROLLBTN)(sb);
    }
    else
    {
        /* 页签空了: 关掉它的页签按钮, 并跳到第一个还非空的页签
         * (GadgetClass::Disable 0x4E1460, 与原版 SidebarClass::AddCameo
         *  0x6A6467 那段对页签按钮 enable/disable 的是同一套接口)。 */
        unsigned char* tabBtn = (unsigned char*)0xB07C48u +
                                (unsigned)TAB_DEFENSE * 0x60u;
        int idx = 0;
        if (IsPlausiblePtr(tabBtn) && IsPlausiblePtr(*(void**)tabBtn))
            ((void (__thiscall *)(void*))(void*)0x4E1460u)(tabBtn);
        while (idx < 3 &&
               *(int*)(s_TabCam + (unsigned)idx * DVEC_SIZE + DVEC_OFF_COUNT) <= 0)
            idx++;
        if (idx >= 3)
        {
            ((int (__thiscall *)(void*))(void*)ADDR_UPDSCROLLBTN)(sb);
        }
        else if (*(int*)((char*)sb + OFF_SIDE_ACTIVETAB) == TAB_DEFENSE)
        {
            ((void (__thiscall *)(void*, int))(void*)ADDR_SETTAB)(sb, idx);
        }
    }
    /* 滚动位置只夹取、不重置: 玩家可能正翻到防御页中间, 一按按钮就被弹回顶部
     * 纯属添乱。夹取上界 = 条目数 - 一屏能显示的条数 (GetVisibleCameoCount)。 */
    {
        int vis = ((int (__thiscall *)(void*))(void*)ADDR_GETVISCNT)(sb);
        if (vis < 1) vis = 1;
        top = *(int*)(strip + OFF_STRIP_TOPROW);
        exc = cnt - vis;
        if (exc < 0) exc = 0;
        if (top < 0) top = 0;
        if (top > exc) top = exc;
        *(int*)(strip + OFF_STRIP_TOPROW) = top;
    }
    *(unsigned char*)((char*)sb + OFF_SIDE_NEEDREDRAW) = 1;
    *(unsigned char*)((char*)sb + OFF_SIDE_NEEDBGREDRAW) = 1;
    ((void (__thiscall *)(void*, int))(void*)ADDR_REPAINTSIDEBAR)(sb, TAB_DEFENSE);
}

/* 从防御页删掉所有"侧栏上也有"的超武 cameo。返回删掉几条, -1 = 不可用。 */
static int SwsDedupRemove(void)
{
    unsigned char* vec, *items;
    int cnt, i, removed = 0;
    if (!SwsTabCameoOk()) return -1;
    vec = s_TabCam + (unsigned)TAB_DEFENSE * DVEC_SIZE;
    cnt = *(int*)(vec + DVEC_OFF_COUNT);
    if (cnt <= 0) return 0;
    items = *(unsigned char**)(vec + DVEC_OFF_ITEMS);
    if (!items || !IsPlausiblePtr(items)) return -1;
    /* 倒着删: 后面先缩, 前面那条的下标才不会因为搬移而失效 */
    for (i = cnt - 1; i >= 0; i--)
    {
        int idx = *(int*)(items + (size_t)i * BT_SIZE);
        int typ = *(int*)(items + (size_t)i * BT_SIZE + 4);
        int j, tail;
        if (typ != ABS_SPECIAL) continue;
        if (!SwsSidebarShows(idx)) continue;
        tail = cnt - 1 - i;
        for (j = 0; j < tail; j++)
        {
            const unsigned char* src = items + (size_t)(i + 1 + j) * BT_SIZE;
            unsigned char* dst = items + (size_t)(i + j) * BT_SIZE;
            int b;
            for (b = 0; b < (int)BT_SIZE; b++) dst[b] = src[b];
        }
        cnt--;
        removed++;
    }
    if (removed) *(int*)(vec + DVEC_OFF_COUNT) = cnt;
    return removed;
}

/* 把"侧栏上有、但防御页没有"的超武 cameo 补回去 (走引擎自己的 AddCameo)。
 * 返回补上几条, -1 = 不可用。 */
static int SwsDedupRestore(void)
{
    void* cur;
    void** items;
    int count, i, added = 0;
    /* 刻意**完全不碰** Ares 的 TabCameos 容器: 补货走引擎自己的 AddCameo,
     * 而 AddCameo 自己就会查重 (Ares 的 SidebarClass_AddCameo_SkipSizeCheck
     * 扫一遍 TabCameos[TabIndex], 已经有了就返回 false), 所以对"已经在页里"
     * 的超武重复调用是**无害的空操作**。这样"切回显示"就不依赖任何反查出来
     * 的地址 —— 容器校验一旦不过, 那条路会静默失败。 */
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return -1;
    if (!IsPlausiblePtr((void*)ADDR_SUPER_ARR)) return -1;
    items = ARR_ITEMS(ADDR_SUPER_ARR);
    count = ARR_COUNT(ADDR_SUPER_ARR);
    if (!items || !IsPlausiblePtr(items) || count <= 0 || count > 4096) return -1;
    for (i = 0; i < count; i++)
    {
        void* sup = items[i];
        void* type, *owner;
        int swIdx;
        if (!sup || !IsPlausiblePtr(sup)) continue;
        type = *(void**)((char*)sup + OFF_SUP_TYPE);
        owner = *(void**)((char*)sup + OFF_SUP_OWNER);
        if (!type || !IsPlausiblePtr(type)) continue;
        if (!owner || !IsPlausiblePtr(owner) || owner != cur) continue;
        if (*(unsigned char*)((char*)sup + OFF_SUP_PRESENT) != 1) continue;
        if (*(unsigned char*)((char*)sup + OFF_SUP_READY) > 1) continue;
        if (*(unsigned char*)((char*)sup + OFF_SUP_SUSPEND) > 1) continue;
        swIdx = *(int*)((char*)type + OFF_SWT_ARRIDX);
        if (swIdx < 0 || swIdx > 1000) continue;
        if (!SwsSidebarShows(swIdx)) continue;
        if (((int (__thiscall *)(void*, int, int))(void*)ADDR_ADDCAMEO)
                ((void*)ADDR_SIDEBAR, ABS_SPECIAL, swIdx))
            added++;
    }
    /* AddCameo 自己会 RepaintSidebar(1), 但它只在"页签从空变非空"那种情况下
     * 切页签; 这里再补一次刷新, 保证刚补进来的都画出来。 */
    if (added)
        ((void (__thiscall *)(void*, int))(void*)ADDR_REPAINTSIDEBAR)
            ((void*)ADDR_SIDEBAR, TAB_DEFENSE);
    return added;
}

/* 把目标态落到防御页上 (幂等)。返回 1 = 确实动过。
 *
 * 只在"搬出"这条路上去改 Ares 的容器; "搬回"只调引擎自己的 AddCameo (它自带
 * 查重), 不依赖任何反查出来的地址, 也就不会因为定位/校验失败而静默失效。
 *
 * 已经在隐藏态时也照样跑一遍 (移除是幂等的, 代价只是扫一遍条目): 新冒进防御
 * 页的超武 cameo 就靠这次复查搬走 —— 不要试图用 AddCameo 入口钩去"挡住新增",
 * 那个点根本不会被调用 (超武 cameo 走的是别的路, 见笔记)。 */
static int SwsDedupSync(void)
{
    int want, r;
    SwsEnsureInit();
    if (!s_Enable) { s_DedupDirty = 0; return 0; }
    /* 目标态**只看去重开关**, 与侧栏显隐无关: 收起 (F4) 时保持防御页当前的
     * 显隐状态。防御页是引擎自己的页签, 收起侧栏并不会让它失效, 所以"侧栏
     * 藏了就把防御页放出来"没有必要 —— 反而会让玩家按 F4 时刚去重掉的图标
     * 莫名其妙全回来。 */
    want = s_DedupHide ? 1 : 0;
    if (want == s_DedupDone && !s_DedupDirty) return 0;
    if (want && !SwsAresFindTabCameos()) { s_DedupDirty = 0; return 0; }
    r = want ? SwsDedupRemove() : SwsDedupRestore();
    if (r < 0)
    {
        /* 状态没变 → 下一帧还会重试, 所以**必须限流**, 否则一次失败能刷两千行
         * 把日志撑爆。 */
        static int s_FailAge = 0;
        if (s_FailAge <= 0)
        {
            LogLine(want ? "sws: dd remove unavailable"
                         : "sws: dd restore unavailable");
            s_FailAge = 600;      /* 600 帧 (~10s) 再记一次 */
        }
        s_FailAge--;
        s_DedupDirty = 0;
        return 0;
    }
    /* SwsDefenseRepair 会写 Ares/引擎维护的字段 (CameoCount) 并调
     * UpdateScrollButtons / SetTab —— 只有"搬出"改了容器才需要它, "搬回"
     * 由 AddCameo 自己把页签状态弄好了。复查时没搬动任何东西也不用动页签。 */
    if (want && r > 0) SwsDefenseRepair();
    /* 只在真动了东西 / 目标态翻转时记一行, 免得复查每 5 帧刷一行 */
    if (r > 0 || want != s_DedupDone)
    {
        char sb[64], n[16];
        unsigned q = AppendCStr(sb, 0, "sws: dd ", sizeof(sb));
        q = AppendCStr(sb, q, want ? "hide n=" : "show n=", sizeof(sb));
        UtoA((unsigned)r, n);
        AppendCStr(sb, q, n, sizeof(sb));
        LogLine(sb);
    }
    s_DedupDone = want;
    s_DedupDirty = 0;
    return r;
}

/* ---------------- 轮询 (切换键 + 点击 + 扫描) ---------------- */


/* ---------------- 滚轮 (由 MOExperience_WheelHook @0x777998 调用) ----------------
 * 语义与战役框一致: 鼠标落在侧栏区内才接管滚轮, dir<0 = 上滚(看更靠前),
 * dir>0 = 下滚(看更靠后); 步长 [SWSidebar] ScrollStep, 默认 1。
 * 返回 1 = 已消费 (调用方不要再滚战役框), 0 = 不在侧栏上。 */
extern "C" __declspec(dllexport) int __cdecl SWS_WheelIn(int mx, int my, int dir)
{
    int step;
    step = s_ScrollStep;   /* SwsEnsureInit 读一次; 这里每滚轮读 ini 是文件 I/O */
    if (!s_Enable || !s_Visible || !s_BoundsKnown) return 0;
    /* 命中范围: 整块 (含右侧滚动条那几像素) */
    if (mx < s_BoundX || mx > s_LastX + SWS_ICON_W + 5) return 0;
    if (my < s_YStart || my >= s_YStart + (s_VisRows * s_RowH - s_Gap)) return 0;
    if (s_MaxSk <= 0) return 0;                  /* 所有列都装得下 */
    if (dir < 0) s_Scroll -= step;
    else          s_Scroll += step;
    if (s_Scroll < 0) s_Scroll = 0;
    if (s_Scroll > s_MaxSk) s_Scroll = s_MaxSk;
    /* s_Scroll 进了缓存重建的指纹 (见 SwsSig), 所以下一帧 SWS_Draw 就会重画 */
    return 1;
}

/* 鼠标是否在侧栏区内 (含滚动条): 战役框 WheelHook 用它决定优先级 */
extern "C" __declspec(dllexport) int __cdecl SWS_OverBar(int mx, int my)
{
    if (!s_Enable || !s_Visible || !s_BoundsKnown) return 0;
    if (mx < s_BoundX || mx > s_LastX + SWS_ICON_W + 5) return 0;
    if (my < s_YStart || my >= s_YStart + (s_VisRows * s_RowH - s_Gap)) return 0;
    return 1;
}

void SWS_Poll(void)
{
    int tog, lb, rb;
    /* ---- 帧门: 这个函数挂在 0x4F42F0, 而那个地址**不是**每帧一次 ----
     * gamemd 里 `call 0x4F42F0` 有 113 个静态调用点, 散在 AI / 对象 / 渲染 /
     * 消息代码里, 多处位于逐对象循环内。日志实测: 每渲染帧被调约 **142 次**
     * (区间 47~225) —— 不加门就是每帧 142 次 GetAsyncKeyState + 2 次 QPC。
     * 用引擎帧号 0xA8ED84 变化做门: 帧号在输入处理之后才推进, 而 0x4F42F0
     * 首次调用在帧首, 所以"与上次相同则跳过"即可, 不必关心它推进几次。 */
    {
        static int s_LastFrame = -1;
        int fr = *(int*)ADDR_FRAME;
        if (fr == s_LastFrame) return;
        s_LastFrame = fr;
    }
    SwsEnsureInit();
    if (!s_Enable) return;
    tog = OU_HotkeyDown(&s_ToggleHk) ? 1 : 0;
    if (tog && !s_PrevTog)
    {
        s_Visible = s_Visible ? 0 : 1;
        s_DedupDirty = 1;    /* 收起 = 防御页恢复原样, 展开 = 重新隐藏 */
        LogLine(s_Visible ? "sws: show" : "sws: hide");
    }
    s_PrevTog = tog;
    if (!s_Visible)
    {
        /* 隐藏时仍采样键态, 并保留显隐箭头的点击 (箭头一直画着, 不处理就变成
         * "看得见但点不动")。
         * ⚠ 边沿必须用**和可见分支同一份** s_PrevLB, 而且每条 return 分支都要
         *   自己写回。否则下一帧进来时 s_PrevLB 还是"上一帧没按键"的旧值而按键
         *   正按着, 会伪造出一个新按下沿, 刚收起的侧栏立刻又弹回来 (症状: 点
         *   一下只闪一下)。 */
        {
            int lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
            int rb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
            if (lb && !s_PrevLB)
            {
                int hx = -1, hy = -1;
                if (GtMouseGameXY(&hx, &hy) &&
                    hx >= s_BtnX && hx < s_BtnX + s_BtnW &&
                    hy >= s_BtnY && hy < s_BtnY + s_BtnH)
                {
                    s_Visible = 1;
                    s_DedupDirty = 1;
                    s_ForceScan = 1;   /* 刚点的展开箭头: 立刻重扫, 别等 15 帧 */
                    s_HoldSkip = 1;    /* 吞掉, 别让这次点击漏到地图 */
                    LogLine("sws: show");
                }
            }
            s_PrevLB = lb;
            s_PrevRB = rb;
        }
        s_ItemN = 0;
        s_PendSel = -2;
        s_SelDown = 0;
        if (!s_PrevLB) s_HoldSkip = 0;   /* 与可见分支同规则: 按住期间不清 */
        /* 收起状态下防御页要恢复原样, 所以这条早退之前先把补货做完。 */
        SwsDedupSync();
        return;
    }
    /* 应用上一帧记下的选择 (选中后的**松开**边沿记下的, 见 s_PendSel 注释)。
     * 此时左键已松开, 引擎"按住即释放"的条件不可能成立。 */
    if (s_PendSel != -2)
    {
        int v = s_PendSel;
        s_PendSel = -2;
        *(int*)ADDR_CURSW = v;
        LogLine(v >= 0 ? "sws: armed" : "sws: cancel");
    }
    {
        lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
        rb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
        /* ---- 上膛统一在**松开**边沿做 (见 s_PendSel 注释) ---- */
        if (s_SelDown && !lb)
        {
            s_SelDown = 0;
            s_PendSel = s_ArmIdx;
            LogLine("sws: release-arm");
        }
        if ((lb && !s_PrevLB) || (rb && !s_PrevRB))
        {
            s_ForceScan = 1;   /* 点击: 立刻重扫, 不等 15 帧节流 */
            int mx = -1, my = -1;
            if (s_BoundsKnown && GtMouseGameXY(&mx, &my))
            {
                if (lb && !s_PrevLB)
                {
                    /* 两个按钮都画在超武块**上方** (栏区判定框之外), 所以必须
                     * 在栏区判定**之前**单独命中 —— 栏区要求 my >= s_BoundY
                     * (=yStart), 按钮在别的行, 放进去就永远点不到。 */
                    if (mx >= s_BtnX && mx < s_BtnX + s_BtnW &&
                        my >= s_BtnY && my < s_BtnY + s_BtnH)
                    {
                        s_Visible = s_Visible ? 0 : 1;
                        s_DedupDirty = 1;
                        LogLine(s_Visible ? "sws: show" : "sws: hide");
                        /* 按下就得吞掉, 否则这次点击会漏到引擎那边, 而"已选中
                         * + 按住"会让引擎把超武就地释放。 */
                        s_HoldSkip = 1;
                        s_PrevLB = lb;    /* return 前自己写回, 见上面的 ⚠ */
                        s_PrevRB = rb;
                        SwsDedupSync();
                        return;
                    }
                    /* 隐藏重复按钮: 显隐按钮左边那个。切换"防御页里的重复超武
                     * 要不要拿掉", 真正搬/补在函数末尾的 SwsDedupSync 里做。
                     * s_HidValid 保证它没画出来的那几帧不吃点击。
                     *
                     * 这里**不**因为"定位不到容器"就不让点: 定位不到只是搬不走
                     * 已经在那儿的, 以后新增的超武照样不会进防御页。所以按钮
                     * 永远给反馈, 搬不动就靠日志说话 (定位器自己会记 dedup off)。 */
                    if (s_HidValid &&
                        mx >= s_HidX && mx < s_HidX + s_BtnW &&
                        my >= s_HidY && my < s_HidY + s_BtnH)
                    {
                        s_DedupHide = s_DedupHide ? 0 : 1;
                        s_DedupDirty = 1;
                        s_HoldSkip = 1;
                        LogLine(s_DedupHide ? "sws: dedup btn hide"
                                            : "sws: dedup btn show");
                        s_PrevLB = lb;
                        s_PrevRB = rb;
                        return;
                    }
                }
                if (mx >= s_BoundX && mx < s_BoundX + s_BoundW &&
                    my >= s_BoundY && my < s_BoundY + s_BoundH)
                {
                    if (lb && !s_PrevLB)
                    {
                    int k;
                    for (k = 0; k < s_ItemN; k++)
                    {
                        /* 被滚出可见范围的 (x=-1) 不参与命中: 不跳过的话
                         * x=-1 会让 "mx>=-1 && mx<59" 恒真, 屏幕左侧整条
                         * 竖带都会误判成点在这些不可见条目上。 */
                        if (s_Items[k].x < 0) continue;
                        if (mx >= s_Items[k].x &&
                            mx < s_Items[k].x + SWS_ICON_W &&
                            my >= s_Items[k].y &&
                            my < s_Items[k].y + SWS_ICON_H)
                        {
                            /* 释放后进入充能: 未就绪不可再选 */
                            if (!s_Items[k].ready)
                            {
                                s_HoldSkip = 1;
                                LogLine("sws: clk notready");
                                break;
                            }
                            if (s_Items[k].instant)
                            {
                                /* Action==None: 点击即生效, 不用再点地图。
                                 * 走标准事件 SpecialPlace (Phobos LaunchSuper
                                 * 同式), 而不是写 CurrentSW —— 后者只是本地
                                 * UI 状态, 引擎当帧不会释放, 所以"点了没反应"。
                                 * 事件由引擎在 DoList 执行, 联机也同步。
                                 * 这条不写 CurrentSWTypeIndex, 所以不受
                                 * "松开才上膛"影响, 按下即发。 */
                                if (SwsPushSpecialPlace(s_Items[k].swIdx))
                                {
                                    /* 事件已排队: 这一击仍然止于侧栏 */
                                    s_HoldSkip = 1;
                                    LogLine("sws: instant");
                                }
                                else
                                {
                                    /* OutList 满 (0x80): 本帧发不出去, 照旧吞掉
                                     * 这一击, 别让它漏到地图上变成误下单。 */
                                    s_HoldSkip = 1;
                                    LogLine("sws: instant queued fail");
                                }
                            }
                            else
                            {
                                /* 手动打靶: 按下只记 idx, **松开**才写
                                 * CurrentSWTypeIndex (见 s_PendSel 注释)。
                                 * 整段按住由 s_HoldSkip 吞掉, 等价于真
                                 * Gadget 消费掉这次完整点击。 */
                                s_ArmIdx = s_Items[k].swIdx;
                                s_SelDown = 1;
                                s_HoldSkip = 1;
                            }
                            break;
                        }
                    }
                    }   /* 栏区判定 (mx/my 在超武块内) */
                }
                else if (rb && !s_PrevRB)
                {
                    /* 右键取消: 目标就是"无选中", 按住右键不会引发释放。
                     * 必须限在栏区里 —— 只按 s_BoundsKnown 判的话, 玩家在**地图
                     * 上**右键取消选中的单位反而会把超武的选中状态清了。 */
                    if (mx >= s_BoundX && mx < s_BoundX + s_BoundW &&
                        my >= s_BoundY && my < s_BoundY + s_BoundH)
                    {
                        *(int*)ADDR_CURSW = -1;
                        LogLine("sws: cancel");
                    }
                }
            }
        }
        /* 吞没只在"这次按下起始于我们栏内"期间有效: 松开即清除,
         * 否则玩家之后点地图也会被吞, 表现为"能选但放不出来"。 */
        if (!lb) s_HoldSkip = 0;
        s_PrevLB = lb;
        s_PrevRB = rb;
    }
    /* 扫描节流: 每 15 个**逻辑帧** (约 0.25 秒) 才重扫一次。侧栏内容本来就
     * 看不出每帧的变化; 玩家点击时 s_ForceScan 立刻补一次, 保证点到的图标
     * 状态是最新的。 */
    {
        static int s_ScanAge = 0;
        s_ScanAge++;
        if (s_ScanAge >= 15 || s_ForceScan)
        {
            s_ScanAge = 0;
            s_ForceScan = 0;
            SwsScan();
            /* 顺便把防御页去重**标记**成"要复查": 新冒进防御页的超武 cameo 靠
             * 这一遍搬走。只在"隐藏"态复查; "显示"态是按钮点一下才需要补, 而
             * 补的判据 (AddCameo 自带查重) 不会因为时间推移而失效。 */
            if (s_DedupHide) s_DedupDirty = 1;
        }
    }
    SwsDedupSync();
}

/* ---------------- 点击吞没门 ---------------- */

/* 鼠标压在哪个**可见的超武图标**上。
 * 返回的是**排序后**的下标 (s_Order 里的位置), -1 = 无。
 *
 * ⚠ 这个下标必须和高亮/图标循环里的 i 用**同一个坐标系**。那两个循环走的
 *   是 s_Items[s_Order[i]], 而条目在 s_Items 里的原序 (SuperClass 数组序)
 *   和排序后 (阵营 → ini 优先级 → ArrayIndex) **不是一回事**; 拿排序下标直接
 *   索引 s_Items 会取到另一个超武。消费方一律经 s_Items[s_Order[hover]] 取。
 *
 * 用的就是**屏上这一帧**的布局 (s_Items[].x/y 由缓存重建时发布), 所以命中与
 * 画面永远一致; 重建一改布局, 下一帧就会重画。
 *
 * 命中不能拿整块栏区矩形来判。栏区底框是按每列的 maxRows 画满的, 某列条目少的
 * 时候下半截是空的, 再加上栏区内部的左右留白 —— 这些地方**根本没有图标**, 却会
 * 把玩家的点击整片吞掉 ("侧栏盖住的地图那块点不动, 框单位/打靶都用不了")。
 * 所以只在真正画了图标的地方挡, 空地一律还给引擎。 */
static int SwsHitItem(int mx, int my)
{
    int k, o;
    if (s_ItemN <= 0) return -1;
    for (k = 0; k < s_ItemN; k++)
    {
        const SwsItem* it;
        /* 扫描刚换过条目集而重建还没跑时 s_Order 是上一套的, 夹一道 */
        o = s_Order[k];
        if (o < 0 || o >= s_ItemN) continue;
        it = &s_Items[o];
        /* 滚出可见范围的 (x=-1) 不算: 不跳过的话 x=-1 会让命中判定恒真 */
        if (it->x < 0) continue;
        if (mx >= it->x && mx < it->x + SWS_ICON_W &&
            my >= it->y && my < it->y + SWS_ICON_H)
            return k;
    }
    return -1;
}

static int SwsMouseOnIcon(int mx, int my)
{
    return SwsHitItem(mx, my) >= 0;
}

extern "C" __declspec(dllexport) unsigned __cdecl SWS_ClickGate(void* regs)
{
    int mx = -1, my = -1;
    int r = 0;
    (void)regs;
    SwsEnsureInit();
    if (!s_Enable || !s_Visible) return 0;
    /* 当帧记下的按下: 一律跳过引擎的 ProcessClickCoords, 让这次点击止于侧栏,
     * 不被当成打靶放置。即时超武也一样吞 —— 它靠 SpecialPlace 事件生效。
     *
     * 命中判定用 s_BoundsKnown (= 缓存里有可合成的一帧), 也就是拿屏上这一帧的
     * 图标矩形判: 点击那帧的重扫只改条目数据, 不改布局。 */
    if (s_HoldSkip) r = ADDR_CLICKGATE_SKIP;
    else if (!s_BoundsKnown) r = 0;
    else if (!GtMouseGameXY(&mx, &my)) r = 0;
    else if (SwsMouseOnIcon(mx, my)) r = ADDR_CLICKGATE_SKIP;
    return (unsigned)r;
}

/* ---------------- 即时超武: SpecialPlace 事件 (联机安全) ----------------
 * "点了就放" 的超武不写 CurrentSW —— 那只是本地 UI 态, 引擎当帧并不释放 (点了
 * 没反应), 而且直接改模拟状态会破坏联机同步。照 Phobos LaunchSuper 往
 * EventClass::OutList 塞一个标准事件:
 *   EventClass::OutList.Add(EventClass(pCurrent->ArrayIndex,
 *       EventType::SpecialPlace, swIndex, CellStruct::Empty));
 * 引擎在 DoList 里统一执行, 联机由 Mega/Connect 广播。OutList 追加复用
 * BuildQueue.cpp 里已验证的那套 (0x4C6B60 ctor + 计数推进), 保持与原版同序。
 * 返回 1=已入队, 0=队列满发不出去。 */
#define SWS_ADDR_CURPLAYER  0xA83D4Cu   /* HouseClass* CurrentPlayer (同 ADDR_CURPLAYER) */
#define SWS_OFF_HOUSE_INDEX 0x30u       /* HouseClass::ArrayIndex */
#define SWS_EVT_SPECIALPLACE 0x12u      /* EventType::SpecialPlace */
typedef void (__attribute__((thiscall)) *Fn_EvSpCtor)(void*, int, int, int, const void*);
#define FN_EVSPCTOR ((Fn_EvSpCtor)(void*)0x4C6B60u)

static int SwsPushSpecialPlace(int swIdx)
{
    unsigned count, next, i;
    unsigned eaxAfter;
    char ev[SWS_EVT_SIZE];
    int houseIdx;
    void* house;
    void* cb;

    house = *(void**)SWS_ADDR_CURPLAYER;
    if (!house || !IsPlausiblePtr(house)) return 0;
    if (swIdx < 0 || swIdx > 0xFFFF) return 0;
    count = *(unsigned*)SWS_ADDR_OUT_COUNT;
    if (count >= SWS_EVT_CAP) return 0;          /* 满了就下帧重试 */
    /* Next 读回来就夹一次: 它是环形计数器的 Tail, 引擎自己会掩码推进,
     * 万一读到脏值也不能拿它当数组下标去写 0xA802D4+next*111。 */
    next = *(unsigned*)SWS_ADDR_OUT_NEXT & (SWS_EVT_SLOTS_N - 1u);

    houseIdx = *(int*)((char*)house + SWS_OFF_HOUSE_INDEX);
    /* ctor 之前必须把 111 字节全清零: EventClass 是 pack(1), union 后面 90
     * 多字节 ctor 不碰, 而 EventClass::operator== 是 memcmp(...,111) ——
     * 留栈垃圾等于给联机同步埋雷。清零是确定值, 比"抄引擎那条"(它也是垃圾)
     * 更安全。 */
    for (i = 0; i < SWS_EVT_SIZE; i++) ev[i] = 0;
    /* CellStruct::Empty (0,0): 与引擎点**原版侧栏**发出的那条逐字节一致
     * ([0..15] 全同, cell 同为 0,0)。存在"收下 cell 却根本不用的"类型
     * (如 SW_Firewall::Activate), 所以"没有目标"正是传 Empty 的意思。 */
    {
        short cell[2] = { 0, 0 };
        FN_EVSPCTOR(ev, houseIdx, SWS_EVT_SPECIALPLACE, swIdx, cell);
    }

    for (i = 0; i < SWS_EVT_SIZE; i++)
        ((char*)(SWS_ADDR_OUT_BUF + next * SWS_EVT_SIZE))[i] = ev[i];

    /* 同款无参回调 (原版追加事件后必调一次, 返回值存并行数组)。
     * 必须用**普通的函数指针调用**, 不能写成内联汇编: 汇编里 "call *%1" 只
     * 声明了 ecx/edx/cc/memory 被破坏, 而真实 call 还会破坏 ebx/esi/edi/ebp
     * —— 编译器以为这几个还是自己的值, 实际已被 callee 改掉, 于是往近 NULL
     * 地址写。让编译器自己生成 call, 保存/恢复由它负责。 */
    cb = *(void**)0x7E1530u;
    if (cb && IsPlausiblePtr(cb))
        eaxAfter = (unsigned)(size_t)((int (__cdecl *)(void))cb)();
    else
        eaxAfter = 0;
    *(unsigned*)(SWS_ADDR_OUT_PAR + next * 4u) = eaxAfter;

    *(unsigned*)SWS_ADDR_OUT_NEXT = (next + 1u) & (SWS_EVT_SLOTS_N - 1u);
    *(unsigned*)SWS_ADDR_OUT_COUNT = count + 1u;
    return 1;
}

/* ================= 绘制: 离屏缓存 + 15 逻辑帧节流 =================
 *
 * 侧栏整块贴在一个**每渲染帧都被重画**的合成面上, 所以不能每帧从零画一遍。
 * 拆成两层:
 *   重建 (SwsRebuild)    画进自有缓存, 触发 = 15 个游戏逻辑帧一次 + 交互;
 *   合成 (SwsCacheBlit)  每渲染帧一次带 alpha 的拷贝, 内层无除法。
 * 理由与逐项开销账见笔记"绘制: 离屏缓存"。
 *
 * 缓存是"16 位色 + 8 位覆盖率"两张平面: 侧栏各层要互相压上去 (底框 → 图标 →
 * 边框 → 提示框), 最后才与面下的地图合成; 只存色会把"半透明"烙死在缓存里,
 * 换个背景就错。存覆盖率才能既做层间混合, 又在合成时与地图正确混合。
 *
 * 两条配套约束:
 *   - **绘制顺序固定**: 底框 → 图标+文字 → 每列边框 → 高亮框 → 滚动条 →
 *     按钮 → 提示框。动这个顺序前先想清楚谁盖谁 (边框要盖住图标、高亮框要
 *     盖住列边框)。
 *   - **悬停提示挂在图标下面、不跟鼠标走**: 锚在鼠标 x 上时, 横移一像素就得
 *     重画整块, 与缓存相冲, 而且逐像素跟随本身就是抖的。
 */

/* ---- 缓存 ---- */
#define SWS_REBUILD_FRAMES 15      /* 重建节流: N 个游戏逻辑帧 (与扫描同频) */
#define SWS_CACHE_MAXPX    2000000 /* 缓存上限 2M 像素 (6MB), 超过就不画 */

static unsigned short* s_CchC = NULL;   /* 16 位色 */
static unsigned char*  s_CchA = NULL;   /* 覆盖率 0~255 (0 = 完全透明) */
static int s_CchCap = 0, s_CchW = 0, s_CchH = 0;
static int s_CchX = 0, s_CchY = 0;      /* 缓存左上角在合成面里的位置 */
static int s_CchOK = 0;                 /* 缓存里有可合成的内容 */
static int s_CchFrame = -1;             /* 上次重建时的逻辑帧号 */
static unsigned s_CchSig = 0;           /* 上次重建时的结构指纹 */
static int s_CchHover = -2;             /* 上次重建用的悬停项 (改了就重画) */
static int s_CchBtn = -1;               /* 上次重建用的按钮悬停态 */
static int s_CchSel = -9999;            /* 上次重建用的当前选中 (改了就重画) */

/* 8 位 RGB -> 目标位深的 16 位色 (与 Blend16 同一套移位/掩码) */
static unsigned short SwsPack(unsigned char r, unsigned char g, unsigned char b)
{
    return (unsigned short)(((unsigned)r * ((1u << g_RBits) - 1u) / 255u)
                                << (g_GBits == 6 ? 11 : 10) |
                            ((unsigned)g * ((1u << g_GBits) - 1u) / 255u) << 5 |
                            ((unsigned)b * ((1u << g_BBits) - 1u) / 255u));
}

/* 缓存内一次"源在上"合成: 源色 c, 源覆盖率 a8 (0~255)。
 * 两条快路吃掉绝大多数像素 —— 源全覆盖 (s_Alpha=100 的图标/文字) 与目标空
 * (最底下那层底框) —— 都不含一次除法; 中间那档 (半透明压半透明, 例如
 * Alpha<100 的图标压在 30% 底框上) 才走除法, 而它只在重建时跑。
 * 逐分量插、不跨分量借位, 与 Blend16 同一套移位/掩码。 */
static void SwsOver(unsigned short* dp, unsigned char* ap,
                    unsigned short c, int a8)
{
    int da = *ap, na, ia, den;
    unsigned bs, gm;
    int r, g2, b, dr, dg, db;
    if (a8 >= 255) { *dp = c; *ap = 255; return; }
    if (da == 0) { *dp = c; *ap = (unsigned char)a8; return; }
    ia = 255 - a8;
    na = a8 + (da * ia + 127) / 255;
    if (na <= 0) { *ap = 0; return; }
    den = 255 * na;
    bs = (g_GBits == 6) ? 11u : 10u;
    gm = (g_GBits == 6) ? 0x3Fu : 0x1Fu;
    r = c & 0x1F; g2 = (c >> 5) & gm; b = (c >> bs) & 0x1F;
    dr = *dp & 0x1F; dg = (*dp >> 5) & gm; db = (*dp >> bs) & 0x1F;
    /* r*a8*255 <= 63*255*255, dc*da*ia 同量级, 和 < 2^24, int 装得下 */
    r = (r * a8 * 255 + dr * da * ia + (den >> 1)) / den;
    g2 = (g2 * a8 * 255 + dg * da * ia + (den >> 1)) / den;
    b = (b * a8 * 255 + db * da * ia + (den >> 1)) / den;
    if (r > 31) r = 31;
    if (g2 > (int)gm) g2 = (int)gm;
    if (b > 31) b = 31;
    *dp = (unsigned short)((b << bs) | (g2 << 5) | r);
    *ap = (unsigned char)na;
}

/* 矩形填充进缓存。四边各夹一次, 内层循环不逐像素判越界。取屏幕坐标。 */
static void SwsCFill(int x, int y, int rw, int rh, unsigned char r,
                     unsigned char g, unsigned char b, int op)
{
    int x0, x1, y0, y1, xx, yy, a8;
    unsigned short c;
    if (!s_CchA || rw <= 0 || rh <= 0 || op <= 0) return;
    if (op > 100) op = 100;
    x -= s_CchX; y -= s_CchY;
    x0 = (x < 0) ? 0 : x;
    x1 = x + rw; if (x1 > s_CchW) x1 = s_CchW;
    y0 = (y < 0) ? 0 : y;
    y1 = y + rh; if (y1 > s_CchH) y1 = s_CchH;
    if (x1 <= x0 || y1 <= y0) return;
    c = SwsPack(r, g, b);
    a8 = (op * 255 + 50) / 100;
    for (yy = y0; yy < y1; yy++)
    {
        unsigned short* crow = s_CchC + (size_t)yy * s_CchW;
        unsigned char* arow = s_CchA + (size_t)yy * s_CchW;
        for (xx = x0; xx < x1; xx++) SwsOver(crow + xx, arow + xx, c, a8);
    }
}
/* 吃侧栏整体透明度 (底框/边框/滚动条/按钮/未就绪罩)。 */
static void SwsCFillA(int x, int y, int rw, int rh, unsigned char r,
                      unsigned char g, unsigned char b, int op)
{
    if (op < 0) op = 0;
    if (op > 100) op = 100;
    SwsCFill(x, y, rw, rh, r, g, b, (op * s_Alpha) / 100);
}
/* **不**吃整体透明度: 只给悬停提示框。它是"此刻正在读"的信息, 必须始终清晰
 * —— 跟着侧栏淡到 30% 就看不清是哪个超武了。 */
static void SwsCFillR(int x, int y, int rw, int rh, unsigned char r,
                      unsigned char g, unsigned char b, int op)
{
    SwsCFill(x, y, rw, rh, r, g, b, op);
}

/* ---- PCX 逐像素贴图进缓存 ----
 * BSurface 布局 (gamemd 0x6B9D00 构造函数实证, 与 YRpp Surface.h:144 一致):
 *   +0x00 vptr / +0x04 Width / +0x08 Height / +0x0C LockLevel /
 *   +0x10 BytesPerPixel(=2) / +0x14 Buffer(WORD*, 行距=Width*2) /
 *   +0x18 Size(=W*H*2) / +0x1C Allocated;  sizeof 0x20
 * ⚠ 别把 0x88/0x8C/0x94/0x98 当成字段偏移 —— 那是虚表大小 (34*4)。
 *
 * 没有兜底: 校验不过就不画这个图标 (条目仍在, 边框/百分比照旧), 且只记一行
 * 日志。**不要**再引回 PCX::BlitToSurface 作兜底: 它只能 blit 进已锁的合成面,
 * 而重建期刻意不持锁面, 为它中途 unlock/relock 再重入半段布局代价太大; 而且它
 * 100% 不透明、不认 Alpha, 留着只会让半透明在个别条目上失效。
 * 而重建期刻意不持锁面 —— 为了它中途 unlock/relock 再重入半段布局, 代价
 * 远大于它兜的那点风险; 而且它 100% 不透明、不认 Alpha, 留着只会让半透明
 * 在个别条目上失效。 */
static void SwsCPcx(int x, int y, int dw, int dh, void* psurf, int opaque)
{
    int pw, ph, bpp, r, xx, x0, x1, y0, a8;
    unsigned short* src;
    unsigned short sxmap[SWS_ICON_W];
    static int s_BadLogged = 0;
    if (!s_CchA || dw <= 0 || dh <= 0 || dw > SWS_ICON_W) return;
    if (!psurf || !IsPlausiblePtr(psurf)) return;
    if (!IsPlausiblePtr(*(void**)psurf)) return;   /* 虚表必须指向代码段 */
    pw = *(int*)((char*)psurf + 0x04);
    ph = *(int*)((char*)psurf + 0x08);
    bpp = *(int*)((char*)psurf + 0x10);
    src = *(unsigned short**)((char*)psurf + 0x14);
    /* 占位面通常尺寸离谱 (1x1 / 0), 这几道校验就是挡它的 */
    if (pw < 1 || pw > 256 || ph < 1 || ph > 256 || bpp != 2) goto bad;
    if (!src || !IsPlausiblePtr((void*)src)) goto bad;
    if (*(int*)((char*)psurf + 0x18) != pw * ph * 2) goto bad;
    x -= s_CchX; y -= s_CchY;
    x0 = (x < 0) ? 0 : x;
    x1 = x + dw; if (x1 > s_CchW) x1 = s_CchW;
    y0 = (y < 0) ? 0 : y;
    if (x1 <= x0) return;
    /* 缩放映射表: 源宽固定, 一次算好 dw 项 (下标相对 x0 —— 用绝对下标会越出
     * 这张 60 项的小表)。逐像素现算是每像素两次整数除法, 22 万像素就 44 万次。 */
    for (xx = x0; xx < x1; xx++)
        sxmap[xx - x0] = (unsigned short)(((xx - x) * pw) / dw);
    /* 悬停的那一个画不透明: 图标也是"此刻正在读"的东西, 不该被侧栏 Alpha 一起
     * 淡掉 (与悬停名提示框同一个规矩)。 */
    a8 = opaque ? 255 : (s_Alpha * 255 + 50) / 100;
    if (a8 <= 0) return;
    for (r = 0; r < dh; r++)
    {
        int ty = y + r;
        int sy = (ph <= 1) ? 0 : (r * ph) / dh;
        unsigned short* crow;
        unsigned char* arow;
        const unsigned short* srow;
        if (ty < y0) continue;              /* 上边被裁 */
        if (ty >= s_CchH) break;            /* 下边被裁: 后面整段都白跑 */
        crow = s_CchC + (size_t)ty * s_CchW;
        arow = s_CchA + (size_t)ty * s_CchW;
        srow = src + (size_t)sy * pw;
        for (xx = x0; xx < x1; xx++)
        {
            unsigned short c = srow[sxmap[xx - x0]];
            if (c == PCX_TRANS_PARENT) continue;   /* 色键 */
            SwsOver(crow + xx, arow + xx, c, a8);
        }
    }
    return;
bad:
    if (!s_BadLogged)
    {
        s_BadLogged = 1;
        LogLine("sws: pcx surface layout bad, icons skipped");
    }
}

/* 覆盖率 blit 进缓存。src 是 1 字节/像素的覆盖率 (GDI DIB 是 4 字节/像素,
 * 取第 2 字节, 白字 R=G=B)。aScale 只做调用方的预乘, 这里**不**再吃
 * s_Alpha —— 覆盖率本身就是不透明度, 与 s_Alpha 无关。 */
static void SwsCCov(const unsigned char* src, int spitch, int sbpp,
                    int x, int y, int w, int h,
                    unsigned char r, unsigned char g, unsigned char b,
                    int aScale)
{
    int xs, ys, x0, x1, y0, y1, xx, yy, cb = (sbpp == 4) ? 2 : 0;
    unsigned short c;
    if (!s_CchA || !src || w <= 0 || h <= 0 || aScale <= 0) return;
    if (aScale > 100) aScale = 100;
    xs = x - s_CchX; ys = y - s_CchY;
    x0 = (xs < 0) ? 0 : xs;
    x1 = xs + w; if (x1 > s_CchW) x1 = s_CchW;
    y0 = (ys < 0) ? 0 : ys;
    y1 = ys + h; if (y1 > s_CchH) y1 = s_CchH;
    if (x1 <= x0 || y1 <= y0) return;
    c = SwsPack(r, g, b);
    for (yy = y0; yy < y1; yy++)
    {
        const unsigned char* srow =
            src + (size_t)(yy + s_CchY - y) * spitch;   /* DIB/提示行号 */
        unsigned short* crow = s_CchC + (size_t)yy * s_CchW;
        unsigned char* arow = s_CchA + (size_t)yy * s_CchW;
        for (xx = x0; xx < x1; xx++)
        {
            int cov = srow[(xx - xs) * sbpp + cb];
            int a;
            if (!cov) continue;
            a = (cov * aScale + 50) / 100;
            if (a > 255) a = 255;
            if (!a) continue;
            SwsOver(crow + xx, arow + xx, c, a);
        }
    }
}

/* 缓存 -> 已锁的合成面: 每渲染帧唯一的一次遍历, 也是唯一碰得到地图的地方。
 * 逐分量线性插值 (与 SwsOver 同一套移位/掩码)。除法用 "(v+128)>>8" 代替:
 * (v*ia) 在 ±7905 内, 算术右移 8 位只是除 255 的近似, 加半个 LSB 把舍入补上
 * (a=1 这种极低不透明度时不补能差整整一个最低位)。a==0 的像素整条跳过, 所以
 * 侧栏没画的地方是**零开销**。
 *
 * ⚠ 这里有**两套坐标系**: 缓存内下标 x ∈ [0,s_CchW) ↔ 画面列 s_CchX + x。
 *   (a) 写的时候必须加 s_CchX, 漏了整块就贴到画面最左边;
 *   (b) 夹取也必须按"缓存内下标"算: 把目标列区间 [0,surfW) 平移成缓存内下标
 *       区间, 再与 [0,s_CchW) 取交, **两头都夹**。只写 "if (超出画面) x1 =
 *       surfW - s_CchX" 是不够的 —— 整块在画面内时那个分支根本不进, x1 就留在
 *       s_CchX+s_CchW, 内层多跑 s_CchX 次, 源指针越过本行读进下一行缓存、目标
 *       也越过右缘写进下一行, 整块侧栏在屏幕上被平铺成一片。
 *   (同层的 SwsCFill/SwsCPcx/SwsCCov 都是"先减原点、再夹、之后一律缓存下标",
 *    没有这个问题; 只有 blit 必须拿未平移的 s_CchX 去跟画面宽度求交。) */
static void SwsCacheBlit(unsigned char* base, int pitch, int surfW, int surfH)
{
    int y, x, x0, x1;
    unsigned bs, gm;
    if (!s_CchC || !s_CchA || !s_CchOK) return;
    bs = (g_GBits == 6) ? 11u : 10u;
    gm = (g_GBits == 6) ? 0x3Fu : 0x1Fu;
    x0 = 0;
    x1 = s_CchW;
    if (s_CchX < 0) x0 = -s_CchX;                       /* dest col 0 */
    if (s_CchX + s_CchW > surfW) x1 = surfW - s_CchX;   /* dest col surfW-1 */
    if (x1 > s_CchW) x1 = s_CchW;
    if (x1 <= x0) return;
    for (y = 0; y < s_CchH; y++)
    {
        int ty = s_CchY + y;
        unsigned short* drow;
        const unsigned short* crow;
        const unsigned char* arow;
        if (ty < 0) continue;
        if (ty >= surfH) break;
        drow = (unsigned short*)(base + (size_t)ty * pitch);
        crow = s_CchC + (size_t)y * s_CchW;
        arow = s_CchA + (size_t)y * s_CchW;
        for (x = x0; x < x1; x++)
        {
            int a = arow[x], ia, t, r, g2, b, dr, dg, db;
            unsigned short c, d;
            int dx = s_CchX + x;              /* 画面列 */
            if (!a) continue;
            c = crow[x];
            if (a >= 255) { drow[dx] = c; continue; }
            d = drow[dx];
            ia = 255 - a;
            r = c & 0x1F; g2 = (c >> 5) & gm; b = (c >> bs) & 0x1F;
            dr = d & 0x1F; dg = (d >> 5) & gm; db = (d >> bs) & 0x1F;
            t = (dr - r) * ia; r += (t + 128) >> 8;
            t = (dg - g2) * ia; g2 += (t + 128) >> 8;
            t = (db - b) * ia; b += (t + 128) >> 8;
            drow[dx] = (unsigned short)(((b & 0x1F) << bs) | ((g2 & gm) << 5) |
                                        (r & 0x1F));
        }
    }
}

/* 尺寸变了才重分配 (只发生在窗口缩放/改配置, 不是每帧)。
 * 只清覆盖率平面: 覆盖率 0 的像素在合成时被整条跳过, 颜色平面留着旧值
 * 永远读不到, 省一半 memset。 */
static int SwsCacheAlloc(int w, int h)
{
    int need, i;
    if (w <= 0 || h <= 0) return 0;
    need = w * h;
    if (need > SWS_CACHE_MAXPX)
    {
        static int s_TooBig = 0;
        if (!s_TooBig)
        {
            s_TooBig = 1;
            LogLine("sws: cache too big, sidebar not drawn (lower MaxRows/Gap)");
        }
        return 0;
    }
    if (need > s_CchCap)
    {
        unsigned short* c;
        unsigned char* a;
        static int s_AllocFail = 0;
        if (s_CchC) { LocalFree(s_CchC); s_CchC = NULL; }
        if (s_CchA) { LocalFree(s_CchA); s_CchA = NULL; }
        s_CchCap = 0;
        c = (unsigned short*)LocalAlloc(LMEM_FIXED, (SIZE_T)need * 2u);
        a = (unsigned char*)LocalAlloc(LMEM_FIXED, (SIZE_T)need);
    if (!c || !a)
    {
        if (c) LocalFree(c);
        if (a) LocalFree(a);
        /* 分配不成时下面会走 nogeo (s_CchOK=0), 而 SWS_Draw 每帧都会
         * 再进来试一次 —— 不限流就是一帧一行日志把日志撑爆。 */
        if (!s_AllocFail) { s_AllocFail = 1; LogLine("sws: cache alloc fail"); }
        return 0;
    }
        s_CchC = c; s_CchA = a; s_CchCap = need;
    }
    s_CchW = w; s_CchH = h;
    for (i = 0; i < need; i++) s_CchA[i] = 0;
    return 1;
}

/* ---- 悬停名提示: 字形缓存 ----
 * 照搬 MOExperience 的 PwrCache 做法: 把 DIB 的覆盖率拷进自有小缓冲, 之后画进
 * 缓存只读它。直接每帧 RenderLine 不行 —— 它每次都要先 ClearDib 一遍
 * 1024x64x4 (256KB), 悬停时每帧一次纯属白烧; 有了它, 换名字或换字号才重新
 * 光栅化。
 * 宽度取 DIB_W: RenderLine 自己就把行宽夹在 DIB_W-8, 所以这块缓冲永远装得下,
 * 可见宽度与直接用 BlitCoverage 完全一致 (字号拉满 + 长名字也不会退化成没提示)。 */
#define SWS_TIP_COV_W 1024
#define SWS_TIP_COV_H 48
static struct {
    int fontPx, bold, sw, adv, ok;
    wchar_t nm[24];
    unsigned char cov[SWS_TIP_COV_W * SWS_TIP_COV_H];
} s_Tip = { 0, 0, 0, 0, 0, { 0 }, { 0 } };

/* 备好悬停提示的字形; 0 = 没有提示/备不成。必须在分配缓存**之前**调用 ——
 * 提示框可能探出侧栏块之外, 缓存矩形得把它整个算进去。 */
static int SwsTipPrepare(const wchar_t* nm)
{
    int gpx, adv, sw, sl = 0, yy, xx;
    if (!nm || !nm[0]) return 0;
    gpx = VecText_FontPx();
    if (gpx <= 0) gpx = 18;
    if (s_Tip.ok && gpx == s_Tip.fontPx && VecText_Bold() == s_Tip.bold &&
        FaceSame(nm, s_Tip.nm))
        return s_Tip.sw;
    s_Tip.ok = 0; s_Tip.sw = 0;
    s_Tip.fontPx = gpx; s_Tip.bold = VecText_Bold();
    SwsCopyName(s_Tip.nm, nm, 24);   /* 有界拷贝: 别用 CopyFace (那是 48 宽的) */
    /* 提示框用 GDI 槽 6, 充能百分比用槽 5: 两者字号来源不同 ([VecText] vs
     * [SWSidebar] FontPx), 共用一个槽会让 GdiEnsure 每次都拆了重建。 */
    if (!GdiEnsure(gpx, s_Tip.bold, 6, VecText_FontFace())) return 0;
    adv = g_G->adv > 0 ? g_G->adv : gpx + 2;
    while (sl < 23 && nm[sl]) sl++;
    sw = sl ? RenderLine(nm, sl) : 0;
    if (sw <= 0 || sw + 2 > SWS_TIP_COV_W || adv > SWS_TIP_COV_H) return 0;
    for (yy = 0; yy < adv; yy++)
    {
        const unsigned char* srow = g_G->bits + (size_t)yy * g_G->pitch;
        for (xx = 0; xx < sw + 2; xx++)
            s_Tip.cov[yy * SWS_TIP_COV_W + xx] = srow[xx * 4 + 2];
    }
    s_Tip.sw = sw; s_Tip.adv = adv; s_Tip.ok = 1;
    return sw;
}

/* 提示框矩形 (屏幕坐标)。ax = 锚点 (图标水平中心), ayTop = 图标**下**边。
 * rw<=0 表示没有提示。
 *
 * 默认挂在图标**下面**: 侧栏块顶上方只有两个按钮那一行 (18px), 挂上去框顶会
 * 探出块外被缓存边界削掉一截 —— 表现就是名字被拦腰截断。挂下面则始终落在块内
 * 那一侧, 只有最后一行可能顶到屏幕底, 那种情况翻上去 (缓存上边界会同时让出)。
 * 水平方向仍要夹: 名字再长也不能跑出画面。 */
static void SwsTipRect(int w, int h, int ax, int ayTop,
                       int* rx, int* ry, int* rw, int* rh)
{
    int sw = s_Tip.ok ? s_Tip.sw : 0, adv = s_Tip.adv, sx, ny;
    *rw = 0; *rh = 0;
    if (sw <= 0) return;
    sx = ax - sw / 2;
    ny = ayTop + 2;
    if (ny - 2 + adv + 4 > h) ny = ayTop - adv - 4;   /* 底下放不下就翻上去 */
    if (sx < 4) sx = 4;
    /* 夹取要按**框**算, 否则右边会跑出屏幕 */
    if (sx + sw + 6 > w - 2) sx = (w - sw - 8 > 4) ? w - sw - 8 : 4;
    *rx = sx - 2; *ry = ny - 2; *rw = sw + 6; *rh = adv + 4;
}

/* 提示框: 半透明黑底 + 主题色边框 + 白字。刻意**不吃**侧栏整体透明度
 * (SwsCFillR / SwsCCov 都不乘 s_Alpha)。 */
static void SwsDrawTip(int w, int h, int ax, int ayTop)
{
    int bx, by, bw, bh, sx;
    if (!s_Tip.ok) return;
    SwsTipRect(w, h, ax, ayTop, &bx, &by, &bw, &bh);
    if (bw <= 0) return;
    SwsCFillR(bx, by, bw, bh, 0, 0, 0, 150);
    SwsCFillR(bx, by, bw, 1, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
    SwsCFillR(bx, by + bh - 1, bw, 1, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
    SwsCFillR(bx, by, 1, bh, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
    SwsCFillR(bx + bw - 1, by, 1, bh, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
    sx = bx + 2;
    SwsCCov(s_Tip.cov, SWS_TIP_COV_W, 1, sx + 1, by + 1, s_Tip.sw, s_Tip.adv,
            0, 0, 0, 85);
    SwsCCov(s_Tip.cov, SWS_TIP_COV_W, 1, sx, by, s_Tip.sw, s_Tip.adv,
            255, 255, 255, 100);
}

/* ---- 显隐按钮: 圆角底板 + 边框 + 实心三角 ----
 * 三层, 由外到内:
 *   1) 底板  整块矩形, 主题色压得很淡, 只为让按钮有个"面"
 *   2) 边框  四条边亮一档, 深色地图上也看得见轮廓
 *   3) 三角  按列画的高度渐变, 尖端指向 dir 指定的方向
 *        dir=0 -> ◀ 侧栏隐藏时, 点它展开
 *        dir=1 -> ▶ 侧栏显示时, 点它收起
 * 悬停态 hov 由调用方算好传进来 —— 按钮画在缓存里, 亮暗变化要靠重建。 */
static void SwsDrawArrow(int x, int y, int bw, int bh, int dir, int hov)
{
    int cx, inX, inY, inW, inH, a;
    if (bw < 8) bw = 8;
    if (bh < 5) bh = 5;
    SwsCFillA(x, y, bw, bh, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B,
              hov ? 70 : 45);
    a = hov ? 225 : 165;
    SwsCFillA(x, y, bw, 1, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, a);
    SwsCFillA(x, y + bh - 1, bw, 1, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, a);
    SwsCFillA(x, y, 1, bh, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, a);
    SwsCFillA(x + bw - 1, y, 1, bh, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, a);
    inX = x + 3; inY = y + 2;                 /* 留 2px 内边距 */
    inW = bw - 6; inH = bh - 4;
    if (inW < 2) inW = 2;
    if (inH < 1) inH = 1;
    for (cx = 0; cx < inW; cx++)
    {
        int hh = (inH * cx) / (inW - 1 > 0 ? inW - 1 : 1);
        int off;
        if (dir) hh = inH - hh;               /* dir=1 ▶ 尖端在右 */
        if (hh < 1) hh = 1;
        if (hh > inH) hh = inH;
        off = (inH - hh) / 2;
        SwsCFillA(inX + cx, inY + off, 1, hh, SWS_ACCENT_R, SWS_ACCENT_G,
                  SWS_ACCENT_B, hov ? 255 : 215);
    }
}

/* ---- 隐藏重复按钮: 禁入牌 (方框 + 斜杠) ----
 * 语义直白: 开着 = 防御页上的重复超武被"禁止"了, 灭 = 原样。
 * 开着偏绿、灭掉偏灰, 悬停整块提亮, 所以不靠形状也读得出状态。 */
static void SwsDrawDedup(int x, int y, int bw, int bh, int on, int hov)
{
    static const unsigned char kOn[3]  = { 90, 235, 150 };
    static const unsigned char kOff[3] = { 155, 155, 165 };
    const unsigned char* c = on ? kOn : kOff;
    int s, bx, by, i, a;
    if (bw < 8) bw = 8;
    if (bh < 5) bh = 5;
    SwsCFillA(x, y, bw, bh, c[0], c[1], c[2], hov ? 70 : 40);
    a = hov ? 235 : (on ? 190 : 115);
    SwsCFillA(x, y, bw, 1, c[0], c[1], c[2], a);
    SwsCFillA(x, y + bh - 1, bw, 1, c[0], c[1], c[2], a);
    SwsCFillA(x, y, 1, bh, c[0], c[1], c[2], a);
    SwsCFillA(x + bw - 1, y, 1, bh, c[0], c[1], c[2], a);
    s = bw - 6;                                /* 取宽高小的那个做正方, 居中 */
    if (bh - 6 < s) s = bh - 6;
    if (s < 3) s = 3;
    bx = x + (bw - s) / 2;
    by = y + (bh - s) / 2;
    a = hov ? 235 : (on ? 210 : 130);
    SwsCFillA(bx, by, s, 1, c[0], c[1], c[2], a);
    SwsCFillA(bx, by + s - 1, s, 1, c[0], c[1], c[2], a);
    SwsCFillA(bx, by, 1, s, c[0], c[1], c[2], a);
    SwsCFillA(bx + s - 1, by, 1, s, c[0], c[1], c[2], a);
    /* 斜杠只在"隐藏中"时画, 于是灭掉的按钮是个空框, 状态一眼可分 */
    if (on)
        for (i = 0; i < s; i++)
            SwsCFillA(bx + i, by + i, 1, 1, c[0], c[1], c[2], hov ? 255 : 225);
}

/* ---- 两个按钮的公共几何 ----
 * 挂在超武块**上方** 2px 的一行, 右对齐到块右缘, 从右往左
 * [显隐][空 3][隐藏重复]; 块因此要多让出 18px 顶部空间, 而不用在右边留位。
 * ⚠ 只能挂块上方: SWS_Draw 拿到的是**合成面**, 宽 = 屏幕宽 − 原版侧栏宽
 * (日志 1752 = 1920-168), 块右缘已紧贴原版侧栏, 挂右边 x 会算到面外被丢掉。
 * 位置由重建期统一发布, 因为 SWS_Poll 的命中读的就是这几个值 —— 两个按钮都在
 * 栏区判定框 (s_BoundX/Y/W/H) **之外**(块顶之上), 命中判定必须排在栏区判定
 * **之前**, 否则永远点不到。 */
#define SWS_BTN_W  20      /* 宽>高, 好点中 */
#define SWS_BTN_H  16
#define SWS_BTN_GAP 3

static void SwsTwoBtnGeom(int xStart, int totW, int w, int blockTop)
{
    int right = xStart + totW;
    s_BtnW = SWS_BTN_W;
    s_BtnH = SWS_BTN_H;
    s_BtnX = right - s_BtnW;
    s_HidX = s_BtnX - SWS_BTN_GAP - s_BtnW;
    if (s_BtnX + s_BtnW > w) s_BtnX = w - s_BtnW;   /* 贴屏幕左边时兜底 */
    if (s_HidX < 0) s_HidX = 0;
    if (s_BtnX < 0) s_BtnX = 0;
    s_BtnY = blockTop - s_BtnH - 2;   /* 上方, 留 2px 缝, 不压图标 */
    if (s_BtnY < 0) s_BtnY = 0;
    s_HidY = s_BtnY;
    s_HidValid = 1;
    s_BtnXKeep = s_BtnX;   /* 收起时原样沿用, 折叠/展开不许跳位 */
    s_BtnYKeep = s_BtnY;
}

/* 侧栏收起时: 只留一个显隐箭头。
 * ⚠ 位置必须和展开态**完全一致**, 所以沿用 SwsTwoBtnGeom 记下的坐标。收起时
 *   块没了不代表锚点该换 —— 玩家要能对着同一个位置点回去。
 *   (别改成"贴屏幕右缘": 展开锚的是"块右缘−宽", 两者差一个滚动条让出的 5px,
 *   于是每次折叠/展开箭头都肉眼可见地跳一下。)
 *   从没展开过 (s_BtnXKeep<0) 才退 fallback。 */
static void SwsCollapsedBtnGeom(int w, int blockTop)
{
    s_BtnW = SWS_BTN_W;
    s_BtnH = SWS_BTN_H;
    if (s_BtnXKeep >= 0)
    {
        s_BtnX = s_BtnXKeep;
        s_BtnY = s_BtnYKeep;
    }
    else
    {
        s_BtnX = w - s_RightMargin - s_BtnW;
        s_BtnY = blockTop - s_BtnH - 2;
    }
    if (s_BtnX < 0) s_BtnX = 0;
    if (s_BtnX + s_BtnW > w) s_BtnX = w - s_BtnW;
    if (s_BtnX < 0) s_BtnX = 0;
    if (s_BtnY < 0) s_BtnY = 0;
    s_HidX = s_BtnX;
    s_HidY = s_BtnY;
    s_HidValid = 0;
}

/* 鼠标压在哪个按钮上: 0=无 1=显隐 2=隐藏重复。亮暗画在缓存里, 所以这个值
 * 变了就得重建。 */
static int SwsHitBtn(int mx, int my)
{
    if (mx < 0 || my < 0) return 0;
    if (s_HidValid && mx >= s_HidX && mx < s_HidX + s_BtnW &&
        my >= s_HidY && my < s_HidY + s_BtnH) return 2;
    if (mx >= s_BtnX && mx < s_BtnX + s_BtnW &&
        my >= s_BtnY && my < s_BtnY + s_BtnH) return 1;
    return 0;
}

/* ---- 重建判据: 一个"结构指纹" ----
 * 凡是把画面改得**面目全非**的输入都进去, 剩下的交给 15 帧节流。
 * ⚠ 刻意**不含** remain (充能倒计时): 只要有一个超武在充能, 它每次扫描
 *   都在变, 放进来等于永不作废 —— 缓存就白做了。倒计时跟着节流走 (每秒
 *   4 次), 视觉上完全够用。
 * ⚠ 也不含 x/y: 那是本函数自己刚算出来的布局, 不是输入。
 * 悬停项/按钮悬停/当前选中 (curSel) 同样不进指纹, 它们在 SWS_Draw 里
 * 单独跟缓存里"上次用的值"比 —— 所以**鼠标移上去、点中、松开上膛**都是
 * 当帧就重画, 不用等节流。 */
static unsigned SwsSig(int w, int h, int pitch)
{
    unsigned s = 0x5A17C0D3u;
    int i;
    s = s * 33u + (unsigned)w;
    s = s * 33u + (unsigned)h;
    s = s * 33u + (unsigned)pitch;
    s = s * 33u + (unsigned)s_Visible;
    s = s * 33u + (unsigned)s_DedupHide;
    s = s * 33u + (unsigned)s_Scroll;
    s = s * 33u + (unsigned)s_ItemN;
    for (i = 0; i < s_ItemN && i < SWS_CAP; i++)
    {
        const SwsItem* it = &s_Items[i];
        s = s * 31u + (unsigned)it->swIdx;
        s = s * 31u + (unsigned)it->side;
        s = s * 31u + (unsigned)it->prio;
        s = s * 31u + (unsigned)it->ready;
        s = s * 31u + (unsigned)it->susp;
        s = s * 31u + (unsigned)it->instant;
        s = s * 31u + (unsigned)(size_t)it->pcxFile;
    }
    return s;
}

/* 收起态重建: 只有一个显隐箭头 (◀, 指左边=展开)。
 * 纵向位置沿用同一套 TopMargin 规则, 但**不减半个块高** —— 隐藏时没有块,
 * 减去半高会让箭头偏上, 与展开后的位置对不上。 */
static void SwsRebuildCollapsed(int w, int h, unsigned sig, int fr, int btnHov)
{
    int by;
    if (s_TopIsPct) by = (int)(((long)h * (long)s_TopMargin) / 100000L);
    else            by = s_TopMargin;
    if (by < 0) by = 0;
    SwsCollapsedBtnGeom(w, by);
    if (!SwsCacheAlloc(s_BtnW, s_BtnH)) { s_CchOK = 0; s_BoundsKnown = 0; return; }
    s_CchX = s_BtnX; s_CchY = s_BtnY;
    SwsDrawArrow(s_BtnX, s_BtnY, s_BtnW, s_BtnH, 0, btnHov == 1);
    s_CchOK = 1;
    s_BoundsKnown = 0;      /* 收起时没有可点的超武块 */
    s_CchSig = sig; s_CchFrame = fr;
    s_CchHover = -1; s_CchBtn = btnHov; s_CchSel = -9999;
}

/* 展开态重建: 布局 + 全部画进缓存。 */
static void SwsRebuild(int w, int h, int mx, int my, int curSel,
                       unsigned sig, int fr, int btnHov)
{
    int n = s_ItemN;
    int colCount[4] = { 0, 0, 0, 0 };
    int colX[4] = { 0, 0, 0, 0 };
    int rows[4];
    int i, f, totW, xStart, yStart, rowH, maxRows, blockH;
    int bar, barX, barW, firstX, right, bottom, top, ch, hover = -1, txtOk;
    int tipX = 0, tipY = 0, tipW = 0, tipH = 0;
    if (n <= 0 || n > SWS_CAP) goto nogeo;

    /* 1) 排序: 阵营 -> ini 优先级 -> ArrayIndex。s_Items 只有 SwsScan 会改,
     *    所以顺序与帧无关, 只在条目数变了才真排一次。 */
    if (s_OrderN != n)
    {
        for (f = 0; f < 4; f++) s_OrderColCount[f] = 0;
        for (i = 0; i < n; i++) s_Order[i] = i;
        for (i = 1; i < n; i++)
        {
            int t = s_Order[i], k = i - 1;
            while (k >= 0)
            {
                SwsItem* a = &s_Items[s_Order[k]];
                SwsItem* b = &s_Items[t];
                int gt = 0;
                if (a->side != b->side) gt = a->side > b->side;
                else if (a->prio != b->prio) gt = a->prio > b->prio;
                else gt = a->swIdx > b->swIdx;
                if (!gt) break;
                s_Order[k + 1] = s_Order[k];
                k--;
            }
            s_Order[k + 1] = t;
        }
        for (i = 0; i < n; i++) s_OrderColCount[s_Items[s_Order[i]].side]++;
        s_OrderN = n;
    }
    for (f = 0; f < 4; f++) colCount[f] = s_OrderColCount[f];
    maxRows = 0;
    for (f = 0; f < 4; f++) if (colCount[f] > maxRows) maxRows = colCount[f];
    if (maxRows <= 0) goto nogeo;
    /* 超过上限的列用滚动条: 可见行数取 min */
    if (s_MaxRows > 0 && maxRows > s_MaxRows) maxRows = s_MaxRows;
    /* 夹紧滚动偏移: 最大偏移 = 最长列条目数 - 可见行数 (滚到最后一屏就停;
     * 写成 colCount-1 会一路滚到只剩最后一行, 中间全空) */
    s_MaxSk = 0;
    for (f = 0; f < 4; f++)
        if (colCount[f] - maxRows > s_MaxSk) s_MaxSk = colCount[f] - maxRows;
    if (s_Scroll > s_MaxSk) s_Scroll = s_MaxSk;
    if (s_Scroll < 0) s_Scroll = 0;
    rowH = SWS_ICON_H + s_Gap;
    blockH = maxRows * rowH - s_Gap;
    totW = 0;
    for (f = 0; f < 4; f++)
        if (colCount[f] > 0) totW += (totW ? s_ColGap : 0) + SWS_ICON_W;
    /* 有列装不下时, 整块左边让出 5px 给右侧那根滚动条 (否则会顶出屏幕) */
    bar = (s_MaxSk > 0);
    barW = bar ? 4 : 0;
    xStart = w - s_RightMargin - totW - (bar ? 5 : 0);
    if (xStart < 0) xStart = 0;
    /* 比例 = 屏幕高 * 比例, 再减去半个块高 -> 视觉上居中 (块不会偏上);
     * 像素 = 直接当 Y 坐标。纵向要多留 SWS_BTN_H+2 给块顶上方的两个按钮。 */
    if (s_TopIsPct)
        yStart = (int)(((long)h * (long)s_TopMargin) / 100000L) - blockH / 2;
    else
        yStart = s_TopMargin;
    if (yStart < SWS_BTN_H + 2) yStart = SWS_BTN_H + 2;
    /* 2) 四列的 x。空列跳过, 所以列是连续排下去的。 */
    firstX = xStart;
    {
        int cx = xStart, first = 1;
        for (f = 0; f < 4; f++)
        {
            if (colCount[f] <= 0) continue;
            if (first) { firstX = cx; first = 0; }
            colX[f] = cx;
            cx += SWS_ICON_W + s_ColGap;
        }
    }
    s_LastX = 0;
    for (f = 0; f < 4; f++) if (colCount[f] > 0) s_LastX = colX[f];
    /* 3) 每条目在屏幕上的位置; 滚出可见范围的 x=-1 (不参与命中/绘制) */
    rows[0] = rows[1] = rows[2] = rows[3] = 0;
    for (i = 0; i < n; i++)
    {
        SwsItem* it = &s_Items[s_Order[i]];
        int r = rows[it->side] - s_Scroll;
        it->x = colX[it->side];
        it->y = yStart + r * rowH;
        rows[it->side]++;
        if (r < 0 || r >= maxRows) it->x = -1;
    }
    /* 4) 两个按钮 (在栏区判定框之外, 命中判定排在栏区判定之前) */
    SwsTwoBtnGeom(xStart, totW, w, yStart);
    barX = s_LastX + SWS_ICON_W + 1;
    /* 5) 悬停 (用刚落好的布局) + 提示框几何。提示框可能探出侧栏块之外
     *    (下面那行、或翻上去时上面那行), 所以必须先量出它, 缓存矩形四条边
     *    才够大。 */
    hover = SwsHitItem(mx, my);
    if (hover >= 0)
    {
        /* hover 是排序后下标 → 必须走 s_Order (见 SwsHitItem 的坐标系警告) */
        SwsItem* it = &s_Items[s_Order[hover]];
        if (SwsTipPrepare(it->name))
            SwsTipRect(w, h, it->x + SWS_ICON_W / 2, it->y + SWS_ICON_H,
                       &tipX, &tipY, &tipW, &tipH);
    }
    /* 6) 缓存矩形 = 按钮行 ∪ 超武块 ∪ 滚动条 ∪ 提示框 (四条边都算) */
    right = s_LastX + SWS_ICON_W;
    if (bar) right = barX + barW;
    if (s_BtnX + s_BtnW > right) right = s_BtnX + s_BtnW;
    if (tipW > 0 && tipX + tipW > right) right = tipX + tipW;
    if (right > w) right = w;
    bottom = yStart + blockH;
    if (tipW > 0 && tipY + tipH > bottom) bottom = tipY + tipH;
    if (bottom > h) bottom = h;
    if (tipW > 0 && tipX < firstX) firstX = tipX;
    if (s_HidX < firstX) firstX = s_HidX;
    if (firstX < 0) firstX = 0;
    /* 条目高亮框是画在 it->x-1 / it->y-1 上的 1px 描边, 而最后一行那条下边
     * 正好落在 yStart+blockH —— 都不在"块"的范围内, 所以左/下各再让 1px,
     * 否则最外圈会少一像素。 */
    if (firstX > 0) firstX--;
    if (bottom < h) bottom++;
    /* 提示框翻到图标上方时 (最后一行顶到屏幕底), 框顶同样在块外。 */
    top = s_BtnY;
    if (tipW > 0 && tipY < top) top = tipY;
    if (!SwsCacheAlloc(right - firstX, bottom - top)) goto nogeo;
    s_CchX = firstX; s_CchY = top;
    /* 7) 发布几何: 点击/滚轮/吞没门都读这几个, 全部对应屏上这一帧 */
    s_BoundX = xStart; s_BoundY = yStart; s_BoundW = totW; s_BoundH = blockH;
    s_BoundsKnown = 1;
    s_YStart = yStart; s_RowH = rowH; s_VisRows = maxRows;

    /* ---- 整块底框 (一次, 不是每列一次) ----
     * 图标不吃"局部不透明度"之外的东西, 逐列画 4 块底框和画 1 块整的
     * 视觉效果一样, 但少 3 次填充、少 3 份重叠边缘。 */
    ch = 0;
    for (f = 0; f < 4; f++)
        if (colCount[f] > 0)
        {
            int t = (colCount[f] < maxRows ? colCount[f] : maxRows) * rowH - s_Gap;
            if (t > ch) ch = t;
        }
    if (ch > 0)
        SwsCFillA(xStart, yStart, totW, ch, 0, 0, 0, SWS_BACKDROP_OP);

    /* 充能百分比走 GDI 槽 5, 字号就是 [SWSidebar] FontPx。**必须**显式
     * GdiEnsure —— 直接拿 g_G 画会借到上一个用过 GDI 的模块的槽 (通常是底电/
     * 超武面板), FontPx 形同虚设且字号随别的模块变。 */
    txtOk = GdiEnsure(s_FontPx, VecText_Bold(), 5, VecText_FontFace());

    /* ---- 图标 + 充能百分比 (高亮框在下面单独一遍) ---- */
    for (i = 0; i < n; i++)
    {
        SwsItem* it = &s_Items[s_Order[i]];
        int isHover = (i == hover);
        if (it->x < 0) continue;              /* 被滚动裁掉 */
        /* 无 SidebarPCX 的超武什么都不画: 要么在 rulesmo.ini 里补
         * SidebarPCX=, 要么在 [SWSidebar] 里给它负值隐藏。 */
        if (it->pcxFile)
        {
            void* ps = SwsPcxLiveSurf(it->pcxFile);
            if (ps)
            {
                SwsCPcx(it->x, it->y, SWS_ICON_W, SWS_ICON_H, ps, isHover);
                /* 未就绪 (释放后充能中): 压暗。只对这类条目做, 通常 0~1 个。
                 * 悬停的那一个不压 —— 玩家正在看它, 让提示框和白框说话。 */
                if (!it->ready && !isHover)
                    SwsCFillA(it->x, it->y, SWS_ICON_W, SWS_ICON_H,
                              0, 0, 0, 45);
            }
            /* 充能百分比: 画在图标顶部 (两趟都用同宽: 多画 2px 会切出字形
             * overhang 的碎片, 形似 '<'; 微软雅黑 extent 非矩形)。 */
            if (txtOk)
            {
                wchar_t st[8];
                int sl = 0, sw, sx, ny, adv;
                /* 必须清满整个数组: 只清前 4 个的话第 5 个是栈上垃圾, 非零时
                 * sl 会变长, RenderLine 就把 "100%" 后面的垃圾字节也画出来。 */
                st[0] = st[1] = st[2] = st[3] = 0;
                st[4] = st[5] = st[6] = st[7] = 0;
                /* 就绪 (100%) 不显示任何字: 图标本身就是"可用"的意思, 再叠
                 * 一层 100% 只是多一次 GDI 文本渲染, 还压住图标。 */
                if (it->susp)
                {
                    st[0] = 'H'; st[1] = 'o'; st[2] = 'l'; st[3] = 'd';
                }
                else if (!it->ready && it->total > 0)
                {
                    int pct = 100 - (it->remain * 100) / it->total;
                    if (pct < 0) pct = 0;
                    if (pct > 99) pct = 99;   /* 满格就是就绪, 上面分支处理 */
                    if (pct >= 10)            /* 按位数输出, 别让 1% 变 "001%" */
                    {
                        st[0] = (wchar_t)('0' + (pct / 10) % 10);
                        st[1] = (wchar_t)('0' + pct % 10);
                        st[2] = '%';
                    }
                    else
                    {
                        st[0] = (wchar_t)('0' + pct);
                        st[1] = '%';
                    }
                }
                while (sl < 7 && st[sl]) sl++;
                adv = g_G->adv > 0 ? g_G->adv : s_FontPx + 2;
                sw = sl ? RenderLine(st, sl) : 0;
                if (sw > 0)
                {
                    sx = it->x + (SWS_ICON_W - sw) / 2;
                    ny = it->y + 1;
                    if (sx < it->x) sx = it->x;
                    SwsCCov(g_G->bits, g_G->pitch, 4, sx + 1, ny + 1, sw, adv,
                            0, 0, 0, 80 * s_Alpha / 100);
                    SwsCCov(g_G->bits, g_G->pitch, 4, sx, ny, sw, adv,
                            255, 255, 255, 100 * s_Alpha / 100);
                }
            }
        }
    }

    /* ---- 每列边框 (画在图标之后, 否则会被 60x48 的图标盖住) ---- */
    for (f = 0; f < 4; f++)
    {
        int ch2;
        const unsigned char* c;
        if (colCount[f] <= 0) continue;
        c = kSideCol[f];
        ch2 = (colCount[f] < maxRows ? colCount[f] : maxRows) * rowH - s_Gap;
        SwsCFillA(colX[f], yStart, SWS_ICON_W, 1, c[0], c[1], c[2], 150);
        SwsCFillA(colX[f], yStart + ch2 - 1, SWS_ICON_W, 1, c[0], c[1], c[2], 150);
        SwsCFillA(colX[f], yStart, 1, ch2, c[0], c[1], c[2], 150);
        SwsCFillA(colX[f] + SWS_ICON_W - 1, yStart, 1, ch2, c[0], c[1], c[2], 150);
    }

    /* ---- 条目高亮框: 悬停=白, 当前选中=阵营色 ----
     * ⚠ 必须排在**每列边框之后**, 而且是独立一遍, 不能塞在图标循环里。
     * 高亮框画在 it->x-1 .. it->x+ICON_W 上, 而 ColGap=0 时这两列正好压在
     * 左右**邻列的外边框**上 (本列右边框在 x+59, 下一列左边框在 x+60)。
     * 边框后画就把高亮框的左右两边盖掉一列, 看上去正是"框缺了一截、位置
     * 错位"。放最后画, 整圈 62x50 的描边才完整。 */
    for (i = 0; i < n; i++)
    {
        SwsItem* it = &s_Items[s_Order[i]];
        const unsigned char* hc;
        if (it->x < 0) continue;              /* 被滚动裁掉 */
        if (i != hover && it->swIdx != curSel) continue;
        hc = (i == hover) ? (const unsigned char*)"\xFF\xFF\xFF"
                          : kSideCol[it->side];
        SwsCFillA(it->x - 1, it->y - 1, SWS_ICON_W + 2, 1, hc[0], hc[1], hc[2], 220);
        SwsCFillA(it->x - 1, it->y + SWS_ICON_H, SWS_ICON_W + 2, 1, hc[0], hc[1], hc[2], 220);
        SwsCFillA(it->x - 1, it->y, 1, SWS_ICON_H, hc[0], hc[1], hc[2], 220);
        SwsCFillA(it->x + SWS_ICON_W, it->y, 1, SWS_ICON_H, hc[0], hc[1], hc[2], 220);
    }

    /* ---- 滚动条: 参照战役框 (MOExperience.cpp DrawBox 战役分支) ----
     * 整块右侧一根: 4px 宽, 轨道 alpha 55 / 滑块 alpha 90, 压暗不抢眼。
     * 高度覆盖整个超武块, 滑块高度按最长列的条目数算。 */
    if (bar)
    {
        int barH, barY, thumbH, thumbY, total;
        barY = yStart + 2;
        barH = blockH - 4;
        total = maxRows + s_MaxSk;                /* = 最长列条目数 */
        if (barH > 12)
        {
            thumbH = barH * maxRows / total;
            if (thumbH < 8) thumbH = 8;
            if (thumbH > barH) thumbH = barH;
            thumbY = (s_MaxSk > 0)
                   ? barY + (barH - thumbH) * (s_MaxSk - s_Scroll) / s_MaxSk
                   : barY + barH - thumbH;
            SwsCFillA(barX, barY, barW, barH, 30, 30, 30, 55);
            SwsCFillA(barX, thumbY, barW, thumbH, 170, 170, 170, 90);
        }
    }

    /* ---- 显隐按钮 ▶ (收起) + 隐藏重复按钮 (禁入牌), 紧挨在显隐按钮**左边**。
     * 箭头任何时候都不能藏, 否则藏起来之后就找不到再展开的地方了。 */
    SwsDrawArrow(s_BtnX, s_BtnY, s_BtnW, s_BtnH, 1, btnHov == 1);
    SwsDrawDedup(s_HidX, s_HidY, s_BtnW, s_BtnH, s_DedupHide, btnHov == 2);

    /* ---- 悬停名字 (带底框, 跟随全局矢量字体, 挂在图标**下面**) ----
     * 隐藏重复按钮**不**出文字提示: 禁入牌 + 颜色已经说明状态, 而自己拼的
     * 中文字面量一旦字形链路有问题就是一片 '?'。 */
    if (hover >= 0)
    {
        SwsItem* it = &s_Items[s_Order[hover]];
        SwsDrawTip(w, h, it->x + SWS_ICON_W / 2, it->y + SWS_ICON_H);
    }

    s_CchOK = 1;
    s_CchSig = sig; s_CchFrame = fr;
    s_CchHover = hover; s_CchBtn = btnHov; s_CchSel = curSel;
    return;

nogeo:
    /* 一个超武都没有 / 缓存分不出来: 整块不画, 两个按钮也就没画出来。
     * s_HidValid 必须一起清掉, 否则 SWS_Poll 会拿上一帧的旧几何把点击判到
     * "隐藏重复" 上。
     * 指纹/帧号**照样记上**: 画不出来时 SWS_Draw 下一帧还会再进来, 不记就变成
     * 每帧重试一次 (分配失败时还会每帧记一行日志)。记上之后退回 15 帧节流。 */
    s_CchOK = 0;
    s_BoundsKnown = 0;
    s_HidValid = 0;
    s_CchSig = sig; s_CchFrame = fr;
    s_CchHover = hover; s_CchBtn = btnHov; s_CchSel = curSel;
}

void SWS_Draw(void* surf, int w, int h, int pitch)
{
    int mx = -1, my = -1;
    int fr, hover = -1, btnHov, curSel = -9999, age;
    unsigned sig;
    unsigned char* vt;
    unsigned char* locked;
    SwsLockFn lockFn;
    SwsUnlockFn unlockFn;

    SwsEnsureInit();
    if (!s_Enable) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    /* 位深是 SwsPack/SwsOver 的前提 (565 还是 555), 必须先定下来。
     * MessageDrawHook 本来就调过一次, 这里只是不依赖那个调用顺序。 */
    if (!g_Calibrated) Calibrate();
    GtMouseGameXY(&mx, &my);
    fr = *(int*)ADDR_FRAME;
    /* 悬停态用**屏上这一帧**的布局算 (也就是上次重建发布的那套), 所以
     * "鼠标移到一个新图标上"这一下会立刻触发重建, 不会等节流。 */
    btnHov = SwsHitBtn(mx, my);
    if (s_Visible && s_CchOK)
    {
        hover = SwsHitItem(mx, my);
        if (IsPlausiblePtr((void*)ADDR_CURSW)) curSel = *(int*)ADDR_CURSW;
    }
    sig = SwsSig(w, h, pitch);
    age = fr - s_CchFrame;
    if (fr < s_CchFrame) age = SWS_REBUILD_FRAMES;   /* 帧号回拨: 新开局/读档 */
    if (!s_CchOK || sig != s_CchSig || hover != s_CchHover ||
        btnHov != s_CchBtn || curSel != s_CchSel ||
        age >= SWS_REBUILD_FRAMES)
    {
        if (s_Visible) SwsRebuild(w, h, mx, my, curSel, sig, fr, btnHov);
        else           SwsRebuildCollapsed(w, h, sig, fr, btnHov);
    }
    if (!s_CchOK) return;
    /* 每帧唯一的一次锁面 + 一次带 alpha 的拷贝 */
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    lockFn = (SwsLockFn)(*(void**)(vt + VT_LOCK));
    unlockFn = (SwsUnlockFn)(*(void**)(vt + VT_UNLOCK));
    if (!lockFn || !unlockFn ||
        !IsPlausiblePtr((const void*)lockFn) ||
        !IsPlausiblePtr((const void*)unlockFn)) return;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return;
    SwsCacheBlit(locked, pitch, w, h);
    unlockFn(surf, NULL);
}
