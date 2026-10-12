/* OrderUtil.cpp - 下单模块共享工具实现 (声明见 OrderUtil.h)。
 * 只读查询 + 标准事件入队; 单机单位令直写 DoList, 联机只走 OutList。 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "ScaleBar.h"

#define ADDR_FRAME 0xA8ED84u   /* Unsorted::CurrentFrame */

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
    if (next >= DO_CAP) return 0;
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

int OU_Enqueue(char ev[EVT_SIZE])
{
    unsigned count;
    unsigned next;
    /* SP: 一律直写 DoList; MP 走 OutList, 满回 0。 */
    if (!OU_IsMP()) return OU_DirectToDoList(ev);
    count = *(unsigned*)ADDR_OUT_COUNT;
    if (count >= EVT_CAP) return 0;
    next = *(unsigned*)ADDR_OUT_NEXT & 0x7Fu;
    if (next >= EVT_CAP) return 0;
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

/* 鼠标屏坐标 → 格 + 点中对象。filterTgt 非 0 时只透出单位/建筑类对象。 */
static int OU_MouseCellInner(int* cx, int* cy, void** tgtOut, int* px, int* py,
                             int filterTgt)
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
    return OU_MouseCellInner(cx, cy, 0, px, py, 0);
}

int OU_MouseCell(int* cx, int* cy)
{
    return OU_MouseCellPx(cx, cy, 0, 0);
}

/* 取格 + 点中对象 (tgtOut 可空, 无目标写 0, 返回值仍是格有效性)。 */
int OU_MouseCellTarget(int* cx, int* cy, void** tgtOut, int* px, int* py)
{
    return OU_MouseCellInner(cx, cy, tgtOut, px, py, 1);
}

typedef int __attribute__((thiscall)) (*Fn_MouseOverObj)(void* self, void* target, int ignoreForce);

/* vtbl+0x74 = MouseOverObject (DecideAction 内部同入口, 只读不下单)。 */
int OU_MouseOverObject(void* self, void* target)
{
    void** vt;
    Fn_MouseOverObj fn;
    if (!self || !IsPlausiblePtr(self)) return -1;
    if (!target || !IsPlausiblePtr(target)) return -1;
    vt = *(void***)self;
    if (!vt || !IsPlausiblePtr(vt)) return -1;
    fn = (Fn_MouseOverObj)vt[0x74 / 4];
    if (!fn || !IsPlausiblePtr((const void*)fn)) return -1;
    return fn(self, target, 0);
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

/* ---------------- 规划线共享 (红绿线投影/取高/细线/线缓冲) ---------------- */

typedef int __attribute__((thiscall)) (*Fn_OUC2C)(void* tac,
    const CoordStruct* c, Point2D* out);
typedef int __attribute__((thiscall)) (*Fn_OUFloorH)(void* cell, const Point2D* p);
#define FN_OU_C2C ((Fn_OUC2C)(void*)0x6D2140u)
#define OU_TAC_INSTANCE 0x887324u
#define FN_OU_FLOORH ((Fn_OUFloorH)(void*)0x47B3A0u)

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

int OU_CellGroundZ(int cx, int cy, int* zOut)
{
    void* cell;
    Point2D zero;
    int z;
    if (!zOut) return 0;
    cell = OU_CellPtr(cx, cy);
    if (!cell || !IsPlausiblePtr(cell)) return 0;
    zero.X = 0; zero.Y = 0;
    z = FN_OU_FLOORH(cell, &zero);
    if (z < -8192 || z > 8192) return 0;
    *zOut = z;
    return 1;
}

void OU_ThinLine(unsigned char* locked, int w, int h, int pitch,
                 int x0, int y0, int x1, int y1,
                 unsigned r, unsigned g, unsigned b, unsigned a)
{
    int dx, dy, sx, sy, err, e2, x, y, n;
    unsigned short* dstRow;
    if (!locked || !IsPlausiblePtr(locked)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    dx = x1 - x0; dy = y1 - y0;
    sx = (dx >= 0) ? 1 : -1;
    sy = (dy >= 0) ? 1 : -1;
    dx = dx >= 0 ? dx : -dx; dy = dy >= 0 ? dy : -dy;
    err = dx - dy;
    x = x0; y = y0;
    for (n = 0; n < 4096; n++)
    {
        if (x >= 0 && y >= 0 && x < w && y < h)
        {
            dstRow = (unsigned short*)(locked + y * pitch);
            dstRow[x] = Blend16(dstRow[x], r, g, b, a);
        }
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
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

int OU_PrevPushCell(OU_PrevLine* buf, int* nio, void* u, int fly, int cx, int cy)
{
    CoordStruct a;
    int gz;
    if (!OU_GetCoords(u, &a)) return 0;
    if (fly) gz = a.Z;
    else if (!OU_CellGroundZ(cx, cy, &gz)) gz = 0;
    return OU_PrevPush(buf, nio, a.X, a.Y, a.Z,
                       cx * OU_LEP_PER_CELL + OU_LEP_PER_CELL / 2,
                       cy * OU_LEP_PER_CELL + OU_LEP_PER_CELL / 2, gz);
}

void OU_PrevDraw(OU_PrevLine* buf, int n, unsigned char* locked, int w, int h, int pitch,
                 unsigned r, unsigned g, unsigned b, unsigned a)
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
        OU_ThinLine(locked, w, h, pitch, ux, uy, tx, ty, r, g, b, a);
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

int OU_KeyDown(unsigned vk)
{
    if (!vk || vk > 255) return 0;
    return (GetAsyncKeyState((int)vk) & 0x8000) ? 1 : 0;
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
