// The loader's platform on the device: the native library directory, dlopen, the running libr15's build ID and the
// sentinel's log. Compiles on the host as well (no Android header), where the test drives it against a temporary directory.
#pragma once

#include <optional>
#include <string>

#include "quest/plugin/plugin_loader.h"
#include "quest/sentinel/quest_config.h"

namespace nevr_quest::plugin {

// The directory part of `path` (no trailing slash); "" when there is none.
std::string DirectoryOf(const char* path);

// The sentinel log level a plugin's NevrLogLevel line goes out at; nullopt when it is dropped (DEBUG, unless `emitDebug`).
// The sentinel has no DEBUG level, so a development build's DEBUG lines go out at Info.
std::optional<nevr_quest::LogLevel> SinkLevel(std::uint32_t level, bool emitDebug);

class SentinelPlatform final : public Platform {
 public:
  // `libraryDir`: the native library directory (the directory of the sentinel's own mapping, ADR 0019 section 1).
  // `emitDebug`: whether a plugin's DEBUG lines are logged (a development build); they are dropped otherwise.
  SentinelPlatform(std::string libraryDir, bool emitDebug);

  bool ReadFile(const std::string& file, std::vector<std::uint8_t>* bytes) override;
  void* Open(const std::string& file) override;
  void Close(void* handle) override;
  void* Symbol(void* handle, const char* name) override;
  std::string GameBuildId() override;
  std::uintptr_t GameBase() override;
  void Emit(const LoadRecord& record) override;
  void EmitPlan(std::size_t entries, std::size_t early, std::size_t normal, std::size_t refused,
                std::size_t disabled) override;
  void PluginLine(std::uint32_t level, const std::string& line) override;
  // The input seam (input_seam_hook.h): a development build's API registers, a release build's refuses.
  const NevrQuestHostApi* QuestHostApi() override;

 private:
  std::string libraryDir_;
  bool emitDebug_;
};

}  // namespace nevr_quest::plugin
