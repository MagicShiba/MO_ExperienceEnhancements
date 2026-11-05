/* WeatherCloudBlend.h - 顺序 16bpp 云层混色。抖动只依赖目标像素坐标。 */
#pragma once
#include <emmintrin.h>

static const unsigned char kWthCloudBayer[16] = {
     0,  8,  2, 10, 12,  4, 14,  6,
     3, 11,  1,  9, 15,  7, 13,  5
};

struct WthCloudTone {
    unsigned rsh, gm;
    unsigned target[3];
};

template<bool Fog>
static inline void WthCloudScalar(unsigned short* dst, const unsigned char* alpha,
                                  int n, int x, int y, const WthCloudTone& tone)
{
    const unsigned char* dr = kWthCloudBayer + (y & 3) * 4;
    for (int i = 0; i < n; i++)
    {
        unsigned a = alpha[i];
        if (!a) continue;
        unsigned v = dst[i], m = 256 - a;
        unsigned d = dr[(x + i) & 3] * 16u + 8u;
        unsigned r = ((v >> tone.rsh) & 31) * m + d;
        unsigned g = ((v >> 5) & tone.gm) * m + d;
        unsigned b = (v & 31) * m + d;
        if (Fog)
        {
            r += tone.target[0] * a;
            g += tone.target[1] * a;
            b += tone.target[2] * a;
        }
        dst[i] = (unsigned short)(((r >> 8) << tone.rsh) | ((g >> 8) << 5) | (b >> 8));
    }
}

#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#define WTH_HAVE_SSE2 1
#define WTH_CLOUD_SSE2 __attribute__((target("sse2")))
struct WthCloudSimd {
    __m128i zero, full, mask5, maskG, rshift, target[3], dither[4][4];

    WTH_CLOUD_SSE2 void Init(const WthCloudTone& tone)
    {
        zero = _mm_setzero_si128();
        full = _mm_set1_epi16(256);
        mask5 = _mm_set1_epi16(31);
        maskG = _mm_set1_epi16((short)tone.gm);
        rshift = _mm_cvtsi32_si128((int)tone.rsh);
        for (int c = 0; c < 3; c++) target[c] = _mm_set1_epi16((short)tone.target[c]);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
            {
                const unsigned char* dr = kWthCloudBayer + y * 4;
                short a = (short)(dr[x] * 16 + 8);
                short b = (short)(dr[(x + 1) & 3] * 16 + 8);
                short c = (short)(dr[(x + 2) & 3] * 16 + 8);
                short d = (short)(dr[(x + 3) & 3] * 16 + 8);
                dither[y][x] = _mm_setr_epi16(a, b, c, d, a, b, c, d);
            }
    }
};

/* 只读取 span 内的 8 个 alpha/像素; 尾部与非 SSE2 CPU 共用标量公式。 */
template<bool Fog>
WTH_CLOUD_SSE2 static void WthCloudSse2(unsigned short* dst, const unsigned char* alpha,
                                      int n, int x, int y, const WthCloudTone& tone,
                                      const WthCloudSimd& s)
{
    int i = 0;
    const __m128i d = s.dither[y & 3][x & 3];
    for (; i + 8 <= n; i += 8)
    {
        __m128i a = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(alpha + i)), s.zero);
        __m128i transparent = _mm_cmpeq_epi16(a, s.zero);
        if (_mm_movemask_epi8(transparent) == 0xFFFF) continue;
        __m128i m = _mm_sub_epi16(s.full, a);
        __m128i v = _mm_loadu_si128((const __m128i*)(dst + i));
        __m128i r = _mm_and_si128(_mm_srl_epi16(v, s.rshift), s.mask5);
        __m128i g = _mm_and_si128(_mm_srli_epi16(v, 5), s.maskG);
        __m128i b = _mm_and_si128(v, s.mask5);
        r = _mm_add_epi16(_mm_mullo_epi16(r, m), d);
        g = _mm_add_epi16(_mm_mullo_epi16(g, m), d);
        b = _mm_add_epi16(_mm_mullo_epi16(b, m), d);
        if (Fog)
        {
            r = _mm_add_epi16(r, _mm_mullo_epi16(s.target[0], a));
            g = _mm_add_epi16(g, _mm_mullo_epi16(s.target[1], a));
            b = _mm_add_epi16(b, _mm_mullo_epi16(s.target[2], a));
        }
        __m128i out = _mm_or_si128(_mm_sll_epi16(_mm_srli_epi16(r, 8), s.rshift),
            _mm_or_si128(_mm_slli_epi16(_mm_srli_epi16(g, 8), 5), _mm_srli_epi16(b, 8)));
        /* 555 的 bit15 在透明像素上同样保持原样。 */
        out = _mm_or_si128(_mm_and_si128(transparent, v), _mm_andnot_si128(transparent, out));
        _mm_storeu_si128((__m128i*)(dst + i), out);
    }
    WthCloudScalar<Fog>(dst + i, alpha + i, n - i, x + i, y, tone);
}
#endif
