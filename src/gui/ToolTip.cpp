/* ToolTip.cpp - 建造列表耗时/耗电显示 (Phobos ExtendedToolTips 的简化移植)。
 *
 * 原版建造提示只有"名字 + 价格" (StripClass::HelpText 0x6A92E0 拼进 0xB07BC4,
 * 0x42 个 wchar), 不显示耗时耗电。本模块在退出点 0x6A93D9 (mov eax,TooltipBuffer,
 * 5B 纯传送) 换成自有缓冲并追加一行, 然后跳 0x6A93DE (pop esi + ret):
 *   名字(价格 $ 换成 ICON_COST) + "\n" + ▲N/▼N + " " + ○MM:SS   (建筑)
 *   名字 + "\n" + ○MM:SS                                        (单位/步兵/飞机)
 * 改代码必须知道的约束:
 *   - 图标是内嵌 PCX 的 PUA 码 (Icon.h), 字体没这些字形, 由 VecText 实画,
 *     所以 BitFont 字宽查表钩 (0x4346C0) 必须对它们回 0, 否则包行错乱。
 *   - 自有缓冲只有 160 字: 引擎不再替我们包行, 超 160px 估宽就先按字边界断行;
 *     盒宽由 GetDim 精确度量, 位置由钳制钩兜底。
 *   - 电量只取 BuildingType PowerBonus-PowerDrain (Extra 不计), 非建筑不显示;
 *     时间走 TechnoClass::TimeToBuild (0x6F47A0) 的假对象求值 (Phobos 同式)。
 *   - 只读查询 + 自有静态缓冲, 不写模拟内存 (联机安全); 开关关/类型非法/
 *     求值失败一律返回 0 跑原指令 (= 原版行为)。
 * 配置 [Features] ToolTips (缺失=默认; 也可在 ESC 设置页改, 现读现用):
 *   1 = 追加第三行; 0 = 原版两行, 且本模块完全惰性 (三个钩子都原样跑,
 *   不追加文本、不平移盒子、不记侧栏避让), 视同 MOE 没有这个功能。
 * Phobos 在场时布局交给它 (本模块不再平移), 只把它画的 Composite 副本对回
 * 侧栏那份的位置。细则见 notes/建造提示.md。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "Icon.h"
#include "MOExperience.h"
#include "ScaleBar.h"
#include "SideScroll.h"
#include "ToolTip.h"

#define TT_SECTION "Features"

/* 自有提示串字数 (引擎那份只有 0x42, 换指针后归我们用)。 */
#define TT_BUF_N 160

#define ADDR_TOOLTIP_BUF 0xB07BC4u

#define TO_TT_EXIT 0x006A93DEu

/* TechnoType WhatAmI (YRpp GeneralDefinitions.h): 类型与实例编号不同,
 * 此处 esi 是类型指针, 故用类型编号。 */
#define WAMI_BTYPE 7u
#define WAMI_ATYPE 3u
#define WAMI_ITYPE 16u
#define WAMI_UTYPE 40u

/* 假对象虚表 (YRpp AbsVTable) + Type/Owner 槽 (反汇编实证, 见文件头)。 */
#define VT_BUILDING 0x7E3EBCu
#define VT_INFANTRY 0x7EB058u
#define VT_UNIT 0x7F5C70u
#define VT_AIRCRAFT 0x7E22A4u
#define OFF_T_BUILDING 0x520u
#define OFF_T_INFANTRY 0x6C0u
#define OFF_T_UNIT 0x6C4u
#define OFF_T_AIRCRAFT 0x6C4u

/* BuildingType 电量字段 (0x44E7B0/0x44E880 反汇编实证, 只用前两个, Extra 不计)。 */
#define OFF_PWR_BONUS 0xEE0u
#define OFF_PWR_DRAIN 0xEE4u

#define FN_TIMETOBUILD ((Fn_TimeToBuild)(void*)0x6F47A0u)

typedef int __attribute__((thiscall)) (*Fn_TimeToBuild)(void* self);
typedef int __attribute__((thiscall)) (*Fn_WhatAmI)(void* self);

static int g_TTInit = 0;
static int g_TTEnable = 1;

static unsigned TT_Reg(void* regs, unsigned off)
{
    return *(unsigned*)((char*)regs + off);
}

static int TT_WhatAmI(void* p)
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

static int TT_WLen(const wchar_t* s)
{
    int n = 0;
    if (!s || !IsPlausiblePtr(s)) return 0;
    while (n < 512 && s[n]) n++;
    return (s[n] == 0) ? n : 0;
}

static void ToolTip_EnsureInit(void)
{
    if (g_TTInit) return;
    g_TTInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_TTEnable = GetPrivateProfileIntA(TT_SECTION, "ToolTips", 1, g_IniPath) ? 1 : 0;
    LogLine(g_TTEnable ? "tooltip: power+time ON" : "tooltip: power+time OFF");
}

/* 设置页运行期读写: 现读现用, 钩子里每帧判 g_TTEnable。 */
int ToolTip_Enabled(void)
{
    ToolTip_EnsureInit();
    return g_TTEnable;
}

void ToolTip_SetEnabled(int on)
{
    ToolTip_EnsureInit();
    g_TTEnable = on ? 1 : 0;
    LogLine(g_TTEnable ? "tooltip: power+time ON" : "tooltip: power+time OFF");
}

/* 假对象求 TimeToBuild (Phobos GetBuildTime 同式): 静态零缓冲 + 虚表 +
 * Type + Owner, 调 0x6F47A0。失败回 -1。 */
static int TT_BuildFrames(void* pType, int wami, void* house)
{
    static unsigned char trick[0x6D0];
    static int zeroed = 0;
    unsigned vt = 0;
    unsigned typeOff = 0;
    int i;
    int frames;
    if (!pType || !house) return -1;
    switch ((unsigned)wami)
    {
    case WAMI_BTYPE: vt = VT_BUILDING; typeOff = OFF_T_BUILDING; break;
    case WAMI_ITYPE: vt = VT_INFANTRY; typeOff = OFF_T_INFANTRY; break;
    case WAMI_UTYPE: vt = VT_UNIT; typeOff = OFF_T_UNIT; break;
    case WAMI_ATYPE: vt = VT_AIRCRAFT; typeOff = OFF_T_AIRCRAFT; break;
    default: return -1;
    }
    if (!zeroed)
    {
        for (i = 0; i < (int)sizeof(trick); i++) trick[i] = 0;
        zeroed = 1;
    }
    *(unsigned*)trick = vt;
    *(void**)(trick + OFF_OWNER) = house;
    *(void**)(trick + typeOff) = pType;
    frames = FN_TIMETOBUILD(trick);
    if (frames < 0 || frames > 36000) return -1;
    if (frames < 54) frames = 54;
    return frames;
}

static int TT_PowerOf(void* pType, int wami)
{
    int bonus, drain;
    if ((unsigned)wami != WAMI_BTYPE) return 0;
    if (!pType || !IsPlausiblePtr(pType)) return 0;
    if (!IsPlausiblePtr((char*)pType + OFF_PWR_DRAIN + 4)) return 0;
    /* 只看 Power (PowerBonus-PowerDrain, Extra 不计, 用户要求) */
    bonus = *(int*)((char*)pType + OFF_PWR_BONUS);
    drain = *(int*)((char*)pType + OFF_PWR_DRAIN);
    if (bonus < -100000 || bonus > 100000) return 0;
    if (drain < -100000 || drain > 100000) return 0;
    return bonus - drain;
}

/* 无 CRT 追加: 十进制无符号 (宽字符)。 */
static int TT_AppendU(wchar_t* dst, int pos, int cap, unsigned v)
{
    char tmp[16];
    int n = 0, i;
    if (v == 0) tmp[n++] = '0';
    else
    {
        while (v && n < 15) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    }
    for (i = n - 1; i >= 0; i--)
    {
        if (pos + 1 >= cap) break;
        dst[pos++] = (wchar_t)tmp[i];
    }
    return pos;
}

/* MM/SS 两位补零 (超过 99 就照实际位数写)。 */
static int TT_Append2(wchar_t* dst, int pos, int v)
{
    if (v < 10 && pos + 1 < TT_BUF_N) dst[pos++] = L'0';
    return TT_AppendU(dst, pos, TT_BUF_N, (unsigned)v);
}

/* 0x6A93D9 (5): 建造提示退出点 (mov eax,TooltipBuffer, 纯传送)。
 * ESI=TechnoType* (退出前仍有效, pop 在 0x6A93DE 才恢复)。
 * 成功则自有缓冲拼好、EAX 指向它后跳 0x6A93DE; 否则返回 0 跑原指令。 */
extern "C" __declspec(dllexport) DWORD __cdecl ToolTip_Extend(void* regs)
{
    static wchar_t buf[TT_BUF_N];
    void* pType;
    void* house;
    int wami;
    int frames, secs;
    int power;
    int pos, origLen, i;
    const wchar_t* orig;

    ToolTip_EnsureInit();
    if (!g_TTEnable) return 0;

    pType = (void*)TT_Reg(regs, REG_OFF_ESI);
    if (!pType || !IsPlausiblePtr(pType)) return 0;
    wami = TT_WhatAmI(pType);
    if (wami != (int)WAMI_BTYPE && wami != (int)WAMI_ITYPE &&
        wami != (int)WAMI_UTYPE && wami != (int)WAMI_ATYPE)
        return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house || !IsPlausiblePtr(house)) return 0;

    frames = TT_BuildFrames(pType, wami, house);
    if (frames < 0) return 0;
    power = TT_PowerOf(pType, wami);

    orig = (const wchar_t*)ADDR_TOOLTIP_BUF;
    if (!IsPlausiblePtr(orig)) return 0;
    origLen = TT_WLen(orig);
    if (origLen <= 0 || origLen > 120) return 0;
    /* 总长上界: 第三行最多 \n + 电量 (图标+数字+空格) + 时间图标 + MM:SS。
     * 盒宽由 GetDim 精确度量, 所以这里只粗卡, 不按字数放弃 (否则长名整体不显示)。 */
    if (origLen + 1 + (power ? 8 : 0) + 6 + 1 > 150) return 0;

    pos = 0;
    /* 逐字拷贝 + 长行预断行: 名字过长引擎会按 maxW 包行, 包点不可控 (包到盒外
     * 即错位); 这里超 160px 估宽就在字边界先断, 度量/绘制全走短行。
     * 估宽保守取大 (CJK 18/数字 11/ASCII 9/图标按步进), 只会早断不会断错。 */
    {
        int lineW = 0;
        for (i = 0; i < origLen && pos + 1 < TT_BUF_N; i++)
        {
            wchar_t c = orig[i];
            int cw;
            if (c == L'\n') { buf[pos++] = c; lineW = 0; continue; }
            /* 价格 $ 换费用图标 (等宽归一, 与第三行图标列对齐; 只换 $+数字,
             * 名字里带 $ 的极罕见, 中了也只是多个图标, 不影响布局)。 */
            if (c == L'$' && i + 1 < origLen &&
                orig[i + 1] >= L'0' && orig[i + 1] <= L'9')
            {
                if (lineW > 0 && lineW + 12 > 160 &&
                    pos + 1 < TT_BUF_N)
                { buf[pos++] = L'\n'; lineW = 0; }
                buf[pos++] = (wchar_t)ICON_COST;
                lineW += Icon_Advance(ICON_COST);
                continue;
            }
            if (c >= L'0' && c <= L'9') cw = 11;
            else if (c < 0x100) cw = 9;
            else cw = 18;
            if (lineW > 0 && lineW + cw > 160 && i + 1 < origLen &&
                pos + 1 < TT_BUF_N)
            { buf[pos++] = L'\n'; lineW = 0; }
            buf[pos++] = c;
            lineW += cw;
        }
    }
    /* 另起一行: [▲N/▼N ]○MM:SS (电量零不显示, 方向由图标表达, N 取绝对值) */
    if (pos + 1 < TT_BUF_N) buf[pos++] = L'\n';
    if (power != 0)
    {
        unsigned mag = (unsigned)(power < 0 ? -power : power);
        if (pos + 1 < TT_BUF_N)
            buf[pos++] = (wchar_t)(power > 0 ? ICON_PWR_UP : ICON_PWR_DOWN);
        pos = TT_AppendU(buf, pos, TT_BUF_N, mag);
        if (pos + 1 < TT_BUF_N) buf[pos++] = L' ';
    }
    if (pos + 1 < TT_BUF_N) buf[pos++] = (wchar_t)ICON_TIME;
    secs = frames / 15;
    pos = TT_Append2(buf, pos, secs / 60);
    if (pos + 1 < TT_BUF_N) buf[pos++] = L':';
    pos = TT_Append2(buf, pos, secs % 60);
    buf[pos] = 0;

    *(DWORD*)((char*)regs + REG_OFF_EAX) = (DWORD)(size_t)buf;
    return TO_TT_EXIT;
}

/* ---------------- 提示盒钳制 (宽名长提示出界修复) ----------------
 * CCToolTip::Draw2 的 FillRect 点 0x478FDC
 * (mov eax,[esi] + 间接 call [eax+0x14], 无 rel, 5B, 纯调用):
 * 盒子画出屏幕即把字连盒整体平移回来 (只平移, 不改布局)。
 * 只动 Composite 面 (侧栏面坐标系不同, 不碰); 范围内即无操作。
 * 帧布局经 Phobos PhobosToolTip.cpp FillRect 钩实证:
 *   ESI=面对象, pRect=ESP+0x34 (XYWH), pText=ESP+0x24 (LTRB)。
 * 钩子经 dist/MOExperience.dll.inj: 0x478FDC = ToolTip_Clamp, 5。
 * 偷的字节无 rel, 永远返回 0 跑原指令 (铁律安全)。 */

/* Phobos 兼容: 同一提示它画两份 (侧栏面 + Composite 的 slave 副本)。
 * 侧栏那份按侧栏宽收边 (右沿贴屏右); slave 副本被它自己的 0x478F52 钩推到
 * "屏宽 + 侧栏宽 - 盒宽", 比侧栏那份右移一个侧栏宽。盒宽 <= 侧栏宽时副本
 * 整块在屏外看不出; 超过时就在屏右沿露一条, 像第二个框。对回
 * "屏宽 - 盒宽" (= 侧栏那份的屏幕位置) 两份即重合。与 ToolTips 开关无关
 * (副本是 Phobos 画的), 故在开关判断之前执行。 */
static void TT_AlignSlaveCopy(void* surf, unsigned esp)
{
    void* comp = *(void**)ADDR_SURF_COMPOSITE;
    int* pr;
    int* pt;
    int cw, d;
    if (!comp || !IsPlausiblePtr(comp) || surf != comp) return;
    cw = GtSurfGet(comp, GtVT_GET_WIDTH);
    pr = (int*)(esp + 0x34u);   /* X,Y,W,H */
    if (cw <= 0 || !IsPlausiblePtr(pr) || pr[2] <= 0 || pr[2] > 2048) return;
    if (pr[0] + pr[2] <= cw) return;
    pt = (int*)(esp + 0x24u);   /* L,T,R,B */
    d = (cw - pr[2]) - pr[0];
    pr[0] += d;
    if (IsPlausiblePtr(pt))
    {
        pt[0] += d;
        pt[2] += d;
    }
}

extern "C" __declspec(dllexport) DWORD __cdecl ToolTip_Clamp(void* regs)
{
    unsigned esp;
    void* surf;
    void* comp;
    int* pr;
    int* pt;
    int minX, maxX, dx, w;
    esp = TT_Reg(regs, REG_OFF_ESP);
    surf = (void*)TT_Reg(regs, REG_OFF_ESI);
    if (!esp || !surf) return 0;
    /* 副本对齐与 ToolTips 开关无关: 副本是 Phobos 画的, 不关本模块的事。 */
    TT_AlignSlaveCopy(surf, esp);
    /* 关闭时本模块完全惰性: 不记侧栏避让, 也不平移。 */
    if (!ToolTip_Enabled()) return 0;
    /* 侧栏滚动条避让: 每次提示绘制都记下面+盒 (面是否侧栏由 SS_Draw 判定)。
     * cameo 提示画在侧栏面里且早于 Blit, 滚动条画在 Blit 钩里, 不记就会盖住字。
     * 记在钳制之前 —— 侧栏面的提示走不到下面的 composite 分支。 */
    {
        int* qr = (int*)(esp + 0x34u);   /* X,Y,W,H, 与 pr 同地址 */
        if (IsPlausiblePtr(qr) && qr[2] > 0 && qr[2] <= 2048 &&
            qr[3] > 0 && qr[3] <= 2048)
            SS_NoteTip(surf, qr[0], qr[1], qr[2], qr[3]);
    }
    /* Phobos 在场时布局交给它 (副本已在上面被对回), 本模块不再平移。 */
    if (MOE_PhobosLoaded()) return 0;
    comp = *(void**)ADDR_SURF_COMPOSITE;
    if (!comp || !IsPlausiblePtr(comp) || surf != comp) return 0;
    pr = (int*)(esp + 0x34u);   /* X,Y,W,H */
    pt = (int*)(esp + 0x24u);   /* L,T,R,B */
    if (!IsPlausiblePtr(pr) || !IsPlausiblePtr(pt)) return 0;
    w = pr[2];
    if (w <= 0 || w > 2048) return 0;
    if (!IsPlausiblePtr((void*)ADDR_VIEWBOUNDS)) return 0;
    /* 边界取满视图区 A, 不能取视口 R: 放大档下 ViewBounds 已缩成 R, 拿它当
     * 右界会把盒子连字整体往左拽最多一整个视口宽, 且位移随档位缩放。
     * CCToolTip 的选面/量宽右界已被改指到同一块影子 (VecText.cpp 的 rep[],
     * notes/地图缩放.md §5.2), 这里必须同源。1x 时 A == R。*/
    if (SB_Zoomed())
    {
        const unsigned* av = SB_AViewBlock();
        minX = (int)av[0];
        maxX = minX + (int)av[2];
    }
    else
    {
        minX = *(int*)ADDR_VIEWBOUNDS;
        maxX = minX + *(int*)(ADDR_VIEWBOUNDS + 8u);
    }
    if (maxX <= minX + 16 || maxX - minX < w + 16) return 0;
    dx = 0;
    if (pr[0] + w > maxX) dx = maxX - (pr[0] + w);
    if (pr[0] + dx < minX) dx = minX - pr[0];
    if (!dx) return 0;
    pr[0] += dx;
    pt[0] += dx;
    pt[2] += dx;
    return 0;
}

/* ---------------- 字形 advance 修正 (绘制期换行排版) ----------------
 * BitFont 字宽查表 0x4346C0 (mov ecx,[ecx+4] + xor eax,eax, 无 rel, 5B):
 * DrawText 内部换行/排版全用它累宽度, PUA 码查表越界读到垃圾宽度,
 * 第三行会被包错行或画出盒外 ("部分显示"/"降在外面"即此因)。
 * 注意: 它返回的是字形数据指针 (调用方 mov cl,[eax] 解引用), 不是像素!
 * PUA 直接回 0 (=缺字, 引擎走 '?' 替代分支, 确定性安全, 永不碰越界内存),
 * 其余跑原指令。
 * 钩子: 0x4346C0 = ToolTip_AdvFix, 5。函数尾 ret 4 (0x4346EE),
 * 栈形与入口一致, 跳过去即带值返回。 */
#define ADDR_ADVFIX_RET 0x004346EEu

extern "C" __declspec(dllexport) DWORD __cdecl ToolTip_AdvFix(void* regs)
{
    unsigned esp;
    unsigned wc;
    esp = TT_Reg(regs, REG_OFF_ESP);
    if (!esp || !IsPlausiblePtr((const void*)(esp + 4u))) return 0;
    /* 关闭时本钩不参与 (视同没有本模块)。 */
    if (!ToolTip_Enabled()) return 0;
    wc = *(unsigned short*)(esp + 4u);
    if (!Icon_Is(wc)) return 0;
    *(DWORD*)((char*)regs + REG_OFF_EAX) = 0;
    return ADDR_ADVFIX_RET;
}
