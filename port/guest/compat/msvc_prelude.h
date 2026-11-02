/* Force-included into every engine file (-include): what MSVC's headers made visible without
   an explicit include, and its spellings of standard names. */
#pragma once

/* musl's strings.h declares index(), which the engine uses as a variable name */
#define index bo1_libc_index
#include <strings.h>
#undef index
#include <float.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
/* Windows.h brings in Winsock; the engine's network code uses the BSD names */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#ifdef __cplusplus
#include <new>
#endif

/* The engine passes its jmp_buf storage as int* and void* (MSVC's jmp_buf is int[16]) */
#include <setjmp.h>
#undef setjmp
#define setjmp(b) setjmp((struct __jmp_buf_tag *)(b))
#define _setjmp(b) _setjmp((struct __jmp_buf_tag *)(b))
#define longjmp(b, v) longjmp((struct __jmp_buf_tag *)(b), (v))

/* fopen with MSVC's text mode: "
" reads as "
" when there's no 'b' (platform/win32_crt.cpp) */
#ifdef __cplusplus
extern "C"
#endif
FILE *bo1_fopen(const char *path, const char *mode);
#define fopen(path, mode) bo1_fopen((path), (mode))

/* MSVC's rand(), whose 15-bit range (RAND_MAX 0x7FFF) the engine hard-codes (platform/win32_crt.cpp) */
#ifdef __cplusplus
extern "C" {
#endif
int bo1_rand(void);
void bo1_srand(unsigned int seed);
#ifdef __cplusplus
}
#endif
#undef RAND_MAX
#define RAND_MAX 0x7FFF
#define rand() bo1_rand()
#define srand(seed) bo1_srand(seed)

#define _iobuf _IO_FILE
#define _time64 time
#define __time64_t time_t
#define _vsnprintf vsnprintf
#define _vscprintf(f, a) vsnprintf(NULL, 0, (f), (a))
#define _fileno fileno
#define _unlink unlink
#define _access access
#define _mkdir(p) mkdir((p), 0777)
#define _getcwd getcwd
#define _isnan isnan
#define _finite isfinite
#define _hypot hypot

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

/* the MSVC "secure" CRT: same results as the plain calls, always NUL-terminated */
#define _TRUNCATE ((size_t)-1)
typedef int errno_t;
static inline int vsprintf_s(char *buffer, size_t size, const char *format, va_list ap)
{
    int n = vsnprintf(buffer, size, format, ap);
    return n < 0 || (size_t)n >= size ? -1 : n;
}
static inline int sprintf_s(char *buffer, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsprintf_s(buffer, size, format, ap);
    va_end(ap);
    return n;
}
static inline int _snprintf_s(char *buffer, size_t size, size_t count, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    size_t limit = count == _TRUNCATE || count + 1 > size ? size : count + 1;
    int n = vsnprintf(buffer, limit, format, ap);
    va_end(ap);
    return n < 0 || (size_t)n >= limit ? -1 : n;
}
static inline errno_t strcpy_s(char *dest, size_t size, const char *src)
{
    if (!dest || !size) return EINVAL;
    size_t n = strlen(src);
    if (n >= size) {
        dest[0] = 0;
        return ERANGE;
    }
    memcpy(dest, src, n + 1);
    return 0;
}
static inline errno_t strncpy_s(char *dest, size_t size, const char *src, size_t count)
{
    if (!dest || !size) return EINVAL;
    size_t n = strnlen(src, count == _TRUNCATE ? size - 1 : count);
    if (n >= size) n = size - 1;
    memcpy(dest, src, n);
    dest[n] = 0;
    return 0;
}
static inline errno_t strcat_s(char *dest, size_t size, const char *src)
{
    size_t used = strnlen(dest, size);
    return used >= size ? EINVAL : strcpy_s(dest + used, size - used, src);
}
static inline errno_t fopen_s(FILE **file, const char *name, const char *mode)
{
    *file = fopen(name, mode);
    return *file ? 0 : errno;
}
static inline char *_itoa(int value, char *buffer, int radix)
{
    if (radix == 16) sprintf(buffer, "%x", (unsigned)value);
    else sprintf(buffer, "%d", value);
    return buffer;
}
#define _atoi64 atoll
#define strtok_s strtok_r
#define sscanf_s sscanf
#define _itoa_s(v, b, n, r) (_itoa((v), (b), (r)), 0)
#define _gmtime64 gmtime
#define _localtime64 localtime
#define _ctime64 ctime
#define _mktime64 mktime
#define _errno() (&errno)
#define _strlwr(s) bo1_strlwr(s)
#define _strupr(s) bo1_strupr(s)
static inline char *bo1_strlwr(char *s)
{
    for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    return s;
}
static inline char *bo1_strupr(char *s)
{
    for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
    return s;
}
#define _strtoui64 strtoull
#define _putenv(s) putenv((char *)(s))
typedef int (*_CoreCrtNonSecureSearchSortCompareFunction)(const void *, const void *);
static inline char *_ui64toa(unsigned long long value, char *buffer, int radix)
{
    if (radix == 16) sprintf(buffer, "%llx", value);
    else sprintf(buffer, "%llu", value);
    return buffer;
}
/* MSVC's argument order: the result first */
static inline errno_t localtime_s(struct tm *result, const time_t *t)
{
    return localtime_r(t, result) ? 0 : EINVAL;
}

/* libc++'s <locale> calls the _l forms, which musl lacks; the guest only uses the C locale */
#include <locale.h>
static inline float strtof_l(const char *s, char **end, locale_t loc) { (void)loc; return strtof(s, end); }
static inline double strtod_l(const char *s, char **end, locale_t loc) { (void)loc; return strtod(s, end); }
static inline long double strtold_l(const char *s, char **end, locale_t loc) { (void)loc; return strtold(s, end); }

#ifdef __cplusplus
/* the array forms, which take the size from the array */
template <size_t N> static inline int sprintf_s(char (&buffer)[N], const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsprintf_s(buffer, N, format, ap);
    va_end(ap);
    return n;
}
template <size_t N> static inline int vsprintf_s(char (&buffer)[N], const char *format, va_list ap)
{
    return vsprintf_s(buffer, N, format, ap);
}
template <size_t N> static inline int _snprintf_s(char (&buffer)[N], size_t count, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    size_t limit = count == _TRUNCATE || count + 1 > N ? N : count + 1;
    int n = vsnprintf(buffer, limit, format, ap);
    va_end(ap);
    return n < 0 || (size_t)n >= limit ? -1 : n;
}
template <size_t N> static inline errno_t strcpy_s(char (&dest)[N], const char *src) { return strcpy_s(dest, N, src); }
template <size_t N> static inline errno_t strcat_s(char (&dest)[N], const char *src) { return strcat_s(dest, N, src); }
template <size_t N> static inline errno_t strncpy_s(char (&dest)[N], const char *src, size_t count)
{
    return strncpy_s(dest, N, src, count);
}
#endif
