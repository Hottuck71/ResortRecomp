/* Writes why the program died into the run report. See crash.c. */
#ifndef CRASH_H
#define CRASH_H
#include <stdint.h>
#include <stdio.h>
void crash_install(FILE* report, const char* frames_path, const volatile uint32_t* guest_pc,
                   const volatile uint32_t* guest_lr);
void crash_thread_start(void); /* in each thread that runs game code, so a stack overflow can be reported */
#endif
