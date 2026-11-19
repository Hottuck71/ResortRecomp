/* The game's CPU on its own thread, with the graphics stream decoded on the main thread. See gx_thread.c. */
#ifndef GX_THREAD_H
#define GX_THREAD_H
#include "host.h"

extern int gx_thread_on;
int gx_thread_setup(void);                 /* returns 0 if the ring can't be made */
void gx_thread_run(void (*cpu)(void));     /* runs cpu on a new thread and decodes until it returns */
void gx_thread_put(const u8* bytes, u32 size); /* CPU thread: bytes for the GPU */
void gx_thread_drain(void);                /* CPU thread: waits until everything sent has been handled */
void gx_thread_field(u32 field);           /* CPU thread: once per video field */
void gx_thread_waits(u64* ticks, u64* count); /* time the CPU thread spent waiting, since the last call */
void gx_stream_consume(const u8* bytes, u32 size); /* graphics thread: in gx_stream.c */
#endif
