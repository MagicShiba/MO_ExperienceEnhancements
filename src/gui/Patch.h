/* Patch.h - 引擎代码打补丁的三件套 (字节比对 / 跳板 / E9 改写)。
 * 各模块下钩共用; 失败路径带日志, 现场只看日志就能定位。
 *
 * 只用 Win32 与 GdiText 的日志 (无 CRT)。全 static inline, 各 TU 内联一份。
 */
#pragma once

#include <windows.h>
#include "GdiText.h"

static inline int PatchSame(const void* a, const void* b, int n)
{
    int i;
    for (i = 0; i < n; i++) if (((const unsigned char*)a)[i] != ((const unsigned char*)b)[i]) return 0;
    return 1;
}

/* 通用跳板: 拷 at 处 n 字节再 jmp 回 back。`n` 须落在完整指令末字节上
 * (落半截即从半个指令反汇编跑飞); 范围内禁 E8/E9 (rel32 基准变跳板自己
 * ⇒ 跳数据地址去), 这里直接拒绝。用前先反汇编确认末字节。 */
static inline void* PatchTrampAt(unsigned at, int n, unsigned back)
{
    unsigned char* b;
    int i;
    if (n < 1 || n > 16) return NULL;
    for (i = 0; i < n; i++)
    {
        unsigned char c = ((unsigned char*)at)[i];
        if (c == 0xE8 || c == 0xE9)
        {
            char m[80], n1[16];
            unsigned q = 0;
            q = AppendCStr(m, q, "tramp refused: PC-relative at ", sizeof(m));
            UtoA(at, n1); q = AppendCStr(m, q, n1, sizeof(m));
            q = AppendCh(m, q, ' ', sizeof(m));
            UtoA((unsigned)c, n1); q = AppendCStr(m, q, n1, sizeof(m));
            LogLine(m);
            return NULL;
        }
    }
    b = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                     PAGE_EXECUTE_READWRITE);
    if (!b) return NULL;
    for (i = 0; i < n; i++) b[i] = ((unsigned char*)at)[i];
    b[n] = 0xE9;
    *(int*)(b + n + 1) = (int)(back - ((unsigned)(size_t)b + n + 5));
    FlushInstructionCache(GetCurrentProcess(), b, (SIZE_T)(n + 5));
    return b;
}

/* at 处改写为 jmp target; n>5 的余量补 NOP。 */
static inline int PatchAt(unsigned at, void* target, int n)
{
    unsigned char* p = (unsigned char*)at;
    DWORD old;
    int rel = (int)((unsigned)(size_t)target - (at + 5));
    int i;
    if (n < 5) return 0;
    if (!VirtualProtect(p, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    p[0] = 0xE9;
    *(int*)(p + 1) = rel;
    for (i = 5; i < n; i++) p[i] = 0x90;
    VirtualProtect(p, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, (SIZE_T)n);
    return 1;
}
