#include "runtime/patch/gpu_score.h"

#include <cmath>

namespace nevr_gpu_score {

double NvidiaScore(std::uint32_t cores, std::uint32_t clockKhz) {
  const std::uint64_t product = static_cast<std::uint64_t>(cores) * clockKhz * 2;
  return static_cast<double>(product) * 1e-9;
}

Decision DecideScore(std::uint32_t vendor, double gameScore, const NvidiaMeasure& nvidia) {
  Decision decision;
  if (gameScore != 0.0) {
    decision.outcome = Outcome::Kept;
    decision.score = gameScore;
    decision.source = "game";
    return decision;
  }
  if (vendor != kVendorNvidia) {
    decision.reason = "vendor_unsupported";
    return decision;
  }
  if (!nvidia.haveCores) {
    decision.reason = nvidia.failure[0] != '\0' ? nvidia.failure : "nvapi_cores_unavailable";
    return decision;
  }
  if (!nvidia.haveClockKhz) {
    decision.reason = nvidia.failure[0] != '\0' ? nvidia.failure : "nvapi_clock_unavailable";
    return decision;
  }
  const double score = NvidiaScore(nvidia.cores, nvidia.clockKhz);
  if (!std::isfinite(score) || score <= 0.0 || score > kMaxPlausibleScore) {
    decision.reason = "score_implausible";
    return decision;
  }
  decision.outcome = Outcome::Filled;
  decision.score = score;
  decision.source = "nvidia_nvapi";
  return decision;
}

const char* OutcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::Kept: return "kept";
    case Outcome::Filled: return "filled";
    case Outcome::Unrated: return "unrated";
  }
  return "unrated";
}

}  // namespace nevr_gpu_score
