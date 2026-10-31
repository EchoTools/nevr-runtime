#include "texture_menu_hook.h"

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "outside_game_call.h"
#include "texture_menu.h"

namespace nevr_quest::texture_menu {

namespace {

namespace tq = nevr_quest::texture_quality;
namespace pin = sentinel::pinned;
using tq::Level;

constexpr int kUnset = tq::kLevelUnset;

// ---- state --------------------------------------------------------------------------------------------------------------

sentinel::GotHook g_fileHook, g_textHook, g_validateHook, g_applyHook, g_checkHook;

std::atomic<pin::RefreshRTSizeFn> g_refreshRtSize{nullptr};
std::atomic<pin::VrFlagFn> g_vrInitialized{nullptr};
std::atomic<pin::VrFlagFn> g_vrGraphicsInitialized{nullptr};
std::atomic<pin::VrFlagFn> g_vrDisplayLost{nullptr};
std::atomic<pin::LocalAppDataDirectoryFn> g_localAppData{nullptr};
std::atomic<pin::EngineResizeFn> g_engineResize{nullptr};
std::atomic<PropertyReader> g_property{nullptr};

// The index the control starts on when the player has not chosen: the configuration's level (`stock` shows as Low).
std::atomic<int> g_defaultIndex{tq::kDefaultMenuIndex};
// What the start resolver found: 0 not asked, 1 the player's marker file, 2 the configuration, 3 the reference input's rule.
enum StartSource : int { kStartNotAsked = 0, kStartPlayerMarker = 1, kStartConfig = 2, kStartReferenceRule = 3 };
std::atomic<int> g_startSource{kStartNotAsked};
std::atomic<int> g_startLevel{kUnset};
std::atomic<bool> g_playerSaved{false};
std::atomic<bool> g_firstDelivery{false};
// The index of the last delivery, and the index the player changed the control to that the poller has not yet written to the
// marker file (kUnset: none). A delivery whose index differs from the one before it is a change in the menu: the first delivery
// is the load, and the game redelivers an unchanged setting when another setting changes.
std::atomic<int> g_lastIndex{kUnset};
std::atomic<int> g_pendingMarker{kUnset};
// The marker file's path, built once by the start resolver on the game's thread and read by the poller after g_markerPathReady.
char g_markerPath[704];
std::atomic<bool> g_markerPathReady{false};
std::atomic<std::uint64_t> g_markerWrites{0};
std::atomic<std::uint64_t> g_markerFailures{0};

std::atomic<std::uint64_t> g_deliveries{0};
std::atomic<std::uint64_t> g_menuLevel{0};      // the Level of the last delivery
std::atomic<std::uint64_t> g_appliedLevel{0};   // the applied Level, mirrored here for the reporter
std::atomic<std::uint64_t> g_applies{0};
std::atomic<std::uint64_t> g_equalCommits{0};
std::atomic<std::uint64_t> g_refused{0};
std::atomic<std::uint64_t> g_failed{0};
std::atomic<std::uint64_t> g_jsonRewrites{0};
std::atomic<std::uint64_t> g_configOverrides{0};
std::atomic<bool> g_disabled{false};
std::atomic<const char*> g_disabledWhy{"none"};
std::atomic<int> g_lastRc{0};
std::atomic<int> g_lastFrom{kUnset};
std::atomic<int> g_lastTo{kUnset};

// Per document: what the last rewrite did (0 none, else texture_menu::Status + 1) and its counts.
enum Document : int { kNone = 0, kMenuSettings = 1, kGameSettings = 2 };
constexpr int kDocuments = 3;
std::atomic<int> g_jsonStatus[kDocuments];
std::atomic<int> g_jsonRows[kDocuments];
std::atomic<int> g_jsonPages[kDocuments];

// The document each thread is inside the file read of (kNone: none). Not a thread_local: the .so would get emulated TLS on the hook
// path (TestNoEmulatedTLSInHookPath). The slots are keyed by pthread_t under a pthread mutex, the sentinel's own lock convention.
struct ArmedSlot {
  pthread_t thread{};
  int document = kNone;
  bool used = false;
};
constexpr int kArmedSlots = 16;
pthread_mutex_t g_armedMutex = PTHREAD_MUTEX_INITIALIZER;
ArmedSlot g_armed[kArmedSlots];

// Under g_armedMutex.
ArmedSlot* FindArmed(pthread_t self) noexcept {
  for (ArmedSlot& slot : g_armed) {
    if (slot.used && pthread_equal(slot.thread, self)) return &slot;
  }
  return nullptr;
}

// Under g_armedMutex. A full table leaves the thread unarmed: the game parses what it read, unrewritten.
ArmedSlot* FirstFreeArmed() noexcept {
  for (ArmedSlot& slot : g_armed) {
    if (!slot.used) return &slot;
  }
  return nullptr;
}

// Arms the calling thread for `document`; kNone disarms it.
void SetArmed(int document) noexcept {
  const pthread_t self = pthread_self();
  pthread_mutex_lock(&g_armedMutex);
  ArmedSlot* slot = FindArmed(self);
  if (document == kNone) {
    if (slot != nullptr) slot->used = false;
  } else {
    if (slot == nullptr) slot = FirstFreeArmed();
    if (slot != nullptr) {
      slot->thread = self;
      slot->document = document;
      slot->used = true;
    }
  }
  pthread_mutex_unlock(&g_armedMutex);
}

// The document the calling thread is armed for, and disarms it (kNone: not armed).
int TakeArmed() noexcept {
  const pthread_t self = pthread_self();
  int document = kNone;
  pthread_mutex_lock(&g_armedMutex);
  if (ArmedSlot* slot = FindArmed(self)) {
    document = slot->document;
    slot->used = false;
  }
  pthread_mutex_unlock(&g_armedMutex);
  return document;
}

// ---- the two documents --------------------------------------------------------------------------------------------------

constexpr unsigned long long kMaxDocumentBytes = 1U << 20;

bool EndsWithPath(const char* path, const char* suffix) noexcept {
  const std::size_t n = std::strlen(path);
  const std::size_t m = std::strlen(suffix);
  if (n < m) return false;
  for (std::size_t i = 0; i < m; ++i) {
    char c = path[n - m + i];
    if (c == '|' || c == '\\') c = '/';  // the game's own separators, before and after it resolves them
    if (c != suffix[i]) return false;
  }
  return true;
}

int DocumentForPath(const char* path) noexcept {
  if (path == nullptr) return kNone;
  if (EndsWithPath(path, "/json/r14/config/uisettings/menu_settings.json")) return kMenuSettings;
  if (EndsWithPath(path, "/json/r14/config/gamesettings_config.json")) return kGameSettings;
  return kNone;
}

std::uint64_t Fnv1a(std::string_view text) noexcept {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char c : text) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

// The rewritten text of each document, kept for the life of the process: the parser may keep pointers into what it was given, and
// a document is read once. A second read of the same text (same length and hash) is served from here. Heap objects that are
// never freed, so no static destructor runs under a game thread.
struct Kept {
  std::uint64_t hash = 0;
  std::size_t inputSize = 0;
  const std::string* text = nullptr;
};
// std::mutex is out: its unlock carries a personality on the hook path. A pthread mutex with a constant initializer has no
// destructor, so it adds no _GLOBAL__sub_I initializer (tools/check_quest_static_init.sh).
pthread_mutex_t g_keptMutex = PTHREAD_MUTEX_INITIALIZER;
Kept g_kept[kDocuments];

struct Served {
  const char* data = nullptr;
  std::size_t size = 0;
};

// Rewrites the document the game has just read, or says nothing (data == nullptr) and the game parses what it read. Runs before
// any game code (the caller calls the original after it returns), so it may allocate and carry a personality
// (outside_game_call.h): it calls no game code, and nothing leaves it (it is noexcept; its work is texture_menu.cpp, which parses
// with no throwing call).
NEVR_OUTSIDE_GAME_CALL Served RewriteDocument(int document, const char* text, unsigned long long length) noexcept {
  if (document <= kNone || document >= kDocuments || text == nullptr || length == 0 || length > kMaxDocumentBytes) return {};
  const std::string_view input(text, static_cast<std::size_t>(length));
  const std::uint64_t hash = Fnv1a(input);
  pthread_mutex_lock(&g_keptMutex);
  const Kept kept = g_kept[document];
  pthread_mutex_unlock(&g_keptMutex);
  if (kept.text != nullptr && kept.hash == hash && kept.inputSize == input.size()) return {kept.text->data(), kept.text->size()};
  const texture_menu::Result r = document == kMenuSettings
                                     ? texture_menu::RewriteMenuSettings(input, g_defaultIndex.load(std::memory_order_acquire))
                                     : texture_menu::RewriteGameSettings(input);
  g_jsonRows[document].store(r.rows, std::memory_order_relaxed);
  g_jsonPages[document].store(r.pages, std::memory_order_relaxed);
  g_jsonStatus[document].store(static_cast<int>(r.status) + 1, std::memory_order_release);
  if (r.status != texture_menu::Status::kRewritten) return {};
  const std::string* const out = new std::string(r.text);
  pthread_mutex_lock(&g_keptMutex);
  g_kept[document] = Kept{hash, input.size(), out};
  pthread_mutex_unlock(&g_keptMutex);
  g_jsonRewrites.fetch_add(1, std::memory_order_relaxed);
  return {out->data(), out->size()};
}

// ---- the start level: what the player chose -----------------------------------------------------------------------------

// Called by the eye-size hook on its first size query, before the game has delivered its settings. Reads the player's choice from
// the marker file (texture_menu.h): it sits in the directory the game keeps its settings in (GetLocalAppDataDirectory, then
// "RAD/LoneEcho"), and exists only once the player has changed the control. The game's own saved slot is not read: a settings file
// that never held the setting reads 0, which is Very Low, and cannot tell "chose Very Low" from "never chose". Returns the Level to
// start at, or kUnset (no marker, nothing readable): the configuration's level then stands. It runs inside the game's call: no
// logging, no exceptions.
// The reference input's start rule (#1737 C4). The reference libr15 starts at the level its own settings file names: the `msaa`
// index V there (his cave's rule, read by tq::ReferenceIndexFromSettings), at level kLow + V. It applies only on the reference input,
// after the player's marker has been looked for and found nothing, and only when the configuration is `stock`. The level is queued
// once as the player's marker, so the next launch reads it from there and this file is not read again. `directory` is the settings
// directory (nullptr: none, which reads as a missing file). It runs inside the game's call: no logging.
char g_referenceText[tq::kReferenceReadCap + 1];
std::atomic<int> g_referenceIndex{kUnset};
std::atomic<const char*> g_referenceFile{"none"};

int ResolveReference(const char* directory) noexcept {
  if (tq::InputKind() != tq::EyeInput::kReference || tq::LatchedLevel() != Level::kStock) return kUnset;
  int index = tq::kReferenceDefaultIndex;
  const char* state = "missing";
  if (directory != nullptr) {
    char path[600];  // %.512s plus the suffix: the directory is at most 512 characters, as the marker path's is
    std::snprintf(path, sizeof(path), "%.512s/RAD/LoneEcho/settings_mp_v2_quest.json", directory);
    std::FILE* const file = std::fopen(path, "r");
    if (file != nullptr) {
      const std::size_t size = std::fread(g_referenceText, 1, tq::kReferenceReadCap, file);
      std::fclose(file);
      bool sawKey = false;
      index = tq::ReferenceIndexFromSettings(g_referenceText, size, &sawKey);
      state = sawKey ? "read" : "no_key";
    }
  }
  const Level level = tq::LevelForReferenceIndex(index);
  g_referenceIndex.store(index, std::memory_order_release);
  g_referenceFile.store(state, std::memory_order_release);
  g_playerSaved.store(true, std::memory_order_release);
  g_startSource.store(kStartReferenceRule, std::memory_order_release);
  g_startLevel.store(static_cast<int>(level), std::memory_order_release);
  g_appliedLevel.store(static_cast<std::uint64_t>(level), std::memory_order_relaxed);
  g_pendingMarker.store(tq::MenuIndexForLevel(level), std::memory_order_release);
  return static_cast<int>(level);
}

int ResolveStart() noexcept {
  g_startSource.store(kStartConfig, std::memory_order_release);
  const pin::LocalAppDataDirectoryFn directory = g_localAppData.load(std::memory_order_acquire);
  if (directory == nullptr) return ResolveReference(nullptr);
  alignas(8) char fixedString[1024];  // a CFixedStringT<512>: its characters come first
  std::memset(fixedString, 0, sizeof(fixedString));
  directory(fixedString);
  fixedString[512] = '\0';
  if (fixedString[0] == '\0') return ResolveReference(nullptr);
  // fixedString holds at most 512 characters (its last byte is the NUL above), so the precision bounds the directory and
  // the suffix fits: a longer directory cannot truncate the path silently.
  static_assert(512 + sizeof("/RAD/LoneEcho/") + sizeof(texture_menu::kMarkerFile) <= sizeof(g_markerPath), "marker path buffer fits the directory");
  std::snprintf(g_markerPath, sizeof(g_markerPath), "%.512s/RAD/LoneEcho/%s", fixedString, texture_menu::kMarkerFile);
  g_markerPathReady.store(true, std::memory_order_release);
  std::FILE* const file = std::fopen(g_markerPath, "r");
  if (file == nullptr) return ResolveReference(fixedString);
  char buffer[256];  // the marker is {"menu_index":N}
  const std::size_t size = std::fread(buffer, 1, sizeof(buffer), file);
  std::fclose(file);
  const int index = texture_menu::MarkedMenuIndex(buffer, size);
  if (index < 0) return ResolveReference(fixedString);
  g_playerSaved.store(true, std::memory_order_release);
  g_startSource.store(kStartPlayerMarker, std::memory_order_release);
  const Level level = tq::LevelForMenuIndex(index);
  g_startLevel.store(static_cast<int>(level), std::memory_order_release);
  g_appliedLevel.store(static_cast<std::uint64_t>(level), std::memory_order_relaxed);
  return static_cast<int>(level);
}

// ---- the five handlers --------------------------------------------------------------------------------------------------

using FileThunk = pin::LibR15JsonDecodeFromFileThunk;
using TextThunk = pin::LibR15JsonDecodeFromTextThunk;
using ValidateThunk = pin::LibR15ValidateGfxSettingsThunk;
using ApplyThunk = pin::LibR15ApplyRuntimeGfxSettingsThunk;
using CheckThunk = pin::LibR15CheckForResizeThunk;

template <typename T>
T LoadAt(const void* base, std::size_t offset) noexcept {
  T value;
  std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
  return value;
}
template <typename T>
void StoreAt(void* base, std::size_t offset, T value) noexcept {
  std::memcpy(static_cast<unsigned char*>(base) + offset, &value, sizeof(value));
}

// CJson::DecodeFrom(CFileSp const&, uint): arms the thread for the two documents for the length of the call.
int HandleDecodeFile(FileThunk::Fn original, pin::CJsonOpaque* self, const char* path, std::uint32_t strip) noexcept {
  SetArmed(DocumentForPath(path));
  const int result = original(self, path, strip);
  SetArmed(kNone);
  return result;
}

// CJson::DecodeFrom(char const*, unsigned long long): the parse. On a thread armed by the handler above, the first text it parses
// is the document, and the game parses the rewrite of it. The thread is disarmed at once: a second parse in the same file read
// (there is none) would be left alone.
int HandleDecodeText(TextThunk::Fn original, pin::CJsonOpaque* self, const char* text, unsigned long long length) noexcept {
  const int document = TakeArmed();
  if (document == kNone) return original(self, text, length);
  const Served served = RewriteDocument(document, text, length);
  if (served.data == nullptr) return original(self, text, length);
  return original(self, served.data, served.size);
}

// ValidateGfxSettings: the game clamps `graphics|msaa` to 0..2. The value the settings held, clamped to the menu's 0..5, goes back.
void HandleValidate(ValidateThunk::Fn original, unsigned char* settings) noexcept {
  if (settings == nullptr) {
    original(settings);
    return;
  }
  const std::int32_t held = LoadAt<std::int32_t>(settings, pin::gfx_layout::kMsaa);
  original(settings);
  const std::int32_t last = tq::kMenuLevelCount - 1;
  StoreAt<std::int32_t>(settings, pin::gfx_layout::kMsaa, held < 0 ? 0 : (held > last ? last : held));
}

// ApplyRuntimeGfxSettings: the game's settings, delivered at boot and on every change. `graphics|msaa` is the control.
void HandleApply(ApplyThunk::Fn original, const unsigned char* settings, void* game, std::uint32_t refreshUi) noexcept {
  if (settings != nullptr) {
    g_deliveries.fetch_add(1, std::memory_order_relaxed);
    unsigned char* const field = const_cast<unsigned char*>(settings) + pin::gfx_layout::kMsaa;
    std::int32_t index = LoadAt<std::int32_t>(field, 0);
    // The first delivery is the load. What the game holds in the slot is not the player's choice (a settings file that never held
    // the setting reads 0, Very Low): the player's marker is, and with none the default (the configuration's level, Low for
    // `stock`). The settings are changed to it, so the control shows it and the next save keeps it.
    if (!g_firstDelivery.exchange(true, std::memory_order_acq_rel)) {
      const bool marked = g_playerSaved.load(std::memory_order_acquire);
      const int start = marked ? tq::MenuIndexForLevel(static_cast<Level>(g_startLevel.load(std::memory_order_acquire)))
                               : g_defaultIndex.load(std::memory_order_acquire);
      if (index != start) {
        StoreAt<std::int32_t>(field, 0, start);
        index = start;
        if (!marked) g_configOverrides.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // Every later delivery with another index is the player's choice in the menu: it is kept for the next launch.
    const int previous = g_lastIndex.exchange(index, std::memory_order_acq_rel);
    if (previous != kUnset && previous != index) g_pendingMarker.store(index, std::memory_order_release);
    const Level level = tq::LevelForMenuIndex(index);
    g_menuLevel.store(static_cast<std::uint64_t>(level), std::memory_order_relaxed);
    tq::RequestLevel(level, tq::RequestSource::kMenu);
  }
  original(settings, game, refreshUi);
}

void RefuseForGood(const char* why) noexcept {
  g_disabledWhy.store(why, std::memory_order_release);
  g_disabled.store(true, std::memory_order_release);
  g_refused.fetch_add(1, std::memory_order_relaxed);
}

// The apply, on the render thread, after the engine's own per-frame check. Everything it calls is a function the engine calls
// itself at this point of the frame.
void ApplyPending(void* engine) noexcept {
  const int wanted = tq::RequestedLevelValue();
  if (wanted == kUnset || !tq::IsLatched() || g_disabled.load(std::memory_order_acquire)) return;
  const Level want = static_cast<Level>(wanted);
  const Level have = tq::LatchedLevel();
  if (want == have) return;

  // Two levels that give the same eye size need no rebuild (`stock` and Low on one headset).
  const tq::Counts counts = tq::CurrentCounts();
  if (counts.nativeWidth != 0 && counts.nativeHeight != 0) {
    const tq::EyeSize native{static_cast<int>(counts.nativeWidth), static_cast<int>(counts.nativeHeight)};
    const tq::EyeSize from = tq::EffectiveSize(have, native);
    const tq::EyeSize to = tq::EffectiveSize(want, native);
    if (from.width == to.width && from.height == to.height) {
      tq::CommitApplied(want);
      g_appliedLevel.store(static_cast<std::uint64_t>(want), std::memory_order_relaxed);
      g_equalCommits.fetch_add(1, std::memory_order_relaxed);
      g_lastFrom.store(static_cast<int>(have), std::memory_order_relaxed);
      g_lastTo.store(static_cast<int>(want), std::memory_order_relaxed);
      return;
    }
  }

  // The eye size is asked through the eye-size route when it is in place (the reference input's RefreshRTSize hook), else through
  // the game's own function. The route is looked up here, not at install, so either install order gives the same answer.
  const pin::RefreshRTSizeFn routed = tq::HookedRefresh();
  const pin::RefreshRTSizeFn refresh = routed != nullptr ? routed : g_refreshRtSize.load(std::memory_order_acquire);
  const pin::VrFlagFn initialized = g_vrInitialized.load(std::memory_order_acquire);
  const pin::VrFlagFn graphicsInitialized = g_vrGraphicsInitialized.load(std::memory_order_acquire);
  const pin::VrFlagFn displayLost = g_vrDisplayLost.load(std::memory_order_acquire);
  const pin::EngineResizeFn expected = g_engineResize.load(std::memory_order_acquire);
  if (refresh == nullptr || initialized == nullptr || graphicsInitialized == nullptr || displayLost == nullptr ||
      expected == nullptr || engine == nullptr) {
    RefuseForGood("game_functions_missing");
    return;
  }
  // Not yet: the request stays pending and is looked at again next frame.
  if ((LoadAt<std::uint8_t>(engine, pin::engine_layout::kReadyFlags) & pin::engine_layout::kReadyBit) == 0) return;
  if (initialized() == 0 || graphicsInitialized() == 0 || displayLost() != 0) return;
  // The engine's resize slot must be CEngine::Resize itself; anything else is a different engine than the one read.
  void* const* const table = LoadAt<void* const*>(engine, 0);
  void* const slot = table != nullptr ? table[pin::engine_layout::kResizeVtableOffset / sizeof(void*)] : nullptr;
  pin::EngineResizeFn resize = nullptr;
  static_assert(sizeof(resize) == sizeof(slot), "function pointer size");
  std::memcpy(&resize, &slot, sizeof(resize));
  if (resize == nullptr || resize != expected) {
    RefuseForGood("resize_slot_is_not_engine_resize");
    return;
  }
  const std::uint32_t displayWidth = LoadAt<std::uint32_t>(engine, pin::engine_layout::kDisplayWidth);
  const std::uint32_t displayHeight = LoadAt<std::uint32_t>(engine, pin::engine_layout::kDisplayHeight);
  const std::int32_t displayMode = LoadAt<std::int32_t>(engine, pin::engine_layout::kDisplayMode);

  // CCanvas::Resize resizes the render-target manager before CVR::Resize refreshes the eye size, so the manager would be built
  // for the previous level. The eye size is refreshed first (it only writes CVR's size, and the eye-size hook answers it with the
  // level committed here).
  const unsigned long long queriesBefore = tq::SizeQueries();
  tq::CommitApplied(want);
  refresh();
  if (tq::SizeQueries() == queriesBefore) {
    // The stored size override is set: the runtime is never asked, so no level can apply. Put everything back.
    tq::CommitApplied(have);
    refresh();
    RefuseForGood("eye_size_override_is_set");
    return;
  }
  g_appliedLevel.store(static_cast<std::uint64_t>(want), std::memory_order_relaxed);
  g_lastFrom.store(static_cast<int>(have), std::memory_order_relaxed);
  g_lastTo.store(static_cast<int>(want), std::memory_order_relaxed);
  const int rc = resize(engine, displayWidth, displayHeight, displayMode);
  g_lastRc.store(rc, std::memory_order_relaxed);
  if (rc == 0) {
    g_applies.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // The engine could not rebuild at the new level: back to the level its buffers were built for, and the failed one is not
  // asked for again.
  g_failed.fetch_add(1, std::memory_order_relaxed);
  tq::CommitApplied(have);
  g_appliedLevel.store(static_cast<std::uint64_t>(have), std::memory_order_relaxed);
  tq::RequestLevel(have, tq::RequestSource::kMenu);
  refresh();
  const int back = resize(engine, displayWidth, displayHeight, displayMode);
  if (back != 0) g_failed.fetch_add(1, std::memory_order_relaxed);
}

// CEngine::CheckForResize: the engine's own per-frame check runs unchanged and its result is returned as it came.
std::uint64_t HandleCheck(CheckThunk::Fn original, void* engine) noexcept {
  const std::uint64_t result = original(engine);
  ApplyPending(engine);
  return result;
}

NEVR_HOOK_RECORD(kFileHook, FileThunk, &HandleDecodeFile);
NEVR_HOOK_RECORD(kTextHook, TextThunk, &HandleDecodeText);
NEVR_HOOK_RECORD(kValidateHook, ValidateThunk, &HandleValidate);
NEVR_HOOK_RECORD(kApplyHook, ApplyThunk, &HandleApply);
NEVR_HOOK_RECORD(kCheckHook, CheckThunk, &HandleCheck);

// ---- the poller: the only place this feature logs -----------------------------------------------------------------------

struct Logged {
  bool start = false;
  int request = kUnset;
  int applied = kUnset;
  bool json[kDocuments] = {false, false, false};
  unsigned long long refused = 0;
  unsigned long long failed = 0;
  // The last eye-size decision written (texture_quality_eye): its source, width and height.
  int eyeSource = -1;
  int eyeW = -1;
  int eyeH = -1;
};
Logged g_logged;
std::atomic<int> g_propertyLast{kUnset};

const char* DocumentName(int document) noexcept {
  return document == kMenuSettings ? "menu_settings" : (document == kGameSettings ? "gamesettings_config" : "unknown");
}

bool ParsePropertyLevel(const char* value, Level* out) noexcept {
  if (tq::ParseLevel(value, out)) return true;
  if (value[0] >= '0' && value[0] <= '9' && value[1] == '\0') {
    *out = tq::LevelForMenuIndex(value[0] - '0');
    return value[0] - '0' < tq::kMenuLevelCount;
  }
  return false;
}

#if defined(__ANDROID__)
int AndroidProperty(const char* name, char* out) noexcept { return __system_property_get(name, out); }
#endif

// Writes the player's last choice in the menu to the marker file, through a temporary file and a rename so a reader never sees half
// of it. Runs on the poller thread, never inside a game call. A failed write is logged with its errno; the choice then lasts
// until the game exits. Returns the number of lines logged.
int WriteMarker() noexcept {
  const int index = g_pendingMarker.exchange(kUnset, std::memory_order_acq_rel);
  if (index == kUnset) return 0;
  const char* const consequence = "this choice is not remembered at the next launch";
  if (!g_markerPathReady.load(std::memory_order_acquire)) {
    g_markerFailures.fetch_add(1, std::memory_order_relaxed);
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_saved",
                        {{"result", "no_directory"}, {"menu_index", index}, {"consequence", consequence}});
    return 1;
  }
  char temporary[sizeof(g_markerPath) + 8];
  std::snprintf(temporary, sizeof(temporary), "%s.tmp", g_markerPath);
  const std::string text = texture_menu::MarkerText(index);
  int error = 0;
  std::FILE* const file = std::fopen(temporary, "w");
  if (file == nullptr) {
    error = errno;
  } else {
    const bool wrote = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed) error = errno != 0 ? errno : EIO;
  }
  if (error == 0 && std::rename(temporary, g_markerPath) != 0) error = errno;
  if (error != 0) {
    std::remove(temporary);
    g_markerFailures.fetch_add(1, std::memory_order_relaxed);
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_saved",
                        {{"result", "write_failed"}, {"menu_index", index}, {"errno", error}, {"consequence", consequence}});
    return 1;
  }
  g_markerWrites.fetch_add(1, std::memory_order_relaxed);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_saved",
                      {{"result", "written"},
                       {"quality", tq::LevelName(tq::LevelForMenuIndex(index))},
                       {"menu_index", index},
                       {"consequence", "the next launch starts at this level"}});
  return 1;
}

int PollLocked() noexcept {
  int lines = 0;
  if (const PropertyReader property = g_property.load(std::memory_order_acquire)) {
    char value[92] = {};
    if (property(kLevelProperty, value) > 0) {
      Level level = Level::kStock;
      if (ParsePropertyLevel(value, &level) && static_cast<int>(level) != g_propertyLast.load(std::memory_order_relaxed)) {
        g_propertyLast.store(static_cast<int>(level), std::memory_order_relaxed);
        tq::RequestLevel(level, tq::RequestSource::kProperty);
      }
    }
  }
  const int source = g_startSource.load(std::memory_order_acquire);
  if (!g_logged.start && source != kStartNotAsked) {
    g_logged.start = true;
    const int start = g_startLevel.load(std::memory_order_acquire);
    const Level startLevel = start == kUnset ? tq::LatchedLevel() : static_cast<Level>(start);
    const char* const sourceName = source == kStartPlayerMarker ? "player_marker"
                                   : source == kStartReferenceRule ? "reference_rule"
                                                                   : "config";
    const int referenceIndex = g_referenceIndex.load(std::memory_order_acquire);
    const sentinel::LogField fields[] = {
        {"source", sourceName},
        {"input", tq::InputName(tq::InputKind())},
        {"quality", tq::LevelName(startLevel)},
        {"menu_index", tq::MenuIndexForLevel(startLevel)},
        {"reference_index", referenceIndex < 0 ? 0 : referenceIndex},
        {"reference_file", g_referenceFile.load(std::memory_order_acquire)},
        {"consequence", "the level is saved as the player's choice so the next launch reads it from there"}};
    // The reference fields ride on the reference rule's line only (the first four fields are the same on every source).
    sentinel::LogFieldList(sentinel::LogLevel::kInfo, "texture_quality_start", fields,
                           source == kStartReferenceRule ? 7U : 4U);
    ++lines;
  }
  for (int document = kMenuSettings; document < kDocuments; ++document) {
    const int status = g_jsonStatus[document].load(std::memory_order_acquire);
    if (status == 0 || g_logged.json[document]) continue;
    g_logged.json[document] = true;
    const bool ok = status - 1 == static_cast<int>(texture_menu::Status::kRewritten);
    sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "texture_menu_json",
                        {{"document", DocumentName(document)},
                         {"result", texture_menu::StatusName(static_cast<texture_menu::Status>(status - 1))},
                         {"rows", g_jsonRows[document].load(std::memory_order_relaxed)},
                         {"pages", g_jsonPages[document].load(std::memory_order_relaxed)}});
    ++lines;
  }
  lines += WriteMarker();
  const int requested = tq::RequestedLevelValue();
  if (requested != kUnset && requested != g_logged.request) {
    const int from = g_logged.request == kUnset ? static_cast<int>(tq::LatchedLevel()) : g_logged.request;
    g_logged.request = requested;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_request",
                        {{"source", tq::LastRequestSource() == tq::RequestSource::kProperty ? "property" : "menu"},
                         {"from", tq::LevelName(static_cast<Level>(from))},
                         {"to", tq::LevelName(static_cast<Level>(requested))}});
    ++lines;
  }
  // The eye size the last RefreshRTSize stored, when it differs from the one written before (the actual size, per the route).
  const tq::Decision decision = tq::LastDecision();
  if (decision.source != tq::DecisionSource::kNone &&
      (static_cast<int>(decision.source) != g_logged.eyeSource || decision.w != g_logged.eyeW || decision.h != g_logged.eyeH)) {
    g_logged.eyeSource = static_cast<int>(decision.source);
    g_logged.eyeW = decision.w;
    g_logged.eyeH = decision.h;
    const tq::Counts counts = tq::CurrentCounts();
    sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_eye",
                        {{"input", tq::InputName(tq::InputKind())},
                         {"quality", tq::LevelName(tq::LatchedLevel())},
                         {"native_w", counts.nativeWidth},
                         {"native_h", counts.nativeHeight},
                         {"w", decision.w},
                         {"h", decision.h},
                         {"source", tq::DecisionSourceName(decision.source)}});
    ++lines;
  }
  const int applied = tq::IsLatched() ? static_cast<int>(tq::LatchedLevel()) : kUnset;
  if (applied != kUnset && applied != g_logged.applied) {
    if (g_logged.applied != kUnset) {
      const tq::Counts counts = tq::CurrentCounts();
      const tq::EyeSize size = tq::EffectiveSize(static_cast<Level>(applied),
                                                 tq::EyeSize{static_cast<int>(counts.nativeWidth), static_cast<int>(counts.nativeHeight)});
      sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_applied",
                          {{"from", tq::LevelName(static_cast<Level>(g_logged.applied))},
                           {"to", tq::LevelName(static_cast<Level>(applied))},
                           {"w", size.width},
                           {"h", size.height},
                           {"rc", g_lastRc.load(std::memory_order_relaxed)},
                           {"applies", static_cast<long long>(g_applies.load(std::memory_order_relaxed))},
                           {"equal_size_commits", static_cast<long long>(g_equalCommits.load(std::memory_order_relaxed))}});
      ++lines;
    }
    g_logged.applied = applied;
  }
  const unsigned long long refused = g_refused.load(std::memory_order_relaxed);
  if (refused != g_logged.refused) {
    g_logged.refused = refused;
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_apply_refused",
                        {{"why", g_disabledWhy.load(std::memory_order_acquire)},
                         {"consequence", "a level change now applies at the next launch"}});
    ++lines;
  }
  const unsigned long long failed = g_failed.load(std::memory_order_relaxed);
  if (failed != g_logged.failed) {
    g_logged.failed = failed;
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_apply_failed",
                        {{"rc", g_lastRc.load(std::memory_order_relaxed)},
                         {"from", tq::LevelName(static_cast<Level>(g_lastFrom.load(std::memory_order_relaxed) == kUnset
                                                                       ? 0
                                                                       : g_lastFrom.load(std::memory_order_relaxed)))},
                         {"to", tq::LevelName(static_cast<Level>(g_lastTo.load(std::memory_order_relaxed) == kUnset
                                                                     ? 0
                                                                     : g_lastTo.load(std::memory_order_relaxed)))},
                         {"consequence", "the engine could not rebuild; the previous level was put back"}});
    ++lines;
  }
  return lines;
}

pthread_mutex_t g_pollMutex = PTHREAD_MUTEX_INITIALIZER;  // one pass at a time (the thread and a test)
pthread_mutex_t g_wait = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond;
bool g_condReady = false;
bool g_running = false;
pthread_t g_thread;
constexpr unsigned kPeriodMs = 500;

int PollOnce() noexcept {
  pthread_mutex_lock(&g_pollMutex);
  const int lines = PollLocked();
  pthread_mutex_unlock(&g_pollMutex);
  return lines;
}

// The poller has no stop: it ends with the process, like the reporter thread (hook_report.h, "Teardown"); the sentinel is never
// unloaded.
void* PollerMain(void*) {
  pthread_mutex_lock(&g_wait);
  for (;;) {
    timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_nsec += static_cast<long>(kPeriodMs) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_nsec -= 1000000000L;
      ++deadline.tv_sec;
    }
    int rc = 0;
    while (rc == 0) rc = pthread_cond_timedwait(&g_cond, &g_wait, &deadline);  // nothing signals it: it ends at the deadline
    pthread_mutex_unlock(&g_wait);
    PollOnce();
    pthread_mutex_lock(&g_wait);
  }
  return nullptr;
}

bool StartPoller() noexcept {
  if (g_running) return true;
  if (!g_condReady) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cond, &attr);
    pthread_condattr_destroy(&attr);
    g_condReady = true;
  }
  const int rc = pthread_create(&g_thread, nullptr, &PollerMain, nullptr);
  if (rc == 0) {
    pthread_setname_np(g_thread, "nevr-texture");
    g_running = true;
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_menu_install", {{"result", "poller_not_started"}, {"errno", rc}});
  }
  return rc == 0;
}

// ---- install ------------------------------------------------------------------------------------------------------------

bool Equal(const char* a, const char* b) noexcept { return std::strcmp(a, b) == 0; }

// Proves one function of the live libr15 (its first words are the pinned build's) and publishes its address.
const char* ResolveFunction(const sentinel::ElfImage& image, const pin::PinnedFunction& f, void** out) noexcept {
  const unsigned char* const fn = reinterpret_cast<const unsigned char*>(image.base) + f.vaddr;
  if (std::memcmp(fn, f.code, f.words * sizeof(std::uint32_t)) != 0) return "prologue_mismatch";
  static_assert(sizeof(*out) == sizeof(fn), "function pointer size");
  std::memcpy(out, &fn, sizeof(fn));
  return nullptr;
}

template <typename Thunk>
sentinel::GotStatus Hook(sentinel::GotHook& hook, const sentinel::GotTarget& target) noexcept {
  return sentinel::InstallThunk<Thunk>(hook, target);
}

}  // namespace

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"gfx_check_calls", &CheckThunk::CallCounter(), sentinel::ReportKind::kCalls},
      {"gfx_check_faults", &CheckThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_apply_faults", &ApplyThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_validate_faults", &ValidateThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_decode_file_faults", &FileThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_decode_text_faults", &TextThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"gfx_menu_deliveries", &g_deliveries, sentinel::ReportKind::kCalls},
      {"gfx_menu_level", &g_menuLevel, sentinel::ReportKind::kCalls},
      {"gfx_applied_level", &g_appliedLevel, sentinel::ReportKind::kCalls},
      {"gfx_live_applies", &g_applies, sentinel::ReportKind::kCalls},
      {"gfx_live_refused", &g_refused, sentinel::ReportKind::kCalls},
      {"gfx_live_failed", &g_failed, sentinel::ReportKind::kFaults},
      {"gfx_menu_json_rewrites", &g_jsonRewrites, sentinel::ReportKind::kCalls},
  };
  int registered = 0;
  for (const Entry& e : entries) registered += sentinel::RegisterReportCounter(e.name, e.value, e.kind) ? 1 : 0;
  if (registered != kCounterCount) {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_menu_counters",
                        {{"result", "refused"}, {"registered", registered}, {"wanted", kCounterCount}});
    return false;
  }
  return true;
}

bool Install(tq::Level level, bool fromConfig) noexcept {
  const char* const source = fromConfig ? "config" : "default";
  auto skipped = [&](const char* why, const char* detail) noexcept {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_menu_install",
                        {{"result", "skipped"}, {"why", why}, {"detail", detail}, {"quality", tq::LevelName(level)},
                         {"source", source},
                         {"consequence", "no Texture Resolution control; the level is the configuration's, fixed at launch"}});
    return false;
  };
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(pin::kLibR15, &image)) return skipped("module_not_loaded", "libr15.so");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return skipped("no_build_id", "libr15.so");
  if (!Equal(id, pin::LibR15PatchedBuildId())) return skipped("build_id_mismatch", id);

  void* refresh = nullptr;
  void* resize = nullptr;
  void* initialized = nullptr;
  void* graphicsInitialized = nullptr;
  void* displayLost = nullptr;
  void* localAppData = nullptr;
  struct Item {
    const pin::PinnedFunction* f;
    void** out;
  };
  const Item items[] = {{&pin::kRefreshRTSize, &refresh},           {&pin::kEngineResize, &resize},
                        {&pin::kVrInitialized, &initialized},       {&pin::kVrGraphicsInitialized, &graphicsInitialized},
                        {&pin::kVrDisplayLost, &displayLost},       {&pin::kLocalAppDataDirectory, &localAppData}};
  for (const Item& item : items) {
    if (const char* why = ResolveFunction(image, *item.f, item.out)) return skipped(why, item.f->name);
  }
  Game game;
  std::memcpy(&game.refreshRtSize, &refresh, sizeof(refresh));
  std::memcpy(&game.engineResize, &resize, sizeof(resize));
  std::memcpy(&game.vrInitialized, &initialized, sizeof(initialized));
  std::memcpy(&game.vrGraphicsInitialized, &graphicsInitialized, sizeof(graphicsInitialized));
  std::memcpy(&game.vrDisplayLost, &displayLost, sizeof(displayLost));
  std::memcpy(&game.localAppDataDirectory, &localAppData, sizeof(localAppData));
#if defined(__ANDROID__)
  const PropertyReader property = &AndroidProperty;
#else
  const PropertyReader property = nullptr;
#endif
  ArmForTest(game, level, fromConfig, property);

  // The three hooks that make the control work first; the documents that offer it only when they hold.
  FileThunk::Arm(kFileHook);
  TextThunk::Arm(kTextHook);
  ValidateThunk::Arm(kValidateHook);
  ApplyThunk::Arm(kApplyHook);
  CheckThunk::Arm(kCheckHook);
  const sentinel::GotStatus validate = Hook<ValidateThunk>(g_validateHook, pin::LibR15ValidateGfxSettings());
  const sentinel::GotStatus apply = Hook<ApplyThunk>(g_applyHook, pin::LibR15ApplyRuntimeGfxSettings());
  const sentinel::GotStatus check = Hook<CheckThunk>(g_checkHook, pin::LibR15CheckForResize());
  const bool engineHooks = validate == sentinel::GotStatus::kOk && apply == sentinel::GotStatus::kOk && check == sentinel::GotStatus::kOk;
  sentinel::GotStatus file = sentinel::GotStatus::kNotInstalled;
  sentinel::GotStatus text = sentinel::GotStatus::kNotInstalled;
  if (engineHooks) {
    text = Hook<TextThunk>(g_textHook, pin::LibR15JsonDecodeFromText());
    file = Hook<FileThunk>(g_fileHook, pin::LibR15JsonDecodeFromFile());
  }
  const bool all = engineHooks && file == sentinel::GotStatus::kOk && text == sentinel::GotStatus::kOk;
  sentinel::LogFields(all ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "texture_menu_install",
                      {{"result", all ? "installed" : "partial"},
                       {"quality", tq::LevelName(level)},
                       {"source", source},
                       {"validate_gfx_settings", sentinel::GotStatusName(validate)},
                       {"apply_runtime_gfx_settings", sentinel::GotStatusName(apply)},
                       {"check_for_resize", sentinel::GotStatusName(check)},
                       {"decode_from_text", sentinel::GotStatusName(text)},
                       {"decode_from_file", sentinel::GotStatusName(file)},
                       {"consequence", all ? "the Texture Resolution control is offered and a change applies at the next frame"
                                           : "a level change applies at the next launch, or the control is not offered"}});
  if (engineHooks) {
    tq::SetStartResolver(&ResolveStart);
    StartPoller();
  }
  return all;
}

void ArmForTest(const Game& game, tq::Level configLevel, bool /*fromConfig*/, PropertyReader property) noexcept {
  g_refreshRtSize.store(game.refreshRtSize, std::memory_order_release);
  g_vrInitialized.store(game.vrInitialized, std::memory_order_release);
  g_vrGraphicsInitialized.store(game.vrGraphicsInitialized, std::memory_order_release);
  g_vrDisplayLost.store(game.vrDisplayLost, std::memory_order_release);
  g_localAppData.store(game.localAppDataDirectory, std::memory_order_release);
  g_engineResize.store(game.engineResize, std::memory_order_release);
  g_property.store(property, std::memory_order_release);
  g_defaultIndex.store(tq::MenuIndexForLevel(configLevel), std::memory_order_release);
  tq::SetStartResolver(&ResolveStart);
  FileThunk::Arm(kFileHook);
  TextThunk::Arm(kTextHook);
  ValidateThunk::Arm(kValidateHook);
  ApplyThunk::Arm(kApplyHook);
  CheckThunk::Arm(kCheckHook);
}

void ResetForTest() noexcept {
  g_referenceIndex.store(kUnset);
  g_referenceFile.store("none");
  g_refreshRtSize.store(nullptr);
  g_vrInitialized.store(nullptr);
  g_vrGraphicsInitialized.store(nullptr);
  g_vrDisplayLost.store(nullptr);
  g_localAppData.store(nullptr);
  g_engineResize.store(nullptr);
  g_property.store(nullptr);
  g_lastIndex.store(kUnset);
  g_pendingMarker.store(kUnset);
  g_markerPathReady.store(false);
  g_markerPath[0] = '\0';
  g_markerWrites.store(0);
  g_markerFailures.store(0);
  g_defaultIndex.store(tq::kDefaultMenuIndex);
  g_startSource.store(kStartNotAsked);
  g_startLevel.store(kUnset);
  g_playerSaved.store(false);
  g_firstDelivery.store(false);
  g_deliveries.store(0);
  g_menuLevel.store(0);
  g_appliedLevel.store(0);
  g_applies.store(0);
  g_equalCommits.store(0);
  g_refused.store(0);
  g_failed.store(0);
  g_jsonRewrites.store(0);
  g_configOverrides.store(0);
  g_disabled.store(false);
  g_disabledWhy.store("none");
  g_lastRc.store(0);
  g_lastFrom.store(kUnset);
  g_lastTo.store(kUnset);
  for (int i = 0; i < kDocuments; ++i) {
    g_jsonStatus[i].store(0);
    g_jsonRows[i].store(0);
    g_jsonPages[i].store(0);
  }
  g_logged = Logged();
  g_propertyLast.store(kUnset);
  SetArmed(kNone);
  pthread_mutex_lock(&g_keptMutex);
  for (Kept& k : g_kept) k = Kept();  // the strings are leaked on purpose (the parser may hold them)
  pthread_mutex_unlock(&g_keptMutex);
}

int PollOnceForTest() noexcept { return PollOnce(); }

Totals CurrentTotals() noexcept {
  return {g_deliveries.load(), g_jsonRewrites.load(), g_applies.load(),       g_equalCommits.load(),
          g_refused.load(),    g_failed.load(),       g_configOverrides.load(), g_markerWrites.load(),
          g_markerFailures.load(), g_disabled.load(), g_disabledWhy.load()};
}

}  // namespace nevr_quest::texture_menu
