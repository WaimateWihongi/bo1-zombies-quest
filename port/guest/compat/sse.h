/* SSE on NEON. sse2neon defines __int64 as a macro for compilers without it, which breaks
   `unsigned __int64` here: clang has the keyword under -fms-extensions. */
#pragma once
#include "sse2neon.h"
#undef __int64

/* MMX operations the model skinning code uses that sse2neon leaves out, with the x86 results */
static inline void _m_empty(void) {}
static inline __m64 _mm_setzero_si64(void) { return vdup_n_s64(0); }
static inline __m64 _mm_cvtsi32_si64(int a) { return vcreate_s64((uint64_t)(uint32_t)a); }
/* interleave the low four bytes / two words of a and b */
static inline __m64 _m_punpcklbw(__m64 a, __m64 b)
{
    return vreinterpret_s64_u8(vzip1_u8(vreinterpret_u8_s64(a), vreinterpret_u8_s64(b)));
}
static inline __m64 _m_punpcklwd(__m64 a, __m64 b)
{
    return vreinterpret_s64_u16(vzip1_u16(vreinterpret_u16_s64(a), vreinterpret_u16_s64(b)));
}
/* four signed words of a, then four of b, to unsigned bytes with saturation */
static inline __m64 _m_packuswb(__m64 a, __m64 b)
{
    return vreinterpret_s64_u8(vqmovun_s16(vcombine_s16(vreinterpret_s16_s64(a), vreinterpret_s16_s64(b))));
}
