/* Compiler builtins: correct for 32-bit pointers without hand-written exclusive pairs. */
#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
	__atomic_compare_exchange_n(p, &t, s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return t;
}

#define a_cas_p a_cas_p
static inline void *a_cas_p(volatile void *p, void *t, void *s)
{
	__atomic_compare_exchange_n((void *volatile *)p, &t, s, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return t;
}

#define a_swap a_swap
static inline int a_swap(volatile int *p, int v)
{
	return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

#define a_fetch_add a_fetch_add
static inline int a_fetch_add(volatile int *p, int v)
{
	return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

#define a_fetch_and a_fetch_and
static inline int a_fetch_and(volatile int *p, int v)
{
	return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST);
}

#define a_fetch_or a_fetch_or
static inline int a_fetch_or(volatile int *p, int v)
{
	return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST);
}

#define a_store a_store
static inline void a_store(volatile int *p, int v)
{
	__atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

#define a_barrier a_barrier
static inline void a_barrier()
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#define a_crash a_crash
static inline void a_crash()
{
	__builtin_trap();
}

#define a_ctz_64 a_ctz_64
static inline int a_ctz_64(unsigned long long x)
{
	return __builtin_ctzll(x);
}

#define a_clz_64 a_clz_64
static inline int a_clz_64(unsigned long long x)
{
	return __builtin_clzll(x);
}

#define a_ctz_32 a_ctz_32
static inline int a_ctz_32(unsigned x)
{
	return __builtin_ctz(x);
}

#define a_clz_32 a_clz_32
static inline int a_clz_32(unsigned x)
{
	return __builtin_clz(x);
}
