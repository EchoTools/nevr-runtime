#include "runtime/patch/netgun_bullets.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>

#include "core/logging.h"
#include "runtime/patch/asset_cache.h"
#include "runtime/patch/resource_override.h"

namespace nevr_netgun_bullets {
namespace {

// One served copy of the rewritten resource, built from the stock bytes the first time the game loads one of the five
// names. The stock bytes are the same for every name, so one copy answers all of them. It lives for the rest of the
// process: the game keeps reading the buffer after the callback returns.
struct Served {
  std::string stockSha256;
  void* bytes = nullptr;
  size_t size = 0;
};
std::mutex g_mutex;
Served g_served;

// The user pointer of each registration: the name the transform answers, for the log line.
struct Target {
  uint64_t name = 0;
};
Target g_targets[kNameCount];

bool Transform(const void* original, uint64_t size, const void** out, uint64_t* outSize, void* user) {
  const uint64_t name = static_cast<const Target*>(user)->name;
  if (original == nullptr || size == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.RESOURCE] netgun bullets left off name=0x%016llx reason=no_buffer bytes=%llu",
        static_cast<unsigned long long>(name), static_cast<unsigned long long>(size));
    return false;
  }
  const std::string sha = nevr_asset_cache::Sha256Hex(original, static_cast<size_t>(size));

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_served.bytes != nullptr && g_served.stockSha256 == sha) {
    *out = g_served.bytes;
    *outSize = g_served.size;
    return true;
  }

  const Result r = Patch(static_cast<const uint8_t*>(original), static_cast<size_t>(size), sha);
  if (r.verdict == Verdict::NotStock) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.RESOURCE] netgun bullets left off name=0x%016llx reason=not_stock (another build, or already rewritten)",
        static_cast<unsigned long long>(name));
    return false;
  }
  if (r.verdict == Verdict::Refused) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.RESOURCE] netgun bullets left off name=0x%016llx reason=%s (the stock resource is served unchanged)",
        static_cast<unsigned long long>(name), r.reason.c_str());
    return false;
  }

  void* const copy = VirtualAlloc(nullptr, r.bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (copy == nullptr) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.RESOURCE] netgun bullets left off name=0x%016llx reason=VirtualAlloc error=%lu",
        static_cast<unsigned long long>(name), GetLastError());
    return false;
  }
  std::memcpy(copy, r.bytes.data(), r.bytes.size());
  g_served.stockSha256 = sha;
  g_served.bytes = copy;
  g_served.size = r.bytes.size();

  const std::string resultSha = nevr_asset_cache::Sha256Hex(copy, g_served.size);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] netgun bullets rewritten name=0x%016llx entries=%zu assault_bullet=0x%016llx stock_sha256=%s "
      "result_sha256=%s matches_plugin_file=%s",
      static_cast<unsigned long long>(name), r.rewritten, static_cast<unsigned long long>(r.assaultBulletId), sha.c_str(),
      resultSha.c_str(), resultSha == kPluginFileSha256 ? "true" : "false");
  *out = copy;
  *outSize = g_served.size;
  return true;
}

}  // namespace

void Install() {
  for (size_t i = 0; i < kNameCount; ++i) {
    g_targets[i].name = kNames[i];
    RegisterResourceTransform(kResourceType, kNames[i], &Transform, &g_targets[i], "netgun bullets");
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.RESOURCE] netgun bullets enabled type=0x%016llx names=%zu pin=%s",
      static_cast<unsigned long long>(kResourceType), kNameCount, kStockSha256);
}

}  // namespace nevr_netgun_bullets
