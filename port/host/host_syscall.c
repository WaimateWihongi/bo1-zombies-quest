/* The guest's Linux syscalls (musl's, via __guest_syscall). The guest's types are laid out so
 * most pass straight to the kernel: struct stat/kstat, off_t, time_t and the timespec it reads
 * back are the 64-bit kernel's layout. Translated here: anything with a pointer or long inside
 * (iovec, sysinfo), timespecs the kernel reads (their tv_nsec has 32 bits of padding above it),
 * memory mapping (kept below 4 GB), and calls that would disturb the host process (signals,
 * thread exit, set_tid_address). Arguments arrive zero-extended to 64 bits. */
#define _GNU_SOURCE
#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

struct guest_timespec {
    int64_t tv_sec;
    int32_t tv_nsec;
    int32_t pad;
};

struct guest_iovec {
    uint32_t base;
    uint32_t len;
};

#define MAX_IOV 64

static long result(long r)
{
    return r == -1 ? -errno : r;
}

static struct timespec *timespec_in(uint64_t guest, struct timespec *out)
{
    if (!guest) return NULL;
    const struct guest_timespec *g = GUEST_PTR(const struct guest_timespec *, guest);
    out->tv_sec = g->tv_sec;
    out->tv_nsec = g->tv_nsec;
    return out;
}

static int iovec_in(uint64_t guest, uint64_t count, struct iovec *out)
{
    if (count > MAX_IOV) return -EINVAL;
    const struct guest_iovec *g = GUEST_PTR(const struct guest_iovec *, guest);
    for (uint64_t i = 0; i < count; i++) {
        out[i].iov_base = (void *)(uintptr_t)g[i].base;
        out[i].iov_len = g[i].len;
    }
    return 0;
}

static long log_writev(int fd, const struct iovec *iov, int count)
{
    long total = 0;
    for (int i = 0; i < count; i++) {
        host_log_guest_write(fd, iov[i].iov_base, iov[i].iov_len);
        total += (long)iov[i].iov_len;
    }
    return total;
}

static long guest_sysinfo(uint64_t guest)
{
    struct sysinfo si;
    if (sysinfo(&si)) return -errno;
    /* musl's struct sysinfo on a 32-bit long */
    struct {
        int32_t uptime;
        uint32_t loads[3];
        uint32_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
        uint16_t procs, pad;
        uint32_t totalhigh, freehigh, mem_unit;
        char reserved[8];
    } *g = GUEST_PTR(void *, guest);
    /* report in 1 MB units so 32-bit fields hold the Quest's RAM */
    uint64_t unit = si.mem_unit ? si.mem_unit : 1;
    memset(g, 0, sizeof *g);
    g->uptime = (int32_t)si.uptime;
    for (int i = 0; i < 3; i++) g->loads[i] = (uint32_t)si.loads[i];
    g->totalram = (uint32_t)(si.totalram * unit >> 20);
    g->freeram = (uint32_t)(si.freeram * unit >> 20);
    g->sharedram = (uint32_t)(si.sharedram * unit >> 20);
    g->bufferram = (uint32_t)(si.bufferram * unit >> 20);
    g->totalswap = (uint32_t)(si.totalswap * unit >> 20);
    g->freeswap = (uint32_t)(si.freeswap * unit >> 20);
    g->procs = si.procs;
    g->mem_unit = 1 << 20;
    return 0;
}

static void unknown(long n)
{
    static uint8_t seen[512];
    if (n >= 0 && n < 512) {
        if (seen[n]) return;
        seen[n] = 1;
    }
    host_warn("syscall: the guest made syscall %ld, which the host does not run", n);
}

long long host_syscall(long long n, long long a, long long b, long long c, long long d, long long e,
                       long long f)
{
    struct timespec ts, ts2;
    struct iovec iov[MAX_IOV];
    int r;

    char path[1024], path2[1024];
    switch (n) {
    /* paths: Windows separators and case (host_path.c) */
    case __NR_openat:
    case __NR_newfstatat:
    case __NR_statx:
    case __NR_mkdirat:
    case __NR_unlinkat:
    case __NR_faccessat:
    case __NR_readlinkat:
    case __NR_fchmodat:
        return result(syscall(n, a, host_guest_path((int)a, (uint64_t)b, path, sizeof path), c, d, e, f));
    case __NR_renameat:
    case __NR_renameat2:
        return result(syscall(n, a, host_guest_path((int)a, (uint64_t)b, path, sizeof path), c,
                              host_guest_path((int)c, (uint64_t)d, path2, sizeof path2), e));
    case __NR_chdir:
        return result(syscall(n, host_guest_path(AT_FDCWD, (uint64_t)a, path, sizeof path)));

    /* files and descriptors */
    case __NR_read:
    case __NR_close:
    case __NR_lseek:
    case __NR_getdents64:
    case __NR_fstat:
    case __NR_fcntl:
    case __NR_ioctl:
    case __NR_dup:
    case __NR_dup3:
    case __NR_pipe2:
    case __NR_getcwd:
    case __NR_ftruncate:
    case __NR_fsync:
    case __NR_fdatasync:
    case __NR_pread64:
    case __NR_pwrite64:
    case __NR_fchmod:
    case __NR_flock:
    /* identity, time, scheduling */
    case __NR_getpid:
    case __NR_getppid:
    case __NR_gettid:
    case __NR_getuid:
    case __NR_geteuid:
    case __NR_getgid:
    case __NR_getegid:
    case __NR_clock_gettime:
    case __NR_clock_getres:
    case __NR_sched_yield:
    case __NR_sched_getaffinity:
    case __NR_getrandom:
    case __NR_uname:
    case __NR_prlimit64:
    case __NR_umask:
    /* sockets whose arguments hold no pointers to pointers */
    case __NR_socket:
    case __NR_bind:
    case __NR_connect:
    case __NR_listen:
    case __NR_accept:
    case __NR_accept4:
    case __NR_getsockname:
    case __NR_getpeername:
    case __NR_sendto:
    case __NR_recvfrom:
    case __NR_setsockopt:
    case __NR_getsockopt:
    case __NR_shutdown:
        return result(syscall(n, a, b, c, d, e, f));

    case __NR_write:
        if (a == 1 || a == 2) {
            host_log_guest_write((int)a, GUEST_PTR(const char *, b), (size_t)c);
            return c;
        }
        return result(syscall(n, a, b, c));
    case __NR_writev:
        if ((r = iovec_in(b, c, iov))) return r;
        if (a == 1 || a == 2) return log_writev((int)a, iov, (int)c);
        return result(writev((int)a, iov, (int)c));
    case __NR_readv:
        if ((r = iovec_in(b, c, iov))) return r;
        return result(readv((int)a, iov, (int)c));
    case __NR_preadv:
        if ((r = iovec_in(b, c, iov))) return r;
        return result(syscall(n, a, iov, c, d, e));
    case __NR_pwritev:
        if ((r = iovec_in(b, c, iov))) return r;
        return result(syscall(n, a, iov, c, d, e));

    case __NR_nanosleep:
        return result(syscall(n, timespec_in(a, &ts), b));
    case __NR_clock_nanosleep:
        return result(syscall(n, a, b, timespec_in(c, &ts), d));
    case __NR_ppoll:
        return result(syscall(n, a, b, timespec_in(c, &ts), d, e));
    case __NR_pselect6:
        return result(syscall(n, a, b, c, d, timespec_in(e, &ts), f));
    case __NR_utimensat:
        if (c) {
            struct timespec times[2];
            timespec_in(c, &times[0]);
            timespec_in(c + sizeof(struct guest_timespec), &times[1]);
            return result(syscall(n, a, b ? host_guest_path((int)a, (uint64_t)b, path, sizeof path) : NULL, times, d));
        }
        return result(syscall(n, a, b ? host_guest_path((int)a, (uint64_t)b, path, sizeof path) : NULL, 0, d));
    case __NR_futex: {
        int op = (int)b & FUTEX_CMD_MASK;
        if (op == FUTEX_WAIT || op == FUTEX_WAIT_BITSET || op == FUTEX_LOCK_PI || op == FUTEX_WAIT_REQUEUE_PI)
            return result(syscall(n, a, b, c, timespec_in(d, &ts2), e, f));
        return result(syscall(n, a, b, c, d, e, f));
    }
    case __NR_sysinfo:
        return guest_sysinfo(a);

    case __NR_mmap:
        return host_guest_mmap(a, b, (int)c, (int)d, (int)e, f);
    case __NR_munmap:
        return host_guest_munmap(a, b);
    case __NR_mprotect:
        return host_guest_mprotect(a, b, (int)c);
    case __NR_madvise:
        return a >> 32 ? -EINVAL : result(syscall(n, a, b, c));
    case __NR_mremap:
    case __NR_brk:
        return -ENOMEM; /* musl falls back to mmap */

    /* the host's signals and thread bookkeeping are bionic's, not the guest's */
    case __NR_rt_sigprocmask:
        if (c) memset(GUEST_PTR(void *, c), 0, 8);
        return 0;
    case __NR_rt_sigaction:
        if (c) memset(GUEST_PTR(void *, c), 0, 32);
        return 0;
    case __NR_sigaltstack:
    case __NR_set_robust_list:
        return 0;
    case __NR_set_tid_address:
        return gettid();
    case __NR_kill:
    case __NR_tkill:
    case __NR_tgkill: {
        int sig = (int)(n == __NR_tgkill ? c : b);
        if (sig == SIGABRT) host_fatal("the guest called abort()");
        if (sig) host_warn("syscall: the guest sent signal %d, ignored", sig);
        return 0;
    }
    case __NR_exit:
        host_fatal("a guest thread called SYS_exit");
    case __NR_exit_group:
        /* _exit, not exit: this is the guest's thread, and running the host's static destructors
           under ART's main thread makes the runtime abort */
        host_info("the guest exited with status %d", (int)a);
        _exit((int)a);

    default:
        unknown(n);
        return -ENOSYS;
    }
}
