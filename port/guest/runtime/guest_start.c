/* Image header and entry point of bo1_guest.elf; replaces musl's crt1 and __libc_start_main. */
#include "guest_thread.h"
#include "guest_host.h"
#include "bo1_abi.h"
#include <elf.h>
#include <stdlib.h>


extern void (*const __guest_init_start[])(void);
extern void (*const __guest_init_end[])(void);
extern char __guest_image_end[];
extern unsigned long long __host_import_table[];
extern const char __host_import_names[];
extern const unsigned __host_import_count;

void __guest_start(struct bo1_guest_boot *boot);
void __guest_thread_start(struct guest_thread *g);
void __guest_thread_attach(void);

#define GUEST_ADDRESS(x) ((uint32_t)(uintptr_t)(x))

__attribute__((section("__TEXT,__guest_header"), used))
const struct bo1_guest_header __guest_header = {
    .magic = BO1_GUEST_MAGIC,
    .abi_version = BO1_GUEST_ABI_VERSION,
    .image_end = GUEST_ADDRESS(__guest_image_end),
    .import_table = GUEST_ADDRESS(__host_import_table),
    .import_names = GUEST_ADDRESS(__host_import_names),
    .import_count = GUEST_ADDRESS(&__host_import_count),
    .start = GUEST_ADDRESS(__guest_start),
    .thread_start = GUEST_ADDRESS(__guest_thread_start),
    .thread_attach = GUEST_ADDRESS(__guest_thread_attach),
};

int main(int argc, char **argv, char **envp);
extern char **__environ;

static struct guest_thread main_thread;
struct guest_thread *guest_main_thread = &main_thread;
static size_t auxv[] = {AT_PAGESZ, 4096, AT_HWCAP, 0, 0, 0};

void __guest_start(struct bo1_guest_boot *boot)
{
    char **argv = (char **)(uintptr_t)boot->argv;
    char **envp = (char **)(uintptr_t)boot->envp;

    main_thread.pt.detach_state = DT_JOINABLE;
    main_thread.pt.tsd = (void **)__pthread_tsd_main;
    guest_thread_initialize(&main_thread);

    libc.can_do_threads = 1;
    libc.page_size = boot->page_size;
    auxv[1] = boot->page_size;
    libc.auxv = auxv;
    __environ = envp;
    __progname = __progname_full = argv[0] ? argv[0] : "bo1";

    for (void (*const *f)(void) = __guest_init_start; f < __guest_init_end; f++) (*f)();

    exit(main((int)boot->argc, argv, envp));
}
