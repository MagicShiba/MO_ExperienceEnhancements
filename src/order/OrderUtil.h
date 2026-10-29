/* OrderUtil.h - 下单/选中模块共享工具 (PatrolCircle/SmartOrder/TreeChop/FuzzyAttack/MindSelect)。
 * 只读查询 + 标准事件入队; 单机单位令直写 DoList, 联机只走 OutList。
 * 实现见 OrderUtil.cpp。 */
#pragma once

#include <windows.h>
#include "EngineAddr.h"

typedef struct { short X, Y; } CellStruct;
typedef struct { int X, Y, Z; } CoordStruct;

typedef void* __attribute__((thiscall)) (*Fn_TargetFromObj)(void* tgt, void* obj);
/* TargetFromCell (0x6E6B20): CellStruct → 8 字节 TARGET 缓冲 (id + rtti@+4)。 */
typedef void* __attribute__((thiscall)) (*Fn_TargetFromCell)(void* tgt, const CellStruct* cell);
typedef void* __attribute__((thiscall)) (*Fn_MegaMissionCtor)(void* ev, int houseIdx,
    int srcID, unsigned srcRttiW, int mission,
    int tgtID, unsigned tgtRttiW, int dstID, unsigned dstRttiW,
    int folID, unsigned folRttiW);
typedef int __attribute__((thiscall)) (*Fn_ClickCoords)(void* disp, Point2D* src, CellStruct* xy,
    CoordStruct* xyz, void** tgt, unsigned char* a5, unsigned char* b6);
typedef int __attribute__((thiscall)) (*Fn_WhatAmI)(void* self);
typedef void (__attribute__((cdecl)) *Fn_SndCb)(void);

#define FN_TGT_OBJ    ((Fn_TargetFromObj)(void*)0x6E6AB0u)
#define FN_TGT_CELL   ((Fn_TargetFromCell)(void*)0x6E6B20u)
#define FN_MEGA       ((Fn_MegaMissionCtor)(void*)0x4C6860u)
#define FN_CLICKCOORDS ((Fn_ClickCoords)(void*)0x692300u)

/* ---------------- 地址 (全只读, 写模拟一律禁止) ---------------- */

#define ADDR_SELOBJ_ITEMS  0xA8ECBCu
#define ADDR_SELOBJ_COUNT  0xA8ECC8u

#define ADDR_OUT_COUNT 0xA802C8u
#define ADDR_OUT_HEAD  0xA802CCu
#define ADDR_OUT_NEXT  0xA802D0u
#define ADDR_OUT_BUF   0xA802D4u
#define ADDR_OUT_PAR   0xA83A54u

/* DoList (执行队列, 16384): OutList 每帧搬运的目的地。单机溢出直注用,
 * 联机不动 (MP 分组只读于中转, SP 执行不读分组, 见 notes/路径点上限.md)。 */
#define ADDR_DO_COUNT 0x8B41F8u
#define ADDR_DO_NEXT  0x8B4200u
#define ADDR_DO_BUF   0x8B4204u
#define ADDR_DO_PAR   0xA70204u
#define DO_CAP  0x4000u
#define DO_MASK 0x3FFFu

#define ADDR_DISPLAY   0x87F7E8u
#define OFF_MAP_CELLS  0x13Cu
#define OFF_MAP_MAXC   0x140u

#define ADDR_IDBASE 0xB0E840u      /* Fetch_ID 索引基址 (纯读存活校验) */
#define ADDR_IDCOUNT 0xB0E844u     /* Fetch_ID 索引计数 */

/* gamemd .text 范围 (虚表与引擎函数验址共用, 单机/联机二选一, 进程内不变)。 */
#define OU_EXE_LO 0x401000u
#define OU_EXE_HI 0x8A0000u
static __inline int OU_InExe(unsigned v) { return (v >= OU_EXE_LO && v < OU_EXE_HI) ? 1 : 0; }

/* 模块日志: tag 含模块名 (如 "othrottle: ClickedMission"), msg 为后半句。 */
void OU_LogTag(const char* tag, const char* msg);
/* UtoA+AppendCStr 合一 (省去各处手写 tmp[16] 中转)。 */
unsigned OU_AppendU(char* dst, unsigned pos, unsigned v, unsigned cap);
/* 取格指针 (越界/空槽回 NULL, OU_CellValid/OU_CellFloorZ 共用)。 */
void* OU_CellPtr(int cx, int cy);
/* 111B 事件拷贝 (OU_DirectToDoList/OU_Enqueue/OT_SubmitRaw 共用)。 */
void OU_CopyEvt(char* dst, const char* src);

#define OFF_PLANFLAG  0x1Du   /* MegaMission.IsPlanningEvent (Z 按住=路径点时置 1) */
#define MISSION_MOVE  2       /* Mission::Move (YRpp GeneralDefinitions.h) */
#define MISSION_ATTACK 1      /* Mission::Attack (注意不是 Action::Attack=5) */
#define MISSION_ENTER 7       /* Mission::Enter (载具/运输, Destination=目标对象) */
#define MISSION_CAPTURE 8     /* Mission::Capture (工程师占领/步兵进驻, Destination=建筑) */
#define MISSION_SABOTAGE 17   /* Mission::Sabotage (备用进驻, Destination=建筑) */
#define MISSION_UNLOAD 16     /* Mission::Unload (载具放载员, 见 SingleUnload.cpp) */

/* 光标 Action (YRpp GeneralDefinitions.h, MouseOver/DecideAction 返回值)。
 * 注意与 Mission 编号不同: 如 Action::Attack=5 对应 Mission::Attack=1。 */
#define ACTION_MOVE 1
#define ACTION_ENTER 3
#define ACTION_ATTACK 5
#define ACTION_CAPTURE 9
#define ACTION_SABOTAGE 16
#define ACTION_GUARDAREA 26
#define ACTION_HEAL 27        /* Action::Heal */
#define ACTION_GREPAIR 29     /* Action::GRepair (对自家建筑维修) */
#define ACTION_F57 57         /* 步兵重映射表尾项 (含义未定, 照抄引擎) */

#define ABS_UNIT 1
#define ABS_AIRCRAFT 2
#define ABS_BUILDING 6
#define ABS_INFANTRY 15

/* 对象 WhatAmI (虚表 0x2C, 三重守卫)。 */
int OU_WhatAmI(void* p);
/* 类型判定 (聚合 WhatAmI): 可实现下令的机动单位 (单位/步兵/飞机);
 * 可作战目标 (上面三类 + 建筑)。 */
int OU_IsUnit(void* p);
int OU_IsTarget(void* p);
/* 联机判定 (GameMode 3=LAN/4=Internet, 其它单机)。订单系统共用。 */
int OU_IsMP(void);
/* 标准事件入队: 单机直写 DoList (上限 16384), 联机走 OutList (满回 0)。 */
int OU_Enqueue(char ev[EVT_SIZE]);
/* 事件缓冲清零 (无 CRT, 不能 memset)。 */
void OU_EvClear(void* ev);
/* ---------------- Surface 锁定 (全 DLL 共用) ----------------
 * 引擎 Surface 虚表: +0x5C = Lock(surf, rect, int, int) -> 首像素,
 * +0x60 = Unlock(surf, rect)。硬编码的引擎内部布局, 全 DLL 只此一份。
 * 成功回已锁定首像素 (失败 NULL); 解锁自身幂等 (surf 无效直接忽略)。
 * 注意: 宽高/pitch 由调用方提供自己的绘制坐标系, 不从 surface 推导。 */
unsigned char* OU_SurfLock(void* surf);
void OU_SurfUnlock(void* surf);
/* PlanCommit 事件 (路径点收尾) 构造 + 入队。回 1=入队成功。 */
int OU_FirePlanCommit(int houseIdx);
/* MegaMission 事件构造 + 下令 —— **全 DLL 唯一的事件构造点**
 * (清事件 → FN_MEGA → 置 IsPlanningEvent → OT_Fire)。
 * tgt/dst/fol 三个槽各是一对 (对象或格的 id, rtti), 不用的槽传 (0, 0)。
 * 形状必须与鼠标真实事件同形 (见 notes/选中与下令上限.md), 改这里即改全部。
 * OU_MakeMega 只填事件 (给走 OU_Enqueue 的调用方); OU_FireMega 填完直接下令
 * (联机 mode=2 才被 OT 接管, 否则落 OU_Enqueue)。回 1=已入队。 */
void OU_MakeMega(char ev[EVT_SIZE], int houseIdx, int srcID, unsigned srcRtti,
                 int mission, int tgtID, unsigned tgtRtti,
                 int dstID, unsigned dstRtti,
                 int folID, unsigned folRtti, int plan);
int OU_FireMega(int houseIdx, int srcID, unsigned srcRtti, int mission,
                int tgtID, unsigned tgtRtti, int dstID, unsigned dstRtti,
                int folID, unsigned folRtti, int plan,
                void* unit, int uid, int idxOk);
/* 单机直写 DoList (111B 拷 + 现造 PAR + 转运三手续), 联机永不调用。
 * 回 1=写入, 0=DoList 满。 */
int OU_DirectToDoList(const char* ev);
/* 格合法性: idx=(Y<<9)+X, 0<=idx<MaxCells 且 Cells[idx] 非空。 */
int OU_CellValid(int cx, int cy);
/* 格地面高度 (lep, 越界/无效格回 0)。 */
int OU_CellFloorZ(int cx, int cy, int* zOut);
/* DynamicVector (Items@+4 / Count@+16) 元素数, 带范围与可读性守卫 (失败回 0,
 * items 置 NULL)。 */
int OU_VecItems(unsigned vecAddr, void*** itemsOut);
/* p 是否该 DynamicVector 的成员 (类型数组判定: 只比指针)。 */
int OU_InVec(unsigned vecAddr, const void* p);

/* ---------------- 类型指针自校准 (SmartArea/SmartOrder/TreeChop 共用) -----
 * 运行时扫描 Type/Owner 指针在对象内的偏移, 防 mod 数据布局差异。
 * pin 缓存一律由调用方以指针传入 (各模块独立, 一家失效不连坐别家)。
 * Type 判据全只读: 指针合理性 + 类型数组成员 (OU_InVec) + ID 字符串可读;
 * Owner 校准只做指针值比较, 不解引用对象字段, 联机安全。 */

/* 类型 ID 字符串合法性 (24 字节窗口: A-Z 0-9 _ . -, 空串/非法字符/超窗判无效)。 */
int OU_StrOk(const char* s);
/* 类型指针 p 的 ID 读出: *idOff pin 命中直读 (读坏即回 -1), 否则扫候选
 * 偏移 {0x20,0x24,0x28} 命中即 pin; 成功拷 24 字节入 out 回 1。 */
int OU_TypeID(void* p, char* out, int* idOff);
/* 单位 u 按类别取 Type 指针 (off3 按槽 pin: 0=Unit 1=Aircraft 2=Infantry):
 * pin 命中直验 (合理性 + vecAddr 数组成员 + ID 可读), 失效回扫 0x40..0x900
 * (4 步进, 命中首个双证成员即 pin); 未知类别回 NULL。 */
void* OU_TypeOf(void* u, int w, int* off3, unsigned vecAddr, int* idOff);
/* 同上单 pin 变体 (无类别槽的对象: TreeChop 的 TerrainType 树)。 */
void* OU_ScanTypeOf(void* u, int* pinnedOff, unsigned vecAddr, int* idOff);
/* Owner 指针偏移自校准 (双单位确认, 防偶发撞值): *ownerOff pin 命中直验
 * (u0 必中, u1 非空须同中), 失效回扫 0x40..0x900; 命中回 1, 失败回 0。 */
int OU_CalOwner(void* u0, void* u1, void* house, int* ownerOff);
/* 玩家主色 (只读): HouseClass::ColorSchemeIndex → ColorScheme::Array[..].Colors[MainShadeIndex]。
 * 与 ESC 玩家色色块 / 单位颜色同一套。成功回 1; r/g/b 可传 0。 */
int OU_HouseColor(void* house, int* r, int* g, int* b);
/* 同上, 按房号查 Houses 表 (信标这类只拿得到房号的场合)。 */
int OU_HouseIndexColor(int houseId, int* r, int* g, int* b);
/* ---- TARGET 编码 (8 字节: id@0, rtti@4) ----
 * 目标槽统一走这两个: 清缓冲 + 调引擎编码 + 取 id/rtti 一手包办。
 * OU_ObjTgt 解不出回 -1 (id 本身可以是 0, 不能用 0 当失败); rttiOut 可传 0。
 * OU_CellTgt 恒回 id (空格=0), 不判失败。 */
int OU_ObjTgt(void* obj, unsigned char* rttiOut);
int OU_ObjId(void* obj);        /* 只要 id, 解不出回 0 */
int OU_CellTgt(int cx, int cy, unsigned char* rttiOut);
/* 鼠标屏坐标 → 格 (ProcessClickCoords + 地图校验)。 */
int OU_MouseCell(int* cx, int* cy);
/* 格 + 光标地面高度 (越界高度归零)。 */
int OU_MouseCellZ(int* cx, int* cy, int* lz);
/* 同上 + 实测像素 (橡皮线用, px/py 可空)。 */
int OU_MouseCellPx(int* cx, int* cy, int* px, int* py);
/* 同上 + 点中对象 (智能下令用; tgtOut 可空, 无目标时写 0, 返回值仍是格有效性)。 */
int OU_MouseCellTarget(int* cx, int* cy, void** tgtOut, int* px, int* py);
/* 光标自适应查询 (只读): 问选中单位 self 点 target 的光标,
 * 经 vtbl+0x74 调用, 与 DecideAction 内部同入口。失败回 -1。 */
int OU_MouseOverObject(void* self, void* target);
/* 同上, 但 ignoreForce=1: 无视当前按住的 Ctrl/Shift/Alt 取单位固有能力。
 * 组合键热键 (如 SmartArea 的 Shift+Middle) 必须用这个: 0 会被物理修饰键
 * 污染 (Shift→ToggleSelect=8, Ctrl→强制攻击), 使真光标失真。只读, 失败回 -1。 */
int OU_MouseOverObjectNative(void* self, void* target);
/* 复刻引擎左键点击的真实派发链 (反汇编实证, 见 notes/侵略模式.md):
 *   ① vtbl+0x74 = Class::GetActionOnObject 取固有光标 (OU_MouseOverObjectNative);
 *   ② 各 Class::ObjectClickedAction 先做一层"类内重映射"再交基类 ——
 *        InfantryClass @0x51F190 (表 0x51F20C): 28 Damage→Capture9, 29 GRepair→Capture9,
 *          27 Heal→Attack5, 57→Attack5 (工程师对血厚敌楼的 28 就在这里变 9);
 *        UnitClass @0x738890: 3 Enter→Move1, 9 Capture/16 Sabotage/27 Heal/29 GRepair→Attack5;
 *        Aircraft/Building 无覆盖。
 *   ③ FootClass::ObjectClickedAction @0x4D74E0 (表 0x4D7D04) Action→Mission。
 * OU_RemapAction 复刻第 ② 层 (actWhat=OU_WhatAmI(下令单位); 未覆盖的类原样返回)。 */
int OU_RemapAction(int actWhat, int action);
/* 单位 unit 对 target 左键(裸, 无视修饰键)会下的 Mission; 无操作/失败回 -1。
 * 攻击=1 / 移动=2 / 进入=7 / 占领=8 / 进驻破坏=17 (Destination=目标)。 */
int OU_NativeMission(void* unit, void* target);
/* 该单位对该目标是否有攻击类光标 (会下 Mission::Attack)。
 * 只读 MouseOver (ignoreForce=1, 无视 Ctrl/Shift/Alt 取固有能力)。
 * 复刻引擎: 光标先过"类内重映射"再判 (见 OU_RemapAction/OU_NativeMission),
 * 所以工程师对血厚敌楼的 Damage(28) 不会误判成攻击。失败/非 Techno 回 0。
 * FuzzyAttack/SmartArea/AttackMove 共用。 */
int OU_CanAttack(void* unit, void* target);
/* 该单位对该目标是否有进入类光标 (Action::Enter, 会下 Mission::Enter/
 * Mission::Sabotage)。只读 MouseOver (ignoreForce=1, 无视修饰键取固有能力)。
 * 只有真工程师才拿得到对建筑的 Enter (Engineer=yes 新单位自动生效,
 * 不靠 ID 表)。失败/非 Techno 回 0。SmartArea 占车/修楼复核用。 */
int OU_CanEnter(void* unit, void* target);
/* 该单位对该建筑是否有维修类光标 (Action::GRepair, 会下进驻修楼令)。
 * 只读 MouseOver (ignoreForce=1, 无视修饰键取固有能力)。
 * 实测: 工程师对自家掉血楼回 GRepair(29)。失败/非建筑回 0。
 * SmartArea 修楼复核用。 */
int OU_CanGRepair(void* unit, void* target);
/* 对象中心坐标 (只读, leptons, 1 格=256)。成功回 1, 失败回 0。 */
int OU_GetCoords(void* p, CoordStruct* out);
/* 全局自适应决策 (只读): DisplayClass::DecideAction (0x692610),
 * 与下单前引擎调的同一函数, 只回 Action 不下单。
 * cx/cy=鼠标格, tgt=点中对象 (须非空)。回 Action (0~80), 失败回 -1。 */
int OU_DecideAction(int cx, int cy, void* tgt);
/* 存活校验 (Fetch_ID 索引二分, 纯读, 永不碰对象本身)。 */
int OU_PtrAliveRaw(void* u, int id);
/* 同上, idxOk=0 时 fail-open 全回活 (索引自检未通过时功能不变)。 */
int OU_PtrAlive(void* u, int id, int idxOk);
/* 存活三连 (IsAlive/InLimbo/Health 只读字节判据, 不碰 Fetch_ID 索引;
 * Aggressive/MindSelect/PlaceScatter 共用)。 */
int OU_TechAliveRaw(void* p);
/* 中立阵营判定 (House->Type->MultiplayPassive, 只读, 读不到按非中立;
 * SmartArea/Aggressive 共用, 自检状态各模块自留)。 */
int OU_HouseNeutral(void* h);
/* 单位 PlanningToken 上的节点数 (只读; token 不存在/野指针回 0)。
 * RallyPath/SingleUnload 共用。 */
int OU_TokenNodes(void* unit);

/* ---------------- 规划线共享 (FuzzyAttack/SmartArea/SmartOrder 红绿线) -----
 * 端点存 leptons 世界坐标, 绘制期经引擎投影。不存屏像素。
 * 全只读 + 渲染面绘制, 不写模拟内存。 */
#define OU_PREV_CAP 128
typedef struct { int ulx, uly, ulz, tlx, tly, tlz; } OU_PrevLine;
/* 世界点 → 屏像素 (引擎 CoordsToClient 0x6D2140 + 缩放折算, 只读)。 */
int OU_ProjWorld(int lx, int ly, int lz, int* px, int* py);
/* 指定 lepton 坐标处地面高度 (MapClass::GetCellFloorHeight 0x578080, 只读)。 */
int OU_GroundZAt(int x, int y, int* zOut);
/* 指定 lepton 坐标处可站立表面高度 (地形 + 桥面; 只读)。
 * refZ 传单位自身 Z, 用于区分在桥上还是桥下 (取更接近的一层)。 */
int OU_SurfaceZAt(int x, int y, int refZ, int* zOut);
/* 指定格中心处地表高度 (地形 + 桥面, 有桥取桥面; 只读)。 */
int OU_CellSurfaceZ(int cx, int cy, int* zOut);

/* ---------------- 手绘几何 (16 位面, 逐像素 Blend16) ----------------
 * 全 DLL 唯一一份 Bresenham / 画点 / 方块。
 * clip 为半开区间 [x0,x1)×[y0,y1); 传 NULL 表示不裁剪。
 * thick 的纵向外扩是居中的: thick=1 → 单行, 3 → y-1/y/y+1。 */
typedef struct { int x0, y0, x1, y1; } OuClip;
int  OU_Put(unsigned char* locked, int w, int h, int pitch,
            const OuClip* clip, int x, int y,
            unsigned r, unsigned g, unsigned b, unsigned a);
void OU_Line(unsigned char* locked, int w, int h, int pitch,
             const OuClip* clip, int x0, int y0, int x1, int y1,
             unsigned r, unsigned g, unsigned b, unsigned a, int thick);
void OU_Box(unsigned char* locked, int w, int h, int pitch,
            const OuClip* clip, int cx, int cy, int m,
            unsigned r, unsigned g, unsigned b, unsigned a);

/* 存一对端点 (leptons 世界坐标; 满回 0)。 */
int OU_PrevPush(OU_PrevLine* buf, int* nio,
                int ulx, int uly, int ulz, int tlx, int tly, int tlz);
/* 双端取 OU_GetCoords 存线 (任一失败回 0)。 */
int OU_PrevPushObjs(OU_PrevLine* buf, int* nio, void* u, void* t);
/* 起点取单位中心, 终点取落点格心地表高度 (含桥面, 与空军高度无关)。 */
int OU_PrevPushCell(OU_PrevLine* buf, int* nio, void* u, int cx, int cy);
/* 绘制全部预览线 (逐对引擎投影, 失败跳过)。thick = 线宽像素。 */
void OU_PrevDraw(OU_PrevLine* buf, int n, unsigned char* locked, int w, int h, int pitch,
                 unsigned r, unsigned g, unsigned b, unsigned a, int thick);
/* 同 OU_PrevDraw, 画虚线 (每 dash 像素实 / 后 gap 像素空)。 */
void OU_PrevDrawDash(OU_PrevLine* buf, int n, unsigned char* locked, int w, int h, int pitch,
                     unsigned r, unsigned g, unsigned b, unsigned a,
                     int thick, int dash, int gap);
/* 单像素细虚线 (逐像素插值, 自带裁剪)。 */
void OU_ThinLineDash(unsigned char* locked, int w, int h, int pitch,
                     int x0, int y0, int x1, int y1,
                     unsigned r, unsigned g, unsigned b, unsigned a,
                     int thick, int dash, int gap);
/* 预览节流 (三家规划线共用): 换格/换key且距上次>minMs, 或超 refreshMs 刷一次。
 * 回 1=该刷 (已更新 *last*); 0=跳过。首轮恒刷。 */
int OU_PrevThrottle(unsigned* lastMs, int* lastCx, int* lastCy, int* lastKey,
                    int cx, int cy, int key, unsigned minMs, unsigned refreshMs);

/* 热键串 (照 AutoLoadDev Hotkey=Ctrl+D 写法, 另支持鼠标键与多主键)。
 * 修饰键 Ctrl/Alt/Shift/Win (+/空格分隔) + 按键, 大小写不敏感。
 * 按键可多个, 最后一个是主键, 前面的是必须按住的附加键。
 * 例: "Shift+P" / "Ctrl+Z+Middle" / "Middle"。
 * 键名: 单个字母/数字, F1~F24, Space/Tab/Enter/Backspace/Insert/Delete/Home/
 * End/PageUp/PageDown/Up/Down/Left/Right/Pause/Num0~Num9, Middle/MMB/LMB/RMB/
 * X1/X2 (鼠标)。
 * "None"/"Off"/"Disabled" = 禁用; 多位纯数字串 = 老 TriggerVK 数值兼容。
 * 完整键表见 Hotkey.md。 */
#define OU_MOD_CTRL  0x01u
#define OU_MOD_SHIFT 0x02u
#define OU_MOD_ALT   0x04u
#define OU_MOD_WIN   0x08u
#define OU_HK_MAXEXTRA 4
typedef struct { unsigned vk; unsigned mods; unsigned extra[OU_HK_MAXEXTRA]; int nExtra; int disabled; } OU_Hotkey;
/* 解析热键串。成功回 1 (None 类写 disabled 回 1); 空串/坏串回 0。 */
int OU_ParseHotkey(const char* spec, OU_Hotkey* out);
/* 主键 / 修饰键 / 附加键 / 全组合是否按下。 */
int OU_KeyDown(unsigned vk);
int OU_ModsDown(unsigned mods);
int OU_ExtraDown(const OU_Hotkey* hk);
int OU_HotkeyDown(const OU_Hotkey* hk);
/* 从 ini 读字符串: 行尾 ;注释截掉, 两端去空; 缺键/空值用 def。无 CRT。 */
void OU_ReadString(const char* section, const char* key, const char* def,
                   char* buf, int buflen);
/* 从 ini 读热键串 (缺键/空串/坏串回 0, 调用方保留原值; 成功含 None 回 1)。 */
int OU_ReadHotkey(const char* section, const char* key, OU_Hotkey* io);
/* 单行开关+热键 (缺键/空串/坏串=默认值启用; "0"=禁用; 其它按 Hotkey 串解析,
 * None 类=禁用)。先按 def 解析出默认值, 再读键覆盖。回 1=启用, 0=禁用。 */
int OU_ReadHotkeySingle(const char* section, const char* key,
                        const char* def, OU_Hotkey* io);
