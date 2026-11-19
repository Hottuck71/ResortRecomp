// The only host file that touches aurora. Everything here is plain calls, with no game knowledge.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXExtra.h>
#include <dolphin/vi.h>

#include "aurora_glue.h"

static bool g_unpaced = false; // --unpaced 1: no pacing, so a test measures how fast this PC can run the game

namespace aurora::gx::fifo {
namespace detail {
extern uint8_t* sBufferData;
extern uint32_t sBufferSize;
extern uint32_t sBufferCapacity;
} // namespace detail
void write_data_grow(const void* data, uint32_t length);
void finish_draw() noexcept;
void publish() noexcept;
void drain();
} // namespace aurora::gx::fifo

static void command(uint16_t sub) {
  GXCmd1u8(GX_AURORA);
  GXCmd1u16(sub);
}

extern "C" FILE* report; // the host's run report

static void log_line(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
  if (level >= LOG_WARNING) {
    std::fprintf(report, "aurora %s: %s\n", module, message);
    std::fflush(report);
  }
}

static bool g_exit;

static void pump_events() {
  const AuroraEvent* event = aurora_update();
  while (event != nullptr && event->type != AURORA_NONE) {
    if (event->type == AURORA_EXIT) {
      g_exit = true;
    }
    ++event;
  }
}

namespace aurora::gfx::efb {
extern float g_widthRatio; // added to aurora by scripts/48_build_aurora.ps1
}

extern "C" {

int glue_init(int argc, char** argv, uint32_t width, uint32_t height, int quiet) {
  AuroraConfig config{};
  if (quiet) { // tests: don't take focus or appear over whatever is on screen
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    config.windowPosX = -6000;
    config.windowPosY = -6000;
  } else {
    config.windowPosX = -1;
    config.windowPosY = -1;
  }
  config.appName = "Wii Sports Resort";
  config.logCallback = &log_line;
  config.logLevel = LOG_WARNING;
  config.windowWidth = 1280;
  config.windowHeight = 720;
  config.vsync = true;
  config.emulateXfb = true;
  config.gxProcessingMode = GX_PROCESSING_THREAD;
  config.mem1Size = 0;
  config.mem2Size = 0;
  int skip_draws = 0;
  int efb_scale = 3;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!std::strcmp(argv[i], "--cache")) { // tests: start from an empty shader cache
      config.cachePath = argv[i + 1];
    } else if (!std::strcmp(argv[i], "--skip-draws")) { // tests: aurora's default, for comparison
      skip_draws = std::atoi(argv[i + 1]);
    } else if (!std::strcmp(argv[i], "--scale")) { // whole multiple of the game's 640x528 frame buffer
      efb_scale = std::atoi(argv[i + 1]);
    } else if (!std::strcmp(argv[i], "--aniso")) { // texture filtering: 1 (plain) to 16
      config.maxTextureAnisotropy = static_cast<uint16_t>(std::atoi(argv[i + 1]));
    } else if (!std::strcmp(argv[i], "--vsync")) {
      config.vsync = std::atoi(argv[i + 1]) != 0;
    } else if (!std::strcmp(argv[i], "--fullscreen")) {
      config.startFullscreen = !quiet && std::atoi(argv[i + 1]) != 0;
    } else if (!std::strcmp(argv[i], "--window-size")) { // WIDTHxHEIGHT
      unsigned w = 0, h = 0;
      if (std::sscanf(argv[i + 1], "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240 && w <= 7680 && h <= 4320) {
        config.windowWidth = w;
        config.windowHeight = h;
      }
    } else if (!std::strcmp(argv[i], "--unpaced")) { // speed tests: frames as fast as the PC can make them
      g_unpaced = std::atoi(argv[i + 1]) != 0;
    } else if (!std::strcmp(argv[i], "--gpu")) { // fast (the high-performance card) or low (power saving)
      // Read by aurora's adapter request (native/patches/aurora-adapter-choice.patch). Its fallback adapter, drawing
      // on the processor, isn't offered: this Dawn build has none ("No supported adapters" with every backend).
      if (!std::strcmp(argv[i + 1], "low")) _putenv_s("AURORA_POWER_PREFERENCE", "low");
    } else if (!std::strcmp(argv[i], "--backend")) { // auto, d3d11, d3d12, vulkan
      const char* b = argv[i + 1];
      config.desiredBackend = !std::strcmp(b, "d3d11")    ? BACKEND_D3D11
                              : !std::strcmp(b, "d3d12")  ? BACKEND_D3D12
                              : !std::strcmp(b, "vulkan") ? BACKEND_VULKAN
                                                          : BACKEND_AUTO;
    }
  }
  if (efb_scale > 8) efb_scale = 8;
  // No multisampling: the game copies depth out of its frame buffer for effects, and aurora can't copy depth
  // from a multisampled one (it stops the program). A higher --scale smooths edges instead.
  config.msaa = 1;
  if (config.maxTextureAnisotropy > 16) config.maxTextureAnisotropy = 16;
  aurora_initialize(argc, argv, &config);

  GXRenderModeObj mode{};
  mode.fbWidth = static_cast<uint16_t>(width);
  mode.efbHeight = static_cast<uint16_t>(height);
  mode.xfbHeight = static_cast<uint16_t>(height);
  VIConfigure(&mode);
  // Aurora's default drops a draw while its shader compiles. A PC game redraws next frame, but this game
  // paints some textures only once, such as Mii faces, so a dropped draw would be lost for good.
  AuroraSetDrawsBlocking(skip_draws == 0);
  // The game cuts and pastes tiles of its frame buffer for blur and shadows. Sized to the window, the buffer has
  // a fractional scale, so tile edges round differently and leave seams. A whole multiple keeps them exact, and
  // only the finished picture is stretched to the window. 0 goes back to following the window.
  // The width ratio makes the 608x456 picture exactly 16:9, as the game draws it (IPL.AR in the NAND).
  if (efb_scale > 0) {
    aurora::gfx::efb::g_widthRatio = 4.0f / 3.0f;
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);
    VISetFrameBufferScale(static_cast<float>(efb_scale));
  }

  pump_events();
  while (!g_exit && !aurora_begin_frame()) {
    pump_events();
  }
  return !g_exit;
}

// The same as aurora's inline write_data: write_data_grow doubles the buffer on every call.
void glue_write(const void* data, uint32_t size) {
  using namespace aurora::gx::fifo;
  if (size <= detail::sBufferCapacity - detail::sBufferSize) {
    std::memcpy(detail::sBufferData + detail::sBufferSize, data, size);
    detail::sBufferSize += size;
  } else {
    write_data_grow(data, size);
  }
}

void glue_draw_done(void) { aurora::gx::fifo::finish_draw(); }

void glue_drain(void) { aurora::gx::fifo::drain(); }

void glue_set_array(uint32_t index, const void* data, uint32_t size, uint32_t stride) {
  GXSetArray(static_cast<GXAttr>(GX_VA_POS + index), data, size, static_cast<uint8_t>(stride), false);
}

void glue_destroy_copy(void* target) { GXDestroyCopyTex(target); }

static bool g_shot, g_shot_key;

int glue_shot_wanted(void) {
  const bool wanted = g_shot;
  g_shot = false;
  return wanted;
}

void glue_load_texture(uint32_t map, const void* data, uint32_t width, uint32_t height, uint32_t format,
                       uint32_t tlut, int mips, uint32_t version) {
  command(GX_AURORA_LOAD_TEXOBJ);
  GXCmd1u8(static_cast<uint8_t>(map));
  GXCmd1u64(reinterpret_cast<uint64_t>(data));
  GXCmd1u32(width);
  GXCmd1u32(height);
  GXCmd1u32(format);
  GXCmd1u32(tlut);
  GXCmd1u8(mips ? 1 : 0);
  GXCmd1u32(0); // no object id: the host has no GXTexObj to name
  GXCmd1u32(version);
}

void glue_load_tlut(uint32_t slot, const void* data, uint32_t format, uint32_t entries) {
  command(GX_AURORA_LOAD_TLUT);
  GXCmd1u8(static_cast<uint8_t>(slot));
  GXCmd1u64(reinterpret_cast<uint64_t>(data));
  GXCmd1u32(format);
  GXCmd1u16(static_cast<uint16_t>(entries));
  GXCmd1u32(0);
  GXCmd1u32(0);
}

void glue_copy_texture(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t out_width,
                       uint32_t out_height, uint32_t format, int mip, void* target, int clear) {
  GXSetTexCopySrc(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(width),
                  static_cast<uint16_t>(height));
  GXSetTexCopyDst(static_cast<uint16_t>(out_width), static_cast<uint16_t>(out_height),
                  static_cast<GXTexFmt>(format), mip != 0);
  GXCopyTex(target, clear != 0);
}

void glue_copy_display(uint32_t x, uint32_t y, uint32_t width, uint32_t height, int clear) {
  GXSetDispCopySrc(static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(width),
                   static_cast<uint16_t>(height));
  GXCopyDisp(nullptr, clear != 0);
}

// Game speed follows frames, so frames must not run faster than the video rate.
static void pace() {
  using clock = std::chrono::steady_clock;
  constexpr auto kFrame = std::chrono::nanoseconds(16683350); // 59.94 Hz
  static clock::time_point next = clock::now();
  next += kFrame;
  auto now = clock::now();
  if (now > next + kFrame * 4) {
    next = now; // fell behind: don't try to catch up
    return;
  }
  if (next - now > std::chrono::milliseconds(2)) {
    std::this_thread::sleep_for(next - now - std::chrono::milliseconds(2));
  }
  while (clock::now() < next) {
  }
}

// Reads the EFB straight from the GPU, so a capture can only ever hold the game's own image. Several can
// wait at once, because a debug capture may ask for more than one point in a frame.
constexpr int kMaxCaptures = 8;
struct PendingCapture {
  uint32_t key, width, height;
  char path[512];
};
static AuroraReadbackStream g_stream;
static PendingCapture g_captures[kMaxCaptures];

void glue_capture(const char* path) {
  static uint32_t next_key;
  if (!g_stream) {
    g_stream = AuroraReadbackCreateStream();
  }
  for (auto& capture : g_captures) {
    if (capture.key) {
      continue;
    }
    uint32_t key = ++next_key;
    if (AuroraReadbackCaptureColor(g_stream, key, &capture.width, &capture.height)) {
      capture.key = key;
      std::snprintf(capture.path, sizeof capture.path, "%s", path);
    }
    return;
  }
}

// Writes colour to the path, and alpha beside it as path.alpha.ppm.
static void save_capture(const PendingCapture& capture, const uint8_t* pixels, size_t size) {
  char alpha_path[540];
  std::snprintf(alpha_path, sizeof alpha_path, "%s.alpha.ppm", capture.path);
  FILE* f = std::fopen(capture.path, "wb");
  if (f) {
    std::fprintf(f, "P6\n%u %u\n255\n", capture.width, capture.height);
    for (size_t i = 0; i < size; i += 4) {
      std::fwrite(pixels + i, 1, 3, f);
    }
    std::fclose(f);
  }
  if (std::strstr(capture.path, "copy-") == nullptr) {
    return; // only debug captures need alpha
  }
  f = std::fopen(alpha_path, "wb");
  if (f) {
    std::fprintf(f, "P6\n%u %u\n255\n", capture.width, capture.height);
    for (size_t i = 0; i < size; i += 4) {
      const uint8_t a[3] = {pixels[i + 3], pixels[i + 3], pixels[i + 3]};
      std::fwrite(a, 1, 3, f);
    }
    std::fclose(f);
  }
}

static void finish_capture() {
  for (auto& capture : g_captures) {
    if (!capture.key) {
      continue;
    }
    const size_t size = static_cast<size_t>(capture.width) * capture.height * 4;
    auto* pixels = static_cast<uint8_t*>(std::malloc(size));
    AuroraReadbackDest dest{capture.key, pixels, static_cast<u32>(size), 0};
    if (pixels && AuroraReadbackGet(g_stream, &dest, 1) && dest.found) {
      save_capture(capture, pixels, size);
      capture.key = 0;
    }
    std::free(pixels);
  }
}

static SDL_AudioStream* g_audio;

int glue_audio_open(uint32_t rate) {
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return 0;
  }
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_S16;
  spec.channels = 2;
  spec.freq = static_cast<int>(rate);
  g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!g_audio) {
    return 0;
  }
  SDL_ResumeAudioStreamDevice(g_audio);
  return 1;
}

void glue_audio_write(const void* samples, uint32_t size) {
  // audio.c keeps the queue near its target; this only stops it running away (a stall, a debugger).
  if (SDL_GetAudioStreamQueued(g_audio) < 32000 * 4 / 4) {
    SDL_PutAudioStreamData(g_audio, samples, static_cast<int>(size));
  }
}

uint32_t glue_audio_queued(void) {
  const int queued = SDL_GetAudioStreamQueued(g_audio);
  return queued > 0 ? static_cast<uint32_t>(queued) : 0;
}

// Only reads input while the game's window has focus, so typing elsewhere does nothing.
void glue_read_input(GlueInput* in) {
  *in = GlueInput{};
  SDL_Window* window = aurora_get_window();
  if (window == nullptr || SDL_GetKeyboardFocus() != window) {
    return;
  }
  const bool* keys = SDL_GetKeyboardState(nullptr);
  float x = 0, y = 0;
  const SDL_MouseButtonFlags mouse = SDL_GetMouseState(&x, &y);
  int width = 1, height = 1;
  SDL_GetWindowSize(window, &width, &height);
  in->pointer_x = x / static_cast<float>(width) * 2.0f - 1.0f;
  in->pointer_y = y / static_cast<float>(height) * 2.0f - 1.0f;
  in->pointer_valid = 1;
  const struct {
    SDL_Scancode key;
    uint32_t bit;
  } map[] = {
      {SDL_SCANCODE_Z, 0x0008},      {SDL_SCANCODE_X, 0x0004},      {SDL_SCANCODE_1, 0x0002},
      {SDL_SCANCODE_2, 0x0001},      {SDL_SCANCODE_RETURN, 0x1000}, {SDL_SCANCODE_BACKSPACE, 0x0010},
      {SDL_SCANCODE_H, 0x0080},      {SDL_SCANCODE_UP, 0x0800},     {SDL_SCANCODE_DOWN, 0x0400},
      {SDL_SCANCODE_LEFT, 0x0100},   {SDL_SCANCODE_RIGHT, 0x0200},
      // Not Wii buttons: the MotionPlus pointer's re-centre (C) and its left-right and up-down flips (F9, F10)
      {SDL_SCANCODE_C, 0x10000},     {SDL_SCANCODE_F9, 0x20000},    {SDL_SCANCODE_F10, 0x40000},
  };
  for (const auto& m : map) {
    if (keys[m.key]) {
      in->buttons |= m.bit;
    }
  }
  if (mouse & SDL_BUTTON_LMASK) {
    in->buttons |= 0x0008; // A
  }
  if (mouse & SDL_BUTTON_RMASK) {
    in->buttons |= 0x0004; // B
  }
  constexpr float kTilt = 0.7f;
  in->pitch = (keys[SDL_SCANCODE_W] ? kTilt : 0) - (keys[SDL_SCANCODE_S] ? kTilt : 0);
  in->roll = (keys[SDL_SCANCODE_D] ? kTilt : 0) - (keys[SDL_SCANCODE_A] ? kTilt : 0);
  in->swing = keys[SDL_SCANCODE_SPACE] ? 1.0f : 0.0f;
  // The stand-in Nunchuk: N plugs it in or takes it out, I J K L for the stick, Q for C, E for Z, F to pump it.
  in->nunchuk_plug = keys[SDL_SCANCODE_N] ? 1 : 0;
  in->nunchuk_x = (keys[SDL_SCANCODE_L] ? 1.0f : 0) - (keys[SDL_SCANCODE_J] ? 1.0f : 0);
  in->nunchuk_y = (keys[SDL_SCANCODE_I] ? 1.0f : 0) - (keys[SDL_SCANCODE_K] ? 1.0f : 0);
  in->nunchuk_c = keys[SDL_SCANCODE_Q] ? 1 : 0;
  in->nunchuk_z = keys[SDL_SCANCODE_E] ? 1 : 0;
  in->nunchuk_swing = keys[SDL_SCANCODE_F] ? 1.0f : 0.0f;
  if (keys[SDL_SCANCODE_F12] && !g_shot_key) {
    g_shot = true;
  }
  g_shot_key = keys[SDL_SCANCODE_F12];
}

void glue_shutdown(void) {
  aurora_end_frame();
  aurora_shutdown();
}


// The window's title, set from the CPU thread (online play's speed readout) and applied here, on the window's
// own thread.
static std::mutex g_title_lock;
static std::string g_title;
static bool g_title_new = false;

void glue_set_title(const char* title) {
  std::lock_guard<std::mutex> lock(g_title_lock);
  g_title = title;
  g_title_new = true;
}

static void apply_title() {
  std::lock_guard<std::mutex> lock(g_title_lock);
  if (!g_title_new) return;
  g_title_new = false;
  if (SDL_Window* window = aurora_get_window()) SDL_SetWindowTitle(window, g_title.c_str());
}

int glue_next_frame(int paced) {
  aurora_end_frame();
  finish_capture();
  if (paced && !g_unpaced) {
    pace();
  }
  static uint32_t frames;
  if (++frames % 120 == 0) {
    const AuroraStats* stats = aurora_get_stats();
    std::fprintf(report, "aurora stats: draws %u merged %u pipelines %u queued %u verts %u fps %.1f\n",
                 stats->drawCallCount, stats->mergedDrawCallCount, stats->createdPipelines,
                 stats->queuedPipelines, stats->lastVertSize, aurora_get_fps());
  }
  apply_title();
  pump_events();
  while (!g_exit && !aurora_begin_frame()) {
    pump_events();
  }
  return !g_exit;
}
}
