/* SWSTest.cpp - 自动化时序回归 (`[Log] TestSws`), 从 SWSBar.cpp 挪出来。
 *
 *   TestSws = off@8000,on@14000,hide@20000,show@26000,dd0@32000,dd1@38000
 *
 * 语法: 动作@毫秒, 逗号分隔, 每项只触发一次。动作走与鼠标/热键同一条状态路径,
 * 所以量到的开销就是真实路径的开销 —— 一次运行就能把"正常/关闭/收起"几种状态
 * 的帧率摆在一起比。空值 = 不启用, 平时零开销。
 *
 * 计时从进对局那刻算, 单位毫秒。用帧号不行: 载入期帧号已跑过上万, 写死的帧号
 * 会在进对局前全部触发完。原理与用法见 notes/超武侧栏.md §11.3。
 */

#include <windows.h>

#include "EngineAddr.h"
#include "SWSBar.h"
#include "SWSTest.h"
#include "GUI/GdiText.h"

#define T_N 10
/* 与 SWS_TestAction 的动作号同序; 0/1 由本文件直接调 SWS_SetEnabled。 */
enum { TK_OFF = 0, TK_ON, TK_HIDE, TK_SHOW, TK_DD0, TK_DD1 };
static const char* const kName[] = { "off", "on", "hide", "show", "dd0", "dd1" };
#define TK_DONE 0xFF

static int s_At[T_N];               /* 触发时刻 (ms) */
static unsigned char s_Kind[T_N];
static int s_N = 0;

/* 不用 CRT: 前缀比较与键长自己写 (编译器可能把循环认成 strlen 习语)。 */
static int Pre(const char* s, const char* p)
{
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}
static unsigned StrLen(const char* k)
{
    unsigned n = 0;
    while (*(volatile const char*)(k + n)) n++;   /* volatile: 防被折成 strlen */
    return n;
}

static void LogTest(const char* name, int ms)
{
    char b[64], n[16];
    unsigned q = AppendCStr(b, 0, "sws: test ", sizeof(b));
    q = AppendCStr(b, q, name, sizeof(b));
    q = AppendCStr(b, q, " ms=", sizeof(b));
    UtoA((unsigned)ms, n);
    AppendCStr(b, q, n, sizeof(b));
    LogLine(b);
}

void SWSTest_Init(void)
{
    char buf[512];
    unsigned pos = 0, n;
    s_N = 0;
    n = GetPrivateProfileStringA("Log", "TestSws", "", buf,
                                 (unsigned)sizeof(buf), g_IniPath);
    if (n == 0 || n >= sizeof(buf)) return;
    while (buf[pos] && s_N < T_N)
    {
        const char* t = buf + pos;
        int k;
        for (k = 0; k < (int)(sizeof(kName) / sizeof(kName[0])); k++)
            if (Pre(t, kName[k])) break;
        k = (k < (int)(sizeof(kName) / sizeof(kName[0]))) ? k : -1;
        if (k >= 0 && t[StrLen(kName[k])] == '@')
        {
            unsigned j = StrLen(kName[k]) + 1;
            int ms = 0;
            while (t[j] >= '0' && t[j] <= '9') { ms = ms * 10 + (t[j] - '0'); j++; }
            s_At[s_N] = ms;
            s_Kind[s_N] = (unsigned char)k;
            s_N++;
            pos += j;
        }
        /* 跳到下一个逗号 (认不出的项直接丢) */
        while (buf[pos] && buf[pos] != ',') pos++;
        while (buf[pos] == ',' || buf[pos] == ' ') pos++;
    }
}

void SWSTest_Tick(void)
{
    static unsigned s_T0 = 0;
    unsigned now, ms;
    int i;
    if (s_N == 0) return;
    if (!*(unsigned char*)ADDR_SCEN_STARTED) return;
    now = GetTickCount();
    if (!s_T0) { s_T0 = now; return; }   /* 首帧只打点 */
    ms = now - s_T0;
    for (i = 0; i < s_N; i++)
    {
        int k = s_Kind[i];
        if (k == TK_DONE || ms < (unsigned)s_At[i]) continue;
        s_Kind[i] = TK_DONE;
        LogTest(kName[k], (int)ms);
        if (k == TK_OFF || k == TK_ON) SWS_SetEnabled(k == TK_ON);
        else                           SWS_TestAction(k);
    }
}
