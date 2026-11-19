/* Fast paths for common float and paired-single work. Translated chunks call these through fastmem.h. */
#define FAST_OPS_BUILD
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include "fastmem.h"

/* --nan-trace: the first NaNs and infinities made from ordinary numbers by a float operation, with where. One
   that only passes along isn't reported, so the first lines point at where one starts. Only slow paths check. */
extern FILE* report;
int fast_nan_trace;
static unsigned nan_reports;

static inline int fast_is_nan(double v) { return v != v; }

u32 host_field(void);

/* --nan-trace N: from video field N on, so the title screen's own NaNs don't use up the reports. */
static void nan_made(CPUState* cpu, const char* op, double a, double b, double c, double result) {
    static struct {
        u32 pc, lr;
        const char* op;
    } seen[512];
    if (host_field() < (u32)fast_nan_trace || nan_reports >= 200 || !report) return;
    for (unsigned i = 0; i < nan_reports; i++) /* each place once, so one busy place can't hide the rest */
        if (seen[i].pc == cpu->pc && seen[i].lr == cpu->lr && seen[i].op == op) return;
    seen[nan_reports].pc = cpu->pc;
    seen[nan_reports].lr = cpu->lr;
    seen[nan_reports].op = op;
    nan_reports++;
    fprintf(report, "NaN or infinity made by %s at block %08X lr %08X field %u: %.9g %.9g %.9g -> %g\n", op, cpu->pc, cpu->lr, host_field(), a, b, c, result);
}

#define NAN_WATCH(op, a, b, c, result)                                                                  \
    do {                                                                                                \
        if (fast_nan_trace && (fast_is_nan(result) || isinf(result)) && isfinite(a) && isfinite(b) && isfinite(c)) \
            nan_made(cpu, op, a, b, c, result);                                                        \
    } while (0)

/* Paired singles are mostly plain floats in main memory. That case runs here, and everything else, such
   as scaled integers, odd alignment, or denormals and NaNs, goes to the full helpers in cpu_core.c. */
#define FAST_PS_ENABLED 0xA0000000u /* HID2 LSQE and PSE */

static inline int fast_ps_plain(CPUState* cpu, u32 type, u32 ea) {
    return type == 0 && (ea & 3u) == 0 && (cpu->hid2 & FAST_PS_ENABLED) == FAST_PS_ENABLED;
}

/* A normal single or a zero: both convert to double exactly, as the helper does. */
static inline int fast_f32_normal(u32 bits) {
    u32 exponent = (bits >> 23) & 0xFFu;
    return (exponent != 0 && exponent != 0xFFu) || (bits << 1) == 0;
}

static inline double fast_f32_extend(u32 bits) {
    float value;
    memcpy(&value, &bits, 4);
    return value;
}

static inline u32 fast_f32_narrow(double value) {
    float single = (float)value;
    u32 bits;
    memcpy(&bits, &single, 4);
    return (bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0 ? 0u : bits; /* denormals become zero */
}

/* Scaled integers. These hold at most 16 bits and the scale is at most 2^32 either way, so the product is
   exact and matches the helper's ldexp. */
static inline double fast_ps_scale(u32 gqr) {
    s32 scale = (s32)((gqr >> 24) & 0x3Fu);
    u64 bits;
    double value;
    if (scale & 0x20) scale -= 0x40;
    bits = (u64)(1023 - scale) << 52;
    memcpy(&value, &bits, 8);
    return value;
}

static inline double fast_ps_int(const u8* p, u32 type) {
    switch (type) {
    case 4: return p[0];
    case 5: return (u16)((p[0] << 8) | p[1]);
    case 6: return (s8)p[0];
    default: return (s16)((p[0] << 8) | p[1]);
    }
}

bool fast_psq_load(CPUState* cpu, u8 d, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
    u32 quant = cpu->gqr[gqr & 7u], type = (quant >> 16) & 7u;
    if (type >= 4u && (cpu->hid2 & FAST_PS_ENABLED) == FAST_PS_ENABLED) {
        u32 size = type & 1u ? 2u : 1u;
        const u8* p = fast_host(cpu, ea, w ? size : size * 2u);
        if (p) {
            double scale = fast_ps_scale(quant);
            cpu->fpr[d] = fast_ps_int(p, type) * scale;
            cpu->ps1[d] = w ? 1.0 : fast_ps_int(p + size, type) * scale;
            return true;
        }
    }
    if (fast_ps_plain(cpu, type, ea)) {
        const u8* p = fast_host(cpu, ea, w ? 4u : 8u);
        if (p) {
            u32 first, second = 0;
            memcpy(&first, p, 4);
            first = __builtin_bswap32(first);
            if (!w) {
                memcpy(&second, p + 4, 4);
                second = __builtin_bswap32(second);
            }
            if (fast_f32_normal(first) && (w || fast_f32_normal(second))) {
                cpu->fpr[d] = fast_f32_extend(first);
                cpu->ps1[d] = w ? 1.0 : fast_f32_extend(second);
                return true;
            }
        }
    }
    return ppc_psq_load(cpu, d, ea, w, gqr, indexed, cia);
}

bool fast_psq_store(CPUState* cpu, u8 s, u32 ea, bool w, u8 gqr, bool indexed, u32 cia) {
    if (fast_ps_plain(cpu, cpu->gqr[gqr & 7u] & 7u, ea)) {
        u8* p = fast_host(cpu, ea, w ? 4u : 8u);
        if (p) {
            u32 first = __builtin_bswap32(fast_f32_narrow(cpu->fpr[s]));
            fast_store_check(cpu, ea);
            memcpy(p, &first, 4);
            if (!w) {
                u32 second = __builtin_bswap32(fast_f32_narrow(cpu->ps1[s]));
                fast_store_check(cpu, ea + 4u);
                memcpy(p + 4, &second, 4);
            }
            return true;
        }
    }
    return ppc_psq_store(cpu, s, ea, w, gqr, indexed, cia);
}

/* Common float arithmetic. The result is the same as the helpers', which do the operation in double and
   round to single. A result that isn't an ordinary number goes to the helper, which handles NaNs, infinities,
   flushing, and exceptions. Not updated here: the FPSCR's status bits (FI, FR, FPRF). Nothing in this game
   reads them: see docs/10-native-runtime.md. */
static inline u32 fast_f32_exponent(float value) {
    u32 bits;
    memcpy(&bits, &value, 4);
    return (bits >> 23) & 0xFFu;
}

static inline int fast_single_normal(float value) {
    u32 exponent = fast_f32_exponent(value);
    return exponent != 0 && exponent != 0xFFu;
}

/* The hardware multiplier only uses 25 bits of its second input. */
static inline int fast_25_ok(double value) {
    u64 bits;
    memcpy(&bits, &value, 8);
    return (bits & 0x7FF0000000000000ull) != 0 || (bits << 12) == 0;
}

static inline double fast_25(double value) {
    u64 bits;
    memcpy(&bits, &value, 8);
    bits = (bits & 0xFFFFFFFFF8000000ull) + (bits & 0x0000000008000000ull);
    memcpy(&value, &bits, 8);
    return value;
}

/* A double result at least as large as the smallest normal single. Below that, the helper decides: with the
   FPSCR's NI bit set (this game sets it) the hardware flushes to zero before rounding, so a value just under
   the smallest normal becomes zero rather than rounding up to it. */
static inline int fast_single_big_enough(double value) {
    u64 bits;
    memcpy(&bits, &value, 8);
    return (bits & 0x7FFFFFFFFFFFFFFFull) >= 0x3810000000000000ull;
}

/* Either a result the helper would round the same way, or an exact zero, which it passes through with its
   sign, as the host does. A tiny non-zero result still goes to the helper, because NI flushes it. */
static inline int fast_single_ok(double exact, float value) {
    u64 bits;
    memcpy(&bits, &exact, 8);
    if ((bits << 1) == 0) return 1;
    return fast_single_big_enough(exact) && fast_single_normal(value);
}

static inline void fast_single_result(CPUState* cpu, u8 d, float value) {
    cpu->fpr[d] = value;
    cpu->ps1[d] = value;
}

void fast_fadds(CPUState* cpu, u8 d, u8 a, u8 b) {
    double exact = cpu->fpr[a] + cpu->fpr[b];
    float value = (float)exact;
    if (fast_single_ok(exact, value)) fast_single_result(cpu, d, value);
    else {
        double in_a = cpu->fpr[a], in_b = cpu->fpr[b];
        ppc_fadds(cpu, d, a, b);
        NAN_WATCH("fadds", in_a, in_b, 0.0, cpu->fpr[d]);
    }
}

void fast_fsubs(CPUState* cpu, u8 d, u8 a, u8 b) {
    double exact = cpu->fpr[a] - cpu->fpr[b];
    float value = (float)exact;
    if (fast_single_ok(exact, value)) fast_single_result(cpu, d, value);
    else {
        double in_a = cpu->fpr[a], in_b = cpu->fpr[b];
        ppc_fsubs(cpu, d, a, b);
        NAN_WATCH("fsubs", in_a, in_b, 0.0, cpu->fpr[d]);
    }
}

void fast_fmuls(CPUState* cpu, u8 d, u8 a, u8 c) {
    double exact = cpu->fpr[a] * fast_25(cpu->fpr[c]);
    float value = (float)exact;
    if (fast_25_ok(cpu->fpr[c]) && fast_single_ok(exact, value)) fast_single_result(cpu, d, value);
    else {
        double in_a = cpu->fpr[a], in_b = cpu->fpr[c];
        ppc_fmuls(cpu, d, a, c);
        NAN_WATCH("fmuls", in_a, in_b, 0.0, cpu->fpr[d]);
    }
}

/* Paired results have no flushing, so only a NaN or infinity needs the helper. */
static inline int fast_pair_ok(float first, float second) {
    return fast_f32_exponent(first) != 0xFFu && fast_f32_exponent(second) != 0xFFu;
}

static inline void fast_pair_result(CPUState* cpu, u8 d, float first, float second) {
    cpu->fpr[d] = first;
    cpu->ps1[d] = second;
}

void fast_ps_add(CPUState* cpu, u8 d, u8 a, u8 b) {
    float first = (float)(cpu->fpr[a] + cpu->fpr[b]), second = (float)(cpu->ps1[a] + cpu->ps1[b]);
    if (fast_pair_ok(first, second)) fast_pair_result(cpu, d, first, second);
    else {
        double a0 = cpu->fpr[a], b0 = cpu->fpr[b], a1 = cpu->ps1[a], b1 = cpu->ps1[b];
        ppc_ps_add_op(cpu, d, a, b);
        NAN_WATCH("ps_add slot 0", a0, b0, 0.0, cpu->fpr[d]);
        NAN_WATCH("ps_add slot 1", a1, b1, 0.0, cpu->ps1[d]);
    }
}

void fast_ps_sub(CPUState* cpu, u8 d, u8 a, u8 b) {
    float first = (float)(cpu->fpr[a] - cpu->fpr[b]), second = (float)(cpu->ps1[a] - cpu->ps1[b]);
    if (fast_pair_ok(first, second)) fast_pair_result(cpu, d, first, second);
    else {
        double a0 = cpu->fpr[a], b0 = cpu->fpr[b], a1 = cpu->ps1[a], b1 = cpu->ps1[b];
        ppc_ps_sub_op(cpu, d, a, b);
        NAN_WATCH("ps_sub slot 0", a0, b0, 0.0, cpu->fpr[d]);
        NAN_WATCH("ps_sub slot 1", a1, b1, 0.0, cpu->ps1[d]);
    }
}

void fast_ps_mul(CPUState* cpu, u8 d, u8 a, u8 c) {
    float first = (float)(cpu->fpr[a] * fast_25(cpu->fpr[c])), second = (float)(cpu->ps1[a] * fast_25(cpu->ps1[c]));
    if (fast_25_ok(cpu->fpr[c]) && fast_25_ok(cpu->ps1[c]) && fast_pair_ok(first, second))
        fast_pair_result(cpu, d, first, second);
    else {
        double a0 = cpu->fpr[a], b0 = cpu->fpr[c], a1 = cpu->ps1[a], b1 = cpu->ps1[c];
        ppc_ps_mul_op(cpu, d, a, c);
        NAN_WATCH("ps_mul slot 0", a0, b0, 0.0, cpu->fpr[d]);
        NAN_WATCH("ps_mul slot 1", a1, b1, 0.0, cpu->ps1[d]);
    }
}

void fast_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c) {
    double scalar = fast_25(cpu->fpr[c]);
    float first = (float)(cpu->fpr[a] * scalar), second = (float)(cpu->ps1[a] * scalar);
    if (fast_25_ok(cpu->fpr[c]) && fast_pair_ok(first, second)) fast_pair_result(cpu, d, first, second);
    else {
        double a0 = cpu->fpr[a], b0 = cpu->fpr[c], a1 = cpu->ps1[a], b1 = cpu->ps1[c];
        ppc_ps_muls0(cpu, d, a, c);
        NAN_WATCH("ps_muls0 slot 0", a0, b0, 0.0, cpu->fpr[d]);
        NAN_WATCH("ps_muls0 slot 1", a1, b1, 0.0, cpu->ps1[d]);
    }
}

/* Single divide, done in double and rounded to single like the helper. */
void fast_fdivs(CPUState* cpu, u8 d, u8 a, u8 b) {
    double exact = cpu->fpr[a] / cpu->fpr[b];
    float value = (float)exact;
    if (fast_single_ok(exact, value)) fast_single_result(cpu, d, value);
    else {
        double in_a = cpu->fpr[a], in_b = cpu->fpr[b];
        ppc_fdivs(cpu, d, a, b);
        NAN_WATCH("fdivs", in_a, in_b, 0.0, cpu->fpr[d]);
    }
}

/* Double results: only a NaN, an infinity, or a denormal (which NI may flush) needs the helper. A double
   result leaves ps1 alone, as the helper does. */
static inline int fast_double_ok(double value) {
    u64 bits;
    memcpy(&bits, &value, 8);
    u64 exponent = (bits >> 52) & 0x7FFu;
    return (exponent != 0 && exponent != 0x7FFu) || (bits << 1) == 0;
}

void fast_fadd(CPUState* cpu, u8 d, u8 a, u8 b) {
    double value = cpu->fpr[a] + cpu->fpr[b];
    if (fast_double_ok(value)) cpu->fpr[d] = value;
    else {
        double in_a = cpu->fpr[a], in_b = cpu->fpr[b];
        ppc_fadd(cpu, d, a, b);
        NAN_WATCH("fadd", in_a, in_b, 0.0, cpu->fpr[d]);
    }
}

void fast_fsub(CPUState* cpu, u8 d, u8 a, u8 b) {
    double value = cpu->fpr[a] - cpu->fpr[b];
    if (fast_double_ok(value)) cpu->fpr[d] = value;
    else {
        double in_a = cpu->fpr[a], in_b = cpu->fpr[b];
        ppc_fsub(cpu, d, a, b);
        NAN_WATCH("fsub", in_a, in_b, 0.0, cpu->fpr[d]);
    }
}

void fast_fmul(CPUState* cpu, u8 d, u8 a, u8 c) {
    double value = cpu->fpr[a] * cpu->fpr[c];
    if (fast_double_ok(value)) cpu->fpr[d] = value;
    else {
        double in_a = cpu->fpr[a], in_b = cpu->fpr[c];
        ppc_fmul(cpu, d, a, c);
        NAN_WATCH("fmul", in_a, in_b, 0.0, cpu->fpr[d]);
    }
}

/* Multiply-add. The helper fuses in double, with the second input cut to 25 bits for singles, and then
   fixes up a result that sits exactly halfway between two singles. That case, and anything unusual, goes
   to the helper. */
static inline int fast_single_tie(double value) {
    u64 bits;
    memcpy(&bits, &value, 8);
    return (bits & 0x1FFFFFFFull) == 0x10000000ull;
}

bool fast_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative, f64* output) {
    f64 addend = subtract ? -b : b;
    if (single) {
        if (fast_25_ok(c)) {
            double result = fma(a, fast_25(c), addend);
            float value = (float)result;
            if (!fast_single_tie(result) && fast_single_normal(value)) {
                *output = negative ? -(double)value : (double)value;
                return true;
            }
        }
    } else {
        double result = fma(a, c, addend);
        if (fast_double_ok(result)) {
            *output = negative ? -result : result;
            return true;
        }
    }
    bool done = ppc_fma(cpu, a, c, b, single, subtract, negative, output);
    if (done) NAN_WATCH(single ? "fmadds family" : "fmadd family", a, c, b, *output);
    return done;
}

static inline int fast_pair_fma(double a0, double c0, double b0, double a1, double c1, double b1, float* out0,
                                float* out1) {
    if (!fast_25_ok(c0) || !fast_25_ok(c1)) return 0;
    double r0 = fma(a0, fast_25(c0), b0), r1 = fma(a1, fast_25(c1), b1);
    if (fast_single_tie(r0) || fast_single_tie(r1)) return 0;
    *out0 = (float)r0;
    *out1 = (float)r1;
    return fast_pair_ok(*out0, *out1);
}

void fast_ps_madd(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract, bool negative) {
    float first, second;
    double b0 = subtract ? -cpu->fpr[b] : cpu->fpr[b], b1 = subtract ? -cpu->ps1[b] : cpu->ps1[b];
    if (fast_pair_fma(cpu->fpr[a], cpu->fpr[c], b0, cpu->ps1[a], cpu->ps1[c], b1, &first, &second)) {
        if (negative) {
            first = -first;
            second = -second;
        }
        fast_pair_result(cpu, d, first, second);
    } else {
        double a0 = cpu->fpr[a], c0 = cpu->fpr[c], e0 = cpu->fpr[b], a1 = cpu->ps1[a], c1 = cpu->ps1[c], e1 = cpu->ps1[b];
        ppc_ps_madd_op(cpu, d, a, c, b, subtract, negative);
        NAN_WATCH("ps_madd family slot 0", a0, c0, e0, cpu->fpr[d]);
        NAN_WATCH("ps_madd family slot 1", a1, c1, e1, cpu->ps1[d]);
    }
}

void fast_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    float first, second;
    if (fast_pair_fma(cpu->fpr[a], cpu->fpr[c], cpu->fpr[b], cpu->ps1[a], cpu->fpr[c], cpu->ps1[b], &first, &second))
        fast_pair_result(cpu, d, first, second);
    else {
        double a0 = cpu->fpr[a], c0 = cpu->fpr[c], e0 = cpu->fpr[b], a1 = cpu->ps1[a], c1 = cpu->fpr[c], e1 = cpu->ps1[b];
        ppc_ps_madds0(cpu, d, a, c, b);
        NAN_WATCH("ps_madds0 slot 0", a0, c0, e0, cpu->fpr[d]);
        NAN_WATCH("ps_madds0 slot 1", a1, c1, e1, cpu->ps1[d]);
    }
}

void fast_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    float first, second;
    if (fast_pair_fma(cpu->fpr[a], cpu->ps1[c], cpu->fpr[b], cpu->ps1[a], cpu->ps1[c], cpu->ps1[b], &first, &second))
        fast_pair_result(cpu, d, first, second);
    else {
        double a0 = cpu->fpr[a], c0 = cpu->ps1[c], e0 = cpu->fpr[b], a1 = cpu->ps1[a], c1 = cpu->ps1[c], e1 = cpu->ps1[b];
        ppc_ps_madds1(cpu, d, a, c, b);
        NAN_WATCH("ps_madds1 slot 0", a0, c0, e0, cpu->fpr[d]);
        NAN_WATCH("ps_madds1 slot 1", a1, c1, e1, cpu->ps1[d]);
    }
}

/* The slow halves of the load and store conversions in fastmem.h: the generated helpers themselves. */
#include "generated.h"

f64 fast_f32_from_bits_slow(u32 bits) { return dolrecomp_f32_from_bits(bits); }
u32 fast_f32_to_bits_slow(f64 value) { return dolrecomp_f32_to_bits(value); }

/* --test-fast-ops: runs each fast path and its helper on the same random and edge-case inputs, and counts
   results that differ in any bit. The FPSCR isn't compared, because the fast paths don't keep its status. */
static u64 test_state = 0x9E3779B97F4A7C15ull;

static u64 test_random(void) {
    test_state ^= test_state << 13;
    test_state ^= test_state >> 7;
    test_state ^= test_state << 17;
    return test_state;
}

static double test_value(void) {
    static const u64 edges[] = {0, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
                                0x7FF8000000000000ull, 0x7FF4000000000000ull, 0x0000000000000001ull,
                                0x3810000000000000ull, 0x380FFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull,
                                0x3FF0000000000000ull, 0x36A0000000000000ull};
    u64 r = test_random(), bits;
    u32 single_bits;
    float single;
    double value;
    switch (r & 7u) {
    case 0: /* an edge case */
        bits = edges[(r >> 8) % (sizeof edges / sizeof *edges)];
        memcpy(&value, &bits, 8);
        return value;
    case 1: /* any double bits */
        bits = test_random();
        memcpy(&value, &bits, 8);
        return value;
    case 2: /* a single near the bottom of the single range */
        single_bits = ((u32)test_random() & 0x807FFFFFu) | ((u32)((r >> 16) % 6u) << 23);
        memcpy(&single, &single_bits, 4);
        return single;
    default: /* an ordinary single, as game code mostly has */
        single_bits = ((u32)test_random() & 0x807FFFFFu) | ((u32)(100 + (r >> 16) % 56u) << 23);
        memcpy(&single, &single_bits, 4);
        return single;
    }
}

static u64 test_bits(double value) {
    u64 bits;
    memcpy(&bits, &value, 8);
    return bits;
}

void traced_fdiv(CPUState* cpu, u8 d, u8 a, u8 b) {
    double in_a = cpu->fpr[a], in_b = cpu->fpr[b];
    ppc_fdiv(cpu, d, a, b);
    NAN_WATCH("fdiv", in_a, in_b, 0.0, cpu->fpr[d]);
}

void traced_ps_div(CPUState* cpu, u8 d, u8 a, u8 b) {
    double a0 = cpu->fpr[a], b0 = cpu->fpr[b], a1 = cpu->ps1[a], b1 = cpu->ps1[b];
    ppc_ps_div_op(cpu, d, a, b);
    NAN_WATCH("ps_div slot 0", a0, b0, 0.0, cpu->fpr[d]);
    NAN_WATCH("ps_div slot 1", a1, b1, 0.0, cpu->ps1[d]);
}

bool traced_frsqrte(CPUState* cpu, f64 value, f64* result) {
    bool done = ppc_frsqrte(cpu, value, result);
    if (done) NAN_WATCH("frsqrte", value, 0.0, 0.0, *result);
    return done;
}

bool traced_fres(CPUState* cpu, f64 value, f64* result) {
    bool done = ppc_fres(cpu, value, result);
    if (done) NAN_WATCH("fres", value, 0.0, 0.0, *result);
    return done;
}

void traced_ps_rsqrte(CPUState* cpu, u8 d, u8 b) {
    double b0 = cpu->fpr[b], b1 = cpu->ps1[b];
    ppc_ps_rsqrte_op(cpu, d, b);
    NAN_WATCH("ps_rsqrte slot 0", b0, 0.0, 0.0, cpu->fpr[d]);
    NAN_WATCH("ps_rsqrte slot 1", b1, 0.0, 0.0, cpu->ps1[d]);
}

void traced_ps_res(CPUState* cpu, u8 d, u8 b) {
    double b0 = cpu->fpr[b], b1 = cpu->ps1[b];
    ppc_ps_res_op(cpu, d, b);
    NAN_WATCH("ps_res slot 0", b0, 0.0, 0.0, cpu->fpr[d]);
    NAN_WATCH("ps_res slot 1", b1, 0.0, 0.0, cpu->ps1[d]);
}

void traced_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    double a0 = cpu->fpr[a], b1 = cpu->ps1[b];
    ppc_ps_sum0(cpu, d, a, c, b);
    NAN_WATCH("ps_sum0", a0, b1, 0.0, cpu->fpr[d]);
}

void traced_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b) {
    double a0 = cpu->fpr[a], b1 = cpu->ps1[b];
    ppc_ps_sum1(cpu, d, a, c, b);
    NAN_WATCH("ps_sum1", a0, b1, 0.0, cpu->ps1[d]);
}

void traced_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c) {
    double a0 = cpu->fpr[a], a1 = cpu->ps1[a], c1 = cpu->ps1[c];
    ppc_ps_muls1(cpu, d, a, c);
    NAN_WATCH("ps_muls1 slot 0", a0, c1, 0.0, cpu->fpr[d]);
    NAN_WATCH("ps_muls1 slot 1", a1, c1, 0.0, cpu->ps1[d]);
}

int fast_ops_self_test(u32 rounds) {
    static CPUState fast_cpu, slow_cpu, saved;
    enum { OPS = 16 };
    u32 failures[OPS] = {0};
    static const char* names[OPS] = {"fdivs",    "fadd",      "fsub",      "fmul",     "fmadds",
                                     "fmsub",    "ps_madd",   "ps_madds0", "ps_madds1", "lfs bits",
                                     "stfs bits", "fnmsubs",  "ps_nmsub", "fadds",    "fsubs",
                                     "fmuls"};
    for (u32 i = 0; i < rounds; i++) {
        for (u32 r = 0; r < 4; r++) {
            fast_cpu.fpr[r] = test_value();
            fast_cpu.ps1[r] = test_value();
        }
        fast_cpu.fpscr = (test_random() & 1u) ? 4u : 0u; /* NI half the time: this game sets it */
        saved = fast_cpu;
        for (u32 op = 0; op < OPS; op++) {
            fast_cpu = saved;
            slow_cpu = saved;
            CPUState* f = &fast_cpu;
            CPUState* s = &slow_cpu;
            f64 out_f = 0, out_s = 0;
            bool ok_f = true, ok_s = true;
            switch (op) {
            case 0: fast_fdivs(f, 3, 0, 1); ppc_fdivs(s, 3, 0, 1); break;
            case 1: fast_fadd(f, 3, 0, 1); ppc_fadd(s, 3, 0, 1); break;
            case 2: fast_fsub(f, 3, 0, 1); ppc_fsub(s, 3, 0, 1); break;
            case 3: fast_fmul(f, 3, 0, 1); ppc_fmul(s, 3, 0, 1); break;
            case 4:
                ok_f = fast_fma(f, f->fpr[0], f->fpr[1], f->fpr[2], true, false, false, &out_f);
                ok_s = ppc_fma(s, s->fpr[0], s->fpr[1], s->fpr[2], true, false, false, &out_s);
                break;
            case 5:
                ok_f = fast_fma(f, f->fpr[0], f->fpr[1], f->fpr[2], false, true, false, &out_f);
                ok_s = ppc_fma(s, s->fpr[0], s->fpr[1], s->fpr[2], false, true, false, &out_s);
                break;
            case 6: fast_ps_madd(f, 3, 0, 1, 2, false, false); ppc_ps_madd_op(s, 3, 0, 1, 2, false, false); break;
            case 7: fast_ps_madds0(f, 3, 0, 1, 2); ppc_ps_madds0(s, 3, 0, 1, 2); break;
            case 8: fast_ps_madds1(f, 3, 0, 1, 2); ppc_ps_madds1(s, 3, 0, 1, 2); break;
            case 9: { /* the same test fastmem.h makes before its quick conversion */
                u32 bits = (u32)test_random();
                if (test_random() & 1) bits &= 0x807FFFFFu;
                if (((bits >> 23) & 0xFFu) - 1u < 0xFEu) {
                    float value;
                    memcpy(&value, &bits, 4);
                    out_f = value;
                } else {
                    out_f = fast_f32_from_bits_slow(bits);
                }
                out_s = dolrecomp_f32_from_bits(bits);
                break;
            }
            case 10: {
                u64 bits = test_bits(f->fpr[0]);
                u32 quick = ((bits >> 52) & 0x7FFu) > 896u || (bits << 1) == 0
                                ? (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu))
                                : fast_f32_to_bits_slow(f->fpr[0]);
                out_f = quick;
                out_s = dolrecomp_f32_to_bits(s->fpr[0]);
                break;
            }
            case 11:
                ok_f = fast_fma(f, f->fpr[0], f->fpr[1], f->fpr[2], true, true, true, &out_f);
                ok_s = ppc_fma(s, s->fpr[0], s->fpr[1], s->fpr[2], true, true, true, &out_s);
                break;
            case 12: fast_ps_madd(f, 3, 0, 1, 2, true, true); ppc_ps_madd_op(s, 3, 0, 1, 2, true, true); break;
            case 13: fast_fadds(f, 3, 0, 1); ppc_fadds(s, 3, 0, 1); break;
            case 14: fast_fsubs(f, 3, 0, 1); ppc_fsubs(s, 3, 0, 1); break;
            case 15: fast_fmuls(f, 3, 0, 1); ppc_fmuls(s, 3, 0, 1); break;
            }
            int same = ok_f == ok_s && test_bits(out_f) == test_bits(out_s) &&
                       test_bits(f->fpr[3]) == test_bits(s->fpr[3]) && test_bits(f->ps1[3]) == test_bits(s->ps1[3]);
            if (!same) {
                if (failures[op] < 3)
                    printf("%s differs: in %016llX %016llX %016llX out %016llX %016llX fpr %016llX %016llX "
                           "ps1 %016llX %016llX\n",
                           names[op], (unsigned long long)test_bits(saved.fpr[0]),
                           (unsigned long long)test_bits(saved.fpr[1]), (unsigned long long)test_bits(saved.fpr[2]),
                           (unsigned long long)test_bits(out_f), (unsigned long long)test_bits(out_s),
                           (unsigned long long)test_bits(f->fpr[3]), (unsigned long long)test_bits(s->fpr[3]),
                           (unsigned long long)test_bits(f->ps1[3]), (unsigned long long)test_bits(s->ps1[3]));
                failures[op]++;
            }
        }
    }
    int total = 0;
    for (u32 op = 0; op < OPS; op++) {
        printf("%-10s %u differences in %u\n", names[op], failures[op], rounds);
        total += (int)failures[op];
    }
    return total;
}

