/* A stand-in backend for checking the decoder. It fills shapes with the first TEV stage's
   texture and the vertex color, so wrong decoding shows up as a wrong picture. This isn't the
   renderer: no TEV, depth, lighting, or filtering, and shapes are drawn in submission order. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "gx_stream.h"

#define WIDTH 640
#define HEIGHT 528
#define MAX_VERTICES 4096
#define XF_VIEWPORT 0x101Au
#define XF_PROJECTION 0x1020u
#define XF_MATERIAL0 0x100Cu
#define XF_TEXGEN 0x1040u
#define VIEWPORT_ORIGIN 342.0f
#define TEX_IDENTITY 60u
#define BP_GENMODE 0x00u
#define BP_BLEND 0x41u
#define BP_TEV_ORDER 0x28u
#define CP_MATRIX_INDEX_A 0x30u
#define CP_MATRIX_INDEX_B 0x40u

typedef struct {
    float x, y, s, t, c[4];
    int good;
} Point;

static u8 image[HEIGHT][WIDTH][3];
static u32 frame, every;
static u64 positions, positions_bad, colors, normals, normals_bad, coords, coords_bad;
static u64 textures_used, textures_failed, format_uses[16];
static const char* prefix;
static u8 texels[1024 * 1024 * 4];

void gx_debug_setup(const char* path_prefix, u32 every_frames) {
    prefix = path_prefix;
    every = every_frames;
}

static float xf_float(const GxState* s, u32 addr) {
    float f;
    memcpy(&f, &s->xf[addr], 4);
    return f;
}

static int to_screen(const GxState* s, const float p[3], u32 matrix, Point* out) {
    float v[3];
    for (u32 row = 0; row < 3; row++) {
        u32 m = matrix * 4u + row * 4u;
        v[row] = xf_float(s, m) * p[0] + xf_float(s, m + 1) * p[1] + xf_float(s, m + 2) * p[2] +
                 xf_float(s, m + 3);
    }
    float a = xf_float(s, XF_PROJECTION), b = xf_float(s, XF_PROJECTION + 1);
    float c = xf_float(s, XF_PROJECTION + 2), d = xf_float(s, XF_PROJECTION + 3);
    float x, y, w;
    if (s->xf[XF_PROJECTION + 6] == 0) { /* perspective */
        x = a * v[0] + b * v[2];
        y = c * v[1] + d * v[2];
        w = -v[2];
        if (w <= 0.0001f) return 0;
    } else {
        x = a * v[0] + b;
        y = c * v[1] + d;
        w = 1.0f;
    }
    out->x = x / w * xf_float(s, XF_VIEWPORT) + xf_float(s, XF_VIEWPORT + 3) - VIEWPORT_ORIGIN;
    out->y = y / w * xf_float(s, XF_VIEWPORT + 1) + xf_float(s, XF_VIEWPORT + 4) - VIEWPORT_ORIGIN;
    return isfinite(out->x) && isfinite(out->y);
}

static u32 tex_matrix(const GxState* s, const GxDraw* d, u32 vertex, u32 gen) {
    const GxLayout* l = d->layout;
    if (l->has_tex_index[gen])
        return d->vertices[(size_t)vertex * l->size + l->tex_index_offset[gen]];
    if (gen < 4) return (s->cp[CP_MATRIX_INDEX_A] >> (6 + gen * 6)) & 63u;
    return (s->cp[CP_MATRIX_INDEX_B] >> ((gen - 4) * 6)) & 63u;
}

/* Regular texgens only, which is what 2D layouts use. */
static int texgen(const GxState* s, const GxDraw* d, u32 vertex, u32 gen, float st[2]) {
    u32 info = s->xf[XF_TEXGEN + gen], source = (info >> 7) & 31u;
    float in[4] = {0, 0, 1, 1};
    u32 m;
    if ((info >> 4) & 7u) return 0;
    if (source >= 5) {
        if (!gx_vertex_tex(d, vertex, source - 5, in)) return 0;
        in[2] = 1;
    } else if (source == 0) {
        if (!gx_vertex_position(d, vertex, in, &m)) return 0;
    } else {
        return 0;
    }
    m = tex_matrix(s, d, vertex, gen);
    if (m == TEX_IDENTITY) {
        st[0] = in[0];
        st[1] = in[1];
        return 1;
    }
    float out[3] = {0, 0, 1};
    u32 rows = (info & 2u) ? 3 : 2;
    for (u32 r = 0; r < rows; r++) {
        u32 a = m * 4u + r * 4u;
        out[r] = xf_float(s, a) * in[0] + xf_float(s, a + 1) * in[1] +
                 xf_float(s, a + 2) * in[2] + xf_float(s, a + 3) * in[3];
    }
    st[0] = out[0] / (out[2] ? out[2] : 1);
    st[1] = out[1] / (out[2] ? out[2] : 1);
    return 1;
}

static int wrap(int v, u32 size, u32 mode) {
    if (mode == 1) return v & (int)(size - 1) & 0x3FF;
    if (mode == 2) {
        int period = (int)size * 2, m = ((v % period) + period) % period;
        return m < (int)size ? m : period - 1 - m;
    }
    return v < 0 ? 0 : v >= (int)size ? (int)size - 1 : v;
}

static void triangle(const Point* a, const Point* b, const Point* c, const GxTexture* t, int blend) {
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (!a->good || !b->good || !c->good || fabsf(area) < 0.001f) return;
    int x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= WIDTH) x1 = WIDTH - 1;
    if (y1 >= HEIGHT) y1 = HEIGHT - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float wb = ((px - a->x) * (c->y - a->y) - (py - a->y) * (c->x - a->x)) / area;
            float wc = ((b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x)) / area;
            float wa = 1 - wb - wc;
            if (wa < 0 || wb < 0 || wc < 0) continue;
            float color[4];
            for (int i = 0; i < 4; i++) color[i] = a->c[i] * wa + b->c[i] * wb + c->c[i] * wc;
            if (t) {
                float s = a->s * wa + b->s * wb + c->s * wc, u = a->t * wa + b->t * wb + c->t * wc;
                int tx = wrap((int)floorf(s * t->width), t->width, t->wrap_s);
                int ty = wrap((int)floorf(u * t->height), t->height, t->wrap_t);
                const u8* texel = texels + ((size_t)ty * t->width + tx) * 4u;
                for (int i = 0; i < 4; i++) color[i] *= texel[i] / 255.0f;
            }
            float alpha = blend ? color[3] : 1.0f;
            for (int i = 0; i < 3; i++)
                image[y][x][i] = (u8)fminf(255.0f, color[i] * 255.0f * alpha + image[y][x][i] * (1 - alpha));
        }
}

/* Reads every attribute of every vertex, so the checks cover all frames. */
static void check_vertex(const GxDraw* d, u32 i) {
    const GxLayout* l = d->layout;
    float n[9], st[2];
    u8 rgba[4];
    if (l->normal) {
        normals++;
        int count = gx_vertex_normal(d, i, n);
        int bad = !count;
        for (int k = 0; k < count; k++) bad |= !isfinite(n[k]) || fabsf(n[k]) > 1000.0f;
        normals_bad += (u64)bad;
    }
    for (u32 c = 0; c < 2; c++)
        if (l->color[c]) colors += (u64)gx_vertex_color(d, i, c, rgba);
    for (u32 t = 0; t < 8; t++)
        if (l->tex[t]) {
            coords++;
            coords_bad += (u64)(!gx_vertex_tex(d, i, t, st) || !isfinite(st[0]) || !isfinite(st[1]));
        }
}

void gx_backend_draw(const GxState* s, const GxDraw* d) {
    static Point points[MAX_VERTICES];
    int want = prefix && (frame + 1) % every == 0;
    u32 count = d->count < MAX_VERTICES ? d->count : MAX_VERTICES;
    u32 order = s->bp[BP_TEV_ORDER], enabled = (order >> 6) & 1u, map = order & 7u;
    u32 gen = (order >> 3) & 7u, gens = s->bp[BP_GENMODE] & 15u;
    GxTexture texture;
    const GxTexture* t = NULL;

    if (enabled && gx_texture_info(s, map, &texture)) {
        textures_used++;
        format_uses[texture.format]++;
        if (texture.width * texture.height > 1024u * 1024u ||
            !plain_memory(texture.address, gx_texture_size(&texture)))
            textures_failed++;
        else if (want && gx_texture_decode(&texture, gx_tlut_memory(), texels) && gen < gens)
            t = &texture;
    }
    u8 material[4];
    store_be(material, s->xf[XF_MATERIAL0], 4);
    for (u32 i = 0; i < count; i++) {
        float p[3];
        u32 matrix;
        positions++;
        points[i].good = 0;
        check_vertex(d, i);
        if (!gx_vertex_position(d, i, p, &matrix) || !isfinite(p[0]) || !isfinite(p[1]) ||
            !isfinite(p[2]) || fabsf(p[0]) > 1e7f || fabsf(p[1]) > 1e7f || fabsf(p[2]) > 1e7f) {
            positions_bad++;
            continue;
        }
        if (!want) continue;
        Point* out = &points[i];
        u8 rgba[4];
        float st[2] = {0, 0};
        out->good = to_screen(s, p, matrix, out);
        if (!gx_vertex_color(d, i, 0, rgba)) memcpy(rgba, material, 4);
        for (int k = 0; k < 4; k++) out->c[k] = rgba[k] / 255.0f;
        if (t) texgen(s, d, i, gen, st);
        out->s = st[0];
        out->t = st[1];
    }
    if (!want) return;
    int blend = s->bp[BP_BLEND] & 1u;
    if (d->primitive == GX_QUADS || d->primitive == GX_QUADS2) {
        for (u32 q = 0; q + 3 < count; q += 4) {
            triangle(&points[q], &points[q + 1], &points[q + 2], t, blend);
            triangle(&points[q], &points[q + 2], &points[q + 3], t, blend);
        }
    } else if (d->primitive == GX_TRIANGLES) {
        for (u32 q = 0; q + 2 < count; q += 3) triangle(&points[q], &points[q + 1], &points[q + 2], t, blend);
    } else if (d->primitive == GX_STRIP) {
        for (u32 q = 0; q + 2 < count; q++) triangle(&points[q], &points[q + 1], &points[q + 2], t, blend);
    } else if (d->primitive == GX_FAN) {
        for (u32 q = 1; q + 1 < count; q++) triangle(&points[0], &points[q], &points[q + 1], t, blend);
    }
}

void gx_backend_display_copy(const GxState* s) {
    (void)s;
    frame++;
    if (!prefix || frame % every) return;
    char path[1024];
    snprintf(path, sizeof path, "%s-%05u.ppm", prefix, frame);
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
        fwrite(image, 1, sizeof image, f);
        fclose(f);
    }
    memset(image, 0, sizeof image);
}

void gx_backend_texture_copy(const GxState* s) { (void)s; }

void gx_debug_write_report(void) {
    fprintf(report, "gx positions_checked %llu\ngx positions_bad %llu\n",
            (unsigned long long)positions, (unsigned long long)positions_bad);
    fprintf(report, "gx normals_checked %llu\ngx normals_bad %llu\n",
            (unsigned long long)normals, (unsigned long long)normals_bad);
    fprintf(report, "gx colors_read %llu\ngx coords_checked %llu\ngx coords_bad %llu\n",
            (unsigned long long)colors, (unsigned long long)coords, (unsigned long long)coords_bad);
    fprintf(report, "gx textures_used %llu\ngx textures_failed %llu\n",
            (unsigned long long)textures_used, (unsigned long long)textures_failed);
    for (int i = 0; i < 16; i++)
        if (format_uses[i]) fprintf(report, "gx texture_format_%d %llu\n", i, (unsigned long long)format_uses[i]);
}
