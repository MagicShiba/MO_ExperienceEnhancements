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

#define SWS_CAP 64
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
static int s_Probe = 1;   /* 位置探针: 无超武也画 4 个阵营色块+尺寸, 证明位置没错 */
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
/* Arm on mouse *release*, not on press. RA2 tests "left button held" for
 * superweapon placement, so arming while the button is still down fires
 * the super at that spot. Phobos dodges this because a real Gadget eats
 * the press before the map sees it; we have no Gadget, so we arm on the
 * release edge instead: press = select intent, release = enter targeting,
 * next press = place. Identical to the stock sidebar from the user's side. */
static int s_ArmIdx = -1;
static int s_WaitRel = 0;
static int s_WaitRelAge = 0;

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

static const unsigned char kSideCol[4][3] = {
    { 70, 150, 255 }, { 255, 70, 70 },
    { 170, 80, 255 }, { 255, 170, 50 },
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
wchar_t name[20];
} SwsItem;

static SwsItem s_Items[SWS_CAP];
static int s_ItemN = 0;
static int s_BoundX = 0, s_BoundY = 0, s_BoundW = 0, s_BoundH = 0;
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

/* ID 合法性: 1~23 个可打印 ASCII 后跟 NUL (24 字节槽)。 */
static int SwsIdOk(const char* s)
{
    int i = 0;
    if (!s || !IsPlausiblePtr(s)) return 0;
    while (i < 24)
    {
        char c = s[i];
        if (!c) return i > 0;
        if (c < 0x20 || c > 0x7E) return 0;
        i++;
    }
    return 0;
}

static void SwsCopyId(char* dst, const char* src)
{
    int i = 0;
    while (i < 31 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void SwsCopyName(wchar_t* dst, const wchar_t* src, int cap)
{
    int i = 0;
    if (!src || !IsPlausiblePtr((void*)src)) { dst[0] = 0; return; }
    while (i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
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

static void SwsFillTrans(void* surf, int x, int y, int w, int h,
                         unsigned char r, unsigned char g, unsigned char b,
                         int op)
{
    unsigned char* vt;
    typedef int (__fastcall *Fn)(void*, void*, void*, void*, int);
    Fn fn;
    char rect[16], col[4];
    if (!surf || w <= 0 || h <= 0) return;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    fn = (Fn)(*(void**)(vt + VT_FILL_RECT_TRANS));
    if (!fn || !IsPlausiblePtr((const void*)fn)) return;
    *(int*)(rect + 0) = x; *(int*)(rect + 4) = y;
    *(int*)(rect + 8) = w; *(int*)(rect + 12) = h;
    col[0] = (char)r; col[1] = (char)g; col[2] = (char)b; col[3] = 0;
    fn(surf, NULL, rect, col, op);
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
            SwsStrEq(k, "Probe") || SwsStrEq(k, "Learn") ||
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
    s_ColGap = GetPrivateProfileIntA(SWS_SECTION, "ColGap", 6, g_IniPath);
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
    s_Probe = GetPrivateProfileIntA(SWS_SECTION, "Probe", 1, g_IniPath) ? 1 : 0;
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

/* wide-string equality, no CRT. */
static int SwsNameEq(const wchar_t* a, const wchar_t* b)
{
    int i = 0;
    if (!a || !b) return 0;
    while (i < 64)
    {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
        i++;
    }
    return 0;
}

/* engine list filter: name must show up in a fresh engine SW-list row
 * (kind 0 = name, 400ms). Engine already applies ShowCameo=false
 * and all other filters, so this inherits them. Read-only. */
static int SwsInEngineList(const wchar_t* nm)
{
    int i;
    unsigned now;
    if (!nm || !nm[0]) return 0;
    now = GetTickCount();
    for (i = 0; i < g_SWCapN && i < SWCAP_MAX; i++)
    {
        SWCapRow* r = &g_SWCap[i];
        if (r->kind != 0) continue;
        if (now - r->tick > 400) continue;
        if (SwsNameEq(nm, r->s)) return 1;
    }
    return 0;
}

/* Engine own show-set: the same DynamicVector the stock sidebar uses to
 * decide which supers are listed (ShowTimers). It already encodes
 * availability + cameo + everything the game filters on, so membership is
 * the authoritative "should be shown" answer. No offset guessing.
 * Read-only, so multiplayer safe. */
static void* s_Show[512];
static int s_ShowN;

static int SwsInShowSet(int n, void* sup)
{
    int j;
    for (j = 0; j < n; j++)
        if (s_Show[j] == sup) return 1;
    return 0;
}

static int SwsLoadShowSet(void)
{
    int showN, j, cnt = 0;
    void** showItems = NULL;
    s_ShowN = 0;
    if (IsPlausiblePtr((void*)0xA83D50u))
    {
        showItems = ARR_ITEMS(0xA83D50u);
        showN = ARR_COUNT(0xA83D50u);
    }
    if (showItems && IsPlausiblePtr(showItems) && showN > 0 && showN <= 4096)
    {
        for (j = 0; j < showN && cnt < 512; j++)
        {
            void* s = showItems[j];
            if (s && IsPlausiblePtr(s)) s_Show[cnt++] = s;
        }
    }
    s_ShowN = cnt;
    return cnt;
}

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

#define PCX_CACHE 64

typedef struct {
    char id[32];
    char file[64];
    int  tried;
} SwsPcxEnt;

static SwsPcxEnt s_Pcx[PCX_CACHE];
static int s_PcxN = 0;

/* 取某超武的 SidebarPCX= 文件名 (读一次, 缓存在我们自己的表里)。
 * 注意: 这里只缓存"文件名", 绝不缓存引擎的 BSurface* —— 引擎会在切页签/
 * 重载美术时释放并重建 PCX surface, 跨帧持有它的指针 = 野指针 (表现为
 * 图画成紫球, 切页时崩溃)。surface 每帧现取。 */
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
            /* Engine show-set (loaded before the item loop below):
             * authoritative visibility answer, no offset guessing. */
            {
                int showCnt = s_ShowN;
                int inShow = 0, j;
                for (j = 0; j < count && j < 4096; j++)
                {
                    void* s2 = items[j];
                    if (!s2 || !IsPlausiblePtr(s2)) continue;
                    if (*(void**)((char*)s2 + OFF_SUP_OWNER) != cur) continue;
                    if (SwsInShowSet(showCnt, s2)) inShow++;
                }
                {
                    char sb[192], hx[16];
                    unsigned q = 0;
                    int z;
                    q = AppendCStr(sb, q, "sws: show T=", sizeof(sb));
                    UtoA((unsigned)showCnt, hx);
                    q = AppendCStr(sb, q, hx, sizeof(sb));
                    q = AppendCStr(sb, q, " inShow=", sizeof(sb));
                    UtoA((unsigned)inShow, hx);
                    q = AppendCStr(sb, q, hx, sizeof(sb));
                    for (z = 0; z < 4 && z < showCnt; z++)
                    {
                        void* t = *(void**)((char*)s_Show[z] + OFF_SUP_TYPE);
                        int c;
                        q = AppendCh(sb, q, ' ', sizeof(sb));
                        if (!t || !IsPlausiblePtr(t)) { q = AppendCh(sb, q, '?', sizeof(sb)); continue; }
                        for (c = 0; c < 20; c++)
                        {
                            char ch = *((char*)t + OFF_SWT_ID + c);
                            if (!ch) break;
                            if (ch < 0x20 || ch > 0x7E) ch = '.';
                            q = AppendCh(sb, q, ch, sizeof(sb));
                        }
                    }
                    LogLine(sb);
                }
            }
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
    SwsLoadShowSet();
    for (i = 0; i < count && n < SWS_CAP; i++)
    {
        void* sup = items[i];
        void* type;
        void* owner;
        const char* id;
        unsigned char present, ready, susp;
        /* Engine show-set is the authority on which supers are visible */
        if (s_ShowN > 0 && !SwsInShowSet(s_ShowN, sup)) continue;
        int swIdx, total, start, left, frame, remain, side, prio, hide;
        const wchar_t* ui;
        wchar_t nm[20];
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
        /* ArrayIndex: do NOT trust a guessed field offset (it returned 135
         * = the type count, not this type's index, which made the engine
         * index Supers[] out of bounds and drop the selection at once).
         * Derive it from the type array itself: find the entry whose ID
         * equals ours. Only container layout (+4 items / +16 count) and the
         * ID offset (+0x24) are used, both already verified. */
        swIdx = SwsArrayIndexOf(type, id);
        if (swIdx < 0) continue;
        ui = *(const wchar_t**)((char*)type + OFF_SWT_UINAME);
        SwsCopyName(nm, ui, 20);
        if (!nm[0])
        {
            int k = 0;
            while (k < 19 && id[k]) { nm[k] = (wchar_t)(unsigned char)id[k]; k++; }
            nm[k] = 0;
        }
        if (!SwsInEngineList(nm) && s_ShowN == 0) continue;
        prio = SwsPrioOf(id, &hide);
        if (hide) continue;
        side = SwsSideOf(id);
        if (side < 0) side = pside;
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
        s_Items[n].pcxFile = SwsPcxFileOf(id);
        /* PCX 优先 (Phobos SWButtonClass::Draw 同序); 没有 PCX 才用 SHP,
         * 且引用原样传给 DrawSHP (不跟 Data/不调 GetData)。 */
        s_Items[n].shp = s_Items[n].pcxFile ? NULL : SwsShpOk(type);
        s_Items[n].swIdx = swIdx;
        s_Items[n].side = side;
        s_Items[n].prio = prio;
        s_Items[n].ready = (ready == 1 && susp == 0) ? 1 : 0;
        s_Items[n].susp = (susp == 1) ? 1 : 0;
        s_Items[n].remain = remain;
        s_Items[n].total = total;
        {
            int k = 0;
            while (k < 20) { s_Items[n].name[k] = nm[k]; if (!nm[k]) break; k++; }
            s_Items[n].name[19] = 0;
        }
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

void SWS_Poll(void)
{
    int tog, lb, rb;
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
        s_ArmIdx = -1;
        s_WaitRel = 0;
        s_WaitRelAge = 0;
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
            int mx = -1, my = -1;
            if (s_BoundValid && GtMouseGameXY(&mx, &my) &&
                mx >= s_BoundX && mx < s_BoundX + s_BoundW &&
                my >= s_BoundY && my < s_BoundY + s_BoundH)
            {
                /* 栏内任意按下都吞掉 (同 Phobos: 悬停新按钮列时
                 * ProcessClickCoords 一律 DoNothing), 防止误下单。 */
                s_EatPress = 1;
                if (lb && !s_PrevLB)
                {
                    int k;
                    for (k = 0; k < s_ItemN; k++)
                    {
                        if (mx >= s_Items[k].x &&
                            mx < s_Items[k].x + s_IconW &&
                            my >= s_Items[k].y &&
                            my < s_Items[k].y + s_IconH)
                        {
                            /* 只记意图, 等松开再上膛。 */
                            s_ArmIdx = s_Items[k].swIdx;
                            s_WaitRel = 1;
                            s_WaitRelAge = 0;
                            break;
                        }
                    }
                }
                else
                {
                    /* 右键取消: 目标是"无选中", 按住右键不会引发释放,
                     * 所以立即生效。 */
                    s_WeSetCur = 1;
                    *(int*)ADDR_CURSW = -1;
                    LogLine("sws: cancel");
                }
            }
        }
        /* 松开左键 = 真正进入打靶态 (此刻按键必为松, 引擎不会误判)。 */
        if (s_WaitRel && !lb)
        {
            s_WaitRel = 0;
            s_WeSetCur = 1;
            *(int*)ADDR_CURSW = s_ArmIdx;
            s_ArmIdx = -1;
            LogLine("sws: armed");
        }
        else if (s_WaitRel)
        {
            /* 兜底: 按住不放超过 60 帧也上膛, 防卡在等待态。 */
            if (++s_WaitRelAge > 60)
            {
                s_WaitRel = 0;
                s_WeSetCur = 1;
                *(int*)ADDR_CURSW = s_ArmIdx;
                s_ArmIdx = -1;
                LogLine("sws: armed(hold)");
            }
        }
        s_PrevLB = lb;
        s_PrevRB = rb;
        /* 吞没标记只活一帧: 引擎本帧的 ProcessClickCoords 已被门跳过,
         * 之后玩家点地图必须能正常落子。 */
        s_EatPress = 0;
    }
    SwsScan();
    SwsLearnTick();
    s_WeSetCur = 0;
}

/* ---------------- 点击吞没门 ---------------- */
extern "C" __declspec(dllexport) unsigned __cdecl SWS_ClickGate(void* regs)
{
    int mx = -1, my = -1;
    (void)regs;
    SwsEnsureInit();
    if (!s_Enable || !s_Visible || !s_ClickFire) return 0;
    /* 当帧记下的按下: 一律跳过引擎的 ProcessClickCoords,
     * 让这次点击止于侧栏, 不被当成打靶放置。 */
    if (s_EatPress) return ADDR_CLICKGATE_SKIP;
    if (!s_BoundValid) return 0;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    if (mx >= s_BoundX && mx < s_BoundX + s_BoundW &&
        my >= s_BoundY && my < s_BoundY + s_BoundH)
        return ADDR_CLICKGATE_SKIP;
    return 0;
}

/* ---------------- 绘制 ---------------- */

static void SwsBuildStatus(const SwsItem* it, wchar_t* out)
{
    if (it->susp)
    {
        out[0] = (wchar_t)'H'; out[1] = (wchar_t)'o';
        out[2] = (wchar_t)'l'; out[3] = (wchar_t)'d'; out[4] = 0;
    }
    else if (it->ready)
    {
        out[0] = (wchar_t)'R'; out[1] = (wchar_t)'e';
        out[2] = (wchar_t)'a'; out[3] = (wchar_t)'d';
        out[4] = (wchar_t)'y'; out[5] = 0;
    }
    else
    {
        int sec = (it->remain + 27) / 54;
        int mm = sec / 60, ss = sec % 60;
        out[0] = (wchar_t)('0' + (mm / 10) % 10);
        out[1] = (wchar_t)('0' + mm % 10);
        out[2] = (wchar_t)':';
        out[3] = (wchar_t)('0' + (ss / 10) % 10);
        out[4] = (wchar_t)('0' + ss % 10);
        out[5] = 0;
    }
}

void SWS_Draw(void* surf, int w, int h, int pitch)
{
    int order[SWS_CAP];
    int colCount[4] = { 0, 0, 0, 0 };
    int colX[4] = { 0, 0, 0, 0 };
    int i, n, f, totW, xStart, yStart, rowH, maxRows, blockH;
    int curSel = -9999;
    void* pal = NULL;
    pal = *(void**)ADDR_CAMEO_PAL;
    if (!pal || !IsPlausiblePtr(pal))
        pal = *(void**)ADDR_SIDEBAR_PAL;
    if (pal && !IsPlausiblePtr(pal)) pal = NULL;
    unsigned char* vt;
    SwsLockFn lockFn;
    SwsUnlockFn unlockFn;
    unsigned char* locked;
    if (!s_Enable || !s_Visible) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    n = s_ItemN;
    if (n <= 0 || n > SWS_CAP)
    {
        /* 无超武时画探针 (Probe=1): 4 个阵营色块 + 画布尺寸,
         * 只证明绘制管线与位置, 不占点击 (BoundValid=0)。 */
        if (s_Probe && n <= 0 && surf && IsPlausiblePtr(surf) &&
            w > 0 && h > 0 && pitch > 0 &&
            GdiEnsure(s_FontPx, s_Bold, 5, s_Face))
        {
            int adv = g_G->adv > 0 ? g_G->adv : s_FontPx + 2;
            int bw = s_IconW, bh = 22, f, py;
            int px = w - s_RightMargin - bw;
            unsigned char* vt2;
            SwsLockFn lockFn2;
            SwsUnlockFn unlockFn2;
            unsigned char* locked2;
            wchar_t dim[24];
            char b1[16], b2[16];
            int dl = 0, k;
            if (px < 0) px = 0;
            py = (h - (4 * (bh + s_Gap) - s_Gap)) / 2;
            if (py < 0) py = 0;
            if (!g_Calibrated) Calibrate();
            for (f = 0; f < 4; f++)
            {
                const unsigned char* c = kSideCol[f];
                int by = py + f * (bh + s_Gap);
                SwsFillTrans(surf, px, by, bw, bh, c[0], c[1], c[2], 35);
                SwsFillTrans(surf, px, by, bw, 1, c[0], c[1], c[2], 95);
                SwsFillTrans(surf, px, by + bh - 1, bw, 1, c[0], c[1], c[2], 95);
                SwsFillTrans(surf, px, by, 1, bh, c[0], c[1], c[2], 95);
                SwsFillTrans(surf, px + bw - 1, by, 1, bh, c[0], c[1], c[2], 95);
            }
            UtoA((unsigned)w, b1);
            for (k = 0; b1[k] && dl < 20; k++) dim[dl++] = (wchar_t)(unsigned char)b1[k];
            dim[dl++] = (wchar_t)'x';
            UtoA((unsigned)h, b2);
            for (k = 0; b2[k] && dl < 23; k++) dim[dl++] = (wchar_t)(unsigned char)b2[k];
            dim[dl] = 0;
            vt2 = *(unsigned char**)surf;
            if (vt2 && IsPlausiblePtr(vt2))
            {
                lockFn2 = (SwsLockFn)(*(void**)(vt2 + VT_LOCK));
                unlockFn2 = (SwsUnlockFn)(*(void**)(vt2 + VT_UNLOCK));
                if (lockFn2 && unlockFn2 &&
                    IsPlausiblePtr((const void*)lockFn2) &&
                    IsPlausiblePtr((const void*)unlockFn2))
                {
                    int dw;
                    locked2 = (unsigned char*)lockFn2(surf, NULL, 0, 0);
                    if (locked2 && IsPlausiblePtr(locked2))
                    {
                        dw = RenderLine(dim, dl);
                        if (dw > 0)
                        {
                            int dx = px + (bw - dw) / 2;
                            int dy = py + 4 * (bh + s_Gap) + 2;
                            if (dx < 0) dx = 0;
                            BlitCoverage(locked2, pitch, w, h, dx + 1, dy + 1,
                                         dw + 2, adv, 0, 0, 0, 70);
                            BlitCoverage(locked2, pitch, w, h, dx, dy,
                                         dw + 2, adv, 255, 255, 0, 100);
                        }
                        unlockFn2(surf, NULL);
                    }
                }
            }
        }
        return;
    }
    /* 排序: 阵营 -> 优先级 -> 超武序号 (插入排序, n 小)。 */
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
    for (f = 0; f < 4; f++) if (colCount[f] > maxRows) maxRows = colCount[f];
    if (maxRows <= 0) return;
    rowH = s_IconH + s_Gap;
    blockH = maxRows * rowH - s_Gap;
    totW = 0;
    for (f = 0; f < 4; f++)
        if (colCount[f] > 0) totW += (totW ? s_ColGap : 0) + s_IconW;
    xStart = w - s_RightMargin - totW;
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
            it->x = colX[it->side];
            it->y = yStart + rows[it->side] * rowH;
            rows[it->side]++;
        }
    }
    s_BoundX = xStart;
    s_BoundY = yStart;
    s_BoundW = totW;
    s_BoundH = blockH;
    s_BoundValid = 1;
    if (!GdiEnsure(s_FontPx, s_Bold, 5, s_Face)) return;
    {
        int adv = g_G->adv > 0 ? g_G->adv : s_FontPx + 2;
        /* 背景 + 边框 (免锁, FillRectTransVt 自走虚表)。 */
        for (i = 0; i < n; i++)
        {
            SwsItem* it = &s_Items[order[i]];
            const unsigned char* c = kSideCol[it->side];
            if (it->pcxFile)
            {
                /* 引擎 PCX 贴图 (含色键), 目标 Composite, 同 Phobos。
                 * surface 每帧现取, 不跨帧持有。 */
                void* pcxInst = (void*)ADDR_PCX_INSTANCE;
                void* ps = SwsPcxLiveSurf(it->pcxFile);
                char rect[16];
                /* 目标面用 SWS_Draw 收到的 surf (游戏正在合成的那张),
                 * 不要用硬编码的 0x88731C: 本机 Ares 构建里那个地址的
                 * 虚表是 NULL, BlitToSurface 内部一调虚函数就跳 0 崩溃
                 * (转储: EIP=0, ECX=0x88731C, 返回地址 0x6BA597)。*/
                if (ps && pcxInst && IsPlausiblePtr(pcxInst))
                {
                    *(int*)(rect + 0) = it->x; *(int*)(rect + 4) = it->y;
                    *(int*)(rect + 8) = s_IconW; *(int*)(rect + 12) = s_IconH;
                    FN_PCXBLIT(pcxInst, rect, surf, ps, PCX_TRANS_PARENT);
                }
                else
                {
                    SwsFillTrans(surf, it->x, it->y, s_IconW, s_IconH, 0, 0, 0,
                                 s_Opacity);
                }
            }
            else if (it->shp && pal)
            {
                char pos[8], bnds[16];
                *(int*)(pos + 0) = it->x; *(int*)(pos + 4) = it->y;
                *(int*)(bnds + 0) = 0; *(int*)(bnds + 4) = 0;
                *(int*)(bnds + 8) = w; *(int*)(bnds + 12) = h;
                FN_DRAWSHAPE(surf, pal, it->shp, 0, pos, bnds,
                             SWS_BF_400, 0, 0, SWS_ZGROUND, 1000, 0,
                             NULL, 0, 0, 0);
            }
            else
            {
                SwsFillTrans(surf, it->x, it->y, s_IconW, s_IconH, 0, 0, 0,
                             s_Opacity);
            }
            if (!it->ready)
                SwsFillTrans(surf, it->x, it->y, s_IconW, s_IconH,
                             0, 0, 0, 45);
            SwsFillTrans(surf, it->x, it->y, s_IconW, 1, c[0], c[1], c[2], 90);
            SwsFillTrans(surf, it->x, it->y + s_IconH - 1, s_IconW, 1,
                         c[0], c[1], c[2], 90);
            SwsFillTrans(surf, it->x, it->y, 1, s_IconH, c[0], c[1], c[2], 90);
            SwsFillTrans(surf, it->x + s_IconW - 1, it->y, 1, s_IconH,
                         c[0], c[1], c[2], 90);
        }
        /* 选中高亮 (白框)。 */
        if (IsPlausiblePtr((void*)ADDR_CURSW))
            curSel = *(int*)ADDR_CURSW;
        if (curSel >= 0)
        {
            for (i = 0; i < n; i++)
            {
                SwsItem* it = &s_Items[order[i]];
                if (it->swIdx != curSel) continue;
                SwsFillTrans(surf, it->x - 1, it->y - 1, s_IconW + 2, 1,
                             255, 255, 255, 95);
                SwsFillTrans(surf, it->x - 1, it->y + s_IconH, s_IconW + 2, 1,
                             255, 255, 255, 95);
                SwsFillTrans(surf, it->x - 1, it->y, 1, s_IconH,
                             255, 255, 255, 95);
                SwsFillTrans(surf, it->x + s_IconW, it->y, 1, s_IconH,
                             255, 255, 255, 95);
            }
        }
        /* 文本 + 进度条 (锁面一次)。 */
        if (!g_Calibrated) Calibrate();
        vt = *(unsigned char**)surf;
        if (!vt || !IsPlausiblePtr(vt)) return;
        lockFn = (SwsLockFn)(*(void**)(vt + VT_LOCK));
        unlockFn = (SwsUnlockFn)(*(void**)(vt + VT_UNLOCK));
        if (!lockFn || !unlockFn ||
            !IsPlausiblePtr((const void*)lockFn) ||
            !IsPlausiblePtr((const void*)unlockFn)) return;
        locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
        if (!locked || !IsPlausiblePtr(locked)) return;
        for (i = 0; i < n; i++)
        {
            SwsItem* it = &s_Items[order[i]];
            int sl = 0, sw, sx, ny2;
            wchar_t st[8];
            unsigned char fr, fg, fb;
            SwsBuildStatus(it, st);
            while (sl < 7 && st[sl]) sl++;
            /* 名 (上) / 状态 (下), 居中。 */
            sw = RenderLine(st, sl);
            ny2 = it->y + s_IconH - adv - 7;
            if (ny2 < it->y + 2) ny2 = it->y + 2;
            /* 字大盒矮时只画名 (状态行放不下就不画, 防串到隔壁图标)。 */
            if (ny2 + adv > it->y + s_IconH - 4) sw = 0;
            if (sw > 0)
            {
                sx = it->x + (s_IconW - sw) / 2;
                if (sx < it->x + 2) sx = it->x + 2;
                if (it->susp) { fr = 255; fg = 60; fb = 60; }
                else if (it->ready) { fr = 0; fg = 255; fb = 0; }
                else { fr = 255; fg = 255; fb = 255; }
                BlitCoverage(locked, pitch, w, h, sx + 1, ny2 + 1, sw + 2,
                             adv, 0, 0, 0, 70);
                BlitCoverage(locked, pitch, w, h, sx, ny2, sw + 2, adv,
                             fr, fg, fb, 100);
            }
            /* 进度条 (底边 3px): 就绪满绿, 蓄力按比黄, 未知灰。 */
            {
                int bx = it->x + 3, bw = s_IconW - 6;
                int by = it->y + s_IconH - 5;
                int fill = 0;
                unsigned char br = 120, bg = 120, bb = 120;
                if (bw > 0 && by + 3 <= h)
                {
                    if (it->ready) { fill = bw; br = 0; bg = 255; bb = 0; }
                    else if (it->total > 0)
                    {
                        fill = (bw * (it->total - it->remain)) / it->total;
                        br = 255; bg = 220; bb = 0;
                    }
                    if (fill > bw) fill = bw;
                    if (fill > 0)
                    {
                        int yy, xx;
                        for (yy = 0; yy < 3; yy++)
                        {
                            int sy = by + yy;
                            unsigned short* row;
                            if (sy < 0 || sy >= h) continue;
                            row = (unsigned short*)(locked + sy * pitch);
                            for (xx = 0; xx < fill; xx++)
                            {
                                int sx2 = bx + xx;
                                if (sx2 < 0 || sx2 >= w) continue;
                                row[sx2] = Blend16(row[sx2], br, bg, bb, 220);
                            }
                        }
                    }
                }
            }
        }
        unlockFn(surf, NULL);
    }
}
