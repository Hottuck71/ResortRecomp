/* The graphics command stream, decoded with no renderer attached. Whichever renderer is used
   plugs in below gx_backend_*. Format: wiibrew.org/wiki/Hardware/GX and YAGCD. */
#ifndef GX_STREAM_H
#define GX_STREAM_H
#include "host.h"

#define GX_XF_SIZE 0x1100u
#define GX_XF_REG 0x1000u

typedef struct {
    u32 bp[256];
    u32 cp[256];
    u32 xf[GX_XF_SIZE]; /* matrices, lights, and at GX_XF_REG the registers */
    u32 bp_mask;        /* set by BP 0xFE, applies to the next BP write only */
} GxState;

/* 0 means the attribute is absent, 1 is direct, 2 is an 8-bit index, 3 a 16-bit index. */
typedef struct {
    u8 size;                 /* bytes per vertex */
    u8 matrix_index_count;   /* one byte each, at the start of the vertex */
    u8 has_position_index;   /* the first of those bytes is the position matrix index */
    u8 has_tex_index[8], tex_index_offset[8];
    u8 position, normal, color[2], tex[8];
    u8 position_offset, normal_offset, color_offset[2], tex_offset[8];
    u8 position_elements, position_format, position_shift;
    u8 normal_elements, normal_format, normal_three_indices;
    u8 color_format[2];
    u8 tex_elements[8], tex_format[8], tex_shift[8];
} GxLayout;

typedef enum {
    GX_QUADS, GX_QUADS2, GX_TRIANGLES, GX_STRIP, GX_FAN, GX_LINES, GX_LINE_STRIP, GX_POINTS
} GxPrimitive;

typedef struct {
    GxPrimitive primitive;
    u32 format; /* which VAT */
    u32 count;
    const u8* vertices;
    const GxLayout* layout;
    int from_list; /* inside a display list */
} GxDraw;

void gx_stream_write(u64 value, u32 size); /* bytes in the order the GP reads them */
const GxState* gx_state(void);

/* Each reads one attribute of one vertex, following its array if indexed, and returns 0 if the
   vertex doesn't have it or the array can't be read. *matrix gets the position matrix index in
   the GX numbering, so its first word in XF memory is *matrix * 4. */
int gx_vertex_position(const GxDraw* draw, u32 vertex, float out[3], u32* matrix);
int gx_vertex_normal(const GxDraw* draw, u32 vertex, float out[9]); /* returns 3 or 9 */
int gx_vertex_color(const GxDraw* draw, u32 vertex, u32 channel, u8 rgba[4]);
int gx_vertex_tex(const GxDraw* draw, u32 vertex, u32 unit, float out[2]);

typedef enum {
    GX_TF_I4 = 0, GX_TF_I8 = 1, GX_TF_IA4 = 2, GX_TF_IA8 = 3, GX_TF_RGB565 = 4, GX_TF_RGB5A3 = 5,
    GX_TF_RGBA8 = 6, GX_TF_C4 = 8, GX_TF_C8 = 9, GX_TF_C14X2 = 10, GX_TF_CMPR = 14
} GxTextureFormat;

typedef struct {
    u32 width, height, format, address; /* physical address */
    u32 wrap_s, wrap_t;                 /* 0 clamp, 1 repeat, 2 mirror */
    u32 tlut_offset, tlut_format;
} GxTexture;

/* gx_texture.c. Returns 0 for a format that doesn't exist. */
int gx_texture_info(const GxState* state, u32 map, GxTexture* texture);
u32 gx_texture_size(const GxTexture* texture); /* bytes in memory */
int gx_texture_decode(const GxTexture* texture, const u8* tlut, u8* rgba);
const u8* gx_tlut_memory(void);

/* Implemented by the host. */
void host_gx_finish(void);                 /* BP 0x45: all drawing so far is done */
void host_gx_nan(u32 addr, u32 value);     /* a NaN went into a matrix */
void host_gx_token(u32 token, int raise);  /* BP 0x47 and 0x48 */

/* Implemented by the backend. */
void gx_backend_draw(const GxState* state, const GxDraw* draw);
void gx_backend_display_copy(const GxState* state);
void gx_backend_texture_copy(const GxState* state);

void gx_stream_write_report(void);

/* gx_adapter.c: passes the stream to aurora when a window is open. */
int gx_adapter_active(void);
void gx_adapter_stop(void);
void gx_adapter_write_report(void);
int gx_adapter_start(int argc, char** argv, int quiet, const char* capture, u32 every);
void gx_adapter_copy_log(u32 from, u32 until);
u32 gx_adapter_frames(void);
void gx_adapter_save_frames(const char* path); /* the last few seconds of copies and draws */
void gx_adapter_copy_shot(u32 frame, u32 target);
void gx_adapter_bp(const GxState* state, u32 reg, u32 value);
void gx_adapter_cp(u32 reg, u32 value);
void gx_adapter_xf(const u8* packet, u32 size);
void gx_adapter_xf_values(u32 addr, const u8* values, u32 count);
void gx_adapter_raw(const u8* packet, u32 size);
void gx_adapter_draw(const GxState* state, const GxDraw* draw, const u8* packet, u32 size);
int gx_adapter_frame(void);
void gx_adapter_note_idle(void);
void gx_adapter_stream_begin(void);
void gx_adapter_stream_end(void);

/* gx_debug.c: preview images for checking the decoder. */
void gx_debug_setup(const char* path_prefix, u32 every_frames);
void gx_debug_write_report(void);

#endif
