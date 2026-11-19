/* Replacements for the PowerPC fres and frsqrte estimates, written for this project.
   Real hardware returns a low-precision estimate from a table. These return the exact
   result instead, which is within the accuracy the architecture promises, but the low
   bits can differ from a real Wii. Special cases follow the PowerPC manual. */
#include <math.h>
#include <string.h>
#include "cpu/cpu.h"

static u64 bits_of(f64 v) { u64 b; memcpy(&b, &v, 8); return b; }
static f64 from_bits(u64 b) { f64 v; memcpy(&v, &b, 8); return v; }

#define SIGN_BIT 0x8000000000000000ull
#define INF_BITS 0x7FF0000000000000ull
#define QNAN_BITS 0x7FF8000000000000ull
#define QUIET_BIT 0x0008000000000000ull

f64 ppc_approx_reciprocal(f64 value) {
    u64 b = bits_of(value);
    if (isnan(value)) return from_bits(b | QUIET_BIT);
    if (value == 0.0) return from_bits((b & SIGN_BIT) | INF_BITS);
    if (isinf(value)) return from_bits(b & SIGN_BIT);
    return 1.0 / value;
}

f64 ppc_approx_rsqrt(f64 value) {
    u64 b = bits_of(value);
    if (isnan(value)) return from_bits(b | QUIET_BIT);
    if (value == 0.0) return from_bits((b & SIGN_BIT) | INF_BITS);
    if (b & SIGN_BIT) return from_bits(QNAN_BITS);
    if (isinf(value)) return 0.0;
    return 1.0 / sqrt(value);
}
