#include "runtime/patch/menu_cards.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>

#include "core/logging.h"
#include "runtime/patch/asset_cache.h"
#include "runtime/patch/resource_override.h"

namespace nevr_menu_cards {
namespace {

// The page with the cards, built once from the stock page the game delivered and served on every later load of
// it. The bytes live for the rest of the process: the game keeps reading the page it was given.
struct Served {
  std::string stockSha256;
  void* bytes = nullptr;
  size_t size = 0;
};
std::mutex g_mutex;
Served g_served;

bool Transform(const void* original, uint64_t size, const void** out, uint64_t* outSize, void* /*user*/) {
  if (original == nullptr || size == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] main-menu cards left off reason=no_page_buffer bytes=%llu",
        static_cast<unsigned long long>(size));
    return false;
  }
  const std::string sha = nevr_asset_cache::Sha256Hex(original, static_cast<size_t>(size));
  const bool pinned = sha == kStockPageSha256;
  Log(EchoVR::LogLevel::Info, "[NEVR.MENU] main-menu page seen bytes=%llu sha256=%s pin=%s",
      static_cast<unsigned long long>(size), sha.c_str(), pinned ? "match" : "mismatch");

  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_served.bytes != nullptr && g_served.stockSha256 == sha) {
    *out = g_served.bytes;
    *outSize = g_served.size;
    return true;
  }

  const Result r = Patch(static_cast<const uint8_t*>(original), static_cast<size_t>(size), sha, kStockPageSha256, Cards());
  if (r.verdict == Verdict::NotStock) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.MENU] main-menu cards left off reason=page_not_pinned (another game build, or a page already changed)");
    return false;
  }
  if (r.verdict == Verdict::Refused) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] main-menu cards left off reason=%s (the stock page is served unchanged)",
        nevr_menu_page::StatusName(r.status));
    return false;
  }

  void* const copy = VirtualAlloc(nullptr, r.bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (copy == nullptr) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] main-menu cards left off reason=VirtualAlloc failed error=%lu",
        GetLastError());
    return false;
  }
  std::memcpy(copy, r.bytes.data(), r.bytes.size());
  g_served.stockSha256 = sha;
  g_served.bytes = copy;
  g_served.size = r.bytes.size();

  const std::string resultSha = nevr_asset_cache::Sha256Hex(copy, g_served.size);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MENU] main-menu cards added page=%llu cards=%zu orig_bytes=%llu new_bytes=%zu result_sha256=%s",
      static_cast<unsigned long long>(kMainMenuPage), Cards().size(), static_cast<unsigned long long>(size),
      g_served.size, resultSha.c_str());
  *out = copy;
  *outSize = g_served.size;
  return true;
}

// The level's transform table with the four buttons placed in ButtonOrder() (#1679), built once and served on every later
// load of it, like the page.
Served g_table;

bool TransformSlots(const void* original, uint64_t size, const void** out, uint64_t* outSize, void* /*user*/) {
  if (original == nullptr || size == 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] main-menu buttons left unplaced reason=no_table_buffer bytes=%llu",
        static_cast<unsigned long long>(size));
    return false;
  }
  const std::string sha = nevr_asset_cache::Sha256Hex(original, static_cast<size_t>(size));
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_table.bytes != nullptr && g_table.stockSha256 == sha) {
    *out = g_table.bytes;
    *outSize = g_table.size;
    return true;
  }

  const SlotResult r = PlaceButtons(static_cast<const uint8_t*>(original), static_cast<size_t>(size), sha, kStockTransformSha256);
  if (r.verdict == Verdict::NotStock) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.MENU] main-menu buttons left unplaced reason=table_not_pinned bytes=%llu sha256=%s (another game build, or a "
        "table already changed: the cards keep the order the stock level gives them)",
        static_cast<unsigned long long>(size), sha.c_str());
    return false;
  }
  if (r.verdict == Verdict::Refused) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MENU] main-menu buttons left unplaced reason=%s (the stock table is served unchanged)",
        nevr_menu_slots::StatusName(r.status));
    return false;
  }

  void* const copy = VirtualAlloc(nullptr, r.bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (copy == nullptr) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] main-menu buttons left unplaced reason=VirtualAlloc failed error=%lu",
        GetLastError());
    return false;
  }
  std::memcpy(copy, r.bytes.data(), r.bytes.size());
  g_table.stockSha256 = sha;
  g_table.bytes = copy;
  g_table.size = r.bytes.size();

  // The slots are read back from the bytes being served, not copied from the plan: the line says where the game will
  // stand each button (left to right, name:slot@x), which is what the screen then shows.
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MENU] main-menu buttons placed bytes=%llu served_bytes=%zu slots_before=%s slots_after=%s (the game stands each "
      "card where its actor's transform says; slot 1 is the left)",
      static_cast<unsigned long long>(size), g_table.size, SlotLine(r.before).c_str(), SlotLine(r.after).c_str());
  *out = copy;
  *outSize = g_table.size;
  return true;
}

}  // namespace

void Install() {
  if (!RegisterResourceTransform(kPageType, kPageName, &Transform, nullptr, "main-menu cards")) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] main-menu cards left off reason=transform_not_registered");
    return;
  }
  if (!RegisterResourceTransform(kTransformType, kPageName, &TransformSlots, nullptr, "main-menu buttons")) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] main-menu buttons left unplaced reason=transform_not_registered");
  } else {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.MENU] main-menu buttons enabled type=0x%016llx name=0x%016llx pin=%s order=play,join,create,tutorial",
        static_cast<unsigned long long>(kTransformType), static_cast<unsigned long long>(kPageName),
        kStockTransformSha256);
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.MENU] main-menu cards enabled page=mnu_master type=0x%016llx name=0x%016llx pin=%s",
      static_cast<unsigned long long>(kPageType), static_cast<unsigned long long>(kPageName), kStockPageSha256);
}

}  // namespace nevr_menu_cards
