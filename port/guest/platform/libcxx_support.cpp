/* The out-of-line parts of libc++ the engine uses. The guest links no libc++ library (only its
 * headers), so the pieces normally compiled into libc++.so are made here: std::string's
 * members, std::sort's specialisations for built-in types, and a few helpers. */
#include <algorithm>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

/* every member of std::string, from the header's own definitions */
template class std::basic_string<char>;

/* std::sort on the arithmetic types: libc++ declares __sort and keeps its body in the library.
   Its __less<T, T> argument is only a tag naming plain `<`. A heap sort: O(n log n), no recursion. */
namespace std {
inline namespace __ndk1 {
template <class _Comp, class _RandomAccessIterator>
void __sort(_RandomAccessIterator first, _RandomAccessIterator last, _Comp)
{
    std::make_heap(first, last, std::less<>());
    std::sort_heap(first, last, std::less<>());
}
} // namespace __ndk1
} // namespace std
template void std::__sort<std::__less<int, int> &, int *>(int *, int *, std::__less<int, int> &);
template void std::__sort<std::__less<unsigned int, unsigned int> &, unsigned int *>(unsigned int *, unsigned int *,
                                                                                    std::__less<unsigned int, unsigned int> &);
template void std::__sort<std::__less<float, float> &, float *>(float *, float *, std::__less<float, float> &);
template void std::__sort<std::__less<double, double> &, double *>(double *, double *, std::__less<double, double> &);

namespace std {
extern const nothrow_t nothrow; /* outside libc++'s inline namespace, as <new> declares it */
const nothrow_t nothrow{};

inline namespace __ndk1 {

/* the smallest prime >= n (unordered containers' bucket counts) */
size_t __next_prime(size_t n)
{
    if (n <= 2) return 2;
    if (!(n & 1)) n++;
    for (;; n += 2) {
        bool prime = true;
        for (size_t d = 3; d * d <= n; d += 2) {
            if (n % d == 0) {
                prime = false;
                break;
            }
        }
        if (prime) return n;
    }
}

string to_string(int value)
{
    char text[16];
    snprintf(text, sizeof text, "%d", value);
    return string(text);
}

string to_string(unsigned value)
{
    char text[16];
    snprintf(text, sizeof text, "%u", value);
    return string(text);
}

void __libcpp_verbose_abort(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

} // namespace __ndk1
} // namespace std
