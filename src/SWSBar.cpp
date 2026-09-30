/* SWSBar.cpp - 超武侧栏 (Phobos SuperWeaponSidebar 移植简化版)。
 *
 * Phobos 原理 (见 notes/超武侧栏.md, references/Phobos/src/Ext/Sidebar/SWSidebar):
 *   Gadget 按钮列 (SWColumnClass/SWButtonClass) + 开关掣, 按
 *   SuperWeaponSidebar.Allow/Significance/RequiredHouses 过滤,
 *   按 PriorityHouses + BuildType::SortsBefore 排序, 左键打靶/右键取消,
 *   背景全套 PCX (On/Off/Top/Center/Bottom), 左侧停靠。
 * 本移植只取最小可用 (Ares 不动, 不建 Gadget, 不碰模拟):
 *   - 数据: 只读扫 SuperClass::Array (Owner==本家 && IsPresent),
 *     纯本地过滤+排序, 不写任何引擎/Gadget 状态 (联机安全)。
 *   - 分组: 右侧 4 列 (0=盟军 1=苏联 2=厄普西隆 3=焚风), 空列隐藏;
 *     归属先查内建表 (rulesmo.ini 离线推导: 授予建筑 Owner/Prerequisite
 *     链, 96 个), 查不到落玩家阵营列 (PlayerSideIndex, 未知回 0 列)。
 *   - 排序/隐藏: 单节 [SWSidebar] 内以超武 ID 为键、整数为值:
 *     负数=隐藏, 0=默认显示 (优先级 1000), 正数=优先级 (越小越靠前);
 *     同优先级按 ArrayIndex。缺键=默认显示。
 *     (超武与支援技能同注册类型, 不区分, IsPresent 即显示。)
 *   - 绘制: 图标盒 (60x48, Phobos cameo 同尺寸) + 阵营色边框 +
 *     名/状态两行 GDI 字 + 底部进度条, 整体透明度 Opacity 可调;
 *     不需要 Phobos 那套 PCX UI 资源。
 *   - 图标: MO 的超武图标是 PCX (rulesmo.ini 里 SidebarPCX=xxx.pcx, 打包在
 *     MIX 里), 不是 SHP —— SidebarImage 指向引擎的 XXICON.SHP 占位引用,
 *     画出来是 11 个一样的紫球。走引擎现成设施:
 *     INI_Rules(0x887048)->ReadString(0x528A10) 读 SidebarPCX=,
 *     PCX::Instance(0xAC4848, this 即该地址) GetSurface(0x6BA140) /
 *     ForceLoadFile(0x6B9D00), BlitToSurface(0x6BA580, 色键
 *     COLOR_PURPLE=0xF81F) 贴到 SWS_Draw 收到的 surf 上。
 *     三条命门: GetSurface 未加载时给的是占位图(必须先 LoadFile);
 *     BSurface* 不可跨帧缓存(切页/重载美术会失效); 目标面不能用硬编码
 *     0x88731C (本机 Ares 构建该处虚表为 NULL, 调用即跳 0 崩溃)。
 *     无 PCX 时才回退 SHP (跟随 Data / GetData, 见笔记)。
 *   - 交互: ToggleKey 切换显隐 (隐藏即不占空间: 不画+不吞点击);
 *     左键图标=选中打靶 (只写 Display 侧 UI 态 CurrentSWTypeIndex,
 *     与 Phobos targeting 分支同义, 非模拟), 右键栏内=取消 (-1);
 *     点击吞没经 0x692419 门 (Phobos SkipOnNewButtons 同点同去向),
 *     防误下单。Action==None 的直发类 (空降/空投等) 本版也只进打靶
 *     ( limitation, 见笔记)。
 *   - 防御支援页去重 (DedupDefensePage): 侧栏有的超武不再在游戏自带的
 *     防御页 (页签 1) 里重复出现。钩 SidebarClass::AddCameo 入口
 *     0x6A6316 拦掉新增 (Phobos 同点), 已经进去的则直接改 Ares 的
 *     RulesExt::TabCameos[1] 搬出来 (引擎自己只删 IsPresent==0 的,
 *     而且 Ares 把容器换成了 Ares.dll 里的 DynamicVectorClass)。
 *     显隐箭头左边有个"禁入牌"按钮在两者之间切换。整段细节见
 *     notes/超武侧栏.md "隐藏防御支援页里的重复超武"。
 *
 * 地址 (YRpp 交叉验证, 全部只读, 首帧探针日志自证):
 *   SuperClass::Array 0xA83CB8 (Items+0/Count+0xC, 同 BuildQueue 厂表惯例);
 *   SuperWeaponTypeClass::Array 0xA8E330 (同上);
 *   Super: Type+0x1C Owner+0x20 RechargeTimer+0x24(Start)/+0x2C(Left)
 *     IsPresent+0x61 IsReady+0x63 IsSuspended+0x64 (头文件逐字节累加);
 *   SWType: ID+0x18 UIName+0x54 ArrayIndex+0x8C RechargeTime+0xA4;
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
/* 容器布局 (YRpp ArrayClasses.h 实证, 含虚表):
 *   +0 vtable / +4 Items / +8 Capacity / +12 IsInit+IsAlloc /
 *   +16 Count / +20 CapInc。
 * 注意: 旧代码的 ADDR_FACARR_ITEMS 这类宏给的已是字段地址
 * (对象基址+4), 不是对象基址, 抄时别弄混。 */
#define ARR_ITEMS(base) (*(void***)((base) + 4u))
#define ARR_COUNT(base) (*(int*)((base) + 16u))
#define ADDR_FRAME       0xA8ED84u
#define ADDR_SCENARIO    0xA8B230u
#define OFF_SCEN_SIDE    0x34B8u
#define ADDR_CURSW       0x8809A0u   /* Display(0x87F7E8)+0x11B8 */

#define OFF_SUP_TYPE     0x28u   /* win 实测: +0x28 T405 全中 (头文件值+12) */
#define OFF_SUP_OWNER    0x2Cu   /* win 实测: +0x2C H405 全中 */
#define OFF_SUP_RTSTART  0x30u
#define OFF_SUP_RTLEFT   0x38u
#define OFF_SUP_PRESENT  0x6Du
#define OFF_SUP_READY    0x6Fu
#define OFF_SUP_SUSPEND  0x70u

#define OFF_SWT_ID       0x24u   /* 与 Super 同源 +12 漂移, 待 id 计数验证 */
#define OFF_SWT_UINAME   0x60u
#define OFF_SWT_ARRIDX   0x98u
#define OFF_SWT_RECH     0xB0u
#define OFF_SWT_SHPIMG   0xB8u   /* SidebarImage (SHP), +12 假设, shp 计数验证 */

/* 图标绘制 (Phobos SWButton::Draw 同参, 只读渲染, 联机安全):
 * CC_Draw_Shape(surf, pal, shp, 0, pos, bounds,
 *   bf_400, 0,0,Ground, 1000, 0, NULL,0,0,0)。
 * 注意: YRpp Surface.h 里 CC_Draw_Shape 是 __fastcall 自由函数
 * (Surface->ECX, Palette->EDX, 其余压栈)。早前误写成 __stdcall 会让
 * 16 个参数全部压栈、ECX 留垃圾, blitter 首次真正被调用即崩溃。 */
typedef void (__fastcall *SwsDrawShapeFn)(void* surf, void* pal, void* shp,
    int frame, const void* pos, const void* bounds, int flags, int remap,
    int zadj, int zgrad, int bright, int tint, void* zshp, int zframe,
    int xoff, int yoff);


#define VT_LOCK            0x5C
#define VT_UNLOCK          0x60

#define ADDR_CLICKGATE_SKIP 0x006925FCu

#define SWS_CAP 160
#define SWS_INI_CAP 160

/* ---------------- 配置 ---------------- */

static int s_Init = 0;
static int s_Enable = 1;
static int s_Opacity = 60;
static int s_Alpha = 100;   /* 侧栏整体透明度 0~100 */
/* 图标盒固定 60x48 (Phobos 原版 cameo 尺寸)。曾做成 IconW/IconH 配置项,
 * 但 PCX 是固定尺寸源资源, 缩放后目标矩形只按屏幕边界裁剪、不按图标盒裁剪,
 * 非默认尺寸会溢出格子与底框错位 —— 故去掉, 直接定常量。 */
#define SWS_ICON_W 60
#define SWS_ICON_H 48
static int s_Gap = 2, s_ColGap = 6;
static int s_RightMargin = 2;
/* 纵向定位: 一个键同时表达"按屏幕比例"和"按像素"。
 *   0 ~ 1  = 比例, 0.5 表示屏幕高的 50% 处对齐 (适合分辨率无关)
 *   > 1    = 像素 Y 坐标, 80 表示距顶 80px (适合精确微调)
 * 范围取 0~1 而不是 0~100, 是为了和 ini 里其它"小数量级"参数 (Opacity 之类
 * 都归一到 0~1 再换算) 保持一致的观感; >1 就明确是像素, 不会有歧义。
 * 旧的 CenterY(0/1) + TopMargin(px) 两个键已合并成 TopMargin。 */
static int s_TopIsPct = 0;      /* 1 = s_TopMargin 是比例 */
static int s_TopMargin = 50000;  /* 比例时是 x100000 (0.5 -> 50000), 像素时是 px */
static int s_FontPx = 12;
static OU_Hotkey s_ToggleHk;
static int s_Visible = 1;
static int s_PrevTog = 0, s_PrevLB = 0, s_PrevRB = 0;
/* 待上膛的超武, **等左键松开才写**。
 *
 * 为什么不按 Phobos 那样"按下就写": Phobos 的 SWButtonClass 是真 Gadget,
 * 构造时带 (GadgetFlag::LeftPress|GadgetFlag::RightPress), 引擎的
 * GadgetClass::Action 消费这次点击时会顺手清掉 MouseClass 的按键态, 所以
 * DisplayClass::ProcessClickCoords 根本看不到"左键按住"这件事。
 *
 * 我们是纯覆盖层+轮询, 引擎照常看到左键按住。而引擎放置超武的判据是
 * 0x692B06 读 Display+0x11B8 (CurrentSWTypeIndex): 一旦非 -1 且按键按住,
 * **当帧就在鼠标处释放** (实测日志: armed 紧跟着引擎自己发的
 * SpecialPlace, cell=236,141, IsExecuted=0x5A, 然后 sws: FIRED)。
 * 延一帧写不足以躲开 —— 那一帧按键还按着。
 *
 * 所以改成"按下只记录, 松开才写": 整段按住期间由 s_HoldSkip 吞掉引擎的
 * 地图点击, 等价于 Gadget 消费掉这次完整点击; 真正写 CurrentSWTypeIndex
 * 时按键已经松开, 引擎的"按住即释放"条件不可能成立。 */
static int s_PendSel = -2;   /* -2 none, -1 cancel, >=0 SW ArrayIndex */
static int s_SelDown = 0;    /* 选中的那一下左键是否还按着 (等松开才上膛) */
static int s_ArmIdx = -1;    /* s_SelDown 期间记住的 swIdx */
/* 注意: 曾有一个 s_EatPress ("吞掉这一帧"), 但它在同一次 SWS_Poll 调用里
 * 置 1 又清零, 门根本读不到 —— 纯死代码, 整段按住由 s_HoldSkip 负责。 */
/* 物理左键按住期间一直吞掉引擎的地图点击。
 * 关键: 引擎判断的是"左键**按住**"而不是边沿, 所以只要"已选中 + 按住"
 * 同时成立就会当场释放。按下即吞, 一直吞到松开, 效果等同真 Gadget
 * 消费掉这次完整点击。 */
static int s_HoldSkip = 0;
/* 吞没门用的"栏区已知"标志。
 * **不能**复用 s_BoundValid: 那个标志 SwsScan() 开头就清零 (只在重建条目
 * 期间有效), 而 SWS_Draw 跑到布局末尾才置 1。点击那帧 SWS_Poll 先跑
 * (还会 s_ForceScan=1 强制重扫), 门随后才跑, 于是正好撞上 s_BoundValid==0
 * 分支被放行 —— 表现就是"点了没反应/要点很多次"。栏区几何 (s_BoundX/Y/W/H)
 * 在重扫期间并没有变, 所以这里另用一个只由 Draw 置位、隐藏时才清零的标志。 */
static int s_BoundsKnown = 0;

/* 即时超武: 走 SpecialPlace 事件 (定义在下方)。 */
static int SwsPushSpecialPlace(int swIdx);
/* 全部在 SwsEnsureInit 里读一次。
 * 铁律: 每帧路径上**不允许**出现 GetPrivateProfileIntA —— 那等于每帧一次
 * ini 文件 I/O, 而且落在 perf 计时之外, 帧率掉了日志却看不出来。 */static int s_ShowDrain = 0;   /* [SWSidebar] DrainSupers: 1=显示火风暴类 (默认 0=过滤掉) */
static int s_ForceScan = 0;     /* 点击时置 1: 强制立刻重扫, 不等 15 帧 */
static int s_ScrollStep = 1;    /* [SWSidebar] ScrollStep */

/* ---------------- 隐藏"防御支援页"里的重复超武 (见文件头"重复超武隐藏") ----
 * 目标态 s_DedupHide (按钮切换 / [SWSidebar] HideDup 初值) 与已落到防御页的
 * 实际态 s_DedupDone 分开存: 两者不同、或 s_DedupDirty 被置 1 时, 轮询里补做
 * 一次"删"或"补"。 */
static int s_DedupHide = 1;     /* 1 = 隐藏重复, 0 = 原样 */
static int s_DedupDone = 0;     /* 已经落到防御页的实际状态 */
static int s_DedupDirty = 0;    /* 需要 (重新) 应用一次 */
static int s_HidX = 0, s_HidY = 0;   /* 隐藏重复按钮几何 (绘制期发布) */
static int s_HidValid = 0;   /* 该按钮这一帧是否真的画出来了 (SWS_Draw 早退时要清) */
static unsigned char* s_TabCam = NULL;  /* Ares RulesExt::TabCameos[4] 基址 */

/* 引擎事件队列 (OutList): SwsPushSpecialPlace 追加 SpecialPlace 事件用。 */
#define SWS_ADDR_OUT_COUNT  0xA802C8u
#define SWS_ADDR_OUT_NEXT   0xA802D0u
#define SWS_ADDR_OUT_BUF    0xA802D4u
#define SWS_ADDR_OUT_PAR    0xA83A54u
#define SWS_EVT_SIZE        111u
#define SWS_EVT_CAP         0x80u
#define SWS_EVT_SLOTS_N     128u   /* 环形容量; 槽位下标掩码 = SWS_EVT_SLOTS_N-1 = 0x7F */


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
    void* shp;
    const char* pcxFile;  /* SidebarPCX= 文件名, 指向我们自己的表 */
    int instant;          /* 1 = 点击即生效 (Action=None 或 SW.UseAITargeting) */
    int row;              /* 该条目在本列的行号 (滚动用) */
    char id[32];          /* 超武 ID (诊断表用: 名字<->swIdx 对照) */
    wchar_t name[24];     /* 悬停提示用; 每类型填一次 */
} SwsItem;

static SwsItem s_Items[SWS_CAP];
static int s_ItemN = 0;
static wchar_t s_ItemNameBuf[24];   /* 从类型缓存带出来的悬停名字 */

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
static int s_BtnX = 0, s_BtnY = 0, s_BtnW = 0, s_BtnH = 0;  /* 隐藏按钮 */
/* SWS_Draw 算好的布局, 供点击/滚动判定使用 */
static int s_ColCount[4] = { 0, 0, 0, 0 };
static int s_ColX[4] = { 0, 0, 0, 0 };
static int s_YStart = 0, s_RowH = 50, s_VisRows = 0;
/* 每列滚动几何, SWS_Draw 布局后发布, 供 SWS_WheelIn 判定 */
typedef struct { int count, vis, x; } SwsColGeom;
static SwsColGeom s_Cols[4];
static int s_NCols = 0, s_LastX = 0, s_ColsValid = 0;
static int s_BarX = 0, s_BarW = 0, s_BarValid = 0, s_MaxSk = 0;

static int s_BoundValid = 0;
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
            SwsStrEq(k, "Enable") || SwsStrEq(k, "Opacity") ||
            SwsStrEq(k, "ToggleKey") ||
            SwsStrEq(k, "Gap") || SwsStrEq(k, "ColGap") ||
            SwsStrEq(k, "RightMargin") ||
            SwsStrEq(k, "TopMargin") || SwsStrEq(k, "Alpha") ||
            SwsStrEq(k, "FontPx") ||
            SwsStrEq(k, "MaxRows") ||
            SwsStrEq(k, "ScrollStep") || SwsStrEq(k, "DrainSupers") ||
            SwsStrEq(k, "DedupDefensePage");
        /* 漏一个全局键的后果: 它会被当成"超武 ID"收进 s_Ini, 显示成一个
         * 永远不亮的空条目。新增配置项时**必须**同时加进这一串 —— 而且要
         * 和上面 SwsEnsureInit 里真正读取的那份保持一致 (曾经两边不一致,
         * 留下一堆早已删除的键, 害得排查时以为它们还生效)。 */
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
    s_Opacity = GetPrivateProfileIntA(SWS_SECTION, "Opacity", 60, g_IniPath);
    s_Gap = GetPrivateProfileIntA(SWS_SECTION, "Gap", 2, g_IniPath);
    s_ColGap = GetPrivateProfileIntA(SWS_SECTION, "ColGap", 0, g_IniPath);
    s_RightMargin = GetPrivateProfileIntA(SWS_SECTION, "RightMargin", 2, g_IniPath);
    /* TopMargin 一个键两用: 0~1 = 比例(屏幕高), >1 = 像素。
     * 默认 0.5 = 垂直居中 (等价于旧的 CenterY=1)。
     * 全整数定点, 不用浮点: 读成 x100000 的定点数 (0.5 -> 50000, 0.25 -> 25000)。 */
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
            /* 判定和缩放必须**分开**:
             *   先由字面值(含小数)决定语义 —— 0~1 算比例, >1 算像素;
             *   再按语义做换算 —— 比例存成 x100000 定点, 像素原样存。
             * 早先无条件乘 100000 再判, 结果 "80" 变成 1000000 像素,
             * 侧栏直接被顶出屏幕。
             * 用整数运算凑: 先把整串看成"万分之一单位"(fp4), 判定用它的原值,
             * 需要时再整体放大。 */
            {
                /* fp4 = 原值 * 10000 (纯整数, 避免 long 溢出与浮点) */
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
    /* 整体透明度: 0=完全透明, 100=不透明。作用于侧栏画出的**每一个**像素
     * (底框/图标/边框/滚动条/箭头/文字), 不是只调某一个元素。 */
    s_Alpha = GetPrivateProfileIntA(SWS_SECTION, "Alpha", 100, g_IniPath);
    if (s_Alpha < 0) s_Alpha = 0;
    if (s_Alpha > 100) s_Alpha = 100;
    /* 充能百分比字号。悬停名走全局 [VecText] 的字体设置, 不在此处配。 */
    s_FontPx = GetPrivateProfileIntA(SWS_SECTION, "FontPx", 12, g_IniPath);
    s_MaxRows = GetPrivateProfileIntA(SWS_SECTION, "MaxRows", 10, g_IniPath);
    /* 诊断开关只读一次: 这两个在 SWS_Poll 每帧调用, 现读 ini 就是每帧一次
     * 文件 I/O, 而且在 perf 计时范围之外, 会让帧率掉了日志却看不出来。 */
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
    if (s_Opacity < 0) s_Opacity = 0;
    if (s_Opacity > 100) s_Opacity = 100;
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
    p = AppendCStr(buf, p, " op=", sizeof(buf));
    p += UtoA((unsigned)s_Opacity, buf + p);
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

static void* s_RawCur = NULL;
static void* s_RawSupItems = NULL;
static int s_RawSupCount = 0;
static void* s_RawHouseItems = NULL;
static int s_RawHouseCount = 0;
static int s_RawInit = 0;

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
 * 调研结论 (YRpp/Phobos):
 *  - SuperWeaponTypeClass::SidebarImage 在 +0xB8 (zero_E4 字段名自证)。
 *    它通常是 Type==0xFFFF 的 SHPReference; Phobos SWButtonClass.cpp:46-64
 *    是把引用"原样"交给 DrawSHP 的, 不跟 Data、不调 GetData()。
 *    引用指向 XXICON.SHP (GameStrings @0x8204FC) 时就是引擎的占位图(紫球)。
 *  - MO 的超武图标是 PCX: rulesmo.ini 里 96 节用 SidebarPCX=xxx.pcx,
 *    磁盘上没有, 全在 MIX 里。所以:
 *      读键: CCINIClass::INI_Rules (0x887048, 游戏已加载好的 rules 对象,
 *            走 MIX) -> INIClass::ReadString (0x528A10, __thiscall)
 *      载图: PCX::Instance (0xAC4848) -> ForceLoadFile (0x6B9D00)
 *            -> GetSurface (0x6BA140)   [引擎自带 PCX 解码, 16bpp]
 *      贴图: PCX::BlitToSurface (0x6BA580, 色键 COLOR_PURPLE)
 *    即 Phobos 用的正是这套 (Utilities/Constructs.cpp:120-169), 我们不自己解 PCX。 */

/* 读 rules 键: 用游戏现成的 ini 对象 CCINIClass::INI_Rules (0x887048) +
 * INIClass::ReadString。Ares 3.0 在 0x528A10 上挂了 INIClass_GetString
 * (inline 5B JMP), 但那是 drop-in 替换 —— Ares 自己读 SidebarPCX= 也走
 * 这一个对象同一个函数, 语义一致。 */
typedef int (__thiscall *SwsIniReadStringFn)(void* ini, const char* section,
    const char* key, const char* def, char* buf, unsigned size);
#define FN_INIREADSTR ((SwsIniReadStringFn)(void*)0x528A10u)
#define ADDR_INI_RULES 0x887048u

typedef void* (__thiscall *SwsPcxGetSurfFn)(void* pcx, const char* name, void* pal);
#define FN_PCXGETSURF ((SwsPcxGetSurfFn)(void*)0x6BA140u)
typedef int (__thiscall *SwsPcxLoadFn)(void* pcx, const char* name, int f1, int f2);
#define FN_PCXLOAD ((SwsPcxLoadFn)(void*)0x6B9D00u)
typedef int (__thiscall *SwsPcxBlitFn)(void* pcx, void* rect, void* dstSurf,
    void* pcxSurf, unsigned short transparent);
#define FN_PCXBLIT ((SwsPcxBlitFn)(void*)0x6BA580u)
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
 * 这三项以前每帧每条目都要重算: SwsArrayIndexOf 线性扫 135 项类型数组做
 * 字符串比较, SwsSideOf/SwsPrioOf 再各扫一张表。40 个条目就是每帧几千次
 * 字符串比较 —— 侧栏一开帧率就掉。类型对象是静态的, 按 type 指针缓存
 * 一次即可, 之后每帧只剩指针比较。 */
#define SWS_TCACHE 320
typedef struct {
    void* type;
    int   swIdx;
    int   side;      /* -2 = 未知(要用玩家阵营) */
    int   prio;
    int   hide;
    int   manual;
    int   instant;      /* 1 = 点击即发射 (Action=None 或 SW.UseAITargeting) */
    const char* pcxFile;
    /* 悬停提示文本, 属于**类型**的固有属性。
     * 早先它被写进 s_Items[n].name, 而 n 是本次扫描的条目下标 ——
     * 建筑一变, 条目顺序就变, 那个槽会属于别的超武, 而名字又只在
     * 首次填充时写一次, 于是悬停提示张冠李戴。 */
    wchar_t name[24];
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
    s_TC[i].prio = 1000;
    s_TC[i].hide = 0;
    s_TC[i].manual = 1;
    s_TC[i].instant = 0;
    s_TC[i].pcxFile = NULL;
    s_TC[i].name[0] = 0;
    s_TC[i].prio = -1;              /* -1 = 尚未填充 */
    s_TC[i].side = -2;
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
 * Ares 侧栏点击钩子 (src/Ext/SWType/Hooks.cpp, SidebarClass_ProcessClickCoords
 * @0x6AAEDF) 的原始判据是:
 *
 *   if(pSuper->Type->Action == Action::None || pData->SW_UseAITargeting) {
 *       R->EAX(HouseClass::CurrentPlayer);
 *       return 0x6AAF10;   // 不用玩家选格, 当场发射
 *   }
 *
 * 而 `Action=Custom` 在 Ares 的 SWType/Body.cpp 里被改写成
 * `Actions::SuperWeaponAllowed` (=0x7F), **不是** None:
 *
 *   if(exINI.ReadString(section,"Action") && !_strcmpi(exINI.value(),"Custom"))
 *       pThis->Action = Actions::SuperWeaponAllowed;
 *
 * MO 的六个超武 (Blasticade/PsychicFlash/HunterSeeker/EliteReserves/
 * Nanocharge/GearChange) 全部是 `Action=Custom`, 所以按 `Action==0` 判
 * "点击即生效" 是判错对象的。
 *
 * 关键: 这两个判据**都能从 rules INI 直接读到**, 根本不用去猜
 * SuperWeaponTypeClass 的字段偏移:
 *   - `Action=`          写 None 才是真·无需选格; 写 Custom 不是
 *   - `SW.UseAITargeting=yes` 手册明说: "人类玩家点击图标时自动发射超武,
 *                         而不需要手动选择目标"
 * MO 里只有 EliteReservesSpecial 满足 (`SW.UseAITargeting=yes`)。
 *
 * 另外长期生效类 (UseChargeDrain=yes: 爆裂屏障/心灵之火) 只能读到 drain 标志,
 * 用来把它们从侧栏过滤掉 (原因见扫描处那段注释)。
 */
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
        /* Type=<Ares NewSWType>: 决定这条超武走哪个 Ares 实现。逐个查过
         * ares(Antares)/src/Misc/SWTypes 下逐个 .cpp, 只有这几个把
         * `pSW->Action = Action::None` 写进了 SuperWeaponTypeClass:
         *   HunterSeeker  -> MO 的 工厂停产维护 / 纳米回复场 / 猎杀机器人
         *   Battery       -> MO 的 心灵之火 (PsychicFlash)
         *   EMPulse/Reveal/SonarPulse 是**条件**赋值 (看 ini 参数), 不能算
         * Firestorm (爆裂屏障) **没有**这个赋值, 保持 SuperWeaponAllowed,
         * 所以它确实需要玩家选格 -> 点两次才是对的。
         * 这条判据和"运行时读 type+0xBC 的 Action==0"是同一件事的两个来源,
         * 两个都判, 谁先到算谁; 目的是别再漏掉 Battery 这类。 */
        typ[0] = 0;
        FN_INIREADSTR(rules, id, "Type", "", typ, (unsigned)sizeof(typ));
        if (SwsStrEq(typ, "HunterSeeker") || SwsStrEq(typ, "Battery"))
            s_Beh[slot].instant = 1;
        /* 长期生效类: 点击是开关 (Ready<->Draining), 同样**一次点击**就完成,
         * 不需要选格 —— Ares 的 SW_Firewall::Activate 收下 cell 参数却根本
         * 不用, 直接置 house 的 firestorm 状态。所以这类也要走立即发射。 */
        if (SwsIniYes(rules, id, "UseChargeDrain"))
        {
            s_Beh[slot].drain = 1;
            s_Beh[slot].instant = 1;
        }
    }
    return &s_Beh[slot];
}

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


/* 现取 surface: GetSurface 命中缓存直接用, 未命中则 LoadFile 再取。
 * 0x6B9D00 / 0x6BA140 / 0x6BA580 与 0xAC4848 均未被 Ares hook。
 * 返回前把尺寸/位深打出来: 占位面通常尺寸离谱 (1x1 / 0)。 */
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

/* SHP 兜底: 跟随 SHPReference::Data (+0x0C) 并按需 GetData() 懒加载。
 * 注意: Phobos 是把引用原样交给 DrawSHP 的, 但那是 YRpp 基线二进制;
 * 本机是 Ares 3.0, 传裸引用进 CC_Draw_Shape 已实测崩 (18:50 转储),
 * 所以这里坚持交出真正带像素的 SHPFile。 */
typedef void* (__fastcall *SwsGetDataFn)(void* self);
#define FN_SHPGETDATA ((SwsGetDataFn)(void*)0x69E580u)

static void* SwsShpOk(void* type)
{
    void* shp, *data;
    if (!type || !IsPlausiblePtr(type)) return NULL;
    shp = *(void**)((char*)type + OFF_SWT_SHPIMG);
    if (!shp || !IsPlausiblePtr(shp)) return NULL;
    if (*(unsigned short*)shp != 0xFFFFu) return shp;
    data = *(void**)((char*)shp + 0x0Cu);
    if (data && IsPlausiblePtr(data)) return data;
    data = FN_SHPGETDATA(shp);
    if (data && IsPlausiblePtr(data)) return data;
    return NULL;
}

/* 首次遇到这个超武类型时把它算一遍 (阵营/优先级/隐藏/ManualFire/图标/悬停名/
 * 即时类)。类型对象是静态的, 所以只算一次 —— 抽成独立函数是因为
 * "这个超武在侧栏上显不显示" 还要被防御页去重 (SwsDedupRemove /
 * SwsDedupRestore) 问, 而那两条路不一定先跑过一次 SwsScan。
 * 填完 tc->prio 就成了"已填"标志 (SwsTypeEntry 用 -1 表示未填)。 */
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
        /* 悬停提示文本: 优先 UIName, 空则退回 ID。
         * 存进**类型缓存**而不是 s_Items[n] —— 条目下标会随
         * 建筑增减重排, 存那里必然错位。 */
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
        if (!tc->pcxFile)
        {
            char sb[128];
            unsigned q2 = AppendCStr(sb, 0, "sws: nopcx ", sizeof(sb));
            q2 = AppendCStr(sb, q2, id, sizeof(sb));
            LogLine(sb);
        }
    }
    /* "点击即生效" 的判据取自 rules INI, 不再用 `Action==0` —— 那个判据是错的
     * (见 SwsBehaviorOf 注释): Action=Custom 会被 Ares 改写成 0x7F, 而 MO 这几个
     * 全是 Custom。真正的判据是 `Action=None` 或 `SW.UseAITargeting`。 */
    bh = SwsBehaviorOf(id);
    tc->instant = bh ? bh->instant : 0;
    /* 火风暴类长期生效超武 (UseChargeDrain=yes: 爆裂屏障/心灵之火)
     * **不显示也不处理** —— 决定放弃了。
     *
     * 为什么放弃 (结论已查实, 见 notes/超武侧栏.md):
     *   Ares 的 ChargeDrain 开关状态机只接在 SuperClass::ClickFire,
     *   而 SpecialPlace 事件走的是 Fire_SW -> Launch -> Activate,
     *   两条入口不相交 —— 事件能把效果打出去, 但 ChargeDrainState
     *   永远停在 Ready, 侧栏显示不出 "生效中"。Phobos 也没接这条
     *   (它的 LaunchSuper 与 Ares 逐条同构), 没有现成实现可抄。
     *   要真修就得写引擎的 ChargeDrainState, 而那需要
     *   HouseClass::Supers 的偏移; 从 YRpp 头推导时出现 40 处尺寸
     *   未知成员, 推导值与先前估值差 0x204, 不可信 —— 拿猜的偏移
     *   写引擎结构迟早崩游戏。
     *   代价远大于收益 (两个超武), 所以过滤掉。
     * [SWSidebar] DrainSupers=1 可以临时放回来试。 */
    if (bh && bh->drain && !s_ShowDrain)
    {
        static int s_DrainHidden = 0;
        tc->hide = 1;
        if (s_DrainHidden < 16)
        {
            char sb[96];
            unsigned q2 = AppendCStr(sb, 0, "sws: hide drain ", sizeof(sb));
            q2 = AppendCStr(sb, q2, id, sizeof(sb));
            LogLine(sb);
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
    int pside;
    s_ItemN = 0;
    s_BoundValid = 0;
    /* 以下全是固定地址字读取, 不碰数组内容, 任何状态下安全。
     * 五元组变化即记一行 (诊断卡点用), 先记再过门。 */
    cur = *(void**)ADDR_CURPLAYER;
    items = IsPlausiblePtr((void*)ADDR_SUPER_ARR) ?
        ARR_ITEMS(ADDR_SUPER_ARR) : NULL;
    count = IsPlausiblePtr((void*)ADDR_SUPER_ARR) ?
        ARR_COUNT(ADDR_SUPER_ARR) : 0;
    hItems = IsPlausiblePtr((void*)ADDR_HOUSE_ARR) ?
        ARR_ITEMS(ADDR_HOUSE_ARR) : NULL;
    hCount = IsPlausiblePtr((void*)ADDR_HOUSE_ARR) ?
        ARR_COUNT(ADDR_HOUSE_ARR) : 0;
    if (!s_RawInit || cur != s_RawCur || items != (void**)s_RawSupItems ||
        count != s_RawSupCount || hItems != (void**)s_RawHouseItems ||
        hCount != s_RawHouseCount)
    {
        char b1[16], b2[16], b3[16], b4[16], b5[16], buf[176];
        unsigned p = 0;
        s_RawCur = cur;
        s_RawSupItems = items;
        s_RawSupCount = count;
        s_RawHouseItems = hItems;
        s_RawHouseCount = hCount;
        s_RawInit = 1;
        p = AppendCStr(buf, p, "sws: raw cur=", sizeof(buf));
        HtoA((unsigned)(size_t)cur, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        p = AppendCStr(buf, p, " sup=", sizeof(buf));
        HtoA((unsigned)(size_t)items, b2);
        p = AppendCStr(buf, p, b2, sizeof(buf));
        p = AppendCh(buf, p, '/', sizeof(buf));
        UtoA((unsigned)count, b3);
        p = AppendCStr(buf, p, b3, sizeof(buf));
        p = AppendCStr(buf, p, " house=", sizeof(buf));
        HtoA((unsigned)(size_t)hItems, b4);
        p = AppendCStr(buf, p, b4, sizeof(buf));
        p = AppendCh(buf, p, '/', sizeof(buf));
        UtoA((unsigned)hCount, b5);
        p = AppendCStr(buf, p, b5, sizeof(buf));
        LogLine(buf);
    }
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
    if (!s_StableInit || items != (void**)s_PrevSupItems ||
        count != s_PrevSupCount || hItems != (void**)s_PrevHouseItems ||
        hCount != s_PrevHouseCount || cur != s_PrevCur)
    {   /* 五元组变化: 只缓存, 本轮不碰任何数组内容 */
        s_PrevSupItems = items;
        s_PrevSupCount = count;
        s_PrevHouseItems = hItems;
        s_PrevHouseCount = hCount;
        s_PrevCur = cur;
        s_StableInit = 1;
        {   /* 诊断: 每个新状态记一行, 看扫描卡在哪 (只在变化时记, 不刷屏) */
            char b1[16], b2[16], b3[16], b4[16], b5[16], buf[160];
            unsigned p = 0;
            p = AppendCStr(buf, p, "sws: tri sup=", sizeof(buf));
            HtoA((unsigned)(size_t)items, b1);
            p = AppendCStr(buf, p, b1, sizeof(buf));
            p = AppendCh(buf, p, '/', sizeof(buf));
            UtoA((unsigned)count, b2);
            p = AppendCStr(buf, p, b2, sizeof(buf));
            p = AppendCStr(buf, p, " house=", sizeof(buf));
            HtoA((unsigned)(size_t)hItems, b3);
            p = AppendCStr(buf, p, b3, sizeof(buf));
            p = AppendCh(buf, p, '/', sizeof(buf));
            UtoA((unsigned)hCount, b4);
            p = AppendCStr(buf, p, b4, sizeof(buf));
            p = AppendCStr(buf, p, " cur=", sizeof(buf));
            HtoA((unsigned)(size_t)cur, b5);
            p = AppendCStr(buf, p, b5, sizeof(buf));
            LogLine(buf);
        }
        return;
    }
    {   /* 入局检查: 只做指针比对, 不解引用对象 */
        int found = 0;
        for (i = 0; i < hCount; i++)
            if (hItems[i] == cur) { found = 1; break; }
        if (!found) return;
    }
    pside = SwsPlayerSide();
    {   /* 诊断计数: 每个稳定三元组记一次各阶段通过数 (只记一次, 不刷屏) */
        static void* s_StatSup = NULL;
        static int s_StatCnt = 0;
        static void* s_StatCur = NULL;
        if (s_StatSup != items || s_StatCnt != count || s_StatCur != cur)
        {
        int cSup = 0, cType = 0, cOwn = 0, cPres = 0, cFlag = 0;
        int cId = 0, cIdx = 0, j;
        s_StatSup = items;
        s_StatCnt = count;
        s_StatCur = cur;
        {   /* Type/Owner 偏移自举: 建 type 集 (135) + house 集 (本家及同表),
             * 扫 sup +0x00~+0x80 逐字命中, 自动定偏。 */
            void* tset[256];
            void* hset[72];
            int tn = 0, hn = 0, j, k, m;
            void** titems;
            int tcount;
            static int s_WinDone = 0;
            int thits[33];
            int hhits[33];
            for (j = 0; j < 33; j++) thits[j] = hhits[j] = 0;
            if (IsPlausiblePtr((void*)ADDR_SWTYPE_ARR))
            {
                titems = ARR_ITEMS(ADDR_SWTYPE_ARR);
                tcount = ARR_COUNT(ADDR_SWTYPE_ARR);
                if (titems && IsPlausiblePtr(titems) && tcount > 0 && tcount <= 1000)
                {
                    for (j = 0; j < tcount && tn < 256; j++)
                    {
                        void* t = titems[j];
                        if (t && IsPlausiblePtr(t)) tset[tn++] = t;
                    }
                }
            }
            if (hItems && hn < 72)
            {
                for (j = 0; j < hCount && hn < 72; j++)
                {
                    void* hh2 = hItems[j];
                    if (hh2 && IsPlausiblePtr(hh2)) hset[hn++] = hh2;
                }
            }
            for (j = 0; j < count && j < 4096; j++)
            {
                void* s2 = items[j];
                if (!s2 || !IsPlausiblePtr(s2)) continue;
                for (k = 0; k < 33; k++)
                {
                    void* w = *(void**)((char*)s2 + k * 4);
                    if (!w || !IsPlausiblePtr(w)) continue;
                    for (m = 0; m < tn; m++)
                        if (tset[m] == w) { thits[k]++; break; }
                    for (m = 0; m < hn; m++)
                        if (hset[m] == w) { hhits[k]++; break; }
                }
            }
            {
                char sb[256], hx[16];
                unsigned q = 0;
                q = AppendCStr(sb, q, "sws: win tN=", sizeof(sb));
                UtoA((unsigned)tn, hx);
                q = AppendCStr(sb, q, hx, sizeof(sb));
                q = AppendCStr(sb, q, " hN=", sizeof(sb));
                UtoA((unsigned)hn, hx);
                q = AppendCStr(sb, q, hx, sizeof(sb));
                for (k = 0; k < 33; k++)
                {
                    if (thits[k] <= 0 && hhits[k] <= 0) continue;
                    q = AppendCStr(sb, q, " +", sizeof(sb));
                    HtoA((unsigned)(k * 4), hx);
                    q = AppendCStr(sb, q, hx, sizeof(sb));
                    q = AppendCStr(sb, q, "T", sizeof(sb));
                    UtoA((unsigned)thits[k], hx);
                    q = AppendCStr(sb, q, hx, sizeof(sb));
                    q = AppendCh(sb, q, 'H', sizeof(sb));
                    UtoA((unsigned)hhits[k], hx);
                    q = AppendCStr(sb, q, hx, sizeof(sb));
                    if (q > 200) break;
                }
                LogLine(sb);
            }
(void)s_WinDone;
        }
        for (j = 0; j < count && j < 4096; j++)
        {
            void* s2 = items[j];
            void* t2;
            void* o2;
            const char* id2;
            int ix2;
            if (!s2 || !IsPlausiblePtr(s2)) continue;
            cSup++;
            t2 = *(void**)((char*)s2 + OFF_SUP_TYPE);
            o2 = *(void**)((char*)s2 + OFF_SUP_OWNER);
            if (!t2 || !IsPlausiblePtr(t2)) continue;
            cType++;
            if (!o2 || !IsPlausiblePtr(o2) || o2 != cur) continue;
            cOwn++;
            if (*(unsigned char*)((char*)s2 + OFF_SUP_PRESENT) != 1) continue;
            cPres++;
            if (*(unsigned char*)((char*)s2 + OFF_SUP_READY) > 1 ||
                *(unsigned char*)((char*)s2 + OFF_SUP_SUSPEND) > 1) continue;
            cFlag++;
            id2 = (const char*)t2 + OFF_SWT_ID;
            if (!SwsIdOk(id2)) continue;
            cId++;
            ix2 = *(int*)((char*)t2 + OFF_SWT_ARRIDX);
            if (ix2 < 0 || ix2 > 1000) continue;
            cIdx++;
        }
        {
            char b1[16], b2[16], b3[16], b4[16], b5[16], b6[16];
            char b7[16], buf[192];
            unsigned q = 0;
            q = AppendCStr(buf, q, "sws: stat n=", sizeof(buf));
            UtoA((unsigned)count, b1);
            q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " sup=", sizeof(buf));
            UtoA((unsigned)cSup, b2);
            q = AppendCStr(buf, q, b2, sizeof(buf));
            q = AppendCStr(buf, q, " typ=", sizeof(buf));
            UtoA((unsigned)cType, b3);
            q = AppendCStr(buf, q, b3, sizeof(buf));
            q = AppendCStr(buf, q, " own=", sizeof(buf));
            UtoA((unsigned)cOwn, b4);
            q = AppendCStr(buf, q, b4, sizeof(buf));
            q = AppendCStr(buf, q, " pres=", sizeof(buf));
            UtoA((unsigned)cPres, b5);
            q = AppendCStr(buf, q, b5, sizeof(buf));
            q = AppendCStr(buf, q, " flg=", sizeof(buf));
            UtoA((unsigned)cFlag, b6);
            q = AppendCStr(buf, q, b6, sizeof(buf));
            q = AppendCStr(buf, q, " id=", sizeof(buf));
            UtoA((unsigned)cId, b7);
            q = AppendCStr(buf, q, b7, sizeof(buf));
            q = AppendCStr(buf, q, " idx=", sizeof(buf));
            UtoA((unsigned)cIdx, b1);
            q = AppendCStr(buf, q, b1, sizeof(buf));
            LogLine(buf);
        }
        }   /* end once-per-triple stat */
    }
    for (i = 0; i < count && n < SWS_CAP; i++)
    {
        void* sup = items[i];
        void* type;
        void* owner;
        const char* id;
        unsigned char present, ready, susp;
        int swIdx, total, start, left, frame, remain, side, prio;
        const char* pcxFile = NULL;
        int instant = 0;
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
        {
            /* 按类型指针缓存: ArrayIndex(按 ID 反查类型数组, 不能用猜的字段
             * 偏移 —— 曾读出 135 = 类型总数, 写进 CurrentSWTypeIndex 会让
             * 引擎 Supers[] 越界并当场清掉选中)、阵营归类、ini 优先级/隐藏、
             * SW.ManualFire、SidebarPCX 文件名、悬停名字。类型对象静态, 只算一次。
             * 声明在外面: 下面填 s_Items[n] 时还要用它取名字。 */
            SwsTypeCache* tc = SwsTypeEntry(type, id);
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
            /* 名字在这里一并带出来 (它属于类型, 不属于本次的条目下标)。
             * 出了这个块就再也拿不到 tc 了。 */
            SwsCopyName(s_ItemNameBuf, tc->name, 24);
        }
        total = *(int*)((char*)type + OFF_SWT_RECH);
        if (total < 0 || total > 1000000) total = 0;
        start = *(int*)((char*)sup + OFF_SUP_RTSTART);
        left = *(int*)((char*)sup + OFF_SUP_RTLEFT);
        frame = *(int*)ADDR_FRAME;
        if (start == -1) remain = left;
        else remain = left - (frame - start);
        if (remain < 0) remain = 0;
        if (total > 0 && remain > total) remain = total;
        s_Items[n].type = type;
        s_Items[n].pcxFile = pcxFile;
        /* PCX 优先 (Phobos SWButtonClass::Draw 同序); 没有 PCX 才回退 SHP。 */
        s_Items[n].shp = pcxFile ? NULL : SwsShpOk(type);
        s_Items[n].swIdx = swIdx;
        s_Items[n].side = (side >= 0) ? side : pside;
        s_Items[n].prio = prio;
        SwsCopyId(s_Items[n].id, id);
        /* 名字随条目一起搬, 保证悬停提示和图标永远对应同一个超武 */
        SwsCopyName(s_Items[n].name, s_ItemNameBuf, 24);
        /* 一次性记下"点击即生效"的那些: 之前"点了没反应"就有两种可能
         * (没被判成 instant / 判成了但释放方式不对), 这行日志能一次分开。
         * 顺带记 swIdx: 引擎自己发的 SpecialPlace 用的是 id=73/100, 而我们
         * 推的是 102/47 —— 必须把 id 和名字对齐才能确认 ID 语义。 */
        if (instant)
        {
            static int s_InstLogged = 0;
            if (s_InstLogged < 24)
            {
                char sb[128], t1[16];
                unsigned q2 = AppendCStr(sb, 0, "sws: instant type ", sizeof(sb));
                q2 = AppendCStr(sb, q2, id, sizeof(sb));
                q2 = AppendCStr(sb, q2, " idx=", sizeof(sb));
                UtoA((unsigned)swIdx, t1);
                q2 = AppendCStr(sb, q2, t1, sizeof(sb));
                LogLine(sb);
                s_InstLogged++;
            }
        }
        s_Items[n].ready = (ready == 1 && susp == 0) ? 1 : 0;
        s_Items[n].susp = (susp == 1) ? 1 : 0;
        s_Items[n].remain = remain;
        s_Items[n].total = total;
        s_Items[n].instant = instant;
        n++;
    }
    s_ItemN = n;
    {
        static void* s_GotSup = NULL;
        static int s_GotCnt = 0;
        static void* s_GotCur = NULL;
        if (s_GotSup != items || s_GotCnt != count || s_GotCur != cur)
        {
            int m, shn = 0;
            char b1[16], b2[16], buf[96];
            unsigned q = 0;
            s_GotSup = items; s_GotCnt = count; s_GotCur = cur;
            for (m = 0; m < n; m++) if (s_Items[m].shp) shn++;
            q = AppendCStr(buf, q, "sws: got ", sizeof(buf));
            UtoA((unsigned)n, b1);
            q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " shp=", sizeof(buf));
            UtoA((unsigned)shn, b2);
            q = AppendCStr(buf, q, b2, sizeof(buf));
            LogLine(buf);
        }
    }
    if (!s_ScanLogged && n > 0)
    {
        char b1[16], buf[128];
        unsigned p = 0;
        s_ScanLogged = 1;
        p = AppendCStr(buf, p, "sws: scan ok n=", sizeof(buf));
        p += UtoA((unsigned)n, buf + p);
        p = AppendCStr(buf, p, " e0=", sizeof(buf));
        UtoA((unsigned)s_Items[0].swIdx, b1);
        p = AppendCStr(buf, p, b1, sizeof(buf));
        LogLine(buf);
    }
}

/* ================= 隐藏"防御支援页"里的重复超武 =================
 *
 * 目标: 侧栏上有的超武, 防御支援页 (游戏自带的 4 页签条第 1 页) 里**不再**
 * 出现; 侧栏上没有的 (ini 隐藏 / SW.ManualFire=no / 火风暴类), 防御页照旧。
 *
 * 为什么要动到游戏内部:
 *   - 引擎只往页签**加** cameo (SidebarClass::AddCameo 0x6A6300), 从不因为
 *     "它已经在超武侧栏上显示了" 而拒绝。已经躺在页签里的那些不会自己消失:
 *     引擎唯一的移除路径是 StripClass::RecheckCameo(0x6AA600), 而它只删
 *     "IsPresent 已经为 0" 的条目 —— 我们的超武都还在, 所以它不会动它们。
 *   - 而且在 Ares 上, 条目根本不在 StripClass::Cameos[75] 里了: Ares 3.0
 *     把增删/绘制/点击/排序全换成自己 .data 段里的
 *     DynamicVectorClass<BuildType> RulesExt::TabCameos[4]
 *     (见 ares(Antares)/src/Ext/Rules/Hooks.CameoList.cpp), 并且没有导出任何
 *     "删某个 cameo" 的接口。所以要把一个**还在**的超武从防御页拿掉, 只能
 *     直接改这个数组 —— 这也是唯一有模拟风险的写操作, 全部本地 UI 数据,
 *     不碰 SuperClass/EventClass, 联机安全。
 *
 * 两个方向刻意不对称 (踩过坑才这么定的):
 *   - **搬出**要改 Ares 那个数组, 所以得先反查它的地址并逐次校验容器。
 *   - **搬回**不碰那个数组, 只调引擎自己的 AddCameo —— 它自带查重, 对
 *     "已经在页里"的超武重复调用是无害的空操作。这么一来"切回显示"就不
 *     依赖任何反查出来的地址, 不会因为容器校验不过而**静默失败**。
 *
 * 曾经还在 `SidebarClass::AddCameo` 入口 (0x6A6316) 钩了个拦截门想"挡住
 * 新增", 但它装上之后**一次都没被调用过** (跑过好几局, 诊断日志一行都没有,
 * 而 76 条确实能搬出来 —— 说明超武 cameo 是从别的路进页签的, 没走
 * AddCameo)。既然拦不住, 就不留这个死钩子, 改由 `SwsDedupSync` 每 5 帧复查
 * 一遍: 新冒进来的下一次复查就被搬走。机制少一个, 也少占 6 字节。 */

/* SidebarClass 侧栏几何 (gamemd 反汇编实证, 注意与 YRpp 头文件差 4 字节:
 *  头文件说 Tabs[0] 是首字段, 实际 +0x1544; 头文件说 CameoCount +0x50,
 *  实际 +0x54 —— 见 notes/超武侧栏.md 的"偏移实证"。) */
#define ADDR_SIDEBAR          0x0087F7E8u   /* SidebarClass::Instance */
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
#define TAB_DEFENSE           1      /* 防御支援页 = 页签 1 (0 建筑/1 防御/2 步兵/3 载具) */
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

/* 页签 i 的 CameoCount 实时值 (Ares 与 TabCameos[i].Count 始终同步)。
 *
 *   MouseClass / SidebarClass / DisplayClass  ——  **同一个静态对象, 地址就是
 *   0x87F7E8 本身** (原版代码直接 `mov ecx,0x87f7e8` 当 this 用, 所以这里
 *   **不能**写 `*(void**)ADDR_SIDEBAR` —— 那样读到的是对象的**虚表指针**,
 *   一个 .text 代码地址。于是: 偏移全算错、`CameoCount` 读出来是代码字节,
 *   最惨的是拿它当 `this` 去调 `UpdateScrollButtons` / `SetTab` → 直接崩。
 *   (Ares 那句 `add eax,0x880d2c` = 0x87f7e8 + 0x1544, 一句话证实了这点。)
 *
 *   StripClass::Index       = pStrip + 0x38
 *   StripClass::TopRowIndex = pStrip + 0x44
 *   StripClass::CameoCount  = pStrip + 0x54
 *   SidebarClass::Tabs      = pThis  + 0x1544, 步长 0xF94
 *
 * 这套偏移是**从 Ares.dll 自己的代码里读出来的**, 不是猜的:
 *   `SidebarClass_AddCameo_SkipSizeCheck` (钩 0x6A63B7) 里有
 *     imul eax, ebx, 0xf94 / add eax, 0x880d2c
 *   而 0x880d2c = 0x87f7e8 + 0x1544, 一步到位;
 *   `StripClass_RecheckCameos` 里是 `cmp dword [edi+0x54],0` /
 *   `mov eax,[edi+0x38]` / `imul eax,[edi+0x44],0x68`。
 * (`0x68` = 2 * sizeof(BuildType) = 2 * 0x34, 因为锚点取的是
 *  `Items[2 * TopRowIndex]`, 一行两个 cameo。)
 *
 * 注意 YRpp 头文件里这几个字段都比实际值小 4 (CameoCount 写 0x50 / 实际 0x54,
 * Cameos 写 0x54 / 实际 0x58) —— **以 Ares.dll 的代码为准, 别以头文件为准**。 */
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
 * 为什么不用"扫 .data 段找 4 连的 DynamicVectorClass": 那条路依赖
 * `StripClass::CameoCount` 互证, 而那条互证在本机对不上 (读出来是垃圾),
 * 于是永远命中 0 个。改成一劳永逸的办法:
 *
 * Ares 导出的 `SidebarClass_AddCameo_SkipSizeCheck` (钩 0x6A63B7) 开头
 * 几十字节里有这两条 (实测反汇编):
 *     8B 04 CD <disp32>              mov eax, [ecx*8 + TabCameos[0].Items]
 *     6B 0C CD <disp32> 34           imul ecx, [ecx*8 + TabCameos[0].Count], 0x34
 * 两个 disp32 都是**编译期写死的绝对地址** (Ares 自己就这么索引它的), 所以:
 *     TabCameos = (Items 的 disp32) - 4        (Items 在容器 +0x04)
 *     两条互证: (Count 的 disp32) - 0x10 == TabCameos
 * 这样定位跟着 Ares 的代码走: 不用扫数据段、不会误命中别的
 * DynamicVectorClass、ASLR 重定位后依然对 (那条指令里的绝对地址会一起被
 * 重定位)。找不到 / 两条对不上 / 容器不自洽, 一律 fail-closed。
 *
 * 只在**确实在跑一局**时才找: `HouseClass::CurrentPlayer` 有效, 且帧号已经
 * 走过 60 (主菜单/加载界面里容器还没建好)。
 *
 * > ⚠ **不要用 `Unsorted::ScenarioInit` (0xA8E7AC) 做这个判断**。它的名字
 * > 像是"场景初始化中", 实际既不是 0/1 标志也不是可靠的非零标志:
 * > gamemd 里有 `inc dword [0xA8E7AC]` (0x442FF7 / 0x44E6ED) 说明它是个
 * > 计数器, 而实战中它又长期是 **0**。先后试过 `!= 1` 和 `!= 0`, 两次都
 * > 让定位器一次都没跑过 —— 而失败路径不记日志, 表现就是"按钮点了
 * > 没反应, 日志里一行 dedup 都没有"。 */
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
    {
        char sb[64], b1[16];
        unsigned q = AppendCStr(sb, 0, "sws: dd off got=", sizeof(sb));
        UtoA((unsigned)gotItems, b1);
        q = AppendCStr(sb, q, b1, sizeof(sb));
        q = AppendCh(sb, q, '/', sizeof(sb));
        UtoA((unsigned)gotCnt, b1);
        AppendCStr(sb, q, b1, sizeof(sb));
        LogLine(sb);
    }
    s_Failed = 1;
    return NULL;
}

/* 每次用之前重新校验一遍容器 (Items 指针会在扩容时变, 所以必须每次现读;
 * 定位出来的**容器头地址**本身终身不变 —— Ares 只在 ClearCameos 里清内容,
 * 从不搬容器)。
 *
 * 刻意**不**拿 `StripClass::CameoCount` 互证: 这条互证已经坑过两轮 (第一轮是
 * `this` 取错, 读出来是 .text 的代码字节; 第二轮是它在本机确实会和
 * `TabCameos[i].Count` 不同步), 而容器地址是从 **Ares 自己的代码**里读出来的,
 * 本身就可信。犯不着为一个不可靠的互证把整条路堵死。 */
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
     * 的超武重复调用是**无害的空操作**。这样"切回显示"这条路就不依赖任何
     * 反查出来的地址 —— 之前就是卡在这里 (容器校验一旦不过就静默失败)。 */
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
 * 已经在隐藏态时也照样每 5 帧跑一遍 (移除是幂等的, 代价只是扫一遍条目):
 * 这代替了原来那个 `AddCameo` 拦截钩 —— 那钩子装上之后一次都没被调用过,
 * 超武 cameo 显然是从别的路进页签的, 于是新冒出来的超武就靠这次复查搬走。 */
static int SwsDedupSync(void)
{
    int want, r;
    SwsEnsureInit();
    if (!s_Enable) { s_DedupDirty = 0; return 0; }
    /* 侧栏整个收起 (F4) 时不隐藏: 否则防御页空着、侧栏也没有, 超武就彻底
     * 没法用了。收起 = 恢复原样, 展开 = 重新隐藏。 */
    want = (s_DedupHide && s_Visible) ? 1 : 0;
    if (want == s_DedupDone && !s_DedupDirty) return 0;
    if (want && !SwsAresFindTabCameos()) { s_DedupDirty = 0; return 0; }
    r = want ? SwsDedupRemove() : SwsDedupRestore();
    if (r < 0)
    {
        /* 状态没变 → 下一帧还会重试, 所以**必须限流**, 否则一次失败能刷两千行
         * 把日志撑爆 (踩过)。 */
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
        char sb[64], b1[16];
        unsigned q = AppendCStr(sb, 0, "sws: dd ", sizeof(sb));
        q = AppendCStr(sb, q, want ? "hide " : "show ", sizeof(sb));
        q = AppendCStr(sb, q, "n=", sizeof(sb));
        UtoA((unsigned)r, b1);
        AppendCStr(sb, q, b1, sizeof(sb));
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
    if (!s_Enable || !s_Visible || !s_ColsValid) return 0;
    /* 命中范围: 整块 (含右侧滚动条那几像素) */
    if (mx < s_BoundX || mx > s_LastX + SWS_ICON_W + 5) return 0;
    if (my < s_YStart || my >= s_YStart + (s_VisRows * s_RowH - s_Gap)) return 0;
    if (s_MaxSk <= 0) return 0;                  /* 所有列都装得下 */
    if (dir < 0) s_Scroll -= step;
    else          s_Scroll += step;
    if (s_Scroll < 0) s_Scroll = 0;
    if (s_Scroll > s_MaxSk) s_Scroll = s_MaxSk;
    return 1;
}

/* 鼠标是否在侧栏区内 (含滚动条): 战役框 WheelHook 用它决定优先级 */
extern "C" __declspec(dllexport) int __cdecl SWS_OverBar(int mx, int my)
{
    if (!s_Enable || !s_Visible || !s_ColsValid) return 0;
    if (mx < s_BoundX || mx > s_LastX + SWS_ICON_W + 5) return 0;
    if (my < s_YStart || my >= s_YStart + (s_VisRows * s_RowH - s_Gap)) return 0;
    return 1;
}

void SWS_Poll(void)
{
    int tog, lb, rb;
    /* ---- 帧门: 这个函数挂在 0x4F42F0, 而那个地址**不是**每帧一次 ----
     *
     * gamemd 里 `call 0x4F42F0` 有 113 个静态调用点, 散在 AI / 对象 / 渲染 /
     * 消息代码里, 其中多处位于逐对象循环内。日志实测: 12,540 个渲染帧里
     * SWS_Poll 被调了 1,782,420 次 = **每渲染帧约 142 次** (区间 47~225)。
     *
     * 于是"每 15 帧扫一次表"实际是每 0.25 秒一次, "每 60 帧记一行日志"实际
     * 是每渲染帧 2.4 行 —— 当初日志 30,688 行里 29,707 行是 sws: poll 就是这么来的。
     * 每帧 142 次 GetAsyncKeyState + 2 次 QPC, 以及当初每次 Poll 都跑的
     * SwsFireWatch (78 条/次 ≈ 1.1 万次迭代/帧), 全是白烧。
     *
     * 这里用引擎帧号 0xA8ED84 变化做门, 同一个渲染帧内只真跑一次。
     * 帧号在输入处理之后才推进, 而 0x4F42F0 的第一次调用发生在帧首, 所以
     * 用"与上次相同则跳过"即可, 不必关心它到底推进几次。
     */
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
        /* 隐藏时仍采样键态, 防切回瞬间残留边沿误触。
         * 同时**保留显隐箭头的点击** —— 箭头在隐藏时依然画在屏幕右缘,
         * 不处理的话就成了"看得见但点不动", 侧栏再也展不开。
         *
         * 边沿判定必须用**和可见分支同一份** s_PrevLB。早先这里另开了一个
         * s_HidPrevLB, 而可见分支点箭头后是 `return` 直接跳走的, 根本没更新
         * s_PrevLB —— 于是下一帧进入本分支时 s_PrevLB 还是 0, 而按键确实按着,
         * 就伪造出一个"新按下沿", 刚收起的侧栏立刻又弹回来, 表现就是
         * "点一下只闪一下, 第二次点才生效"。 */
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
                    s_HoldSkip = 1;      /* 吞掉, 别让这次点击漏到地图 */
                    LogLine("sws: show");
                }
            }
            s_PrevLB = lb;
            s_PrevRB = rb;
        }
        s_ItemN = 0;
        s_BoundValid = 0;
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
                    /* 两个按钮都画在超武块**右侧** (栏区判定框之外), 所以必须
                     * 在栏区判定**之前**单独命中 —— 早先把显隐按钮放在栏区判定
                     * 里面, 而栏区要求 my >= s_BoundY (=yStart), 按钮在别的行,
                     * 于是永远点不到 (表现就是"有个绿块但没反应")。 */
                    if (mx >= s_BtnX && mx < s_BtnX + s_BtnW &&
                        my >= s_BtnY && my < s_BtnY + s_BtnH)
                    {
                        s_Visible = s_Visible ? 0 : 1;
                        s_DedupDirty = 1;
                        LogLine(s_Visible ? "sws: show" : "sws: hide");
                        /* 按下就得吞掉, 否则这次点击会漏到引擎那边,
                         * 而"已选中 + 按住"会让引擎把超武就地释放。 */
                        s_HoldSkip = 1;
                        /* 关键: 这里 return 绕过了函数末尾的按键态更新,
                         * 必须自己记下来 —— 否则下一帧进入隐藏分支时会拿
                         * 到"上一帧没按键"的旧值, 伪造出一个按下沿, 刚收起
                         * 的侧栏又弹回来 (点一下只闪一下)。 */
                        s_PrevLB = lb;
                        s_PrevRB = rb;
                        SwsDedupSync();
                        return;
                    }
                    /* 隐藏重复按钮: 显隐按钮左边那个。切换"防御页里的重复超武
                     * 要不要拿掉", 真正搬/补在函数末尾的 SwsDedupSync 里做。
                     * s_HidValid 保证它没画出来的那几帧不吃点击。
                     *
                     * 这里**不**因为"定位不到容器"就不让点: 拦截门
                     * (0x6A6316) 是独立的, 定位不到只是搬不走已经在那儿的,
                     * 以后新增的超武照样不会进防御页。所以按钮永远给反馈,
                     * 搬不动就靠日志说话 (定位器自己会记 dedup off)。 */
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
                     * 必须限在栏区里 —— 早先这里只要 s_BoundsKnown 就生效,
                     * 于是玩家在**地图上**右键取消选中的单位, 反而把超武的
                     * 选中状态给清了, 两边打架。 */
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
    /* 扫描节流: 每 15 **渲染帧** (约 0.25 秒) 才重扫一次。扫描要读 78 条超武的
     * Type/Owner/IsReady/充能计时, 实测几微秒, 但没必要每帧跑 ——
     * 侧栏内容本来就看不出每帧的变化。
     * 玩家点击时 (下面 s_ForceScan) 立刻补一次, 保证点到的图标状态是最新的。
     * 注: 早先注释写"1 秒"是错的 —— 当时 SWS_Poll 每渲染帧跑 142 次,
     * 15 次"调用"其实只有 0.25 秒; 帧门修掉之后这个数字才真的是帧。 */
    {
        static int s_ScanAge = 0;
        s_ScanAge++;
        if (s_ScanAge >= 15 || s_ForceScan)
        {
            s_ScanAge = 0;
            s_ForceScan = 0;
            SwsScan();
            /* 顺便把防御页去重**标记**成"要复查": 已经在隐藏态时, 新冒进
             * 防御页的超武 cameo 靠这一遍搬走 (代替那个从未被调用的
             * AddCameo 拦截钩)。SwsDedupSync 自己会做幂等判定, 搬不动
             * 就只跑一次扫表、不会动页签。
             * 只在"隐藏"态复查: "显示"态是按钮点一下才需要补, 而补的判据
             * (AddCameo 自带查重) 不会因为时间推移而失效。 */
            if (s_DedupHide) s_DedupDirty = 1;
        }
    }
    SwsDedupSync();
}

/* ---------------- 点击吞没门 ---------------- */

/* 鼠标是否真的压在一个**可见的超武图标**上。
 *
 * 吞没门不能拿整块栏区矩形来判。栏区底框是按每列的 maxRows 画满的, 某列
 * 条目少的时候下半截是空的, 再加上栏区内部的左右留白 —— 这些地方**根本没有
 * 图标**, 却会把玩家的点击/选中整片吞掉, 表现就是"侧栏盖住的地图那块点不动,
 * 框单位/打靶都用不了"。所以只在真正画了图标的地方挡, 空地一律还给引擎。 */
static int SwsMouseOnIcon(int mx, int my)
{
    int k;
    if (s_ItemN <= 0) return 0;
    for (k = 0; k < s_ItemN; k++)
    {
        /* 滚出可见范围的 (x=-1) 不算: 不跳过的话 x=-1 会让命中判定恒真 */
        if (s_Items[k].x < 0) continue;
        if (mx >= s_Items[k].x && mx < s_Items[k].x + SWS_ICON_W &&
            my >= s_Items[k].y && my < s_Items[k].y + SWS_ICON_H)
            return 1;
    }
    return 0;
}

extern "C" __declspec(dllexport) unsigned __cdecl SWS_ClickGate(void* regs)
{
    int mx = -1, my = -1;
    int r = 0;
    (void)regs;
    SwsEnsureInit();
    if (!s_Enable || !s_Visible) return 0;
    /* 当帧记下的按下: 一律跳过引擎的 ProcessClickCoords,
     * 让这次点击止于侧栏, 不被当成打靶放置。即时超武也一样吞 ——
     * 它靠 SpecialPlace 事件生效, 不依赖引擎处理这次点击。
     *
     * 命中判定用 s_BoundsKnown 而不是 s_BoundValid: 后者在 SwsScan 开头
     * 被清零, 而点击那帧必然 s_ForceScan=1 触发重扫, 门恰好撞上它被清零的
     * 窗口而放行 (这是"要点很多次"的直接原因)。栏区几何在重扫期间不变。*/
    if (s_HoldSkip) r = ADDR_CLICKGATE_SKIP;
    else if (!s_BoundsKnown) r = 0;
    else if (!GtMouseGameXY(&mx, &my)) r = 0;
    else if (SwsMouseOnIcon(mx, my)) r = ADDR_CLICKGATE_SKIP;
    return (unsigned)r;
}

/* ---------------- 即时超武: SpecialPlace 事件 (联机安全) ----------------
 * Action==None 的超武是"点了就放", Phobos 不写 CurrentSW, 而是往
 * EventClass::OutList 塞一个 SpecialPlace 事件 (见 Phobos
 * src/Ext/Sidebar/SWSidebar/SWButtonClass.cpp LaunchSuper):
 *
 *   EventClass::OutList.Add(EventClass(pCurrent->ArrayIndex,
 *       EventType::SpecialPlace, swIndex, CellStruct::Empty));
 *
 * 写 CurrentSW 只是本地 UI 状态, 引擎当帧并不会释放, 所以点了没反应;
 * 而且直接改模拟状态会破坏联机同步。这里完全照 Phobos 的做法, 并且
 * 复用 BuildQueue.cpp 里已验证的 OutList 追加 (0x4C6B60 ctor + 计数推进),
 * 保持和原版同序 —— 引擎在 DoList 里统一执行, 联机由 Mega/Connect 广播。
 *
 * SwPush 返回 1=已入队, 0=队列满发不出去。 */
#define SWS_ADDR_CURPLAYER  0xA83D4Cu   /* HouseClass* CurrentPlayer */
#define SWS_OFF_HOUSE_INDEX 0x30u       /* HouseClass::ArrayIndex */
#define SWS_EVT_SPECIALPLACE 0x12u      /* EventType::SpecialPlace (GeneralDefinitions.h) */
/* ctor (thiscall): this=111B 缓冲, (houseIdx, EventType, id, CellStruct) */
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
    /* ctor 之前必须把 111 字节全清零: EventClass 是 pack(1) 且 union 后面
     * 90 多字节 ctor 不碰, EventClass::operator== 又是 memcmp(...,111),
     * 留栈垃圾等于给联机同步埋雷。抓包对比也证实: 引擎自己那条 [15..] 也是
     * 垃圾 (90 50 59 08), 但我们没必要学它 —— 清零是确定值, 更安全。 */
    for (i = 0; i < SWS_EVT_SIZE; i++) ev[i] = 0;
    /* CellStruct::Empty (0,0) —— 这就是引擎点**原版侧栏**时发的事件里带的格子。
     * 逐字节对比过 (用户点原版侧栏抓的地面真值):
     *   引擎(原版侧栏) id=47 : 12 00 00 17511B00 2F000000 00000000 00
     *   我们                   id=47 : 12 00 00 CB631B00 2F000000 00000000 00
     * `[0..15]` 完全一致, cell 同为 0,0。
     * (中途我误信了另一批 cell=242,125 / [1]=0x5A 的事件, 那是 AI/触发
     *  产生的另一条路径, 拿它当基准就错了 —— 改回 Empty。)
     * CellStruct 收下却根本不用的类型存在 (如 SW_Firewall::Activate),
     * 所以"没有目标"正是传 Empty 的意思。 */
    {
        short cell[2] = { 0, 0 };
        FN_EVSPCTOR(ev, houseIdx, SWS_EVT_SPECIALPLACE, swIdx, cell);
    }

    for (i = 0; i < SWS_EVT_SIZE; i++)
        ((char*)(SWS_ADDR_OUT_BUF + next * SWS_EVT_SIZE))[i] = ev[i];

    /* 同款无参回调 (原版追加事件后必调一次, 返回值存并行数组)。
     * 必须用**普通的函数指针调用**, 不能写成内联汇编: 汇编里
     * "call *%1" 只声明了 ecx/edx/cc/memory 被破坏, 而真实 call 还会
     * 破坏 ebx/esi/edi/ebp —— 编译器以为这几个寄存器还是自己的值,
     * 实际已经被 callee 改掉 (实测崩在这里: EAX=ESI=0x400DDDA5, 往
     * 近 NULL 地址写)。让编译器自己生成 call, 保存/恢复由它负责。 */
    cb = *(void**)0x7E1530u;
    if (cb && IsPlausiblePtr(cb))
        eaxAfter = (unsigned)(size_t)((int (__cdecl *)(void))cb)();
    else
        eaxAfter = 0;
    *(unsigned*)(SWS_ADDR_OUT_PAR + next * 4u) = eaxAfter;

    *(unsigned*)SWS_ADDR_OUT_NEXT = (next + 1u) & (SWS_EVT_SLOTS_N - 1u);
    *(unsigned*)SWS_ADDR_OUT_COUNT = count + 1u;
    /* 记下自己塞进去的那一条 (houseIdx/swIdx), 用来和抓包日志里引擎自己
     * 产生的 SuperWeapon 事件逐字节对比 —— 别再靠"应该对吧"来判断。 */
    {
        /* 我们构造出来的事件前 16 字节, 便于和抓包日志里引擎发的对照。 */
        char sb[256], hb[4];
        unsigned q = AppendCStr(sb, 0, "sws: push h=", sizeof(sb));
        UtoA((unsigned)houseIdx, hb);
        q = AppendCStr(sb, q, hb, sizeof(sb));
        q = AppendCStr(sb, q, " id=", sizeof(sb));
        UtoA((unsigned)swIdx, hb);
        q = AppendCStr(sb, q, hb, sizeof(sb));
        for (i = 0; i < 16; i++)
        {
            if (i && (i % 8) == 0) q = AppendCh(sb, q, ' ', sizeof(sb));
            q = AppendCStr(sb, q, hb, sizeof(sb));
        }
        LogLine(sb);
    }
    return 1;
}

/* ---------------- 绘制 ---------------- */


/* ---- PCX 逐像素贴图 (不走引擎 BlitToSurface) ----
 * 引擎 BlitToSurface 每调一次就锁/解锁一次合成面, N 个条目 = N 次锁面。
 * 所以改成在下面那次锁面里直接写像素。
 *
 * BSurface 布局 (gamemd.exe 0x6B9D00 的构造函数据实, 与 YRpp Surface.h:144 一致):
 *   +0x00 void* vptr          (BSurface 虚表 0x7E2070)
 *   +0x04 int   Width
 *   +0x08 int   Height
 *   +0x0C int   LockLevel
 *   +0x10 int   BytesPerPixel  (=2, 16bpp RGB565)
 *   +0x14 void* Buffer         (WORD*, 紧排, 行距 = Width*2)
 *   +0x18 int   Size           (= Width*Height*2)
 *   +0x1C bool  Allocated
 *   sizeof == 0x20, alignof == 4
 *
 * 早先这里写的是 0x88/0x8C/0x94/0x98 —— 那是把**虚表大小** (34*4=0x88) 当成了
 * 字段偏移, 实际对象只有 0x20 字节。于是每次都在下面的校验处提前 return 0,
 * 图标一直退回引擎 BlitToSurface, 而那条路是整块覆盖、100% 不透明, 根本不
 * 认 s_Alpha —— 现象就是"Alpha 对底框/边框有效, 对图标完全无效"。
 *
 * 布局对不上仍然返回 0, 由调用方退回引擎 blit, 保证不会画不出来。 */
static int SwsPcxBlitLocked(unsigned short* dstBase, int pitch,
                            int surfW, int surfH,
                            int dx, int dy, int dw, int dh, void* psurf)
{
    int pw, ph, bpp, x, y;
    unsigned short* src;
    if (!dstBase || !psurf || !IsPlausiblePtr(psurf)) return 0;
    /* 虚表必须指向代码段, 否则说明根本不是 BSurface */
    if (!IsPlausiblePtr(*(void**)psurf)) return 0;
    pw = *(int*)((char*)psurf + 0x04);
    ph = *(int*)((char*)psurf + 0x08);
    bpp = *(int*)((char*)psurf + 0x10);
    src = *(unsigned short**)((char*)psurf + 0x14);
    if (pw < 1 || pw > 256 || ph < 1 || ph > 256 || bpp != 2) return 0;
    if (!src || !IsPlausiblePtr((void*)src)) return 0;
    /* Size 自洽校验: Buffer 至少得有这么多字节, 挡掉读到野指针 */
    if (*(int*)((char*)psurf + 0x18) != pw * ph * 2) return 0;
    {
        static int s_Logged = 0;
        if (!s_Logged)
        {
            char sb[160], b1[16], b2[16], b3[16], b4[16];
            unsigned q = AppendCStr(sb, 0, "sws: bsurf ", sizeof(sb));
            UtoA((unsigned)pw, b1); q = AppendCStr(sb, q, b1, sizeof(sb));
            q = AppendCh(sb, q, 'x', sizeof(sb));
            UtoA((unsigned)ph, b2); q = AppendCStr(sb, q, b2, sizeof(sb));
            q = AppendCh(sb, q, ' ', sizeof(sb));
            UtoA((unsigned)bpp, b3); q = AppendCStr(sb, q, b3, sizeof(sb));
            q = AppendCh(sb, q, ' ', sizeof(sb));
            UtoA((unsigned)(size_t)src, b4); q = AppendCStr(sb, q, b4, sizeof(sb));
            LogLine(sb);
            s_Logged = 1;
        }
    }
    for (y = 0; y < dh; y++)
    {
        int ty = dy + y;
        int sy = (ph <= 1) ? 0 : (y * ph) / dh;
        unsigned short* drow;
        const unsigned short* srow;
        if (ty < 0 || ty >= surfH) continue;
        drow = dstBase + (size_t)ty * pitch / 2;
        srow = src + (size_t)sy * pw;
        for (x = 0; x < dw; x++)
        {
            int tx = dx + x;
            int sx = (pw <= 1) ? 0 : (x * pw) / dw;
            unsigned short c;
            if (tx < 0 || tx >= surfW) continue;
            c = srow[sx];
            if (c == PCX_TRANS_PARENT) continue;   /* 色键 */
            /* 整体透明度 <100 时不能直接覆盖: 要和底下的像素混合, 否则
             * 图标会永远是 100% 不透明, Alpha 就只对底框/边框这些
             * 走 SwsFillLocked 的元素生效。 */
            if (s_Alpha >= 100) drow[tx] = c;
            else
            {
                unsigned short bgc = drow[tx];
                int sr = c & 0x1F, sg = (c >> 5) & 0x3F, sb = (c >> 11) & 0x1F;
                int br = bgc & 0x1F, bg = (bgc >> 5) & 0x3F, bb = (bgc >> 11) & 0x1F;
                int nr = (sr * s_Alpha + br * (100 - s_Alpha)) / 100;
                int ng = (sg * s_Alpha + bg * (100 - s_Alpha)) / 100;
                int nb = (sb * s_Alpha + bb * (100 - s_Alpha)) / 100;
                drow[tx] = (unsigned short)(nb << 11 | ng << 5 | nr);
            }
        }
    }
    return 1;
}

/* 锁面状态下的半透明填充 (替代每条目 6 次 FillRectTrans 虚表调用)
 * op 先乘上**侧栏整体透明度** s_Alpha: 这样底框/边框/滚动条/箭头这些各带
 * 自己的局部 alpha 的元素, 只要走这里就自动被整体透明度统一调制。 */
/* 实际填充: 调用方已经把 s_Alpha 乘进去了, 这里不再管整体透明度。 */
static void SwsFillCore(unsigned char* base, int pitch, int surfW, int surfH,
                        int x, int y, int rw, int rh,
                        unsigned char r, unsigned char g, unsigned char b,
                        int op)
{
    int yy, xx;
    if (!base || rw <= 0 || rh <= 0) return;
    if (op < 0) op = 0;
    if (op > 100) op = 100;
    if (op <= 0) return;
    for (yy = 0; yy < rh; yy++)
    {
        int ty = y + yy;
        unsigned short* row;
        if (ty < 0 || ty >= surfH) continue;
        row = (unsigned short*)(base + (size_t)ty * pitch);
        for (xx = 0; xx < rw; xx++)
        {
            int tx = x + xx;
            if (tx < 0 || tx >= surfW) continue;
            row[tx] = Blend16(row[tx], r, g, b, (unsigned)op * 255 / 100);
        }
    }
}

/* 吃侧栏整体透明度的填充: 底框/边框/滚动条/箭头/图标底板都走这里。 */
static void SwsFillLocked(unsigned char* base, int pitch, int surfW, int surfH,
                          int x, int y, int rw, int rh,
                          unsigned char r, unsigned char g, unsigned char b,
                          int op)
{
    if (op < 0) op = 0;
    if (op > 100) op = 100;
    SwsFillCore(base, pitch, surfW, surfH, x, y, rw, rh, r, g, b,
                (op * s_Alpha) / 100);
}

/* **不**吃整体透明度的填充: 只给悬停提示框用。
 * 提示框是"此刻正在读"的信息, 必须始终清晰 —— 跟着侧栏透明度一起淡到 30%
 * 就看不清是哪个超武了。它是侧栏里唯一无视 Alpha 的元素。 */
static void SwsFillRawLocked(unsigned char* base, int pitch, int surfW, int surfH,
                             int x, int y, int rw, int rh,
                             unsigned char r, unsigned char g, unsigned char b,
                             int op)
{
    SwsFillCore(base, pitch, surfW, surfH, x, y, rw, rh, r, g, b, op);
}



/* ---- 显隐按钮几何 (旧版: 横扁手柄, 挂在超武块上方 2px) ----
 * 已被 SwsTwoBtnGeom 取代: 现在两个按钮统一挂在超武块**右侧**、同一行,
 * 显隐在右、隐藏重复在左 (需求: "在显示/隐藏按钮左边添加一个按钮")。
 * 位置必须由绘制期统一发布, 因为 SWS_Poll 的点击命中读的就是这四个值 ——
 * 放在栏区判定框**之外** (超武块右边), 所以命中判定要排在栏区判定**之前**,
 * 否则永远点不到 (曾经就因为这个位置而"看得见点不动")。 */

/* ---- 显隐按钮: 圆角底板 + 边框 + 实心三角 ----
 * 三层, 由外到内:
 *   1) 底板  整块矩形, 绿黑半透 (和侧栏底板一个观感)
 *   2) 边框  四条边亮一档, 让它在深色地图上也看得见轮廓
 *   3) 三角  按列画的高度渐变, 尖端指向 dir 指定的方向
 *        dir=0 -> ◀ 侧栏隐藏时, 点它展开
 *        dir=1 -> ▶ 侧栏显示时, 点它收起
 * 底板用 SwsFillLocked 画, 所以会自动吃到侧栏整体透明度 s_Alpha。 */
static void SwsDrawArrow(unsigned char* base, int pitch, int surfW, int surfH,
                         int x, int y, int bw, int bh, int dir,
                         int mx, int my)
{
    int cx, hov;
    int inX, inY, inW, inH;
    if (!base || bw <= 0 || bh <= 0) return;
    if (bw < 8) bw = 8;
    if (bh < 5) bh = 5;
    hov = (mx >= x && mx < x + bw && my >= y && my < y + bh);

    /* 1) 底板: 主题色压得很淡, 只为让按钮有个"面", 不再额外垫一圈黑底
     *    (早先底板是纯黑往外扩 2px, 边框又是绿的, 结果看着就是"一层黑一层绿")。
     *    底板和边框都走 SwsFillLocked, 所以自动吃到侧栏整体透明度 s_Alpha。 */
    SwsFillLocked(base, pitch, surfW, surfH, x, y, bw, bh,
                  SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, hov ? 70 : 45);

    /* 2) 边框: 就一层, 上下左右各一条 1px 主题色边 */
    {
        int bc = hov ? 225 : 165;
        SwsFillLocked(base, pitch, surfW, surfH, x, y, bw, 1,
                      SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, bc);
        SwsFillLocked(base, pitch, surfW, surfH, x, y + bh - 1, bw, 1,
                      SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, bc);
        SwsFillLocked(base, pitch, surfW, surfH, x, y, 1, bh,
                      SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, bc);
        SwsFillLocked(base, pitch, surfW, surfH, x + bw - 1, y, 1, bh,
                      SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, bc);
    }

    /* 3) 三角: 留 2px 内边距, 按列画高度 -> 尖端指向 dir */
    inX = x + 3; inY = y + 2;
    inW = bw - 6; inH = bh - 4;
    if (inW < 2) inW = 2;
    if (inH < 1) inH = 1;
    for (cx = 0; cx < inW; cx++)
    {
        int hh = (inH * cx) / (inW - 1 > 0 ? inW - 1 : 1);
        int off;
        if (dir) hh = inH - hh;             /* dir=1 ▶ 尖端在右 */
        if (hh < 1) hh = 1;
        if (hh > inH) hh = inH;
        off = (inH - hh) / 2;
        SwsFillLocked(base, pitch, surfW, surfH, inX + cx, inY + off, 1, hh,
                      SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, hov ? 255 : 215);
    }
}

/* ---- 两个按钮的公共几何 ----
 * 挂在超武块**上方** 2px 的一行, 右对齐到块的右缘, 从右往左是
 * [显隐][空 3][隐藏重复]。所以超武块要多让出 18px 的顶部空间, 而
 * **不用**在右边留位 (侧栏右边是原版侧栏的地盘)。
 *
 * 位置必须由绘制期统一发布, 因为 SWS_Poll 的点击命中读的就是这几个值 ——
 * 两个按钮都在栏区判定框 (s_BoundX/Y/W/H) **之外**(块顶之上), 所以命中
 * 判定要排在栏区判定**之前**, 否则永远点不到 (曾经就因为这个位置而
 * "看得见点不动")。 */
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
    /* 块贴屏幕最左边时兜一下底, 别让按钮跑出屏幕 */
    if (s_BtnX + s_BtnW > w) s_BtnX = w - s_BtnW;
    if (s_HidX < 0) s_HidX = 0;
    if (s_BtnX < 0) s_BtnX = 0;
    s_BtnY = blockTop - s_BtnH - 2;   /* 上方, 留 2px 缝, 不压图标 */
    if (s_BtnY < 0) s_BtnY = 0;
    s_HidY = s_BtnY;
    s_HidValid = 1;
}

/* 侧栏收起时: 只留一个显隐箭头, 贴屏幕右缘 (超武块没了, 没有右缘可对齐)。
 * 像素 TopMargin 模式下它和展开时落在同一行 (都用 TopMargin-18); 比例模式下
 * 展开时要减半个块高, 所以会差 blockH/2 —— 这是原有行为, 不动。
 * 隐藏重复按钮既不画也不可点 —— 侧栏都没了, "侧栏里有" 也就无从谈起
 * (见 SwsDedupSync 的 want)。 */
static void SwsCollapsedBtnGeom(int w, int blockTop)
{
    s_BtnW = SWS_BTN_W;
    s_BtnH = SWS_BTN_H;
    s_BtnX = w - s_RightMargin - s_BtnW;
    if (s_BtnX < 0) s_BtnX = 0;
    s_BtnY = blockTop - s_BtnH - 2;
    if (s_BtnY < 0) s_BtnY = 0;
    s_HidX = s_BtnX;
    s_HidY = s_BtnY;
    s_HidValid = 0;
}

/* ---- 隐藏重复按钮: 禁入牌 (方框 + 斜杠) ----
 * 语义直白: 开着 = 防御页上的重复超武被"禁止"了, 灭 = 原样。
 * 开着偏绿、灭掉偏灰, 悬停整块提亮, 所以不靠形状也读得出状态。 */
static void SwsDrawDedup(unsigned char* base, int pitch, int surfW, int surfH,
                         int x, int y, int bw, int bh, int on, int mx, int my)
{
    static const unsigned char kOn[3]  = { 90, 235, 150 };
    static const unsigned char kOff[3] = { 155, 155, 165 };
    const unsigned char* c = on ? kOn : kOff;
    int hov, s, bx, by, i, a;
    if (!base || bw <= 0 || bh <= 0) return;
    if (bw < 8) bw = 8;
    if (bh < 5) bh = 5;
    hov = (mx >= x && mx < x + bw && my >= y && my < y + bh);
    /* 底板 + 1px 边框 (走 SwsFillLocked, 自动吃侧栏整体透明度) */
    SwsFillLocked(base, pitch, surfW, surfH, x, y, bw, bh,
                  c[0], c[1], c[2], hov ? 70 : 40);
    a = hov ? 235 : (on ? 190 : 115);
    SwsFillLocked(base, pitch, surfW, surfH, x, y, bw, 1, c[0], c[1], c[2], a);
    SwsFillLocked(base, pitch, surfW, surfH, x, y + bh - 1, bw, 1, c[0], c[1], c[2], a);
    SwsFillLocked(base, pitch, surfW, surfH, x, y, 1, bh, c[0], c[1], c[2], a);
    SwsFillLocked(base, pitch, surfW, surfH, x + bw - 1, y, 1, bh, c[0], c[1], c[2], a);
    /* 里面的"牌子": 取宽高小的那个做正方, 居中 */
    s = bw - 6;
    if (bh - 6 < s) s = bh - 6;
    if (s < 3) s = 3;
    bx = x + (bw - s) / 2;
    by = y + (bh - s) / 2;
    a = hov ? 235 : (on ? 210 : 130);
    SwsFillLocked(base, pitch, surfW, surfH, bx, by, s, 1, c[0], c[1], c[2], a);
    SwsFillLocked(base, pitch, surfW, surfH, bx, by + s - 1, s, 1, c[0], c[1], c[2], a);
    SwsFillLocked(base, pitch, surfW, surfH, bx, by, 1, s, c[0], c[1], c[2], a);
    SwsFillLocked(base, pitch, surfW, surfH, bx + s - 1, by, 1, s, c[0], c[1], c[2], a);
    /* 斜杠: 只在"隐藏中"时画, 于是灭掉的按钮是个空框, 状态一眼可分 */
    if (on)
    {
        a = hov ? 255 : 225;
        for (i = 0; i < s; i++)
            SwsFillLocked(base, pitch, surfW, surfH, bx + i, by + i, 1, 1,
                          c[0], c[1], c[2], a);
    }
}

/* ---- 悬停提示框: 半透明黑底 + 主题色边框 + 白字 ----
 * ax = 水平锚点, ayUp = 提示要挂的那个东西的**上**边, ayDn = 它的**下**边
 * (挂上面放不下就自动改挂下面)。
 *
 * 刻意**不吃**侧栏整体透明度 s_Alpha: 用 SwsFillRawLocked 而不是
 * SwsFillLocked, 文字的 BlitCoverage 也直接给 100/85 而不是乘 s_Alpha。
 * 提示框是玩家**此刻正在读**的信息, 侧栏整体调到 Alpha=30 时它要是也跟着
 * 淡到 30%, 就看不清是哪个超武了。它是侧栏里唯一无视 Alpha 的元素。
 *
 * 顺带记一下两个绘制函数的约定是相反的 (踩过):
 *   SwsFillLocked  内部乘 s_Alpha -> 只能给原始不透明度
 *   BlitCoverage   不乘 s_Alpha   -> 调用方才预乘
 * 早先提示框给 SwsFillLocked 传了 70 * s_Alpha / 100, 等于乘两次,
 * Alpha=30 时底框实际只剩 6%, 几乎看不见。
 * 字体跟随全局矢量字体设置 ([VecText] FontFace/FontPx/Bold)。 */
static void SwsDrawTip(unsigned char* locked, int pitch, int w, int h,
                       const wchar_t* nm, int ax, int ayUp, int ayDn)
{
    int sl = 0, sw, sx, ny, adv, gpx;
    if (!locked || !nm || !nm[0]) return;
    gpx = VecText_FontPx();
    if (gpx <= 0) gpx = 18;
    if (!GdiEnsure(gpx, VecText_Bold(), 5, VecText_FontFace())) return;
    adv = g_G->adv > 0 ? g_G->adv : gpx + 2;
    while (sl < 23 && nm[sl]) sl++;
    sw = sl ? RenderLine(nm, sl) : 0;
    if (sw <= 0) return;
    sx = ax - sw / 2;
    ny = ayUp - adv - 2;
    if (ny < 2) ny = ayDn;
    /* 框比文字左右各宽 2px, 夹取要按**框**算, 否则右边会跑出屏幕 */
    if (sx < 4) sx = 4;
    if (sx + sw + 6 > w - 2) sx = (w - sw - 8 > 4) ? w - sw - 8 : 4;
    {
        int bx = sx - 2, by = ny - 2, bwid = sw + 6, bhei = adv + 4;
        SwsFillRawLocked(locked, pitch, w, h, bx, by, bwid, bhei, 0, 0, 0, 150);
        SwsFillRawLocked(locked, pitch, w, h, bx, by, bwid, 1,
                         SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
        SwsFillRawLocked(locked, pitch, w, h, bx, by + bhei - 1, bwid, 1,
                         SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
        SwsFillRawLocked(locked, pitch, w, h, bx, by, 1, bhei,
                         SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
        SwsFillRawLocked(locked, pitch, w, h, bx + bwid - 1, by, 1, bhei,
                         SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
    }
    BlitCoverage(locked, pitch, w, h, sx + 1, ny + 1, sw, adv, 0, 0, 0, 85);
    BlitCoverage(locked, pitch, w, h, sx, ny, sw, adv, 255, 255, 255, 100);
}


void SWS_Draw(void* surf, int w, int h, int pitch)
{
    /* 精简绘制 (为帧率):
     *   - 一列一个底框 + 一列一组边框, 不再每个超武画一个方框
     *   - 不画"就绪"等状态字 (只在鼠标悬停时画名字)
     *   - 悬停项: 高亮框 + 名字文本
     * 之前每条目 6 次填充 + 未就绪罩 + 状态字渲染, 64 个条目时是主要开销。
     */
    int order[SWS_CAP];
    int colCount[4] = { 0, 0, 0, 0 };
    int colX[4] = { 0, 0, 0, 0 };
    int i, n, f, totW, xStart, yStart, rowH, maxRows, blockH;
    int curSel = -9999;
    int mx = -1, my = -1;
    int hover = -1;
    int dedupOn = 0;
    unsigned char* vt;
    SwsLockFn lockFn;
    SwsUnlockFn unlockFn;
    unsigned char* locked;

    if (!s_Enable) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    GtMouseGameXY(&mx, &my);

    /* ---- 侧栏隐藏时: 只画展开箭头, 其余全跳过 ----
     * 箭头**必须留着** —— 连它一起藏掉的话, 玩家就找不到再展开的地方了
     * (这正是"箭头隐藏了点哪里展开?"那个问题)。所以这条早退放在拿到锁面
     * 之后、画超武之前, 而不是像原来那样连箭头一起 return 掉。 */
    if (!s_Visible)
    {
        unsigned char* lk;
        unsigned char* v2;
        v2 = *(unsigned char**)surf;
        if (!v2 || !IsPlausiblePtr(v2)) return;
        lockFn = (SwsLockFn)(*(void**)(v2 + VT_LOCK));
        unlockFn = (SwsUnlockFn)(*(void**)(v2 + VT_UNLOCK));
        if (!lockFn || !unlockFn ||
            !IsPlausiblePtr((const void*)lockFn) ||
            !IsPlausiblePtr((const void*)unlockFn)) return;
        lk = (unsigned char*)lockFn(surf, NULL, 0, 0);
        if (!lk || !IsPlausiblePtr(lk)) return;
        {
            /* 纵向位置沿用同一套 TopMargin 规则, 但**不减半个块高** ——
             * 隐藏时没有块, 减去半高会让箭头偏上, 与展开后的位置对不上。 */
            int by;
            if (s_TopIsPct) by = (int)(((long)h * (long)s_TopMargin) / 100000L);
            else            by = s_TopMargin;
            if (by < 0) by = 0;
            SwsCollapsedBtnGeom(w, by);
        }
        SwsDrawArrow(lk, pitch, w, h, s_BtnX, s_BtnY, s_BtnW, s_BtnH, 0, mx, my);
        unlockFn(surf, NULL);
        return;
    }

    n = s_ItemN;
    /* 一个超武都没有时整块不画, 两个按钮也就没画出来 —— 必须把 s_HidValid
     * 清掉, 否则 SWS_Poll 会拿上一帧的旧几何把点击判到"隐藏重复"上。 */
    s_HidValid = 0;
    if (n <= 0 || n > SWS_CAP) return;

    /* ---- 排序: 阵营 -> ini 优先级 -> ArrayIndex ---- */
    for (i = 0; i < n; i++) order[i] = i;
    for (i = 1; i < n; i++)
    {
        int t = order[i], k = i - 1;
        while (k >= 0)
        {
            SwsItem* a = &s_Items[order[k]];
            SwsItem* b = &s_Items[t];
            int gt = 0;
            if (a->side != b->side) gt = a->side > b->side;
            else if (a->prio != b->prio) gt = a->prio > b->prio;
            else gt = a->swIdx > b->swIdx;
            if (!gt) break;
            order[k + 1] = order[k];
            k--;
        }
        order[k + 1] = t;
    }
    for (i = 0; i < n; i++) colCount[s_Items[order[i]].side]++;
    maxRows = 0;
    for (f = 0; f < 4; f++)
    {
        if (colCount[f] > maxRows) maxRows = colCount[f];
    }
    if (maxRows <= 0) return;
    /* 超过上限的列用滚动条: 可见行数取 min */
    if (s_MaxRows > 0 && maxRows > s_MaxRows) maxRows = s_MaxRows;
    /* 夹紧滚动偏移: 最大偏移 = 最长列条目数 - 可见行数 (滚到最后一屏就停,
     * 早先写成 colCount-1 会一路滚到只剩最后一行, 中间全空。 */
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
    /* 有列装不下时, 整块左边让出 5px 给右侧那根滚动条 (否则会顶出屏幕)。
     * 纵向要多留 SWS_BTN_H+2 给块顶上方的两个按钮 (见 SwsTwoBtnGeom)。 */
    s_BarValid = (s_MaxSk > 0);
    s_BarW = s_BarValid ? 4 : 0;
    xStart = w - s_RightMargin - totW - (s_BarValid ? 5 : 0);
    if (xStart < 0) xStart = 0;
    /* 比例 = 屏幕高 * 比例, 再减去半个块高 -> 视觉上居中 (块不会偏上);
     * 像素 = 直接当 Y 坐标。 */
    if (s_TopIsPct)
    {
        yStart = (int)(((long)h * (long)s_TopMargin) / 100000L) - blockH / 2;
    }
    else
    {
        yStart = s_TopMargin;
    }
    /* 块顶上方要放两个按钮, 像素模式下把它一起顶上去 (比例模式下按半块高
     * 居中, 两个按钮多半还在屏幕里, 夹取在 SwsTwoBtnGeom 里做)。 */
    if (yStart < SWS_BTN_H + 2) yStart = SWS_BTN_H + 2;
    {
        int cx = xStart;
        for (f = 0; f < 4; f++)
        {
            if (colCount[f] <= 0) continue;
            colX[f] = cx;
            cx += SWS_ICON_W + s_ColGap;
        }
    }
    {
        int rows[4] = { 0, 0, 0, 0 };
        for (i = 0; i < n; i++)
        {
            SwsItem* it = &s_Items[order[i]];
            int r = rows[it->side] - s_Scroll;
            it->x = colX[it->side];
            it->y = yStart + r * rowH;
            it->row = rows[it->side];
            rows[it->side]++;
            /* 滚出可见范围的条目不参与命中/绘制 */
            if (r < 0 || r >= maxRows) it->x = -1;
        }
    }
    /* 两个按钮: 显隐 (▶ 收起) 在右, 隐藏重复在左, 挂在超武块**正上方**一行、
     * 右对齐到块的右缘。挂块顶上方是落在栏区判定框之外的, 所以 SWS_Poll 里
     * 它们的命中判定必须排在栏区判定**之前** (见 SwsTwoBtnGeom 注释)。 */
    SwsTwoBtnGeom(xStart, totW, w, yStart);
    {
        static int s_LayLog = 0;
        if (!s_LayLog)
        {
            char sb[160], b1[16], b2[16], b3[16], b4[16], b5[16], b6[16];
            unsigned q = AppendCStr(sb, 0, "sws: lay cap=", sizeof(sb));
            UtoA((unsigned)s_MaxRows, b1);
            q = AppendCStr(sb, q, b1, sizeof(sb));
            q = AppendCStr(sb, q, " vis=", sizeof(sb));
            UtoA((unsigned)maxRows, b2);
            q = AppendCStr(sb, q, b2, sizeof(sb));
            q = AppendCStr(sb, q, " cnt=", sizeof(sb));
            UtoA((unsigned)colCount[0], b3);
            q = AppendCStr(sb, q, b3, sizeof(sb));
            q = AppendCh(sb, q, '/', sizeof(sb));
            UtoA((unsigned)colCount[1], b4);
            q = AppendCStr(sb, q, b4, sizeof(sb));
            q = AppendCh(sb, q, '/', sizeof(sb));
            UtoA((unsigned)colCount[2], b5);
            q = AppendCStr(sb, q, b5, sizeof(sb));
            q = AppendCh(sb, q, '/', sizeof(sb));
            UtoA((unsigned)colCount[3], b6);
            q = AppendCStr(sb, q, b6, sizeof(sb));
            q = AppendCStr(sb, q, " n=", sizeof(sb));
            UtoA((unsigned)n, b1);
            q = AppendCStr(sb, q, b1, sizeof(sb));
            LogLine(sb);
            s_LayLog = 1;
        }
    }
    s_BoundX = xStart;
    s_BoundY = yStart;
    s_BoundW = totW;
    s_BoundH = blockH;
    s_BoundValid = 1;
    s_BoundsKnown = 1;   /* 吞没门用: 不随 SwsScan 清零 (见声明处注释) */
    for (f = 0; f < 4; f++) { s_ColCount[f] = colCount[f]; s_ColX[f] = colX[f]; }
    s_YStart = yStart;
    s_RowH = rowH;
    s_VisRows = maxRows;
    /* 滚轮用的几何 (SWS_WheelIn 据此判断命中与能否再滚) */
    s_ColsValid = 1;
    s_NCols = 0;
    for (f = 0; f < 4; f++)
    {
        s_Cols[f].count = colCount[f];
        s_Cols[f].x = colX[f];
        s_Cols[f].vis = (colCount[f] < maxRows) ? colCount[f] : maxRows;
        if (colCount[f] > 0) { s_NCols++; s_LastX = colX[f]; }
    }
    s_BarX = s_LastX + SWS_ICON_W + 1;
    if (!g_Calibrated) Calibrate();

    /* 悬停项 (悬停时才画文字, 平时零文本开销) */
    GtMouseGameXY(&mx, &my);
    for (i = 0; i < n; i++)
    {
        SwsItem* it = &s_Items[order[i]];
        if (it->x < 0) continue;                 /* 被滚动裁掉 */
        if (mx >= it->x && mx < it->x + SWS_ICON_W &&
            my >= it->y && my < it->y + SWS_ICON_H)
        {
            hover = i;
            break;
        }
    }
    if (IsPlausiblePtr((void*)ADDR_CURSW)) curSel = *(int*)ADDR_CURSW;

    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    lockFn = (SwsLockFn)(*(void**)(vt + VT_LOCK));
    unlockFn = (SwsUnlockFn)(*(void**)(vt + VT_UNLOCK));
    if (!lockFn || !unlockFn ||
        !IsPlausiblePtr((const void*)lockFn) ||
        !IsPlausiblePtr((const void*)unlockFn)) return;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return;

    /* ---- 每列一个底框 (图标之前) ---- */
    for (f = 0; f < 4; f++)
    {
        int ch;
        if (colCount[f] <= 0) continue;
        /* 底框只画可见行数 (超出部分不显示) */
        ch = (colCount[f] < maxRows ? colCount[f] : maxRows) * rowH - s_Gap;
        SwsFillLocked(locked, pitch, w, h, colX[f], yStart, SWS_ICON_W, ch,
                      0, 0, 0, s_Opacity);
    }

    /* ---- 图标 + 悬停高亮 ---- */
    for (i = 0; i < n; i++)
    {
        SwsItem* it = &s_Items[order[i]];
        void* ps = NULL;
        if (it->x < 0) continue;              /* 被滚动裁掉 */
        if (it->pcxFile) ps = SwsPcxLiveSurf(it->pcxFile);
        if (ps)
        {
            /* 优先在已持有的锁面里直接写像素; 布局对不上才退回引擎 blit */
            if (!SwsPcxBlitLocked((unsigned short*)locked, pitch, w, h,
                                  it->x, it->y, SWS_ICON_W, SWS_ICON_H, ps))
            {
                void* pcxInst = (void*)ADDR_PCX_INSTANCE;
                if (pcxInst && IsPlausiblePtr(pcxInst))
                {
                    char rect[16];
                    unlockFn(surf, NULL);
                    *(int*)(rect + 0) = it->x; *(int*)(rect + 4) = it->y;
                    *(int*)(rect + 8) = SWS_ICON_W; *(int*)(rect + 12) = SWS_ICON_H;
                    FN_PCXBLIT(pcxInst, rect, surf, ps, PCX_TRANS_PARENT);
                    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
                    if (!locked || !IsPlausiblePtr(locked)) return;
                }
            }
            /* 未就绪 (释放后充能中): 压暗。只对这类条目做, 通常 0~1 个。 */
            if (!it->ready)
                SwsFillLocked(locked, pitch, w, h, it->x, it->y, SWS_ICON_W,
                              SWS_ICON_H, 0, 0, 0, 45);
        }
        else if (it->shp)
        {
            /* 无 SidebarPCX 的超武: SidebarImage 是引擎的 XXICON 占位图,
             * 画出来是紫球, 所以不画 (可在 [SWSidebar] 里用负值隐藏)。 */
        }
        /* 充能百分比: 画在图标底部 (有 PCX 图标才画, 否则只占位) */
        if (it->pcxFile)
        {
            wchar_t st[8];
            int sl = 0, sw, sx, ny, adv;
            /* 必须清满整个数组: 只清前 4 个的话, 第 5 个是栈上垃圾, 非零时
             * sl 会变长, RenderLine 就把 "100%" 后面的垃圾字节也画出来 ——
             * 那就是一直去不掉的 '<' 碎片。 */
            st[0] = st[1] = st[2] = st[3] = 0;
            st[4] = st[5] = st[6] = st[7] = 0;
            /* 就绪 (100%) 不显示任何字: 图标本身就是"可用"的意思, 再叠一层
             * 100% 只是多一次 GDI 文本渲染, 还压住图标。 */
            if (it->susp)
            {
                st[0] = 'H'; st[1] = 'o'; st[2] = 'l'; st[3] = 'd';
            }
            else if (!it->ready && it->total > 0)
            {
                int pct = 100 - (it->remain * 100) / it->total;
                if (pct < 0) pct = 0;
                if (pct > 99) pct = 99;      /* 满格就是就绪, 由上面分支处理 */
                /* 按位数输出: 早先固定写三字符, 1% 就成了 "001%"。 */
                if (pct >= 10)
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
            adv = g_G ? (g_G->adv > 0 ? g_G->adv : s_FontPx + 2) : s_FontPx + 2;
            sw = sl ? RenderLine(st, sl) : 0;
            if (sw > 0)
            {
                /* 顶部显示。两趟都用 sw 宽度: 多画 2px 会切出字形 overhang
                 * 的碎片 (形似 '<'; 微软雅黑 extent 非矩形, 见 GdiText 注释)。
                 * BlitCoverage 自己不乘 s_Alpha, 所以这里要预乘。 */
                sx = it->x + (SWS_ICON_W - sw) / 2;
                ny = it->y + 1;
                if (sx < it->x) sx = it->x;
                BlitCoverage(locked, pitch, w, h, sx + 1, ny + 1, sw, adv,
                             0, 0, 0, 80 * s_Alpha / 100);
                BlitCoverage(locked, pitch, w, h, sx, ny, sw, adv,
                             255, 255, 255, 100 * s_Alpha / 100);
            }
        }
        if (i == hover || it->swIdx == curSel)
        {
            const unsigned char* hc = (i == hover) ?
                (const unsigned char*)"\xFF\xFF\xFF" : kSideCol[it->side];
            SwsFillLocked(locked, pitch, w, h, it->x - 1, it->y - 1,
                          SWS_ICON_W + 2, 1, hc[0], hc[1], hc[2], 220);
            SwsFillLocked(locked, pitch, w, h, it->x - 1, it->y + SWS_ICON_H,
                          SWS_ICON_W + 2, 1, hc[0], hc[1], hc[2], 220);
            SwsFillLocked(locked, pitch, w, h, it->x - 1, it->y, 1, SWS_ICON_H,
                          hc[0], hc[1], hc[2], 220);
            SwsFillLocked(locked, pitch, w, h, it->x + SWS_ICON_W, it->y, 1,
                          SWS_ICON_H, hc[0], hc[1], hc[2], 220);
        }
    }

    /* ---- 每列边框 (画在图标之后, 否则会被 60x48 的图标盖住) ---- */
    for (f = 0; f < 4; f++)
    {
        int ch;
        const unsigned char* c;
        if (colCount[f] <= 0) continue;
        c = kSideCol[f];
        ch = (colCount[f] < maxRows ? colCount[f] : maxRows) * rowH - s_Gap;
        SwsFillLocked(locked, pitch, w, h, colX[f], yStart, SWS_ICON_W, 1,
                      c[0], c[1], c[2], 150);
        SwsFillLocked(locked, pitch, w, h, colX[f], yStart + ch - 1, SWS_ICON_W, 1,
                      c[0], c[1], c[2], 150);
        SwsFillLocked(locked, pitch, w, h, colX[f], yStart, 1, ch,
                      c[0], c[1], c[2], 150);
        SwsFillLocked(locked, pitch, w, h, colX[f] + SWS_ICON_W - 1, yStart, 1, ch,
                      c[0], c[1], c[2], 150);
    }

    /* ---- 滚动条: 参照战役框 (MOExperience.cpp DrawBox 战役分支) ----
     * 整块右侧一根: 4px 宽, 轨道 alpha 55 / 滑块 alpha 90, 压暗不抢眼。
     * 高度覆盖整个超武块, 滑块高度按最长列的条目数算。 */
    if (s_BarValid)
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
            SwsFillLocked(locked, pitch, w, h, s_BarX, barY, s_BarW, barH,
                          30, 30, 30, 55);
            SwsFillLocked(locked, pitch, w, h, s_BarX, thumbY, s_BarW, thumbH,
                          170, 170, 170, 90);
        }
    }

    /* ---- 显隐按钮: 向右箭头 ▶ (收起), 贴在超武块右侧 ----
     * 侧栏显示时用 ▶ (往右收); 隐藏时只留一个 ◀ (往左展开) 贴在屏幕右缘,
     * 见 SWS_Draw 开头的 !s_Visible 分支 —— 箭头任何时候都不能藏,
     * 否则藏起来之后就找不到再展开的地方了。 */
    SwsDrawArrow(locked, pitch, w, h, s_BtnX, s_BtnY, s_BtnW, s_BtnH,
                 1, mx, my);
    /* ---- 隐藏重复按钮: 禁入圆牌, 紧挨在显隐按钮**左边** ----
     * (几何与命中判定见 SwsTwoBtnGeom / SWS_Poll)。
     *
     * 位置注意: SWS_Draw 拿到的 surf 是**合成面**, 宽度 = 屏幕宽 - 原版侧栏
     * 宽 (1920 时是 1752, 见日志 "draw: ok dual surf=1752x1080")。所以
     * 侧栏块的右缘已经紧贴原版侧栏, 右边一个像素都放不下 —— 两个按钮只能
     * 挂在块**上方**。曾经挂在右边, 结果画到合成面外面去了, 看着像"没有"。 */
    dedupOn = s_DedupHide;
    SwsDrawDedup(locked, pitch, w, h, s_HidX, s_HidY, s_BtnW, s_BtnH,
                 dedupOn, mx, my);

    /* ---- 悬停名字 (带底框, 跟随全局矢量字体) ----
     * 隐藏重复按钮**不**出文字提示: 它的禁入牌 + 颜色已经说明状态, 而
     * 自己拼的中文字面量一旦字形链路有问题就是一片 '?' (踩过一次)。 */
    if (hover >= 0)
    {
        SwsItem* it = &s_Items[order[hover]];
    SwsDrawTip(locked, pitch, w, h, it->name, mx,
                   it->y, it->y + SWS_ICON_H + 2);
    }
    unlockFn(surf, NULL);

}
