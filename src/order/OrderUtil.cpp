/* OrderUtil.cpp - 下单模块共享工具实现 (声明见 OrderUtil.h)。
 * 只读查询 + 标准事件入队; 单机单位令直写 DoList, 联机只走 OutList。 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"   /* OT_Fire: OU_FireMega 的收尾 */
#include "ScaleBar.h"

int OU_IsUnit(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY || w == ABS_AIRCRAFT) ? 1 : 0;
}

int OU_IsTarget(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY ||
            w == ABS_AIRCRAFT || w == ABS_BUILDING) ? 1 : 0;
}

int OU_WhatAmI(void* p)
{
    void** vt;
    Fn_WhatAmI fn;
    if (!p || !IsPlausiblePtr(p)) return -1;
    vt = *(void***)p;
    if (!vt || !IsPlausiblePtr(vt)) return -1;
    fn = (Fn_WhatAmI)vt[0x2C / 4];
    if (!fn || !IsPlausiblePtr((const void*)fn)) return -1;
    return fn(p);
}

int OU_IsMP(void)
{
    int gm = *(int*)0xA8B238u;   /* SessionClass::Instance->GameMode */
    return (gm == 3 || gm == 4) ? 1 : 0;   /* 3=LAN, 4=Internet */
}

/* ---------------- 跨模块共享小函数 (OT/WP/ML 共用, 免各文件手写一份) ---------------- */

void OU_LogTag(const char* tag, const char* msg)
{
    char buf[64];
    unsigned q = 0;
    q = AppendCStr(buf, q, tag ? tag : "?", sizeof(buf));
    q = AppendCStr(buf, q, msg ? msg : "", sizeof(buf));
    LogLine(buf);
}

unsigned OU_AppendU(char* dst, unsigned pos, unsigned v, unsigned cap)
{
    char tmp[16];
    UtoA(v, tmp);
    return AppendCStr(dst, pos, tmp, cap);
}

void* OU_CellPtr(int cx, int cy)
{
    void** cells;
    int maxc, idx;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return 0;
    cells = *(void***)(ADDR_DISPLAY + OFF_MAP_CELLS);
    maxc = *(int*)(ADDR_DISPLAY + OFF_MAP_MAXC);
    if (!cells || !IsPlausiblePtr(cells) || maxc <= 0 || maxc > 512 * 512) return 0;
    idx = (cy << 9) + cx;
    if (idx < 0 || idx >= maxc) return 0;
    return cells[idx];
}

void OU_CopyEvt(char* dst, const char* src)
{
    unsigned i;
    if (!dst || !src) return;
    for (i = 0; i < EVT_SIZE; i++) dst[i] = src[i];
}

/* 现造 Timings (与原版追加体同函数)。内联 call 须声明 ebx/esi/edi/ebp
 * 被破坏, 否则后续账本写进垃圾值。 */
static unsigned OU_SndTimings(void)
{
    Fn_SndCb snd = *(Fn_SndCb*)0x7E1530u;
    unsigned r;
    __asm__ volatile (
        "call *%1\n\t"
        "movl %%eax, %0"
        : "=r" (r)
        : "r" (snd)
        : "ecx", "edx", "ebx", "esi", "edi", "ebp", "cc", "memory");
    return r;
}

/* 单机直写 DoList (111B 拷 + 现造 PAR + 原版转运三手续), 联机永不调用。 */
int OU_DirectToDoList(const char* ev)
{
    unsigned count = *(unsigned*)ADDR_DO_COUNT;
    unsigned next;
    char* slot;
    void* hp;
    if (OU_IsMP()) return 0;
    if (count >= DO_CAP) return 0;
    next = *(unsigned*)ADDR_DO_NEXT & DO_MASK;
    OU_CopyEvt((char*)(ADDR_DO_BUF + next * EVT_SIZE), ev);
    *(unsigned*)(ADDR_DO_PAR + next * 4u) = OU_SndTimings();
    slot = (char*)(ADDR_DO_BUF + next * EVT_SIZE);
    slot[1] = (char)(((unsigned char)slot[1]) & 0xFEu);
    hp = *(void**)ADDR_CURPLAYER;
    if (hp) slot[2] = *(const char*)((const char*)hp + OFF_HOUSE_INDEX);
    /* +3 执行帧 = 当前帧+1: 同帧批次转运前已定稿, 盖 +0 会错过。 */
    *(int*)(slot + 3) = *(int*)ADDR_FRAME + 1;
    *(unsigned*)ADDR_DO_NEXT = (next + 1u) & DO_MASK;
    *(unsigned*)ADDR_DO_COUNT = count + 1u;
    return 1;
}

/* 事件缓冲清零 (无 CRT)。 */
void OU_EvClear(void* ev)
{
    unsigned char* p = (unsigned char*)ev;
    unsigned i;
    if (!p) return;
    for (i = 0; i < EVT_SIZE; i++) p[i] = 0;
}

/* ---------------- Surface 锁定 ---------------- */

typedef void* (__fastcall *OuLockFn)(void*, void*, int, int);
typedef void  (__fastcall *OuUnlockFn)(void*, void*);

unsigned char* OU_SurfLock(void* surf)
{
    unsigned char* vt;
    OuLockFn fn;
    unsigned char* locked;
    if (!surf || !IsPlausiblePtr(surf)) return 0;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    fn = (OuLockFn)(*(void**)(vt + 0x5Cu));
    if (!fn || !IsPlausiblePtr((const void*)fn)) return 0;
    locked = (unsigned char*)fn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return 0;
    return locked;
}

void OU_SurfUnlock(void* surf)
{
    unsigned char* vt;
    OuUnlockFn fn;
    if (!surf || !IsPlausiblePtr(surf)) return;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    fn = (OuUnlockFn)(*(void**)(vt + 0x60u));
    if (fn && IsPlausiblePtr((const void*)fn)) fn(surf, NULL);
}

/* PlanCommit 事件 (路径点收尾) 构造 + 入队。 */
typedef void* __attribute__((thiscall)) (*Fn_PlanCommitCtor)(void* ev, int houseIdx, int evtType);
int OU_FirePlanCommit(int houseIdx)
{
    char ev[EVT_SIZE];
    OU_EvClear(ev);
    ((Fn_PlanCommitCtor)(void*)0x4C66C0u)(ev, houseIdx, EVT_PLANCOMMIT);
    return OU_Enqueue(ev);
}

/* MegaMission 事件构造 (3 个目标槽各一对 (id, rtti), 不用的槽传 0)。 */
void OU_MakeMega(char ev[EVT_SIZE], int houseIdx, int srcID, unsigned srcRtti,
                 int mission, int tgtID, unsigned tgtRtti,
                 int dstID, unsigned dstRtti,
                 int folID, unsigned folRtti, int plan)
{
    OU_EvClear(ev);
    FN_MEGA(ev, houseIdx, srcID, srcRtti, mission,
            tgtID, tgtRtti, dstID, dstRtti, folID, folRtti);
    ev[OFF_PLANFLAG] = (char)(plan ? 1 : 0);
}

int OU_FireMega(int houseIdx, int srcID, unsigned srcRtti, int mission,
                int tgtID, unsigned tgtRtti, int dstID, unsigned dstRtti,
                int folID, unsigned folRtti, int plan,
                void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    OU_MakeMega(ev, houseIdx, srcID, srcRtti, mission,
                tgtID, tgtRtti, dstID, dstRtti, folID, folRtti, plan);
    return OT_Fire(ev, unit, uid, idxOk);
}

int OU_Enqueue(char ev[EVT_SIZE])
{
    unsigned count;
    unsigned next;
    /* SP: 一律直写 DoList; MP 走 OutList, 满回 0。 */
    if (!OU_IsMP()) return OU_DirectToDoList(ev);
    count = *(unsigned*)ADDR_OUT_COUNT;
    if (count >= EVT_CAP) return 0;
    next = *(unsigned*)ADDR_OUT_NEXT & 0x7Fu;
    OU_CopyEvt((char*)(ADDR_OUT_BUF + next * EVT_SIZE), ev);
    *(unsigned*)(ADDR_OUT_PAR + next * 4u) = OU_SndTimings();
    *(unsigned*)ADDR_OUT_NEXT = (next + 1u) & 0x7Fu;
    *(unsigned*)ADDR_OUT_COUNT = count + 1u;
    return 1;
}

int OU_CellValid(int cx, int cy)
{
    return OU_CellPtr(cx, cy) ? 1 : 0;
}

/* 格地面高度 (lep): CellClass::GetFloorHeight 0x47B3A0 —— thiscall 传
 * Point2D* 单指针 + ret 4, 与按值 8 字节严格区分。 */
typedef int __attribute__((thiscall)) (*Fn_FloorH)(void* cell, const Point2D* p);
#define FN_FLOORH ((Fn_FloorH)(void*)0x47B3A0u)

int OU_CellFloorZ(int cx, int cy, int* zOut)
{
    void* cell = OU_CellPtr(cx, cy);
    Point2D zero;
    int z;
    if (!cell || !IsPlausiblePtr(cell)) return 0;
    zero.X = 0;
    zero.Y = 0;
    z = FN_FLOORH(cell, &zero);
    if (z < -8192 || z > 8192) return 0;
    *zOut = z;
    return 1;
}

/* DynamicVector 元素数 (守卫后)。items 恒被写 (失败置 NULL)。 */
int OU_VecItems(unsigned vecAddr, void*** itemsOut)
{
    void** items;
    int cnt;
    *itemsOut = 0;
    cnt = *(int*)(size_t)(vecAddr + OFF_VEC_COUNT);
    items = *(void***)(size_t)(vecAddr + OFF_VEC_ITEMS);
    if (cnt <= 0 || cnt > 20000) return 0;
    if (!items || !IsPlausiblePtr(items)) return 0;
    *itemsOut = items;
    return cnt;
}

/* ---- 玩家主色 (只读) ----
 * HouseClass::ColorSchemeIndex (+0x16054) → ColorScheme::Array[..].Colors[MainShadeIndex]。
 * 与 ESC 玩家色色块、单位颜色是同一套。 */

int OU_HouseColor(void* house, int* r, int* g, int* b)
{
    void** items;
    unsigned char* sc;
    int cnt, idx, main;

    if (!house || !IsPlausiblePtr(house)) return 0;
    idx = *(int*)((char*)house + OFF_HOUSE_COLORSCHEME);
    items = *(void***)(size_t)ADDR_SCHEME_ITEMS;
    cnt = *(int*)(size_t)ADDR_SCHEME_COUNT;
    if (!items || !IsPlausiblePtr(items) || cnt <= 0 || cnt > 1000) return 0;
    if (idx < 0 || idx >= cnt) return 0;
    sc = (unsigned char*)items[idx];
    if (!sc || !IsPlausiblePtr(sc)) return 0;
    main = *(int*)(sc + OFF_SCHEME_MAIN);
    if (main < 0 || main > 255) return 0;
    if (r) *r = sc[OFF_SCHEME_COLORS + main * 3 + 0];
    if (g) *g = sc[OFF_SCHEME_COLORS + main * 3 + 1];
    if (b) *b = sc[OFF_SCHEME_COLORS + main * 3 + 2];
    return 1;
}

int OU_HouseIndexColor(int houseId, int* r, int* g, int* b)
{
    void** houses;
    int cnt;

    houses = *(void***)(size_t)ADDR_HOUSES_ITEMS;
    cnt = *(int*)(size_t)ADDR_HOUSES_COUNT;
    if (!houses || !IsPlausiblePtr(houses) || cnt <= 0 || cnt > 64) return 0;
    if (houseId < 0 || houseId >= cnt) return 0;
    return OU_HouseColor(houses[houseId], r, g, b);
}

/* p 是否该 DynamicVector 的成员 (静态类型数组判定: 只比指针, 不解引用)。 */
int OU_InVec(unsigned vecAddr, const void* p)
{
    void** items;
    int i, cnt;
    if (!p) return 0;
    cnt = OU_VecItems(vecAddr, &items);
    for (i = 0; i < cnt; i++)
        if (items[i] == p) return 1;
    return 0;
}

/* ---- 类型指针自校准 (SmartArea/SmartOrder/TreeChop 共用, 声明见 OrderUtil.h) ----
 * pin 缓存留在各模块, 经出参传入; 判定顺序与各模块原实现逐行等价。 */

int OU_StrOk(const char* s)
{
    int n = 0;
    if (!s) return 0;
    while (n < 24)
    {
        char c = s[n];
        if (!c) return (n > 0) ? 1 : 0;
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '-')) return 0;
        n++;
    }
    return 0;
}

int OU_TypeID(void* p, char* out, int* idOff)
{
    static const unsigned cands[3] = { 0x20u, 0x24u, 0x28u };
    int i;
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (*idOff >= 0)
    {
        const char* s = (const char*)p + (unsigned)*idOff;
        int k;
        if (!OU_StrOk(s)) { *idOff = -1; }
        else
        {
            for (k = 0; k < 24; k++) out[k] = s[k];
            return 1;
        }
    }
    for (i = 0; i < 3; i++)
    {
        const char* s = (const char*)p + cands[i];
        int k;
        if (!OU_StrOk(s)) continue;
        *idOff = (int)cands[i];
        for (k = 0; k < 24; k++) out[k] = s[k];
        return 1;
    }
    return 0;
}

/* Type 指针扫描核心 (单 pin): pin 命中直验, 任一判据不过即弃 pin 回扫。 */
void* OU_ScanTypeOf(void* u, int* pinnedOff, unsigned vecAddr, int* idOff)
{
    unsigned off;
    char idb[24];
    int k;
    if (*pinnedOff >= 0)
    {
        void* p = *(void**)((char*)u + (unsigned)*pinnedOff);
        if (p && IsPlausiblePtr(p) && OU_InVec(vecAddr, p) && OU_TypeID(p, idb, idOff))
            return p;
        *pinnedOff = -1;
    }
    for (off = 0x40u; off < 0x900u; off += 4u)
    {
        void* p = *(void**)((char*)u + off);
        if (!p || !IsPlausiblePtr(p)) continue;
        if (!OU_InVec(vecAddr, p)) continue;
        for (k = 0; k < 24; k++) idb[k] = 0;
        if (!OU_TypeID(p, idb, idOff)) continue;
        *pinnedOff = (int)off;
        return p;
    }
    return 0;
}

/* Techno 三类别槽位 (0=Unit 1=Aircraft 2=Infantry, 未知 -1)。 */
static int OU_ClassSlot(int w)
{
    if (w == ABS_UNIT) return 0;
    if (w == ABS_AIRCRAFT) return 1;
    if (w == ABS_INFANTRY) return 2;
    return -1;
}

void* OU_TypeOf(void* u, int w, int* off3, unsigned vecAddr, int* idOff)
{
    int slot = OU_ClassSlot(w);
    if (slot < 0) return 0;
    return OU_ScanTypeOf(u, &off3[slot], vecAddr, idOff);
}

int OU_CalOwner(void* u0, void* u1, void* house, int* ownerOff)
{
    unsigned off;
    if (!u0 || !house) return 0;
    if (*ownerOff >= 0)
    {
        if (*(void**)((char*)u0 + (unsigned)*ownerOff) == house &&
            (!u1 || *(void**)((char*)u1 + (unsigned)*ownerOff) == house))
            return 1;
        *ownerOff = -1;
    }
    for (off = 0x40u; off < 0x900u; off += 4u)
    {
        if (*(void**)((char*)u0 + off) != house) continue;
        if (u1 && *(void**)((char*)u1 + off) != house) continue;
        *ownerOff = (int)off;
        return 1;
    }
    return 0;
}

/* ---- TARGET 编码 (8 字节: id@0, rtti@4) ----
 * 每个要送进 MegaMission 的目标槽都得先清缓冲再调 FN_TGT_OBJ/FN_TGT_CELL,
 * 然后取 id/rtti。 */

int OU_ObjTgt(void* obj, unsigned char* rttiOut)
{
    char tb[8];
    int i;
    for (i = 0; i < 8; i++) tb[i] = 0;
    if (!obj || !FN_TGT_OBJ(tb, obj)) return -1;
    if (rttiOut) *rttiOut = (unsigned char)tb[4];
    return *(int*)(tb + 0);
}

int OU_ObjId(void* obj)
{
    int id = OU_ObjTgt(obj, 0);
    return id < 0 ? 0 : id;
}

/* 格编码: 引擎不报失败 (空格也填, id=0), 与原调用点一致不判返回值。 */
int OU_CellTgt(int cx, int cy, unsigned char* rttiOut)
{
    char tb[8];
    CellStruct cs;
    int i;
    for (i = 0; i < 8; i++) tb[i] = 0;
    cs.X = (short)cx;
    cs.Y = (short)cy;
    FN_TGT_CELL(tb, &cs);
    if (rttiOut) *rttiOut = (unsigned char)tb[4];
    return *(int*)(tb + 0);
}

/* 鼠标屏坐标 → 格 + 点中对象。filterTgt 非 0 时只透出单位/建筑类对象。 */
static int OU_MouseCellInner(int* cx, int* cy, void** tgtOut, int* px, int* py,
                             int filterTgt, int* zOut)
{
    int mx, my;
    Point2D pt;
    CellStruct center;
    CoordStruct crd;
    void* tgtObj = 0;
    unsigned char b5 = 0, b6 = 0;
    center.X = center.Y = 0;
    crd.X = crd.Y = crd.Z = 0;
    if (tgtOut) *tgtOut = 0;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    pt.X = mx; pt.Y = my;
    /* ProcessClickCoords 按视口空间判格, GtMouseGameXY 给物理点,
     * 放大档须先过缩放反变换 (scale=1 恒等); 栏带不进引擎查询。 */
    if (!SB_MapPoint(&pt)) return 0;
    if (!FN_CLICKCOORDS((void*)ADDR_DISPLAY, &pt, &center, &crd,
                        &tgtObj, &b5, &b6))
        return 0;
    *cx = (int)center.X; *cy = (int)center.Y;
    if (px) *px = mx;
    if (py) *py = my;
    if (zOut) *zOut = crd.Z;
    if (!OU_CellValid(*cx, *cy)) return 0;
    if (tgtOut && tgtObj && IsPlausiblePtr(tgtObj))
    {
        if (!filterTgt) *tgtOut = tgtObj;
        else
        {
            int w = OU_WhatAmI(tgtObj);
            if (w == ABS_UNIT || w == ABS_INFANTRY ||
                w == ABS_AIRCRAFT || w == ABS_BUILDING)
                *tgtOut = tgtObj;
        }
    }
    return 1;
}

int OU_MouseCellPx(int* cx, int* cy, int* px, int* py)
{
    return OU_MouseCellInner(cx, cy, 0, px, py, 0, 0);
}

/* 格 + 光标地面高度 (越界高度归零)。 */
int OU_MouseCellZ(int* cx, int* cy, int* lz)
{
    if (!OU_MouseCellInner(cx, cy, 0, 0, 0, 1, lz)) return 0;
    if (*lz < -8192 || *lz > 8192) *lz = 0;
    return 1;
}

int OU_MouseCell(int* cx, int* cy)
{
    return OU_MouseCellPx(cx, cy, 0, 0);
}

/* 取格 + 点中对象 (tgtOut 可空, 无目标写 0, 返回值仍是格有效性)。 */
int OU_MouseCellTarget(int* cx, int* cy, void** tgtOut, int* px, int* py)
{
    return OU_MouseCellInner(cx, cy, tgtOut, px, py, 1, 0);
}

typedef int __attribute__((thiscall)) (*Fn_MouseOverObj)(void* self, void* target, int ignoreForce);

/* vtbl+0x74 直调, ignoreForce 透传。
 * ignoreForce=1 时无视 Ctrl/Shift/Alt 修饰键, 取单位固有动作
 * (反汇编实证: 步兵 0x51E3B0 开头读该参数, 非零即跳过两处按键检查)。
 * 能力判定必须用 1 (FuzzyAttack 按住 Ctrl / SmartArea 按住 Shift 时
 * 0 会被污染成强制攻击/ToggleSelect)。 */
static int OU_MouseOverRaw(void* self, void* target, int ignoreForce)
{
    void** vt;
    Fn_MouseOverObj fn;
    if (!self || !IsPlausiblePtr(self)) return -1;
    if (!target || !IsPlausiblePtr(target)) return -1;
    vt = *(void***)self;
    if (!vt || !IsPlausiblePtr(vt)) return -1;
    fn = (Fn_MouseOverObj)vt[0x74 / 4];
    if (!fn || !IsPlausiblePtr((const void*)fn)) return -1;
    return fn(self, target, ignoreForce);
}

/* vtbl+0x74 = MouseOverObject (DecideAction 内部同入口, 只读不下单)。 */
int OU_MouseOverObject(void* self, void* target)
{
    return OU_MouseOverRaw(self, target, 0);
}

/* 同上, ignoreForce=1: 无视物理修饰键取固有能力
 * (组合键热键必须用这个, 见 .h)。 */
int OU_MouseOverObjectNative(void* self, void* target)
{
    return OU_MouseOverRaw(self, target, 1);
}

/* 复刻引擎 Class::ObjectClickedAction 的"类内重映射" (见 .h 与 notes/侵略模式.md)。
 * actWhat=OU_WhatAmI(下令单位); 未覆盖的类原样返回 (无兵种特判)。 */
int OU_RemapAction(int actWhat, int action)
{
    if (actWhat == ABS_INFANTRY)
    {
        /* 表 0x51F20C: val1→Capture9, val2→Attack5, 其余不变。 */
        if (action == 28 || action == ACTION_GREPAIR) return ACTION_CAPTURE;
        if (action == ACTION_HEAL || action == ACTION_F57) return ACTION_ATTACK;
        return action;
    }
    if (actWhat == ABS_UNIT)
    {
        /* 0x738890: 3→Move1, 9/16/27/29→Attack5。 */
        if (action == ACTION_ENTER) return ACTION_MOVE;
        if (action == ACTION_CAPTURE || action == ACTION_SABOTAGE ||
            action == ACTION_HEAL || action == ACTION_GREPAIR) return ACTION_ATTACK;
        return action;
    }
    return action;   /* Aircraft/Building: 无类内重映射 */
}

/* 复刻引擎 ③: 重映射后的 Action → Mission (FootClass 0x4D74E0 表)。
 * 只列本模块可安全重放且目标可达的类别; 其余回 -1 (引擎无操作/未知)。 */
int OU_NativeMission(void* unit, void* target)
{
    int act;
    if (!unit || !IsPlausiblePtr(unit)) return -1;
    act = OU_MouseOverRaw(unit, target, 1);
    if (act < 0) return -1;
    act = OU_RemapAction(OU_WhatAmI(unit), act);
    switch (act)
    {
    case ACTION_ATTACK:
    case 53: case 63: case 64: case 71:   /* 引擎同归 Mission::Attack(1) */
        return MISSION_ATTACK;
    case ACTION_MOVE:     return MISSION_MOVE;       /* 1/62 → Mission 2 */
    case ACTION_ENTER:    return MISSION_ENTER;      /* 3 → 7  (Destination) */
    case ACTION_CAPTURE:  return MISSION_CAPTURE;    /* 9 → 8  (Destination) */
    case ACTION_SABOTAGE: return MISSION_SABOTAGE;   /* 16 → 17 (Destination) */
    default:              return -1;
    }
}

/* 该类对目标是否有攻击类光标 (会下 Mission::Attack)。
 * 只读 MouseOver (ignoreForce=1); 复刻引擎: 光标先过类内重映射再判,
 * 工程师对血厚敌楼的 Damage(28) 重映射成 Capture(9), 不再误判成攻击。 */
int OU_CanAttack(void* unit, void* target)
{
    int uw, tw;
    if (!unit || !IsPlausiblePtr(unit)) return 0;
    if (!target || !IsPlausiblePtr(target)) return 0;
    uw = OU_WhatAmI(unit);
    if (uw != ABS_UNIT && uw != ABS_INFANTRY && uw != ABS_AIRCRAFT) return 0;
    tw = OU_WhatAmI(target);
    if (tw != ABS_UNIT && tw != ABS_INFANTRY &&
        tw != ABS_AIRCRAFT && tw != ABS_BUILDING) return 0;
    return (OU_NativeMission(unit, target) == MISSION_ATTACK) ? 1 : 0;
}

/* 该单位对该目标是否有进入类光标 (Action::Enter)。
 * 只读 MouseOver (ignoreForce=1, 无视修饰键取固有能力)。 */
int OU_CanEnter(void* unit, void* target)
{
    int tw, act;
    if (!unit || !IsPlausiblePtr(unit)) return 0;
    if (!target || !IsPlausiblePtr(target)) return 0;
    tw = OU_WhatAmI(target);
    if (tw != ABS_UNIT && tw != ABS_INFANTRY &&
        tw != ABS_AIRCRAFT && tw != ABS_BUILDING) return 0;
    act = OU_MouseOverRaw(unit, target, 1);
    return (act == ACTION_ENTER) ? 1 : 0;
}

/* 该单位对该建筑是否有维修类光标 (Action::GRepair)。
 * 只读 MouseOver (ignoreForce=1, 无视修饰键取固有能力)。 */
int OU_CanGRepair(void* unit, void* target)
{
    int tw, act;
    if (!unit || !IsPlausiblePtr(unit)) return 0;
    if (!target || !IsPlausiblePtr(target)) return 0;
    tw = OU_WhatAmI(target);
    if (tw != ABS_BUILDING) return 0;
    act = OU_MouseOverRaw(unit, target, 1);
    return (act == 29) ? 1 : 0;
}

typedef CoordStruct* __attribute__((thiscall)) (*Fn_GetCoords)(void* self, CoordStruct* out);

#define OU_LEP_PER_CELL 256
#define OU_MAP_CELLS 512

/* vtbl+0x48 取坐标 + 范围校验 (地图 512 格内, |z| < 8192)。 */
int OU_GetCoords(void* p, CoordStruct* out)
{
    void** vt;
    Fn_GetCoords fn;
    CoordStruct cs;
    if (!p || !IsPlausiblePtr(p) || !out) return 0;
    vt = *(void***)p;
    if (!vt || !IsPlausiblePtr(vt)) return 0;
    fn = (Fn_GetCoords)vt[0x48 / 4];
    if (!fn || !IsPlausiblePtr((const void*)fn)) return 0;
    cs.X = cs.Y = cs.Z = 0;
    fn(p, &cs);
    if (cs.X < 0 || cs.Y < 0 ||
        cs.X >= OU_MAP_CELLS * OU_LEP_PER_CELL ||
        cs.Y >= OU_MAP_CELLS * OU_LEP_PER_CELL)
        return 0;
    if (cs.Z < -8192 || cs.Z > 8192) return 0;
    *out = cs;
    return 1;
}

typedef int __attribute__((thiscall)) (*Fn_DecideAction)(void* disp, void* cell, void* obj, int unk);
#define FN_DECID ((Fn_DecideAction)(void*)0x692610u)

int OU_DecideAction(int cx, int cy, void* tgt)
{
    CellStruct cs;
    int r;
    if (!tgt || !IsPlausiblePtr(tgt)) return -1;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return -1;
    cs.X = (short)cx; cs.Y = (short)cy;
    r = FN_DECID((void*)ADDR_DISPLAY, &cs, tgt, 1);
    if (r < 0 || r > 80) return -1;
    return r;
}

int OU_PtrAliveRaw(void* u, int id)
{
    void* base;
    int cnt, lo, hi;
    if (!u || id <= 0) return 0;
    base = *(void**)ADDR_IDBASE;
    cnt = *(int*)ADDR_IDCOUNT;
    if (!base || !IsPlausiblePtr(base) || cnt <= 0 || cnt > 200000) return 0;
    lo = 0; hi = cnt;
    while (lo < hi)
    {
        int mid = lo + ((hi - lo) >> 1);
        char* e = (char*)base + (unsigned)mid * 8u;
        int eid = *(int*)(e + 0);
        if (eid < id) lo = mid + 1;
        else if (eid > id) hi = mid;
        else return (*(void**)(e + 4) == u) ? 1 : 0;
    }
    return 0;
}

int OU_PtrAlive(void* u, int id, int idxOk)
{
    if (!idxOk) return 1;
    return OU_PtrAliveRaw(u, id);
}

/* 存活三连 (IsAlive/InLimbo/Health; 不碰 Fetch_ID 索引)。 */
int OU_TechAliveRaw(void* p)
{
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (!((unsigned char*)p)[OFF_ISALIVE]) return 0;
    if (((unsigned char*)p)[OFF_INLIMBO]) return 0;
    if (*(int*)((char*)p + OFF_HEALTH) <= 0) return 0;
    return 1;
}

/* 中立阵营判定 (House->Type->MultiplayPassive)。读不到按非中立。 */
int OU_HouseNeutral(void* h)
{
    void* t;
    if (!h || !IsPlausiblePtr(h)) return 0;
    t = *(void**)((char*)h + OFF_HOUSE_TYPE);
    if (!IsPlausiblePtr(t)) return 0;
    return (*(unsigned char*)((char*)t + OFF_HTYPE_PASSIVE)) ? 1 : 0;
}

/* 单位 PlanningToken 上的节点数 (只读; token 不存在/野指针回 0)。 */
int OU_TokenNodes(void* unit)
{
    void* t;
    void** arr;
    int n;
    if (!unit || !IsPlausiblePtr(unit)) return 0;
    t = *(void**)((char*)unit + OFF_TECH_TOKEN);
    if (!t || !IsPlausiblePtr(t)) return 0;
    arr = *(void***)((char*)t + OFF_TOK_NODES);
    n = *(int*)((char*)t + OFF_TOK_NCOUNT);
    if (!arr || !IsPlausiblePtr(arr) || n < 0 || n > 1000) return 0;
    return n;
}

/* ---------------- 规划线共享 (红绿线投影/取高/细线/线缓冲) ---------------- */

typedef int __attribute__((thiscall)) (*Fn_OUC2C)(void* tac,
    const CoordStruct* c, Point2D* out);
typedef int __attribute__((thiscall)) (*Fn_OUFloorZAt)(void* map, const CoordStruct* c);
#define FN_OU_C2C ((Fn_OUC2C)(void*)0x6D2140u)
#define OU_TAC_INSTANCE 0x887324u
#define FN_OU_FLOORZAT ((Fn_OUFloorZAt)(void*)0x578080u)

/* CellClass::Flags +0x140, BridgeHead|BridgeBody; 桥高
 * CellClass::BridgeLevels(4) * Unsorted::LevelHeight(104)。 */
#define OFF_OU_CELLFLAGS 0x140u
#define OU_CELLFLAG_BRIDGE 0x500u
#define OU_BRIDGE_HEIGHT 416

/* 世界点 → 屏像素 (含高度; CoordsToClient 给渲染面坐标, 缩放档再折到屏幕)。 */
int OU_ProjWorld(int lx, int ly, int lz, int* px, int* py)
{
    void* tac = *(void**)OU_TAC_INSTANCE;
    CoordStruct c;
    Point2D o;
    if (!px || !py) return 0;
    if (!tac || !IsPlausiblePtr(tac)) return 0;
    c.X = lx; c.Y = ly; c.Z = lz;
    o.X = 0; o.Y = 0;
    FN_OU_C2C(tac, &c, &o);
    if (!SB_ViewToScreen(o.X, o.Y, px, py)) { *px = o.X; *py = o.Y; }
    return 1;
}

/* 指定 lepton 坐标处的地面高度 (MapClass::GetCellFloorHeight 0x578080, 只读)。
 * 按 X/Y 低 8 位采子格, 比 OU_CellFloorZ 的格角更精确。 */
int OU_GroundZAt(int x, int y, int* zOut)
{
    CoordStruct c;
    int z;
    if (!zOut) return 0;
    if (x < 0 || y < 0 ||
        x >= OU_MAP_CELLS * OU_LEP_PER_CELL ||
        y >= OU_MAP_CELLS * OU_LEP_PER_CELL)
        return 0;
    c.X = x; c.Y = y; c.Z = 0;
    z = FN_OU_FLOORZAT((void*)ADDR_DISPLAY, &c);
    if (z < -8192 || z > 8192) return 0;
    *zOut = z;
    return 1;
}

/* 可站立表面高度: GetCellFloorHeight 不含桥高。topBridge=1 时有桥直接取桥面,
 * 否则按 refZ 取更接近的一层 (桥下单位仍取地形)。只读。 */
static int OU_SurfaceZRaw(int x, int y, int refZ, int topBridge, int* zOut)
{
    void* cell;
    int z, bz, dg, db;
    if (!OU_GroundZAt(x, y, &z)) return 0;
    cell = OU_CellPtr(x >> 8, y >> 8);
    if (cell && IsPlausiblePtr(cell) &&
        (*(unsigned*)((char*)cell + OFF_OU_CELLFLAGS) & OU_CELLFLAG_BRIDGE))
    {
        bz = z + OU_BRIDGE_HEIGHT;
        if (topBridge) z = bz;
        else
        {
            dg = refZ - z; if (dg < 0) dg = -dg;
            db = refZ - bz; if (db < 0) db = -db;
            if (db <= dg) z = bz;
        }
    }
    if (z < -8192 || z > 8192) return 0;
    *zOut = z;
    return 1;
}

int OU_SurfaceZAt(int x, int y, int refZ, int* zOut)
{
    return OU_SurfaceZRaw(x, y, refZ, 0, zOut);
}

int OU_CellSurfaceZ(int cx, int cy, int* zOut)
{
    if (cx < 0 || cy < 0 || cx >= OU_MAP_CELLS || cy >= OU_MAP_CELLS) return 0;
    return OU_SurfaceZRaw(cx * OU_LEP_PER_CELL + OU_LEP_PER_CELL / 2,
                          cy * OU_LEP_PER_CELL + OU_LEP_PER_CELL / 2,
                          0, 1, zOut);
}

/* ---------------- 手绘几何 (16 位面, 逐像素 Blend16) ----------------
 * Bresenham / 画点 / 实心方块, 全 DLL 只此一份。
 * thick 的纵向外扩**居中** (thick=1 单行, 3 即 y-1/y/y+1)。
 *
 * clip 传 NULL 表示不裁剪; 否则只画 [x0,x1) × [y0,y1) 内的像素 (GridView 用)。
 * 调用方给的 locked/w/h/pitch 是当前锁定的 16 位面, 与本文件其它绘制同口径。 */

int OU_Put(unsigned char* locked, int w, int h, int pitch,
           const OuClip* clip, int x, int y,
           unsigned r, unsigned g, unsigned b, unsigned a)
{
    unsigned short* row;
    if (!locked || x < 0 || y < 0 || x >= w || y >= h) return 0;
    if (clip && (x < clip->x0 || x >= clip->x1 ||
                 y < clip->y0 || y >= clip->y1)) return 0;
    row = (unsigned short*)(locked + (size_t)y * pitch);
    row[x] = Blend16(row[x], r, g, b, a);
    return 1;
}

void OU_Line(unsigned char* locked, int w, int h, int pitch,
             const OuClip* clip, int x0, int y0, int x1, int y1,
             unsigned r, unsigned g, unsigned b, unsigned a, int thick)
{
    int dx, dy, sx, sy, err, e2, x, y, n, k, lo;
    if (!locked || !IsPlausiblePtr(locked)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    if (thick < 1) thick = 1;
    if (thick > 8) thick = 8;
    lo = -((thick - 1) / 2);              /* 居中: thick=1 → +0, 3 → -1..+1 */
    dx = x1 - x0; dy = y1 - y0;
    sx = (dx >= 0) ? 1 : -1;
    sy = (dy >= 0) ? 1 : -1;
    dx = dx >= 0 ? dx : -dx; dy = dy >= 0 ? dy : -dy;
    err = dx - dy;
    x = x0; y = y0;
    for (n = 0; n < 8192; n++)            /* 上限防呆: 远超屏幕对角, 防脏坐标死循环 */
    {
        for (k = 0; k < thick; k++)
            OU_Put(locked, w, h, pitch, clip, x, y + lo + k, r, g, b, a);
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

void OU_Box(unsigned char* locked, int w, int h, int pitch,
            const OuClip* clip, int cx, int cy, int m,
            unsigned r, unsigned g, unsigned b, unsigned a)
{
    int k, j;
    for (k = -m; k <= m; k++)
        for (j = -m; j <= m; j++)
            OU_Put(locked, w, h, pitch, clip, cx + k, cy + j, r, g, b, a);
}

/* 虚线: 沿线段每 (dash+gap) 像素画 dash 段实线, 其余留空 (风格贴近引擎聚集点线)。 */
void OU_ThinLineDash(unsigned char* locked, int w, int h, int pitch,
                     int x0, int y0, int x1, int y1,
                     unsigned r, unsigned g, unsigned b, unsigned a,
                     int thick, int dash, int gap)
{
    int dx, dy, adx, ady, n, i, period;
    if (!locked || !IsPlausiblePtr(locked)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    if (thick < 1) thick = 1;
    if (thick > 4) thick = 4;
    if (dash < 1) dash = 6;
    if (gap < 1) gap = 4;
    period = dash + gap;
    dx = x1 - x0; dy = y1 - y0;
    adx = dx >= 0 ? dx : -dx; ady = dy >= 0 ? dy : -dy;
    n = adx > ady ? adx : ady;
    if (n <= 0) n = 1;
    for (i = 0; i <= n; i++)
    {
        int px, py, k;
        if ((i % period) >= dash) continue;
        px = x0 + (int)(((long long)dx * i) / n);
        py = y0 + (int)(((long long)dy * i) / n);
        if (px < 0 || py < 0 || px >= w || py >= h) continue;
        for (k = 0; k < thick; k++)
        {
            unsigned short* row;
            int yy = py + k;
            if (yy >= h) break;
            row = (unsigned short*)(locked + yy * pitch);
            row[px] = Blend16(row[px], r, g, b, a);
        }
    }
}

int OU_PrevPush(OU_PrevLine* buf, int* nio,
                int ulx, int uly, int ulz, int tlx, int tly, int tlz)
{
    int n;
    if (!buf || !nio) return 0;
    n = *nio;
    if (n < 0 || n >= OU_PREV_CAP) return 0;
    buf[n].ulx = ulx; buf[n].uly = uly; buf[n].ulz = ulz;
    buf[n].tlx = tlx; buf[n].tly = tly; buf[n].tlz = tlz;
    *nio = n + 1;
    return 1;
}

int OU_PrevPushObjs(OU_PrevLine* buf, int* nio, void* u, void* t)
{
    CoordStruct a, b;
    if (!OU_GetCoords(u, &a) || !OU_GetCoords(t, &b)) return 0;
    return OU_PrevPush(buf, nio, a.X, a.Y, a.Z, b.X, b.Y, b.Z);
}

int OU_PrevPushCell(OU_PrevLine* buf, int* nio, void* u, int cx, int cy)
{
    CoordStruct a;
    int gz;
    if (!OU_GetCoords(u, &a)) return 0;
    if (!OU_CellSurfaceZ(cx, cy, &gz)) gz = 0;
    return OU_PrevPush(buf, nio, a.X, a.Y, a.Z,
                       cx * OU_LEP_PER_CELL + OU_LEP_PER_CELL / 2,
                       cy * OU_LEP_PER_CELL + OU_LEP_PER_CELL / 2, gz);
}

void OU_PrevDraw(OU_PrevLine* buf, int n, unsigned char* locked, int w, int h, int pitch,
                 unsigned r, unsigned g, unsigned b, unsigned a, int thick)
{
    void* tac;
    int k;
    if (!buf || n <= 0) return;
    if (n > OU_PREV_CAP) n = OU_PREV_CAP;
    tac = *(void**)OU_TAC_INSTANCE;
    if (!tac || !IsPlausiblePtr(tac)) return;
    for (k = 0; k < n; k++)
    {
        int ux, uy, tx, ty;
        if (!OU_ProjWorld(buf[k].ulx, buf[k].uly, buf[k].ulz, &ux, &uy)) continue;
        if (!OU_ProjWorld(buf[k].tlx, buf[k].tly, buf[k].tlz, &tx, &ty)) continue;
        OU_Line(locked, w, h, pitch, 0, ux, uy, tx, ty, r, g, b, a, thick);
    }
}

/* 同 OU_PrevDraw, 但画虚线 (dash 实 / gap 空 像素)。 */
void OU_PrevDrawDash(OU_PrevLine* buf, int n, unsigned char* locked, int w, int h, int pitch,
                     unsigned r, unsigned g, unsigned b, unsigned a,
                     int thick, int dash, int gap)
{
    void* tac;
    int k;
    if (!buf || n <= 0) return;
    if (n > OU_PREV_CAP) n = OU_PREV_CAP;
    tac = *(void**)OU_TAC_INSTANCE;
    if (!tac || !IsPlausiblePtr(tac)) return;
    for (k = 0; k < n; k++)
    {
        int ux, uy, tx, ty;
        if (!OU_ProjWorld(buf[k].ulx, buf[k].uly, buf[k].ulz, &ux, &uy)) continue;
        if (!OU_ProjWorld(buf[k].tlx, buf[k].tly, buf[k].tlz, &tx, &ty)) continue;
        OU_ThinLineDash(locked, w, h, pitch, ux, uy, tx, ty, r, g, b, a, thick, dash, gap);
    }
}

int OU_PrevThrottle(unsigned* lastMs, int* lastCx, int* lastCy, int* lastKey,
                    int cx, int cy, int key, unsigned minMs, unsigned refreshMs)
{
    unsigned now;
    if (!lastMs || !lastCx || !lastCy || !lastKey) return 0;
    now = GetTickCount();
    if (now - *lastMs < minMs) return 0;
    if (cx == *lastCx && cy == *lastCy && key == *lastKey &&
        now - *lastMs < refreshMs) return 0;
    *lastMs = now;
    *lastCx = cx; *lastCy = cy; *lastKey = key;
    return 1;
}

/* ---------------- 热键串 (照 AutoLoadDev Hotkey= 写法, 无 CRT) ---------------- */

static int OU_Lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int OU_StrEqNI(const char* a, const char* b)
{
    while (*a && *b)
    {
        if (OU_Lower((unsigned char)*a) != OU_Lower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* 单个键名 -> VK; 0 = 不认识 (多位纯数字按裸 VK 数值处理, 见 OU_ParseHotkey)。 */
static unsigned OU_MapKeyToken(const char* t)
{
    static const struct { const char* name; unsigned vk; } named[] =
    {
        { "Space", VK_SPACE },  { "Tab", VK_TAB },      { "Enter", VK_RETURN },
        { "Return", VK_RETURN },{ "Backspace", VK_BACK },{ "Back", VK_BACK },
        { "Insert", VK_INSERT },{ "Ins", VK_INSERT },   { "Delete", VK_DELETE },
        { "Del", VK_DELETE },   { "Home", VK_HOME },    { "End", VK_END },
        { "PageUp", VK_PRIOR }, { "PgUp", VK_PRIOR },   { "PageDown", VK_NEXT },
        { "PgDn", VK_NEXT },    { "Up", VK_UP },        { "Down", VK_DOWN },
        { "Left", VK_LEFT },    { "Right", VK_RIGHT },  { "Pause", VK_PAUSE },
        { "Num0", VK_NUMPAD0 }, { "Num1", VK_NUMPAD1 }, { "Num2", VK_NUMPAD2 },
        { "Num3", VK_NUMPAD3 }, { "Num4", VK_NUMPAD4 }, { "Num5", VK_NUMPAD5 },
        { "Num6", VK_NUMPAD6 }, { "Num7", VK_NUMPAD7 }, { "Num8", VK_NUMPAD8 },
        { "Num9", VK_NUMPAD9 },
        { "Middle", VK_MBUTTON }, { "MMB", VK_MBUTTON },
        { "LMB", VK_LBUTTON },  { "RMB", VK_RBUTTON },
        { "X1", VK_XBUTTON1 },  { "X2", VK_XBUTTON2 },
    };
    int i, n = 0;
    while (t[n]) n++;
    if (n == 1)
    {
        unsigned char c = (unsigned char)t[0];
        if (c >= 'a' && c <= 'z') return (unsigned)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return (unsigned)c;
        if (c >= '0' && c <= '9') return (unsigned)c;
        return 0;
    }
    if (n >= 2 && (t[0] == 'F' || t[0] == 'f'))
    {
        int num = 0, ok = 1;
        for (i = 1; i < n; i++)
        {
            if (t[i] < '0' || t[i] > '9') { ok = 0; break; }
            num = num * 10 + (t[i] - '0');
        }
        if (ok && num >= 1 && num <= 24) return (unsigned)(VK_F1 + num - 1);
    }
    for (i = 0; i < (int)(sizeof(named) / sizeof(named[0])); i++)
        if (OU_StrEqNI(t, named[i].name)) return named[i].vk;
    return 0;
}

int OU_ParseHotkey(const char* spec, OU_Hotkey* out)
{
    char toks[8][16];
    int ntok = 0, i;
    unsigned mods = 0;
    unsigned keys[8];
    int nkey = 0;
    const char* p;
    if (!spec || !out) return 0;
    out->vk = 0; out->mods = 0; out->nExtra = 0; out->disabled = 0;
    /* 注: 行尾 ;注释由 OU_ReadHotkey 在加载时统一剥掉, 这里只做纯语法解析。 */
    if (OU_StrEqNI(spec, "None") || OU_StrEqNI(spec, "Off") ||
        OU_StrEqNI(spec, "Disabled"))
    {
        out->disabled = 1;
        return 1;
    }
    p = spec;
    while (*p && ntok < 8)
    {
        int n = 0;
        while (*p == '+' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        while (*p && *p != '+' && *p != ' ' && *p != '\t' && n < 15)
            toks[ntok][n++] = *p++;
        toks[ntok][n] = 0;
        if (n) ntok++;
    }
    if (ntok == 1)
    {
        /* 多位纯数字 = 裸 TriggerVK 数值 (例 "80"=P, "4"=中键)。 */
        int alld = 1, v = 0;
        for (i = 0; toks[0][i]; i++)
        {
            if (toks[0][i] < '0' || toks[0][i] > '9') { alld = 0; break; }
            v = v * 10 + (toks[0][i] - '0');
        }
        if (alld && i > 1 && v >= 1 && v <= 255) { out->vk = (unsigned)v; return 1; }
    }
    for (i = 0; i < ntok; i++)
    {
        const char* t = toks[i];
        if (OU_StrEqNI(t, "Ctrl") || OU_StrEqNI(t, "Control")) { mods |= OU_MOD_CTRL; continue; }
        if (OU_StrEqNI(t, "Alt") || OU_StrEqNI(t, "Menu")) { mods |= OU_MOD_ALT; continue; }
        if (OU_StrEqNI(t, "Shift")) { mods |= OU_MOD_SHIFT; continue; }
        if (OU_StrEqNI(t, "Win") || OU_StrEqNI(t, "Windows")) { mods |= OU_MOD_WIN; continue; }
        {
            unsigned vk = OU_MapKeyToken(t);
            if (vk && nkey < 8) keys[nkey++] = vk;
        }
    }
    if (!nkey) return 0;
    out->mods = mods;
    out->vk = keys[nkey - 1];       /* 最后一个非修饰键是主键 */
    for (i = 0; i < nkey - 1 && i < OU_HK_MAXEXTRA; i++) out->extra[i] = keys[i];
    out->nExtra = (nkey - 1 < OU_HK_MAXEXTRA) ? nkey - 1 : OU_HK_MAXEXTRA;
    return 1;
}

/* 帧内缓存: GetAsyncKeyState 是 user32 系统调用 (~1us), 而一帧里十来个 Draw
 * 各查一遍 Ctrl/Shift/中键就是十几 us 白烧 (实测 sm/msgtail 段)。同一游戏帧
 * 内同一键只问一次系统; 暂停时帧号冻结 (迷你循环), 用 16ms 墙钟兜底重查,
 * 保证 F5 之类在暂停中照样灵敏。 */
static unsigned s_KeyFr[256];
static unsigned s_KeyTick = 0;
static unsigned char s_KeyDown[256];

int OU_KeyDown(unsigned vk)
{
    unsigned fr, now;
    int d;
    if (!vk || vk > 255) return 0;
    fr = *(volatile unsigned*)(size_t)ADDR_FRAME;   /* 引擎帧号 */
    now = GetTickCount();
    if (s_KeyFr[vk] == fr && (now - s_KeyTick) < 16u)
        return s_KeyDown[vk] ? 1 : 0;
    d = (GetAsyncKeyState((int)vk) & 0x8000) ? 1 : 0;
    s_KeyFr[vk] = fr;
    s_KeyTick = now;
    s_KeyDown[vk] = (unsigned char)d;
    return d;
}

int OU_ModsDown(unsigned mods)
{
    int down = 1;
    if (mods & OU_MOD_CTRL) down &= OU_KeyDown(VK_CONTROL);
    if (mods & OU_MOD_SHIFT) down &= OU_KeyDown(VK_SHIFT);
    if (mods & OU_MOD_ALT) down &= OU_KeyDown(VK_MENU);
    if (mods & OU_MOD_WIN) down &= (OU_KeyDown(VK_LWIN) || OU_KeyDown(VK_RWIN));
    return down;
}

int OU_ExtraDown(const OU_Hotkey* hk)
{
    int i;
    if (!hk) return 0;
    for (i = 0; i < hk->nExtra; i++)
        if (!OU_KeyDown(hk->extra[i])) return 0;
    return 1;
}

int OU_HotkeyDown(const OU_Hotkey* hk)
{
    if (!hk || hk->disabled) return 0;
    return OU_KeyDown(hk->vk) && OU_ModsDown(hk->mods) && OU_ExtraDown(hk);
}

void OU_ReadString(const char* section, const char* key, const char* def,
                   char* buf, int buflen)
{
    int i, j;
    if (!buf || buflen <= 0) return;
    buf[0] = 0;
    if (section && key)
        GetPrivateProfileStringA(section, key, "", buf, buflen, g_IniPath);
    buf[buflen - 1] = 0;
    /* 加载时统一清洗: 行尾 ;注释截掉 (ini 接口不剥行尾注释), 两端去空。 */
    for (i = 0; buf[i]; i++)
    {
        if (buf[i] == ';') { buf[i] = 0; break; }
    }
    while (i > 0 && (buf[i - 1] == ' ' || buf[i - 1] == '\t')) buf[--i] = 0;
    j = 0;
    while (buf[j] == ' ' || buf[j] == '\t') j++;
    if (j > 0)
    {
        for (i = 0; buf[j]; i++, j++) buf[i] = buf[j];
        buf[i] = 0;
    }
    if (!buf[0] && def)
    {
        for (i = 0; i < buflen - 1 && def[i]; i++) buf[i] = def[i];
        buf[i] = 0;
    }
}

int OU_ReadHotkey(const char* section, const char* key, OU_Hotkey* io)
{
    char buf[64];
    OU_Hotkey tmp;
    if (!section || !key || !io) return 0;
    OU_ReadString(section, key, "", buf, sizeof(buf));
    if (!buf[0]) return 0;          /* 缺键/空串: 调用方保留原值 */
    if (!OU_ParseHotkey(buf, &tmp)) return 0;   /* 坏串: 调用方保留原值 */
    *io = tmp;
    return 1;
}

/* 单行开关+热键 (缺键/空串/坏串=默认值启用; "0"=禁用; 其它按 Hotkey 串解析,
 * None 类=禁用)。先按 def 解析出默认值, 再读键覆盖。回 1=启用, 0=禁用。 */
int OU_ReadHotkeySingle(const char* section, const char* key,
                        const char* def, OU_Hotkey* io)
{
    char buf[64];
    OU_Hotkey tmp;
    if (!section || !key || !io) return 0;
    OU_ParseHotkey(def ? def : "", io);         /* 默认 (常量串, 必成功) */
    OU_ReadString(section, key, "", buf, sizeof(buf));
    if (!buf[0]) return io->disabled ? 0 : 1;   /* 缺键/空串: 留默认 */
    if (buf[0] == '0' && buf[1] == 0)           /* "0"=禁用 */
    {
        io->disabled = 1;
        return 0;
    }
    if (!OU_ParseHotkey(buf, &tmp))             /* 坏串: 留默认 */
        return io->disabled ? 0 : 1;
    *io = tmp;
    return tmp.disabled ? 0 : 1;
}
