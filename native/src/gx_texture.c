/* Texture state and pixel formats, from the public format descriptions (YAGCD, wiibrew.org). */
#include <string.h>
#include "gx_stream.h"

/* First BP register of each group, for texture maps 0-3 and 4-7. */
static const u8 mode0_reg[2] = {0x80, 0xA0}, image0_reg[2] = {0x88, 0xA8},
                image3_reg[2] = {0x94, 0xB4}, tlut_reg[2] = {0x98, 0xB8};

static const struct { u8 w, h, bits; } block[16] = {
    [GX_TF_I4] = {8, 8, 4},     [GX_TF_I8] = {8, 4, 8},      [GX_TF_IA4] = {8, 4, 8},
    [GX_TF_IA8] = {4, 4, 16},   [GX_TF_RGB565] = {4, 4, 16}, [GX_TF_RGB5A3] = {4, 4, 16},
    [GX_TF_RGBA8] = {4, 4, 32}, [GX_TF_C4] = {8, 8, 4},      [GX_TF_C8] = {8, 4, 8},
    [GX_TF_C14X2] = {4, 4, 16}, [GX_TF_CMPR] = {8, 8, 4},
};

int gx_texture_info(const GxState* s, u32 map, GxTexture* t) {
    u32 half = map >> 2, n = map & 3;
    u32 image0 = s->bp[image0_reg[half] + n], mode0 = s->bp[mode0_reg[half] + n];
    u32 tlut = s->bp[tlut_reg[half] + n];
    t->width = (image0 & 0x3FFu) + 1;
    t->height = ((image0 >> 10) & 0x3FFu) + 1;
    t->format = (image0 >> 20) & 15;
    t->address = (s->bp[image3_reg[half] + n] & 0xFFFFFFu) << 5;
    t->wrap_s = mode0 & 3;
    t->wrap_t = (mode0 >> 2) & 3;
    t->tlut_offset = (tlut & 0x3FFu) << 9;
    t->tlut_format = (tlut >> 10) & 3;
    return block[t->format].w != 0;
}

u32 gx_texture_size(const GxTexture* t) {
    u32 bw = block[t->format].w, bh = block[t->format].h;
    u32 across = (t->width + bw - 1) / bw, down = (t->height + bh - 1) / bh;
    return across * down * bw * bh * block[t->format].bits / 8u;
}

static void put(u8* out, u32 w, u32 h, u32 x, u32 y, u32 r, u32 g, u32 b, u32 a) {
    if (x >= w || y >= h) return;
    u8* p = out + ((size_t)y * w + x) * 4u;
    p[0] = (u8)r;
    p[1] = (u8)g;
    p[2] = (u8)b;
    p[3] = (u8)a;
}

static void rgb565(u32 v, u32 c[4]) {
    u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    c[0] = (r << 3) | (r >> 2);
    c[1] = (g << 2) | (g >> 4);
    c[2] = (b << 3) | (b >> 2);
    c[3] = 255;
}

static void rgb5a3(u32 v, u32 c[4]) {
    if (v & 0x8000u) {
        u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        c[0] = (r << 3) | (r >> 2);
        c[1] = (g << 3) | (g >> 2);
        c[2] = (b << 3) | (b >> 2);
        c[3] = 255;
    } else {
        c[0] = ((v >> 8) & 15) * 17;
        c[1] = ((v >> 4) & 15) * 17;
        c[2] = (v & 15) * 17;
        c[3] = ((v >> 12) & 7) * 255 / 7;
    }
}

static void palette(const GxTexture* t, const u8* tlut, u32 index, u32 c[4]) {
    u32 v = (u32)load_be(tlut + t->tlut_offset + index * 2u, 2);
    if (t->tlut_format == 1) rgb565(v, c);
    else if (t->tlut_format == 2) rgb5a3(v, c);
    else c[0] = c[1] = c[2] = v & 255, c[3] = v >> 8;
}

/* A CMPR tile is four 4x4 blocks, each two 16-bit colors and sixteen 2-bit picks. */
static void cmpr_block(const u8* p, u8* out, u32 w, u32 h, u32 x0, u32 y0) {
    u32 v0 = (u32)load_be(p, 2), v1 = (u32)load_be(p + 2, 2), c[4][4];
    rgb565(v0, c[0]);
    rgb565(v1, c[1]);
    for (u32 i = 0; i < 3; i++) {
        if (v0 > v1) {
            c[2][i] = (2 * c[0][i] + c[1][i]) / 3;
            c[3][i] = (c[0][i] + 2 * c[1][i]) / 3;
        } else {
            c[2][i] = (c[0][i] + c[1][i]) / 2;
            c[3][i] = c[2][i];
        }
    }
    c[2][3] = 255;
    c[3][3] = v0 > v1 ? 255 : 0;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            u32 pick = (p[4 + y] >> (6 - x * 2)) & 3;
            put(out, w, h, x0 + x, y0 + y, c[pick][0], c[pick][1], c[pick][2], c[pick][3]);
        }
}

int gx_texture_decode(const GxTexture* t, const u8* tlut, u8* out) {
    u32 size = gx_texture_size(t), w = t->width, h = t->height, f = t->format;
    u32 bw = block[f].w, bh = block[f].h, c[4];
    const u8* p = plain_memory(t->address, size);
    if (!p) return 0;
    for (u32 by = 0; by < h; by += bh)
        for (u32 bx = 0; bx < w; bx += bw) {
            if (f == GX_TF_CMPR) {
                for (u32 i = 0; i < 4; i++, p += 8)
                    cmpr_block(p, out, w, h, bx + (i & 1) * 4, by + (i >> 1) * 4);
                continue;
            }
            if (f == GX_TF_RGBA8) { /* alpha and red first, then green and blue */
                for (u32 i = 0; i < 16; i++)
                    put(out, w, h, bx + (i & 3), by + (i >> 2), p[i * 2 + 1], p[32 + i * 2],
                        p[32 + i * 2 + 1], p[i * 2]);
                p += 64;
                continue;
            }
            for (u32 y = 0; y < bh; y++)
                for (u32 x = 0; x < bw; x++) {
                    u32 px = bx + x, py = by + y, v;
                    switch (f) {
                    case GX_TF_I4:
                        v = ((x & 1) ? p[0] & 15 : p[0] >> 4) * 17;
                        put(out, w, h, px, py, v, v, v, v);
                        if (x & 1) p++;
                        break;
                    case GX_TF_I8:
                        put(out, w, h, px, py, p[0], p[0], p[0], p[0]);
                        p++;
                        break;
                    case GX_TF_IA4:
                        v = (p[0] & 15) * 17;
                        put(out, w, h, px, py, v, v, v, (p[0] >> 4) * 17);
                        p++;
                        break;
                    case GX_TF_IA8:
                        put(out, w, h, px, py, p[1], p[1], p[1], p[0]);
                        p += 2;
                        break;
                    case GX_TF_RGB565:
                        rgb565((u32)load_be(p, 2), c);
                        put(out, w, h, px, py, c[0], c[1], c[2], c[3]);
                        p += 2;
                        break;
                    case GX_TF_RGB5A3:
                        rgb5a3((u32)load_be(p, 2), c);
                        put(out, w, h, px, py, c[0], c[1], c[2], c[3]);
                        p += 2;
                        break;
                    case GX_TF_C4:
                        palette(t, tlut, (x & 1) ? p[0] & 15 : p[0] >> 4, c);
                        put(out, w, h, px, py, c[0], c[1], c[2], c[3]);
                        if (x & 1) p++;
                        break;
                    case GX_TF_C8:
                        palette(t, tlut, p[0], c);
                        put(out, w, h, px, py, c[0], c[1], c[2], c[3]);
                        p++;
                        break;
                    default: /* C14X2 */
                        palette(t, tlut, (u32)load_be(p, 2) & 0x3FFFu, c);
                        put(out, w, h, px, py, c[0], c[1], c[2], c[3]);
                        p += 2;
                        break;
                    }
                }
        }
    return 1;
}
