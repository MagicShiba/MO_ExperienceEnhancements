#include <windows.h>
#include <emmintrin.h>
#include "Resample565.h"

static void R565_Row2Scalar(const unsigned short* src, unsigned short* dst, int count)
{
    while (count >= 4)
    {
        dst[0] = src[0]; dst[1] = src[2];
        dst[2] = src[4]; dst[3] = src[6];
        src += 8; dst += 4; count -= 4;
    }
    while (count-- > 0) { *dst++ = *src; src += 2; }
}

/* 局部启用 SSE2，分派器与标量后备不要求 CPU 支持 SSE2。 */
__attribute__((target("sse2"), noinline))
static void R565_Row2SSE2(const unsigned short* src, unsigned short* dst, int count)
{
    while (count >= 8)
    {
        __m128i a = _mm_loadu_si128((const __m128i*)src);
        __m128i b = _mm_loadu_si128((const __m128i*)(src + 8));
        /* 每个 dword 的低 word 是偶数列。符号扩展使 pack 保留全部 16 位。 */
        a = _mm_srai_epi32(_mm_slli_epi32(a, 16), 16);
        b = _mm_srai_epi32(_mm_slli_epi32(b, 16), 16);
        _mm_storeu_si128((__m128i*)dst, _mm_packs_epi32(a, b));
        src += 16; dst += 8; count -= 8;
    }
    R565_Row2Scalar(src, dst, count);
}

static int R565_HasSSE2()
{
    /* 渲染线程使用；0=未查询，1=标量，2=SSE2。所有行操作共用一次查询。 */
    static int cpu = 0;
    if (!cpu) cpu = IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) ? 2 : 1;
    return cpu == 2;
}

void R565_DownsampleRow2(const unsigned short* src, unsigned short* dst, int count)
{
    if (count <= 0) return;
    if (R565_HasSSE2() && count >= 8) R565_Row2SSE2(src, dst, count);
    else R565_Row2Scalar(src, dst, count);
}

void R565_MapRow(const unsigned short* src, unsigned short* dst, const int* map, int count)
{
    if (count <= 0) return;
    while (count >= 4)
    {
        dst[0] = src[map[0]]; dst[1] = src[map[1]];
        dst[2] = src[map[2]]; dst[3] = src[map[3]];
        dst += 4; map += 4; count -= 4;
    }
    while (count-- > 0) *dst++ = src[*map++];
}

typedef unsigned int R565_Word __attribute__((may_alias, aligned(1)));

static void R565_CopyScalar(const unsigned short* src, unsigned short* dst, int count)
{
    while (count >= 8)
    {
        R565_Word a = *(const R565_Word*)src;
        R565_Word b = *(const R565_Word*)(src + 2);
        R565_Word c = *(const R565_Word*)(src + 4);
        R565_Word d = *(const R565_Word*)(src + 6);
        *(R565_Word*)dst = a; *(R565_Word*)(dst + 2) = b;
        *(R565_Word*)(dst + 4) = c; *(R565_Word*)(dst + 6) = d;
        src += 8; dst += 8; count -= 8;
    }
    while (count-- > 0) *dst++ = *src++;
}

__attribute__((target("sse2"), noinline))
static void R565_CopySSE2(const unsigned short* src, unsigned short* dst, int count)
{
    while (count >= 32)
    {
        __m128i a = _mm_loadu_si128((const __m128i*)src);
        __m128i b = _mm_loadu_si128((const __m128i*)(src + 8));
        __m128i c = _mm_loadu_si128((const __m128i*)(src + 16));
        __m128i d = _mm_loadu_si128((const __m128i*)(src + 24));
        _mm_storeu_si128((__m128i*)dst, a);
        _mm_storeu_si128((__m128i*)(dst + 8), b);
        _mm_storeu_si128((__m128i*)(dst + 16), c);
        _mm_storeu_si128((__m128i*)(dst + 24), d);
        src += 32; dst += 32; count -= 32;
    }
    while (count >= 8)
    {
        _mm_storeu_si128((__m128i*)dst, _mm_loadu_si128((const __m128i*)src));
        src += 8; dst += 8; count -= 8;
    }
    R565_CopyScalar(src, dst, count);
}

void R565_CopyRow(const unsigned short* src, unsigned short* dst, int count)
{
    if (count <= 0) return;
    if (R565_HasSSE2() && count >= 8) R565_CopySSE2(src, dst, count);
    else R565_CopyScalar(src, dst, count);
}
