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
 *     搬出**只能少删**: 判据是"侧栏此刻真的在画它"的真子集 (类型门 ∧ 实例门
 *     ∧ 扫描快照门), 因为搬出单向, 多删一条就是超武彻底消失。
 *   - 清单: 本家拥有的超武 (`SuperClass::Array` 里 owner==本家且 IsPresent==1),
 *     **不是 rules ini, 也不是防御支援页** —— 两者都只覆盖基础规则那一份数据。
 *   - 规则旗标 (ShowCameo / ManualFire / UseAITargeting / SidebarPCX /
 *     ChargeToDrainRatio 等): 一律读 Ares 已算好的 ExtData, 见下面的
 *     "Ares SWTypeExt::ExtData" 一节。
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

/* DynamicVectorClass 自洽吗 —— **只判容器自己, 一个对象都不碰**。
 *   +0x00 vtable / +0x04 Items / +0x08 Capacity / +0x0C IsInit+IsAlloc
 *   +0x10 Count / +0x14 CapacityIncrement                  sizeof 0x18
 * 成功返回 Items 指针并写出 Count (Count==0 是合法的"还没有", 由调用方自己判),
 * 失败返回 NULL。IsPlausiblePtr 是纯算术 (GdiText.cpp), 不读内存, 怎么调都安全。
 *
 * ⚠⚠ **上界不许拍脑袋**。原先写 `count > 4096` 当"这是垃圾值"的判据, 而 MOE
 * 满编剧本里 `SuperClass::Array` 实测就有 **4480 条** (`items=0x322A3570
 * cap=4480 ini=1 cnt=4480 inc=10`, 容器完全自洽, ≈9 个 House × ~500 个超武
 * 类型) —— 于是那些剧本里整个超武侧栏**静默消失**, 症状就是"有时候能显示有时候
 * 不能"。真正的安全网是**逐项全校验** (type / owner==本家 / IsPresent / ID 可打印
 * / 标志位 0或1, 见 SwsScan), 上界只该卡"大到读不下去"。
 * `VectorClass::Add` 先 Reserve 再写 `Items[Count++]`, 所以 `Count <= Capacity`
 * 恒成立 —— 这一条可以当硬门, 它同时挡掉"Count 是野值"的真垃圾。 */
#define SWS_VEC_HARD_MAX 65536
static void** SwsVecItems(unsigned base, int* pCount)
{
    void** items;
    int cap, cnt;
    if (!IsPlausiblePtr((void*)(size_t)base)) return NULL;
    items = ARR_ITEMS(base);
    cap = *(int*)(base + 8u);
    cnt = ARR_COUNT(base);
    if (!items || !IsPlausiblePtr(items)) return NULL;
    if (cap < 1 || cap > SWS_VEC_HARD_MAX) return NULL;
    if (cnt < 0 || cnt > cap) return NULL;   /* Count<=Capacity 恒成立, 反之即垃圾 */
    if (pCount) *pCount = cnt;
    return items;
}
#define ADDR_FRAME       0xA8ED84u
#define ADDR_SCENARIO    0xA8B230u
#define OFF_SCEN_SIDE    0x34B8u
#define ADDR_CURSW       0x8809A0u   /* Display(0x87F7E8)+0x11B8 */

/* Super 字段 (括号里是 YRpp 头文件值, 本机实测 = 头文件 + 0x0C) */
#define OFF_SUP_TYPE     0x28u   /* (0x1C) T405 全中 */
#define OFF_SUP_OWNER    0x2Cu   /* (0x20) H405 全中 */
/* ★ 引擎 `SuperClass::GetCameoChargeStage` 的**原式** (gamemd 0x6CBF85 起,
 * Ares 在 0x6CBF5B 挂了 ChargeDrain 分支):
 *
 *     006CBF85  mov edi, [ecx+0x24]        ; A = 实例级倒计时总长
 *     006CBF88  cmp edi, -1
 *     006CBF8B  mov esi, edi
 *     006CBF8D  jne 0x6CBF95
 *     006CBF8F  mov esi, [ebx+0xB0]        ; A == -1 ⇒ 回退 Type->RechargeTime
 *     006CBF95  mov eax, [ecx+0x38]        ; Left
 *     006CBF98  mov ecx, [ecx+0x30]        ; Start
 *     006CBF9B  cmp ecx, -1  / je          ; Start == -1 ⇒ 不扣流逝帧
 *     006CBFA0  mov edx, [0xA8ED84]        ; CurrentFrame
 *     006CBFA6  sub edx, ecx               ; elapsed
 *     006CBFA8  cmp edx, eax / jge → xor    ; elapsed >= Left ⇒ rem = 0
 *     006CBFAC  sub eax, edx               ; 否则 rem = Left - elapsed
 *     006CBFB7  sub esi, eax  ; 006CBFC1 fild ; pct = (A - rem) / A
 *
 * ⚠⚠ 之前**只**拿 `Type->RechargeTime` 当总量, 漏了 `SuperClass+0x24` 这个
 *   "每个实例自己的倒计时总长"。难度不同 / 剧情设定不同都会写它 ——
 *   这就是"终结冲击在我们侧栏从 50% 开始、原本侧栏却是 11%"的成因:
 *   两者用了不同的分母。`ebx` 是 Type、`ecx` 是 Super, 别把两者搞混。
 */
#define OFF_SUP_RTTOTAL  0x24u   /* (0x18) 实例级倒计时总长, -1 = 用类型的 */
/* ChargeDrainState: 0=Charging / 1=Ready / 2=Draining(运作中)。
 * 实证见 SwsScan 里 s_Items[n].drainState 那段注释 (Ares 二进制 0x1003517F/0x10035217)。 */
#define OFF_SUP_DRAINSTATE 0x7Cu
#define OFF_SUP_RTSTART  0x30u   /* (0x24) RechargeTimer.Start */
#define OFF_SUP_RTLEFT   0x38u   /* (0x2C) RechargeTimer.Left  */
#define OFF_SUP_PRESENT  0x6Du   /* (0x61) IsPresent  */
#define OFF_SUP_READY    0x6Fu   /* (0x63) IsReady    */
#define OFF_SUP_SUSPEND  0x70u   /* (0x64) IsSuspended */

/* SuperWeaponType 字段 (同样 +0x0C 漂移; ID 与 Super 同源, 计数已验证)。
 *
 * ⚠ `ManualControl` 的 ini 键名是 **`ManualControl`** (gamemd .rdata 0x842644
 *   字符串实证), **不是** `SW.ManualFire` —— `SW.ManualFire` 是 **Ares** 的键,
 *   写进 Ares 自己的 `SWTypeExt::ExtData::SW_ManualFire`, 与本字段无关。以前
 *   拿 `SW.ManualFire` 跟这个字段对账, 于是永远对不上 (那条 `flags OFFSET?`
 *   日志**实测一直触发**), 而真正该读的 `ManualControl=` 又没人读 ⇒ 判据整个
 *   是空的, `ManualControl=no`/`SW.ManualFire=no` 的那批一个都过滤不掉
 *   (IronGuardSpecial 就是这么漏进侧栏的)。
 */
#define OFF_SWT_ID       0x24u   /* (0x18) */
#define OFF_SWT_UINAME   0x60u   /* (0x54) */
#define OFF_SWT_ARRIDX   0x98u   /* (0x8C) */
#define OFF_SWT_ACTION   0xBCu   /* (0xB0) Action: 0 = None = 点击即生效 */
#define OFF_SWT_RECH     0xB0u   /* (0xA4) RechargeTime */
#define OFF_SWT_DRAIN       0xE5u /* UseChargeDrain */
#define OFF_SWT_MANUALCTRL  0xF5u /* ManualControl */

#define VT_LOCK            0x5C
#define VT_UNLOCK          0x60

#define ADDR_CLICKGATE_SKIP 0x006925FCu

#define SWS_CAP 160
#define SWS_INI_CAP 160
/* 类型下标位图 (swIdx) 的尺寸。防御页 cameo 的 ItemIndex、Super 的
 * Type->ArrayIndex、SuperWeaponType::Array 下标三者同源
 * (HouseClass::Supers 按类型序建), 所以一张按 swIdx 取位的表就能同时表示
 * "见过" 和 "活的"。2048 远大于实测类型数 (~500), 上界只作野值闸门。 */
#define SWS_LIVE_BITS 2048
#define SWS_LIVE_BYTES (SWS_LIVE_BITS / 8)

/* =======================================================================
 * Ares SWTypeExt::ExtData —— 超武行为的**权威来源**
 *
 * 不自己复刻 ini 解析, 直接读 Ares 已经算好的结果: Ares 把每个超武的扩展数据
 * 放在 `SWTypeExt::ExtMap` (ContainerMap<SuperWeaponTypeClass, ExtData>), 键是
 * **类型对象指针** (`ExtMap.Find(pSuper->Type)`, 见 `Ext/SWType/Hooks.Update.cpp:188`
 * 与 `_Container.hpp:226` `using key_type = base_type*`)。
 *
 * ★ 硬证据 —— `HouseClass_UpdateSuperWeaponsUnavailable` 里 AddCameo 的判门
 *   (`0x1003961D`, 对应 `Hooks.Update.cpp:186-193`):
 *       mov ecx, <ExtMap>; push [eax+0x28]; call find
 *       cmp byte ptr [eax+0x1E5], 0     ; SW_ShowCameo
 *       jne  → AddCameo(0x1F, index)
 *   `+0x1E5` 就是引擎自己决定"进不进防御页"的那个 1 字节 bool, 读它 = 与引擎
 *   逐位一致, 不会出现"游戏里没有、侧栏里却有"。
 *
 * 字段偏移取自 `SWTypeExt::ExtData::LoadFromINIFile` (0x10033300 附近), 指令模式固定:
 *     push <键名>; push edi; lea eax,[esp+18h]; push eax;
 *     lea ecx,[esi+OFF];  call Valueable<T>::Read
 * `esi` = ExtData 基址, 所以 `OFF` 就是字段偏移。逐条 (只认紧邻的那一对):
 *     10033451 lea ecx,[esi+0x1E2]  <- "SW.ManualFire"
 *     1003347D lea ecx,[esi+0x1E5]  <- "SW.ShowCameo"      ★
 *     10033499 lea ecx,[esi+0x1E6]  <- "SW.Unstoppable"
 *     100335C7 lea ecx,[esi+0x20E]  <- "SW.UseAITargeting" ★
 *     10033B60 lea ecx,[esi+0x4B0]  <- "SidebarPCX"        ★
 *
 * ★ 定位一律**从 Ares 自己的指令里读地址**, 不写死绝对地址 (Ares.dll 的
 *   ImageBase 是 0x10000000, ASLR 后 base 会变):
 *   - `ExtMap` = AresBase + 0xC1C54 —— 从 `mov ecx, <imm32>` 取。
 *   - `find()` = 该 `call` 的落点 (AresBase + 0x57C70)。
 * ===================================================================== */
#define ARES_EXT_OWNER     0x000u   /* AttachedToObject = SuperWeaponTypeClass*
                                      * (Ares `Extension<T,TExt>` 的 0 号成员:
                                      *  `_Container.hpp:61` 定义, :74 自带注释
                                      *  "offset 0 of an ExtData is AttachedToObject") */
#define ARES_EXT_MANUAL    0x1E2u   /* SW_ManualFire     (bool, 1 字节) */
#define ARES_EXT_SHOW      0x1E5u   /* SW_ShowCameo      (bool, 1 字节) */
#define ARES_EXT_AITGT     0x20Eu   /* SW_UseAITargeting (bool, 1 字节) */
#define ARES_EXT_PCXNAME   0x4B0u   /* SidebarPCX 文件名 (定长串, 已小写) */
#define ARES_EXT_RATIO     0x220u   /* SW.ChargeToDrainRatio (double) */
#define ARES_EXT_RATIO_SET 0x228u   /* 上面那个"ini 真的写了"标志 (bool) */
#define ARES_HOOK_SCAN_EXT 0x30u    /* 扫钩子体的字节数 (只用到 +0x1C) */
/* 同一个导出钩子的名字, 下面图标抓取那一节还要用 (此处先定义以便定位器引用) */
#define ARES_HOOK_PCX     "StripClass_Draw_SuperWeapon_PCX"

typedef void* (__thiscall *SwsAresFindFn)(void* self, const void* key);

static void*        s_AresExtMap = NULL;   /* ContainerMapBase* (静态对象) */
static SwsAresFindFn s_AresFind = NULL;
static int          s_AresExtFail = 0;
static int          s_AresExtTried = 0;

/* Ares 映像范围 + 后面的日志/名字校验助手 (定义都在下方, 这里先声明) */
static int SwsAresImage(unsigned char** ppLo, unsigned char** ppHi);
static int SwsIdOk(const char* s);
static void LogS(const char* tag, const char* s);
static void LogI(const char* tag, int v);
static int SwsTabCameoOk(void);
static int SwsEngineCleanList(void);
static void SwsCleanStampCheck(void);
static int SwsStripCameoCount(int tab);
#define TAB_DEFENSE           1      /* 0 建筑 / 1 防御 / 2 步兵 / 3 载具 (见下重复定义) */

/* 引擎干净清单的**快照** (按超武下标开位)。
 * `s_CleanMode==1` 时它是硬闸门: 不在快照里的一律不画。
 * 只增不减 —— 因为引擎那个容器会被去重删空, 直接读它会自我蚕食
 * (详见 SwsEngineCleanList 上方那段)。 */
static unsigned char s_CleanBits[SWS_LIVE_BYTES];
static int s_CleanMode = 0;
static int s_CleanN = -1;

/* 定位 ExtMap + find()。地址全部来自 Ares 自己的代码, fail-closed。 */
static int SwsAresLocateExt(void)
{
    unsigned char *base, *imgHi, *p, *fn;
    FARPROC hook;
    int gotMap = 0, gotCall = 0;
    unsigned mapAddr = 0;
    if (s_AresExtMap && s_AresFind) return 1;
    if (s_AresExtFail) return 0;
    /* ⚠⚠ 这两处失败**绝不能**置 `s_AresExtFail` (闩死): Ares.dll 未必在第一次
     *   询问时就已经映射进来 (SwsFillType 可能被 `SwsSidebarShows` 提前调到,
     *   那条路没有 `frame >= 60` 的门槛)。一旦在这里闩死, 后面再也不会重试,
     *   于是**全程 x0 且一行日志都没有** —— 卡在这上面很久。
     *   判据: "还没准备好"是可恢复的, 只有"准备好了但内容不对"才是真失败。 */
    if (!SwsAresImage(&base, &imgHi)) return 0;
    hook = GetProcAddress((HMODULE)base, ARES_HOOK_PCX);
    if (!hook || (unsigned char*)(size_t)hook < base ||
        (unsigned char*)(size_t)hook >= imgHi) return 0;
    fn = (unsigned char*)(size_t)hook;
    for (p = fn; p + 6 <= fn + ARES_HOOK_SCAN_EXT; p++)
    {
        /* B9 <imm32>              mov ecx, ExtMap */
        if (p[0] == 0xB9) { memcpy(&mapAddr, p + 1, 4); gotMap = 1; }
        /* E8 <rel32>              call find */
        else if (p[0] == 0xE8)
        {
            int rel;
            memcpy(&rel, p + 1, 4);
            s_AresFind = (SwsAresFindFn)(size_t)(unsigned)(p + 5 + rel);
            gotCall = 1;
        }
    }
    if (!gotMap || !gotCall) goto fail;
    /* ExtMap 必须落在 Ares 映像内的数据区 (.data: VA 0xC0000..0xC2C44) */
    if ((unsigned char*)(size_t)mapAddr < base ||
        (unsigned char*)(size_t)mapAddr >= imgHi) goto fail;
    /* find() 必须是 Ares 映像内的可执行体 */
    if ((unsigned char*)(size_t)s_AresFind < base ||
        (unsigned char*)(size_t)s_AresFind >= imgHi) goto fail;
    /* ★ 必须确认签名真的是 "1 个栈参 + 自己清栈" (`ret 4` = C2 04 00)。
     *   我们是**跨模块**按指针调它: 签名猜错 = 栈被清错 = 崩溃, 而不是返回 NULL。
     *   所以这里把落点附近的 `ret` 指令字节抠出来核对 —— 对不上就整个 ExtData
     *   路线停用, 退回 ini 快照 (行为不倒退, 只是少了图标/即时判定)。
     *   实测本机 3.0 的 Epilogue 在 +0x73 (`0x10057CE3`)。 */
    {
        const unsigned char* f = (const unsigned char*)(size_t)s_AresFind;
        int ok4 = 0, i;
        for (i = 0; i < 0x100; i++)
            if (f[i] == 0xC2 && f[i + 1] == 0x04 && f[i + 2] == 0x00)
            { ok4 = 1; break; }
        if (!ok4) goto fail;
    }
    s_AresExtMap = (void*)(size_t)mapAddr;
    /* 定位成功必须**自证**一行: 三个地址都打出来, 否则"取不到 ExtData"根本无从
     * 分辨是"没定位到"还是"定位到了但 find() 查不到"。 */
    {
        char b[16], buf[96];
        unsigned q = AppendCStr(buf, 0, "sws: extmap map=", sizeof(buf));
        HtoA((unsigned)(size_t)s_AresExtMap, b);
        q = AppendCStr(buf, q, b, sizeof(buf));
        q = AppendCStr(buf, q, " find=", sizeof(buf));
        HtoA((unsigned)(size_t)s_AresFind, b);
        q = AppendCStr(buf, q, b, sizeof(buf));
        q = AppendCStr(buf, q, " base=", sizeof(buf));
        HtoA((unsigned)(size_t)base, b);
        AppendCStr(buf, q, b, sizeof(buf));
        LogLine(buf);
    }
    return 1;
fail:
    s_AresExtMap = NULL;
    s_AresFind = NULL;
    s_AresExtFail = 1;
    LogI("sws: extmap locate FAIL m=", gotMap);
    LogI("sws: extmap locate FAIL c=", gotCall);
    return 0;
}

/* 取某超武的 Ares ExtData; 取不到返回 NULL。
 *
 * ⚠ **键 = `SuperWeaponTypeClass*` 本身, 不是 ID 字符串**。
 *   - Ares 源码 `Ext/SWType/Hooks.Update.cpp:188`: `SWTypeExt::ExtMap.Find(pSuper->Type)`,
 *     `_Container.hpp:226` `using key_type = base_type*` (base_type = SuperWeaponTypeClass)。
 *   - 本机 3.0 二进制同一条路 (`0x1003961D`):
 *         mov eax,[esi]            ; SuperClass*
 *         mov ecx, <ExtMap>
 *         push [eax+0x28]          ; ← SuperClass+0x28 = Type 指针
 *         call ContainerMapBase::find
 *     同一函数里紧接着 `mov edi,[eax+0x98]` = `Type->ArrayIndex`, 反证 +0x28 取到的
 *     确实是 SuperWeaponTypeClass*。
 *   - `find` 的散列是对**指针自己的 4 个字节**做 FNV-1a (`movzx eax,bl` / `shr eax,8`
 *     / `xor 0x811C9DC5` / `imul 0x1000193`), 全程不解引用 ⇒ 键只可能是指针。
* ⚠ 早先误把 `+0x24` 当成"存放 `char* ID` 的指针字段", 于是 `*(void**)(type+0x24)`
 *   读出来的是**字符串内容** (`oy1S` / `Warh` / `eadS` 之类), 三个候选键全错,
 *   还因此踩出一次 CRASH。实际它是**内联定长字符数组**, 键就是它的首地址。
 *
 * 失败不致命: 调用方全部退回旧的 ini 快照路径, 行为不倒退。 */
static void* SwsAresExt(void* type)
{
    void* p;
    int slot = 0;
    if (!type || !IsPlausiblePtr(type)) return NULL;
    if (!SwsAresLocateExt()) return NULL;
    if (!SwsIdOk((const char*)type + OFF_SWT_ID)) return NULL;
    s_AresExtTried++;
    /* ★ 键 = **`SuperWeaponTypeClass*` 本身** (类型指针), 不是 `&type->ID`。
     *
     * 证据 (本机 Ares.dll `HouseClass_UpdateSuperWeaponsUnavailable` @0x100395EE,
     * 对应源码 `Ext/SWType/Hooks.Update.cpp:188`):
     *     100395EE  mov eax, [ecx+0x28]      ; SuperClass+0x28 = Type
     *     100395F1  mov edi, [eax+0x98]      ; Type->ArrayIndex (反证 +0x28 是 Type)
     *     1003961D  mov eax, [esi]           ; = pSuper
     *     10039624  push [eax+0x28]          ; ★ 键 = pSuper->Type
     *     10039627  call find
     * 与源码 `SWTypeExt::ExtMap.Find(pSuper->Type)` 逐字一致; `_Container.hpp:226`
     * `using key_type = base_type*` 佐证。
     *
     * ⚠ 曾被 `StripClass_Draw_SuperWeapon_PCX` 的 `push [eax+0x24]` 误导 —— 那里
     *   eax 是**另一个**对象, 与本路径无关。按 &type->ID 查恒返回 NULL。
     *
     * 取到之后必须校验 `AttachedToObject == type`: `find()` 按指针哈希查桶,
     * 碰撞会返回别人的 ExtData, 不校验就会把别的超武的 ShowCameo/PCX 套到这个
     * 类型上 (症状是图标/过滤随机错且难复现)。 */
    p = s_AresFind(s_AresExtMap, type);
    if (!p) { slot = 1; goto miss; }
    if (!IsPlausiblePtr(p)) { slot = 2; goto miss; }
    if (*(void**)((const char*)p + ARES_EXT_OWNER) != type) return NULL;
    return p;
miss:
    /* 只记一次, 免得每帧刷屏。
     * ⚠ `slot` 进来时已经是非 0 (1=NULL, 2=野指针), 所以判据必须用**函数级
     *   static**, 不能用 `slot` —— 之前写成 `if (!slot)` 等于永远不成立,
     *   把"`find()` 返回 NULL"这个最关键的现象整整藏了两轮。 */
    {
        static int s_Said = 0;
        if (!s_Said)
        {
            s_Said = 1;
            LogI(slot == 1 ? "sws: ext find() returned NULL, id="
                           : "sws: ext find() ptr implausible", s_AresExtTried);
            {
                char b[40], buf[128];
                unsigned q = AppendCStr(buf, 0, "sws: ext tried type=", sizeof(buf));
                /* ⚠ 必须接住 AppendCStr 的返回值 —— 丢掉它下一段会写回同一
                 *   个位置, 把 ID 整段覆盖掉, 看起来就像"ID 是空的"。 */
                q = AppendCStr(buf, q, (const char*)type + OFF_SWT_ID, sizeof(buf));
                q = AppendCStr(buf, q, " type=", sizeof(buf));
                HtoA((unsigned)(size_t)type, b);
                q = AppendCStr(buf, q, b, sizeof(buf));
                q = AppendCStr(buf, q, " map=", sizeof(buf));
                HtoA((unsigned)(size_t)s_AresExtMap, b);
                AppendCStr(buf, q, b, sizeof(buf));
                LogLine(buf);
            }
        }
    }
    return NULL;
}

/* 读 ExtData 里的 1 字节 bool。ExtData 是 operator new 出来的堆块, 未初始化
 * 垃圾值一律当 false —— 宁可多藏一个 (可点"过滤"放出来), 不可漏藏一个。 */
static int SwsAresExtBool(void* ext, unsigned off)
{
    unsigned char v;
    if (!ext) return -1;
    v = *(const unsigned char*)((const char*)ext + off);
    return v ? 1 : 0;
}

/* 取 ExtData 里的 `SW.ChargeToDrainRatio` (火风暴类"维持时长 = 充能时间 × ratio")。
 * Ares 存的是 **double** @ +0x220, +0x228 是"ini 里真的写了"的标志位; 没写就
 * 回退**全局默认 1.0** —— 见 Ares `SWTypeExt::GetChargeToDrainRatio()`:
 *     10032690  cmp byte [ecx+0x228], 0
 *     10032699  fld  qword [ecx+0x220]          ; ini 写了 → 用它
 *     100326A5  fld  qword [eax+0x760]          ; 没写   → 1.0
 * 返回 0 表示"读不到" (ExtData 没找到 / 值不合理), 调用方回退 1.0。
 *
 * ⚠ 为什么不从"激活瞬间的 TimeLeft"反推维持时长: 那要跨帧缓存一个峰值, 而且
 *   玩家在充能中途激活时 duration 会被 (rechargeTime - left) 缩放, 峰值不等于
 *   真正的维持时长。直接乘 ratio 是解析式的, 和引擎 `ClickFire` 里
 *   `duration = (rechargeTime - left) * ratio` 逐字对应。 */
static double SwsAresExtRatio(void* ext)
{
    double r;
    if (!ext) return 0.0;
    if (!*(const unsigned char*)((const char*)ext + ARES_EXT_RATIO_SET)) return 1.0;
    r = *(const double*)((const char*)ext + ARES_EXT_RATIO);
    /* 只接受 (0,1]: ratio=0 会让分母为 0 (除零), >1 是脏数据。 */
    if (!(r > 0.0) || r > 1.0) return 1.0;
    return r;
}

/* 取 ExtData 里的 SidebarPCX 文件名。Ares 自己已经把串小写化并存成定长串,
 * 直接拷出来即可 (只拷**名字**, 不碰 BSurface* —— 见下面图标一节的告警)。 */
static const char* SwsAresExtPcxName(void* ext, char* dst, size_t cap)
{
    const char* src;
    size_t i;
    if (!ext || !dst || cap < 2) return NULL;
    src = (const char*)((const char*)ext + ARES_EXT_PCXNAME);
    for (i = 0; i + 1 < cap && i < 0x20 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return dst[0] ? dst : NULL;
}

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
static int s_ForceScan = 0;   /* 点击时置 1: 强制立刻重扫, 不等 15 帧 */
static int s_ScrollStep = 1;  /* [SWSidebar] ScrollStep */
/* `sws: act` 自证日志的配额。换 ini/换剧本时由 SwsCacheEpoch 重给满 ——
 * 不重给的话第一局就把 40 条用完, 后面出问题的剧本正好没得可看。 */
static int s_DiagLeft = 40;

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

/* 列的**绘制顺序** (下标 = side, 值 = 从左到右第几列)。
 * 需求: 盟军靠屏幕外缘、焚风靠地图内侧。屏幕外缘是哪一侧取决于面板在哪边:
 *   面板在右 (原版侧栏在右, 地图右缘 1752) → 外缘是右边 → 焚风先出场, 盟军最后
 *                                                     = {3,2,1,0}
 *   面板在左 → 外缘是左边 → 盟军先出场, 焚风最后 = {0,1,2,3}
 * `SwsRebuild` 每帧按 `s_SideRight` 指向对应那张表。 */
static const int kColOrderRight[4] = { 3, 2, 1, 0 };   /* 焚风→盟军 (盟军靠屏幕缘) */
static const int kColOrderLeft[4]  = { 0, 1, 2, 3 };   /* 盟军→焚风 (盟军靠屏幕缘) */
static const int* kColOrder = kColOrderRight;

/* ---------------- 扫描结果 ---------------- */

typedef struct {
    void* type;
    int swIdx;
    int side;
    int prio;
    int ready;
    int susp;
    /* 火风暴类状态机: 0=Charging / 1=Ready / 2=Draining(运作中)。
     * 只有 UseChargeDrain 的超武非 0 (其他类型该字段无意义, 由 tc->drain 门控)。 */
    int drainState;
    /* 运作中百分比分母 = 满充能时间 × ChargeToDrainRatio (= 维持时长)。
     * 只有 drainState==2 时才用; 0 表示没算出来, 绘制层回退到 total。 */
    int drainTotal;
    int remain;   /* 剩余帧 */
    int total;    /* 总帧 (<=0=未知) */
    int x, y;
    /* 原版侧栏的排序键 (gamemd `BuildType::SortsBefore` @0x6A8420):
     *   1) `Type->RechargeTime` 升序 (小的/冷却快的排前面)
     *   2) 相等则 `Type->UIName` 字典序升序 (`strcmp` <= 0)
     * 只用来自定义 ini 优先级的那几项, 其余按这个排 —— 见 SwsOrigNameLess。 */
    int rech;
    const char* pcxFile;  /* SidebarPCX= 文件名, 指向我们自己的表 */
    int instant;          /* 1 = 点击即生效 (Action=None 或 SW.UseAITargeting) */
    unsigned short* capPix;   /* 从引擎 cameo 抓下来的 60x48 像素 (优先于 pcxFile) */
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
static int s_SideRight = 1;         /* [SWSidebar] Side=right(默认)|left */
static int s_BtnX = 0, s_BtnY = 0, s_BtnW = 0, s_BtnH = 0;  /* 显隐按钮 */
/* 展开态那一帧记下的按钮坐标: 收起时原样沿用, 折叠/展开不许跳位。
 * 见 SwsCollapsedBtnGeom。 -1 = 还没展开过。 */
static int s_BtnXKeep = -1, s_BtnYKeep = -1;
/* SWS_Draw 算好的布局, 供点击/滚动判定使用 (现在由缓存重建路径发布) */
static int s_YStart = 0, s_RowH = 50, s_VisRows = 0;
/* 命中判定要用**列的横向范围**, 不能用 `s_BoundX`/`s_LastX`: 面板在右时列序是
 * 反的 (焚风→盟军), 循环结束时 `s_LastX` 反而是最左那一列, 用它做右边界会把
 * 整块判成"不在栏内"。 */
static int s_ColMinX = 0, s_ColMaxX = 0, s_MaxSk = 0;
/* 单阵营拆两列时, 被拆的那个阵营 side (0~3); -1 = 没拆。拆开后物理列 0/1
 * 共用它的边框色, 且条目按行奇偶分列, 见 SwsRebuild 的排序/定位两节。 */
static int s_SplitSide = -1;


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

/* 同上, 但返回"是否不同" (给"内容变了才记日志"用) */
static int SwsStrDiff(const char* a, const char* b)
{
    return !SwsStrEq(a, b);
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
            SwsStrEq(k, "ScrollStep") ||
            SwsStrEq(k, "Side") ||
            SwsStrEq(k, "DedupDefensePage");
        /* 漏一个全局键的后果: 它会被当成“超武 ID”收进 s_Ini, 显示成一个
         * 永远不亮的空条目。新增配置项**必须**同时加进这里, 并与
         * SwsEnsureInit 里真正读取的那份保持一致 —— 两边不一致时会留下堆早已删除的键, 排查时以为它们还生效。*/
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
    /* 原版"防御支援"页去重: 按钮的初始状态 (之后由点击切换)。 */
    s_DedupHide = GetPrivateProfileIntA(SWS_SECTION, "DedupDefensePage", 1,
                                        g_IniPath) ? 1 : 0;
    s_DedupDone = !s_DedupHide;
    s_ScrollStep = GetPrivateProfileIntA(SWS_SECTION, "ScrollStep", 1, g_IniPath);
    if (s_ScrollStep < 1) s_ScrollStep = 1;
    /* 面板停靠边: right(默认)=地图右缘 / left=地图左缘。跟着改的不只是锚点,
     * 还有**列序** (盟军永远靠屏幕外缘) 与按钮/滚动条/命中框的镜像。 */
    {
        char sideKey[16];
        sideKey[0] = 0;
        GetPrivateProfileStringA(SWS_SECTION, "Side", "right", sideKey,
                                 (unsigned)sizeof(sideKey), g_IniPath);
        s_SideRight = !(sideKey[0] == 'l' || sideKey[0] == 'L');
        kColOrder = s_SideRight ? kColOrderRight : kColOrderLeft;
    }
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
    int count = 0, i;
    items = SwsVecItems(ADDR_SWTYPE_ARR, &count);
    if (items && count > 0)
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

/* ---- 每类型缓存 ----
 * ArrayIndex (线性扫 135 项类型数组做字符串比较)、阵营、ini 优先级/隐藏
 * 都是**类型**的固有属性, 类型对象静态, 按 type 指针缓存一次即可, 之后每
 * 帧只剩指针比较。 */
#define SWS_TCACHE 640
typedef struct {
    void* type;
    int   swIdx;
    int   side;      /* -2 = 查表未命中, 要用玩家阵营 */
    int   prio;      /* -1 = 尚未填充 (SwsTypeEntry 用它当"已填"标志) */
    int   hide;
    int   show;      /* 1 = SW.ShowCameo (Ares: 默认 1;=0 压根不进防御页) */
    int   manual;    /* 1 = SW.ManualFire */
    int   drain;     /* 1 = UseChargeDrain */
    int   actionNone;/* 1 = 对象上的 Action == None (点击即生效) */
    int   instant;   /* 1 = 点击即发射 */
    double ratio;     /* SW.ChargeToDrainRatio (火风暴类维持时长系数, 缺省 1.0);
                       * 只对 drain 类有意义, 别的类留 1.0 */
    const char* pcxFile;
    char pcxName[64];  /* pcxFile 指向的自有缓冲 (文件名很短, 内联省一次间接) */
    unsigned short* capPix;  /* 引擎 cameo 抓到的 60x48 像素 (优先于 pcxFile) */
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
    s_TC[i].show = 1;
    s_TC[i].manual = 1;
    s_TC[i].drain = 0;
    s_TC[i].actionNone = 0;
    s_TC[i].instant = 0;
    s_TC[i].pcxFile = NULL;
    s_TC[i].pcxName[0] = 0;
    s_TC[i].capPix = NULL;
    s_TC[i].name[0] = 0;
    return &s_TC[i];
}

/* 三条显示旗标读**类型对象**: 引擎建对象用的是合并后的 ini
 * (基础 + gamemode + 地图/战役内嵌), 所以战役现场改写的值天然生效。
 * ⚠ 偏移已逐个对回 gamemd: CTOR (`0x6CE62E` 起) 写的是
 *   `[ebp+0xE6]=1 / [ebp+0xF5]=0 / [ebp+0xB8]=0 / [ebp+0xBC]=0`, 而
 *   `SuperWeaponTypeClass_FindIndex` (0x6CEE60) 用 `add ecx,0x24` 取 ID,
 *   `SidebarClass::DrawCameo` 用 `mov edx,[eax+0xB8]` 取 SidebarImage ——
 *   三处互证 0xBC=Action / 0xE5=UseChargeDrain / 0xF5=ManualControl。
 * 下面那条对账日志现在拿 `ManualControl` (gamemd 的键名, .rdata 0x842644)
 * 比 `0xF5`; 以前写的是 `SW.ManualFire` (Ares 的键), 所以**永远**对不上,
 * 于是"偏移可疑"这个结论一直挂着, 而真正的判据形同虚设。 */
static void SwsFlagsOf(SwsTypeCache* tc, void* type, const char* id)
{
    int iniManual = 1, iniHasManual = 0;
    void* rules = *(void**)ADDR_INI_RULES;
    tc->manual = *(unsigned char*)((char*)type + OFF_SWT_MANUALCTRL) ? 0 : 1;
    tc->drain = *(unsigned char*)((char*)type + OFF_SWT_DRAIN) ? 1 : 0;
    /* Action == None: 引擎不消费目标格, 原版侧栏点一下就直接打在鼠标上。
     * 默认值就是 None (CTOR `mov [ebp+0xBC],0`), 所以任何**没写** `Action=`
     * 的超武 —— 包括地图内嵌 ini 新增的那批 (WarheadSpecial1) —— 都是"点一下
     * 就生效"。MO 基础规则里的超武几乎都写了 `Action=Custom`, Ares 又把它改写
     * 成 `Actions::SuperWeaponAllowed` (0x7F), 落在 else 分支要选格。 */
    tc->actionNone = (*(int*)((char*)type + OFF_SWT_ACTION) == 0) ? 1 : 0;
    if (!rules || !IsPlausiblePtr(rules)) return;
    {
        char b[32];
        b[0] = 0;
        FN_INIREADSTR(rules, id, "ManualControl", "", b, (unsigned)sizeof(b));
        if (b[0])
        {
            iniHasManual = 1;
            if (SwsStrEq(b, "no") || SwsStrEq(b, "false")) iniManual = 0;
        }
    }
    /* 对账: 对"ini 里显式写过 ManualControl"的类型, 对象字段与 ini 读数应当一致
     * —— 不一致就说明偏移又推错了, 立刻能看出来。ini 没写过的 (只有地图内嵌 ini
     * 才声明的那些) 对不上是正常的, 不算错。 */
    if (iniHasManual && (iniManual == 0) != (tc->manual == 0))
        LogS("sws: flags OFFSET? manual ", id);
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
    void* ext = NULL;
    char pcxName[64];
    tc->side = SwsSideOf(id);
    tc->prio = SwsPrioOf(id, &h);
    tc->hide = h;
    /* 对象字段 = 引擎从**合并后**的 ini 建出来的值 (战役/地图改写天然生效):
     * ManualControl / UseChargeDrain / Action。ini 那份只用来对账 (SwsFlagsOf)。 */
    SwsFlagsOf(tc, type, id);

    /* 行为判据一律取 Ares 自己算好的 ExtData: `SW.ShowCameo` / `SW.ManualFire` /
     * `SW.UseAITargeting` / `SidebarPCX` / `SW.ChargeToDrainRatio` 全是 **Ares 的
     * 键**, 类型对象上没有对应字段; 且 Ares 对 ini 的合并顺序与改写 (例如把
     * `Action=Custom` 改成 `Actions::SuperWeaponAllowed` = 0x7F) 没法在外部复刻,
     * 而 ExtData 里存的就是算完的最终值。
     * 取不到时 (定位失败 / 该类型无扩展数据) 行为降级但不崩。 */
    ext = SwsAresExt(type);
    tc->ratio = 1.0;
    if (ext)
    {
        int v;
        if ((v = SwsAresExtBool(ext, ARES_EXT_SHOW)) >= 0)  tc->show = v;
        /* Ares 的 SW_ManualFire 与 gamemd 的 ManualControl 是两个独立的键
         * (rulesmo.ini 里 StasisGridSpawn/ConfusionGridSpawn 只写了后者),
         * 所以这里**只**用它来额外收紧, 绝不拿它去覆盖 manual。 */
        if (!SwsAresExtBool(ext, ARES_EXT_MANUAL)) tc->manual = 0;
        /* 火风暴类的维持时长系数: 侧栏算"运作中百分比"的分母要用它。 */
        tc->ratio = SwsAresExtRatio(ext);
    }
    if (!tc->show)
    {
        /* `SW.ShowCameo=no` 的压根不进防御页 (Ares
         * HouseClass_UpdateSuperWeaponsUnavailable: `if(pData->SW_ShowCameo)`
         * AddCameo), 侧栏跟着藏才是"与游戏一致" (IronGuardSpecial 就是这类)。
         * 记一行便于核对到底藏了谁 —— 只藏不记, 出问题分不清是"读错了"还是
         * "键名又写错了"。 */
        static int s_ShowHidden = 0;
        if (s_ShowHidden < 16)
        {
            LogS("sws: hide cameo ", id);
            s_ShowHidden++;
        }
    }
    /* 图标: Ares ExtData 里的 `SidebarPCX` 是权威 (已经是合并后的最终文件名,
     * 且已小写化), 优先于快照。取不到才退回快照。 */
    if (!ext || !SwsAresExtPcxName(ext, pcxName, sizeof(pcxName)))
        pcxName[0] = 0;
    tc->pcxFile = pcxName[0] ? pcxName : NULL;
    /* tc->pcxFile 可能指向 pcxName[] (栈上) 或快照里的静态串, 统一复制进
     * 类型缓存的自有缓冲, 免得下一轮 pcxName 出作用域后成为野指针。 */
    if (tc->pcxFile)
    {
        int k = 0;
        while (k < (int)sizeof(tc->pcxName) - 1 && tc->pcxFile[k])
        { tc->pcxName[k] = tc->pcxFile[k]; k++; }
        tc->pcxName[k] = 0;
        tc->pcxFile = tc->pcxName;
    }
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
        /* 缺 SidebarPCX 的超武: 一次性记下来, 便于查它们的图标 (还可能靠引擎
         * 抓取兜底, 见"从引擎手里拿超武 cameo 图标")。 */
        if (!tc->pcxFile) LogS("sws: nopcx ", id);
    }
    /* "点击即生效" = **一次点击就完成, 无需再点地图选格**。判据与 Phobos 的
 * `SWButtonClass::LaunchSuper` 逐字对齐 (MO 原版侧栏就是它):
 *   if (pType->Action == Action::None || pSWExt->SW_UseAITargeting)
 *       OutList.Add(Event(..., SpecialPlace, swIndex, CellStruct::Empty));
 *   else
 *       CurrentSWTypeIndex = swIndex;          // + EVA_SelectTarget
 * (Phobos `src/Ext/Sidebar/SWSidebar/SWButtonClass.cpp:203`)
 *
 * 两个条件缺一不可:
 *  - `Action == 0` 读**对象字段**: 引擎/Ares 建对象时已把合并后的 Action 写好,
 *    gamemd 自己的侧栏点击处理 (`0x6AAF08 cmp dword [ecx+0xBC],edi`) 判的也是它。
 *  - `SW_UseAITargeting` 读 **ExtData**: `WarheadSpecial1` 在 rulesmo.ini 里是
 *    `Type=GenericWarhead` + `Action=Custom` (Ares 在 0x1003332A 改写成 0x7F),
 *    纯 `Action==0` 判不出来; 它"点一下就生效"靠的是 emigdal.map 内嵌 ini 里
 *    那行 `SW.UseAITargeting=yes` —— 只存在于地图 ini, 从 ExtData 才读得到。
 */
    tc->instant = tc->actionNone;
    if (ext && SwsAresExtBool(ext, ARES_EXT_AITGT)) tc->instant = 1;

    /* 火风暴类 (UseChargeDrain) **不特殊化**, 按普通超武处理即可:
     *   - 这几类的 `Action` 本来就是 0 (实测 v=0x00000000), 于是 `actionNone=1`
     *     自动让 `instant=1`, 走 SpecialPlace 立即分支 —— 即 ARES 手册的
     *     "点击激活", 不需要再点地图。
     *   - "激活中"由引擎自己的 ChargeDrain 状态机维护, 侧栏只读不写。 */
    (void)tc->drain;
    /* 自证日志: 把**判据的实际取值**打出来, 限 40 条。
     * `instant` 判错时 (症状: 原版点一下就生效、侧栏要点两次), 光看
     * `nopcx` / `armed` 是分不清"`Action` 偏移/取值读错了"还是"点击那步没走到
     * instant 分支"的 —— 只有把 v=<Action> 打出来才能一眼定位。
     * 只给"有看点"的类型记 (即时类 / 被藏的 / 手动不可点的 / 火风暴类 / 无 PCX),
     * 否则每个剧本几十行噪声。 */
    if (tc->instant || !tc->show || !tc->manual || tc->drain || !tc->pcxFile)
    {
        if (s_DiagLeft > 0)
        {
            char b[16], buf[160];
            unsigned q = AppendCStr(buf, 0, "sws: act ", sizeof(buf));
            q = AppendCStr(buf, q, id, sizeof(buf));
            q = AppendCStr(buf, q, " v=", sizeof(buf));
            HtoA((unsigned)*(int*)((char*)type + OFF_SWT_ACTION), b);
            q = AppendCStr(buf, q, b, sizeof(buf));
            q = AppendCStr(buf, q, " i", sizeof(buf));
            UtoA((unsigned)tc->instant, b);
            q = AppendCStr(buf, q, b, sizeof(buf));
            q = AppendCStr(buf, q, " m", sizeof(buf));
            UtoA((unsigned)tc->manual, b);
            q = AppendCStr(buf, q, b, sizeof(buf));
            q = AppendCStr(buf, q, " d", sizeof(buf));
            UtoA((unsigned)tc->drain, b);
            q = AppendCStr(buf, q, b, sizeof(buf));
            q = AppendCStr(buf, q, " s", sizeof(buf));
            UtoA((unsigned)tc->show, b);
            q = AppendCStr(buf, q, b, sizeof(buf));
            q = AppendCStr(buf, q, " p=", sizeof(buf));
            q = AppendCStr(buf, q, tc->pcxFile ? tc->pcxFile : "-", sizeof(buf));
            /* `x` = ExtData 取到没有。Ares ExtData 是判据的权威来源,
             * x=0 就说明定位器或 find() 出了问题, 得回去查上面那段。 */
            q = AppendCStr(buf, q, ext ? " x1" : " x0", sizeof(buf));
            LogLine(buf);
            s_DiagLeft--;
        }
    }
}

/* 这个超武 (按 SuperWeaponType 数组下标) 到底显不显示在超武侧栏上。
  * 判据 = SwsScan 的显示门: !hide && SW_ShowCameo && ManualFire (本家/IsPresent
  * 由调用方在 SuperClass 数组上自己判)。
  *
  * `SW_ShowCameo` 必须在判据里: Ares 的 `HouseClass_UpdateSuperWeaponsUnavailable`
  * 是 `if(pData->SW_ShowCameo) AddCameo(...)` —— `=no` 的压根不会进防御页。
  * 侧栏不跟着藏, 就是"游戏里没有、侧栏里却有"; 反过来漏掉它, `SwsDedupRestore`
  * 会把引擎本来不给的 cameo 主动加进防御页, 那是**改游戏行为**。
  *
  * "搬回"共用。而"切回显示"时 `s_DedupHide` 恰好是 0, 一旦在这里判掉, 搬回
  * 循环会把所有超武都 `continue` 掉, 按钮就永远没反应 (日志表现为
  * `dedup show n=0`)。要判"侧栏开没开 / 要不要去重"由**调用方**自己判。 */
static int SwsSidebarShows(int swIdx)
{
    void** items;
    void* type;
    const char* id;
    int count = 0;
    SwsTypeCache* tc;
    if (swIdx < 0 || swIdx >= SWS_VEC_HARD_MAX) return 0;
    items = SwsVecItems(ADDR_SWTYPE_ARR, &count);
    if (!items || count <= 0) return 0;
    if (swIdx >= count) return 0;
    type = items[swIdx];
    if (!type || !IsPlausiblePtr(type)) return 0;
    id = (const char*)type + OFF_SWT_ID;
    if (!SwsIdOk(id)) return 0;
    tc = SwsTypeEntry(type, id);
    if (!tc) return 0;
    if (tc->prio < 0) SwsFillType(tc, type, id);
    if (tc->swIdx < 0) return 0;
    /* ★ `drain` (UseChargeDrain) **不再参与过滤** —— 支持之后它只用来决定
 * instant (见 SwsFillType)。火风暴类现在与普通超武同等对待。 */
return (tc->hide || !tc->show || !tc->manual) ? 0 : 1;
}

/* ================= 从引擎手里拿超武 cameo 图标 =================
 *
 * 为什么不能只靠 `SidebarPCX=`: 战役/地图可在**地图内嵌 ini** 里新增或改写
 * `[XxxSpecial]` 节, 而 `CCINIClass::INI_Rules` 只有**基础规则**。实测
 * `SP_CryospearSpecial` / `SP_TPSpecial` 在 rulesmo.ini 里出现 0 次 ⇒ 读出来空
 * (`sws: nopcx` ⇒ 空格子); 改写过的超武读出来是**改之前**的旧值 ⇒ 用错图像。
 *
 * 引擎把合并后的值存在 Ares 的 `SWTypeExt::ExtData::SidebarPCX` (`AresPCXFile`,
 * `operator=` 里还会 `_strlwr_s` 小写化), 而 `SWTypeExt::ExtMap` 内部是 Ares 自己
 * 实现的一套映射 (`ContainerMapBase`, 见 `src/Ext/_Container.hpp`), 定位那个容器
 * 成本过高。⇒ 改走引擎自己那条链: **在 Ares 填好 `CameoPCX` 之后把它读出来。**
 *
 * gamemd 原码 (离线反汇编确认):
 * ```
 * 6A9950  73 0C                 jae 0x6A995E            ; 越界分支
 * 6A9952  8B 90 B8 00 00 00     mov edx,[eax+0xB8]      ; ← Ares 的钩点 (EAX=SWType)
 * 6A9958  89 54 24 48           mov [esp+0x48],edx      ; ← ★ 我们钩这里 (5 字节)
 * 6A995C  EB 08                 jmp 0x6A9966
 * 6A995E  C7 44 24 48 00000000 mov [esp+0x48],0        ; 越界分支
 * 6A9966  A1 4C 3D A8 00        mov eax,[0xA83D4C]     ; 两条路的汇合点
 * ```
 * Ares 的钩在 0x6A9952 替换掉那 6 字节, **把 `CameoPCX` 填好之后**才落到 0x6A9958。
 * 于是 0x6A9958 这个点上: `CameoPCX` 已填好 (还没被 0x6A99A3 的
 * `StripClass_Draw_DrawPCX` blit 掉再清零), 且 **EAX 已被 Syringe trampoline 还原成
 * `SuperWeaponTypeClass*`** —— 我们要的 pSW 和图标都在手上。
 *
 * ★★ **钩 0x6A9958, 绝不碰 0x6A9952**。三条推论:
 *   ① 不要按"站点首字节是 E9"判断 —— **依赖别人补丁的字节编码 = 依赖一个你控制不了
 *      的实现细节**。
 *   ② 不要 `GetProcAddress` 拿钩子体再自己 call —— Ares 的 `GET(pSW, EAX)` 编译成
 *      `mov eax,[esp+4]`, 那个"寄存器快照"是 Syringe trampoline 压在栈上的, 裸 call
 *      过去 `[esp+4]` 是游戏栈垃圾。
 *   ③ 不要自己造寄存器快照 —— Ares 压栈压的是 `[eax+0x24]`(= ID 串地址) 而不是 eax,
 *      与 `Container<TX,TCont>` 的 `key_type = base_type*` 对不上。
 *   ⇒ **改成"在 Ares 之后观察"**: 不读它的补丁、不造快照、不调它的函数体。
 *
 * 壳 (28 字节), 替换 0x6A9958 那 5 字节, 跑完跳 0x6A9966:
 * ```
 * 60                    pushad
 * A3 <slot>             mov [slot],eax          ; pSW = EAX (Syringe 已还原)
 * E8 <rel>              call Sws_CaptureCameo
 * 61                    popad
 * 8B 90 B8 00 00 00     mov edx,[eax+0xB8]      ; ★ 补回被我们盖掉的那条指令
 * 89 54 24 48           mov [esp+0x48],edx      ;   (Ares 的钩跳过了它, 所以要补)
 * E9 <rel>              jmp 0x006A9966          ; 直接落到原来那个 jmp 的落点
 * ```
 * ⚠ pushad/popad 必须成对: 中间那个是真实 call, 会破坏 ebx/esi/edi/ebp, 而编译器
 *   只当 ecx/edx/memory 被破坏 (笔记 §7 同一个坑)。
 * ⚠ `mov [slot],eax` 不能省: 抓取函数是**另一个函数**, 没有寄存器快照可读。漏了
 *   它 ⇒ 抓到的 type = 0。
 * ⚠ 写 gamemd 的 .text **必须** `VirtualProtect(PAGE_EXECUTE_READWRITE)` +
 *   `FlushInstructionCache` —— **这是本项目所有装钩的地方的共同约定**
 *   (`AirWay.cpp:71` / `ChatEdit.cpp:1110` / `FullMap.cpp:421` / `VecText.cpp:2142`)。
 *   只**读** Ares / 只写**堆**的那两处不 protect 也没事 —— 别因为"那两处没炸"就
 *   以为可以省。
 * ⚠ 必须**拷像素**, 不能留 `BSurface*`: 引擎切页签/重载美术会释放重建 (同 §7)。
 * 全程只读游戏状态 (读的都是 UI 绘制链上的对象), 不碰模拟 ⇒ 联机安全。
 */

#define ADDR_CAMEO_OBS_HOOK  0x006A9958u   /* 我们自己的观察点 */
#define ADDR_CAMEO_OBS_RESUME 0x006A9966u /* 原 jmp 的落点 */
#define ADDR_CAMEO_OBS_LEN  0x5u
#define ARES_HOOK_SCAN       0x100u        /* 扫钩子体的字节数 (DrawPCX 的清零在 +0x7D) */
#define ARES_HOOK_DRAW       "StripClass_Draw_DrawPCX"

static void*  s_PswSlot = NULL;     /* 壳写的 SuperWeaponTypeClass* */
static unsigned s_CameoPcx = 0;     /* Ares 的 BSurface* CameoPCX 的地址 */
static void*  s_CapBuf = NULL;      /* 可执行内存 (壳) */
static int    s_HookDone = 0;

/* Ares 映像范围 (定义在下面防御页那一段, 这里先声明) */
static int SwsAresImage(unsigned char** ppLo, unsigned char** ppHi);

/* 逐超武的图标缓存: 按**类型指针**索引 (类型对象终身静态)。
 * 只缓存 60x48 的 16 位色, 抓到一次终身有效。60*48*2 = 5760B/条, 320 条 = 1.8MB。 */
#define SWS_ICON_CAP 512
typedef struct { void* type; unsigned short* pix; } SwsCap;
static SwsCap s_Cap[SWS_ICON_CAP];
static int s_CapN = 0;

static unsigned short* SwsCaptured(void* type)
{
    int i;
    for (i = 0; i < s_CapN; i++) if (s_Cap[i].type == type) return s_Cap[i].pix;
    return NULL;
}

/* 壳里调的抓取函数。**必须**被 pushad/popad 包住 (调用方是机器码壳) —— 真实 call
 * 还会破坏 ebx/esi/edi/ebp, 编译器却以为只有 ecx/edx/memory 被破坏。 */
extern "C" void Sws_CaptureCameo(void)
{
    void* cam, *type;
    unsigned short* dst;
    int slot, px, w, h, bpp;
    const unsigned short* src;
    if (!s_PswSlot || !s_CameoPcx) return;
    cam = *(void**)s_CameoPcx;
    type = *(void**)s_PswSlot;
    if (!cam || !type || !IsPlausiblePtr(cam) || !IsPlausiblePtr(type)) return;
    if (SwsCaptured(type)) return;                  /* 已有, 不重复拷 */
    w = *(int*)((char*)cam + 0x04);
    h = *(int*)((char*)cam + 0x08);
    bpp = *(int*)((char*)cam + 0x10);
    src = *(const unsigned short**)((char*)cam + 0x14);
    if (w != SWS_ICON_W || h != SWS_ICON_H || bpp != 2) return;  /* 只认 60x48 */
    if (!src || !IsPlausiblePtr((void*)src)) return;
    for (slot = 0; slot < s_CapN; slot++) if (s_Cap[slot].type == type) break;
    /* s_CapN 没有上界, 必须自己夹; 表满留一格哨兵。 */
    if (slot < 0 || slot > SWS_ICON_CAP - 1) return;
    if (slot >= s_CapN && s_CapN >= SWS_ICON_CAP - 1) return;
    dst = (unsigned short*)LocalAlloc(LMEM_FIXED,
                     (SIZE_T)(SWS_ICON_W * SWS_ICON_H) * sizeof(unsigned short));
    if (!dst) return;
    for (px = 0; px < SWS_ICON_W * SWS_ICON_H; px++) dst[px] = src[px];
    s_Cap[slot].type = type;
    s_Cap[slot].pix = dst;
    if (slot == s_CapN) s_CapN++;
    if (s_CapN <= 8)
    {
        char buf[64], b1[16];
        unsigned q;
        q = AppendCStr(buf, 0, "sws: cap ", sizeof(buf));
        q = AppendCStr(buf, q, (const char*)type + OFF_SWT_ID, sizeof(buf));
        q = AppendCStr(buf, q, " n=", sizeof(buf));
        UtoA((unsigned)s_CapN, b1);
        AppendCStr(buf, q, b1, sizeof(buf));
        LogLine(buf);
    }
}

/* 从 Ares 的钩子体里认出 `CameoPCX` 的地址。找 Ares .data 段里的绝对地址被
 * 读/写的地方, **取第一个命中**:
 *   `A3 imm32`                    mov [imm32], eax
 *   `89 0D/15/1D/25 imm32`        mov [imm32], ecx/edx/ebx/esp
 *   `C7 05 imm32 00000000`        mov dword [imm32], 0
 *   `83 3D imm32 00`              cmp  dword [imm32], 0   ★ DrawPCX 就是靠这条读的
 * 离线核对 Ares.dll:
 *   SuperWeapon_PCX +0x17 mov [m],eax -> 0x100C20CC
 *   DrawPCX         +0x03 cmp d[m],0   -> 0x100C20CC
 * ⚠ 两条都必须在, 且必须是**同一个**地址 —— 那是"这个地址真的是 CameoPCX"的唯一
 *   证据, 认错就是把别人的内存当图标缓存写。窗口须 ≥ 0x80 (DrawPCX 的清零在 +0x7D)。 */
static unsigned SwsFindCameoPcxIn(void* fn, unsigned char* lo, unsigned char* hi)
{
    unsigned char* p;
    if (!fn || (unsigned char*)fn < lo || (unsigned char*)fn >= hi) return 0;
    for (p = (unsigned char*)fn; p + 10 <= (unsigned char*)fn + ARES_HOOK_SCAN; p++)
    {
        unsigned a;
        if (p[0] == 0xA3)
            a = *(unsigned*)(p + 1);
        else if (p[0] == 0x89 && (p[1] == 0x0D || p[1] == 0x15 ||
                                  p[1] == 0x1D || p[1] == 0x25))
            a = *(unsigned*)(p + 2);
        else if (p[0] == 0xC7 && p[1] == 0x05 &&
                 p[6] == 0 && p[7] == 0 && p[8] == 0 && p[9] == 0)
            a = *(unsigned*)(p + 2);
        else if (p[0] == 0x83 && p[1] == 0x3D && p[6] == 0)
            a = *(unsigned*)(p + 2);
        else
            continue;
        if ((unsigned char*)a >= lo && (unsigned char*)a < hi) return a;
    }
    return 0;
}

/* 装壳。返回 1 = 已装 (含失败后的"不再试")。
 *
 * 这里**只读** Ares 的两个导出符号来定位 `CameoPCX`, **不调用它们、不改它们的
 * 补丁** —— 我们自己的钩在 Ares 之后, 两者互不相干。 */
static int SwsEnsureCameoHook(void)
{
    unsigned char *base, *imgHi;
    FARPROC fPcx, fDraw;
    void* vPcx;
    unsigned aPcx, bPcx;
    unsigned char* st;
    unsigned char* site;
    DWORD old = 0;
    char sb[128], b1[16], b2[16];
    unsigned q;
    if (s_HookDone) return 1;
    if (!IsPlausiblePtr((void*)ADDR_CURPLAYER)) return 0;
    if (*(int*)ADDR_FRAME < 60) return 0;
    if (!SwsAresImage(&base, &imgHi)) { s_HookDone = 1; return 0; }
    fPcx = GetProcAddress((HMODULE)base, ARES_HOOK_PCX);
    fDraw = GetProcAddress((HMODULE)base, ARES_HOOK_DRAW);
    vPcx = (void*)(size_t)fPcx;
    /* ⚠⚠ **绝不能用 `IsPlausiblePtr` 判 Ares 里的地址**。它只放行
     *   `[0x10000, 0x7FF00000)`, 而 Ares.dll 的 PE ImageBase 就是 **0x10000000**
     *   (离线读头确认) —— 没有 ASLR 时整个模块都在上界之外, 每一个地址都会被判成
     *   "不可信"。一律按 `SwsAresImage()` 给的**映像范围**判。 */
    if (!fPcx || !fDraw || !vPcx ||
        (unsigned char*)vPcx < base || (unsigned char*)vPcx >= imgHi)
    {
        q = AppendCStr(sb, 0, "sws: cameo getproc FAIL pcx=", sizeof(sb));
        HtoA((unsigned)(size_t)fPcx, b1);
        q = AppendCStr(sb, q, b1, sizeof(sb));
        q = AppendCStr(sb, q, " draw=", sizeof(sb));
        HtoA((unsigned)(size_t)fDraw, b2);
        q = AppendCStr(sb, q, b2, sizeof(sb));
        q = AppendCStr(sb, q, " base=", sizeof(sb));
        HtoA((unsigned)(size_t)base, b1);
        AppendCStr(sb, q, b1, sizeof(sb));
        LogLine(sb);
        goto fail;
    }
    aPcx = SwsFindCameoPcxIn(vPcx, base, imgHi);
    bPcx = SwsFindCameoPcxIn((void*)(size_t)fDraw, base, imgHi);
    if (!aPcx || !bPcx || bPcx != aPcx)
    {
        q = AppendCStr(sb, 0, "sws: cameo pcx mismatch a=", sizeof(sb));
        HtoA(aPcx, b1);
        q = AppendCStr(sb, q, b1, sizeof(sb));
        q = AppendCStr(sb, q, " b=", sizeof(sb));
        HtoA(bPcx, b2);
        AppendCStr(sb, q, b2, sizeof(sb));
        LogLine(sb);
        goto fail;
    }
    s_CapBuf = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                            PAGE_EXECUTE_READWRITE);
    if (!s_CapBuf) goto fail;
    s_PswSlot = (void*)((char*)s_CapBuf + 32);
    s_CameoPcx = aPcx;
    st = (unsigned char*)s_CapBuf;
    st[0] = 0x60;                                   /* pushad            */
    st[1] = 0xA3;                                   /* mov [slot],eax    */
    *(unsigned*)(st + 2) = (unsigned)(size_t)s_PswSlot;
    st[6] = 0xE8;                                   /* call <抓取>       */
    *(int*)(st + 7) = (int)(long)Sws_CaptureCameo - (int)(long)(st + 11);
    st[11] = 0x61;                                  /* popad             */
    st[12] = 0x8B; st[13] = 0x90; st[14] = 0xB8;
    st[15] = 0x00; st[16] = 0x00; st[17] = 0x00;   /* mov edx,[eax+0xB8] */
    st[18] = 0x89; st[19] = 0x54; st[20] = 0x24; st[21] = 0x48; /* [esp+48],edx */
    st[22] = 0xE9;                                  /* jmp 0x6A9966      */
    *(int*)(st + 23) = (int)(long)ADDR_CAMEO_OBS_RESUME - (int)(long)(st + 27);
    site = (unsigned char*)ADDR_CAMEO_OBS_HOOK;
    /* 盖掉的是 `mov [esp+0x48],edx` (4) + `jmp +8` 的首字节 (1), 共 5 字节;
     * 壳里把那条 mov 补回来, 再直接跳到原来那个 jmp 的落点。 */
    if (!VirtualProtect(site, ADDR_CAMEO_OBS_LEN, PAGE_EXECUTE_READWRITE, &old))
        goto fail;
    site[0] = 0xE9;
    *(int*)(site + 1) = (int)(long)st - (int)(long)(site + 5);
    VirtualProtect(site, ADDR_CAMEO_OBS_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, ADDR_CAMEO_OBS_LEN);
    s_HookDone = 1;
    q = AppendCStr(sb, 0, "sws: cameo hook ON pcx=", sizeof(sb));
    HtoA(aPcx, b1);
    q = AppendCStr(sb, q, b1, sizeof(sb));
    q = AppendCStr(sb, q, " base=", sizeof(sb));
    HtoA((unsigned)(size_t)base, b2);
    AppendCStr(sb, q, b2, sizeof(sb));
    LogLine(sb);
    return 1;
fail:
    LogLine("sws: cameo hook FAIL");
    s_HookDone = 1;
    return 0;
}
/* ---------------- 侧栏清单 = 本家拥有的超武 (Super 对象) ----------------
 *
 * 数据源是 `SuperClass::Array` 里 `Owner==本家 && IsPresent==1` 的那些对象 ——
 * **不是** rules ini, **也不是**防御支援页 (为什么见 SwsScan 里那段注释)。 */

/* ---- ini / 类型数组换代: 把 ini 派生缓存清掉 ----
 * `s_Ini` (配置优先级表) 与 `s_TC` (类型缓存) 是一次性填的, 而
 *   ① 换战役/重开/读档时 `CCINIClass::INI_Rules` 会被换成另一份 (gamemd
 *      `0x52D0FA mov [0x887048],eax`), 键值全变;
 *   ② `SuperWeaponTypeClass::Array` 换容器时**类型对象也会重建**, 于是按
 *      `type` 指针缓存的条目全部张冠李戴 (同地址复用 = 拿到别的超武的图标)。
 * 判据用**指针**而不是内容: 换一份 ini 一定换指针, 而每帧读内容比对不便宜。 */
static void* s_EpochRules = NULL;
static void* s_EpochTypes = NULL;

static void SwsCacheEpoch(void)
{
    void* rules = *(void**)ADDR_INI_RULES;
    void* types = 0;
    int cnt = 0;
    void** it = SwsVecItems(ADDR_SWTYPE_ARR, &cnt);
    if (it) types = it;
    if (rules == s_EpochRules && types == s_EpochTypes) return;
    s_EpochRules = rules;
    s_EpochTypes = types;
    s_IniN = 0;
    SwsLoadIniList();
    s_TCN = 0;
    s_DiagLeft = 40;
    LogLine("sws: ini epoch reset");
}

/* ---- 扫描早退/扫空的旁路日志 ----
 *
 * SwsScan 的每一条 return 都是**静默**的, 而 s_ItemN=0 有两种截然不同的后果:
 *   (a) 侧栏一格不画, **且** 防御页去重也停手 (见 SwsDedupSync) —— 侧栏与防御页
 *       都还是"原样但没有侧栏", 也就是"完全看不到超武侧栏"的那张图;
 *   (b) 只是这一帧没扫出来 (数组换/半初始化垃圾), 下一轮就恢复。
 * 现场分不出这两种, 所以必须留一条**任何情况下都会打**的旁路日志, 否则症状
 * ("有时候有有时候没有")根本无从定位。单个 tag 只记一次, 状态变了会再记。 */
static int s_ScanZeroTag = 0;   /* 上次记下的 tag (字符串比较太贵, 用索引) */
static const char* const kScanTag[] = {
    "cur", "supitems", "houserr", "houseitems",
    "stability", "nohouse", "n0", "ncap"
};
#define SCAN_TAG_N ((int)(sizeof(kScanTag) / sizeof(kScanTag[0])))

static void SwsScanZero(int tag, int v1, int v2)
{
    char buf[96], n1[16], n2[16];
    unsigned q;
    if (tag < 0 || tag >= SCAN_TAG_N) return;
    if (s_ScanZeroTag == tag + 1) return;   /* 同一个原因只记一次 */
    s_ScanZeroTag = tag + 1;
    q = AppendCStr(buf, 0, "sws: scan0 ", sizeof(buf));
    q = AppendCStr(buf, q, kScanTag[tag], sizeof(buf));
    q = AppendCStr(buf, q, " a=", sizeof(buf));
    UtoA((unsigned)v1, n1);
    q = AppendCStr(buf, q, n1, sizeof(buf));
    q = AppendCStr(buf, q, " b=", sizeof(buf));
    UtoA((unsigned)v2, n2);
    q = AppendCStr(buf, q, n2, sizeof(buf));
    AppendCStr(buf, q, "", sizeof(buf));
    LogLine(buf);
}

/* 容器门失败时把整个 DynamicVectorClass 头打出来 (Items/Capacity/IsInit/
 * IsAlloc/Count/CapInc 各是多少)。只在"值变了"时记, 否则每 15 帧刷一行。
 * DynamicVectorClass 布局 (YRpp ArrayClasses.h, 带虚表):
 *   +0x00 vtable / +0x04 Items / +0x08 Capacity / +0x0C IsInit+IsAlloc
 *   +0x10 Count / +0x14 CapacityIncrement  —— sizeof 0x18 */
static void SwsDumpVec(unsigned base, int cnt)
{
    static unsigned s_Last = 0xFFFFFFFFu;
    unsigned h;
    char buf[128], t1[16], t2[16];
    unsigned q;
    (void)cnt;
    h = (unsigned)*(unsigned*)(base + 4u) * 2654435761u
      ^ (unsigned)*(int*)(base + 8u) * 40503u
      ^ (unsigned)*(int*)(base + 16u) * 2246822519u
      ^ (unsigned)*(int*)(base + 20u);
    if (h == s_Last) return;
    s_Last = h;
    q = AppendCStr(buf, 0, "sws: vec ", sizeof(buf));
    HtoA(base, t1);
    q = AppendCStr(buf, q, t1, sizeof(buf));
    q = AppendCStr(buf, q, " items=", sizeof(buf));
    HtoA((unsigned)*(unsigned*)(base + 4u), t1);
    q = AppendCStr(buf, q, t1, sizeof(buf));
    q = AppendCStr(buf, q, " cap=", sizeof(buf));
    UtoA((unsigned)*(int*)(base + 8u), t2);
    q = AppendCStr(buf, q, t2, sizeof(buf));
    q = AppendCStr(buf, q, " ini=", sizeof(buf));
    UtoA((unsigned)*(unsigned char*)(base + 12u), t2);
    q = AppendCStr(buf, q, t2, sizeof(buf));
    q = AppendCStr(buf, q, " cnt=", sizeof(buf));
    UtoA((unsigned)*(int*)(base + 16u), t2);
    q = AppendCStr(buf, q, t2, sizeof(buf));
    q = AppendCStr(buf, q, " inc=", sizeof(buf));
    UtoA((unsigned)*(int*)(base + 20u), t2);
    q = AppendCStr(buf, q, t2, sizeof(buf));
    AppendCStr(buf, q, "", sizeof(buf));
    LogLine(buf);
}

static void SwsScan(void)
{
    void* cur;
    void** items;
    void** hItems;
    int count, hCount, i, n = 0;
    int pside, frame;
    /* 图标抓取钩子: 装上之后引擎每次画防御页超武 cameo 都会把像素交给我们。
     * 每轮试一次 (内部自带"装过就不再试")。 */
    SwsEnsureCameoHook();
    /* ini / 类型数组换代就把所有 ini 派生缓存清掉 (见 SwsCacheEpoch)。 */
    SwsCacheEpoch();
    /* 逐项被拒的计数 (只在扫空时一次性打出来, 见下)。别用它改逻辑 ——
     * 它存在的意义是"扫空时能一眼看出是哪一道显示门吃掉了全部条目"。 */
    int rOwner = 0, rPresent = 0, rType = 0, rFlag = 0, rId = 0,
        rCache = 0, rIdx = 0, rHidden = 0, rShow = 0, rNotClean = 0;
    s_ItemN = 0;
    /* 以下全是固定地址字读取, 不碰数组内容, 任何状态下安全。 */
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) { SwsScanZero(0, 0, 0); return; }
    count = 0;
    items = SwsVecItems(ADDR_SUPER_ARR, &count);
    if (!items) { SwsScanZero(2, count, 0); SwsDumpVec(ADDR_SUPER_ARR, count); return; }
    if (!IsPlausiblePtr((void*)ADDR_HOUSE_ARR)) { SwsScanZero(3, 0, 0); return; }
    hCount = 0;
    hItems = SwsVecItems(ADDR_HOUSE_ARR, &hCount);
    if (!hItems) { SwsScanZero(4, hCount, 0); SwsDumpVec(ADDR_HOUSE_ARR, hCount); return; }
    /* 空数组是合法的"还没有" (刚开局/刚换剧本), 不是错误 —— 交 0 条即可。 */
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
    /* ★ 数据源就是本家拥有的超武 (Super 对象, 见 SwsScan 里那段)。防御支援页
     * 那一份是引擎按 SW_ShowCameo 挑过的滞后镜像, 而且会被我们单方面删条目 ——
     * 拿它当数据源等于自己改自己 ("先复制一份再过滤" 就是现在这个结构)。 */
    /* ★★ 数据源: **引擎的干净清单** (`TabCameos[防御页]`, 长度以
     * `CameoCount` 为准 —— 引擎刚跑完 `CameoIsGone` 过滤)。
     *
     * 为什么改成以它为准: 玩家在原版侧栏看不见不该出现的超武, 说明引擎手上
     * 已经有过滤好的结果。我们自己扫 `SuperClass::Array` 再复刻一遍
     * `SW_ShowCameo` / `ManualFire` / 火风暴, 就是在**猜**引擎已经算好的答案 ——
     * 猜错一次就是"游戏里没有、侧栏里有", 而且时好时坏。
     *
     * 所以改成"读引擎的清单 → 在**副本**上过滤 → 交给侧栏绘制", 我们自己的规则
     * 只做减法, 永远不负责"加进来"。这样侧栏 ⊆ 引擎给的, 不可能凭空多出条目。
     *
     * 拿不到清单 (容器没定位上 / 校验不过) 时退回旧的全数组扫描, 行为不倒退。 */
    {
        SwsCleanStampCheck();
        SwsEngineCleanList();
        /* ⚠ 快照为空时**不开闸门**: 空说明"这个数据源此刻不可信" (容器刚被
         * 去重删空), 而不是"本家没有超武"。硬开会把侧栏杀成一格不画, 而且
         * 引擎侧栏也跟着空掉, 容器再也长不回来 (实测)。 */
        s_CleanMode = (s_CleanN > 0) ? 1 : 0;
        /* 只在**快照条目数变了**才记。快照只增, 所以这里的值单调不减。 */
        {
            static int s_LastClean = -1;
            if (s_CleanN != s_LastClean)
            {
                s_LastClean = s_CleanN;
                LogI("sws: clean snapshot n=", s_CleanN);
            }
        }
    }
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
        unsigned short* capPix = NULL;
        int instant = 0;
        SwsTypeCache* tc;
        if (!sup || !IsPlausiblePtr(sup)) { rType++; continue; }
        type = *(void**)((char*)sup + OFF_SUP_TYPE);
        if (!type || !IsPlausiblePtr(type)) { rType++; continue; }
        owner = *(void**)((char*)sup + OFF_SUP_OWNER);
        if (!type || !IsPlausiblePtr(type)) { rType++; continue; }
        if (!owner || !IsPlausiblePtr(owner) || owner != cur) { rOwner++; continue; }
        present = *(unsigned char*)((char*)sup + OFF_SUP_PRESENT);
        if (present != 1) { rPresent++; continue; }
        ready = *(unsigned char*)((char*)sup + OFF_SUP_READY);
        susp = *(unsigned char*)((char*)sup + OFF_SUP_SUSPEND);
        if (ready > 1 || susp > 1) { rFlag++; continue; }
        id = (const char*)type + OFF_SWT_ID;
        if (!SwsIdOk(id)) { rId++; continue; }
        /* 按类型指针缓存 (ArrayIndex 按 ID 反查类型数组, 不能用猜的字段
         * 偏移)、阵营、ini 优先级/隐藏、SidebarPCX、悬停名。 */
        tc = SwsTypeEntry(type, id);
        if (!tc) { rCache++; continue; }
        if (tc->swIdx < 0) { rIdx++; continue; }
        if (tc->prio < 0) SwsFillType(tc, type, id);
        /* ★★ 引擎清单闸门: 不在引擎那份干净清单里的一律不画。
         * 这一条取代了原先"自己判 SW_ShowCameo"的做法 —— 引擎已经判过了, 我们
         * 再判一遍只会引入分歧 (原版 `SW.ShowCameo=false` 的 IronGuard /
         * StasisGridSpawn / ConfusionGridSpawn 就是这么漏进侧栏的)。 */
        if (s_CleanMode)
        {
            int b = tc->swIdx;
            if (b < 0 || b >= SWS_LIVE_BITS ||
                !(s_CleanBits[b >> 3] & (1u << (b & 7))))
            { rNotClean++; continue; }
        }
        /* 引擎抓到的图标: 每次扫描重新挂一次 (SwsTypeEntry 把 capPix 清成 NULL),
         * 因为 s_Cap[] 会随超武解锁/战役切换而重抓, 不能只认第一次。 */
        if (!tc->capPix) tc->capPix = SwsCaptured(type);
        /* 与防御页一致: `SW.ShowCameo=no` 压根不进防御页 (Ares
         * HouseClass_UpdateSuperWeaponsUnavailable 里 `if(pData->SW_ShowCameo)`),
         * `SW.ManualFire=no` 在防御页上会被拒绝点击, `UseChargeDrain` 那批也过滤
         * (理由见 SwsFillType)。三条都来自**合并后**的规则快照 + 对象字段。
         * 不能拿引擎 ShowTimers 集合当显示清单 —— 它只有 11 个, 是
         * "正在计时"的子集, 不是侧栏该有的全量。 */
        if (tc->hide || !tc->manual) { rHidden++; continue; }
        if (!tc->show) { rShow++; continue; }
        swIdx = tc->swIdx;
        side = tc->side;
        prio = tc->prio;
        pcxFile = tc->pcxFile;
        capPix = tc->capPix;
        instant = tc->instant;
        /* 倒计时: **逐字照抄** `SuperClass::GetCameoChargeStage` (0x6CBF85)。
         * 关键在总量取 `SuperClass+0x24` (实例级), 只有它是 -1 才回退
         * `Type->RechargeTime` —— 之前一律用后者, 于是难度/剧情一旦给这个
         * 实例单独设了时长, 我们与原版侧栏的分母就不同 (症状: 我们 50%,
         * 原版 11%)。 */
        total = *(int*)((char*)sup + OFF_SUP_RTTOTAL);
        if (total == -1 || total <= 0 || total > 1000000)
        {
            total = *(int*)((char*)type + OFF_SWT_RECH);
            if (total < 0 || total > 1000000) total = 0;
        }
        start = *(int*)((char*)sup + OFF_SUP_RTSTART);
        left = *(int*)((char*)sup + OFF_SUP_RTLEFT);
        /* Start == -1 ⇒ 引擎不扣流逝帧, 直接拿 Left */
        if (start == -1) remain = left;
        else remain = left - (frame - start);
        if (remain < 0) remain = 0;          /* 同引擎的 "elapsed >= Left ⇒ 0" */
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
        /* ★ 火风暴类 (UseChargeDrain) 的"运作中"状态 = `ChargeDrainState`。
         *
         *   偏移 +0x7C, 三态 (Ares 源码 `Ext/SWType/Hooks.cpp:386-400` 的
         *   `switch(ChargeDrainState)` 配 `Text.Charging`/`Text_Ready`/
         *   `Text_Active`), 本机 3.0 二进制实证:
         *       1003517F  mov dword [esi+0x7C], 1     ; ← Ready
         *       10035217  mov dword [esi+0x7C], 2     ; ← Draining (激活中)
         *       10035517  mov dword [esi+0x7C], 0     ; ← Charging
         *   且 ClickFire (0x6CB9D9) 是 `state==1 → 写 2`(激活),
         *   (0x6CB954) 是 `state==2 → 写 1`(停用), 与源码
         *   `Ready→Draining` / `Draining→Ready` 完全对上。
         *
         *   只有 `UseChargeDrain` 的类型才有这个状态机, 其余类型该字段无意义,
         *   所以按 `tc->drain` 门控, 避免误显示。 */
        s_Items[n].drainState = tc->drain ? (*(int*)((char*)sup + OFF_SUP_DRAINSTATE) & 0xFF) : 0;
        if (s_Items[n].drainState < 0 || s_Items[n].drainState > 2)
            s_Items[n].drainState = 0;
        /* ★ 运作中百分比的**分母** = 维持时长, 不是满充能时间。
         *
         * 引擎的 `SuperClass::GetCameoChargeStage` 在 `ChargeDrainState==2` 时
         * 返回 `remaining / A`, A 取 `GetRechargeTime()` —— 也就是**满充能时间**
         * (gamemd 0x6CC260: `Super+0x24`, 为 -1 才回退 `Type+0xB0`)。
         * 而激活瞬间 `RechargeTimer.TimeLeft` 被写成
         * `duration = (rechargeTime - left) * ratio` (Ares `ClickFire`), 满充能
         * 激活时 left≈0, 所以 TimeLeft 从**满充能的 ratio 倍**开始往下走。
         *
         * 于是照抄引擎的话, 侧栏一激活就从 `ratio` 开始显示: 爆裂屏障 50%、
         * 心灵之火 25% —— 正是先前实测到的 49%/24%。
         *
         * 原版侧栏之所以是 100% 起跳, 是因为它拿维持时长当分母。所以这里
         * 显式算 `drainTotal = total * ratio`, 而**不是**引擎那个 A。
         * ratio 来自 Ares ExtData `SW.ChargeToDrainRatio` (double @ +0x220),
         * 缺省 1.0 —— 与 `ClickFire` 里 `duration` 的算法逐字对应。
         *
         * 注意 `total` 本身仍按引擎口径取 (实例级优先), 这样"充能中"的
         * 百分比与原版完全一致, 只有运作中换分母。 */
        s_Items[n].drainTotal = 0;
        if (tc->drain && total > 0)
        {
            double dt = (double)total * (tc->ratio > 0.0 ? tc->ratio : 1.0);
            if (dt >= 1.0 && dt <= 1000000.0) s_Items[n].drainTotal = (int)dt;
            if (s_Items[n].drainTotal <= 0)       s_Items[n].drainTotal = total;
        }
        /* 自证: 火风暴类的 ChargeDrainState 变化 (0→2 激活 / 2→1 停用) 各记一行。
         * 顶栏 `Run` 是**唯一**的外部可见反馈, 万一偏移读错 (恒 0 / 恒 2) 只有
         * 这行能立刻分辨。 */
        if (tc->drain)
        {
            static int s_LastDs = -1;
            if (s_LastDs != s_Items[n].drainState)
            {
                char lb[16], line[96];
                unsigned w;
                s_LastDs = s_Items[n].drainState;
                w = AppendCStr(line, 0, "sws: drain ", sizeof(line));
                /* ⚠ 这句的返回值必须接回 w: 早先漏了, w 还停在 "sws: drain " 的
                 * 末尾, 下一句 " state=" 直接把 ID 覆盖掉, 日志里就变成
                 * `sws: drain  state=2` (ID 永远空), 几个火风暴类完全分不开。 */
                w = AppendCStr(line, w, (const char*)type + OFF_SWT_ID, sizeof(line));
                w = AppendCStr(line, w, " state=", sizeof(line));
                UtoA((unsigned)s_Items[n].drainState, lb);
                w = AppendCStr(line, w, lb, sizeof(line));
                w = AppendCStr(line, w, " ready=", sizeof(line));
                UtoA((unsigned)ready, lb);
                w = AppendCStr(line, w, lb, sizeof(line));
                /* 运作中百分比的两个量都打出来: `dt` 是分母 (维持时长),
                 * `left` 是分子。激活瞬间应该看到 left≈dt → 百分比≈100%,
                 * 若 dt 退回等于 `tot` 就是 ChargeToDrainRatio 没读到 (会退回
                 * ratio 起跳, 症状: 爆裂屏障 50% / 心灵之火 25%)。 */
                w = AppendCStr(line, w, " tot=", sizeof(line));
                UtoA((unsigned)total, lb);
                w = AppendCStr(line, w, lb, sizeof(line));
                w = AppendCStr(line, w, " dt=", sizeof(line));
                UtoA((unsigned)s_Items[n].drainTotal, lb);
                w = AppendCStr(line, w, lb, sizeof(line));
                w = AppendCStr(line, w, " left=", sizeof(line));
                UtoA((unsigned)remain, lb);
                AppendCStr(line, w, lb, sizeof(line));
                LogLine(line);
            }
        }
s_Items[n].remain = remain;
    s_Items[n].total = total;
    s_Items[n].instant = instant;
    s_Items[n].capPix = capPix;
    s_Items[n].rech = *(int*)((char*)type + OFF_SWT_RECH);
    n++;
    }
    s_ItemN = n;
    s_OrderN = -1;      /* 条目变了, 排序缓存作废 (见 s_Order 声明处) */
    if (n > 0)
    {
        /* 条目数**变化**就打一行 (不是只在第一次): "侧栏忽然空了/又回来了"
         * 的现场全靠这一行定位, 只记第一次的话看不出是哪一局出的问题。 */
        static int s_LastN = -1;
        s_ScanZeroTag = 0;
        if (n != s_LastN)
        {
            s_LastN = n;
            LogI("sws: scan ok n=", n);
        }
        /* ★ 把**实际画出来的 ID 逐个列出来** (条目集变化时才记)。
         *   "本该过滤掉的仍然没过滤掉" 这类反馈, 光看 `n=` 与逐条的
         *   `sws: act` 判据日志对不上号 —— 后者只在"有看点"的类型上记, 而且
         *   配额有限; 这里直接把成品清单打出来, 与截图逐个核对, 一眼看出
         *   多出来的是谁、该靠哪条规则滤掉。 */
        {
            static char s_LastSet[512];
            static int s_LastSetInit = 0;
            char cur[512];
            unsigned q = 0;
            int k;
            cur[0] = 0;
            for (k = 0; k < n; k++)
            {
                const char* sid = (const char*)s_Items[k].type + OFF_SWT_ID;
                /* 每写一个字节都要重新判容量: 原来只在进循环前查一次
                 * (q+1 < 512), 但一轮最多追加 20+1 字节 ⇒ 侧栏条目一多就把
                 * cur[512] 写溢出几字节, 直接砸掉本函数栈帧 (每次 poll 都发生)。*/
                {
                    unsigned j;
                    for (j = 0; j < 20 && sid[j] && q + 2 < sizeof(cur); j++)
                        cur[q++] = sid[j];
                    if (q + 1 < sizeof(cur)) cur[q++] = ',';
                }
            }
            cur[q] = 0;
            if (!s_LastSetInit || SwsStrDiff(s_LastSet, cur))
            {
                char line[600], t1[16];
                unsigned w = AppendCStr(line, 0, "sws: list ", sizeof(line));
                w = AppendCStr(line, w, cur, sizeof(line));
                /* ★ 带上**当前规则集地址**, 这样日志能对上"是哪张图/哪局"。
                 *   之前日志里最新一局只有 Deploy1/Warhead 两三个超武, 而用户
                 *   截图里有十来个 —— 两边根本不是同一张图, 于是"清单里有没有
                 *   那个不该出现的"根本无从比对。规则集指针每换一张图必变。 */
                w = AppendCStr(line, w, "   [rules=", sizeof(line));
                HtoA((unsigned)(size_t)*(void**)ADDR_INI_RULES, t1);
                w = AppendCStr(line, w, t1, sizeof(line));
                w = AppendCStr(line, w, " cameo=", sizeof(line));
                UtoA((unsigned)SwsStripCameoCount(TAB_DEFENSE), t1);
                w = AppendCStr(line, w, t1, sizeof(line));
                w = AppendCStr(line, w, " snap=", sizeof(line));
                UtoA((unsigned)s_CleanN, t1);
                AppendCStr(line, w, t1, sizeof(line));
                LogLine(line);
                {
                    unsigned j;
                    for (j = 0; j + 1 < sizeof(s_LastSet) && cur[j]; j++)
                        s_LastSet[j] = cur[j];
                    s_LastSet[j] = 0;
                }
                s_LastSetInit = 1;
            }
        }
    }
    /* 一条都没扫出来 = 侧栏整块不画 (SwsRebuild 的 goto nogeo), 而且防御页去重
     * 也一并停手 —— 症状正是"有时候能显示有时候不能"。把每一道显示门吃掉了多少
     * 条一起打出来, 才能一眼看出是哪一道把条目全吃掉了。 */
    if (n <= 0)
    {
        char buf[128], t1[16], t2[16];
        unsigned q = AppendCStr(buf, 0, "sws: scan0 n0 tot=", sizeof(buf));
        UtoA((unsigned)count, t1);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCStr(buf, q, " owner=", sizeof(buf));
        UtoA((unsigned)rOwner, t1);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCStr(buf, q, " present=", sizeof(buf));
        UtoA((unsigned)rPresent, t1);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCStr(buf, q, " type=", sizeof(buf));
        UtoA((unsigned)rType, t1);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCStr(buf, q, " flag=", sizeof(buf));
        UtoA((unsigned)rFlag, t1);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCStr(buf, q, " id=", sizeof(buf));
        UtoA((unsigned)rId, t1);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCStr(buf, q, " cache=", sizeof(buf));
        UtoA((unsigned)rCache, t1);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCStr(buf, q, " idx=", sizeof(buf));
        UtoA((unsigned)rIdx, t1);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCStr(buf, q, " hidden=", sizeof(buf));
        UtoA((unsigned)rHidden, t2);
        q = AppendCStr(buf, q, t2, sizeof(buf));
        q = AppendCStr(buf, q, " nocameo=", sizeof(buf));
        UtoA((unsigned)rShow, t2);
        q = AppendCStr(buf, q, t2, sizeof(buf));
        q = AppendCStr(buf, q, " notseen=", sizeof(buf));
        UtoA((unsigned)rNotClean, t1);
        q = AppendCStr(buf, q, t1, sizeof(buf));
        q = AppendCStr(buf, q, " page=", sizeof(buf));
        UtoA(1u, t2);
        q = AppendCStr(buf, q, t2, sizeof(buf));
        q = AppendCStr(buf, q, " hcnt=", sizeof(buf));
        UtoA((unsigned)hCount, t2);
        AppendCStr(buf, q, t2, sizeof(buf));
        LogLine(buf);
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
#define OFF_SIDE_ACTIVETAB    0x539Cu
#define OFF_SIDE_NEEDREDRAW   0x53A6u
#define OFF_SIDE_NEEDBGREDRAW 0x53A7u
#define ABS_SPECIAL           0x1F   /* 引擎实际用的超武 cameo 类型 */
/* TAB_DEFENSE 已在上方前置声明处定义 */
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

/* =======================================================================
 * 引擎干净清单 —— 自定义侧栏的权威数据源
 *
 * 玩家在原版侧栏看不见不该出现的超武 ⇒ 引擎手上**一定有一份过滤好的清单**。
 * 找到它就不必自己复刻过滤规则 (那是所有 bug 的来源)。
 *
 * 清单 = `RulesExt::TabCameos[防御页]`, 长度以 `StripClass::CameoCount`
 * (页签头 +0x54) 为准。证据:
 *  - Ares 源码 `Ext/Rules/Hooks.CameoList.cpp:308-331` (`StripClass_RecheckCameos
 *    @0x6AA600`, 注释 "complete replacement of StripClass::Recalc"):
 *        auto const kept  = std::remove_if(cameos.begin(), cameos.end(), CameoIsGone);
 *        auto const count = int(kept - cameos.begin());
 *        cameos.Count     = count;      // 容器长度 = 过滤后存活数
 *        pStrip->CameoCount = count;    // 侧栏绘制数 = 同一个值
 *    ⇒ 两者恒等, 且都已过 `CameoIsGone` (`!Super->IsPresent` 已 remove)。
 *  - 本机二进制: `0x6AA617 mov eax,[ebp+0x54]` / `cmp eax,edi(0)`;
 *    绘制循环上界 `0x6A9022 cmp edi,[eax+0x54]`。
 *  ⚠ `+0x58` **不是** CameoCount, 是 `StripClass::Index` (页签号): 被
 *    `mov [0xA8E334]` → `mov eax,[eax+edi*4]` 拿去索引页签数组。
 *
 * 每项 `BuildType` 步长 0x34 (gamemd `0x6A900F`/`0x6A9014`/`0x6A9017` 写入序列
 * 实证, 另有 `cmp eax,0x35` = 53 条上限):
 *   +0x00 int ItemIndex / +0x04 int ItemType (超武=0x1F) / +0x08 / +0x0C
 *
 * ⚠⚠⚠ **本容器会被 `SwsDedupRemove` 缩短**, 所以它**不能**直接当数据源:
 *   "读它 → 过滤 → 删它" 构成自我蚕食 —— 现场日志 `clean list n=0` +
 *   `notseen=25` + `tc=0/0/0/0`, 侧栏一格不画, 而且引擎侧栏也跟着空掉,
 *   容器再也长不回来 (引擎补货也走这个容器)。
 *
 *   ⇒ 做法: **第一次读到时就存一份快照**, 之后一直用那份快照当数据源。
 *     快照只增不减 (条目出现就并进来), 去重随便删容器, 都不影响侧栏。
 *     这才真正做到"拷贝一份, 在副本上过滤" —— 用户要的就是这个结构。
 * ===================================================================== */
/* 把引擎容器当前的内容并进快照 (只增不减)。
 * 返回快照里的条目数; -1 = 容器不可用 (本轮不开闸门)。 */
static int SwsEngineCleanList(void)
{
    unsigned char *vec, *items;
    int cnt, lim, i;
    if (!SwsTabCameoOk()) return -1;
    vec = s_TabCam + (unsigned)TAB_DEFENSE * DVEC_SIZE;
    cnt = *(int*)(vec + DVEC_OFF_COUNT);
    /* CameoCount 是权威长度 (引擎刚跑完 CameoIsGone); 容器自己的 Count 在
     * ClearCameos→AddCameo 的窗口里可能还没同步, 取两者小的。 */
    lim = SwsStripCameoCount(TAB_DEFENSE);
    if (lim >= 0 && lim < cnt) cnt = lim;
    if (cnt <= 0) return 0;
    items = *(unsigned char**)(vec + DVEC_OFF_ITEMS);
    if (!items || !IsPlausiblePtr(items)) return -1;
    for (i = 0; i < cnt; i++)
    {
        const unsigned char* it = items + (size_t)i * BT_SIZE;
        int idx = *(const int*)(it + 0);
        int typ = *(const int*)(it + 4);
        if (typ != ABS_SPECIAL) continue;      /* 不是超武 */
        if (idx < 0 || idx >= SWS_LIVE_BITS) continue;
        /* 并进快照 (只增) —— 这一步才是"拷贝一份" */
        s_CleanBits[idx >> 3] |= (unsigned char)(1u << (idx & 7));
    }
    /* 快照条目数 = 位图 popcount。**必须在这里更新 `s_CleanN`** ——
     *   漏掉它的话 `s_CleanMode = (s_CleanN > 0)` 永远是 0, 闸门从来没打开过。
     *   注意要数**整张快照**而不是本轮读到的 `n`: 快照是累积的, 容器被去重
     *   删短之后 `n` 会变小, 但之前见过的超武仍然有效。 */
    {
        int total = 0, b;
        for (b = 0; b < SWS_LIVE_BYTES; b++)
        {
            unsigned char v = s_CleanBits[b];
            while (v) { total += (v & 1u); v = (unsigned char)(v >> 1); }
        }
        s_CleanN = total;
    }
    return cnt;
}

/* 清空快照。只在"引擎把整套超武重建了"时做 (换剧本), 否则快照会越攒越多,
 * 把上一局出现过的超武带进这一局。判据用类型数组的 Items 指针。
 *
 * ⚠ 首次调用**不能**触发清空: `s_Last` 初始为 NULL 而 `it` 非 NULL, 直接
 *   "变了就清" 会在开局的第二轮把刚攒下的快照清掉。首帧只登记不动作。 */
static void SwsCleanStampCheck(void)
{
    void** it = SwsVecItems(ADDR_SWTYPE_ARR, NULL);
    static void* s_Last = NULL;
    static int s_Init = 0;
    if (!s_Init) { s_Init = 1; s_Last = it; return; }
    if (it == s_Last) return;
    s_Last = it;
    if (s_CleanN > 0)
    {
        memset(s_CleanBits, 0, sizeof(s_CleanBits));
        LogLine("sws: clean snapshot reset (types rebuilt)");
    }
    s_CleanN = -1;
    s_CleanMode = 0;
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
    /* 按映像范围判, **不用** IsPlausiblePtr: Ares 的 ImageBase 是 0x10000000,
     * 落在 IsPlausiblePtr 的上界 (0x7FF00000) 之外。 */
    if (!fn || (unsigned char*)(size_t)fn < base || (unsigned char*)(size_t)fn >= imgHi)
    { s_Failed = 1; return NULL; }
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

/* ---- 搬出 (从防御页删除) 的判据: 必须是 SwsSidebarShows 的**真子集** ----
 *
 * 铁律: **删之前必须能证明侧栏此刻真的在画这个超武**。删错了的后果与其它方向
 * 相反 —— "侧栏不画 + 防御页被删" = 两头都没了, 而搬出是**单向**的, 不会自愈
 * (只有玩家手点"隐藏重复"按钮变回显示态才会补回来)。所以宁可少删。
 *
 * 只拿类型门 (SwsSidebarShows) 判会在三种情况下删出"丢失":
 *   (1) 扫描早退 —— SwsScan 开头就把 s_ItemN 清 0, 而"数组换了 (开局/重开/
 *       读档)"那一轮、本家还没进 House 数组、半初始化垃圾这些都直接 return。
 *       那一帧侧栏一格都不画, 按类型门删却照做 → 整个防御页的超武被一次清空。
 *   (2) 实例缺失 —— 引擎把 cameo 放进防御页 (Ares
 *       HouseClass_UpdateSuperWeaponsUnavailable / ICBM 补给箱 / Grant1ShotSW)
 *       与本家 Super 变成 IsPresent==1 之间存在时间差; 这段时间类型门过、
 *       SwsScan 不收。**搬回**那条路本来就有实例门 (见 SwsDedupRestore),
 *       搬出没有 = 两个方向不对称, 差的就是这个洞。
 *   (3) 侧栏收起 (F4) —— 侧栏什么都不画, 目标态却仍是"隐藏重复" (见
 *       SwsDedupSync), 于是 F4 一下整页超武消失。
 * 三道门一起补上: 类型门 ∧ 实例门 ∧ 快照门 (最近一次扫描的条目表里真的有它)。
 * 三道门一起补上: 类型门 ∧ 实例门 ∧ 快照门 (最近一次扫描的条目表里真的有它)。
 */


/* 本家"活的"超武 (IsPresent==1) 下标位图。**一趟建好, 供搬出逐条 O(1) 查** ——
 * 早期版本写成 SwsHasLiveSuper(swIdx) 每条 cameo 走一遍全表, 而
 * `SuperClass::Array` 在满编剧本里有 4480 条、防御页 cameo 也有几十条 ⇒
 * 单次去重复查就是十几万次循环。位图按 swIdx 取位, 大小固定。
 *
 * 判据与 SwsDedupRestore 逐字相同 —— 两个方向必须对称, 否则就在"camo 在页里 /
 * Super 不在"的窗口里删出丢失。返回建的个数, -1 = 容器不可用。 */
static int SwsBuildLiveSet(unsigned char* live)
{
    void** items;
    int count = 0, i, n = 0;
    void* cur = *(void**)ADDR_CURPLAYER;
    for (i = 0; i < SWS_LIVE_BYTES; i++) live[i] = 0;
    if (!cur || !IsPlausiblePtr(cur)) return -1;
    items = SwsVecItems(ADDR_SUPER_ARR, &count);
    if (!items) return -1;
    for (i = 0; i < count; i++)
    {
        void* sup = items[i];
        void *type, *owner;
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
        if (swIdx < 0 || swIdx >= SWS_LIVE_BITS) continue;
        if (!(live[swIdx >> 3] & (unsigned char)(1u << (swIdx & 7))))
        {
            live[swIdx >> 3] |= (unsigned char)(1u << (swIdx & 7));
            n++;
        }
    }
    return n;
}

static int SwsLiveHas(const unsigned char* live, int swIdx)
{
    if (swIdx < 0 || swIdx >= SWS_LIVE_BITS) return 0;
    return (live[swIdx >> 3] & (unsigned char)(1u << (swIdx & 7))) ? 1 : 0;
}

/* 最近一次 SwsScan 的条目表里有没有这个超武。s_Items 存的是**类型**下标
 * (tc->swIdx), 与防御页 cameo 的 ItemIndex 同源 (HouseClass::Supers 按类型序
 * 建, Supers[i]->Type->ArrayIndex == i), 所以直接比下标即可。 */
static int SwsSidebarShowing(int swIdx)
{
    int k;
    if (s_ItemN <= 0) return 0;         /* 扫描早退: 侧栏此刻一格都没画 */
    if (s_ItemN > SWS_CAP) return 0;
    for (k = 0; k < s_ItemN; k++)
        if (s_Items[k].swIdx == swIdx) return 1;
    return 0;
}

/* 侧栏**此刻画得出图标**吗? 两条路都空的条目还画不出图 (空格子), 而搬出是
 * 单向的 —— 搬走之后引擎就不再画它, 抓取钩子也就永远抓不到它 ⇒ 图标永远补不上,
 * 变成"两边都没有"。所以图标还没着落的条目**先留在防御页上**, 等抓到了再搬。
 * (重复显示几秒比永久空格子好; 抓到之后下一轮复查自动搬走, 无感。) */
static int SwsSidebarHasIcon(int swIdx)
{
    int k;
    if (s_ItemN <= 0 || s_ItemN > SWS_CAP) return 0;
    for (k = 0; k < s_ItemN; k++)
        if (s_Items[k].swIdx == swIdx)
            return (s_Items[k].capPix || s_Items[k].pcxFile) ? 1 : 0;
    return 0;
}

/* 从防御页删掉所有"侧栏上确实也有"的超武 cameo。返回删掉几条, -1 = 不可用。
 *
 * ⚠⚠ 与 `SwsEngineCleanList` 的**致命顺序依赖**:
 *   引擎的干净清单就是**这个容器本身**, 而本函数会把它缩短。于是
 *     "读清单 → 过滤 → 绘制 → 删容器" 这个闭环一旦成立, 下一轮读到的清单
 *     就只剩"上次没删掉的" —— 逐轮自我蚕食, 最后侧栏变空。
 *
 *   两个方向都踩过:
 *     - 只删不读: 侧栏照旧全量, 但 `ShowCameo=false` 的又会漏进来 (老 bug);
 *     - 只读不删: 过滤对了, 但同一个超武在引擎侧栏和自定义侧栏**同时**出现。
 *
 *   ⇒ 正确做法: **读一份快照当数据源, 再对容器做去重**。快照必须在删除**之前**
 *     取, 且删除只针对"本轮确实画出来了"的条目 —— 画出来的条件里已经包含
 *     "在快照里", 所以删除是快照的子集, 不会把还没画的条目误删。
 *
 *   `SwsScan` 每轮都重建快照 (见 s_CleanBits), `SwsDedupSync` 在其后跑,
 *   于是每轮都是"先按引擎的完整清单决定画什么, 再把画了的搬走"。
 *   引擎下一帧会通过 `HouseClass_UpdateSuperWeaponsUnavailable` / AddCameo
 *   把该给的补回来 —— 这本来就是它每帧在做的事, 我们只是搭个便车。 */
static int SwsDedupRemove(void)
{
    unsigned char* vec, *items;
    unsigned char live[SWS_LIVE_BYTES];
    int cnt, i, removed = 0;
    if (!SwsTabCameoOk()) return -1;
    /* 实例门 (本家 IsPresent==1 的超武下标位图)。建不起来 (-1) 就整条停手:
     * 没有它就无法证明"侧栏在画", 而多删一条就是永久丢失。 */
    if (SwsBuildLiveSet(live) < 0) return -1;
    vec = s_TabCam + (unsigned)TAB_DEFENSE * DVEC_SIZE;
    cnt = *(int*)(vec + DVEC_OFF_COUNT);
    /* CameoCount 是权威上限 (它会剔除已消失的 Cameo), 而增删路径没有反向同步,
     * 所以只信它 —— 下面要按 cnt 原地搬 0x34 字节的条目, cnt 偏大就越界写。*/
    {
        int lim = SwsStripCameoCount(TAB_DEFENSE);
        if (lim >= 0 && lim < cnt) cnt = lim;
    }
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
        if (!SwsLiveHas(live, idx)) continue;
        if (!SwsSidebarShowing(idx)) continue;
        /* 图标还没着落的**先别搬**: 搬走之后引擎不再画它, 抓取钩子就永远抓不到,
         * 两边都没图标 (见 SwsSidebarHasIcon 那段)。 */
        if (!SwsSidebarHasIcon(idx)) continue;
        /* 倒着删: 后面先缩, 前面那条的下标才不会因为搬移而失效。
         * tail = 尾条目到当前条目的距离 = 这次要前移的条数。 */
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
    count = 0;
    items = SwsVecItems(ADDR_SUPER_ARR, &count);
    if (!items || count <= 0) return -1;
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
        if (swIdx < 0 || swIdx >= SWS_LIVE_BITS) continue;
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
    /* 目标态 = "去重开关 ∧ 侧栏此刻真的在显示"。
     *
     * `s_Visible` 必须参与: 收起 (F4) 之后侧栏一格都不画, 若目标态还停在"隐藏
     * 重复", 那些刚被搬走的超武就**两头都没了** —— 收起期间玩家在防御页里再也
     * 找不到它们, 而搬出是单向的, 展开时也不会补回防御页。原先注释写的理由
     * ("防御页是引擎自己的页签, 侧栏藏了照样能用")只在**还没搬走过**的时候成立,
     * 一旦搬走过就是反的。 */
    want = (s_DedupHide && s_Visible) ? 1 : 0;
    if (want == s_DedupDone && !s_DedupDirty) return 0;
    if (want && !SwsAresFindTabCameos()) { s_DedupDirty = 0; return 0; }
    /* 想删, 但侧栏还没扫出任何条目 (开局/重开那一轮 SwsScan 早退, 本家还没进
     * House 数组, 半初始化垃圾): **这一轮什么都不做**, dirty 留着等下一次扫描。
     * 删了就是永久丢失, 晚 0.25s 无所谓。 */
    if (want && s_ItemN <= 0)
    {
        static int s_WaitAge = 0;
        if (s_WaitAge <= 0)
        {
            LogLine("sws: dd wait scan");
            s_WaitAge = 600;      /* 600 帧 (~10s) 再记一次 */
        }
        s_WaitAge--;
        s_DedupDirty = 1;
        return 0;
    }
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

/* ---------------- 图标兜底: 让引擎自己把防御页重画一次 ----------------
 *
 * 抓取钩 (`0x6A9958`) 只在**引擎正在画防御页 cameo** 时才有数据, 而:
 *   ① 钩子要 `frame >= 60` 才装得上 (见 SwsEnsureCameoHook), 装上之前侧栏
 *      早就画过一轮了;
 *   ② 防御页默认不是激活页签;
 *   ③ 地图内嵌 ini 新增的超武 (SP_CryospearSpecial / WarheadSpecial1 …) 压根
 *      不在 rulesmo.ini 里 ⇒ `SidebarPCX=` 读不到 ⇒ 自己载 PCX 这条路也空。
 * ⇒ 结果是"格子空着"。用户点一次"过滤/显示"按钮之所以能救回来, 就是按钮那条路
 *   顺手调了一次 `RepaintSidebar(1)`, 引擎于是把防御页重画, 抓取钩抓到像素。
 *   这里把那次重画自己做掉, 每轮扫描最多催一次, 催满 32 次停手。
 *
 * 纯 UI 重绘请求 (`SwsDedupRestore` / `SwsDefenseRepair` 早就在调它), 不碰模拟。
 * 副作用只有一次多余的侧栏重画, 而侧栏本来每帧都在重画。
 *
 * ⚠⚠ 实测修正: **光调 `RepaintSidebar(1)` 抓不到**。用户在 emigdal
 *   地图上手动点"过滤/显示"按钮能一次救回两个图标, 而按钮那条路除了
 *   `RepaintSidebar` 之外**还走了 `AddCameo`**; 只抄 `RepaintSidebar` 的
 *   `SwsNudgePaint` 连催 32 次一个也没抓到 (日志: `SwsNudgePaint` 跑满, 无
 *   `sws: cap`)。差别就在 `AddCameo` —— 它会把 super 挂到防御页的 cameo 槽上,
 *   而抓取钩 (`0x6A9958`) 只在**槽被画**时才有机会拷像素。所以这里改成
 *   "AddCameo(所有本家超武) + RepaintSidebar", 即复用 `SwsDedupRestore` 那条
 *   已被用户操作实证有效的路径。
 *
 * 为什么这样安全 (联机铁律):
 *   - `AddCameo` 是 UI 层操作, 不写任何模拟状态, 引擎自己就在用它建防御页;
 *   - 它自带查重 (Ares 的 `SidebarClass_AddCameo_SkipSizeCheck` 扫一遍
 *     `TabCameos[TabIndex]`), 重复调用是无害空操作;
 *   - 紧接着的 `SwsDedupSync()` 仍是**单向搬出** (只从防御页删, 绝不加),
 *     所以即便这里多加了不该有的 cameo, 下一轮也会被搬走。 */
#define SWS_NUDGE_MAX 8
static int s_NudgeLeft = SWS_NUDGE_MAX;

static void SwsNudgePaint(void)
{
    int i, added = 0;
    if (s_ItemN <= 0 || s_NudgeLeft <= 0) return;
    for (i = 0; i < s_ItemN; i++)
        if (!s_Items[i].capPix && !s_Items[i].pcxFile) break;
    if (i >= s_ItemN) return;          /* 图标都齐了, 不用再催 */
    s_NudgeLeft--;
    /* 与 SwsDedupRestore 逐字同一条路: 引擎自己的 AddCameo (自带查重),
     * 只对本家、已就绪、且侧栏本来就显示的超武调用。 */
    {
        void* cur = *(void**)ADDR_CURPLAYER;
        void** items;
        int count;
        if (!cur || !IsPlausiblePtr(cur)) return;
        count = 0;
        items = SwsVecItems(ADDR_SUPER_ARR, &count);
        if (!items || count <= 0) return;
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
            if (swIdx < 0 || swIdx >= SWS_LIVE_BITS) continue;
            if (!SwsSidebarShows(swIdx)) continue;
            if (((int (__thiscall *)(void*, int, int))(void*)ADDR_ADDCAMEO)
                    ((void*)ADDR_SIDEBAR, ABS_SPECIAL, swIdx))
                added++;
        }
    }
    ((void (__thiscall *)(void*, int))(void*)ADDR_REPAINTSIDEBAR)
        ((void*)ADDR_SIDEBAR, TAB_DEFENSE);
    if (s_DiagLeft > 0 && s_NudgeLeft == 0)
    {
        char b[16], buf[64];
        unsigned q = AppendCStr(buf, 0, "sws: nudge done added=", sizeof(buf));
        UtoA((unsigned)added, b);
        AppendCStr(buf, q, b, sizeof(buf));
        LogLine(buf);
    }
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
    if (mx < s_ColMinX || mx > s_ColMaxX + SWS_ICON_W + 5) return 0;
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
    if (mx < s_ColMinX || mx > s_ColMaxX + SWS_ICON_W + 5) return 0;
    if (my < s_YStart || my >= s_YStart + (s_VisRows * s_RowH - s_Gap)) return 0;
    return 1;
}

void SWS_Poll(void)
{
    static int s_LastItemN = -1;
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
            /* 换剧本: 条目集整个变了 ⇒ 图标催画的配额重新给满 */
            if (s_ItemN != s_LastItemN) { s_LastItemN = s_ItemN; s_NudgeLeft = SWS_NUDGE_MAX; }
            SwsScan();
            /* 图标还缺就让引擎重画一次防御页 (抓取钩只在这时候拿得到像素)。
             * 必须在 SwsDedupSync **之前**: 去重会把没有图标的条目"留在防御页上"
             * 等图标 (SwsSidebarHasIcon), 催画要赶在它被搬走之前。 */
            SwsNudgePaint();
            /* 顺便把防御页去重**标记**成"要复查": 新冒进防御页的超武 cameo 靠
             * 这一遍搬走。只在"隐藏"态复查; "显示"态是按钮点一下才需要补, 而
             * 补的判据 (AddCameo 自带查重) 不会因为时间推移而失效。 */
            if (s_DedupHide) s_DedupDirty = 1;
        }
        else if (s_CleanMode)
        {
            /* ★ 两次扫描之间容器会被去重缩短, 所以**只要它被改过就立刻重建
             * 快照**, 否则下一轮会拿一份"已经被自己删过"的清单当数据源 ——
             * 逐轮自我蚕食。判据便宜 (读两个 int 比一下), 每帧做无所谓。 */
            static int s_LastSeenCnt = -1, s_LastSeenCam = -1;
            int nowCnt = *(int*)(s_TabCam + (unsigned)TAB_DEFENSE * DVEC_SIZE +
                                 DVEC_OFF_COUNT);
            int nowCam = SwsStripCameoCount(TAB_DEFENSE);
            if (nowCnt != s_LastSeenCnt || nowCam != s_LastSeenCam)
            {
                s_LastSeenCnt = nowCnt;
                s_LastSeenCam = nowCam;
                s_ForceScan = 1;
            }
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
static void SwsCPSrc(int x, int y, int dw, int dh,
                    const unsigned short* src, int pw, int ph, int opaque)
{
    int r, xx, x0, x1, y0, a8;
    unsigned short sxmap[SWS_ICON_W];
    if (!s_CchA || dw <= 0 || dh <= 0 || dw > SWS_ICON_W) return;
    if (!src || pw < 1 || pw > 256 || ph < 1 || ph > 256) return;
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
}

static void SwsCPcx(int x, int y, int dw, int dh, void* psurf, int opaque)
{
    int pw, ph, bpp;
    const unsigned short* src;
    static int s_BadLogged = 0;
    if (!psurf || !IsPlausiblePtr(psurf)) return;
    if (!IsPlausiblePtr(*(void**)psurf)) return;   /* 虚表必须指向代码段 */
    pw = *(int*)((char*)psurf + 0x04);
    ph = *(int*)((char*)psurf + 0x08);
    bpp = *(int*)((char*)psurf + 0x10);
    src = *(const unsigned short**)((char*)psurf + 0x14);
    /* 占位面通常尺寸离谱 (1x1 / 0), 这几道校验就是挡它的 */
    if (pw < 1 || pw > 256 || ph < 1 || ph > 256 || bpp != 2) goto bad;
    if (!src || !IsPlausiblePtr((void*)src)) goto bad;
    if (*(int*)((char*)psurf + 0x18) != pw * ph * 2) goto bad;
    SwsCPSrc(x, y, dw, dh, src, pw, ph, opaque);
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
    /* 两个按钮在栏区判定框**之上**, 判定顺序必须排在栏区判定之前。跟着面板换边
     * 一起镜像: 面板在右 → 按钮贴块的**右**缘 (靠屏幕外缘); 在左 → 贴左缘。 */
    if (s_SideRight)
    {
        s_BtnX = right - s_BtnW;
        s_HidX = s_BtnX - SWS_BTN_GAP - s_BtnW;
    }
    else
    {
        s_BtnX = xStart;
        s_HidX = s_BtnX + s_BtnW + SWS_BTN_GAP;
    }
    if (s_BtnX + s_BtnW > w) s_BtnX = w - s_BtnW;   /* 顶出屏幕时拉回 */
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
/* 收起态: 第二参是"外缘那一侧的可用宽度起点" —— 展开态那边传的是锚点
     * (右缘=g_MapRight / 左缘=屏幕左缘 0), 这里传画布缘即可, 按钮贴缘画。 */
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
    /* 收起态只有箭头, 贴**外缘**: 面板在右就贴右边, 在左就贴左边。
     * (展开态那个位置是展开帧算好后存进 s_BtnXKeep 的, 折叠不许跳位。) */
    s_BtnX = s_SideRight ? (w - s_RightMargin - s_BtnW) : (s_RightMargin);
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
    /* 锚点 (地图区右缘) 也要进签名: F11 切侧栏显示/隐藏时地图右缘在 1752/1920
     * 之间跳, 而画布尺寸不变 —— 不进签名的话缓存图不会被重建, 侧栏就会以旧坐标
     * 留在地图上 (表现为"切换后地图上残留一块图标")。*/
    s = s * 33u + (unsigned)g_MapRight;
    /* 面板在左时锚点是屏幕左缘(恒 0), 列序/滚动条/命中框全都镜像 ⇒ 把
     * s_SideRight 放进签名, 否则切 Side 之后缓存不失效, 画面还是旧的。 */
    s = s * 33u + (unsigned)s_SideRight;
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
    /* ★ drainState 必须进指纹: 火风暴类激活/停用只改这一个字段, 不加的话
     * 状态切换不会触发重绘, 顶栏的 `Run` 要等下一次别的原因重绘才出现。 */
    s = s * 31u + (unsigned)it->drainState;
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
    /* 收起态的箭头贴**外缘**: 面板在右 → 地图右缘; 在左 → 地图左缘(=0 起步)。
     * SwsCollapsedBtnGeom 内部只拿它当"贴哪一侧"的提示, 真正贴边由 s_SideRight 决定。 */
    SwsCollapsedBtnGeom(s_SideRight
                            ? ((g_MapRight > 0 && g_MapRight <= w) ? g_MapRight : w)
                            : 0,
                        by);
    if (!SwsCacheAlloc(s_BtnW, s_BtnH)) { s_CchOK = 0; s_BoundsKnown = 0; return; }
    s_CchX = s_BtnX; s_CchY = s_BtnY;
    SwsDrawArrow(s_BtnX, s_BtnY, s_BtnW, s_BtnH, 0, btnHov == 1);
    s_CchOK = 1;
    s_BoundsKnown = 0;      /* 收起时没有可点的超武块 */
    s_CchSig = sig; s_CchFrame = fr;
    s_CchHover = -1; s_CchBtn = btnHov; s_CchSel = -9999;
}

/* ---------------- 原版侧栏的排序规则 ----------------
 *
 * 引擎那边不是"按添加顺序排", 而是**排序插入**: gamemd `SidebarClass::AddCameo`
 * 走 `StripClass_AddCameo`, 比较器是 `BuildType::operator<` → `SortsBefore`
 * (gamemd `0x6A8420`, YRpp `SidebarClass.h` 里 `JMP_STD(0x6A8420)`)。
 *
 * 逐条反汇编那个比较器, 对两个特殊类条目 (超武 `ItemType==0x1F` 全部命中这一支):
 *     006A84AB  mov eax,[ecx+edi*4]     ; Type[leftIndex]
 *     006A84B1  mov edx,[eax+0xB0]      ; left .RechargeTime
 *     006A84B7  mov esi,[ecx+0xB0]      ; right.RechargeTime
 *     006A84C1  → return 1             ; left < right ⇒ 左在前
 *     006A84CD  → return 0             ; left > right
 *     006A84DB  mov eax,[eax+0x60]      ; 相等 ⇒ 比 UIName
 *     006A84E1  jmp 0x6A86F3            ; strcmp(left,right) 后 `setle`
 * ⇒ **原版顺序 = RechargeTime 升序, 相等按 UIName 字典序升序** (冷却快的排前面)。
 *
 * 注意 `isSpecial` 那道门判的是 `ItemType ∈ {0x1F, 0x20, 0x39}`; 超武之间两边
 * 都是 0x1F, 所以恒走上面这条, 不涉及建筑/单位的那些分支。
 *
 * 我们用它做**默认顺序**; ini 里给了自定义优先级的那些仍按自定义排前面。 */
static int SwsWcsAfter(const wchar_t* a, const wchar_t* b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a > *b;      /* 字典序: a 在后 */
}

static int SwsOrigAfter(const SwsItem* a, const SwsItem* b)
{
    if (a->rech != b->rech) return a->rech > b->rech;
    return SwsWcsAfter(a->name, b->name);
}

/* ---- 重建期"整块不画"的旁路日志 ----
 * SwsRebuild 的三条 goto nogeo 与 SwsScan 的早退一样静默, 而症状完全一样
 * (屏上就是没有侧栏)。原因变了才再记一行, 别每帧刷。 */
static const char* s_DrawZeroTag = NULL;
static void SwsDrawZero(const char* tag, int v1, int v2)
{
    char buf[96], t1[16], t2[16];
    unsigned q;
    if (s_DrawZeroTag == tag) return;
    s_DrawZeroTag = tag;
    q = AppendCStr(buf, 0, "sws: draw0 ", sizeof(buf));
    q = AppendCStr(buf, q, tag, sizeof(buf));
    q = AppendCStr(buf, q, " a=", sizeof(buf));
    UtoA((unsigned)v1, t1);
    q = AppendCStr(buf, q, t1, sizeof(buf));
    q = AppendCStr(buf, q, " b=", sizeof(buf));
    UtoA((unsigned)v2, t2);
    q = AppendCStr(buf, q, t2, sizeof(buf));
    AppendCStr(buf, q, "", sizeof(buf));
    LogLine(buf);
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
    int bar, barX, barW, firstX, right, bottom, top, ch, hover = -1, txtOk, anchor;
    int tipX = 0, tipY = 0, tipW = 0, tipH = 0;
    /* 三条 goto nogeo 都是"侧栏整块不画"的静默出口, 各自记一行 (只记一次,
     * 原因变了会再记), 否则"有时候不显示"在日志里一片干净。 */
    if (n <= 0 || n > SWS_CAP) { SwsDrawZero("nitems", n, s_ItemN); goto nogeo; }

    /* 1) 排序: 阵营 -> ini 自定义优先级 -> **原版顺序**。
     *    自定义优先级默认 1000 (= 没写), 自定义项填正数 (<1000) ⇒ 自然排到
     *    前面, 不用额外分组; 没自定义的那些按 SwsOrigAfter (原版规则) 排。
     *    s_Items 只有 SwsScan 会改, 所以顺序与帧无关, 只在条目数变了才真排一次。 */
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
                else gt = SwsOrigAfter(a, b);
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
    /* 只有**一个阵营**有超武时, 默认拆成两列显示 —— 一列 60x48 排到底, 右边空一大
     * 片, 纵向也只用了一半。拆法: 两个物理列共用该阵营的边框色, 条目按**行奇偶**
     * 分列 (0,1,0,1,...), 于是每列 ceil(n/2) 条、可见行数也跟着减半。
     * 只有 1 条时不拆 (第二列会是空框)。 */
    s_SplitSide = -1;
    {
        int nonEmpty = 0, only = -1;
        for (f = 0; f < 4; f++) if (colCount[f] > 0) { nonEmpty++; only = f; }
        if (nonEmpty == 1 && colCount[only] >= 2)
        {
            int cnt = colCount[only];
            int half = (cnt + 1) / 2;
            s_SplitSide = only;
            colCount[only] = 0;
            colCount[0] = half;          /* 偶数序号 (0,2,4..) */
            colCount[1] = cnt - half;    /* 奇数序号; cnt 为奇数时少一条 */
        }
    }
    maxRows = 0;
    for (f = 0; f < 4; f++) if (colCount[f] > maxRows) maxRows = colCount[f];
    if (maxRows <= 0) { SwsDrawZero("norows", maxRows, n); goto nogeo; }
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
    /* 有列装不下时, 整块**外侧**让出 5px 给那根滚动条 (否则会顶出屏幕)。
     * 外侧 = 靠屏幕边缘那一侧, 随 s_SideRight 变。 */
    bar = (s_MaxSk > 0);
    barW = bar ? 4 : 0;
    /* 锚到地图区边缘 (态0: 左缘 0 / 右缘 1752; 态1,2 铺满时锚屏幕缘), 不是画布
     * 边缘 —— 原版侧栏恒在右, 锚画布缘会把整块塞到它底下。FullMap 没接管
     * (g_Map*=0) 时才退回画布缘。*/
    if (s_SideRight)
    {
        anchor = (g_MapRight > 0 && g_MapRight <= w) ? g_MapRight : w;
        xStart = anchor - s_RightMargin - totW - (bar ? 5 : 0);
        if (xStart < 0) xStart = 0;
    }
    /* 面板在左时锚**屏幕左缘 x=0**, 不需要问 FullMap 要"地图左缘":
 * 原版侧栏恒在右 ⇒ 正常态0 视口左缘就是屏幕左缘 (态1/2 FullMap 把视口往右
 * 推开 sbW, 留给左面板的正好是那条空档)。不需要为此改 FullMap。 */
    else
    {
        anchor = 0;
        xStart = anchor + s_RightMargin + (bar ? 5 : 0);
        if (xStart + totW > w) xStart = w - totW;
        if (xStart < 0) xStart = 0;
    }
    /* 比例 = 屏幕高 * 比例, 再减去半个块高 -> 视觉上居中 (块不会偏上);
     * 像素 = 直接当 Y 坐标。纵向要多留 SWS_BTN_H+2 给块顶上方的两个按钮。 */
    if (s_TopIsPct)
        yStart = (int)(((long)h * (long)s_TopMargin) / 100000L) - blockH / 2;
    else
        yStart = s_TopMargin;
    if (yStart < SWS_BTN_H + 2) yStart = SWS_BTN_H + 2;
    /* 2) 各列的 x。空列跳过, 所以列是连续排下去的。
     *    列序 = `kColOrder`: **盟军永远靠屏幕外缘, 焚风永远靠地图内侧**,
     *    所以面板在右时列序是反的 (焚风→盟军), 在左时是自然序 (盟军→焚风)。*/
    firstX = xStart;
    {
        int cx = xStart, first = 1;
        const int* ord = kColOrder;
        for (f = 0; f < 4; f++)
        {
            int sd = ord[f];
            if (colCount[sd] <= 0) continue;
            if (first) { firstX = cx; first = 0; }
            colX[sd] = cx;
            cx += SWS_ICON_W + s_ColGap;
        }
    }
    /* 命中判定用的横向范围 = 最左列 ~ 最右列 (列序可能是反的, 不能取循环末值) */
    s_ColMinX = 0x7FFFFFF;
    s_ColMaxX = -1;
    for (f = 0; f < 4; f++)
        if (colCount[f] > 0)
        {
            if (colX[f] < s_ColMinX) s_ColMinX = colX[f];
            if (colX[f] > s_ColMaxX) s_ColMaxX = colX[f];
        }
    if (s_ColMaxX < 0) { s_ColMinX = 0; s_ColMaxX = -1; }
    /* 3) 每条目在屏幕上的位置; 滚出可见范围的 x=-1 (不参与命中/绘制)。
     *    拆列时 (`s_SplitSide>=0`) 条目按**行奇偶**分进物理列 0/1 —— 注意用的
     *    是"该阵营内的序号"而不是 `it->side`, 因为此时 colCount 已经被改写成
     *    两个物理列, `it->side` 不再是列号。 */
    rows[0] = rows[1] = rows[2] = rows[3] = 0;
    {
        int k = 0;
        for (i = 0; i < n; i++)
        {
            SwsItem* it = &s_Items[s_Order[i]];
            int c = (s_SplitSide >= 0) ? (k & 1) : it->side;
            int r = rows[c] - s_Scroll;
            it->x = colX[c];
            it->y = yStart + r * rowH;
            rows[c]++;
            if (r < 0 || r >= maxRows) it->x = -1;
            k++;
        }
    }
    /* 4) 两个按钮 (在栏区判定框之外, 命中判定排在栏区判定之前) */
    SwsTwoBtnGeom(xStart, totW, anchor, yStart);
    /* 滚动条贴**外侧**那一列 (与 xStart 里预留的 5px 对应) */
    barX = s_SideRight ? (s_ColMaxX + SWS_ICON_W + 1) : (xStart - 5);
    /* 面板在左时滚动条落在块的**左边**, 而 firstX = xStart —— 缓存矩形从 xStart
     * 起算就会把整根条子裁掉。把它并进矩形左缘 (同时也是提示框的左夹取)。 */
    if (bar && barX < firstX) firstX = barX;
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
    right = s_ColMaxX + SWS_ICON_W;
    if (bar && barX + barW > right) right = barX + barW;
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
    if (!SwsCacheAlloc(right - firstX, bottom - top))
    { SwsDrawZero("cache", right - firstX, bottom - top); goto nogeo; }
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
        int drawn = 0;
        if (it->x < 0) continue;              /* 被滚动裁掉 */
        /* 图标来源, 两个都拿不到就什么都不画 (条目/边框/百分比照旧):
         *   1) **引擎抓到的那张** (优先): 就是防御页此刻画的那张, 战役改写/
         *      地图内嵌 ini 声明的一律对;
         *   2) `SidebarPCX=` 自己读的那条路: 走的是基础规则那份 ini, 战役改写
         *      过的会拿到旧值, 但对那 96 个 rulesmo.ini 里定义的基本一致。
         * ⚠ 两条都空 = 空格子 (症状: 防御页明明有图, 侧栏那个位置是空的)。 */
        if (it->capPix)
        {
            SwsCPSrc(it->x, it->y, SWS_ICON_W, SWS_ICON_H,
                     it->capPix, SWS_ICON_W, SWS_ICON_H, isHover);
            drawn = 1;
        }
        else if (it->pcxFile)
        {
            void* ps = SwsPcxLiveSurf(it->pcxFile);
            if (ps)
            {
                SwsCPcx(it->x, it->y, SWS_ICON_W, SWS_ICON_H, ps, isHover);
                drawn = 1;
            }
        }
        if (drawn)
        {
            /* 未满格 (释放后充能中 / 火风暴类运作中): 压暗。**判据与上面那个
             * 百分比门控完全一致** (`remain > 0 || !ready`), 别用 `!ready` 单判 ——
             * 火风暴类的 IsReady 不可信 (Ares 激活时不清它, 详见上面 `pct` 处的
             * 注释与实测日志 `state=2 ready=1 left=410`)。只按 `!ready` 判的话
             * 这类超武永远不会压暗, 与其它超武"不满即压暗"的表现不一致。
             *
             * 通常 0~1 个条目会被压暗。悬停的那一个不压 —— 玩家正在看它,
             * 让提示框和白框说话。 */
            if ((it->remain > 0 || !it->ready) && !isHover)
                SwsCFillA(it->x, it->y, SWS_ICON_W, SWS_ICON_H, 0, 0, 0, 45);
        }
        {
            /* 图标顶部文字, 最多两行:
             *   行1: 状态/百分比 —— `Hold` (暂停) 或充能百分比 (所有超武一致)
             *   行2: `Run` —— 仅火风暴类运作中 (ChargeDrainState==Draining)
             *
             * 两行各调一次 `RenderLine` 是安全的: 它内部 `ClearDib()` + 重画,
             * 不跨调用留状态 (GdiText.cpp:895)。
             * 行2 只对火风暴类出现, 别的超武行1 位置/语义完全不变。 */
            if (txtOk)
            {
                wchar_t l1[8], l2[8];
                int li;
                /* 两行都要清满: 只清前 4 个的话第 5 个是栈上垃圾, 非零时
                 * sl 会变长, RenderLine 就把 "100%" 后面的垃圾字节也画出来。 */
                l1[0] = l1[1] = l1[2] = l1[3] = 0;
                l1[4] = l1[5] = l1[6] = l1[7] = 0;
                l2[0] = l2[1] = l2[2] = l2[3] = 0;
                l2[4] = l2[5] = l2[6] = l2[7] = 0;
                /* 就绪 (100%) 不显示任何字: 图标本身就是"可用"的意思, 再叠
                 * 一层 100% 只是多一次 GDI 文本渲染, 还压住图标。 */
                if (it->susp)
                {
                    l1[0] = 'H'; l1[1] = 'o'; l1[2] = 'l'; l1[3] = 'd';
                }
                else if (it->total > 0 && (it->remain > 0 || !it->ready))
                {
                    int pct;
                    /* ⚠ 上面那个门控看的是**计时器有没有在跑** (`remain > 0`),
                     * 不是 `IsReady`。火风暴类 (UseChargeDrain) 的 `IsReady`
                     * 不可信: Ares 激活它走 `ClickFire` → `Launch()`, **不清
                     * IsReady** (它本来就是从 Ready 点过来的), 停用/耗尽后也没人
                     * 补清 —— 引擎里唯一把 IsReady 置 1 的地方是计时器归零那条路
                     * (Ares `0x10035587`), 置 0 只在发射路径。
                     * 实测日志 (运作中的爆裂屏障):
                     *     sws: drain BlasticadeSpecial state=2 ready=1 tot=3600 ...
                     * `ready=1` 而 `left` 在减 —— 所以老门控 `!ready` 会把这些超武
                     * **充能时的上升百分比整个挡掉**, 之前只看得到下降的那条
                     * (靠 `|| drainState==2` 兜底)。
                     *
                     * `remain > 0` 是"计时器在跑"的直接证据, 三种状态都成立:
                     *   充能中 → remain>0 → 上升百分比
                     *   就绪   → remain=0 → 不显示 (图标本身即"可用")
                     *   运作中 → remain>0 → 下降百分比 + 行2 `Run`
                     * 附带 `|| !it->ready` 防回归: 万一某类型 IsReady=0 而计时器
                     * 没跑, 老行为照样显示。 */
                    /* ★ 火风暴类**运作中**时百分比是**倒着走**的 (燃料表),
                     * 与充能时相反, 并且分母要换成**维持时长**
                     * (`drainTotal` = 满充能 × ChargeToDrainRatio)。
                     *
                     * 引擎 `SuperClass::GetCameoChargeStage` 的 ChargeDrain 分支
                     * (gamemd 0x6CBF02 起) 算的是 `remaining / GetRechargeTime()`,
                     * 分母用的是**满充能时间**, 所以原版内部那个"充能条"在激活
                     * 瞬间是停在 `ratio` 处 (爆裂屏障 50%、心灵之火 25%)。
                     * 但**侧栏百分比不是那个充能条** —— 原版侧栏拿维持时长当
                     * 分母, 所以是 100% 起跳往下走。实测: 我们先前 49%/24%,
                     * 原版 100%, 差的就是这个分母。
                     *
                     * 不跟原版一致的话, 激活后百分比会从 ratio 跳起, 与原版相反。 */
                    if (it->drainState == 2)
                    {
                        int dt = it->drainTotal > 0 ? it->drainTotal : it->total;
                        pct = (it->remain * 100) / dt;
                    }
                    else
                    {
                        pct = 100 - (it->remain * 100) / it->total;
                    }
                    if (pct < 0) pct = 0;
                    if (pct > 99) pct = 99;      /* 满格就是就绪, 由上面分支处理 */
                    /* 按位数输出: 早先固定写三字符, 1% 就成了 "001%"。 */
                    if (pct >= 10)            /* 按位数输出, 别让 1% 变 "001%" */
                    {
                        l1[0] = (wchar_t)('0' + (pct / 10) % 10);
                        l1[1] = (wchar_t)('0' + pct % 10);
                        l1[2] = '%';
                    }
                    else
                    {
                        l1[0] = (wchar_t)('0' + pct);
                        l1[1] = '%';
                    }
                }
                /* 行2: 火风暴类"运作中"。用英文 `Run` 而不是中文"运作中":
                 * 图标只有 60px 宽, FontPx=16 下 4 个汉字必然溢出/糊成一团
                 * (而 `Hold` 只有 4 个拉丁字母, 一直好好的)。引擎自己给这
                 * 几类的 `Text.Active` 走 CSF 标签 (`NAME:BLASTRUNNING`),
                 * 我们读不到 CSF, 所以取与 `Hold` 视觉宽度一致的 3 字母。 */
                if (!it->susp && it->drainState == 2)
                {
                    l2[0] = 'R'; l2[1] = 'u'; l2[2] = 'n';
                }
                for (li = 0; li < 2; li++)
                {
                    const wchar_t* st = li ? l2 : l1;
                    int sl = 0, sw, sx, ny, adv;
                    while (sl < 7 && st[sl]) sl++;
                    adv = g_G->adv > 0 ? g_G->adv : s_FontPx + 2;
                    if (!sl) continue;
                    sw = RenderLine(st, sl);
                    if (sw <= 0) continue;
                    sx = it->x + (SWS_ICON_W - sw) / 2;
                    ny = it->y + 1 + li * (adv + 1);
                    if (sx < it->x) sx = it->x;
                    if (ny + adv > it->y + SWS_ICON_H) continue;  /* 越界就别画 */
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
        /* 拆列时物理列 0/1 是同一个阵营 (s_SplitSide), 边框色必须跟着阵营走,
         * 否则两列会画成盟军蓝 + 苏联红。 */
        c = kSideCol[s_SplitSide >= 0 ? s_SplitSide : f];
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

    /* ---- 滚动条: 参照战役框 (MsgText.cpp DrawBox 战役分支) ----
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
            /* ★ 滑块位置 = scroll/maxSk 正比。原来写成 (maxSk - scroll), 于是看最上面
             * (scroll=0) 时滑块跑到最下面, 看最下面反而在最上面 —— 和内容反着来。
             * 现在: scroll=0 -> barY (上), scroll=maxSk -> 底部。*/
            thumbY = (s_MaxSk > 0)
                   ? barY + (barH - thumbH) * s_Scroll / s_MaxSk
                   : barY;
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
    s_DrawZeroTag = NULL;   /* 画成了: 下次再失败要重新记一行 */
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