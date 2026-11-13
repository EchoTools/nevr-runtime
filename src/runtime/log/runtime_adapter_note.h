#pragma once

// #1846: the GPU name and driver the runtime read itself (render_config's first line), handed to the GPU-hung notice
// (gpu_hung_notice.h) so it can name them when the game prints its System Info after its HUNG line.
//
// Kept in its own small translation unit, with no game, hook or logging dependency, so the producer
// (patch/render_config_hook.cpp) links into test_render_config_hook without builtin_filter.cpp.

#include <string>

namespace nevr_gpu_hung {

/// Record the adapter the runtime read. Safe from any thread; an empty argument leaves the earlier value in place.
void NoteRuntimeAdapter(const char* gpu_name, const char* gpu_driver);

/// The adapter last noted ("" for a value never noted). Safe from any thread.
void KnownRuntimeAdapter(std::string& gpu_name, std::string& gpu_driver);

}  // namespace nevr_gpu_hung
