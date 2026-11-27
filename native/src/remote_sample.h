/* A real Wii Remote's raw values and calibration, as fake_remote.c replays them. On their own, with no game
   headers, so the launcher and PairJoyCon.exe can build joycon.c too. */
#ifndef REMOTE_SAMPLE_H
#define REMOTE_SAMPLE_H
#include <stdint.h>

/* The same types as the game's own headers (identical typedefs may be repeated). */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;

/* One report's worth of a real Wii Remote, as remote_driver.c reads it: raw values, to be read with that remote's
   own calibration (RemoteCalibration). */
#define SAMPLE_HAS_MOTION_PLUS 0x80u
#define SAMPLE_HAS_NUNCHUK 0x80u
#define NO_DOT 0x3FFu
typedef struct {
    u16 buttons;     /* the remote's buttons, without the accelerometer's low bits */
    u16 accel[3];    /* ten bits each */
    u16 gyro[3];     /* MotionPlus yaw, roll, pitch: fourteen bits each */
    u8 gyro_flags;   /* SAMPLE_HAS_MOTION_PLUS, then slow mode: bit 0 yaw, bit 1 roll, bit 2 pitch */
    u8 nunchuk_flags; /* SAMPLE_HAS_NUNCHUK, then bit 0 C, bit 1 Z, pressed */
    u8 stick[2];
    u16 naccel[3];   /* the Nunchuk's accelerometer, ten bits each */
    u16 dots[2][2];  /* two sensor bar dots, x and y, ten bits; NO_DOT when unseen */
    u8 offset;       /* milliseconds into the field */
    u8 pad[3];
} RemoteSample;

/* A real remote's calibration, sent to every PC when a player joins, so their stand-in for that player reads
   that player's raw values as the console would. */
typedef struct {
    u8 present;        /* a real remote drives this player */
    u8 motion_plus;
    u8 nunchuk;        /* its Nunchuk was plugged in when the game started */
    u8 pad;
    u8 accel[10];      /* EEPROM 0x16: zero and 1 g for each axis, and a check byte */
    u8 gyro[32];       /* MotionPlus calibration, at 0xA40020 while it is on */
    u8 nunchuk_cal[16];/* at 0xA40020 of the Nunchuk */
    u8 pad2[2];
} RemoteCalibration;

#endif
