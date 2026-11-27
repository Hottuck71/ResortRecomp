/* Nintendo Switch Joy-Cons as Wii Remotes Plus, through Windows HID. A Joy-Con has to be paired with Windows first
   (hold the small sync button on its rail, then add it in Bluetooth settings).

   Nothing here talks Wii Remote protocol: each Joy-Con's buttons, stick, accelerometer, and gyro become
   RemoteSamples, the same raw values a real Wii Remote Plus with MotionPlus sends, read with a fixed calibration
   (joycon_calibration_at). The stand-ins in fake_remote.c replay them, as they replay a real remote's samples
   online, so the game's own WPAD, KPAD, and MotionPlus code see a Wii Remote Plus.

   Holding it like a Wii Remote (upright, its top pointed at the screen):
     right Joy-Con: A is A, ZR is B (the trigger under your finger, where a Wii Remote's B is), the stick is the
       d-pad, Y is 1, B is 2, X is -, + is +, HOME is HOME, and R or pressing the stick re-centres the pointer.
     left Joy-Con on its own: the right arrow is A, ZL is B, the stick is the d-pad, the left arrow is 1, the down
       arrow is 2, the up arrow is +, - is -, Capture is HOME, and L or pressing the stick re-centres the pointer.
     With pair_nunchuk (Wii Sports Resort) a left Joy-Con beside a right one is the Nunchuk: its stick is the
       Nunchuk's, ZL is Z, L is C, its arrows are the d-pad too, and - is -.
   Held sideways (stick on the left, rails up, as Mario Party 9 and some Wii Party games want the remote): the
   face button at the bottom is 2, the right one 1, the left one A, the top one B; the stick is the d-pad turned to
   match; SL is - and SR is +. SL and SR together switch that Joy-Con between upright and sideways.

   Protocol: github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering. Output report 0x01 carries rumble and a
   subcommand, whose reply is input report 0x21; 0x10 carries rumble alone. Set-up: SPI flash reads for the stick
   calibration (factory 0x603D left / 0x6046 right, user 0x8010 / 0x801B) and the IMU's (0x6020, user 0x8026),
   0x40 1 (IMU on), 0x48 1 (rumble on), 0x03 0x30 (full reports), 0x30 (player lights). Report 0x30 then comes
   about 67 times a second with three IMU samples 5 ms apart: 200 samples a second.

   Axes. Turning a Joy-Con's own axes into a Wii Remote's follows SDL's Switch driver (which turns them into
   SDL's: +x right, +y up out of the buttons, +z towards you, held as half of a pad) and then a Wii Remote's
   (+x to its left, +y towards its tip, +z up out of its buttons, as Dolphin's emulated remote). Accelerometer
   values are what it measures (1 g up at rest), 0x80 zero and 0x9A for 1 g in the top eight of ten bits, as
   fake_remote.c's stand-in. MotionPlus rates follow Dolphin's emulated MotionPlus: each raw value is 8192 plus
   the rate about that axis (pitch x, roll y, yaw z, by the right-hand rule in those axes), 20 counts a degree a
   second in slow mode and 4.4 in fast, as motion_pointer.c and the stand-in's calibration expect.

   Each Joy-Con has its own thread, so Bluetooth waits never hold up the game or each other, and a scanning thread
   finds new ones every two seconds. */
#include <windows.h>
#include <hidsdi.h>
#include <math.h>
#include <setupapi.h>
#include <string.h>
#include "joycon.h"

#define NINTENDO_VENDOR 0x057E
#define JOYCON_LEFT_PRODUCT 0x2006
#define JOYCON_RIGHT_PRODUCT 0x2007
#define MAX_DEVICES 8
#define QUEUE 256u
#define MAX_REPORT 512
#define SCAN_EVERY_MS 2000u
#define RETRY_AFTER_MS 6000u   /* a Joy-Con that didn't answer set-up (paired but switched off) */
#define REPLY_WAIT_MS 400u
#define READ_WAIT_MS 100u
#define LOST_AFTER_MS 3000u    /* no report this long: it's gone */
#define FIELD_MS 16u

#define STICK_DEAD 0.15f
#define STICK_DPAD 0.5f
#define ACCEL_ZERO 512.0f      /* ten-bit, the calibration below */
#define ACCEL_PER_G 104.0f     /* 0x9A - 0x80, times 4 */
#define NUNCHUK_ACCEL_PER_G 204.0f /* 0xB3 - 0x80, times 4 */
#define NUNCHUK_STICK_RANGE 96.0f
#define MP_ZERO 8192.0f
#define MP_SLOW_COUNTS 20.0f   /* a degree a second */
#define MP_FAST_COUNTS 4.4f
#define MP_MAX 16383.0f
#define GYRO_STILL_DPS 1.5f    /* spread over a window below this on every axis: held still, learn the zero */
#define GYRO_STILL_SAMPLES 100u
#define GYRO_STILL_LIMIT 12.0f /* a learned zero further off than this is no zero */

/* The Joy-Con's buttons, as one number: report bytes 3 (right), 4 (shared), 5 (left). */
#define J_Y 0x000001u
#define J_X 0x000002u
#define J_B 0x000004u
#define J_A 0x000008u
#define J_RSR 0x000010u
#define J_RSL 0x000020u
#define J_R 0x000040u
#define J_ZR 0x000080u
#define J_MINUS 0x000100u
#define J_PLUS 0x000200u
#define J_RSTICK 0x000400u
#define J_LSTICK 0x000800u
#define J_HOME 0x001000u
#define J_CAPTURE 0x002000u
#define J_DOWN 0x010000u
#define J_UP 0x020000u
#define J_RIGHT 0x040000u
#define J_LEFT 0x080000u
#define J_LSR 0x100000u
#define J_LSL 0x200000u
#define J_L 0x400000u
#define J_ZL 0x800000u

/* A Wii Remote's, as fake_remote.c */
#define W_LEFT 0x0100u
#define W_RIGHT 0x0200u
#define W_DOWN 0x0400u
#define W_UP 0x0800u
#define W_PLUS 0x1000u
#define W_TWO 0x0001u
#define W_ONE 0x0002u
#define W_B 0x0004u
#define W_A 0x0008u
#define W_MINUS 0x0010u
#define W_HOME 0x0080u

extern FILE* report;

typedef struct {
    u32 buttons;
    float sx, sy;      /* the stick, -1 to 1, up positive, in the Joy-Con's own upright frame */
    float accel[3];    /* g, the Joy-Con's axes */
    float gyro[3];     /* degrees a second, the Joy-Con's axes, zero taken off */
    double when;
} Frame;

typedef struct {
    int used;                 /* this entry holds a Joy-Con (its thread may still be setting it up) */
    char path[1024];
    HANDLE handle, thread;
    int right;
    u32 in_size, out_size;
    u8 counter;
    int use_set_output_report;
    /* calibration */
    int stick_center[2], stick_above[2], stick_below[2];
    float accel_scale[3], gyro_scale[3];
    s16 gyro_offset[3];
    float gyro_zero[3];       /* learned while held still, on top of the factory offset */
    float still_min[3], still_max[3], still_sum[3];
    u32 still_count, zeros_learned;
    /* shared with the game's threads, under lock */
    Frame queue[QUEUE];
    u32 head, tail, dropped;
    Frame latest;
    volatile LONG ready, gone;
    LONG wanted_leds, wanted_rumble;
    int sideways, rails_held;
    u32 last_buttons;
    u32 reports, frames, setups;
    int sent_leds, sent_rumble;
    DWORD rumble_sent_at;
} Device;

typedef struct {
    int remote, nunchuk;      /* device indices, or -1 */
    int nunchuk_known;        /* the Nunchuk the game was last told about */
    volatile LONG plug_event;
    int recentre_pending;
    u32 leds;
    int rumble;
} Player;

static CRITICAL_SECTION lock;
static Device devices[MAX_DEVICES];
static Player players[JOYCON_SLOTS];
static int started, pair_mode;
static volatile LONG sideways_all;
static struct { char path[1024]; DWORD at; } failures[MAX_DEVICES];
static u32 scans, found_total;

static double seconds_now(void) {
    static LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---- Talking to a Joy-Con ---- */

/* Rumble for both of its motors: 320 Hz high band and 160 Hz low band at this strength, 0 to 1. */
static void rumble_bytes(float amp, u8 out[4]) {
    const u32 hf = 0x0100u, lf = 0x40u; /* the encoded frequencies */
    u32 enc = 0;
    if (amp > 0.23f) enc = (u32)lroundf(log2f(amp * 8.7f) * 32.0f);
    else if (amp > 0.12f) enc = (u32)lroundf(log2f(amp * 17.0f) * 16.0f);
    u32 hf_amp = enc * 2u, lf_amp = enc / 2u + 0x40u;
    out[0] = (u8)(hf & 0xFFu);
    out[1] = (u8)(hf_amp + ((hf >> 8) & 0xFFu));
    out[2] = (u8)(lf + ((lf_amp >> 8) & 0xFFu));
    out[3] = (u8)(lf_amp & 0xFFu);
}

static int write_report(Device* d, const u8* data, u32 size) {
    u8 buffer[MAX_REPORT];
    OVERLAPPED wait;
    DWORD done = 0;
    u32 length = d->out_size && d->out_size <= MAX_REPORT ? d->out_size : 49u;
    memset(buffer, 0, sizeof buffer);
    memcpy(buffer, data, size < length ? size : length);
    if (d->use_set_output_report) return HidD_SetOutputReport(d->handle, buffer, length);
    memset(&wait, 0, sizeof wait);
    wait.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    BOOL finished = WriteFile(d->handle, buffer, length, &done, &wait);
    if (!finished && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(wait.hEvent, 500) == WAIT_OBJECT_0) {
            finished = GetOverlappedResult(d->handle, &wait, &done, FALSE);
        } else {
            CancelIo(d->handle);
            GetOverlappedResult(d->handle, &wait, &done, TRUE);
        }
    }
    CloseHandle(wait.hEvent);
    if (finished) return 1;
    if (HidD_SetOutputReport(d->handle, buffer, length)) {
        d->use_set_output_report = 1;
        return 1;
    }
    return 0;
}

static void rumble_data(Device* d, u8* out8) {
    u8 motor[4];
    rumble_bytes(d->sent_rumble ? 0.6f : 0.0f, motor);
    memcpy(out8, motor, 4);
    memcpy(out8 + 4, motor, 4);
}

static int send_subcommand(Device* d, u8 id, const u8* args, u32 size) {
    u8 r[64];
    memset(r, 0, sizeof r);
    r[0] = 0x01;
    r[1] = (u8)(d->counter++ & 0x0Fu);
    rumble_data(d, r + 2);
    r[10] = id;
    if (args && size) memcpy(r + 11, args, size > 40 ? 40 : size);
    return write_report(d, r, 11 + size);
}

static void send_rumble(Device* d) {
    u8 r[10];
    r[0] = 0x10;
    r[1] = (u8)(d->counter++ & 0x0Fu);
    rumble_data(d, r + 2);
    write_report(d, r, sizeof r);
}

/* Reads one input report, waiting up to ms. Returns its size, 0 if none came, -1 if the Joy-Con is gone. */
static int read_report(Device* d, u8* buffer, u32 ms) {
    OVERLAPPED wait;
    DWORD got = 0;
    u32 want = d->in_size && d->in_size <= MAX_REPORT ? d->in_size : MAX_REPORT;
    memset(&wait, 0, sizeof wait);
    wait.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    int result = 0;
    if (ReadFile(d->handle, buffer, want, &got, &wait)) {
        result = (int)got;
    } else if (GetLastError() != ERROR_IO_PENDING) {
        result = -1;
    } else if (WaitForSingleObject(wait.hEvent, ms) != WAIT_OBJECT_0) {
        CancelIo(d->handle);
        GetOverlappedResult(d->handle, &wait, &got, TRUE);
        result = 0;
    } else {
        result = GetOverlappedResult(d->handle, &wait, &got, FALSE) ? (int)got : -1;
    }
    CloseHandle(wait.hEvent);
    return result;
}

static void handle_frames(Device* d, const u8* r);

/* Sends a subcommand and waits for its reply (report 0x21 naming it). Full reports met meanwhile are kept. Returns
   the reply's size into reply, or 0. */
static int ask(Device* d, u8 id, const u8* args, u32 size, u8* reply) {
    for (int tries = 0; tries < 3; tries++) {
        if (!send_subcommand(d, id, args, size)) continue;
        DWORD until = GetTickCount() + REPLY_WAIT_MS;
        while ((LONG)(until - GetTickCount()) > 0) {
            int got = read_report(d, reply, 50);
            if (got < 0) return 0;
            if (got >= 16 && reply[0] == 0x21 && reply[14] == id) return got;
            if (got >= 49 && reply[0] == 0x30 && d->ready) handle_frames(d, reply);
        }
    }
    return 0;
}

static int read_spi(Device* d, u32 address, u8* out, u32 size) {
    u8 args[5] = {(u8)address, (u8)(address >> 8), (u8)(address >> 16), (u8)(address >> 24), (u8)size};
    u8 reply[MAX_REPORT];
    int got = ask(d, 0x10, args, sizeof args, reply);
    if (got < (int)(20 + size)) return 0;
    if (reply[15] != args[0] || reply[16] != args[1]) return 0; /* a reply to an earlier read */
    memcpy(out, reply + 20, size);
    return 1;
}

static int u12(const u8* p, int high) { return high ? (p[1] >> 4) | (p[2] << 4) : p[0] | ((p[1] & 0x0F) << 8); }

static void stick_calibration(Device* d) {
    u8 f[9], user[22];
    const u8* c = NULL;
    for (int a = 0; a < 2; a++) {
        d->stick_center[a] = 2048;
        d->stick_above[a] = d->stick_below[a] = 1400;
    }
    if (read_spi(d, d->right ? 0x6046u : 0x603Du, f, 9)) c = f;
    if (read_spi(d, 0x8010u, user, 22)) { /* user calibration, if the player ever made one */
        const u8* u = d->right ? user + 11 : user;
        if (u[0] == 0xB2 && u[1] == 0xA1) c = u + 2;
    }
    if (!c) return;
    /* left: above centre, centre, below centre; right: centre, below centre, above centre */
    const u8* above = d->right ? c + 6 : c;
    const u8* centre = d->right ? c : c + 3;
    const u8* below = d->right ? c + 3 : c + 6;
    for (int a = 0; a < 2; a++) {
        int ce = u12(centre, a), ab = u12(above, a), be = u12(below, a);
        if (ce == 0xFFF || ab == 0xFFF || be == 0xFFF || ab < 200 || be < 200) continue; /* blank flash */
        d->stick_center[a] = ce;
        d->stick_above[a] = ab;
        d->stick_below[a] = be;
    }
}

static s16 s16_at(const u8* p) { return (s16)(p[0] | (p[1] << 8)); }

static void imu_calibration(Device* d) {
    u8 f[24], user[26];
    for (int a = 0; a < 3; a++) {
        d->accel_scale[a] = 1.0f / 4096.0f;
        d->gyro_scale[a] = 1.0f / 14.2842f;
        d->gyro_offset[a] = 0;
    }
    if (!read_spi(d, 0x6020u, f, 24)) return;
    s16 accel_origin[3], accel_coeff[3], gyro_origin[3], gyro_coeff[3];
    for (int a = 0; a < 3; a++) {
        accel_origin[a] = s16_at(f + a * 2);
        accel_coeff[a] = s16_at(f + 6 + a * 2);
        gyro_origin[a] = s16_at(f + 12 + a * 2);
        gyro_coeff[a] = s16_at(f + 18 + a * 2);
    }
    if (read_spi(d, 0x8026u, user, 26) && user[0] == 0xB2 && user[1] == 0xA1)
        for (int a = 0; a < 3; a++) {
            accel_origin[a] = s16_at(user + 2 + a * 2);
            gyro_origin[a] = s16_at(user + 14 + a * 2);
        }
    for (int a = 0; a < 3; a++)
        if (accel_coeff[a] == accel_origin[a] || gyro_coeff[a] == gyro_origin[a]) return; /* blank: keep defaults */
    for (int a = 0; a < 3; a++) {
        d->accel_scale[a] = 4.0f / ((float)accel_coeff[a] - (float)accel_origin[a]);
        d->gyro_scale[a] = 936.0f / ((float)gyro_coeff[a] - (float)gyro_origin[a]);
        d->gyro_offset[a] = gyro_origin[a];
    }
}

static int set_up(Device* d) {
    u8 reply[MAX_REPORT], arg;
    if (!ask(d, 0x02, NULL, 0, reply)) return 0; /* device info: is anyone there? */
    stick_calibration(d);
    imu_calibration(d);
    arg = 0x01;
    ask(d, 0x40, &arg, 1, reply); /* IMU on */
    ask(d, 0x48, &arg, 1, reply); /* rumble on */
    arg = 0x30;
    if (!ask(d, 0x03, &arg, 1, reply)) return 0; /* full reports */
    d->sent_leds = -1;
    d->setups++;
    return 1;
}

/* ---- Reports to frames ---- */

static float stick_axis(const Device* d, int raw, int a) {
    float v = (float)(raw - d->stick_center[a]);
    v /= v > 0 ? (float)d->stick_above[a] : (float)d->stick_below[a];
    v = clampf(v, -1.0f, 1.0f);
    if (fabsf(v) < STICK_DEAD) return 0.0f;
    return (v - (v > 0 ? STICK_DEAD : -STICK_DEAD)) / (1.0f - STICK_DEAD);
}

/* Learns the gyro's zero whenever the Joy-Con sits still for half a second. */
static void learn_zero(Device* d, const float* dps) {
    if (!d->still_count)
        for (int a = 0; a < 3; a++) d->still_min[a] = d->still_max[a] = d->still_sum[a] = 0;
    for (int a = 0; a < 3; a++) {
        if (!d->still_count || dps[a] < d->still_min[a]) d->still_min[a] = dps[a];
        if (!d->still_count || dps[a] > d->still_max[a]) d->still_max[a] = dps[a];
        d->still_sum[a] += dps[a];
    }
    int moving = 0;
    for (int a = 0; a < 3; a++) moving |= d->still_max[a] - d->still_min[a] > GYRO_STILL_DPS;
    if (moving) {
        d->still_count = 0;
        return;
    }
    if (++d->still_count < GYRO_STILL_SAMPLES) return;
    float mean[3];
    int sensible = 1;
    for (int a = 0; a < 3; a++) {
        mean[a] = d->still_sum[a] / (float)d->still_count;
        sensible &= fabsf(mean[a]) < GYRO_STILL_LIMIT;
    }
    if (sensible)
        for (int a = 0; a < 3; a++) d->gyro_zero[a] += mean[a] * (d->zeros_learned ? 0.5f : 1.0f);
    d->zeros_learned++;
    d->still_count = 0;
}

static void handle_frames(Device* d, const u8* r) {
    Frame f;
    double now = seconds_now();
    memset(&f, 0, sizeof f);
    f.buttons = (u32)r[3] | ((u32)r[4] << 8) | ((u32)r[5] << 16);
    const u8* stick = d->right ? r + 9 : r + 6;
    f.sx = stick_axis(d, u12(stick, 0), 0);
    f.sy = stick_axis(d, u12(stick, 1), 1);
    d->reports++;
    /* Three IMU samples 5 ms apart; as SDL reads them, the last in the report is the oldest. */
    for (int k = 2; k >= 0; k--) {
        const u8* imu = r + 13 + k * 12;
        float raw_dps[3];
        for (int a = 0; a < 3; a++) {
            f.accel[a] = (float)s16_at(imu + a * 2) * d->accel_scale[a];
            raw_dps[a] = (float)(s16_at(imu + 6 + a * 2) - d->gyro_offset[a]) * d->gyro_scale[a];
        }
        learn_zero(d, raw_dps);
        for (int a = 0; a < 3; a++) f.gyro[a] = raw_dps[a] - d->gyro_zero[a];
        f.when = now - 0.005 * k;
        EnterCriticalSection(&lock);
        if (d->tail - d->head == QUEUE) {
            d->head++;
            d->dropped++;
        }
        d->queue[d->tail++ % QUEUE] = f;
        d->latest = f;
        d->frames++;
        LeaveCriticalSection(&lock);
    }
}

/* ---- Each Joy-Con's thread ---- */

static DWORD WINAPI run(void* arg) {
    Device* d = (Device*)arg;
    u8 r[MAX_REPORT];
    if (!set_up(d)) {
        fprintf(report, "Joy-Con: %s didn't answer (switched off?)\n", d->right ? "right" : "left");
        fflush(report);
        InterlockedExchange(&d->gone, 1);
        return 0;
    }
    EnterCriticalSection(&lock);
    d->head = d->tail;
    LeaveCriticalSection(&lock);
    InterlockedExchange(&d->ready, 1);
    fprintf(report, "Joy-Con: %s ready, stick centre %d %d, gyro offset %d %d %d\n", d->right ? "right" : "left",
            d->stick_center[0], d->stick_center[1], d->gyro_offset[0], d->gyro_offset[1], d->gyro_offset[2]);
    fflush(report);
    DWORD heard = GetTickCount();
    for (;;) {
        int leds = (int)d->wanted_leds, rumble = (int)d->wanted_rumble;
        if (leds != d->sent_leds) {
            u8 arg = (u8)(leds & 0x0F);
            d->sent_leds = leds;
            send_subcommand(d, 0x30, &arg, 1); /* player lights; the reply passes by below */
        }
        if (rumble != d->sent_rumble || (rumble && GetTickCount() - d->rumble_sent_at > 100u)) {
            d->sent_rumble = rumble;
            d->rumble_sent_at = GetTickCount();
            send_rumble(d);
        }
        int got = read_report(d, r, READ_WAIT_MS);
        if (got < 0 || GetTickCount() - heard > LOST_AFTER_MS) break;
        if (got == 0) continue;
        heard = GetTickCount();
        if (got >= 49 && r[0] == 0x30) handle_frames(d, r);
        else if (r[0] == 0x3F) { /* back to simple reports, as after a reconnect: ask again */
            u8 reply[MAX_REPORT], arg = 0x30;
            ask(d, 0x03, &arg, 1, reply);
        }
    }
    fprintf(report, "Joy-Con: %s disconnected\n", d->right ? "right" : "left");
    fflush(report);
    InterlockedExchange(&d->ready, 0);
    InterlockedExchange(&d->gone, 1);
    return 0;
}

/* ---- Finding Joy-Cons ---- */

static int recently_failed(const char* path) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (failures[i].path[0] && !strcmp(failures[i].path, path)) {
            if (GetTickCount() - failures[i].at < RETRY_AFTER_MS) return 1;
            failures[i].path[0] = 0;
        }
    return 0;
}

static void note_failure(const char* path) {
    int slot = 0;
    for (int i = 0; i < MAX_DEVICES; i++) { /* an empty entry, else the oldest */
        if (!failures[i].path[0]) {
            slot = i;
            break;
        }
        if (failures[i].at < failures[slot].at) slot = i;
    }
    snprintf(failures[slot].path, sizeof failures[slot].path, "%s", path);
    failures[slot].at = GetTickCount();
}

static int open_already(const char* path) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devices[i].used && !strcmp(devices[i].path, path)) return 1;
    return 0;
}

static void open_device(const char* path, int right) {
    int slot = -1;
    for (int i = 0; i < MAX_DEVICES && slot < 0; i++)
        if (!devices[i].used) slot = i;
    if (slot < 0) return;
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        note_failure(path);
        return;
    }
    Device* d = &devices[slot];
    EnterCriticalSection(&lock);
    memset(d, 0, sizeof *d);
    d->used = 1;
    snprintf(d->path, sizeof d->path, "%s", path);
    d->handle = h;
    d->right = right;
    d->sideways = sideways_all != 0;
    d->wanted_leds = 1;
    d->sent_leds = -1;
    LeaveCriticalSection(&lock);
    PHIDP_PREPARSED_DATA parsed;
    HIDP_CAPS caps;
    d->in_size = MAX_REPORT;
    d->out_size = 49;
    if (HidD_GetPreparsedData(h, &parsed)) {
        if (HidP_GetCaps(parsed, &caps) == HIDP_STATUS_SUCCESS) {
            if (caps.InputReportByteLength) d->in_size = caps.InputReportByteLength;
            if (caps.OutputReportByteLength) d->out_size = caps.OutputReportByteLength;
        }
        HidD_FreePreparsedData(parsed);
    }
    found_total++;
    fprintf(report, "Joy-Con: found a %s one, reports %u in, %u out\n", right ? "right" : "left", d->in_size, d->out_size);
    fflush(report);
    d->thread = CreateThread(NULL, 0, run, d, 0, NULL);
}

static void scan(void) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO list = SetupDiGetClassDevsA(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (list == INVALID_HANDLE_VALUE) return;
    scans++;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA item;
        char buffer[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_A* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)buffer;
        item.cbSize = sizeof item;
        if (!SetupDiEnumDeviceInterfaces(list, NULL, &guid, i, &item)) break;
        detail->cbSize = sizeof *detail;
        if (!SetupDiGetDeviceInterfaceDetailA(list, &item, detail, sizeof buffer, NULL, NULL)) continue;
        if (open_already(detail->DevicePath) || recently_failed(detail->DevicePath)) continue;
        HANDLE probe = CreateFileA(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (probe == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attributes;
        attributes.Size = sizeof attributes;
        int ok = HidD_GetAttributes(probe, &attributes) && attributes.VendorID == NINTENDO_VENDOR &&
                 (attributes.ProductID == JOYCON_LEFT_PRODUCT || attributes.ProductID == JOYCON_RIGHT_PRODUCT);
        CloseHandle(probe);
        if (ok) open_device(detail->DevicePath, attributes.ProductID == JOYCON_RIGHT_PRODUCT);
    }
    SetupDiDestroyDeviceInfoList(list);
}

/* Who plays as whom. Players keep their Joy-Con while it's connected. With pair_mode, player 1 is the first right
   Joy-Con (or a left one if there's no right), and the first other left Joy-Con is its Nunchuk. */
static void assign_players(void) {
    int taken[MAX_DEVICES] = {0};
    for (int p = 0; p < JOYCON_SLOTS; p++) {
        Player* pl = &players[p];
        if (pl->remote >= 0 && !devices[pl->remote].ready) pl->remote = -1;
        if (pl->nunchuk >= 0 && !devices[pl->nunchuk].ready) pl->nunchuk = -1;
        if (pl->remote >= 0) taken[pl->remote] = 1;
        if (pl->nunchuk >= 0) taken[pl->nunchuk] = 1;
    }
    if (pair_mode) {
        Player* one = &players[0];
        /* a right Joy-Con arriving takes over from a left one playing alone, which becomes its Nunchuk */
        if (one->remote >= 0 && !devices[one->remote].right && one->nunchuk < 0)
            for (int i = 0; i < MAX_DEVICES; i++)
                if (devices[i].ready && devices[i].right && !taken[i]) {
                    one->nunchuk = one->remote;
                    one->remote = i;
                    taken[i] = 1;
                    break;
                }
        for (int want_right = 1; want_right >= 0 && one->remote < 0; want_right--)
            for (int i = 0; i < MAX_DEVICES && one->remote < 0; i++)
                if (devices[i].ready && !taken[i] && devices[i].right == want_right) {
                    one->remote = i;
                    taken[i] = 1;
                }
        if (one->remote >= 0 && devices[one->remote].right && one->nunchuk < 0)
            for (int i = 0; i < MAX_DEVICES && one->nunchuk < 0; i++)
                if (devices[i].ready && !taken[i] && !devices[i].right) {
                    one->nunchuk = i;
                    taken[i] = 1;
                }
    }
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (!devices[i].ready || taken[i]) continue;
        for (int p = pair_mode ? 1 : 0; p < JOYCON_SLOTS; p++)
            if (players[p].remote < 0) {
                players[p].remote = i;
                taken[i] = 1;
                break;
            }
    }
    for (int p = 0; p < JOYCON_SLOTS; p++) {
        int now = players[p].remote >= 0 && players[p].nunchuk >= 0;
        if (players[p].remote >= 0 && now != players[p].nunchuk_known) {
            players[p].nunchuk_known = now;
            InterlockedExchange(&players[p].plug_event, now);
        }
    }
}

static DWORD WINAPI scanner(void* unused) {
    (void)unused;
    for (u32 n = 0;; n++) {
        for (int i = 0; i < MAX_DEVICES; i++) {
            Device* d = &devices[i];
            if (!d->used || !d->gone) continue;
            WaitForSingleObject(d->thread, INFINITE);
            CloseHandle(d->thread);
            CloseHandle(d->handle);
            if (!d->setups) note_failure(d->path);
            EnterCriticalSection(&lock);
            d->used = 0;
            d->ready = 0;
            LeaveCriticalSection(&lock);
        }
        if (n % (SCAN_EVERY_MS / 250u) == 0) scan();
        EnterCriticalSection(&lock);
        assign_players();
        LeaveCriticalSection(&lock);
        Sleep(250);
    }
    return 0;
}

void joycon_start(int pair_nunchuk) {
    if (started) return;
    started = 1;
    pair_mode = pair_nunchuk != 0;
    InitializeCriticalSection(&lock);
    for (int p = 0; p < JOYCON_SLOTS; p++) {
        players[p].remote = players[p].nunchuk = -1;
        players[p].plug_event = -1;
        players[p].leds = 1u << p;
    }
    CreateThread(NULL, 0, scanner, NULL, 0, NULL);
}

/* ---- Frames to Wii Remote samples ---- */

/* The Wii Remote buttons for a Joy-Con held as a remote. */
static u32 remote_buttons(int right, int sideways, u32 b, float sx, float sy) {
    u32 w = 0;
    float x = sx, y = sy;
    if (right && sideways) { /* turned the other way from a Wii Remote: see wii_axes */
        x = -x;
        y = -y;
    }
    if (y > STICK_DPAD) w |= W_UP;
    if (y < -STICK_DPAD) w |= W_DOWN;
    if (x < -STICK_DPAD) w |= W_LEFT;
    if (x > STICK_DPAD) w |= W_RIGHT;
    if (right) {
        if (!sideways) {
            if (b & J_A) w |= W_A;
            if (b & (J_Y | J_RSL)) w |= W_ONE;
            if (b & (J_B | J_RSR)) w |= W_TWO;
            if (b & J_X) w |= W_MINUS;
        } else { /* sideways: bottom 2, right 1, left A, top B */
            if (b & J_A) w |= W_TWO;
            if (b & J_X) w |= W_ONE;
            if (b & J_B) w |= W_A;
            if (b & J_Y) w |= W_B;
            if (b & J_RSL) w |= W_MINUS;
            if (b & J_RSR) w |= W_PLUS;
        }
        if (b & J_ZR) w |= W_B;
        if (b & J_PLUS) w |= W_PLUS;
        if (b & J_HOME) w |= W_HOME;
    } else {
        if (!sideways) {
            if (b & J_RIGHT) w |= W_A;
            if (b & (J_LEFT | J_LSL)) w |= W_ONE;
            if (b & (J_DOWN | J_LSR)) w |= W_TWO;
            if (b & J_UP) w |= W_PLUS;
        } else { /* the arrows sideways: left (bottom) 2, down (right) 1, up (left) A, right (top) B */
            if (b & J_LEFT) w |= W_TWO;
            if (b & J_DOWN) w |= W_ONE;
            if (b & J_UP) w |= W_A;
            if (b & J_RIGHT) w |= W_B;
            if (b & J_LSL) w |= W_MINUS;
            if (b & J_LSR) w |= W_PLUS;
        }
        if (b & J_ZL) w |= W_B;
        if (b & J_MINUS) w |= W_MINUS;
        if (b & J_CAPTURE) w |= W_HOME;
    }
    return w;
}

static u32 recentre_buttons(int right) { return right ? (J_R | J_RSTICK) : (J_L | J_LSTICK); }

/* A Joy-Con's accelerometer (g) and gyro (degrees a second) in a Wii Remote's axes: +x to its left, +y towards its
   tip, +z up out of its buttons; rates by the right-hand rule about those axes (x pitch, y roll, z yaw). Upright,
   the Joy-Con's top is the remote's tip. Sideways (stick on the left, rails up) a left Joy-Con's top points left,
   as a sideways Wii Remote's tip does, but a right one's points right: it's turned half round. */
static void wii_axes(int right, int sideways, const float* a, const float* g, float* wa, float* wg) {
    if (right) {
        wa[0] = -a[1];
        wa[1] = a[0];
        wa[2] = -a[2];
        wg[0] = g[1];
        wg[1] = -g[0];
        wg[2] = g[2];
        if (sideways) {
            wa[0] = -wa[0];
            wa[1] = -wa[1];
            wg[0] = -wg[0];
            wg[1] = -wg[1];
        }
    } else {
        wa[0] = a[1];
        wa[1] = a[0];
        wa[2] = a[2];
        wg[0] = -g[1];
        wg[1] = -g[0];
        wg[2] = -g[2];
    }
}

static u16 ten_bits(float v) { return (u16)lroundf(clampf(v, 0.0f, 1023.0f)); }

static u16 gyro_value(float dps, int* slow) {
    *slow = fabsf(dps) < (MP_MAX - MP_ZERO) / MP_SLOW_COUNTS;
    return (u16)lroundf(clampf(MP_ZERO + dps * (*slow ? MP_SLOW_COUNTS : MP_FAST_COUNTS), 0.0f, MP_MAX));
}

static void make_sample(const Device* d, const Frame* f, const Device* nd, const Frame* nf, int sideways,
                        RemoteSample* s) {
    float wa[3], wg[3];
    int slow[3];
    memset(s, 0, sizeof *s);
    s->buttons = (u16)remote_buttons(d->right, sideways, f->buttons, f->sx, f->sy);
    wii_axes(d->right, sideways, f->accel, f->gyro, wa, wg);
    for (int a = 0; a < 3; a++) s->accel[a] = ten_bits(ACCEL_ZERO + wa[a] * ACCEL_PER_G);
    s->gyro[0] = gyro_value(wg[2], &slow[0]); /* yaw */
    s->gyro[1] = gyro_value(wg[1], &slow[1]); /* roll */
    s->gyro[2] = gyro_value(wg[0], &slow[2]); /* pitch */
    s->gyro_flags = (u8)(SAMPLE_HAS_MOTION_PLUS | (slow[0] ? 1 : 0) | (slow[1] ? 2 : 0) | (slow[2] ? 4 : 0));
    s->dots[0][0] = s->dots[0][1] = s->dots[1][0] = s->dots[1][1] = NO_DOT;
    if (nd && nf) { /* the left Joy-Con as a Nunchuk, held upright as one */
        float na[3], ng[3];
        wii_axes(0, 0, nf->accel, nf->gyro, na, ng);
        s->stick[0] = (u8)lroundf(clampf(128.0f + nf->sx * NUNCHUK_STICK_RANGE, 0, 255));
        s->stick[1] = (u8)lroundf(clampf(128.0f + nf->sy * NUNCHUK_STICK_RANGE, 0, 255));
        for (int a = 0; a < 3; a++) s->naccel[a] = ten_bits(ACCEL_ZERO + na[a] * NUNCHUK_ACCEL_PER_G);
        s->nunchuk_flags = (u8)(SAMPLE_HAS_NUNCHUK | ((nf->buttons & J_L) ? 1 : 0) | ((nf->buttons & J_ZL) ? 2 : 0));
        u32 b = nf->buttons;
        if (b & J_UP) s->buttons |= W_UP;
        if (b & J_DOWN) s->buttons |= W_DOWN;
        if (b & J_LEFT) s->buttons |= W_LEFT;
        if (b & J_RIGHT) s->buttons |= W_RIGHT;
        if (b & J_MINUS) s->buttons |= W_MINUS;
    }
}

int joycon_ready_at(int slot) {
    if (!started || slot < 0 || slot >= JOYCON_SLOTS) return 0;
    EnterCriticalSection(&lock);
    int r = players[slot].remote >= 0 && devices[players[slot].remote].ready;
    LeaveCriticalSection(&lock);
    return r;
}

/* Factory calibration: zero and 1 g for each axis, then a checksum, as fake_remote.c's stand-in. */
static void accel_calibration(u8* p) {
    static const u8 values[9] = {0x80, 0x80, 0x80, 0x00, 0x9A, 0x9A, 0x9A, 0x00, 0x40};
    u32 sum = 0x55;
    memcpy(p, values, 9);
    for (int i = 0; i < 9; i++) sum += values[i];
    p[9] = (u8)sum;
}

static u32 crc32(const u8* data, u32 size) {
    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put16(u8* p, u32 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

/* MotionPlus calibration in the layout the game reads (fast half, then slow, each three zeros, three scales, and a
   reference in sixes of degrees a second; a CRC of the halves in bytes 14-15 and 30-31). The game reads a zero
   field as 5000 + field / 10 and a scale field as 4000 + field / 10 counts at the reference rate (fake_remote.c):
   zero 31920 is 8192, slow 10000 at 42 is about 20 counts a degree a second, as the stand-in's, and fast 12800 at
   200 is 4.4. */
static void gyro_calibration(u8* block) {
    static const u32 scale[2] = {12800u, 10000u}, reference[2] = {200u, 42u};
    u8 halves[28];
    memset(block, 0, 32);
    for (u32 half = 0; half < 2; half++) {
        u8* p = block + half * 16u;
        for (u32 axis = 0; axis < 3; axis++) {
            put16(p + axis * 2u, 31920u);
            put16(p + 6 + axis * 2u, scale[half]);
        }
        p[12] = (u8)reference[half];
    }
    memcpy(halves, block, 14);
    memcpy(halves + 14, block + 16, 14);
    u32 crc = crc32(halves, sizeof halves);
    block[14] = (u8)(crc >> 24);
    block[15] = (u8)(crc >> 16);
    block[30] = (u8)(crc >> 8);
    block[31] = (u8)crc;
}

/* A Nunchuk's: accelerometer zero 0x80 and 1 g 0xB3, stick 0x20 to 0xE0 about 0x80, and the two sums. */
static void nunchuk_calibration(u8* out) {
    static const u8 cal[14] = {0x80, 0x80, 0x80, 0x00, 0xB3, 0xB3, 0xB3, 0x00, 0xE0, 0x20, 0x80, 0xE0, 0x20, 0x80};
    u32 sum = 0;
    for (int i = 0; i < 14; i++) sum += cal[i];
    memcpy(out, cal, 14);
    out[14] = (u8)(sum + 0x55);
    out[15] = (u8)(sum + 0xAA);
}

void joycon_calibration_at(int slot, RemoteCalibration* out) {
    memset(out, 0, sizeof *out);
    if (!joycon_ready_at(slot)) return;
    out->present = 1;
    out->motion_plus = 1;
    EnterCriticalSection(&lock);
    out->nunchuk = (u8)(players[slot].nunchuk >= 0);
    players[slot].nunchuk_known = out->nunchuk; /* the game hears it now: no plug event for it */
    InterlockedExchange(&players[slot].plug_event, -1);
    LeaveCriticalSection(&lock);
    accel_calibration(out->accel);
    gyro_calibration(out->gyro);
    nunchuk_calibration(out->nunchuk_cal);
}

u32 joycon_take_at(int slot, RemoteSample* out, double* when, u32 max, s32* plug, int* recentre) {
    u32 n = 0;
    *plug = -1;
    if (recentre) *recentre = 0;
    if (!started || slot < 0 || slot >= JOYCON_SLOTS) return 0;
    EnterCriticalSection(&lock);
    Player* pl = &players[slot];
    *plug = InterlockedExchange(&pl->plug_event, -1);
    if (pl->remote < 0 || !devices[pl->remote].ready) {
        LeaveCriticalSection(&lock);
        return 0;
    }
    Device* d = &devices[pl->remote];
    Device* nd = pl->nunchuk >= 0 && devices[pl->nunchuk].ready ? &devices[pl->nunchuk] : NULL;
    if (nd) nd->head = nd->tail; /* only its latest state matters */
    /* Far behind, as when samples piled up while players joined: keep only the newest, so play isn't late. */
    if (d->tail - d->head > 3u * max) {
        d->dropped += d->tail - d->head - max;
        d->head = d->tail - max;
    }
    while (d->head != d->tail && n < max) {
        Frame f = d->queue[d->head++ % QUEUE];
        u32 rails = J_RSL | J_RSR | J_LSL | J_LSR;
        int both_rails = (f.buttons & (d->right ? (J_RSL | J_RSR) : (J_LSL | J_LSR))) ==
                         (d->right ? (J_RSL | J_RSR) : (J_LSL | J_LSR));
        if (both_rails && !d->rails_held) {
            d->sideways = !d->sideways;
            fprintf(report, "Joy-Con %d: held %s\n", slot + 1, d->sideways ? "sideways" : "upright");
        }
        d->rails_held = both_rails;
        if (both_rails) f.buttons &= ~rails; /* the switch, not - and + */
        u32 centre = recentre_buttons(d->right);
        if ((f.buttons & centre) && !(d->last_buttons & centre) && recentre) *recentre = 1;
        d->last_buttons = f.buttons;
        make_sample(d, &f, nd, nd ? &nd->latest : NULL, d->sideways, &out[n]);
        if (when) when[n] = f.when;
        n++;
    }
    LeaveCriticalSection(&lock);
    for (u32 i = 0; i < n; i++) out[i].offset = (u8)(i * FIELD_MS / n);
    return n;
}

void joycon_output_at(int slot, u32 leds, int rumble) {
    if (!started || slot < 0 || slot >= JOYCON_SLOTS) return;
    EnterCriticalSection(&lock);
    Player* pl = &players[slot];
    pl->leds = leds & 15u;
    pl->rumble = rumble != 0;
    if (pl->remote >= 0) {
        InterlockedExchange(&devices[pl->remote].wanted_leds, (LONG)pl->leds);
        InterlockedExchange(&devices[pl->remote].wanted_rumble, pl->rumble);
    }
    if (pl->nunchuk >= 0) InterlockedExchange(&devices[pl->nunchuk].wanted_leds, (LONG)pl->leds);
    LeaveCriticalSection(&lock);
}

void joycon_set_sideways(int on) {
    InterlockedExchange(&sideways_all, on != 0);
    if (!started) return;
    EnterCriticalSection(&lock);
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devices[i].used) devices[i].sideways = on != 0;
    LeaveCriticalSection(&lock);
}

void joycon_make_report(const RemoteSample* s, u8 out[22]) {
    const u16* g = s->gyro;
    u8 f = s->gyro_flags;
    memset(out, 0, 22);
    out[0] = 0x37;
    out[1] = (u8)((s->buttons >> 8) | ((s->accel[0] & 3u) << 5));
    out[2] = (u8)(s->buttons | (((s->accel[1] >> 1) & 1u) << 5) | (((s->accel[2] >> 1) & 1u) << 6));
    out[3] = (u8)(s->accel[0] >> 2);
    out[4] = (u8)(s->accel[1] >> 2);
    out[5] = (u8)(s->accel[2] >> 2);
    memset(out + 6, 0xFF, 10);
    out[16] = (u8)g[0];
    out[17] = (u8)g[1];
    out[18] = (u8)g[2];
    out[19] = (u8)(((g[0] >> 8) << 2) | ((f & 1) << 1) | ((f >> 2) & 1));
    out[20] = (u8)(((g[1] >> 8) << 2) | (((f >> 1) & 1) << 1) | ((s->nunchuk_flags & SAMPLE_HAS_NUNCHUK) ? 1 : 0));
    out[21] = (u8)(((g[2] >> 8) << 2) | 2);
}

void joycon_write_report(FILE* out) {
    if (!started || !found_total) return;
    EnterCriticalSection(&lock);
    for (int i = 0; i < MAX_DEVICES; i++) {
        Device* d = &devices[i];
        if (!d->used) continue;
        fprintf(out, "joy-con %s: %s, %u reports, %u samples, %u dropped, gyro zero learned %u times (%.2f %.2f %.2f), %s\n",
                d->right ? "right" : "left", d->ready ? "ready" : "not ready", d->reports, d->frames, d->dropped,
                d->zeros_learned, d->gyro_zero[0], d->gyro_zero[1], d->gyro_zero[2], d->sideways ? "sideways" : "upright");
    }
    for (int p = 0; p < JOYCON_SLOTS; p++)
        if (players[p].remote >= 0)
            fprintf(out, "joy-con player %d: %s remote%s\n", p + 1, devices[players[p].remote].right ? "right" : "left",
                    players[p].nunchuk >= 0 ? ", left Nunchuk" : "");
    LeaveCriticalSection(&lock);
}

/* ---- --test-joycon ---- */

static int check(const char* what, int ok) {
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    return ok ? 0 : 1;
}

int joycon_self_test(void) {
    int failures = 0;
    Device right, left;
    Frame f, nf;
    RemoteSample s;
    u8 cal[32], r[22];
    memset(&right, 0, sizeof right);
    memset(&left, 0, sizeof left);
    right.right = 1;
    /* Lying flat, buttons up: 1 g up on the Wii Remote's z. A right Joy-Con's z points into its buttons. */
    memset(&f, 0, sizeof f);
    f.accel[2] = -1.0f;
    make_sample(&right, &f, NULL, NULL, 0, &s);
    failures += check("right Joy-Con flat reads 1 g up", s.accel[2] == 616 && s.accel[0] == 512 && s.accel[1] == 512);
    memset(&f, 0, sizeof f);
    f.accel[2] = 1.0f;
    make_sample(&left, &f, NULL, NULL, 0, &s);
    failures += check("left Joy-Con flat reads 1 g up", s.accel[2] == 616);
    /* Tip up: gravity leans towards the tip (+y). The tip is the Joy-Con's top, its +x. */
    memset(&f, 0, sizeof f);
    f.accel[0] = 0.5f;
    f.accel[2] = -0.87f;
    make_sample(&right, &f, NULL, NULL, 0, &s);
    failures += check("tip up reads +y", s.accel[1] == 564);
    /* A rate about the Joy-Con's x (its long axis) is roll; still is 8192 everywhere, in slow mode. */
    memset(&f, 0, sizeof f);
    make_sample(&right, &f, NULL, NULL, 0, &s);
    failures += check("still is 8192, slow", s.gyro[0] == 8192 && s.gyro[1] == 8192 && s.gyro[2] == 8192 && (s.gyro_flags & 7) == 7);
    f.gyro[0] = 100.0f;
    make_sample(&right, &f, NULL, NULL, 0, &s);
    failures += check("long-axis turn is roll, 20 counts a degree", s.gyro[1] == 8192 - 2000 && s.gyro[0] == 8192 && s.gyro[2] == 8192);
    f.gyro[0] = 1000.0f;
    make_sample(&right, &f, NULL, NULL, 0, &s);
    failures += check("fast turns switch to fast mode", !(s.gyro_flags & 2) && s.gyro[1] == 8192 - 4400);
    /* The accelerometer and gyro agree: a pitch up (Wii +x rate) turns gravity towards +y. Integrate a small step. */
    {
        float a[3] = {0, 0, -1}, g[3] = {0}, wa0[3], wg0[3], wa1[3], wg1[3];
        wii_axes(1, 0, a, g, wa0, wg0);
        g[0] = 4.0f;
        g[1] = 10.0f;
        g[2] = -6.0f;
        /* gravity in the body frame turns opposite to the body: dv/dt = -w x v, in the Joy-Con's axes */
        float dv[3] = {-(g[1] * a[2] - g[2] * a[1]), -(g[2] * a[0] - g[0] * a[2]), -(g[0] * a[1] - g[1] * a[0])};
        float a2[3] = {a[0] + dv[0] * 0.001f, a[1] + dv[1] * 0.001f, a[2] + dv[2] * 0.001f};
        wii_axes(1, 0, a2, g, wa1, wg1);
        float wdv[3] = {wa1[0] - wa0[0], wa1[1] - wa0[1], wa1[2] - wa0[2]};
        float expect[3] = {-(wg1[1] * wa0[2] - wg1[2] * wa0[1]), -(wg1[2] * wa0[0] - wg1[0] * wa0[2]),
                           -(wg1[0] * wa0[1] - wg1[1] * wa0[0])};
        int agree = 1;
        for (int i = 0; i < 3; i++) agree &= fabsf(wdv[i] - expect[i] * 0.001f) < 1e-5f;
        failures += check("right Joy-Con: gyro and accelerometer turn the same way", agree);
        float al[3] = {0, 0, 1}, gl[3] = {3.0f, -7.0f, 5.0f}, la0[3], lg0[3], la1[3], lg1[3];
        wii_axes(0, 0, al, gl, la0, lg0);
        float dl[3] = {-(gl[1] * al[2] - gl[2] * al[1]), -(gl[2] * al[0] - gl[0] * al[2]), -(gl[0] * al[1] - gl[1] * al[0])};
        float al2[3] = {al[0] + dl[0] * 0.001f, al[1] + dl[1] * 0.001f, al[2] + dl[2] * 0.001f};
        wii_axes(0, 0, al2, gl, la1, lg1);
        agree = 1;
        for (int i = 0; i < 3; i++) {
            float e = -((i == 0) ? (lg0[1] * la0[2] - lg0[2] * la0[1]) : (i == 1) ? (lg0[2] * la0[0] - lg0[0] * la0[2])
                                                                                   : (lg0[0] * la0[1] - lg0[1] * la0[0]));
            agree &= fabsf((la1[i] - la0[i]) - e * 0.001f) < 1e-5f;
        }
        failures += check("left Joy-Con: gyro and accelerometer turn the same way", agree);
    }
    /* Buttons. */
    memset(&f, 0, sizeof f);
    f.buttons = J_A | J_ZR;
    make_sample(&right, &f, NULL, NULL, 0, &s);
    failures += check("right upright: A and ZR are A and B", s.buttons == (W_A | W_B));
    make_sample(&right, &f, NULL, NULL, 1, &s);
    failures += check("right sideways: A (bottom) is 2", (s.buttons & W_TWO) && !(s.buttons & W_A));
    memset(&f, 0, sizeof f);
    f.sy = 1.0f;
    make_sample(&right, &f, NULL, NULL, 0, &s);
    failures += check("stick up is d-pad up", s.buttons == W_UP);
    make_sample(&right, &f, NULL, NULL, 1, &s);
    failures += check("right sideways: stick towards the top is d-pad down (right on screen)", s.buttons == W_DOWN);
    /* The Nunchuk. */
    memset(&f, 0, sizeof f);
    memset(&nf, 0, sizeof nf);
    nf.sx = 1.0f;
    nf.accel[2] = 1.0f;
    nf.buttons = J_ZL;
    make_sample(&right, &f, &left, &nf, 0, &s);
    failures += check("left as Nunchuk: stick right, Z held, 1 g up",
                      s.stick[0] == 224 && s.stick[1] == 128 && (s.nunchuk_flags & 3) == 2 && s.naccel[2] == 716 &&
                          (s.nunchuk_flags & SAMPLE_HAS_NUNCHUK));
    /* Calibration and the report. */
    gyro_calibration(cal);
    failures += check("MotionPlus calibration zero field", cal[0] == 0x7C && cal[1] == 0xB0 && cal[28] == 42 && cal[12] == 200);
    memset(&f, 0, sizeof f);
    f.gyro[2] = 50.0f;
    make_sample(&right, &f, NULL, NULL, 0, &s);
    joycon_make_report(&s, r);
    u32 yaw = (u32)(r[16] | (r[19] >> 2) << 8);
    failures += check("report carries MotionPlus data", r[0] == 0x37 && (r[21] & 3) == 2 && yaw == s.gyro[0] && yaw == 8192 + 1000);
    {
        u8 m[4];
        rumble_bytes(0.0f, m);
        failures += check("rumble off is the neutral 00 01 40 40", m[0] == 0x00 && m[1] == 0x01 && m[2] == 0x40 && m[3] == 0x40);
    }
    printf("joy-con self-test: %d failures\n", failures);
    return failures;
}
