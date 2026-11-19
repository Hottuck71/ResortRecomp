/* Feeds the decoded command stream to aurora. Most packets pass straight through. What changes is
   anything aurora holds as a host pointer: vertex arrays, textures, palettes, and EFB copies. */
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aurora_glue.h"
#include "fake_remote.h"
#include "gx_stream.h"
#include "gx_thread.h"

#define EFB_WIDTH 640u
#define EFB_HEIGHT 528u
#define ARRAYS 16u
#define MAPS 8u
#define TLUT_SLOTS 20u
#define BP_GEN_MODE 0x00u
#define BP_BLEND 0x41u
#define LOGIC_COPY 3u
#define LOGIC_OR 7u
#define AURORA_LOGIC_OPS 0x29u /* the ops aurora implements */
#define CULL_ALL 3u
#define BP_MASK 0xFEu
#define BP_DONE 0x45u
#define BP_TOKEN 0x47u
#define BP_TOKEN_RAISE 0x48u
#define BP_COPY_SOURCE 0x49u
#define BP_COPY_SIZE 0x4Au
#define BP_COPY_TARGET 0x4Bu
#define BP_PIXEL_CONTROL 0x43u
#define BP_Z_MODE 0x40u
#define BP_Z_TEXTURE 0xF5u
#define BP_COPY 0x52u
#define BP_SCISSOR_OFFSET 0x59u
#define BP_TLUT_SOURCE 0x64u
#define BP_TLUT_LOAD 0x65u
#define XF_VIEWPORT_Z_SCALE 0x101Cu
#define XF_VIEWPORT_Z_ORIGIN 0x101Fu
#define DEPTH_MAX 16777215.0f
#define XF_VIEWPORT_X 0x101Du
#define XF_VIEWPORT_Y 0x101Eu
#define PIXEL_FORMAT_Z24 3u
#define AURORA_FORMAT_CTF 0x20u
#define AURORA_FORMAT_ZTF 0x10u
#define VIEWPORT_OFFSET_FIX 2.0f /* the SDK adds 342 to viewport origins, and aurora assumes 340 */

static int active;
static u32 array_base[ARRAYS], array_size[ARRAYS], array_declared[ARRAYS];
static u32 texture_dirty;
static struct { u32 source, entries; } tlut_loads[0x400];
static u8 packet[5 + 4 * 0x10000];
static u32 unknown_copy_formats, unknown_logic_ops;
static const char* capture_prefix;
static int quiet_mode, idle_this_frame;
static u64 copies_evicted, shots_saved;
static u32 busy_frames;
static unsigned long long busy_started;

/* Aurora keeps EFB copies on the GPU, keyed by address, and prefers them over memory. On the console
   the copy simply lands in memory, so when the game later reuses that memory the new data wins. To
   notice that, each copy leaves a stamp at its address. A missing stamp means the memory was reused. */
#define COPY_SLOTS 8192u
#define STAMP_MAGIC 0xC0917E5Du
static struct {
    u32 target, stamp, bound;
} copies[COPY_SLOTS];
static u32 next_stamp;
static u32 log_from, log_until; /* --copy-log: every copy and texture bind between these frames */
static u32 frames;

void gx_adapter_copy_log(u32 from, u32 until) {
    log_from = from;
    log_until = until;
}

static int logging(void) { return frames >= log_from && frames < log_until; }

/* --copy-shot: saves the EFB just before each copy to one address in one frame. */
static u32 shot_frame, shot_target, shot_count;

void gx_adapter_copy_shot(u32 frame, u32 target) {
    shot_frame = frame;
    shot_target = target;
}

static u32 copy_slot(u32 target) {
    u32 at = (target >> 5) * 2654435761u % COPY_SLOTS;
    for (u32 i = 0; i < COPY_SLOTS; i++, at = (at + 1) % COPY_SLOTS)
        if (!copies[at].target || copies[at].target == target) return at;
    return at;
}

/* A rolling record of the last few seconds of copies and draws, kept all the time and saved beside an F12
   shot. A problem the player sees can then be read afterwards, without having to reproduce it. */
#define RING_LINES 131072u
/* Records hold numbers, and text is only made when the log is saved, so keeping it costs next to nothing. */
enum { RING_DRAWS, RING_COPY, RING_SHOW, RING_EVICT };
typedef struct {
    u32 kind, frame, v[14];
} RingRecord;
static RingRecord* ring;
static u32 ring_next;

static RingRecord* ring_record(u32 kind) {
    if (!ring) {
        ring = calloc(RING_LINES, sizeof *ring);
        if (!ring) return NULL;
    }
    RingRecord* r = &ring[ring_next++ % RING_LINES];
    r->kind = kind;
    r->frame = frames;
    return r;
}

static void ring_save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f || !ring) {
        if (f) fclose(f);
        return;
    }
    u32 count = ring_next < RING_LINES ? ring_next : RING_LINES;
    for (u32 i = ring_next - count; i != ring_next; i++) {
        const RingRecord* r = &ring[i % RING_LINES];
        const u32* v = r->v;
        switch (r->kind) {
        case RING_DRAWS:
            fprintf(f, "F%u draws %u z %06X zt %06X bl %06X ac %06X t0 %08X t1 %08X zb %06X\n", r->frame, v[0], v[1],
                    v[2], v[3], v[4], v[5], v[6], v[7]);
            break;
        case RING_COPY:
        case RING_SHOW:
            fprintf(f, "F%u %s %u,%u %ux%u to %08X fmt %02X mip %u clear %u pe %06X z %06X cz %06X cc %06X%06X\n",
                    r->frame, r->kind == RING_SHOW ? "show" : "copy", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                    v[8], v[9], v[10], v[11], v[12]);
            break;
        default:
            fprintf(f, "F%u evict %08X\n", r->frame, v[0]);
            break;
        }
    }
    fclose(f);
}

/* Draws in a row with the same depth, blend, and first two textures become one record. */
static struct {
    u32 count, zmode, ztex, blend, alpha, tex0, tex1, zbias;
} group;

static void group_flush(void) {
    if (!group.count) return;
    RingRecord* r = ring_record(RING_DRAWS);
    if (r) memcpy(r->v, &group, 8 * sizeof(u32));
    group.count = 0;
}

static void stamp_copy(u32 target) {
    u8* memory = plain_memory(target, 8);
    u32 slot = copy_slot(target);
    if (!memory) return;
    copies[slot].target = target;
    copies[slot].stamp = ++next_stamp;
    store_be(memory, STAMP_MAGIC, 4);
    store_be(memory + 4, next_stamp, 4);
}

static void check_copy(u32 target, void* host) {
    u32 slot = copy_slot(target);
    const u8* memory = host;
    if (copies[slot].target != target) return;
    if (load_be(memory, 4) == STAMP_MAGIC && load_be(memory + 4, 4) == copies[slot].stamp) return;
    glue_destroy_copy(host);
    copies_evicted++;
    RingRecord* evicted = ring_record(RING_EVICT);
    if (evicted) evicted->v[0] = target;
    if (log_until) fprintf(report, "evict frame %u %08X\n", frames, target);
    /* Removing from an open table: everything after this slot may need to move up. */
    copies[slot].target = 0;
    for (u32 at = (slot + 1) % COPY_SLOTS; copies[at].target; at = (at + 1) % COPY_SLOTS) {
        u32 moved = copies[at].target, stamp = copies[at].stamp, bound = copies[at].bound;
        copies[at].target = 0;
        u32 place = copy_slot(moved);
        copies[place].target = moved;
        copies[place].stamp = stamp;
        copies[place].bound = bound;
    }
}
static u32 capture_every;
static u32 capture_from;

int gx_adapter_active(void) { return active; }

u32 gx_adapter_frames(void) { return frames; }

void gx_adapter_note_idle(void) { idle_this_frame = 1; }

void gx_adapter_stop(void) {
    if (active) glue_shutdown();
    active = 0;
}

int gx_adapter_start(int argc, char** argv, int quiet, const char* capture, u32 every) {
    capture_prefix = capture;
    quiet_mode = quiet;
    capture_every = every;
    for (int i = 1; i + 1 < argc; i++) /* frames before this aren't saved, so a test can capture densely late on */
        if (!strcmp(argv[i], "--capture-from")) capture_from = (u32)atoi(argv[i + 1]);
    active = glue_init(argc, argv, EFB_WIDTH, EFB_HEIGHT, quiet);
    return active;
}

static u32 cull_mode(u32 gen_mode) { return (gen_mode >> 14) & 3u; }

/* Aurora stops on most logic ops. OR is close to adding, and others fall back to a plain copy. */
static u32 usable_blend(u32 value) {
    u32 op = (value >> 12) & 15u;
    int blend = value & 1u, logic = (value >> 1) & 1u, subtract = (value >> 11) & 1u;
    if (blend || !logic || subtract || (AURORA_LOGIC_OPS >> op) & 1u) return value;
    if (op == LOGIC_OR) { /* blend on, logic off, source and destination factors of one */
        return (value & ~0x7E3u) | 1u | (1u << 5) | (1u << 8);
    }
    unknown_logic_ops++;
    return (value & ~0xF000u) | (LOGIC_COPY << 12);
}

static void forward_bp(u32 reg, u32 value) {
    packet[0] = 0x61;
    store_be(packet + 1, (reg << 24) | value, 4);
    glue_write(packet, 5);
}

static int is_texture_reg(u32 reg) { return (reg >= 0x80 && reg <= 0x9B) || (reg >= 0xA0 && reg <= 0xBB); }

static u32 map_of(u32 reg) {
    return (reg >= 0xA0 ? 4u : 0u) + (reg & 3u);
}

/* The same bits a real GXSetTexCopyDst writes, turned back into a format. */
static u32 copy_format(const GxState* s, u32 value) {
    u32 hw = ((value >> 4) & 7u) | (((value >> 3) & 1u) << 3);
    int intensity = (value >> 15) & 1u;
    int depth = (s->bp[BP_PIXEL_CONTROL] & 7u) == PIXEL_FORMAT_Z24;
    if (depth) return (hw == 1 || hw == 3 || hw == 6) ? AURORA_FORMAT_ZTF | hw
                                                      : AURORA_FORMAT_ZTF | AURORA_FORMAT_CTF | hw;
    if (intensity && hw <= 3) return hw;
    if (hw >= 4 && hw <= 6) return hw;
    if (hw == 0 || hw == 2 || hw == 3 || (hw >= 7 && hw <= 0xC)) return AURORA_FORMAT_CTF | hw;
    unknown_copy_formats++;
    return 6;
}

static void efb_copy(const GxState* s, u32 value) {
    u32 source = s->bp[BP_COPY_SOURCE], size = s->bp[BP_COPY_SIZE];
    u32 x = source & 0x3FFu, y = (source >> 10) & 0x3FFu;
    u32 w = (size & 0x3FFu) + 1, h = ((size >> 10) & 0x3FFu) + 1;
    int clear = (value >> 11) & 1u;
    group_flush();
    RingRecord* noted = ring_record((value >> 14) & 1u ? RING_SHOW : RING_COPY);
    if (noted) {
        const u32 fields[13] = {x, y, w, h, (s->bp[BP_COPY_TARGET] & 0xFFFFFFu) << 5,
                                (value >> 14) & 1u ? 0u : copy_format(s, value), (value >> 9) & 1u, (u32)clear,
                                s->bp[BP_PIXEL_CONTROL], s->bp[BP_Z_MODE], s->bp[0x51], s->bp[0x4F], s->bp[0x50]};
        memcpy(noted->v, fields, sizeof fields);
    }
    if ((value >> 14) & 1u) {
        if (capture_prefix && frames + 1 >= capture_from && (frames + 1) % capture_every == 0) {
            char path[600];
            snprintf(path, sizeof path, "%s-%05u.ppm", capture_prefix, frames + 1);
            glue_capture(path);
        }
        if (glue_shot_wanted()) {
            char path[600];
            _mkdir("analysis/native/shots");
            snprintf(path, sizeof path, "analysis/native/shots/shot-%05u.ppm", frames + 1);
            glue_capture(path);
            snprintf(path, sizeof path, "analysis/native/shots/log-%05u.txt", frames + 1);
            ring_save(path);
            shots_saved++;
        }
        glue_copy_display(x, y, w, h, clear);
        return;
    }
    u32 mip = (value >> 9) & 1u, target = (s->bp[BP_COPY_TARGET] & 0xFFFFFFu) << 5;
    u32 out_w = mip ? w / 2 : w, out_h = mip ? h / 2 : h;
    if (shot_target == target && frames == shot_frame) {
        char path[600];
        snprintf(path, sizeof path, "analysis/native/window/copy-%05u-%08X-%u.ppm", frames, target, shot_count++);
        glue_capture(path);
    }
    if (log_until && (logging() || copies[copy_slot(target)].target != target))
        fprintf(report, "copy frame %u src %u,%u %ux%u to %08X out %ux%u format %02X mip %u clear %d pe %06X zmode %06X blend %06X\n",
                frames, x, y, w, h, target, out_w, out_h, copy_format(s, value), mip, clear,
                s->bp[BP_PIXEL_CONTROL], s->bp[BP_Z_MODE], s->bp[BP_BLEND]);
    glue_copy_texture(x, y, w, h, out_w, out_h, copy_format(s, value), (int)mip,
                      plain_memory(target, 32), clear);
    stamp_copy(target);
}

void gx_adapter_bp(const GxState* s, u32 reg, u32 value) {
    if (!active) return;
    switch (reg) {
    case BP_DONE: /* the game may reuse memory after this, so aurora has to catch up first */
    case BP_TOKEN_RAISE:
        glue_drain();
        return;
    case BP_MASK:
    case BP_TOKEN:
        return; /* masks are applied already, and the host answers these itself */
    case BP_COPY:
        efb_copy(s, value);
        return;
    case BP_TLUT_LOAD: {
        u32 slot = value & 0x3FFu;
        tlut_loads[slot].source = (s->bp[BP_TLUT_SOURCE] & 0xFFFFFFu) << 5;
        tlut_loads[slot].entries = ((value >> 10) & 0x7FFu) * 16u;
        texture_dirty = 0xFFu;
        return;
    }
    default:
        break;
    }
    if (is_texture_reg(reg)) texture_dirty |= 1u << map_of(reg);
    if (reg == BP_SCISSOR_OFFSET) { /* the hardware subtracts this, and aurora adds it */
        u32 x = (0u - (s->bp[reg] & 0x3FFu)) & 0x1FFu, y = (0u - ((s->bp[reg] >> 10) & 0x3FFu)) & 0x1FFu;
        forward_bp(reg, x | (y << 10));
        return;
    }
    if (reg == BP_GEN_MODE && cull_mode(s->bp[reg]) == CULL_ALL) { /* aurora stops on this value */
        forward_bp(reg, s->bp[reg] & ~(3u << 14));
        return;
    }
    if (reg == BP_BLEND) {
        forward_bp(reg, usable_blend(s->bp[reg]));
        return;
    }
    forward_bp(reg, s->bp[reg]);
}

void gx_adapter_cp(u32 reg, u32 value) {
    if (!active) return;
    if (reg >= 0xA0 && reg <= 0xAF) { /* a host pointer has to take the place of this */
        array_base[reg & 15u] = value;
        array_declared[reg & 15u] = 0;
        return;
    }
    packet[0] = 0x08;
    packet[1] = (u8)reg;
    store_be(packet + 2, value, 4);
    glue_write(packet, 6);
}

void gx_adapter_xf(const u8* p, u32 size) {
    if (!active) return;
    u32 head = (u32)load_be(p + 1, 4), addr = head & 0xFFFFu, count = (head >> 16) + 1;
    memcpy(packet, p, size);
    for (u32 i = 0; i < count; i++) {
        u32 at = addr + i;
        if (at != XF_VIEWPORT_X && at != XF_VIEWPORT_Y && at != XF_VIEWPORT_Z_SCALE && at != XF_VIEWPORT_Z_ORIGIN)
            continue;
        u32 bits = (u32)load_be(packet + 5 + i * 4, 4);
        float f;
        memcpy(&f, &bits, 4);
        if (at == XF_VIEWPORT_X || at == XF_VIEWPORT_Y) f -= VIEWPORT_OFFSET_FIX;
        else if (f > DEPTH_MAX) f = DEPTH_MAX; /* the game sends 2^24, which aurora maps past 1.0 */
        memcpy(&bits, &f, 4);
        store_be(packet + 5 + i * 4, bits, 4);
    }
    glue_write(packet, size);
}

/* An indexed load fetches from memory on the hardware. Here it becomes a load of the fetched values. */
void gx_adapter_xf_values(u32 addr, const u8* values, u32 count) {
    if (!active) return;
    packet[0] = 0x10;
    store_be(packet + 1, ((count - 1) << 16) | addr, 4);
    memcpy(packet + 5, values, count * 4);
    glue_write(packet, 5 + count * 4);
}

void gx_adapter_raw(const u8* p, u32 size) {
    if (active) glue_write(p, size);
}

static void need_array(const GxState* s, u32 index, u32 size) {
    u32 base = array_base[index] & 0x3FFFFFFFu;
    if (size <= array_declared[index]) return;
    void* host = plain_memory(base, size);
    if (!host) return;
    /* Aurora copies a re-declared array into its per-frame storage again. Growing by at least double keeps a
       big mesh drawn in pieces with rising indices from being copied once a draw, which overflowed the
       storage buffer in Speed Slice. */
    u32 grown = array_declared[index] * 2u;
    if (grown > size && grown <= 0x400000u && plain_memory(base, grown)) size = grown;
    size = (size + 1023u) & ~1023u;
    if (!plain_memory(base, size)) size = array_declared[index] > size ? array_declared[index] : size;
    array_declared[index] = size;
    glue_set_array(index, host, size, s->cp[0xB0 + index] & 0xFFu);
}

static void scan_attribute(const GxState* s, const GxDraw* d, u32 kind, u32 offset, u32 array, u32 element) {
    if (kind < 2) return;
    u32 highest = 0, step = d->layout->size, stride = s->cp[0xB0 + array] & 0xFFu;
    const u8* field = d->vertices + offset;
    if (kind == 2) {
        if (255u * stride + element <= array_declared[array]) return; /* no index could go past it */
        for (u32 v = 0; v < d->count; v++, field += step)
            if (field[0] > highest) highest = field[0];
    } else {
        if (65535u * stride + element <= array_declared[array]) return;
        for (u32 v = 0; v < d->count; v++, field += step) {
            u32 index = ((u32)field[0] << 8) | field[1];
            if (index > highest) highest = index;
        }
    }
    need_array(s, array, highest * stride + element);
}

static void bind_textures(const GxState* s) {
    for (u32 map = 0; map < MAPS && texture_dirty; map++) {
        if (!(texture_dirty & (1u << map))) continue;
        texture_dirty &= ~(1u << map);
        GxTexture t;
        if (!gx_texture_info(s, map, &t) || !t.address) continue;
        void* host = plain_memory(t.address, gx_texture_size(&t));
        if (!host) continue;
        check_copy(t.address, host);
        if (log_until) {
            u32 at = copy_slot(t.address);
            int copied = copies[at].target == t.address;
            if (logging() || (copied && !copies[at].bound))
                fprintf(report, "bind frame %u map %u %08X %ux%u format %02X copied %d\n", frames, map, t.address,
                        t.width, t.height, t.format, copied);
            if (copied) copies[at].bound = 1;
        }
        u32 slot = t.tlut_offset >> 9;
            if (t.format == GX_TF_C4 || t.format == GX_TF_C8 || t.format == GX_TF_C14X2) {
            const void* palette = plain_memory(tlut_loads[slot & 0x3FFu].source, 32);
            if (palette) glue_load_tlut(slot % TLUT_SLOTS, palette, t.tlut_format, tlut_loads[slot & 0x3FFu].entries);
        }
        u32 mode1 = s->bp[(map < 4 ? 0x84u : 0xA4u) + (map & 3u)];
        /* The frame number makes aurora look at the data again, in case it changed in place. */
        glue_load_texture(map, host, t.width, t.height, t.format, slot % TLUT_SLOTS, ((mode1 >> 8) & 0xFFu) != 0, frames);
    }
}

static float xf_value(const GxState* s, u32 at) {
    float f;
    memcpy(&f, &s->xf[at], 4);
    return f;
}

/* With --copy-log, each draw in those frames too: where its first vertex lands (normalised device
   coordinates and w), and the state that decides whether it shows. For finding draws that go missing. */
static void log_draw(const GxState* s, const GxDraw* d) {
    float pos[3] = {0, 0, 0}, v[3] = {0, 0, 0}, x = 0, y = 0, z = 0, w = 0;
    u32 matrix = 0;
    int have = gx_vertex_position(d, 0, pos, &matrix);
    if (have) {
        for (u32 row = 0; row < 3; row++) {
            u32 m = matrix * 4u + row * 4u;
            v[row] = xf_value(s, m) * pos[0] + xf_value(s, m + 1) * pos[1] + xf_value(s, m + 2) * pos[2] +
                     xf_value(s, m + 3);
        }
        float a = xf_value(s, 0x1020), b = xf_value(s, 0x1021), c = xf_value(s, 0x1022), e = xf_value(s, 0x1023);
        float f = xf_value(s, 0x1024), g = xf_value(s, 0x1025);
        if (s->xf[0x1026] == 0) {
            x = a * v[0] + b * v[2];
            y = c * v[1] + e * v[2];
            z = f * v[2] + g;
            w = -v[2];
        } else {
            x = a * v[0] + b;
            y = c * v[1] + e;
            z = f * v[2] + g;
            w = 1.0f;
        }
        if (w != 0) {
            x /= w;
            y /= w;
            z /= w;
        }
    }
    GxTexture t = {0};
    int textured = gx_texture_info(s, 0, &t);
    fprintf(report,
            "draw frame %u prim %u n %u vat %u list %d mtx %u at %.2f %.2f %.2f ndc %.3f %.3f %.3f w %.2f pos %08X "
            "desc %06X %06X t0 %08X %ux%u f%u z %06X bl %06X ac %06X gen %06X cull %u mat %08X %08X amb %08X "
            "chan %08X %08X tev %u kc %06X %06X\n",
            frames, (u32)d->primitive, d->count, d->format, d->from_list, matrix, pos[0], pos[1], pos[2], x, y, z, w,
            array_base[0], s->cp[0x50], s->cp[0x60], textured ? t.address : 0u, t.width, t.height, t.format,
            s->bp[BP_Z_MODE], s->bp[BP_BLEND], s->bp[0xF3], s->bp[BP_GEN_MODE], cull_mode(s->bp[BP_GEN_MODE]),
            s->xf[0x100C], s->xf[0x100D], s->xf[0x100A], s->xf[0x100E], s->xf[0x1010],
            ((s->bp[BP_GEN_MODE] >> 10) & 15u) + 1u, s->bp[0xE0] & 0xFFFFFFu, s->bp[0xE1] & 0xFFFFFFu);
}

void gx_adapter_draw(const GxState* s, const GxDraw* d, const u8* p, u32 size) {
    if (!active) return;
    const GxLayout* l = d->layout;
    /* With everything culled, the hardware drops triangles but still draws lines and points. */
    if (log_until && logging()) log_draw(s, d);
    if (cull_mode(s->bp[BP_GEN_MODE]) == CULL_ALL && d->primitive < GX_LINES) return;
    bind_textures(s);
    u32 tex0 = (s->bp[0x94] & 0xFFFFFFu) << 5, tex1 = (s->bp[0x95] & 0xFFFFFFu) << 5;
    if (group.count && (group.zmode != s->bp[BP_Z_MODE] || group.ztex != s->bp[BP_Z_TEXTURE] ||
                        group.blend != s->bp[BP_BLEND] || group.alpha != s->bp[0xF3] || group.tex0 != tex0 ||
                        group.tex1 != tex1))
        group_flush();
    group.zmode = s->bp[BP_Z_MODE];
    group.ztex = s->bp[BP_Z_TEXTURE];
    group.blend = s->bp[BP_BLEND];
    group.alpha = s->bp[0xF3];
    group.tex0 = tex0;
    group.tex1 = tex1;
    group.zbias = s->bp[0xF4];
    group.count++;
    scan_attribute(s, d, l->position, l->position_offset, 0, 12);
    scan_attribute(s, d, l->normal, l->normal_offset, 1, 36);
    for (u32 c = 0; c < 2; c++) scan_attribute(s, d, l->color[c], l->color_offset[c], 2 + c, 4);
    for (u32 t = 0; t < 8; t++) scan_attribute(s, d, l->tex[t], l->tex_offset[t], 4 + t, 8);
    glue_write(p, size);
    glue_draw_done();
}

/* The rolling record of the last frames, saved when the program closes too. */
void gx_adapter_save_frames(const char* path) {
    group_flush();
    ring_save(path);
}

void gx_adapter_write_report(void) {
    fprintf(report, "copies evicted %llu unknown formats %u unknown logic ops %u shots %llu\n",
            (unsigned long long)copies_evicted, unknown_copy_formats, unknown_logic_ops,
            (unsigned long long)shots_saved);
}

/* Called after a display copy. Returns 0 if the window has been closed. */
/* Where each frame's time goes, in the run report. */
static unsigned long long tick_now(void) { return __builtin_ia32_rdtsc(); }
static unsigned long long frame_start, spent_present, spent_stream, stream_begun;
/* Reading the clock on every write cost about 3% of a frame, so only some frames are measured. */
#define STREAM_TIMED_EVERY 8u
static int stream_timed; /* a write can finish in a later frame than it began */
void gx_adapter_stream_begin(void) {
    stream_timed = frames % STREAM_TIMED_EVERY == 0;
    if (stream_timed) stream_begun = tick_now();
}
void gx_adapter_stream_end(void) {
    if (stream_timed) spent_stream += (tick_now() - stream_begun) * STREAM_TIMED_EVERY;
}

/* Game work that overran a frame, for finding short drops that averages hide. Loads are left out. */
#define SLOW_FRAME_MS 20.0
#define MAX_SLOW_LOG 3000u
static unsigned long long last_frame_end;
static u32 slow_logged;

int gx_adapter_frame(void) {
    if (!active) return 1;
    if (++frames % 60 == 0) {
        fprintf(report, "frame %u\n", frames);
        fflush(report);
    }
    unsigned long long before = tick_now();
    if (idle_this_frame && last_frame_end && slow_logged < MAX_SLOW_LOG) {
        double ms = (before - last_frame_end) / 4.4e6;
        if (ms > SLOW_FRAME_MS) {
            fprintf(report, "slow frame %u: %.1f ms\n", frames, ms);
            slow_logged++;
        }
    }
    /* A frame with no idle time means the game is busy, usually loading. It runs flat out then,
       because pacing would only hold the load back. */
    if (idle_this_frame) {
        if (busy_frames >= 30) /* a load has just finished */
            fprintf(report, "busy spell: %u frames in %.1f seconds, ending at frame %u\n", busy_frames,
                    (tick_now() - busy_started) / 4.4e9, frames);
        busy_frames = 0;
    } else if (busy_frames++ == 0) {
        busy_started = tick_now();
    }
    int running = glue_next_frame(idle_this_frame);
    idle_this_frame = 0;
    last_frame_end = tick_now();
    spent_present += last_frame_end - before;
    if (frames % 240 == 0) {
        unsigned long long total = tick_now() - frame_start;
        if (frame_start) {
            if (gx_thread_on) {
                /* The graphics stream is on its own thread: "other" is time it waited for the CPU, and the
                   CPU thread's own wait for graphics is its spare time. */
                u64 waited, waits;
                gx_thread_waits(&waited, &waits);
                fprintf(report, "time frame %u: stream %.1f%% present %.1f%% other %.1f%%, %.1f ms a frame, "
                                "CPU waited %.2f ms a frame\n",
                        frames, 100.0 * spent_stream / total, 100.0 * spent_present / total,
                        100.0 * ((double)total - spent_stream - spent_present) / total, total / 240.0 / 4.4e6,
                        waited / 240.0 / 4.4e6);
            } else {
                fprintf(report, "time frame %u: stream %.1f%% present %.1f%% game %.1f%%, %.1f ms a frame\n", frames,
                        100.0 * spent_stream / total, 100.0 * spent_present / total,
                        100.0 * ((double)total - spent_stream - spent_present) / total, total / 240.0 / 4.4e6);
            }
        }
        frame_start = tick_now();
        spent_present = spent_stream = 0;
    }
    if (!quiet_mode) {
        GlueInput input;
        glue_read_input(&input);
        fake_remote_live(input.buttons, input.pointer_x, input.pointer_y, input.pointer_valid, input.pitch,
                         input.roll, input.swing);
        fake_remote_live_nunchuk(input.nunchuk_x, input.nunchuk_y, input.nunchuk_c, input.nunchuk_z,
                                 input.nunchuk_swing, input.nunchuk_plug);
    }
    return running;
}
