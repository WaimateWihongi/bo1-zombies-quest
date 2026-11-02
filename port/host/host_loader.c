/* Loads bo1_guest.elf into the low 4 GB and fills its import table.
 *
 * The image is a static ET_EXEC linked at BO1_GUEST_LINK_BASE, but that address isn't free in
 * every process (on the Quest, ART's large-object space and JIT cache cover it), so the host puts
 * the image wherever host_memory_place_image finds room. Code reaches the image PC-relatively,
 * so moving it means adding the difference to the absolute pointer words that .guest_relocs
 * lists (written by port/tools/guest_relocs.py). */
#include "host.h"

#include <elf.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>

struct host_image host_image;

static void missing_import(void)
{
    host_fatal("the guest called a host function the host does not have");
}

static const Elf64_Shdr *find_section(const void *elf, size_t size, const char *wanted)
{
    const Elf64_Ehdr *eh = elf;
    if (!eh->e_shoff || eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > size ||
        eh->e_shstrndx >= eh->e_shnum)
        return NULL;
    const Elf64_Shdr *sh = (const Elf64_Shdr *)((const char *)elf + eh->e_shoff);
    const Elf64_Shdr *names = &sh[eh->e_shstrndx];
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_name >= names->sh_size) continue;
        if (!strcmp((const char *)elf + names->sh_offset + sh[i].sh_name, wanted)) return &sh[i];
    }
    return NULL;
}

/* adds delta to every absolute pointer word; returns 0 or -ENOEXEC */
static int relocate(const void *elf, size_t size, uint64_t load, uint64_t span, uint32_t delta)
{
    const Elf64_Shdr *s = find_section(elf, size, ".guest_relocs");
    if (!s || s->sh_offset + s->sh_size > size || s->sh_size < 12) {
        host_error("loader: the image has no .guest_relocs table");
        return -ENOEXEC;
    }
    const uint32_t *table = (const uint32_t *)((const char *)elf + s->sh_offset);
    uint32_t abs32 = table[1], abs64 = table[2];
    if (memcmp(table, "GRL1", 4) || 12 + 4 * ((uint64_t)abs32 + abs64) > s->sh_size) {
        host_error("loader: bad .guest_relocs table");
        return -ENOEXEC;
    }
    const uint32_t *offset = table + 3;
    for (uint32_t i = 0; i < abs32; i++, offset++) {
        if (*offset + 4ull > span) return -ENOEXEC;
        uint32_t *word = (uint32_t *)(uintptr_t)(load + *offset);
        *word += delta; /* wraps modulo 2^32: every guest address is 32-bit */
    }
    for (uint32_t i = 0; i < abs64; i++, offset++) {
        if (*offset + 8ull > span) return -ENOEXEC;
        uint64_t *word = (uint64_t *)(uintptr_t)(load + *offset);
        *word = (uint32_t)(*word + delta);
    }
    host_info("loader: moved %u + %u pointer words", abs32, abs64);
    return 0;
}

int host_load_image(const void *elf, size_t size)
{
    const Elf64_Ehdr *eh = elf;
    if (size < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_machine != EM_AARCH64 || eh->e_type != ET_EXEC) {
        host_error("loader: not an AArch64 static executable");
        return -ENOEXEC;
    }
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const char *)elf + eh->e_phoff);
    uint64_t low = UINT64_MAX, high = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < low) low = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > high) high = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (low != BO1_GUEST_LINK_BASE || high > 0x100000000ull) {
        host_error("loader: image spans %#llx-%#llx, expected to start at %#x", (unsigned long long)low,
                   (unsigned long long)high, BO1_GUEST_LINK_BASE);
        return -ENOEXEC;
    }

    uint64_t span = (high - low + BO1_GUEST_IMAGE_ALIGN - 1) & ~(uint64_t)(BO1_GUEST_IMAGE_ALIGN - 1);
    uint64_t load = host_memory_place_image(span, BO1_GUEST_IMAGE_ALIGN);
    if (!load) return -ENOMEM;
    uint32_t delta = (uint32_t)(load - low);
    if (mmap((void *)(uintptr_t)load, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
             0) == MAP_FAILED)
        return -errno;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_offset + ph[i].p_filesz > size) return -ENOEXEC;
        memcpy((void *)(uintptr_t)(ph[i].p_vaddr + delta), (const char *)elf + ph[i].p_offset, ph[i].p_filesz);
    }
    int r = relocate(elf, size, load, span, delta);
    if (r) return r;

    const struct bo1_guest_header *header = (const void *)(uintptr_t)load;
    if (header->magic != BO1_GUEST_MAGIC || header->abi_version != BO1_GUEST_ABI_VERSION) {
        host_error("loader: bad guest header (magic %#x, ABI %u)", header->magic, header->abi_version);
        return -ENOEXEC;
    }
    host_image.header = header;
    host_image.base = (uint32_t)load;
    host_image.end = header->image_end;

    uint32_t count = *GUEST_PTR(const uint32_t *, header->import_count);
    uint64_t *table = GUEST_PTR(uint64_t *, header->import_table);
    const char *name = GUEST_PTR(const char *, header->import_names);
    int missing = 0;
    for (uint32_t i = 0; i < count; i++, name += strlen(name) + 1) {
        void (*f)(void) = host_resolve_import(name);
        if (!f) {
            host_error("loader: no host function %s", name);
            f = missing_import;
            missing++;
        }
        table[i] = (uint64_t)(uintptr_t)f;
    }

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uint64_t start = (ph[i].p_vaddr + delta) & ~(uint64_t)0xfff;
        uint64_t end = (ph[i].p_vaddr + delta + ph[i].p_memsz + 0xfff) & ~(uint64_t)0xfff;
        int prot = (ph[i].p_flags & PF_R ? PROT_READ : 0) | (ph[i].p_flags & PF_W ? PROT_WRITE : 0) |
                   (ph[i].p_flags & PF_X ? PROT_EXEC : 0);
        if (mprotect((void *)(uintptr_t)start, end - start, prot)) return -errno;
        if (prot & PROT_EXEC) __builtin___clear_cache((char *)(uintptr_t)start, (char *)(uintptr_t)end);
    }
    host_info("loader: guest image at %#llx-%#llx (linked at %#x), %u imports%s", (unsigned long long)load,
              (unsigned long long)(load + (high - low)), BO1_GUEST_LINK_BASE, count,
              missing ? " (some missing)" : "");
    return 0;
}
