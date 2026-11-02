/* Helpers clang emits calls to for its Darwin target, which musl does not provide. */
#include <math.h>

struct sincos_double { double s, c; };
struct sincos_float { float s, c; };

/* Clang turns a sin and cos of the same value into one __sincos_stret call on this target, so
   calling sin() and cos() here directly would compile into a call to this very function and
   recurse until the stack runs out. Through pointers it can't see the pair. */
static double (*volatile sin_fn)(double) = sin;
static double (*volatile cos_fn)(double) = cos;
static float (*volatile sinf_fn)(float) = sinf;
static float (*volatile cosf_fn)(float) = cosf;

struct sincos_double __sincos_stret(double x)
{
    struct sincos_double r = {sin_fn(x), cos_fn(x)};
    return r;
}

struct sincos_float __sincosf_stret(float x)
{
    struct sincos_float r = {sinf_fn(x), cosf_fn(x)};
    return r;
}

/* not pow(10, x): for this (Apple) target the compiler turns pow(10, x) into a call to __exp10 -
   itself - and compiled these to a trap. exp(x ln 10) it leaves alone. */
double __exp10(double x)
{
    return exp(x * 2.302585092994045684);
}

float __exp10f(float x)
{
    return expf(x * 2.302585093f);
}
