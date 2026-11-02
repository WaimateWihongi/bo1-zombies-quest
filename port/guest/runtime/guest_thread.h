#ifndef GUEST_THREAD_H
#define GUEST_THREAD_H

#include "pthread_impl.h"
#include <setjmp.h>

/* musl's struct pthread followed by the runtime's own per-thread state. The thread pointer is
   the address just past `pt` (musl's TLS_ABOVE_TP layout); nothing uses static TLS, C/C++
   thread-locals go through emulated TLS (-femulated-tls). */
struct guest_thread {
    struct pthread pt;
    void **emutls;
    unsigned emutls_count;
    void *(*start)(void *);
    void *arg;
    unsigned stack_size;
    jmp_buf exit_jmp;
};

#define GUEST_THREAD_TP(g) ((unsigned)(uintptr_t)((char *)&(g)->pt + sizeof(struct pthread)))

static inline struct guest_thread *guest_thread_self(void)
{
    return (struct guest_thread *)__pthread_self();
}

void guest_thread_initialize(struct guest_thread *g);

#endif
