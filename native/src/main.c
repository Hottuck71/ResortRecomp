/* Host program for the recompiled game. Contains no emulator code.
   Loads main.dol, sets up the state the Wii's loader leaves behind, runs the translated code
   from a dispatch loop, and answers the hardware the game still touches directly. Registers
   without special handling just remember what was written. See docs/10-native-runtime.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gx_stream.h"
#include "fake_remote.h"
#include "audio.h"
#include "real_remote.h"
#include "profile.h"
#include "gx_thread.h"
#include "crash.h"
#include "net.h"
#include "net_status.h"
#include "remote_driver.h"
#include <stdatomic.h>

#define TB_DIVIDER 12u /* 729 MHz CPU, 60.75 MHz timebase */
#define MMIO_SPAN 0x10000u
#define LOCKED_CACHE_BASE 0xE0000000u
#define LOCKED_CACHE_SIZE 0x4000u
#define MAX_SEEN 4096
#define RING 64
#define IDLE_LOOP 0x8004E180u  /* SelectThread, waiting for a thread to become runnable */
#define FRAME_MARK 0x801CAA60u /* the game's per-frame wait has just returned */

CPUState cpu;
static u32 spr[1024];
static u32 locked_cache_transfers, locked_cache_failures;
static u8 mmio_cc[MMIO_SPAN], mmio_cd[MMIO_SPAN];
static u8 locked_cache[LOCKED_CACHE_SIZE];
static u64 total_cycles, dec_fire_tb;
static int dec_armed;
FILE* report;

/* Research capture (--fifo): every graphics write, for scripts/46_fifo_analyze.py.
   Records are 17 bytes, little-endian: kind, pc, address, value. Kind is the write size,
   or one of the marks below. */
#define FIFO_MARK_PI 0xFFu    /* a write to the PI FIFO registers */
#define FIFO_MARK_FIELD 0xFEu /* a video field began */
#define FIFO_LOG_LIMIT 1500000000ll
static FILE* fifo_log;
static long long fifo_log_bytes;

static void fifo_note(u8 kind, u32 addr, u64 value) {
    u8 record[17];
    record[0] = kind;
    memcpy(record + 1, &cpu.pc, 4);
    memcpy(record + 5, &addr, 4);
    memcpy(record + 9, &value, 8);
    if (fifo_log_bytes < FIFO_LOG_LIMIT) fwrite(record, 1, sizeof record, fifo_log);
    fifo_log_bytes += sizeof record;
}

typedef struct {
    u32 addr, first_pc;
    u64 first_cycle, count;
    u8 write, size;
    u32 last_value;
} Seen;
static Seen seen[MAX_SEEN];
static int seen_count;
static u32 ring[RING];
static u32 ring_pos;
static u32* pc_counts;

/* Only calls that reach the dispatch loop are seen, so these are all cross-chunk calls. */
typedef struct {
    u32 addr;
    const char* name;
    int hit;
} Milestone;
static Milestone milestones[] = {
    {0x80006310u, "__start", 0},
    {0x80044460u, "OSInit", 0},
    {0x8003AD50u, "IPCCltInit", 0},
    {0x8004FD70u, "__OSInitSTM", 0},
    {0x8022E3A8u, "main", 0},
};
#define MILESTONE_COUNT (sizeof milestones / sizeof *milestones)

static void note_access(u32 addr, u8 size, int write, u32 value) {
    for (int i = 0; i < seen_count; i++) {
        Seen* s = &seen[i];
        if (s->addr == addr && s->write == (u8)write) {
            s->count++;
            s->last_value = value;
            return;
        }
    }
    if (seen_count < MAX_SEEN)
        seen[seen_count++] = (Seen){addr, cpu.pc, total_cycles, 1, (u8)write, size, value};
}

static u8* mmio_slot(u32 addr) {
    u32 top = addr >> 16;
    if (top == 0xCC00u || top == 0x0C00u) return &mmio_cc[addr & 0xFFFFu];
    if (top == 0xCD00u || top == 0x0D00u || top == 0xCD80u || top == 0x0D80u)
        return &mmio_cd[addr & 0xFFFFu];
    return NULL;
}

/* Addresses used with translation off are physical. */
u8* plain_memory(u32 addr, u32 size) {
    if (addr + size <= cpu.ram_size) return cpu.ram + addr;
    if (addr >= 0x10000000u && addr - 0x10000000u + size <= cpu.mem2_size)
        return cpu.mem2 + (addr - 0x10000000u);
    if (addr >= LOCKED_CACHE_BASE && addr - LOCKED_CACHE_BASE + size <= LOCKED_CACHE_SIZE)
        return locked_cache + (addr - LOCKED_CACHE_BASE);
    return NULL;
}

u64 load_be(const u8* p, u32 size) {
    u64 v = 0;
    for (u32 i = 0; i < size; i++) v = (v << 8) | p[i];
    return v;
}

void store_be(u8* p, u64 v, u32 size) {
    for (u32 i = 0; i < size; i++) p[i] = (u8)(v >> (8 * (size - 1 - i)));
}

/* Interrupt and IPC registers. */
#define PI_INTSR 0xCC003000u
#define PI_INTMR 0xCC003004u
#define PI_FIFO_BASE 0xCC00300Cu
#define PI_FIFO_END 0xCC003010u
#define PI_FIFO_WRITE 0xCC003014u
#define GATHER_BURST 32u
#define PI_PE_TOKEN 0x200u
#define PI_PE_FINISH 0x400u
#define PE_INTERRUPT 0xCC00100Au
#define PE_TOKEN 0xCC00100Eu
#define PE_TOKEN_ENABLE 1u
#define PE_FINISH_ENABLE 2u
#define PE_TOKEN_STATUS 4u
#define PE_FINISH_STATUS 8u
#define PI_HOLLYWOOD 0x4000u
#define PI_VI 0x100u
#define PI_DSP 0x40u
#define DSP_CSR 0xCC00500Au
#define DSP_CSR_AI_INT 0x08u
#define DSP_CSR_AI_MASK 0x10u
#define DSP_CSR_ACK_BITS 0xA8u /* writing 1 to these clears them */
#define AI_DMA_CONTROL 0x5036u
#define AI_DMA_RUNNING 0x8000u
#define AI_FRAME_TICKS (60750000u / 1000u * 3u)
#define AX_DSP_READY 0x806F4380u
#define AX_MAIL_NEW_FRAME 0xBABE0080u
#define AI_SCNT 0xCD006C08u
#define AI_TICKS_PER_SAMPLE (60750000u / 48000u)
#define VI_DI0 0x2030u
#define VI_DI_STATUS 0x8000u
#define VI_DI_ENABLE 0x1000u
#define VI_FIELD_TICKS (60750000u / 60u)
#define IPC_PPCMSG 0xCD000000u
#define IPC_PPCCTRL 0xCD000004u
#define IPC_ARMMSG 0xCD000008u
#define HW_PPCIRQFLAG 0xCD000030u
#define HW_PPCIRQMASK 0xCD000034u
#define IPC_X1 1u
#define IPC_Y2 2u
#define IPC_Y1 4u
#define IPC_IY1 0x10u
#define IPC_IY2 0x20u
#define IRQ_IPC 0x40000000u

static u32 pi_fifo_base, pi_fifo_end, pi_fifo_write, gather_fill;
static u32 pe_interrupt, pe_token;
static volatile int stop_requested; /* also set from the graphics thread */
static int fifo_is_live;
static void update_fifo_is_live(void);
static u32 pi_intsr, pi_intmr, ipc_ppcmsg, ipc_armmsg, ipc_ctrl, hw_irqflag, hw_irqmask;
static u64 vi_next_tb, vi_fields, ai_next_tb;
static u32 dsp_csr;

/* Each request is acknowledged, then answered, one event at a time. */
#define IPC_QUEUE 64
typedef struct {
    u32 request;
    int ack_only;
} IpcEvent;
static IpcEvent ipc_queue[IPC_QUEUE];
static u32 ipc_head, ipc_tail;

static void update_irq(void) {
    if (((ipc_ctrl & IPC_Y1) && (ipc_ctrl & IPC_IY1)) ||
        ((ipc_ctrl & IPC_Y2) && (ipc_ctrl & IPC_IY2)))
        hw_irqflag |= IRQ_IPC;
    if (hw_irqflag & hw_irqmask) pi_intsr |= PI_HOLLYWOOD;
    else pi_intsr &= ~PI_HOLLYWOOD;
    int retrace = 0;
    for (u32 i = 0; i < 4; i++)
        if (load_be(mmio_cc + VI_DI0 + i * 4u, 2) & VI_DI_STATUS) retrace = 1;
    if (retrace) pi_intsr |= PI_VI;
    else pi_intsr &= ~PI_VI;
    if ((dsp_csr & DSP_CSR_AI_INT) && (dsp_csr & DSP_CSR_AI_MASK)) pi_intsr |= PI_DSP;
    else pi_intsr &= ~PI_DSP;
    if ((pe_interrupt & PE_TOKEN_ENABLE) && (pe_interrupt & PE_TOKEN_STATUS)) pi_intsr |= PI_PE_TOKEN;
    else pi_intsr &= ~PI_PE_TOKEN;
    if ((pe_interrupt & PE_FINISH_ENABLE) && (pe_interrupt & PE_FINISH_STATUS)) pi_intsr |= PI_PE_FINISH;
    else pi_intsr &= ~PI_PE_FINISH;
}

/* Audio DMA: each block the game queues takes 3 ms to play, then interrupts. */
static void ai_tick(void) {
    if (cpu.timebase < ai_next_tb) return;
    ai_next_tb = cpu.timebase + AI_FRAME_TICKS;
    if (!(load_be(mmio_cc + AI_DMA_CONTROL, 2) & AI_DMA_RUNNING)) return;
    dsp_csr |= DSP_CSR_AI_INT;
    update_irq();
}

/* Called by the stream decoder. Work finishes as it is read. With the decoder on its own thread, each signal
   joins an ordered queue, and the CPU thread applies them one at a time: a raised signal waits until the game
   has cleared the previous one of its kind. Merging two finishes into one interrupt would lose a callback the
   game counts on, and it would wait for it forever. */
enum { GX_SIGNAL_FINISH, GX_SIGNAL_TOKEN, GX_SIGNAL_TOKEN_RAISE };
#define GX_SIGNALS 1024u
static struct {
    u32 kind, token;
} gx_signals[GX_SIGNALS];
static _Atomic u32 gx_signal_head, gx_signal_tail;

static void post_gx_signal(u32 kind, u32 token) {
    u32 tail = atomic_load_explicit(&gx_signal_tail, memory_order_relaxed);
    while (tail - atomic_load_explicit(&gx_signal_head, memory_order_acquire) >= GX_SIGNALS) {
        if (stop_requested) return; /* the CPU thread has stopped taking them */
    }
    gx_signals[tail % GX_SIGNALS].kind = kind;
    gx_signals[tail % GX_SIGNALS].token = token;
    atomic_store_explicit(&gx_signal_tail, tail + 1u, memory_order_release);
}

/* The first few NaNs written into XF matrices, with the guest code running at the time and its callers from
   the stack's back chain. With --gx-thread 0 the stream is read as the game writes it, so this is the code
   that sent the matrix. */
void host_gx_nan(u32 addr, u32 value) {
    static u32 count;
    if (count++ >= 30) return;
    fprintf(report, "NaN into XF %03X (%08X) at field %llu: pc %08X lr %08X, callers", addr, value,
            (unsigned long long)vi_fields, cpu.pc, cpu.lr);
    u32 frame = cpu.gpr[1];
    for (int depth = 0; depth < 12 && frame; depth++) {
        const u8* p = plain_memory(frame & 0x3FFFFFFFu, 8);
        if (!p) break;
        u32 back = (u32)load_be(p, 4);
        const u8* q = back ? plain_memory(back & 0x3FFFFFFFu, 8) : NULL;
        if (!q) break;
        fprintf(report, " %08X", (u32)load_be(q + 4, 4));
        frame = back;
    }
    fprintf(report, "\n");
}

void host_gx_finish(void) {
    if (gx_thread_on) {
        post_gx_signal(GX_SIGNAL_FINISH, 0);
        return;
    }
    pe_interrupt |= PE_FINISH_STATUS;
    update_irq();
}

void host_gx_token(u32 token, int raise) {
    if (gx_thread_on) {
        post_gx_signal(raise ? GX_SIGNAL_TOKEN_RAISE : GX_SIGNAL_TOKEN, token);
        return;
    }
    pe_token = token;
    if (raise) pe_interrupt |= PE_TOKEN_STATUS;
    update_irq();
}

/* Returns 1 if any signal was applied. Only one interrupt is raised at a time: the game's token handler
   writes the status register back with both status bits, so a finish raised alongside a token would be
   cleared without its own handler running. In the single-thread build each signal is taken before the next
   one is even decoded, so they never overlap there either. */
static int apply_gx_posted(void);

/* Online play needs every PC's game to see the GPU's finishes and tokens at the same moments. Applied after
   every block, as in single play, they land wherever the graphics thread happens to be, which differs from PC
   to PC. With this on they're applied only where the game looks: when it goes idle, and when it reads or clears
   the GPU's interrupt and token registers, each time after the graphics thread has finished everything it was
   given. Those points come at the same place in the game's code on every PC. */
static int gx_sync_signals;

static void sync_gx_signals(void) {
    if (!gx_thread_on || !gx_sync_signals) return;
    gx_thread_drain();
    apply_gx_posted();
}

static int apply_gx_posted(void) {
    u32 head = atomic_load_explicit(&gx_signal_head, memory_order_relaxed);
    u32 tail = atomic_load_explicit(&gx_signal_tail, memory_order_acquire);
    int applied = 0;
    while (head != tail) {
        u32 kind = gx_signals[head % GX_SIGNALS].kind, token = gx_signals[head % GX_SIGNALS].token;
        if (kind != GX_SIGNAL_TOKEN && (pe_interrupt & (PE_FINISH_STATUS | PE_TOKEN_STATUS)))
            break; /* the game hasn't taken the last interrupt yet */
        if (kind == GX_SIGNAL_FINISH) {
            pe_interrupt |= PE_FINISH_STATUS;
        } else {
            pe_token = token;
            if (kind == GX_SIGNAL_TOKEN_RAISE) pe_interrupt |= PE_TOKEN_STATUS;
        }
        head++;
        applied = 1;
    }
    if (!applied) return 0;
    atomic_store_explicit(&gx_signal_head, head, memory_order_release);
    update_irq();
    return 1;
}

/* Raises the display interrupts the game has enabled, once per field. */
static void vi_tick(void) {
    if (cpu.timebase < vi_next_tb) return;
    vi_next_tb = cpu.timebase + VI_FIELD_TICKS;
    vi_fields++;
    gx_thread_field(vi_fields); /* keeps the CPU at most about a field ahead of the graphics thread */
    fake_remote_field();
    net_field((u32)vi_fields); /* online: every player's input for this field, before the game reads any */
    if (fifo_log) fifo_note(FIFO_MARK_FIELD, 0, vi_fields);
    for (u32 i = 0; i < 2; i++) {
        u8* reg = mmio_cc + VI_DI0 + i * 4u;
        u32 value = (u32)load_be(reg, 2);
        if (value & VI_DI_ENABLE) store_be(reg, value | VI_DI_STATUS, 2);
    }
    update_irq();
}

static void ipc_pump(void) {
    if (ipc_ctrl & (IPC_Y1 | IPC_Y2)) return; /* the game has not taken the last event */
    if (ipc_head == ipc_tail) return;
    IpcEvent e = ipc_queue[ipc_head++ % IPC_QUEUE];
    if (e.ack_only) {
        ipc_ctrl |= IPC_Y2;
    } else {
        ipc_armmsg = e.request;
        ipc_ctrl |= IPC_Y1;
    }
    update_irq();
}

static void ipc_push(u32 request, int ack_only) {
    ipc_queue[ipc_tail++ % IPC_QUEUE] = (IpcEvent){request, ack_only};
}

void read_guest_string(u32 addr, char* out, u32 size) {
    u8* p = plain_memory(addr & 0x3FFFFFFFu, 1);
    u32 i = 0;
    while (p && i + 1 < size && p[i]) {
        out[i] = (char)p[i];
        i++;
    }
    out[i] = 0;
}

/* Takes a request from the game. ios.c does the work; this is only the reply protocol. */
static void ios_request(u32 phys) {
    u8* r = plain_memory(phys, 0x40);
    ipc_push(phys, 1);
    if (!r) {
        fprintf(report, "IOS: bad request pointer %08X\n", phys);
        return;
    }
    u32 cmd = (u32)load_be(r, 4);
    s32 result;
    if (ios_handle(r, &result)) {
        store_be(r + 4, (u32)result, 4);
        store_be(r + 8, cmd, 4); /* the reply carries the original command here */
        store_be(r, 8, 4);
        ipc_push(phys, 0);
    }
}

static int hw_read(u32 ea, u32* value) {
    switch (ea) {
    case PI_INTSR: *value = pi_intsr; return 1;
    case PI_INTMR: *value = pi_intmr; return 1;
    case PE_INTERRUPT: sync_gx_signals(); *value = pe_interrupt; return 1;
    case DSP_CSR: *value = dsp_csr; return 1;
    case PE_TOKEN: sync_gx_signals(); *value = pe_token; return 1;
    case PI_FIFO_BASE: *value = pi_fifo_base; return 1;
    case PI_FIFO_END: *value = pi_fifo_end; return 1;
    case PI_FIFO_WRITE: *value = pi_fifo_write; return 1;
    case IPC_PPCMSG: *value = ipc_ppcmsg; return 1;
    case IPC_PPCCTRL: *value = ipc_ctrl; return 1;
    case IPC_ARMMSG: *value = ipc_armmsg; return 1;
    case HW_PPCIRQFLAG: *value = hw_irqflag; return 1;
    case HW_PPCIRQMASK: *value = hw_irqmask; return 1;
    case AI_SCNT: /* counts samples at 48 kHz while playback is on, so startup can see it move */
        *value = (load_be(mmio_cd + 0x6C00u, 4) & 1u) ? (u32)(cpu.timebase / AI_TICKS_PER_SAMPLE) : 0;
        return 1;
    default: return 0;
    }
}

static int hw_write(u32 ea, u32 v) {
    switch (ea) {
    case PI_INTSR:
        pi_intsr &= ~v;
        update_irq();
        return 1;
    case PI_INTMR: pi_intmr = v; return 1;
    case DSP_CSR:
        dsp_csr = (v & ~DSP_CSR_ACK_BITS) | (dsp_csr & ~v & DSP_CSR_ACK_BITS);
        update_irq();
        return 1;
    case PE_INTERRUPT: /* status bits clear when 1 is written to them */
        pe_interrupt = (v & 3u) | (pe_interrupt & ~v & 0xCu);
        update_irq();
        sync_gx_signals(); /* online: the next signal, if one is waiting */
        return 1;
    case PI_FIFO_BASE:
        pi_fifo_base = v;
        update_fifo_is_live();
        return 1;
    case PI_FIFO_END: pi_fifo_end = v; return 1;
    case PI_FIFO_WRITE:
        pi_fifo_write = v & 0x3FFFFFFFu;
        gather_fill = 0;
        return 1;
    case IPC_PPCMSG: ipc_ppcmsg = v; return 1;
    case IPC_PPCCTRL:
        ipc_ctrl = (ipc_ctrl & ~(IPC_IY1 | IPC_IY2)) | (v & (IPC_IY1 | IPC_IY2));
        ipc_ctrl &= ~(v & (IPC_Y1 | IPC_Y2)); /* writing 1 clears these */
        if (v & IPC_X1) ios_request(ipc_ppcmsg);
        update_irq();
        ipc_pump();
        return 1;
    case HW_PPCIRQFLAG:
        hw_irqflag &= ~v;
        update_irq();
        ipc_pump();
        return 1;
    case HW_PPCIRQMASK:
        hw_irqmask = v;
        update_irq();
        return 1;
    default: return 0;
    }
}

/* The write-gather pipe: bytes land at the PI write pointer, which moves in 32-byte bursts.
   GXEndDisplayList reads that pointer back to learn how long a recorded list is. */
static void gather_write(u64 value, u8 size) {
    /* Only the GP's own FIFO is live. Bytes elsewhere are a display list being recorded, and those
       have to reach memory. Nothing reads the live ring, so it isn't stored. */
    if (fifo_is_live) {
        gx_stream_write(value, size);
    } else {
        u8* out = plain_memory(pi_fifo_write + gather_fill, size);
        if (out) store_be(out, value, size);
    }
    gather_fill += size;
    while (gather_fill >= GATHER_BURST) {
        gather_fill -= GATHER_BURST;
        pi_fifo_write += GATHER_BURST;
        if (pi_fifo_end && pi_fifo_write >= pi_fifo_end) pi_fifo_write = pi_fifo_base;
    }
}

/* Straight from the chunks' store helpers (fastmem.h), for the busiest register. */
void host_wgpipe_write(u64 value, u32 size) {
    if (fifo_log) fifo_note((u8)size, 0xCC008000u, value);
    gather_write(value, (u8)size);
}

static void update_fifo_is_live(void) {
    u32 gp_base = (u32)load_be(mmio_cc + 0x20, 2) | ((u32)load_be(mmio_cc + 0x22, 2) << 16);
    fifo_is_live = (pi_fifo_base & 0x3FFFFFFFu) == gp_base;
}

static u64 external_read(CPUState* c, u32 ea, u8 size) {
    (void)c;
    u8* p = plain_memory(ea, size);
    if (p) return load_be(p, size);
    u32 special;
    if (hw_read(ea, &special)) {
        note_access(ea, size, 0, special);
        return special;
    }
    p = mmio_slot(ea);
    u64 value = p ? load_be(p, size) : 0;
    note_access(ea, size, 0, (u32)value);
    return value;
}

static void external_write(CPUState* c, u32 ea, u64 value, u8 size) {
    (void)c;
    if ((ea & 0xFFFFFFE0u) == 0xCC008000u) { /* by far the busiest register, so it comes first */
        if (fifo_log) fifo_note(size, ea, value);
        gather_write(value, size);
        return;
    }
    u8* p = plain_memory(ea, size);
    if (p) {
        store_be(p, value, size);
        return;
    }
    note_access(ea, size, 1, (u32)value);
    if (fifo_log && ea >= 0xCC00300Cu && ea <= 0xCC003014u) fifo_note(FIFO_MARK_PI, ea, value);
    if (hw_write(ea, (u32)value)) return;
    p = mmio_slot(ea);
    if (p) store_be(p, value, size);
    if (ea >= 0xCC002030u && ea < 0xCC002040u) update_irq();
    if (ea >= 0xCC000020u && ea <= 0xCC000023u) update_fifo_is_live();
    /* EXI control registers: no device is attached yet, so a transfer ends at once with
       empty data. Clearing the start bit is how the hardware reports that. */
    if ((ea & 0xFFFFFF00u) == 0xCD006800u && ea <= 0xCD006834u &&
        ((ea - 0xCD006800u) % 0x14u) == 0x0Cu)
        store_be(p, value & ~1ull, size);
}

static u32 read_spr(u32 n) {
    switch (n) {
    case 18: return cpu.dsisr;
    case 19: return cpu.dar;
    case 22: return dec_armed ? (u32)(dec_fire_tb - cpu.timebase) : spr[22];
    case 26: return cpu.srr0;
    case 27: return cpu.srr1;
    case 287: return 0x00087102u; /* Broadway */
    case 920: return cpu.hid2;
    default: break;
    }
    if (n >= 912 && n <= 919) return cpu.gqr[n - 912];
    return spr[n];
}

/* Blocks of 32 bytes: the count is split across both registers, and 0 means 128. */
static void locked_cache_dma(u32 upper, u32 lower) {
    u32 blocks = ((upper & 31u) << 2) | ((lower >> 2) & 3u);
    u32 size = (blocks ? blocks : 128u) * 32u;
    u8* memory = plain_memory((upper & ~31u) & 0x3FFFFFFFu, size);
    u32 offset = (lower & ~31u) - LOCKED_CACHE_BASE;
    locked_cache_transfers++;
    if (!memory || offset + size > LOCKED_CACHE_SIZE) {
        locked_cache_failures++;
        return;
    }
    if (lower & 16u) memcpy(locked_cache + offset, memory, size);
    else memcpy(memory, locked_cache + offset, size);
}

static void write_spr(u32 n, u32 v) {
    switch (n) {
    case 18: cpu.dsisr = v; return;
    case 19: cpu.dar = v; return;
    case 22:
        spr[22] = v;
        dec_fire_tb = cpu.timebase + v;
        dec_armed = 1;
        return;
    case 26: cpu.srr0 = v; return;
    case 27: cpu.srr1 = v; return;
    case 284: cpu.timebase = (cpu.timebase & 0xFFFFFFFF00000000ull) | v; return;
    case 285: cpu.timebase = ((u64)v << 32) | (u32)cpu.timebase; return;
    case 920: cpu.hid2 = v; return;
    case 923: /* DMAL: a transfer between the locked cache and memory */
        spr[n] = v;
        if (v & 2u) locked_cache_dma(spr[922], v);
        return;
    default: break;
    }
    if (n >= 912 && n <= 919) {
        cpu.gqr[n - 912] = v;
        return;
    }
    spr[n] = v;
}

static const char* stop_reason = "cycle limit reached";

static void stop_run(const char* reason) {
    stop_requested = 1;
    stop_reason = reason;
}

void host_stop(const char* reason) { stop_run(reason); }

static void fallback(CPUState* c, u32 raw, u32 cia) {
    u32 op = raw >> 26, xo = (raw >> 1) & 0x3FFu, rd = (raw >> 21) & 31u;
    u32 n = ((raw >> 16) & 31u) | (((raw >> 11) & 31u) << 5);
    c->pc = cia + 4u; /* generated code returns to the dispatch loop after a fallback */
    if (op == 31 && xo == 339) {
        c->gpr[rd] = read_spr(n);
        return;
    }
    if (op == 31 && xo == 467) {
        write_spr(n, c->gpr[rd]);
        return;
    }
    if (op == 31 && (xo == 54 || xo == 86 || xo == 470 || xo == 982)) return; /* caches */
    fprintf(report, "Unhandled instruction %08X at %08X\n", raw, cia);
    stop_run("unhandled instruction");
}

/* Does what the game's exception stub at 0x80044CB0 does. The OS copies that stub to low
   memory at startup, where translated code can't run it. */
static void enter_os_handler(u32 number) {
    u32 ctx_phys = (u32)load_be(cpu.ram + 0xC0, 4);
    u8* ctx = plain_memory(ctx_phys & 0x3FFFFFFFu, 0x1C8);
    if (!ctx_phys || !ctx) {
        fprintf(report, "Exception %u before a context exists (0xC0 = %08X)\n", number, ctx_phys);
        stop_run("exception with no OS context");
        return;
    }
    store_be(ctx + 12, cpu.gpr[3], 4);
    store_be(ctx + 16, cpu.gpr[4], 4);
    store_be(ctx + 20, cpu.gpr[5], 4);
    store_be(ctx + 418, load_be(ctx + 418, 2) | 2u, 2);
    store_be(ctx + 128, cpu.cr, 4);
    store_be(ctx + 132, cpu.lr, 4);
    store_be(ctx + 136, cpu.ctr, 4);
    store_be(ctx + 140, cpu.xer, 4);
    store_be(ctx + 408, cpu.srr0, 4);
    store_be(ctx + 412, cpu.srr1, 4);

    u32 recoverable = cpu.srr1 & 2u;
    cpu.msr |= 0x30u;
    cpu.srr1 = cpu.msr;
    cpu.gpr[3] = number;
    cpu.gpr[4] = (u32)load_be(cpu.ram + 0xD4, 4);
    cpu.gpr[5] = recoverable;
    cpu.pc = recoverable ? (u32)load_be(cpu.ram + 0x3000u + number * 4u, 4) : 0x80044D50u;
    cpu.srr0 = cpu.pc;
}

static void handle_exception_flags(void) {
    u32 flags = cpu.exception;
    cpu.exception = 0;
    if (flags & PPC_EXC_SYSTEM_CALL) { /* the OS stub only flushes caches */
        cpu.msr = (cpu.msr & ~0x87C0FFFFu) | (cpu.srr1 & 0x87C0FFFFu);
        cpu.pc = cpu.srr0;
        return;
    }
    if (flags & PPC_EXC_FP_UNAVAILABLE) { /* the OS swaps float state between threads here */
        enter_os_handler(7);
        return;
    }
    fprintf(report, "CPU exception flags %08X program %08X at %08X dar %08X\n", flags,
            cpu.program_exception, cpu.srr0, cpu.dar);
    stop_run("CPU exception");
}

static void check_interrupts(void) {
    if (!(cpu.msr & 0x8000u)) return; /* MSR[EE] */
    if (dec_armed && cpu.timebase >= dec_fire_tb) {
        dec_armed = 0;
        spr[22] = 0xFFFFFFFFu;
        ppc_take_exception(&cpu, 0, 0x900u, cpu.pc, 0);
        enter_os_handler(8);
        return;
    }
    if (pi_intsr & pi_intmr) {
        ppc_take_exception(&cpu, 0, 0x500u, cpu.pc, 0);
        enter_os_handler(4);
    }
}

/* Game functions that host code replaces. */
static void os_report(void) {
    char format[512], out[1024];
    read_guest_string(cpu.gpr[3], format, sizeof format);
    u32 next_gpr = 4, next_fpr = 1, o = 0;
    for (const char* f = format; *f && o + 300 < sizeof out; f++) {
        if (*f != '%') {
            out[o++] = *f;
            continue;
        }
        char spec[16];
        u32 s = 0;
        spec[s++] = *f++;
        while (*f && strchr("-+ #0123456789.lhz", *f) && s < 12) {
            if (*f != 'l' && *f != 'h' && *f != 'z') spec[s++] = *f;
            f++;
        }
        if (!*f) break;
        spec[s++] = *f;
        spec[s] = 0;
        if (*f == '%') {
            out[o++] = '%';
        } else if (*f == 's') {
            char text[256];
            read_guest_string(next_gpr <= 10 ? cpu.gpr[next_gpr++] : 0, text, sizeof text);
            o += (u32)snprintf(out + o, sizeof out - o, spec, text);
        } else if (strchr("feEgG", *f)) {
            o += (u32)snprintf(out + o, sizeof out - o, spec, cpu.fpr[next_fpr++]);
        } else {
            o += (u32)snprintf(out + o, sizeof out - o, spec, next_gpr <= 10 ? cpu.gpr[next_gpr++] : 0);
        }
    }
    out[o] = 0;
    fprintf(report, "OSReport: %s%s", out, o && out[o - 1] == '\n' ? "" : "\n");
}

static u32 last_alloc[3]; /* size, unused, heap */

/* Yaz0, the game's main archive compression. The format is public. The game's own decompressor is
   the same algorithm, so this only changes how fast it runs. */
static int host_yaz0(const u8* src, u8* dst, u32 size) {
    u32 in = 16, out = 0, group = 0, bits = 0;
    while (out < size) {
        if (!bits) {
            group = src[in++];
            bits = 8;
        }
        if (group & 0x80u) {
            dst[out++] = src[in++];
        } else {
            u32 first = src[in], second = src[in + 1];
            u32 distance = (((first & 15u) << 8) | second) + 1, count = first >> 4;
            in += 2;
            count = count ? count + 2 : (u32)src[in++] + 0x12;
            if (distance > out) return 0;
            if (count > size - out) count = size - out;
            for (u32 i = 0; i < count; i++, out++) dst[out] = dst[out - distance];
        }
        group <<= 1;
        bits--;
    }
    return 1;
}

static u8* guest_range(u32 addr, u32 size) { return plain_memory(addr & 0x3FFFFFFFu, size); }

/* An EGG expanded heap keeps its MEM head at +0x38, with free and used lists inside it. */
static void report_heap(u32 heap) {
    static const struct { u32 offset; const char* name; } lists[2] = {{0x3C, "free"}, {0x44, "used"}};
    const u8* head = plain_memory((heap + 0x38) & 0x3FFFFFFFu, 0x50);
    for (u32 i = 0; head && i < 2; i++) {
        u32 at = (u32)load_be(head + lists[i].offset, 4), count = 0, total = 0, biggest = 0;
        while (at && count < 200000) {
            const u8* block = plain_memory(at & 0x3FFFFFFFu, 16);
            if (!block) break;
            u32 size = (u32)load_be(block + 4, 4);
            total += size;
            if (size > biggest) biggest = size;
            count++;
            at = (u32)load_be(block + 12, 4);
        }
        fprintf(report, "  heap %s: %u blocks, %u bytes, largest %u\n", lists[i].name, count, total, biggest);
    }
}

/* Called from the entry label of each function in native/replacements.txt. */
int native_replaced(CPUState* ctx, u32 pc) {
    switch (pc) {
    case 0x80045DF0u: /* __OSInitAudioSystem: audio will be native, so no DSP to start */
    case 0x80045FC0u: /* __OSStopAudioSystem */
    case 0x80064160u: /* __AXOutInitDSP: same reason */
        break;
    case 0x8005DB90u: /* WUDInit: Wii Remotes will come through Windows, so no Bluetooth stack */
        fake_remote_start();
        ctx->gpr[3] = 1;
        break;
    case 0x8006C6B0u: /* Bluetooth buffer pool */
        ctx->gpr[3] = fake_remote_buffer();
        break;
    case 0x80074E78u: /* Bluetooth HID send: the report sits in the buffer in r4 */
        fake_remote_send(ctx->gpr[3], ctx->gpr[4]); /* r3: the remote's handle */
        break;
    case 0x80026A80u: /* MotionPlus authenticity check: a real remote answers it for itself */
        if (fake_remote_is_real()) return 0;
        fake_remote_verify_motion_plus(ctx->gpr[3]);
        break;
    case 0x8007501Cu: /* Bluetooth stack tick, at the end of WPAD's 1 ms manager */
        if (fake_remote_tick(ctx)) return 1;
        break;
    case 0x801C1654u: /* EGG heap allocation: not replaced, only remembered for the check below */
        last_alloc[0] = ctx->gpr[3];
        last_alloc[2] = ctx->gpr[5];
        return 0;
    case 0x800460B0u: /* DCInvalidateRange: PC memory needs no cache upkeep */
    case 0x800460E0u: /* DCFlushRange */
    case 0x80046110u: /* DCStoreRange */
    case 0x80046140u: /* DCFlushRangeNoSync */
    case 0x80046170u: /* DCStoreRangeNoSync */
    case 0x800461D0u: /* ICInvalidateRange */
        break;
    case 0x800461A0u: { /* DCZeroRange */
        u32 start = ctx->gpr[3] & ~31u, size = ((ctx->gpr[3] & 31u) + ctx->gpr[4] + 31u) & ~31u;
        u8* out = guest_range(start, size);
        if (!out) return 0;
        memset(out, 0, size);
        break;
    }
    case 0x80004000u:   /* memcpy */
    case 0x8000A9F0u: { /* memmove */
        u8* to = guest_range(ctx->gpr[3], ctx->gpr[5]);
        u8* from = guest_range(ctx->gpr[4], ctx->gpr[5]);
        if (!to || !from || !ctx->gpr[5]) return 0;
        memmove(to, from, ctx->gpr[5]);
        break;
    }
    case 0x80004350u: { /* memset */
        u8* to = guest_range(ctx->gpr[3], ctx->gpr[5]);
        if (!to || !ctx->gpr[5]) return 0;
        memset(to, (int)(ctx->gpr[4] & 255u), ctx->gpr[5]);
        break;
    }
    case 0x801C961Cu: /* decompressor: a null destination means an allocation failed */
        if (ctx->gpr[4]) {
            const u8* src = guest_range(ctx->gpr[3], 16);
            if (!src || memcmp(src, "Yaz", 3)) return 0;
            u32 size = (u32)load_be(src + 4, 4);
            u8* dst = guest_range(ctx->gpr[4], size);
            src = guest_range(ctx->gpr[3], size / 8 + 32); /* only to check that memory is there */
            if (!dst || !src || !host_yaz0(src, dst, size)) return 0;
            ctx->gpr[3] = size;
            break;
        }
        fprintf(report, "Decompress to null: alloc size %u heap %08X from %08X\n", last_alloc[0],
                last_alloc[2], ctx->lr);
        report_heap(last_alloc[2]);
        stop_run("decompress to null");
        return 1;
    case 0x8022F9D8u: { /* scene entry: only watched, for the report */
        u32 manager = (u32)load_be(plain_memory(0x006F4C98u, 4), 4);
        const u8* m = plain_memory(manager & 0x3FFFFFFFu, 0x40);
        fprintf(report, "Scene enter at field %llu, %.1f seconds: current %d previous %d next %d\n",
                (unsigned long long)vi_fields, (double)clock() / CLOCKS_PER_SEC, (s32)load_be(m + 0x18, 4),
                (s32)load_be(m + 0x1C, 4), (s32)load_be(m + 0x14, 4));
        return 0;
    }
    case 0x8015A880u: /* NW4R SoundStartable::detail_StartSound(handle, sound ID, start info): only watched */
        audio_sound_requested(ctx->gpr[5]);
        return 0;
    case 0x8015A8E0u: /* the same by label: the ID isn't known yet */
        audio_sound_requested(0xFFFFFFFEu); /* audio.c counts these apart */
        return 0;
    case 0x801452A0u: /* BasicSound::StartPrepared: the sound just requested got going */
        audio_sound_started();
        return 0;
    case 0x80064020u: /* AI callback: the DSP that AX waits for is always ready here */
        store_be(plain_memory(AX_DSP_READY & 0x3FFFFFFFu, 4), 1, 4);
        return 0;
    case 0x80099AA0u: /* DSPCheckMailToDSP: mail is taken at once */
        ctx->gpr[3] = 0;
        break;
    case 0x80099AE0u: /* DSPSendMailToDSP: the word after the new-frame mail is the command list */
        if (ctx->gpr[3] != AX_MAIL_NEW_FRAME) audio_frame();
        break;
    case 0x80235CC8u: /* OSReport */
        os_report();
        break;
    case 0x800493A0u: { /* OSFatal */
        char message[512];
        read_guest_string(ctx->gpr[5], message, sizeof message);
        fprintf(report, "OSFatal called from %08X: %s\n", ctx->lr, message);
        for (u32 sp = ctx->gpr[1], i = 0; i < 12 && sp; i++) {
            u8* frame = plain_memory(sp & 0x3FFFFFFFu, 8);
            if (!frame) break;
            sp = (u32)load_be(frame, 4);
            u8* next = sp ? plain_memory(sp & 0x3FFFFFFFu, 8) : NULL;
            if (next) fprintf(report, "  stack return %08X\n", (u32)load_be(next + 4, 4));
        }
        stop_run("OSFatal");
        return 1;
    }
    default:
        return 0;
    }
    ctx->pc = ctx->lr;
    return 1;
}

static u8* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* data = malloc(*size);
    if (data && fread(data, 1, *size, f) != *size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    return data;
}

static void put(u32 offset, u32 value) {
    store_be(cpu.ram + offset, value, 4);
}

/* Values follow the public Wii memory map (wiibrew.org/wiki/Memory_map). */
static int load_game(const char* game) {
    char path[1024];
    size_t size;
    snprintf(path, sizeof path, "%s/sys/main.dol", game);
    u8* dol = read_file(path, &size);
    if (!dol) {
        fprintf(stderr, "Can't read %s\n", path);
        return 0;
    }
    /* The BSS range spans the small data sections, so it has to be cleared before they load. */
    u32 bss = (u32)load_be(dol + 0xD8, 4), bss_size = (u32)load_be(dol + 0xDC, 4);
    memset(cpu.ram + (bss & 0x01FFFFFFu), 0, bss_size);
    for (int i = 0; i < 18; i++) {
        u32 off = (u32)load_be(dol + i * 4, 4);
        u32 addr = (u32)load_be(dol + 0x48 + i * 4, 4);
        u32 len = (u32)load_be(dol + 0x90 + i * 4, 4);
        if (len) memcpy(cpu.ram + (addr & 0x01FFFFFFu), dol + off, len);
    }
    cpu.pc = (u32)load_be(dol + 0xE0, 4);
    free(dol);

    snprintf(path, sizeof path, "%s/sys/boot.bin", game);
    u8* boot = read_file(path, &size);
    if (!boot) {
        fprintf(stderr, "Can't read %s\n", path);
        return 0;
    }
    memcpy(cpu.ram, boot, 0x20);
    free(boot);

    /* The apploader leaves the FST and bi2 at the top of MEM1, with the arena below them. */
    u32 fst_size, bi2_size, tmd_size;
    const u8* fst = disc_fst(&fst_size);
    const u8* bi2 = disc_bi2(&bi2_size);
    const u8* tmd = disc_tmd(&tmd_size);
    u32 fst_at = (0x01800000u - fst_size) & ~0x1Fu;
    u32 bi2_at = (fst_at - bi2_size) & ~0x1Fu;
    memcpy(cpu.ram + fst_at, fst, fst_size);
    memcpy(cpu.ram + bi2_at, bi2, bi2_size);

    put(0x20, 0x0D15EA5Eu);
    put(0x24, 1);
    put(0x28, 0x01800000u);
    put(0x2C, 0x23u);
    put(0x34, 0x80000000u | fst_at);
    put(0x38, 0x80000000u | fst_at);
    put(0x3C, fst_size);
    put(0xF4, 0x80000000u | bi2_at);
    put(0xF0, 0x01800000u);
    put(0xF8, 0x0E7BE2C0u);
    put(0xFC, 0x2B73A840u);
    put(0x3100, 0x01800000u);
    put(0x3104, 0x01800000u);
    put(0x3108, 0x81800000u);
    put(0x3110, 0x80000000u | fst_at);
    put(0x3118, 0x04000000u);
    put(0x311C, 0x04000000u);
    put(0x3120, 0x93600000u);
    put(0x3124, 0x90000800u);
    put(0x3128, 0x935E0000u);
    put(0x3130, 0x935E0000u);
    put(0x3134, 0x93600000u);
    put(0x3138, 0x11u);
    /* The running IOS and the one the disc expects, with the IOS date and its memory layout.
       The number comes from the TMD. The rest was checked against a running console state. */
    put(0x3140, ((u32)load_be(tmd + 0x188, 4) << 16) | 0x161Fu);
    put(0x3144, 0x00030310u);
    put(0x3148, 0x93600000u);
    put(0x314C, 0x93620000u);
    put(0x3158, 0x0000FF01u);
    put(0x315C, 0x80800113u);
    memcpy(cpu.ram + 0x3180, cpu.ram, 4); /* game ID */
    put(0x3184, 0x80000000u);
    put(0x3188, ((u32)load_be(tmd + 0x188, 4) << 16) | 0x1219u);
    put(0x3198, 0x03E00000u);
    put(0x319C, 0x80000000u);
    put(0xEC, 0x81800000u);
    return 1;
}

u32 host_field(void) { return (u32)vi_fields; }

/* Online play's desync check: a checksum of main memory (MEM1), the same on every PC while their games agree. */
u64 host_memory_hash(void) {
    /* The graphics thread writes into guest memory too (the stamps EFB copies leave), on its own schedule, so it
       finishes what it was given first: then every PC hashes the same work. */
    if (gx_thread_on) gx_thread_drain();
    const u64* words = (const u64*)cpu.ram;
    u64 h = 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < cpu.ram_size / 8u; i++) h = (h ^ words[i]) * 0x100000001B3ull;
    return h;
}

static void write_report(void) {
#ifdef PGO_GEN /* a training build saves its counts now, in case the run is stopped before it exits */
    { extern void __gcov_dump(void); __gcov_dump(); }
#endif
    fprintf(report, "\nStopped: %s\n", stop_reason);
    fprintf(report, "cycles %llu timebase %llu pc %08X lr %08X msr %08X r1 %08X r3 %08X\n",
            (unsigned long long)total_cycles, (unsigned long long)cpu.timebase, cpu.pc, cpu.lr,
            cpu.msr, cpu.gpr[1], cpu.gpr[3]);
    fprintf(report, "video fields %llu\n", (unsigned long long)vi_fields);
    fprintf(report, "game frames %u\n", pc_counts[(FRAME_MARK - 0x80000000u) / 4u]);
    disc_write_report();
    fprintf(report, "locked cache transfers %u failed %u\n", locked_cache_transfers, locked_cache_failures);
    gx_adapter_write_report();
    fake_remote_write_report();
    net_write_report();
    if (remote_driver_ready()) remote_driver_write_report(report);
    audio_write_report();
    gx_stream_write_report();
    gx_debug_write_report();
    fprintf(report, "\n[stack]\n");
    for (u32 sp = cpu.gpr[1], i = 0; i < 24 && sp; i++) {
        u8* frame = plain_memory(sp & 0x3FFFFFFFu, 8);
        if (!frame) break;
        sp = (u32)load_be(frame, 4);
        u8* next = sp ? plain_memory(sp & 0x3FFFFFFFu, 8) : NULL;
        if (next) fprintf(report, "%08X\n", (u32)load_be(next + 4, 4));
    }
    fprintf(report, "\n[threads] address state priority pc lr then stack\n");
    u32 t = (u32)load_be(cpu.ram + 0xDC, 4); /* the OS keeps active threads in this list */
    for (int n = 0; n < 40 && t; n++) {
        u8* th = plain_memory(t & 0x3FFFFFFFu, 0x318);
        if (!th) break;
        fprintf(report, "%08X state %u priority %u pc %08X lr %08X:", t,
                (u32)load_be(th + 0x2C8, 2), (u32)load_be(th + 0x2D0, 4),
                (u32)load_be(th + 0x198, 4), (u32)load_be(th + 0x84, 4));
        u32 sp = (u32)load_be(th + 4, 4);
        for (int i = 0; i < 12 && sp; i++) {
            u8* frame = plain_memory(sp & 0x3FFFFFFFu, 8);
            if (!frame) break;
            sp = (u32)load_be(frame, 4);
            u8* next = sp ? plain_memory(sp & 0x3FFFFFFFu, 8) : NULL;
            if (next) fprintf(report, " %08X", (u32)load_be(next + 4, 4));
        }
        fprintf(report, "\n");
        t = (u32)load_be(th + 0x2FC, 4);
    }
    fprintf(report, "\n[registers]\n");
    for (int i = 0; i < 32; i++)
        fprintf(report, "r%-2d %08X   f%-2d %g\n", i, cpu.gpr[i], i, cpu.fpr[i]);
    fprintf(report, "\n[milestones]\n");
    for (size_t i = 0; i < MILESTONE_COUNT; i++)
        fprintf(report, "%s %s\n", milestones[i].hit ? "reached" : "NOT-reached",
                milestones[i].name);
    fprintf(report, "\n[hardware] first_cycle first_pc addr rw size count last_value\n");
    for (int i = 0; i < seen_count; i++) {
        Seen* s = &seen[i];
        fprintf(report, "%llu %08X %08X %c %u %llu %08X\n", (unsigned long long)s->first_cycle,
                s->first_pc, s->addr, s->write ? 'W' : 'R', s->size,
                (unsigned long long)s->count, s->last_value);
    }
    fprintf(report, "\n[recent]\n");
    for (u32 i = 0; i < RING; i++) fprintf(report, "%08X\n", ring[(ring_pos + i) % RING]);
    fprintf(report, "\n[hot]\n");
    for (int pick = 0; pick < 25; pick++) {
        u32 best = 0, best_count = 0;
        for (u32 i = 0; i < 0x01800000u / 4u; i++)
            if (pc_counts[i] > best_count) {
                best_count = pc_counts[i];
                best = i;
            }
        if (!best_count) break;
        fprintf(report, "%08X %u\n", 0x80000000u + best * 4u, best_count);
        pc_counts[best] = 0;
    }
}

/* With every thread asleep, nothing can happen before the next interrupt. */
static void skip_idle_time(void) {
    u64 next = vi_next_tb;
    if (dec_armed && dec_fire_tb < next) next = dec_fire_tb;
    if (ai_next_tb < next) next = ai_next_tb;
    if (next * TB_DIVIDER > total_cycles) total_cycles = next * TB_DIVIDER;
}


static int run_window;
static u64 run_max_cycles;
static u32 run_profile_from, run_profile_until;

/* The CPU: on the main thread, or on its own when the graphics stream has the main thread. */
static void cpu_loop(void) {
    if (run_profile_until) profile_start(run_profile_from, run_profile_until, &cpu.pc, gx_adapter_frames);
    while (!stop_requested && total_cycles < run_max_cycles) {
        u32 pc = cpu.pc;
        if (pc < cpu.ram_size) pc |= 0x80000000u;
        if (!run_window) {
            for (size_t i = 0; i < MILESTONE_COUNT; i++)
                if (!milestones[i].hit && milestones[i].addr == pc) milestones[i].hit = 1;
            ring[ring_pos++ % RING] = pc;
        }
        DolRecompFunction fn = dolrecomp_find_original(pc);
        if (!fn) {
            stop_reason = "jumped to an address with no translated code";
            break;
        }
        if (!run_window && pc < 0x81800000u) pc_counts[(pc - 0x80000000u) / 4u]++;
        cpu.pc = pc;
        cpu.downcount = 0;
        fn(&cpu);
        total_cycles += (u64)(cpu.downcount < 0 ? -cpu.downcount : 1);
        if (pc == IDLE_LOOP) {
            /* The game may be waiting for the GPU. On the console it answers quickly, so let the graphics
               thread catch up before deciding that nothing can happen until the next interrupt. */
            int signalled = 0;
            if (gx_thread_on) {
                gx_thread_drain();
                signalled = apply_gx_posted() || atomic_load(&gx_signal_head) != atomic_load(&gx_signal_tail);
            }
            if (!signalled) skip_idle_time();
            gx_adapter_note_idle();
        }
        cpu.timebase = total_cycles / TB_DIVIDER;
        if (cpu.exception) handle_exception_flags();
        if (gx_thread_on && !gx_sync_signals) apply_gx_posted();
        vi_tick();
        ai_tick();
        check_interrupts();
    }
}

int main(int argc, char** argv) {
    const char* game = "exports/disc/DATA";
    const char* report_path = "analysis/native/run.txt";
    const char* nand = "exports/native-nand";
    const char* fifo_path = NULL;
    const char* preview_path = NULL;
    const char* input_path = NULL;
    int window = 0;
    const char* capture_path = NULL;
    const char* wav_path = NULL;
    int audio_log = 0;
    u32 sim_from = 0, sim_until = 0;
    u32 log_from = 0, log_until = 0, shot_frame = 0, shot_target = 0, profile_from = 0, profile_until = 0;
    int scan_remote = 0;
    u32 capture_every = 300;
    u32 preview_every = 600;
    u64 max_cycles = 20000000000ull;
    int gx_thread_wanted = 1;
    u32 net_host_port = 0, net_player_count = 2, sim_real_remote = 0, sim_unplug_ms = 0;
    const char* net_join = NULL;
    const char* net_relay = NULL; /* --net-relay name[:port]: rooms through a relay server */
    const char* net_host_room = NULL, *net_join_room = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--game")) game = argv[i + 1];
        else if (!strcmp(argv[i], "--report")) report_path = argv[i + 1];
        else if (!strcmp(argv[i], "--nand")) nand = argv[i + 1];
        else if (!strcmp(argv[i], "--fifo")) fifo_path = argv[i + 1];
        else if (!strcmp(argv[i], "--input")) input_path = argv[i + 1];
        else if (!strcmp(argv[i], "--window")) window = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--capture")) capture_path = argv[i + 1];
        else if (!strcmp(argv[i], "--wav")) wav_path = argv[i + 1];
        else if (!strcmp(argv[i], "--audio-log")) audio_log = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--scan-remote")) scan_remote = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--sim-remote")) sscanf(argv[i + 1], "%u,%u", &sim_from, &sim_until);
        else if (!strcmp(argv[i], "--copy-log")) sscanf(argv[i + 1], "%u,%u", &log_from, &log_until);
        else if (!strcmp(argv[i], "--profile")) sscanf(argv[i + 1], "%u,%u", &profile_from, &profile_until);
        else if (!strcmp(argv[i], "--copy-shot")) sscanf(argv[i + 1], "%u,%x", &shot_frame, &shot_target);
        else if (!strcmp(argv[i], "--capture-every")) capture_every = (u32)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--gx-thread")) gx_thread_wanted = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--report-every")) fake_remote_report_every((u32)atoi(argv[i + 1]));
        else if (!strcmp(argv[i], "--remotes")) fake_remote_count((u32)atoi(argv[i + 1])); /* stand-ins, 1 to 4 */
        else if (!strcmp(argv[i], "--net-host")) net_host_port = (u32)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--net-players")) net_player_count = (u32)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--net-join")) net_join = argv[i + 1];
        else if (!strcmp(argv[i], "--net-relay")) net_relay = argv[i + 1];
        else if (!strcmp(argv[i], "--rumble")) real_remote_set_rumble(atoi(argv[i + 1]));
        else if (!strcmp(argv[i], "--net-host-room")) net_host_room = argv[i + 1];
        else if (!strcmp(argv[i], "--net-join-room")) net_join_room = argv[i + 1];
        else if (!strcmp(argv[i], "--sim-real-remote")) sim_real_remote = atoi(argv[i + 1]); /* 1, or 2 with a Nunchuk */
        else if (!strcmp(argv[i], "--sim-unplug-after")) sim_unplug_ms = (u32)atoi(argv[i + 1]) * 1000u; /* seconds */
        else if (!strcmp(argv[i], "--test-remote-driver")) return remote_driver_self_test() ? 1 : 0;
        else if (!strcmp(argv[i], "--nunchuk")) fake_remote_nunchuk(atoi(argv[i + 1]));
        else if (!strcmp(argv[i], "--nan-trace")) { /* report where float operations first make NaNs */
            extern int fast_nan_trace;
            fast_nan_trace = atoi(argv[i + 1]);
        }
        else if (!strcmp(argv[i], "--test-motion-pointer")) { /* checks motion_pointer.c, then exits */
            int motion_pointer_self_test(void);
            return motion_pointer_self_test() ? 1 : 0;
        }
        else if (!strcmp(argv[i], "--test-net-status")) /* the online status window, off-screen, into BMPs */
            return net_status_self_test(argv[i + 1]);
        else if (!strcmp(argv[i], "--test-fast-ops")) { /* checks fast_ops.c against the full helpers, then exits */
            int fast_ops_self_test(u32 rounds);
            return fast_ops_self_test((u32)atoi(argv[i + 1])) ? 1 : 0;
        }
        else if (!strcmp(argv[i], "--preview")) preview_path = argv[i + 1];
        else if (!strcmp(argv[i], "--preview-every")) preview_every = (u32)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--cycles")) max_cycles = strtoull(argv[i + 1], NULL, 10);
    }
    if (argc == 2 && !strcmp(argv[1], "--find-remote")) { /* checks the HID code without running the game */
        u32 devices = 0;
        int found;
        report = stdout;
        found = real_remote_scan_once(&devices);
        printf("%u HID devices checked, Wii Remote %s\n", devices, found ? "found" : "not found");
        return 0;
    }
    report = fopen(report_path, "w");
    if (!report) {
        fprintf(stderr, "Can't write %s\n", report_path);
        return 1;
    }
    {   /* a crash says why in the report, and saves the frame log beside it */
        char frames[600];
        size_t stem = strlen(report_path) > 4 && !strcmp(report_path + strlen(report_path) - 4, ".txt")
                          ? strlen(report_path) - 4 : strlen(report_path);
        snprintf(frames, sizeof frames, "%.*s-frames.txt", (int)stem, report_path);
        crash_install(report, frames, &cpu.pc, &cpu.lr);
    }
    gx_debug_setup(preview_path, preview_every);
    if (fifo_path && !(fifo_log = fopen(fifo_path, "wb"))) return 1;
    pc_counts = calloc(0x01800000u / 4u, sizeof(u32));
    if (!pc_counts || !cpu_init(&cpu) || !cpu_alloc_mem2(&cpu, WII_MEM2_SIZE)) return 1;
    cpu.external_read = external_read;
    cpu.external_write = external_write;
    cpu.instruction_fallback = fallback;
    if (!disc_open(game) || !load_game(game)) return 1;
    if (net_relay && !net_host_room && !net_join_room) net_relay = NULL;
    if (net_host_port || net_join || net_relay) { /* online: everyone is here before the game starts */
        static char online_save[600];
        RemoteCalibration mine;
        memset(&mine, 0, sizeof mine);
        /* This PC's real remote, set up by the runtime, so every PC's stand-in for this player can replay it. */
        if ((window == 1 || sim_real_remote) && remote_driver_start(4000, sim_real_remote != 0, sim_real_remote == 2))
            remote_driver_calibration(&mine);
        if (sim_real_remote && sim_unplug_ms) real_remote_unplug_simulated_after(sim_unplug_ms);
        snprintf(online_save, sizeof online_save, "%s-online", nand); /* a player's copy of the host's save */
        /* The game's window opens once everyone is in; until then a small window says what's happening. */
        net_status_open(window == 1);
        int joining = net_join || (net_relay && net_join_room);
        int player = net_relay && net_join_room ? net_start_join_relay(net_relay, net_join_room, online_save, &mine)
                     : net_relay ? net_start_host_relay(net_relay, net_host_room, net_player_count, nand, &mine)
                     : net_join ? net_start_join(net_join, online_save, &mine)
                     : net_start_host(net_host_port, net_player_count, nand, &mine);
        if (joining) nand = online_save;
        if (player < 0) {
            fprintf(report, "Online: couldn't start\n");
            fclose(report);
            net_status_fail(net_last_error());
            return 1;
        }
        net_status_close();
        fake_remote_net_mode(net_players(), (u32)player);
        for (u32 p = 0; p < net_players(); p++) {
            RemoteCalibration cal;
            net_calibration(p, &cal);
            fake_remote_set_calibration(p, &cal);
        }
        gx_sync_signals = 1;
    }
    fake_remote_setup(input_path);
    if (sim_until) fake_remote_simulate(sim_from, sim_until);
    else if ((window == 1 || scan_remote) && !net_active()) fake_remote_use_real(); /* online: stand-ins only, for now */
    if (window && !gx_adapter_start(argc, argv, window == 2, capture_path, capture_every)) return 1;
    if (log_until) gx_adapter_copy_log(log_from, log_until);
    if (shot_target) gx_adapter_copy_shot(shot_frame, shot_target);
    audio_setup(window == 1, wav_path, audio_log);
    ios_init(nand);
    cpu.msr = 0x00002032u;  /* FP, IR, DR, RI */
    cpu.hid2 = 0xE0000000u; /* paired singles and quantized loads on */

    run_window = window;
    run_max_cycles = max_cycles;
    run_profile_from = profile_from;
    run_profile_until = profile_until;
    /* The graphics stream gets its own thread when there's a window, unless --gx-thread 0. The fifo log
       records the stream in order with CPU events, so it keeps one thread. */
    if (window && gx_thread_wanted && !fifo_log && gx_thread_setup()) gx_thread_run(cpu_loop);
    else cpu_loop();
    /* Beside the report, so a test run never replaces what a play session left. */
    char beside[600];
    size_t stem = strlen(report_path) > 4 && !strcmp(report_path + strlen(report_path) - 4, ".txt")
                      ? strlen(report_path) - 4 : strlen(report_path);
    if (profile_until) {
        snprintf(beside, sizeof beside, "%.*s-profile.txt", (int)stem, report_path);
        profile_save(beside);
    }
    snprintf(beside, sizeof beside, "%.*s-frames.txt", (int)stem, report_path);
    gx_adapter_save_frames(beside);
    write_report();
    fclose(report);
    gx_adapter_stop();
    if (fifo_log) { /* memory at the end, so display lists and textures can be looked up */
        char path[1024];
        fclose(fifo_log);
        snprintf(path, sizeof path, "%s.mem1", fifo_path);
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(cpu.ram, 1, cpu.ram_size, f); fclose(f); }
        snprintf(path, sizeof path, "%s.mem2", fifo_path);
        f = fopen(path, "wb");
        if (f) { fwrite(cpu.mem2, 1, cpu.mem2_size, f); fclose(f); }
    }
    printf("Stopped: %s, pc %08X after %llu cycles\n", stop_reason, cpu.pc,
           (unsigned long long)total_cycles);
    return 0;
}
