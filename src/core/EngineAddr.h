/* EngineAddr.h - gamemd 引擎地址与结构偏移的唯一出处。
 *
 * 铁律: 新增引擎地址只许加在这个文件里, 不要在 .cpp 里写本地 #define ——
 * 同一地址的副本之间会悄悄跑偏 (`OFF_HOUSE_MONEY` 就分叉出 0x2F0 / 0x30C
 * 两个值), 读错字段的那处判据恒失败, 功能静默失效。
 * 每个地址后面必须能指出出处 (函数地址 / 指令字节 / YRpp 成员名), 因为引擎是
 * 按版本硬编码的, 没有出处就无法在换 exe 时复核。
 *
 * 本文件只含宏, 不依赖任何其它头, 可以在任何位置 include。
 */
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

/* SessionClass::Instance - 首成员即 GameMode (0=战役/菜单 3=LAN 4=Internet 5=Skirmish)。 */
#define ADDR_SESSION            0xA8B238u
#define ADDR_GAMEMODE           0xA8B238u   /* 旧名, 同址 */

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

/* BuildingClass::Array。 */
#define ADDR_BLD_ARR            0xA8EB40u

/* HouseClass 偏移。 */
#define OFF_HOUSE_INDEX         0x30u     /* HouseClass::ArrayIndex */
#define OFF_HOUSE_TYPE          0x34u     /* HouseClass::Type -> HouseTypeClass* */
#define OFF_IS_HUMAN            0x1ECu    /* HouseClass::IsHumanPlayer */
/* HouseClass::Money。实证: gamemd 0x705DF3 = `mov eax,[ecx+0x30C]; ret`
 * (Available_Money), 0x4F9950 GiveMoney = `mov edx,[ecx+0x30C]; add; mov [ecx+0x30C]`。
 * 0x2F0 是错的 (曾误用于 RepairAll 的 Owner 校验)。 */
#define OFF_HOUSE_MONEY         0x30Cu

/* HouseTypeClass::MultiplayPassive - 中立方为 1。 */
#define OFF_HTYPE_PASSIVE       0x1A6u

/* ==================== 对象 (Object/Techno/Building) ==================== */

#define OFF_HEALTH              0x6Cu     /* ObjectClass::Health */
#define OFF_INLIMBO             0x81u     /* ObjectClass::InLimbo (在工厂/运输内) */
#define OFF_ISALIVE             0x90u     /* ObjectClass::IsAlive */
#define OFF_NEXT_OBJ            0x30u     /* ObjectClass::NextObject */
#define OFF_OWNER               0x21Cu    /* TechnoClass::Owner -> HouseClass* */
#define OFF_BLD_TYPE            0x520u    /* BuildingClass::Type */
#define OFF_BLD_REPAIRING       0x6E8u    /* BuildingClass::IsBeingRepaired
                                           * (0x4506C6 读 / 0x4509B4 清 0) */
#define OFF_TYPE_STRENGTH       0xA0u     /* ObjectTypeClass::Strength
                                           * (0x5F5C42 自己的 `fild [eax+0xA0]`) */
#define OFF_TECH_TOKEN          0x514u    /* TechnoClass 规划令牌 (0x705D20 实证) */

/* 动态向量 (DynamicVectorClass) 通用布局。 */
#define OFF_VEC_ITEMS           4u
#define OFF_VEC_COUNT           16u

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
 * `cmp eax,6` = BuildingClass 实证)。返回值见 AbstractType。 */
#define OFF_VT_WHATAMI          0x2Cu
#define WHATAMI_UNIT            1
#define WHATAMI_AIRCRAFT        2
#define WHATAMI_BUILDING        6
#define WHATAMI_INFANTRY        15

/* 具体类的类型指针成员 (YRpp 布局换算, BuildingClass 那项 0x520 与
 * OFF_BLD_TYPE 同值 ⇒ 其余同类可信)。 */
#define OFF_UNIT_TYPE           0x6C4u
#define OFF_AIRCRAFT_TYPE       0x6C4u
#define OFF_INFANTRY_TYPE       0x6C0u

/* TechnoClass 成员。Veterancy 是 float: 0~1 = 新兵升老兵进度, 1~2 = 老兵升精英,
 * <0 = 不吃经验 (YRpp VeterancyStruct::IsNegative)。Ammo 是当前弹药。 */
#define OFF_TECH_VETERANCY      0x150u
#define OFF_TECH_AMMO           0x2FCu
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
/* UnitClass::NonPassengerCount: 卸载状态机保留几个不下车 (炮手)。
 * 卸载状态 3 的判据就是 NumPassengers <= 这个值 (0x73D8C5)。 */
#define OFF_TECH_NONPASS        0x6E4u
/* BuildingClass::Occupants: DynamicVectorClass<InfantryClass*> 本体 0x684,
 * Items 0x688, Count 0x694 (GetOccupantCount 0x4581F0 直接返回 [ecx+0x694];
 * 0x4585C0 KillOccupants 读 Items/Count, 0x522941 AddOccupant 用
 * lea [ebp+0x684] 实证)。驻军不走 Passengers 链, 是另一套存储。 */
#define OFF_BLD_OCC_VEC         0x684u
#define OFF_BLD_OCC_ITEMS       0x688u
#define OFF_BLD_OCC_COUNT       0x694u

/* 类型的名字字段: AbstractTypeClass::ID (0x24, char[0x18]) 与
 * ObjectTypeClass::ImageFile (0x1F8, char[0x19])。art ini 的节名用 ImageFile,
 * 空时落 ID (与引擎查 art 的顺序一致)。 */
#define OFF_TYPE_ID             0x24u
#define OFF_TYPE_IMAGEFILE      0x1F8u

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

/* TechnoTypeClass 成员 (图标 SHP 与弹药上限)。CameoFile 是 `Cameo=` 写的文件名
 * (0x19 字节, 紧挨在 Cameo 指针前); MO 的图标多为 .pcx, 此时 Cameo 指向的是
 * XXICON.SHP 占位图, 必须按 CameoFile 走 PCX 才拿得到真图。 */
#define OFF_TT_CAMEOFILE        0x6D6u
#define OFF_TT_CAMEO            0x6F0u
#define OFF_TT_AMMO             0x684u
/* rules 里的 `PipWrap=` (一排显示几个弹药格) 与 `PipScale=`。
 * 偏移由 YRpp 同版本布局算出 (Ammo=0x684 与上面对得上, 可信):
 * PipScale=0x3D4 (枚举 dword), PipWrap=0x3E4 (int)。 */
#define OFF_TT_PIPSCALE         0x3D4u
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
#define ADDR_VIEWBOUNDS         0x00886FA0u   /* ViewBounds (缩放档 = R) */
#define ADDR_TACTICAL           0x00887324u   /* TacticalClass::Instance (指针) */
#define ADDR_WWMOUSE            0x00887640u

/* 雷达点击门 (点击是否已被 UI 消费)。 */
#define ADDR_CLICKGATE_SKIP     0x006925FCu

/* ==================== 覆盖物 / 格子 (OverlayClass / CellClass) ==================== */

/* OverlayClass::Array (DynamicVectorClass<OverlayClass*>), 布局同其它容器:
 * +0x00 虚表 / +0x04 数据指针 / +0x08 容量 / +0x10 个数。
 * 实证: 0x5FC3FF 构造时 `inc [0xA8EC60]` 再 `mov [ecx+eax*4],esi` (ecx=[0xA8EC54]);
 * 0x5345B2 遍历时先读 [0xA8EC60] 再取 [0xA8EC54]。
 * 注意: 覆盖物对象的 Location 不是格子坐标 (0x5FC4A5 起只在特定条件下才 SetLocation),
 * 别拿 OU_GetCoords 反推箱子所在格 —— 一律走格子数组。 */
#define ADDR_OVERLAYS_ITEMS     0x00A8EC54u
#define ADDR_OVERLAYS_COUNT     0x00A8EC60u

/* OverlayTypeClass::Array (DynamicVectorClass<OverlayTypeClass*>)。 */
#define ADDR_OVLTYPES_ITEMS     0x00A83D84u
#define ADDR_OVLTYPES_COUNT     0x00A83D90u   /* 0x5FD9BB 遍历用 */

/* OverlayClass::Type: 0x5FD0A6 `mov edx,[eax+0xac]` (PlaceOverlay 读图像)。
 * 覆盖物对象 sizeof = 0xB0, Type 是最后一个成员 (0x4A194D `push 0xb0`)。 */
#define OFF_OVL_TYPE            0x00ACu
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
#define OFF_ANIMTYPE_IMAGE      0xA4u

/* 动画播放参数 (int, YRpp AnimTypeClass 成员顺序: Rate -> Start -> LoopStart ->
 * LoopEnd)。注意 +0x2B0 里存的**不是** artmo.ini 写的那个 Rate: 活进程实测
 * (tools/animdump.c) RAIN/SNOW 在这里都是 2, 而两者 art 里写的是 Rate=400 ——
 * 引擎读 ini 时就换算成了"每动画帧要过几个逻辑帧" (= 900/Rate 截断, 见 ModEnc
 * Rate), 所以这个字段可以直接当间隔用, 别再除一次。
 * 循环段: RAIN 有 31 帧 / SNOW 20 帧, 而两者 LoopEnd=19, 必须按类型字段截断。
 * 实证: gamemd 0x42477B-0x4247B1 用 [type+0x2B8]/[type+0x2BC] 复位动画帧,
 * 0x4246FC 用 LoopEnd-Start 判循环。 */
#define OFF_ANIMTYPE_RATE       0x2B0u
#define OFF_ANIMTYPE_START      0x2B4u
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
/* Unsorted::TacticalSelectables (12B/项, 上限 500) = 物件屏坐标表。 */
#define ADDR_TACTICAL_SELARR    0x00B0CEC8u
/* 引擎"本帧整幅重画"标志 (GScreenClass::Render 的 mode 0 趟消费它)。 */
#define ADDR_FULLREDRAW         0x00B0E63Cu

/* PCX 实例 (改色盘用)。 */
#define ADDR_PCX_INSTANCE       0xAC4848u

/* ==================== 光照 (ScenarioClass) ==================== */

/* ScenarioClass 里的光照字段偏移。地图 ini 的 [Lighting] 段读进来就放这儿
 * (gamemd 0x68A7xx 那段读入代码): Ambient 与 tint = ini 值 ×100,
 * Ground / Level = ini 值 ×1000 (常量 0x7E2AC0=100.0, 0x7E4658=1000.0)。
 * 本项目只动 Ambient 与 R/G/B, Ground/Level 不碰。 */
#define OFF_SCEN_AMBIENT_ORIG  0x3528u   /* AmbientOriginal (ini 的 Ambient) */
#define OFF_SCEN_AMBIENT_CUR   0x352Cu   /* AmbientCurrent */
#define OFF_SCEN_AMBIENT_TGT   0x3530u   /* AmbientTarget */
#define OFF_SCEN_TINT_R        0x3534u   /* NormalLighting.Tint.R (ini 的 Red) */
#define OFF_SCEN_TINT_G        0x3538u   /* NormalLighting.Tint.G (Green) */
#define OFF_SCEN_TINT_B        0x353Cu   /* NormalLighting.Tint.B (Blue) */

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
#define OFF_DISP_FOLLOW         0x119Cu      /* bool FollowObject */
#define OFF_DISP_FOLLOWOBJ      0x11A0u      /* ObjectClass* ObjectToFollow */
#define FN_DISP_SETFOLLOW       0x004AEB30u  /* __thiscall(self, obj), ret 4 */

/* CommandClass "Follow" 的 Execute (F 键, KeyboardMD.ini 的 Follow=70)。
 * 0x537A10 = `a1 c8 ec a8 00` (mov eax,[0xA8ECC8], 5 字节), 收尾 `ret 4`。 */
#define ADDR_CMD_FOLLOW         0x00537A10u

/* ObjectClass::Location (CoordStruct, leptons)。引擎跟随每帧取坐标就在这里:
 * 0x55B6CF `add eax,0x9c` 之后连读 3 个 dword (X/Y/Z)。 */
#define OFF_OBJ_LOCATION        0x009Cu

/* ==================== 事件 (EventClass) ==================== */

#define EVT_SIZE                111u      /* EventClass 结构的字节数 */
#define EVT_CAP                 0x80u     /* 事件队列容量 */
#define EVT_PLANCOMMIT          0x2Bu     /* EventType::PlanCommit (YRpp GeneralDefinitions.h) */

/* ==================== Syringe 入口钩的寄存器偏移 ==================== */

#define REG_OFF_ESI             0x0Cu     /* YRpp Syringe.h REGISTERS */
#define REG_OFF_EDI             0x08u
#define REG_OFF_EBP             0x10u
#define REG_OFF_EAX             0x24u

#endif /* MOE_ENGINEADDR_H */
