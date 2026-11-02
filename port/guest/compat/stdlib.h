/* musl's stdlib.h without its POSIX random(): the engine has its own random(), and MSVC's CRT
   has none. */
#define random __musl_random
#include_next <stdlib.h>
#undef random
