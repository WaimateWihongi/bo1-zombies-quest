/* Host threads that run guest code. ILP32 code keeps stack addresses in 32-bit registers, so
   every such thread gets a stack mapped below 4 GB. The guest's thread pointer (musl's struct
   pthread) is a host thread-local, read and written through the host_get_tp/host_set_tp imports. */
#define _GNU_SOURCE
#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>

#define GUARD_SIZE (64 * 1024)
#define MIN_GUEST_STACK (1024 * 1024)

static __thread uint32_t guest_tp;

uint32_t host_get_tp(void)
{
    return guest_tp;
}

void host_set_tp(uint32_t tp)
{
    guest_tp = tp;
}

typedef uint64_t (*guest_function)(uint64_t, uint64_t, uint64_t, uint64_t);

static void check_stack(void)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    if (sp >> 32) host_fatal("guest code called on a stack above 4 GB (%#lx)", (unsigned long)sp);
}

/* Starts a guest thread's own entry (__guest_start, __guest_thread_start), which sets up its
   thread pointer itself. */
void host_enter_guest(uint32_t entry, uint32_t arg)
{
    check_stack();
    ((guest_function)(uintptr_t)entry)(arg, 0, 0, 0);
}

/* Calls guest code from a host thread; one the guest did not create gets a guest thread first. */
uint64_t host_call_guest(uint32_t function, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    check_stack();
    if (!guest_tp) ((guest_function)(uintptr_t)host_image.header->thread_attach)(0, 0, 0, 0);
    return ((guest_function)(uintptr_t)function)(a, b, c, d);
}

struct native_thread {
    void (*entry)(void *);
    void *arg;
    void *stack;
    size_t stack_size;
    pthread_t thread;
    struct native_thread *next;
};

/* Finished threads' stacks are freed by a reaper that joins them, as a thread cannot unmap
   the stack it runs on. */
static pthread_mutex_t reap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reap_cond = PTHREAD_COND_INITIALIZER;
static struct native_thread *reap_queue;

static void *reaper(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&reap_lock);
        while (!reap_queue) pthread_cond_wait(&reap_cond, &reap_lock);
        struct native_thread *t = reap_queue;
        reap_queue = t->next;
        pthread_mutex_unlock(&reap_lock);
        pthread_join(t->thread, NULL);
        host_low_unmap(t->stack, t->stack_size);
        free(t);
    }
    return NULL;
}

static void *native_thread_main(void *p)
{
    struct native_thread *t = p;
    t->entry(t->arg);
    pthread_mutex_lock(&reap_lock);
    t->next = reap_queue;
    reap_queue = t;
    pthread_cond_signal(&reap_cond);
    pthread_mutex_unlock(&reap_lock);
    return NULL;
}

void host_thread_initialize(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, reaper, NULL);
    pthread_detach(thread);
}

int host_native_thread_create(void (*entry)(void *), void *arg, size_t stack_size)
{
    stack_size = (stack_size + 0xffff) & ~(size_t)0xffff;
    struct native_thread *t = calloc(1, sizeof *t);
    if (!t) return ENOMEM;
    t->entry = entry;
    t->arg = arg;
    t->stack_size = stack_size + GUARD_SIZE;
    t->stack = host_low_map(t->stack_size, PROT_READ | PROT_WRITE);
    if (!t->stack) {
        free(t);
        return ENOMEM;
    }
    mprotect(t->stack, GUARD_SIZE, PROT_NONE);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, (char *)t->stack + GUARD_SIZE, stack_size);
    int r = pthread_create(&t->thread, &attr, native_thread_main, t);
    pthread_attr_destroy(&attr);
    if (r) {
        host_low_unmap(t->stack, t->stack_size);
        free(t);
    }
    return r;
}

static void guest_thread_entry(void *guest_thread)
{
    host_enter_guest(host_image.header->thread_start, (uint32_t)(uintptr_t)guest_thread);
}

/* import: the guest's pthread_create */
int host_thread_create(void *guest_thread, uint32_t stack_size)
{
    if (stack_size < MIN_GUEST_STACK) stack_size = MIN_GUEST_STACK;
    return host_native_thread_create(guest_thread_entry, guest_thread, stack_size);
}
