/* This PC's real Wii Remote, driven by the runtime itself, for online play.

   In single play the game talks to the real remote through fake_remote.c, report for report. Online it can't:
   every PC's game talks to stand-ins, one per player, and they must all see the same reports. So this sets up
   the remote itself and streams what it measures: each report becomes a RemoteSample (raw buttons,
   accelerometer, MotionPlus gyro, Nunchuk, and sensor bar dots), sent with the player's input every field
   (net.c), and every PC's stand-in for this player replays them, reading them with this remote's own calibration,
   which goes to everyone when the player joins.

   Set-up, from wiibrew.org/wiki/Wiimote: status (is an extension in?), accelerometer calibration from EEPROM
   0x16, MotionPlus at 0xA600FA; the Nunchuk first if there is one (0x55 to 0xA400F0 and 0x00 to 0xA400FB, the
   unencrypted way, then its ID and calibration); then 0x55 to 0xA600F0 and 4, or 5 with a Nunchuk, to
   0xA600FE, which switches MotionPlus on in front of the Nunchuk; its calibration from 0xA40020; the IR camera;
   and continuous reports in mode 0x37 (buttons, accelerometer, ten bytes of IR, six of extension).

   It runs on its own thread, so Bluetooth waits never hold up the game. A Nunchuk plugged in or out later is
   seen in the MotionPlus data's extension bit, and MotionPlus is switched over to match. With no sensor bar,
   the dots come from the MotionPlus pointer (motion_pointer.c), or from the mouse while it moves.

   With no Wii Remote, a Joy-Con (joycon.c) plays instead: it gives samples as a Wii Remote Plus would, with its own
   fixed calibration, and the same pointer. A Wii Remote that's connected is waited for first. */
#include <windows.h>
#include <math.h>
#include <string.h>
#include "remote_driver.h"
#include "motion_pointer.h"
#include "real_remote.h"
#include "joycon.h"

#define STEP_WAIT_MS 700u
#define QUEUE 128u
#define FIELD_MS 16u
#define PLUG_CONFIRM 12u /* MotionPlus samples in a row before an extension change counts */

extern FILE* report;

static CRITICAL_SECTION lock;
static RemoteSample queue[QUEUE];
static u32 queue_head, queue_tail, dropped;
static RemoteCalibration cal;
static volatile LONG ready, plug_event = -1, wanted_leds = 1, wanted_rumble;
static int motion_plus, nunchuk_in, mode_set;
static u32 samples, inits, plugs, step_timeouts, status_reports;
static u8 sent_leds = 0xFF, sent_rumble;
static int joycon_source; /* this PC's player is a Joy-Con, not a Wii Remote */
static int joycon_a_held;

/* ---- Talking to the remote ---- */

static void send_out(const u8* bytes, u32 size) {
    u8 out[REMOTE_REPORT_SIZE];
    memset(out, 0, sizeof out);
    memcpy(out, bytes, size);
    if (size > 1) out[1] = (u8)((out[1] & ~1u) | (sent_rumble & 1u)); /* every output report carries rumble */
    real_remote_write(out, size);
}

static void set_reporting(void) {
    const u8 mode[3] = {0x12, 0x04, 0x37};
    send_out(mode, sizeof mode);
    mode_set = 1;
}

/* Reads until a report with this id arrives, or the wait runs out. Other reports pass by, except that a status
   report means the remote stopped its data reports, so they're asked for again once set up. */
static int wait_report(u8 id, u8* out, u32 ms, int (*wanted)(const u8*, const void*), const void* arg) {
    DWORD until = GetTickCount() + ms;
    double when;
    while ((LONG)(until - GetTickCount()) > 0) {
        if (!real_remote_read(out, &when)) {
            Sleep(1);
            continue;
        }
        if (out[0] == 0x20) status_reports++;
        if (out[0] == id && (!wanted || wanted(out, arg))) return 1;
    }
    step_timeouts++;
    return 0;
}

static int reply_for(const u8* r, const void* arg) { return (u32)((r[4] << 8) | r[5]) == *(const u32*)arg; }
static int ack_for(const u8* r, const void* arg) { (void)arg; return r[3] == 0x16; }

/* Reads memory: space 0 is EEPROM, 4 the registers. Replies come 16 bytes at a time. */
static int read_memory(u32 space, u32 address, u8* out, u32 size) {
    const u8 ask[7] = {0x17, (u8)space, (u8)(address >> 16), (u8)(address >> 8), (u8)address, (u8)(size >> 8), (u8)size};
    u8 r[REMOTE_REPORT_SIZE];
    send_out(ask, sizeof ask);
    for (u32 done = 0; done < size;) {
        u32 at = (address + done) & 0xFFFFu;
        if (!wait_report(0x21, r, STEP_WAIT_MS, reply_for, &at)) return 0;
        if (r[3] & 0x0Fu) return 0; /* nothing there */
        u32 step = (r[3] >> 4) + 1u;
        if (step > size - done) step = size - done;
        memcpy(out + done, r + 6, step);
        done += step;
    }
    return 1;
}

static int write_memory(u32 address, const u8* data, u32 size) {
    u8 w[22] = {0x16, 0x04, (u8)(address >> 16), (u8)(address >> 8), (u8)address, (u8)size};
    u8 r[REMOTE_REPORT_SIZE];
    memcpy(w + 6, data, size);
    send_out(w, 22);
    return wait_report(0x22, r, STEP_WAIT_MS, ack_for, NULL) && r[4] == 0;
}

static int write_byte(u32 address, u8 value) { return write_memory(address, &value, 1); }

static int status_flags(u8* flags) {
    const u8 ask[2] = {0x15, 0x00};
    u8 r[REMOTE_REPORT_SIZE];
    send_out(ask, sizeof ask);
    if (!wait_report(0x20, r, STEP_WAIT_MS, NULL, NULL)) return 0;
    *flags = r[3];
    return 1;
}

/* Waits a moment and lets status reports from a MotionPlus switching over go by. */
static void settle(u32 ms) {
    DWORD until = GetTickCount() + ms;
    u8 r[REMOTE_REPORT_SIZE];
    double when;
    while ((LONG)(until - GetTickCount()) > 0)
        if (!real_remote_read(r, &when)) Sleep(1);
        else if (r[0] == 0x20) status_reports++;
}

/* The Nunchuk on its own, MotionPlus off: unencrypted set-up, ID, calibration. */
static int start_nunchuk(void) {
    u8 id[6];
    if (!write_byte(0x04A400F0u, 0x55) || !write_byte(0x04A400FBu, 0x00)) return 0;
    if (!read_memory(4, 0xA400FAu, id, 6)) return 0;
    if (!(id[2] == 0xA4 && id[3] == 0x20 && id[4] == 0x00 && id[5] == 0x00)) return 0; /* not a Nunchuk */
    if (!read_memory(4, 0xA40020u, cal.nunchuk_cal, 16)) memset(cal.nunchuk_cal, 0, 16);
    return 1;
}

/* MotionPlus on, passing the Nunchuk through if there is one, and its calibration. */
static int start_motion_plus(int with_nunchuk) {
    if (!write_byte(0x04A600F0u, 0x55) || !write_byte(0x04A600FEu, with_nunchuk ? 0x05 : 0x04)) return 0;
    settle(150);
    for (int tries = 0; tries < 3; tries++)
        if (read_memory(4, 0xA40020u, cal.gyro, 32)) return 1;
    return 0;
}

static void stop_motion_plus(void) {
    write_byte(0x04A400F0u, 0x55);
    settle(150);
}

/* The IR camera, for a sensor bar if there is one: sensitivity from wiibrew's suggested settings, basic mode. */
static void start_camera(void) {
    static const u8 block1[9] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00, 0xC0};
    static const u8 block2[2] = {0x40, 0x00};
    const u8 on1[2] = {0x13, 0x04}, on2[2] = {0x1A, 0x04};
    send_out(on1, 2);
    send_out(on2, 2);
    write_byte(0x04B00030u, 0x08);
    write_memory(0x04B00000u, block1, 9);
    write_memory(0x04B0001Au, block2, 2);
    write_byte(0x04B00033u, 0x01);
    write_byte(0x04B00030u, 0x08);
}

static int set_up(void) {
    u8 flags = 0, id[6];
    const u8 lights[2] = {0x11, (u8)((wanted_leds & 15) << 4)};
    send_out(lights, 2);
    if (!status_flags(&flags)) return 0;
    if (!read_memory(0, 0x16u, cal.accel, 10)) return 0;
    /* A MotionPlus left on from before answers at 0xA4 and not 0xA6: switch it off first. */
    if (read_memory(4, 0xA400FAu, id, 6) && id[2] == 0xA4 && id[3] == 0x20 && id[5] == 0x05) stop_motion_plus();
    motion_plus = read_memory(4, 0xA600FAu, id, 6) && id[2] == 0xA6 && id[3] == 0x20 && id[5] == 0x05;
    status_flags(&flags);
    nunchuk_in = (flags & 2) ? start_nunchuk() : 0;
    if (motion_plus && !start_motion_plus(nunchuk_in)) motion_plus = 0;
    start_camera();
    set_reporting();
    cal.present = 1;
    cal.motion_plus = (u8)motion_plus;
    cal.nunchuk = (u8)nunchuk_in;
    inits++;
    fprintf(report, "Remote driver: set up, MotionPlus %s, Nunchuk %s\n", motion_plus ? "yes" : "no",
            nunchuk_in ? "in" : "out");
    fflush(report);
    return 1;
}

/* A Nunchuk went in or came out while MotionPlus was on: switch it over to match, as the game does. */
static void replug(int now_in) {
    stop_motion_plus();
    if (now_in) now_in = start_nunchuk();
    if (motion_plus) start_motion_plus(now_in);
    set_reporting();
    nunchuk_in = now_in;
    plugs++;
    InterlockedExchange(&plug_event, now_in);
    fprintf(report, "Remote driver: Nunchuk %s\n", now_in ? "plugged in" : "taken out");
    fflush(report);
}

/* ---- Reports to samples ---- */

static void decode_dots(const u8* ir, RemoteSample* s) {
    for (int pair = 0; pair < 1; pair++) { /* the first two of the four dots */
        const u8* p = ir + pair * 5;
        s->dots[0][0] = (u16)(p[0] | ((p[2] >> 4) & 3) << 8);
        s->dots[0][1] = (u16)(p[1] | ((p[2] >> 6) & 3) << 8);
        s->dots[1][0] = (u16)(p[3] | (p[2] & 3) << 8);
        s->dots[1][1] = (u16)(p[4] | ((p[2] >> 2) & 3) << 8);
    }
    for (int d = 0; d < 2; d++)
        if (s->dots[d][0] >= 1023 || s->dots[d][1] >= 1023) s->dots[d][0] = s->dots[d][1] = NO_DOT;
}

/* The six extension bytes: MotionPlus data (byte 5 bits 1-0 are 2), a passed-through Nunchuk (0), or with
   MotionPlus off the Nunchuk's own format. Returns the MotionPlus extension bit, or -1. */
static int decode_extension(const u8* e, RemoteSample* s) {
    if (motion_plus && (e[5] & 3u) == 2u) {
        s->gyro[0] = (u16)(e[0] | (e[3] >> 2) << 8);
        s->gyro[1] = (u16)(e[1] | (e[4] >> 2) << 8);
        s->gyro[2] = (u16)(e[2] | (e[5] >> 2) << 8);
        s->gyro_flags = (u8)(SAMPLE_HAS_MOTION_PLUS | ((e[3] >> 1) & 1) | (((e[4] >> 1) & 1) << 1) | ((e[3] & 1) << 2));
        return e[4] & 1;
    }
    if (motion_plus && (e[5] & 3u) == 0u && nunchuk_in) {
        s->stick[0] = e[0];
        s->stick[1] = e[1];
        s->naccel[0] = (u16)(e[2] << 2 | ((e[5] >> 4) & 1) << 1);
        s->naccel[1] = (u16)(e[3] << 2 | ((e[5] >> 5) & 1) << 1);
        s->naccel[2] = (u16)((e[4] >> 1) << 3 | ((e[5] >> 6) & 3) << 1);
        s->nunchuk_flags = (u8)(SAMPLE_HAS_NUNCHUK | ((e[5] & 8) ? 0 : 1) | ((e[5] & 4) ? 0 : 2));
    } else if (!motion_plus && nunchuk_in) {
        s->stick[0] = e[0];
        s->stick[1] = e[1];
        s->naccel[0] = (u16)(e[2] << 2 | ((e[5] >> 2) & 3));
        s->naccel[1] = (u16)(e[3] << 2 | ((e[5] >> 4) & 3));
        s->naccel[2] = (u16)(e[4] << 2 | ((e[5] >> 6) & 3));
        s->nunchuk_flags = (u8)(SAMPLE_HAS_NUNCHUK | ((e[5] & 2) ? 0 : 1) | ((e[5] & 1) ? 0 : 2));
    }
    return -1;
}

/* Where the game should see the sensor bar: the remote's own dots if it sees a bar, else the mouse while it
   moves, else the MotionPlus pointer; and pressing A with nothing on the screen centres that pointer, as in
   single play. */
static void choose_dots(const u8* r, double when, RemoteSample* s) {
    static int a_held;
    float x, y;
    int a_pressed = (r[2] & 0x08) && !a_held;
    a_held = (r[2] & 0x08) != 0;
    if (motion_plus) motion_pointer_feed(r, when);
    if (s->dots[0][0] != NO_DOT) return;
    if (fake_remote_mouse_pointer(&x, &y)) {
        motion_pointer_place(x, y);
        fake_remote_dots_for(x, y, s->dots);
    } else if (motion_plus && motion_pointer_get(&x, &y)) {
        fake_remote_dots_for(x, y, s->dots);
    } else if (motion_plus && a_pressed) {
        motion_pointer_calibrate();
        fake_remote_dots_for(0, 0, s->dots);
    }
}

static void keep(const RemoteSample* s) {
    EnterCriticalSection(&lock);
    if (queue_tail - queue_head == QUEUE) {
        queue_head++;
        dropped++;
    }
    queue[queue_tail++ % QUEUE] = *s;
    LeaveCriticalSection(&lock);
}

static DWORD WINAPI run(void* unused) {
    u8 r[REMOTE_REPORT_SIZE];
    double when;
    u32 changed = 0;
    (void)unused;
    for (;;) {
        if (!real_remote_connected()) {
            InterlockedExchange(&ready, 0);
            Sleep(100);
            continue;
        }
        if (!ready) {
            if (set_up()) InterlockedExchange(&ready, 1);
            else Sleep(500);
            continue;
        }
        if ((u8)wanted_leds != sent_leds || (u8)wanted_rumble != sent_rumble) {
            sent_rumble = (u8)wanted_rumble;
            sent_leds = (u8)wanted_leds;
            const u8 lights[2] = {0x11, (u8)(sent_leds << 4)};
            send_out(lights, 2);
        }
        if (!real_remote_read(r, &when)) {
            Sleep(1);
            continue;
        }
        if (r[0] == 0x20) { /* data reports stop after a status report until asked for again */
            status_reports++;
            if (!motion_plus && ((r[3] & 2) != 0) != (nunchuk_in != 0)) replug((r[3] & 2) != 0);
            else set_reporting();
            continue;
        }
        if (r[0] != 0x37) continue;
        RemoteSample s;
        memset(&s, 0, sizeof s);
        s.buttons = (u16)(((r[1] << 8) | r[2]) & 0x9F1Fu);
        s.accel[0] = (u16)(r[3] << 2 | ((r[1] >> 5) & 3));
        s.accel[1] = (u16)(r[4] << 2 | ((r[2] >> 5) & 1) << 1);
        s.accel[2] = (u16)(r[5] << 2 | ((r[2] >> 6) & 1) << 1);
        decode_dots(r + 6, &s);
        int extension = decode_extension(r + 16, &s);
        choose_dots(r, when, &s);
        samples++;
        keep(&s);
        /* MotionPlus says whether something is plugged into it: follow a lasting change. */
        if (extension >= 0) {
            changed = (extension != nunchuk_in) ? changed + 1 : 0;
            if (changed >= PLUG_CONFIRM) {
                changed = 0;
                replug(extension);
            }
        }
    }
    return 0;
}

int remote_driver_start(u32 wait_ms, int simulated, int sim_nunchuk) {
    InitializeCriticalSection(&lock);
    if (simulated) real_remote_start_simulated(sim_nunchuk);
    else real_remote_start();
    if (!simulated) joycon_start(1);
    CreateThread(NULL, 0, run, NULL, 0, NULL);
    DWORD until = GetTickCount() + wait_ms;
    /* A Joy-Con plays only if no Wii Remote is connected: a connected one is waited for. */
    while (!ready && (LONG)(until - GetTickCount()) > 0) {
        if (!simulated && joycon_ready_at(0) && !real_remote_connected()) break;
        Sleep(20);
    }
    if (!ready && !simulated && joycon_ready_at(0)) {
        joycon_source = 1;
        fprintf(report, "Remote driver: no Wii Remote; a Joy-Con plays\n");
    }
    if (!ready && !joycon_source)
        fprintf(report, "Remote driver: no Wii Remote or Joy-Con ready within %u ms; the keyboard plays\n", wait_ms);
    return ready != 0 || joycon_source;
}

int remote_driver_ready(void) { return joycon_source ? joycon_ready_at(0) : ready != 0; }

void remote_driver_calibration(RemoteCalibration* out) {
    if (joycon_source) joycon_calibration_at(0, out);
    else if (ready) *out = cal;
    else memset(out, 0, sizeof *out);
}

/* The Joy-Con's samples, with sensor bar dots as for a real remote with no sensor bar: the mouse while it moves,
   else its MotionPlus pointer, and A pressed with nothing on the screen centres that pointer. Single play
   (fake_remote.c) uses it too. */
u32 remote_driver_take_joycon(RemoteSample* out, u32 max, s32* plug) {
    double when[64];
    int recentre = 0;
    if (max > 64) max = 64;
    u32 n = joycon_take_at(0, out, when, max, plug, &recentre);
    if (recentre) motion_pointer_recentre();
    for (u32 i = 0; i < n; i++) {
        RemoteSample* s = &out[i];
        u8 r[REMOTE_REPORT_SIZE];
        float x, y;
        joycon_pointer_report(0, i, s, r);
        int a_pressed = (s->buttons & 0x0008u) && !joycon_a_held;
        joycon_a_held = (s->buttons & 0x0008u) != 0;
        motion_pointer_feed(r, when[i]);
        if (fake_remote_mouse_pointer(&x, &y)) {
            motion_pointer_place(x, y);
            fake_remote_dots_for(x, y, s->dots);
        } else if (motion_pointer_get(&x, &y)) {
            fake_remote_dots_for(x, y, s->dots);
        } else if (a_pressed) {
            motion_pointer_calibrate();
            fake_remote_dots_for(0, 0, s->dots);
        }
    }
    return n;
}

u32 remote_driver_take(RemoteSample* out, u32 max, s32* plug) {
    u32 n = 0;
    if (joycon_source) return remote_driver_take_joycon(out, max, plug);
    *plug = InterlockedExchange(&plug_event, -1);
    if (!ready) return 0;
    EnterCriticalSection(&lock);
    /* Far behind, as when samples piled up while players joined: keep only the newest, so play isn't late. */
    if (queue_tail - queue_head > 3u * max) {
        dropped += queue_tail - queue_head - max;
        queue_head = queue_tail - max;
    }
    while (queue_head != queue_tail && n < max) out[n++] = queue[queue_head++ % QUEUE];
    LeaveCriticalSection(&lock);
    for (u32 i = 0; i < n; i++) out[i].offset = (u8)(i * FIELD_MS / n);
    return n;
}

void remote_driver_output(u32 leds, int rumble) {
    InterlockedExchange(&wanted_leds, (LONG)(leds & 15u));
    InterlockedExchange(&wanted_rumble, rumble ? 1 : 0);
    if (joycon_source) joycon_output_at(0, leds, rumble);
}

void remote_driver_write_report(FILE* out) {
    fprintf(out, "remote driver: %s, %u set-ups, %u samples, %u dropped, %u Nunchuk changes, %u status reports, "
                 "%u timed-out steps\n",
            ready ? "ready" : "not ready", inits, samples, dropped, plugs, status_reports, step_timeouts);
    if (joycon_source) joycon_write_report(out);
}

/* --test-remote-driver: sets up the simulated remote and checks what comes back. */
int remote_driver_self_test(void) {
    int failures = 0;
    if (!report) report = stdout;
    real_remote_sim_still_for(0);
    if (!remote_driver_start(5000, 1, 1)) {
        printf("not ready\n");
        return 1;
    }
    printf("calibration: accel %02X %02X %02X / %02X %02X %02X, MotionPlus %d, Nunchuk %d, gyro zero %02X%02X, "
           "Nunchuk stick centre %02X\n",
           cal.accel[0], cal.accel[1], cal.accel[2], cal.accel[4], cal.accel[5], cal.accel[6], cal.motion_plus,
           cal.nunchuk, cal.gyro[0], cal.gyro[1], cal.nunchuk_cal[10]);
    if (!cal.motion_plus || !cal.nunchuk || cal.accel[0] != 0x82 || cal.nunchuk_cal[10] != 0x84) failures++;
    Sleep(500);
    RemoteSample got[MAX_SAMPLES];
    s32 plug;
    remote_driver_take(got, MAX_SAMPLES, &plug); /* what piled up meanwhile is trimmed, as when players join */
    u32 total = 0, with_gyro = 0, with_nunchuk = 0;
    u16 yaw_low = 0xFFFF, yaw_high = 0, stick_x = 0;
    for (int field = 0; field < 60; field++) {
        Sleep(16);
        u32 n = remote_driver_take(got, MAX_SAMPLES, &plug);
        for (u32 i = 0; i < n; i++) {
            total++;
            if (got[i].gyro_flags & SAMPLE_HAS_MOTION_PLUS) {
                with_gyro++;
                if (got[i].gyro[0] < yaw_low) yaw_low = got[i].gyro[0];
                if (got[i].gyro[0] > yaw_high) yaw_high = got[i].gyro[0];
            }
            if (got[i].nunchuk_flags & SAMPLE_HAS_NUNCHUK) {
                with_nunchuk++;
                stick_x = got[i].stick[0];
            }
        }
    }
    printf("one second: %u samples, %u with MotionPlus (yaw %u to %u), %u with the Nunchuk (stick x %u)\n", total,
           with_gyro, yaw_low, yaw_high, with_nunchuk, stick_x);
    if (total < 90 || with_gyro < 50 || with_nunchuk < 50 || yaw_high - yaw_low < 200 || stick_x != 0xC0) failures++;
    remote_driver_write_report(stdout);
    printf("remote driver self-test: %d failures\n", failures);
    return failures;
}
