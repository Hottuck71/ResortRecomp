/* Does the DSP's job for AX. The game's own AX, NW4R, and sound code run unchanged and keep their
   voice blocks up to date. Each 3 ms frame, this reads those blocks, decodes and mixes the running
   voices, and writes back their position and state, so the game sees sounds advance and finish.
   Field offsets come from the game's own voice sync code. The ADPCM format is public.

   A voice ends or loops only when its address lands on its end address, as the DSP's accelerator does: one
   already past its end plays on. NW4R's stream player counts on that. It moves a stream voice's end and loop
   addresses around its ring buffer while the voice plays (the block before the song's loop point, or the stop
   point at the end of a song), and the new end is often behind the play position until the voice wraps
   around. Treating "past the end" as the end jumped there at once, which skipped music, broke the player's
   block count, and cut jingles short. A stream voice also keeps its ADPCM history when it loops, as on the
   DSP, since its ring buffer's data runs on across the wrap.

   At its end the accelerator sets the address back to the loop address, looping or not; a voice that doesn't
   loop then stops there. NW4R counts on that too: it points a one-shot voice's loop address at its buffer of
   silence, and AxVoice::IsPlayFinished (0x80140EE0) takes a voice as finished only once its address is inside
   that buffer. A voice left at its end address stopped, but its sound never finished. A jingle on the BGM_SUB
   player then stayed "playing" for good, and the game, which fades BGM_MAIN out while BGM_SUB plays
   (0x802BD034), kept the main music at volume 0: Swordplay Showdown's music and anything after it. */
#include <stdlib.h>
#include <string.h>
#include "aurora_glue.h"
#include "audio.h"
#include "host.h"

#define AX_PB_BASE 0x806F43E0u  /* word: the voice block array */
#define AX_PB_COUNT 0x806F43ECu /* word: how many voices */
#define AX_CYCLES_USED 0x806F43F4u  /* word: __AXSyncPBs' estimate of this frame's DSP cycles */
#define AX_CYCLES_LIMIT 0x806F43F8u /* word: past this, __AXSyncPBs drops the remaining voices */
#define PB_SIZE 0x140u
#define FRAME_SAMPLES 96u /* 3 ms at 32 kHz */
#define OUTPUT_RATE 32000u
#define QUEUE_HIGH (OUTPUT_RATE / 12u) /* about 83 ms of sound waiting: play a little faster */
#define QUEUE_LOW (OUTPUT_RATE / 25u)  /* about 40 ms: a little slower */

#define PB_SRC_SELECT 0x08u
#define PB_MIXER_CONTROL 0x0Cu
#define PB_STATE 0x10u
#define PB_IS_STREAM 0x12u
#define PB_MAIN_LEFT 0x14u
#define PB_MAIN_LEFT_DELTA 0x16u
#define PB_MAIN_RIGHT 0x18u
#define PB_MAIN_RIGHT_DELTA 0x1Au
#define PB_VE_VOLUME 0x6Au
#define PB_VE_DELTA 0x6Cu
#define PB_LOOP_FLAG 0x6Eu
#define PB_FORMAT 0x70u
#define PB_LOOP_ADDRESS 0x72u
#define PB_END_ADDRESS 0x76u
#define PB_CURRENT_ADDRESS 0x7Au
#define PB_ADPCM_COEFS 0x7Eu
#define PB_ADPCM_SCALE 0xA0u
#define PB_ADPCM_YN1 0xA2u
#define PB_ADPCM_YN2 0xA4u
#define PB_SRC_RATIO 0xA6u
#define PB_SRC_FRACTION 0xAAu
#define PB_SRC_LAST 0xAEu /* the last samples, oldest first */
#define PB_LOOP_SCALE 0xB4u
#define PB_LOOP_YN1 0xB6u
#define PB_LOOP_YN2 0xB8u

/* Mixer control bits (Wii AX): main left, main right, and whether the main volumes ramp this frame. The aux
   sends (effects) and surround have their own bits; they aren't mixed here yet. */
#define MIX_MAIN_LEFT 0x1u
#define MIX_MAIN_RIGHT 0x2u
#define MIX_MAIN_RAMP 0x4u

#define FORMAT_ADPCM 0x00u
#define FORMAT_PCM16 0x0Au
#define FORMAT_PCM8 0x19u
#define STATE_RUNNING 1u

static int output_live;
static FILE* wav;
static u32 wav_samples;
static u64 frames, voice_frames, voices_ended, unknown_formats, peak;

/* Every run's report gets music stream starts and stops, sounds the game asked for that didn't start, and a
   count per sound at the end. --audio-log 1 adds every voice's start and stop, and who stopped it, for finding
   sounds that go missing. "Past the end" counts samples read beyond a voice's end address, which play on until
   the address wraps around to it. */
#define MAX_TRACKED 256u
#define OVERSHOOT_LOG_LIMIT 60u
static int audio_log;
static int old_mixer; /* --audio-log 2: the mixer as it was before 2026-11-08, for comparing runs */
static struct {
    u8 running, ended_here, stream;
    u32 start, peak, ve_peak, main_peak; /* loudest output, highest envelope volume, highest main volume */
    u64 started;
} track[MAX_TRACKED];
static u32 current_voice;
static u64 overshoots[2][2]; /* [stream][looping] */
static u64 starts[2], ended_here[2], stopped_by_game[2];
static u32 overshoots_logged;
static u64 over_budget_frames;
static u64 stale_steps[2]; /* voice frames with a main volume step but no ramp: [stream] */
static u32 budget_limit, budget_peak, budget_logged;

static u32 be16(const u8* p) { return (u32)load_be(p, 2); }
static u32 be32(const u8* p) { return (u32)load_be(p, 4); }

/* Addresses in voice blocks are physical, split in two 16-bit halves. */
static const u8* sample_memory(u32 addr) {
    if (addr < cpu.ram_size) return cpu.ram + addr;
    if (addr >= 0x10000000u && addr - 0x10000000u < cpu.mem2_size) return cpu.mem2 + (addr - 0x10000000u);
    return NULL;
}

static s32 clamp16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* Returns the next source sample and moves the voice on. *ended is set when a voice runs out. */
static s32 next_sample(u8* pb, int* ended) {
    u32 format = be16(pb + PB_FORMAT), address = be32(pb + PB_CURRENT_ADDRESS);
    u32 end = be32(pb + PB_END_ADDRESS);
    s32 value = 0;
    if (format == FORMAT_ADPCM) {
        if ((address & 15u) == 0) { /* each 8-byte frame starts with a scale and predictor byte */
            const u8* header = sample_memory(address >> 1);
            if (header) store_be(pb + PB_ADPCM_SCALE, *header, 2);
            address += 2;
        }
        const u8* data = sample_memory(address >> 1);
        u32 scale = be16(pb + PB_ADPCM_SCALE);
        s32 nibble = data ? ((address & 1u) ? *data & 15 : *data >> 4) : 0;
        if (nibble >= 8) nibble -= 16;
        u32 predictor = (scale >> 4) & 7u;
        s32 c1 = (s16)be16(pb + PB_ADPCM_COEFS + predictor * 4u);
        s32 c2 = (s16)be16(pb + PB_ADPCM_COEFS + predictor * 4u + 2u);
        s32 yn1 = (s16)be16(pb + PB_ADPCM_YN1), yn2 = (s16)be16(pb + PB_ADPCM_YN2);
        value = clamp16(((nibble << (scale & 15u)) * 2048 + c1 * yn1 + c2 * yn2 + 1024) >> 11);
        store_be(pb + PB_ADPCM_YN2, (u16)yn1, 2);
        store_be(pb + PB_ADPCM_YN1, (u16)value, 2);
    } else if (format == FORMAT_PCM16) {
        const u8* data = sample_memory(address * 2u);
        value = data ? (s16)be16(data) : 0;
    } else if (format == FORMAT_PCM8) {
        const u8* data = sample_memory(address);
        value = data ? (s32)(s8)*data << 8 : 0;
    } else {
        unknown_formats++;
        *ended = 1;
        return 0;
    }
    if (address > end) {
        int stream = be16(pb + PB_IS_STREAM) != 0, looping = be16(pb + PB_LOOP_FLAG) != 0;
        overshoots[stream][looping]++;
        if (audio_log && overshoots_logged++ < OVERSHOOT_LOG_LIMIT)
            fprintf(report, "audio %llu: voice %u past its end: address %08X end %08X loop %08X stream %d looping %d\n",
                    (unsigned long long)frames, current_voice, address, end, be32(pb + PB_LOOP_ADDRESS), stream, looping);
    }
    if (old_mixer ? address >= end : address == end) {
        if (!old_mixer) address = be32(pb + PB_LOOP_ADDRESS); /* stopping too: see the top of the file */
        if (be16(pb + PB_LOOP_FLAG)) {
            address = be32(pb + PB_LOOP_ADDRESS);
            if (format == FORMAT_ADPCM && (old_mixer || !be16(pb + PB_IS_STREAM))) {
                store_be(pb + PB_ADPCM_SCALE, be16(pb + PB_LOOP_SCALE), 2);
                store_be(pb + PB_ADPCM_YN1, be16(pb + PB_LOOP_YN1), 2);
                store_be(pb + PB_ADPCM_YN2, be16(pb + PB_LOOP_YN2), 2);
            }
        } else {
            *ended = 1;
        }
    } else {
        address++;
    }
    store_be(pb + PB_CURRENT_ADDRESS, address, 4);
    return value;
}

static void mix_voice(u8* pb, s32* mix) {
    u32 control = old_mixer ? MIX_MAIN_LEFT | MIX_MAIN_RIGHT | MIX_MAIN_RAMP : be32(pb + PB_MIXER_CONTROL);
    int to_left = (control & MIX_MAIN_LEFT) != 0, to_right = (control & MIX_MAIN_RIGHT) != 0;
    int ramp = (control & MIX_MAIN_RAMP) != 0; /* without it the DSP leaves the main volumes alone */
    if (!(be32(pb + PB_MIXER_CONTROL) & MIX_MAIN_RAMP) && (be16(pb + PB_MAIN_LEFT_DELTA) || be16(pb + PB_MAIN_RIGHT_DELTA)))
        stale_steps[be16(pb + PB_IS_STREAM) != 0]++; /* the old mixer applied these, every frame */
    u32 ratio = be32(pb + PB_SRC_RATIO), fraction = be16(pb + PB_SRC_FRACTION);
    s32 last = (s16)be16(pb + PB_SRC_LAST + 6), previous = (s16)be16(pb + PB_SRC_LAST + 4);
    s32 volume = (s32)be16(pb + PB_VE_VOLUME), volume_delta = (s16)be16(pb + PB_VE_DELTA);
    s32 left = (s32)be16(pb + PB_MAIN_LEFT), left_delta = ramp ? (s16)be16(pb + PB_MAIN_LEFT_DELTA) : 0;
    s32 right = (s32)be16(pb + PB_MAIN_RIGHT), right_delta = ramp ? (s16)be16(pb + PB_MAIN_RIGHT_DELTA) : 0;
    int ended = 0;
    for (u32 i = 0; i < FRAME_SAMPLES && !ended; i++) {
        fraction += ratio;
        while (fraction >= 0x10000u && !ended) {
            fraction -= 0x10000u;
            previous = last;
            last = next_sample(pb, &ended);
        }
        s32 sample = previous + (s32)(((s64)(last - previous) * (s32)fraction) >> 16);
        s64 enveloped = ((s64)sample * volume) >> 15;
        if (to_left) mix[i * 2] += (s32)((enveloped * left) >> 15);
        if (to_right) mix[i * 2 + 1] += (s32)((enveloped * right) >> 15);
        volume += volume_delta;
        left += left_delta;
        right += right_delta;
        if (volume < 0) volume = 0;
        if (left < 0) left = 0;
        if (right < 0) right = 0;
    }
    store_be(pb + PB_SRC_FRACTION, fraction & 0xFFFFu, 2);
    store_be(pb + PB_SRC_LAST + 4, (u16)previous, 2);
    store_be(pb + PB_SRC_LAST + 6, (u16)last, 2);
    store_be(pb + PB_VE_VOLUME, (u32)(volume > 0xFFFF ? 0xFFFF : volume), 2);
    if (ramp) {
        store_be(pb + PB_MAIN_LEFT, (u32)(left > 0xFFFF ? 0xFFFF : left), 2);
        store_be(pb + PB_MAIN_RIGHT, (u32)(right > 0xFFFF ? 0xFFFF : right), 2);
    }
    if (ended) {
        store_be(pb + PB_STATE, 0, 2);
        voices_ended++;
        if (current_voice < MAX_TRACKED) track[current_voice].ended_here = 1;
    }
}

/* Notes voices starting and stopping, from one frame to the next. */
static void watch_voice(u32 v, const u8* pb) {
    if (v >= MAX_TRACKED) return;
    int running = be16(pb + PB_STATE) == STATE_RUNNING;
    u32 address = be32(pb + PB_CURRENT_ADDRESS);
    if (running && !track[v].running) {
        int stream = be16(pb + PB_IS_STREAM) != 0;
        track[v].stream = (u8)stream;
        track[v].start = address;
        track[v].started = frames;
        track[v].peak = track[v].ve_peak = track[v].main_peak = 0;
        track[v].ended_here = 0;
        starts[stream]++;
        if (audio_log || stream)
            fprintf(report, "audio %llu: voice %u starts: format %u stream %d looping %d address %08X end %08X loop %08X "
                    "ratio %08X volume %04X%+d main %04X%+d %04X%+d mixer %08X\n",
                    (unsigned long long)frames, v, be16(pb + PB_FORMAT), stream, be16(pb + PB_LOOP_FLAG) != 0, address,
                    be32(pb + PB_END_ADDRESS), be32(pb + PB_LOOP_ADDRESS), be32(pb + PB_SRC_RATIO), be16(pb + PB_VE_VOLUME),
                    (s16)be16(pb + PB_VE_DELTA), be16(pb + PB_MAIN_LEFT), (s16)be16(pb + PB_MAIN_LEFT_DELTA),
                    be16(pb + PB_MAIN_RIGHT), (s16)be16(pb + PB_MAIN_RIGHT_DELTA), be32(pb + PB_MIXER_CONTROL));
    } else if (!running && track[v].running) {
        int stream = track[v].stream;
        if (track[v].ended_here) ended_here[stream]++;
        else stopped_by_game[stream]++;
        if (audio_log || stream)
            fprintf(report, "audio %llu: voice %u stops after %llu frames (%s), stream %d, started at %08X, now %08X, "
                    "peak %u, highest volume %04X main %04X\n",
                    (unsigned long long)frames, v, (unsigned long long)(frames - track[v].started),
                    track[v].ended_here ? "reached its end here" : "stopped by the game", stream, track[v].start, address,
                    track[v].peak, track[v].ve_peak, track[v].main_peak);
    }
    if (running) {
        u32 ve = be16(pb + PB_VE_VOLUME), main = be16(pb + PB_MAIN_LEFT) | be16(pb + PB_MAIN_RIGHT);
        if (ve > track[v].ve_peak) track[v].ve_peak = ve;
        if (main > track[v].main_peak) track[v].main_peak = main;
    }
    track[v].running = (u8)running;
}

static void write_wav_header(void) {
    u8 h[44];
    u32 bytes = wav_samples * 4u;
    memcpy(h, "RIFF", 4);
    u32 v = bytes + 36;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    u16 w = 1;
    memcpy(h + 20, &w, 2);
    w = 2;
    memcpy(h + 22, &w, 2);
    v = OUTPUT_RATE;
    memcpy(h + 24, &v, 4);
    v = OUTPUT_RATE * 4u;
    memcpy(h + 28, &v, 4);
    w = 4;
    memcpy(h + 32, &w, 2);
    w = 16;
    memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, wav);
    fseek(wav, 0, SEEK_END);
}

void audio_setup(int live, const char* wav_path, int log) {
    audio_log = log != 0;
    old_mixer = log == 2;
    output_live = live && glue_audio_open(OUTPUT_RATE);
    if (wav_path && (wav = fopen(wav_path, "wb"))) write_wav_header();
}

void audio_frame(void) {
    u32 base = (u32)load_be(plain_memory(AX_PB_BASE & 0x3FFFFFFFu, 4), 4);
    u32 count = (u32)load_be(plain_memory(AX_PB_COUNT & 0x3FFFFFFFu, 4), 4);
    s32 mix[FRAME_SAMPLES * 2];
    s16 out[FRAME_SAMPLES * 2 + 2]; /* room for one repeated sample */
    memset(mix, 0, sizeof mix);
    frames++;
    /* The game's own DSP budget: __AXSyncPBs adds up each voice's cost by priority and drops every voice past
       the limit, telling NW4R it's gone. On a Wii that only happens when the DSP really is overloaded. */
    budget_limit = (u32)load_be(plain_memory(AX_CYCLES_LIMIT & 0x3FFFFFFFu, 4), 4);
    u32 used = (u32)load_be(plain_memory(AX_CYCLES_USED & 0x3FFFFFFFu, 4), 4);
    if (used > budget_peak) budget_peak = used;
    if (used >= budget_limit) {
        over_budget_frames++;
        if (audio_log && budget_logged++ < 40)
            fprintf(report, "audio %llu: DSP budget passed: estimate %u, limit %u, so voices were dropped\n",
                    (unsigned long long)frames, used, budget_limit);
    }
    for (u32 v = 0; v < count; v++) {
        u8* pb = plain_memory((base + v * PB_SIZE) & 0x3FFFFFFFu, PB_SIZE);
        if (!pb) continue;
        watch_voice(v, pb);
        if (be16(pb + PB_STATE) != STATE_RUNNING) continue;
        voice_frames++;
        current_voice = v;
        s32 before[FRAME_SAMPLES * 2];
        int measure = v < MAX_TRACKED && (audio_log || track[v].stream);
        if (measure) memcpy(before, mix, sizeof mix);
        mix_voice(pb, mix);
        if (measure)
            for (u32 i = 0; i < FRAME_SAMPLES * 2; i++) {
                u32 size = (u32)abs(mix[i] - before[i]);
                if (size > track[v].peak) track[v].peak = size;
            }
    }
    for (u32 i = 0; i < FRAME_SAMPLES * 2; i++) {
        out[i] = (s16)clamp16(mix[i]);
        u32 size = (u32)abs(out[i]);
        if (size > peak) peak = size;
    }
    if (output_live) {
        /* The game's clock and the sound card's drift apart a little (the game ran 0.13% fast in a 21 minute
           session). Rather than drop a whole 3 ms frame when the queue gets long, which clicks, one sample is
           left out while it's long and one repeated while it's short. */
        u32 queued = glue_audio_queued() / 4u, samples = FRAME_SAMPLES;
        if (queued > QUEUE_HIGH) samples--;
        else if (queued < QUEUE_LOW) {
            out[FRAME_SAMPLES * 2] = out[FRAME_SAMPLES * 2 - 2];
            out[FRAME_SAMPLES * 2 + 1] = out[FRAME_SAMPLES * 2 - 1];
            samples++;
        }
        glue_audio_write(out, samples * 4u);
    }
    if (wav) {
        fwrite(out, 1, FRAME_SAMPLES * 4u, wav);
        wav_samples += FRAME_SAMPLES;
    }
}

#define NO_SOUND 0xFFFFFFFFu
#define BY_LABEL 0xFFFFFFFEu /* asked for by name: no ID to count it under */
#define SOUND_IDS 4096u /* the archive has 1,610 */
static u32 requested_sound = NO_SOUND;
static u64 sounds_requested, sounds_started;
static u32 sound_requests[SOUND_IDS], sound_starts[SOUND_IDS];

/* A request that wasn't followed by its start failed: NW4R refused it (its player was full of louder sounds,
   its data wasn't loaded, ...). The first few of each sound are noted with their time. */
static void close_request(void) {
    if (requested_sound >= SOUND_IDS) return;
    u32 failed = sound_requests[requested_sound] - sound_starts[requested_sound];
    if (failed <= 3)
        fprintf(report, "audio %llu: sound %u didn't start\n", (unsigned long long)frames, requested_sound);
}

void audio_sound_requested(unsigned id) {
    close_request();
    sounds_requested++;
    requested_sound = id;
    if (id < SOUND_IDS) sound_requests[id]++;
    if (audio_log) fprintf(report, "audio %llu: sound %u requested\n", (unsigned long long)frames, id);
}

void audio_sound_started(void) {
    if (requested_sound == NO_SOUND) return; /* started after a prepare, not a request */
    sounds_started++;
    if (requested_sound < SOUND_IDS) sound_starts[requested_sound]++;
    if (audio_log) fprintf(report, "audio %llu: sound %u started\n", (unsigned long long)frames, requested_sound);
    requested_sound = NO_SOUND;
}

void audio_write_report(void) {
    if (wav) {
        write_wav_header();
        fclose(wav);
        wav = NULL;
    }
    fprintf(report, "audio frames %llu voice_frames %llu voices_ended %llu unknown_formats %llu peak %llu\n",
            (unsigned long long)frames, (unsigned long long)voice_frames, (unsigned long long)voices_ended,
            (unsigned long long)unknown_formats, (unsigned long long)peak);
    fprintf(report, "audio voices: %llu started, %llu reached their end here, %llu stopped by the game; "
            "streams: %llu started, %llu reached their end here, %llu stopped by the game\n",
            (unsigned long long)starts[0], (unsigned long long)ended_here[0], (unsigned long long)stopped_by_game[0],
            (unsigned long long)starts[1], (unsigned long long)ended_here[1], (unsigned long long)stopped_by_game[1]);
    close_request();
    requested_sound = NO_SOUND;
    fprintf(report, "audio sounds: %llu requested, %llu started\n", (unsigned long long)sounds_requested,
            (unsigned long long)sounds_started);
    for (u32 id = 0; id < SOUND_IDS; id++)
        if (sound_requests[id])
            fprintf(report, "audio sound %u: requested %u, started %u\n", id, sound_requests[id], sound_starts[id]);
    fprintf(report, "audio main volume steps without a ramp: sounds %llu frames, streams %llu frames\n",
            (unsigned long long)stale_steps[0], (unsigned long long)stale_steps[1]);
    fprintf(report, "audio DSP budget: limit %u, highest estimate %u, frames over the limit %llu\n", budget_limit,
            budget_peak, (unsigned long long)over_budget_frames);
    fprintf(report, "audio past the end: sounds %llu (looping %llu), streams %llu (looping %llu)\n",
            (unsigned long long)(overshoots[0][0] + overshoots[0][1]), (unsigned long long)overshoots[0][1],
            (unsigned long long)(overshoots[1][0] + overshoots[1][1]), (unsigned long long)overshoots[1][1]);
}
