/* --profile from,until: about 1000 samples a second of where the game thread is. Translated code stores
   its guest address before each memory access, so the guest address is close to exact. Reported by
   scripts/53_profile_report.py. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "profile.h"

#define MAX_SAMPLES 2000000u
#define PREFERRED_BASE 0x140000000ull

static struct {
    uint32_t pc, frame;
    uint64_t rip;
} *samples;
static uint32_t count, from, until;
static const volatile uint32_t* guest_pc;
static uint32_t (*current_frame)(void);
static HANDLE game_thread;

static DWORD WINAPI sampler(void* unused) {
    (void)unused;
    timeBeginPeriod(1);
    for (;;) {
        Sleep(1);
        uint32_t frame = current_frame();
        if (frame < from || frame >= until || count >= MAX_SAMPLES) continue;
        CONTEXT state;
        state.ContextFlags = CONTEXT_CONTROL;
        SuspendThread(game_thread);
        if (GetThreadContext(game_thread, &state)) {
            samples[count].pc = *guest_pc;
            samples[count].rip = state.Rip;
            samples[count].frame = frame;
            count++;
        }
        ResumeThread(game_thread);
    }
    return 0;
}

void profile_start(uint32_t first, uint32_t last, const volatile uint32_t* pc, uint32_t (*frame)(void)) {
    from = first;
    until = last;
    guest_pc = pc;
    current_frame = frame;
    samples = malloc(sizeof *samples * MAX_SAMPLES);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, sampler, NULL, 0, NULL);
}

/* Host addresses are saved as they would be at the program's preferred base, to match its symbol table. */
void profile_save(const char* path) {
    FILE* f = fopen(path, "w");
    uint64_t base = (uint64_t)GetModuleHandleA(NULL);
    if (!f) return;
    for (uint32_t i = 0; i < count; i++)
        fprintf(f, "%08X %llX %u\n", samples[i].pc, (unsigned long long)(samples[i].rip - base + PREFERRED_BASE),
                samples[i].frame);
    fclose(f);
}
