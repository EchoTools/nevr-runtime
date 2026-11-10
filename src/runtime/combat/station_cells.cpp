#include "runtime/combat/station_cells.h"

#include <atomic>
#include <cstring>

#include "core/logging.h"
#include "runtime/hook/script_hooks.h"

namespace nevr_combat::station_cells {
namespace {

using CountFn = std::int32_t (*)(std::int64_t* ctx);
using PositionFn = std::uint64_t* (*)(std::uint64_t* out, std::int64_t* ctx);

CountFn g_count = nullptr;
PositionFn g_position = nullptr;
std::atomic<const volatile std::uint8_t*> g_counts{nullptr};
std::atomic_bool g_registered{false};

// The thread data's gear type (+0) and cell (+4).
const std::int32_t* ThreadData(std::int64_t* ctx) {
  std::int32_t* data = nullptr;
  std::memcpy(&data, reinterpret_cast<std::uint8_t*>(ctx) + kThreadDataOffset, sizeof data);
  return data;
}

std::int32_t CountHook(std::int64_t* ctx) {
  const std::int32_t stock = g_count(ctx);
  const std::int32_t* thread = ThreadData(ctx);
  if (thread == nullptr) return stock;
  const auto* data = *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(ctx[0]) + 0x68);
  std::int32_t max_icons = 0;
  std::memcpy(&max_icons, data + kMaxIconsOffset, sizeof max_icons);
  return Count(stock, max_icons, thread[0], g_counts.load());
}

std::uint64_t* PositionHook(std::uint64_t* out, std::int64_t* ctx) {
  std::uint64_t* const r = g_position(out, ctx);
  const std::int32_t* thread = ThreadData(ctx);
  if (thread != nullptr && OffCanvas(thread[0], thread[1], g_counts.load())) {
    const float off_canvas[2] = {kFar, kFar};
    std::memcpy(out, off_canvas, sizeof off_canvas);
  }
  return r;
}

}  // namespace

std::uint8_t Published(const volatile std::uint8_t* counts, std::int32_t type) {
  if (counts == nullptr || type < 0 || type > 2) return 0;
  return counts[type];
}

std::int32_t Count(std::int32_t stock, std::int32_t max_icons, std::int32_t type, const volatile std::uint8_t* counts) {
  return Published(counts, type) != 0 ? max_icons : stock;
}

bool OffCanvas(std::int32_t type, std::int32_t cell, const volatile std::uint8_t* counts) {
  const std::uint8_t n = Published(counts, type);
  return n != 0 && cell >= n;
}

void Register(const volatile std::uint8_t* counts) {
  if (g_registered.exchange(true)) return;
  g_counts.store(counts);
  const nevr_script_hooks::Hook rows[] = {
      {kScript, kCountRva, kCountPrologue, sizeof kCountPrologue, reinterpret_cast<void*>(&CountHook),
       reinterpret_cast<void**>(&g_count), "station cells: every cell of a published grid"},
      {kScript, kPositionRva, kPositionPrologue, sizeof kPositionPrologue, reinterpret_cast<void*>(&PositionHook),
       reinterpret_cast<void**>(&g_position), "station cells: cells past the published count off the canvas"},
  };
  nevr_script_hooks::Register(rows, sizeof rows / sizeof rows[0]);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.COMBAT] station cells fix registered script=%016llx hooks=2 (a LOADOUTS column that shrinks leaves no icon "
      "of the longer layout behind; installed when the equipment station's script loads)",
      static_cast<unsigned long long>(kScript));
}

#ifdef NEVR_TEST_HOOKS
void ResetForTest() {
  g_registered = false;
  g_counts.store(nullptr);
}
#endif

}  // namespace nevr_combat::station_cells
