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
/* ChargeDrainState@0x7C (由 SuperClass.h 布局自 CanHold@0x60 /
 * ChronoMapCoords@0x62 / Animation@0x68 三个已知锚点推出):
 *   -1=None 0=Charging 1=Ready 2=Draining (GeneralDefinitions.h) */
#define OFF_SUP_DRAIN    0x7Cu

#define OFF_SWT_ID       0x24u   /* 与 Super 同源 +12 漂移, 待 id 计数验证 */
#define OFF_SWT_UINAME   0x60u
#define OFF_SWT_ARRIDX   0x98u
#define OFF_SWT_RECH     0xB0u
#define OFF_SWT_ACTION   0xBCu   /* Action (Action::None=0 => 点击即生效) */
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
#define FN_DRAWSHAPE ((SwsDrawShapeFn)(void*)0x4AED70u)
#define ADDR_CAMEO_PAL   0x87F6B0u
#define ADDR_SIDEBAR_PAL 0x87F6CCu
#define SWS_BF_400 0x400
#define SWS_ZGROUND 0


#define VT_FILL_RECT_TRANS 0x1C
#define VT_LOCK            0x5C
#define VT_UNLOCK          0x60

#define ADDR_CLICKGATE      0x00692419u
#define ADDR_CLICKGATE_SKIP 0x006925FCu

#define SWS_CAP 160
#define SWS_INI_CAP 160

/* ---------------- 配置 ---------------- */

static int s_Init = 0;
static int s_Enable = 1;
static int s_Opacity = 60;
static int s_IconW = 60, s_IconH = 48;
static int s_Gap = 2, s_ColGap = 6;
static int s_RightMargin = 2;
static int s_CenterY = 1, s_TopMargin = 80;
static int s_FontPx = 12, s_Bold = 0;
static char s_FontFace[64];
static wchar_t s_Face[FONT_FACE_MAX];
static int s_ClickFire = 1;
/* Learn mode: diff the whole Display object across frames and dump every
 * offset the engine itself touches when the player picks/releases a real
 * superweapon cameo. That is the ground truth we then replay ourselves. */
static int s_Learn = 0;
static int s_PcxOn = 0;   /* engine PCX path switch (crash isolation) */
static int s_WeSetCur = 0;   /* our own write happened this frame */
static OU_Hotkey s_ToggleHk;
static int s_Visible = 1;
static int s_PrevTog = 0, s_PrevLB = 0, s_PrevRB = 0;
/* Pending selection, applied one frame later.
 * Reason: we are not a Gadget, so the engine still sees the very same
 * left-press in ProcessClickCoords later this frame. Writing CurrentSW
 * right away makes the engine release the super at that spot instantly.
 * Phobos avoids this by being a real Gadget (the click is consumed by
 * Gadget::Action before the map sees it). We reproduce that by deferring
 * the write and swallowing the press through the 0x692419 gate. */
static int s_PendSel = -2;   /* -2 none, -1 cancel, >=0 SW ArrayIndex */
static int s_EatPress = 0;   /* gate: swallow this frame's click */
/* 物理左键按住期间一直吞掉引擎的地图点击。
 * 实测 (日志): 0x692419 门工作正常 (鼠标在栏内时返回跳过), 但"等松开再
 * 上膛"永远等不到松开边沿, 每次都走 60 帧兜底, 而兜底触发时按键还按着
 * —— 引擎看到"已选中 + 左键按住"就当场释放。这才是原地释放的真正原因。
 * 现在改成: 按下即上膛, 同时把整段按住期间都吞掉, 效果等同真 Gadget
 * 消费掉这次完整点击。 */
static int s_HoldSkip = 0;
/* 即时超武: 走 SpecialPlace 事件, 定义在下方"绘制"之前。 */
static int SwsPushSpecialPlace(int swIdx);
/* 诊断开关: 全部在 SwsEnsureInit 里读一次。
 * 之前 SwsFireWatch 每帧 GetPrivateProfileIntA 一次 = 每帧一次 ini 文件
 * I/O, 而且它在 SWS_Poll 里, 落在 perf 日志的 d/s 计时之外, 帧率掉了
 * 日志却完全看不出来。 */
static int s_ForceScan = 0;     /* 点击时置 1: 强制立刻重扫, 不等 15 帧 */
static int s_EvWatchOn = 1;     /* [SWSidebar] EvWatch */
static int s_FireWatchOn = 1;   /* [SWSidebar] FireWatch */
static int s_ScrollStep = 1;    /* [SWSidebar] ScrollStep */
/* OutList 抓包 (定义在下方): 引擎事件队列的地址, 供 SwsEventWatch 用。 */
#define SWS_ADDR_OUT_COUNT  0xA802C8u
#define SWS_ADDR_OUT_NEXT   0xA802D0u
#define SWS_ADDR_OUT_BUF    0xA802D4u
#define SWS_ADDR_OUT_PAR    0xA83A54u
#define SWS_EVT_SIZE        111u
#define SWS_EVT_CAP         0x80u
#define SWS_EVT_SLOTS_N     24
static void SwsEventWatch(void);
static void SwsEvCheck(void);
static void SwsEvNote(unsigned slot);
static void SwsFireWatch(void);
static void SwsDrainWatch(void);
/* Arm on mouse *release*, not on press. RA2 tests "left button held" for
 * superweapon placement, so arming while the button is still down fires
 * the super at that spot. Phobos dodges this because a real Gadget eats
 * the press before the map sees it; we have no Gadget, so we arm on the
 * release edge instead: press = select intent, release = enter targeting,
 * next press = place. Identical to the stock sidebar from the user's side. */

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
    int drain;            /* 1 = UseChargeDrain: 长期生效, 点击是开关 */
    int unstoppable;      /* 1 = SW.Unstoppable: 生效中不许手动关 */
    int drainState;       /* ChargeDrainState: -1/0=Charging 1=Ready 2=Draining */
    int row;              /* 该条目在本列的行号 (滚动用) */
    char id[32];          /* 超武 ID (诊断表用: 名字<->swIdx 对照) */
    wchar_t name[24];     /* 悬停提示用; 每类型填一次 */
} SwsItem;

static SwsItem s_Items[SWS_CAP];
static int s_ItemN = 0;
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
            SwsStrEq(k, "ToggleKey") || SwsStrEq(k, "ClickFire") ||
            SwsStrEq(k, "IconW") || SwsStrEq(k, "IconH") ||
            SwsStrEq(k, "Gap") || SwsStrEq(k, "ColGap") ||
            SwsStrEq(k, "RightMargin") || SwsStrEq(k, "CenterY") ||
            SwsStrEq(k, "TopMargin") || SwsStrEq(k, "FontPx") ||
            SwsStrEq(k, "Bold") || SwsStrEq(k, "FontFace") ||
            SwsStrEq(k, "Learn") || SwsStrEq(k, "MaxRows") ||
            SwsStrEq(k, "Pcx");
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
    s_IconW = GetPrivateProfileIntA(SWS_SECTION, "IconW", 60, g_IniPath);
    s_IconH = GetPrivateProfileIntA(SWS_SECTION, "IconH", 48, g_IniPath);
    s_Gap = GetPrivateProfileIntA(SWS_SECTION, "Gap", 2, g_IniPath);
    s_ColGap = GetPrivateProfileIntA(SWS_SECTION, "ColGap", 0, g_IniPath);
    s_RightMargin = GetPrivateProfileIntA(SWS_SECTION, "RightMargin", 2, g_IniPath);
    s_CenterY = GetPrivateProfileIntA(SWS_SECTION, "CenterY", 1, g_IniPath) ? 1 : 0;
    s_TopMargin = GetPrivateProfileIntA(SWS_SECTION, "TopMargin", 80, g_IniPath);
    s_FontPx = GetPrivateProfileIntA(SWS_SECTION, "FontPx", 12, g_IniPath);
    s_Bold = GetPrivateProfileIntA(SWS_SECTION, "Bold", 0, g_IniPath) ? 1 : 0;
    OU_ReadString(SWS_SECTION, "FontFace", "msyh.ttc",
                  s_FontFace, sizeof(s_FontFace));
    s_ClickFire = GetPrivateProfileIntA(SWS_SECTION, "ClickFire", 1, g_IniPath) ? 1 : 0;
    s_Learn = GetPrivateProfileIntA(SWS_SECTION, "Learn", 0, g_IniPath) ? 1 : 0;
    s_PcxOn = GetPrivateProfileIntA(SWS_SECTION, "Pcx", 0, g_IniPath) ? 1 : 0;
    s_MaxRows = GetPrivateProfileIntA(SWS_SECTION, "MaxRows", 10, g_IniPath);
    /* 诊断开关只读一次: 这两个在 SWS_Poll 每帧调用, 现读 ini 就是每帧一次
     * 文件 I/O, 而且在 perf 计时范围之外, 会让帧率掉了日志却看不出来。 */
    s_EvWatchOn = GetPrivateProfileIntA(SWS_SECTION, "EvWatch", 1, g_IniPath);
    s_FireWatchOn = GetPrivateProfileIntA(SWS_SECTION, "FireWatch", 1, g_IniPath);
    s_ScrollStep = GetPrivateProfileIntA(SWS_SECTION, "ScrollStep", 1, g_IniPath);
    if (s_ScrollStep < 1) s_ScrollStep = 1;
    if (s_ScrollStep > 10) s_ScrollStep = 10;
    if (s_MaxRows < 1) s_MaxRows = 1;
    if (s_MaxRows > 40) s_MaxRows = 40;
    OU_ParseHotkey("F4", &s_ToggleHk);
    OU_ReadHotkey(SWS_SECTION, "ToggleKey", &s_ToggleHk);
    if (s_Opacity < 0) s_Opacity = 0;
    if (s_Opacity > 100) s_Opacity = 100;
    if (s_IconW < 40) s_IconW = 40;
    if (s_IconW > 120) s_IconW = 120;
    if (s_IconH < 24) s_IconH = 24;
    if (s_IconH > 96) s_IconH = 96;
    if (s_Gap < 0) s_Gap = 0;
    if (s_Gap > 16) s_Gap = 16;
    if (s_ColGap < 0) s_ColGap = 0;
    if (s_ColGap > 32) s_ColGap = 32;
    if (s_FontPx < 8) s_FontPx = 8;
    if (s_FontPx > 32) s_FontPx = 32;
    SwsLoadIniList();
    ResolveFontFace(s_FontFace, "sws", s_Face, L"Microsoft YaHei");
    p = 0;
    p = AppendCStr(buf, p, "sws: en=", sizeof(buf));
    p += UtoA((unsigned)s_Enable, buf + p);
    p = AppendCStr(buf, p, " op=", sizeof(buf));
    p += UtoA((unsigned)s_Opacity, buf + p);
    p = AppendCStr(buf, p, " click=", sizeof(buf));
    p += UtoA((unsigned)s_ClickFire, buf + p);
    p = AppendCStr(buf, p, " iniN=", sizeof(buf));
    p += UtoA((unsigned)s_IniN, buf + p);
    p = AppendCStr(buf, p, " pcx=", sizeof(buf));
    p += UtoA((unsigned)s_PcxOn, buf + p);
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
    int   drain;        /* 1 = UseChargeDrain 长期生效 */
    int   unstoppable;  /* 1 = SW.Unstoppable */
    const char* pcxFile;
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
    s_TC[i].drain = 0;
    s_TC[i].unstoppable = 0;
    s_TC[i].pcxFile = NULL;
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
 * 另外长期生效类 (UseChargeDrain=yes: Blasticade / PsychicFlash) 点击是
 * **开关**而非一次性发射, 侧栏要显示 Active, 见 SwsItem::drainState。
 */
typedef struct {
    char id[32];
    int instant;    /* 1 = 点击即发射, 无需选格 */
    int drain;      /* 1 = UseChargeDrain (长期生效, 点击是开关) */
    int unstoppable;/* 1 = SW.Unstoppable (生效中不许手动关) */
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
    s_Beh[slot].unstoppable = 0;
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
        if (SwsIniYes(rules, id, "SW.Unstoppable")) s_Beh[slot].unstoppable = 1;
    }
    return &s_Beh[slot];
}

static const char* SwsPcxFileOf(const char* id)
{
    int i, slot = -1;
    void* rules;
    char buf[64];
    if (!s_PcxOn) return NULL;
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

/* ---------------- 性能自测 (QPC, GetTickCount 只有 15.6ms 分辨率) ---------------- */
static LARGE_INTEGER g_Qpf;
static LONGLONG g_ScanStart;
static double g_ScanUsAcc = 0.0;
static int g_ScanCnt = 0;

static void SwsPerfInit(void)
{
    if (!g_Qpf.QuadPart) QueryPerformanceFrequency(&g_Qpf);
}

static double SwsUsSince(LONGLONG t0)
{
    LARGE_INTEGER now;
    if (!g_Qpf.QuadPart || !t0) return 0.0;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - t0) * 1000000.0 / (double)g_Qpf.QuadPart;
}

static void SwsScan(void)
{
    SwsPerfInit();
    g_ScanStart = 0;
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        g_ScanStart = t.QuadPart;
    }
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
int drain = 0;
int unstoppable = 0;
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
             * SW.ManualFire、SidebarPCX 文件名。类型对象静态, 只算一次。 */
            SwsTypeCache* tc = SwsTypeEntry(type, id);
            if (!tc) continue;
            if (tc->swIdx < 0) continue;
            if (tc->prio < 0)
            {
                int h = 0;
                tc->side = SwsSideOf(id);
                tc->prio = SwsPrioOf(id, &h);
                tc->hide = h;
                tc->manual = SwsManualFireOk(id);
                tc->pcxFile = SwsPcxFileOf(id);
                {
                    /* 悬停提示文本: 优先 UIName, 空则退回 ID */
                    const wchar_t* uin = *(const wchar_t**)((char*)type +
                                                            OFF_SWT_UINAME);
                    wchar_t* dst = s_Items[0].name;   /* 只借个缓冲写下面 */
                    (void)dst;
                    SwsCopyName(s_Items[n].name, uin, 24);
                    if (!s_Items[n].name[0])
                    {
                        int k = 0;
                        while (k < 23 && id[k]) { s_Items[n].name[k] =
                            (wchar_t)(unsigned char)id[k]; k++; }
                        s_Items[n].name[k] = 0;
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
                /* "点击即生效" 的判据取自 rules INI, 不再用
                 * `Action==0` —— 那个判据是错的 (见 SwsBehaviorOf 注释):
                 * Action=Custom 会被 Ares 改写成 0x7F, 而 MO 这六个全是
                 * Custom。真正的判据是 `Action=None` 或 `SW.UseAITargeting`。
                 * 同时把长期生效 (UseChargeDrain) 与 Unstoppable 一起取回来,
                 * 侧栏要按 ChargeDrain 三态显示。 */
                {
                    SwsBehavior* bh = SwsBehaviorOf(id);
                    tc->instant = bh ? bh->instant : 0;
                    tc->drain = bh ? bh->drain : 0;
                    tc->unstoppable = bh ? bh->unstoppable : 0;
                }
                drain = tc->drain;
                unstoppable = tc->unstoppable;
            }
            /* 支援技能与超武同分类, 一并显示; 只排除 SW.ManualFire=no。
             * 不能拿引擎 ShowTimers 集合当显示清单 —— 它只有 11 个, 是
             * "正在计时"的子集, 不是侧栏该有的全量。 */
            if (tc->hide || !tc->manual) continue;
            swIdx = tc->swIdx;
            side = tc->side;
            prio = tc->prio;
            pcxFile = tc->pcxFile;
            instant = tc->instant;
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
        s_Items[n].drain = drain;
        s_Items[n].unstoppable = unstoppable;
        s_Items[n].drainState = *(int*)((char*)sup + OFF_SUP_DRAIN);
        n++;
    }
    s_ItemN = n;
    g_ScanUsAcc += SwsUsSince(g_ScanStart);
    g_ScanCnt++;
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

/* ---------------- 轮询 (切换键 + 点击 + 扫描) ---------------- */


/* ---------------- 学习模式: 抄引擎真实点击改了哪些字段 ---------------- */

#define ADDR_DISPLAY    0x87F7E8u
#define DISP_REGION     0x1400u

static void SwsLearnTick(void)
{
    static unsigned char shadow[DISP_REGION];
    static int primed = 0;
    static int logged = 0;
    static int prevCur = -99;
    unsigned char* d;
    int cur, i, n;
    if (!s_Learn || logged >= 3) return;
    d = (unsigned char*)ADDR_DISPLAY;
    if (!IsPlausiblePtr((void*)ADDR_DISPLAY)) return;
    cur = *(int*)ADDR_CURSW;
    if (!primed)
    {
        for (i = 0; i < (int)DISP_REGION; i++) shadow[i] = d[i];
        primed = 1;
        prevCur = cur;
        return;
    }
    /* 只在"引擎自己"改变选中态的那一帧报 (排除我们自己写的)。 */
    if (!s_WeSetCur && cur != prevCur)
    {
        char sb[512], hx[16];
        unsigned q = 0;
        n = 0;
        q = AppendCStr(sb, q, cur >= 0 ? "sws: learn SEL " : "sws: learn CLR ",
                       sizeof(sb));
        UtoA((unsigned)(prevCur < 0 ? 0 : prevCur), hx);
        q = AppendCStr(sb, q, hx, sizeof(sb));
        q = AppendCh(sb, q, '>', sizeof(sb));
        UtoA((unsigned)(cur < 0 ? 0 : cur), hx);
        q = AppendCStr(sb, q, hx, sizeof(sb));
        for (i = 0; i < (int)DISP_REGION && n < 14 && q < 470; i += 4)
        {
            unsigned a, b2;
            a = *(unsigned*)(shadow + i);
            b2 = *(unsigned*)(d + i);
            if (a == b2) continue;
            n++;
            q = AppendCh(sb, q, ' ', sizeof(sb));
            HtoA((unsigned)i, hx);
            q = AppendCStr(sb, q, hx, sizeof(sb));
            q = AppendCh(sb, q, ':', sizeof(sb));
            UtoA(a, hx);
            q = AppendCStr(sb, q, hx, sizeof(sb));
            q = AppendCh(sb, q, '>', sizeof(sb));
            UtoA(b2, hx);
            q = AppendCStr(sb, q, hx, sizeof(sb));
        }
        LogLine(sb);
        logged++;
    }
    prevCur = cur;
    for (i = 0; i < (int)DISP_REGION; i++) shadow[i] = d[i];
}

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
    if (mx < s_BoundX || mx > s_LastX + s_IconW + 5) return 0;
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
    if (mx < s_BoundX || mx > s_LastX + s_IconW + 5) return 0;
    if (my < s_YStart || my >= s_YStart + (s_VisRows * s_RowH - s_Gap)) return 0;
    return 1;
}

void SWS_Poll(void)
{
    int tog, lb, rb;
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    SwsEnsureInit();
    if (!s_Enable) return;
    tog = OU_HotkeyDown(&s_ToggleHk) ? 1 : 0;
    if (tog && !s_PrevTog)
    {
        s_Visible = s_Visible ? 0 : 1;
        LogLine(s_Visible ? "sws: show" : "sws: hide");
    }
    s_PrevTog = tog;
    if (!s_Visible)
    {
        /* 隐藏时仍采样键态, 防切回瞬间残留边沿误触。 */
        s_PrevLB = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
        s_PrevRB = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
        s_ItemN = 0;
        s_BoundValid = 0;
        s_PendSel = -2;
        s_EatPress = 0;
        s_HoldSkip = 0;
        return;
    }
    /* 应用上一帧记下的选择: 引擎已消费完那次点击, 现在进打靶态安全。
     * (隐藏态不回填, 免得 F4 关掉后又冒出个待选超武。) */
    if (s_PendSel != -2)
    {
        int v = s_PendSel;
        s_PendSel = -2;
        s_WeSetCur = 1;
        *(int*)ADDR_CURSW = v;
        LogLine(v >= 0 ? "sws: armed" : "sws: cancel");
    }    if (s_ClickFire)
    {
        lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
        rb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
        if ((lb && !s_PrevLB) || (rb && !s_PrevRB))
        {
            s_ForceScan = 1;   /* 点击: 立刻重扫, 不等 15 帧节流 */
            int mx = -1, my = -1;
            if (s_BoundValid && GtMouseGameXY(&mx, &my) &&
                mx >= s_BoundX && mx < s_BoundX + s_BoundW &&
                my >= s_BoundY && my < s_BoundY + s_BoundH)
            {
                int hit = 0;
                if (lb && !s_PrevLB)
                {
                    /* 隐藏按钮 (在栏外, 单独判) */
                    if (mx >= s_BtnX && mx < s_BtnX + s_BtnW &&
                        my >= s_BtnY && my < s_BtnY + s_BtnH)
                    {
                        s_Visible = s_Visible ? 0 : 1;
                        LogLine(s_Visible ? "sws: show" : "sws: hide");
                        return;
                    }
                    int k;
                    for (k = 0; k < s_ItemN; k++)
                    {
                        /* 被滚出可见范围的 (x=-1) 不参与命中: 不跳过的话
                         * x=-1 会让 "mx>=-1 && mx<59" 恒真, 屏幕左侧整条
                         * 竖带都会误判成点在这些不可见条目上。 */
                        if (s_Items[k].x < 0) continue;
                        if (mx >= s_Items[k].x &&
                            mx < s_Items[k].x + s_IconW &&
                            my >= s_Items[k].y &&
                            my < s_Items[k].y + s_IconH)
                        {
                            /* 释放后进入充能: 未就绪不可再选 */
                            if (!s_Items[k].ready)
                            {
                                s_EatPress = 1;
                                s_HoldSkip = 1;
                                LogLine("sws: clk notready");
                                hit = 1;
                                break;
                            }
                            /* 长期生效类 (爆裂屏障/心灵之火) **不走 SpecialPlace
                             * 事件**。实测 47/102 推了 7 次一次没中 (而非 drain 的
                             * 53/73/100 每次都中, FIRED 都有记录), 而且带真实鼠标格
                             * 的事件还会让"点完它之后别的武器第一次点击失效"。
                             * Ares 的 ClickFire 只在 ChargeDrainState 为
                             * Draining/Ready 时才动作, Charging 态两个分支都不进,
                             * 什么都不发生 —— 所以事件这条路对该类无效。
                             * 先退回打靶路径(可用), 一次点击的问题等拿到逐字节
                             * 对比数据再定。 */
                            if (s_Items[k].drain)
                            {
                                s_PendSel = s_Items[k].swIdx;
                                s_EatPress = 1;
                                s_HoldSkip = 1;
                                LogLine("sws: drain select");
                                hit = 1;
                                break;
                            }
                            if (s_Items[k].instant)
                            {
                                /* 长期生效类生效中 + SW.Unstoppable=yes:
                                 * Ares 侧栏钩子 (SWType/Hooks.cpp 0x6AAEDF)
                                 * 写得很直白 —— "disallow manuals and active
                                 * unstoppables":
                                 *     bool unstoppable = UseChargeDrain
                                 *         && ChargeDrainState == Draining
                                 *         && SW_Unstoppable;
                                 *     if(manual || unstoppable) return;
                                 * 手册也说 SW.Unstoppable = "是否禁止手工停止,
                                 * 简直就是火风暴专用"。所以照抄引擎, 吞掉这一击,
                                 * 记一行便于确认。 */
                                if (s_Items[k].drain &&
                                    s_Items[k].drainState == 2 &&
                                    s_Items[k].unstoppable)
                                {
                                    s_EatPress = 1;
                                    s_HoldSkip = 1;
                                    LogLine("sws: drain unstoppable, click eaten");
                                    hit = 1;
                                    break;
                                }
                                /* Action==None: 点击即生效, 不用再点地图。
                                 * 走标准事件 SpecialPlace (Phobos LaunchSuper
                                 * 同式), 而不是写 CurrentSW —— 后者只是本地
                                 * UI 状态, 引擎当帧不会释放, 所以"点了没反应"。
                                 * 事件由引擎在 DoList 执行, 联机也同步。 */
                                if (SwsPushSpecialPlace(s_Items[k].swIdx))
                                {
                                    /* 事件已排队: 这一击仍然止于侧栏 */
                                    s_EatPress = 1;
                                    s_HoldSkip = 1;
                                    LogLine(s_Items[k].drain ? "sws: instant drain"
                                                             : "sws: instant");
                                }
                                else
                                {
                                    /* OutList 满 (0x80): 本帧发不出去, 照旧吞掉
                                     * 这一击, 别让它漏到地图上变成误下单。 */
                                    s_EatPress = 1;
                                    s_HoldSkip = 1;
                                    LogLine("sws: instant queued fail");
                                }
                            }
                            else
                            {
                                /* 手动打靶: 按下即上膛, 并吞掉整段按住,
                                 * 等效于真 Gadget 消费掉这次完整点击。 */
                                s_PendSel = s_Items[k].swIdx;
                                s_EatPress = 1;
                                s_HoldSkip = 1;
                            }
                            hit = 1;
                            break;
                        }
                    }
                    if (!hit)
                    {
                        /* 栏内空白处按下: 吞掉, 防误下单 */
                        s_EatPress = 1;
                        s_HoldSkip = 1;
                    }
                }
                else if (rb && !s_PrevRB)
                {
                    /* 右键取消: 目标就是"无选中", 按住右键不会引发释放 */
                    s_WeSetCur = 1;
                    *(int*)ADDR_CURSW = -1;
                    LogLine("sws: cancel");
                }
            }
        }
        /* 吞没只在"这次按下起始于我们栏内"期间有效: 松开即清除,
         * 否则玩家之后点地图也会被吞, 表现为"能选但放不出来"。 */
        if (!lb) s_HoldSkip = 0;
        s_PrevLB = lb;
        s_PrevRB = rb;
        /* 吞没标记只活一帧: 引擎本帧的 ProcessClickCoords 已被门跳过,
         * 之后玩家点地图必须能正常落子。 */
        s_EatPress = 0;
    }
    /* 扫描节流: 每 15 游戏帧 (1 秒) 才重扫一次。扫描要读 78 条超武的
     * Type/Owner/IsReady/充能计时, 虽然实测只有几微秒, 但它和绘制是两回事
     * —— 侧栏内容本来就一秒变一次看不出来, 却每帧都跑。
     * 玩家点击时 (下面 s_ForceScan) 立刻补一次, 保证点到的图标状态是最新的。*/
    {
        static int s_ScanAge = 0;
        s_ScanAge++;
        if (s_ScanAge >= 15 || s_ForceScan)
        {
            s_ScanAge = 0;
            s_ForceScan = 0;
            SwsScan();
        }
    }
    SwsEventWatch();
    SwsEvCheck();
    SwsFireWatch();
    SwsDrainWatch();
    SwsLearnTick();
    s_WeSetCur = 0;
    /* 把 Poll 自身也计时上报: 之前 perf 只记 d(绘制)/s(扫描), SWS_Poll 里
     * 的东西 (含每帧 GetPrivateProfileIntA 这种文件 I/O) 完全在盲区里,
     * 帧率掉了日志却显示 d=1400us 一切正常。
     * 汇总周期 60 帧: 每帧一行 LogLine 本身就要写文件, 两处每帧打
     * (perf + poll) 光日志 I/O 就是几百微秒, 而且日志会涨到几 MB。 */
    {
        static double s_PollAcc = 0.0;
        static int s_PollN = 0;
        int fr = *(int*)ADDR_FRAME;
        s_PollAcc += SwsUsSince(t0.QuadPart);
        s_PollN++;
        if (s_PollN >= 60)
        {
            char sb[80], b1[16], b2[16];
            unsigned q = AppendCStr(sb, 0, "sws: poll ", sizeof(sb));
            UtoA((unsigned)(s_PollAcc / s_PollN), b1);
            q = AppendCStr(sb, q, b1, sizeof(sb));
            q = AppendCStr(sb, q, "us n=", sizeof(sb));
            UtoA((unsigned)s_PollN, b2);
            q = AppendCStr(sb, q, b2, sizeof(sb));
            LogLine(sb);
            s_PollAcc = 0.0;
            s_PollN = 0;
        }
        (void)fr;
    }
}

/* ---------------- 点击吞没门 ---------------- */
extern "C" __declspec(dllexport) unsigned __cdecl SWS_ClickGate(void* regs)
{
    static int s_GateLog = 0;
    int mx = -1, my = -1;
    int r = 0;
    (void)regs;
    SwsEnsureInit();
    if (!s_Enable || !s_Visible || !s_ClickFire) return 0;
    /* 当帧记下的按下: 一律跳过引擎的 ProcessClickCoords,
     * 让这次点击止于侧栏, 不被当成打靶放置。即时超武也一样吞 ——
     * 它靠 SpecialPlace 事件生效, 不依赖引擎处理这次点击。 */
    if (s_EatPress || s_HoldSkip) r = ADDR_CLICKGATE_SKIP;
    else if (!s_BoundValid) r = 0;
    else if (!GtMouseGameXY(&mx, &my)) r = 0;
    else if (mx >= s_BoundX && mx < s_BoundX + s_BoundW &&
             my >= s_BoundY && my < s_BoundY + s_BoundH)
        r = ADDR_CLICKGATE_SKIP;
    if (s_GateLog < 40 && s_BoundValid)
    {
        char sb[192], b1[16], b2[16], b3[16], b4[16];
        unsigned q = AppendCStr(sb, 0, "sws: gate ", sizeof(sb));
        UtoA((unsigned)mx, b1);
        q = AppendCStr(sb, q, b1, sizeof(sb));
        q = AppendCh(sb, q, ' ', sizeof(sb));
        UtoA((unsigned)my, b2);
        q = AppendCStr(sb, q, b2, sizeof(sb));
        q = AppendCh(sb, q, ' ', sizeof(sb));
        UtoA((unsigned)s_BoundValid, b3);
        q = AppendCStr(sb, q, b3, sizeof(sb));
        q = AppendCh(sb, q, ' ', sizeof(sb));
        UtoA((unsigned)s_EatPress, b4);
        q = AppendCStr(sb, q, b4, sizeof(sb));
        q = AppendCh(sb, q, ' ', sizeof(sb));
        UtoA((unsigned)(r ? 1 : 0), b4);
        q = AppendCStr(sb, q, b4, sizeof(sb));
        LogLine(sb);
        s_GateLog++;
    }
    return (unsigned)r;
}

/* 事件抓包 (纯读, 零行为): 盯着 EventClass::OutList 的写指针, 有新条目
 * 就把 Type / HouseIndex / 前 32 字节记一行。用途: **别再靠猜**。
 * 之前"点了没反应"就是因为我照抄 Phobos 就以为完事了, 而没先看一眼
 * 引擎自己在玩家点即时超武时到底往 OutList 里塞了什么。
 * 用法: 进游戏先用**原版侧栏**点一次即时超武 (或用我们的面板点),
 * 看日志里 `swsev: t=<Type> h=<house> ...` 那几行, 拿它当基准。
 * 布局: EventClass pack(1) sizeof=111, Type@0(1B), HouseIndex@2,
 * SpecialPlace{ int ID; CellStruct Location; } (YRpp EventClass.h)。 */
#define SWS_EV_TYPE  0x12u   /* EventType::SpecialPlace */

/* --- IsExecuted 追踪: 证明引擎真的吃下了我们塞的事件 ---
 * EventClass 布局 (YRpp, pack1): [0]Type [1]IsExecuted [2]HouseIndex
 * [3..6]Frame [7..]DataBuffer。引擎执行事件时把 [1] 置 1。
 * 之前一直只能"事件进队列了", 无法证明"引擎执行了" —— 这正是"点了没反应"
 * 查不下去的原因。现在记住刚塞进去的槽位, 之后每轮回看 [1], 变了就记一行。
 * 这比看效果 (充能计时) 更直接, 也不受渲染节奏影响。 */
typedef struct { unsigned slot; int armed; } SwsEvSlot;
static SwsEvSlot g_SwsEvSeen[SWS_EVT_SLOTS_N];
static int g_SwsEvSeenN = 0;

static void SwsEvNote(unsigned slot)
{
    if (g_SwsEvSeenN >= SWS_EVT_SLOTS_N) return;
    g_SwsEvSeen[g_SwsEvSeenN].slot = slot;
    g_SwsEvSeen[g_SwsEvSeenN].armed = 1;
    g_SwsEvSeenN++;
}

static void SwsEvCheck(void)
{
    int i, j;
    for (i = 0; i < g_SwsEvSeenN; i++)
    {
        const unsigned char* e;
        if (!g_SwsEvSeen[i].armed) continue;
        e = (const unsigned char*)(SWS_ADDR_OUT_BUF +
                                   g_SwsEvSeen[i].slot * SWS_EVT_SIZE);
        if (e[0] != (unsigned char)SWS_EV_TYPE) continue;  /* 槽已被复用 */
        if (e[1])   /* IsExecuted */
        {
            char sb[96], t1[16], t2[16];
            unsigned q = AppendCStr(sb, 0, "swsev: EXECUTED slot=", sizeof(sb));
            UtoA(g_SwsEvSeen[i].slot, t1);
            q = AppendCStr(sb, q, t1, sizeof(sb));
            q = AppendCStr(sb, q, " id=", sizeof(sb));
            UtoA((unsigned)*(int*)(e + 7), t2);
            q = AppendCStr(sb, q, t2, sizeof(sb));
            LogLine(sb);
            g_SwsEvSeen[i].armed = 0;
        }
    }
    /* 压缩掉已确认的, 免得长期占满 */
    for (i = 0, j = 0; i < g_SwsEvSeenN; i++)
        if (g_SwsEvSeen[i].armed) g_SwsEvSeen[j++] = g_SwsEvSeen[i];
    g_SwsEvSeenN = j;
}

static unsigned g_SwsEvNext = 0;
static int g_SwsEvInit = 0;

static void SwsHexB(char* out, unsigned v)
{
    static const char* dig = "0123456789ABCDEF";
    out[0] = dig[(v >> 4) & 15u];
    out[1] = dig[v & 15u];
    out[2] = 0;
}

static void SwsEventWatch(void)
{
    unsigned next, cur, adv;
    int guard = 0;

    if (!s_EvWatchOn) return;   /* SwsEnsureInit 里读一次, 别每帧读 ini */

    next = *(unsigned*)SWS_ADDR_OUT_NEXT & 0x7Fu;
    if (!g_SwsEvInit) { g_SwsEvNext = next; g_SwsEvInit = 1; return; }
    adv = (next - g_SwsEvNext) & 0x7Fu;
    if (!adv) return;
    if (adv > 8u) { g_SwsEvNext = next; return; }   /* 读档/菜单: 重同步 */
    cur = g_SwsEvNext;
    while (cur != next && guard < 8)
    {
        const unsigned char* e =
            (const unsigned char*)(SWS_ADDR_OUT_BUF + cur * SWS_EVT_SIZE);
        char sb[176], hb[4], t1[16], t2[16];
        unsigned q = 0, i;
        int typ = (int)e[0];
        int house = (int)e[2];
        /* 只详记超武相关 (SpecialPlace), 其余只记类型+house, 防刷屏。 */
        q = AppendCStr(sb, q, "swsev: t=", sizeof(sb));
        UtoA((unsigned)typ, t1);
        q = AppendCStr(sb, q, t1, sizeof(sb));
        q = AppendCStr(sb, q, " h=", sizeof(sb));
        UtoA((unsigned)house, t2);
        q = AppendCStr(sb, q, t2, sizeof(sb));
        if (typ == (int)SWS_EV_TYPE)
        {
            int id = *(int*)(e + 7);
            int lx = *(short*)(e + 11);
            int ly = *(short*)(e + 13);
            q = AppendCStr(sb, q, " SP id=", sizeof(sb));
            UtoA((unsigned)id, t1);
            q = AppendCStr(sb, q, t1, sizeof(sb));
            q = AppendCStr(sb, q, " cell=", sizeof(sb));
            UtoA((unsigned)lx, t1);
            q = AppendCStr(sb, q, t1, sizeof(sb));
            q = AppendCh(sb, q, ',', sizeof(sb));
            UtoA((unsigned)ly, t2);
            q = AppendCStr(sb, q, t2, sizeof(sb));
        }
        q = AppendCStr(sb, q, " raw=", sizeof(sb));
        for (i = 0; i < 32; i++)
        {
            SwsHexB(hb, (unsigned)e[i]);
            if (i && (i % 8) == 0) q = AppendCh(sb, q, ' ', sizeof(sb));
            q = AppendCStr(sb, q, hb, sizeof(sb));
        }
        (void)q;
        LogLine(sb);
        cur = (cur + 1u) & 0x7Fu;
        guard++;
    }
    g_SwsEvNext = next;
}

/* --- 释放侦测: 用**效果**证明超武真的放了, 而不是猜机制 ---
 * 之前一直在猜"引擎执行了没有"(IsExecuted 那条路走不通: 引擎自己那条
 * SpecialPlace 首字节是 0x5A, 我们的恒为 0, [1] 根本不是可靠的执行标志)。
 * 换个更硬的事实: 超武一旦释放, 充能计时必然从"就绪"变成"满充能中",
 * 即该超武的 ready 出现 1 -> 0 边沿。这条边沿对**我们的面板**和
 * **原版侧栏**一视同仁, 所以既能验证我们的点击, 也能当原版的基线。
 *
 * 必须按 swIdx 记, 不能按 s_Items 下标: 扫描每帧重建且按阵营/优先级排序,
 * 下标 k 这一帧和下一帧未必是同一个超武。 */
#define SWS_FIRE_SLOTS 512
static int s_FirePrev[SWS_FIRE_SLOTS];

static void SwsFireWatch(void)
{
    int k;
    static int s_TblDumped = 0;
    /* 用缓存值, 不要在这里 GetPrivateProfileIntA。
     * 那个 API 每次调用都会打开并解析 ini 文件, 放每帧就是每帧一次文件
     * I/O —— 而且它在 SWS_Poll 里, 落在 perf 日志的 d/s 计时之外, 所以
     * 日志上 d=1400us 看着正常, 实际帧率被拖垮 (这种"测不到的开销"
     * 最容易漏)。开关一律在 SwsEnsureInit 读一次。 */
    if (!s_FireWatchOn) return;
    /* 首轮把整张表打一遍。**不能**用"第一次调用"当触发条件: 启动那一帧
     * 类型/超武表还没加载 (日志里 sws: raw sup=0x0/0), s_ItemN 还是 0,
     * 于是打了 0 行然后就再也不打了 —— 白等一整局。
     * 改成"第一次拿到非空表时打一次"。 */
    if (!s_TblDumped)
    {
        if (s_ItemN <= 0) return;          /* 还没加载, 下一帧再试 */
        for (k = 0; k < SWS_FIRE_SLOTS; k++) s_FirePrev[k] = 0;
        s_TblDumped = 1;
        {
            int j;
            for (j = 0; j < s_ItemN; j++)
            {
                char sb[192], t1[16], t2[16], t3[16];
                unsigned q = AppendCStr(sb, 0, "sws: tbl ", sizeof(sb));
                UtoA((unsigned)s_Items[j].swIdx, t1);
                q = AppendCStr(sb, q, t1, sizeof(sb));
                q = AppendCh(sb, q, ' ', sizeof(sb));
                q = AppendCStr(sb, q, s_Items[j].id, sizeof(sb));
                q = AppendCStr(sb, q, " inst=", sizeof(sb));
                UtoA((unsigned)s_Items[j].instant, t2);
                q = AppendCStr(sb, q, t2, sizeof(sb));
                q = AppendCStr(sb, q, " dr=", sizeof(sb));
                UtoA((unsigned)s_Items[j].drain, t2);
                q = AppendCStr(sb, q, t2, sizeof(sb));
                q = AppendCh(sb, q, '/', sizeof(sb));
                UtoA((unsigned)s_Items[j].drainState + 1u, t2);
                q = AppendCStr(sb, q, t2, sizeof(sb));
                q = AppendCStr(sb, q, "/u", sizeof(sb));
                UtoA((unsigned)s_Items[j].unstoppable, t2);
                q = AppendCStr(sb, q, t2, sizeof(sb));
                q = AppendCStr(sb, q, " rdy=", sizeof(sb));
                UtoA((unsigned)s_Items[j].ready, t2);
                q = AppendCStr(sb, q, t2, sizeof(sb));
                q = AppendCStr(sb, q, " rem=", sizeof(sb));
                UtoA((unsigned)s_Items[j].remain, t2);
                q = AppendCStr(sb, q, t2, sizeof(sb));
                q = AppendCh(sb, q, '/', sizeof(sb));
                UtoA((unsigned)s_Items[j].total, t3);
                q = AppendCStr(sb, q, t3, sizeof(sb));
                LogLine(sb);
            }
        }
        return;
    }
    for (k = 0; k < s_ItemN; k++)
    {
        int id = s_Items[k].swIdx;
        if (id < 0 || id >= SWS_FIRE_SLOTS) continue;
        if (s_FirePrev[id] == 1 && s_Items[k].ready == 0)
        {
            char sb[128], t1[16], t2[16];
            unsigned q = AppendCStr(sb, 0, "sws: FIRED id=", sizeof(sb));
            UtoA((unsigned)id, t1);
            q = AppendCStr(sb, q, t1, sizeof(sb));
            q = AppendCStr(sb, q, " inst=", sizeof(sb));
            UtoA((unsigned)s_Items[k].instant, t2);
            q = AppendCStr(sb, q, t2, sizeof(sb));
            q = AppendCStr(sb, q, " rem=", sizeof(sb));
            UtoA((unsigned)s_Items[k].remain, t2);
            q = AppendCStr(sb, q, t2, sizeof(sb));
            q = AppendCh(sb, q, '/', sizeof(sb));
            UtoA((unsigned)s_Items[k].total, t2);
            q = AppendCStr(sb, q, t2, sizeof(sb));
            LogLine(sb);
        }
        s_FirePrev[id] = s_Items[k].ready;
    }
}

/* ChargeDrainState 跳变日志: 长期生效类 (爆裂屏障/心灵之火) 靠它才知道
 * 有没有真的激活/关闭。只在变的时候记, 不刷屏。OFF_SUP_DRAIN=0x7C 是推算
 * 出来的, 这行日志同时也是那个偏移对不对的验证 —— 如果值是乱七八糟的
 * (不是 -1/0/1/2) 就说明偏移错了。 */
static int s_DrainPrev[SWS_FIRE_SLOTS];
static int s_DrainInit = 0;

static void SwsDrainWatch(void)
{
    int k;
    if (!s_FireWatchOn) return;
    if (!s_DrainInit)
    {
        for (k = 0; k < SWS_FIRE_SLOTS; k++) s_DrainPrev[k] = -99;
        s_DrainInit = 1;
        return;
    }
    for (k = 0; k < s_ItemN; k++)
    {
        int id = s_Items[k].swIdx;
        if (id < 0 || id >= SWS_FIRE_SLOTS) continue;
        if (!s_Items[k].drain) continue;
        if (s_DrainPrev[id] != s_Items[k].drainState)
        {
            /* ChargeDrainState: -1=None 0=Charging 1=Ready 2=Draining
             * (GeneralDefinitions.h)。这里之前写成 {None,Charging,Ready,...}
             * 是**差一错误** —— 0 是 Charging 不是 None, 导致打出的 "Charging"
             * 其实是状态 1(Ready), 差点让我误判成"没充好所以点不动"。 */
            static const char* kName[4] = { "Charging", "Ready", "Draining", "?" };
            const char* nm = (s_Items[k].drainState >= 0 && s_Items[k].drainState <= 2)
                             ? kName[s_Items[k].drainState] : "None";
            char sb[144], t1[16], t2[16];
            unsigned q = AppendCStr(sb, 0, "sws: drain id=", sizeof(sb));
            UtoA((unsigned)id, t1);
            q = AppendCStr(sb, q, t1, sizeof(sb));
            q = AppendCStr(sb, q, " ", sizeof(sb));
            q = AppendCStr(sb, q, s_Items[k].id, sizeof(sb));
            q = AppendCStr(sb, q, " ", sizeof(sb));
            q = AppendCStr(sb, q, nm, sizeof(sb));
            q = AppendCStr(sb, q, " unstop=", sizeof(sb));
            UtoA((unsigned)s_Items[k].unstoppable, t2);
            q = AppendCStr(sb, q, t2, sizeof(sb));
            LogLine(sb);
            s_DrainPrev[id] = s_Items[k].drainState;
        }
    }
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
    next = *(unsigned*)SWS_ADDR_OUT_NEXT & 0x7Fu;

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

    *(unsigned*)SWS_ADDR_OUT_NEXT = (next + 1u) & 0x7Fu;
    *(unsigned*)SWS_ADDR_OUT_COUNT = count + 1u;
    SwsEvNote(next);
    /* 记下自己塞进去的那一条 (houseIdx/swIdx), 用来和抓包日志里引擎自己
     * 产生的 SuperWeapon 事件逐字节对比 —— 别再靠"应该对吧"来判断。 */
    {
        /* 把**我们构造出来的**事件前 32 字节原样打出来, 好和抓包日志里
         * 引擎自己发的那条逐字节对比。drain 类 (47/102) 推了 7 次一次没中,
         * 而 53/73/100 每次都中, 差异一定在这 32 字节里。 */
        char sb[256], hb[4];
        unsigned q = AppendCStr(sb, 0, "sws: push h=", sizeof(sb));
        UtoA((unsigned)houseIdx, hb);
        q = AppendCStr(sb, q, hb, sizeof(sb));
        q = AppendCStr(sb, q, " id=", sizeof(sb));
        UtoA((unsigned)swIdx, hb);
        q = AppendCStr(sb, q, hb, sizeof(sb));
        for (i = 0; i < 32; i++)
        {
            SwsHexB(hb, (unsigned)(unsigned char)ev[i]);
            if (i && (i % 8) == 0) q = AppendCh(sb, q, ' ', sizeof(sb));
            q = AppendCStr(sb, q, hb, sizeof(sb));
        }
        LogLine(sb);
    }
    return 1;
}

/* ---------------- 绘制 ---------------- */


/* ---- PCX 逐像素贴图 (不走引擎 BlitToSurface) ----
 * 引擎 BlitToSurface 每调一次就锁/解锁一次合成面: 62 个条目 = 62 次锁面,
 * 实测 d=5960us/帧, 这就是掉帧主因 (扫描只要 2us)。所以改成在下面那次
 * 锁面里直接写像素。BSurface 布局 (YRpp Surface.h):
 *   Surface{Width+0x88, Height+0x8C}
 *   -> XSurface{LockLevel+0x90, BytesPerPixel+0x94}
 *   -> BSurface::MemoryBuffer{void* Buffer +0x98; int Size; bool Allocated}
 * 布局对不上就返回 0, 由调用方退回引擎 blit, 保证不会画不出来。 */
static int SwsPcxBlitLocked(unsigned short* dstBase, int pitch,
                            int surfW, int surfH,
                            int dx, int dy, int dw, int dh, void* psurf)
{
    int pw, ph, bpp, x, y;
    unsigned short* src;
    if (!dstBase || !psurf || !IsPlausiblePtr(psurf)) return 0;
    pw = *(int*)((char*)psurf + 0x88);
    ph = *(int*)((char*)psurf + 0x8C);
    bpp = *(int*)((char*)psurf + 0x94);
    src = *(unsigned short**)((char*)psurf + 0x98);
    if (pw < 1 || pw > 256 || ph < 1 || ph > 256 || bpp != 2) return 0;
    if (!src || !IsPlausiblePtr((void*)src)) return 0;
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
            drow[tx] = c;
        }
    }
    return 1;
}

/* 锁面状态下的半透明填充 (替代每条目 6 次 FillRectTrans 虚表调用) */
static void SwsFillLocked(unsigned char* base, int pitch, int surfW, int surfH,
                          int x, int y, int rw, int rh,
                          unsigned char r, unsigned char g, unsigned char b,
                          int op)
{
    int yy, xx;
    if (!base || rw <= 0 || rh <= 0) return;
    if (op < 0) op = 0;
    if (op > 100) op = 100;
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



static LONGLONG g_DrawStart;
void SWS_Draw(void* surf, int w, int h, int pitch)
{
    /* 精简绘制 (为帧率):
     *   - 一列一个底框 + 一列一组边框, 不再每个超武画一个方框
     *   - 不画"就绪"等状态字 (只在鼠标悬停时画名字)
     *   - 悬停项: 高亮框 + 名字文本
     * 之前每条目 6 次填充 + 未就绪罩 + 状态字渲染, 64 个条目时是主要开销。
     */
    LARGE_INTEGER td;
    int order[SWS_CAP];
    int colCount[4] = { 0, 0, 0, 0 };
    int colX[4] = { 0, 0, 0, 0 };
    int i, n, f, totW, xStart, yStart, rowH, maxRows, blockH;
    int curSel = -9999;
    int mx = -1, my = -1;
    int hover = -1;
    unsigned char* vt;
    SwsLockFn lockFn;
    SwsUnlockFn unlockFn;
    unsigned char* locked;
    SwsPerfInit();
    QueryPerformanceCounter(&td);
    g_DrawStart = td.QuadPart;

    if (!s_Enable || !s_Visible) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    n = s_ItemN;
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
    rowH = s_IconH + s_Gap;
    blockH = maxRows * rowH - s_Gap;
    totW = 0;
    for (f = 0; f < 4; f++)
        if (colCount[f] > 0) totW += (totW ? s_ColGap : 0) + s_IconW;
    /* 有列装不下时, 整块左边让出 5px 给右侧那根滚动条 (否则会顶出屏幕) */
    s_BarValid = (s_MaxSk > 0);
    s_BarW = s_BarValid ? 4 : 0;
    xStart = w - s_RightMargin - totW - (s_BarValid ? 5 : 0);
    if (xStart < 0) xStart = 0;
    yStart = s_CenterY ? (h - blockH) / 2 : s_TopMargin;
    if (yStart < 0) yStart = 0;
    {
        int cx = xStart;
        for (f = 0; f < 4; f++)
        {
            if (colCount[f] <= 0) continue;
            colX[f] = cx;
            cx += s_IconW + s_ColGap;
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
    /* 隐藏按钮: 放在超武块正上方 (右上角) */
    s_BtnW = 24;
    s_BtnH = 14;
    s_BtnX = xStart + totW - s_BtnW;
    s_BtnY = yStart - s_BtnH - 2;
    if (s_BtnY < 0) s_BtnY = yStart + 2;
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
    s_BarX = s_LastX + s_IconW + 1;
    if (!g_Calibrated) Calibrate();

    /* 悬停项 (悬停时才画文字, 平时零文本开销) */
    GtMouseGameXY(&mx, &my);
    for (i = 0; i < n; i++)
    {
        SwsItem* it = &s_Items[order[i]];
        if (it->x < 0) continue;                 /* 被滚动裁掉 */
        if (mx >= it->x && mx < it->x + s_IconW &&
            my >= it->y && my < it->y + s_IconH)
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
        SwsFillLocked(locked, pitch, w, h, colX[f], yStart, s_IconW, ch,
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
                                  it->x, it->y, s_IconW, s_IconH, ps))
            {
                void* pcxInst = (void*)ADDR_PCX_INSTANCE;
                if (pcxInst && IsPlausiblePtr(pcxInst))
                {
                    char rect[16];
                    unlockFn(surf, NULL);
                    *(int*)(rect + 0) = it->x; *(int*)(rect + 4) = it->y;
                    *(int*)(rect + 8) = s_IconW; *(int*)(rect + 12) = s_IconH;
                    FN_PCXBLIT(pcxInst, rect, surf, ps, PCX_TRANS_PARENT);
                    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
                    if (!locked || !IsPlausiblePtr(locked)) return;
                }
            }
            /* 未就绪 (释放后充能中): 压暗。只对这类条目做, 通常 0~1 个。 */
            if (!it->ready)
                SwsFillLocked(locked, pitch, w, h, it->x, it->y, s_IconW,
                              s_IconH, 0, 0, 0, 45);
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
            /* 长期生效类 (UseChargeDrain): 点击是**开关**而不是一次性发射,
             * 状态是三态而不是百分比。引擎/Ares 的侧栏也是按 ChargeDrainState
             * 三档出字的 (SWType/Hooks.cpp SuperClass_GetCameoName):
             *   Charging -> Text.Charging / Ready -> Text.Ready
             *   Draining -> Text.Active   (rulesmo 里两个都是 NAME:BLASTRUNNING)
             * 之前这里只画百分比, 于是爆裂屏障/心灵之火"生效中"这个状态
             * 完全没体现 —— 就是用户说的"没在超武侧栏上体现"。 */
            else if (it->drain)
            {
                if (it->drainState == 2)          /* Draining = 生效中 */
                {
                    st[0] = 'O'; st[1] = 'N';
                }
                else if (it->drainState == 1)     /* Ready = 可激活 */
                {
                    st[0] = 'R'; st[1] = 'd'; st[2] = 'y';
                }
                else if (it->total > 0)           /* Charging: 仍给百分比 */
                {
                    int pct = 100 - (it->remain * 100) / it->total;
                    if (pct < 0) pct = 0;
                    if (pct > 99) pct = 99;
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
                 * 的碎片 (形似 '<'; 微软雅黑 extent 非矩形, 见 GdiText 注释)。 */
                sx = it->x + (s_IconW - sw) / 2;
                ny = it->y + 1;
                if (sx < it->x) sx = it->x;
                BlitCoverage(locked, pitch, w, h, sx + 1, ny + 1, sw, adv,
                             0, 0, 0, 80);
                BlitCoverage(locked, pitch, w, h, sx, ny, sw, adv,
                             255, 255, 255, 100);
            }
        }
        if (i == hover || it->swIdx == curSel)
        {
            const unsigned char* hc = (i == hover) ?
                (const unsigned char*)"\xFF\xFF\xFF" : kSideCol[it->side];
            SwsFillLocked(locked, pitch, w, h, it->x - 1, it->y - 1,
                          s_IconW + 2, 1, hc[0], hc[1], hc[2], 220);
            SwsFillLocked(locked, pitch, w, h, it->x - 1, it->y + s_IconH,
                          s_IconW + 2, 1, hc[0], hc[1], hc[2], 220);
            SwsFillLocked(locked, pitch, w, h, it->x - 1, it->y, 1, s_IconH,
                          hc[0], hc[1], hc[2], 220);
            SwsFillLocked(locked, pitch, w, h, it->x + s_IconW, it->y, 1,
                          s_IconH, hc[0], hc[1], hc[2], 220);
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
        SwsFillLocked(locked, pitch, w, h, colX[f], yStart, s_IconW, 1,
                      c[0], c[1], c[2], 150);
        SwsFillLocked(locked, pitch, w, h, colX[f], yStart + ch - 1, s_IconW, 1,
                      c[0], c[1], c[2], 150);
        SwsFillLocked(locked, pitch, w, h, colX[f], yStart, 1, ch,
                      c[0], c[1], c[2], 150);
        SwsFillLocked(locked, pitch, w, h, colX[f] + s_IconW - 1, yStart, 1, ch,
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

    /* ---- 隐藏按钮 (画在最上层) ---- */
    {
        const unsigned char* bc = s_Visible ? (const unsigned char*)
            "\x40\xFF\x60" : (const unsigned char*)"\x80\x80\x80";
        SwsFillLocked(locked, pitch, w, h, s_BtnX, s_BtnY, s_BtnW, s_BtnH,
                      bc[0], bc[1], bc[2], 150);
        SwsFillLocked(locked, pitch, w, h, s_BtnX, s_BtnY, s_BtnW, 1,
                      255, 255, 255, 120);
    }

    /* ---- 悬停名字 (带底框, 跟随全局矢量字体) ---- */
    if (hover >= 0)
    {
        SwsItem* it = &s_Items[order[hover]];
        wchar_t nm[24];
        int sl = 0, sw, sx, ny;
        int adv;
        SwsCopyName(nm, it->name, 23);
        if (!nm[0]) { /* 退到 ID 字符串 */ }
        {
            /* 悬停提示跟随全局矢量字体设置 ([VecText] FontFace/FontPx/Bold) */
            int gpx = VecText_FontPx();
            if (gpx <= 0) gpx = 18;
            if (GdiEnsure(gpx, VecText_Bold(), 5, VecText_FontFace()))
            {
            adv = g_G->adv > 0 ? g_G->adv : gpx + 2;
            while (sl < 23 && nm[sl]) sl++;
            sw = sl ? RenderLine(nm, sl) : 0;
            if (sw > 0)
            {
                sx = mx - sw / 2;
                ny = it->y - adv - 2;
                if (ny < 0) ny = it->y + s_IconH + 2;
                if (sx < 2) sx = 2;
                if (sx + sw + 2 > w - 2) sx = w - sw - 4;
                /* 底框: 半透明黑, 让白字在亮图上也读得清 */
                SwsFillLocked(locked, pitch, w, h, sx - 2, ny - 2, sw + 6,
                              adv + 4, 0, 0, 0, 70);
                BlitCoverage(locked, pitch, w, h, sx + 1, ny + 1, sw, adv,
                             0, 0, 0, 70);
                BlitCoverage(locked, pitch, w, h, sx, ny, sw, adv,
                             255, 255, 255, 100);
            }
            }
        }
    }
    unlockFn(surf, NULL);

    /* 耗时汇总: 每 60 帧记一行**平均**值。
     * 原来这里是每帧一行 LogLine —— 一帧一次文件写, 21 万行把日志撑到
     * 4MB+, 光这个 I/O 就是几百微秒/帧, 属于自己把自己拖慢。
     * 和 SWS_Poll 里的 sws: poll 一起节流。 */
    {
        static int s_N = 0;
        static double s_DrawUs = 0.0;
        static double s_ScanUs = 0.0;
        static int s_Items2 = 0;
        s_N++;
        s_DrawUs += SwsUsSince(g_DrawStart);
        s_ScanUs += (g_ScanCnt ? g_ScanUsAcc / g_ScanCnt : 0.0);
        s_Items2 = n;
        if (s_N >= 60)
        {
            char sb[160], b1[16], b2[16], b3[16];
            unsigned q = AppendCStr(sb, 0, "sws: perf d=", sizeof(sb));
            UtoA((unsigned)(s_DrawUs / s_N), b1);
            q = AppendCStr(sb, q, b1, sizeof(sb));
            q = AppendCStr(sb, q, " s=", sizeof(sb));
            UtoA((unsigned)(s_ScanUs / s_N), b2);
            q = AppendCStr(sb, q, b2, sizeof(sb));
            q = AppendCStr(sb, q, " n=", sizeof(sb));
            UtoA((unsigned)s_Items2, b3);
            q = AppendCStr(sb, q, b3, sizeof(sb));
            LogLine(sb);
            s_N = 0;
            s_DrawUs = 0.0;
            s_ScanUs = 0.0;
            g_ScanUsAcc = 0.0;
            g_ScanCnt = 0;
        }
    }
}
