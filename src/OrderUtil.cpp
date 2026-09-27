/* OrderUtil.cpp - 下单模块共享工具实现 (声明见 OrderUtil.h)。
 * 全只读 + OutList 入队, 不写模拟内存 (联机安全)。 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"

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
    next = *(unsigned*)ADDR_OUT_NEXT;
    for (i = 0; i < EVT_SIZE; i++)
        ((char*)(ADDR_OUT_BUF + next * EVT_SIZE))[i] = ev[i];
    snd = *(Fn_SndCb*)0x7E1530u;
    __asm__ volatile (
        "call *%1\n\t"
        "movl %%eax, %0"
        : "=r" (eaxAfter)
        : "r" (snd)
        : "ecx", "edx", "cc", "memory");
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
