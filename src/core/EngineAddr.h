/* EngineAddr.h - gamemd 引擎地址与结构偏移的统一定义。
 * 新增地址集中在此, 注明函数、指令或 YRpp 成员出处, 便于核验引擎兼容性。
 * 本文件只含宏, 不依赖其它头。 */
#ifndef MOE_ENGINEADDR_H
#define MOE_ENGINEADDR_H

/* ==================== 时序 / 场景 ==================== */

/* Unsorted::CurrentFrame - 逻辑帧号, 每帧 +1; 倒退 = 新开局或读档。 */
#define ADDR_FRAME              0xA8ED84u
#define ADDR_FRAME_NO           0xA8ED84u   /* 旧名 (FrameCap/MOExperience 用过), 同址 */

/* ScenarioClass::Instance - 换图代号取它的首字段。 */
#define ADDR_SCENARIO           0xA8B230u

/* Unsorted::ScenarioStarted - 只有真正进对局才为 1 (大厅/载入为 0)。 */
#define ADDR_SCEN_STARTED       0xA8ED5Cu

/* Game::SpecialDialog (int) - 非 0 = 有模态对话框/ESC 菜单; 引擎此时不派发命令热键。 */
#define ADDR_SPECIAL_DIALOG     0xA8EDA0u

/* SessionClass::Instance - 首成员即 GameMode (0=战役/菜单 3=LAN 4=Internet 5=Skirmish)。 */
#define ADDR_SESSION            0xA8B238u
#define ADDR_GAMEMODE           0xA8B238u   /* 旧名, 同址 */

/* GameModeOptionsClass::Instance.MultiEngineer (0xA8B250 + 0x1C, YRpp
 * Unsorted::MultiEngineer): 遭遇战设置里的"多位工程师"勾选。Ares 拿它决定
 * 工程师占敌楼是先损伤还是直接占领 (见 OU_EngDamageOn)。 */
#define ADDR_MULTI_ENGINEER     0xA8B26Cu

/* GameModeOptionsClass::Instance.BuildOffAlly (0xA8B250 + 0x14, YRpp
 * GameModeOptionsClass.h): 建造范围扫描 0x4A8FFA 判它决定要不要把盟友建造场
 * 算进基地范围。引擎只在遭遇战存/读整块 GameModeOptions, 战役读档后该值不
 * 恢复, 由 BuildAlly_Poll 补回 (见 BuildAlly.cpp)。 */
#define ADDR_BUILD_OFF_ALLY     0xA8B264u

/* Game::IsActive (bool, YRpp Unsorted.h)。 */
#define ADDR_ISACTIVE           0xA8E9A0u

/* 载入进度屏相位: 1/2/3 = 正在载入对局。 */
#define ADDR_LOAD_PHASE         0xA9FAB0u

/* PlanningModeActive - Z 路径点 / 规划令处于激活态 (ClickedMission 内 0x637AA0 读它)。 */
#define ADDR_PLANMODE           0xAC4CF4u

/* ==================== 玩家 / 房屋 ==================== */

/* 本机 HouseClass* (PlayerPtr)。 */
#define ADDR_CURPLAYER          0xA83D4Cu

/* HouseClass::Array - 容器本体 + 首成员布局 (items / count 两个跟随地址)。 */
#define ADDR_HOUSE_ARR          0xA80228u
#define ADDR_HOUSES_ITEMS       0xA8022Cu
#define ADDR_HOUSES_COUNT       0xA80238u

/* 对象类型数组 (DynamicVectorClass<T*>), 取自 YRpp 对应类的 DEFINE_REFERENCE。 */
#define ADDR_BLD_ARR            0xA8EB40u   /* BuildingClass::Array */
#define ADDR_INF_ARR            0xA83DE8u   /* InfantryClass::Array */
#define ADDR_UNIT_ARR           0x8B4108u   /* UnitClass::Array */
#define ADDR_AIRCRAFT_ARR       0xA8E390u   /* AircraftClass::Array */

/* HouseClass 偏移。 */
#define OFF_HOUSE_INDEX         0x30u     /* HouseClass::ArrayIndex */
#define OFF_HOUSE_TYPE          0x34u     /* HouseClass::Type -> HouseTypeClass* */
#define OFF_IS_HUMAN            0x1ECu    /* HouseClass::IsHumanPlayer */
/* HouseClass::Money: 0x705DF3 (Available_Money) 与 0x4F9950 (GiveMoney)
 * 均通过 [ecx+0x30C] 访问。 */
#define OFF_HOUSE_MONEY         0x30Cu

/* HouseClass::PlanningPaths[12] - WaypointPathClass* 数组。
 * 0x5023B0 GetPlanningWaypointAt 按 CellStruct 遍历它, 空槽 new(0x40) 现建。
 * 崩溃取证用: 整表没初始化 与 只有一格被写坏 是两种完全不同的病因。 */
#define OFF_HOUSE_PLANPATHS     0x210u
#define HOUSE_PLANPATH_N        12

/* HouseTypeClass::MultiplayPassive - 中立方为 1。 */
#define OFF_HTYPE_PASSIVE       0x1A6u

/* ==================== 对象 (Object/Techno/Building) ==================== */

#define OFF_HEALTH              0x6Cu     /* ObjectClass::Health */
#define OFF_INLIMBO             0x81u     /* ObjectClass::InLimbo (在工厂/运输内) */
#define OFF_ISALIVE             0x90u     /* ObjectClass::IsAlive */
#define OFF_NEXT_OBJ            0x30u     /* ObjectClass::NextObject */
#define OFF_OWNER               0x21Cu    /* TechnoClass::Owner -> HouseClass* */
#define OFF_BLD_TYPE            0x520u    /* BuildingClass::Type */
#define OFF_BLD_REPAIRING       0x6E8u    /* "正在被修" 标志 (IsBeingRepaired)。
                                           * UnitClass 与 BuildingClass 共用这个偏移:
                                           * 载具 0x447017 置 1, 修满 0x4508D0/
                                           * 0x4509B4 清 0; 建筑 0x43B98A 写。 */
#define OFF_TYPE_STRENGTH       0xA0u     /* ObjectTypeClass::Strength
                                           * (0x5F5C42 自己的 `fild [eax+0xA0]`) */
#define OFF_TECH_TOKEN          0x514u    /* TechnoClass 规划令牌 (0x705D20 实证) */

/* 动态向量 (DynamicVectorClass) 通用布局。 */
#define OFF_VEC_ITEMS           4u
#define OFF_VEC_COUNT           16u
/* DynamicVectorClass<T*> 的 items/count 取值样板 (各调用点共用, 防漂移)。 */
#define ARR_ITEMS(base)         (*(void***)((base) + OFF_VEC_ITEMS))
#define ARR_COUNT(base)         (*(int*)((base) + OFF_VEC_COUNT))

/* 静态类型数组 (DynamicVector of ObjectTypeClass*)。判定"某 Type 指针是否合法"
 * 只比指针, 不解引用。 */
#define ADDR_TYPARRAY           0xA8EB00u   /* Unit/Aircraft/Infantry 类型 */
#define ADDR_TERRAINTYPES       0xA8E318u   /* TerrainType (树/矿等) */

/* ==================== 选中集 ==================== */

/* ObjectClass::CurrentObjects (DynamicVectorClass<ObjectClass*>, YRpp ObjectClass.h)。
 * 实证: MapClass::UnselectAll 0x48DC90 读 [0xA8ECC8] 当个数、[0xA8ECBC] 当数组,
 * 逐个 call [vtbl+0x150] (ObjectClass::Deselect)。 */
#define ADDR_CUR_OBJECTS        0x00A8ECB8u   /* 容器本体 (items=+4, count=+0x10) */

/* AbstractClass::WhatAmI 虚表槽 (0x692590 `mov eax,[ecx]; call [eax+0x2c]` 后
 * `cmp eax,6` = BuildingClass 实证)。返回值是 AbstractType, 取值常量用
 * OrderUtil.h 的 ABS_* 一套。 */
#define OFF_VT_WHATAMI          0x2Cu

/* TechnoClass::GreatestThreat 虚表槽 (基类 0x6F8DF0, FootClass 覆写 0x4D9920)。
 * 签名 (ThreatType, CoordStruct* 原点, bool 只打本方敌人) -> 目标或 NULL。
 * 引擎自己的自动索敌就是它, THREAT_RANGE 是"只在本单位射程内找"。只读查询。 */
#define OFF_VT_GREATEST_THREAT  0x3C4u

/* TechnoClass::SetTarget(AbstractClass*) 虚表槽 (0x6FCDB0, 收尾 `ret 4`)。
 * 只写目标槽 (+0x2B4) 并清掉飞行中子弹对旧目标的引用, 不动
 * Mission/Destination/Path。换索敌目标用它, 不要重发 Mission=Move ——
 * 那会反复重置寻路, 单位迈不开步。 */
#define OFF_VT_SETTARGET        0x3C8u

/* TechnoClass::GetWeapon(int) 虚表槽: 返回 WeaponStruct*, 首成员 WeaponTypeClass*。 */
#define OFF_VT_GETWEAPON        0x3F8u

/* 具体类的类型指针成员 (YRpp 布局换算, BuildingClass 那项 0x520 与
 * OFF_BLD_TYPE 同值 ⇒ 其余同类可信)。 */
#define OFF_UNIT_TYPE           0x6C4u
#define OFF_AIRCRAFT_TYPE       0x6C4u
#define OFF_INFANTRY_TYPE       0x6C0u

/* WeaponTypeClass 成员, 单位 leptons。0x6F7248 `mov edi,[ebx+0xB4]` 取出后叠加
 * 命中/地形加成当射程用; +0xB8 是 MinimumRange (0x6F737F 判非 0)。 */
#define OFF_WT_RANGE            0xB4u

/* TechnoClass 成员。Veterancy 是 float: 0~1 = 新兵升老兵进度, 1~2 = 老兵升精英,
 * <0 = 不吃经验 (YRpp VeterancyStruct::IsNegative)。Ammo 是当前弹药。 */
#define OFF_TECH_VETERANCY      0x150u
/* 箱子加成的倍率 (double), 紧跟在 Veterancy + align 之后。
 * 实证 = 箱子拾取分支自己: 0x482E71 `fmul qword [ecx+0x158]` / 0x482E79 存回,
 * 前一条 0x482E61 `cmp dword [ecx+0x15C],0x3FF00000` (= 现值是不是 1.0);
 * 火力同形在 0x483240 / 0x483248 (偏移 +0x160 / 高半 +0x164)。
 * **升级给的倍率不在这里** —— 引擎用 HasAbility 现算 (见 FN_TECH_HASABILITY),
 * 这两个字段只有箱子 (与 Ares AttachEffect) 会写。 */
#define OFF_TECH_ARMOR_MULT     0x158u
#define OFF_TECH_FIREPOWER_MULT 0x160u
/* FootClass::SpeedMultiplier (double): 速度箱子分支 0x483060
 * `fmul qword [edi+0x580]` → 0x48306C 存回, 同一处 0x483057 `cmp eax,2` 跳过飞机。
 * 0x578 是 SpeedPercentage (YRpp FootClass.h `unused_574` 往后数)。 */
#define OFF_FOOT_SPEED_MULT     0x580u
/* TechnoClass::Cloakable (bool): 隐形箱子分支 0x48294F `mov byte [edx+0x3D2],1`。
 * 类型的 `Cloakable=` 在建对象时也会置它, 所以"箱子给的隐形"要再看类型键
 * (ini 走 ReadBool, 别猜 TechnoTypeClass 偏移)。 */
#define OFF_TECH_CLOAKABLE      0x3D2u

/* TechnoClass::CloakState (int, YRpp CloakState 枚举): 0=Uncloaked 1=Cloaking
 * 2=Cloaked 3=Uncloaking。紧跟在 Owner(0x21C) 之后; 写入点 0x454E85
 * `mov dword [esi+0x220],2` = Cloak() 落到完全隐形态。 */
#define OFF_TECH_CLOAKSTATE     0x220u
#define CLOAKSTATE_CLOAKED      2

/* TechnoClass 上的"鼠标命中豁免"标记 (bool, 语义未定)。全 exe 仅 7 处引用,
 * 都是 `mov al,[x+0x41A]; test al,al; jne 放行` —— 置位时跳过隐形可见性检查
 * (0x6DA3FF 像素命中 / 0x69252D 点击后处理)。照抄引擎, 不猜含义。 */
#define OFF_TECH_HITEXEMPT      0x41Au

/* 引擎原生自愈: 虚函数 vtable+0x294 = ShouldSelfHealOneStep, 调用点 0x6FA743
 * (`mov eax,[esi]; call [eax+0x294]`, 前面 0x6FA735 先判 [esi+0x90]=IsAlive)。
 * Ares 把这个虚函数整个换掉 (Ares 源码 Hooks.cpp DEFINE_HOOK 0x6FA743)。
 *
 * SelfHealing (TechnoTypeClass, bool): 0x70BE8D
 *   `call [eax+0x84]` 取类型 → `mov cl,[eax+0xD14]; test cl,cl` 非零即"能自愈",
 *   就是这条坐实了偏移。ini 键同名, 但直接读内存比再解析一遍更省也更快。
 * Organic (TechnoTypeClass, bool): 0x71503F `mov [ebp+0xD97], al`, 键名前一条
 *   0x71502B `push 0x843714`("Organic") → 0x5295F0 ReadBool。只有生物载具为真,
 *   它决定"载具按步兵口径还是载具口径回血"。 */
#define OFF_TYPE_SELFHEAL       0xD14u
#define OFF_TYPE_ORGANIC        0xD97u

/* HouseClass 上的合计 (int, 累加值不是布尔): 面板只读这两个数就算完了。
 * 0x446392 `mov eax,[ebp+0x21C]`(建筑的 owner House) + `add [eax+0x164], ecx`
 * (ecx = 类型 +0x1564), 0x4463B4 同形写 +0x168 (类型 +0x1568); 拆楼/卖掉在
 * 0x4459AE / 0x448AE8 里 `sub` 并 clamp 到 0。Phobos
 * `TechnoExt::ApplyGainedSelfHeal` 判"有没有这个光环"读的也是这两处。 */
#define OFF_HOUSE_INFSELFHEAL   0x164u
#define OFF_HOUSE_UNITSELFHEAL  0x168u

/* TechnoClass::HasAbility(Ability) (0x70D0D0, __thiscall, 只读, 返回 AL)。
 * 0x70D0D5 `lea edi,[esi+0x150]` 取 Veterancy, 0x74FF90/0x750010 判老兵/精英;
 * 类型取虚表 +0x84, 能力位 = [type+0x29C+ability] (老兵) / [type+0x2AE+ability]
 * (精英)。只读查询, 联机可用。ability 取值见 YRpp GeneralDefinitions.h
 * `enum class Ability` (Faster=0 Stronger=1 Firepower=2 ROF=4 Sight=5
 * Cloak=6 SelfHeal=9), 这里只定义面板用到的。 */
#define FN_TECH_HASABILITY      0x0070D0D0u
#define ABIL_FASTER             0
#define ABIL_STRONGER           1
#define ABIL_FIREPOWER          2
#define ABIL_SELFHEAL           9
/* TechnoClass::Tiberium (StorageClass: 4 个 float, 每种矿石装了多少 bails)。
 * 0x73E412 `lea ecx,[esi+0x33C]` + call StorageClass::GetAmount 实证。
 * GetTotalValue (0x6C9600, __thiscall ecx=&Tiberium, 返回 int) 是引擎自己的
 * 折算: 逐种乘 TiberiumClass::Array[i]->Value(+0xB8) 累加, 即车上矿石的钱
 * (不含精炼厂 OrePurifier 加成)。只读, 联机可用。 */
#define OFF_TECH_TIBERIUM       0x33Cu
#define FN_STORAGE_TOTALVALUE   0x006C9600u
/* UnitTypeClass::Harvester (bool): `Harvester=` 的解析结果, 0x7476B9
 * `mov [edi+0xE0E], al`; 紧邻 +0xE0F 是 `Weeder=` (0x7476CD)。
 * **只有 UnitTypeClass 有这一字节**, 别的类型不要读。 */
#define OFF_UNITTYPE_HARVESTER  0xE0Eu

#define OFF_TECH_AMMO           0x2FCu
/* TechnoClass::Target: 当前攻击目标 (TarCom)。SetTarget (0x6FCDB0) 开头读旧值
 * 比较、末尾写入 0x2B4, 双向实证。 */
#define OFF_TECH_TARGET         0x2B4u
/* PassengersClass { int NumPassengers; FootClass* FirstPassenger; }, 链走
 * ObjectClass::NextObject (+0x30)。 */
#define OFF_TECH_PASSNUM        0x114u
#define OFF_TECH_PASSFIRST      0x118u
/* FootClass::Transporter: 载员指回载具, 载具自己为 NULL (运行时实证:
 * 车下载员此处 == 载具指针, 载具自己为 0)。 */
#define OFF_TECH_TRANSPORT      0x11Cu
/* FootClass::Container: 驻军建筑 / 坦克掩体用的"容器"字段 (0x6EA500 attach
 * 写 0x5D4, 0x457DE0 放人时据此断开)。车下载员恒为 0, 不要拿它判载具归属。 */
#define OFF_TECH_CONTAINER      0x5D4u
/* BuildingClass::Occupants: DynamicVectorClass<InfantryClass*> 本体 0x684,
 * Items 0x688, Count 0x694 (GetOccupantCount 0x4581F0 直接返回 [ecx+0x694];
 * 0x4585C0 KillOccupants 读 Items/Count, 0x522941 AddOccupant 用
 * lea [ebp+0x684] 实证)。驻军不走 Passengers 链, 是另一套存储。 */
#define OFF_BLD_OCC_ITEMS       0x688u
#define OFF_BLD_OCC_COUNT       0x694u

/* HouseClass::IsAlliedWith(HouseClass const*) 0x4F9A50, __thiscall 只读,
 * 返回值只在 AL (函数指针须声明为 unsigned char)。同盟含自己。 */
#define FN_HOUSE_ISALLIED       0x004F9A50u

/* MissionClass::ForceMission(Mission) 0x5B2FD0, __thiscall 单参:
 * 立刻把 CurrentMission 换掉并清 QueuedMission/状态计时 (不改任何"任务结果")。
 * 任务处理器想中途结束自己时用它, 例: 吸收型建筑卸载完一个人要回到 Guard。 */
#define FN_MISSION_FORCE        0x005B2FD0u

/* 类型的名字字段: AbstractTypeClass::ID (0x24, char[0x18]) 与
 * ObjectTypeClass::ImageFile (0x1F8, char[0x19])。art ini 的节名用 ImageFile,
 * 空时落 ID (与引擎查 art 的顺序一致)。 */
#define OFF_TYPE_ID             0x24u
#define OFF_TYPE_IMAGEFILE      0x1F8u
/* 类型的显示名 (wchar_t*) —— 引擎装载时把 UIName 的 CSF 键解析好存在这里:
 * 0x410960 `push 0x7f / push <AbsType.cpp> / xor edx,edx / call 0x734E60`
 * (StringTable::LoadString) 之后 `mov [esi+0x60], eax`; 没有 UIName 的类型
 * 落到常量 0x887734。键本身是 `type+0x3D` 的 char[0x20] (0x410AF2 处写入)。
 * 只读缓存, 不要自己调 CSF —— 那函数用 ecx/edx 传参 + 两个栈参 (文件名/行号)。 */
#define OFF_TYPE_UINAME         0x60u

/* CCINIClass::INI_Art (对象本体, 0x887180) 与 ReadString
 * (0x528A10, __thiscall: section, key, def, buf, size; 返回写入长度)。
 * MO 的单位图标走 Ares 的 art 键 CameoPCX=xxx.pcx, 引擎自己的 Cameo= 字段是空的,
 * 所以只能问 art ini 要文件名。
 *
 * ⚠ INIClass 内部按**指针**缓存上一次查的节: `obj+4` = 节名字符串指针,
 * `obj+8` = 该节数据。0x528A4D 处是 `cmp 节名指针, [obj+4]` —— 指针相同就直接用
 * 缓存, 连字符串都不比。所以 section 必须传**持久的、每个节不同地址**的字符串
 * (类型对象里的 ImageFile/ID 正合适); 传栈上局部数组会让所有后续查询全部命中
 * 第一次的缓存, 表现为"所有单位图标都是同一个"。 */
#define ADDR_INI_ART            0x00887180u
#define FN_INI_READSTRING       0x00528A10u

/* CCINIClass::INI_Rules: 0x887048 里存的是 CCINIClass* (规则 ini 本体),
 * 换图/重开/读档会换指针 —— 按它派生的缓存必须比指针作废。
 * ReadBool (0x5295F0, __thiscall: section, key, def) 返回值只有 AL 有效
 * (尾部是 `mov al, [def]`), 必须按 1 字节接, 别声明成 int。
 * 节名同样受上面那条指针缓存约束: 传类型对象里的 ID, 不要传栈上数组。
 * 用途: Ares 的 TechnoType 键 `NoManualUnload=` (靠载员实现特殊功能的单位)。 */
#define ADDR_INI_RULES          0x00887048u
#define FN_INI_READBOOL         0x005295F0u

/* TechnoTypeClass 成员 (图标 SHP 与弹药上限)。CameoFile 是 `Cameo=` 写的文件名
 * (0x19 字节, 紧挨在 Cameo 指针前); MO 的图标多为 .pcx, 此时 Cameo 指向的是
 * XXICON.SHP 占位图, 必须按 CameoFile 走 PCX 才拿得到真图。 */
#define OFF_TT_CAMEOFILE        0x6D6u
#define OFF_TT_CAMEO            0x6F0u
#define OFF_TT_AMMO             0x684u
/* rules 里的 `PipWrap=` (一排显示几个弹药格) 与 `PipScale=`。
 * 偏移由 YRpp 同版本布局算出 (Ammo=0x684 与上面对得上, 可信):
 * PipScale=0x3D4 (枚举 dword), PipWrap=0x3E4 (int)。 */
#define OFF_TT_PIPWRAP          0x3E4u

/* ==================== 选中 / 取消选中 (只动本地 UI 选择集) ==================== */

/* MapClass::UnselectAll (0x48DC90, __fastcall 无实参): 逐个 Deselect + 清界面。 */
#define FN_MAP_UNSELECTALL      0x0048DC90u
/* TechnoClass::Select (0x6FBFA0, __thiscall, 返回 bool): 内部过 CanBeSelectedNow
 * (0x5F4520) 与 0x637840 (加进 CurrentObjects)。 */
#define FN_TECHNO_SELECT        0x006FBFA0u

/* ==================== 规划令牌 (PlanningToken) ==================== */

#define OFF_TOK_NODES           8u        /* PlanningNodes 数组 */
#define OFF_TOK_NCOUNT          0x14u     /* 节点数 */

/* ==================== 侧栏 (Strip) ==================== */

#define OFF_STRIP_TOPROW        0x44u
#define OFF_STRIP_CAMCOUNT      0x54u
#define ADDR_SB_REDRAW          0x00B0B518u   /* 侧栏脏标记 */
#define ADDR_SB_NEEDBG          0x00884B8Fu   /* this+0x53A7 页签底图重画 */
#define ADDR_SIDEBAR_W          0x007F5BF8u   /* 侧栏列宽 (168) */

/* ==================== 画面 surface / 视口 ==================== */

#define ADDR_SURF_PRIMARY       0x00887308u
#define ADDR_SURF_SIDEBAR       0x00887300u
#define ADDR_SURF_MAP           0x008872FCu   /* 渲染时与 Composite 互换的持久地图面 */
#define ADDR_SURF_COMPOSITE     0x0088731Cu
#define ADDR_SURF_ALT           0x00887310u   /* 备用面 (崩溃诊断的面快照读它) */
#define ADDR_VIEWBOUNDS         0x00886FA0u   /* ViewBounds (缩放档 = R) */
#define ADDR_TACTICAL           0x00887324u   /* TacticalClass::Instance (指针) */
#define ADDR_WWMOUSE            0x00887640u
/* TacticalClass::SetTacticalPosition(CoordStruct const*) (__thiscall)。
 * 只改本机摄像机: 不写模拟、不发事件, 雷达点行/收藏跳转共用。 */
#define ADDR_SET_TACTICAL_POS   0x006D6070u

/* 格 → 屏幕换算链。MapClass::Instance 是 0x87F7E8 处的对象本身 (与 DisplayClass 同址),
 * 不是指针变量, 直接当 this 传。 */
#define ADDR_MAP_INSTANCE       0x0087F7E8u
#define FN_MAP_GET_CELL         0x005657A0u   /* CellClass* GetCellAt(CellStruct*) */
#define FN_CELL_GET_COORDS      0x00480A30u   /* CoordStruct* GetCellCoords(CoordStruct*) */
#define FN_Z_TO_SCREEN          0x006D20E0u   /* int ZToScreen(int z) */

/* 雷达点击门 (点击是否已被 UI 消费)。 */
#define ADDR_CLICKGATE_SKIP     0x006925FCu

/* ==================== 覆盖物 / 格子 (OverlayClass / CellClass) ==================== */

/* OverlayTypeClass::Array (DynamicVectorClass<OverlayTypeClass*>)。 */
#define ADDR_OVLTYPES_ITEMS     0x00A83D84u
#define ADDR_OVLTYPES_COUNT     0x00A83D90u   /* 0x5FD9BB 遍历用 */

/* OverlayTypeClass::Crate (bool) —— 0x481A31 `mov bl,[edx+0x2aa]`;
 * 相邻 0x2A9=Wall, 0x2AB=CrateTrigger (0x5FD9F1 / 0x481A70)。 */
#define OFF_OVLTYPE_CRATE       0x02AAu

/* CellClass::OverlayTypeIndex (int, -1 = 无覆盖物): 0x481A1C `mov eax,[esi+0x44]`。
 * CellClass::OverlayData (uchar, 箱子的类型字节): 0x481AC8 `mov bl,[esi+0x11e]`,
 * 0x5FD105 放箱子时写 0xFF (= 未定型), 0x56BFF9 由 PlacePowerupCrate 写实际类型。 */
#define OFF_CELL_OVERLAY        0x0044u
#define OFF_CELL_OVDATA         0x011Eu

/* CellClass::AltFlags: bit1 = 格内有建筑 (幽灵地基占位也用) / bit3 = 已揭示。
 * 揭示时 CellClass::Unshroud 0x4876F6 `or al,0x18` 置 bit3|bit4; 重新压黑时
 * MapClass::Reshroud 0x577B48 `and ecx,0xffffffe7` 一起清掉。ShroudClass::GetLevel
 * 0x6D88F4 判黑幕入口就是 `test byte [cell+0x12c],0x18` 再 `test al,8`。 */
#define OFF_CELL_ALTFLAGS       0x012Cu
#define CELL_ALT_BUILDING       0x0002u
#define CELL_SHROUD_REVEALED    0x0008u

/* CellClass 的"隐形探测覆盖"计数: SensorsOfHouses 从 +0x7C 起, 每玩家一个
 * WORD (隐形探测器 SensorsSight 单位在场时 ++/--)。+0xAC 是 DisguiseSensors
 * 同形数组, 两者差 0x30 = 24 个玩家 x 2 字节; 0x486800 DrawObjectsCloaked
 * 同时读 +0x78(CloakedByHouses 位) 与 +0x7C, 三处互证。
 * 索引 = HouseClass::ArrayIndex (OFF_HOUSE_INDEX), 只有前 24 个玩家有效。 */
#define OFF_CELL_SENSORS        0x007Cu
#define CELL_SENSORS_MAXHOUSE   24

/* CellClass::Sensors_InclHouse(int houseIdx) __thiscall, 返回 AL:
 * 实现就是 `[cell+0x7C+idx*2] > 0` (0x4870D0)。只读, 联机可调。 */
#define FN_CELL_SENSORS         0x004870D0u

/* DisplayClass::GetCellAtCoords(CoordStruct const*) __thiscall -> CellClass*:
 * 坐标 >> 8 得格, 查 Display+0x13C 的格表; 越界回占位格 0xABDC50 (恒非空)。
 * 0x692540 / 0x6DA41B 两处隐形可见性判定都用它。 */
#define FN_GETCELLATCOORDS      0x00565730u

/* TechnoTypeClass 上由 INI 键 InvisibleInGame 派生的标记 (bool, type+0xC9A):
 * 解析点 0x460DF1..0x460E09 (`ReadBool("InvisibleInGame")` 为真即置 1)。
 * 引擎取到点击对象后 0x69256B 读它, 非 0 直接清目标 —— 这类对象点不到。 */
#define OFF_TYPE_INVISIBLE      0x0C9Au

/* ==================== 动画类型 (天气 SHp 来源) ==================== */

/* AnimTypeClass::Array (DynamicVectorClass<AnimTypeClass*>)。
 * 结构: +0x00 虚表 / +0x04 数据指针 (元素数组) / +0x10 个数。
 * 实证: gamemd 0x428BB3 读 [0x8B4154] 取第 i 个指针, 0x428BBC
 * `add ecx,0x24` 后与传入 ID 做 _strcmpi 比较; 0x428BA8 读 [0x8B4160] 是个数。 */
#define ADDR_ANIMTYPE_ARR       0x008B4150u
#define ADDR_ANIMTYPE_COUNT     0x008B4160u

/* AnimTypeClass 成员偏移:
 *   +0x24 = char ID[0x18]  (AbstractTypeClass::ID)
 *   +0xA4 = SHPStruct* Image, +0xA8 = bool ImageAllocated (ObjectTypeClass)
 *   +0x1F8 = char ImageFile[0x19] (ObjectTypeClass) —— 有 Image= 时用这个
 * 实证: gamemd 0x4279A8..0x4279E1 反复读写 [esi+0xA4]/[esi+0xA8],
 * 且 0x5F70C7 清零 +0xA4、0x5F70CD 清零字节 +0xA8。 */
#define OFF_ANIMTYPE_ID         0x24u

/* 动画播放参数 (int, YRpp AnimTypeClass 成员顺序: Rate -> Start -> LoopStart ->
 * LoopEnd)。注意 +0x2B0 里存的**不是** artmo.ini 写的那个 Rate: 活进程实测
 * (tools/animdump.c) RAIN/SNOW 在这里都是 2, 而两者 art 里写的是 Rate=400 ——
 * 引擎读 ini 时就换算成了"每动画帧要过几个逻辑帧" (= 900/Rate 截断, 见 ModEnc
 * Rate), 所以这个字段可以直接当间隔用, 别再除一次。
 * 循环段: RAIN 有 31 帧 / SNOW 20 帧, 而两者 LoopEnd=19, 必须按类型字段截断。
 * 实证: gamemd 0x42477B-0x4247B1 用 [type+0x2B8]/[type+0x2BC] 复位动画帧,
 * 0x4246FC 用 LoopEnd-Start 判循环。 */
#define OFF_ANIMTYPE_RATE       0x2B0u
#define OFF_ANIMTYPE_LOOPSTART  0x2B8u
#define OFF_ANIMTYPE_LOOPEND    0x2BCu

/* 天气动画用的调色板 (ConvertClass*): 引擎画动画用的那一个。
 * YRpp FileSystem.h: ANIM_PAL = 0x87F6C0 (ConvertClass*)。 */
#define ADDR_ANIM_PAL            0x0087F6C0u

/* 引擎 SHp 绘制入口 CC_Draw_Shape (gamemd 0x4AED70)。
 * __fastcall: ecx=Surface, edx=ConvertClass*; 栈参顺序见 YRpp Surface.h
 * 的 DSurface::DrawSHP。返回 0x38 (ret 0x38)。 */
#define FN_DRAW_SHAPE            0x004AED70u

/* ==================== 引擎 SHp 绘制 / cameo ==================== */

/* FileSystem::CAMEO_PAL (ConvertClass*), 侧栏 cameo 用的调色板。 */
#define ADDR_CAMEO_PAL          0x0087F6B0u
/* PCX::Instance 的取面/装图 (SWSBar/PcxImage 同一条路):
 * GetSurface(pcx, name, pal) / ForceLoadFile(pcx, name, 2, 0)。 */
#define FN_PCX_GETSURF          0x006BA140u
#define FN_PCX_LOAD             0x006B9D00u
/* gamemd BSurface 虚表: 渲染 cameo 到自有 16 位缓冲时拼假面用 (只用到 GetRect)。 */
#define ADDR_BSHAPE_VTBL        0x007E2070u
/* DSurface::DrawSHP 的 cameo 调用参数 (与侧栏同口径): BlitterFlags 0x400,
 * Brightness 1000。 */
#define CAMEO_BLIT_FLAGS        0x400
#define CAMEO_BRIGHTNESS        0x3E8

/* ==================== 聊天 / 消息列表 ==================== */

#define ADDR_MSGLIST            0xA8BC60u   /* MessageListClass::Instance */
#define OFF_MSGLIST_EDIT        0x19u       /* MessageListClass::IsEdit (聊天框输入态) */

/* ==================== 战术渲染 (TacticalClass) ==================== */

/* TacticalClass::Render(this, surf, flag, mode): mode 0 准备 / 1 地形 / 2 覆盖+物件。 */
#define ADDR_TAC_RENDER         0x006D3D10u
/* TacticalClass::MakeSelection(this, filter) 与原版框选物件过滤回调。 */
#define ADDR_TAC_MAKESEL        0x006D9FF0u
#define ADDR_TAC_SELFILTER      0x004AC2B0u
/* TacticalClass::RegisterDirtyArea(this, RECT*, bool): 矩形推进脏区表,
 * 0x56BE73 实证。侧栏外挂面/玩家色/比例尺重画共用。 */
#define ADDR_REG_DIRTY_AREA     0x006D2790u

/* TacticalClass 成员偏移 (YRpp TacticalClass.h 换算 + 0x6D6070 / 0x6D9FF0 反汇编实证)。 */
#define OFF_TAC_TACX            0xB0u    /* 视口原点 X */
#define OFF_TAC_TACY            0xB4u
#define OFF_TAC_LSTX            0xB8u    /* 上次视口原点 X (位移=0 => 走整幅重画) */
#define OFF_TAC_LSTY            0xBCu
#define OFF_TAC_VISCNT          0xE0u    /* 可见单元数 */
#define OFF_TAC_VISARR          0xE4u    /* 可见单元表 (4B/项) */
#define OFF_TAC_CURX            0xD64u   /* 相机中心 X */
#define OFF_TAC_CURY            0xD68u
#define OFF_TAC_PRX             0xD6Cu
#define OFF_TAC_PRY             0xD70u
#define OFF_TAC_TGX             0xD74u
#define OFF_TAC_TGY             0xD78u
#define OFF_TAC_F7C             0xD7Cu   /* 缩放档相关标记 (各 1 字节, 按 int 读写会互相覆盖) */
#define OFF_TAC_F7D             0xD7Du
#define OFF_TAC_BAND_X1         0xD90u   /* 框选带左上 */
#define OFF_TAC_BAND_Y1         0xD94u
#define OFF_TAC_BAND_X2         0xD98u   /* 框选带右下 */
#define OFF_TAC_BAND_Y2         0xD9Cu
#define OFF_TAC_SELCNT          0xDB0u   /* 可选物件表项数 */

/* ViewBounds 的缓存副本 (0x6D5F60 抄出来): 4 个**连续** int = X, Y, W, H。
 * 战术渲染实际读这四个, 改 ViewBounds 不改缓存 = 两边不一致。 */
#define ADDR_VIEWCACHE          0x00B0CE28u
/* 战术渲染的 Z 缓冲 / 属性缓冲 (setDims 按 ViewBounds 重建)。 */
#define ADDR_TAC_ZBUF           0x00887644u
#define ADDR_TAC_ABUF           0x0087E8A4u
/* 脏区表项数 (表指针 0xB0CE7C, 20B/项)。渲染只会在 count>0 时删末项,
 * 所以"提前把 count 清 0"= 一项都不动。 */
#define ADDR_DIRTY_COUNT        0x00B0CE88u
/* Unsorted::TacticalSelectables (12B/项) = 物件屏坐标表。
 * 容量引擎原版 500, 由 SelPoolCap.cpp 整表搬到堆上 4096; 表基址在运行时
 * 会变, **只有 SelPoolCap 能碰这张表**, 别处不要再直接读写 0xB0CEC8。 */
#define ADDR_TACTICAL_SELARR    0x00B0CEC8u
/* 引擎"本帧整幅重画"标志 (GScreenClass::Render 的 mode 0 趟消费它)。 */
#define ADDR_FULLREDRAW         0x00B0E63Cu

/* PCX 实例 (改色盘用)。 */
#define ADDR_PCX_INSTANCE       0xAC4848u

/* ==================== 光照 (ScenarioClass) ==================== */

/* ScenarioClass 里的光照字段偏移。地图 ini 的 [Lighting] 段读进来就放这儿
 * (gamemd 0x68A7xx 那段读入代码): Ambient 与 tint = ini 值 ×100,
 * Ground / Level = ini 值 ×1000 (常量 0x7E2AC0=100.0, 0x7E4658=1000.0)。
 * 菜单会改 Ambient / R/G/B / Ground / Level 六项 (Ion / Dominator 那几组不碰)。 */
#define OFF_SCEN_AMBIENT_ORIG  0x3528u   /* AmbientOriginal (ini 的 Ambient) */
#define OFF_SCEN_AMBIENT_CUR   0x352Cu   /* AmbientCurrent */
#define OFF_SCEN_AMBIENT_TGT   0x3530u   /* AmbientTarget */
#define OFF_SCEN_TINT_R        0x3534u   /* NormalLighting.Tint.R (ini 的 Red) */
#define OFF_SCEN_TINT_G        0x3538u   /* NormalLighting.Tint.G (Green) */
#define OFF_SCEN_TINT_B        0x353Cu   /* NormalLighting.Tint.B (Blue) */
/* 地形层级亮度修正: 每层 +Level、0 平面 -Ground, 只在建 convert 时用
 * (CellClass::CalculateLightSourceLighting 0x484180 尾部), 详见 notes/光照预设.md 2.1。 */
#define OFF_SCEN_GROUND        0x3540u   /* NormalLighting.Ground (ini 的 Ground) */
#define OFF_SCEN_LEVEL         0x3544u   /* NormalLighting.Level  (ini 的 Level) */

/* ScenarioClass::Name (wchar_t[0x2D], 地图 [Basic] Name, 如 "Tournament Desert")。
 * 0x686DD0 实证: 以场景文件路径 (FileName, +0x125C) 构造 CCINIClass, 再用
 * 0x528F00 把 ini 里的 Name 读进 `[scen+0x1360]`。读档时这个 ini 是存档内嵌的
 * 地图, 所以读档后它仍是原图名 —— 判"是不是同一张图"用这个。
 * 注意 FileName (+0x125C) 读档后是存档名 (.sav), 比不出图。
 * 可能为空 (地图没写 Name), 调用方需自行判断。只读。 */
#define OFF_SCEN_NAME          0x1360u

/* ScenarioClass::UIName (char[0x20], 地图 [Basic] UIName)。多数遭遇战图不填,
 * 只在战役任务里用。Name 为空时可以当一个较弱的备选。只读。 */
#define OFF_SCEN_UINAME        0x13BAu

/* ScenarioClass::RecalcLighting(R, G, B, tint) —— __fastcall: ecx=R, edx=G,
 * B 与 tint 走栈且由被调方自己回收 (ret 8)。它内部会重算所有格子的光照
 * 并让地图面重画, 所以改完字段调它一个就够。
 * tint 分量的单位是"真实值 ×1000", 上限 1000 (=1.0), 引擎内部会夹紧。 */
#define FN_RECALC_LIGHTING     0x0053AD00u

/* ==================== 跟随 (Follow) ==================== */

/* DisplayClass 的跟随状态 (对象本身就是 0x87F7E8, 与 MapClass/MouseClass 同址)。
 * YRpp DisplayClass.h 的 FollowObject / ObjectToFollow:
 *   0x4AEB10 getter = `mov al,[ecx+0x119c]; test; mov eax,[ecx+0x11a0]`
 *   0x4AEB30 setter = `mov eax,[esp+4]; test; mov [ecx+0x11a0],eax; setne al;
 *                      mov [ecx+0x119c],al; ret 4` */
#define FN_DISP_SETFOLLOW       0x004AEB30u  /* __thiscall(self, obj), ret 4 */

/* CommandClass "Follow" 的 Execute (F 键, KeyboardMD.ini 的 Follow=70)。
 * 0x537A10 = `a1 c8 ec a8 00` (mov eax,[0xA8ECC8], 5 字节), 收尾 `ret 4`。 */
#define ADDR_CMD_FOLLOW         0x00537A10u

/* ObjectClass::Location (CoordStruct, leptons)。引擎跟随每帧取坐标就在这里:
 * 0x55B6CF `add eax,0x9c` 之后连读 3 个 dword (X/Y/Z)。 */
#define OFF_OBJ_LOCATION        0x009Cu

/* ==================== 玩家配色 (ColorScheme) ==================== */

/* HouseClass::ColorSchemeIndex → ColorScheme::Array[idx]。Array 是
 * DynamicVectorClass<ColorScheme*> @ 0xB054D0 (Items +4 / Count +0x10)。
 * Colors[256] 每项 3 字节 (RGB), MainShadeIndex 选主色 —— ESC 玩家色色块、
 * 单位颜色、信标箭头用的是同一套。 */
#define OFF_HOUSE_COLORSCHEME   0x16054u
#define ADDR_SCHEME_ITEMS       0x00B054D4u
#define ADDR_SCHEME_COUNT       0x00B054E0u
#define OFF_SCHEME_COLORS       0x04u     /* unsigned char Colors[256][3] */
#define OFF_SCHEME_ID           0x304u    /* char* Name */
#define OFF_SCHEME_BASE         0x308u    /* BaseColor (HSV 三字节) */
#define OFF_SCHEME_LIGHT        0x30Cu    /* LightConvertClass* */
#define OFF_SCHEME_SHADES       0x310u    /* 色阶数, 正常配色 53 */
#define OFF_SCHEME_MAIN         0x330u    /* MainShadeIndex */

/* ==================== 通信信标 (BeaconManagerClass) ==================== */

/* BeaconManagerClass::Instance (YRpp BeaconManagerClass.h, sizeof 0x80)。
 * Beacons[8][3] 在 +0x00，AllocatedCount 在 +0x60。
 * 布署模式字节 = DisplayClass+0x11B4，即绝对地址 0x88099C
 * (0x87F7E8+0x11B4)；SWSBar.cpp 的 OFF_DISP_BEACON 是同址的旧名。 */
#define ADDR_BEACON_MGR         0x0089C3B0u
#define OFF_BC_PERIOD           0x7Cu     /* int RadarBeaconAnimPeriod = 雷达帧数*4, 三处 idiv 的除数 */

/* 美术: LoadArt 0x4309D0 把两个 SHP 整个读进内存, 只取头部宽高帧数并填
 * +0x64/68/6C (BeaconSize+帧数) 与 +0x70/74/78/7C (RadarBeaconSize+帧数+周期);
 * 读不进来就留 NULL, +0x7C 停在 0。 */
#define ADDR_BC_ART             0x0089C474u  /* PBEACON.SHP 头指针 */
#define ADDR_BC_RADARART        0x0089C478u  /* RDRBEACN.SHP 头指针 */
#define ADDR_BC_LOADART         0x004309D0u  /* __thiscall(self=0x89C3B0) */

/* BeaconClass (sizeof 0x114): Coord +0x00 / Bitfield +0x0C (1=已放置, 2=本地选中)
 * / Text[128] +0x0E / HouseID +0x110。
 * VisibleToPlayer (0x4308B0, __thiscall, 纯读): 已放置 + 与本家同盟 + 该房可见。 */
#define OFF_BC_BITFIELD         0x0Cu
#define OFF_BC_HOUSEID          0x110u
#define FN_BC_VISIBLE           0x004308B0u

/* 单机入口/尾段与组包拦截 (前两处是 `mov eax,[0xA8B238]` 起的 GameMode 判定, 第三处偷
 * `mov ecx,[0xA8DA78]` 跳过组包/广播)。0x430CD4 联机也要挂 (记槽序号),
 * 另两处只在单机。见 notes/通信信标.md。 */
#define ADDR_BC_HOTKEY          0x005370A0u  /* Execute 入口, 5B mov eax,[sess] */
#define ADDR_BC_HOTKEY_GO       0x005370AEu  /* 跳过判定, call 0x731A30 */
#define ADDR_BC_HOTKEY_NEXT     0x005370A5u  /* 原指令之后 */
#define ADDR_BC_PLACETAIL       0x00430CD4u  /* PlaceBeacon 尾, 5B 同上 */
#define ADDR_BC_PLACETAIL_GO    0x00430CEAu  /* EVA + 操作提示段 */
#define ADDR_BC_PLACETAIL_NEXT  0x00430CD9u
#define ADDR_BC_SENDBUILD       0x00430D99u  /* 组 IPX 包起点, 6B mov ecx,[0xA8DA78] */
#define ADDR_BC_SENDBUILD_SKIP  0x00430E50u  /* 共享收尾 (pop edi/esi/ebp/ebx) */
#define ADDR_BC_SENDBUILD_NEXT  0x00430D9Fu

/* 满了自动替换最旧一槽 (联机容量 3, 单机容量由池化决定)。两处判满:
 *   0x430F4A  CanPlaceBeacon 的"满"返回 (xor al,al; ret 4, 共 5 字节), 唯一调用方是
 *             SetPlaceBeaconMode 0x4AC9AD —— 返回 0 时它静默退出, B 键毫无反应。
 *   0x430BD4  PlaceBeacon 的"满且本地 → 放弃"判定 (8 字节 cmp [esp+0x1F4],-1),
 *             紧跟 je 0x430F20。此地址只由扫槽循环的出口顺序到达, 无跳转指向。
 *             满时把挑中的槽号写回 [esp+0x1F4] 再跳 0x430BE2 分配对象; 分配完
 *             0x430C33 读到的 id 已不是 -1, 于是走 0x430E5D 那段"按 id 覆盖指定
 *             槽": 释放该槽旧信标、AllocatedCount--, 再装新的。随后组包广播的
 *             包里 id 就是这一槽 —— 对端收到同一个包走的正是这段代码, 所以
 *             联机的本地与对端跑同一段引擎逻辑, 不用额外的删除包。
 * 不要改用 DeleteBeacon 0x4311C0 单槽删除: 它在 0x431355 判 [esp+0x13],
 * 只有 (-1,-1) 的"删除选中"才为 1, 单槽删除根本不广播, 联机会各算各的。 */
#define ADDR_BC_CANFULL         0x00430F4Au
#define ADDR_BC_FULLCHK         0x00430BD4u
#define ADDR_BC_FULLCHK_NEXT    0x00430BE2u  /* 越过 `je 0x430F20` 之后: 分配对象 */
#define ADDR_BC_FULLCHK_GIVEUP  0x00430F20u  /* 原版早退 (pop 4 个寄存器后 ret 0x14) */

/* ---------------- 单机信标数量上限 (BeaconCap) ----------------
 * 使用原有 24 格作为单机池, 对象 HouseID 保持真实归属。
 * Z 为分段寻址替换, L/H 为按池查询; 线性 8*3 遍历无需修改。
 * DeleteAllBeacons 入口包装按 HouseID 过滤, 只调用定向 DeleteBeacon。 */
#define ADDR_BC_CAP_Z1          0x00430BB1u  /* PlaceBeacon: lea eax,[ebx+ebx*2] */
#define ADDR_BC_CAP_Z2          0x00430C64u  /* PlaceBeacon 写槽: lea ecx,[eax+ebx*2] */
#define ADDR_BC_CAP_Z3          0x00430C67u  /* 同上: mov edx,ebx */
#define ADDR_BC_CAP_Z4          0x00430E5Du  /* 按 id 覆盖段: lea eax,[eax+ebx*2] */
#define ADDR_BC_CAP_Z5          0x00430E60u  /* 同上: mov ecx,ebx */
#define ADDR_BC_CAP_Z6          0x00430F36u  /* CanPlaceBeacon: lea edx,[edx+edx*2] */
#define ADDR_BC_CAP_Z9          0x00431221u  /* DeleteBeacon: lea ecx,[ecx+eax*2] */
#define ADDR_BC_CAP_Z10         0x004312FAu  /* DeleteBeacon: lea edx,[ecx+ecx*2] */
#define ADDR_BC_CAP_Z11         0x0043131Bu  /* DeleteBeacon 清槽: lea/mov/add 共 7B */
#define ADDR_BC_CAP_Z13         0x004314B7u  /* EditBeaconMessage 定向寻址 */
#define ADDR_BC_CAP_Z14         0x004315C5u  /* EditBeaconMessage 选中寻址 */

#define ADDR_BC_CAP_L1          0x00430BCFu  /* PlaceBeacon 扫槽 cmp ecx,3 */
#define ADDR_BC_CAP_L2          0x00430C49u  /* PlaceBeacon 再扫 cmp eax,3 */
#define ADDR_BC_CAP_L3          0x00430F45u  /* CanPlaceBeacon cmp eax,3 */
#define ADDR_BC_CAP_L4          0x00431004u  /* SelectBeacon cmp esi,3 */
#define ADDR_BC_CAP_L5          0x00431124u  /* DeleteBeacon cmp esi,3 */
#define ADDR_BC_CAP_L6          0x004311A5u  /* DeleteBeacon cmp edx,3 */
#define ADDR_BC_CAP_L7          0x0043120Du  /* DeleteBeacon cmp edx,3 */
#define ADDR_BC_CAP_L8          0x00431309u  /* DeleteBeacon cmp ebx,3 */
#define ADDR_BC_CAP_L10         0x004314A3u  /* EditBeaconMessage 扫描 cmp edx,3 */
#define ADDR_BC_CAP_L11         0x004315D9u  /* EditBeaconMessage 扫描 cmp eax,3 */
#define ADDR_BC_CAP_L12         0x00431886u  /* 逐个绘制 cmp ebp,3 */

#define ADDR_BC_CAP_H4          0x00431015u  /* SelectBeacon 外层 cmp eax,8 */
#define ADDR_BC_CAP_H6          0x00431135u  /* DeleteBeacon 外层 cmp eax,8 */
#define ADDR_BC_CAP_H7          0x004311AEu  /* DeleteBeacon 外层 cmp ebp,8 */
#define ADDR_BC_CAP_H8          0x00431216u  /* DeleteBeacon 外层 cmp edi,8 */
#define ADDR_BC_CAP_H9          0x004314ACu  /* EditBeaconMessage 扫描外层 cmp edi,8 */
#define ADDR_BC_CAP_H11         0x0043181Du  /* 逐帧绘制 mov [esp+0x14],8 */

#define ADDR_BC_CAP_DELETEALL       0x00431410u  /* 6B push ebp; mov ebp,ecx; mov eax,[ebp+60] */
#define ADDR_BC_CAP_DELETEALL_NEXT  0x00431416u
#define ADDR_BC_CAP_DELETE          0x004311C0u  /* thiscall(self, house, slot), ret 8 */
#define ADDR_BC_CAP_RESET           0x00430980u  /* thiscall(self), 原生线性清 24 格 */

/* ==================== 主窗口句柄 ==================== */

/* 引擎存的游戏主窗 HWND (ChatEdit/FullMap/GdiText 都用这一处)。输入法定位
 * 要以它为基准: 主窗客户区原点通常在屏幕 (0,0), 那里客户区坐标就是屏幕坐标。 */
#define ADDR_GAME_HWND          0x00B73550u

/* ==================== GameOptions / 渲染计时 ==================== */

/* GameOptionsClass @ 0xA8EB60 (首成员即 GameSpeed), UnitActionLines 在 +0x1E
 * (byte): 引擎自带的"点击移动时画单位行动线"开关。 */
#define ADDR_GAMEOPT            0xA8EB60u
#define OFF_GO_ACTIONLINES      0x1Eu

/* TacticalClass::StartDrawActionLineTimer (0x70D150, __stdcall 无参): 只写本地
 * 渲染计时, 让行动线从头开始画, 不改模拟。 */
#define FN_START_ACTIONLINES    0x0070D150u

/* ==================== 音效 (sound.ini 的 VocClass 条目) ==================== */

/* VocClass::FindIndex (0x7514D0): fastcall(ecx = 名字) -> 下标, 找不到 -1。
 * 大小写不敏感; 音效表未装载时同样返回 -1。 */
#define FN_VOC_FINDINDEX        0x007514D0u

/* VocClass::PlayGlobal (0x750920): fastcall(ecx = 下标, edx = pan 0..0x4000),
 * 栈 = 音量 float + AudioController*, ret 8。音量是 sound.ini Volume 之上的
 * 倍率 (传 1.0 = 原音量); 传了 controller 才会在末尾 SetEvent 绑定, 循环音必须
 * 走这条才能停。 */
#define FN_VOC_PLAYGLOBAL       0x00750920u

/* AudioController.AudioIndex 必须指向这个实例: End/Stop 拿它当"这个控制器属于
 * 音频系统"的凭据, 不设就整只函数空转。 */
#define ADDR_AUDIO_IDX_INST     0x0087E294u

/* AudioController::End: thiscall, 按 Stamp/EventType 验证归属后请求淡出。 */
#define FN_AUDIO_CTRL_END       0x00405FD0u

/* 音频系统启用标志; 设备关闭会释放事件池, 不可自行解引用旧 Event。 */
#define ADDR_AUDIO_ENABLED      0x0087E2A0u

/* AudioController::GetEvent: thiscall, 验证 EventType 与 Stamp, 失效返回 NULL。
 * 先检查音频总开关, 不把池中复用的事件识别为原控制器的事件。 */
#define FN_AUDIO_CTRL_GETEVENT  0x00406130u
#define OFF_AUDIO_EVENT_STATE  0x1Cu      /* 4 = 已结束, 其余为待播/延时/在播 */

/* 音频设备voice池创建: fastcall(ecx=设备, edx=请求数量), 返回实际创建数。 */
#define FN_AUDIO_CREATE_VOICES  0x00403530u
#define ADDR_AUDIO_VOICE_INIT   0x00406C2Cu  /* call CreateVoices, 5字节 */
#define ADDR_AUDIO_VOICE_REINIT 0x00406E41u  /* 设备重建的同一调用 */
#define ADDR_AUDIO_CACHE_BUDGET 0x00403F33u  /* mov edx, 0x100000, 5字节 */

/* ==================== 欢呼 (C 键 / EventClass::ALLCHEER) ==================== */

/* AllToCheerCommandClass::Execute (0x730F30, void, 尾部 `add esp,0x70; ret`)。
 * 钩 8 字节 (`mov eax,[0xA8B538]` + `sub esp,0x70`) 可整条重放, 无相对跳转。
 * 0x730FDB 是同一函数尾部那条裸 `ret`, 栈未分配时跳它即干净返回。 */
#define ADDR_CMD_CHEER_RET      0x00730FDBu
/* Unsorted::MuteSWLaunches: 非 0 时原版 C 键直接返回 (静音不欢呼)。 */
#define ADDR_MUTE_SWLAUNCHES    0x00A8B538u

/* RulesClass::Instance (指针变量, YRpp RulesClass.h)。
 * +0x1C8 = CheerSound (VocClass 下标, rules.ini 的 CheerSound=)。 */
#define ADDR_RULES              0x008871E0u
#define OFF_RULES_CHEERSOUND    0x1C8u

/* TechnoClass::Cheer(bool Force) 虚表槽。基类 0x41BF90 是空实现, 只有
 * InfantryClass 重写 (0x522C00 = 播 Sequence::Cheer); 收尾 `ret 4`。 */
#define OFF_VT_CHEER            0x388u

/* ==================== 单位损失播报 (RadarEvent / EVA) ==================== */

/* RadarEventType::UnitLost (YRpp GeneralDefinitions.h:1112)。
 * Ares 3.0 把 FootClass::Destroyed 的 0x4D98C0 整段重写: 对"当前玩家控制"的
 * 阵亡单位调下面两个函数, 心控来的单位 Owner 已改成我方, 因此也算我方损失。
 * 这两个入口都是纯表现层 (本地雷达事件对象 / 本地 EVA 播放), Ares 与本项目
 * 均未在它们上面挂钩。 */
#define RE_UNIT_LOST                7

/* RadarEventClass::Create: fastcall(ecx=RadarEventType), 栈=CellStruct(4B), ret 4。
 * 站点 0x65FA70 的入口 6 字节 = push ebp / mov ebp,esp / and esp,~7 (无 rel),
 * 序言未执行时可直接跳下面的 ret 4。 */
#define ADDR_RE_CREATE_RET          0x0065FB47u  /* 函数尾 ret 4 */

/* VoxClass::PlayIndex: fastcall(ecx=语音索引, edx=nUnk), 栈=nUnk2, ret 4。
 * 站点 0x752480 的入口 5 字节是一条 mov eax,[abs] (无 rel)。Ares 的 EVA.Lost 走它,
 * 不走按名字的 VoxClass::Play (0x752700, 原版 FootClass::Destroyed 才用)。 */
#define ADDR_VOX_PLAYINDEX_RET      0x00752581u  /* 函数尾 ret 4 */

/* TechnoClass::IsMindControlled (0x7105E0): thiscall 无栈参, 纯只读,
 * 返回 [this+0x2C0] (MindControlledBy) || [this+0x2C4] (MindControlledByAUnit)。
 * MindSelect.cpp 与本模块共用, 覆盖临时心控与永久心控两种。返回值只有 AL 有效。 */
#define FN_TECH_ISMINDCONTROLLED    0x007105E0u

/* ==================== 事件 (EventClass) ==================== */

#define EVT_SIZE                111u      /* EventClass 结构的字节数 */
#define EVT_CAP                 0x80u     /* 事件队列容量 */
#define EVT_PLANCOMMIT          0x2Bu     /* EventType::PlanCommit (YRpp GeneralDefinitions.h) */

/* ==================== Syringe 入口钩的寄存器偏移 ==================== */

#define REG_OFF_ESI             0x0Cu     /* YRpp Syringe.h REGISTERS */
#define REG_OFF_EDI             0x08u
#define REG_OFF_EBP             0x10u
#define REG_OFF_ESP             0x14u     /* 钩点现场栈顶 (MsgText 读栈参实证) */
#define REG_OFF_EAX             0x24u
#define REG_OFF_ECX             0x20u
#define REG_OFF_EDX             0x1Cu

#endif /* MOE_ENGINEADDR_H */
