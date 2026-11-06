/* ShpClip.cpp - SHP 底层绘制必须按实际目标面裁剪，而非只相信视口矩形。
 * 只改变本地光栅范围，不调用模拟状态函数。 */
#include <windows.h>
#include "GdiText.h"
#include "Patch.h"
#include "SurfaceClip.h"

static void* s_RleTramp = 0;
static void* s_PlainTramp = 0;

/* 两条底层入口的 ECX=目标 Surface，EDX=裁剪矩形。原生裁剪器会同步
 * 调整源/目标矩形及行数，不能只在 Lock 时挡住首行坐标。 */
static int ShpClipBounds(void* surf, const RectangleStruct* bounds,
                         RectangleStruct* clipped)
{
    *clipped = *bounds;
    return SurfaceClipBounds(&clipped->X, &clipped->Y,
        &clipped->Width, &clipped->Height,
        *(int*)((char*)surf + 4), *(int*)((char*)surf + 8));
}
static void* s_ClipCall = (void*)ShpClipBounds;

/* 局部矩形必须存活到整个原生调用结束。跳板通过 call 运行，因此复制栈参，
 * 让原生 ret N 清理副本，再清理原调用的 N 字节。 */
#define SHP_CLIP_ENTRY(name, tramp, bytes) \
__attribute__((naked)) static void name(void) \
{ \
    __asm__ volatile ( \
        "subl $16, %%esp\n\t" \
        "pushfl\n\t" \
        "pushal\n\t" \
        "leal 36(%%esp), %%eax\n\t" \
        "pushl %%eax\n\t" \
        "pushl %%edx\n\t" \
        "pushl %%ecx\n\t" \
        "call *%0\n\t" \
        "addl $12, %%esp\n\t" \
        "testl %%eax, %%eax\n\t" \
        "jz 2f\n\t" \
        "leal 36(%%esp), %%eax\n\t" \
        "movl %%eax, 20(%%esp)\n\t" \
        "popal\n\t" \
        "popfl\n\t" \
        "pushl %%ebp\n\t" \
        "movl %%esp, %%ebp\n\t" \
        "subl $" #bytes ", %%esp\n\t" \
        "pushal\n\t" \
        "leal 24(%%ebp), %%esi\n\t" \
        "leal 32(%%esp), %%edi\n\t" \
        "movl $" #bytes "/4, %%ecx\n\t" \
        "cld\n\t" \
        "rep movsl\n\t" \
        "popal\n\t" \
        "call *%1\n\t" \
        "popl %%ebp\n\t" \
        "addl $16, %%esp\n\t" \
        "ret $" #bytes "\n\t" \
        "2: popal\n\t" \
        "popfl\n\t" \
        "addl $16, %%esp\n\t" \
        "xorl %%eax, %%eax\n\t" \
        "ret $" #bytes "\n\t" \
        : : "m"(s_ClipCall), "m"(tramp) : "memory", "cc"); \
}
SHP_CLIP_ENTRY(ShpClipRle, s_RleTramp, 52)
SHP_CLIP_ENTRY(ShpClipPlain, s_PlainTramp, 40)

void ShpClip_Install(void)
{
    static int installed = 0;
    static const unsigned char rle[] = { 0x83, 0xEC, 0x6C, 0x8B, 0x44, 0x24, 0x7C };
    static const unsigned char plain[] = { 0x83, 0xEC, 0x4C, 0x8B, 0x44, 0x24, 0x5C };
    if (installed) return;
    installed = 1;
    const unsigned sites[] = { 0x437A10u, 0x4373B0u };
    const unsigned char* want[] = { rle, plain };
    void* entry[] = { (void*)ShpClipRle, (void*)ShpClipPlain };
    void** tramp[] = { &s_RleTramp, &s_PlainTramp };
    for (unsigned i = 0; i < 2; ++i)
    {
        if (PatchSame((void*)sites[i], want[i], 7))
        {
            *tramp[i] = PatchTrampAt(sites[i], 7, sites[i] + 7);
            if (*tramp[i] && PatchAt(sites[i], entry[i], 7))
            {
                LogLine(i ? "shpclip: plain hook ON" : "shpclip: RLE hook ON");
                continue;
            }
        }
        LogLine(i ? "shpclip: plain hook OFF (site unavailable)"
                  : "shpclip: RLE hook OFF (site unavailable)");
    }
}
