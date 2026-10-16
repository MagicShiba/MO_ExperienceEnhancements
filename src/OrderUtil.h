/* OrderUtil.h - 下单/选中模块共享工具 (PatrolCircle/SmartOrder/TreeChop/FuzzyAttack/MindSelect)。
 * 只读查询 + 标准事件入队; 单机单位令直写 DoList, 联机只走 OutList。
 * 实现见 OrderUtil.cpp。 */
#pragma once

#include <windows.h>

typedef struct { short X, Y; } CellStruct;
typedef struct { int X, Y, Z; } CoordStruct;

typedef void* __attribute__((thiscall)) (*Fn_TargetFromObj)(void* tgt, void* obj);
typedef void* __attribute__((thiscall)) (*Fn_MegaMissionCtor)(void* ev, int houseIdx,
    int srcID, unsigned srcRttiW, int mission,
    int tgtID, unsigned tgtRttiW, int dstID, unsigned dstRttiW,
    int folID, unsigned folRttiW);
typedef int __attribute__((thiscall)) (*Fn_ClickCoords)(void* disp, Point2D* src, CellStruct* xy,
    CoordStruct* xyz, void** tgt, unsigned char* a5, unsigned char* b6);
typedef int __attribute__((thiscall)) (*Fn_WhatAmI)(void* self);
typedef void (__attribute__((cdecl)) *Fn_SndCb)(void);

#define FN_TGT_OBJ    ((Fn_TargetFromObj)(void*)0x6E6AB0u)
#define FN_MEGA       ((Fn_MegaMissionCtor)(void*)0x4C6860u)
#define FN_CLICKCOORDS ((Fn_ClickCoords)(void*)0x692300u)

/* ---------------- 地址 (全只读, 写模拟一律禁止) ---------------- */

#define ADDR_CURPLAYER     0xA83D4Cu
#define OFF_HOUSE_INDEX    0x30u
#define ADDR_SELOBJ_ITEMS  0xA8ECBCu
#define ADDR_SELOBJ_COUNT  0xA8ECC8u

#define ADDR_OUT_COUNT 0xA802C8u
#define ADDR_OUT_HEAD  0xA802CCu
#define ADDR_OUT_NEXT  0xA802D0u
#define ADDR_OUT_BUF   0xA802D4u
#define ADDR_OUT_PAR   0xA83A54u
#define EVT_SIZE 111u
#define EVT_CAP  0x80u

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
/* 取格指针 (越界/空槽回 NULL, OU_CellValid/OU_CellGroundZ 共用)。 */
void* OU_CellPtr(int cx, int cy);
/* 111B 事件拷贝 (OU_DirectToDoList/OU_Enqueue/OT_SubmitRaw 共用)。 */
void OU_CopyEvt(char* dst, const char* src);

#define OFF_PLANFLAG  0x1Du   /* MegaMission.IsPlanningEvent (Z 按住=路径点时置 1) */
#define MISSION_MOVE  2       /* Mission::Move (YRpp GeneralDefinitions.h) */
#define MISSION_ATTACK 1      /* Mission::Attack (注意不是 Action::Attack=5) */
#define MISSION_ENTER 7       /* Mission::Enter (载具/运输, Destination=目标对象) */
#define MISSION_CAPTURE 8     /* Mission::Capture (工程师占领/步兵进驻, Destination=建筑) */
#define MISSION_SABOTAGE 17   /* Mission::Sabotage (备用进驻, Destination=建筑) */

/* 光标 Action (YRpp GeneralDefinitions.h, MouseOver/DecideAction 返回值)。
 * 注意与 Mission 编号不同: 如 Action::Attack=5 对应 Mission::Attack=1。 */
#define ACTION_MOVE 1
#define ACTION_ENTER 3
#define ACTION_ATTACK 5
#define ACTION_CAPTURE 9
#define ACTION_SABOTAGE 16
#define ACTION_GUARDAREA 26

#define ABS_UNIT 1
#define ABS_AIRCRAFT 2
#define ABS_BUILDING 6
#define ABS_INFANTRY 15

/* 对象 WhatAmI (虚表 0x2C, 三重守卫)。 */
int OU_WhatAmI(void* p);
/* 联机判定 (GameMode 3=LAN/4=Internet, 其它单机)。订单系统共用。 */
int OU_IsMP(void);
/* 标准事件入队: 单机直写 DoList (上限 16384), 联机走 OutList (满回 0)。 */
int OU_Enqueue(char ev[EVT_SIZE]);
/* 单机直写 DoList (111B 拷 + 现造 PAR + 转运三手续), 联机永不调用。
 * 回 1=写入, 0=DoList 满。 */
int OU_DirectToDoList(const char* ev);
/* 格合法性: idx=(Y<<9)+X, 0<=idx<MaxCells 且 Cells[idx] 非空。 */
int OU_CellValid(int cx, int cy);
/* 鼠标屏坐标 → 格 (ProcessClickCoords + 地图校验)。 */
int OU_MouseCell(int* cx, int* cy);
/* 同上 + 实测像素 (橡皮线用, px/py 可空)。 */
int OU_MouseCellPx(int* cx, int* cy, int* px, int* py);
/* 同上 + 点中对象 (智能下令用; tgtOut 可空, 无目标时写 0, 返回值仍是格有效性)。 */
int OU_MouseCellTarget(int* cx, int* cy, void** tgtOut, int* px, int* py);
/* 光标自适应查询 (只读): 问选中单位 self 点 target 的光标,
 * 经 vtbl+0x74 调用, 与 DecideAction 内部同入口。失败回 -1。 */
int OU_MouseOverObject(void* self, void* target);
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

/* ---------------- 规划线共享 (FuzzyAttack/SmartArea/SmartOrder 红绿线) -----
 * 端点存 leptons 世界坐标, 绘制期经引擎投影。不存屏像素。
 * 全只读 + 渲染面绘制, 不写模拟内存。 */
#define OU_PREV_CAP 128
typedef struct { int ulx, uly, ulz, tlx, tly, tlz; } OU_PrevLine;
/* 世界点 → 屏像素 (引擎 CoordsToClient 0x6D2140 + 缩放折算, 只读)。 */
int OU_ProjWorld(int lx, int ly, int lz, int* px, int* py);
/* 格地面高度 (CellClass::GetFloorHeight 0x47B3A0, 只读)。 */
int OU_CellGroundZ(int cx, int cy, int* zOut);
/* 单像素细线 (Bresenham, 自带裁剪; locked=已锁定渲染面)。
 * thick<=1 = 1 像素; 2 = 两行 (每个像素再补 (x,y+1)), 视觉上 2 像素粗。 */
void OU_ThinLine(unsigned char* locked, int w, int h, int pitch,
                 int x0, int y0, int x1, int y1,
                 unsigned r, unsigned g, unsigned b, unsigned a,
                 int thick);
/* 存一对端点 (leptons 世界坐标; 满回 0)。 */
int OU_PrevPush(OU_PrevLine* buf, int* nio,
                int ulx, int uly, int ulz, int tlx, int tly, int tlz);
/* 双端取 OU_GetCoords 存线 (任一失败回 0)。 */
int OU_PrevPushObjs(OU_PrevLine* buf, int* nio, void* u, void* t);
/* 起点取单位中心, 终点取落点格心 (地面高度; fly!=0 用单位自身高度)。 */
int OU_PrevPushCell(OU_PrevLine* buf, int* nio, void* u, int fly, int cx, int cy);
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
