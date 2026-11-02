/* Threads for the guest: replaces musl's pthread_create.c and __init_tls.c.
 *
 * The host owns the real (bionic) threads and their stacks, which it maps below 4 GB. A guest
 * thread is a calloc'd struct guest_thread; the host runs __guest_thread_start on a new thread,
 * and pthread_exit unwinds back there with longjmp instead of calling SYS_exit. The thread-exit
 * bookkeeping follows musl's own __pthread_exit (MIT, see musl/COPYRIGHT.musl). */
#include "guest_thread.h"
#include "guest_host.h"
#include "lock.h"
#include "stdio_impl.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

volatile int __thread_list_lock;
static int tl_lock_count;
static int tl_lock_waiters;

static void init_file_lock(FILE *f)
{
    if (f && f->lock < 0) f->lock = 0;
}

void __pthread_tsd_run_dtors(void);
void __do_orphaned_stdio_locks(void);

void __tl_lock(void)
{
    int tid = __pthread_self()->tid;
    int val = __thread_list_lock;
    if (val == tid) {
        tl_lock_count++;
        return;
    }
    while ((val = a_cas(&__thread_list_lock, 0, tid)))
        __wait(&__thread_list_lock, &tl_lock_waiters, val, 0);
}

void __tl_unlock(void)
{
    if (tl_lock_count) {
        tl_lock_count--;
        return;
    }
    a_store(&__thread_list_lock, 0);
    if (tl_lock_waiters) __wake(&__thread_list_lock, 1, 0);
}

void __tl_sync(pthread_t td)
{
    a_barrier();
    int val = __thread_list_lock;
    if (!val) return;
    __wait(&__thread_list_lock, &tl_lock_waiters, val, 0);
    if (tl_lock_waiters) __wake(&__thread_list_lock, 1, 0);
}

void __do_cleanup_push(struct __ptcb *cb)
{
    struct pthread *self = __pthread_self();
    cb->__next = self->cancelbuf;
    self->cancelbuf = cb;
}

void __do_cleanup_pop(struct __ptcb *cb)
{
    __pthread_self()->cancelbuf = cb->__next;
}

/* Fills in a thread's musl fields and makes it the calling host thread's guest thread. */
void guest_thread_initialize(struct guest_thread *g)
{
    struct pthread *t = &g->pt;
    t->self = t;
    t->locale = &libc.global_locale;
    t->robust_list.head = &t->robust_list.head;
    if (!t->next) t->next = t->prev = t;
    host_set_tp(GUEST_THREAD_TP(g));
    t->tid = __syscall(SYS_gettid);
}

_Noreturn void __pthread_exit(void *result)
{
    struct guest_thread *g = guest_thread_self();
    pthread_t self = &g->pt;

    self->canceldisable = 1;
    self->cancelasync = 0;
    self->result = result;

    while (self->cancelbuf) {
        void (*f)(void *) = self->cancelbuf->__f;
        void *x = self->cancelbuf->__x;
        self->cancelbuf = self->cancelbuf->__next;
        f(x);
    }

    __pthread_tsd_run_dtors();

    int state = a_cas(&self->detach_state, DT_JOINABLE, DT_EXITING);

    LOCK(self->killlock);
    __tl_lock();
    if (self->next == self) {
        /* the last thread: process exit, with atexit handlers */
        __tl_unlock();
        UNLOCK(self->killlock);
        self->detach_state = state;
        exit(0);
    }
    self->tid = 0;
    UNLOCK(self->killlock);

    /* hand back robust mutexes the thread still holds */
    volatile void *volatile *rp;
    while ((rp = self->robust_list.head) && rp != &self->robust_list.head) {
        pthread_mutex_t *m = (void *)((char *)rp - offsetof(pthread_mutex_t, _m_next));
        int waiters = m->_m_waiters;
        int priv = (m->_m_type & 128) ^ 128;
        self->robust_list.pending = rp;
        self->robust_list.head = *rp;
        int cont = a_swap(&m->_m_lock, 0x40000000);
        self->robust_list.pending = 0;
        if (cont < 0 || waiters) __wake(&m->_m_lock, 1, priv);
    }

    __do_orphaned_stdio_locks();

    if (!--libc.threads_minus_1) libc.need_locks = -1;
    self->next->prev = self->prev;
    self->prev->next = self->next;
    self->prev = self->next = self;
    __tl_unlock();

    /* A detached thread's struct is freed by nobody: the joiner of a joinable one frees
       nothing either (map_base is 0), so both leak a few hundred bytes. The engine makes its
       threads once at startup. */
    a_store(&self->detach_state, DT_EXITED);
    __wake(&self->detach_state, 1, 1);

    longjmp(g->exit_jmp, 1);
}

/* Called by the host on the new thread's own stack. Returning ends the host thread. */
void __guest_thread_start(struct guest_thread *g)
{
    guest_thread_initialize(g);
    if (!setjmp(g->exit_jmp)) __pthread_exit(g->start(g->arg));
    host_set_tp(0);
}

int __pthread_create(pthread_t *restrict res, const pthread_attr_t *restrict attrp,
                     void *(*entry)(void *), void *restrict arg)
{
    struct guest_thread *g = calloc(1, sizeof *g);
    void **tsd = calloc(PTHREAD_KEYS_MAX, sizeof(void *));
    if (!g || !tsd) {
        free(g);
        free(tsd);
        return EAGAIN;
    }
    g->start = entry;
    g->arg = arg;
    g->stack_size = attrp ? attrp->_a_stacksize : 0;
    g->pt.tsd = tsd;
    g->pt.locale = &libc.global_locale;
    g->pt.detach_state = attrp && attrp->_a_detach ? DT_DETACHED : DT_JOINABLE;

    if (!libc.threaded) {
        for (FILE *f = *__ofl_lock(); f; f = f->next) init_file_lock(f);
        __ofl_unlock();
        init_file_lock(__stdin_used);
        init_file_lock(__stdout_used);
        init_file_lock(__stderr_used);
        libc.threaded = 1;
    }
    libc.need_locks = 1;

    struct pthread *self = __pthread_self();
    __tl_lock();
    libc.threads_minus_1++;
    g->pt.next = self->next;
    g->pt.prev = self;
    g->pt.next->prev = &g->pt;
    g->pt.prev->next = &g->pt;
    __tl_unlock();

    int r = host_thread_create(g, g->stack_size);
    if (r) {
        __tl_lock();
        g->pt.next->prev = g->pt.prev;
        g->pt.prev->next = g->pt.next;
        if (!--libc.threads_minus_1) libc.need_locks = 0;
        __tl_unlock();
        free(tsd);
        free(g);
        return r;
    }
    *res = &g->pt;
    return 0;
}

weak_alias(__pthread_exit, pthread_exit);
weak_alias(__pthread_create, pthread_create);

/* Host threads that call into the guest (audio callbacks...) get a guest thread from this
   pool on their first call; it cannot use malloc, which already needs a thread pointer. */
extern struct guest_thread *guest_main_thread;
static struct guest_thread attach_pool[16];
static void *attach_tsd[16][PTHREAD_KEYS_MAX];
static int attach_used;

void __guest_thread_attach(void)
{
    int i = a_fetch_add(&attach_used, 1);
    if (i >= 16) host_abort("too many host threads attached to the guest");
    struct guest_thread *g = &attach_pool[i];
    g->pt.tsd = attach_tsd[i];
    g->pt.detach_state = DT_DETACHED;
    guest_thread_initialize(g);

    libc.threaded = 1;
    libc.need_locks = 1;
    __tl_lock();
    libc.threads_minus_1++;
    g->pt.next = guest_main_thread->pt.next;
    g->pt.prev = &guest_main_thread->pt;
    g->pt.next->prev = &g->pt;
    g->pt.prev->next = &g->pt;
    __tl_unlock();
}

/* -femulated-tls: compiler-rt's control block layout. */
struct __emutls_control {
    size_t size;
    size_t align;
    union {
        uintptr_t index;
        void *address;
    } object;
    void *value;
};

static volatile int emutls_lock[1];
static uintptr_t emutls_next_index;

void *__emutls_get_address(struct __emutls_control *control)
{
    uintptr_t index = __atomic_load_n(&control->object.index, __ATOMIC_ACQUIRE);
    if (!index) {
        LOCK(emutls_lock);
        index = control->object.index;
        if (!index) {
            index = ++emutls_next_index;
            __atomic_store_n(&control->object.index, index, __ATOMIC_RELEASE);
        }
        UNLOCK(emutls_lock);
    }
    struct guest_thread *g = guest_thread_self();
    if (index > g->emutls_count) {
        unsigned count = index + 16;
        void **slots = realloc(g->emutls, count * sizeof(void *));
        if (!slots) host_abort("emulated TLS: out of memory");
        memset(slots + g->emutls_count, 0, (count - g->emutls_count) * sizeof(void *));
        g->emutls = slots;
        g->emutls_count = count;
    }
    void *object = g->emutls[index - 1];
    if (!object) {
        size_t align = control->align < sizeof(void *) ? sizeof(void *) : control->align;
        object = aligned_alloc(align, (control->size + align - 1) & ~(align - 1));
        if (!object) host_abort("emulated TLS: out of memory");
        if (control->value) memcpy(object, control->value, control->size);
        else memset(object, 0, control->size);
        g->emutls[index - 1] = object;
    }
    return object;
}
