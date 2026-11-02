/* Crash and hang reports for the guest, in logcat.
 *
 * A fatal signal in any thread logs that thread's registers and frame-pointer backtrace, then
 * goes on to the default action (so Android still writes its tombstone). SIGUSR2 sent to the
 * process (`adb shell run-as <package> kill -USR2 <pid>`) makes every thread log its backtrace
 * and carry on: a snapshot of where a hung game is.
 *
 * Guest addresses are logged as link addresses (bo1_guest.linked.elf), ready for
 *   llvm-symbolizer --obj=build/bo1_guest.linked.elf <address>... */
#define _GNU_SOURCE
#include "host.h"

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

/* the per-thread snapshot signal: a real-time one, since ART blocks SIGUSR1 in every thread
   and takes it for itself */
#define SNAPSHOT_SIGNAL 42

static uint64_t to_link(uint64_t address)
{
    uint64_t base = host_image.base, end = host_image.end;
    if (base && address >= base && address < end) return address - base + BO1_GUEST_LINK_BASE;
    return address;
}

static int in_guest_image(uint64_t address) { return address >= host_image.base && address < host_image.end; }

static void log_backtrace(const char *why, const ucontext_t *uc)
{
    const struct sigcontext *m = &uc->uc_mcontext;
    char line[1024];
    int n = snprintf(line, sizeof line, "%s: tid %d pc %#llx%s lr %#llx%s frames", why, (int)syscall(SYS_gettid),
                     (unsigned long long)to_link(m->pc), in_guest_image(m->pc) ? "" : "(host)",
                     (unsigned long long)to_link(m->regs[30]), in_guest_image(m->regs[30]) ? "" : "(host)");
    /* the guest's frame records: [fp] = caller's fp, [fp + 8] = return address (both 64-bit) */
    uint64_t fp = m->regs[29];
    for (int i = 0; i < 24 && fp && fp < 0x100000000ull && !(fp & 7) && n < (int)sizeof line - 24; i++) {
        const uint64_t *record = (const uint64_t *)(uintptr_t)fp;
        uint64_t next = record[0], ret = record[1];
        if (in_guest_image(ret)) n += snprintf(line + n, sizeof line - (size_t)n, " %#llx", (unsigned long long)to_link(ret));
        if (next <= fp) break;
        fp = next;
    }
    host_logf(6, "%s", line);
}

static void on_snapshot(int sig, siginfo_t *info, void *context)
{
    (void)sig;
    (void)info;
    log_backtrace("snapshot", (const ucontext_t *)context);
}

/* SIGUSR2: ask every thread for its backtrace (each answers in its own SNAPSHOT_SIGNAL handler) */
static void on_snapshot_request(int sig, siginfo_t *info, void *context)
{
    (void)sig;
    (void)info;
    (void)context;
    DIR *tasks = opendir("/proc/self/task");
    if (!tasks) return;
    pid_t self = getpid();
    for (struct dirent *e; (e = readdir(tasks));) {
        int tid = atoi(e->d_name);
        if (tid > 0) syscall(SYS_tgkill, self, tid, SNAPSHOT_SIGNAL);
    }
    closedir(tasks);
}

static struct sigaction previous[32];

static void on_fatal(int sig, siginfo_t *info, void *context)
{
    char why[64];
    snprintf(why, sizeof why, "FATAL signal %d at %#llx", sig, (unsigned long long)(uintptr_t)info->si_addr);
    log_backtrace(why, (const ucontext_t *)context);
    /* hand it on: the previous handler (ART's chain) or the default action */
    sigaction(sig, &previous[sig], NULL);
    raise(sig);
}

void host_crash_initialize(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = on_snapshot;
    sigaction(SNAPSHOT_SIGNAL, &sa, NULL);
    sa.sa_sigaction = on_snapshot_request;
    sigaction(SIGUSR2, &sa, NULL);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sa.sa_sigaction = on_fatal;
    const int fatal[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
    for (unsigned i = 0; i < sizeof fatal / sizeof fatal[0]; i++) sigaction(fatal[i], &sa, &previous[fatal[i]]);
}
