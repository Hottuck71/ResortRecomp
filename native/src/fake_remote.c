/* A Wii Remote with MotionPlus, at the Bluetooth HID report level. The game's own WPAD, KPAD, and
   MotionPlus code run unchanged above this, so a real remote can later supply the same reports.
   Protocol: wiibrew.org/wiki/Wiimote and wiibrew.org/wiki/Wiimote/Extension_Controllers.

   Input comes from a script: each line is "<field> <buttons> [ptr <x> <y>] [swing <n>] [nun <x> <y>]
   [nswing <n>] [plug <0|1>]", which holds until the next line. Buttons are names joined by +, or - for none; C
   and Z are the Nunchuk's. ptr x and y are -1 to 1 across the screen; nun is the Nunchuk's stick, -1 to 1, up
   positive; plug puts the Nunchuk in or takes it out.

   The stand-in has a Nunchuk to plug into its MotionPlus, as three sports need (Archery, Cycling, Power
   Cruising) and the game asks: N on the keyboard, plug in a script, and --nunchuk 1 starts with it in. The game
   asks for it to be taken out at the start, as it does on a Wii. A real remote's own Nunchuk needs nothing
   here: its reports pass through.

   With no real remote, a Joy-Con (joycon.c) can be player 1: the stand-in replays its samples as it replays a real
   remote's online, with the Joy-Con's calibration, and a left Joy-Con beside a right one is its Nunchuk. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "fake_remote.h"
#include "motion_pointer.h"
#include "real_remote.h"
#include "remote_driver.h"
#include "joycon.h"

#define WUD_STATE 0x80717F48u    /* byte: 2 ready for callbacks, 3 running */
#define WUD_RECV_CB 0x80717F2Cu
#define WUD_CONN_CB 0x80717F30u
#define WPAD_CHANNELS 0x80707CD0u
#define WUD_READY 2u
#define WUD_RUNNING 3u
#define SCRATCH_SEND 0x80002000u /* free low memory: the send buffer, report, and device info */
#define SCRATCH_REPORT 0x80002200u
#define SCRATCH_DEVICE 0x80002300u
#define DEVICE_SIZE 0x60u     /* one connection record per remote, from SCRATCH_DEVICE */
#define CONNECT_AFTER_TICKS 200u
/* The manager ticks each millisecond, and remotes report at 100 Hz. --report-every changes it, to test the
   real remote's faster rate without one. */
static u32 REPORT_EVERY_TICKS = 10u;
#define REPORT_SIZE 22u
#define QUEUE 256
#define MAX_LOG 500
#define MAX_SCRIPT 4096

#define BTN_LEFT 0x0100u
#define BTN_RIGHT 0x0200u
#define BTN_DOWN 0x0400u
#define BTN_UP 0x0800u
#define BTN_PLUS 0x1000u
#define BTN_TWO 0x0001u
#define BTN_ONE 0x0002u
#define BTN_B 0x0004u
#define BTN_A 0x0008u
#define BTN_MINUS 0x0010u
#define BTN_HOME 0x0080u
#define BTN_C 0x100000u /* the Nunchuk's buttons: kept apart from the remote's 16 bits */
#define BTN_Z 0x200000u

#define MP_ZERO 0x2000u /* a gyro at rest */
#define MP_AUTH_STATE 0xF7u
#define POINTER_X_SCALE 0.557f /* measured: where the game's cursor lands for a given dot position */
#define POINTER_Y_SCALE 0.80f
#define POINTER_Y_CENTER 0.30f  /* the sensor bar sits above the screen */
#define ACCEL_PER_G 26.0f
#define GYRO_COUNTS_PER_DEGREE 20.0f
#define TILT_EASE 0.12f
#define GYRO_ZERO_FIELD 31920u /* the game reads 5000 + field / 10, which gives MP_ZERO */
#define GYRO_SCALE_FIELD 10000u /* the game reads 4000 + field / 10 counts */
#define GYRO_ANGLE_FIELD 42u   /* at this many times 6 degrees per second: about 20 counts each */
#define NUNCHUK_ACCEL_ZERO 512.0f /* ten-bit values: the calibration below says 0x80 and 0xB3 in the top bits */
#define NUNCHUK_ACCEL_PER_G 204.0f
#define NUNCHUK_STICK_RANGE 96.0f /* centre 0x80, ends 0x20 and 0xE0 */
#define MP_PASSTHROUGH_NUNCHUK 5u

typedef struct {
    u32 field, buttons;
    float x, y;
    int pointer;
    float swing;
    float nx, ny, nswing; /* the Nunchuk's stick and a pump of the Nunchuk */
    int plug;             /* 1 plug the Nunchuk in, 0 take it out, -1 leave it */
} Step;

typedef struct {
    u8 data[REPORT_SIZE];
} Report;

/* One stand-in remote. Up to four, one per player: the game tells them apart by the handle in their connection
   record, and WPAD gives each its own channel. R is the one being worked on. */
typedef struct {
    u32 buttons;
    float x, y, pitch, roll, swing;
    float shown_pitch, shown_roll; /* eased, so tilt has a rate */
    int pointer;
    float nx, ny, nswing;
    u32 nbuttons;
} LiveInput;

/* Switching MotionPlus on or off takes a moment on a real one, while its port reads as unplugged. */
#define MP_SWITCH_TICKS 20u
#define MAX_REMOTES 4u

typedef struct {
    u32 handle;
    Report queue[QUEUE];
    u32 queue_head, queue_tail;
    Step script[MAX_SCRIPT];
    u32 script_count, script_at;
    Step current;
    LiveInput live;
    u32 reports_sent;
    int connected, data_mode, continuous, mp_active, ir_on;
    int nunchuk; /* its Nunchuk is plugged in */
    u32 mp_mode, ext_turn, nunchuk_reports, mp_sample_tick, nunchuk_plugs;
    int mp_switching, mp_switch_on;
    u32 mp_switch_mode, mp_switch_at;
    float mp_last_pitch, mp_last_roll;
    u32 connect_at;
    u8 leds;
    u8 eeprom[0x1800], ext_reg[256], mp_reg[256], ir_reg[256], speaker_reg[256];
    /* Online, driven by a player's real remote: its calibration, and its samples waiting for their moment. */
    RemoteCalibration cal;
    int raw;
    RemoteSample pending[32];
    u32 pending_tick[32], pending_head, pending_tail;
    RemoteSample latest; /* the last sample's dots and buttons */
    u16 last_gyro[3], last_naccel[3];
    u8 last_gyro_flags, last_stick[2], last_nunchuk_flags, rumble_out;
    u32 typed_buttons; /* the player's keyboard buttons, on top of the remote's */
} Remote;

static Remote remotes[MAX_REMOTES];
static Remote* R = remotes;
static u32 remote_count = 1;
static u32 ticks, log_count;
/* With a real remote, the mouse only stands in for the pointer for a moment after it moves. */
#define MOUSE_POINTER_MS 1500u
static u32 mouse_moved_at, ir_from_remote, ir_from_mouse, ir_none;
static float mouse_last_x, mouse_last_y, mouse_window_x, mouse_window_y;
static u32 ir_from_motion, calibrations;
static int enabled;
static volatile u32 plug_presses;   /* N presses, from the window's thread; the remote's thread acts on them */
static u32 plug_presses_done;
static int using_real;      /* the game is talking to a real remote, as player 1 */
static int real_available;  /* real_remote.c is scanning */
static u32 sim_from, sim_until; /* --sim-remote: pretend a remote exists between these fields */
static u32 switches;
static int using_joycon;     /* a Joy-Con is player 1, through the stand-in */
static int joycon_available; /* joycon.c is looking */
static u32 keyboard_buttons; /* the keyboard's, added to a Joy-Con's */
/* Online play: this PC's script and keyboard go into local_step and local_live, then out as a RemoteInput, and
   every remote is built only from the inputs received for the field (fake_remote_apply_input). */
static int net_mode;
static u32 net_me;
static Step local_step;
static LiveInput local_live;
static s32 pending_plug = -1;

static u32 guest32(u32 addr) { return (u32)load_be(plain_memory(addr & 0x3FFFFFFFu, 4), 4); }

static float clamp1(float v) { return v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v; }

static void note(const char* what, const u8* p, u32 n) {
    if (log_count++ >= MAX_LOG) return;
    fprintf(report, "Remote %s:", what);
    for (u32 i = 0; i < n; i++) fprintf(report, " %02X", p[i]);
    fprintf(report, "\n");
}

static u32 button_bits(const char* text) {
    static const struct { const char* name; u32 bit; } names[] = {
        {"A", BTN_A}, {"B", BTN_B}, {"1", BTN_ONE}, {"2", BTN_TWO}, {"PLUS", BTN_PLUS},
        {"MINUS", BTN_MINUS}, {"HOME", BTN_HOME}, {"UP", BTN_UP}, {"DOWN", BTN_DOWN},
        {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT}, {"C", BTN_C}, {"Z", BTN_Z},
    };
    char copy[64];
    u32 bits = 0;
    snprintf(copy, sizeof copy, "%s", text);
    for (char* word = strtok(copy, "+"); word; word = strtok(NULL, "+"))
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            if (!strcmp(word, names[i].name)) bits |= names[i].bit;
    return bits;
}

static void load_script(const char* path) {
    FILE* f = fopen(path, "r");
    char line[128], buttons[64];
    if (!f) {
        fprintf(stderr, "Can't read %s\n", path);
        exit(1);
    }
    while (fgets(line, sizeof line, f) && R->script_count < MAX_SCRIPT) {
        Step s = {0};
        float x, y;
        s.plug = -1;
        if (line[0] == '#' || sscanf(line, "%u %63s", &s.field, buttons) != 2) continue;
        s.buttons = button_bits(buttons);
        char* swing = strstr(line, "swing");
        if (swing) sscanf(swing, "swing %f", &s.swing);
        char* at = strstr(line, "ptr");
        if (at && sscanf(at, "ptr %f %f", &x, &y) == 2) {
            s.x = x;
            s.y = y;
            s.pointer = 1;
        }
        char* stick = strstr(line, "nun ");
        if (stick) sscanf(stick, "nun %f %f", &s.nx, &s.ny);
        char* pump = strstr(line, "nswing");
        if (pump) sscanf(pump, "nswing %f", &s.nswing);
        char* plug = strstr(line, "plug");
        if (plug) sscanf(plug, "plug %d", &s.plug);
        R->script[R->script_count++] = s;
    }
    fclose(f);
    fprintf(report, "Remote: %u script lines from %s\n", R->script_count, path);
}

/* Factory calibration: zero and 1 g for each axis, then a checksum over the first nine bytes. */
static void write_accel_calibration(u8* p) {
    static const u8 values[9] = {0x80, 0x80, 0x80, 0x00, 0x9A, 0x9A, 0x9A, 0x00, 0x40};
    u32 sum = 0x55;
    memcpy(p, values, 9);
    for (int i = 0; i < 9; i++) sum += values[i];
    p[9] = (u8)sum;
}

/* The extension registers at 0xA4 with MotionPlus off: the Nunchuk's, or nothing. Nunchuk calibration: the
   accelerometer's zero and 1 g (top eight bits of each axis, then a byte of low bits), the stick's maximum,
   minimum, and centre for each axis, then the sum of those plus 0x55 and plus 0xAA. It sits at 0x20 and again at
   0x30. The ID at 0xFA is 00 00 A4 20 00 00. */
static void nunchuk_calibration(u8* out) {
    static const u8 cal[14] = {0x80, 0x80, 0x80, 0x00, 0xB3, 0xB3, 0xB3, 0x00, 0xE0, 0x20, 0x80, 0xE0, 0x20, 0x80};
    u32 sum = 0;
    if (R->cal.present && (R->cal.nunchuk_cal[8] | R->cal.nunchuk_cal[10])) { /* the player's own Nunchuk */
        memcpy(out, R->cal.nunchuk_cal, 16);
        return;
    }
    for (int i = 0; i < 14; i++) sum += cal[i];
    memcpy(out, cal, 14);
    out[14] = (u8)(sum + 0x55);
    out[15] = (u8)(sum + 0xAA);
}

static void plain_extension_registers(void) {
    static const u8 id[6] = {0x00, 0x00, 0xA4, 0x20, 0x00, 0x00};
    memset(R->ext_reg, 0xFF, sizeof R->ext_reg);
    if (!R->nunchuk) return;
    nunchuk_calibration(R->ext_reg + 0x20);
    nunchuk_calibration(R->ext_reg + 0x30);
    memcpy(R->ext_reg + 0xFA, id, 6);
}

/* An active MotionPlus with something plugged into it keeps that extension's last two ID bytes at its F8 and F9
   and the extension's calibration at 0x40. The game reads them when the MotionPlus data says an extension is
   there, to tell a Nunchuk (00 00) from a Classic Controller (01 01) (0x8002B7A8, 0x80029D60, 0x80028DD0).
   Without them the game drew the Nunchuk as an unknown "?" and the Nunchuk sports still refused it. */
static void passthrough_registers(void) {
    if (R->nunchuk) {
        R->ext_reg[0xF8] = R->ext_reg[0xF9] = 0x00;
        nunchuk_calibration(R->ext_reg + 0x40);
    } else {
        R->ext_reg[0xF8] = R->ext_reg[0xF9] = 0xFF;
        memset(R->ext_reg + 0x40, 0xFF, 16);
    }
}

void fake_remote_nunchuk(int on) {
    for (u32 i = 0; i < MAX_REMOTES; i++) remotes[i].nunchuk = on;
}

void fake_remote_set_calibration(u32 player, const RemoteCalibration* cal) {
    if (player < MAX_REMOTES) remotes[player].cal = *cal;
}

void fake_remote_count(u32 count) { remote_count = count < 1 ? 1 : count > MAX_REMOTES ? MAX_REMOTES : count; }

void fake_remote_setup(const char* script_path) {
    static const u8 mp_id[6] = {0x00, 0x00, 0xA6, 0x20, 0x00, 0x05};
    enabled = 1;
    for (u32 i = 0; i < MAX_REMOTES; i++) {
        R = &remotes[i];
        R->handle = i;
        R->connect_at = CONNECT_AFTER_TICKS + i * 50u; /* one at a time, as players would switch them on */
        write_accel_calibration(R->eeprom + 0x16);
        write_accel_calibration(R->eeprom + 0x20);
        if (R->cal.present) { /* a real remote: its own calibration, and its Nunchuk as it was */
            memcpy(R->eeprom + 0x16, R->cal.accel, 10);
            memcpy(R->eeprom + 0x20, R->cal.accel, 10);
            R->nunchuk = R->cal.nunchuk;
            R->raw = 1;
            fprintf(report, "Remote %u: a real remote's samples, MotionPlus %d, Nunchuk %d\n", i + 1, R->cal.motion_plus,
                    R->cal.nunchuk);
        }
        plain_extension_registers();
        memcpy(R->mp_reg + 0xFA, mp_id, 6);
    }
    R = remotes;
    if (script_path) load_script(script_path); /* the first remote's, player 1 */
    if (R->nunchuk) fprintf(report, "Remote: the stand-ins start with their Nunchuks plugged in\n");
    if (remote_count > 1) fprintf(report, "Remote: %u stand-in remotes\n", remote_count);
}

void fake_remote_start(void) {
    store_be(plain_memory(WUD_STATE & 0x3FFFFFFFu, 1), WUD_READY, 1);
}

static Report* new_report(u32 id) {
    Report* r = &R->queue[R->queue_tail++ % QUEUE];
    memset(r->data, 0, REPORT_SIZE);
    r->data[0] = (u8)id;
    u32 buttons = R->current.buttons | R->live.buttons;
    /* A remote or Joy-Con replayed from samples: every report carries what it holds now, so a held button
       stays held through the status and acknowledgement reports between its samples (rumble sends many). */
    if (R->raw) buttons |= R->latest.buttons;
    r->data[1] = (u8)(buttons >> 8);
    r->data[2] = (u8)buttons;
    return r;
}

static void send_status(void) {
    Report* r = new_report(0x20);
    int extension = !R->mp_switching && (R->mp_active || R->nunchuk);
    r->data[3] = (u8)((R->leds << 4) | (extension ? 2u : 0u) | (R->ir_on ? 8u : 0u));
    r->data[6] = 0xC0; /* battery */
}

static void send_ack(u32 id, u32 error) {
    Report* r = new_report(0x22);
    r->data[3] = (u8)id;
    r->data[4] = (u8)error;
}

static u8* region(u32 address, u32* available) {
    u32 space = address >> 24, device = (address >> 16) & 0xFFu, offset = address & 0xFFFFu;
    u8* base = NULL;
    if (space == 0) {
        *available = offset < sizeof R->eeprom ? (u32)sizeof R->eeprom - offset : 0;
        return R->eeprom + offset;
    }
    if ((device == 0xA4 || device == 0xA6) && R->mp_switching) return NULL; /* nothing answers mid-switch */
    if (device == 0xA4) base = R->ext_reg;
    else if (device == 0xA6 && !R->mp_active) base = R->mp_reg; /* activation moves it to 0xA4 */
    else if (device == 0xB0) base = R->ir_reg;
    else if (device == 0xA2) base = R->speaker_reg;
    if (!base || offset > 0xFF) return NULL;
    *available = 256u - offset;
    return base + offset;
}

/* A Nunchuk set up with 0xAA at its 0xF0 and a key at 0x40 sends its registers and data encrypted, and the game
   sets the Nunchuk up that way. WPAD works the key into two eight-byte tables, at 0xB14 and 0xB1C of its channel
   block (0x8002C6D0), and decodes the byte at register address a as (byte ^ b1c[a & 7]) + b14[a & 7]
   (0x8002D2E0). The stand-in encodes with those same tables, the reverse, which gives what a real Nunchuk given
   that key sends. */
static int nunchuk_encrypted(void) { return R->nunchuk && !R->mp_active && !R->mp_switching && R->ext_reg[0xF0] == 0xAA; }

/* WPAD's channel block for the current remote: the one holding its handle at 0x907. */
static u8* channel_block(void) {
    for (u32 channel = 0; channel < 4; channel++) {
        u8* block = plain_memory(guest32(WPAD_CHANNELS + channel * 4u) & 0x3FFFFFFFu, 0xBA8);
        if (block && block[0x907] == R->handle) return block;
    }
    return NULL;
}

static void encrypt(u8* data, u32 size, u32 address) {
    const u8* block = channel_block();
    if (!block) return;
    for (u32 i = 0; i < size; i++) {
        u32 k = (address + i) & 7u;
        data[i] = (u8)((u8)(data[i] - block[0xB14 + k]) ^ block[0xB1C + k]);
    }
}

static void read_memory(const u8* p) {
    u32 address = (u32)load_be(p, 4), size = (u32)load_be(p + 4, 2), available = 0;
    const u8* source = region(address, &available);
    int missing_extension = (address >> 16 & 0xFFu) == 0xA4 && !R->mp_active && !R->nunchuk;
    if (!source || !available || missing_extension) {
        Report* r = new_report(0x21);
        r->data[3] = 7; /* error: nothing at that address */
        store_be(r->data + 4, address & 0xFFFFu, 2);
        return;
    }
    if (size > available) size = available;
    for (u32 done = 0; done < size;) {
        u32 step = size - done < 16 ? size - done : 16;
        Report* r = new_report(0x21);
        r->data[3] = (u8)((step - 1) << 4);
        store_be(r->data + 4, (address + done) & 0xFFFFu, 2);
        memcpy(r->data + 6, source + done, step);
        if ((address >> 16 & 0xFFu) == 0xA4 && nunchuk_encrypted()) encrypt(r->data + 6, step, (address + done) & 0xFFu);
        done += step;
    }
}

static u32 crc32(const u8* data, u32 size) {
    u32 crc = 0xFFFFFFFFu;
    for (u32 i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Gyro calibration, in the layout the game's WPAD code reads: fast then slow halves of 14 bytes,
   each holding three zero points, three scales, and a reference angle. Bytes 14-15 and 30-31 hold a
   CRC of the two halves. The values only need to be sensible, because this gyro never moves. */
static void write_gyro_calibration(u8* block) {
    u8 halves[28];
    memset(block, 0, 32);
    for (u32 half = 0; half < 2; half++) {
        u8* p = block + half * 16u;
        for (u32 axis = 0; axis < 3; axis++) {
            store_be(p + axis * 2u, GYRO_ZERO_FIELD, 2);
            store_be(p + 6 + axis * 2u, GYRO_SCALE_FIELD, 2);
        }
        p[12] = GYRO_ANGLE_FIELD;
    }
    memcpy(halves, block, 14);
    memcpy(halves + 14, block + 16, 14);
    u32 crc = crc32(halves, sizeof halves);
    block[14] = (u8)(crc >> 24);
    block[15] = (u8)(crc >> 16);
    block[30] = (u8)(crc >> 8);
    block[31] = (u8)crc;
}

/* Writing 0x55 to F0 of the extension registers switches MotionPlus off again. The mode written to switch it
   on says what it passes through: 4 nothing, 5 a Nunchuk, 7 a Classic Controller. */
static void motion_plus_active(int on, u32 mode) {
    const u8 active_id[6] = {0x00, 0x00, 0xA4, 0x20, (u8)mode, 0x05};
    R->mp_active = on;
    R->mp_mode = on ? mode : 0;
    R->ext_turn = 0;
    if (on) {
        memcpy(R->ext_reg + 0xFA, active_id, 6);
        if (R->raw && R->cal.motion_plus) memcpy(R->ext_reg + 0x20, R->cal.gyro, 32); /* the player's own gyro */
        else write_gyro_calibration(R->ext_reg + 0x20);
        R->ext_reg[MP_AUTH_STATE] = 0; /* above 0xD means a device that must finish the check first */
        passthrough_registers();
    } else {
        plain_extension_registers(); /* the Nunchuk, if any, answers at 0xA4 again */
    }
    fprintf(report, "Remote: MotionPlus %s, mode %u, at field %u\n", on ? "on" : "off", mode, host_field());
    send_status();
}

/* A real MotionPlus switching on or off: its port reads as unplugged for a moment, so the remote sends a status
   report with no extension, and another when it's back (motion_plus_active, from the tick). The game forgets the
   extension it knew on the first. That matters with a Nunchuk plugged in: switching straight over kept the
   Nunchuk's encryption in the game's mind, it decoded the MotionPlus's replies with it, and never finished. */
static void motion_plus_switch(int on, u32 mode) {
    R->mp_switching = 1;
    R->mp_switch_on = on;
    R->mp_switch_mode = mode;
    R->mp_switch_at = ticks + MP_SWITCH_TICKS;
    send_status();
}

static void write_memory(const u8* p) {
    u32 address = (u32)load_be(p, 4), size = p[4], available = 0;
    u8* target = region(address, &available);
    if (!target) {
        send_ack(0x16, 7);
        return;
    }
    if (size > 16) size = 16;
    if (size > available) size = available;
    memcpy(target, p + 5, size);
    send_ack(0x16, 0);
    if ((address & 0xFFFFFFu) == 0xA600FEu && p[5] != 0) motion_plus_switch(1, p[5]);
    if ((address & 0xFFFFFFu) == 0xA400F0u && p[5] == 0x55 && R->mp_active) motion_plus_switch(0, 0);
}

static void output_report(const u8* r, u32 size) {
    u32 id = r[0], flags = size > 1 ? r[1] : 0;
    note("got", r, size < 24 ? size : 24);
    if (using_real && !sim_until) {
        if (id == 0x12) R->data_mode = r[2];
        if (id == 0x13 || id == 0x1A) R->ir_on = (flags >> 2) & 1;
        /* Speaker data needs a remote audio stream, which the mixer doesn't make yet. */
        if (id != 0x18) real_remote_write(r, size);
        return;
    }
    switch (id) {
    case 0x11:
        R->leds = (u8)(flags >> 4);
        break;
    case 0x12:
        R->continuous = (flags >> 2) & 1;
        R->data_mode = r[2];
        break;
    case 0x13:
    case 0x1A:
        R->ir_on = (flags >> 2) & 1;
        break;
    case 0x15:
        send_status();
        return;
    case 0x16:
        write_memory(r + 1);
        return;
    case 0x17:
        read_memory(r + 1);
        return;
    default:
        break;
    }
    if (flags & 2) send_ack(id, 0);
    if (using_joycon && R == remotes && size > 1 && (id == 0x11 || (flags & 1) != R->rumble_out)) {
        R->rumble_out = (u8)(flags & 1);
        joycon_output_at(0, R->leds, R->rumble_out); /* its player lights and rumble */
    }
    if (net_mode && R->handle == net_me && remote_driver_ready() && size > 1 &&
        (id == 0x11 || (flags & 1) != R->rumble_out)) {
        R->rumble_out = (u8)(flags & 1);
        remote_driver_output(R->leds, R->rumble_out);
    }
}

void fake_remote_send(u32 handle, u32 buffer) {
    const u8* b = plain_memory(buffer & 0x3FFFFFFFu, 0x40);
    R = &remotes[handle < remote_count ? handle : 0u];
    if (b) output_report(b + 0x12, (u32)load_be(b + 2, 2));
}

u32 fake_remote_buffer(void) { return SCRATCH_SEND; }

void fake_remote_live(u32 buttons, float x, float y, int pointer, float pitch, float roll, float swing) {
    static u32 held;
    LiveInput* live = net_mode ? &local_live : &remotes[0].live; /* this PC's player */
    u32 pressed = buttons & ~held & 0x70000u; /* the pointer keys act once per press */
    held = buttons;
    if (pressed & 0x10000u) motion_pointer_recentre();
    if (pressed & 0x20000u) motion_pointer_flip_x();
    if (pressed & 0x40000u) motion_pointer_flip_y();
    buttons &= 0xFFFFu;
    if (!net_mode) keyboard_buttons = buttons | (keyboard_buttons & (BTN_C | BTN_Z));
    if (pointer && (x != mouse_last_x || y != mouse_last_y)) {
        mouse_window_x = x;
        mouse_window_y = y;
        mouse_moved_at = ticks ? ticks : 1u;
        mouse_last_x = x;
        mouse_last_y = y;
    }
    live->buttons = buttons;
    live->pointer = pointer;
    live->x = x * POINTER_X_SCALE;
    live->y = POINTER_Y_CENTER + y * POINTER_Y_SCALE;
    live->pitch = pitch;
    live->roll = roll;
    live->swing = swing;
}

void fake_remote_live_nunchuk(float x, float y, int c, int z, float swing, int plug_key) {
    static int plug_held;
    LiveInput* live = net_mode ? &local_live : &remotes[0].live;
    if (plug_key && !plug_held) plug_presses++;
    plug_held = plug_key;
    live->nx = x;
    live->ny = y;
    live->nbuttons = (c ? BTN_C : 0u) | (z ? BTN_Z : 0u);
    live->nswing = swing;
    if (!net_mode) keyboard_buttons = (keyboard_buttons & 0xFFFFu) | live->nbuttons;
}

/* Gravity in the remote's frame, with a burst added during a swing. */
static void put_accel(u8* r, u8* out) {
    (void)r;
    float gx = sinf(R->live.shown_roll), gy = sinf(R->live.shown_pitch);
    float gz = cosf(R->live.shown_roll) * cosf(R->live.shown_pitch);
    float burst = R->live.swing * 3.0f * sinf((float)ticks * 0.05f);
    float axis[3] = {gx, gy + burst, gz};
    for (int i = 0; i < 3; i++) {
        float v = 128.0f + axis[i] * ACCEL_PER_G;
        out[i] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

/* Two dots of a sensor bar, as a remote aimed at (x, y) would see them. */
static void dots_at(int d[2][2], float px, float py) {
    int cx = 512 - (int)(px * 400.0f), cy = 384 + (int)(py * 280.0f);
    d[0][0] = cx - 100;
    d[0][1] = cy;
    d[1][0] = cx + 100;
    d[1][1] = cy;
}

static void dots(int d[2][2]) {
    dots_at(d, R->live.pointer ? R->live.x : R->current.x, R->live.pointer ? R->live.y : R->current.y);
}

/* Sensor bar dots in a real remote's report, for a pointer at window position (x, y). */
static void put_ir_at(u8* r, float x, float y) {
    int d[2][2];
    u8* out;
    dots_at(d, x * POINTER_X_SCALE, POINTER_Y_CENTER + y * POINTER_Y_SCALE);
    if (r[0] == 0x33) {
        out = r + 6;
        for (int i = 0; i < 2; i++) {
            out[i * 3] = (u8)d[i][0];
            out[i * 3 + 1] = (u8)d[i][1];
            out[i * 3 + 2] = (u8)(((d[i][1] >> 8) << 6) | ((d[i][0] >> 8) << 4) | 2);
        }
        return;
    }
    out = r[0] == 0x36 ? r + 3 : r + 6;
    out[0] = (u8)d[0][0];
    out[1] = (u8)d[0][1];
    out[2] = (u8)(((d[0][1] >> 8) << 6) | ((d[0][0] >> 8) << 4) | ((d[1][1] >> 8) << 2) | (d[1][0] >> 8));
    out[3] = (u8)d[1][0];
    out[4] = (u8)d[1][1];
}

static void put_ir10(u8* out) {
    int d[2][2];
    memset(out, 0xFF, 10);
    if (!R->ir_on || !(R->current.pointer || R->live.pointer)) return;
    dots(d);
    out[0] = (u8)d[0][0];
    out[1] = (u8)d[0][1];
    out[2] = (u8)(((d[0][1] >> 8) << 6) | ((d[0][0] >> 8) << 4) | ((d[1][1] >> 8) << 2) | (d[1][0] >> 8));
    out[3] = (u8)d[1][0];
    out[4] = (u8)d[1][1];
}

static void put_ir12(u8* out) {
    int d[2][2];
    memset(out, 0xFF, 12);
    if (!R->ir_on || !(R->current.pointer || R->live.pointer)) return;
    dots(d);
    for (int i = 0; i < 2; i++) {
        out[i * 3] = (u8)d[i][0];
        out[i * 3 + 1] = (u8)d[i][1];
        out[i * 3 + 2] = (u8)(((d[i][1] >> 8) << 6) | ((d[i][0] >> 8) << 4) | 2);
    }
}

static u32 gyro_counts(float degrees_per_second) {
    float v = (float)MP_ZERO + degrees_per_second * GYRO_COUNTS_PER_DEGREE;
    return v < 0 ? 0 : v > 16383 ? 16383 : (u32)v;
}

/* Gyro rates follow the eased tilt, plus a burst during a swing. Always in slow mode. Rates are over the time
   since the last MotionPlus sample, which in pass-through mode is every other report. */
static void put_motion_plus(u8* out) {
    if (!R->mp_active) {
        memset(out, 0, 6);
        return;
    }
    float seconds = (float)(ticks - R->mp_sample_tick) / 1000.0f;
    if (!R->mp_sample_tick || seconds <= 0 || seconds > 0.1f) seconds = REPORT_EVERY_TICKS / 1000.0f;
    R->mp_sample_tick = ticks;
    u32 yaw = gyro_counts(R->live.swing * 300.0f);
    u32 roll = gyro_counts((R->live.shown_roll - R->mp_last_roll) / seconds * 57.3f);
    u32 pitch = gyro_counts((R->live.shown_pitch - R->mp_last_pitch) / seconds * 57.3f + R->live.swing * 300.0f);
    R->mp_last_roll = R->live.shown_roll;
    R->mp_last_pitch = R->live.shown_pitch;
    out[0] = (u8)yaw;
    out[1] = (u8)roll;
    out[2] = (u8)pitch;
    out[3] = (u8)(((yaw >> 8) << 2) | 3);
    out[4] = (u8)(((roll >> 8) << 2) | 2);
    out[5] = (u8)(((pitch >> 8) << 2) | 2);
    if (R->nunchuk) out[4] |= 1; /* an extension is plugged into the MotionPlus */
}


/* The Nunchuk's six bytes. On its own: stick x and y, then ten-bit x, y, z acceleration (top eight bits in bytes
   2 to 4, low two bits of each in byte 5) and C and Z in byte 5's bits 1 and 0, 0 when pressed. Passed through a
   MotionPlus: z loses its lowest bit, and so do the low bits of x and y, to make room for byte 4 bit 0
   (extension present) and byte 5 bits 0 and 1, which are 0 for Nunchuk data and 2 for MotionPlus data; C and Z
   move to bits 3 and 2. Its tilt follows the remote's, and a pump of the Nunchuk is out of step with a swing. */
static void put_nunchuk(u8* out, int passthrough) {
    u32 buttons = R->current.buttons | R->live.nbuttons;
    float sx = clamp1(R->current.nx + R->live.nx), sy = clamp1(R->current.ny + R->live.ny);
    float pump = (R->current.nswing > R->live.nswing ? R->current.nswing : R->live.nswing) * 3.0f * -sinf((float)ticks * 0.05f);
    float axis[3] = {sinf(R->live.shown_roll), sinf(R->live.shown_pitch) + pump, cosf(R->live.shown_roll) * cosf(R->live.shown_pitch)};
    u32 a[3];
    for (int i = 0; i < 3; i++) {
        float v = NUNCHUK_ACCEL_ZERO + axis[i] * NUNCHUK_ACCEL_PER_G;
        a[i] = v < 0 ? 0u : v > 1023 ? 1023u : (u32)v;
    }
    u32 c = (buttons & BTN_C) ? 0u : 1u, z = (buttons & BTN_Z) ? 0u : 1u;
    out[0] = (u8)(128.0f + sx * NUNCHUK_STICK_RANGE);
    out[1] = (u8)(128.0f + sy * NUNCHUK_STICK_RANGE);
    out[2] = (u8)(a[0] >> 2);
    out[3] = (u8)(a[1] >> 2);
    if (passthrough) {
        out[4] = (u8)(((a[2] >> 3) << 1) | 1u);
        out[5] = (u8)((((a[2] >> 1) & 3u) << 6) | (((a[1] >> 1) & 1u) << 5) | (((a[0] >> 1) & 1u) << 4) | (c << 3) |
                      (z << 2));
    } else {
        out[4] = (u8)(a[2] >> 2);
        out[5] = (u8)(((a[2] & 3u) << 6) | ((a[1] & 3u) << 4) | ((a[0] & 3u) << 2) | (c << 1) | z);
    }
    R->nunchuk_reports++;
}

/* The extension bytes: MotionPlus data; in Nunchuk pass-through mode MotionPlus and Nunchuk data by turns, as a
   real MotionPlus sends them; or the Nunchuk on its own while MotionPlus is off. */
static void put_extension(u8* out) {
    if (R->mp_switching) memset(out, 0, 6);
    else if (R->mp_active && R->nunchuk && R->mp_mode == MP_PASSTHROUGH_NUNCHUK && (R->ext_turn++ & 1u)) put_nunchuk(out, 1);
    else if (R->mp_active || !R->nunchuk) put_motion_plus(out);
    else {
        put_nunchuk(out, 0);
        if (nunchuk_encrypted()) encrypt(out, 6, 0);
    }
}


/* ---- A stand-in replaying a real remote's samples (online) ---- */

static void put_accel_raw(u8* r, u8* out, const RemoteSample* s) {
    for (int i = 0; i < 3; i++) out[i] = (u8)(s->accel[i] >> 2);
    r[1] |= (u8)((s->accel[0] & 3u) << 5);
    r[2] |= (u8)(((s->accel[1] >> 1) & 1u) << 5 | ((s->accel[2] >> 1) & 1u) << 6);
}

static void put_dots10(u8* out, u16 dots[2][2]) {
    memset(out, 0xFF, 10);
    if (!R->ir_on || dots[0][0] == NO_DOT) return;
    out[0] = (u8)dots[0][0];
    out[1] = (u8)dots[0][1];
    out[2] = (u8)(((dots[0][1] >> 8) << 6) | ((dots[0][0] >> 8) << 4) | ((dots[1][1] >> 8 & 3) << 2) | (dots[1][0] >> 8 & 3));
    out[3] = (u8)dots[1][0];
    out[4] = (u8)dots[1][1];
}

static void put_dots12(u8* out, u16 dots[2][2]) {
    memset(out, 0xFF, 12);
    if (!R->ir_on) return;
    for (int i = 0; i < 2; i++) {
        if (dots[i][0] == NO_DOT) continue;
        out[i * 3] = (u8)dots[i][0];
        out[i * 3 + 1] = (u8)dots[i][1];
        out[i * 3 + 2] = (u8)(((dots[i][1] >> 8) << 6) | ((dots[i][0] >> 8) << 4) | 2);
    }
}

static void put_motion_plus_raw(u8* out) {
    const u16* g = R->last_gyro;
    u8 f = R->last_gyro_flags;
    if (!(f & SAMPLE_HAS_MOTION_PLUS)) { /* nothing yet: a gyro at rest, by the stand-in's own zero */
        static const u16 rest[3] = {MP_ZERO, MP_ZERO, MP_ZERO};
        g = rest;
        f = 7;
    }
    out[0] = (u8)g[0];
    out[1] = (u8)g[1];
    out[2] = (u8)g[2];
    out[3] = (u8)(((g[0] >> 8) << 2) | ((f & 1) << 1) | ((f >> 2) & 1));
    out[4] = (u8)(((g[1] >> 8) << 2) | (((f >> 1) & 1) << 1) | (R->nunchuk ? 1 : 0));
    out[5] = (u8)(((g[2] >> 8) << 2) | 2);
}

static void put_nunchuk_raw(u8* out, int passthrough) {
    const u16* a = R->last_naccel;
    u32 c = ((R->last_nunchuk_flags & 1) || (R->typed_buttons & BTN_C)) ? 0u : 1u;
    u32 z = ((R->last_nunchuk_flags & 2) || (R->typed_buttons & BTN_Z)) ? 0u : 1u;
    out[0] = R->last_stick[0];
    out[1] = R->last_stick[1];
    out[2] = (u8)(a[0] >> 2);
    out[3] = (u8)(a[1] >> 2);
    if (passthrough) {
        out[4] = (u8)(((a[2] >> 3) << 1) | 1u);
        out[5] = (u8)((((a[2] >> 1) & 3u) << 6) | (((a[1] >> 1) & 1u) << 5) | (((a[0] >> 1) & 1u) << 4) | (c << 3) | (z << 2));
    } else {
        out[4] = (u8)(a[2] >> 2);
        out[5] = (u8)(((a[2] & 3u) << 6) | ((a[1] & 3u) << 4) | ((a[0] & 3u) << 2) | (c << 1) | z);
    }
    R->nunchuk_reports++;
}

/* As put_extension, from the player's latest MotionPlus and Nunchuk values. The real remote's own MotionPlus
   takes turns the same way, so each value goes out about once. */
static void put_extension_raw(u8* out) {
    if (R->mp_switching) memset(out, 0, 6);
    else if (R->mp_active && R->nunchuk && R->mp_mode == MP_PASSTHROUGH_NUNCHUK && (R->ext_turn++ & 1u)) put_nunchuk_raw(out, 1);
    else if (R->mp_active) put_motion_plus_raw(out);
    else if (R->nunchuk) {
        put_nunchuk_raw(out, 0);
        if (nunchuk_encrypted()) encrypt(out, 6, 0);
    } else memset(out, 0, 6);
}

static void send_data_raw(const RemoteSample* s) {
    if (s->gyro_flags & SAMPLE_HAS_MOTION_PLUS) {
        memcpy(R->last_gyro, s->gyro, sizeof R->last_gyro);
        R->last_gyro_flags = s->gyro_flags;
    }
    if (s->nunchuk_flags & SAMPLE_HAS_NUNCHUK) {
        memcpy(R->last_stick, s->stick, 2);
        memcpy(R->last_naccel, s->naccel, sizeof R->last_naccel);
        R->last_nunchuk_flags = s->nunchuk_flags;
    }
    R->latest = *s;
    R->live.buttons = (s->buttons | R->typed_buttons) & 0xFFFFu;
    Report* r = new_report((u32)R->data_mode);
    u8* d = r->data;
    u16 dots[2][2];
    memcpy(dots, s->dots, sizeof dots);
    R->reports_sent++;
    switch (R->data_mode) {
    case 0x31: put_accel_raw(d, d + 3, s); break;
    case 0x33: put_accel_raw(d, d + 3, s); put_dots12(d + 6, dots); break;
    case 0x32:
    case 0x34: put_extension_raw(d + 3); break;
    case 0x35: put_accel_raw(d, d + 3, s); put_extension_raw(d + 6); break;
    case 0x36: put_dots10(d + 3, dots); put_extension_raw(d + 13); break;
    case 0x37: put_accel_raw(d, d + 3, s); put_dots10(d + 6, dots); put_extension_raw(d + 16); break;
    default: break;
    }
}

void fake_remote_dots_for(float x, float y, u16 dots[2][2]) {
    int d[2][2];
    dots_at(d, x * POINTER_X_SCALE, POINTER_Y_CENTER + y * POINTER_Y_SCALE);
    for (int i = 0; i < 2; i++) {
        int seen = d[i][0] >= 0 && d[i][0] < 1023 && d[i][1] >= 0 && d[i][1] < 1023;
        dots[i][0] = seen ? (u16)d[i][0] : NO_DOT;
        dots[i][1] = seen ? (u16)d[i][1] : NO_DOT;
    }
}

int fake_remote_mouse_pointer(float* x, float* y) {
    if (!mouse_moved_at || ticks - mouse_moved_at >= MOUSE_POINTER_MS) return 0;
    *x = mouse_window_x;
    *y = mouse_window_y;
    return 1;
}

static void send_data(void) {
    R->live.shown_pitch += (R->live.pitch - R->live.shown_pitch) * TILT_EASE;
    R->live.shown_roll += (R->live.roll - R->live.shown_roll) * TILT_EASE;
    Report* r = new_report((u32)R->data_mode);
    u8* d = r->data;
    R->reports_sent++;
    switch (R->data_mode) {
    case 0x31: put_accel(d, d + 3); break;
    case 0x33: put_accel(d, d + 3); put_ir12(d + 6); break;
    case 0x32: put_extension(d + 3); break;
    case 0x34: put_extension(d + 3); break;
    case 0x35: put_accel(d, d + 3); put_extension(d + 6); break;
    case 0x36: put_ir10(d + 3); put_extension(d + 13); break;
    case 0x37: put_accel(d, d + 3); put_ir10(d + 6); put_extension(d + 16); break;
    default: break;
    }
}

/* WPAD registers its callbacks while WUD says ready, and only then does the manager run fully. */
void fake_remote_field(void) {
    u8* state = plain_memory(WUD_STATE & 0x3FFFFFFFu, 1);
    if (enabled && *state == WUD_READY && guest32(WUD_CONN_CB)) *state = WUD_RUNNING;
}

/* Plugs the stand-in's Nunchuk in or takes it out, as a player does when the game asks. With MotionPlus on,
   the extension bit in its data tells the game. With it off, the remote sends a status report. */
static void plug_nunchuk(int on) {
    if (on == R->nunchuk) return;
    R->nunchuk = on;
    R->nunchuk_plugs++;
    fprintf(report, "Remote: Nunchuk %s at field %u\n", on ? "plugged in" : "taken out", host_field());
    if (!R->mp_active) {
        plain_extension_registers();
        if (R->connected && !(using_real && R == remotes)) send_status();
    } else {
        passthrough_registers(); /* the MotionPlus data's extension bit tells the game */
    }
}

static void update_script(void) {
    if (net_mode) { /* the script is this PC's player's, and goes out with the next input */
        if (R != remotes) return;
        while (R->script_at < R->script_count && R->script[R->script_at].field <= host_field()) {
            local_step = R->script[R->script_at++];
            if (local_step.plug >= 0) pending_plug = local_step.plug;
        }
        if (plug_presses != plug_presses_done) {
            plug_presses_done = plug_presses;
            pending_plug = !remotes[net_me].nunchuk;
        }
        return;
    }
    while (R->script_at < R->script_count && R->script[R->script_at].field <= host_field()) {
        R->current = R->script[R->script_at++];
        R->live.swing = R->current.swing;
        if (R->current.plug >= 0) plug_nunchuk(R->current.plug);
    }
    if (R == remotes && plug_presses != plug_presses_done) {
        plug_presses_done = plug_presses;
        plug_nunchuk(!R->nunchuk);
    }
}

void fake_remote_net_mode(u32 players, u32 me) {
    net_mode = 1;
    net_me = me;
    fake_remote_count(players);
}

void fake_remote_local_input(RemoteInput* out) {
    memset(out, 0, sizeof *out);
    out->buttons = (local_step.buttons | local_live.buttons | local_live.nbuttons) & (0xFFFFu | BTN_C | BTN_Z);
    if (local_live.pointer) {
        out->x = local_live.x;
        out->y = local_live.y;
        out->pointer = 1;
    } else if (local_step.pointer) {
        out->x = local_step.x;
        out->y = local_step.y;
        out->pointer = 1;
    }
    out->pitch = local_live.pitch;
    out->roll = local_live.roll;
    out->swing = local_step.swing > local_live.swing ? local_step.swing : local_live.swing;
    out->nx = clamp1(local_step.nx + local_live.nx);
    out->ny = clamp1(local_step.ny + local_live.ny);
    out->nswing = local_step.nswing > local_live.nswing ? local_step.nswing : local_live.nswing;
    out->plug = pending_plug;
    pending_plug = -1;
    s32 plug = -1;
    out->sample_count = remote_driver_take(out->samples, MAX_SAMPLES, &plug);
    if (plug >= 0) out->plug = plug;
    if (local_step.pointer) /* a test script aims for the remote */
        for (u32 i = 0; i < out->sample_count; i++) {
            int d[2][2];
            dots_at(d, local_step.x, local_step.y);
            for (int k = 0; k < 2; k++) {
                out->samples[i].dots[k][0] = (u16)d[k][0];
                out->samples[i].dots[k][1] = (u16)d[k][1];
            }
        }
}

void fake_remote_apply_input(u32 player, const RemoteInput* in) {
    if (player >= remote_count) return;
    R = &remotes[player];
    memset(&R->current, 0, sizeof R->current);
    R->current.plug = -1;
    R->live.buttons = in->buttons & 0xFFFFu;
    R->live.nbuttons = in->buttons & (BTN_C | BTN_Z);
    R->live.pointer = in->pointer != 0;
    R->live.x = in->x;
    R->live.y = in->y;
    R->live.pitch = in->pitch;
    R->live.roll = in->roll;
    R->live.swing = in->swing;
    R->live.nx = in->nx;
    R->live.ny = in->ny;
    R->live.nswing = in->nswing;
    R->typed_buttons = in->buttons;
    if (in->plug >= 0) plug_nunchuk(in->plug);
    for (u32 i = 0; i < in->sample_count && i < MAX_SAMPLES; i++) {
        if (R->pending_tail - R->pending_head == 32u) R->pending_head++; /* too far behind: drop the oldest */
        R->pending[R->pending_tail % 32u] = in->samples[i];
        R->pending_tick[R->pending_tail % 32u] = ticks + in->samples[i].offset;
        R->pending_tail++;
    }
    R = remotes;
}

void fake_remote_use_real(void) {
    real_available = 1;
    real_remote_start();
    joycon_available = 1;
    joycon_start(1);
}

void fake_remote_simulate(u32 from_field, u32 until_field) {
    sim_from = from_field;
    sim_until = until_field;
}

int fake_remote_is_real(void) { return using_real && !sim_until; }

static int real_wanted(void) {
    if (sim_until) return host_field() >= sim_from && host_field() < sim_until;
    return real_available && real_remote_connected();
}

/* Back to a clean slate, as a remote that has just turned on would be. */
static void forget_remote(void) {
    R->connected = 0;
    R->data_mode = 0;
    R->continuous = 0;
    R->mp_active = 0;
    R->mp_mode = 0;
    R->mp_switching = 0;
    R->ir_on = 0;
    R->queue_head = R->queue_tail = 0;
    plain_extension_registers();
    R->connect_at = ticks + CONNECT_AFTER_TICKS;
    switches++;
}

/* Player 1's stand-in replays a Joy-Con's samples, read with its calibration, or goes back to its own. */
static void use_joycon(int on) {
    R = remotes;
    using_joycon = on;
    memset(&R->cal, 0, sizeof R->cal);
    R->raw = 0;
    write_accel_calibration(R->eeprom + 0x16);
    write_accel_calibration(R->eeprom + 0x20);
    if (on) {
        joycon_calibration_at(0, &R->cal);
        memcpy(R->eeprom + 0x16, R->cal.accel, 10);
        memcpy(R->eeprom + 0x20, R->cal.accel, 10);
        R->nunchuk = R->cal.nunchuk;
        R->raw = 1;
    }
    R->pending_head = R->pending_tail = 0;
    memset(&R->latest, 0, sizeof R->latest);
    R->last_gyro_flags = R->last_nunchuk_flags = 0;
    R->rumble_out = 0;
    plain_extension_registers();
    if (on) fprintf(report, "Remote: a Joy-Con is player 1, Nunchuk %s\n", R->nunchuk ? "in" : "out");
}

/* The Joy-Con's newest samples, to go out on the coming ticks, and the keyboard's buttons on top. */
static void take_joycon_samples(void) {
    R = remotes;
    R->typed_buttons = keyboard_buttons;
    if (!R->connected || R->pending_head != R->pending_tail) return;
    RemoteSample got[MAX_SAMPLES];
    s32 plug = -1;
    u32 n = remote_driver_take_joycon(got, MAX_SAMPLES, &plug);
    if (plug >= 0) plug_nunchuk(plug);
    for (u32 i = 0; i < n; i++) {
        R->pending[R->pending_tail % 32u] = got[i];
        R->pending_tick[R->pending_tail % 32u] = ticks + got[i].offset;
        R->pending_tail++;
    }
}

/* With no sensor bar the remote sees no dots, so the mouse can still aim. */
static void add_mouse_pointer(u8* r) {
    u8* ir = NULL;
    u32 size = 0;
    if (r[0] == 0x33) ir = r + 6, size = 12;
    else if (r[0] == 0x36) ir = r + 3, size = 10;
    else if (r[0] == 0x37) ir = r + 6, size = 10;
    if (!ir || !R->live.pointer || ir[0] != 0xFF || ir[1] != 0xFF) return;
    if (size == 12) put_ir12(ir);
    else put_ir10(ir);
}

/* A real remote with no sensor bar in view reports no IR points. The mouse fills in then, but only for a
   moment after it moves: a pointer held still in one place while the real remote turns would pull the
   game's MotionPlus heading toward that spot, as it uses the pointer to correct drift. */
static void add_mouse_pointer_real(u8* r, double when) {
    motion_pointer_feed(r, when);
    u8* ir = NULL;
    if (r[0] == 0x33 || r[0] == 0x37) ir = r + 6;
    else if (r[0] == 0x36) ir = r + 3;
    if (!ir) return;
    if (ir[0] != 0xFF || ir[1] != 0xFF) {
        ir_from_remote++;
        return;
    }
    float x, y;
    /* A press of A while the game would see no pointer: the game's "point at the screen and press A" step
       only takes A with the pointer on the screen, so with no sensor bar it worked only when the MotionPlus
       pointer happened to be there. Pressing A now centres it, as pointing at the screen would. In menus an
       A press off the screen does nothing anyway. */
    static int a_held;
    int a_pressed = (r[2] & 0x08) && !a_held;
    a_held = (r[2] & 0x08) != 0;
    if (mouse_moved_at && ticks - mouse_moved_at < MOUSE_POINTER_MS) {
        ir_from_mouse++;
        add_mouse_pointer(r);
        motion_pointer_place(mouse_window_x, mouse_window_y); /* the MotionPlus pointer carries on from here */
    } else if (R->ir_on && motion_pointer_get(&x, &y)) {
        ir_from_motion++;
        put_ir_at(r, x, y);
    } else if (R->ir_on && a_pressed) {
        motion_pointer_calibrate();
        calibrations++;
        ir_from_motion++;
        put_ir_at(r, 0, 0);
    } else {
        ir_none++;
    }
}

/* Runs at the end of WPAD's 1 ms manager, in its alarm context. A pending event is delivered by
   entering the guest callback directly: it then returns to the manager as the original would. */
static int enter_connect(CPUState* ctx, int connected) {
    ctx->gpr[3] = SCRATCH_DEVICE + R->handle * DEVICE_SIZE;
    ctx->gpr[4] = (u32)connected;
    ctx->pc = guest32(WUD_CONN_CB);
    return 1;
}

static int enter_report(CPUState* ctx) {
    ctx->gpr[3] = R->handle;
    ctx->gpr[4] = SCRATCH_REPORT;
    ctx->gpr[5] = REPORT_SIZE;
    ctx->pc = guest32(WUD_RECV_CB);
    return 1;
}

/* Runs at the end of WPAD's 1 ms manager, in its alarm context. A pending event is delivered by
   entering the guest callback directly: it then returns to the manager as the original would. One event a
   tick, so with several remotes their reports take turns. */
int fake_remote_tick(CPUState* ctx) {
    static u32 turn;
    if (!enabled) return 0;
    ticks++;
    for (u32 i = 0; i < remote_count; i++) {
        R = &remotes[i];
        update_script();
        if (R->mp_switching && (s32)(ticks - R->mp_switch_at) >= 0) {
            R->mp_switching = 0;
            motion_plus_active(R->mp_switch_on, R->mp_switch_mode);
        }
    }
    R = remotes;
    int wanted = real_wanted();
    int joycon = !wanted && joycon_available && !sim_until && !net_mode && joycon_ready_at(0);
    if (R->connected && (wanted != using_real || joycon != using_joycon)) { /* a real remote or Joy-Con came or went */
        fprintf(report, "Remote: switching to %s at field %u\n",
                wanted ? "real remote" : joycon ? "Joy-Con" : "stand-in", host_field());
        forget_remote();
        using_real = wanted;
        use_joycon(joycon);
        return enter_connect(ctx, 0);
    }
    if (using_joycon) take_joycon_samples();
    for (u32 i = 0; i < remote_count; i++) {
        R = &remotes[i];
        if (R->connected || ticks < R->connect_at) continue;
        if (i == 0) {
            using_real = wanted;
            if (joycon != using_joycon) use_joycon(joycon);
            R = &remotes[i];
        }
        u8* device = plain_memory((SCRATCH_DEVICE + i * DEVICE_SIZE) & 0x3FFFFFFFu, DEVICE_SIZE);
        memset(device, 0, DEVICE_SIZE);
        memcpy(device, "Nintendo RVL-CNT-01", 19);
        device[0x56] = (u8)i;
        R->connected = 1;
        fprintf(report, "Remote %u: connecting at field %u\n", i + 1, host_field());
        return enter_connect(ctx, 1);
    }
    /* In Nunchuk pass-through mode half the reports carry MotionPlus data, so report twice as often. */
    for (u32 i = 0; i < remote_count; i++) {
        R = &remotes[i];
        if (!R->connected || (i == 0 && using_real && !sim_until)) continue;
        if (R->raw) {
            if (R->queue_head == R->queue_tail && R->pending_head != R->pending_tail &&
                (s32)(ticks - R->pending_tick[R->pending_head % 32u]) >= 0) {
                RemoteSample sample = R->pending[R->pending_head++ % 32u];
                if (R->data_mode) send_data_raw(&sample);
            }
            continue;
        }
        u32 every = REPORT_EVERY_TICKS;
        if (R->mp_active && R->nunchuk && R->mp_mode == MP_PASSTHROUGH_NUNCHUK && every > 1) every /= 2;
        if (R->queue_head == R->queue_tail && R->data_mode && ticks % every == 0) send_data();
    }
    for (u32 k = 0; k < remote_count; k++) {
        u32 i = (turn + k) % remote_count;
        R = &remotes[i];
        if (!R->connected) continue;
        u8* out = plain_memory(SCRATCH_REPORT & 0x3FFFFFFFu, REPORT_SIZE);
        if (i == 0 && using_real && !sim_until) {
            double when = 0;
            if (!real_remote_read(out, &when)) continue;
            out[1] |= (u8)(R->live.buttons >> 8);
            out[2] |= (u8)R->live.buttons;
            add_mouse_pointer_real(out, when);
            if (out[0] < 0x30) note("real", out, REPORT_SIZE);
            R->reports_sent++;
        } else {
            if (R->queue_head == R->queue_tail) continue;
            Report* r = &R->queue[R->queue_head++ % QUEUE];
            memcpy(out, r->data, REPORT_SIZE);
            if (r->data[0] < 0x30) note("sent", r->data, REPORT_SIZE);
        }
        turn = i + 1;
        return enter_report(ctx);
    }
    return 0;
}

/* The game checks that a MotionPlus is genuine with a big-number exchange. A stand-in has no way
   to answer that and shouldn't try, so this takes the place of the whole check and records the
   result the check leaves on success. It keeps the check's own conditions for running. */
void fake_remote_verify_motion_plus(u32 channel) {
    u8* block = plain_memory(guest32(WPAD_CHANNELS + channel * 4u) & 0x3FFFFFFFu, 0xBA8);
    if (!enabled || !block) return;
    if (block[0x942] || (s8)block[0x943] < 0 || !load_be(block + 0x840, 4)) return;
    block[0x941] = 0;
    store_be(block + 0x93E, 0, 2);
    block[0x943] = 0xC;
    block[0x942] = 1;
    block[0xBA4] = 1;
    fprintf(report, "Remote: MotionPlus marked verified at field %u\n", host_field());
}

void fake_remote_report_every(u32 ticks) {
    if (ticks) REPORT_EVERY_TICKS = ticks;
}

void fake_remote_write_report(void) {
    for (u32 i = 0; i < remote_count; i++) {
        R = &remotes[i];
        fprintf(report, "remote %u mode %02X motion_plus %d ir %d data_reports %u real %d switches %u\n", i + 1,
                (u32)R->data_mode, R->mp_active, R->ir_on, R->reports_sent, i == 0 ? using_real : 0, switches);
        if (R->nunchuk_plugs || R->nunchuk_reports)
            fprintf(report, "stand-in Nunchuk: %s, plugged in or out %u times, MotionPlus mode %u, %u Nunchuk reports\n",
                    R->nunchuk ? "in" : "out", R->nunchuk_plugs, R->mp_mode, R->nunchuk_reports);
    }
    R = remotes;
    if (using_real)
    {
        fprintf(report,
                "real remote pointer: from its IR camera %u, from the mouse %u, from MotionPlus %u, none %u, "
                "centred by A %u\n",
                ir_from_remote, ir_from_mouse, ir_from_motion, ir_none, calibrations);
        motion_pointer_write_report(report);
    }
    if (real_available) real_remote_write_report();
    if (joycon_available) joycon_write_report(report);
    const u8* block = plain_memory(guest32(WPAD_CHANNELS) & 0x3FFFFFFFu, 0xBA8);
    if (block)
        fprintf(report,
                "wpad device %02X format %02X 906 %02X 920 %u 930 %02X 932 %02X 933 %02X 934 %02X "
                "942 %02X 943 %02X b9e %02X b9f %02X ba4 %02X\n",
                block[0x905], (u32)load_be(block + 0x8FC, 4), block[0x906],
                (u32)load_be(block + 0x920, 4), block[0x930], block[0x932], block[0x933], block[0x934],
                block[0x942], block[0x943], block[0xB9E], block[0xB9F], block[0xBA4]);
    if (block && (R->nunchuk_plugs || R->nunchuk_reports)) { /* the Nunchuk calibration as the game took it (0x80028DD0) */
        static const u32 at[12] = {0x890, 0x892, 0x894, 0x896, 0x898, 0x89A, 0x888, 0x886, 0x884, 0x88E, 0x88C, 0x88A};
        s32 v[12];
        for (int i = 0; i < 12; i++) v[i] = (s16)load_be(block + at[i], 2);
        fprintf(report,
                "wpad Nunchuk calibration: accelerometer zero %d %d %d, 1 g %d %d %d; stick x max %d min %d centre %d, "
                "y max %d min %d centre %d\n",
                v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
    }
}
