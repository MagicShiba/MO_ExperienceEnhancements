/* SWSBar.cpp - 超武侧栏 (Phobos SuperWeaponSidebar 移植简化版)。
 *
 * 原理、设计取舍与地址推导见 notes/超武侧栏.md。这里只留
 * "读/改这份代码时必须知道"的约束。
 *
 * 不建 Gadget、不碰模拟 (Ares 不动):
 *   - 数据: `SuperClass::Array` 里 `Owner==本家 && IsPresent==1` 的对象,
 *     只读+本地过滤+排序 —— 联机安全。防御页 (TabCameos) 只是 UI 镜像而不是
 *     拥有全集 (在手 78 个超武时它的 CameoCount 只有 56), 不能拿它当显示闸门:
 *     引擎重建窗口内采样会得到极少条目, 开闸后显示数塌到 2->0 且不自愈。
 *   - 分组: 右侧 4 列 (盟/苏/厄/焚风), 空列隐藏; 归属查内建表 (由 rulesmo.ini
 *     授予建筑 Owner/Prerequisite 链离线推导), 查不到落玩家阵营。
 *   - 排序/隐藏: `[SWSidebar]` 单节, 以超武 ID 为键: 负数=隐藏, 0/缺键=
 *     默认 (优先级 1000), 正数=优先级 (越小越靠前, 同值按 ArrayIndex)。
 *   - 图标: 优先 PCX (`SidebarPCX=`, 引擎抓像素), 无则 SHP (`SidebarImage`,
 *     扫描时用引擎 `CC_Draw_Shape` 渲进自有 BSurface, 见 SwsRenderShp)。
 *   - 绘制: 图标盒 60x48 + 阵营色边框 + 充能百分比 + 悬停名提示 (名下有花费时
 *     追加一行 `$金额`, 与原版侧栏的 Ares `Money.Amount` 提示同式),
 *     `[SWSidebar] Alpha` 可调; 自有离屏缓存, 15 个游戏逻辑帧重建一次
 *     (交互时当帧重建), 每渲染帧只做一次带 alpha 的合成。
 *   - 交互: ToggleKey 切换显隐 (隐藏即不占空间: 不画+不吞点击); 左键图标=
 *     选中打靶 (照抄引擎侧栏上膛序列: 清放置模式 + `MapClass::UnselectAll`,
 *     再写 CurrentSWTypeIndex; 全是本地 UI 态, 非模拟), 右键栏内=取消;
 *     点击吞没经 0x692419 门防误下单。
 *   - 资金门 (Ares `Money.Amount`, 只读): 就绪但资金不足的超武压暗 (同引擎
 *     `StripClass_Draw_SuperDarken`), 且点击不选中 (同 `ProcessCameoClick`)。
 *   - 防御支援页去重: 侧栏有的超武不在引擎自带的防御页 (页签 1) 重复出现,
 *     显隐箭头左边的按钮切换。搬出**只能少删**: 判据是"侧栏此刻真的在画它"的
 *     真子集 (类型门 ∧ 实例门 ∧ 扫描快照门), 搬出单向, 多删一条就是永久丢失。
 *   - 规则旗标 (ShowCameo / ManualFire / UseAITargeting / SidebarPCX /
 *     ChargeToDrainRatio 等): 一律读 Ares 已算好的 ExtData。
 *
 * 地址 (只读为主, 首帧探针日志自证)。Ares 下结构体字段比 YRpp 头文件大 0x0C,
 * 头文件地址不可直接抄:
 *   SuperClass::Array 0xA83CB8 / HouseClass::Array 0xA80228 (含虚表);
 *   SuperWeaponTypeClass::Array 0xA8E330;
 *   CurrentPlayer *(0xA83D4C); CurrentFrame *(0xA8ED84);
 *   Scenario PlayerSideIndex *(*(0xA8B230)+0x34B8);
 *   Display 对象 0x87F7E8, CurrentSWTypeIndex +0x11B8 (=0x8809A0)。
 * 任一校验失败即 fail-closed (不画/不点, 记日志)。
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

/* 容器自洽检查 —— **只判容器自己, 一个对象都不碰**。
 *   +0x00 vtable / +0x04 Items / +0x08 Capacity / +0x0C IsInit+IsAlloc
 *   +0x10 Count / +0x14 CapacityIncrement                  sizeof 0x18
 * 成功返回 Items 并写出 Count (Count==0 是合法的"还没有"), 失败返回 NULL。
 * IsPlausiblePtr 是纯算术 (GdiText.cpp), 不读内存。
 *
 * 上界只卡"大到读不下去": `Count <= Capacity` 恒成立 (`VectorClass::Add` 先
 * Reserve 再写 `Items[Count++]`), 反之即垃圾。真正的安全网是逐项全校验
 * (见 SwsScan), 不在 count 上设小上界。 */
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
/* Display 对象即 0x87F7E8 本身, 不可解引用。偏移与 YRpp DisplayClass.h 对应。 */
#define ADDR_CURSW       0x8809A0u   /* Display+0x11B8 CurrentSWTypeIndex */
/* 上膛时清掉的放置/模式字段 (gamemd 0x6AAF46..0x6AAF9D 那一段写的几个)。 */
#define OFF_DISP_CURBLD      0x11A4u  /* ObjectClass*    CurrentBuilding */
#define OFF_DISP_CURBLDTYPE  0x11A8u  /* ObjectTypeClass* CurrentBuildingType */
#define OFF_DISP_CURBLDOWNER 0x11ACu  /* int             CurrentBuildingOwnerArrayIndex */
#define OFF_DISP_POWERMODE   0x11B2u  /* bool PowerToggleMode */
#define OFF_DISP_PLANNING    0x11B3u  /* bool PlanningMode */
#define OFF_DISP_BEACON      0x11B4u  /* bool PlaceBeaconMode */
/* Display::SetActiveFoundation / Map::SetRepairMode / SetSellMode,
 * Map::UnselectAll (static __fastcall, 无栈参)。 */
typedef void (__thiscall *SwsModeFn)(void*, int);
typedef void (__fastcall *SwsUnselectFn)(void);
#define FN_SET_FOUNDATION ((SwsModeFn)(void*)0x4A8BF0u)
#define FN_SET_REPAIR     ((SwsModeFn)(void*)0x4AC8C0u)
#define FN_SET_SELL       ((SwsModeFn)(void*)0x4AC660u)
#define FN_UNSELECT_ALL   ((SwsUnselectFn)(void*)0x48DC90u)

/* Super 字段 (括号里是 YRpp 头文件值, 实际 = 头文件 + 0x0C) */
#define OFF_SUP_TYPE     0x28u   /* (0x1C) */
#define OFF_SUP_OWNER    0x2Cu   /* (0x20) */
/* 充能百分比原式 (gamemd 0x6CBF85, Ares 在 0x6CBF5B 挂 ChargeDrain 分支;
 * ebx=Type, ecx=Super):
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
 * 总量必须用实例级 A (`Super+0x24`), 不用 `Type->RechargeTime` (两者分母不同)。
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

/* SuperWeaponType 字段 (同样 +0x0C 漂移)。
 * `ManualControl` 的 ini 键名是 `ManualControl` (gamemd .rdata 字符串),
 * 不是 Ares 的 `SW.ManualFire` (那是 ExtData 里的另一字段, 见下)。 */
#define OFF_SWT_ID       0x24u   /* (0x18) */
#define OFF_SWT_UINAME   0x60u   /* (0x54) */
#define OFF_SWT_ARRIDX   0x98u   /* (0x8C) */
#define OFF_SWT_ACTION   0xBCu   /* (0xB0) Action: 0 = None = 点击即生效 */
#define OFF_SWT_RECH     0xB0u   /* (0xA4) RechargeTime */
#define OFF_SWT_DRAIN       0xE5u /* UseChargeDrain */
#define OFF_SWT_MANUALCTRL  0xF5u /* ManualControl */
#define OFF_SWT_SIDEBARIMAGE 0xB8u /* SHPStruct* SidebarImage (cameo SHP) */

#define VT_LOCK            0x5C
#define VT_UNLOCK          0x60

#define ADDR_CLICKGATE_SKIP 0x006925FCu

#define SWS_CAP 160
/* 类型下标位图 (swIdx) 的尺寸。防御页 cameo 的 ItemIndex、Super 的
 * Type->ArrayIndex、SuperWeaponType::Array 下标三者同源
 * (HouseClass::Supers 按类型序建), 所以一张按 swIdx 取位的表就能同时表示
 * "见过" 和 "活的"。2048 远大于类型数 (~500), 上界只作野值闸门。 */
#define SWS_LIVE_BITS 2048
#define SWS_LIVE_BYTES (SWS_LIVE_BITS / 8)

/* =======================================================================
 * Ares SWTypeExt::ExtData —— 超武行为的权威来源, 不自己复刻 ini 解析。
 * Ares 把每个超武的扩展数据放在 `SWTypeExt::ExtMap`, 键是**类型对象指针**
 * (`ExtMap.Find(pSuper->Type)`)。证据 (`HouseClass_UpdateSuperWeaponsUnavailable`
 * 里 AddCameo 的判门, 对应 `Hooks.Update.cpp:186-193`):
 *       mov ecx, <ExtMap>; push [eax+0x28]; call find
 *       cmp byte ptr [eax+0x1E5], 0     ; SW_ShowCameo
 *       jne  → AddCameo(0x1F, index)
 * `+0x1E5` 就是引擎决定"进不进防御页"的 1 字节 bool, 读它与引擎逐位一致。
 *
 * 字段偏移取自 `SWTypeExt::ExtData::LoadFromINIFile` 的指令模式
 * (`lea ecx,[esi+OFF]` 紧邻键名字符串, `esi` = ExtData 基址):
 *     10033451 lea ecx,[esi+0x1E2]  <- "SW.ManualFire"
 *     1003347D lea ecx,[esi+0x1E5]  <- "SW.ShowCameo"
 *     10033499 lea ecx,[esi+0x1E6]  <- "SW.Unstoppable"
 *     100335C7 lea ecx,[esi+0x20E]  <- "SW.UseAITargeting"
 *     10033B60 lea ecx,[esi+0x4B0]  <- "SidebarPCX"
 *
 * 定位一律从 Ares 自己的指令里读地址, 不写死绝对地址 (ASLR 后 base 会变):
 *   - `ExtMap` = `mov ecx, <imm32>` 的立即数;
 *   - `find()` = 紧邻 `call` 的落点。
 * ===================================================================== */
#define ARES_EXT_OWNER     0x000u   /* AttachedToObject = SuperWeaponTypeClass* */
#define ARES_EXT_MONEY     0x1A0u   /* Money.Amount (int; <0 = 发射花费) */
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
/* 多键日志拼装: 就地推进 AppendCStr 游标 q (buf 为 char 数组), 免掉每处重复写
 * tmp[16] 与 sizeof(buf)。输出与手写逐段 AppendCStr 逐字节一致:
 *   q = LOGP(buf, "sws: x ");      // 起头 (游标从 0 开始)
 *   q = LOGS(buf, q, " key=");     // 追加字符串 / 键
 *   q = LOGI(buf, q, val);         // 十进制值
 *   q = LOGH(buf, q, val);         // 十六进制值 (无 "0x" 前缀, 同 HtoA) */
static unsigned SwsLogU(char* buf, unsigned q, size_t cap, unsigned v)
{
    char n[16];
    UtoA(v, n);
    return AppendCStr(buf, q, n, cap);
}
static unsigned SwsLogH(char* buf, unsigned q, size_t cap, unsigned v)
{
    char n[16];
    HtoA(v, n);
    return AppendCStr(buf, q, n, cap);
}
#define LOGP(buf, s)     (AppendCStr((buf), 0, (s), sizeof(buf)))
#define LOGS(buf, q, s)  ((q) = AppendCStr((buf), (q), (s), sizeof(buf)))
#define LOGI(buf, q, v)  ((q) = SwsLogU((buf), (q), sizeof(buf), (unsigned)(v)))
#define LOGH(buf, q, v)  ((q) = SwsLogH((buf), (q), sizeof(buf), (unsigned)(v)))
static int SwsTabCameoOk(void);
static int SwsEngineCleanList(void);
static void SwsCleanStampCheck(void);
static int SwsStripCameoCount(int tab);
#define TAB_DEFENSE           1      /* 0 建筑 / 1 防御 / 2 步兵 / 3 载具 */

/* 引擎防御页的抽样快照 (按超武下标开位), 只增不减, **纯诊断用**。
 * 不做显示闸门: 防御页不是拥有全集 (在手 78 个只有 56 槽), 拿它当闸门会在
 * 超武变化时把显示压塌。去重删容器前先抽样, 避免"读→删"自我蚕食。 */
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
    /* 这两处失败不置 `s_AresExtFail` (不闩死): Ares.dll 未必在第一次询问时
     * 已经映射 (SwsFillType 可能经 `SwsSidebarShows` 被提前调到)。资源没准备好
     * 是可恢复的, 只有"找到了但内容不对"才是真失败。 */
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
    /* 跨模块按指针调 `find()`: 签名 ("1 个栈参 + 自己清栈", `ret 4` = C2 04 00)
     * 必须先核对, 猜错 = 栈清错 = 崩溃。对不上则整条 ExtData 路线停用,
     * 退回 ini 快照。 */
    {
        const unsigned char* f = (const unsigned char*)(size_t)s_AresFind;
        int ok4 = 0, i;
        for (i = 0; i < 0x100; i++)
            if (f[i] == 0xC2 && f[i + 1] == 0x04 && f[i + 2] == 0x00)
            { ok4 = 1; break; }
        if (!ok4) goto fail;
    }
    s_AresExtMap = (void*)(size_t)mapAddr;
    /* 定位成功自证一行 (map/find/base 全打出), 方便区分"没定位到"与
     * "定位到了但 find() 查不到"。 */
    {
        char buf[96];
        unsigned q = LOGP(buf, "sws: extmap map=");
        q = LOGH(buf, q, (size_t)s_AresExtMap);
        q = LOGS(buf, q, " find=");
        q = LOGH(buf, q, (size_t)s_AresFind);
        q = LOGS(buf, q, " base=");
        LOGH(buf, q, (size_t)base);
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

/* 取某超武的 Ares ExtData; 取不到返回 NULL (调用方退回 ini 快照)。
 * 键 = `SuperWeaponTypeClass*` 本身, 不是 ID 字符串 (`find()` 对指针做 FNV-1a,
 * 全程不解引用)。取到后须校验 `AttachedToObject == type` (哈希碰撞会返回别人的)。 */
static void* SwsAresExt(void* type)
{
    void* p;
    int slot = 0;
    if (!type || !IsPlausiblePtr(type)) return NULL;
    if (!SwsAresLocateExt()) return NULL;
    if (!SwsIdOk((const char*)type + OFF_SWT_ID)) return NULL;
    s_AresExtTried++;
    /* 键 = 类型指针本身。依据: `HouseClass_UpdateSuperWeaponsUnavailable`
     * 里 `push [eax+0x28]` (eax=pSuper, +0x28=Type) 后 `call find`,
     * 与 `SWTypeExt::ExtMap.Find(pSuper->Type)` 对应。
     * 取到后须校验 AttachedToObject == type (防哈希碰撞串到别的超武)。 */
    p = s_AresFind(s_AresExtMap, type);
    if (!p) { slot = 1; goto miss; }
    if (!IsPlausiblePtr(p)) { slot = 2; goto miss; }
    if (*(void**)((const char*)p + ARES_EXT_OWNER) != type) return NULL;
    return p;
miss:
    /* miss 只记一次, 免得每帧刷屏。判据用函数级 static (slot 进来已非 0)。 */
    {
        static int s_Said = 0;
        if (!s_Said)
        {
            s_Said = 1;
            LogI(slot == 1 ? "sws: ext find() returned NULL, id="
                           : "sws: ext find() ptr implausible", s_AresExtTried);
            {
                char buf[128];
                unsigned q = LOGP(buf, "sws: ext tried type=");
                /* LOGS 返回新游标, 否则下一段覆盖 ID。 */
                q = LOGS(buf, q, (const char*)type + OFF_SWT_ID);
                q = LOGS(buf, q, " type=");
                q = LOGH(buf, q, (size_t)type);
                q = LOGS(buf, q, " map=");
                LOGH(buf, q, (size_t)s_AresExtMap);
                LogLine(buf);
            }
        }
    }
    return NULL;
}

/* 读 ExtData 里的 1 字节 bool。堆块未初始化垃圾值一律当 false
 * (宁可多藏一个, 不可漏藏一个)。 */
static int SwsAresExtBool(void* ext, unsigned off)
{
    unsigned char v;
    if (!ext) return -1;
    v = *(const unsigned char*)((const char*)ext + off);
    return v ? 1 : 0;
}

/* 取 ExtData 里的 `SW.ChargeToDrainRatio` (火风暴类维持时长 = 满充能时间 × ratio)。
 * Ares 存 double @ +0x220, +0x228 是"ini 真写了"标志; 没写回退全局默认 1.0。
 * 返回 0 = 读不到, 调用方回退 1.0。只接受 (0,1] (0 会除零, >1 是脏数据)。 */
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

/* 取 ExtData 里的 SidebarPCX 文件名 (Ares 已小写化, 定长串; 只拷名字,
 * 不碰 BSurface*)。 */
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
 * 铁律: 每帧路径上不许出现 GetPrivateProfileIntA (每帧一次 ini 文件 I/O)。
 * 全部读一次。 */

static int s_Init = 0;
static int s_Enable = 1;
/* 侧栏唯一的透明度旋钮 0~100 (底框/边框/滚动条/按钮/文字/图标; 悬停提示框、
 * 悬停图标除外)。 */
static int s_Alpha = 100;
/* 图标盒固定 60x48 (Phobos 原版 cameo 尺寸), 不做配置项: PCX 是固定尺寸源,
 * 非默认尺寸会溢出格子与底框错位。 */
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

/* 待上膛的超武, 等左键松开才写 CurrentSWTypeIndex。
 * 引擎 0x692B06 一见 (CurrentSWTypeIndex != -1 且当帧按键按着) 就在鼠标处释放,
 * 所以"按下只记录, 松开才写": 按住期由 s_HoldSkip 吞掉引擎地图点击, 写时按键
 * 已松开。Phobos 的真 Gadget 无此问题 (Action 消费点击时顺手清按键态)。 */
static int s_PendSel = -2;   /* -2 none, -1 cancel, >=0 SW ArrayIndex */
static int s_SelDown = 0;    /* 选中的那一下左键是否还按着 (等松开才上膛) */
static int s_ArmIdx = -1;    /* s_SelDown 期间记住的 swIdx */
/* 物理左键按住期间一直吞掉引擎地图点击 (引擎判的是"按住"不是边沿):
 * 按下即吞, 一直到松开。 */
static int s_HoldSkip = 0;
/* 吞没门/滚轮用的"栏区已知"标志 = 缓存里有一帧可合成的画面。
 * 无条目/收起时清零。 */
static int s_BoundsKnown = 0;

/* 即时超武: 走 SpecialPlace 事件 (定义在下方)。
 * 手动打靶类: 走 SwsArmSW —— 引擎上膛序列 (含 UnselectAll), 定义在下方。 */
static int SwsPushSpecialPlace(int swIdx);
static void SwsArmSW(int swIdx);
static int s_ForceScan = 0;   /* 点击时置 1: 强制立刻重扫, 不等 15 帧 */
static int s_ScrollStep = 1;  /* [SWSidebar] ScrollStep */
/* `sws: act` 日志配额 (换 ini/换剧本由 SwsCacheEpoch 重给满)。 */
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
static SwsIniEnt s_Ini[SWS_CAP];
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
/* 填充模式的列边框统一用中性灰 (一列内混阵营, 阵营色不再有意义)。 */
static const unsigned char kFillCol[3] = { 150, 150, 150 };

/* 列的绘制顺序 (下标=side, 值=从左到右第几列): 盟军永远靠屏幕外缘。
 * 面板在右 (原版侧栏在右) → {3,2,1,0}; 面板在左 → {0,1,2,3}。
 * `SwsRebuild` 按 `s_SideRight` 选表。 */
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
    /* 原版侧栏排序键 (gamemd `BuildType::SortsBefore` @0x6A8420):
     *   1) `Type->RechargeTime` 升序; 2) 相等则 `Type->UIName` 字典序升序。
     * 只用来自定义 ini 优先级的那几项, 其余按这个排 (见 SwsOrigAfter)。 */
    int rech;
    const char* pcxFile;  /* SidebarPCX= 文件名, 指向我们自己的表 */
    int instant;          /* 1 = 点击即生效 (Action=None 或 SW.UseAITargeting) */
    int money;            /* Ares Money.Amount: <0 = 发射花费 (显示/选中的门控) */
    unsigned short* capPix;   /* 从引擎 cameo 抓下来的 60x48 像素 (优先于 pcxFile) */
    wchar_t name[24];     /* 悬停提示用; 每类型填一次 */
} SwsItem;

static SwsItem s_Items[SWS_CAP];
static int s_ItemN = 0;
/* 排序结果缓存。排序键全来自 s_Items (只有 SwsScan 会改), 顺序与帧无关,
 * 只在缓存重建时排一次 (SwsScan 结束置 s_OrderN=-1 作废)。
 * s_Items (原序) 与 s_Order (排序后) 是两套下标: 遍历一律写
 * s_Items[s_Order[i]], 不可拿排序下标直接索引 s_Items。 */
static int s_Order[SWS_CAP];
static int s_OrderN = -1;           /* -1 = 还没排过 */
/* 上次排序时的显示模式: 分阵营/填充两套排序键不同, 模式一变就作废重排
 * (条目数没变时 s_OrderN 检测不到)。 */
static int s_OrderFill = -1;
static int s_OrderColCount[4];

/* 侧栏主题色 (淡蓝): 箭头、箭头边框、文字描边统一用它, 方便一处改色。 */
#define SWS_ACCENT_R 0xA5
#define SWS_ACCENT_G 0xD3
#define SWS_ACCENT_B 0xFF
static int s_BoundX = 0, s_BoundY = 0, s_BoundW = 0, s_BoundH = 0;
/* 面板级滚动偏移 (所有列一起上下滚): 列间距为 0 时塞不进逐列滚动条,
 * 与战役框同样整框一个偏移。 */
static int s_Scroll = 0;
static int s_MaxRows = 10;         /* [SWSidebar] MaxRows, 0 = 不限 */
static int s_SideRight = 1;         /* [SWSidebar] Side=right(默认)|left */
/* [SWSidebar] FillMode: 0=按阵营分列 (默认), 1=填充式 (排序不看阵营, 各列高度
 * 尽量均衡: 先两列, 再依次加到三/四列, 满四列才滚动)。显隐箭头上右键切换
 * (只改运行时状态, 不回写 ini; 初值由 ini 给)。 */
static int s_FillMode = 0;
static int s_BtnX = 0, s_BtnY = 0, s_BtnW = 0, s_BtnH = 0;  /* 显隐按钮 */
/* 展开态记下的按钮坐标, 收起时原样沿用 (折叠/展开不跳位)。-1 = 还没展开过。 */
static int s_BtnXKeep = -1, s_BtnYKeep = -1;
/* SWS_Draw 算好的布局, 供点击/滚动判定使用 (由缓存重建路径发布) */
static int s_YStart = 0, s_RowH = 50, s_VisRows = 0;
/* 命中判定用列的横向范围 (不用 s_BoundX: 面板在右时列序是反的,
 * 循环结束的末列反而是最左一列)。 */
static int s_ColMinX = 0, s_ColMaxX = 0, s_MaxSk = 0;
/* 单阵营拆两列时被拆的 side (0~3); -1 = 没拆。拆开后物理列 0/1 共用边框色,
 * 条目按行奇偶分列。 */
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

/* 本家现有资金 (HouseClass::Money)。偏移实证: gamemd 0x705DF3 =
 * `mov eax,[ecx+0x30C]; ret` (Available_Money 就是它) + 0x4F9950 GiveMoney
 * `add [ecx+0x30C]`。只读查询, 联机安全。 */
#define OFF_HOUSE_MONEY 0x30Cu

/* 资金够不够发射这个超武。moneyAmount = Ares Money.Amount (负 = 花费)。
 * 判据同 HouseClass::CanTransactMoney: amount>=0 恒真, 否则 money >= -amount。
 * 读不到资金/脏值时放行 (fail-open): 引擎/Ares 发射那一步自己会再拦一次。 */
static int SwsAfford(int moneyAmount)
{
    void* cur;
    int money;
    if (moneyAmount >= 0) return 1;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return 1;
    money = *(int*)((char*)cur + OFF_HOUSE_MONEY);
    if (money < 0 || money > 1000000000) return 1;
    return money + moneyAmount >= 0;
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

/* 键长 (volatile 逐字节: 防编译器认成 strlen 习语, 无 CRT 链接缺符号)。 */
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
    while (pos < n && s_IniN < SWS_CAP)
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
            SwsStrEq(k, "FillMode") ||
            SwsStrEq(k, "DedupDefensePage");
        /* 新增全局配置项必须同步加进这里, 否则会被当成超武 ID 收进 s_Ini
         * (一个永远不亮的空条目)。与 SwsEnsureInit 里真正读取的那份保持一致。 */
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
     * 全整数定点不用浮点。先由字面值定语义再换算, 不可先乘 100000 再判
     * ("80" 会变成 1000000 像素)。 */
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
    /* 显示模式: 0=按阵营分列 (默认), 1=填充式。可在显隐箭头上右键实时切换。 */
    s_FillMode = GetPrivateProfileIntA(SWS_SECTION, "FillMode", 0, g_IniPath) ? 1 : 0;
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
    p = LOGP(buf, "sws: en=");
    p = LOGI(buf, p, s_Enable);
    p = LOGS(buf, p, " a=");
    p = LOGI(buf, p, s_Alpha);
    p = LOGS(buf, p, " iniN=");
    p = LOGI(buf, p, s_IniN);
    p = LOGS(buf, p, " rows=");
    p = LOGI(buf, p, s_MaxRows);
    p = LOGS(buf, p, " fill=");
    p = LOGI(buf, p, s_FillMode);
    LogLine(buf);
}

/* ---------------- 扫描 (只读, 联机安全) ----------------
 * 组合门控: 先纯安全读 (固定地址字 + 三元组比对, 不碰数组内容);
 * 五元组 (超武/House/本家) 与上一轮任一不同 → 只缓存、本轮返回;
 * 稳定 → 数组已落定, 再做入局检查 (本家须在 House 内) 与逐项全校验。
 * 瞬态垃圾至多存活一轮。 */

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
 * 图标优先 PCX (`SidebarPCX=`, 在 MIX 里, 磁盘上没有), 由引擎 (Ares) 的
 * `CameoPCX` 抓像素; 没写 `SidebarPCX=` 的走 SHP 兜底 (`SidebarImage`,
 * 用 `CC_Draw_Shape` 渲进自有 BSurface, 见 SwsRenderShp)。
 *   读键: CCINIClass::INI_Rules (0x887048) -> INIClass::ReadString (0x528A10)
 *   载图: PCX::Instance (0xAC4848) -> GetSurface / ForceLoadFile
 * 两条命门: GetSurface 未加载给的是占位图 (必须先 ForceLoadFile);
 * BSurface* 不可跨帧缓存 (切页签/重载美术会失效), 每次重建现取。 */

/* Ares 在 0x528A10 挂了 INIClass_GetString (inline 5B JMP), 是 drop-in 替换,
 * 语义与原版一致。 */
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
 * ArrayIndex、阵营、ini 优先级/隐藏都是类型的固有属性 (类型对象静态),
 * 按 type 指针缓存一次, 之后每帧只剩指针比较。 */
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
    int   money;      /* Money.Amount (int; <0 = 发射花费, 缺省 0 = 免费) */
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
    s_TC[i].money = 0;
    s_TC[i].pcxFile = NULL;
    s_TC[i].pcxName[0] = 0;
    s_TC[i].capPix = NULL;
    s_TC[i].name[0] = 0;
    return &s_TC[i];
}

/* 三条显示旗标读类型对象 (引擎用合并后的 ini 建对象, 战役现场改写天然生效)。
 * 偏移已对回 gamemd: CTOR 写 `[ebp+0xE6]=1 / [ebp+0xF5]=0 / [ebp+0xB8]=0 /
 * [ebp+0xBC]=0`, 与 FindIndex 取 ID (+0x24)、DrawCameo 取 SidebarImage (+0xB8)
 * 三处互证 0xBC=Action / 0xE5=UseChargeDrain / 0xF5=ManualControl。
 * 对账日志用 gamemd 的键名 `ManualControl` 比 `0xF5`; Ares 的 `SW.ManualFire`
 * 是另一字段, 拿它比永远对不上。 */
static void SwsFlagsOf(SwsTypeCache* tc, void* type, const char* id)
{
    int iniManual = 1, iniHasManual = 0;
    void* rules = *(void**)ADDR_INI_RULES;
    tc->manual = *(unsigned char*)((char*)type + OFF_SWT_MANUALCTRL) ? 0 : 1;
    tc->drain = *(unsigned char*)((char*)type + OFF_SWT_DRAIN) ? 1 : 0;
    /* Action == None: 引擎不消费目标格, 原版侧栏点一下直接打在鼠标上。
     * 默认值就是 None, 所以没写 `Action=` 的 (含地图内嵌 ini 新增的) 都是
     * "点一下就生效"。MO 基础规则多写 `Action=Custom`, Ares 又改写成
     * `Actions::SuperWeaponAllowed` (0x7F), 落在 else 分支要选格。 */
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
    /* 对账: ini 显式写过 ManualControl 的类型, 对象字段与 ini 读数应当一致
     * (不一致 = 偏移推错了)。ini 没写过的不算错。 */
    if (iniHasManual && (iniManual == 0) != (tc->manual == 0))
        LogS("sws: flags OFFSET? manual ", id);
}

/* 现取 surface: GetSurface 命中直接用, 未命中则 ForceLoadFile 再取。 */
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


/* 首次遇到某超武类型时算一遍 (阵营/优先级/隐藏/旗标/图标/悬停名/即时类),
 * 类型对象静态, 只算一次。抽成独立函数是因为去重 (SwsDedupRemove/Restore)
 * 也要问"这个超武显不显示", 而那两条路不一定先跑过 SwsScan。
 * 填完 tc->prio 即"已填"标志。 */
static void SwsFillType(SwsTypeCache* tc, void* type, const char* id)
{
    int h = 0;
    void* ext = NULL;
    char pcxName[64];
    tc->side = SwsSideOf(id);
    tc->prio = SwsPrioOf(id, &h);
    tc->hide = h;
    /* 对象字段 = 引擎从合并后的 ini 建出来的值, ini 那份只用来对账。 */
    SwsFlagsOf(tc, type, id);

    /* 行为判据取 Ares 算好的 ExtData 最终值 (Ares 的键在类型对象上无对应字段,
     * 且它的 ini 合并/改写没法在外部复刻)。取不到则降级, 不崩。 */
    ext = SwsAresExt(type);
    tc->ratio = 1.0;
    if (ext)
    {
        int v;
        if ((v = SwsAresExtBool(ext, ARES_EXT_SHOW)) >= 0)  tc->show = v;
        /* Ares 的 SW_ManualFire 与 gamemd 的 ManualControl 是两个独立的键,
         * 这里只用它额外收紧, 不覆盖 manual。 */
        if (!SwsAresExtBool(ext, ARES_EXT_MANUAL)) tc->manual = 0;
        /* 火风暴类的维持时长系数: 侧栏算"运作中百分比"的分母要用它。 */
        tc->ratio = SwsAresExtRatio(ext);
        /* 发射所需资金 (Ares Money.Amount: 负 = 花费, 0/正 = 免费或返还)。
         * 与引擎/Ares 同源 (SidebarClass_ProcessCameoClick 用同一个值拦发射,
         * StripClass_Draw_SuperDarken 用同一个值压暗)。越界当 0 (免费)。 */
        {
            int m = *(const int*)((const char*)ext + ARES_EXT_MONEY);
            if (m > -1000000000 && m < 1000000000) tc->money = m;
        }
    }
    if (!tc->show)
    {
        /* `SW.ShowCameo=no` 的不进防御页, 侧栏跟着藏才是与游戏一致。
         * 记一行便于核对藏了谁。 */
        static int s_ShowHidden = 0;
        if (s_ShowHidden < 16)
        {
            LogS("sws: hide cameo ", id);
            s_ShowHidden++;
        }
    }
    /* 图标: ExtData 的 SidebarPCX 优先 (合并后的最终名, 已小写), 取不到退回快照。 */
    if (!ext || !SwsAresExtPcxName(ext, pcxName, sizeof(pcxName)))
        pcxName[0] = 0;
    tc->pcxFile = pcxName[0] ? pcxName : NULL;
    /* pcxFile 可能指向栈上的 pcxName[], 统一拷进类型缓存自有缓冲。 */
    if (tc->pcxFile)
    {
        int k = 0;
        while (k < (int)sizeof(tc->pcxName) - 1 && tc->pcxFile[k])
        { tc->pcxName[k] = tc->pcxFile[k]; k++; }
        tc->pcxName[k] = 0;
        tc->pcxFile = tc->pcxName;
    }
    {
        /* 悬停提示: 优先 UIName, 空则退回 ID。存类型缓存 (条目下标会重排)。 */
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
        /* 缺 SidebarPCX 的记一行, 便于查图标 (还可能靠引擎抓取兜底)。
         * 与 hide cameo 一样只记前 16 条, 换图/多自定义超武时不刷屏。 */
        {
            static int s_NoPcx = 0;
            if (!tc->pcxFile && s_NoPcx < 16)
            {
                LogS("sws: nopcx ", id);
                s_NoPcx++;
            }
        }
    }
    /* "点击即生效" = 一次点击完成, 无需再点地图选格。与 Phobos 的
     * `SWButtonClass::LaunchSuper` 对齐:
     *   Action == None (读对象字段, gamemd 侧栏点击判的也是它) 或
     *   SW_UseAITargeting (读 ExtData: 只存在于地图内嵌 ini 的键, 对象上没有)。
     * 火风暴类不特殊化: 它的 Action 本来就是 0, 自动 instant=1; "激活中"由引擎
     * ChargeDrain 状态机维护, 侧栏只读。 */
    tc->instant = tc->actionNone;
    if (ext && SwsAresExtBool(ext, ARES_EXT_AITGT)) tc->instant = 1;

    (void)tc->drain;   /* 绘制层才用 drainState/drainTotal, 这里只留判据 */
    /* 自证日志: 判据实际取值, 限 40 条, 只给"有看点"的类型记
     * (即时类 / 被藏的 / 不可点的 / 火风暴类 / 无 PCX)。 */
    if (tc->instant || !tc->show || !tc->manual || tc->drain || !tc->pcxFile)
    {
        if (s_DiagLeft > 0)
        {
            char buf[160];
            unsigned q = LOGP(buf, "sws: act ");
            q = LOGS(buf, q, id);
            q = LOGS(buf, q, " v=");
            q = LOGH(buf, q, *(int*)((char*)type + OFF_SWT_ACTION));
            q = LOGS(buf, q, " i");
            q = LOGI(buf, q, tc->instant);
            q = LOGS(buf, q, " m");
            q = LOGI(buf, q, tc->manual);
            q = LOGS(buf, q, " d");
            q = LOGI(buf, q, tc->drain);
            q = LOGS(buf, q, " s");
            q = LOGI(buf, q, tc->show);
            q = LOGS(buf, q, " p=");
            q = LOGS(buf, q, tc->pcxFile ? tc->pcxFile : "-");
            /* `x` = ExtData 取到没有 (x=0 回去查定位器/find)。 */
            q = LOGS(buf, q, ext ? " x1" : " x0");
            LogLine(buf);
            s_DiagLeft--;
        }
    }
}

/* 这个超武 (按 SuperWeaponType 数组下标) 显不显示在超武侧栏。
 * 判据 = !hide && SW_ShowCameo && ManualFire (本家/IsPresent 由调用方判)。
 * `SW_ShowCameo` 不能漏: `=no` 的不进防御页, 侧栏不跟着藏就是"游戏里没有、
 * 侧栏里却有"; 反过来漏掉它, Restore 会把引擎不给的 cameo 加进防御页
 * (改游戏行为)。
 * 这里不判 `s_Enable` / `s_DedupHide` (与"搬回"共用, 切回显示时后者恰为 0,
 * 判了则搬回循环全被 continue, 按钮没反应)。开关类判据由调用方判。 */
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
    /* drain (UseChargeDrain) 不参与过滤, 只决定 instant (见 SwsFillType)。 */
    return (tc->hide || !tc->show || !tc->manual) ? 0 : 1;
}

/* ================= 从引擎手里拿超武 cameo 图标 =================
 *
 * 不能只靠 `SidebarPCX=`: 地图内嵌 ini 可新增/改写 `[XxxSpecial]` 节, 而
 * `CCINIClass::INI_Rules` 只有基础规则 (新增的读出来空, 改写过的读出来是旧值)。
 * 改走引擎那条链: 在 Ares 填好 `CameoPCX` 之后把它读出来。
 *
 * gamemd 原码:
 * ```
 * 6A9950  73 0C                 jae 0x6A995E            ; 越界分支
 * 6A9952  8B 90 B8 00 00 00     mov edx,[eax+0xB8]      ; ← Ares 的钩点 (EAX=SWType)
 * 6A9958  89 54 24 48           mov [esp+0x48],edx      ; ← ★ 我们钩这里 (5 字节)
 * 6A995C  EB 08                 jmp 0x6A9966
 * 6A995E  C7 44 24 48 00000000 mov [esp+0x48],0        ; 越界分支
 * 6A9966  A1 4C 3D A8 00        mov eax,[0xA83D4C]     ; 两条路的汇合点
 * ```
 * Ares 的钩在 0x6A9952 把 `CameoPCX` 填好之后才落到 0x6A9958。
 * 该点上 `CameoPCX` 已填好 (还没被 DrawPCX blit 掉清零), EAX 经 Syringe
 * trampoline 还原成 `SuperWeaponTypeClass*`。
 *
 * 只钩 0x6A9958, 不碰 0x6A9952: 不依赖 Ares 补丁的字节编码, 不裸调它的函数体
 * (它的寄存器快照是 trampoline 压栈的, 裸调读到的是游戏栈垃圾), 只在它之后观察。
 *
 * 壳 (27 字节), 替换 0x6A9958 那 5 字节, 跑完跳 0x6A9966:
 * ```
 * 60                    pushad
 * A3 <slot>             mov [slot],eax          ; pSW = EAX (Syringe 已还原)
 * E8 <rel>              call Sws_CaptureCameo
 * 61                    popad
 * 8B 90 B8 00 00 00     mov edx,[eax+0xB8]      ; ★ 补回被盖掉的指令
 * 89 54 24 48           mov [esp+0x48],edx      ;   (Ares 的钩跳过了它, 所以要补)
 * E9 <rel>              jmp 0x006A9966          ; 直接落到原来那个 jmp 的落点
 * ```
 * pushad/popad 必须成对 (中间是真实 call, 破坏 ebx/esi/edi/ebp);
 * `mov [slot],eax` 不能省 (抓取函数是另一个函数, 无寄存器快照可读)。
 * 写 gamemd .text 必须 `VirtualProtect(PAGE_EXECUTE_READWRITE)` +
 * `FlushInstructionCache`。必须拷像素, 不留 `BSurface*` (切页签/重载美术会释放)。
 * 全程只读 UI 绘制链对象, 联机安全。
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

/* ---- SHP 图标: 用引擎 `CC_Draw_Shape` 把 `SidebarImage` 渲进自有 BSurface ----
 * 没写 `SidebarPCX=` 的超武 (含地图内嵌 ini 新增的节) Ares 的 `CameoPCX` 为 NULL,
 * 抓取钩拿不到像素, 且引擎要等防御页画过一次才有。这里直接复刻引擎那次调用 (调色板
 * `FileSystem::CAMEO_PAL`, frame 0, flags 0x400, brightness 0x3e8), 目标是自有
 * BSurface, 画完拷像素 —— 扫描时即时出图。只读类型字段 + 引擎现成调色板, 不碰
 * 模拟状态, 联机安全。
 *
 * BSurface 布局 (对回 gamemd 0x411510 的 GetRect / 0x411630 BytesPerPixel /
 * 0x411640 GetPitch): +0 虚表 / +4 W / +8 H / +0xC LockLevel / +0x10 字节数 /
 * +0x14 缓冲指针 / +0x18 字节长 / +0x1C own。`CC_Draw_Shape` 只用 GetRect, 不锁面,
 * 直接写 +0x14, 所以栈上拼一个 0x20 的假 BSurface 就够。 */
#define ADDR_CAMEO_PAL   0x0087F6B0u   /* FileSystem::CAMEO_PAL (ConvertClass*) */
#define BSHAPE_VTBL      0x007E2070u   /* gamemd BSurface 虚表 */
typedef struct { int x, y, w, h; } SwsRect;
typedef struct { int x, y; } SwsPt;
/* gamemd 0x4Aed70 `CC_Draw_Shape` (__fastcall: ecx=Surface, edx=Palette, ret 0x38)。
 * 栈参顺序与 YRpp `DrawSHP` 一致: SHP, Frame, Position, Bounds, Flags, Remap,
 * ZAdjust, ZGradient, Brightness, Tint, ZShape, ZFrame, XOff, YOff。 */
typedef void (__fastcall *SwsDrawShpFn)(void* surf, void* pal, void* shp,
    int frame, SwsPt* pos, SwsRect* bounds, int flags, int remap, int zadjust,
    int zgrad, int bright, int tint, void* zshape, int zframe, int xoff, int yoff);

static int SwsRenderShp(void* shp, unsigned short* out)
{
    unsigned int surf[8];          /* 0x20 假 BSurface */
    unsigned char* s = (unsigned char*)surf;
    void* pal;
    SwsRect bounds;
    SwsPt pos;
    int i;
    if (!shp || !IsPlausiblePtr(shp)) return 0;
    if (!IsPlausiblePtr((void*)ADDR_CAMEO_PAL)) return 0;
    pal = *(void**)ADDR_CAMEO_PAL;
    if (!pal || !IsPlausiblePtr(pal)) return 0;
    for (i = 0; i < 0x20; i++) s[i] = 0;
    *(void**)(s + 0x00) = (void*)BSHAPE_VTBL;
    *(int*)(s + 0x04) = SWS_ICON_W;
    *(int*)(s + 0x08) = SWS_ICON_H;
    *(int*)(s + 0x0C) = 0;
    *(int*)(s + 0x10) = 2;
    *(void**)(s + 0x14) = (void*)out;
    *(int*)(s + 0x18) = SWS_ICON_W * SWS_ICON_H * 2;
    *(int*)(s + 0x1C) = 1;
    /* 先铺色键 (紫), 让 SHP 的透明像素落成色键 (绘制层会跳过)。 */
    for (i = 0; i < SWS_ICON_W * SWS_ICON_H; i++) out[i] = PCX_TRANS_PARENT;
    pos.x = 0; pos.y = 0;
    bounds.x = 0; bounds.y = 0; bounds.w = SWS_ICON_W; bounds.h = SWS_ICON_H;
    ((SwsDrawShpFn)(void*)0x004AED70u)(surf, pal, shp, 0, &pos, &bounds, 0x400,
        0, 0, 0, 0x3e8, 0, NULL, 0, 0, 0);
    return 1;
}

/* 扫描时直接给某个类型渲染 SHP 图标 (不等引擎画过一次防御页才抓)。
 * 只用于"无 SidebarPCX"的类型, 且**不**登记进 s_Cap[] —— 引擎抓取 (可能是
 * 地图内嵌声明的 PCX) 仍优先覆盖 tc->capPix。返回 LocalAlloc 的像素或 NULL。 */
static unsigned short* SwsRenderShpCap(void* type)
{
    void* shp = *(void**)((char*)type + OFF_SWT_SIDEBARIMAGE);
    unsigned short* dst;
    if (!shp || !IsPlausiblePtr(shp)) return NULL;
    dst = (unsigned short*)LocalAlloc(LMEM_FIXED,
                     (SIZE_T)(SWS_ICON_W * SWS_ICON_H) * sizeof(unsigned short));
    if (!dst) return NULL;
    if (!SwsRenderShp(shp, dst)) { LocalFree(dst); return NULL; }
    return dst;
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
    type = *(void**)s_PswSlot;
    if (!type || !IsPlausiblePtr(type)) return;
    if (SwsCaptured(type)) return;                  /* 已有, 不重复拷 */
    cam = *(void**)s_CameoPcx;
    if (!cam || !IsPlausiblePtr(cam)) return;       /* SHP 类无 CameoPCX, 交给扫描直渲 */
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
        char buf[64];
        unsigned q;
        q = LOGP(buf, "sws: cap ");
        q = LOGS(buf, q, (const char*)type + OFF_SWT_ID);
        q = LOGS(buf, q, " n=");
        LOGI(buf, q, s_CapN);
        LogLine(buf);
    }
}

/* 从 Ares 钩子体里认出 `CameoPCX` 的地址: 找 .data 段绝对地址被读/写处,
 * 取第一个命中 (mov [m],eax / mov [m],reg / mov dword [m],0 / cmp dword [m],0)。
 * 调用方用两个钩子体交叉确认必须是同一地址 (认错 = 把别人内存当图标写)。 */
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

/* 装壳。返回 1 = 已装 (含失败后不再试)。
 * 只读 Ares 的两个导出符号来定位 `CameoPCX`, 不调它们、不改它的补丁
 * (我们的钩在 Ares 之后, 互不相干)。 */
static int SwsEnsureCameoHook(void)
{
    unsigned char *base, *imgHi;
    FARPROC fPcx, fDraw;
    void* vPcx;
    unsigned aPcx, bPcx;
    unsigned char* st;
    unsigned char* site;
    DWORD old = 0;
    char sb[128];
    unsigned q;
    if (s_HookDone) return 1;
    if (!IsPlausiblePtr((void*)ADDR_CURPLAYER)) return 0;
    if (*(int*)ADDR_FRAME < 60) return 0;
    if (!SwsAresImage(&base, &imgHi)) { s_HookDone = 1; return 0; }
    fPcx = GetProcAddress((HMODULE)base, ARES_HOOK_PCX);
    fDraw = GetProcAddress((HMODULE)base, ARES_HOOK_DRAW);
    vPcx = (void*)(size_t)fPcx;
    /* Ares 地址不用 `IsPlausiblePtr` 判 (它只放行 [0x10000,0x7FF00000),
     * 而 Ares.dll 基址 0x10000000 落在上界之外): 一律按 SwsAresImage 的映像范围判。 */
    if (!fPcx || !fDraw || !vPcx ||
        (unsigned char*)vPcx < base || (unsigned char*)vPcx >= imgHi)
    {
        q = LOGP(sb, "sws: cameo getproc FAIL pcx=");
        q = LOGH(sb, q, (size_t)fPcx);
        q = LOGS(sb, q, " draw=");
        q = LOGH(sb, q, (size_t)fDraw);
        q = LOGS(sb, q, " base=");
        LOGH(sb, q, (size_t)base);
        LogLine(sb);
        goto fail;
    }
    aPcx = SwsFindCameoPcxIn(vPcx, base, imgHi);
    bPcx = SwsFindCameoPcxIn((void*)(size_t)fDraw, base, imgHi);
    if (!aPcx || !bPcx || bPcx != aPcx)
    {
        q = LOGP(sb, "sws: cameo pcx mismatch a=");
        q = LOGH(sb, q, aPcx);
        q = LOGS(sb, q, " b=");
        LOGH(sb, q, bPcx);
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
    q = LOGP(sb, "sws: cameo hook ON pcx=");
    q = LOGH(sb, q, aPcx);
    q = LOGS(sb, q, " base=");
    LOGH(sb, q, (size_t)base);
    LogLine(sb);
    return 1;
fail:
    LogLine("sws: cameo hook FAIL");
    s_HookDone = 1;
    return 0;
}
/* ---------------- 侧栏清单 = 本家拥有的超武 (Super 对象) ----------------
 * 数据源是 `SuperClass::Array` 里 `Owner==本家 && IsPresent==1` 的对象,
 * 不是 rules ini, 也不是防御支援页 (见 SwsScan)。 */

/* ---- ini / 类型数组换代: 把 ini 派生缓存清掉 ----
 * `s_Ini` 与 `s_TC` 一次性填, 但换战役/重开/读档会换 INI_Rules, 类型数组换容器
 * 时类型对象也会重建 (按 type 指针缓存的条目全部错位)。判据用指针 (换 ini 必换
 * 指针, 每帧比内容太贵)。 */
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
 * SwsScan 的 return 都是静默的, 而 s_ItemN=0 可能是"真没有"也可能是"这一帧没扫
 * 出来"。现场分不出, 所以任何早退都留一条日志 (同 tag 只记一次, 状态变了再记)。 */
static int s_ScanZeroTag = 0;   /* 上次记下的 tag (字符串比较太贵, 用索引) */
static const char* const kScanTag[] = {
    "cur", "supitems", "houserr", "houseitems"
};
#define SCAN_TAG_N ((int)(sizeof(kScanTag) / sizeof(kScanTag[0])))

static void SwsScanZero(int tag, int v1, int v2)
{
    char buf[96];
    unsigned q;
    if (tag < 0 || tag >= SCAN_TAG_N) return;
    if (s_ScanZeroTag == tag + 1) return;   /* 同一个原因只记一次 */
    s_ScanZeroTag = tag + 1;
    q = LOGP(buf, "sws: scan0 ");
    q = LOGS(buf, q, kScanTag[tag]);
    q = LOGS(buf, q, " a=");
    q = LOGI(buf, q, v1);
    q = LOGS(buf, q, " b=");
    q = LOGI(buf, q, v2);
    LogLine(buf);
}

/* 容器门失败时把 DynamicVectorClass 头打出来 (只在值变了时记)。
 * 布局见 SwsVecItems 上方。 */
static void SwsDumpVec(unsigned base, int cnt)
{
    static unsigned s_Last = 0xFFFFFFFFu;
    unsigned h;
    char buf[128];
    unsigned q;
    (void)cnt;
    h = (unsigned)*(unsigned*)(base + 4u) * 2654435761u
      ^ (unsigned)*(int*)(base + 8u) * 40503u
      ^ (unsigned)*(int*)(base + 16u) * 2246822519u
      ^ (unsigned)*(int*)(base + 20u);
    if (h == s_Last) return;
    s_Last = h;
    q = LOGP(buf, "sws: vec ");
    q = LOGH(buf, q, base);
    q = LOGS(buf, q, " items=");
    q = LOGH(buf, q, *(unsigned*)(base + 4u));
    q = LOGS(buf, q, " cap=");
    q = LOGI(buf, q, *(int*)(base + 8u));
    q = LOGS(buf, q, " ini=");
    q = LOGI(buf, q, *(unsigned char*)(base + 12u));
    q = LOGS(buf, q, " cnt=");
    q = LOGI(buf, q, *(int*)(base + 16u));
    q = LOGS(buf, q, " inc=");
    q = LOGI(buf, q, *(int*)(base + 20u));
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
    /* 逐项被拒计数: 只在扫空时一次性打出 ("哪道显示门吃掉了全部条目"),
     * 不参与逻辑。rNotClean = 不在防御页快照里, 只供核对, 不过滤。 */
    int rOwner = 0, rPresent = 0, rType = 0, rFlag = 0, rId = 0,
        rCache = 0, rIdx = 0, rHidden = 0, rShow = 0, rNotClean = 0;
    s_ItemN = 0;
    /* 以下全是固定地址字读取, 不碰数组内容, 任何状态下安全。 */
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) { SwsScanZero(0, 0, 0); return; }
    count = 0;
    items = SwsVecItems(ADDR_SUPER_ARR, &count);
    if (!items) { SwsScanZero(1, count, 0); SwsDumpVec(ADDR_SUPER_ARR, count); return; }
    if (!IsPlausiblePtr((void*)ADDR_HOUSE_ARR)) { SwsScanZero(2, 0, 0); return; }
    hCount = 0;
    hItems = SwsVecItems(ADDR_HOUSE_ARR, &hCount);
    if (!hItems) { SwsScanZero(3, hCount, 0); SwsDumpVec(ADDR_HOUSE_ARR, hCount); return; }
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
    /* 数据源: `SuperClass::Array` (本家 + IsPresent, 见下面循环)。
     * 防御页快照只做诊断抽样 (条目数进日志, 供核对"引擎给了谁")。 */
    {
        SwsCleanStampCheck();
        SwsEngineCleanList();
        /* 快照为空不开闸门: 空 = 数据源此刻不可信 (容器刚被去重删空),
         * 不是"本家没有超武"。 */
        s_CleanMode = (s_CleanN > 0) ? 1 : 0;
        /* 快照条目数变了才记 (快照只增, 单调不减)。 */
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
        /* 防御页快照只记数不过滤 (见上面说明)。显示清单 = Super 对象 (Owner==本家 &&
         * IsPresent) + 类型门 (!hide && manual && show), 与引擎 AddCameo 的
         * ShowCameo 判门同源 (读同一 ExtData +0x1E5)。 */
        if (s_CleanMode)
        {
            int b = tc->swIdx;
            if (b < 0 || b >= SWS_LIVE_BITS ||
                !(s_CleanBits[b >> 3] & (1u << (b & 7))))
            { rNotClean++; }
        }
        /* 图标每次扫描重挂 (s_Cap[] 会随解锁/切战役重抓, 不认第一次)。
         * 引擎抓的一律优先 (PCX/地图内嵌声明); 无 SidebarPCX 的走 SHP 直渲,
         * 不必等引擎画过一次防御页 (否则图标要切防御栏才出现)。 */
        {
            unsigned short* eng = SwsCaptured(type);
            if (eng) tc->capPix = eng;
            else if (!tc->capPix && !tc->pcxFile) tc->capPix = SwsRenderShpCap(type);
        }
        /* 显示门 (与防御页一致): !hide && manual && show。
         * 不拿 ShowTimers 当清单 (那只是"正在计时"的子集)。 */
        if (tc->hide || !tc->manual) { rHidden++; continue; }
        if (!tc->show) { rShow++; continue; }
        swIdx = tc->swIdx;
        side = tc->side;
        prio = tc->prio;
        pcxFile = tc->pcxFile;
        capPix = tc->capPix;
        instant = tc->instant;
        /* 倒计时照抄 `SuperClass::GetCameoChargeStage` (总量取实例级 Super+0x24,
         * -1 才回退 Type->RechargeTime; 公式见文件头)。 */
        total = *(int*)((char*)sup + OFF_SUP_RTTOTAL);
        if (total == -1 || total <= 0 || total > 1000000)
        {
            total = *(int*)((char*)type + OFF_SWT_RECH);
            if (total < 0 || total > 1000000) total = 0;
        }
        start = *(int*)((char*)sup + OFF_SUP_RTSTART);
        left = *(int*)((char*)sup + OFF_SUP_RTLEFT);
        /* Start == -1 ⇒ 引擎不扣流逝帧, 直接拿 Left (elapsed>=Left ⇒ 0, 同引擎)。 */
        if (start == -1) remain = left;
        else remain = left - (frame - start);
        if (remain < 0) remain = 0;
        if (total > 0 && remain > total) remain = total;
        s_Items[n].type = type;
        s_Items[n].pcxFile = pcxFile;
        s_Items[n].swIdx = swIdx;
        s_Items[n].side = (side >= 0) ? side : pside;
        s_Items[n].prio = prio;
        /* 名字属于类型, 存类型缓存 (条目下标会重排, 直接拷 tc->name)。 */
        SwsCopyName(s_Items[n].name, tc->name, 24);
        s_Items[n].ready = (ready == 1 && susp == 0) ? 1 : 0;
        s_Items[n].susp = (susp == 1) ? 1 : 0;
        /* 火风暴类 (UseChargeDrain) 的"运作中" = `ChargeDrainState` @ +0x7C,
         * 三态 0=Charging / 1=Ready / 2=Draining。其他类型该字段无意义,
         * 按 tc->drain 门控。 */
        s_Items[n].drainState = tc->drain ? (*(int*)((char*)sup + OFF_SUP_DRAINSTATE) & 0xFF) : 0;
        if (s_Items[n].drainState < 0 || s_Items[n].drainState > 2)
            s_Items[n].drainState = 0;
        /* 运作中百分比的分母 = 维持时长 (`drainTotal = total * ratio`), 不是满充能
         * 时间: 激活瞬间 TimeLeft 从满充能的 ratio 倍开始走, 照抄引擎分母会从
         * ratio 起跳 (爆裂屏障 50%) 而不是 100%。ratio 来自 ExtData, 缺省 1.0。
         * "充能中"仍按引擎口径, 与原版一致。 */
        s_Items[n].drainTotal = 0;
        if (tc->drain && total > 0)
        {
            double dt = (double)total * (tc->ratio > 0.0 ? tc->ratio : 1.0);
            if (dt >= 1.0 && dt <= 1000000.0) s_Items[n].drainTotal = (int)dt;
            if (s_Items[n].drainTotal <= 0)       s_Items[n].drainTotal = total;
        }
        /* 自证: ChargeDrainState 变化各记一行 (偏移读错恒 0/恒 2 时立刻能分辨)。
         * tot=满充能, dt=分母(维持时长), left=分子; 激活瞬间应 left≈dt≈100%。 */
        if (tc->drain)
        {
            static int s_LastDs = -1;
            if (s_LastDs != s_Items[n].drainState)
            {
                char line[96];
                unsigned w;
                s_LastDs = s_Items[n].drainState;
                w = LOGP(line, "sws: drain ");
                w = LOGS(line, w, (const char*)type + OFF_SWT_ID);
                w = LOGS(line, w, " state=");
                w = LOGI(line, w, s_Items[n].drainState);
                w = LOGS(line, w, " ready=");
                w = LOGI(line, w, ready);
                /* dt=分母(维持时长), left=分子。若 dt 退回等于 tot, 就是 ratio
                 * 没读到 (会从 ratio 起跳)。 */
                w = LOGS(line, w, " tot=");
                w = LOGI(line, w, total);
                w = LOGS(line, w, " dt=");
                w = LOGI(line, w, s_Items[n].drainTotal);
                w = LOGS(line, w, " left=");
                LOGI(line, w, remain);
                LogLine(line);
            }
        }
s_Items[n].remain = remain;
    s_Items[n].total = total;
    s_Items[n].instant = instant;
    s_Items[n].money = tc->money;
    s_Items[n].capPix = capPix;
    s_Items[n].rech = *(int*)((char*)type + OFF_SWT_RECH);
    n++;
    }
    s_ItemN = n;
    s_OrderN = -1;      /* 条目变了, 排序缓存作废 (见 s_Order 声明处) */
    if (n > 0)
    {
        /* 条目数变化就打一行 ("侧栏忽然空了/又回来了"靠它定位)。 */
        static int s_LastN = -1;
        s_ScanZeroTag = 0;
        if (n != s_LastN)
        {
            s_LastN = n;
            LogI("sws: scan ok n=", n);
        }
        /* 实际画出的 ID 逐个列出 (条目集变化时才记): 与截图核对多出来的是谁、
         * 该靠哪条规则滤掉 (`sws: act` 只记"有看点"的类型, 配额有限, 对不上号)。 */
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
                /* 逐字节判容量 (单轮最多追加 21 字节, 只判一次会溢出 cur[512])。 */
                {
                    unsigned j;
                    for (j = 0; j < 20 && sid[j] && q + 2 < sizeof(cur); j++)
                        cur[q++] = sid[j];
                    if (q + 1 < sizeof(cur)) cur[q++] = ',';
                }
            }
            cur[q] = 0;
            if (!s_LastSetInit || !SwsStrEq(s_LastSet, cur))
            {
                char line[600];
                unsigned w = LOGP(line, "sws: list ");
                w = LOGS(line, w, cur);
                /* 带上当前规则集地址 (换图必变), 日志才能对上是哪局。 */
                w = LOGS(line, w, "   [rules=");
                w = LOGH(line, w, (size_t)*(void**)ADDR_INI_RULES);
                w = LOGS(line, w, " cameo=");
                w = LOGI(line, w, SwsStripCameoCount(TAB_DEFENSE));
                w = LOGS(line, w, " snap=");
                LOGI(line, w, s_CleanN);
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
    /* 一条都没扫出 = 侧栏整块不画且去重停手。把各道显示门吃掉的条数一起打出,
     * 一眼看出是哪道全吃掉了。正常态 (开局没超武/收起) 本来就是 0, 每轮都打
     * 等于 4 次/秒刷屏: 同签名只记一次, 计数变了再记。 */
    if (n <= 0)
    {
        char buf[128];
        unsigned sig;
        static unsigned s_LastN0 = 0xFFFFFFFFu;
        sig = (unsigned)count * 31u + (unsigned)rOwner;
        sig = sig * 31u + (unsigned)rPresent;
        sig = sig * 31u + (unsigned)rType;
        sig = sig * 31u + (unsigned)rFlag;
        sig = sig * 31u + (unsigned)rId;
        sig = sig * 31u + (unsigned)rCache;
        sig = sig * 31u + (unsigned)rIdx;
        sig = sig * 31u + (unsigned)rHidden;
        sig = sig * 31u + (unsigned)rShow;
        sig = sig * 31u + (unsigned)rNotClean;
        sig = sig * 31u + (unsigned)hCount;
        if (sig == s_LastN0) return;
        s_LastN0 = sig;
        unsigned q = LOGP(buf, "sws: scan0 n0 tot=");
        q = LOGI(buf, q, count);
        q = LOGS(buf, q, " owner=");
        q = LOGI(buf, q, rOwner);
        q = LOGS(buf, q, " present=");
        q = LOGI(buf, q, rPresent);
        q = LOGS(buf, q, " type=");
        q = LOGI(buf, q, rType);
        q = LOGS(buf, q, " flag=");
        q = LOGI(buf, q, rFlag);
        q = LOGS(buf, q, " id=");
        q = LOGI(buf, q, rId);
        q = LOGS(buf, q, " cache=");
        q = LOGI(buf, q, rCache);
        q = LOGS(buf, q, " idx=");
        q = LOGI(buf, q, rIdx);
        q = LOGS(buf, q, " hidden=");
        q = LOGI(buf, q, rHidden);
        q = LOGS(buf, q, " nocameo=");
        q = LOGI(buf, q, rShow);
        q = LOGS(buf, q, " notseen=");
        q = LOGI(buf, q, rNotClean);
        q = LOGS(buf, q, " page=");
        q = LOGI(buf, q, 1);
        q = LOGS(buf, q, " hcnt=");
        LOGI(buf, q, hCount);
        LogLine(buf);
    }
}

/* ================= 隐藏"防御支援页"里的重复超武 =================
 * 目标: 侧栏上有的超武, 防御支援页 (页签 1) 里不再出现; 侧栏上没有的照旧。
 *
 * 引擎只往页签加 cameo, 从不因"已在超武侧栏显示"而拒绝; 唯一的移除路径
 * RecheckCameo(0x6AA600) 只删 IsPresent==0 的条目。Ares 把容器换成 .data 段的
 * DynamicVectorClass<BuildType> RulesExt::TabCameos[4], 没导出删除接口 ——
 * 只能直接改这个数组。这是本文件唯一的写操作, 但全是本地 UI 数据
 * (不碰 SuperClass/EventClass)。
 *
 * 两个方向不对称: 搬出改 Ares 数组 (反查地址 + 逐次校验容器); 搬回只调引擎
 * 自己的 AddCameo (自带查重, 重复调用无害) —— "切回显示"不依赖反查地址。
 * 复查由 SwsDedupSync 负责。 */

/* SidebarClass 侧栏几何 (gamemd 反汇编; 与 YRpp 头文件差 4 字节, 以 Ares 代码为准):
 * Tabs[0] +0x1544, CameoCount +0x54。 */
#define ADDR_SIDEBAR          ADDR_DISPLAY   /* 侧栏对象就在这个地址上 */
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
/* 四个容器的 CapacityIncrement (ClearCameos 写死, 定位指纹用)。 */
#define TABCAM_INC      100

/* 页签 i 的 CameoCount 实时值。
 * MouseClass/SidebarClass/DisplayClass 是同一个静态对象, 地址就是 0x87F7E8
 * 本身 (原版直接 `mov ecx,0x87f7e8` 当 this): 不能解引用, 那样拿到的是虚表指针。
 *   StripClass::Index +0x38 / TopRowIndex +0x44 / CameoCount +0x54
 *   SidebarClass::Tabs = pThis + 0x1544, 步长 0xF94
 * 偏移从 Ares 代码读出 (SkipSizeCheck 的 `imul eax,ebx,0xf94` /
 * `add eax,0x880d2c`=0x87f7e8+0x1544; RecheckCameos 的 `cmp [edi+0x54],0`)。
 * YRpp 头文件这几个字段小 4, 以 Ares 代码为准。 */
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
 * 引擎防御页抽样 —— 纯诊断, 不是数据源 (理由见上方 s_CleanBits 处说明)。
 *
 * 采样 = `RulesExt::TabCameos[防御页]`, 长度以 `StripClass::CameoCount`
 * (+0x54) 为准。每项 `BuildType` 步长 0x34: +0x00 ItemIndex /
 * +0x04 ItemType (超武=0x1F)。注意 `+0x58` 不是 CameoCount,
 * 是 `StripClass::Index` (页签号)。
 *
 * 本容器会被 `SwsDedupRemove` 缩短, 不能直接读 ("读→删"自我蚕食):
 * 去重删之前先并进快照, 快照只增不减, 只进日志不进判据。
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
    /* CameoCount 是权威长度; 容器自己的 Count 在 ClearCameos→AddCameo 窗口里
     * 可能没同步, 取小的。 */
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
    /* 快照条目数 = 整张快照 popcount (累积值, 只进日志 `snap=` 供核对)。 */
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

/* 清空快照: 只在引擎重建整套超武时 (换剧本, 判据=类型数组 Items 指针变了),
 * 否则上局的超武会带进这局。首帧只登记不动作。 */
static void SwsCleanStampCheck(void)
{
    void** it = SwsVecItems(ADDR_SWTYPE_ARR, NULL);
    static void* s_Last = NULL;
    static int s_StampInit = 0;
    if (!s_StampInit) { s_StampInit = 1; s_Last = it; return; }
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

/* 取 Ares.dll 映像范围 [lo,hi) (解析 PE 头拿 SizeOfImage): 校验反查地址落在
 * .data 里, 以及容器 vtable 是 Ares 函数而非野指针。 */
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

/* 单个 DynamicVectorClass<BuildType> 自洽检查 (只判自身)。
 * 指纹: CapacityIncrement 恒为 TABCAM_INC (Ares ClearCameos 写死, 终身不变)。 */
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

/* 定位 Ares 的 RulesExt::TabCameos[4]: 从 Ares 导出的
 * `SidebarClass_AddCameo_SkipSizeCheck` 开头几十字节里抓两条绝对地址
 * (`mov eax,[ecx*8+d]` 取 Items, `imul ecx,[ecx*8+d],0x34` 取 Count),
 * 于是 TabCameos = Items地址-4, 两条互证 (Count地址-0x10 须指向同一容器)。
 * 跟着 Ares 代码走: 不扫数据段、不误命中、ASLR 后仍对。找不到/对不上/
 * 容器不自洽一律 fail-closed。
 * 只在跑一局时找 (本家指针有效 + 帧号>60; 不用 `Unsorted::ScenarioInit`,
 * 那是个长期为 0 的计数器)。 */
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
    /* "在跑一局"判据: 本家指针有效 + 帧号走过 60。地址按映像范围判
     * (Ares 基址落在 IsPlausiblePtr 上界之外, 不用它)。 */
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return NULL;
    if (*(int*)ADDR_FRAME < 60) return NULL;
    if (!SwsAresImage(&base, &imgHi)) { s_Failed = 1; return NULL; }
    fn = GetProcAddress((HMODULE)base, ARES_SKIPCHECK);
    /* 按映像范围判, 不用 IsPlausiblePtr (见上)。 */
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
    /* Items 在容器 +0x04, Count 在 +0x10, 两条须指向同一容器的两个字段。 */
    if ((unsigned char*)(size_t)itemsAddr - 4 !=
        (unsigned char*)(size_t)cntAddr - 0x10)
        goto fail;
    found = (unsigned char*)(size_t)itemsAddr - 4;
    /* 四个容器逐个自洽 + vtable 相同 (同一类的连续数组)。 */
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
        char sb[96];
        unsigned q = LOGP(sb, "sws: dd tabcameo=");
        q = LOGH(sb, q, (size_t)found);
        q = LOGS(sb, q, " tc=");
        for (t = 0; t < TABCAM_N; t++)
        {
            q = LOGS(sb, q, t ? "/" : "");
            q = LOGI(sb, q, *(int*)(found + (unsigned)t * DVEC_SIZE +
                                    DVEC_OFF_COUNT));
        }
        LogLine(sb);
    }
    return s_TabCam;
fail:
    LogS("sws: dd off got=", gotItems ? (gotCnt ? "1/1" : "1/0") : "0/0");
    s_Failed = 1;
    return NULL;
}

/* 每次用之前重校验容器 (Items 扩容会变, 现读; 容器头地址终身不变,
 * Ares 只清内容不搬容器)。不拿 `StripClass::CameoCount` 互证 (它与
 * TabCameos[i].Count 不同步, 当闸门等于堵死整条路)。 */
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

/* 防御页条目变动后把页签状态补齐 (抄 Ares StripClass_RecheckCameos 收尾)。
 * 侧栏对象就在 ADDR_SIDEBAR 地址上, 不要解引用 (拿到的是虚表指针,
 * 当 this 调引擎函数直接崩)。 */
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
    /* Ares 维护两份计数: 容器 Count (只改这份) 与 StripClass::CameoCount
     * (页签布局按它算)。两份都要跟上, 否则页签按旧计数画、点到空位。 */
    if (SwsStripCameoCount(TAB_DEFENSE) >= 0)
        *(int*)(strip + OFF_STRIP_CAMCOUNT) = cnt;
    if (cnt > 0)
    {
        ((int (__thiscall *)(void*))(void*)ADDR_UPDSCROLLBTN)(sb);
    }
    else
    {
        /* 页签空了: 关掉它的页签按钮, 跳到第一个非空页签
         * (与原版 AddCameo 的 enable/disable 同一套 Gadget 接口)。 */
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
    /* 滚动位置只夹取不重置 (玩家可能正翻到页中间)。上界 = 条目数 - 一屏条数。 */
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

/* ---- 搬出 (从防御页删除) 的判据: 必须是 SwsSidebarShows 的真子集 ----
 * 铁律: 删前须证明侧栏此刻真的在画它。删错 = "侧栏不画 + 防御页被删"两头都没了,
 * 且搬出单向不会自愈 (只有手点按钮切回显示态才补回)。宁可少删。
 * 只拿类型门会在两种情况下删出丢失:
 *   (1) 扫描早退 —— 那一帧侧栏一格不画, 照删会一次清空整页;
 *   (2) 实例缺失 —— 引擎把 cameo 放进防御页与 Super 变成 IsPresent==1 之间有
 *       时间差 (搬回那条路本来就有实例门, 搬出没有就是不对称的洞)。
 * 三道门补上: 类型门 ∧ 实例门 ∧ 快照门 (最近一次扫描条目表里真有它)。
 * 收起时 s_Items 为空, 快照门天然全挡, 不会误删。 */


/* 遍历本家、已就绪且未暂停的超武实例 (一趟, 只读)。每命中一个调 cb(sup,type,swIdx,ctx)。
 * 返回命中数; 玩家/容器不可用返回 -1; 容器条目数经 pContainerN 带出 (可 NULL)。
 * 判据 (owner==本家 ∧ IsPresent==1 ∧ IsReady<=1 ∧ IsSuspended<=1) 由下面三处共用。 */
typedef void (*SwsSuperCb)(void* sup, void* type, int swIdx, void* ctx);
static int SwsForEachReadySuper(SwsSuperCb cb, void* ctx, int* pContainerN)
{
    void** items;
    int count = 0, i, n = 0;
    void* cur = *(void**)ADDR_CURPLAYER;
    if (pContainerN) *pContainerN = -1;
    if (!cur || !IsPlausiblePtr(cur)) return -1;
    items = SwsVecItems(ADDR_SUPER_ARR, &count);
    if (!items) return -1;
    if (pContainerN) *pContainerN = count;
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
        if (cb) cb(sup, type, swIdx, ctx);
        n++;
    }
    return n;
}

/* 本家"活的"超武 (IsPresent==1) 下标位图, 一趟建好供搬出逐条 O(1) 查。
 * 判据与 SwsDedupRestore 相同 (两方向对称)。返回个数, -1 = 容器不可用。 */
struct SwsLiveCtx { unsigned char* live; int n; };
static void SwsLiveCb(void* sup, void* type, int swIdx, void* ctx)
{
    struct SwsLiveCtx* c = (struct SwsLiveCtx*)ctx;
    (void)sup; (void)type;
    if (!(c->live[swIdx >> 3] & (unsigned char)(1u << (swIdx & 7))))
    {
        c->live[swIdx >> 3] |= (unsigned char)(1u << (swIdx & 7));
        c->n++;
    }
}
static int SwsBuildLiveSet(unsigned char* live)
{
    struct SwsLiveCtx c;
    int i;
    for (i = 0; i < SWS_LIVE_BYTES; i++) live[i] = 0;
    c.live = live; c.n = 0;
    if (SwsForEachReadySuper(SwsLiveCb, &c, NULL) < 0) return -1;
    return c.n;
}

static int SwsLiveHas(const unsigned char* live, int swIdx)
{
    if (swIdx < 0 || swIdx >= SWS_LIVE_BITS) return 0;
    return (live[swIdx >> 3] & (unsigned char)(1u << (swIdx & 7))) ? 1 : 0;
}

/* 最近一次 SwsScan 条目表里有没有这个超武 (s_Items 存类型下标, 与防御页
 * cameo 的 ItemIndex 同源, 直接比下标)。 */
static int SwsSidebarShowing(int swIdx)
{
    int k;
    if (s_ItemN <= 0) return 0;         /* 扫描早退: 侧栏此刻一格都没画 */
    if (s_ItemN > SWS_CAP) return 0;
    for (k = 0; k < s_ItemN; k++)
        if (s_Items[k].swIdx == swIdx) return 1;
    return 0;
}

/* 侧栏此刻画得出图标吗? 无图标的条目先留在防御页上等抓取 (搬出单向, 搬走后
 * 钩子永远抓不到, 变"两边都没有"; 重复显示几秒比永久空格子好, 抓到后复查搬走)。 */
static int SwsSidebarHasIcon(int swIdx)
{
    int k;
    if (s_ItemN <= 0 || s_ItemN > SWS_CAP) return 0;
    for (k = 0; k < s_ItemN; k++)
        if (s_Items[k].swIdx == swIdx)
            return (s_Items[k].capPix || s_Items[k].pcxFile) ? 1 : 0;
    return 0;
}

/* 从防御页删掉"侧栏上确实也有"的超武 cameo。返回删掉几条, -1 = 不可用。
 * 顺序依赖: 读快照 (数据源) 在前, 删容器在后; 删的是"本轮画出来了"的子集
 * (已含"在快照里"), 不会误删没画的。 */
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
        /* 倒着删: 后面先缩, 前面下标才不因搬移失效。tail = 本次前移条数。 */
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
 * 返回补上几条; 玩家/容器不可用返回 -1 (与旧 SwsDedupRestore 判据一致:
 * 容器 count<=0 也算不可用)。SwsDedupRestore 与 SwsNudgePaint 共用这一条路。 */
struct SwsAddCtx { int added; };
static void SwsAddCb(void* sup, void* type, int swIdx, void* ctx)
{
    struct SwsAddCtx* c = (struct SwsAddCtx*)ctx;
    (void)sup; (void)type;
    if (!SwsSidebarShows(swIdx)) return;
    /* 不碰 Ares 容器: AddCameo 自带查重, 对已在页里的重复调用是无害空操作。 */
    if (((int (__thiscall *)(void*, int, int))(void*)ADDR_ADDCAMEO)
            ((void*)ADDR_SIDEBAR, ABS_SPECIAL, swIdx))
        c->added++;
}
static int SwsAddMissingCameos(void)
{
    struct SwsAddCtx c;
    int cn = 0, hits;
    c.added = 0;
    hits = SwsForEachReadySuper(SwsAddCb, &c, &cn);
    if (hits < 0 || cn <= 0) return -1;
    return c.added;
}

/* 把"侧栏上有、但防御页没有"的超武 cameo 补回去, 返回补上几条, -1 = 不可用。 */
static int SwsDedupRestore(void)
{
    int added = SwsAddMissingCameos();
    if (added < 0) return -1;
    /* AddCameo 只在特定情况切页签; 这里补一次刷新, 保证刚补进来的画出来。 */
    if (added)
        ((void (__thiscall *)(void*, int))(void*)ADDR_REPAINTSIDEBAR)
            ((void*)ADDR_SIDEBAR, TAB_DEFENSE);
    return added;
}

/* 把目标态落到防御页上 (幂等)。返回 1 = 确实动过。
 * 已在隐藏态也照跑一遍: 新冒进防御页的 cameo 靠复查搬走 (移除幂等, 代价一遍扫描)。
 * 不用 AddCameo 入口钩挡新增 (超武 cameo 不走那条路)。 */
static int SwsDedupSync(void)
{
    int want, r;
    SwsEnsureInit();
    if (!s_Enable) { s_DedupDirty = 0; return 0; }
    /* 目标态只看去重开关: 收起 (F4) 不碰防御页 —— 已经搬走的保持搬走,
     * 侧栏折叠期间原游戏侧栏里的超武也不恢复, 由用户点"隐藏重复"按钮切回。
     * 收起时 s_Items 为空, Remove 的真子集门 (SidebarShowing/HasIcon) 天然
     * 一条都搬不动, 只能少删, 不会误删; 展开后扫描 + 复查会自动补搬。 */
    want = s_DedupHide ? 1 : 0;
    if (want == s_DedupDone && !s_DedupDirty) return 0;
    if (want && !SwsAresFindTabCameos()) { s_DedupDirty = 0; return 0; }
    /* 收起时不扫 (s_Items 为空是正常的): 这一轮什么都不做, 脏位清掉免得
     * 空转; 展开后第一次扫描会重新置脏补搬。 */
    if (want && !s_Visible)
    {
        s_DedupDone = want;
        s_DedupDirty = 0;
        return 0;
    }
    /* 想删但侧栏还没扫出条目 (开局/重开 SwsScan 早退那一轮): 什么都不做,
     * dirty 留给下一次扫描。 */
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
        /* 失败会每帧重试, 必须限流 (600 帧记一次), 否则刷爆日志。 */
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
    /* 只有搬出改了容器才调 SwsDefenseRepair (搬回由 AddCameo 自己收尾;
     * 没搬动任何东西也不用动页签)。 */
    if (want && r > 0) SwsDefenseRepair();
    /* 真动了东西 / 目标态翻转才记一行 (复查不刷屏)。 */
    if (r > 0 || want != s_DedupDone)
    {
        char sb[64];
        unsigned q = LOGP(sb, "sws: dd ");
        q = LOGS(sb, q, want ? "hide n=" : "show n=");
        LOGI(sb, q, r);
        LogLine(sb);
    }
    s_DedupDone = want;
    s_DedupDirty = 0;
    return r;
}

/* ---------------- 图标兜底: 让引擎自己把防御页重画一次 ----------------
 * 抓取钩只在引擎画防御页 cameo 时有数据, 但钩子要 frame>=60 才装、防御页默认
 * 不是激活页签、地图内嵌新增的超武读不到 SidebarPCX —— 结果是"格子空着"。
 * 光调 RepaintSidebar 抓不到 (槽没画): 必须先 AddCameo 把 super 挂到槽上
 * (自带查重, 无害), 再 Repaint。这里复用 SwsDedupRestore 那条路, 每轮扫描最多
 * 催一次, 催满停手。纯 UI 重绘, 不碰模拟。
 * 安全: 紧接着的 SwsDedupSync 仍是单向搬出, 多加的 cameo 下一轮会被搬走。 */
#define SWS_NUDGE_MAX 8
static int s_NudgeLeft = SWS_NUDGE_MAX;

static void SwsNudgePaint(void)
{
    int i;
    if (s_ItemN <= 0 || s_NudgeLeft <= 0) return;
    for (i = 0; i < s_ItemN; i++)
        if (!s_Items[i].capPix && !s_Items[i].pcxFile) break;
    if (i >= s_ItemN) return;          /* 图标都齐了, 不用再催 */
    s_NudgeLeft--;
    /* 与 SwsDedupRestore 同一条路: 补 cameo (自带查重) 后重画防御页。
     * 玩家/容器不可用时静默返回 (与旧的早退一致)。 */
    {
        int added = SwsAddMissingCameos();
        if (added < 0) return;
        ((void (__thiscall *)(void*, int))(void*)ADDR_REPAINTSIDEBAR)
            ((void*)ADDR_SIDEBAR, TAB_DEFENSE);
        if (s_DiagLeft > 0 && s_NudgeLeft == 0)
        {
            char buf[64];
            unsigned q = LOGP(buf, "sws: nudge done added=");
            LOGI(buf, q, added);
            LogLine(buf);
        }
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

/* 切显示模式: 分阵营 <-> 填充。排序键随之变 (填充不看阵营), 作废排序缓存;
 * 模式进 SwsSig, 缓存下一帧自动重建。只改运行时状态, 不回写 ini。 */
static void SwsToggleFill(void)
{
    s_FillMode = s_FillMode ? 0 : 1;
    s_OrderN = -1;
    LogLine(s_FillMode ? "sws: fillmode on" : "sws: fillmode off");
}

int SWS_Enabled(void)
{
    SwsEnsureInit();
    return s_Enable;
}

static void SwsInvalidateCache(void);

void SWS_SetEnabled(int enable)
{
    SwsEnsureInit();
    enable = enable ? 1 : 0;
    if (enable == s_Enable) return;
    s_Enable = enable;
    SwsInvalidateCache();
    s_BoundsKnown = 0;
    if (enable)
    {
        s_ForceScan = 1;
        s_DedupDirty = 1;
    }
    else
    {
        s_HidValid = 0;
    }
}

int SWS_FillMode(void)
{
    SwsEnsureInit();
    return s_FillMode;
}

void SWS_SetFillMode(int fill)
{
    SwsEnsureInit();
    if ((fill ? 1 : 0) == s_FillMode) return;
    SwsToggleFill();
}

int SWS_MaxRows(void)
{
    SwsEnsureInit();
    return s_MaxRows;
}

void SWS_SetMaxRows(int rows)
{
    SwsEnsureInit();
    if (rows < 1) rows = 1;
    if (rows > 40) rows = 40;
    if (rows == s_MaxRows) return;
    s_MaxRows = rows;
    s_Scroll = 0;
    s_OrderN = -1;
    SwsInvalidateCache();
    s_BoundsKnown = 0;
    s_ForceScan = 1;
}

void SWS_Poll(void)
{
    static int s_LastItemN = -1;
    int tog;
    /* ---- 帧门: 挂在 0x4F42F0, 但它不是每帧一次 (上百个静态调用点, 散在
     * AI/对象/渲染/消息代码, 多处在逐对象循环内, 每渲染帧被调上百次)。
     * 用引擎帧号变化做门 ("与上次相同则跳过")。 */
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
        s_DedupDirty = 1;    /* 收起不碰防御页, 展开后复查补搬 */
        LogLine(s_Visible ? "sws: show" : "sws: hide");
    }
    s_PrevTog = tog;
    if (!s_Visible)
    {
        /* 隐藏时仍采样键态, 保留显隐箭头点击 (箭头一直画着)。
         * 边沿须用和可见分支同一份 s_PrevLB, 每条 return 分支自己写回,
         * 否则伪造新按下沿, 刚收起立刻又弹回 (点一下只闪一下)。 */
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
            else if (rb && !s_PrevRB)
            {
                /* 收起态右键显隐箭头 = 切换分阵营/填充 (不展开)。 */
                int hx = -1, hy = -1;
                if (GtMouseGameXY(&hx, &hy) &&
                    hx >= s_BtnX && hx < s_BtnX + s_BtnW &&
                    hy >= s_BtnY && hy < s_BtnY + s_BtnH)
                    SwsToggleFill();
            }
            s_PrevLB = lb;
            s_PrevRB = rb;
        }
        s_ItemN = 0;
        s_PendSel = -2;
        s_SelDown = 0;
        if (!s_PrevLB) s_HoldSkip = 0;   /* 与可见分支同规则: 按住期间不清 */
        /* 收起状态下防御页保持去重态 (不再恢复原样), 所以这条早退之前先把
         * 同步做完, 免得切态那一帧漏掉。 */
        SwsDedupSync();
        return;
    }
    /* 应用上一帧记下的选择 (松开边沿记下, 此时左键已松开,
     * 引擎"按住即释放"的条件不可能成立)。 */
    if (s_PendSel != -2)
    {
        int v = s_PendSel;
        s_PendSel = -2;
        /* 走引擎上膛序列 (含 UnselectAll), 不是裸写 CurrentSWTypeIndex:
         * 选中着单位时裸写会"点了地图放不出去", 见 SwsArmSW 上方注释。 */
        SwsArmSW(v);
        LogLine(v >= 0 ? "sws: armed" : "sws: cancel");
    }
    {
        int lb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
        int rb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 1 : 0;
        /* 上膛统一在松开边沿做 (见 s_PendSel)。 */
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
                    /* 两按钮画在超武块上方 (栏区判定框之外), 须在栏区判定之前
                     * 单独命中 (栏区要求 my >= s_BoundY, 按钮在别的行)。 */
                    if (mx >= s_BtnX && mx < s_BtnX + s_BtnW &&
                        my >= s_BtnY && my < s_BtnY + s_BtnH)
                    {
                        s_Visible = s_Visible ? 0 : 1;
                        s_DedupDirty = 1;
                        LogLine(s_Visible ? "sws: show" : "sws: hide");
                        /* 按下即吞 (这次点击止于侧栏, "已选中+按住"不让引擎就地释放)。
                         * return 前自己写回键态 (见隐藏分支)。 */
                        s_HoldSkip = 1;
                        s_PrevLB = lb;
                        s_PrevRB = rb;
                        SwsDedupSync();
                        return;
                    }
                    /* 隐藏重复按钮: 切换"防御页里的重复超武拿不拿掉",
                     * 真正搬/补在末尾 SwsDedupSync 里做。
                     * s_HidValid 保证没画出的帧不吃点击。按钮永远给反馈,
                     * 搬不动靠日志说话。 */
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
                else if (rb && !s_PrevRB)
                {
                    /* 展开态右键显隐箭头 = 切换分阵营/填充。按钮在栏区判定框
                     * 之上 (与上面左键同理), 须在此单独命中。 */
                    if (mx >= s_BtnX && mx < s_BtnX + s_BtnW &&
                        my >= s_BtnY && my < s_BtnY + s_BtnH)
                    {
                        SwsToggleFill();
                        s_HoldSkip = 1;      /* 吞掉, 别漏到地图变取消 */
                        s_PrevLB = lb;
                        s_PrevRB = rb;
                        SwsDedupSync();
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
                        /* 滚出可见范围的 (x=-1) 不参与命中 (否则屏幕左侧竖带
                         * 恒判中)。 */
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
                            /* 资金不足不可选中 (与原版一致: 压暗 + 不响应点击)。
                             * 即时类也拦 (点一下即发射, 引擎/Ares 也会拦)。 */
                            if (!SwsAfford(s_Items[k].money))
                            {
                                s_HoldSkip = 1;
                                LogLine("sws: clk nofunds");
                                break;
                            }
                            if (s_Items[k].instant)
                            {
                                /* Action==None 即时类: 走标准事件 SpecialPlace
                                 * (与 Phobos LaunchSuper 同式), 不写 CurrentSW
                                 * (本地 UI 状态, 引擎当帧不释放)。由引擎 DoList
                                 * 执行, 联机同步。按下即发, 不受"松开才上膛"影响。 */
                                if (SwsPushSpecialPlace(s_Items[k].swIdx))
                                {
                                    /* 事件已排队: 这一击仍止于侧栏。 */
                                    s_HoldSkip = 1;
                                    LogLine("sws: instant");
                                }
                                else
                                {
                                    /* OutList 满 (0x80): 本帧发不出, 照旧吞掉,
                                     * 别漏到地图变误下单。 */
                                    s_HoldSkip = 1;
                                    LogLine("sws: instant queued fail");
                                }
                            }
                            else
                            {
                                /* 手动打靶: 按下只记 idx, 松开才写 CurrentSW
                                 * (见 s_PendSel)。按住期由 s_HoldSkip 吞掉,
                                 * 等价于真 Gadget 消费掉这次完整点击。 */
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
                    /* 右键取消 ("无选中")。限在栏区里: 只按 s_BoundsKnown 判,
                     * 地图上右键取消单位会把超武选中状态清了。 */
                    if (mx >= s_BoundX && mx < s_BoundX + s_BoundW &&
                        my >= s_BoundY && my < s_BoundY + s_BoundH)
                    {
                        SwsArmSW(-1);
                        LogLine("sws: cancel");
                    }
                }
            }
        }
        /* 吞没只在"按下起始于栏内"期间有效: 松开即清, 否则之后点地图也被吞。 */
        if (!lb) s_HoldSkip = 0;
        s_PrevLB = lb;
        s_PrevRB = rb;
    }
    /* 扫描节流: 每 15 逻辑帧 (约 0.25s) 重扫一次; 点击时 s_ForceScan 立刻补一次。 */
    {
        static int s_ScanAge = 0;
        s_ScanAge++;
        if (s_ScanAge >= 15 || s_ForceScan)
        {
            s_ScanAge = 0;
            s_ForceScan = 0;
            /* 换剧本条目集全变 ⇒ 图标催画配额重给满。 */
            if (s_ItemN != s_LastItemN) { s_LastItemN = s_ItemN; s_NudgeLeft = SWS_NUDGE_MAX; }
            SwsScan();
            /* 图标还缺就催引擎重画防御页 (抓取钩只在那时拿得到像素)。
             * 须在 SwsDedupSync 之前 (无图标条目去重会留在页上等图标)。 */
            SwsNudgePaint();
            /* 新冒进防御页的 cameo 靠这一遍复查搬走。只在隐藏态标脏;
             * 显示态补货走 AddCameo 自带查重, 不随时间失效。 */
            if (s_DedupHide) s_DedupDirty = 1;
        }
        else if (s_CleanMode)
        {
            /* 防御页容器变了 (引擎新加 cameo / 去重搬过) 就提前重扫一轮,
             * 新到手的超武不等 15 帧节流。只读两 int, 每帧做无妨。 */
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

/* 鼠标压在哪个可见超武图标上。返回排序后的下标 (s_Order 里的位置), -1 = 无。
 * 下标须与高亮/图标循环同坐标系 (都走 s_Items[s_Order[i]]; 原序与排序后不是一回事)。
 * 用屏上这一帧的布局判 (x/y 由重建发布), 命中与画面永远一致。
 * 只在真正画了图标的地方挡: 栏区底框按 maxRows 画满, 空地还给引擎, 否则盖住的
 * 地图点不动。 */
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

extern "C" __declspec(dllexport) unsigned __cdecl SWS_ClickGate(void* regs)
{
    int mx = -1, my = -1;
    int r = 0;
    (void)regs;
    SwsEnsureInit();
    if (!s_Enable || !s_Visible) return 0;
    /* 当帧记下的按下止于侧栏 (跳过引擎 ProcessClickCoords, 不当打靶放置)。
     * 即时超武也吞 (靠 SpecialPlace 事件生效)。命中用屏上这一帧的图标矩形
     * (s_BoundsKnown = 缓存有可合成的一帧; 点击那帧的重扫只改数据不改布局)。 */
    if (s_HoldSkip) r = ADDR_CLICKGATE_SKIP;
    else if (!s_BoundsKnown) r = 0;
    else if (!GtMouseGameXY(&mx, &my)) r = 0;
    else if (SwsHitItem(mx, my) >= 0) r = ADDR_CLICKGATE_SKIP;
    return (unsigned)r;
}

/* 手动打靶类上膛: 照抄引擎侧栏 0x6AAF46..0x6AAF9D (Action != None 分支)。
 * swIdx >= 0 为选中, < 0 为取消。UnselectAll 是打得出去的前提: 有选中时
 * DecideAction (0x692610) 会提前返回, 到不了超武判据 0x692B06, 详见 notes。
 * 全是本地 UI 态, 与引擎侧栏同一条路, 联机安全。 */
static void SwsArmSW(int swIdx)
{
    char* disp = (char*)ADDR_DISPLAY;
    if (swIdx < 0)
    {
        *(int*)ADDR_CURSW = -1;
        return;
    }
    *(int*)(disp + OFF_DISP_CURBLD)      = 0;
    *(int*)(disp + OFF_DISP_CURBLDTYPE)  = 0;
    *(int*)(disp + OFF_DISP_CURBLDOWNER) = -1;
    FN_SET_FOUNDATION((void*)ADDR_DISPLAY, 0);
    FN_SET_REPAIR((void*)ADDR_DISPLAY, 0);
    FN_SET_SELL((void*)ADDR_DISPLAY, 0);
    *(unsigned char*)(disp + OFF_DISP_POWERMODE) = 0;
    *(unsigned char*)(disp + OFF_DISP_PLANNING)  = 0;
    *(unsigned char*)(disp + OFF_DISP_BEACON)    = 0;
    *(int*)ADDR_CURSW = swIdx;
    FN_UNSELECT_ALL();
}

/* ---------------- 即时超武: SpecialPlace 事件 (联机安全) ----------------
 * "点了就放"的不写 CurrentSW (本地 UI 态, 引擎当帧不释放), 照 Phobos LaunchSuper
 * 往 EventClass::OutList 塞标准事件 (SpecialPlace, cell=Empty), 引擎 DoList 执行,
 * 联机广播。OutList 追加复用 BuildQueue 已验证的那套 (0x4C6B60 ctor + 计数推进)。
 * 返回 1=已入队, 0=队列满。 */
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

    house = *(void**)ADDR_CURPLAYER;
    if (!house || !IsPlausiblePtr(house)) return 0;
    if (swIdx < 0 || swIdx > 0xFFFF) return 0;
    count = *(unsigned*)SWS_ADDR_OUT_COUNT;
    if (count >= SWS_EVT_CAP) return 0;          /* 满了下帧重试 */
    /* Next 先夹一次: 环形 Tail, 脏值不能直接当数组下标。 */
    next = *(unsigned*)SWS_ADDR_OUT_NEXT & (SWS_EVT_SLOTS_N - 1u);

    houseIdx = *(int*)((char*)house + SWS_OFF_HOUSE_INDEX);
    /* ctor 前 111 字节全清零: EventClass pack(1), union 尾部 ctor 不碰, 而
     * operator== 是 memcmp(...,111) —— 栈垃圾会埋联机同步雷。 */
    for (i = 0; i < SWS_EVT_SIZE; i++) ev[i] = 0;
    /* CellStruct::Empty (0,0): 与引擎点原版侧栏发出的逐字节一致。
     * "没有目标"正是传 Empty 的意思 (有类型收下 cell 根本不用)。 */
    {
        short cell[2] = { 0, 0 };
        FN_EVSPCTOR(ev, houseIdx, SWS_EVT_SPECIALPLACE, swIdx, cell);
    }

    for (i = 0; i < SWS_EVT_SIZE; i++)
        ((char*)(SWS_ADDR_OUT_BUF + next * SWS_EVT_SIZE))[i] = ev[i];

    /* 原版追加事件后必调的无参回调 (返回值存并行数组)。用普通函数指针调,
     * 不写内联汇编 (汇编 "call *%1" 声明的破坏集不含 ebx/esi/edi/ebp,
     * 编译器会误判寄存器仍有效)。 */
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
 * 合成面每渲染帧都被重画, 不能每帧从零画一遍。拆两层:
 *   重建 (SwsRebuild)   画进自有缓存, 15 逻辑帧一次 + 交互当帧;
 *   合成 (SwsCacheBlit) 每渲染帧一次带 alpha 拷贝, 内层无除法。
 * 缓存 = "16 位色 + 8 位覆盖率"两平面: 各层先互压 (底框→图标→边框→提示框),
 * 最后再与地图合成; 只存色会把半透明烙死, 换背景就错。
 *
 * 配套约束:
 *   - 绘制顺序固定: 底框 → 图标+文字 → 每列边框 → 高亮框 → 滚动条 → 按钮 → 提示框
 *     (边框盖图标, 高亮框盖列边框)。
 *   - 悬停提示挂图标下面、不跟鼠标走 (锚鼠标 x 则横移一像素就得重画整块)。 */

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

static void SwsInvalidateCache(void)
{
    s_CchOK = 0;
}

/* 8 位 RGB -> 目标位深的 16 位色 (与 Blend16 同一套移位/掩码) */
static unsigned short SwsPack(unsigned char r, unsigned char g, unsigned char b)
{
    return (unsigned short)(((unsigned)r * ((1u << g_RBits) - 1u) / 255u)
                                << (g_GBits == 6 ? 11 : 10) |
                            ((unsigned)g * ((1u << g_GBits) - 1u) / 255u) << 5 |
                            ((unsigned)b * ((1u << g_BBits) - 1u) / 255u));
}

/* 缓存内一次"源在上"合成: 源色 c, 源覆盖率 a8 (0~255)。
 * 源全覆盖与目标空两条快路无除法; 半透明压半透明才走除法 (只在重建时跑)。
 * 逐分量插、不跨分量借位。 */
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

/* 矩形填充进缓存。四边各夹一次, 内层不逐像素判越界。取屏幕坐标。 */
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
/* ---- PCX 逐像素贴图进缓存 ----
 * BSurface 布局 (gamemd 0x6B9D00 构造):
 *   +0x00 vptr / +0x04 Width / +0x08 Height / +0x0C LockLevel /
 *   +0x10 BytesPerPixel(=2) / +0x14 Buffer(WORD*, 行距=Width*2) /
 *   +0x18 Size(=W*H*2) / +0x1C Allocated;  sizeof 0x20
 * (0x88/0x8C/0x94/0x98 是虚表大小, 不是字段偏移。)
 * 无兜底: 校验不过就不画这个图标 (条目仍在)。不引回 PCX::BlitToSurface
 * (它只能进已锁合成面, 且 100% 不透明不认 Alpha)。 */
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
    /* 缩放映射表一次算好 dw 项 (下标相对 x0, 60 项小表; 逐像素现算太贵)。 */
    for (xx = x0; xx < x1; xx++)
        sxmap[xx - x0] = (unsigned short)(((xx - x) * pw) / dw);
    /* 悬停的图标画不透明 (与悬停提示框同规矩, "正在读"的不淡)。 */
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
    /* 占位面尺寸离谱 (1x1/0), 这几道校验挡它。 */
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

/* 覆盖率 blit 进缓存。src 1 字节/像素 (GDI DIB 4 字节/像素取第 2 字节)。
 * aScale 是调用方预乘, 这里不再吃 s_Alpha (覆盖率本身就是不透明度)。 */
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

/* 缓存 -> 已锁合成面: 每渲染帧唯一遍历, 唯一碰地图处。逐分量线性插值,
 * 除法用 "(v+128)>>8" 近似 (a=1 极低不透明度时补半个 LSB 舍入)。
 * a==0 像素整条跳过, 侧栏没画处零开销。
 * 两套坐标系: 缓存下标 x ↔ 画面列 s_CchX+x。写须加 s_CchX; 夹取须把目标列
 * 区间平移成缓存下标再与 [0,s_CchW) 取交、两头都夹 —— 只夹一头时整块在画面内
 * 分支不进, 内层多跑 s_CchX 次, 源/目标指针双双越界串行 (整块被平铺)。
 * (同层 CFill/CPcx/CCov 都是"先减原点再夹", 无此问题。) */
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

/* 尺寸变了才重分配。只清覆盖率平面 (覆盖率 0 的像素合成时跳过, 颜色平面旧值
 * 读不到, 省一半 memset)。 */
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
        /* 分配不成走 nogeo (s_CchOK=0), SWS_Draw 每帧会再试: 限流, 否则刷爆日志。 */
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
 * DIB 覆盖率拷进自有小缓冲, 之后只读它 (直接每帧 RenderLine 太贵:
 * 每次先 ClearDib 1024x64x4)。换名字/换字号才重光栅化。
 * 宽度取 DIB_W (RenderLine 行宽夹在 DIB_W-8, 缓冲永远装得下)。 */
#define SWS_TIP_COV_W 1024
#define SWS_TIP_COV_H 128
#define SWS_TIP_MAXLN 4      /* 名字按 \n 切行上限 */
/* 提示文本缓冲: UIName(<=23) + "\n$" + 花费(<=10 位) + 结尾。 */
#define SWS_TIP_TEXT_CAP 48
static struct {
    int fontPx, bold, sw, adv, ok;
    int nl;                        /* 行数 1..MAXLN */
    int wl[SWS_TIP_MAXLN];         /* 每行宽 (盒按最宽行, 短行左对齐) */
    wchar_t nm[SWS_TIP_TEXT_CAP];
    unsigned char cov[SWS_TIP_COV_W * SWS_TIP_COV_H];
} s_Tip = { 0 };

/* 备好悬停提示字形; 返回最宽行宽, 0 = 无提示/备不成。须在分配缓存之前调
 * (提示框可能探出侧栏块, 缓存矩形须把它算进去)。
 * 名字里的 \n 保留换行: 按行光栅化后上下叠放 (行高 adv, 短行左对齐)。
 * money = Ares Money.Amount: <0 时另起一行显示 "$花费", 与原版侧栏提示
 * (Ares `StripClass_GetTip_MoneySW` 用 TXT_MONEY_FORMAT_2 "%s $%d" 再换行) 同式。 */
static int SwsTipPrepare(const wchar_t* nm, int money)
{
    static wchar_t txt[SWS_TIP_TEXT_CAP];
    int gpx, adv, sw, sl = 0, k, yy, xx;
    int ls[SWS_TIP_MAXLN];   /* 每行起始下标 */
    int nl;
    if (!nm || !nm[0]) return 0;
    /* 拼"名字 + \n$花费" (花费是 -Money.Amount 的绝对值; 0/正不显示)。 */
    {
        int t = 0;
        while (nm[t] && t < SWS_TIP_TEXT_CAP - 14) { txt[t] = nm[t]; t++; }
        if (money < 0)
        {
            unsigned v = (unsigned)(-(long)money);
            wchar_t dig[12];
            int dn = 0, i;
            if (v == 0) dig[dn++] = L'0';
            while (v && dn < 11) { dig[dn++] = (wchar_t)(L'0' + v % 10); v /= 10; }
            txt[t++] = L'\n';
            txt[t++] = L'$';
            for (i = dn - 1; i >= 0; i--) txt[t++] = dig[i];
        }
        txt[t] = 0;
        nm = txt;
    }
    gpx = VecText_FontPx();
    if (gpx <= 0) gpx = 18;
    if (s_Tip.ok && gpx == s_Tip.fontPx && VecText_Bold() == s_Tip.bold &&
        FaceSame(nm, s_Tip.nm))
        return s_Tip.sw;
    s_Tip.ok = 0; s_Tip.sw = 0; s_Tip.nl = 0;
    s_Tip.fontPx = gpx; s_Tip.bold = VecText_Bold();
    SwsCopyName(s_Tip.nm, nm, SWS_TIP_TEXT_CAP);
    /* 提示框用 GDI 槽 6 (充能百分比用槽 5): 字号来源不同, 共用会让 GdiEnsure
     * 每次拆了重建。 */
    if (!GdiEnsure(gpx, s_Tip.bold, 6, VecText_FontFace())) return 0;
    adv = g_G->adv > 0 ? g_G->adv : gpx + 2;
    while (sl < SWS_TIP_TEXT_CAP - 1 && nm[sl]) sl++;
    /* 按 \n 切行 (空行留空行高); 行数超限整个不画。 */
    nl = 0;
    ls[nl++] = 0;
    for (k = 0; k < sl; k++)
    {
        if (nm[k] != L'\n') continue;
        if (nl >= SWS_TIP_MAXLN) return 0;
        ls[nl++] = k + 1;
    }
    if (nl * adv > SWS_TIP_COV_H) return 0;
    sw = 0;
    for (k = 0; k < nl; k++)
    {
        int s = ls[k], e = (k + 1 < nl) ? ls[k + 1] - 1 : sl;
        int lw = 0;
        while (e > s && nm[e - 1] == L'\r') e--;
        if (e > s)
        {
            lw = RenderLine(nm + s, e - s);
            for (yy = 0; yy < adv; yy++)
            {
                const unsigned char* srow = g_G->bits + (size_t)yy * g_G->pitch;
                unsigned char* drow =
                    s_Tip.cov + (size_t)(k * adv + yy) * SWS_TIP_COV_W;
                for (xx = 0; xx < lw + 2; xx++)
                    drow[xx] = srow[xx * 4 + 2];
            }
        }
        s_Tip.wl[k] = lw;
        if (lw > sw) sw = lw;
    }
    if (sw <= 0) return 0;   /* 全空行: 无内容可显示 */
    s_Tip.sw = sw; s_Tip.adv = adv; s_Tip.nl = nl; s_Tip.ok = 1;
    return sw;
}

/* 提示框矩形 (屏幕坐标)。ax = 图标水平中心锚点, ayTop = 图标下边,
 * panelRight = 侧栏块右缘 (xStart + totW)。
 * 默认挂图标下面居中; 居中盒会探出块左右缘时盒右缘贴块右缘
 * (右侧图标的长名否则会被裁掉半边)。底下放不下翻上去。
 * 名字含 \n 时多行叠放: 盒宽按最宽行, 盒高 = 行数×adv + 2。 */
static void SwsTipRect(int w, int h, int panelRight, int ax, int ayTop,
                       int* rx, int* ry, int* rw, int* rh)
{
    int sw = s_Tip.ok ? s_Tip.sw : 0, adv = s_Tip.adv, sx, ny, th;
    *rw = 0; *rh = 0;
    if (sw <= 0) return;
    th = s_Tip.nl * adv;
    sx = ax - sw / 2;
    ny = ayTop + 2;
    if (ny + th + 1 > h) ny = ayTop - th - 2;   /* 底下放不下就翻上去 */
    if (sx + sw + 4 > panelRight)
        sx = panelRight - sw - 4;   /* 盒右缘贴面板右缘 */
    if (sx < 4) sx = 4;
    /* 夹取按框算, 否则右边跑出屏幕。 */
    if (sx + sw + 6 > w - 2) sx = (w - sw - 8 > 4) ? w - sw - 8 : 4;
    *rx = sx - 2; *ry = ny - 1; *rw = sw + 6; *rh = th + 2;
}

/* 提示框: 半透明黑底 + 主题色边框 + 白字。不吃侧栏整体透明度。
 * 多行按 \n 上下叠放 (短行左对齐), 整块在盒内垂直居中。 */
static void SwsDrawTip(int w, int h, int panelRight, int ax, int ayTop)
{
    int bx, by, bw, bh, sx, k;
    if (!s_Tip.ok) return;
    SwsTipRect(w, h, panelRight, ax, ayTop, &bx, &by, &bw, &bh);
    if (bw <= 0) return;
    /* 提示框不吃整体 Alpha ("正在读"的信息须始终清晰), 直接用不带 s_Alpha 的 SwsCFill。 */
    SwsCFill(bx, by, bw, bh, 0, 0, 0, 150);
    SwsCFill(bx, by, bw, 1, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
    SwsCFill(bx, by + bh - 1, bw, 1, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
    SwsCFill(bx, by, 1, bh, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
    SwsCFill(bx + bw - 1, by, 1, bh, SWS_ACCENT_R, SWS_ACCENT_G, SWS_ACCENT_B, 200);
    sx = bx + 2;
    for (k = 0; k < s_Tip.nl; k++)
    {
        if (s_Tip.wl[k] <= 0) continue;   /* 空行留空 */
        SwsCCov(s_Tip.cov + (size_t)k * s_Tip.adv * SWS_TIP_COV_W,
                SWS_TIP_COV_W, 1, sx, by + 1 + k * s_Tip.adv,
                s_Tip.wl[k], s_Tip.adv, 255, 255, 255, 100);
    }
}

/* ---- 显隐按钮: 圆角底板 + 边框 + 实心三角 (三层由外到内)。
 * dir=0 ◀ (收起时, 点它展开); dir=1 ▶ (展开时, 点它收起)。
 * 悬停 hov 由调用方算好 (按钮在缓存里, 亮暗变化靠重建)。 */
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

/* ---- 隐藏重复按钮: 禁入牌 (方框 + 斜杠, 开着偏绿、灭掉偏灰, 悬停提亮)。
 * 斜杠只在隐藏中画, 灭掉是空框, 状态一眼可分。 */
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
    /* 斜杠只在隐藏中画 (灭掉是空框)。 */
    if (on)
        for (i = 0; i < s; i++)
            SwsCFillA(bx + i, by + i, 1, 1, c[0], c[1], c[2], hov ? 255 : 225);
}

/* ---- 两个按钮的公共几何 ----
 * 挂超武块上方 2px 一行, 右对齐块右缘, 从右往左 [显隐][空3][隐藏重复];
 * 块多让 18px 顶部空间, 右边不留位。
 * 只能挂块上方: SWS_Draw 拿到的是合成面 (宽 = 屏幕宽 − 原版侧栏宽),
 * 块右缘紧贴原版侧栏, 挂右边 x 会算到面外被丢掉。
 * 位置由重建期统一发布 (Poll 命中读这几个值); 两按钮都在栏区判定框之外
 * (块顶之上), 命中须排在栏区判定之前, 否则永远点不到。 */
#define SWS_BTN_W  20      /* 宽>高, 好点中 */
#define SWS_BTN_H  16
#define SWS_BTN_GAP 3

static void SwsTwoBtnGeom(int xStart, int totW, int w, int blockTop)
{
    int right = xStart + totW;
    s_BtnW = SWS_BTN_W;
    s_BtnH = SWS_BTN_H;
    /* 两按钮在栏区判定框之上, 判定排在栏区之前。跟面板换边镜像:
     * 在右贴块右缘 (靠屏幕外缘), 在左贴左缘。 */
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
    s_BtnXKeep = s_BtnX;   /* 收起沿用, 折叠/展开不跳位 */
    s_BtnYKeep = s_BtnY;
}

/* 侧栏收起: 只留一个显隐箭头, 位置与展开态完全一致 (沿用记下的坐标,
 * 玩家对着同一位置点回去; 别改成贴屏幕缘, 会差 5px 跳一下)。
 * 从没展开过 (s_BtnXKeep<0) 才退 fallback。 */
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
    /* 收起只有箭头, 贴外缘 (右贴右边, 左贴左边)。 */
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

/* 鼠标压在哪个按钮: 0=无 1=显隐 2=隐藏重复。亮暗在缓存里, 变了就重建。 */
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
 * 画面大变的输入都进去, 剩下交 15 帧节流。刻意不含 remain (充能中每轮都变,
 * 放进来等于永不作废; 倒计时跟节流走, 每秒 4 次够用)。不含 x/y (自己算的布局,
 * 不是输入)。悬停/按钮悬停/选中 (curSel) 也不进, SWS_Draw 单独比上次值
 * (移上去、点中、松开上膛都是当帧重画, 不等节流)。 */
static unsigned SwsSig(int w, int h, int pitch)
{
    unsigned s = 0x5A17C0D3u;
    int i;
    s = s * 33u + (unsigned)w;
    s = s * 33u + (unsigned)h;
    s = s * 33u + (unsigned)pitch;
    /* 锚点 (地图区右缘) 也进签名: F11 切侧栏显隐时右缘跳动而画布尺寸不变,
     * 不进则缓存不重建, 侧栏以旧坐标残留。 */
    s = s * 33u + (unsigned)g_MapRight;
    /* s_SideRight 也进 (切 Side 后列序/滚动条/命中全镜像, 缓存须失效)。 */
    s = s * 33u + (unsigned)s_SideRight;
    s = s * 33u + (unsigned)s_Visible;
    /* 显示模式进签名: 箭头右键切换后列布局/排序全变, 缓存须失效。 */
    s = s * 33u + (unsigned)s_FillMode;
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
    /* drainState 须进指纹 (火风暴激活/停用只改它, 不加则切换不重绘)。 */
    s = s * 31u + (unsigned)it->drainState;
    s = s * 31u + (unsigned)it->instant;
        s = s * 31u + (unsigned)(size_t)it->pcxFile;
    }
    return s;
}

/* 收起态重建: 只有一个显隐箭头 (◀=展开)。
 * 纵向沿用 TopMargin 规则但不减半个块高 (无块, 减了与展开位置对不上)。 */
static void SwsRebuildCollapsed(int w, int h, unsigned sig, int fr, int btnHov)
{
    int by;
    if (s_TopIsPct) by = (int)(((long)h * (long)s_TopMargin) / 100000L);
    else            by = s_TopMargin;
    if (by < 0) by = 0;
    /* 箭头贴外缘 (右=地图右缘, 左=0 起步)。贴边由 s_SideRight 定, 参数只提示一侧。 */
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
 * 引擎是排序插入 (`SidebarClass::AddCameo` → `StripClass_AddCameo`,
 * 比较器 `BuildType::SortsBefore` @0x6A8420): 超武 (ItemType 恒 0x1F) 只走
 * "RechargeTime 升序, 相等按 UIName 字典序升序" (冷却快的在前)。
 * 我们拿它做默认顺序; ini 给了自定义优先级的仍按自定义排前面。 */
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

/* ---- 重建期"整块不画"的旁路日志 (三条 goto nogeo 与扫描早退一样静默,
 * 原因变了才再记, 不每帧刷)。 */
static const char* s_DrawZeroTag = NULL;
static void SwsDrawZero(const char* tag, int v1, int v2)
{
    char buf[96];
    unsigned q;
    if (s_DrawZeroTag == tag) return;
    s_DrawZeroTag = tag;
    q = LOGP(buf, "sws: draw0 ");
    q = LOGS(buf, q, tag);
    q = LOGS(buf, q, " a=");
    q = LOGI(buf, q, v1);
    q = LOGS(buf, q, " b=");
    q = LOGI(buf, q, v2);
    LogLine(buf);
}

/* 展开态重建: 布局 + 全部画进缓存。 */
static void SwsRebuild(int w, int h, int mx, int my, int curSel,
                       unsigned sig, int fr, int btnHov)
{
    int n = s_ItemN;
    int colCount[4] = { 0, 0, 0, 0 };
    int colX[4] = { 0, 0, 0, 0 };
    int fillCols = 1;                       /* 填充模式用到的物理列数 (1~4) */
    int rows[4];
    int i, f, totW, xStart, yStart, rowH, maxRows, blockH;
    int bar, barX, barW, firstX, right, bottom, top, ch, hover = -1, txtOk, anchor;
    int tipX = 0, tipY = 0, tipW = 0, tipH = 0;
    /* 三条 goto nogeo 都是静默出口, 各记一行 (原因变了再记)。 */
    if (n <= 0 || n > SWS_CAP) { SwsDrawZero("nitems", n, s_ItemN); goto nogeo; }

    /* 1) 排序: 阵营 -> ini 自定义优先级 -> 原版顺序。
     *    自定义默认 1000 (=没写), 填正数 (<1000) 自然排前面, 不用额外分组。
     *    填充模式不看阵营: 只按 ini 优先级 -> 原版顺序, 让各列高度尽量均衡。
     *    s_Items 只有 SwsScan 会改, 顺序与帧无关, 条目数或显示模式变了才真排一次。 */
    if (s_OrderN != n || s_OrderFill != s_FillMode)
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
                if (!s_FillMode && a->side != b->side) gt = a->side > b->side;
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
        s_OrderFill = s_FillMode;
    }
    for (f = 0; f < 4; f++) colCount[f] = s_OrderColCount[f];
    s_SplitSide = -1;
    if (s_FillMode)
    {
        /* 填充模式: 不看阵营, 按容量定列数 (2~4), 行优先 (k%cols) 让各列高度
         * 尽量均衡 (与单阵营拆两列的观感一致)。1 列装得下时仍用 2 列 (n>=2),
         * 超 4 列容量才用满 4 列并滚动。s_OrderColCount (按阵营) 在此不用。 */
        int cols, kk;
        for (f = 0; f < 4; f++) colCount[f] = 0;
        cols = (n + s_MaxRows - 1) / s_MaxRows;   /* 容量所需列数 */
        if (n >= 2 && cols < 2) cols = 2;
        if (cols < 1) cols = 1;
        if (cols > 4) cols = 4;
        fillCols = cols;
        for (kk = 0; kk < n; kk++) colCount[kk % cols]++;
    }
    else
    {
        /* 只有一个阵营有超武时默认拆两列 (一列排到底太空)。两物理列共用边框色,
         * 条目按行奇偶分列, 每列 ceil(n/2) 条。只有 1 条不拆。 */
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
    /* 超过上限的列用滚动条。 */
    if (s_MaxRows > 0 && maxRows > s_MaxRows) maxRows = s_MaxRows;
    /* 夹紧滚动偏移: 最大偏移 = 最长列 − 可见行数 (不是 colCount−1, 那会滚到
     * 只剩最后一行、中间全空)。 */
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
    /* 有列装不下, 整块外侧 (靠屏幕边缘那侧, 随 Side 变) 让 5px 给滚动条。 */
    bar = (s_MaxSk > 0);
    barW = bar ? 4 : 0;
    /* 锚地图区边缘 (不是画布缘, 原版侧栏恒在右, 锚画布缘会被它盖住)。
     * FullMap 没接管 (g_Map*=0) 才退回画布缘。 */
    if (s_SideRight)
    {
        anchor = (g_MapRight > 0 && g_MapRight <= w) ? g_MapRight : w;
        xStart = anchor - s_RightMargin - totW - (bar ? 5 : 0);
        if (xStart < 0) xStart = 0;
    }
    /* 面板在左锚屏幕左缘 x=0 (原版侧栏恒在右, 正常视口左缘即屏幕左缘;
     * FullMap 态1/2 推开视口留给左面板的正是那条空档, 不用改 FullMap)。 */
    else
    {
        anchor = 0;
        xStart = anchor + s_RightMargin + (bar ? 5 : 0);
        if (xStart + totW > w) xStart = w - totW;
        if (xStart < 0) xStart = 0;
    }
    /* 比例 = 屏幕高×比例 − 半个块高 (视觉居中); 像素 = 直接当 Y。
     * 纵向多留 SWS_BTN_H+2 给块顶的按钮。 */
    if (s_TopIsPct)
        yStart = (int)(((long)h * (long)s_TopMargin) / 100000L) - blockH / 2;
    else
        yStart = s_TopMargin;
    if (yStart < SWS_BTN_H + 2) yStart = SWS_BTN_H + 2;
    /* 2) 各列的 x。空列跳过, 列连续排。列序见 kColOrder
     * (盟军靠屏幕外缘: 在右是反序, 在左是自然序)。 */
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
    /* 命中横向范围 = 最左列 ~ 最右列 (列序可能反的, 取极值不取循环末值)。 */
    s_ColMinX = 0x7FFFFFF;
    s_ColMaxX = -1;
    for (f = 0; f < 4; f++)
        if (colCount[f] > 0)
        {
            if (colX[f] < s_ColMinX) s_ColMinX = colX[f];
            if (colX[f] > s_ColMaxX) s_ColMaxX = colX[f];
        }
    if (s_ColMaxX < 0) { s_ColMinX = 0; s_ColMaxX = -1; }
    /* 3) 每条目屏幕位置; 滚出可见范围的 x=-1 (不命中/不绘制)。
     *    拆列时按"该阵营内序号"奇偶分列 (此时 colCount 已是两物理列,
     *    it->side 不再是列号); 填充模式按行优先下标 k%fillCols 分列。 */
    rows[0] = rows[1] = rows[2] = rows[3] = 0;
    {
        int k = 0;
        for (i = 0; i < n; i++)
        {
            SwsItem* it = &s_Items[s_Order[i]];
            int c;
            if (s_FillMode) c = k % fillCols;
            else c = (s_SplitSide >= 0) ? (k & 1) : it->side;
            if (c < 0) c = 0;
            int r = rows[c] - s_Scroll;
            it->x = colX[c];
            it->y = yStart + r * rowH;
            rows[c]++;
            if (r < 0 || r >= maxRows) it->x = -1;
            k++;
        }
    }
    /* 4) 两按钮 (栏区判定框之外, 命中排在栏区之前) */
    SwsTwoBtnGeom(xStart, totW, anchor, yStart);
    /* 滚动条贴外侧那列 (与 xStart 预留 5px 对应)。 */
    barX = s_SideRight ? (s_ColMaxX + SWS_ICON_W + 1) : (xStart - 5);
    /* 面板在左滚动条在块左边: 并进矩形左缘 (提示框左夹取同理), 否则被裁掉。 */
    if (bar && barX < firstX) firstX = barX;
    /* 5) 悬停 (用刚落好的布局) + 提示框几何。提示框可能探出块外, 先量出它,
     * 缓存矩形四条边才够大。 */
    hover = SwsHitItem(mx, my);
    if (hover >= 0)
    {
        /* hover 是排序后下标, 经 s_Order 取 (见 SwsHitItem)。 */
        SwsItem* it = &s_Items[s_Order[hover]];
        if (SwsTipPrepare(it->name, it->money))
            SwsTipRect(w, h, xStart + totW, it->x + SWS_ICON_W / 2,
                       it->y + SWS_ICON_H, &tipX, &tipY, &tipW, &tipH);
    }
    /* 6) 缓存矩形 = 按钮行 ∪ 超武块 ∪ 滚动条 ∪ 提示框。 */
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
    /* 高亮描边落在 it->x-1/it->y-1, 末行下边正好是 yStart+blockH —— 块外,
     * 左/下各让 1px, 否则外圈少一像素。 */
    if (firstX > 0) firstX--;
    if (bottom < h) bottom++;
    /* 提示框翻到图标上方时框顶同样在块外, 一并算进 top。 */
    top = s_BtnY;
    if (tipW > 0 && tipY < top) top = tipY;
    if (!SwsCacheAlloc(right - firstX, bottom - top))
    { SwsDrawZero("cache", right - firstX, bottom - top); goto nogeo; }
    s_CchX = firstX; s_CchY = top;
    /* 7) 发布几何: 点击/滚轮/吞没门都读这几个, 对应屏上这一帧。 */
    s_BoundX = xStart; s_BoundY = yStart; s_BoundW = totW; s_BoundH = blockH;
    s_BoundsKnown = 1;
    s_YStart = yStart; s_RowH = rowH; s_VisRows = maxRows;

    /* ---- 整块底框 (一次, 不是每列一次: 视觉一样, 少填充少重叠边)。 ---- */
    ch = 0;
    for (f = 0; f < 4; f++)
        if (colCount[f] > 0)
        {
            int t = (colCount[f] < maxRows ? colCount[f] : maxRows) * rowH - s_Gap;
            if (t > ch) ch = t;
        }
    if (ch > 0)
        SwsCFillA(xStart, yStart, totW, ch, 0, 0, 0, SWS_BACKDROP_OP);

    /* 充能百分比走 GDI 槽 5 (字号 [SWSidebar] FontPx)。须显式 GdiEnsure:
     * 直接拿 g_G 会借到上一个用 GDI 的模块的槽, 字号随别的模块变。 */
    txtOk = GdiEnsure(s_FontPx, VecText_Bold(), 5, VecText_FontFace());

    /* ---- 图标 + 充能百分比 (高亮框在下面单独一遍) ---- */
    for (i = 0; i < n; i++)
    {
        SwsItem* it = &s_Items[s_Order[i]];
        int isHover = (i == hover);
        int drawn = 0;
        if (it->x < 0) continue;              /* 被滚动裁掉 */
        /* 图标来源 (都拿不到则空着, 条目/边框/百分比照旧):
         *   1) 引擎抓到的 (优先, 战役改写/地图内嵌声明的一律对);
         *   2) SidebarPCX= 自己读 (基础规则 ini, 改写过的可能是旧值)。
         * 两条都空 = 空格子。 */
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
            /* 未满格 (充能中 / 火风暴运作中) 压暗, 悬停的不压 (看它时让提示框
             * 和白框说话)。判据与下面百分比门控一致 (`remain>0 || !ready`,
             * 不单用 !ready: 火风暴的 IsReady 激活时不清, 单判永远不压暗)。
             * 就绪但资金不足同样压暗 (与引擎/Ares `StripClass_Draw_SuperDarken`
             * 一致: Ready && !CanTransactMoney)。 */
            if ((it->remain > 0 || !it->ready || !SwsAfford(it->money)) && !isHover)
                SwsCFillA(it->x, it->y, SWS_ICON_W, SWS_ICON_H, 0, 0, 0, 45);
        }
        {
            /* 图标顶部文字, 最多两行:
             *   行1: `Hold` (暂停) 或充能百分比; 行2: `Run` (仅火风暴运作中)。
             * 两行各调一次 RenderLine (内部 ClearDib+重画, 不跨调用留状态)。
             * 行2 只对火风暴出现, 行1 位置/语义不变。 */
            if (txtOk)
            {
                wchar_t l1[8], l2[8];
                int li;
                /* 两行清满 (不清满尾部是栈垃圾, 非零时 RenderLine 会画出来)。 */
                l1[0] = l1[1] = l1[2] = l1[3] = 0;
                l1[4] = l1[5] = l1[6] = l1[7] = 0;
                l2[0] = l2[1] = l2[2] = l2[3] = 0;
                l2[4] = l2[5] = l2[6] = l2[7] = 0;
                /* 就绪 (100%) 不显示字 (图标本身即"可用", 叠字多一次渲染还压图标)。 */
                if (it->susp)
                {
                    l1[0] = 'H'; l1[1] = 'o'; l1[2] = 'l'; l1[3] = 'd';
                }
                else if (it->total > 0 && (it->remain > 0 || !it->ready))
                {
                    int pct;
                    /* 门控看计时器在不在跑 (`remain>0`), 不看 IsReady (火风暴激活
                     * 走 ClickFire→Launch, 不清 IsReady, 老门控会挡掉它的上升百分比)。
                     * 充能中→上升百分比; 就绪→不显示; 运作中→下降百分比 + 行2 `Run`。
                     * 附带 `|| !it->ready` 防回归 (IsReady=0 而计时器没跑时照旧显示)。 */
                    /* 火风暴运作中百分比倒着走 (燃料表), 分母换维持时长 drainTotal
                     * (满充能×ratio)。原版侧栏同样 100% 起跳往下走, 不跟则从 ratio
                     * 跳起、与原版相反。 */
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
                    if (pct > 99) pct = 99;      /* 满格即就绪, 由上面分支处理 */
                    /* 按位数输出 (1% 不写成 "001%")。 */
                    if (pct >= 10)
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
                /* 行2: 火风暴"运作中"。用英文 `Run` (图标 60px, 汉字溢出/糊;
                 * 读不到 CSF, 取与 `Hold` 同宽的 3 字母)。 */
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
                    if (ny + adv > it->y + SWS_ICON_H) continue;  /* 越界不画 */
                    SwsCCov(g_G->bits, g_G->pitch, 4, sx + 1, ny + 1, sw, adv,
                            0, 0, 0, 80 * s_Alpha / 100);
                    SwsCCov(g_G->bits, g_G->pitch, 4, sx, ny, sw, adv,
                            255, 255, 255, 100 * s_Alpha / 100);
                }
            }
        }
    }

    /* ---- 每列边框 (画在图标之后, 否则被 60x48 图标盖住)。 ---- */
    for (f = 0; f < 4; f++)
    {
        int ch2;
        const unsigned char* c;
        if (colCount[f] <= 0) continue;
        /* 拆列时物理列 0/1 同一阵营, 边框色跟阵营走 (否则两列变两色)。
         * 填充模式一列内混阵营, 边框统一用中性灰 (阵营色无意义)。 */
        c = kSideCol[s_FillMode ? 0 : (s_SplitSide >= 0 ? s_SplitSide : f)];
        if (s_FillMode) c = kFillCol;
        ch2 = (colCount[f] < maxRows ? colCount[f] : maxRows) * rowH - s_Gap;
        SwsCFillA(colX[f], yStart, SWS_ICON_W, 1, c[0], c[1], c[2], 150);
        SwsCFillA(colX[f], yStart + ch2 - 1, SWS_ICON_W, 1, c[0], c[1], c[2], 150);
        SwsCFillA(colX[f], yStart, 1, ch2, c[0], c[1], c[2], 150);
        SwsCFillA(colX[f] + SWS_ICON_W - 1, yStart, 1, ch2, c[0], c[1], c[2], 150);
    }

    /* ---- 条目高亮框: 悬停=白, 当前选中=阵营色 ----
     * 须排在每列边框之后、独立一遍: 高亮框左右边压在邻列外边框上 (ColGap=0 时
     * 本列右边框 x+59, 下一列左边框 x+60), 先画会被盖掉一截。放最后整圈才完整。 */
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

    /* ---- 滚动条 (参照战役框): 整块右侧 4px, 轨道 55 / 滑块 90, 压暗不抢眼。
     * 高度盖整个块, 滑块高度按最长列条目数算。 */
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
            /* 滑块位置与 scroll/maxSk 同向 (scroll=0 在上, =maxSk 在底部)。 */
            thumbY = (s_MaxSk > 0)
                   ? barY + (barH - thumbH) * s_Scroll / s_MaxSk
                   : barY;
            SwsCFillA(barX, barY, barW, barH, 30, 30, 30, 55);
            SwsCFillA(barX, thumbY, barW, thumbH, 170, 170, 170, 90);
        }
    }

    /* ---- 显隐按钮 ▶ (收起) + 隐藏重复按钮 (左边)。箭头任何时候都画,
     * 否则收起后找不到展开处。 */
    SwsDrawArrow(s_BtnX, s_BtnY, s_BtnW, s_BtnH, 1, btnHov == 1);
    SwsDrawDedup(s_HidX, s_HidY, s_BtnW, s_BtnH, s_DedupHide, btnHov == 2);

    /* ---- 悬停名字 (底框 + 全局矢量字体, 挂图标下面)。隐藏重复按钮不出文字
     * (牌面+颜色已说明状态, 自拼中文字面量有风险)。 ---- */
    if (hover >= 0)
    {
        SwsItem* it = &s_Items[s_Order[hover]];
        SwsDrawTip(w, h, xStart + totW, it->x + SWS_ICON_W / 2,
                   it->y + SWS_ICON_H);
    }

    s_CchOK = 1;
    s_CchSig = sig; s_CchFrame = fr;
    s_CchHover = hover; s_CchBtn = btnHov; s_CchSel = curSel;
    s_DrawZeroTag = NULL;   /* 画成了, 下次失败重记 */
    return;

nogeo:
    /* 整块不画, 两按钮也没画: s_HidValid 一起清 (否则 Poll 拿旧几何误判点击)。
     * 指纹/帧号照记 (退回 15 帧节流, 不每帧重试/刷日志)。 */
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
    /* 位深是 SwsPack/SwsOver 的前提 (565/555), 先定 (不依赖 Hook 调用顺序)。 */
    if (!g_Calibrated) Calibrate();
    GtMouseGameXY(&mx, &my);
    fr = *(int*)ADDR_FRAME;
    /* 悬停用屏上这一帧的布局算 (上次重建发布的): 移到新图标上立刻触发重建,
     * 不等节流。 */
    btnHov = SwsHitBtn(mx, my);
    if (s_Visible && s_CchOK)
    {
        hover = SwsHitItem(mx, my);
        if (IsPlausiblePtr((void*)ADDR_CURSW)) curSel = *(int*)ADDR_CURSW;
    }
    sig = SwsSig(w, h, pitch);
    age = fr - s_CchFrame;
    if (fr < s_CchFrame) age = SWS_REBUILD_FRAMES;   /* 帧号回拨 (新开局/读档) */
    if (!s_CchOK || sig != s_CchSig || hover != s_CchHover ||
        btnHov != s_CchBtn || curSel != s_CchSel ||
        age >= SWS_REBUILD_FRAMES)
    {
        if (s_Visible) SwsRebuild(w, h, mx, my, curSel, sig, fr, btnHov);
        else           SwsRebuildCollapsed(w, h, sig, fr, btnHov);
    }
    if (!s_CchOK) return;
    /* 每帧唯一一次锁面 + 带 alpha 拷贝。 */
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