/* A pointer worked out from a real remote's MotionPlus, for play without a sensor bar. See motion_pointer.c. */
#ifndef MOTION_POINTER_H
#define MOTION_POINTER_H
#include <stdio.h>
#include "host.h"

void motion_pointer_feed(const u8* report, double seconds); /* every data report from the real remote, and when it came */
int motion_pointer_get(float* x, float* y);  /* window units, -1 to 1; returns 1 while it is on the screen */
void motion_pointer_place(float x, float y); /* puts it where the mouse is */
void motion_pointer_recentre(void);
void motion_pointer_calibrate(void); /* A pressed with no pointer on the screen */
void motion_pointer_flip_x(void);
void motion_pointer_flip_y(void);
void motion_pointer_write_report(FILE* report);
int motion_pointer_self_test(void);
#endif
