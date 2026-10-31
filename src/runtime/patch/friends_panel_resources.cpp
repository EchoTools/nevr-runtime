#include "runtime/patch/friends_panel_resources.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "runtime/patch/asset_cache.h"
#include "runtime/patch/friends_canvas.h"
#include "runtime/patch/friends_level.h"
#include "runtime/patch/friends_package.h"
#include "runtime/patch/resource_override.h"
#include "runtime/patch/ui_canvas.h"

namespace nevr_friends_panel_resources {
namespace {

namespace fc = nevr_friends_canvas;
namespace fl = nevr_friends_level;
namespace fp = nevr_friends_package;
namespace uc = nevr_ui_canvas;
using Bytes = std::vector<std::uint8_t>;

// ---- the pictures set ----

bool ReadFile(const std::string& path, Bytes* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  Bytes b;
  std::uint8_t chunk[65536];
  for (std::size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) b.insert(b.end(), chunk, chunk + n);
  std::fclose(f);
  *out = std::move(b);
  return true;
}

bool WriteFile(const std::string& path, const Bytes& bytes) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

// Creates every directory of `dir` that is missing.
void MakeDirectories(const std::string& dir) {
  for (std::size_t at = dir.find('\\', 3); at != std::string::npos; at = dir.find('\\', at + 1))
    CreateDirectoryA(dir.substr(0, at).c_str(), nullptr);
  CreateDirectoryA(dir.c_str(), nullptr);
}

// The panel's pictures: the names friends_package.h writes (rows a, b, c; the profile's a, b, c).
fc::Pictures Pictures() { return fc::StandardPictures(); }

// ---- transforms ----

std::uint64_t Digest(const void* data, std::uint64_t size) {
  std::uint64_t h = 0xCBF29CE484222325ULL;  // FNV-1a
  const auto* p = static_cast<const std::uint8_t*>(data);
  for (std::uint64_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ULL;
  return h ^ size;
}

using EditFn = bool (*)(const Bytes& in, Bytes* out, std::string* why);

struct Edit {
  const char* label;
  std::uint64_t type;
  std::uint64_t name;
  EditFn apply;
  std::mutex mutex;
  // Every answer served stays alive for the rest of the process (the game keeps reading it); the newest answers a
  // reload of the same stock bytes.
  std::deque<std::pair<std::uint64_t, Bytes>> served;
  bool logged = false;
};

bool BuildPanel(const Bytes& in, Bytes* out, std::string* why) {
  uc::Canvas season, panel;
  if (!uc::Decode(in.data(), in.size(), &season, why)) return false;
  if (!fc::BuildFriendsPanel(season, Pictures(), &panel, why)) return false;
  *out = uc::Encode(panel);
  return true;
}

bool BuildProfile(const Bytes& in, Bytes* out, std::string* why) {
  uc::Canvas popup, profile;
  if (!uc::Decode(in.data(), in.size(), &popup, why)) return false;
  if (!fc::BuildFriendProfile(popup, Pictures(), &profile, why)) return false;
  *out = uc::Encode(profile);
  return true;
}

// The main menu's transform table, with the four buttons placed first (#1679): the panel's transform is registered at
// boot, before the main-menu cards (boot.cpp), so it is the one that serves this table, and it serves both edits.
bool EditTransforms(const Bytes& in, Bytes* out, std::string* why) {
  bool placed = false;
  const bool ok = fl::EditTransformsWithButtons(in, nevr_asset_cache::Sha256Hex(in.data(), in.size()),
                                                nevr_menu_cards::kStockTransformSha256, out, why, &placed);
  Log(EchoVR::LogLevel::Info, "[NEVR.MENU] friends panel transforms: main-menu buttons %s",
      placed ? "placed (play, join, create, tutorial)" : "left in the stock order (the table is not the pinned stock one)");
  return ok;
}

constexpr std::uint64_t kCanvasType = EchoVR::CSymbol64Hash("CUICanvasResource");
static_assert(EchoVR::CSymbol64Hash("Win10", kCanvasType) == 0x59A9BD6E4525ECC4ULL, "the canvases' on-disk type");

std::array<Edit, 7> g_edits = {{
    {"friends panel canvas", kCanvasType, fc::kSeasonCanvas, &BuildPanel, {}, {}, false},
    {"friend profile canvas", kCanvasType, fc::kPopupCanvas, &BuildProfile, {}, {}, false},
    {"friends panel pointer meshes", fl::kPointerMeshType, fl::kLevel, &fl::EditPointerMeshes, {}, {}, false},
    {"friends panel transforms", fl::kTransformType, fl::kLevel, &EditTransforms, {}, {}, false},
    {"friends panel actor data", fl::kActorDataType, fl::kLevel, &fl::EditActorData, {}, {}, false},
    {"friends panel page elements", fl::kPageElementType, fl::kLevel, &fl::EditPageElements, {}, {}, false},
    {"friends panel scripts", fl::kScriptType, fl::kLevel, &fl::EditScripts, {}, {}, false},
}};

bool Transform(const void* original, std::uint64_t size, const void** out, std::uint64_t* out_size, void* user) {
  auto* e = static_cast<Edit*>(user);
  try {
    const std::uint64_t digest = Digest(original, size);
    std::lock_guard<std::mutex> lock(e->mutex);
    for (auto it = e->served.rbegin(); it != e->served.rend(); ++it) {
      if (it->first != digest) continue;
      *out = it->second.data();
      *out_size = it->second.size();
      return true;
    }
    const auto* p = static_cast<const std::uint8_t*>(original);
    const Bytes in(p, p + size);
    Bytes built;
    std::string why;
    const bool ok = e->apply(in, &built, &why);
    if (!e->logged) {
      e->logged = true;
      if (ok) {
        Log(EchoVR::LogLevel::Info, "[NEVR.MENU] %s built from the delivered resource bytes=%llu new_bytes=%zu", e->label,
            static_cast<unsigned long long>(size), built.size());
      } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.MENU] %s left stock reason=%s bytes=%llu (the friends panel does not show as built)", e->label,
            why.c_str(), static_cast<unsigned long long>(size));
      }
    }
    if (!ok) return false;
    e->served.emplace_back(digest, std::move(built));
    *out = e->served.back().second.data();
    *out_size = e->served.back().second.size();
    return true;
  } catch (const std::exception& error) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] %s failed reason=%s (stock bytes served)", e->label, error.what());
    return false;
  }
}

}  // namespace

bool WritePicturesSet(const std::string& data_root) {
  fp::PackageSet set;
  if (!fp::BuildFriendsPictures(fp::kWin10, fc::kRows, fc::kSlots, &set)) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] friends panel pictures set NOT written reason=build_failed");
    return false;
  }
  char id[17];
  std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(fp::kSetId));
  const std::string manifests = data_root + "\\rad15\\win10\\manifests", packages = data_root + "\\rad15\\win10\\packages";
  const std::array<std::pair<std::string, const Bytes*>, 2> files = {
      {{manifests + "\\" + id, &set.manifest}, {packages + "\\" + id + "_0", &set.package}}};
  int written = 0;
  for (const auto& f : files) {
    Bytes there;
    if (ReadFile(f.first, &there) && there == *f.second) continue;
    MakeDirectories(f.first.substr(0, f.first.rfind('\\')));
    if (!WriteFile(f.first, *f.second)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.MENU] friends panel pictures set NOT written path=%s error=%lu", f.first.c_str(),
          GetLastError());
      return false;
    }
    ++written;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.MENU] friends panel pictures set ready root=%s files_written=%d manifest_bytes=%zu package_bytes=%zu",
      data_root.c_str(), written, set.manifest.size(), set.package.size());
  return true;
}

int RegisterTransforms() {
  int registered = 0;
  for (Edit& e : g_edits) registered += RegisterResourceTransform(e.type, e.name, &Transform, &e, e.label) ? 1 : 0;
  Log(registered == static_cast<int>(g_edits.size()) ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.MENU] friends panel transforms registered=%d of %zu", registered, g_edits.size());
  return registered;
}

}  // namespace nevr_friends_panel_resources
