/* OrderUtil.h - 下单/选中模块共享工具 (PatrolCircle/SmartOrder/TreeChop/FuzzyAttack/MindSelect)。
 * 引擎只读查询 + OutList 标准事件入队, 不写模拟内存 (联机安全)。
 * 实现见 OrderUtil.cpp。构建见 build-fast.bat (同一 DLL)。 */
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
/* 同上 + 点中对象 (智能下令用; tgtOut 可空, 无目标时写 0, 返回值仍是格有效性)。 */
int OU_MouseCellTarget(int* cx, int* cy, void** tgtOut, int* px, int* py);
/* 光标自适应查询 (只读, 联机安全): 问选中单位 self 点 target 会是什么光标。
 * 经 vtbl+0x74 (MouseOverObject) 调用, 与 DecideAction 内部同入口;
 * 只读不下单, 不写模拟内存。失败回 -1。 */
int OU_MouseOverObject(void* self, void* target);
/* 对象中心坐标 (只读, 联机安全): vtbl+0x48 (Techno MouseOver 头部
 * 0x4DDED0 内同槽调用, 取出后做格查询, 实证是取坐标)。
 * 成功写 *out (leptons, 1 格=256) 回 1; 槽坏/越界回 0。
 * 注意: 地址系反汇编推断, 调用方必须另做合理性兜底 (本函数已做范围校验)。 */
int OU_GetCoords(void* p, CoordStruct* out);
/* 全局自适应决策 (只读, 联机安全): DisplayClass::DecideAction (0x692610),
 * 与左右键下单前引擎调的同一函数 (按选中代表综合判定, 只回 Action 不下单)。
 * 全函数反汇编实证: 无事件构造/OutList 引用, 纯读。
 * cx/cy=鼠标格, tgt=点中对象 (须非空; 空地不调, 直接散开)。
 * thiscall ECX=Display, 栈 (cell*, obj, 1) (双调用点同形)。
 * 回 Action (0~80), 失败回 -1。 */
int OU_DecideAction(int cx, int cy, void* tgt);
/* 存活校验 (Fetch_ID 索引二分, 纯读, 永不碰对象本身)。 */
int OU_PtrAliveRaw(void* u, int id);
/* 同上, idxOk=0 时 fail-open 全回活 (索引自检未通过时功能不变)。 */
int OU_PtrAlive(void* u, int id, int idxOk);

/* 热键串 (照 AutoLoadDev Hotkey=Ctrl+D 写法, 另支持鼠标键与多主键)。
 * 语法: 修饰键 Ctrl/Alt/Shift/Win (大小写无所谓, 可多个, +/空格分隔) + 按键。
 * 大小写不敏感, 空格/Tab 自动忽略, 行尾 ;注释在加载时由 OU_ReadHotkey
 * 统一截掉 ("Ctrl+Z" 与 "  ctrl + z   ;注释" 等价)。
 * 按键可多个, 最后一个是主键, 前面的是必须按住的附加键。
 * 例: "Shift+P" / "Ctrl+Z+Middle" / "Z+Middle" / "Middle"。
 * 键名: 单个字母/数字, F1~F24, Space/Tab/Enter/Backspace/Insert/Delete/Home/
 * End/PageUp/PageDown/Up/Down/Left/Right/Pause/Num0~Num9, Middle/MMB/LMB/RMB/
 * X1/X2 (鼠标)。
 * "None"/"Off"/"Disabled" = 禁用; 多位纯数字串 = 老 TriggerVK 数值兼容
 * (单位数字符按字母数字键解, 如 "4" 是数字键 4 不是中键)。
 * 完整键表见 Hotkey.md。 */
#define OU_MOD_CTRL  0x01u
#define OU_MOD_SHIFT 0x02u
#define OU_MOD_ALT   0x04u
#define OU_MOD_WIN   0x08u
#define OU_HK_MAXEXTRA 4
typedef struct { unsigned vk; unsigned mods; unsigned extra[OU_HK_MAXEXTRA]; int nExtra; int disabled; } OU_Hotkey;
/* 解析热键串 (输入须已去注释, 见 OU_ReadHotkey)。成功回 1 (None 类写
 * disabled 回 1); 空串/坏串回 0。 */
int OU_ParseHotkey(const char* spec, OU_Hotkey* out);
/* 主键 / 修饰键 / 附加键 / 全组合是否按下。 */
int OU_KeyDown(unsigned vk);
int OU_ModsDown(unsigned mods);
int OU_ExtraDown(const OU_Hotkey* hk);
int OU_HotkeyDown(const OU_Hotkey* hk);
/* 从 ini 读热键串并加载 (行尾 ;注释在这里统一剥掉; 缺键/空串/坏串回 0,
 * 调用方保留原值; 成功含 None 回 1)。数字项不用管, Int 接口遇 ; 本来就停。 */
/* 从 ini 读字符串并加载时统一清洗: 行尾 ;注释截掉, 两端空格/Tab 去掉;
 * 缺键/空值用 def。无 CRT。所有读串入口都走这里, 解析处不再各自处理。 */
void OU_ReadString(const char* section, const char* key, const char* def,
                   char* buf, int buflen);
int OU_ReadHotkey(const char* section, const char* key, OU_Hotkey* io);
/* 单行开关+热键 (缺键/空串/坏串=默认值启用; "0"=禁用; 其它按 Hotkey 串解析,
 * None 类=禁用)。先按 def 解析出默认值, 再读键覆盖。回 1=启用, 0=禁用。 */
int OU_ReadHotkeySingle(const char* section, const char* key,
                        const char* def, OU_Hotkey* io);
