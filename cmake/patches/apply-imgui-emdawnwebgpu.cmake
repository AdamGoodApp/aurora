# Dear ImGui 1.91.9b predates emdawnwebgpu and rejects IMGUI_IMPL_WEBGPU_BACKEND_DAWN under Emscripten.
# emdawnwebgpu implements Dawn's webgpu.h in the browser, so the Dawn code path is the correct one there.
if (NOT DEFINED IMGUI_SOURCE_DIR)
  message(FATAL_ERROR "IMGUI_SOURCE_DIR is required")
endif ()

set(_file "${IMGUI_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp")
if (NOT EXISTS "${_file}")
  message(FATAL_ERROR "ImGui WebGPU backend source is missing: ${_file}")
endif ()

file(READ "${_file}" _source)
set(_old "#error neither IMGUI_IMPL_WEBGPU_BACKEND_DAWN nor IMGUI_IMPL_WEBGPU_BACKEND_WGPU may be defined if targeting emscripten!")
set(_new "// aurora: emdawnwebgpu provides Dawn's webgpu.h under Emscripten; IMGUI_IMPL_WEBGPU_BACKEND_DAWN is valid.")

string(FIND "${_source}" "${_new}" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "ImGui emdawnwebgpu patch already applied")
  return()
endif ()

string(FIND "${_source}" "${_old}" _patch_site)
if (_patch_site EQUAL -1)
  message(FATAL_ERROR "Failed to apply ImGui emdawnwebgpu patch: expected #error not found")
endif ()

string(REPLACE "${_old}" "${_new}" _patched_source "${_source}")
file(WRITE "${_file}" "${_patched_source}")
message(STATUS "Applied ImGui emdawnwebgpu patch")
