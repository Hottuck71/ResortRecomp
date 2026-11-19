/* A pointer worked out from a real remote's MotionPlus, for play without a sensor bar.

   The game corrects MotionPlus heading drift with the IR pointer, and its menus and "point at the screen"
   steps need one. With no sensor bar the remote's camera sees nothing, so this integrates the gyro's heading
   and pitch into a pointer, and fake_remote.c turns it into sensor bar dots while it is on the screen, as a
   real sensor bar is only seen when the remote points at the TV.

   - The gyro's zero is learned whenever the remote is held steady, wherever that zero is: the samples
     staying close to their own recent average is what counts, not closeness to the usual 8192. (It used to
     need samples within 700 counts of 8192, which a remote whose zero sits further off never gave: the
     player's one learned no zero in 20 minutes.)
   - Pressing A with no pointer on the screen (fake_remote.c) centres it, taking the last moment's average as
     the zero if none has been learned yet. A later long steady spell away from that zero corrects it.
   - Time between samples comes from when the reports arrived. A remote sends about 170 a second, and with a
     Nunchuk in pass-through mode only every other one is MotionPlus data.
   - Up and down is learned by comparing the gyro's pitch with gravity from the accelerometer.
   - Left and right can't be learned from gravity. It starts from a best guess, and F9 flips it; the choice is
     kept in analysis/native/pointer-settings.txt.
   - C re-centres it, and moving the mouse puts it where the mouse is.
   If the data doesn't look like MotionPlus data, the pointer stays off and the report says so.

   MotionPlus data, 6 bytes: yaw, roll, and pitch rates as 14-bit values (low byte in bytes 0 to 2, high six
   bits in the top of bytes 3 to 5), slow-mode flags (byte 3 bit 1 yaw, bit 0 pitch; byte 4 bit 1 roll), byte 4
   bit 0 set when an extension is plugged in, and byte 5's low two bits 2 for MotionPlus data (0 for a passed-
   through Nunchuk's). About 20 counts a degree a second in slow mode, and 4.4 in fast. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "motion_pointer.h"

#define GYRO_ZERO 8192.0f
#define SLOW_COUNTS 20.0f
#define FAST_COUNTS 4.4f
#define SAMPLE_SECONDS 0.005f    /* the first guess at the time between samples, until it is measured */
#define TIME_FOLLOW 0.02f
#define HALF_WIDTH_DEGREES 18.0f  /* turning this far from the centre reaches the window's side */
#define HALF_HEIGHT_DEGREES 14.0f
#define ON_SCREEN 1.15f           /* past this the dots are out of view, as a real sensor bar would be */
#define STILL_COUNTS 60.0f        /* about 3 degrees a second: at rest, by the zero already known */
#define STILL_SAMPLES 60u         /* about 0.35 seconds */
#define STEADY_COUNTS 40.0f       /* how far samples may stray from their recent average while held steady */
#define STEADY_MEAN 0.1f          /* that average follows this much of each sample */
#define CALM_MEAN 0.02f           /* a slower average, for a zero when A is pressed before one is learned */
#define RELEARN_SAMPLES (STILL_SAMPLES * 4u) /* steady this long away from the zero: the zero was wrong */
#define RELEARN_COUNTS 700.0f
#define ZERO_FOLLOW 0.01f
#define SIGN_EVIDENCE 400.0f      /* how much agreement before up and down is settled */
#define MAX_LOGGED 12u
#define SETTINGS "analysis/native/pointer-settings.txt"

static struct {
    float zero[3]; /* yaw, roll, pitch, in counts */
    float mean[3], calm[3]; /* recent averages of the samples */
    float yaw, pitch; /* degrees from the centre */
    float pitch_sign, yaw_sign, pitch_evidence;
    float last_accel_pitch, dt;
    double last_time;
    int have_accel_pitch, have_mean, zero_known, flipped, flipped_y, loaded;
    u32 still, steady, samples, rejected, still_samples, recentres, relearns, logged;
} mp = {.zero = {GYRO_ZERO, GYRO_ZERO, GYRO_ZERO}, .pitch_sign = 1.0f, .yaw_sign = 1.0f, .dt = SAMPLE_SECONDS};

static void load_settings(void) {
    mp.loaded = 1;
    FILE* f = fopen(SETTINGS, "r");
    if (!f) return;
    int flip = 0, flip_y = 0;
    if (fscanf(f, "flip_x %d flip_y %d", &flip, &flip_y) >= 1 && flip) {
        mp.flipped = 1;
        mp.yaw_sign = -1.0f;
    }
    mp.flipped_y = flip_y != 0;
    fclose(f);
}

static void save_settings(void) {
    FILE* f = fopen(SETTINGS, "w");
    if (!f) return;
    fprintf(f, "flip_x %d flip_y %d\n", mp.flipped, mp.flipped_y);
    fclose(f);
}

/* A line in the play report for the first few zero changes, so a session with a real remote shows them. */
static void log_zero(const char* what) {
    if (!report || mp.logged >= MAX_LOGGED) return;
    mp.logged++;
    fprintf(report, "motion pointer: %s, zero %.0f %.0f %.0f, after %u samples, %.1f ms apart\n", what, mp.zero[0],
            mp.zero[1], mp.zero[2], mp.samples, mp.dt * 1000.0f);
}

/* Where a report keeps its accelerometer and extension bytes. */
static const u8* extension_of(const u8* r) {
    switch (r[0]) {
    case 0x34: return r + 3;
    case 0x35: return r + 6;
    case 0x36: return r + 13;
    case 0x37: return r + 16;
    case 0x3D: return r + 1;
    default: return NULL;
    }
}

static const u8* accel_of(const u8* r) {
    return r[0] == 0x31 || r[0] == 0x33 || r[0] == 0x35 || r[0] == 0x37 ? r + 3 : NULL;
}

void motion_pointer_feed(const u8* report_bytes, double seconds) {
    if (!mp.loaded) load_settings();
    const u8* e = extension_of(report_bytes);
    if (!e) return;
    if ((e[5] & 3u) != 2u) { /* a passed-through Nunchuk's data, or not MotionPlus at all */
        mp.rejected++;
        return;
    }
    float raw[3] = {(float)(e[0] | ((e[3] >> 2) << 8)), (float)(e[1] | ((e[4] >> 2) << 8)),
                    (float)(e[2] | ((e[5] >> 2) << 8))};
    int slow[3] = {(e[3] >> 1) & 1, (e[4] >> 1) & 1, e[3] & 1};
    mp.samples++;

    /* Time between samples, averaged, since reports can arrive in twos. A gap over 50 ms is a pause, not a rate. */
    if (mp.last_time > 0) {
        double gap = seconds - mp.last_time;
        if (gap >= 0 && gap < 0.05) mp.dt += ((float)gap - mp.dt) * TIME_FOLLOW;
    }
    mp.last_time = seconds;

    /* Steady: every sample near the recent average, in slow mode. */
    if (!mp.have_mean) {
        memcpy(mp.mean, raw, sizeof raw);
        memcpy(mp.calm, raw, sizeof raw);
        mp.have_mean = 1;
    }
    int steady = slow[0] && slow[1] && slow[2];
    for (int i = 0; i < 3; i++) {
        if (fabsf(raw[i] - mp.mean[i]) > STEADY_COUNTS) steady = 0;
        mp.mean[i] += (raw[i] - mp.mean[i]) * STEADY_MEAN;
        mp.calm[i] += (raw[i] - mp.calm[i]) * CALM_MEAN;
    }
    mp.steady = steady ? mp.steady + 1 : 0;

    /* At rest by the zero already known: follow it slowly. */
    int still = mp.zero_known && slow[0] && slow[1] && slow[2];
    for (int i = 0; i < 3 && still; i++)
        if (fabsf(raw[i] - mp.zero[i]) > STILL_COUNTS) still = 0;
    mp.still = still ? mp.still + 1 : 0;
    if (mp.still >= STILL_SAMPLES) {
        for (int i = 0; i < 3; i++) mp.zero[i] += (raw[i] - mp.zero[i]) * ZERO_FOLLOW;
        mp.still_samples++;
    }

    if (!mp.zero_known && mp.steady >= STILL_SAMPLES) {
        /* The first rest teaches the zero, and is taken as pointing at the screen. */
        for (int i = 0; i < 3; i++) mp.zero[i] += (mp.mean[i] - mp.zero[i]) * 0.2f;
        if (mp.steady >= STILL_SAMPLES * 2u) {
            mp.zero_known = 1;
            mp.yaw = mp.pitch = 0;
            log_zero("zero learned from a rest");
        }
    } else if (mp.zero_known && !mp.still && mp.steady >= RELEARN_SAMPLES) {
        int near = 1;
        for (int i = 0; i < 3; i++)
            if (fabsf(mp.mean[i] - mp.zero[i]) > RELEARN_COUNTS) near = 0;
        if (near) {
            for (int i = 0; i < 3; i++) mp.zero[i] += (mp.mean[i] - mp.zero[i]) * 0.05f;
            if (mp.steady == RELEARN_SAMPLES) {
                mp.relearns++;
                log_zero("a long rest away from the zero; zero corrected");
            }
        }
    }

    float rate[3];
    for (int i = 0; i < 3; i++) rate[i] = (raw[i] - mp.zero[i]) / (slow[i] ? SLOW_COUNTS : FAST_COUNTS);
    if (mp.still >= STILL_SAMPLES) rate[0] = rate[2] = 0; /* no creep while resting */

    /* Up and down: the gyro's pitch should agree with gravity's. Only clear turns count as evidence. */
    const u8* a = accel_of(report_bytes);
    if (a) {
        float ax = (float)a[0] - 128.0f, ay = (float)a[1] - 128.0f, az = (float)a[2] - 128.0f;
        float accel_pitch = atan2f(ay, sqrtf(ax * ax + az * az)) * 57.2958f;
        if (mp.have_accel_pitch && fabsf(rate[2]) > 20.0f && fabsf(rate[2]) < 300.0f) {
            float change = accel_pitch - mp.last_accel_pitch;
            if (fabsf(change) < 10.0f) mp.pitch_evidence += change * rate[2] * mp.dt;
            float learned = mp.pitch_sign;
            if (mp.pitch_evidence > SIGN_EVIDENCE * 0.01f) learned = 1.0f;
            if (mp.pitch_evidence < -SIGN_EVIDENCE * 0.01f) learned = -1.0f;
            if (learned != mp.pitch_sign) { /* the movement so far was counted the wrong way round */
                mp.pitch_sign = learned;
                mp.pitch = -mp.pitch;
            }
        }
        mp.last_accel_pitch = accel_pitch;
        mp.have_accel_pitch = 1;
    }

    if (!mp.zero_known) return; /* with the zero unknown, any turn would come out wrong */
    mp.yaw += rate[0] * mp.dt * mp.yaw_sign;
    mp.pitch += rate[2] * mp.dt * mp.pitch_sign;
    /* Keep it within reach of the screen, so turning far away and back returns to the same place. */
    const float limit_x = HALF_WIDTH_DEGREES * 2.5f, limit_y = HALF_HEIGHT_DEGREES * 2.5f;
    if (mp.yaw > limit_x) mp.yaw = limit_x;
    if (mp.yaw < -limit_x) mp.yaw = -limit_x;
    if (mp.pitch > limit_y) mp.pitch = limit_y;
    if (mp.pitch < -limit_y) mp.pitch = -limit_y;
}

int motion_pointer_get(float* x, float* y) {
    if (!mp.zero_known) return 0; /* not until it has seen the remote held still once */
    /* Turning left (counter-clockwise seen from above) moves the pointer left; pitching up moves it up. */
    float px = -mp.yaw / HALF_WIDTH_DEGREES, py = (mp.flipped_y ? mp.pitch : -mp.pitch) / HALF_HEIGHT_DEGREES;
    *x = px;
    *y = py;
    return fabsf(px) < ON_SCREEN && fabsf(py) < ON_SCREEN;
}

void motion_pointer_place(float x, float y) {
    mp.yaw = -x * HALF_WIDTH_DEGREES;
    mp.pitch = (mp.flipped_y ? y : -y) * HALF_HEIGHT_DEGREES;
}

void motion_pointer_flip_y(void) {
    if (!mp.loaded) load_settings();
    mp.flipped_y = !mp.flipped_y;
    save_settings();
}

/* Centred because the player pressed A to point at the screen: trusted from now on, even if the remote
   hasn't rested long enough to learn the gyro's zero. Then the zero is the samples' average over about the
   last 0.3 seconds, while the player was pointing, unless a rest under way has already moved it there. */
void motion_pointer_calibrate(void) {
    if (!mp.zero_known && mp.have_mean) {
        if (mp.steady < STILL_SAMPLES) memcpy(mp.zero, mp.calm, sizeof mp.zero);
        mp.zero_known = 1;
        log_zero("A pressed before a rest; zero taken from the last moment");
    }
    mp.zero_known = 1;
    mp.yaw = mp.pitch = 0;
    mp.recentres++;
}

void motion_pointer_recentre(void) {
    mp.yaw = mp.pitch = 0;
    mp.recentres++;
}

void motion_pointer_flip_x(void) {
    if (!mp.loaded) load_settings();
    mp.flipped = !mp.flipped;
    mp.yaw_sign = mp.flipped ? -1.0f : 1.0f;
    mp.yaw = -mp.yaw;
    save_settings();
}

void motion_pointer_write_report(FILE* out) {
    fprintf(out,
            "motion pointer: %u MotionPlus samples, %u other, %u still, zero %.0f %.0f %.0f%s, %.1f ms apart, "
            "%u zero corrections, up-down sign %+.0f (evidence %.2f), left-right %s, up-down %s, %u re-centres\n",
            mp.samples, mp.rejected, mp.still_samples, mp.zero[0], mp.zero[1], mp.zero[2],
            mp.zero_known ? "" : " (not learned)", mp.dt * 1000.0f, mp.relearns, mp.pitch_sign, mp.pitch_evidence,
            mp.flipped ? "flipped" : "as guessed", mp.flipped_y ? "flipped" : "as learned", mp.recentres);
}

/* --test-motion-pointer: feeds made-up reports and checks the pointer follows. Returns failures. */
static double test_clock;
static float test_zero[3] = {GYRO_ZERO + 37.0f, GYRO_ZERO - 20.0f, GYRO_ZERO - 55.0f};

static void make_report(u8* r, float yaw_rate, float pitch_rate, float accel_pitch_degrees, int noise) {
    memset(r, 0, 22);
    r[0] = 0x37;
    float p = accel_pitch_degrees / 57.2958f;
    r[3] = 128;
    r[4] = (u8)(128.0f + sinf(p) * 26.0f);
    r[5] = (u8)(128.0f + cosf(p) * 26.0f);
    memset(r + 6, 0xFF, 10);
    u32 yaw = (u32)(test_zero[0] + yaw_rate * SLOW_COUNTS + (float)noise);
    u32 roll = (u32)(test_zero[1]);
    u32 pitch = (u32)(test_zero[2] + pitch_rate * SLOW_COUNTS - (float)noise);
    u8* e = r + 16;
    e[0] = (u8)yaw;
    e[1] = (u8)roll;
    e[2] = (u8)pitch;
    e[3] = (u8)(((yaw >> 8) << 2) | 3);
    e[4] = (u8)(((roll >> 8) << 2) | 2);
    e[5] = (u8)(((pitch >> 8) << 2) | 2);
}

/* A report as a remote with a Nunchuk sends between MotionPlus ones in pass-through mode. */
static void make_nunchuk_report(u8* r) {
    memset(r, 0, 22);
    r[0] = 0x37;
    r[3] = r[4] = r[5] = 128;
    memset(r + 6, 0xFF, 10);
    u8* e = r + 16;
    e[0] = e[1] = 0x80;
    e[2] = e[3] = 0x80;
    e[4] = 0x5B;
    e[5] = 0x0C; /* C and Z up, low bits 0: Nunchuk data */
}

static void feed(const u8* r, double gap) {
    test_clock += gap;
    motion_pointer_feed(r, test_clock);
}

/* A fresh pointer, unflipped, that leaves the player's settings file alone. */
static void reset_for_test(float zero_yaw, float zero_roll, float zero_pitch) {
    memset(&mp, 0, sizeof mp);
    mp.zero[0] = mp.zero[1] = mp.zero[2] = GYRO_ZERO;
    mp.pitch_sign = mp.yaw_sign = 1.0f;
    mp.dt = SAMPLE_SECONDS;
    mp.loaded = 1;
    test_zero[0] = zero_yaw;
    test_zero[1] = zero_roll;
    test_zero[2] = zero_pitch;
}

int motion_pointer_self_test(void) {
    u8 r[22];
    float x, y;
    int failures = 0;
    reset_for_test(GYRO_ZERO + 37.0f, GYRO_ZERO - 20.0f, GYRO_ZERO - 55.0f);
    /* Held still, with the zero off by a few counts: it must be learned, and the pointer must not creep. */
    for (int i = 0; i < 400; i++) {
        make_report(r, 0, 0, 0, (i % 3) - 1);
        feed(r, 0.005);
    }
    int known = motion_pointer_get(&x, &y);
    printf("still: zero known %d, pointer %.3f %.3f, zero %.1f %.1f %.1f\n", known, x, y, mp.zero[0], mp.zero[1],
           mp.zero[2]);
    if (!known || fabsf(x) > 0.02f || fabsf(y) > 0.02f) failures++;
    /* Pitch up at 50 degrees a second for 0.4 s while gravity agrees: up-down is learned, pointer goes up. */
    for (int i = 0; i < 80; i++) {
        make_report(r, 0, 50.0f, i * 50.0f * 0.005f, 0);
        feed(r, 0.005);
    }
    motion_pointer_get(&x, &y);
    printf("pitch up 20 degrees: sign %+.0f, pointer %.3f %.3f (want y near %.3f)\n", mp.pitch_sign, x, y,
           -20.0f / HALF_HEIGHT_DEGREES);
    if (mp.pitch_sign != 1.0f || fabsf(y + 20.0f / HALF_HEIGHT_DEGREES) > 0.1f) failures++;
    /* And back down while gravity agrees again. */
    for (int i = 0; i < 80; i++) {
        make_report(r, 0, -50.0f, 20.0f - i * 50.0f * 0.005f, 0);
        feed(r, 0.005);
    }
    /* Turn left at 90 degrees a second for 0.1 s: 9 degrees, half the way to the left side. */
    for (int i = 0; i < 20; i++) {
        make_report(r, 90.0f, 0, 0, 0);
        feed(r, 0.005);
    }
    motion_pointer_get(&x, &y);
    printf("turn left 9 degrees: pointer %.3f %.3f (want x near -0.5, y near 0)\n", x, y);
    if (fabsf(x + 0.5f) > 0.06f || fabsf(y) > 0.1f) failures++;
    /* Hold still for 2 seconds: nothing should move. */
    float x0 = x, y0 = y;
    for (int i = 0; i < 400; i++) {
        make_report(r, 0, 0, 0, (i % 3) - 1);
        feed(r, 0.005);
    }
    motion_pointer_get(&x, &y);
    printf("still again: pointer moved %.4f %.4f\n", x - x0, y - y0);
    if (fabsf(x - x0) > 0.01f || fabsf(y - y0) > 0.01f) failures++;
    /* Turn far right: off the screen, and the dots go away. */
    for (int i = 0; i < 200; i++) {
        make_report(r, -120.0f, 0, 0, 0);
        feed(r, 0.005);
    }
    int on = motion_pointer_get(&x, &y);
    printf("turn far right: on screen %d, pointer %.3f\n", on, x);
    if (on) failures++;
    /* Data that isn't MotionPlus is turned away. */
    u32 before = mp.samples;
    make_report(r, 0, 0, 0, 0);
    r[16 + 5] &= ~2u;
    feed(r, 0.005);
    if (mp.samples != before) failures++;
    /* Start over with gravity read the other way round: up-down must be learned as the opposite sign, and
       the pointer must still go up when the remote pitches up by gravity's account. */
    mp.yaw = mp.pitch = 0;
    mp.pitch_sign = 1.0f;
    mp.pitch_evidence = 0;
    mp.have_accel_pitch = 0;
    for (int i = 0; i < 80; i++) {
        make_report(r, 0, 50.0f, -(i * 50.0f * 0.005f), 0);
        feed(r, 0.005);
    }
    motion_pointer_get(&x, &y);
    printf("gravity the other way, pitched down 20 degrees: sign %+.0f, pointer y %.3f (want near %.3f)\n",
           mp.pitch_sign, y, 20.0f / HALF_HEIGHT_DEGREES);
    if (mp.pitch_sign != -1.0f || fabsf(y - 20.0f / HALF_HEIGHT_DEGREES) > 0.1f) failures++;

    /* A gyro whose zero is far from 8192, as the player's seemed to be: a rest must still teach it. */
    reset_for_test(GYRO_ZERO + 1829.0f, GYRO_ZERO - 902.0f, GYRO_ZERO - 31.0f);
    for (int i = 0; i < 300; i++) {
        make_report(r, 0, 0, 0, (i % 5) - 2);
        feed(r, 1.0 / 170.0);
    }
    known = motion_pointer_get(&x, &y);
    printf("zero far from 8192: known %d, zero %.0f %.0f %.0f (want 10021 7290 8161), %.2f ms apart\n", known,
           mp.zero[0], mp.zero[1], mp.zero[2], mp.dt * 1000.0f);
    if (!known || fabsf(mp.zero[0] - test_zero[0]) > 5.0f || fabsf(mp.zero[1] - test_zero[1]) > 5.0f) failures++;

    /* With a Nunchuk, MotionPlus data comes in every other report, 170 reports a second. Turning left at 90
       degrees a second for 0.1 s must still be 9 degrees: half the way to the left side. */
    for (int i = 0; i < 400; i++) { /* time to measure the new spacing */
        if (i & 1) make_nunchuk_report(r);
        else make_report(r, 0, 0, 0, (i % 3) - 1);
        feed(r, 1.0 / 170.0);
    }
    motion_pointer_recentre();
    for (int i = 0; i < 18; i++) { /* 0.1 s: nine MotionPlus samples */
        if (i & 1) make_nunchuk_report(r);
        else make_report(r, 90.0f, 0, 0, 0);
        feed(r, 1.0 / 170.0);
    }
    motion_pointer_get(&x, &y);
    printf("with a Nunchuk, turn left 9 degrees: pointer %.3f (want near -0.5), %.2f ms apart, %u other\n", x,
           mp.dt * 1000.0f, mp.rejected);
    if (fabsf(x + 0.5f) > 0.06f || mp.rejected < 200) failures++;

    /* A pressed before any rest, the remote moving a little: the zero comes from the last moment, and once it
       is set down, the pointer must stop moving. */
    reset_for_test(GYRO_ZERO + 900.0f, GYRO_ZERO + 400.0f, GYRO_ZERO - 600.0f);
    for (int i = 0; i < 200; i++) {
        make_report(r, (float)((i % 7) - 3) * 3.0f, 0, 0, 0);
        feed(r, 1.0 / 170.0);
    }
    motion_pointer_calibrate();
    for (int i = 0; i < 400; i++) {
        make_report(r, 0, 0, 0, (i % 3) - 1);
        feed(r, 1.0 / 170.0);
    }
    motion_pointer_get(&x, &y);
    x0 = x;
    y0 = y;
    for (int i = 0; i < 340; i++) {
        make_report(r, 0, 0, 0, (i % 3) - 1);
        feed(r, 1.0 / 170.0);
    }
    motion_pointer_get(&x, &y);
    printf("A before a rest: zero %.0f %.0f %.0f (want %.0f %.0f %.0f), still for 2 s moved %.4f %.4f\n", mp.zero[0],
           mp.zero[1], mp.zero[2], test_zero[0], test_zero[1], test_zero[2], x - x0, y - y0);
    if (fabsf(mp.zero[0] - test_zero[0]) > 10.0f || fabsf(x - x0) > 0.01f || fabsf(y - y0) > 0.01f) failures++;

    printf("motion pointer self-test: %d failures\n", failures);
    return failures;
}
