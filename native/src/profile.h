/* See profile.c. */
#ifndef PROFILE_H
#define PROFILE_H
#include <stdint.h>

void profile_start(uint32_t first, uint32_t last, const volatile uint32_t* pc, uint32_t (*frame)(void));
void profile_save(const char* path);

#endif
