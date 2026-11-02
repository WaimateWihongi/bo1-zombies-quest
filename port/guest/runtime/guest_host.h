/* Host functions the guest imports (port/imports.list). Guest pointers pass as-is; sizes are
   32-bit. Never import a function that returns a host pointer. */
#ifndef GUEST_HOST_H
#define GUEST_HOST_H

#include <stdint.h>

long long __guest_syscall(long long n, long long a, long long b, long long c,
                          long long d, long long e, long long f);
unsigned __guest_get_tp(void);
void host_set_tp(unsigned tp);
int host_thread_create(void *thread, unsigned stack_size);
void host_log(int priority, const char *text);
_Noreturn void host_abort(const char *why);

#endif
