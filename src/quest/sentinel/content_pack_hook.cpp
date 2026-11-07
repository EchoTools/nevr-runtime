#include "content_pack_hook.h"

#include <atomic>
#include <cstdint>

#include "data_root.h"
#include "hook_install.h"
#include "hook_log.h"
#include "quest/data/content_folder.h"

namespace nevr_quest::content_pack_hook {

namespace {

sentinel::GotHook g_hook;
std::atomic<bool> g_ran{false};

sentinel::LogLevel LevelOf(nevr_content::LineLevel level) noexcept {
  switch (level) {
    case nevr_content::LineLevel::kDebug: return sentinel::LogLevel::kDebug;
    case nevr_content::LineLevel::kInfo: return sentinel::LogLevel::kInfo;
    case nevr_content::LineLevel::kWarning: return sentinel::LogLevel::kWarn;
  }
  return sentinel::LogLevel::kWarn;
}

// One [NEVR.CONTENT] line, the PC's text, as one sentinel record.
void LogLine(nevr_content::LineLevel level, const char* line) noexcept {
  sentinel::LogFields(LevelOf(level), "content", {{"msg", line}});
}

// The served file's line, after the game's function returned.
void NoteServed(const nevr_content::Served& served, const char* path, int result) noexcept {
  char line[640];
  LogLine(content::ServedLine(served, path, result, line, sizeof(line)), line);
}

// CSysFile::Open. A read of a path the pack serves is handed the pack's held file; every other open is the game's, unchanged.
int Handler(OpenThunk::Fn original, const char* path, unsigned flags, long* handle) noexcept {
  const nevr_content::Served* const served = content::ServedFor(path, flags);
  if (served == nullptr) return original(path, flags, handle);
  const int result = original(served->serve_path.c_str(), flags, handle);
  NoteServed(*served, path, result);
  return result;
}

NEVR_HOOK_RECORD(kOpenHook, OpenThunk, &Handler);

// Puts the thunk on CSysFile::Open's slot: nullptr when it is in, else the GOT status.
const char* InstallHook() {
  OpenThunk::Arm(kOpenHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<OpenThunk>(g_hook, sentinel::pinned::LibR15CSysFileOpen());
  if (status == sentinel::GotStatus::kOk) return nullptr;
  OpenThunk::Disarm();
  return sentinel::GotStatusName(status);
}

// Whether the game has created its resource manager, and so read its manifests, for the libr15 that is loaded. No libr15: false
// (the hook cannot go in either, and says so).
bool DataReadAlready() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return false;
  return ResourceManagerExists(static_cast<std::uintptr_t>(image.base));
}

}  // namespace

bool ResourceManagerExists(std::uintptr_t image_base) noexcept {
  void* const* const manager = reinterpret_cast<void* const*>(image_base + sentinel::pinned::kResourceManagerPtrVaddr);
  return *manager != nullptr;
}

void RunIn(const std::string& engine_root, const std::string& content_dir) noexcept {
  content::Config config;
  config.content_dir = content_dir;
  config.engine_root = engine_root;
  config.data_read_already = DataReadAlready();
  config.install_hook = &InstallHook;
  config.log = &LogLine;
  content::Run(config);
}

void Run(const std::string& engine_root) noexcept {
  if (g_ran.exchange(true, std::memory_order_acq_rel)) return;
  RunIn(engine_root, content::ContentDir(data_root::kMediaRoot));
}

int HandleForTest(OpenThunk::Fn original, const char* path, unsigned flags, long* handle) noexcept {
  return Handler(original, path, flags, handle);
}

}  // namespace nevr_quest::content_pack_hook
