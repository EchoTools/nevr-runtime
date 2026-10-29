#include "runtime/patch/netgun_bullets.h"

#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "core/logging.h"
#include "runtime/patch/asset_cache.h"

namespace nevr_netgun_bullets {
namespace {

// The rewritten resources, built once per stock digest and served on every later load of any level that delivers
// the same bytes. They live for the rest of the process; the seam copies them into the game's heap on each load.
std::mutex g_mutex;
std::map<std::pair<Resource, std::string>, std::vector<uint8_t>> g_served;

#ifdef NEVR_TEST_HOOKS
std::vector<std::string> g_testPins[2];
bool g_testPinsSet = false;
#endif

std::vector<std::string> Pins(Resource res) {
#ifdef NEVR_TEST_HOOKS
  if (g_testPinsSet) return g_testPins[res == Resource::Particles ? 0 : 1];
#endif
  return StockPins(res);
}

bool Serve(Resource res, const void* original, uint64_t size, const void** out, uint64_t* outSize, void* user) {
  const char* level = static_cast<const Level*>(user)->name;  // Install passes the level the transform is for
  if (original == nullptr || size == 0) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] netgun bullet effects left off level=%s resource=%s reason=no_buffer bytes=%llu (the loader "
        "offered no bytes to read; nothing is served, so the game keeps what it has)",
        level, ResourceName(res), static_cast<unsigned long long>(size));
    return false;
  }
  const std::string sha = nevr_asset_cache::Sha256Hex(original, static_cast<size_t>(size));

  std::lock_guard<std::mutex> lock(g_mutex);
  const auto key = std::make_pair(res, sha);
  const auto served = g_served.find(key);
  if (served != g_served.end()) {
    *out = served->second.data();
    *outSize = served->second.size();
    return true;
  }

  const std::vector<std::string> pins = Pins(res);
  Result r = Decide(res, static_cast<const uint8_t*>(original), static_cast<size_t>(size), sha, pins);
  if (r.verdict == Verdict::NotStock) {
    std::string pinList;
    for (const std::string& pin : pins) pinList += (pinList.empty() ? "" : ",") + pin;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] netgun bullet effects left off level=%s resource=%s reason=not_pinned bytes=%llu "
        "sha256=%s pins=%s (another game build, or a resource already changed; the game's own bytes are served)",
        level, ResourceName(res), static_cast<unsigned long long>(size), sha.c_str(), pinList.c_str());
    return false;
  }
  if (r.verdict == Verdict::Refused) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] netgun bullet effects left off level=%s resource=%s reason=%s sha256=%s (the pinned "
        "resource did not have the stock layout; the game's own bytes are served)",
        level, ResourceName(res), StatusName(r.status), sha.c_str());
    return false;
  }

  const std::string resultSha = nevr_asset_cache::Sha256Hex(r.bytes.data(), r.bytes.size());
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] netgun bullet effects added level=%s resource=%s entries=%zu->%zu orig_bytes=%llu "
      "new_bytes=%zu stock_sha256=%s result_sha256=%s (the magnum, smg, Chain and rifle bullets "
      "get the scout bullet's trail and impact, the magnum its explosion too)",
      level, ResourceName(res), r.entriesBefore, r.entriesAfter, static_cast<unsigned long long>(size),
      r.bytes.size(), sha.c_str(), resultSha.c_str());
  std::vector<uint8_t>& kept = g_served[key];
  kept = std::move(r.bytes);
  *out = kept.data();
  *outSize = kept.size();
  return true;
}

bool TransformParticles(const void* original, uint64_t size, const void** out, uint64_t* outSize, void* user) {
  return Serve(Resource::Particles, original, size, out, outSize, user);
}

bool TransformBullets(const void* original, uint64_t size, const void** out, uint64_t* outSize, void* user) {
  return Serve(Resource::Bullets, original, size, out, outSize, user);
}

}  // namespace

void Install(RegisterFn reg) {
  for (const Level& level : kLevels) {
    const std::string particles = std::string("netgun bullet effects ") + level.name + " CParticleEffectCR";
    const std::string bullets = std::string("netgun bullet effects ") + level.name + " CR15NetBulletCR";
    reg(kParticleType, level.hash, &TransformParticles, const_cast<Level*>(&level), particles.c_str());
    reg(kBulletType, level.hash, &TransformBullets, const_cast<Level*>(&level), bullets.c_str());
  }
  // The seam logs each transform it takes ("Registered transform: ..."); this line says what was asked of it.
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] netgun bullet effects requested levels=%zu transforms=%zu pins=%s,%s,%s (once a combat level "
      "loads, the magnum, smg, Chain and rifle bullets get particle effects if its resources are these stock files; "
      "any other bytes are served unchanged)",
      std::size(kLevels), 2 * std::size(kLevels), kParticleStockSha256, kParticleStockGaussSha256,
      kBulletStockSha256);
}

#ifdef NEVR_TEST_HOOKS
void TestHook_SetPins(const std::vector<std::string>& particles, const std::vector<std::string>& bullets) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_testPins[0] = particles;
  g_testPins[1] = bullets;
  g_testPinsSet = true;
  g_served.clear();
}

void TestHook_Reset() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_testPinsSet = false;
  g_served.clear();
}
#endif

}  // namespace nevr_netgun_bullets
