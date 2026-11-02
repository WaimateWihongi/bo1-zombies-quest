/* The MSVC C runtime's text mode, which the engine relies on.
 *
 * On Windows a file opened without 'b' reads "\r\n" as "\n". Game files copied from the PC keep
 * their CRLF line ends, and the engine's parsers expect them gone (localization.txt's first line
 * names the language; with the '\r' left in, "english\r" finds no zone folder). Every fopen in the
 * engine comes here (msvc_prelude.h): a read-only text-mode open loads the file, drops the '\r'
 * of each "\r\n", and serves the result from a memory stream. Anything else is a plain fopen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" FILE *bo1_fopen(const char *path, const char *mode)
{
    bool text_read = mode[0] == 'r' && !strchr(mode, 'b') && !strchr(mode, '+');
    if (!text_read) return (fopen)(path, mode);

    FILE *raw = (fopen)(path, "rb");
    if (!raw) return NULL;
    size_t capacity = 65536, size = 0;
    char *data = (char *)malloc(capacity);
    for (size_t n; data && (n = fread(data + size, 1, capacity - size, raw)) > 0;) {
        size += n;
        if (size == capacity) {
            char *bigger = (char *)realloc(data, capacity *= 2);
            if (!bigger) {
                free(data);
                data = NULL;
            }
            data = bigger;
        }
    }
    fclose(raw);
    if (!data) return NULL;

    size_t kept = 0;
    for (size_t i = 0; i < size; i++)
        if (!(data[i] == '\r' && i + 1 < size && data[i + 1] == '\n')) data[kept++] = data[i];

    /* a stream that owns its buffer (freed by fclose), holding exactly the translated text */
    FILE *stream = fmemopen(NULL, kept ? kept : 1, "w+");
    if (stream) {
        if (kept) fwrite(data, 1, kept, stream);
        rewind(stream);
    }
    free(data);
    return stream;
}

/* MSVC's rand(): a per-thread linear congruential generator giving 15 bits (RAND_MAX 0x7FFF).
 * The engine hard-codes that range ("rand() / 32767.0", "rand() - 0x3FFF"); with musl's 31-bit
 * rand(), crandom() returned values far outside [-1, 1] and GaussianRandom's "inside the unit
 * circle" loop never ended. Same constants as MSVC, so sequences match the PC game's. */
static thread_local unsigned int t_rand_state = 1;

extern "C" int bo1_rand(void)
{
    t_rand_state = t_rand_state * 214013u + 2531011u;
    return (int)((t_rand_state >> 16) & 0x7FFF);
}

extern "C" void bo1_srand(unsigned int seed) { t_rand_state = seed; }
