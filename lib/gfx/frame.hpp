#pragma once

#include "resources.hpp"
#include "frame_packet.hpp"

#include <optional>

namespace aurora::gfx::detail {

inline constexpr size_t FrameSlotCount = 2;
#ifdef __EMSCRIPTEN__
// Browser build: buffer mapping completes only when the worker yields to its event loop, which the blocking
// game loop never does. Frames are recorded into CPU memory and uploaded with Queue::WriteBuffer into the
// GPU staging buffer right before the existing in-encoder copies. The render worker runs inline, so a single
// staging buffer is released at the end of every frame.
inline constexpr size_t StagingBufferCount = 1;
#else
inline constexpr size_t StagingBufferCount = FrameSlotCount + 3;
#endif
inline constexpr uint64_t StagingBufferSize = UniformBufferSize + VertexBufferSize + IndexBufferSize +
                                              StorageBufferSize + (UseTextureBuffer ? TextureUploadSize : 0);

const wgpu::Buffer& staging_buffer(size_t slot);
#ifdef __EMSCRIPTEN__
// CPU mirror of staging_buffer(slot); the recording writes here instead of a mapped range.
uint8_t* staging_cpu(size_t slot);
#endif

struct RegisteredDrawType {
  DrawCallback draw = nullptr;
  void* userdata = nullptr;
};

struct RegisteredEncoderTaskType {
  EncoderTaskCallback callback = nullptr;
  void* userdata = nullptr;
  EncoderTaskCompletionCallback afterSubmit = nullptr;
};

std::optional<RegisteredDrawType> find_runtime_draw_type(DrawTypeId id);
std::optional<RegisteredEncoderTaskType> find_runtime_encoder_task_type(EncoderTaskId id);

} // namespace aurora::gfx::detail

namespace aurora::gfx {
void initialize();
void shutdown();
bool begin_frame();
void end_frame(EndFrameCallback callback);
uint32_t current_frame() noexcept;
void after_submit() noexcept;
void gpu_synchronize();
void after_present() noexcept;
float calculate_fps() noexcept;
} // namespace aurora::gfx
