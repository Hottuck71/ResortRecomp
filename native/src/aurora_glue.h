/* The small interface between the C adapter and aurora. See aurora_glue.cpp. */
#ifndef AURORA_GLUE_H
#define AURORA_GLUE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int glue_init(int argc, char** argv, uint32_t width, uint32_t height, int quiet);
void glue_capture(const char* path); /* the EFB as it stands, read from the GPU */
void glue_write(const void* data, uint32_t size);
void glue_draw_done(void);
void glue_set_array(uint32_t index, const void* data, uint32_t size, uint32_t stride);
void glue_load_texture(uint32_t map, const void* data, uint32_t width, uint32_t height, uint32_t format,
                       uint32_t tlut, int mips, uint32_t version);
void glue_destroy_copy(void* target);
int glue_shot_wanted(void); /* F12 was pressed since the last call */
void glue_load_tlut(uint32_t slot, const void* data, uint32_t format, uint32_t entries);
void glue_copy_texture(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t out_width,
                       uint32_t out_height, uint32_t format, int mip, void* target, int clear);
void glue_copy_display(uint32_t x, uint32_t y, uint32_t width, uint32_t height, int clear);
void glue_shutdown(void);
void glue_set_title(const char* title); /* UTF-8; applied on the next frame */

/* What the player is doing, read once per frame. Buttons use the Wii Remote report's bit layout. */
typedef struct {
    uint32_t buttons;
    float pointer_x, pointer_y; /* -1 to 1 across the window */
    int pointer_valid;
    float pitch, roll;          /* held tilt, radians */
    float swing;                /* 0 at rest, up to 1 during a swing */
    float nunchuk_x, nunchuk_y; /* the stand-in Nunchuk's stick, -1 to 1, up positive */
    int nunchuk_c, nunchuk_z;
    float nunchuk_swing;        /* a pump of the Nunchuk */
    int nunchuk_plug;           /* the key that plugs it in or takes it out */
} GlueInput;
void glue_read_input(GlueInput* input);
int glue_audio_open(uint32_t rate);
void glue_audio_write(const void* samples, uint32_t size);
uint32_t glue_audio_queued(void); /* bytes waiting to be played */
int glue_next_frame(int paced); /* returns 0 when the window has been closed */
void glue_drain(void);

#ifdef __cplusplus
}
#endif
#endif
