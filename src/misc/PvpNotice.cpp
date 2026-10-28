/* PvpNotice.cpp - 无 AI 的对局里定时在聊天中声明本机使用了拓展 (非纯净版本)。
 *
 * 判定: GameMode (0xA8B238) 且 ScenarioStarted (0xA8ED5C) —— 大厅/载入屏不算,
 *       那里发出去的消息会跟着开局重置一起没掉。
 *         LAN(3) / Internet(4) = 联机, 恒每 30 秒一条, 不受配置影响。
 *         Skirmish(5)          = 单人, 按 [PvpNotice] Single:
 *                                0=不发 (默认) / 1=也发 (自测) / 2=只发一次。
 *       两种都还要求房表里除中立房外没有非人类房, 即没有 AI。
 * 文本: 硬编码在本文件, 不走 ini / 语言文件, 只按系统语言选中/英文。
 *       若游戏根目录存在 shroud.shp (替换件 = 透明迷雾), 句中再补一句
 *       "也启用了透明迷雾。", 免得不透明的一方按原版迷雾判断视野。
 *
 * 发送: 引擎的聊天广播是 MainLoop 按键处理函数 0x55E420 的内部代码块
 *       (起点 0x55EDBF), 不是可调用函数。MessageListClass::Input (0x5D4510)
 *       收下回车只关框并返回 3, 真正的广播在调用者的栈帧里 —— 所以 ChatEdit
 *       那套 "写 EditBuffer + 原生 Input" 只解决提交, 不能拿来代发。
 *       这里伪造 0x55E420 的栈帧后跳进该块, 由引擎原码自行收尾 ret ——
 *       不拼报文、不写模拟内存。该块会再调一次 0x4F42F0, 故 PowerDrawBoth
 *       有重入闸 (见 MOExperience.cpp)。
 *       前置: 0xABCE18 (聊天目标模式) 由"开框"那一步置位 (Enter=1/同盟=2/
 *       全部=3), 收底清 0; 直接跳块时它是 0, "发给谁"的判定全落空, 只剩本地
 *       回声。故发送前临时置 3, 发完还原。
 *       正文先写进 MessageListClass::EditBuffer, EditInitialPos 置 0
 *       (GetEditBuffer 0x5D4400 只按 InitialPos 取址)。
 */

#include <windows.h>
#include "EngineAddr.h"
#include "GdiText.h"
#include "Lang.h"
#include "MOEVersion.h"
#include "PvpNotice.h"

/* ---------------- 引擎地址 ---------------- */

#define ADDR_MSGLIST       0xA8BC60u  /* MessageListClass::Instance */
#define OFF_ML_EDITBUF     0x28u
#define OFF_ML_INITPOS     0x2B4u     /* EditInitialPos */
#define ADDR_CHAT_MODE     0xABCE18u  /* 聊天目标模式: 1=玩家 / 2=同盟 / 3=全部 */
#define ADDR_CHAT_MASK     0xA8D667u  /* 本次实际投递目标掩码, 0 = 只本地回声 */
#define ADDR_NET_FLAG      0xA8DBA0u  /* 0 = 逐连接直发, 非 0 = 汇总成一条广播 */

#define GM_LAN      3
#define GM_INTERNET 4
#define GM_SKIRMISH 5

/* 报文正文域 0x30..0x10F = 224 字节 = 112 wchar, 取 108 留出结尾 0。 */
#define PN_TEXT_CAP  108
#define PN_INTERVAL  30000u
#define PN_WARMUP    3000u    /* 开局后再静默 3 秒才发首条 */

/* ---------------- 状态 ---------------- */

static int      g_Init = 0;
static int      g_Single = 0;      /* [PvpNotice] Single: 0/1/2, 只管单人 */
static int      g_SingleDone = 0;  /* Single=2 本局已发过 */
static unsigned g_NextTick = 0;    /* 下次可发时刻, 0 = 立即 */
static int      g_InMatch = 0;     /* 已见过 ScenarioStarted, 首条已排期 */
static int      g_Dbg = 0;         /* 首次发送打一次投递掩码 */
static wchar_t  g_Text[PN_TEXT_CAP];
static int      g_Len = 0;

/* ---------------- 文本 ---------------- */

static void PN_MakeText(const wchar_t* head, const wchar_t* mid, const wchar_t* tail)
{
    int n = 0, i;

    for (i = 0; head[i] && n < PN_TEXT_CAP - 1; i++) g_Text[n++] = head[i];
    for (i = 0; MOE_VERSION[i] && n < PN_TEXT_CAP - 1; i++) g_Text[n++] = (wchar_t)MOE_VERSION[i];
    for (i = 0; mid[i] && n < PN_TEXT_CAP - 1; i++) g_Text[n++] = mid[i];
    for (i = 0; tail[i] && n < PN_TEXT_CAP - 1; i++) g_Text[n++] = tail[i];
    g_Text[n] = 0;
    g_Len = n;
}

void PvpNotice_EnsureInit(void)
{
    char buf[64];
    unsigned p = 0;
    int shroud;

    if (g_Init) return;
    g_Init = 1;
    if (!g_IniPath[0]) BuildPaths();

    /* 根目录放了替换用的 shroud.shp = 同时开了透明迷雾, 一并声明。 */
    shroud = FileAtGameRoot("shroud.shp");
    if (Lang_IsChinese())
        PN_MakeText(L"大家看我！宣布个事！本机启用了「心灵终结体验拓展 ",
                    shroud ? L"」，也启用了透明迷雾。" : L"」。",
                    L"这是自动发送的，联机不要作弊哦！");
    else
        PN_MakeText(L"Mental Omega Experience Enhancements ",
                    shroud ? L" (non-vanilla, translucent shroud)" : L" (non-vanilla)",
                    L" may affect match balance.");
    g_Single = GetPrivateProfileIntA("PvpNotice", "Single", 0, g_IniPath);
    if (g_Single < 0) g_Single = 0;
    if (g_Single > 2) g_Single = 2;
    p = AppendCStr(buf, 0, "pvponotice: 30s, single=", sizeof(buf));
    p = AppendInt(buf, p, sizeof(buf), g_Single);
    p = AppendCStr(buf, p, ", shroud=", sizeof(buf));
    p = AppendInt(buf, p, sizeof(buf), shroud);
    LogLine(buf);
}

/* ---------------- 判定 ---------------- */

/* 房表里没有"既非人类又非中立"的房 = 没有 AI。真人数不限。 */
static int PN_NoAi(void)
{
    void** items = *(void***)(size_t)ADDR_HOUSES_ITEMS;
    int n = *(const int*)(size_t)ADDR_HOUSES_COUNT, i;

    if (!items || n <= 0) return 0;
    for (i = 0; i < n; i++)
    {
        char* h = (char*)items[i];
        char* ht;
        if (!h) continue;
        if (*(unsigned char*)(h + OFF_IS_HUMAN)) continue;
        ht = *(char**)(h + OFF_HOUSE_TYPE);
        if (ht && *(unsigned char*)(ht + OFF_HTYPE_PASSIVE) == 1) continue;
        return 0;
    }
    return 1;
}

/* ---------------- 发送 ----------------
 * 伪造 0x55E420 的栈帧后跳进聊天广播块, 由引擎原码收尾返回。
 * 哨兵压各寄存器的真实值: 引擎块尾声 pop edi/esi/ebp/ebx 会把它们弹回,
 * 用固定常量会破坏调用者的 callee-saved 寄存器。分支判断仍需 edi=3/ebp=4。 */
__attribute__((naked)) static void PN_ChatSendCore(void)
{
    __asm__ __volatile__(
        "sub $0x890, %esp\n\t"     /* 0x55E420 的局部区大小 */
        "pushl %ebx\n\t"
        "pushl %ebp\n\t"
        "pushl %esi\n\t"
        "pushl %edi\n\t"
        "movl $3, 0x10(%esp)\n\t"  /* 正文来源 = EditBuffer */
        "movl $3, %edi\n\t"        /* 报文类型置 0x0C (聊天) */
        "movl $4, %ebp\n\t"
        "xorl %ebx, %ebx\n\t"
        "xorl %esi, %esi\n\t"
        "movl $0x55edbf, %eax\n\t"
        "jmp *%eax\n\t"
    );
}

static void PN_Send(void)
{
    char* ml = (char*)(size_t)ADDR_MSGLIST;
    wchar_t* eb = (wchar_t*)(ml + OFF_ML_EDITBUF);
    int* mode = (int*)(size_t)ADDR_CHAT_MODE;
    int old = *mode;
    int i;

    for (i = 0; i <= g_Len; i++) eb[i] = g_Text[i];
    *(int*)(ml + OFF_ML_INITPOS) = 0;

    /* 玩家按回车开聊时引擎会把 0xABCE18 置 1 (1=玩家 / 2=同盟 / 3=全部),
     * 收底再清 0。我们直接跳广播块, 没经过"开框"那一步, 于是它停在 0 ——
     * 广播块里"发给谁"的判定全落空, 掩码 0xA8D667 保持 0, 只做本地回声,
     * 对方收不到。这里按"发给全部人类玩家"临时置 3, 发完还原。 */
    *mode = 3;
    PN_ChatSendCore();
    *mode = old;

    /* 广播块不清编辑缓冲, 留着下次开聊天框会看到残句。 */
    eb[0] = 0;

    if (!g_Dbg)
    {
        char b[72];
        unsigned p = 0;
        g_Dbg = 1;
        p = AppendCStr(b, 0, "pvponotice: mode ", sizeof(b));
        p = AppendInt(b, p, sizeof(b), old);
        p = AppendCStr(b, p, "->3, mask=", sizeof(b));
        p = AppendInt(b, p, sizeof(b), (int)*(const unsigned char*)(size_t)ADDR_CHAT_MASK);
        p = AppendCStr(b, p, ", net=", sizeof(b));
        p = AppendInt(b, p, sizeof(b), (int)*(const unsigned char*)(size_t)ADDR_NET_FLAG);
        LogLine(b);
    }
}

/* ---------------- 每循环 ---------------- */

void PvpNotice_Poll(void)
{
    int gm = *(const int*)(size_t)ADDR_SESSION;
    int mp = (gm == GM_LAN || gm == GM_INTERNET);
    unsigned now;

    /* 战役/菜单, 或还没真正开局 (大厅、载入屏) —— 一条都不发。
     * 大厅里发的会随开局重置消息列表一起没掉, 所以首条要等 ScenarioStarted。 */
    if ((!mp && gm != GM_SKIRMISH) ||
        !*(const volatile unsigned char*)(size_t)ADDR_SCEN_STARTED)
    {
        g_InMatch = 0;
        g_NextTick = 0;
        g_SingleDone = 0;
        return;
    }

    if (!g_InMatch)
    { g_InMatch = 1; g_NextTick = GetTickCount() + PN_WARMUP; }  /* 首条延后 */

    /* 有 AI 就不发; 这里别清 g_NextTick, 否则房表刚建好的那几帧会把开局的
     * 静默期抹掉, 首条又提前到载入尾巴上。 */
    if (!PN_NoAi()) return;

    if (!mp && (g_Single == 0 || (g_Single == 2 && g_SingleDone))) return;

    now = GetTickCount();
    if ((int)(now - g_NextTick) < 0) return;

    g_NextTick = now + PN_INTERVAL;
    if (!mp) g_SingleDone = 1;
    PN_Send();
    LogLine("pvponotice: sent");
}
