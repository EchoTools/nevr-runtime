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

// Where the sentinel's platform reads the libr15 patch state from (ADR 0021 section 12). The sentinel's constructor sets
// it to a function that reads what `Attach` recorded; it is a pointer, not a call, so this library does not link the
// attach code. Never set (a host test, a build without the patch): every call reads "not verified", so the plugin
// identity is the live note. Returns the previous source.
using Libr15PatchSource = Libr15PatchState (*)();
Libr15PatchSource SetLibr15PatchSource(Libr15PatchSource source);

// Where the sentinel's platform reads the game's main level from (#1106). The sentinel sets it to a function that reads the
// game object through the frame hook's netgame and names the symbol (src/quest/sentinel/main_level.cpp). Same rule as the
// patch source above: a pointer, so this library does not link the frame dispatcher or the name table. Never set: the
// level is none. Returns the previous source.
using MainLevelSource = MainLevel (*)();
MainLevelSource SetMainLevelSource(MainLevelSource source);

// The result of reading one file of the library directory: read; not there (the normal case for an optional file); or there and
// not usable (a link, a directory, too large, a read error), with the reason as a token.
enum class ReadOutcome { kRead, kAbsent, kUnreadable };

class SentinelPlatform final : public Platform {
 public:
  // `libraryDir`: the native library directory (the directory of the sentinel's own mapping, ADR 0019 section 1).
  // `emitDebug`: whether a plugin's DEBUG lines are logged (a development build); they are dropped otherwise.
  SentinelPlatform(std::string libraryDir, bool emitDebug);

  // The game's main level from the source the sentinel set (SetMainLevelSource); none when no source is set.
  MainLevel CurrentMainLevel() override;
  bool ReadFile(const std::string& file, std::vector<std::uint8_t>* bytes) override;
  // ReadFile with the difference between "not there" and "not usable". `*why` is a reason token for kUnreadable, "" otherwise.
  ReadOutcome ReadFileOutcome(const std::string& file, std::vector<std::uint8_t>* bytes, const char** why);
  void* Open(const std::string& file) override;
  void Close(void* handle) override;
  void* Symbol(void* handle, const char* name) override;
  std::string GameBuildId() override;
  Libr15PatchState Libr15Patch() override;
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
