/* Nintendo Switch Joy-Cons as Wii Remotes Plus (and the left one as a Nunchuk). See joycon.c. */
#ifndef JOYCON_H
#define JOYCON_H
#include <stdio.h>
#include "remote_sample.h" /* no game headers: the launcher and PairJoyCon.exe build this too */

#define JOYCON_SLOTS 4 /* players: each a Joy-Con standing in for a Wii Remote */

/* Starts looking for Joy-Cons paired with Windows. pair_nunchuk 1: a left Joy-Con next to a right one is the right
   one's Nunchuk (for one player, as Wii Sports Resort); 0: every Joy-Con is a remote of its own. Safe to call twice. */
void joycon_start(int pair_nunchuk);
int joycon_ready_at(int slot);
/* The calibration a stand-in reads this player's samples with: the same every time, a Wii Remote Plus's. */
void joycon_calibration_at(int slot, RemoteCalibration* out);
/* The player's samples since the last call (about 200 a second; offsets spread over a field), each one's time in
   seconds in when (may be NULL), *plug 1 or 0 when the Nunchuk Joy-Con came or went (else -1), and *recentre 1 when
   the player pressed the pointer's re-centre button (may be NULL). Dots are left as NO_DOT for the caller. */
u32 joycon_take_at(int slot, RemoteSample* out, double* when, u32 max, s32* plug, int* recentre);
void joycon_output_at(int slot, u32 leds, int rumble); /* the game's player lights and rumble */
/* Held sideways (stick on the left, as Mario Party 9 is played): buttons and motion turned to match. Every Joy-Con,
   now and later; SL and SR pressed together switch one Joy-Con. */
void joycon_set_sideways(int on);
/* A Wii Remote data report (mode 0x37) carrying this sample, for motion_pointer_feed. */
void joycon_make_report(const RemoteSample* s, u8 out[22]);
void joycon_write_report(FILE* out);
/* How many Joy-Cons Windows has connected as game devices, left and right, without opening them. */
int joycon_scan_once(u32* left, u32* right);
int joycon_self_test(void); /* --test-joycon: the conversions, with no Joy-Con needed */

#endif
