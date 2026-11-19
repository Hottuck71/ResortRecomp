/* Decodes the graphics command stream. Packets are applied as soon as they are complete, so
   display lists and vertex arrays are read from memory at the moment the GP would use them. */
#include <stdlib.h>
#include <string.h>
#include "gx_stream.h"
#include "gx_thread.h"

#define CP_MATRIX_INDEX_A 0x30u
#define CP_VCD_LOW 0x50u
#define CP_VCD_HIGH 0x60u
#define CP_VAT_A 0x70u
#define CP_VAT_B 0x80u
#define CP_VAT_C 0x90u
#define CP_ARRAY_BASE 0xA0u
#define CP_ARRAY_STRIDE 0xB0u
#define BP_MASK 0xFEu
#define BP_COPY 0x52u
#define BP_DONE 0x45u
#define BP_TLUT_SOURCE 0x64u
#define BP_TLUT_LOAD 0x65u
#define BP_TOKEN 0x47u
#define BP_TOKEN_RAISE 0x48u
#define COPY_TO_DISPLAY (1u << 14)
#define MAX_ERRORS 8

typedef struct {
    u64 nop, bp, cp, xf, xf_indexed, draw, call, invalidate;
    u64 vertices, primitives[8], display_copies, texture_copies, unknown;
    u64 bad_positions;
} Counts;

static GxState state = {.bp_mask = 0xFFFFFFu};
static Counts live, lists;
static u8* buffer;
static u32 buffer_size, buffer_used, buffer_need = 1;
static int in_list;
static u8 tlut_memory[0x400u << 9];
static u32 errors_logged;

const GxState* gx_state(void) { return &state; }
const u8* gx_tlut_memory(void) { return tlut_memory; }

static const u8 component_size[8] = {1, 1, 2, 2, 4, 4, 4, 4};
static const u8 color_size[8] = {2, 3, 4, 2, 3, 4, 4, 4};

/* Bit positions of each texture coordinate's fields in VAT registers A, B, C. */
static const struct { u8 reg, count, format, shift; } tex_bits[8] = {
    {0, 21, 22, 25}, {1, 0, 1, 4}, {1, 9, 10, 13}, {1, 18, 19, 22},
    {1, 27, 28, 0xFF}, {2, 5, 6, 9}, {2, 14, 15, 18}, {2, 23, 24, 27},
};

static void make_layout(u32 format, GxLayout* l) {
    u32 low = state.cp[CP_VCD_LOW], high = state.cp[CP_VCD_HIGH];
    u32 vat[3] = {state.cp[CP_VAT_A + format], state.cp[CP_VAT_B + format],
                  state.cp[CP_VAT_C + format]};
    u32 a = vat[0], at = 0;
    memset(l, 0, sizeof *l);
    l->has_position_index = low & 1;
    for (u32 bit = 0; bit < 9; bit++) {
        if (bit && ((low >> bit) & 1)) {
            l->has_tex_index[bit - 1] = 1;
            l->tex_index_offset[bit - 1] = l->matrix_index_count;
        }
        l->matrix_index_count += (low >> bit) & 1;
    }
    at = l->matrix_index_count;

    l->position = (low >> 9) & 3;
    l->position_elements = (a & 1) ? 3 : 2;
    l->position_format = (a >> 1) & 7;
    l->position_shift = (a >> 4) & 31;
    l->position_offset = (u8)at;
    if (l->position == 1) at += l->position_elements * component_size[l->position_format];
    else if (l->position) at += l->position - 1;

    l->normal = (low >> 11) & 3;
    l->normal_elements = ((a >> 9) & 1) ? 9 : 3;
    l->normal_format = (a >> 10) & 7;
    l->normal_three_indices = l->normal_elements == 9 && (a >> 31);
    l->normal_offset = (u8)at;
    if (l->normal == 1) at += l->normal_elements * component_size[l->normal_format];
    else if (l->normal) at += (l->normal - 1) * (l->normal_three_indices ? 3 : 1);

    for (u32 c = 0; c < 2; c++) {
        l->color[c] = (low >> (13 + c * 2)) & 3;
        l->color_format[c] = (a >> (14 + c * 4)) & 7;
        l->color_offset[c] = (u8)at;
        if (l->color[c] == 1) at += color_size[l->color_format[c]];
        else if (l->color[c]) at += l->color[c] - 1;
    }
    for (u32 t = 0; t < 8; t++) {
        u32 v = vat[tex_bits[t].reg];
        l->tex[t] = (high >> (t * 2)) & 3;
        l->tex_elements[t] = ((v >> tex_bits[t].count) & 1) ? 2 : 1;
        l->tex_format[t] = (v >> tex_bits[t].format) & 7;
        /* Coordinate 4's shift is the one field that sits in the next register. */
        l->tex_shift[t] = t == 4 ? vat[2] & 31 : (v >> tex_bits[t].shift) & 31;
        l->tex_offset[t] = (u8)at;
        if (l->tex[t] == 1) at += l->tex_elements[t] * component_size[l->tex_format[t]];
        else if (l->tex[t]) at += l->tex[t] - 1;
    }
    l->size = (u8)at;
}

static float read_component(const u8* p, u32 format, u32 shift) {
    float scale = 1.0f / (float)(1u << shift);
    switch (format) {
    case 0: return p[0] * scale;
    case 1: return (s8)p[0] * scale;
    case 2: return (u16)load_be(p, 2) * scale;
    case 3: return (s16)load_be(p, 2) * scale;
    default: {
        u32 bits = (u32)load_be(p, 4);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }
    }
}

int gx_vertex_position(const GxDraw* d, u32 vertex, float out[3], u32* matrix) {
    const GxLayout* l = d->layout;
    const u8* v = d->vertices + (size_t)vertex * l->size;
    const u8* p = v + l->position_offset;
    u32 step = component_size[l->position_format];
    *matrix = l->has_position_index ? v[0] : state.cp[CP_MATRIX_INDEX_A] & 63u;
    if (l->position >= 2) {
        u32 index = l->position == 2 ? p[0] : (u32)load_be(p, 2);
        u32 stride = state.cp[CP_ARRAY_STRIDE] & 0xFFu;
        p = plain_memory((state.cp[CP_ARRAY_BASE] + index * stride) & 0x3FFFFFFFu,
                         l->position_elements * step);
        if (!p) return 0;
    } else if (!l->position) {
        return 0;
    }
    out[2] = 0;
    for (u32 i = 0; i < l->position_elements; i++)
        out[i] = read_component(p + i * step, l->position_format, l->position_shift);
    return 1;
}

/* Points at an attribute's data: in the vertex if direct, or in its array if indexed. */
static const u8* attribute(const u8* field, u32 kind, u32 array, u32 size) {
    if (kind == 1) return field;
    if (!kind) return NULL;
    u32 index = kind == 2 ? field[0] : (u32)load_be(field, 2);
    u32 stride = state.cp[CP_ARRAY_STRIDE + array] & 0xFFu;
    return plain_memory((state.cp[CP_ARRAY_BASE + array] + index * stride) & 0x3FFFFFFFu, size);
}

int gx_vertex_normal(const GxDraw* d, u32 vertex, float out[9]) {
    const GxLayout* l = d->layout;
    const u8* field = d->vertices + (size_t)vertex * l->size + l->normal_offset;
    u32 step = component_size[l->normal_format], format = l->normal_format;
    u32 shift = step == 1 ? 6 : step == 2 ? 14 : 0; /* normals have a fixed scale */
    u32 elements = l->normal_elements;
    for (u32 part = 0; part < elements; part += 3) {
        const u8* source;
        if (l->normal_three_indices) {
            source = attribute(field + (part / 3) * (l->normal - 1), l->normal, 1, 3 * step);
        } else {
            source = attribute(field, l->normal, 1, elements * step);
            if (source) source += part * step;
        }
        if (!source) return 0;
        for (u32 i = 0; i < 3; i++) out[part + i] = read_component(source + i * step, format, shift);
    }
    return (int)elements;
}

int gx_vertex_color(const GxDraw* d, u32 vertex, u32 channel, u8 rgba[4]) {
    const GxLayout* l = d->layout;
    u32 format = l->color_format[channel];
    const u8* p = attribute(d->vertices + (size_t)vertex * l->size + l->color_offset[channel],
                            l->color[channel], 2 + channel, color_size[format]);
    if (!p) return 0;
    u32 v;
    switch (format) {
    case 0: /* RGB565 */
        v = (u32)load_be(p, 2);
        rgba[0] = (u8)(((v >> 11) & 31) * 255 / 31);
        rgba[1] = (u8)(((v >> 5) & 63) * 255 / 63);
        rgba[2] = (u8)((v & 31) * 255 / 31);
        rgba[3] = 255;
        break;
    case 1: /* RGB8 */
    case 2: /* RGBX8 */
        memcpy(rgba, p, 3);
        rgba[3] = 255;
        break;
    case 3: /* RGBA4 */
        v = (u32)load_be(p, 2);
        for (u32 i = 0; i < 4; i++) rgba[i] = (u8)(((v >> (12 - i * 4)) & 15) * 17);
        break;
    case 4: /* RGBA6 */
        v = (u32)load_be(p, 3);
        for (u32 i = 0; i < 4; i++) rgba[i] = (u8)(((v >> (18 - i * 6)) & 63) * 255 / 63);
        break;
    default: /* RGBA8 */
        memcpy(rgba, p, 4);
        break;
    }
    return 1;
}

int gx_vertex_tex(const GxDraw* d, u32 vertex, u32 unit, float out[2]) {
    const GxLayout* l = d->layout;
    u32 step = component_size[l->tex_format[unit]], elements = l->tex_elements[unit];
    const u8* p = attribute(d->vertices + (size_t)vertex * l->size + l->tex_offset[unit],
                            l->tex[unit], 4 + unit, elements * step);
    if (!p) return 0;
    out[1] = 0;
    for (u32 i = 0; i < elements; i++)
        out[i] = read_component(p + i * step, l->tex_format[unit], l->tex_shift[unit]);
    return 1;
}

static void note_unknown(const u8* p, u32 available) {
    (in_list ? &lists : &live)->unknown++;
    if (errors_logged++ < MAX_ERRORS) {
        fprintf(report, "GX: unknown byte %02X%s, next", p[0], in_list ? " in a display list" : "");
        for (u32 i = 1; i < available && i < 12; i++) fprintf(report, " %02X", p[i]);
        fprintf(report, "\n");
    }
}

static void apply_bp(u32 word) {
    u32 reg = word >> 24, value = word & 0xFFFFFFu;
    if (reg == BP_MASK) {
        state.bp_mask = value;
        state.bp[reg] = value;
        return;
    }
    state.bp[reg] = (state.bp[reg] & ~state.bp_mask) | (value & state.bp_mask);
    state.bp_mask = 0xFFFFFFu;
    gx_adapter_bp(&state, reg, value);
    Counts* c = in_list ? &lists : &live;
    switch (reg) {
    case BP_COPY:
        if (value & COPY_TO_DISPLAY) {
            c->display_copies++;
            gx_backend_display_copy(&state);
            if (!gx_adapter_frame()) host_stop("window closed");
        } else {
            c->texture_copies++;
            gx_backend_texture_copy(&state);
        }
        break;
    case BP_DONE:
        host_gx_finish();
        break;
    case BP_TLUT_LOAD: { /* offset in 512-byte units, then the size in 16-entry units */
        u32 offset = (value & 0x3FFu) << 9, size = ((value >> 10) & 0x7FFu) * 32u;
        const u8* source = plain_memory((state.bp[BP_TLUT_SOURCE] & 0xFFFFFFu) << 5, size);
        if (source && offset + size <= sizeof tlut_memory) memcpy(tlut_memory + offset, source, size);
        break;
    }
    case BP_TOKEN:
        host_gx_token(value & 0xFFFFu, 0);
        break;
    case BP_TOKEN_RAISE:
        host_gx_token(value & 0xFFFFu, 1);
        break;
    default:
        break;
    }
}

static void store_xf(u32 addr, u32 value) {
    if (addr < GX_XF_SIZE) state.xf[addr] = value;
    /* A NaN in a position matrix makes whatever is drawn with it vanish. */
    if (addr < 0x100 && (value & 0x7F800000u) == 0x7F800000u && (value & 0x007FFFFFu)) host_gx_nan(addr, value);
}

static void run_list(u32 addr, u32 size);

/* Applies the packet at p, or returns 0 and sets *need if more bytes must arrive first. */
static u32 apply_packet(const u8* p, u32 available, u32* need) {
    Counts* c = in_list ? &lists : &live;
    u32 op = p[0];
    switch (op) {
    case 0x00:
        c->nop++;
        return 1;
    case 0x48:
        c->invalidate++;
        gx_adapter_raw(p, 1);
        return 1;
    case 0x61:
        if (available < 5) break;
        c->bp++;
        apply_bp((u32)load_be(p + 1, 4));
        return 5;
    case 0x08:
        if (available < 6) break;
        c->cp++;
        state.cp[p[1]] = (u32)load_be(p + 2, 4);
        gx_adapter_cp(p[1], state.cp[p[1]]);
        return 6;
    case 0x10: {
        if (available < 5) {
            *need = 5;
            return 0;
        }
        u32 head = (u32)load_be(p + 1, 4), count = (head >> 16) + 1, addr = head & 0xFFFFu;
        if (available < 5 + count * 4) {
            *need = 5 + count * 4;
            return 0;
        }
        c->xf++;
        for (u32 i = 0; i < count; i++) store_xf(addr + i, (u32)load_be(p + 5 + i * 4, 4));
        gx_adapter_xf(p, 5 + count * 4);
        return 5 + count * 4;
    }
    case 0x20:
    case 0x28:
    case 0x30:
    case 0x38: {
        if (available < 5) break;
        u32 word = (u32)load_be(p + 1, 4), array = 12 + ((op - 0x20) >> 3);
        u32 index = word >> 16, count = ((word >> 12) & 15) + 1, addr = word & 0xFFFu;
        u32 stride = state.cp[CP_ARRAY_STRIDE + array] & 0xFFu;
        const u8* source = plain_memory((state.cp[CP_ARRAY_BASE + array] + index * stride) & 0x3FFFFFFFu,
                                        count * 4);
        c->xf_indexed++;
        for (u32 i = 0; source && i < count; i++) store_xf(addr + i, (u32)load_be(source + i * 4, 4));
        if (source) gx_adapter_xf_values(addr, source, count);
        return 5;
    }
    case 0x40:
        if (available < 9) break;
        c->call++;
        run_list((u32)load_be(p + 1, 4), (u32)load_be(p + 5, 4));
        return 9;
    default:
        if (op >= 0x80 && op < 0xC0) {
            GxLayout layout;
            GxDraw draw;
            if (available < 3) {
                *need = 3;
                return 0;
            }
            make_layout(op & 7, &layout);
            draw.count = (u32)load_be(p + 1, 2);
            if (available < 3 + draw.count * layout.size) {
                *need = 3 + draw.count * layout.size;
                return 0;
            }
            draw.primitive = (GxPrimitive)((op >> 3) & 7);
            draw.format = op & 7;
            draw.vertices = p + 3;
            draw.layout = &layout;
            draw.from_list = in_list;
            c->draw++;
            c->primitives[draw.primitive]++;
            c->vertices += draw.count;
            if (gx_adapter_active()) gx_adapter_draw(&state, &draw, p, 3 + draw.count * layout.size);
            else gx_backend_draw(&state, &draw); /* the checks and preview are for headless runs */
            return 3 + draw.count * layout.size;
        }
        note_unknown(p, available);
        return 1;
    }
    *need = 9; /* a fixed-size packet that hasn't fully arrived */
    if (op == 0x61 || (op >= 0x20 && op <= 0x38)) *need = 5;
    if (op == 0x08) *need = 6;
    return 0;
}

static void run_list(u32 addr, u32 size) {
    const u8* p = plain_memory(addr & 0x3FFFFFFFu, size);
    if (in_list || !p) {
        fprintf(report, "GX: display list %08X size %u skipped (%s)\n", addr, size,
                in_list ? "nested" : "not in memory");
        return;
    }
    in_list = 1;
    for (u32 at = 0, need; at < size;) {
        u32 used = apply_packet(p + at, size - at, &need);
        if (!used) {
            fprintf(report, "GX: display list %08X ends inside a packet\n", addr);
            break;
        }
        at += used;
    }
    in_list = 0;
}

static void pump(void) {
    u32 at = 0;
    buffer_need = 1;
    while (at < buffer_used) {
        u32 used = apply_packet(buffer + at, buffer_used - at, &buffer_need);
        if (!used) break;
        at += used;
        buffer_need = 1;
    }
    if (at < buffer_used) memmove(buffer, buffer + at, buffer_used - at);
    buffer_used -= at;
}

static void make_room(u32 size) {
    if (buffer_used + size <= buffer_size) return;
    while (buffer_used + size > buffer_size) buffer_size = buffer_size ? buffer_size * 2 : 1u << 20;
    buffer = realloc(buffer, buffer_size);
    if (!buffer) {
        fprintf(stderr, "Out of memory for the graphics stream\n");
        exit(1);
    }
}

void gx_stream_write(u64 value, u32 size) {
    u8 bytes[8];
    u8* out = bytes;
    if (!gx_thread_on) {
        make_room(size);
        out = buffer + buffer_used;
    }
    if (size == 4) {
        u32 v = __builtin_bswap32((u32)value);
        memcpy(out, &v, 4);
    } else if (size == 1) {
        *out = (u8)value;
    } else {
        store_be(out, value, size);
    }
    if (gx_thread_on) { /* the graphics thread decodes it: gx_thread.c */
        gx_thread_put(bytes, size);
        return;
    }
    buffer_used += size;
    if (buffer_used >= buffer_need) {
        gx_adapter_stream_begin();
        pump();
        gx_adapter_stream_end();
    }
}

/* On the graphics thread: bytes from the ring, decoded as they would have been on the CPU thread. */
void gx_stream_consume(const u8* bytes, u32 size) {
    make_room(size);
    memcpy(buffer + buffer_used, bytes, size);
    buffer_used += size;
    if (buffer_used >= buffer_need) {
        gx_adapter_stream_begin();
        pump();
        gx_adapter_stream_end();
    }
}

static void write_counts(const char* name, const Counts* c) {
    fprintf(report, "gx %s bp %llu\ngx %s cp %llu\ngx %s xf %llu\ngx %s draw %llu\n", name,
            (unsigned long long)c->bp, name, (unsigned long long)c->cp, name,
            (unsigned long long)c->xf, name, (unsigned long long)c->draw);
    fprintf(report, "gx %s nop %llu\ngx %s invalidate %llu\ngx %s call %llu\ngx %s vertices %llu\n",
            name, (unsigned long long)c->nop, name, (unsigned long long)c->invalidate, name,
            (unsigned long long)c->call, name, (unsigned long long)c->vertices);
    fprintf(report, "gx %s display_copies %llu\ngx %s texture_copies %llu\ngx %s unknown %llu\n",
            name, (unsigned long long)c->display_copies, name,
            (unsigned long long)c->texture_copies, name, (unsigned long long)c->unknown);
}

void gx_stream_write_report(void) {
    fprintf(report, "\n[gx] stream counts\n");
    write_counts("live", &live);
    write_counts("lists", &lists);
    fprintf(report, "gx waiting_bytes %u\n", buffer_used);
    fprintf(report, "gx viewport");
    for (u32 i = 0; i < 6; i++) {
        float f;
        memcpy(&f, &state.xf[0x101A + i], 4);
        fprintf(report, " %g", f);
    }
    fprintf(report, "\ngx projection");
    for (u32 i = 0; i < 7; i++) {
        float f;
        memcpy(&f, &state.xf[0x1020 + i], 4);
        fprintf(report, " %g", f);
    }
    fprintf(report, "\ngx bp genmode %06X scis %06X %06X box %06X zmode %06X blend %06X dstalpha %06X pe %06X alpha %06X\n",
            state.bp[0], state.bp[0x20], state.bp[0x21], state.bp[0x59], state.bp[0x40], state.bp[0x41],
            state.bp[0x42], state.bp[0x43], state.bp[0xF3]);
    fprintf(report, "gx xf chans %X texgens %X mtx %X %X cp mtx %X %X vcd %X %X\n", state.xf[0x1009], state.xf[0x103F],
            state.xf[0x1018], state.xf[0x1019], state.cp[0x30], state.cp[0x40], state.cp[0x50], state.cp[0x60]);
}
