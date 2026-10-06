/* WeaponRange.cpp - 单选按住 Ctrl 显示单位攻击范围圈。
 *
 * 触发: 只选中 1 个单位 (Unit/Infantry/Aircraft) 且按住 Ctrl 才画。
 * 内容: 地面圈 (蓝, 当前武器最大射程, 与引擎原生圈同色) + 最小射程
 * (红, MinimumRange==0 不画) + 空中圈 (青虚线, 本单位对空武器射程)。
 * 当前武器 = CurrentWeaponNumber 下标 + 精英级换 EliteWeapon (实际数据);
 * 对空武器在下标 0/1 里找第一个可对空者 (找不到则无青圈;
 * 与当前同把时同半径, 虚线区分)。
 * 高度: 只取单位自身所在高度定圆心, 半径不做地形修正。
 *
 * 取数 (全只读, 失败静默不画, 联机安全):
 *   类型: Unit/Aircraft+0x6C4, Infantry+0x6C0 (ToolTip/DeployPreview 同式),
 *   经 TechnoType 数组 (0xA8EB00) 做 InTypeSet 校验。
 *   武器: TechnoType::GetWeapon(0x7177C0)/GetEliteWeapon(0x7177E0)
 *   (thiscall ECX=类型, 栈下标, 回 WeaponStruct*, 首 dword=WeaponType*)。
 *   精英/下标: Techno 内 Owner 指针锚定 (本家 CurrentPlayer 0xA83D4C):
 *   命中 house 的槽位若 +4 处浮点在 [-0.5,3] 且 -20 处下标在 [0,17]
 *   且 -16 处军衔在 [0,2], 即 InitialOwner, 由此得 Veterancy(+4,
 *   >=2.0 为精英) 与 CurrentWeaponNumber(-20)。找不到则按 0/0 退化。
 *   Range: WeaponType 内 (Range,MinimumRange) 相邻 int 对, 运行时投票
 *   pin (多把武器一致才缓存, 失败不画)。对空经 WeaponType::
 *   AllowedThreats(0x772A90) & Air(0x4)。铁律:
 *   (a) ECX 必须是 WeaponType*, 传弹头进去必崩;
 *   (b) 只判 Air 位, 不得加 <=0xFFFF 上限 (掩码最大位 0x10000)。
 *
 * 绘制: 圆心经 TacticalClass::CoordsToClient(0x6D2140) 投影 (与
 * DeployPreview 同系, 之后过 SB_ViewToScreen 折缩放档), 世界圆按
 * 72 点逐点投影连线 (1px 细线, Blend16)。圆上各点取圆心同一 Z
 * (自身高度, 不跟地形起伏)。
 * 构建见 build-fast.bat (32 位 mingw, 无 CRT, 与 MOExperience 同一 DLL)。
 * 渲染挂 MOExperience_MessageDrawHook (见 MOExperience.cpp)。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "ScaleBar.h"
#include "WeaponRange.h"

#define WR_SECTION "WeaponRange"

/* ---------------- 私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define WR_TYPARRAY 0xA8EB00u
#define WR_OFF_VEC_ITEMS 4u
#define WR_OFF_VEC_COUNT 16u

#define WR_OFF_T_UNIT 0x6C4u
#define WR_OFF_T_AIRCRAFT 0x6C4u
#define WR_OFF_T_INFANTRY 0x6C0u

typedef void* __attribute__((thiscall)) (*Fn_WRGetWeapon)(void* type, int idx);
typedef int __attribute__((thiscall)) (*Fn_WRThreats)(void* weaponType);
typedef int __attribute__((thiscall)) (*Fn_WRC2C)(void* tac,
    const CoordStruct* c, Point2D* out);
typedef void* (__fastcall *WR_LockFn)(void*, void*, int, int);
typedef int (__fastcall *WR_UnlockFn)(void*, void*);

#define FN_WR_WEAPON ((Fn_WRGetWeapon)(void*)0x7177C0u)
#define FN_WR_ELITEWEAPON ((Fn_WRGetWeapon)(void*)0x7177E0u)
#define FN_WR_THREATS ((Fn_WRThreats)(void*)0x772A90u)
#define FN_WR_C2C ((Fn_WRC2C)(void*)0x6D2140u)
#define WR_TAC_INSTANCE 0x887324u
#define WR_THREAT_AIR 0x4

#define VT_WR_LOCK   0x5C
#define VT_WR_UNLOCK 0x60

/* ---------------- 配置 ---------------- */

static int g_WrInit = 0;
static int g_WrEnable = 1;

static void WR_EnsureInit(void)
{
    if (g_WrInit) return;
    g_WrInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_WrEnable = GetPrivateProfileIntA(WR_SECTION, "Enable", 1, g_IniPath) ? 1 : 0;
    LogLine(g_WrEnable ? "wrange: ON (ctrl+single)" : "wrange: OFF");
}

/* ---------------- 类型取得 (DeployPreview 同式) ---------------- */

static int WR_TypeCount(void*** itemsOut)
{
    void** items;
    int cnt;
    *itemsOut = 0;
    cnt = *(int*)(WR_TYPARRAY + WR_OFF_VEC_COUNT);
    items = *(void***)(WR_TYPARRAY + WR_OFF_VEC_ITEMS);
    if (cnt <= 0 || cnt > 20000) return 0;
    if (!items || !IsPlausiblePtr(items)) return 0;
    *itemsOut = items;
    return cnt;
}

static int WR_InTypeSet(void* p)
{
    void** items = 0;
    int cnt, i;
    cnt = WR_TypeCount(&items);
    if (!cnt) return 0;
    for (i = 0; i < cnt; i++)
        if (items[i] == p) return 1;
    return 0;
}

static void* WR_TypeOf(void* u, int w)
{
    unsigned off;
    void* p;
    if (!u || !IsPlausiblePtr(u)) return 0;
    if (w == ABS_UNIT || w == ABS_AIRCRAFT) off = WR_OFF_T_UNIT;
    else if (w == ABS_INFANTRY) off = WR_OFF_T_INFANTRY;
    else return 0;
    p = *(void**)((char*)u + off);
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (!WR_InTypeSet(p)) return 0;
    return p;
}

/* ---------------- 精英/当前武器下标 (Owner 锚定自校准) ----------------
 * InitialOwner 命中时: +4=Veterancy(float), -20=CurrentWeaponNumber,
 * -16=CurrentRanking。找不到锚点则退化 elite=0/idx=0。 */

static int WR_EliteIndex(void* u, int* eliteOut, int* idxOut)
{
    void* house;
    unsigned o;
    *eliteOut = 0;
    *idxOut = 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house || !IsPlausiblePtr(house)) return 0;
    for (o = 0x80u; o <= 0x200u; o += 4u)
    {
        void* hit;
        float f;
        int cwn, rank;
        if (o < 20u) continue;
        hit = *(void**)((char*)u + o);
        if (hit != house) continue;
        f = *(float*)((char*)u + o + 4u);
        cwn = *(int*)((char*)u + o - 20u);
        rank = *(int*)((char*)u + o - 16u);
        if (f < -0.5f || f > 3.0f) continue;
        if (cwn < 0 || cwn > 17) continue;
        if (rank < 0 || rank > 2) continue;
        *eliteOut = (f >= 2.0f) ? 1 : 0;
        *idxOut = cwn;
        return 1;
    }
    return 0;
}

/* ---------------- Range 偏移投票 pin ----------------
 * (Range,MinimumRange) 为 WeaponType 内相邻 int 对: 256<=Range<=40 格,
 * 0<=Min<=Range。多把武器 (主/精英/副) 一致才缓存。 */

#define WR_OFF_NONE -1

static int g_WrRangeOff = WR_OFF_NONE;
static int g_WrLoggedOff = 0;

static int WR_PairSane(void* wt, int off)
{
    int r, m;
    if (off < 0x40 || off > 0x1C0) return 0;
    if (!IsPlausiblePtr((char*)wt + off + 8)) return 0;
    r = *(int*)((char*)wt + off);
    m = *(int*)((char*)wt + off + 4);
    if (r < 256 || r > 40 * 256) return 0;
    if (m < 0 || m > r) return 0;
    if (m > 20 * 256) return 0;
    return 1;
}

static int WR_StrictSane(void* wt, int off)
{
    int r, m;
    if (!WR_PairSane(wt, off)) return 0;
    r = *(int*)((char*)wt + off);
    m = *(int*)((char*)wt + off + 4);
    if (r < 2 * 256 || r > 30 * 256) return 0;
    if (r % 256 != 0) return 0;
    if (m != 0 && m % 256 != 0) return 0;
    return 1;
}

/* 收集候选 WeaponType* (去重, 上限 4): 主/精英 × 下标0/1。 */
static int WR_CollectWT(void* type, void** out)
{
    int n = 0, i, k;
    for (i = 0; i < 2; i++)
    {
        void* ws;
        void* wt;
        ws = FN_WR_WEAPON(type, i);
        if (ws && IsPlausiblePtr(ws))
        {
            wt = *(void***)ws;
            if (wt && IsPlausiblePtr(wt))
            {
                for (k = 0; k < n; k++)
                    if (out[k] == wt) break;
                if (k >= n && n < 4) out[n++] = wt;
            }
        }
        ws = FN_WR_ELITEWEAPON(type, i);
        if (ws && IsPlausiblePtr(ws))
        {
            wt = *(void***)ws;
            if (wt && IsPlausiblePtr(wt))
            {
                for (k = 0; k < n; k++)
                    if (out[k] == wt) break;
                if (k >= n && n < 4) out[n++] = wt;
            }
        }
    }
    return n;
}

static int WR_FindRangeOff(void** wts, int n)
{
    int off, best = WR_OFF_NONE, bestVotes = 0;
    for (off = 0x60; off <= 0x1C0; off += 4)
    {
        int v = 0, k;
        for (k = 0; k < n; k++)
            if (WR_PairSane(wts[k], off)) v++;
        if (v > bestVotes) { bestVotes = v; best = off; }
    }
    if (bestVotes < n || n <= 0) return WR_OFF_NONE;
    if (n < 2)
    {
        /* 单样本加严 (防 Damage 等偶然命中): 整格 + 常规射程段。 */
        if (!WR_StrictSane(wts[0], best)) return WR_OFF_NONE;
    }
    return best;
}

/* ---------------- 对空加成 AirRangeBonus ----------------
 * TechnoType+0x68C (YRpp 字段顺序手算: AirRangeBonus→Weapon[0] 共 0x20C,
 * TypeList=0x1C 含 unknown_18, 与 DP 向量偏移 items+4/count+16 自洽;
 * CameoFile 对齐注释 align_6EF 交叉确认)。
 * 结构双锚: Weapon[0] 偏移 ==0x898 且 Elite[0]-Weapon[0]==0x1FC
 * (实测: W0 与手算 0x20C 完全一致, 但两数组间隔比 YRpp 多 0x18,
 * 中间有未建模字段, 以实测为准), 否则本局禁用加成。
 * 编码: int 轻子数 (与 Range 同制, GGI 实测 768=3 格);
 * 超 10 格按 float 格试, 都不像则 0 (fail-soft)。
 * 每类型缓存 (含失败), 引擎真值。 */

#define WR_OFF_AIRRANGE 0x68Cu
#define WR_EXP_W0 0x898
#define WR_EXP_WSPAN 0x1FC

static void* g_WrBonusType = 0;
static int g_WrBonus = 0;
static int g_WrBonusDone = 0;   /* 本类型已求值 (成功失败都缓存) */
static int g_WrBonusLogged = 0;

static int WR_AirBonus(void* type)
{
    void* ws0;
    void* wse;
    int w0, we;
    unsigned raw;
    float f;
    if (!type || !IsPlausiblePtr(type)) return 0;
    if (type == g_WrBonusType && g_WrBonusDone) return g_WrBonus;
    g_WrBonusType = type;
    g_WrBonus = 0;
    g_WrBonusDone = 1;
    ws0 = FN_WR_WEAPON(type, 0);
    wse = FN_WR_ELITEWEAPON(type, 0);
    if (!ws0 || !IsPlausiblePtr(ws0) || !wse || !IsPlausiblePtr(wse)) return 0;
    w0 = (int)((char*)ws0 - (char*)type);
    we = (int)((char*)wse - (char*)type);
    if (w0 != WR_EXP_W0 || we - w0 != WR_EXP_WSPAN)
    {
        if (!g_WrBonusLogged)
        {
            char buf[96], b1[16], b2[16];
            unsigned q = 0;
            g_WrBonusLogged = 1;
            q = AppendCStr(buf, q, "wrange: airbonus OFF struct mismatch w0=", sizeof(buf));
            UtoA((unsigned)w0, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
            q = AppendCStr(buf, q, " span=", sizeof(buf));
            UtoA((unsigned)(we - w0), b2); q = AppendCStr(buf, q, b2, sizeof(buf));
            (void)q;
            LogLine(buf);
        }
        return 0;
    }
    raw = *(unsigned*)((char*)type + WR_OFF_AIRRANGE);
    f = *(float*)((char*)type + WR_OFF_AIRRANGE);
    if (raw <= 10u * 256u) g_WrBonus = (int)raw;   /* int 轻子数 */
    else if (f >= 0.0f && f <= 10.0f) g_WrBonus = (int)(f * 256.0f);
    else return 0;   /* 数值不像加成: 当 0 (已缓存, 不刷日志) */
    {
        char buf[96], b1[16], b2[16];
        unsigned q = 0;
        q = AppendCStr(buf, q, "wrange: airbonus=", sizeof(buf));
        UtoA((unsigned)g_WrBonus, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " raw=", sizeof(buf));
        UtoA(raw, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
        (void)q;
        LogLine(buf);
    }
    return g_WrBonus;
}

/* ---------------- 取数结果 ---------------- */

typedef struct
{
    int ok;
    int range;   /* 当前武器最大射程, leptons */
    int minRange;/* 当前武器最小射程, leptons */
    int aaRange; /* 对空武器射程, leptons (0=无) */
    CoordStruct center;
} WRInfo;

/* 可对空? (ECX=WeaponType, 只判 Air 位, 无上限门。) */
static int WR_CanAir(void* wt)
{
    int th;
    if (!wt || !IsPlausiblePtr(wt)) return 0;
    th = FN_WR_THREATS(wt);
    return (th & WR_THREAT_AIR) ? 1 : 0;
}

static WRInfo WR_Resolve(void* u)
{
    WRInfo r;
    void* type;
    int w, elite, idx;
    void* ws;
    void* wt;
    int j;
    r.ok = 0; r.range = 0; r.minRange = 0; r.aaRange = 0;
    r.center.X = r.center.Y = r.center.Z = 0;
    w = OU_WhatAmI(u);
    if (w != ABS_UNIT && w != ABS_AIRCRAFT && w != ABS_INFANTRY) return r;
    type = WR_TypeOf(u, w);
    if (!type) return r;
    WR_EliteIndex(u, &elite, &idx);   /* 失败即 0/0 退化 */
    if (idx < 0 || idx > 17) idx = 0;
    ws = elite ? FN_WR_ELITEWEAPON(type, idx) : FN_WR_WEAPON(type, idx);
    if (!ws || !IsPlausiblePtr(ws)) return r;
    wt = *(void***)ws;
    if (!wt || !IsPlausiblePtr(wt)) return r;
    /* Range 偏移: 缓存命中先验活, 否则投票。 */
    if (g_WrRangeOff != WR_OFF_NONE && !WR_PairSane(wt, g_WrRangeOff))
        g_WrRangeOff = WR_OFF_NONE;
    if (g_WrRangeOff == WR_OFF_NONE)
    {
        void* wts[4];
        int n;
        n = WR_CollectWT(type, wts);
        if (n <= 0) return r;
        g_WrRangeOff = WR_FindRangeOff(wts, n);
        if (g_WrRangeOff == WR_OFF_NONE) return r;
        if (!g_WrLoggedOff)
        {
            char buf[64], b1[16];
            unsigned q = 0;
            g_WrLoggedOff = 1;
            q = AppendCStr(buf, q, "wrange: rangeOff=", sizeof(buf));
            UtoA((unsigned)g_WrRangeOff, b1);
            q = AppendCStr(buf, q, b1, sizeof(buf));
            (void)q;
            LogLine(buf);
        }
    }
    r.range = *(int*)((char*)wt + g_WrRangeOff);
    r.minRange = *(int*)((char*)wt + g_WrRangeOff + 4);
    if (r.range < 256 || r.range > 40 * 256) return r;
    if (r.minRange < 0 || r.minRange > r.range) r.minRange = 0;
    /* 地面圈=当前武器; 空中圈=下标 0/1 里第一个可对空者 (同精英档)。 */
    if (WR_CanAir(wt)) r.aaRange = r.range;
    else
    {
        for (j = 0; j < 2; j++)
        {
            void* wsj;
            void* wtj;
            int rj;
            if (j == idx) continue;
            wsj = elite ? FN_WR_ELITEWEAPON(type, j) : FN_WR_WEAPON(type, j);
            if (!wsj || !IsPlausiblePtr(wsj)) continue;
            wtj = *(void***)wsj;
            if (!wtj || !IsPlausiblePtr(wtj)) continue;
            if (!WR_CanAir(wtj)) continue;
            if (!WR_PairSane(wtj, g_WrRangeOff)) continue;
            rj = *(int*)((char*)wtj + g_WrRangeOff);
            if (rj < 256 || rj > 40 * 256) continue;
            r.aaRange = rj;
            break;
        }
    }
    /* 空中圈 = 对空武器射程 + 本类型 AirRangeBonus (地面圈不加)。 */
    if (r.aaRange >= 256) r.aaRange += WR_AirBonus(type);
    if (!OU_GetCoords(u, &r.center)) return r;
    r.ok = 1;
    return r;
}

/* ---------------- 绘制 ---------------- */

static unsigned char* s_WrLocked = 0;
static int s_WrW = 0, s_WrH = 0, s_WrPitch = 0;

static void WR_Plot(int x, int y, int cr, int cg, int cb, unsigned a)
{
    unsigned short* dstRow;
    if (x < 0 || y < 0 || x >= s_WrW || y >= s_WrH) return;
    dstRow = (unsigned short*)(s_WrLocked + y * s_WrPitch);
    dstRow[x] = Blend16(dstRow[x], (unsigned)cr, (unsigned)cg, (unsigned)cb, a);
}

/* 1px 细线 (Bresenham, 自带裁剪)。 */
static void WR_ThinLine(int x0, int y0, int x1, int y1,
                        int cr, int cg, int cb, unsigned a)
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
        WR_Plot(x, y, cr, cg, cb, a);
        if (x == x1 && y == y1) break;
        e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

/* 世界点 → 屏像素 (DeployPreview 同系: CoordsToClient + 缩放折算)。 */
static int WR_ProjLep(int lx, int ly, int lz, int* px, int* py)
{
    void* tac = *(void**)WR_TAC_INSTANCE;
    CoordStruct c;
    Point2D o;
    if (!tac || !IsPlausiblePtr(tac)) return 0;
    c.X = lx; c.Y = ly; c.Z = lz;
    o.X = 0; o.Y = 0;
    FN_WR_C2C(tac, &c, &o);
    if (!SB_ViewToScreen(o.X, o.Y, px, py)) { *px = o.X; *py = o.Y; }
    return 1;
}

#define WR_SEGS 72

/* 世界圆 (圆心同 Z, 自身高度) → 屏折线。
 * 无 libm (构建 -nostdlib): 用旋转迭代代替 cos/sin
 * (步进 2π/72, cos=0.9961946980917455, sin=0.08715574274765817)。
 * dash=1 时画虚线 (对空圈与最大圈同半径重合时仍可分辨)。 */
static void WR_Circle(int cx, int cy, int cz, int radius,
                      int cr, int cg, int cb, unsigned a, int dash)
{
    int i, px0 = 0, py0 = 0, px1, py1, first = 1;
    double dx, dy;
    const double c = 0.9961946980917455;
    const double s = 0.08715574274765817;
    if (radius < 128) return;
    dx = (double)radius;
    dy = 0.0;
    for (i = 0; i <= WR_SEGS; i++)
    {
        double nx, ny;
        int lx = cx + (int)dx;
        int ly = cy + (int)dy;
        if (!WR_ProjLep(lx, ly, cz, &px1, &py1)) return;
        if (!first && !(dash && (i % 4) >= 2))
            WR_ThinLine(px0, py0, px1, py1, cr, cg, cb, a);
        first = 0;
        px0 = px1; py0 = py1;
        nx = dx * c - dy * s;
        ny = dx * s + dy * c;
        dx = nx; dy = ny;
    }
}

void WeaponRange_Draw(void* surf, int w, int h, int pitch)
{
    unsigned char* vt;
    WR_LockFn lockFn;
    WR_UnlockFn unlockFn;
    unsigned char* locked;
    void** items;
    int selN;
    void* u;
    WRInfo r;
    extern int g_Calibrated;
    WR_EnsureInit();
    if (!g_WrEnable) return;
    if (!OU_KeyDown(VK_CONTROL)) return;   /* 按住 Ctrl 才显示 */
    if (!g_Calibrated) return;
    if (!surf || !IsPlausiblePtr(surf)) return;
    if (w <= 0 || h <= 0 || pitch <= 0) return;
    selN = *(int*)ADDR_SELOBJ_COUNT;
    items = *(void***)ADDR_SELOBJ_ITEMS;
    if (selN != 1 || !items || !IsPlausiblePtr(items)) return;
    u = items[0];
    if (!u || !IsPlausiblePtr(u)) return;
    r = WR_Resolve(u);
    if (!r.ok) return;
    vt = *(unsigned char**)surf;
    if (!vt || !IsPlausiblePtr(vt)) return;
    lockFn = (WR_LockFn)(*(void**)(vt + VT_WR_LOCK));
    unlockFn = (WR_UnlockFn)(*(void**)(vt + VT_WR_UNLOCK));
    if (!lockFn || !unlockFn) return;
    locked = (unsigned char*)lockFn(surf, NULL, 0, 0);
    if (!locked || !IsPlausiblePtr(locked)) return;
    s_WrLocked = locked; s_WrW = w; s_WrH = h; s_WrPitch = pitch;
    /* 地面蓝圈 (当前武器) + 最小红圈 (0 不画) + 空中青虚线圈。 */
    WR_Circle(r.center.X, r.center.Y, r.center.Z, r.range, 64, 128, 255, 230, 0);
    if (r.minRange >= 256)
        WR_Circle(r.center.X, r.center.Y, r.center.Z, r.minRange, 255, 64, 64, 230, 0);
    if (r.aaRange >= 256)
        WR_Circle(r.center.X, r.center.Y, r.center.Z, r.aaRange, 64, 255, 255, 230, 1);
    s_WrLocked = 0;
    unlockFn(surf, NULL);
}
