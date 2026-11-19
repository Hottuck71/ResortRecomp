/* Force-included when compiling translated chunks. Most guest memory access is to cached MEM1 and
   MEM2, so those become inline loads and stores. Everything else, such as hardware registers and
   uncached addresses, still goes through the ordinary functions. */
#ifndef FASTMEM_H
#define FASTMEM_H
#include <string.h>
#include "cpu/cpu.h"

/* mtfsf, mtfsfi, and mcrfs only refreshed FPSCR's summary bits, so the rounding mode and non-IEEE mode the
   game sets with mtfsf never reached the host. In non-IEEE mode a real Wii flushes results too small for a
   normal float to zero. Without that, Basketball's head-turn code took the length of a vector of about 1e-22,
   squared it into a denormal, and its reciprocal square root overflowed: the players lost their heads to
   NaN matrices. The control version also sets the host's flush-to-zero and denormals-are-zero, as Dolphin's
   JIT does, on the CPU thread where the game's code runs. */
#define ppc_fpscr_updated ppc_fpscr_control_updated

#define FAST_MEM1 0x80000000u
#define FAST_MEM2 0x90000000u
#define FAST_MEM1_SIZE 0x01800000u
#define FAST_MEM2_SIZE 0x04000000u

static inline u8* fast_host(CPUState* cpu, u32 addr, u32 size) {
    u32 at = addr - FAST_MEM1;
    if (at <= FAST_MEM1_SIZE - size) return cpu->ram + at;
    at = addr - FAST_MEM2;
    if (at <= FAST_MEM2_SIZE - size) return cpu->mem2 + at;
    return NULL;
}

/* A store near a reserved address has to end the reservation, or stwcx. would wrongly succeed. */
static inline void fast_store_check(CPUState* cpu, u32 addr) {
    if (cpu->reserve_valid && (((cpu->reserve_addr & ~0x40000000u) ^ (addr & ~0x40000000u)) & ~31u) == 0)
        cpu->reserve_valid = false;
}

static inline u8 fast_read8(CPUState* cpu, u32 addr) {
    const u8* p = fast_host(cpu, addr, 1);
    return p ? *p : mem_read8(cpu, addr);
}

static inline u16 fast_read16(CPUState* cpu, u32 addr) {
    const u8* p = fast_host(cpu, addr, 2);
    u16 v;
    if (!p) return mem_read16(cpu, addr);
    memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

static inline u32 fast_read32(CPUState* cpu, u32 addr) {
    const u8* p = fast_host(cpu, addr, 4);
    u32 v;
    if (!p) return mem_read32(cpu, addr);
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}

static inline u64 fast_read64(CPUState* cpu, u32 addr) {
    const u8* p = fast_host(cpu, addr, 8);
    u64 v;
    if (!p) return mem_read64(cpu, addr);
    memcpy(&v, p, 8);
    return __builtin_bswap64(v);
}

/* The graphics FIFO's write-gather pipe takes most stores that aren't to memory, so they skip the generic
   memory and hardware dispatch and go straight to main.c. */
#define FAST_WGPIPE(addr) (((addr) & 0xFFFFFFE0u) == 0xCC008000u)
void host_wgpipe_write(u64 value, u32 size);

static inline void fast_write8(CPUState* cpu, u32 addr, u8 value) {
    u8* p = fast_host(cpu, addr, 1);
    if (!p) {
        if (FAST_WGPIPE(addr)) {
            host_wgpipe_write(value, 1);
            return;
        }
        mem_write8(cpu, addr, value);
        return;
    }
    fast_store_check(cpu, addr);
    *p = value;
}

static inline void fast_write16(CPUState* cpu, u32 addr, u16 value) {
    u8* p = fast_host(cpu, addr, 2);
    if (!p) {
        if (FAST_WGPIPE(addr)) {
            host_wgpipe_write(value, 2);
            return;
        }
        mem_write16(cpu, addr, value);
        return;
    }
    fast_store_check(cpu, addr);
    value = __builtin_bswap16(value);
    memcpy(p, &value, 2);
}

static inline void fast_write32(CPUState* cpu, u32 addr, u32 value) {
    u8* p = fast_host(cpu, addr, 4);
    if (!p) {
        if (FAST_WGPIPE(addr)) {
            host_wgpipe_write(value, 4);
            return;
        }
        mem_write32(cpu, addr, value);
        return;
    }
    fast_store_check(cpu, addr);
    value = __builtin_bswap32(value);
    memcpy(p, &value, 4);
}

static inline void fast_write64(CPUState* cpu, u32 addr, u64 value) {
    u8* p = fast_host(cpu, addr, 8);
    if (!p) {
        mem_write64(cpu, addr, value);
        return;
    }
    fast_store_check(cpu, addr);
    value = __builtin_bswap64(value);
    memcpy(p, &value, 8);
}

/* Common float and paired-single work, in fast_ops.c. These stay as calls on purpose: copied into every
   call site they made the translated code 27% bigger and slower, because it is limited by fetching code. */
#if !defined(FAST_OPS_BUILD) && !defined(SLOW_FLOAT) /* -DSLOW_FLOAT: every float op through the full helpers, for tests */
/* The generated header first, so its own inline helpers keep their names, and only the calls in chunks
   below are redirected. It has an include guard, so the chunk's own include of it does nothing. */
#include "generated.h"

/* Float loads and stores convert between single and double bits on every lfs and stfs. The generated
   helpers are inline, but in chunk-sized functions the compiler gives up inlining them. Ordinary numbers
   take these few instructions, which are exactly what the helpers do for them, and the rest go to them. */
f64 fast_f32_from_bits_slow(u32 bits);
u32 fast_f32_to_bits_slow(f64 value);

static inline __attribute__((always_inline)) f64 fast_f32_from_bits(u32 bits) {
    if (__builtin_expect(((bits >> 23) & 0xFFu) - 1u < 0xFEu, 1)) {
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }
    return fast_f32_from_bits_slow(bits);
}

static inline __attribute__((always_inline)) u32 fast_f32_to_bits(f64 value) {
    u64 bits;
    memcpy(&bits, &value, 8);
    if (__builtin_expect(((bits >> 52) & 0x7FFu) > 896u || (bits << 1) == 0, 1))
        return (u32)(((bits >> 32) & 0xC0000000u) | ((bits >> 29) & 0x3FFFFFFFu));
    return fast_f32_to_bits_slow(value);
}

// #define dolrecomp_f32_from_bits fast_f32_from_bits
// #define dolrecomp_f32_to_bits fast_f32_to_bits

bool fast_psq_load(CPUState* cpu, u8 d, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
bool fast_psq_store(CPUState* cpu, u8 s, u32 ea, bool w, u8 gqr, bool indexed, u32 cia);
void fast_fadds(CPUState* cpu, u8 d, u8 a, u8 b);
void fast_fsubs(CPUState* cpu, u8 d, u8 a, u8 b);
void fast_fmuls(CPUState* cpu, u8 d, u8 a, u8 c);
void fast_ps_add(CPUState* cpu, u8 d, u8 a, u8 b);
void fast_ps_sub(CPUState* cpu, u8 d, u8 a, u8 b);
void fast_ps_mul(CPUState* cpu, u8 d, u8 a, u8 c);
void fast_ps_muls0(CPUState* cpu, u8 d, u8 a, u8 c);
void fast_fdivs(CPUState* cpu, u8 d, u8 a, u8 b);
void fast_fadd(CPUState* cpu, u8 d, u8 a, u8 b);
void fast_fsub(CPUState* cpu, u8 d, u8 a, u8 b);
void fast_fmul(CPUState* cpu, u8 d, u8 a, u8 c);
bool fast_fma(CPUState* cpu, f64 a, f64 c, f64 b, bool single, bool subtract, bool negative, f64* output);
void fast_ps_madd(CPUState* cpu, u8 d, u8 a, u8 c, u8 b, bool subtract, bool negative);
void fast_ps_madds0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void fast_ps_madds1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);

#define ppc_fadds fast_fadds
#define ppc_fsubs fast_fsubs
#define ppc_fmuls fast_fmuls
#define ppc_ps_add_op fast_ps_add
#define ppc_ps_sub_op fast_ps_sub
#define ppc_ps_mul_op fast_ps_mul
#define ppc_ps_muls0 fast_ps_muls0
#define ppc_fdivs fast_fdivs
#define ppc_fadd fast_fadd
#define ppc_fsub fast_fsub
#define ppc_fmul fast_fmul
#define ppc_fma fast_fma
#define ppc_ps_madd_op fast_ps_madd
#define ppc_ps_madds0 fast_ps_madds0
#define ppc_ps_madds1 fast_ps_madds1
void traced_fdiv(CPUState* cpu, u8 d, u8 a, u8 b);
void traced_ps_div(CPUState* cpu, u8 d, u8 a, u8 b);
bool traced_frsqrte(CPUState* cpu, f64 value, f64* result);
bool traced_fres(CPUState* cpu, f64 value, f64* result);
void traced_ps_rsqrte(CPUState* cpu, u8 d, u8 b);
void traced_ps_res(CPUState* cpu, u8 d, u8 b);
void traced_ps_sum0(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void traced_ps_sum1(CPUState* cpu, u8 d, u8 a, u8 c, u8 b);
void traced_ps_muls1(CPUState* cpu, u8 d, u8 a, u8 c);
#define ppc_fdiv traced_fdiv
#define ppc_ps_div_op traced_ps_div
#define ppc_frsqrte traced_frsqrte
#define ppc_fres traced_fres
#define ppc_ps_rsqrte_op traced_ps_rsqrte
#define ppc_ps_res_op traced_ps_res
#define ppc_ps_sum0 traced_ps_sum0
#define ppc_ps_sum1 traced_ps_sum1
#define ppc_ps_muls1 traced_ps_muls1
#define ppc_psq_load_inline fast_psq_load
#define ppc_psq_store_inline fast_psq_store
#endif

#define mem_read8 fast_read8
#define mem_read16 fast_read16
#define mem_read32 fast_read32
#define mem_read64 fast_read64
#define mem_write8 fast_write8
#define mem_write16 fast_write16
#define mem_write32 fast_write32
#define mem_write64 fast_write64

#endif
