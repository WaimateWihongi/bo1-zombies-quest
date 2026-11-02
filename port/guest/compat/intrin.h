/* MSVC intrinsics for the guest (AArch64). Clang provides most of them itself under
   -fms-extensions on every target (_Interlocked*, _BitScan*, _byteswap_*, _rotl, __debugbreak,
   _ReturnAddress); this adds the rest. SSE comes from sse2neon (sse.h), which maps it onto NEON. */
#pragma once

#include "sse.h"

#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define _ReadWriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _WriteBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define _ReadBarrier() __atomic_signal_fence(__ATOMIC_SEQ_CST)
#define YieldProcessor() __asm__ __volatile__("yield")

#ifdef __cplusplus
extern "C" unsigned long GetCurrentThreadId(void);
#else
unsigned long GetCurrentThreadId(void);
#endif

/* x86 segment reads in decompiled code: fs:[0x24] is the current thread's ID (the TEB) */
static inline unsigned long __readfsdword(unsigned long offset)
{
    if (offset == 0x24) return GetCurrentThreadId();
    __builtin_trap();
}

/* The Interlocked family, type-generic: decompiled code passes unsigned int * and other
   32-bit types where the Windows prototypes say long *, which MSVC let through. */
/* the operand's type without volatile, for the compare-exchange's expected value */
#ifdef __cplusplus
#define BO1_PLAIN_TYPE(p) __remove_cvref(__typeof__(*(p)))
#else
#define BO1_PLAIN_TYPE(p) __typeof__((__typeof__(*(p)))0 + 0)
#endif
#define BO1_CAS(p, exchange, comparand) ({     BO1_PLAIN_TYPE(p) bo1_c_ = (BO1_PLAIN_TYPE(p))(comparand);     __atomic_compare_exchange_n((p), &bo1_c_, (BO1_PLAIN_TYPE(p))(exchange), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);     bo1_c_; })
#define _InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define _InterlockedDecrement(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define _InterlockedExchange(p, v) __atomic_exchange_n((p), (BO1_PLAIN_TYPE(p))(v), __ATOMIC_SEQ_CST)
#define _InterlockedExchangeAdd(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define _InterlockedCompareExchange(p, e, c) BO1_CAS((p), (e), (c))
#define _InterlockedCompareExchange64(p, e, c) BO1_CAS((p), (e), (c))
#define _InterlockedOr(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define _InterlockedAnd(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define _InterlockedXor(p, v) __atomic_fetch_xor((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedIncrement _InterlockedIncrement
#define InterlockedDecrement _InterlockedDecrement
#define InterlockedExchange _InterlockedExchange
#define InterlockedExchangeAdd _InterlockedExchangeAdd
#define InterlockedCompareExchange _InterlockedCompareExchange
#define InterlockedCompareExchange64 _InterlockedCompareExchange64
#define InterlockedCompareExchangePointer(p, e, c) BO1_CAS((p), (e), (c))
#define InterlockedExchangePointer(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr _InterlockedOr
#define InterlockedAnd _InterlockedAnd
#define InterlockedXor _InterlockedXor
#define InterlockedIncrement64 _InterlockedIncrement
#define InterlockedDecrement64 _InterlockedDecrement
#define InterlockedExchangeAdd64 _InterlockedExchangeAdd
#define InterlockedExchange64 _InterlockedExchange

/* a cycle counter: the virtual counter (tens of MHz) stands in for the TSC */
static inline unsigned long long __rdtsc(void)
{
    unsigned long long t;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(t));
    return t;
}
