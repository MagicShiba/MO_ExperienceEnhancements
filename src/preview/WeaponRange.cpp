/* WeaponRange.cpp - 单选按住 Ctrl 显示单位攻击范围圈。
 *
 * 蓝=最大射程, 红=最小射程, 青虚线=对空, 绿=部署, 黄=探测, 紫=识别。
 * 圆心取单位所在位置的地表投影 (含桥面); 对空圈高度由 WeaponRangeAAZ 控制。
 * 只读 + 渲染面绘制, 联机安全; 细节见 notes/攻击范围显示.md。
 */

#include <windows.h>
#include "GdiText.h"
#include "OrderUtil.h"
#include "ScaleBar.h"
#include "WeaponRange.h"

#define WR_SECTION "Features"

/* ---------------- 私有地址 (共享部分见 OrderUtil.h) ---------------- */

#define WR_OFF_T_UNIT 0x6C4u      /* 飞行单位与单位同偏移 */
#define WR_OFF_T_INFANTRY 0x6C0u

typedef void* __attribute__((thiscall)) (*Fn_WRGetWeapon)(void* type, int idx);
typedef int __attribute__((thiscall)) (*Fn_WRThreats)(void* weaponType);
typedef int __attribute__((thiscall)) (*Fn_WRC2C)(void* tac,
    const CoordStruct* c, Point2D* out);
#define FN_WR_WEAPON ((Fn_WRGetWeapon)(void*)0x7177C0u)
#define FN_WR_ELITEWEAPON ((Fn_WRGetWeapon)(void*)0x7177E0u)
#define FN_WR_THREATS ((Fn_WRThreats)(void*)0x772A90u)
#define FN_WR_C2C ((Fn_WRC2C)(void*)0x6D2140u)
#define WR_TAC_INSTANCE 0x887324u
#define WR_THREAT_AIR 0x4

/* ---------------- 配置 ---------------- */

static int g_WrInit = 0;
static int g_WrEnable = 1;
static int g_WrAAZ = 0;

static void WR_EnsureInit(void)
{
    unsigned aaZ;
    if (g_WrInit) return;
    g_WrInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_WrEnable = GetPrivateProfileIntA(WR_SECTION, "WeaponRange", 1, g_IniPath) ? 1 : 0;
    aaZ = GetPrivateProfileIntA(WR_SECTION, "WeaponRangeAAZ", 0, g_IniPath);
    g_WrAAZ = (aaZ > 8192u) ? 8192 : (int)aaZ;
    if (!g_WrEnable) LogLine("wrange: OFF");
    else if (g_WrAAZ > 0) LogLine("wrange: ON (ctrl+single, aa-high)");
    else LogLine("wrange: ON (ctrl+single, aa-ground)");
}

/* ---------------- 类型取得 (DeployPreview 同式) ---------------- */

static int WR_InTypeSet(void* p) { return OU_InVec(ADDR_TYPARRAY, p); }

static void* WR_TypeOf(void* u, int w)
{
    unsigned off;
    void* p;
    if (!u || !IsPlausiblePtr(u)) return 0;
    if (w == ABS_UNIT || w == ABS_AIRCRAFT) off = WR_OFF_T_UNIT;   /* 两者同偏移 */
    else if (w == ABS_INFANTRY) off = WR_OFF_T_INFANTRY;
    else return 0;
    p = *(void**)((char*)u + off);
    if (!p || !IsPlausiblePtr(p)) return 0;
    if (!WR_InTypeSet(p)) return 0;
    return p;
}

/* ---------------- 精英/当前武器下标 (Owner 指针锚定) ----------------
 * InitialOwner: +4=Veterancy(float,>=2 精英), -20=CurrentWeaponNumber,
 * -16=CurrentRanking。找不到则 elite=0/idx=0。 */

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
 * (Range,MinimumRange) 相邻 int 对, 多把武器一致才缓存。 */

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
 * TechnoType+0x68C, int 轻子数 (与 Range 同制)。结构锚: Weapon[0]
 * 偏移 0x898 且 Elite[0] 差 0x1FC, 不符则本类型无加成 (fail-soft)。 */

#define WR_OFF_AIRRANGE 0x68Cu
#define WR_EXP_W0 0x898
#define WR_EXP_WSPAN 0x1FC
/* 隐形探测 SensorsSight +0x5F0, 间谍识别 DetectDisguiseRange +0x5F4 (格)。 */
#define WR_OFF_SENSORSSIGHT 0x5F0u
#define WR_OFF_DISGRANGE 0x5F4u

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
    if (raw <= 10u * 256u) g_WrBonus = (int)raw;
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
    int aaRange; /* 对空武器射程 (+AirRangeBonus), leptons (0=无) */
    int depRange;  /* 部署武器作用半径 (弹头 CellSpread 为主), leptons (0=无) */
    int thCur;     /* 诊断: 当前武器威胁位掩码 */
    int depDf;     /* 诊断: DeployFire 标志读值 */
    int depFw;     /* 诊断: DeployFireWeapon 读值 */
    int depCs;     /* 诊断: 部署弹头 CellSpread 轻子 */
    int depWhy;    /* 诊断: 0=画出 1=副空 2=同蓝半径 4=射程离谱 */
    int sensRange; /* 隐形探测 SensorsSight, leptons (0=无) */
    int disRange;  /* 间谍识别 DetectDisguiseRange, leptons (0=无) */
    CoordStruct center;
} WRInfo;

/* 探测/识别格数 (0=无能力; 范围外当无, fail-soft)。 */
static void WR_SenseRanges(void* type, int* sensCells, int* disCells)
{
    int s, d;
    *sensCells = 0;
    *disCells = 0;
    if (!type || !IsPlausiblePtr(type)) return;
    if (!IsPlausiblePtr((char*)type + WR_OFF_DISGRANGE + 4)) return;
    s = *(int*)((char*)type + WR_OFF_SENSORSSIGHT);
    d = *(int*)((char*)type + WR_OFF_DISGRANGE);
    if (s >= 1 && s <= 40) *sensCells = s;
    if (d >= 1 && d <= 40) *disCells = d;
}

/* 威胁位掩码 (ECX=WeaponType)。可对空 = Air 位。 */
static int WR_Threats(void* wt)
{
    if (!wt || !IsPlausiblePtr(wt)) return 0;
    return FN_WR_THREATS(wt);
}

static int WR_CanAir(void* wt)
{
    return (WR_Threats(wt) & WR_THREAT_AIR) ? 1 : 0;
}

/* 搜索分支跳过低伤害占位扫描器 (Damage 在 Range-16; 当前/部署分支不用,
 * 心控与辐射爆发 Damage 也是 0/1)。 */
static int WR_LowDamage(void* wt, int rangeOff)
{
    int dmg;
    if (!wt || !IsPlausiblePtr(wt)) return 1;
    if (rangeOff < 28) return 1;
    dmg = *(int*)((char*)wt + rangeOff - 16);
    if (dmg < -100000 || dmg > 1000000) return 1;
    return (dmg <= 1) ? 1 : 0;
}

/* 类型/武器注册名读取 (SmartArea 同式自校准; 武器名允许小写)。
 * 单位与武器各 pin 各的偏移, 读不到回 0 (不过滤, fail-open)。 */
static int g_WrIdOff = -1;
static int g_WrWIdOff = -1;

static int WR_StrOk(const char* s, int lower)
{
    int n = 0;
    if (!s || !IsPlausiblePtr(s)) return 0;
    while (n < 24)
    {
        char c = s[n];
        if (!c) return (n > 0) ? 1 : 0;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-') { n++; continue; }
        if (lower && (c >= 'a' && c <= 'z')) { n++; continue; }
        return 0;
    }
    return 0;
}

static int WR_IDAt(void* p, int* pinOff, int lower, char* out)
{
    static const unsigned cands[3] = { 0x20u, 0x24u, 0x28u };
    int i, k;
    if (!p || !IsPlausiblePtr(p) || !pinOff || !out) return 0;
    if (*pinOff >= 0)
    {
        const char* s = (const char*)p + (unsigned)*pinOff;
        if (!WR_StrOk(s, lower)) *pinOff = -1;
        else { for (k = 0; k < 24; k++) out[k] = s[k]; return 1; }
    }
    for (i = 0; i < 3; i++)
    {
        const char* s = (const char*)p + cands[i];
        if (!WR_StrOk(s, lower)) continue;
        *pinOff = (int)cands[i];
        for (k = 0; k < 24; k++) out[k] = s[k];
        return 1;
    }
    return 0;
}

static int WR_Streq(const char* a, const char* b)
{
    int i;
    for (i = 0; i < 24; i++)
    {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

/* 已知占位武器名 (精确匹配; 误杀率为零, 真武器不可能叫这名)。 */
static int WR_DummyName(void* wt)
{
    static const char* dums[3] = { "VirtualScanner", "EngineerScanner", "TankScanner" };
    char id[24];
    int k, d;
    for (k = 0; k < 24; k++) id[k] = 0;
    if (!wt || !IsPlausiblePtr(wt)) return 0;
    if (!WR_IDAt(wt, &g_WrWIdOff, 1, id)) return 0;
    for (d = 0; d < 3; d++)
        if (WR_Streq(id, dums[d])) return 1;
    return 0;
}

/* 部署开火: 标志 +0x6AC, 用哪把 +0x6A8 (0=主,1=副,默认1), 不可用顺延。
 * 半径看弹头 CellSpread: WarheadType+0x124 float 格, 弹头指针在
 * Range-8; 读不到回落武器 Range。 */
#define WR_OFF_DEPLOYF 0x6ACu
#define WR_OFF_DEPLOYFW 0x6A8u
#define WR_OFF_WARHEAD 8
#define WR_OFF_CELLSPREAD 0x124u

/* 弹头 CellSpread (轻子, 0=不可用)。 */
static int WR_CellSpread(void* wt, int rangeOff)
{
    void* wh;
    float cs;
    if (!wt || !IsPlausiblePtr(wt)) return 0;
    if (rangeOff < 28) return 0;
    wh = *(void**)((char*)wt + rangeOff - WR_OFF_WARHEAD);
    if (!wh || !IsPlausiblePtr(wh)) return 0;
    if (!IsPlausiblePtr((char*)wh + WR_OFF_CELLSPREAD + 4)) return 0;
    cs = *(float*)((char*)wh + WR_OFF_CELLSPREAD);
    if (cs < 0.5f || cs > 40.0f) return 0;
    return (int)(cs * 256.0f);
}

static void* s_WrLogWt = 0;

static WRInfo WR_Resolve(void* u)
{
    WRInfo r;
    void* type;
    int w, elite, idx;
    void* ws;
    void* wt;
    int j;
    int sensC = 0, disC = 0;
    r.ok = 0; r.range = 0; r.minRange = 0; r.aaRange = 0;
    r.thCur = 0;
    r.depRange = 0; r.depDf = 0; r.depFw = 1; r.depCs = 0; r.depWhy = 0;
    r.sensRange = 0; r.disRange = 0;
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
    /* 地面圈=当前武器; 空中圈=下标 0/1 里第一个可对空者
     * (占位扫描器按名 + 低伤害跳过)。 */
    r.thCur = WR_Threats(wt);
    if ((r.thCur & WR_THREAT_AIR) != 0) r.aaRange = r.range;
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
            if (WR_LowDamage(wtj, g_WrRangeOff)) continue;
            if (WR_DummyName(wtj)) continue;
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
    /* 部署圈: 仅 DeployFire 单位 (门控 +0x6AC; 单字节读无对齐要求,
     * 不得用 IsPlausiblePtr guard)。
     * 部署用哪把严格按 DeployFireWeapon (+0x6A8): 0=主, 1=副, 缺键默认 1
     * (YURI/DESO/GGI 均无此键且部署都用副; 自爆类显式填 0 用主)。
     * 所选不可用 (空/占位) 则顺延另一槽; 半径 = 弹头 CellSpread
     * (原地生效按落点杀伤), 读不到回落武器 Range; 与蓝圈同半径不画。 */
    {
        void* wsd;
        void* wtd;
        int rd, cs, rr;
        int depfw = 1;
        int depIdx;
        r.depDf = *(unsigned char*)((char*)type + WR_OFF_DEPLOYF);
        if (r.depDf != 1) r.depDf = 0;
        if (IsPlausiblePtr((char*)type + WR_OFF_DEPLOYFW + 4))
        {
            depfw = *(int*)((char*)type + WR_OFF_DEPLOYFW);
            if (depfw < 0 || depfw > 17) depfw = 1;
        }
        r.depFw = depfw;
        if (r.depDf == 1)
        {
            depIdx = (depfw == 0) ? 0 : 1;
            wsd = elite ? FN_WR_ELITEWEAPON(type, depIdx) : FN_WR_WEAPON(type, depIdx);
            wtd = (wsd && IsPlausiblePtr(wsd)) ? *(void***)wsd : 0;
            if (!wtd || !IsPlausiblePtr(wtd))
            {
                depIdx = depIdx ? 0 : 1;
                wsd = elite ? FN_WR_ELITEWEAPON(type, depIdx) : FN_WR_WEAPON(type, depIdx);
                wtd = (wsd && IsPlausiblePtr(wsd)) ? *(void***)wsd : 0;
            }
            if (!wtd || !IsPlausiblePtr(wtd)) r.depWhy = 1;
            else if (WR_DummyName(wtd)) r.depWhy = 3;
            else if (!WR_PairSane(wtd, g_WrRangeOff)) r.depWhy = 4;
            else
            {
                rd = *(int*)((char*)wtd + g_WrRangeOff);
                cs = WR_CellSpread(wtd, g_WrRangeOff);
                r.depCs = cs;
                rr = 0;
                if (cs >= 128) rr = cs;
                else if (rd >= 256 && rd <= 40 * 256) rr = rd;
                if (rr >= 128 && rr != r.range) r.depRange = rr;
                else r.depWhy = (rr == r.range && rr >= 128) ? 2 : 4;
            }
        }
    }
    /* 探测/识别圈 (格→轻子, 0 即无能力不画)。 */
    WR_SenseRanges(type, &sensC, &disC);
    r.sensRange = sensC * 256;
    r.disRange = disC * 256;
    if (!OU_GetCoords(u, &r.center)) return r;
    /* 圆心统一取地表投影 (含桥面), 不考虑自身高度; 对空圈绘制时再按配置抬高。 */
    {
        int gz;
        if (OU_SurfaceZAt(r.center.X, r.center.Y, r.center.Z, &gz))
            r.center.Z = gz;
    }
    r.ok = 1;
    /* 诊断 (每把新武器一行, 远测用)。 */
    if (wt != s_WrLogWt)
    {
        char buf[224], b1[16], b2[16], b3[16], b4[16], b5[16];
        char tid[24], wid[24];
        int k;
        unsigned q = 0;
        s_WrLogWt = wt;
        for (k = 0; k < 24; k++) tid[k] = wid[k] = 0;
        WR_IDAt(type, &g_WrIdOff, 0, tid);
        WR_IDAt(wt, &g_WrWIdOff, 1, wid);
        q = AppendCStr(buf, q, "wrange: wt=", sizeof(buf));
        HtoA((unsigned)(size_t)wt, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " tid=", sizeof(buf));
        q = AppendCStr(buf, q, tid, sizeof(buf));
        q = AppendCStr(buf, q, " wid=", sizeof(buf));
        q = AppendCStr(buf, q, wid, sizeof(buf));
        q = AppendCStr(buf, q, " elite=", sizeof(buf));
        UtoA((unsigned)elite, b2); q = AppendCStr(buf, q, b2, sizeof(buf));
        q = AppendCStr(buf, q, " idx=", sizeof(buf));
        UtoA((unsigned)(idx < 0 ? 0 : idx), b3); q = AppendCStr(buf, q, b3, sizeof(buf));
        q = AppendCStr(buf, q, " r=", sizeof(buf));
        UtoA((unsigned)r.range, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " aa=", sizeof(buf));
        UtoA((unsigned)r.aaRange, b4); q = AppendCStr(buf, q, b4, sizeof(buf));
        q = AppendCStr(buf, q, " sens=", sizeof(buf));
        UtoA((unsigned)sensC, b5); q = AppendCStr(buf, q, b5, sizeof(buf));
        q = AppendCStr(buf, q, " dis=", sizeof(buf));
        UtoA((unsigned)disC, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " dep=", sizeof(buf));
        UtoA((unsigned)r.depRange, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " depf=", sizeof(buf));
        UtoA((unsigned)r.depDf, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " dfw=", sizeof(buf));
        UtoA((unsigned)r.depFw, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " csp=", sizeof(buf));
        UtoA((unsigned)r.depCs, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " why=", sizeof(buf));
        UtoA((unsigned)r.depWhy, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        q = AppendCStr(buf, q, " th=", sizeof(buf));
        UtoA((unsigned)r.thCur, b1); q = AppendCStr(buf, q, b1, sizeof(buf));
        (void)q;
        LogLine(buf);
    }
    return r;
}

/* ---------------- 绘制 ---------------- */

static unsigned char* s_WrLocked = 0;
static int s_WrW = 0, s_WrH = 0, s_WrPitch = 0;

/* 1px 细线 (公共实现: OrderUtil 的 OU_Line)。 */
static void WR_ThinLine(int x0, int y0, int x1, int y1,
                        int cr, int cg, int cb, unsigned a)
{
    OU_Line(s_WrLocked, s_WrW, s_WrH, s_WrPitch, 0, x0, y0, x1, y1,
            (unsigned)cr, (unsigned)cg, (unsigned)cb, a, 1);
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

/* 世界圆 (圆心同 Z, 地面/桥面投影) → 屏折线。
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
    locked = OU_SurfLock(surf);
    if (!locked) return;
    s_WrLocked = locked; s_WrW = w; s_WrH = h; s_WrPitch = pitch;
    /* 地面蓝圈 (当前武器) + 最小红圈 (0 不画) + 空中青虚线圈
     * + 隐形探测黄圈 + 间谍识别紫圈 (后两者 0 不画)。 */
    WR_Circle(r.center.X, r.center.Y, r.center.Z, r.range, 64, 128, 255, 230, 0);
    if (r.minRange >= 256)
        WR_Circle(r.center.X, r.center.Y, r.center.Z, r.minRange, 255, 64, 64, 230, 0);
    if (r.aaRange >= 256)
    {
        int az = r.center.Z;
        if (g_WrAAZ > 0) az += g_WrAAZ;
        WR_Circle(r.center.X, r.center.Y, az, r.aaRange, 64, 255, 255, 230, 1);
    }
    if (r.depRange >= 256)
        WR_Circle(r.center.X, r.center.Y, r.center.Z, r.depRange, 0, 255, 0, 230, 0);
    if (r.sensRange >= 256)
        WR_Circle(r.center.X, r.center.Y, r.center.Z, r.sensRange, 255, 215, 0, 230, 0);
    if (r.disRange >= 256)
        WR_Circle(r.center.X, r.center.Y, r.center.Z, r.disRange, 180, 100, 255, 230, 0);
    s_WrLocked = 0;
    OU_SurfUnlock(surf);
}
