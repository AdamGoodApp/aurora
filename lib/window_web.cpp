// Browser (Emscripten) window backend.
//
// The page creates an OffscreenCanvas inside the dedicated worker that runs the game and hands it to the
// module as Module['auroraCanvas']; there is no SDL window or SDL video subsystem. SDL is initialized for
// events only so custom events (surface refresh, resize) keep their native flow.
#include "window.hpp"

#include "internal.hpp"
#include "time_internal.hpp"
#include "dolphin/vi/vi_internal.hpp"
#include "webgpu/gpu.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>

#include <emscripten/emscripten.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

EM_JS(int, aurora_emscripten_canvas_width, (), { return Module['auroraCanvas'].width | 0; });
EM_JS(int, aurora_emscripten_canvas_height, (), { return Module['auroraCanvas'].height | 0; });
EM_JS(void, aurora_emscripten_post_canvas_bitmap, (), {
  const bitmap = Module['auroraCanvas'].transferToImageBitmap();
  postMessage({type : 'frame', bitmap : bitmap}, [bitmap]);
});

namespace aurora::window {
namespace {
constexpr Module Log{"aurora::window"};

AuroraWindowSize g_windowSize;
std::vector<AuroraEvent> g_events;
std::atomic_bool g_paused = false;
std::atomic_bool g_exitRequested = false;
bool g_lastPaused = false;
bool g_windowCreated = false;
float g_frameBufferScale = 0.f;
bool g_frameBufferAspectFit = false;

bool operator==(const AuroraWindowSize& lhs, const AuroraWindowSize& rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height && lhs.fb_width == rhs.fb_width &&
         lhs.fb_height == rhs.fb_height && lhs.native_fb_height == rhs.native_fb_height &&
         lhs.native_fb_width == rhs.native_fb_width && lhs.scale == rhs.scale;
}

// Same frame-buffer sizing rules as lib/window.cpp.
Vec2<int> scale_frame_buffer_to_aspect(int w, int h, float scale, float aspect) {
  if (w <= 0 || h <= 0 || scale <= 0.f || aspect <= 0.f) {
    return {std::max(w, 1), std::max(h, 1)};
  }
  const int baseW = std::max(1, static_cast<int>(std::lround(static_cast<float>(w) * scale)));
  const int baseH = std::max(1, static_cast<int>(std::lround(static_cast<float>(h) * scale)));
  if (aspect >= static_cast<float>(w) / static_cast<float>(h)) {
    return {std::max(1, static_cast<int>(std::lround(static_cast<float>(baseH) * aspect))), baseH};
  }
  return {baseW, std::max(1, static_cast<int>(std::lround(static_cast<float>(baseW) / aspect)))};
}

Vec2<int> fit_frame_buffer_to_aspect(int width, int height, float aspect) {
  if (width <= 0 || height <= 0 || aspect <= 0.f) {
    return {std::max(width, 1), std::max(height, 1)};
  }
  if (static_cast<float>(width) / static_cast<float>(height) > aspect) {
    return {std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * aspect))), height};
  }
  return {width, std::max(1, static_cast<int>(std::lround(static_cast<float>(width) / aspect)))};
}

void resize_swapchain() noexcept {
  const auto size = get_window_size();
  if (size == g_windowSize) {
    return;
  }
  g_windowSize = size;
#ifdef AURORA_ENABLE_GX
  webgpu::resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height);
#endif
}

void sync_paused() {
  const bool paused = is_paused();
  if (g_lastPaused == paused) {
    return;
  }
  g_lastPaused = paused;
  time::internal::set_pause_reason(time::internal::PauseReason::Window, paused);
  g_events.push_back(AuroraEvent{
      .type = paused ? AURORA_PAUSED : AURORA_UNPAUSED,
  });
}

void process_event(SDL_Event& event) {
  if (event.type == SDL_EVENT_QUIT) {
    g_events.push_back(AuroraEvent{.type = AURORA_EXIT});
  } else if (event.type == g_sdlCustomEventsStart + CustomEvent::FutureResize) {
    resize_swapchain();
    g_events.push_back(AuroraEvent{
        .type = AURORA_WINDOW_RESIZED,
        .windowSize = get_window_size(),
    });
  } else if (event.type == g_sdlCustomEventsStart + CustomEvent::RefreshSurface) {
#ifdef AURORA_ENABLE_GX
    webgpu::refresh_surface(false);
#endif
  }
  g_events.push_back(AuroraEvent{
      .type = AURORA_SDL_EVENT,
      .sdl = event,
  });
}
} // namespace

SurfaceLock::SurfaceLock() noexcept = default;
SurfaceLock::~SurfaceLock() = default;

bool initialize() {
  TRY(SDL_InitSubSystem(SDL_INIT_EVENTS), "Error initializing SDL events: {}", SDL_GetError());
  return true;
}

bool initialize_event_watch() { return true; }

bool push_custom_event(CustomEvent eventType) {
  SDL_Event event{.type = g_sdlCustomEventsStart + eventType};
  return SDL_PushEvent(&event);
}

void shutdown() {
  g_windowCreated = false;
  SDL_Quit();
}

bool create_window(AuroraBackend backend) {
  if (backend != BACKEND_WEBGPU) {
    return false;
  }
  g_windowCreated = true;
  return true;
}

bool create_renderer() { return false; }

void destroy_window() { g_windowCreated = false; }

void show_window() {}

AuroraWindowSize get_window_size() {
  const int native_fb_w = aurora_emscripten_canvas_width();
  const int native_fb_h = aurora_emscripten_canvas_height();
  int fb_w = native_fb_w;
  int fb_h = native_fb_h;
  if (g_frameBufferScale > 0.f) {
    const auto [baseW, baseH] = vi::configured_fb_size();
    const auto [scaledW, scaledH] =
        scale_frame_buffer_to_aspect(static_cast<int>(baseW), static_cast<int>(baseH), g_frameBufferScale,
                                     static_cast<float>(fb_w) / static_cast<float>(fb_h));
    fb_w = scaledW;
    fb_h = scaledH;
  }
  if (g_frameBufferAspectFit) {
    const auto [baseW, baseH] = vi::configured_fb_size();
    if (baseW > 0 && baseH > 0) {
      const auto [fitW, fitH] =
          fit_frame_buffer_to_aspect(fb_w, fb_h, static_cast<float>(baseW) / static_cast<float>(baseH));
      fb_w = fitW;
      fb_h = fitH;
    }
  }
  return {
      .width = static_cast<uint32_t>(native_fb_w),
      .height = static_cast<uint32_t>(native_fb_h),
      .fb_width = static_cast<uint32_t>(fb_w),
      .fb_height = static_cast<uint32_t>(fb_h),
      .native_fb_width = static_cast<uint32_t>(native_fb_w),
      .native_fb_height = static_cast<uint32_t>(native_fb_h),
      .scale = 1.f,
  };
}

const AuroraEvent* poll_events() {
  g_events.clear();
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    process_event(event);
  }
  sync_paused();
  if (g_exitRequested.load(std::memory_order_acquire)) {
    g_events.push_back(AuroraEvent{.type = AURORA_EXIT});
  }
  g_events.push_back(AuroraEvent{.type = AURORA_NONE});
  return g_events.data();
}

SDL_Window* get_sdl_window() { return nullptr; }

SDL_Renderer* get_sdl_renderer() { return nullptr; }

bool is_paused() noexcept { return g_paused.load(std::memory_order_acquire); }

bool is_presentable() noexcept { return g_windowCreated; }

void set_surface_ready(bool) noexcept {}

void set_title(const char*) {}

void set_fullscreen(bool) {}

bool get_fullscreen() { return false; }

void set_window_size(uint32_t, uint32_t) {}

void set_window_position(uint32_t, uint32_t) {}

void center_window() {}

void request_frame_buffer_resize() {
  TRY_WARN(push_custom_event(CustomEvent::FutureResize), "Failed to push SDL event for future resize: {}",
           SDL_GetError());
}

void set_frame_buffer_scale(float scale) {
  if (scale < 0.f) {
    scale = 0.f;
  }
  if (g_frameBufferScale == scale) {
    return;
  }
  g_frameBufferScale = scale;
  request_frame_buffer_resize();
}

void set_frame_buffer_aspect_fit(bool fit) {
  if (g_frameBufferAspectFit == fit) {
    return;
  }
  g_frameBufferAspectFit = fit;
  request_frame_buffer_resize();
}

void set_background_input(bool) {}

void emscripten_present_bitmap() { aurora_emscripten_post_canvas_bitmap(); }

} // namespace aurora::window

void aurora_emscripten_set_paused(bool paused) {
  aurora::window::g_paused.store(paused, std::memory_order_release);
}

void aurora_emscripten_request_exit(void) {
  aurora::window::g_exitRequested.store(true, std::memory_order_release);
}
