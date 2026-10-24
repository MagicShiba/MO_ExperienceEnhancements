/* EngineAddr.h - gamemd 引擎地址与结构偏移的唯一出处。
 *
 * 为什么集中: 这些地址原来各自写在用它的 .cpp 里, 同一个地址最多有 11 份副本
 * (ADDR_FRAME), 改一处要翻 11 个文件, 而且副本之间会悄悄跑偏 —— 2026-10-25 收拢
 * 时正好抓到一例: OFF_HOUSE_MONEY 在 RepairAll.cpp 写成 0x2F0, 别处是 0x30C。
 * 0x2F0 是错误的, 于是那个模块的 "Owner 合法性校验" 读的是一个无关字段, 校验恒
 * 失败 → 修复全部静默地什么都不做。
 *
 * 铁律: 新增引擎地址只许加在这个文件里, 不要再在 .cpp 里写本地 #define。
 * 每个地址后面必须能指出出处 (函数地址 / 指令字节 / YRpp 成员名), 因为引擎是
 * 按版本硬编码的, 没有出处就无法在下一次换 exe 时复核。
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
#define OFF_TECH_TOKEN          0x514u    /* TechnoClass 规划令牌 (0x705D20 实证) */

/* 动态向量 (DynamicVectorClass) 通用布局。 */
#define OFF_VEC_ITEMS           4u
#define OFF_VEC_COUNT           16u

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

/* PCX 实例 (改色盘用)。 */
#define ADDR_PCX_INSTANCE       0xAC4848u

/* ==================== 事件 (EventClass) ==================== */

#define EVT_SIZE                111u      /* EventClass 结构的字节数 */
#define EVT_CAP                 0x80u     /* 事件队列容量 */
#define EVT_PLANCOMMIT          0x2Bu     /* EventType::PlanCommit (YRpp GeneralDefinitions.h) */

/* ==================== Syringe 入口钩的寄存器偏移 ==================== */

#define REG_OFF_ESI             0x0Cu     /* YRpp Syringe.h REGISTERS */
#define REG_OFF_EAX             0x24u

#endif /* MOE_ENGINEADDR_H */
