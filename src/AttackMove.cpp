/* AttackMove.cpp - custom attack-move hotkey.
 *
 * Vanilla Ctrl+Shift+LMB = Action::AttackMoveNav=62 / AttackMoveTar=63 (cursor
 * AttackOutOfRange2) driving Mission::AttackMove=29 as a FootClass overlay
 * (MegaMission + MegaDestination(cell) / MegaTarget(object)). This module
 * re-issues the same sim order as a standard MegaMission event (Mission=29) so
 * any hotkey can trigger it: single key (X2/A) fires at the hover cell, combo
 * with LMB (e.g. A+LMB) fires on the LMB edge.
 *
 * Net-safe: read-only queries + OutList enqueue, no sim memory writes.
 * Direct order uses IsPlanningEvent=0; holding Z queues (plan=1), same as
 * vanilla Z+Ctrl+Shift waypoint engage.
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"

#define AM_SECTION "Features"
#define MISSION_ATTACKMOVE 29   /* Mission::AttackMove (YRpp GeneralDefinitions.h) */

typedef void* __attribute__((thiscall)) (*Fn_TargetFromCell)(void* tgt, const CellStruct* cell);
#define FN_TGT_CELL ((Fn_TargetFromCell)(void*)0x6E6B20u)

#define AM_UNIT_CAP 256
#define AM_PEND_CAP 512
#define AM_DRAIN_PER_POLL 128

typedef struct { void* unit; int uid; void* tgt; int tid; short x, y; } AMPend;

static AMPend g_AmPend[AM_PEND_CAP];
static int g_AmPendCount = 0;
static int g_AmIdxOk = 0;
static void* g_AmHouse = 0;
static int g_AmHouseIdx = 0;

static int g_AmInit = 0;
static int g_AmEnable = 1;
static OU_Hotkey g_AmHk;
static int g_AmPrevTrig = 0;

static void AM_EnsureInit(void)
{
    if (g_AmInit) return;
    g_AmInit = 1;
    BuildPaths();
    GtEnsureLog();
    /* 单行: 缺键=X2 启用; 0=禁用; 其它按 Hotkey 串 (坏串留默认, None=禁用) */
    g_AmEnable = OU_ReadHotkeySingle(AM_SECTION, "AttackMove", "X2", &g_AmHk);
    LogLine("attackmove: ON (custom, default X2, mission=29)");
    if (!g_AmEnable) LogLine("attackmove: OFF");
}

static int AM_IsAttacker(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY || w == ABS_AIRCRAFT) ? 1 : 0;
}

static int AM_Fire(int houseIdx, int srcID, unsigned char srcRtti,
                   int tgtID, unsigned char tgtRtti,
                   int dstID, unsigned char dstRtti, int plan)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_ATTACKMOVE,
            tgtID, (unsigned)tgtRtti, dstID, (unsigned)dstRtti, 0, 0);
    ev[OFF_PLANFLAG] = (char)(plan ? 1 : 0);
    return OU_Enqueue(ev);
}

static void AM_ShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_AmPendCount; k++)
        g_AmPend[k] = g_AmPend[k + 1];
    g_AmPendCount--;
}

static void AM_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx, sent = 0;
    if (!cur || cur != g_AmHouse) { g_AmPendCount = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (houseIdx != g_AmHouseIdx) { g_AmPendCount = 0; return; }
    while (g_AmPendCount > 0 && sent < AM_DRAIN_PER_POLL)
    {
        AMPend* e = &g_AmPend[0];
        char srcB[8], tgtB[8], dstB[8];
        CellStruct cs;
        unsigned i;
        int srcID, tgtID = 0, dstID;
        unsigned char srcR, tgtR = 0, dstR;
        for (i = 0; i < 8; i++) srcB[i] = tgtB[i] = dstB[i] = 0;
        if (!OU_PtrAlive(e->unit, e->uid, g_AmIdxOk)) { AM_ShiftDown(); continue; }
        if (!AM_IsAttacker(e->unit)) { AM_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { AM_ShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        cs.X = e->x; cs.Y = e->y;
        FN_TGT_CELL(dstB, &cs);
        dstID = *(int*)(dstB + 0); dstR = (unsigned char)dstB[4];
        /* Target object died -> fall back to pure cell AttackMove. */
        if (e->tgt && OU_PtrAlive(e->tgt, e->tid, g_AmIdxOk) && FN_TGT_OBJ(tgtB, e->tgt))
        {
            tgtID = *(int*)(tgtB + 0); tgtR = (unsigned char)tgtB[4];
        }
        {
            int plan = OU_KeyDown('Z') ? 1 : 0;
            if (!AM_Fire(houseIdx, srcID, srcR, tgtID, tgtR, dstID, dstR, plan))
                return; /* OutList full: continue next poll */
        }
        AM_ShiftDown();
        sent++;
    }
}

static void AM_Press(void)
{
    void* cur;
    void** items;
    int selN, i, nu = 0;
    int cx, cy;
    void* tgt = 0;
    int tid = 0;
    char buf[96], b1[16], b2[16];
    unsigned q;
    static void* s_units[AM_UNIT_CAP];
    static int s_uids[AM_UNIT_CAP];

    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) { LogLine("am: no selection"); return; }
    for (i = 0; i < selN && nu < AM_UNIT_CAP; i++)
    {
        void* o = items[i];
        char tb[8];
        unsigned ti;
        if (!AM_IsAttacker(o)) continue;
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        s_units[nu] = o;
        s_uids[nu] = FN_TGT_OBJ(tb, o) ? *(int*)(tb + 0) : 0;
        nu++;
    }
    if (nu <= 0) { LogLine("am: no attackers"); return; }
    g_AmIdxOk = 0;
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        if (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 200000 &&
            s_uids[0] > 0 && OU_PtrAliveRaw(s_units[0], s_uids[0]))
            g_AmIdxOk = 1;
    }
    if (!OU_MouseCellTarget(&cx, &cy, &tgt, 0, 0)) { LogLine("am: off map"); return; }
    if (tgt)
    {
        char tb[8];
        unsigned ti;
        int w = OU_WhatAmI(tgt);
        if (w != ABS_UNIT && w != ABS_INFANTRY &&
            w != ABS_AIRCRAFT && w != ABS_BUILDING)
            tgt = 0;
        else
        {
            for (ti = 0; ti < 8; ti++) tb[ti] = 0;
            if (FN_TGT_OBJ(tb, tgt))
                tid = *(int*)(tb + 0);
            else
                tgt = 0;
        }
    }
    q = 0;
    q = AppendCStr(buf, q, "am: attackmove units=", sizeof(buf));
    UtoA((unsigned)nu, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, tgt ? " tgt=1" : " tgt=0", sizeof(buf));
    UtoA(0, b2);
    (void)q; (void)b2;
    LogLine(buf);
    g_AmPendCount = 0;
    for (i = 0; i < nu && g_AmPendCount < AM_PEND_CAP; i++)
    {
        g_AmPend[g_AmPendCount].unit = s_units[i];
        g_AmPend[g_AmPendCount].uid = s_uids[i];
        g_AmPend[g_AmPendCount].tgt = tgt;
        g_AmPend[g_AmPendCount].tid = tid;
        g_AmPend[g_AmPendCount].x = (short)cx;
        g_AmPend[g_AmPendCount].y = (short)cy;
        g_AmPendCount++;
    }
    g_AmHouse = cur;
    g_AmHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    AM_Drain();
}

void AttackMove_Poll(void)
{
    int trigDown;
    int combo;
    AM_EnsureInit();
    trigDown = OU_KeyDown(g_AmHk.vk);
    combo = (OU_ModsDown(g_AmHk.mods) && OU_ExtraDown(&g_AmHk)) ? 1 : 0;
    if (g_AmPendCount > 0) { AM_Drain(); g_AmPrevTrig = trigDown; return; }
    if (!g_AmEnable) { g_AmPrevTrig = trigDown; return; }
    if (trigDown && !g_AmPrevTrig && combo)
        AM_Press();
    g_AmPrevTrig = trigDown;
}
