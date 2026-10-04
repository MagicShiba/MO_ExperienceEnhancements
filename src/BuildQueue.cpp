/* BuildQueue.cpp - 建筑排队 (本地队列 + 复刻点击, 不碰模拟)。
 *
 * 设计: 队列状态只存 DLL 本地, 下的全是"点击会下的标准 PRODUCE 事件",
 * 各端模拟天然一致 (混装原版端也只看到普通订单), 构造上不影响联机。
 *   - 左键跨型号点击: 忙时走拒绝路径 0x6AB689, 吞掉记入本地; 闲时原生下单
 *     第 1 个并顺手记 4 个。忙闲由原生判断, 切换清产的原生 abandon 路
 *     (0x6AA7BC/0x6AA884) 全版本零触发 (跨型号点击永远走忙拒绝+本地记入,
 *     在产从未被切掉; 日志 h1/sw 全程缺席为证, 0.2.113d 起撤钩, 如复现
 *     切换清产由 pw 照出再议)。
 *   - 点建筑图标时工厂忙 (原版走拒绝音路径 0x6AB689): 吞掉点击记入本地
 *     队列 (house/type/naval/cat), 跳 0x6AB95A (与原版拒绝同尾, 只跳过
 *     播音; 多路到达对账 esp 一致, 跳过字节不碰 EBX/ESI/EDI/EBP, 尾部先
 *     重载 ECX/EDX, 故等价)。Shift+左键一次记 5 个 (槽不够记到满)。
 *     条目不存工厂指针 (0.2.95d 起): 工厂对象会死/换 (日志实证: 开工后
 *     存指针的 R0 全灭队列), 一律按 (house,naval,cat) 每轮现解析槽位,
 *     不存在 stale。cat 取自点击帧原生栈 [esp+0x24] (与原生 GPF 入参同
 *     值, 非 +0xE08 探针), naval 取 [type+0xCCE] (与原生同偏移)。
 *   - 图标数字 (DrawStrip 的 CountTotal 取数点 0x6A9C86, 5 字节恰两条指令
 *     mov ebx,eax + cmp ebx,1): 本地有排队则 EAX/EBX=原版数+本地数后跳
 *     0x6A9CC9 借用原版绘制 (esp 一致, 绘制块只用 EBX/EBP/ESI/ESP);
 *     本地无排队跑原指令 (单位图标零影响)。
 *   - BuildQueue_Poll (每循环, 挂 PowerDrawBoth): 无对账无丢单, 只喂单:
 *     逐条目按 (house,naval,cat) 现解析槽位工厂 (GPF, 槽位即权威, 无 stale);
 *     工厂不存在/非本家→留着等 (卖了重建/具现后自动续上, 永不丢单);
 *     工厂 Object==null (无在产/待放置) 才复刻一次点击下单; 同一工厂每轮
 *     只喂一项 (事件下帧才执行, 当轮再喂会被原版拒绝, 故用 fed 限流,
 *     fed 按本轮解析出的工厂指针区分)。每帧另采样 Shift 进 32 帧闩锁
 *     (左键记数用"当前或近期按下", 防单帧竞态/提前松开)。
  *   - 右键规则 (用户定稿; Shift=清全部):
  *     点一次暂停, 再点取消; 取消时优先减数 (N>1 弹尾, 在产/暂停不动),
  *     最后剩在产单个走原生真取消。Shift+右键=取消全部: 清空本地后跑
  *     原生 0x2E (在产亦清, 显示归零); 未暂停的按住 Shift 点只先暂停
  *     (随后暂停态再 Shift 点清零), 天然两步防误删。c 记原生 shift
  *     决策位 ([esp+0x13]), 与物理键不一致另记 `bq: sh`。
  *     真分发 (0.2.109d 反汇编定案): [esp+0xB0]&0x10 为右键;
  *     [esp+0x18]==null→0x6AB0AC 明路 (H0 state 检查, 空闲态才到);
  *     在产未暂停→0x6AAFE1 暗暂停 (hd 哨兵, 暂停保持原生);
  *     已暂停/空闲→按 shift 进 0x6AAE4A (0x2E, Shift 清全) 或 0x6AAFBD
  *     (0x10, 经 jmp 0x6AB1FF 共享尾构造)。剥离在明取消分支
  *     (0x6AB197, d=1/5/0/8) 与暗分发点 (0x6AAE44, d=6/7/8) 两处;
  *     明暂停检查 state==1 一律原生暂停。
  *     全程不仿造事件构造 (E8 点一律不碰)。
  *     (0.2.91d 的 R1 镜像清已删: 放置完成与取消在 Object==null 上不可分;
  *     0.2.95d 的 R0 工厂指针对账已删: 改 per-round 现解析, 无 stale;
  *     0.2.113d 撤 H1/H2/H0P/ex/ec: 全版本零触发, 死钩。)
 *   - 建筑页/防御页天然分开: 条目按 house+heap 匹配 (heap=ArrayIndex 类型
 *     唯一, 同型必同类), 喂单按 (naval,cat) 解析各页工厂; 取消只动本家
 *     本型, 不碰别型 (0.2.95d: R0 丢单 path 已删, 跨型号消失只可能是显示
 *     或原生模拟问题, 本地队列永不跨清)。
 *   - Shift 检测走原版键位函数 0x53EC70 (ecx=0x10, AH&0x80 为按下;
 *     与 0x54F25B 处原版 Shift 检测逐字节同用法; GetKeyState 后备)。
 * 待放置时 Object 非空故下一个先等着, 放下后自动开工 (=Phobos 文档行为)。
 * 存档/读档/换 CurrentPlayer 即清空 (未下单=未扣费, 经济一致)。
 * 开关关时钩子全跑原指令、轮询直接返回 (=原版行为)。
 *
  * 诊断 trace: bq: qd/qf (左键记入, a=heap b=want c=raw d=used),
  * bq: qn (工厂未具现仍记入, a=heap b=4 c=raw d=used),
  * bq: h0 (右键到暂停检查, a=state b=heap),
  * bq: hd (到暗暂停, a=heap b=本地数 c=0 d=0; 纯哨兵, 暂停保持原生),
  * bq: cp (右键, a=heap b=剩本地 c=原生shift位 d=1弹一/0走原生/
  * 5本地空但模拟>1拦原生/6暗取消弹一/7暗取消拦原生/8Shift清全走原生/
  * 10明暂停Shift转取消分支),
  * bq: sh (原生shift位与物理Shift不一致, a=heap b=原生位 c=物理键 d=0),
  * bq: cs (取消时模拟侧, a=heap b=模拟数 c=在产中 d=总数),
  * bq: pw (生产监视, a=Object低16 b=暂停位 c=线号 d=1换项/空/2暂停边沿),
  * bq: ev (OutList 新增事件, a=类型(0xE下单/0xF暂停/0x10单个取消/0x2E全部
  * 取消) b=house c=HeapID d=当时计数; 与路径无关, 发送瞬间必留痕),
  * bqueue: queued/fire/unqueued (记入/开工/移除, heap+depth),
  * bqueue: base=模块基址。h0/cp-d0 常驻 (看路由, 点击级无刷屏)。
 *
 * 铁律 (Syringe HandleException 实证: 跳板原样拷贝被偷字节, rel/call
 * 不修正; Phobos 全库也从不在 rel 字节上 return 0):
 *   .inj 钩子凡偷了含 rel 的字节 (E8 call / Jcc), 永远不许 return 0;
 *   条件跳转永不跳回钩子自身地址 (那是补丁, 会自递归)。
  * 本文件钩子皆纯传送字节 (0x6AB689 push/or, 0x6AB6CE mov/mov,
  * 0x6AB0C1 Jcc 永跳, 0x6AB197 push/push/mov, 0x6AAE44 Jcc 永跳,
  * 0x6AAFE1 mov, 0x6A9C86 mov/cmp, 0x6A9C6C mov/mov): return 0 跑原指令安全,
  * 跳转只跳前向原生目标。另铁律补遗 (0.2.109d 崩溃实证):
  * 9/10 字节钩避免非零返回 (疑触 Syringe 短桩问题), 大块改用 Jcc 分发点。
 *
 * 事件复刻 (与点击路径 0x6AB741-0x6AB7C7 逐字节同义):
 *   EventClass ctor 0x4C6970 (thiscall, this=111 字节栈缓冲,
 *     houseIndex/0xE(Produce)/rtti/heap/naval 入栈),
 *   OutList (0xA802C8 计数, 满 0x80 则下帧重试) 111 字节拷入
 *   0xA802D4+next*111, 同款无参回调 call [0x7E1530] (全二进制数十处同调,
 *   必忽略寄存器), 并行数组 0xA83A54[next]=调用后 EAX (全二进制只写不读),
 *   计数器按原版掩码推进。
 * 参数来源 (MSVC+Yrpp 头 offsetof 探针求值, 原版反汇编交叉验证):
 *   rtti=7 (AbstractType::BuildingType, vtable+0x2C 验证);
 *   heap=[type+0xDF8] (BuildingType.ArrayIndex);
 *   naval=[type+0xCCE] (TechnoType.Naval, 原版两次喂给 GetPrimaryFactory);
 *   houseIndex=[house+0x30] (House.ArrayIndex, 原版事件构造同取);
 *   Factory.Object=+0x58, Factory.Owner=+0x6C (SetObject 写入证实);
 *   工厂槽位: GetPrimaryFactory (0x500510, house/abs/naval/cat→槽位工厂,
 *   槽位即权威); 条目不存工厂指针, 每轮现解析, 故不需要 FactoryClass::Array。
 *   CurrentPlayer=*(0xA83D4C)。
 * 点击钩子寄存器 (0x6AB689/0x6AB6CE): EBP=abs (原版 cmp ebp,7 紧邻在前),
 * EDI=图标类型 (0x6AB602 取自 [esp+0x28], 原版 0x6AB6CE 处 [edi+0xCCE]),
 * ESI=工厂 (0x6AB602 处 GetPrimaryFactory 结果, 0x6AB61E mov esi,eax)。
 * 原版右键流 (反汇编实证, 0x6AB0AC-0x6AB0A7, 此处不钩仅记录):
 * state==1 则置 2 发 SUSPEND 后跳 0x6AB28f 退出 (暂停),
 * state!=1 才进 0x6AB18d 取消分支 (IsQueued→0x2E/0x10)。
 *
 * 钩子经 dist/MOExperience.dll.inj 由 Syringe 挂载 (返回 0=跑原指令, 非0=跳往)。
 */

#include <windows.h>
#include "GdiText.h"

#define BQ_SECTION "BuildQueue"

/* Syringe REGISTERS 布局 (YRpp Syringe.h)。 */
#define R_EDI 0x08
#define R_ESI 0x0C
#define R_EBP 0x10
#define R_ESP 0x14
#define R_EBX 0x18
#define R_ECX 0x20
#define R_EAX 0x24

#define ABS_BUILDINGTYPE 7u
#define EVT_PRODUCE 0xEu

#define ADDR_CURPLAYER 0xA83D4Cu
#define OFF_HOUSE_INDEX 0x30u
#define OFF_TT_NAVAL 0xCCEu
#define OFF_BT_ARRAYINDEX 0xDF8u
#define OFF_FAC_OBJECT 0x58u
#define OFF_FAC_OWNER 0x6Cu

#define ADDR_OUT_COUNT 0xA802C8u
#define ADDR_OUT_NEXT 0xA802D0u
#define ADDR_OUT_BUF 0xA802D4u
#define ADDR_OUT_PAR 0xA83A54u
#define EVT_SIZE 111u
#define EVT_CAP 0x80u

#define ADDR_FACARR_ITEMS 0xA83E34u
#define ADDR_FACARR_COUNT 0xA83E40u
/* FactoryClass::Array 布局经构造自注册反汇编实证 (inc-store Count,
 * indexed-store Items, CountTotal 同地址遍历): Items=[0xA83E34],
 * Count=[0xA83E40]。只用于显示求和 (不做存活判断, 无 stale 问题)。 */

#define TO_CLICK_EXIT 0x006AB95Au
#define TO_CANCEL_EXIT 0x006AB28Fu
#define TO_DARK_CANCEL_NATIVE 0x006AAFBDu
#define TO_DARK_ALL_NATIVE 0x006AAE4Au
#define TO_PAUSE_NATIVE 0x006AB0C7u
#define TO_CANCEL_BRANCH 0x006AB18Du
#define TO_DRAWNUM 0x006A9CC9u
#define TO_DRAWSKIP 0x006A9D88u

#define BQ_CAP 16

/* 待发事件窗守卫: Poll 挂渲染 (可快于逻辑帧), 同一工厂连续两轮都可能
 * 见到 Object==null (上轮发的 PRODUCE 还没执行), 连发两单则第二单在
 * 执行侧被 BeginProduction 拒绝 (0x4FA520 起首即 busy+building 返 3),
 * 静默丢单。pend 记住本轮发过单的线, N 轮内不再发 (执行后 Object 非空,
 * 本来就会停; 哑单则 N 轮后重试)。键为 (house,naval,cat), 与喂单同键。 */
#define BQ_PEND_N 45
#define BQ_PEND_SLOTS 8
typedef struct
{
    void* house;
    unsigned naval;
    unsigned buildcat;
    int age;
} BQPend;

static BQPend g_BQPend[BQ_PEND_SLOTS];

static void BQ_PendAge(void)
{
    int s;
    for (s = 0; s < BQ_PEND_SLOTS; s++)
    {
        if (g_BQPend[s].house && g_BQPend[s].age < BQ_PEND_N)
            g_BQPend[s].age++;
    }
}

static void BQ_PendClear(void)
{
    int s;
    for (s = 0; s < BQ_PEND_SLOTS; s++)
        g_BQPend[s].house = 0;
}

static int BQ_Pending(void* house, unsigned naval, unsigned cat)
{
    int s;
    for (s = 0; s < BQ_PEND_SLOTS; s++)
    {
        if (g_BQPend[s].house == house && g_BQPend[s].naval == naval &&
            g_BQPend[s].buildcat == cat && g_BQPend[s].age < BQ_PEND_N)
            return 1;
    }
    return 0;
}

static void BQ_MarkPending(void* house, unsigned naval, unsigned cat)
{
    int s;
    int free = -1;
    for (s = 0; s < BQ_PEND_SLOTS; s++)
    {
        if (g_BQPend[s].house == house && g_BQPend[s].naval == naval &&
            g_BQPend[s].buildcat == cat)
        {
            g_BQPend[s].age = 0;
            return;
        }
        if (!g_BQPend[s].house || g_BQPend[s].age >= BQ_PEND_N)
            free = s;
    }
    if (free < 0) return; /* 槽满: 不记 (fail-open, 等价于无守卫) */
    g_BQPend[free].house = house;
    g_BQPend[free].naval = naval;
    g_BQPend[free].buildcat = cat;
    g_BQPend[free].age = 0;
}

static void BQ_Trace(const char* head, int a, int b, int c, int d);

/* OutList 事件哨兵 (0.2.107d, 纯读零行为): 每轮看 OutList 写指针, 新增
 * 条目逐条记 `bq: ev` (a=类型 b=house c=HeapID d=当时计数)。
 * 布局经 YRpp EventClass.h 实证 (pack1, sizeof=111, DataBuffer@7):
 * Type@0(1B: 0xE下单/0xF暂停/0x10单个取消/0x2E全部取消), HouseIndex@2,
 * HeapID@11 (produce/suspend/abandon 通用, 余者记原始值)。
 * 用途: 与发送路径无关——无论谁 (原生分支/Ares/未知) 构造暂停/取消事件,
 * 进 OutList 的瞬间必留痕, 自带 house+heap 可归因; 我方 fire(0xE) 自带
 * `bqueue: fire` 行, 此处照记 (自校验 Type@0 解读是否正确)。
 * 环形 128 槽: 单轮推进>8 视为读档/菜单, 静默重同步 (防刷屏)。 */
static unsigned g_BQEvNext = 0;
static int g_BQEvInit = 0;

/* 规划事件抓包 (联机同步研究用, 量极小): Type + 前 48 字节 hex。
 * 用途: 原生巡逻建圈/关环/提交的真实事件序列
 * (Mega=0x04/Connect=0x2A/Commit=0x2B), 与我方构造逐字节对比,
 * 目标是去掉本地直调、纯事件关环 (联机安全)。
 * 测试法: 单机安静局, 不碰中键, 选 1 单位, Z+左键点 4 格,
 * 左键点首格关环, 松 Z 提交; 再用 Z+中键画一次矩环作对比。 */
static void BQ_HexB(char* out, unsigned v)
{
    static const char* dig = "0123456789ABCDEF";
    out[0] = dig[(v >> 4) & 15u];
    out[1] = dig[v & 15u];
    out[2] = 0;
}

static void BQ_EvHex(const unsigned char* e)
{
    char buf[128], hb[4];
    unsigned q = 0, i;
    q = AppendCStr(buf, q, "pev: t=", sizeof(buf));
    BQ_HexB(hb, (unsigned)e[0]);
    q = AppendCStr(buf, q, hb, sizeof(buf));
    for (i = 1; i < 48; i++)
    {
        BQ_HexB(hb, (unsigned)e[i]);
        q = AppendCStr(buf, q, (i & 3) == 1 ? " " : "", sizeof(buf));
        q = AppendCStr(buf, q, hb, sizeof(buf));
        if (q + 4 >= sizeof(buf)) break;
    }
    (void)q;
    LogLine(buf);
}

static void BQ_EvWatch(void)
{
    unsigned next = *(unsigned*)ADDR_OUT_NEXT;
    unsigned cur;
    unsigned adv;
    int guard;
    if (!g_BQEvInit)
    {
        g_BQEvNext = next;
        g_BQEvInit = 1;
        return;
    }
    adv = (next - g_BQEvNext) & 0x7Fu;
    if (!adv) return;
    if (adv > 8u) /* 大跳变: 读档/菜单/爆流, 静默重同步 */
    {
        g_BQEvNext = next;
        return;
    }
    cur = g_BQEvNext;
    guard = 0;
    while (cur != next && guard < 8)
    {
        const unsigned char* e =
            (const unsigned char*)(ADDR_OUT_BUF + cur * EVT_SIZE);
        int heap = *(int*)(e + 11);
        BQ_Trace("bq: ev", (int)e[0], (int)e[2], heap,
                 (int)*(unsigned*)ADDR_OUT_COUNT);
        if (e[0] == 0x04 || e[0] == 0x2A || e[0] == 0x2B)
            BQ_EvHex(e); /* 规划事件额外 hex (原生序列研究) */
        cur = (cur + 1u) & 0x7Fu;
        guard++;
    }
    g_BQEvNext = next;
}

/* 生产监视器 (0.2.106d, 纯诊断零行为): 每轮记各 (house,naval,cat) 线的
 * 工厂 Object 指针与 +0x70 暂停位, 变化才记一行 `bq: pw`。
 * 用途: 在产到底是被谁杀的 (Object 删除/置空), 还是自然完工换项
 * (Object 换新), 与点击 trace 做时间关联: cp 后 1~2 帧内 Object 变空/
 * 换新=点击杀的; 无 cp 关联的变化=自然推进或别路。+0x38 进度字每帧都变,
 * 故不监视 (防刷屏), 只看 Object 存亡与 +0x70 边沿 (暂停 hint)。 */
#define BQ_WATCH_SLOTS 8
typedef struct
{
    void* house;
    unsigned naval;
    unsigned buildcat;
    void* obj;
    unsigned susp;
    int live;
} BQWatch;

static BQWatch g_BQWatch[BQ_WATCH_SLOTS];

static void BQ_WatchClear(void)
{
    int s;
    for (s = 0; s < BQ_WATCH_SLOTS; s++)
        g_BQWatch[s].live = 0;
}

static void BQ_Watch(void* house, unsigned naval, unsigned cat,
                     void* factory)
{
    int s;
    int free = -1;
    void* obj;
    unsigned susp;
    if (!house || !factory) return;
    obj = *(void**)((char*)factory + OFF_FAC_OBJECT);
    susp = *(unsigned char*)((char*)factory + 0x70u) ? 1u : 0u;
    for (s = 0; s < BQ_WATCH_SLOTS; s++)
    {
        if (g_BQWatch[s].live && g_BQWatch[s].house == house &&
            g_BQWatch[s].naval == naval && g_BQWatch[s].buildcat == cat)
        {
            int kind = 0;
            if (g_BQWatch[s].obj != obj) kind = 1;
            else if (g_BQWatch[s].susp != susp) kind = 2;
            if (kind)
            {
                BQ_Trace("bq: pw", (int)((size_t)obj & 0xFFFFu),
                         (int)susp, (int)(naval * 16u + cat), kind);
                g_BQWatch[s].obj = obj;
                g_BQWatch[s].susp = susp;
            }
            return;
        }
        if (!g_BQWatch[s].live && free < 0) free = s;
    }
    if (free < 0) return;
    g_BQWatch[free].house = house;
    g_BQWatch[free].naval = naval;
    g_BQWatch[free].buildcat = cat;
    g_BQWatch[free].obj = obj;
    g_BQWatch[free].susp = susp;
    g_BQWatch[free].live = 1;
}

/* 条目不存工厂指针: 每轮按 (house,naval,cat) 现解析槽位, 无 stale。
 * buildcat 取自点击帧原生栈 [esp+0x24] (与原生 GPF 入参同值)。 */
typedef struct
{
    void* house;
    void* type;
    unsigned naval;
    unsigned buildcat;
    int flying; /* 已发单待模拟认领 (见喂单确认制) */
} BQEntry;

static int g_BQInit = 0;
static int g_BQEnable = 0;
static BQEntry g_BQList[BQ_CAP];
static int g_BQCount = 0;
static void* g_BQHouse = 0;
/* Shift 闩锁: Poll 每帧采样 1 位, 32 帧窗口。左键用闩锁 (防单帧竞态/
 * 提前松开), 右键清空只认当前按下 (防 0.5s 内的误删)。 */
static unsigned g_BQShiftHist = 0;

static void* BQ_ModBase(void)
{
    static void* base = 0;
    if (!base)
    {
        HMODULE h = 0;
        GetModuleHandleExA(0x00000004u, (LPCSTR)BQ_ModBase, &h);
        base = h ? (void*)h : (void*)0xFFFFFFFFu;
    }
    return base;
}

static void BQ_EnsureInit(void)
{
    if (g_BQInit) return;
    g_BQInit = 1;
    BuildPaths();
    GtEnsureLog();
    g_BQEnable =
        GetPrivateProfileIntA(BQ_SECTION, "Enable", 0, g_IniPath) ? 1 : 0;
    LogLine(g_BQEnable ? "bqueue: local building queue ON"
                       : "bqueue: local building queue OFF");
    {
        char mb[48], hx[16];
        unsigned q = 0;
        q = AppendCStr(mb, q, "bqueue: modbase=", sizeof(mb));
        HtoA((unsigned)(size_t)BQ_ModBase(), hx);
        q = AppendCStr(mb, q, hx, sizeof(mb));
        (void)q;
        LogLine(mb);
    }
}

static unsigned BQ_Reg(void* regs, unsigned off)
{
    return *(unsigned*)((char*)regs + off);
}

typedef int __attribute__((thiscall)) (*Fn_WhatAmI)(void* self);
typedef void* __attribute__((thiscall)) (*Fn_EventCtor)(void* ev, int houseIdx, int evtType, int rtti, int heap, int naval);
typedef void (__attribute__((cdecl)) *Fn_SndCb)(void);
typedef unsigned short __attribute__((fastcall)) (*Fn_KeyState)(unsigned key);
typedef void* __attribute__((thiscall)) (*Fn_GetPrimaryFactory)(void* house, unsigned abs, unsigned naval, unsigned buildcat);
/* 工厂内该型的模拟数 (在产+在排, 原版绘制同调法): thiscall ECX=工厂,
 * 栈参=类型指针。0x4CA670 经原生绘制调用点实证。 */
typedef int __attribute__((thiscall)) (*Fn_CountTotal)(void* factory, void* type);
#define FN_COUNTTOTAL ((Fn_CountTotal)(void*)0x4CA670u)

#define FN_EVENTCTOR ((Fn_EventCtor)(void*)0x4C6970u)
#define FN_GETPRIMARYFACTORY ((Fn_GetPrimaryFactory)(void*)0x500510u)

static int BQ_WhatAmI(void* pType)
{
    void** vt;
    Fn_WhatAmI fn;
    if (!pType) return -1;
    vt = *(void***)pType;
    if (!vt) return -1;
    fn = (Fn_WhatAmI)vt[0x2C / 4];
    if (!fn) return -1;
    return fn(pType);
}

static int BQ_HeapOf(void* pType)
{
    return *(int*)((char*)pType + OFF_BT_ARRAYINDEX);
}

/* 原版键位检测 (0x53EC70, ecx=键码, 返回 AX, AH&0x80 为按下;
 * 与 0x54F25B 处原版 Shift 检测逐字节同用法)。 */
static int BQ_ShiftRaw(void)
{
    unsigned r = ((Fn_KeyState)(void*)0x53EC70u)(0x10u);
    return (r & 0x8000u) ? 1 : 0;
}

/* 点击用 Shift: 当前按下或 32 帧窗口内按过 (Poll 每帧采样)。 */
static int BQ_ShiftDown(void)
{
    if (BQ_ShiftRaw()) return 1;
    return g_BQShiftHist != 0u ? 1 : 0;
}

static void BQ_Trace(const char* head, int a, int b, int c, int d)
{
    char buf[96], b1[16], b2[16], b3[16], b4[16];
    unsigned q = 0;
    q = AppendCStr(buf, q, head, sizeof(buf));
    HtoA((unsigned)a, b1);
    q = AppendCStr(buf, q, " a=", sizeof(buf));
    q = AppendCStr(buf, q, b1, sizeof(buf));
    HtoA((unsigned)b, b2);
    q = AppendCStr(buf, q, " b=", sizeof(buf));
    q = AppendCStr(buf, q, b2, sizeof(buf));
    HtoA((unsigned)c, b3);
    q = AppendCStr(buf, q, " c=", sizeof(buf));
    q = AppendCStr(buf, q, b3, sizeof(buf));
    HtoA((unsigned)d, b4);
    q = AppendCStr(buf, q, " d=", sizeof(buf));
    q = AppendCStr(buf, q, b4, sizeof(buf));
    (void)q;
    LogLine(buf);
}

static void BQ_Log2(const char* head, int heap, int depth);
static void BQ_RemoveAt(int idx);
static int BQ_ClearType(void* house, int heap);
static int BQ_SimCount(void* house, void* type);

/* 本家本型本地数 (取消钩用, heap 匹配)。 */
static int BQ_LocalCount(void* house, int heap)
{
    int i, n = 0;
    if (!house) return 0;
    for (i = 0; i < g_BQCount; i++)
    {
        if (g_BQList[i].house == house &&
            BQ_HeapOf(g_BQList[i].type) == heap)
            n++;
    }
    return n;
}

/* 弹掉本家本型最后一个本地 (右键从队尾减, FIFO 剩下有序)。
 * 返回弹出后剩余数。 */
static int BQ_PopLast(void* house, int heap)
{
    int i;
    if (!house) return 0;
    for (i = g_BQCount - 1; i >= 0; i--)
    {
        if (g_BQList[i].house == house &&
            BQ_HeapOf(g_BQList[i].type) == heap)
        {
            BQ_RemoveAt(i);
            break;
        }
    }
    return BQ_LocalCount(house, heap);
}

static void BQ_RemoveAt(int idx)
{
    int k;
    int heap;
    if (idx < 0 || idx >= g_BQCount) return;
    heap = BQ_HeapOf(g_BQList[idx].type);
    for (k = idx; k + 1 < g_BQCount; k++)
        g_BQList[k] = g_BQList[k + 1];
    g_BQCount--;
    BQ_Log2("bqueue: unqueued", heap, g_BQCount);
}

/* 按 (house,naval,cat) 取主工厂 (与原版点击路径同调用; 槽位即权威,
 * 无 stale。工厂未具现/已死返回空, 调用方留着等, 永不丢单)。 */
static void* BQ_Resolve(void* house, unsigned naval, unsigned cat)
{
    if (!house) return 0;
    return FN_GETPRIMARYFACTORY(house, ABS_BUILDINGTYPE, naval, cat);
}

static void BQ_Log2(const char* head, int heap, int depth)
{
    char buf[64], b1[16], b2[16];
    unsigned q;
    q = 0;
    q = AppendCStr(buf, q, head, sizeof(buf));
    HtoA((unsigned)heap, b1);
    q = AppendCStr(buf, q, " heap=", sizeof(buf));
    q = AppendCStr(buf, q, b1, sizeof(buf));
    HtoA((unsigned)depth, b2);
    q = AppendCStr(buf, q, " depth=", sizeof(buf));
    q = AppendCStr(buf, q, b2, sizeof(buf));
    (void)q;
    LogLine(buf);
}

/* 复刻一次点击下单 (PRODUCE 事件手工进 OutList, 见文件头)。
 * OutList 满返回 0 (下帧重试), 否则返回 1。 */
static int BQ_FireProduce(void* house, void* type)
{
    unsigned count;
    unsigned next;
    char ev[EVT_SIZE];
    int houseIdx;
    int heap;
    int naval;
    unsigned i;
    Fn_SndCb snd;
    unsigned eaxAfter;

    count = *(unsigned*)ADDR_OUT_COUNT;
    if (count >= EVT_CAP) return 0;
    /* OutList_Next 是引擎的字段, 值必须是 0..EVT_CAP-1 才配当写下标。原来直接拿
     * 它算 next*EVT_SIZE 写进事件槽, 脏值就是一次任意地址写。OrderUtil.cpp:28
     * 那份已经 mask 过了, 这里漏了 (孪生代码不同步)。*/
    next = *(unsigned*)ADDR_OUT_NEXT & 0x7Fu;
    if (next >= EVT_CAP) return 0;

    houseIdx = *(int*)((char*)house + OFF_HOUSE_INDEX);
    heap = BQ_HeapOf(type);
    naval = *(unsigned char*)((char*)type + OFF_TT_NAVAL) ? 1 : 0;
    FN_EVENTCTOR(ev, houseIdx, EVT_PRODUCE, ABS_BUILDINGTYPE, heap, naval);

    for (i = 0; i < EVT_SIZE; i++)
        ((char*)(ADDR_OUT_BUF + next * EVT_SIZE))[i] = ev[i];

    /* 同款无参回调 + 取调用后 EAX 存并行数组 (与原版同序)。 */
    snd = *(Fn_SndCb*)0x7E1530u;
    __asm__ volatile (
        "call *%1\n\t"
        "movl %%eax, %0"
        : "=r" (eaxAfter)
        : "r" (snd)
        /* 必须列出 ebx/esi/edi/ebp: 引擎这个回调是真实函数, 会动 callee-saved
         * 寄存器, 而编译器以为它不会 —— 于是下面用 next/count 写 OutList_Count /
         * OutList_Next 时可能拿到垃圾, 引擎同帧按这份坏账本去派发事件就会跳进堆
         * (实测 0xC0000005, EIP 落在非代码区)。OrderUtil.cpp:46 早修好了。*/
        : "ecx", "edx", "ebx", "esi", "edi", "ebp", "cc", "memory");
    *(unsigned*)(ADDR_OUT_PAR + next * 4u) = eaxAfter;

    *(unsigned*)ADDR_OUT_NEXT = (next + 1u) & 0x7Fu;
    *(unsigned*)ADDR_OUT_COUNT = count + 1u;
    BQ_Log2("bqueue: fire", heap, g_BQCount);
    return 1;
}

/* 每循环轮询 (挂 PowerDrawBoth)。首行采样 Shift 进闩锁 (开关关/
 * 队列空也采样, 保证闩锁连续); 之后开关关/队列空直接返回。
 * 对账 (R0) 在喂单之前, 倒序扫: 工厂已死/易主→丢弃 (数字不能留给不存
 * 在的工厂)。就绪/待放置 (Object 非空) 只等放置, 不动队列: 放下后
 * Object==null, 下一轮自动开工 (=Phobos 文档行为)。 */
void BuildQueue_Poll(void)
{
    void* cur;
    int i;

    BQ_EnsureInit();
    g_BQShiftHist = (g_BQShiftHist << 1) | (BQ_ShiftRaw() ? 1u : 0u);
    if (!g_BQEnable) return;
    BQ_EvWatch(); /* 事件哨兵: 队列空也看 (drain 窗口正是关键) */
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur || (g_BQHouse && cur != g_BQHouse))
    {
        g_BQCount = 0; /* 菜单/读档/换人: 丢队列 (未下单=未扣费) */
        g_BQHouse = cur;
        BQ_PendClear();
        BQ_WatchClear();
        return;
    }
    if (!g_BQHouse) g_BQHouse = cur;
    BQ_PendAge();
    /* 生产监视 (drain 后也看: 沿 pend 键逐线采样, 变化才记行)。 */
    for (i = 0; i < BQ_PEND_SLOTS; i++)
    {
        if (g_BQPend[i].house && g_BQPend[i].house == cur)
            BQ_Watch(cur, g_BQPend[i].naval, g_BQPend[i].buildcat,
                     BQ_Resolve(cur, g_BQPend[i].naval,
                                g_BQPend[i].buildcat));
    }
    if (g_BQCount <= 0) return;
    /* 无对账无丢单 (0.2.95d): 条目不存工厂指针, 无 stale 可对;
     * 跨型号消失只可能来自 pop/clear (heap 键, 有 cp trace) 或换 house
     * 清空, 均有日志可查。
     * 确认制 (0.2.101d): 发单只 mark flying 不移除, 下轮见模拟认领
     * (CountTotal>0) 才移除; 执行拒收 (忙+建筑直接返 3) 则条目留着、
     * 数字留着, pend 窗后重发。发而不删, 永不哑单丢数 (认领前图标短暂
     * 双计一次, 1~2 轮即消)。 */
    if (g_BQCount <= 0) return;
    for (i = 0; i < g_BQCount; i++)
    {
        BQEntry* e = &g_BQList[i];
        void* factory;
        int k;
        int heap;
        if (e->house != cur) continue;
        factory = BQ_Resolve(cur, e->naval, e->buildcat);
        BQ_Watch(cur, e->naval, e->buildcat, factory);
        if (!factory) continue; /* 工厂不存在: 留着等具现/重建, 不丢单 */
        if (e->flying)
        {
            if (BQ_SimCount(cur, e->type) > 0)
            {
                heap = BQ_HeapOf(e->type);
                for (k = i; k + 1 < g_BQCount; k++)
                    g_BQList[k] = g_BQList[k + 1];
                g_BQCount--;
                BQ_Log2("bqueue: onboard", heap, g_BQCount);
                i--;
            }
            else if (BQ_Pending(cur, e->naval, e->buildcat))
                continue; /* 等执行 */
            /* 否则 pend 过期未认领: 下沉重发 (经 owner/object 门) */
        }
        else
        {
            if (BQ_Pending(cur, e->naval, e->buildcat)) continue;
        }
        if (*(void**)((char*)factory + OFF_FAC_OWNER) != cur) continue;
        if (*(void**)((char*)factory + OFF_FAC_OBJECT)) continue;
        BQ_MarkPending(cur, e->naval, e->buildcat);
        if (!BQ_FireProduce(cur, e->type)) continue;
        e->flying = 1;
    }
}

/* 0x6AB689 (5): 工厂忙时点建筑图标, 吞掉点击记入本地队列, 跳拒绝同尾。
 * regs: EBP=abs, EDI=图标类型, ESI=工厂。
 * Shift+左键一次记 5 个 (槽不够记到满, 满则走原版拒绝)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_QueueClick(void* regs)
{
    unsigned abs;
    void* pType;
    void* factory;
    void* house;
    int want;
    int free;
    int i;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    factory = (void*)BQ_Reg(regs, R_ESI);
    if (abs != ABS_BUILDINGTYPE || !pType || !factory) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house) return 0;
    want = BQ_ShiftDown() ? 5 : 1;
    BQ_Trace("bq: qd", BQ_HeapOf(pType), want, BQ_ShiftRaw(),
             BQ_ShiftDown());
    free = BQ_CAP - g_BQCount;
    if (free <= 0) return 0; /* 满: 走原版拒绝 */
    if (want > free) want = free;
    if (!g_BQHouse) g_BQHouse = house;
    if (house != g_BQHouse) return 0;
    for (i = 0; i < want; i++)
    {
        g_BQList[g_BQCount].house = house;
        g_BQList[g_BQCount].type = pType;
        g_BQList[g_BQCount].naval =
            *(unsigned char*)((char*)pType + OFF_TT_NAVAL) ? 1u : 0u;
        g_BQList[g_BQCount].buildcat =
            *(unsigned*)(BQ_Reg(regs, R_ESP) + 0x24u);
        g_BQList[g_BQCount].flying = 0; /* 槽位复用必须清 (0.2.103d:
            残留 flying=1 会在模拟有同型时被误认领移除) */
        g_BQCount++;
    }
    BQ_Log2("bqueue: queued", BQ_HeapOf(pType), g_BQCount);
    return TO_CLICK_EXIT;
}

/* 0x6AB6CE (10): 工厂闲时继续路径 (mov edx,[esp+0x24] + mov al,[edi+0xcce])。
 * 原版只会即时下单 1 个; Shift+左键则顺手记 4 个 (合计 5 个), 跑原指令让
 * 原版下单第 1 个。regs: EBP=abs, EDI=图标类型, ESI=工厂 (可空)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_FreeShift(void* regs)
{
    unsigned abs;
    void* pType;
    void* factory;
    void* house;
    int want;
    int free;
    int i;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    factory = (void*)BQ_Reg(regs, R_ESI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    if (!BQ_ShiftDown())
    {
        BQ_Trace("bq: qf", BQ_HeapOf(pType), 0, BQ_ShiftRaw(), 0);
        return 0;
    }
    house = *(void**)ADDR_CURPLAYER;
    if (!house) return 0;
    if (!factory) /* ESI 可空 (首次下单工厂未具现): 记 trace, 照记不误 */
        BQ_Trace("bq: qn", BQ_HeapOf(pType), 4, BQ_ShiftRaw(),
                 BQ_ShiftDown());
    want = 4;
    free = BQ_CAP - g_BQCount;
    if (free <= 0) return 0;
    if (want > free) want = free;
    if (!g_BQHouse) g_BQHouse = house;
    if (house != g_BQHouse) return 0;
    for (i = 0; i < want; i++)
    {
        g_BQList[g_BQCount].house = house;
        g_BQList[g_BQCount].type = pType;
        g_BQList[g_BQCount].naval =
            *(unsigned char*)((char*)pType + OFF_TT_NAVAL) ? 1u : 0u;
        g_BQList[g_BQCount].buildcat =
            *(unsigned*)(BQ_Reg(regs, R_ESP) + 0x24u);
        g_BQList[g_BQCount].flying = 0; /* 槽位复用必须清, 同上 */
        g_BQCount++;
    }
    BQ_Trace("bq: qf", BQ_HeapOf(pType), want, BQ_ShiftRaw(), 1);
    BQ_Log2("bqueue: queued", BQ_HeapOf(pType), g_BQCount);
    return 0;
}

/* 原生 shift 决策位 ([esp+0x13], 分发处由 [esp+0xB8]&1 置位, 原生
 * 0x6AAE3E/0x6AB1D9 凭它选 0x10 还是 0x2E) 与物理 Shift 键对照。
 * 两者不一致记一行 `bq: sh` (a=heap b=原生位 c=物理键 d=0), 用于定位
 * "按住 shift 却只减一" (原生位没置上)。 */
static void BQ_ShiftCheck(void* regs, int heap)
{
    unsigned esp;
    int bit;
    int raw;
    esp = BQ_Reg(regs, R_ESP);
    bit = (*(unsigned char*)(esp + 0x13u)) ? 1 : 0;
    raw = BQ_ShiftRaw() ? 1 : 0;
    if (bit != raw)
        BQ_Trace("bq: sh", heap, bit, raw, 0);
}

/* 0x6AB0C1 (6): 右键 state 检查 (jne 0x6AB18D, rel, 永远跳转)。
 * EAX=state 地址 (0x6AB0B8 已算好), EDI=图标类型, EBP=abs。
 * state!=1→跳取消分支 (原生 jne 去向); state==1 (本要暂停):
 * Shift+右键且本地有本型→清空后转取消分支 (原生下 0x2E);
 * 否则一律原生暂停 (0.2.113d 起: d=3 剥离退役, 与暂停优先模型一致)。
 * 只读不写。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_PausePeel(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    unsigned stateAddr;
    unsigned state;
    int heap;

    BQ_EnsureInit();
    stateAddr = BQ_Reg(regs, R_EAX);
    state = stateAddr ? *(unsigned*)stateAddr : 0u;
    {
        unsigned ab = BQ_Reg(regs, R_EBP);
        void* pt = (void*)BQ_Reg(regs, R_EDI);
        int hh = 0;
        if (ab == ABS_BUILDINGTYPE && pt &&
            BQ_WhatAmI(pt) == (int)ABS_BUILDINGTYPE)
            hh = BQ_HeapOf(pt);
        BQ_Trace("bq: h0", (int)state, hh, 0, 0);
    }
    if (state != 1u) return TO_CANCEL_BRANCH;
    if (!g_BQEnable) return TO_PAUSE_NATIVE;
    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return TO_PAUSE_NATIVE;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return TO_PAUSE_NATIVE;
    house = *(void**)ADDR_CURPLAYER;
    if (!house || house != g_BQHouse || g_BQCount <= 0)
        return TO_PAUSE_NATIVE;
    heap = BQ_HeapOf(pType);
    {
        unsigned esp = BQ_Reg(regs, R_ESP);
        int bit = (*(unsigned char*)(esp + 0x13u)) ? 1 : 0;
        BQ_ShiftCheck(regs, heap);
        if (bit && BQ_LocalCount(house, heap) > 0)
        {
            BQ_ClearType(house, heap);
            BQ_Trace("bq: cp", heap, 0, 1, 10);
            return TO_CANCEL_BRANCH;
        }
    }
    return TO_PAUSE_NATIVE;
}

/* 0x6AB197 (8): 右键取消分支入口 (push eax + push ecx +
 * mov ecx,[0xa83d4c], 无 rel)。
 * regs: EBP=abs, EDI=图标类型。到此的都是右键 (state!=1 才进取消分支;
 * state==1 的第一次暂停走 SUSPEND 后跳出口, 到不了这里)。
 *   - Shift+右键: 清空本家本型本地, 跑原指令 (原生清模拟)。
 *   - 本地有本型: 弹掉最后一个本地, 跳原生出口 0x6AB28F (在产的留最后)。
 *   - 本地无本型: 看模拟总数 (0.2.104d): 模拟>1 (本地已 drain, 在产+
 *     在排全在模拟, 此时原生 IsQueued 为真, Shift 常驻更是 AbandonAll)
 *     →拦下直接退, 在产继续; 模拟<=1 (仅剩在产单个或全空) 才跑原指令
 *     (原生暂停后取消/清空, 事件全原生字节)。
 * 全程只读寄存器+本地队列, 不仿造事件, 不碰 E8。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_CancelPeel(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    int heap;
    int left;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house || g_BQCount <= 0) return 0;
    if (house != g_BQHouse) return 0;
    heap = BQ_HeapOf(pType);
    {
        /* Shift+右键到明取消分支 (0.2.112d): 清空本地后跑原生 (原生凭同
         * 字节下 0x2E 清模拟; 空闲态右键才到这里)。c 记原生决策位。 */
        unsigned esp = BQ_Reg(regs, R_ESP);
        int bit = (*(unsigned char*)(esp + 0x13u)) ? 1 : 0;
        BQ_ShiftCheck(regs, heap);
        if (bit)
        {
            BQ_ClearType(house, heap);
            BQ_Trace("bq: cp", heap, 0, 1, 8);
            return 0;
        }
    }
    left = BQ_LocalCount(house, heap);
    if (left <= 0)
    {
        /* 本地无本型: 按总数 (本地+模拟) 决策 (0.2.104d)。
         * 本地已 drain、模拟还抱着在产/在排时, 放行原生=下发 ABANDON:
         * IsQueued 为真则 0x10, Shift 常驻手上更是 0x2E AbandonAll,
         * 在产进度作废 ("5 个 50% 一点右键从头开始"即此)。
         * 故总数>1 时什么都不做直接退 (在产继续); 总数<=1 (仅剩在产
         * 单个或全空) 才走原生。 */
        int sim = BQ_SimCount(house, pType);
        void* fac = BQ_Resolve(house,
            *(unsigned char*)((char*)pType + OFF_TT_NAVAL) ? 1u : 0u,
            *(unsigned*)(BQ_Reg(regs, R_ESP) + 0x24u));
        int busy = (fac &&
            *(void**)((char*)fac + OFF_FAC_OBJECT)) ? 1 : 0;
        BQ_Trace("bq: cs", heap, sim, busy, sim);
        if (sim > 1)
        {
            BQ_Trace("bq: cp", heap, 0, 0, 5);
            return TO_CANCEL_EXIT;
        }
        BQ_Trace("bq: cp", heap, 0, 0, 0);
        return 0;
    }
    left = BQ_PopLast(house, heap);
    BQ_Trace("bq: cp", heap, left, 0, 1);
    return TO_CANCEL_EXIT;
}

/* 真分发 (0.2.109d 反汇编定案, 右键点建筑图标):
 *   0x6AADB5: [esp+0xb0]&0x10 为右键 (否则跳原生出口);
  *   0x6AADC3: [esp+0x18]==null→0x6AB0AC 明路 (H0 state 检查,
 *     空闲/过渡态才到, 旧钩全在此);
 *   0x6AAE0D: [[esp+0x18]+0x38]!=0(在产忙) 且 [[esp+0x18]+0x70]==0(未暂停)
 *     →0x6AAFE1 暗暂停 (SUSPEND#1, 经 0x6AB02A 构造, ec 调用者 0x6AB02F);
 *   否则 (空闲或已暂停) →音效→[esp+0x13]shift?
 *   0x6AAE44: 无 shift→0x6AAFBD 暗取消 (0x10, 经 jmp 0x6AB1FF 共享尾构造,
 *     ec 调用者恒为 0x6AB204——旧 ex 哨兵蹲的 0x6AB1F8/0x6AB22A 全被 jmp
 *     绕过, 这就是 "减数对、进度死" 的真凶路);
 *   有 shift→0x6AAE4A 暗清全部 (0x2E, 同共享尾)。
 * 在产时右键永远走暗路, 明路钩子 (H0/H3) 够不着——0.2.96d 起找错分支,
 * 此版把剥离搬到真路上。暂停保持原生 (用户要"点一次暂停"); 取消按总数:
 * 本地有本型→弹尾减数 (在产/暂停不动); 本地空但模拟>1→拦下 (防 0x10 杀
 * 在产、0x2E 清全部); 仅剩在产单个→走原生真取消。
 * Shift+右键=取消全部 (0.2.111d 用户定稿): 清空本地后跑原生 0x2E,
 * 显示归零; 未暂停的先走暗暂停 (天然两步防误删)。 */

/* 0x6AAFE1 (5): 暗暂停入口 (mov eax,[0x8871e0], 无 rel)。
 * regs: EDI=图标类型 (存疑时 WhatAmI 守卫), EBP=abs。
 * 纯哨兵: 记 `bq: hd` 后永远跑原指令 (暂停保持原生)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_DarkPauseSeen(void* regs)
{
    void* pType;
    void* house;
    int heap;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;
    pType = (void*)BQ_Reg(regs, R_EDI);
    heap = (pType && BQ_WhatAmI(pType) == (int)ABS_BUILDINGTYPE) ?
        BQ_HeapOf(pType) : -1;
    house = *(void**)ADDR_CURPLAYER;
    BQ_Trace("bq: hd", heap,
             (int)(heap >= 0 && house ? BQ_LocalCount(house, heap) : 0),
             0, 0);
    return 0;
}

/* 清空本家本型全部本地 (shift+右键清全部用, 逐条移除以便 unqueued
 * 有 trace 可查)。返回清空前数量。 */
static int BQ_ClearType(void* house, int heap)
{
    int i;
    int n = 0;
    if (!house) return 0;
    for (i = g_BQCount - 1; i >= 0; i--)
    {
        if (g_BQList[i].house == house &&
            BQ_HeapOf(g_BQList[i].type) == heap)
        {
            BQ_RemoveAt(i);
            n++;
        }
    }
    return n;
}

/* 暗路剥离: 本地有本型→弹尾 (d=6) 跳原生出口;
 * 本地空→模拟>1 拦下 (d=7); 否则走原生 (d=0)。Shift 方向由分发点处理。 */
static DWORD BQ_DarkPeel(void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    int heap;
    int left;

    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    house = *(void**)ADDR_CURPLAYER;
    if (!house || g_BQCount <= 0) return 0;
    if (house != g_BQHouse) return 0;
    heap = BQ_HeapOf(pType);
    left = BQ_LocalCount(house, heap);
    if (left <= 0)
    {
        int sim = BQ_SimCount(house, pType);
        BQ_Trace("bq: cs", heap, sim, 0, sim);
        if (sim > 1)
        {
            BQ_Trace("bq: cp", heap, 0, 0, 7);
            return TO_CANCEL_EXIT;
        }
        BQ_Trace("bq: cp", heap, 0, 0, 0);
        return 0;
    }
    left = BQ_PopLast(house, heap);
    BQ_Trace("bq: cp", heap, left, 0, 6);
    return TO_CANCEL_EXIT;
}

/* 0x6AAE44 (6): 右键暗分发 (je 0x6AAFBD, rel, 永远跳转)。
 * 到此=右键+非空闲态 (在产未暂停已由上游 0x6AAE0D 分走暗暂停;
 * 此处为已暂停/空闲)。EDI=图标类型, EBP=abs,
 * [esp+0x13]=shift (0→暗取消 0x6AAFBD, 1→暗清全 0x6AAE4A)。
 * Jcc 永跳 (铁律: 偷 rel 永不 return 0; 另 0.2.109d 崩溃教训:
 * 9/10 字节钩非零返回疑触 Syringe 短桩问题, 此处改回 6 字节 Jcc
 * 模式, 与 H0/H1 同构)。去向: 剥离跳 0x6AB28F 原生出口;
 * 放行跳回原生去向 (保持栈/寄存器与原生到达一致, 跳过仅为 je 本身)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_DarkDispatchPeel(
    void* regs)
{
    unsigned abs;
    void* pType;
    void* house;
    unsigned esp;
    int shift;
    int heap;

    BQ_EnsureInit();
    esp = BQ_Reg(regs, R_ESP);
    shift = (*(unsigned char*)(esp + 0x13u)) ? 1 : 0;
    if (!g_BQEnable)
        return (DWORD)(shift ? TO_DARK_ALL_NATIVE : TO_DARK_CANCEL_NATIVE);
    abs = BQ_Reg(regs, R_EBP);
    pType = (void*)BQ_Reg(regs, R_EDI);
    if (abs != ABS_BUILDINGTYPE || !pType)
        return (DWORD)(shift ? TO_DARK_ALL_NATIVE : TO_DARK_CANCEL_NATIVE);
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE)
        return (DWORD)(shift ? TO_DARK_ALL_NATIVE : TO_DARK_CANCEL_NATIVE);
    house = *(void**)ADDR_CURPLAYER;
    if (!house || house != g_BQHouse)
        return (DWORD)(shift ? TO_DARK_ALL_NATIVE : TO_DARK_CANCEL_NATIVE);
    heap = BQ_HeapOf(pType);
    if (shift)
    {
        /* Shift+右键=取消全部 (用户定稿, 0.2.111d): 清空本家本型本地后跑
         * 原生 (0x2E 清模拟, 在产亦清, 显示归零, 如 5→0/10→0)。
         * 未暂停的到不了这里 (上游进了暗暂停块先暂停), 天然两步防误删:
         * 松开 shift 点=减一, 按住 shift 点=清零。 */
        BQ_ClearType(house, heap);
        BQ_Trace("bq: cp", heap, 0, 1, 8);
        return TO_DARK_ALL_NATIVE;
    }
    {
        DWORD r = BQ_DarkPeel(regs);
        if (r) return r;
    }
    return TO_DARK_CANCEL_NATIVE;
}

/* 0x6A9C6C (6): CountTotal 前工厂空检查 (test eax,eax +
 * mov [esp+0x18],eax, 无 rel)。EAX=GPF 工厂 (可空), [ESP+0x34]=图标类型。
 * 工厂非空→跑原指令; 工厂空 (建筑页没下过单槽位无工厂/绘制按 buildcat=0
 * 取槽而防御页在它槽, 连带被跳过) 且本地有本型→EAX/EBX=模拟全厂和+本地数
 * 后跳绘制块 (0.2.111d: 原先只画本地数, 在产的 1 个被漏计, 防御页点 5 个
 * 显示 4 即此因); 工厂空且本地无→置 [esp+0x18]=0 后跳原生跳过处。
 * [esp+0x18] 在两目标之后无人读, 绘制跳跃无需处理它。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_DrawNullFac(void* regs)
{
    unsigned esp;
    void* pType;
    void* cur;
    unsigned v;
    int n;
    int i;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;
    if (BQ_Reg(regs, R_EAX)) return 0;

    esp = BQ_Reg(regs, R_ESP);
    pType = *(void**)(esp + 0x34u);
    cur = *(void**)ADDR_CURPLAYER;
    n = 0;
    if (pType && cur)
    {
        for (i = 0; i < g_BQCount; i++)
        {
            if (g_BQList[i].house == cur && g_BQList[i].type == pType)
                n++;
        }
    }
    if (n <= 0)
    {
        *(unsigned*)(esp + 0x18u) = 0u;
        return TO_DRAWSKIP;
    }
    /* 工厂空不代表没在产 (槽位不对而已): 全厂求和补上, 与 ShowCount 同口径。 */
    v = (unsigned)BQ_SimCount(cur, pType) + (unsigned)n;
    if (!v) return 0;
    *(unsigned*)((char*)regs + R_EAX) = v;
    *(unsigned*)((char*)regs + R_EBX) = v;
    return TO_DRAWNUM;
}

/* 本家各工厂中该型的模拟数之和。绘制只认一个槽位工厂 (buildcat=0),
 * 跨页在产会被漏计 (防御页 5 显示 4 即此因); 全扫才对。地址全经反汇编
 * 实证, 只读不写。 */
static int BQ_SimCount(void* house, void* type)
{
    void** items;
    int count;
    int i;
    int sim = 0;
    if (!house || !type) return 0;
    items = *(void***)ADDR_FACARR_ITEMS;
    count = *(int*)ADDR_FACARR_COUNT;
    if (!items || count <= 0) return 0;
    for (i = 0; i < count; i++)
    {
        void* fac = items[i];
        if (!fac) continue;
        if (*(void**)((char*)fac + OFF_FAC_OWNER) != house) continue;
        sim += FN_COUNTTOTAL(fac, type);
    }
    return sim;
}

/* 0x6A9C86 (5): DrawStrip 的 CountTotal 取数点 (mov ebx,eax + cmp ebx,1)。
 * EAX 原生数只认单槽工厂, 跨页漏计, 故本地有排队或原生数为 0 时改用全厂
 * 求和 (sim+n) 后跳绘制块借用原版绘制; 其余 (单位/建筑页单个在产原生数>0)
 * 跑原指令零影响。[ESP+0x34]=图标类型 (调用前后 esp 无净变化)。 */
extern "C" __declspec(dllexport) DWORD __cdecl BQ_ShowCount(void* regs)
{
    unsigned esp;
    void* pType;
    void* cur;
    unsigned v;
    int n;
    int i;
    int sim;

    BQ_EnsureInit();
    if (!g_BQEnable) return 0;

    esp = BQ_Reg(regs, R_ESP);
    pType = *(void**)(esp + 0x34u);
    if (!pType) return 0;
    if (BQ_WhatAmI(pType) != (int)ABS_BUILDINGTYPE) return 0;
    cur = *(void**)ADDR_CURPLAYER;
    if (!cur) return 0;
    n = 0;
    for (i = 0; i < g_BQCount; i++)
    {
        if (g_BQList[i].house == cur && g_BQList[i].type == pType)
            n++;
    }
    if (n <= 0 && BQ_Reg(regs, R_EAX)) return 0;
    sim = BQ_SimCount(cur, pType);
    v = (unsigned)sim + (unsigned)n;
    if (!v) return 0;
    *(unsigned*)((char*)regs + R_EAX) = v;
    *(unsigned*)((char*)regs + R_EBX) = v;
    return TO_DRAWNUM;
}

/* 钩历史 (皆已删, 教训保留):
 * 0.2.90d 前仿事件构造调用→执行侧崩 (E8 点永不碰);
 * 0.2.91d R1 镜像清→放置瞬间清队列 (放置与取消不可分);
 * 0.2.95d R0 工厂指针对账→开工换指针丢单 (改现解析);
 * 0.2.113d 撤 H1/H2/H0P/ex/ec (全版本零触发, 死钩)。
 * 反汇编实证 (0x6AA7B6-0x6AA931): 左键点建筑图标, 原生先按 [[esp+0x2c]]
 * 发 ABANDON (0x6AA7E7) +AbandonAll (0x6AA8CD) 切在产——但跨型号点击实际
 * 永远走忙拒绝+本地记入 (h1/sw 全程缺席), 故撤钩后行为不变。 */

