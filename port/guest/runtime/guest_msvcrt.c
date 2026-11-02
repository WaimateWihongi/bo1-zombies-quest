/* MSVC CRT functions the engine calls that need more than a macro */
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

void *_aligned_realloc(void *ptr, size_t size, size_t alignment)
{
    if (!ptr) {
        void *p;
        if (alignment < sizeof(void *)) alignment = sizeof(void *);
        return posix_memalign(&p, alignment, size) ? NULL : p;
    }
    if (!size) {
        free(ptr);
        return NULL;
    }
    /* realloc keeps malloc's 16-byte alignment; larger alignments need a copy */
    if (alignment <= 16) return realloc(ptr, size);
    void *p;
    if (posix_memalign(&p, alignment, size)) return NULL;
    size_t old = malloc_usable_size(ptr);
    memcpy(p, ptr, old < size ? old : size);
    free(ptr);
    return p;
}
