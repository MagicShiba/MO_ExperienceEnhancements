/* OrderUtil.h - 下单模块共享工具 (PatrolCircle/AirSpread/TreeChop/FuzzyAttack)。
 * 引擎只读查询 + OutList 标准事件入队, 不写模拟内存 (联机安全)。
 * 实现见 OrderUtil.cpp。构建见 构建.bat (同一 DLL)。 */
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
#define ADDR_OUT_NEXT  0xA802D0u
#define ADDR_OUT_BUF   0xA802D4u
#define ADDR_OUT_PAR   0xA83A54u
#define EVT_SIZE 111u
#define EVT_CAP  0x80u

#define ADDR_DISPLAY   0x87F7E8u
#define OFF_MAP_CELLS  0x13Cu
#define OFF_MAP_MAXC   0x140u

#define ADDR_IDBASE 0xB0E840u      /* Fetch_ID 索引基址 (纯读存活校验) */
#define ADDR_IDCOUNT 0xB0E844u     /* Fetch_ID 索引计数 */

#define OFF_PLANFLAG  0x1Du   /* MegaMission.IsPlanningEvent (Z 按住=路径点时置 1) */
#define MISSION_MOVE  2       /* Mission::Move (YRpp GeneralDefinitions.h) */
#define MISSION_ATTACK 1      /* Mission::Attack (注意不是 Action::Attack=5) */

#define ABS_UNIT 1
#define ABS_AIRCRAFT 2
#define ABS_BUILDING 6
#define ABS_INFANTRY 15

/* 对象 WhatAmI (虚表 0x2C, 指针三重守卫+函数指针守卫)。 */
int OU_WhatAmI(void* p);
/* OutList 手工入队 (与 0x6521C0 同义)。满返回 0。 */
int OU_Enqueue(char ev[EVT_SIZE]);
/* 格合法性: idx=(Y<<9)+X, 0<=idx<MaxCells 且 Cells[idx] 非空。 */
int OU_CellValid(int cx, int cy);
/* 鼠标屏坐标 → 格 (ProcessClickCoords + 地图校验)。 */
int OU_MouseCell(int* cx, int* cy);
/* 同上 + 实测像素 (橡皮线用, px/py 可空)。 */
int OU_MouseCellPx(int* cx, int* cy, int* px, int* py);
/* 存活校验 (Fetch_ID 索引二分, 纯读, 永不碰对象本身)。 */
int OU_PtrAliveRaw(void* u, int id);
/* 同上, idxOk=0 时 fail-open 全回活 (索引自检未通过时功能不变)。 */
int OU_PtrAlive(void* u, int id, int idxOk);
