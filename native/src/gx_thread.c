/* The game's CPU and the graphics stream on separate threads, as the console's CPU and GPU run side by side.

   The CPU thread writes the bytes the game sends to the GPU into a ring, and the main thread, which owns the
   window, decodes and draws them. The CPU thread only waits for the graphics thread in two places:
   - when the game goes idle with commands still queued, since it may be waiting for the GPU to finish, and
     on the console that answer comes quickly;
   - at each video field, so it can't run more than about one field ahead of what has been drawn.
   Signals from the GPU (finish, tokens) go back through gx_thread_post and are applied on the CPU thread. */
#include <windows.h>
#include <immintrin.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_thread.h"
#include "gx_stream.h"
#include "crash.h"

#define RING_SIZE (64u << 20)
#define RING_MASK (RING_SIZE - 1u)
#define WAKE_EVERY 256u /* bytes written between checks for a sleeping graphics thread */

static u8* ring;
static _Atomic u32 ring_head; /* consumed by the graphics thread */
static _Atomic u32 ring_tail; /* written by the CPU thread */
static u32 tail_local, unflushed;
static atomic_int consumer_sleeping, cpu_finished;
static HANDLE data_event;
static u32 field_fence[4];
static u64 waited_ticks, waits;
static void (*cpu_main)(void);

int gx_thread_on;

static u64 ticks(void) { return __rdtsc(); }

int gx_thread_setup(void) {
    ring = VirtualAlloc(NULL, RING_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    data_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    gx_thread_on = ring && data_event;
    return gx_thread_on;
}

/* Wakes the graphics thread if it went to sleep waiting for data. */
static void wake_consumer(void) {
    unflushed = 0;
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&consumer_sleeping, memory_order_relaxed)) SetEvent(data_event);
}

void gx_thread_put(const u8* bytes, u32 size) {
    while (tail_local + size - atomic_load_explicit(&ring_head, memory_order_acquire) > RING_SIZE) {
        wake_consumer(); /* full: the graphics thread is behind */
        SwitchToThread();
    }
    u32 at = tail_local & RING_MASK;
    if (at + size <= RING_SIZE) {
        memcpy(ring + at, bytes, size);
    } else {
        for (u32 i = 0; i < size; i++) ring[(tail_local + i) & RING_MASK] = bytes[i];
    }
    tail_local += size;
    atomic_store_explicit(&ring_tail, tail_local, memory_order_release);
    unflushed += size;
    if (unflushed >= WAKE_EVERY) wake_consumer();
}

/* Waits until the graphics thread has handled every byte up to position `until`. */
static void wait_for(u32 until) {
    if ((s32)(atomic_load_explicit(&ring_head, memory_order_acquire) - until) >= 0) return;
    wake_consumer();
    u64 start = ticks();
    u32 spins = 0;
    while ((s32)(atomic_load_explicit(&ring_head, memory_order_acquire) - until) < 0) {
        if (atomic_load_explicit(&cpu_finished, memory_order_relaxed)) break;
        if (++spins < 4000) _mm_pause();
        else SwitchToThread();
    }
    waited_ticks += ticks() - start;
    waits++;
}

void gx_thread_drain(void) {
    if (gx_thread_on) wait_for(tail_local);
}

void gx_thread_field(u32 field) {
    if (!gx_thread_on) return;
    field_fence[field & 3u] = tail_local;
    if (field) wait_for(field_fence[(field - 1u) & 3u]);
}

void gx_thread_waits(u64* ticks_out, u64* count_out) {
    *ticks_out = waited_ticks;
    *count_out = waits;
    waited_ticks = 0;
    waits = 0;
}

static DWORD WINAPI cpu_thread(void* unused) {
    (void)unused;
    crash_thread_start();
    cpu_main();
    atomic_store(&cpu_finished, 1);
    SetEvent(data_event);
    return 0;
}

void gx_thread_run(void (*cpu)(void)) {
    cpu_main = cpu;
    /* Translated code turns guest calls into host calls, so the CPU thread gets a big stack. */
    HANDLE thread = CreateThread(NULL, 256u << 20, cpu_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!thread) {
        gx_thread_on = 0;
        cpu();
        return;
    }
    u32 head = 0;
    for (;;) {
        u32 tail = atomic_load_explicit(&ring_tail, memory_order_acquire);
        if (tail != head) {
            u32 size = tail - head, at = head & RING_MASK;
            if (size > RING_SIZE - at) size = RING_SIZE - at;
            gx_stream_consume(ring + at, size);
            head += size;
            atomic_store_explicit(&ring_head, head, memory_order_release);
            continue;
        }
        /* Only the CPU thread ends the run, even when the window closes, so it is never left waiting here. */
        if (atomic_load(&cpu_finished)) break;
        u32 spins = 0;
        while (atomic_load_explicit(&ring_tail, memory_order_acquire) == head && ++spins < 2000) _mm_pause();
        if (spins >= 2000) {
            atomic_store(&consumer_sleeping, 1);
            if (atomic_load(&ring_tail) == head && !atomic_load(&cpu_finished)) WaitForSingleObject(data_event, 2);
            atomic_store(&consumer_sleeping, 0);
        }
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
