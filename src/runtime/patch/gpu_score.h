#pragma once

// #1419: the GPU score the game's auto-selector reads, filled by the runtime when the game's own rating failed.
//
// The game writes the score during D3D12 device init: NVIDIA cores x base clock (kHz) x 2 x 1e-9 through NVAPI, AMD's AGS
// teraFlops. Under Proton the NVIDIA base-clock query is refused (dxvk-nvapi answers only the current clock), so the score
// stays 0 and the selector picks quality level 1 on any GPU. This file is the pure half: the formula and the decision.
// No Windows call is made here; the NVAPI query and the write into the game are in patch/render_config_hook.cpp, and a
// unit test feeds the decision directly (src/runtime/tests/test_gpu_score.cpp).

#include <cstdint>

namespace nevr_gpu_score {

/// The vendor code the game stores for NVIDIA (the selector compares it with 1; AMD is 2).
inline constexpr std::uint32_t kVendorNvidia = 1;
/// No real adapter rates above this in the game's units (TFLOPS); a larger value is a misread, not a score.
inline constexpr double kMaxPlausibleScore = 1000.0;

/// What the NVIDIA query found. `failure` names the first step that did not answer, "" when none failed.
struct NvidiaMeasure {
  bool haveCores = false;
  std::uint32_t cores = 0;
  bool haveClockKhz = false;
  std::uint32_t clockKhz = 0;
  const char* failure = "";
};

enum class Outcome { Kept, Filled, Unrated };

struct Decision {
  Outcome outcome = Outcome::Unrated;
  double score = 0.0;               // the score the selector should see
  const char* source = "none";      // "game" (kept), "nvidia_nvapi" (filled), "none" (unrated)
  const char* reason = "";          // why unrated; "" otherwise
};

/// The game's NVIDIA score: cores x clock (kHz) x 2 x 1e-9, in the same integer and then double steps the game takes
/// (imul, add to double the product, mulsd by 1e-9).
double NvidiaScore(std::uint32_t cores, std::uint32_t clockKhz);

/// The score the selector should see. A non-zero game score is kept as it is, whatever the vendor (Windows is untouched).
/// A zero score is filled only for NVIDIA with both NVAPI values, and only when the result is a finite, positive, plausible
/// number; otherwise it stays zero and the reason says which step failed.
Decision DecideScore(std::uint32_t vendor, double gameScore, const NvidiaMeasure& nvidia);

const char* OutcomeName(Outcome outcome);

}  // namespace nevr_gpu_score
