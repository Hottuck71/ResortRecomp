/* Shared by the host sources. Contains no emulator code. */
#ifndef HOST_H
#define HOST_H
#include <stdio.h>
#include "generated.h"

extern CPUState cpu;
extern FILE* report;

/* main.c */
u8* plain_memory(u32 addr, u32 size);
u64 load_be(const u8* p, u32 size);
void store_be(u8* p, u64 v, u32 size);
void read_guest_string(u32 addr, char* out, u32 size);
void host_stop(const char* reason);

/* disc.c: the game's disc, served from the extracted files. They are only ever read. */
int disc_open(const char* game);
int disc_read(u64 offset, u8* out, u32 size);
const u8* disc_header(void); /* the first 0x440 bytes of boot.bin */
const u8* disc_fst(u32* size);
const u8* disc_bi2(u32* size);
const u8* disc_tmd(u32* size);
const u8* disc_ticket(u32* size);
void disc_write_report(void);

/* ios.c: the services the game asks IOS for. */
void ios_init(const char* nand_root);
/* Returns 1 when the request is answered now, with the result in *result. */
int ios_handle(u8* request, s32* result);

#endif
