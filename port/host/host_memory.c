/* Guest address space: every byte the guest can address lies below 4 GB.
 *
 * At startup the host reserves (PROT_NONE, no backing) every free range between 256 MB and 4 GB,
 * puts the guest image at the top of the largest one, then hands out pages from those
 * reservations with a bitmap. "Unmapping" puts PROT_NONE back so nothing else in the process (the JVM, the driver)
 * can take the address range from under the guest. */
#define _GNU_SOURCE
#include "host.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define LOW_START 0x10000000ull
#define LOW_END 0xffff0000ull
#define MAX_REGIONS 32

struct region {
    uint64_t start, end;
    uint8_t *used; /* one bit per page */
    size_t pages, rover;
};

static struct region regions[MAX_REGIONS];
static int region_count;
static size_t page_size;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int bit(const struct region *r, size_t i)
{
    return r->used[i >> 3] >> (i & 7) & 1;
}

static void set_bits(struct region *r, size_t first, size_t count, int value)
{
    for (size_t i = first; i < first + count; i++) {
        if (value) r->used[i >> 3] |= 1 << (i & 7);
        else r->used[i >> 3] &= ~(1 << (i & 7));
    }
}

static int reserve(uint64_t start, uint64_t end)
{
    void *p = mmap((void *)(uintptr_t)start, end - start, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return -errno;
    if ((uintptr_t)p != start) {
        /* kernels before 4.17 take MAP_FIXED_NOREPLACE as a hint */
        munmap(p, end - start);
        return -EEXIST;
    }
    return 0;
}

static void add_region(uint64_t start, uint64_t end)
{
    start = (start + page_size - 1) & ~(uint64_t)(page_size - 1);
    end &= ~(uint64_t)(page_size - 1);
    if (end <= start || end - start < 16 * 1024 * 1024 || region_count == MAX_REGIONS) return;
    if (reserve(start, end)) {
        host_warn("memory: could not reserve %#llx-%#llx", (unsigned long long)start,
                  (unsigned long long)end);
        return;
    }
    struct region *r = &regions[region_count++];
    r->start = start;
    r->end = end;
    r->pages = (end - start) / page_size;
    r->used = calloc((r->pages + 7) / 8, 1);
    host_info("memory: guest region %#llx-%#llx (%llu MB)", (unsigned long long)start,
              (unsigned long long)end, (unsigned long long)((end - start) >> 20));
}

int host_memory_initialize(void)
{
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    /* every gap in the low 4 GB, from the process's own map */
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return -errno;
    uint64_t cursor = LOW_START;
    char line[512];
    while (fgets(line, sizeof line, maps) && cursor < LOW_END) {
        unsigned long long start, end;
        if (sscanf(line, "%llx-%llx", &start, &end) != 2) continue;
        if (end <= cursor) continue;
        if (start > cursor) add_region(cursor, start < LOW_END ? start : LOW_END);
        cursor = end;
    }
    fclose(maps);
    if (cursor < LOW_END) add_region(cursor, LOW_END);
    return region_count ? 0 : -ENOMEM;
}

static void log_low_map(void)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    while (maps && fgets(line, sizeof line, maps)) {
        unsigned long long start;
        if (sscanf(line, "%llx", &start) == 1 && start < 0x100000000ull) host_error("  %s", line);
    }
    if (maps) fclose(maps);
}

/* The top of the largest region, so the heap keeps the long run below it */
uint64_t host_memory_place_image(uint64_t size, uint64_t align)
{
    size = (size + page_size - 1) & ~(uint64_t)(page_size - 1);
    struct region *best = NULL;
    for (int i = 0; i < region_count; i++)
        if (!best || regions[i].end - regions[i].start > best->end - best->start) best = &regions[i];
    uint64_t address = best && best->end - best->start >= size + align ? (best->end - size) & ~(align - 1) : 0;
    if (!address) {
        host_error("memory: no free range below 4 GB holds the %llu MB guest image; the low 4 GB:",
                   (unsigned long long)(size >> 20));
        log_low_map();
        return 0;
    }
    pthread_mutex_lock(&lock);
    set_bits(best, (address - best->start) / page_size, size / page_size, 1);
    pthread_mutex_unlock(&lock);
    return address;
}

static struct region *find_region(uint64_t address, uint64_t size)
{
    for (int i = 0; i < region_count; i++)
        if (address >= regions[i].start && address + size <= regions[i].end) return &regions[i];
    return NULL;
}

static uint64_t allocate_pages(size_t count)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < region_count; i++) {
            struct region *r = &regions[i];
            size_t first = pass ? 0 : r->rover, run = 0;
            for (size_t p = first; p < r->pages; p++) {
                if (bit(r, p)) {
                    run = 0;
                    continue;
                }
                if (++run == count) {
                    size_t start = p + 1 - count;
                    set_bits(r, start, count, 1);
                    r->rover = p + 1;
                    return r->start + (uint64_t)start * page_size;
                }
            }
        }
    }
    return 0;
}

static void release_pages(uint64_t address, uint64_t size)
{
    struct region *r = find_region(address, size);
    if (!r) return;
    mmap((void *)(uintptr_t)address, size, PROT_NONE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    size_t first = (address - r->start) / page_size;
    set_bits(r, first, size / page_size, 0);
    if (first < r->rover) r->rover = first;
}

long long host_guest_mmap(uint64_t address, uint64_t size, int prot, int flags, int fd, int64_t offset)
{
    if (!size) return -EINVAL;
    size = (size + page_size - 1) & ~(uint64_t)(page_size - 1);
    pthread_mutex_lock(&lock);
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        struct region *r = find_region(address, size);
        if (!r) {
            pthread_mutex_unlock(&lock);
            return -ENOMEM;
        }
        set_bits(r, (address - r->start) / page_size, size / page_size, 1);
    } else {
        address = allocate_pages(size / page_size);
        if (!address) {
            pthread_mutex_unlock(&lock);
            host_error("memory: out of guest address space (%llu KB asked)",
                       (unsigned long long)(size >> 10));
            return -ENOMEM;
        }
    }
    pthread_mutex_unlock(&lock);
    flags = (flags & ~MAP_FIXED_NOREPLACE) | MAP_FIXED;
    void *p = mmap((void *)(uintptr_t)address, size, prot, flags, fd, offset);
    if (p == MAP_FAILED) {
        int e = errno;
        pthread_mutex_lock(&lock);
        release_pages(address, size);
        pthread_mutex_unlock(&lock);
        return -e;
    }
    return (long long)address;
}

long long host_guest_munmap(uint64_t address, uint64_t size)
{
    size = (size + page_size - 1) & ~(uint64_t)(page_size - 1);
    pthread_mutex_lock(&lock);
    struct region *r = find_region(address, size);
    if (r) release_pages(address, size);
    pthread_mutex_unlock(&lock);
    return r ? 0 : -EINVAL;
}

long long host_guest_mprotect(uint64_t address, uint64_t size, int prot)
{
    if (!find_region(address, size)) return -EINVAL;
    return mprotect((void *)(uintptr_t)address, size, prot) ? -errno : 0;
}

void *host_low_map(size_t size, int prot)
{
    long long r = host_guest_mmap(0, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return r < 0 ? NULL : (void *)(uintptr_t)r;
}

void host_low_unmap(void *address, size_t size)
{
    host_guest_munmap((uintptr_t)address, size);
}
