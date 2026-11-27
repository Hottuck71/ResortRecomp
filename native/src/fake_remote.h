/* See fake_remote.c. */
#ifndef FAKE_REMOTE_H
#define FAKE_REMOTE_H
#include "host.h"

#include "remote_sample.h" /* RemoteSample and RemoteCalibration */

#define MAX_SAMPLES 6u /* a remote reports about 170 times a second: under three a field */

/* One player's input for one video field, as online play sends it: everything a stand-in remote is built from.
   Floats travel as their bits, so every PC builds the same reports from them. */
typedef struct {
    u32 buttons;              /* the remote's 16 bits, and the Nunchuk's C (0x100000) and Z (0x200000) */
    float x, y;               /* pointer, in the stand-in's own units (see fake_remote_live) */
    s32 pointer;              /* 1 while pointing at the screen */
    float pitch, roll, swing; /* tilt in radians, swing 0 to 1 */
    float nx, ny, nswing;     /* Nunchuk stick, -1 to 1, and a pump of it */
    s32 plug;                 /* -1 nothing, 0 take the Nunchuk out, 1 plug it in */
    u32 sample_count;         /* with a real remote: its reports during the field, replayed by the stand-in */
    RemoteSample samples[MAX_SAMPLES];
} RemoteInput;

void fake_remote_setup(const char* script_path); /* NULL for a remote with no input */
void fake_remote_count(u32 count);               /* stand-in remotes, 1 to 4; before fake_remote_setup */
void fake_remote_start(void);                    /* from the WUDInit replacement */
u32 fake_remote_buffer(void);                    /* stands in for the Bluetooth buffer pool */
void fake_remote_send(u32 handle, u32 buffer);   /* the game sends a report to one remote */
int fake_remote_tick(CPUState* ctx);             /* returns 1 if it redirected ctx to a callback */
void fake_remote_verify_motion_plus(u32 channel);
/* Live input from the player. Pitch and roll are radians, swing is 0 to 1. */
void fake_remote_live(u32 buttons, float x, float y, int pointer, float pitch, float roll, float swing);
void fake_remote_nunchuk(int on);               /* --nunchuk 1: start with it plugged in; before fake_remote_setup */
/* The stand-in Nunchuk: stick -1 to 1 (up positive), C and Z held, pump 0 to 1, and the key that plugs it in or out. */
void fake_remote_live_nunchuk(float x, float y, int c, int z, float swing, int plug_key);
void fake_remote_use_real(void);                     /* start looking for a real remote */
void fake_remote_simulate(u32 from_field, u32 until_field); /* tests the switching logic */
int fake_remote_is_real(void);
void fake_remote_field(void);                    /* once per video field */
void fake_remote_report_every(u32 ticks);        /* stand-in reports every this many ms */
void fake_remote_write_report(void);

/* Online play (net.c). In net mode this PC's keyboard, mouse, and script make one RemoteInput a field, which
   goes to the other players, and every stand-in is built only from the inputs every PC has for that field. */
void fake_remote_net_mode(u32 players, u32 me);          /* before fake_remote_setup; me is this PC's player */
void fake_remote_local_input(RemoteInput* out);           /* this PC's input, now */
void fake_remote_apply_input(u32 player, const RemoteInput* in); /* one player's input for this field */
void fake_remote_set_calibration(u32 player, const RemoteCalibration* cal); /* before fake_remote_setup */
/* For remote_driver.c: sensor bar dots for a pointer at window position (x, y), -1 to 1, and the mouse's
   position if it moved in the last moment. */
void fake_remote_dots_for(float x, float y, u16 dots[2][2]);
int fake_remote_mouse_pointer(float* x, float* y);

u32 host_field(void); /* main.c: video fields so far */

#endif
