/* The kernel32 part of the Win32 API the engine calls, on musl.
 *
 * HANDLEs point at objects below (first word a kind tag) or are Windows' pseudo handles: -1 for the
 * current process, -2 for the current thread. Waitable objects (events, mutexes, semaphores,
 * threads) each carry a pthread mutex and condition variable. Paths need no care here: the host
 * turns '\' into '/' and matches case for every path syscall (port/host/host_path.c). */
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "win32_internal.h"

/* ---- errors ---------------------------------------------------------------------------------- */

static thread_local DWORD t_last_error;

DWORD GetLastError(void) { return t_last_error; }
void SetLastError(DWORD error) { t_last_error = error; }

static DWORD error_from_errno(int e)
{
    switch (e) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    default: return 31; /* ERROR_GEN_FAILURE */
    }
}

static BOOL fail_errno(void)
{
    t_last_error = error_from_errno(errno);
    return FALSE;
}

/* ---- handles --------------------------------------------------------------------------------- */

enum Kind : uint32_t {
    KIND_FILE = 0x454c4946,   /* 'FILE' */
    KIND_EVENT = 0x544e5645,  /* 'EVNT' */
    KIND_MUTEX = 0x5854554d,  /* 'MUTX' */
    KIND_SEMA = 0x414d4553,   /* 'SEMA' */
    KIND_THREAD = 0x44524854, /* 'THRD' */
    KIND_FIND = 0x444e4946,   /* 'FIND' */
    KIND_SNAP = 0x50414e53,   /* 'SNAP' */
};

struct Object {
    Kind kind;
    int refs; /* handles plus, for threads, the running thread itself */
};

struct Waitable : Object {
    pthread_mutex_t lock;
    pthread_cond_t changed;
};

struct FileObject : Object {
    int fd;
};

struct EventObject : Waitable {
    bool manual, signaled;
    unsigned pulses; /* PulseEvent wakes the waiters present now without leaving it signaled */
};

struct MutexObject : Waitable {
    pthread_t owner;
    int depth;
};

struct SemaphoreObject : Waitable {
    LONG count, maximum;
};

struct ThreadObject : Waitable {
    pthread_t thread;
    DWORD id;
    LPTHREAD_START_ROUTINE start;
    void *arg;
    bool suspended, done, attached; /* attached: a thread the layer didn't start (main, host threads) */
    DWORD exit_code;
};

struct FindObject : Object {
    DIR *dir;
    char directory[MAX_PATH];
    char pattern[MAX_PATH];
};

static pthread_mutex_t g_refs_lock = PTHREAD_MUTEX_INITIALIZER;

#define PSEUDO_PROCESS ((HANDLE)(intptr_t)-1)
#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)

static Object *object(HANDLE h, Kind kind)
{
    if (!h || h == PSEUDO_PROCESS || h == PSEUDO_THREAD) return NULL;
    Object *o = (Object *)h;
    return o->kind == kind ? o : NULL;
}

static void init_waitable(Waitable *w, Kind kind)
{
    w->kind = kind;
    w->refs = 1;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->changed, NULL);
}

static void destroy(Object *o)
{
    switch (o->kind) {
    case KIND_FILE:
        close(((FileObject *)o)->fd);
        delete (FileObject *)o;
        break;
    case KIND_EVENT:
    case KIND_MUTEX:
    case KIND_SEMA:
    case KIND_THREAD: {
        Waitable *w = (Waitable *)o;
        pthread_mutex_destroy(&w->lock);
        pthread_cond_destroy(&w->changed);
        if (o->kind == KIND_EVENT) delete (EventObject *)o;
        else if (o->kind == KIND_MUTEX) delete (MutexObject *)o;
        else if (o->kind == KIND_SEMA) delete (SemaphoreObject *)o;
        else delete (ThreadObject *)o;
        break;
    }
    case KIND_FIND:
        if (((FindObject *)o)->dir) closedir(((FindObject *)o)->dir);
        delete (FindObject *)o;
        break;
    case KIND_SNAP:
        delete o;
        break;
    }
}

static void release(Object *o)
{
    pthread_mutex_lock(&g_refs_lock);
    int left = --o->refs;
    pthread_mutex_unlock(&g_refs_lock);
    if (!left) destroy(o);
}

BOOL CloseHandle(HANDLE h)
{
    if (!h || h == INVALID_HANDLE_VALUE || h == PSEUDO_THREAD) return h == PSEUDO_THREAD;
    release((Object *)h);
    return TRUE;
}

/* ---- waiting --------------------------------------------------------------------------------- */

static void deadline(DWORD ms, struct timespec *at)
{
    clock_gettime(CLOCK_REALTIME, at);
    at->tv_sec += ms / 1000;
    at->tv_nsec += (long)(ms % 1000) * 1000000;
    if (at->tv_nsec >= 1000000000) {
        at->tv_sec++;
        at->tv_nsec -= 1000000000;
    }
}

/* waits on w->changed until ready(w) holds or the time runs out; w->lock is held throughout */
template <typename Ready> static DWORD wait_until(Waitable *w, DWORD ms, Ready ready)
{
    struct timespec at;
    if (ms != INFINITE && ms) deadline(ms, &at);
    while (!ready()) {
        if (!ms) return WAIT_TIMEOUT;
        int r = ms == INFINITE ? pthread_cond_wait(&w->changed, &w->lock)
                               : pthread_cond_timedwait(&w->changed, &w->lock, &at);
        if (r == ETIMEDOUT) return ready() ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }
    return WAIT_OBJECT_0;
}

static void run_apcs(void);

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (Object *o = object(h, KIND_EVENT)) {
        EventObject *e = (EventObject *)o;
        pthread_mutex_lock(&e->lock);
        unsigned pulses = e->pulses;
        DWORD r = wait_until(e, ms, [&] { return e->signaled || e->pulses != pulses; });
        if (r == WAIT_OBJECT_0 && !e->manual) e->signaled = false;
        pthread_mutex_unlock(&e->lock);
        return r;
    }
    if (Object *o = object(h, KIND_MUTEX)) {
        MutexObject *m = (MutexObject *)o;
        pthread_t self = pthread_self();
        pthread_mutex_lock(&m->lock);
        DWORD r = wait_until(m, ms, [&] { return !m->depth || pthread_equal(m->owner, self); });
        if (r == WAIT_OBJECT_0) {
            m->owner = self;
            m->depth++;
        }
        pthread_mutex_unlock(&m->lock);
        return r;
    }
    if (Object *o = object(h, KIND_SEMA)) {
        SemaphoreObject *s = (SemaphoreObject *)o;
        pthread_mutex_lock(&s->lock);
        DWORD r = wait_until(s, ms, [&] { return s->count > 0; });
        if (r == WAIT_OBJECT_0) s->count--;
        pthread_mutex_unlock(&s->lock);
        return r;
    }
    if (Object *o = object(h, KIND_THREAD)) {
        ThreadObject *t = (ThreadObject *)o;
        pthread_mutex_lock(&t->lock);
        DWORD r = wait_until(t, ms, [&] { return t->done; });
        pthread_mutex_unlock(&t->lock);
        return r;
    }
    t_last_error = 6; /* ERROR_INVALID_HANDLE */
    return WAIT_FAILED;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL all, DWORD ms)
{
    /* polling: the engine waits on several objects only off the hot path */
    DWORD start = GetTickCount();
    for (;;) {
        DWORD ready = 0;
        for (DWORD i = 0; i < count; i++) {
            if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) {
                if (!all) return WAIT_OBJECT_0 + i;
                ready++;
            }
        }
        if (all && ready == count) return WAIT_OBJECT_0;
        if (ms != INFINITE && GetTickCount() - start >= ms) return WAIT_TIMEOUT;
        usleep(500);
    }
}

/* ---- events, mutexes, semaphores ------------------------------------------------------------- */

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES, BOOL manual_reset, BOOL initial, LPCSTR)
{
    EventObject *e = new EventObject();
    init_waitable(e, KIND_EVENT);
    e->manual = manual_reset;
    e->signaled = initial;
    return e;
}

static EventObject *event(HANDLE h) { return (EventObject *)object(h, KIND_EVENT); }

BOOL SetEvent(HANDLE h)
{
    EventObject *e = event(h);
    if (!e) return FALSE;
    pthread_mutex_lock(&e->lock);
    e->signaled = true;
    pthread_cond_broadcast(&e->changed);
    pthread_mutex_unlock(&e->lock);
    return TRUE;
}

BOOL ResetEvent(HANDLE h)
{
    EventObject *e = event(h);
    if (!e) return FALSE;
    pthread_mutex_lock(&e->lock);
    e->signaled = false;
    pthread_mutex_unlock(&e->lock);
    return TRUE;
}

BOOL PulseEvent(HANDLE h)
{
    EventObject *e = event(h);
    if (!e) return FALSE;
    pthread_mutex_lock(&e->lock);
    e->pulses++;
    e->signaled = false;
    pthread_cond_broadcast(&e->changed);
    pthread_mutex_unlock(&e->lock);
    return TRUE;
}

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL owned, LPCSTR)
{
    MutexObject *m = new MutexObject();
    init_waitable(m, KIND_MUTEX);
    if (owned) {
        m->owner = pthread_self();
        m->depth = 1;
    }
    return m;
}

BOOL ReleaseMutex(HANDLE h)
{
    MutexObject *m = (MutexObject *)object(h, KIND_MUTEX);
    if (!m) return FALSE;
    pthread_mutex_lock(&m->lock);
    BOOL ok = m->depth && pthread_equal(m->owner, pthread_self());
    if (ok && !--m->depth) pthread_cond_broadcast(&m->changed);
    pthread_mutex_unlock(&m->lock);
    return ok;
}

HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES, LONG initial, LONG maximum, LPCSTR)
{
    SemaphoreObject *s = new SemaphoreObject();
    init_waitable(s, KIND_SEMA);
    s->count = initial;
    s->maximum = maximum;
    return s;
}

BOOL ReleaseSemaphore(HANDLE h, LONG count, LPLONG previous)
{
    SemaphoreObject *s = (SemaphoreObject *)object(h, KIND_SEMA);
    if (!s) return FALSE;
    pthread_mutex_lock(&s->lock);
    if (previous) *previous = s->count;
    BOOL ok = s->count + count <= s->maximum;
    if (ok) {
        s->count += count;
        pthread_cond_broadcast(&s->changed);
    }
    pthread_mutex_unlock(&s->lock);
    return ok;
}

/* ---- critical sections and SRW locks --------------------------------------------------------- */

/* The CRITICAL_SECTION is 24 bytes on ILP32, exactly a musl pthread_mutex_t: the mutex lives in it. */
static_assert(sizeof(pthread_mutex_t) <= sizeof(CRITICAL_SECTION), "CRITICAL_SECTION holds a mutex");

void InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init((pthread_mutex_t *)cs, &attr);
    pthread_mutexattr_destroy(&attr);
}

BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD)
{
    InitializeCriticalSection(cs);
    return TRUE;
}

void DeleteCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_destroy((pthread_mutex_t *)cs); }
void EnterCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_lock((pthread_mutex_t *)cs); }
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) { return !pthread_mutex_trylock((pthread_mutex_t *)cs); }
void LeaveCriticalSection(LPCRITICAL_SECTION cs) { pthread_mutex_unlock((pthread_mutex_t *)cs); }

/* An SRWLOCK is one pointer-sized word that starts as 0 (SRWLOCK_INIT): its rwlock is made on first use. */
static pthread_rwlock_t *srw(PSRWLOCK lock)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)__atomic_load_n(&lock->Ptr, __ATOMIC_ACQUIRE);
    if (rw) return rw;
    pthread_rwlock_t *fresh = (pthread_rwlock_t *)malloc(sizeof *fresh);
    pthread_rwlock_init(fresh, NULL);
    void *expected = NULL;
    if (__atomic_compare_exchange_n(&lock->Ptr, &expected, (void *)fresh, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return fresh;
    pthread_rwlock_destroy(fresh);
    free(fresh);
    return (pthread_rwlock_t *)expected;
}

void InitializeSRWLock(PSRWLOCK lock) { lock->Ptr = NULL; }
void AcquireSRWLockExclusive(PSRWLOCK lock) { pthread_rwlock_wrlock(srw(lock)); }
void ReleaseSRWLockExclusive(PSRWLOCK lock) { pthread_rwlock_unlock(srw(lock)); }
void AcquireSRWLockShared(PSRWLOCK lock) { pthread_rwlock_rdlock(srw(lock)); }
void ReleaseSRWLockShared(PSRWLOCK lock) { pthread_rwlock_unlock(srw(lock)); }
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock) { return !pthread_rwlock_trywrlock(srw(lock)); }

/* ---- threads --------------------------------------------------------------------------------- */

static thread_local ThreadObject *t_self;

static void *thread_main(void *p)
{
    ThreadObject *t = (ThreadObject *)p;
    t_self = t;
    pthread_mutex_lock(&t->lock);
    while (t->suspended) pthread_cond_wait(&t->changed, &t->lock);
    pthread_mutex_unlock(&t->lock);
    DWORD code = t->start(t->arg);
    pthread_mutex_lock(&t->lock);
    t->done = true;
    t->exit_code = code;
    pthread_cond_broadcast(&t->changed);
    pthread_mutex_unlock(&t->lock);
    release(t); /* the running thread's reference */
    return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID arg, DWORD flags,
                    LPDWORD id)
{
    ThreadObject *t = new ThreadObject();
    init_waitable(t, KIND_THREAD);
    t->refs = 2; /* the handle and the thread */
    t->start = start;
    t->arg = arg;
    t->suspended = flags & CREATE_SUSPENDED;
    t->exit_code = 259; /* STILL_ACTIVE */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    /* Windows threads get 1 MB unless asked; the engine's workers ask for less than ARM code needs */
    size_t size = stack < 256 * 1024 ? 1024 * 1024 : (stack + 65535) & ~(size_t)65535;
    pthread_attr_setstacksize(&attr, size);
    int r = pthread_create(&t->thread, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (r) {
        delete t;
        t_last_error = 8; /* ERROR_NOT_ENOUGH_MEMORY */
        return NULL;
    }
    t->id = (DWORD)(uintptr_t)t->thread;
    if (id) *id = t->id;
    return t;
}

DWORD ResumeThread(HANDLE h)
{
    ThreadObject *t = (ThreadObject *)object(h, KIND_THREAD);
    if (!t) return 0;
    pthread_mutex_lock(&t->lock);
    DWORD was = t->suspended;
    t->suspended = false;
    pthread_cond_broadcast(&t->changed);
    pthread_mutex_unlock(&t->lock);
    return was;
}

/* POSIX can't stop another thread from outside; only the hang detector asks */
DWORD SuspendThread(HANDLE) { return (DWORD)-1; }

BOOL GetExitCodeThread(HANDLE h, LPDWORD code)
{
    ThreadObject *t = (ThreadObject *)object(h, KIND_THREAD);
    if (!t) return FALSE;
    pthread_mutex_lock(&t->lock);
    *code = t->exit_code;
    pthread_mutex_unlock(&t->lock);
    return TRUE;
}

/* Priorities and affinities: the Quest's scheduler decides. The engine's choices are kept for
   GetThreadPriority. */
BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
int GetThreadPriority(HANDLE) { return THREAD_PRIORITY_NORMAL; }
DWORD_PTR SetThreadAffinityMask(HANDLE, DWORD_PTR) { return 0xff; }
DWORD SetThreadIdealProcessor(HANDLE, DWORD) { return 0; }
DWORD GetPriorityClass(HANDLE) { return NORMAL_PRIORITY_CLASS; }
BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }

HANDLE GetCurrentThread(void) { return PSEUDO_THREAD; }
DWORD GetCurrentThreadId(void) { return (DWORD)(uintptr_t)pthread_self(); }
HANDLE GetCurrentProcess(void) { return PSEUDO_PROCESS; }
DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }

BOOL SwitchToThread(void)
{
    sched_yield();
    return TRUE;
}

/* DuplicateHandle's main use: a real handle for the current thread (Sys_InitMainThread) */
BOOL DuplicateHandle(HANDLE, HANDLE source, HANDLE, HANDLE *target, DWORD, BOOL, DWORD)
{
    if (source == PSEUDO_THREAD) {
        if (!t_self) {
            ThreadObject *t = new ThreadObject();
            init_waitable(t, KIND_THREAD);
            t->thread = pthread_self();
            t->id = GetCurrentThreadId();
            t->attached = true;
            t->exit_code = 259;
            t_self = t; /* this reference lives as long as the thread */
        }
        pthread_mutex_lock(&g_refs_lock);
        t_self->refs++;
        pthread_mutex_unlock(&g_refs_lock);
        *target = t_self;
        return TRUE;
    }
    if (!source || source == PSEUDO_PROCESS) {
        *target = source;
        return TRUE;
    }
    pthread_mutex_lock(&g_refs_lock);
    ((Object *)source)->refs++;
    pthread_mutex_unlock(&g_refs_lock);
    *target = source;
    return TRUE;
}

HANDLE OpenThread(DWORD, BOOL, DWORD) { return NULL; }
HANDLE OpenProcess(DWORD, BOOL, DWORD id) { return id == (DWORD)getpid() ? PSEUDO_PROCESS : NULL; }
BOOL GetThreadContext(HANDLE, CONTEXT *) { return FALSE; }

/* ---- asynchronous procedure calls (ReadFileEx completions) ----------------------------------- */

struct Apc {
    LPOVERLAPPED_COMPLETION_ROUTINE routine;
    DWORD error, transferred;
    LPOVERLAPPED overlapped;
    Apc *next;
};
static thread_local Apc *t_apcs, **t_apcs_tail = &t_apcs;

static void queue_apc(LPOVERLAPPED_COMPLETION_ROUTINE routine, DWORD error, DWORD transferred, LPOVERLAPPED o)
{
    Apc *a = new Apc{routine, error, transferred, o, NULL};
    if (!t_apcs) t_apcs_tail = &t_apcs;
    *t_apcs_tail = a;
    t_apcs_tail = &a->next;
}

static void run_apcs(void)
{
    while (Apc *a = t_apcs) {
        t_apcs = a->next;
        if (!t_apcs) t_apcs_tail = &t_apcs;
        a->routine(a->error, a->transferred, a->overlapped);
        delete a;
    }
}

#define WAIT_IO_COMPLETION 0xC0u

DWORD SleepEx(DWORD ms, BOOL alertable)
{
    if (alertable && t_apcs) {
        run_apcs();
        return WAIT_IO_COMPLETION;
    }
    if (ms == INFINITE) {
        /* nothing will ever queue an APC to this thread from elsewhere: the reads complete inline */
        bo1_log("SleepEx(INFINITE) with nothing queued");
        for (;;) sleep(3600);
    }
    Sleep(ms);
    return 0;
}

/* ---- time ------------------------------------------------------------------------------------ */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void Sleep(DWORD ms)
{
    if (!ms) {
        sched_yield();
        return;
    }
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

DWORD GetTickCount(void) { return (DWORD)(monotonic_ns() / 1000000); }
ULONGLONG GetTickCount64(void) { return monotonic_ns() / 1000000; }
DWORD timeGetTime(void) { return GetTickCount(); }
UINT timeBeginPeriod(UINT) { return 0; }
UINT timeEndPeriod(UINT) { return 0; }

BOOL QueryPerformanceCounter(LARGE_INTEGER *count)
{
    count->QuadPart = (LONGLONG)monotonic_ns();
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER *frequency)
{
    frequency->QuadPart = 1000000000;
    return TRUE;
}

/* FILETIME: 100 ns units since 1601 */
#define UNIX_EPOCH_IN_FILETIME 116444736000000000ull

static void to_filetime(uint64_t t100ns, LPFILETIME ft)
{
    ft->dwLowDateTime = (DWORD)t100ns;
    ft->dwHighDateTime = (DWORD)(t100ns >> 32);
}

static uint64_t from_filetime(const FILETIME *ft) { return (uint64_t)ft->dwHighDateTime << 32 | ft->dwLowDateTime; }

static void timespec_to_filetime(const struct timespec *ts, LPFILETIME ft)
{
    to_filetime(UNIX_EPOCH_IN_FILETIME + (uint64_t)ts->tv_sec * 10000000ull + (uint64_t)ts->tv_nsec / 100, ft);
}

void GetSystemTimeAsFileTime(LPFILETIME ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    timespec_to_filetime(&ts, ft);
}

static void fill_systemtime(const struct tm *tm, long ms, LPSYSTEMTIME st)
{
    st->wYear = (WORD)(tm->tm_year + 1900);
    st->wMonth = (WORD)(tm->tm_mon + 1);
    st->wDayOfWeek = (WORD)tm->tm_wday;
    st->wDay = (WORD)tm->tm_mday;
    st->wHour = (WORD)tm->tm_hour;
    st->wMinute = (WORD)tm->tm_min;
    st->wSecond = (WORD)tm->tm_sec;
    st->wMilliseconds = (WORD)ms;
}

void GetSystemTime(LPSYSTEMTIME st)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    fill_systemtime(&tm, ts.tv_nsec / 1000000, st);
}

void GetLocalTime(LPSYSTEMTIME st)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    fill_systemtime(&tm, ts.tv_nsec / 1000000, st);
}

BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    struct tm tm = {};
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min = st->wMinute;
    tm.tm_sec = st->wSecond;
    struct timespec ts = {timegm(&tm), (long)st->wMilliseconds * 1000000};
    timespec_to_filetime(&ts, ft);
    return TRUE;
}

LONG CompareFileTime(const FILETIME *a, const FILETIME *b)
{
    uint64_t x = from_filetime(a), y = from_filetime(b);
    return x < y ? -1 : x > y;
}

static void cpu_times(clockid_t clock, LPFILETIME kernel, LPFILETIME user)
{
    struct timespec ts = {};
    clock_gettime(clock, &ts);
    to_filetime(0, kernel);
    to_filetime((uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100, user);
}

BOOL GetThreadTimes(HANDLE h, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user)
{
    if (h != PSEUDO_THREAD) return FALSE;
    to_filetime(0, creation);
    to_filetime(0, exit);
    cpu_times(CLOCK_THREAD_CPUTIME_ID, kernel, user);
    return TRUE;
}

BOOL GetProcessTimes(HANDLE, LPFILETIME creation, LPFILETIME exit, LPFILETIME kernel, LPFILETIME user)
{
    to_filetime(0, creation);
    to_filetime(0, exit);
    cpu_times(CLOCK_PROCESS_CPUTIME_ID, kernel, user);
    return TRUE;
}

BOOL GetSystemTimes(LPFILETIME idle, LPFILETIME kernel, LPFILETIME user)
{
    to_filetime(0, idle);
    to_filetime(0, kernel);
    to_filetime(0, user);
    return FALSE;
}

BOOL QueryThreadCycleTime(HANDLE, ULONG64 *cycles)
{
    *cycles = monotonic_ns();
    return TRUE;
}

BOOL QueryProcessCycleTime(HANDLE, ULONG64 *cycles)
{
    *cycles = monotonic_ns();
    return TRUE;
}

/* ---- files ----------------------------------------------------------------------------------- */

static int file_fd(HANDLE h)
{
    FileObject *f = (FileObject *)object(h, KIND_FILE);
    return f ? f->fd : -1;
}

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD disposition, DWORD, HANDLE)
{
    int flags = O_CLOEXEC;
    bool read = access & (GENERIC_READ | 0x0001 /* FILE_READ_DATA */);
    bool write = access & (GENERIC_WRITE | 0x0002 /* FILE_WRITE_DATA */ | FILE_APPEND_DATA);
    flags |= read && write ? O_RDWR : write ? O_WRONLY : O_RDONLY;
    if ((access & FILE_APPEND_DATA) && !(access & (GENERIC_WRITE | 0x0002))) flags |= O_APPEND;
    switch (disposition) {
    case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default: break; /* OPEN_EXISTING */
    }
    int fd = open(name, flags, 0666);
    if (fd < 0) {
        fail_errno();
        return INVALID_HANDLE_VALUE;
    }
    struct stat st;
    if (!fstat(fd, &st) && S_ISDIR(st.st_mode)) {
        close(fd);
        t_last_error = ERROR_ACCESS_DENIED;
        return INVALID_HANDLE_VALUE;
    }
    FileObject *f = new FileObject();
    f->kind = KIND_FILE;
    f->refs = 1;
    f->fd = fd;
    t_last_error = (disposition == OPEN_ALWAYS || disposition == CREATE_ALWAYS) && !(flags & O_TRUNC) ? 0 : 0;
    return f;
}

BOOL ReadFile(HANDLE h, LPVOID buffer, DWORD size, LPDWORD read_size, LPOVERLAPPED overlapped)
{
    int fd = file_fd(h);
    if (fd < 0) return FALSE;
    ssize_t n;
    if (overlapped) {
        off_t at = (off_t)((uint64_t)overlapped->OffsetHigh << 32 | overlapped->Offset);
        n = pread(fd, buffer, size, at);
        overlapped->Internal = 0;
        overlapped->InternalHigh = n < 0 ? 0 : (ULONG_PTR)n;
        if (overlapped->hEvent) SetEvent(overlapped->hEvent);
    } else {
        n = read(fd, buffer, size);
    }
    if (n < 0) return fail_errno();
    if (read_size) *read_size = (DWORD)n;
    return TRUE;
}

/* The read happens now; its completion routine runs at the thread's next alertable wait, as on Windows */
BOOL ReadFileEx(HANDLE h, LPVOID buffer, DWORD size, LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE done)
{
    int fd = file_fd(h);
    if (fd < 0) return FALSE;
    off_t at = (off_t)((uint64_t)overlapped->OffsetHigh << 32 | overlapped->Offset);
    size_t got = 0;
    while (got < size) {
        ssize_t n = pread(fd, (char *)buffer + got, size - got, at + (off_t)got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    overlapped->Internal = 0;
    overlapped->InternalHigh = got;
    queue_apc(done, got || !size ? 0 : ERROR_HANDLE_EOF, (DWORD)got, overlapped);
    return TRUE;
}

BOOL GetOverlappedResult(HANDLE, LPOVERLAPPED overlapped, LPDWORD transferred, BOOL)
{
    *transferred = (DWORD)overlapped->InternalHigh;
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED overlapped)
{
    int fd = file_fd(h);
    if (fd < 0) return FALSE;
    ssize_t n = overlapped ? pwrite(fd, buffer, size, (off_t)((uint64_t)overlapped->OffsetHigh << 32 | overlapped->Offset))
                           : write(fd, buffer, size);
    if (n < 0) return fail_errno();
    if (written) *written = (DWORD)n;
    return TRUE;
}

DWORD SetFilePointer(HANDLE h, LONG distance, PLONG high, DWORD method)
{
    int fd = file_fd(h);
    off_t offset = high ? (off_t)((uint64_t)(uint32_t)*high << 32 | (uint32_t)distance) : (off_t)distance;
    off_t r = lseek(fd, offset, method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) {
        fail_errno();
        return INVALID_SET_FILE_POINTER;
    }
    if (high) *high = (LONG)(r >> 32);
    return (DWORD)r;
}

BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, PLARGE_INTEGER position, DWORD method)
{
    off_t r = lseek(file_fd(h), (off_t)distance.QuadPart,
                    method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) return fail_errno();
    if (position) position->QuadPart = r;
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    struct stat st;
    if (fstat(file_fd(h), &st)) {
        fail_errno();
        return INVALID_FILE_SIZE;
    }
    if (high) *high = (DWORD)((uint64_t)st.st_size >> 32);
    return (DWORD)st.st_size;
}

BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    struct stat st;
    if (fstat(file_fd(h), &st)) return fail_errno();
    size->QuadPart = st.st_size;
    return TRUE;
}

BOOL SetEndOfFile(HANDLE h)
{
    int fd = file_fd(h);
    off_t at = lseek(fd, 0, SEEK_CUR);
    return at >= 0 && !ftruncate(fd, at) ? TRUE : fail_errno();
}

BOOL FlushFileBuffers(HANDLE h) { return !fsync(file_fd(h)) ? TRUE : fail_errno(); }

BOOL GetFileTime(HANDLE h, LPFILETIME creation, LPFILETIME access, LPFILETIME write)
{
    struct stat st;
    if (fstat(file_fd(h), &st)) return fail_errno();
    if (creation) timespec_to_filetime(&st.st_mtim, creation);
    if (access) timespec_to_filetime(&st.st_atim, access);
    if (write) timespec_to_filetime(&st.st_mtim, write);
    return TRUE;
}

BOOL DeleteFileA(LPCSTR name) { return !unlink(name) ? TRUE : fail_errno(); }
BOOL MoveFileA(LPCSTR from, LPCSTR to) { return !rename(from, to) ? TRUE : fail_errno(); }
BOOL CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES) { return !mkdir(name, 0777) ? TRUE : fail_errno(); }
BOOL RemoveDirectoryA(LPCSTR name) { return !rmdir(name) ? TRUE : fail_errno(); }

BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists)
{
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return fail_errno();
    int out = open(to, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0666);
    if (out < 0) {
        close(in);
        return fail_errno();
    }
    char buffer[65536];
    ssize_t n;
    BOOL ok = TRUE;
    while ((n = read(in, buffer, sizeof buffer)) > 0)
        if (write(out, buffer, (size_t)n) != n) ok = FALSE;
    close(in);
    close(out);
    return ok && n == 0;
}

static DWORD attributes_of(const struct stat *st, const char *name)
{
    DWORD a = S_ISDIR(st->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (!(st->st_mode & 0200)) a |= FILE_ATTRIBUTE_READONLY;
    if (name[0] == '.' && strcmp(name, ".") && strcmp(name, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    const char *last = slash > back ? slash : back;
    return last ? last + 1 : path;
}

DWORD GetFileAttributesA(LPCSTR name)
{
    struct stat st;
    if (stat(name, &st)) {
        fail_errno();
        return INVALID_FILE_ATTRIBUTES;
    }
    return attributes_of(&st, base_name(name));
}

BOOL SetFileAttributesA(LPCSTR name, DWORD attributes)
{
    struct stat st;
    if (stat(name, &st)) return fail_errno();
    mode_t mode = attributes & FILE_ATTRIBUTE_READONLY ? st.st_mode & ~0222 : st.st_mode | 0200;
    return !chmod(name, mode) ? TRUE : fail_errno();
}

/* FindFirstFile: the pattern's directory part is opened (the host resolves its case) and the
   name part matched without regard to case, as on Windows */
static bool find_next(FindObject *f, LPWIN32_FIND_DATAA data)
{
    while (struct dirent *e = readdir(f->dir)) {
        if (fnmatch(f->pattern, e->d_name, FNM_CASEFOLD)) {
            /* "*.*" matches names without a dot on Windows */
            if (strcmp(f->pattern, "*.*") || strchr(e->d_name, '.')) continue;
        }
        char full[MAX_PATH * 2];
        snprintf(full, sizeof full, "%s/%s", f->directory, e->d_name);
        struct stat st = {};
        stat(full, &st);
        memset(data, 0, sizeof *data);
        data->dwFileAttributes = attributes_of(&st, e->d_name);
        timespec_to_filetime(&st.st_mtim, &data->ftCreationTime);
        timespec_to_filetime(&st.st_atim, &data->ftLastAccessTime);
        timespec_to_filetime(&st.st_mtim, &data->ftLastWriteTime);
        data->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        data->nFileSizeLow = (DWORD)st.st_size;
        snprintf(data->cFileName, sizeof data->cFileName, "%s", e->d_name);
        return true;
    }
    return false;
}

static void split_pattern(LPCSTR pattern, char *directory, char *name)
{
    const char *base = base_name(pattern);
    size_t dlen = (size_t)(base - pattern);
    if (!dlen) strcpy(directory, ".");
    else {
        if (dlen >= MAX_PATH) dlen = MAX_PATH - 1;
        memcpy(directory, pattern, dlen);
        directory[dlen] = 0;
    }
    snprintf(name, MAX_PATH, "%s", *base ? base : "*");
}

HANDLE FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
    FindObject *f = new FindObject();
    f->kind = KIND_FIND;
    f->refs = 1;
    split_pattern(pattern, f->directory, f->pattern);
    f->dir = opendir(f->directory);
    if (!f->dir || !find_next(f, data)) {
        t_last_error = ERROR_FILE_NOT_FOUND;
        destroy(f);
        return INVALID_HANDLE_VALUE;
    }
    return f;
}

BOOL FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA data)
{
    FindObject *f = (FindObject *)object(h, KIND_FIND);
    if (f && find_next(f, data)) return TRUE;
    t_last_error = 18; /* ERROR_NO_MORE_FILES */
    return FALSE;
}

BOOL FindClose(HANDLE h) { return CloseHandle(h); }

/* the CRT's _findfirst family, on the same matching */
intptr_t _findfirst64i32(const char *pattern, struct _finddata64i32_t *data)
{
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) {
        errno = ENOENT;
        return -1;
    }
    bo1_find_to_crt(&found, data);
    return (intptr_t)h;
}

int _findnext64i32(intptr_t handle, struct _finddata64i32_t *data)
{
    WIN32_FIND_DATAA found;
    if (!FindNextFileA((HANDLE)handle, &found)) {
        errno = ENOENT;
        return -1;
    }
    bo1_find_to_crt(&found, data);
    return 0;
}

int _findclose(intptr_t handle) { return CloseHandle((HANDLE)handle) ? 0 : -1; }

DWORD GetCurrentDirectoryA(DWORD size, LPSTR buffer)
{
    char cwd[MAX_PATH];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    DWORD n = (DWORD)strlen(cwd);
    if (n + 1 > size) return n + 1;
    memcpy(buffer, cwd, n + 1);
    return n;
}

BOOL SetCurrentDirectoryA(LPCSTR name) { return !chdir(name) ? TRUE : fail_errno(); }

DWORD GetFullPathNameA(LPCSTR name, DWORD size, LPSTR buffer, LPSTR *file_part)
{
    char full[MAX_PATH * 2];
    if (name[0] == '/' || name[0] == '\\') snprintf(full, sizeof full, "%s", name);
    else {
        char cwd[MAX_PATH];
        if (!getcwd(cwd, sizeof cwd)) return 0;
        snprintf(full, sizeof full, "%s/%s", cwd, name);
    }
    DWORD n = (DWORD)strlen(full);
    if (n + 1 > size) return n + 1;
    memcpy(buffer, full, n + 1);
    if (file_part) *file_part = (LPSTR)base_name(buffer);
    return n;
}

/* ---- modules and the process ----------------------------------------------------------------- */

/* The "executable" lives in the data folder, so the engine's install path (the exe's folder) is
   where the player's game files are. */
DWORD GetModuleFileNameA(HMODULE, LPSTR name, DWORD size)
{
    char path[MAX_PATH];
    int n = snprintf(path, sizeof path, "%s\\BlackOpsMP.exe", bo1_data_root());
    if ((DWORD)n >= size) n = (int)size - 1;
    memcpy(name, path, (size_t)n);
    name[n] = 0;
    return (DWORD)n;
}

static int g_module_anchor;
HMODULE GetModuleHandleA(LPCSTR name) { return name ? NULL : (HMODULE)&g_module_anchor; }

BOOL GetModuleHandleExA(DWORD, LPCSTR, HMODULE *module)
{
    *module = (HMODULE)&g_module_anchor;
    return TRUE;
}

HMODULE LoadLibraryA(LPCSTR) { return NULL; } /* no DLLs: every caller copes with NULL */
FARPROC GetProcAddress(HMODULE, LPCSTR) { return NULL; }

LPSTR GetCommandLineA(void) { return bo1_command_line(); }

DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size)
{
    const char *v = getenv(name);
    if (!v) return 0;
    DWORD n = (DWORD)strlen(v);
    if (n + 1 > size) return n + 1;
    memcpy(buffer, v, n + 1);
    return n;
}

void ExitProcess(UINT code) { _exit((int)code); }
BOOL TerminateProcess(HANDLE, UINT code) { _exit((int)code); }

void OutputDebugStringA(LPCSTR text) { bo1_log("%s", text); }
BOOL IsDebuggerPresent(void) { return FALSE; }
void DebugBreak(void) { __builtin_trap(); }

void RaiseException(DWORD code, DWORD, DWORD, const ULONG_PTR *)
{
    bo1_log("RaiseException 0x%08lx", (unsigned long)code);
    __builtin_trap();
}

LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER) { return NULL; }
UINT SetErrorMode(UINT) { return 0; }

DWORD FormatMessageA(DWORD, LPCVOID, DWORD message, DWORD, LPSTR buffer, DWORD size, va_list *)
{
    return buffer && size ? (DWORD)snprintf(buffer, size, "Windows error %lu", (unsigned long)message) : 0;
}

/* ---- memory ---------------------------------------------------------------------------------- */

/* VirtualAlloc reservations, so MEM_RELEASE (size 0) knows how much to unmap */
struct Reservation {
    uintptr_t base;
    size_t size;
};
static Reservation g_reservations[4096];
static int g_reservation_count;
static pthread_mutex_t g_reservations_lock = PTHREAD_MUTEX_INITIALIZER;

static Reservation *find_reservation(uintptr_t address)
{
    for (int i = 0; i < g_reservation_count; i++)
        if (address >= g_reservations[i].base && address < g_reservations[i].base + g_reservations[i].size)
            return &g_reservations[i];
    return NULL;
}

static int protection(DWORD protect)
{
    switch (protect & 0xff) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (address && (type & MEM_COMMIT) && !(type & MEM_RESERVE)) {
        /* commit inside an earlier reservation */
        uintptr_t start = (uintptr_t)address & ~(page - 1);
        uintptr_t end = ((uintptr_t)address + size + page - 1) & ~(page - 1);
        if (mprotect((void *)start, end - start, protection(protect))) return NULL;
        return address;
    }
    size = (size + page - 1) & ~(page - 1);
    int prot = type & MEM_COMMIT ? protection(protect) : PROT_NONE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | (prot == PROT_NONE ? MAP_NORESERVE : 0) | (address ? MAP_FIXED_NOREPLACE : 0);
    void *p = mmap(address, size, prot, flags, -1, 0);
    if (p == MAP_FAILED) {
        t_last_error = 8;
        return NULL;
    }
    pthread_mutex_lock(&g_reservations_lock);
    if (g_reservation_count < (int)(sizeof g_reservations / sizeof *g_reservations))
        g_reservations[g_reservation_count++] = {(uintptr_t)p, size};
    else
        bo1_log("VirtualAlloc: too many reservations to track");
    pthread_mutex_unlock(&g_reservations_lock);
    return p;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD type)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (type & MEM_DECOMMIT) {
        uintptr_t start = (uintptr_t)address & ~(page - 1);
        uintptr_t end = ((uintptr_t)address + size + page - 1) & ~(page - 1);
        madvise((void *)start, end - start, MADV_DONTNEED);
        return !mprotect((void *)start, end - start, PROT_NONE);
    }
    pthread_mutex_lock(&g_reservations_lock);
    Reservation *r = find_reservation((uintptr_t)address);
    BOOL ok = r && r->base == (uintptr_t)address;
    if (ok) {
        munmap((void *)r->base, r->size);
        *r = g_reservations[--g_reservation_count];
    }
    pthread_mutex_unlock(&g_reservations_lock);
    return ok;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old)
{
    if (old) *old = PAGE_READWRITE;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)address & ~(page - 1);
    uintptr_t end = ((uintptr_t)address + size + page - 1) & ~(page - 1);
    return !mprotect((void *)start, end - start, protection(protect));
}

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size)
{
    if (size < sizeof *info) return 0;
    memset(info, 0, sizeof *info);
    pthread_mutex_lock(&g_reservations_lock);
    Reservation *r = find_reservation((uintptr_t)address);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    info->BaseAddress = (PVOID)((uintptr_t)address & ~(page - 1));
    if (r) {
        info->AllocationBase = (PVOID)r->base;
        info->RegionSize = r->base + r->size - (uintptr_t)info->BaseAddress;
        info->State = MEM_COMMIT;
        info->Protect = PAGE_READWRITE;
    } else {
        /* not ours: the image, the heap or a stack; report it readable */
        info->AllocationBase = info->BaseAddress;
        info->RegionSize = page;
        info->State = MEM_COMMIT;
        info->Protect = PAGE_READWRITE;
    }
    pthread_mutex_unlock(&g_reservations_lock);
    return sizeof *info;
}

/* The 32-bit guest addresses under 4 GB, and the host keeps part of that; the engine sizes its
   pools from these numbers, so report what it can really use. */
static void memory_numbers(uint64_t *total, uint64_t *avail)
{
    struct sysinfo si;
    uint64_t phys = 0, free_ = 0;
    if (!sysinfo(&si)) {
        phys = (uint64_t)si.totalram * si.mem_unit;
        free_ = (uint64_t)(si.freeram + si.bufferram) * si.mem_unit;
    }
    const uint64_t cap = 2048ull << 20;
    *total = phys && phys < cap ? phys : cap;
    *avail = free_ && free_ < *total ? free_ : *total;
}

void GlobalMemoryStatus(LPMEMORYSTATUS status)
{
    uint64_t total, avail;
    memory_numbers(&total, &avail);
    status->dwLength = sizeof *status;
    status->dwMemoryLoad = (DWORD)(100 - avail * 100 / total);
    status->dwTotalPhys = (SIZE_T)total;
    status->dwAvailPhys = (SIZE_T)avail;
    status->dwTotalPageFile = (SIZE_T)total;
    status->dwAvailPageFile = (SIZE_T)avail;
    status->dwTotalVirtual = 0x7ffe0000;
    status->dwAvailVirtual = 0x70000000;
}

BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX status)
{
    uint64_t total, avail;
    memory_numbers(&total, &avail);
    status->dwMemoryLoad = (DWORD)(100 - avail * 100 / total);
    status->ullTotalPhys = total;
    status->ullAvailPhys = avail;
    status->ullTotalPageFile = total;
    status->ullAvailPageFile = avail;
    status->ullTotalVirtual = 0x7ffe0000;
    status->ullAvailVirtual = 0x70000000;
    status->ullAvailExtendedVirtual = 0;
    return TRUE;
}

BOOL K32GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS counters, DWORD size)
{
    memset(counters, 0, size);
    counters->cb = size;
    return TRUE;
}

BOOL GetProcessHandleCount(HANDLE, PDWORD count)
{
    *count = 0;
    return TRUE;
}

void GetSystemInfo(LPSYSTEM_INFO info)
{
    memset(info, 0, sizeof *info);
    info->wProcessorArchitecture = 12; /* PROCESSOR_ARCHITECTURE_ARM64 */
    info->dwPageSize = (DWORD)sysconf(_SC_PAGESIZE);
    info->lpMinimumApplicationAddress = (LPVOID)0x10000;
    info->lpMaximumApplicationAddress = (LPVOID)0xfffeffff;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    info->dwNumberOfProcessors = cpus > 0 ? (DWORD)cpus : 1;
    info->dwActiveProcessorMask = (DWORD_PTR)((1ull << info->dwNumberOfProcessors) - 1);
    info->dwAllocationGranularity = 65536;
}

BOOL GetProcessAffinityMask(HANDLE, PDWORD_PTR process_mask, PDWORD_PTR system_mask)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    *process_mask = *system_mask = info.dwActiveProcessorMask;
    return TRUE;
}

DWORD GetCurrentProcessorNumber(void) { return 0; }

BOOL GetVersionExA(LPOSVERSIONINFOA info)
{
    /* Windows 7: the newest the 2010 engine knows about */
    info->dwMajorVersion = 6;
    info->dwMinorVersion = 1;
    info->dwBuildNumber = 7601;
    info->dwPlatformId = 2;
    info->szCSDVersion[0] = 0;
    return TRUE;
}

/* ---- toolhelp: nothing to enumerate ---------------------------------------------------------- */

HANDLE CreateToolhelp32Snapshot(DWORD, DWORD)
{
    Object *o = new Object();
    o->kind = KIND_SNAP;
    o->refs = 1;
    return o;
}

BOOL Thread32First(HANDLE, LPTHREADENTRY32) { return FALSE; }
BOOL Thread32Next(HANDLE, LPTHREADENTRY32) { return FALSE; }
BOOL Module32First(HANDLE, LPMODULEENTRY32) { return FALSE; }
BOOL Module32Next(HANDLE, LPMODULEENTRY32) { return FALSE; }

/* ---- global memory (the clipboard's) --------------------------------------------------------- */

struct GlobalBlock {
    SIZE_T size;
    alignas(16) unsigned char data[];
};

HGLOBAL GlobalAlloc(UINT flags, SIZE_T size)
{
    GlobalBlock *b = (GlobalBlock *)calloc(1, sizeof(GlobalBlock) + size);
    if (b) b->size = size;
    (void)flags;
    return b;
}

LPVOID GlobalLock(HGLOBAL memory) { return memory ? ((GlobalBlock *)memory)->data : NULL; }
BOOL GlobalUnlock(HGLOBAL) { return TRUE; }
SIZE_T GlobalSize(HGLOBAL memory) { return memory ? ((GlobalBlock *)memory)->size : 0; }

HGLOBAL GlobalFree(HGLOBAL memory)
{
    free(memory);
    return NULL;
}
