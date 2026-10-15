/* FuzzyAttack.cpp - Ctrl+中键模糊下令 (快速单位难。对开火
 * 目标太小点不中时用)。
 *
 * 触发: 按住 Ctrl (避开 Z) 的同时按住鼠标中键 (Hotkey, 默认 Ctrl+Middle),
 * 显示生效格块菱形框 + 红色规划细线 (单位→分到的敌, 与松手分组同规则,
 * 预览限 128 线, 换格或 400ms 刷一次, 松手即消), 松开才在当前格执行:
 * 以鼠标格为基准扫 Range x Range (默认 4x4, 可配 2~16) 找敌方对象,
 * 选中单位分组强制攻击 (MegaMission Mission=Attack=1, Target=敌对象,
 * 直接下令 IsPlanningEvent=0)。点中敌人则它在 0 环必在首位, 点空地则扫
 * 附近敌人。端点坐标走 OU_GetCoords (vtbl+0x48, 单位中心含高度),
 * 绘制期经引擎 CoordsToClient 投影。
 * 与巡逻圈/砍树/散开全互斥: 要 Ctrl 且不能按 Z (Z+中键=巡逻,
 * Ctrl+Z+中键=砍树, 裸中键=散开)。
 *
 * 找敌 (YRpp 交叉确认):
 *   CellClass::FindObjectOfType (thiscall 0x47C4D0, ECX=格, 栈 abs+alt)
 *   按 Unit/Infantry/Aircraft/Building 各取链头, 再沿 ObjectClass.NextObject
 *   (+0x30) 走链收同格堆叠 (步兵一格 3 个, 只取链头会漏)。链步数上限 64,
 *   指针 + WhatAmI 双重守卫, 偏移不对时自动退化不断链。
 *   敌我判定: Techno.Owner 指针 != 本家 (Owner 偏移用两个选中单位双确认自
 *   校准, 失败直接放弃, 绝不误击友军)。环扫顺序即由近及远, 首个即最近。
 * 分组 (SquadSize=几打一, 默认 4): 单位 <= SquadSize → 全员集火
 *   enemies[0]; 否则 nUsed=clamp(nU/SquadSize,1,nE), 前 nUsed 个敌人均衡分
 *   (sizes 差 <=1), 剩下的敌人没单 (集火优先, 不搞 1v1 添油)。
 *
 * 联机: 只发标准事件, 各端同一份原版代码执行, 不影响联机。
 * 本地只读 + 标准事件入队, 不写模拟内存。
 * 构建见 build-fast.bat (32 位 mingw, 无 CRT, 与 MOExperience 同一 DLL)。
 * 轮询挂 PowerDrawBoth (见 MOExperience.cpp), 开关关时直接返回。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "OrderThrottle.h"

#define FA_SECTION "FuzzyAttack"

/* ---------------- 模糊攻击私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define OFF_NEXT_OBJ 0x30u    /* ObjectClass.NextObject (YRpp 成员顺序) */

/* CellClass::FindObjectOfType (0x47C4D0, ECX=格, 栈 abs+alt): 返回链头或空。 */
typedef void* __attribute__((thiscall)) (*Fn_FindObjType)(void* cell, int abs, int alt);
typedef void* (__fastcall *FA_LockFn)(void*, void*, int, int);
typedef int (__fastcall *FA_UnlockFn)(void*, void*);

#define VT_FA_LOCK   0x5C
#define VT_FA_UNLOCK 0x60

#define FN_FINDOBJTYPE ((Fn_FindObjType)(void*)0x47C4D0u)

/* ---------------- 配置 ---------------- */

static int g_FaInit = 0;
static int g_FaEnable = 1;
static OU_Hotkey g_FaHk;      /* Hotkey=Ctrl+Middle (Z 按住时让路巡逻/砍树) */
static int g_FaRange = 4;       /* 扫描块边长 (格, 鼠标在块内 [Range/2][Range/2]) */
static int g_FaSquad = 4;       /* 几打一 (单位<=此数则全员集火一个) */

static void FA_EnsureInit(void)
{
    char buf[96], b1[16];
    unsigned q = 0;
    if (g_FaInit) return;
    g_FaInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_FaEnable = GetPrivateProfileIntA(FA_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    OU_ParseHotkey("Ctrl+Middle", &g_FaHk);           /* 默认 (必成功) */
    OU_ReadHotkey(FA_SECTION, "Hotkey", &g_FaHk);     /* 有则覆盖, 坏串留默认 */
    if (g_FaHk.disabled) g_FaEnable = 0;
    g_FaRange = GetPrivateProfileIntA(FA_SECTION, "Range", 4, g_IniPath);
    g_FaSquad = GetPrivateProfileIntA(FA_SECTION, "SquadSize", 4, g_IniPath);
    if (g_FaRange < 2) g_FaRange = 2;
    if (g_FaRange > 16) g_FaRange = 16;
    if (g_FaSquad < 2) g_FaSquad = 2;
    if (g_FaSquad > 16) g_FaSquad = 16;
    q = AppendCStr(buf, q, "fuzzyattack: ON (ctrl+middle) range=", sizeof(buf));
    UtoA((unsigned)g_FaRange, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    (void)q;
    LogLine(buf);
    if (!g_FaEnable) LogLine("fuzzyattack: OFF");
}

/* ---------------- 小工具 (通用部分见 OrderUtil.h) ---------------- */

/* 攻击者: 地面 + 飞机 (难。点的高速单位, 建筑除外)。 */
static int FA_IsAttacker(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY || w == ABS_AIRCRAFT) ? 1 : 0;
}

/* 可攻击的目标类型 (单位/步兵/飞机/建筑)。 */
static int FA_IsTargetType(void* p)
{
    int w = OU_WhatAmI(p);
    return (w == ABS_UNIT || w == ABS_INFANTRY ||
            w == ABS_AIRCRAFT || w == ABS_BUILDING) ? 1 : 0;
}

/* 强制攻击: MegaMission(Attack, Target=敌对象, Dst=空, 直接下令 flag=0)。 */
static int FA_FireAttack(int houseIdx, int srcID, unsigned char srcRtti,
                         int tgtID, unsigned char tgtRtti,
                         void* unit, int uid, int idxOk)
{
    char ev[EVT_SIZE];
    unsigned i;
    for (i = 0; i < EVT_SIZE; i++) ev[i] = 0;
    FN_MEGA(ev, houseIdx, srcID, (unsigned)srcRtti, MISSION_ATTACK,
            tgtID, (unsigned)tgtRtti, 0, 0, 0, 0);
    ev[OFF_PLANFLAG] = 0;
    return OT_Fire(ev, unit, uid, idxOk);
}

static void* FA_CellPtr(int cx, int cy)
{
    void** cells;
    int maxc, idx;
    if (!OU_CellValid(cx, cy)) return 0;
    cells = *(void***)(ADDR_DISPLAY + OFF_MAP_CELLS);
    maxc = *(int*)(ADDR_DISPLAY + OFF_MAP_MAXC);
    idx = (cy << 9) + cx;
    if (idx < 0 || idx >= maxc) return 0;
    return cells[idx];
}

/* ---------------- Owner 自校准 (敌我判定, 双单位确认) ---------------- */

static int g_FaOwnerOff = -1;  /* Techno 内 Owner 指针偏移 pin */

/* pin 命中直验, 否则全扫; 两个选中单位同偏移命中才算 (防偶发撞值)。
 * 只做值比较不解引用, 安全。 */
static int FA_CalOwner(void* u0, void* u1, void* house)
{
    unsigned off;
    if (!u0 || !house) return 0;
    if (g_FaOwnerOff >= 0)
    {
        if (*(void**)((char*)u0 + (unsigned)g_FaOwnerOff) == house &&
            (!u1 || *(void**)((char*)u1 + (unsigned)g_FaOwnerOff) == house))
            return 1;
        g_FaOwnerOff = -1;
    }
    for (off = 0x40u; off < 0x900u; off += 4u)
    {
        if (*(void**)((char*)u0 + off) != house) continue;
        if (u1 && *(void**)((char*)u1 + off) != house) continue;
        g_FaOwnerOff = (int)off;
        return 1;
    }
    return 0;
}

/* ---------------- 找敌 (环扫 + 链走) ---------------- */

#define FA_ENEMY_CAP 256

/* 链收集: 首节点起沿 NextObject 走, 只收目标类型, 去重, 上限守卫。
 * 指针/WhatAmI 双重守卫, 偏移不对时自动停 (退化为链头单收)。 */
static void FA_CollectChain(void* first, void* curHouse,
                            void** out, int* tids, int* nio)
{
    void* o = first;
    int steps = 0;
    (void)curHouse;
    while (o && steps < 64 && *nio < FA_ENEMY_CAP)
    {
        int dup;
        steps++;
        if (!IsPlausiblePtr(o)) break;
        for (dup = 0; dup < *nio; dup++)
            if (out[dup] == o) break;
        if (dup >= *nio)
        {
            void* oh;
            char tb[8];
            unsigned ti;
            if (!FA_IsTargetType(o)) goto next;
            /* 敌我: Owner 非空、合理、非本家 (中立也算敌, 可强制攻击)。 */
            oh = *(void**)((char*)o + (unsigned)g_FaOwnerOff);
            if (!oh || !IsPlausiblePtr(oh) || oh == curHouse) goto next;
            for (ti = 0; ti < 8; ti++) tb[ti] = 0;
            if (!FN_TGT_OBJ(tb, o)) goto next;
            out[*nio] = o;
            tids[*nio] = *(int*)(tb + 0);
            (*nio)++;
        }
next:
        o = *(void**)((char*)o + OFF_NEXT_OBJ);
    }
}

/* 块扫: 与显示框同一格块 (偏移 lo..hi), d 由小到大环进,
 * 收集顺序即由近及远。每格按四类取链头再走链 (一格多步兵不漏)。 */
static int FA_FindEnemies(int cx, int cy, void* curHouse,
                          void** enemies, int* eids)
{
    static const int abss[4] = { ABS_UNIT, ABS_INFANTRY, ABS_AIRCRAFT, ABS_BUILDING };
    int n = 0, d, dx, dy, t;
    int lo = -(g_FaRange / 2);
    int hi = lo + g_FaRange - 1;
    for (d = 0; d <= g_FaRange && n < FA_ENEMY_CAP; d++)
    {
        for (dy = -d; dy <= d && n < FA_ENEMY_CAP; dy++)
        {
            for (dx = -d; dx <= d && n < FA_ENEMY_CAP; dx++)
            {
                int adx = dx >= 0 ? dx : -dx, ady = dy >= 0 ? dy : -dy;
                int cheb = adx >= ady ? adx : ady;
                void* cell;
                if (cheb != d) continue;
                if (dx < lo || dx > hi || dy < lo || dy > hi) continue;
                cell = FA_CellPtr(cx + dx, cy + dy);
                if (!cell) continue;
                for (t = 0; t < 4; t++)
                {
                    void* head = FN_FINDOBJTYPE(cell, abss[t], 0);
                    if (!head || !IsPlausiblePtr(head)) continue;
                    FA_CollectChain(head, curHouse, enemies, eids, &n);
                }
            }
        }
    }
    return n;
}

/* ---------------- 待发队列 (保序 drip) ---------------- */

#define FA_UNIT_CAP 1024   /* 快照单位上限 (超限进 OT 队列分帧, 与 OT_QCAP 对齐) */
#define FA_PEND_CAP 1024   /* 待发上限 (同上) */
#define FA_DRAIN_PER_POLL 128

typedef struct { void* unit; int uid; void* foe; int fid; } FAPend;

static FAPend g_FaPend[FA_PEND_CAP];
static int g_FaPendCount = 0;
static int g_FaIdxOk = 0;         /* ID 索引机制自检通过 (存活校验总开关) */
static void* g_FaHouse = 0;
static int g_FaHouseIdx = 0;
static int g_FaPrevTrig = 0;

static void FA_ShiftDown(void)
{
    int k;
    for (k = 0; k + 1 < g_FaPendCount; k++)
    {
        g_FaPend[k].unit = g_FaPend[k + 1].unit;
        g_FaPend[k].uid = g_FaPend[k + 1].uid;
        g_FaPend[k].foe = g_FaPend[k + 1].foe;
        g_FaPend[k].fid = g_FaPend[k + 1].fid;
    }
    g_FaPendCount--;
}

/* 排空: 发完即止 (攻击无多阶段门控)。中途 house 变化则清空。 */
static void FA_Drain(void)
{
    void* cur = *(void**)ADDR_CURPLAYER;
    int houseIdx;
    int sent = 0;
    char buf[64], b1[16];
    unsigned q;
    if (!cur || cur != g_FaHouse) { g_FaPendCount = 0; return; }
    houseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    if (houseIdx != g_FaHouseIdx) { g_FaPendCount = 0; return; }
    while (g_FaPendCount > 0 && sent < FA_DRAIN_PER_POLL)
    {
        FAPend* e = &g_FaPend[0];
        char srcB[8], tgtB[8];
        unsigned i;
        int srcID, tgtID;
        unsigned char srcR, tgtR;
        for (i = 0; i < 8; i++) srcB[i] = tgtB[i] = 0;
        /* 先验活 (纯读索引): 战死/目标已毁即丢弃, 不碰悬空指针 */
        if (!OU_PtrAlive(e->unit, e->uid, g_FaIdxOk)) { FA_ShiftDown(); continue; }
        if (!FA_IsAttacker(e->unit)) { FA_ShiftDown(); continue; }
        if (!OU_PtrAlive(e->foe, e->fid, g_FaIdxOk)) { FA_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(tgtB, e->foe)) { FA_ShiftDown(); continue; }
        if (!FN_TGT_OBJ(srcB, e->unit)) { FA_ShiftDown(); continue; }
        srcID = *(int*)(srcB + 0); srcR = (unsigned char)srcB[4];
        tgtID = *(int*)(tgtB + 0); tgtR = (unsigned char)tgtB[4];
        if (!FA_FireAttack(houseIdx, srcID, srcR, tgtID, tgtR,
                             e->unit, e->uid, g_FaIdxOk))
            return; /* 满: 下轮继续 */
        FA_ShiftDown();
        sent++;
    }
    if (g_FaPendCount <= 0)
    {
        q = 0;
        q = AppendCStr(buf, q, "fa: drained sent=", sizeof(buf));
        UtoA((unsigned)sent, b1);
        q = AppendCStr(buf, q, b1, sizeof(buf));
        (void)q;
        LogLine(buf);
    }
}

/* ---------------- 规划线预览 (按住时跑, 与松手分组同规则, 含高度) ---------------- */

/* 规划线缓冲 (端点 leptons 世界坐标, 见 OrderUtil.h OU_PrevLine)。 */
static OU_PrevLine s_FaPrevBuf[OU_PREV_CAP];
static int s_FaPrevN = 0;

/* 预览: 当前块找敌、与 FA_Press 同式分组, 只存线不下令。
 * 节流: 换格且距上次>80ms, 或超 400ms 刷一次。 */
static void FA_Preview(void)
{
    void* cur;
    void** items;
    int selN, i, nu = 0, ne = 0, nUsed, pos;
    int cx, cy;
    static unsigned s_LastMs = 0;
    static int s_LastCx = -9999, s_LastCy = -9999, s_LastKey = 0;
    static void* s_pu[FA_UNIT_CAP];
    static void* s_pf[FA_ENEMY_CAP];
    static int s_pfid[FA_ENEMY_CAP];

    if (!OU_MouseCell(&cx, &cy)) return;
    if (!OU_PrevThrottle(&s_LastMs, &s_LastCx, &s_LastCy, &s_LastKey,
                         cx, cy, 0, 80, 400)) return;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) { s_FaPrevN = 0; return; }
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items)) { s_FaPrevN = 0; return; }
    for (i = 0; i < selN && nu < FA_UNIT_CAP; i++)
        if (FA_IsAttacker(items[i])) s_pu[nu++] = items[i];
    if (nu <= 0) { s_FaPrevN = 0; return; }
    if (!FA_CalOwner(s_pu[0], nu > 1 ? s_pu[1] : 0, cur))
    { s_FaPrevN = 0; return; }
    ne = FA_FindEnemies(cx, cy, cur, s_pf, s_pfid);
    if (ne <= 0) { s_FaPrevN = 0; return; }
    if (nu <= g_FaSquad)
        nUsed = 1;
    else
    {
        nUsed = nu / g_FaSquad;
        if (nUsed < 1) nUsed = 1;
        if (nUsed > ne) nUsed = ne;
    }
    s_FaPrevN = 0;
    pos = 0;
    {
        int base = nu / nUsed, rem = nu % nUsed, j;
        for (j = 0; j < nUsed && pos < nu; j++)
        {
            int take = base + (j < rem ? 1 : 0);
            int k;
            for (k = 0; k < take && pos < nu; k++, pos++)
                OU_PrevPushObjs(s_FaPrevBuf, &s_FaPrevN, s_pu[pos], s_pf[j]);
        }
    }
}

/* ---------------- 触发 ---------------- */

static void FA_Press(void)
{
    void* cur;
    void** items;
    int selN, i, nu = 0, ne = 0;
    int cx, cy;
    int nUsed, pos;
    char buf[96], b1[16], b2[16], b3[16];
    unsigned q;
    static void* s_units[FA_UNIT_CAP];
    static int s_uids[FA_UNIT_CAP];
    static void* s_foes[FA_ENEMY_CAP];
    static int s_fids[FA_ENEMY_CAP];

    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || !IsPlausiblePtr(cur)) return;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN <= 0 || !items || !IsPlausiblePtr(items))
    { LogLine("fa: no selection"); return; }
    for (i = 0; i < selN && nu < FA_UNIT_CAP; i++)
    {
        void* o = items[i];
        if (FA_IsAttacker(o)) s_units[nu++] = o;
    }
    if (nu <= 0) { LogLine("fa: no attackers"); return; }
    /* Owner 自校准 (双确认; 失败直接放弃, 绝不误击友军)。 */
    if (!FA_CalOwner(s_units[0], nu > 1 ? s_units[1] : 0, cur))
    { LogLine("fa: noowner"); return; }
    if (!OU_MouseCell(&cx, &cy)) { LogLine("fa: off map"); return; }
    /* 快照单位 Fetch_ID (存活校验用; 选中单位必活, 此处 vcall 安全) */
    for (i = 0; i < nu; i++)
    {
        char tb[8];
        unsigned ti;
        for (ti = 0; ti < 8; ti++) tb[ti] = 0;
        s_uids[i] = FN_TGT_OBJ(tb, s_units[i]) ? *(int*)(tb + 0) : 0;
    }
    /* ID 索引自检 (fail-open): 首单位查不到则本轮跳过存活校验 */
    g_FaIdxOk = 0;
    {
        void* base = *(void**)ADDR_IDBASE;
        int cnt = *(int*)ADDR_IDCOUNT;
        if (base && IsPlausiblePtr(base) && cnt > 0 && cnt <= 200000 &&
            s_uids[0] > 0 &&
            OU_PtrAliveRaw(s_units[0], s_uids[0]))
            g_FaIdxOk = 1;
    }
    ne = FA_FindEnemies(cx, cy, cur, s_foes, s_fids);
    if (ne <= 0) { LogLine("fa: no enemies"); return; }
    /* 分组: 单位<=Squad 则全员集火首敌 (最近);
     * 否则 nUsed=clamp(nU/Squad,1,nE), 前 nUsed 敌均衡分, 其余没单。 */
    if (nu <= g_FaSquad)
        nUsed = 1;
    else
    {
        nUsed = nu / g_FaSquad;
        if (nUsed < 1) nUsed = 1;
        if (nUsed > ne) nUsed = ne;
    }
    q = 0;
    q = AppendCStr(buf, q, "fa: atk foes=", sizeof(buf));
    UtoA((unsigned)ne, b1);
    q = AppendCStr(buf, q, b1, sizeof(buf));
    q = AppendCStr(buf, q, " units=", sizeof(buf));
    UtoA((unsigned)nu, b2);
    q = AppendCStr(buf, q, b2, sizeof(buf));
    q = AppendCStr(buf, q, " squads=", sizeof(buf));
    UtoA((unsigned)nUsed, b3);
    q = AppendCStr(buf, q, b3, sizeof(buf));
    (void)q;
    LogLine(buf);
    g_FaPendCount = 0;
    pos = 0;
    {
        int base = nu / nUsed, rem = nu % nUsed, j;
        for (j = 0; j < nUsed && pos < nu; j++)
        {
            int take = base + (j < rem ? 1 : 0);
            int k;
            for (k = 0; k < take && pos < nu; k++, pos++)
            {
                if (g_FaPendCount >= FA_PEND_CAP) break;
                g_FaPend[g_FaPendCount].unit = s_units[pos];
                g_FaPend[g_FaPendCount].uid = s_uids[pos];
                g_FaPend[g_FaPendCount].foe = s_foes[j];
                g_FaPend[g_FaPendCount].fid = s_fids[j];
                g_FaPendCount++;
            }
        }
    }
    g_FaHouse = cur;
    g_FaHouseIdx = *(int*)((char*)cur + OFF_HOUSE_INDEX);
    FA_Drain();
}

/* ---------------- 每循环轮询 (挂 PowerDrawBoth) ---------------- */

static void FA_BoxReset(void);
static void FA_BoxSample(void);

void FuzzyAttack_Poll(void)
{
    int trigDown;
    int combo;

    FA_EnsureInit();
    trigDown = OU_KeyDown(g_FaHk.vk);
    /* 有 Z 归巡逻/砍树 (仲裁, 不在 Hotkey 里): 这里排除 */
    combo = (OU_ModsDown(g_FaHk.mods) && OU_ExtraDown(&g_FaHk) &&
             !OU_KeyDown('Z')) ? 1 : 0;
    if (g_FaPendCount > 0)
    { FA_Drain(); g_FaPrevTrig = trigDown; return; } /* drip 续发 */
    if (!g_FaEnable) { g_FaPrevTrig = trigDown; return; }
    if (trigDown && !g_FaPrevTrig)
    {
        FA_BoxReset();            /* 按下: 采样器清零 (无动作, 松开才执行) */
        s_FaPrevN = 0;
    }
    if (trigDown && combo)
    {
        FA_BoxSample();           /* 按住: 采样画框 (+像素质校准) */
        FA_Preview();             /* 按住: 规划线预览 (红色, 松手即消) */
    }
    else if (!trigDown && g_FaPrevTrig && combo)
    {
        s_FaPrevN = 0;
        FA_Press();               /* 松开: 当前格执行 */
    }
    g_FaPrevTrig = trigDown;
}

/* ---------------- 渲染期范围框 (按住显示, 松开执行) ---------------- */
/* 格→像素固定等距映射: X+1 格 = (+30,+15)px, Y+1 格 = (-30,+15)px
 * (RA2 格 60x30, 无缩放)。故意不用采样解算: 中键按住平移镜头,
 * 前后采样不在同一镜头帧, 解出的矩阵是垃圾 (框乱 skew 的根因)。
 * 参考点按轮刷新 (像素+格同轮, 恒一致), 框 glue 在鼠标上。 */

static int g_FaBRx = 0, g_FaBRy = 0, g_FaBRcx = 0, g_FaBRcy = 0;
static int g_FaBHas = 0;

static void FA_BoxReset(void)
{
    g_FaBHas = 0;
}

/* 按住期间每轮跟踪最新 (像素+格) 参考点。 */
static void FA_BoxSample(void)
{
    int mx, my, cx, cy;
    if (!GtMouseGameXY(&mx, &my)) return;
    if (!OU_MouseCell(&cx, &cy)) return;
    g_FaBRx = mx; g_FaBRy = my; g_FaBRcx = cx; g_FaBRcy = cy;
    g_FaBHas = 1;
}

static unsigned char* s_FaLocked = 0;
static int s_FaW = 0, s_FaH = 0, s_FaPitch = 0;

static void FA_Plot(int x, int y, unsigned a)
{
    unsigned short* dstRow;
    if (x < 0 || y < 0 || x >= s_FaW || y >= s_FaH) return;
    dstRow = (unsigned short*)(s_FaLocked + y * s_FaPitch);
    dstRow[x] = Blend16(dstRow[x], 255, 255, 255, a);
}

/* 3px 线 (Bresenham + 上下各 1px, 零依赖)。 */
static void FA_Line(int x0, int y0, int x1, int y1, unsigned a)
{
    int dx, dy, sx, sy, err, e2, x, y, n;
    dx = x1 - x0; dy = y1 - y0;
    sx = (dx >= 0) ? 1 : -1;
    sy = (dy >= 0) ? 1 : -1;
    dx = dx >= 0 ? dx : -dx; dy = dy >= 0 ? dy : -dy;
    err = dx - dy;
    x = x0; y = y0;
    for (n = 0; n < 4096; n++)
    {
        FA_Plot(x, y, a);
        FA_Plot(x, y - 1, a);
        FA_Plot(x, y + 1, a);
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

/* 实心方块标记 (半边 m)。 */
static void FA_Mark(int cx, int cy, int m, unsigned a)
{
    int k, j;
    for (k = -m; k <= m; k++)
        for (j = -m; j <= m; j++)
            FA_Plot(cx + k, cy + j, a);
}

/* 格块角 → 像素 (参考点 + 固定等距差)。 */
static void FA_BoxCorner(int ccx, int ccy, int* px, int* py)
{
    int dx = ccx - g_FaBRcx, dy = ccy - g_FaBRcy;
    *px = g_FaBRx + dx * 30 - dy * 30;
    *py = g_FaBRy + dx * 15 + dy * 15;
}

void FuzzyAttack_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* vt;
    FA_LockFn lockFn;
    FA_UnlockFn unlockFn;
    unsigned char* locked;
    int lo, hi, x0, y0, x1, y1, x2, y2, x3, y3;
    extern int g_Calibrated;
    if (!g_FaEnable || !g_FaBHas) return;
    if (!OU_KeyDown(g_FaHk.vk)) return;  /* 按住才画 */
    if (!OU_ModsDown(g_FaHk.mods) || !OU_ExtraDown(&g_FaHk)) return;
    if (OU_KeyDown('Z')) return;            /* 有 Z 归别家 */
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    lockFn = (FA_LockFn)(*(void**)(vt + VT_FA_LOCK));
    unlockFn = (FA_UnlockFn)(*(void**)(vt + VT_FA_UNLOCK));
    if (!lockFn || !unlockFn) return;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return;
    s_FaLocked = locked; s_FaW = w; s_FaH = h; s_FaPitch = pitch;
    /* 生效格块 (参考格为块中心锚点, 与 Find 一致) 菱形框 + 四角标记。
     * 注: 中键拖动平移镜头时参考点按轮更新, 帧间小漂, 与各家线同 caveat。 */
    lo = -(g_FaRange / 2);
    hi = lo + g_FaRange - 1;
    FA_BoxCorner(g_FaBRcx + lo, g_FaBRcy + lo, &x0, &y0);
    FA_BoxCorner(g_FaBRcx + hi + 1, g_FaBRcy + lo, &x1, &y1);
    FA_BoxCorner(g_FaBRcx + hi + 1, g_FaBRcy + hi + 1, &x2, &y2);
    FA_BoxCorner(g_FaBRcx + lo, g_FaBRcy + hi + 1, &x3, &y3);
    FA_Line(x0, y0, x1, y1, 230);
    FA_Line(x1, y1, x2, y2, 230);
    FA_Line(x2, y2, x3, y3, 230);
    FA_Line(x3, y3, x0, y0, 230);
    FA_Mark(x0, y0, 1, 230);
    FA_Mark(x1, y1, 1, 230);
    FA_Mark(x2, y2, 1, 230);
    FA_Mark(x3, y3, 1, 230);
    /* 规划线 (红色细线, 单位中心→分到的敌中心, 含高度, 只在按住时存在)。 */
    OU_PrevDraw(s_FaPrevBuf, s_FaPrevN, locked, w, h, pitch, 255, 0, 0, 255, 1);
    s_FaLocked = 0;
    unlockFn(surf, NULL);
}
