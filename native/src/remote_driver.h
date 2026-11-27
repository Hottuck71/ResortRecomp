/* This PC's real Wii Remote, driven by the runtime itself for online play. See remote_driver.c. */
#ifndef REMOTE_DRIVER_H
#define REMOTE_DRIVER_H
#include <stdio.h>
#include "fake_remote.h"

/* Starts talking to the remote (real_remote.c, or its simulator with --sim-real-remote) and waits up to wait_ms
   for one to answer and be set up: MotionPlus on, the Nunchuk if there is one, calibration read, reports
   flowing. Returns 1 if it's ready. */
int remote_driver_start(u32 wait_ms, int simulated, int sim_nunchuk);
int remote_driver_ready(void);
void remote_driver_calibration(RemoteCalibration* out);
/* CPU thread, once a field: the remote's samples since the last call (spread evenly over the field), and
   *plug set to 1 or 0 when the Nunchuk went in or came out, else -1. */
u32 remote_driver_take(RemoteSample* out, u32 max, s32* plug);
/* The game's player lights and rumble for this PC's stand-in, passed on to the real remote. */
void remote_driver_output(u32 leds, int rumble);
void remote_driver_write_report(FILE* out);
/* A Joy-Con as this PC's remote (joycon.c, player 1), with its pointer's dots: online when no Wii Remote is
   there, and single play. */
u32 remote_driver_take_joycon(RemoteSample* out, u32 max, s32* plug);
int remote_driver_self_test(void); /* --test-remote-driver: against the simulated remote */

#endif
