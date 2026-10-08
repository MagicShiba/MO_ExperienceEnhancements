/* OrderUtil.cpp - 下单模块共享工具实现 (声明见 OrderUtil.h)。
 * 全只读 + OutList 入队, 不写模拟内存 (联机安全)。 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "ScaleBar.h"

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

int OU_Enqueue(char ev[EVT_SIZE])
{
    unsigned count = *(unsigned*)ADDR_OUT_COUNT;
    unsigned next;
    unsigned i;
    Fn_SndCb snd;
    unsigned eaxAfter;
    if (count >= EVT_CAP) return 0;
    next = *(unsigned*)ADDR_OUT_NEXT & 0x7Fu;   /* 当写下标用之前先夹 */
    if (next >= EVT_CAP) return 0;
    for (i = 0; i < EVT_SIZE; i++)
        ((char*)(ADDR_OUT_BUF + next * EVT_SIZE))[i] = ev[i];
    snd = *(Fn_SndCb*)0x7E1530u;
    /* 这条内联 "call *%1" 必须声明 ebx/esi/edi/ebp 被破坏: 真实 call 会改这四个
     * callee-saved 寄存器, 漏声明则紧接着用它算索引 / 写 OutList 账本时写进的是
     * callee 改过的垃圾 → 引擎按坏账本派发事件跳进野地址 (0xC0000005)。
     * 同坑见 notes/智能中键.md。 */
    __asm__ volatile (
        "call *%1\n\t"
        "movl %%eax, %0"
        : "=r" (eaxAfter)
        : "r" (snd)
        : "ecx", "edx", "ebx", "esi", "edi", "ebp", "cc", "memory");
    *(unsigned*)(ADDR_OUT_PAR + next * 4u) = eaxAfter;
    *(unsigned*)ADDR_OUT_NEXT = (next + 1u) & 0x7Fu;
    *(unsigned*)ADDR_OUT_COUNT = count + 1u;
    return 1;
}

int OU_CellValid(int cx, int cy)
{
    void** cells;
    int maxc, idx;
    if (cx < 0 || cy < 0 || cx >= 512 || cy >= 512) return 0;
    cells = *(void***)(ADDR_DISPLAY + OFF_MAP_CELLS);
    maxc = *(int*)(ADDR_DISPLAY + OFF_MAP_MAXC);
    if (!cells || !IsPlausiblePtr(cells) || maxc <= 0 || maxc > 512 * 512) return 0;
    idx = (cy << 9) + cx;
    if (idx < 0 || idx >= maxc) return 0;
    return cells[idx] ? 1 : 0;
}

int OU_MouseCellPx(int* cx, int* cy, int* px, int* py)
{
    int mx, my;
    Point2D pt;
    CellStruct center;
    CoordStruct crd;
    void* tgtObj = 0;
    unsigned char b5 = 0, b6 = 0;
    center.X = center.Y = 0;
    crd.X = crd.Y = crd.Z = 0;
    if (!GtMouseGameXY(&mx, &my)) return 0;
    pt.X = mx; pt.Y = my;
    /* ProcessClickCoords 按**视口空间**判格/拾取对象, 而 GtMouseGameXY 给的是
     * 物理屏幕点 —— 放大档 (视口 R = A*scale) 下必须先过缩放模块的反变换,
     * 否则取到的格偏 A/R 倍。scale=1 时恒等。*/
    SB_MapPoint(&pt, 0);
    if (!FN_CLICKCOORDS((void*)ADDR_DISPLAY, &pt, &center, &crd,
                        &tgtObj, &b5, &b6))
        return 0;
    *cx = (int)center.X; *cy = (int)center.Y;
    if (px) *px = mx;
    if (py) *py = my;
    return OU_CellValid(*cx, *cy);
}

int OU_MouseCell(int* cx, int* cy)
{
    return OU_MouseCellPx(cx, cy, 0, 0);
}

/* 取格 + 点中对象 (ProcessClickCoords 原型见 OrderUtil.h, tgt 透出)。 */
int OU_MouseCellTarget(int* cx, int* cy, void** tgtOut, int* px, int* py)
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
    SB_MapPoint(&pt, 0);          /* 见 OU_MouseCellPx: 物理点 -> 视口点 */
    if (!FN_CLICKCOORDS((void*)ADDR_DISPLAY, &pt, &center, &crd,
                        &tgtObj, &b5, &b6))
        return 0;
    *cx = (int)center.X; *cy = (int)center.Y;
    if (px) *px = mx;
    if (py) *py = my;
    if (!OU_CellValid(*cx, *cy)) return 0;
    if (tgtOut && tgtObj && IsPlausiblePtr(tgtObj))
    {
        int w = OU_WhatAmI(tgtObj);
        if (w == ABS_UNIT || w == ABS_INFANTRY ||
            w == ABS_AIRCRAFT || w == ABS_BUILDING)
            *tgtOut = tgtObj;
    }
    return 1;
}

typedef int __attribute__((thiscall)) (*Fn_MouseOverObj)(void* self, void* target, int ignoreForce);

/* vtbl+0x74 = MouseOverObject (Unit 0x73FD50 / Infantry 0x51E3B0 同槽,
 * gamemd 反汇编 + 虚表扫描双确认; DecideAction 内部同入口)。 */
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
