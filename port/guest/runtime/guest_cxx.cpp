// The parts of the C++ ABI the engine needs, built without exceptions or RTTI.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

namespace std {
struct nothrow_t {
    explicit nothrow_t() = default;
};
}

extern "C" {
#include "guest_host.h"
}

void *operator new(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p) host_abort("operator new: out of memory");
    return p;
}

void *operator new[](size_t size)
{
    return operator new(size);
}

void *operator new(size_t size, const std::nothrow_t &) noexcept
{
    return malloc(size ? size : 1);
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept
{
    return malloc(size ? size : 1);
}

void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" {

void *__dso_handle = &__dso_handle;

void __cxa_pure_virtual(void)
{
    host_abort("pure virtual function called");
}

void __cxa_deleted_virtual(void)
{
    host_abort("deleted virtual function called");
}

// Function-local statics: one recursive lock for all of them (a static's constructor may
// initialise another). The guard's first byte says "done".
static pthread_mutex_t guard_lock;
static pthread_once_t guard_once = PTHREAD_ONCE_INIT;

static void guard_lock_initialize(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&guard_lock, &attr);
}

int __cxa_guard_acquire(unsigned long long *guard)
{
    if (__atomic_load_n((unsigned char *)guard, __ATOMIC_ACQUIRE)) return 0;
    pthread_once(&guard_once, guard_lock_initialize);
    pthread_mutex_lock(&guard_lock);
    if (*(unsigned char *)guard) {
        pthread_mutex_unlock(&guard_lock);
        return 0;
    }
    return 1;
}

void __cxa_guard_release(unsigned long long *guard)
{
    __atomic_store_n((unsigned char *)guard, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&guard_lock);
}

void __cxa_guard_abort(unsigned long long *)
{
    pthread_mutex_unlock(&guard_lock);
}

}
