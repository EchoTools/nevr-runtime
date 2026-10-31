#include "texture_quality_hook.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"

namespace nevr_quest::texture_quality {

namespace {

using sentinel::pinned::OvrJavaOpaque;

// The vrapi_GetSystemPropertyInt slot, at every level; CVR::RefreshRTSize's slot, on the reference input only.
sentinel::GotHook g_hook;
sentinel::GotHook g_refreshHook;

constexpr int kUnset = kLevelUnset;
// The applied level (a Level, or kUnset): the level the swap chain and the render targets are built for, which the answers follow.
// Written by the first install in the process, once by the start resolver, then by CommitApplied.
std::atomic<int> g_latched{kUnset};
// The level last asked for (a Level, or kUnset) and who asked.
std::atomic<int> g_requested{kUnset};
std::atomic<int> g_requestSource{static_cast<int>(RequestSource::kMenu)};
std::atomic<StartResolver> g_resolver{nullptr};
std::atomic<bool> g_startAsked{false};

std::atomic<std::uint64_t> g_nativeWidth{0};
std::atomic<std::uint64_t> g_nativeHeight{0};
std::atomic<std::uint64_t> g_served{0};
std::atomic<std::uint64_t> g_sizeQueries{0};

// The RefreshRTSize route (#1737 C4): what the live body is, whether its hook is in place, and the game's pieces it uses.
std::atomic<int> g_input{static_cast<int>(EyeInput::kUnknown)};
std::atomic<bool> g_refreshLive{false};
std::atomic<std::uint64_t*> g_storedPair{nullptr};
std::atomic<const std::uint64_t*> g_overridePair{nullptr};
std::atomic<const OvrJavaOpaque*> g_java{nullptr};
std::atomic<ViewportScaleFn> g_scale{nullptr};
std::atomic<PropertyFn> g_property{nullptr};
// What the last RefreshRTSize call stored (LastDecision).
std::atomic<int> g_decisionW{0};
std::atomic<int> g_decisionH{0};
std::atomic<int> g_decisionSource{static_cast<int>(DecisionSource::kNone)};

std::uint64_t NonNegative(int value) noexcept { return value > 0 ? static_cast<std::uint64_t>(value) : 0U; }

// The first eye-size query comes before the game delivers its settings, so the level the player saved is asked here, once. The
// resolver is the live apply's (it reads the settings file); with none set the configuration's level stands.
void AskStartResolverOnce() noexcept {
  if (g_startAsked.load(std::memory_order_acquire)) return;
  if (g_startAsked.exchange(true, std::memory_order_acq_rel)) return;
  const StartResolver resolver = g_resolver.load(std::memory_order_acquire);
  if (resolver == nullptr) return;
  const int saved = resolver();
  if (saved == kUnset || g_latched.load(std::memory_order_acquire) == kUnset) return;
  g_latched.store(saved, std::memory_order_release);
}

// vrapi_GetSystemPropertyInt. Calls the original with the game's own arguments every time, tells the hardware dump's
// observer (if any) what the runtime answered, then replaces the answer to the eye width and height ids when the applied level
// has a size. The size is the applied level's for the headset the runtime's own answer names (HeadsetForStockSize), so the
// same level is a Quest 2 table on a Quest 2. Never logs or throws: it is live across the call into the runtime
// (callback_thunk.h rule 1).
int Handler(SystemPropertyIntThunk::Fn original, const OvrJavaOpaque* java, int id) noexcept {
  const int native = original(java, id);
  const sentinel::SysPropIntObserver observer = sentinel::GetSysPropIntObserver();
  if (observer != nullptr) observer(java, id, native);
  if (id != kEyeWidthPropertyId && id != kEyeHeightPropertyId) return native;
  AskStartResolverOnce();
  g_sizeQueries.fetch_add(1, std::memory_order_relaxed);
  const int latched = g_latched.load(std::memory_order_acquire);
  if (id == kEyeWidthPropertyId) {
    g_nativeWidth.store(NonNegative(native), std::memory_order_relaxed);
    if (latched != kUnset) {
      const int width = SizeFor(static_cast<Level>(latched), HeadsetForStockSize(native)).width;
      if (width > 0) {
        g_served.fetch_add(1, std::memory_order_relaxed);
        return width;
      }
    }
  } else {
    g_nativeHeight.store(NonNegative(native), std::memory_order_relaxed);
    if (latched != kUnset) {
      const int height = SizeFor(static_cast<Level>(latched), HeadsetForStockSize(native)).height;
      if (height > 0) {
        g_served.fetch_add(1, std::memory_order_relaxed);
        return height;
      }
    }
  }
  return native;
}

NEVR_HOOK_RECORD(kHook, SystemPropertyIntThunk, &Handler);

void RecordDecision(int w, int h, DecisionSource source) noexcept {
  g_decisionW.store(w, std::memory_order_relaxed);
  g_decisionH.store(h, std::memory_order_relaxed);
  g_decisionSource.store(static_cast<int>(source), std::memory_order_relaxed);
}

// CVR::RefreshRTSize, reconstructed from libr15 0x1a709a0 (the body the reference build replaced its two vrapi calls in). The
// stock body: the scale is CustomViewportScale(0); a non-zero stored override gives {w * scale.x, h * scale.x} with no query;
// otherwise the runtime is asked for ids 5 and 6 (through the vrapi PLT stub, so the vrapi hook answers them with the applied
// level, as on the stock input) and the pair is {scale.x * w, scale.x * h}. The result is stored as the game stores it: the two
// int32 truncations (fcvtzs) in the pair at 0x37a33e0. No sizing decision is made here. It never calls `original` on the normal
// path, because the reference body's cave would run and touch the words the C8 path depends on. It logs nothing and is live across
// the game's own calls, so it is personality-free like every hook (callback_thunk.h rule 1).
void RefreshHandler(RefreshThunk::Fn original) noexcept {
  std::uint64_t* const storedPair = g_storedPair.load(std::memory_order_acquire);
  const std::uint64_t* const overridePair = g_overridePair.load(std::memory_order_acquire);
  const OvrJavaOpaque* const java = g_java.load(std::memory_order_acquire);
  const ViewportScaleFn scale = g_scale.load(std::memory_order_acquire);
  const PropertyFn property = g_property.load(std::memory_order_acquire);
  if (storedPair == nullptr || overridePair == nullptr || java == nullptr || scale == nullptr || property == nullptr) {
    original();  // the pins are not resolved: the game's own path
    return;
  }
  const EyeVector s = scale(0);  // only s.x is used
  const std::uint64_t overrideValue = __atomic_load_n(overridePair, __ATOMIC_RELAXED);
  float fw = 0.0F;
  float fh = 0.0F;
  if (overrideValue != 0) {  // a stored override: no query, no level
    fw = static_cast<float>(static_cast<std::int32_t>(overrideValue & 0xffffffffU)) * s.x;
    fh = static_cast<float>(static_cast<std::int32_t>(overrideValue >> 32)) * s.x;
    RecordDecision(0, 0, DecisionSource::kGameOverride);
  } else {
    AskStartResolverOnce();
    const int w = property(java, kEyeWidthPropertyId);   // id 5 first, then 6, as the game asks
    const int h = property(java, kEyeHeightPropertyId);
    const int latched = g_latched.load(std::memory_order_acquire);
    const bool native = latched == kUnset || static_cast<Level>(latched) == Level::kStock;
    RecordDecision(w, h, native ? DecisionSource::kNative : DecisionSource::kLevel);
    fw = s.x * static_cast<float>(w);
    fh = s.x * static_cast<float>(h);
  }
  const std::uint64_t packed =
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(TruncateToInt(fh))) << 32) |
      static_cast<std::uint64_t>(static_cast<std::uint32_t>(TruncateToInt(fw)));
  __atomic_store_n(storedPair, packed, __ATOMIC_RELAXED);
}

NEVR_HOOK_RECORD(kRefreshHook, RefreshThunk, &RefreshHandler);

enum class Latch { kNew, kSame, kOther };

// The first non-stock level to ask wins; a different one later does not change it.
Latch TryLatch(Level level) noexcept {
  int expected = kUnset;
  if (g_latched.compare_exchange_strong(expected, static_cast<int>(level), std::memory_order_acq_rel)) return Latch::kNew;
  return expected == static_cast<int>(level) ? Latch::kSame : Latch::kOther;
}

void Unlatch() noexcept { g_latched.store(kUnset, std::memory_order_release); }

// Proves a function's first words are the pinned build's, then gives its address.
template <typename Fn>
Fn FunctionAt(const unsigned char* address) noexcept {
  static_assert(sizeof(Fn) == sizeof(address), "function pointer size");
  Fn fn = nullptr;
  std::memcpy(&fn, &address, sizeof(fn));
  return fn;
}

bool WordsAt(const unsigned char* at, const std::uint32_t* words, std::size_t count) noexcept {
  return std::memcmp(at, words, count * sizeof(std::uint32_t)) == 0;
}

// The address of body word `index` of the live RefreshRTSize, or of the cave's first word for kCaveBadIndex.
std::uint64_t BodyAddress(int index) noexcept {
  if (index == kCaveBadIndex) return kReferenceCaveVaddr;
  if (index < 0) return kRefreshVaddr;
  return kRefreshVaddr + 4 * static_cast<std::uint64_t>(index);
}

// The route's install, on the reference input only: classifies the live body, proves the two pinned functions and takes
// RefreshRTSize's slot, then the vrapi slot (rolled back with RefreshRTSize's when that one refuses). On the stock body it installs
// nothing here. On an unrecognised body it installs nothing and says why.
sentinel::GotStatus InstallRoute(sentinel::ElfImage& image) noexcept {
  unsigned char* const base = reinterpret_cast<unsigned char*>(image.base);
  std::uint32_t body[kRefreshBodyWords] = {};
  std::uint32_t caveWord0 = 0;
  std::memcpy(body, base + kRefreshVaddr, sizeof(body));
  std::memcpy(&caveWord0, base + kReferenceCaveVaddr, sizeof(caveWord0));
  const Classification c = ClassifyRefreshBody(body, kRefreshVaddr, caveWord0);
  g_input.store(static_cast<int>(c.input), std::memory_order_release);
  if (c.input == EyeInput::kUnknown) {
    char at[19];
    char expected[19];
    char actual[19];
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_route",
                        {{"result", "refused"}, {"input", "unknown"}, {"why", "refresh_body_unrecognized"},
                         {"va", sentinel::HexString(at, BodyAddress(c.badIndex))},
                         {"expected", sentinel::HexString(expected, c.expected)},
                         {"actual", sentinel::HexString(actual, c.actual)},
                         {"consequence", "the eye size is the game's own; no level applies"}});
    return sentinel::GotStatus::kOriginalMismatch;
  }
  if (c.input == EyeInput::kStock) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_route",
                        {{"result", "vrapi_only"}, {"input", "stock"}, {"slot", sentinel::pinned::kGetSystemPropertyIntSymbol},
                         {"consequence", "CVR::RefreshRTSize is the game's own; the vrapi hook answers ids 5 and 6"}});
    return sentinel::GotStatus::kOk;
  }

  // The reference input: the pinned functions and globals must be the ones the route calls.
  const sentinel::pinned::PinnedCode& scale = sentinel::pinned::kCustomViewportScale;
  const sentinel::pinned::PinnedCode& stub = sentinel::pinned::kVrapiPltStub;
  for (const sentinel::pinned::PinnedCode* pin : {&scale, &stub}) {
    if (WordsAt(base + pin->vaddr, pin->code, pin->words)) continue;
    char va[19];
    char expected[19];
    char actual[19];
    std::uint32_t firstWord = 0;
    std::memcpy(&firstWord, base + pin->vaddr, sizeof(firstWord));
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_route",
                        {{"result", "refused"}, {"input", "reference"}, {"why", "pin_mismatch"}, {"pin", pin->name},
                         {"va", sentinel::HexString(va, pin->vaddr)},
                         {"expected", sentinel::HexString(expected, pin->code[0])},
                         {"actual", sentinel::HexString(actual, firstWord)},
                         {"consequence", "the eye size is the game's own; no level applies"}});
    return sentinel::GotStatus::kOriginalMismatch;
  }
  g_storedPair.store(reinterpret_cast<std::uint64_t*>(base + sentinel::pinned::eye_layout::kStoredPairVaddr),
                     std::memory_order_release);
  g_overridePair.store(reinterpret_cast<const std::uint64_t*>(base + sentinel::pinned::eye_layout::kOverridePairVaddr),
                       std::memory_order_release);
  g_java.store(reinterpret_cast<const OvrJavaOpaque*>(base + sentinel::pinned::eye_layout::kOvrJavaVaddr),
               std::memory_order_release);
  g_scale.store(FunctionAt<ViewportScaleFn>(base + scale.vaddr), std::memory_order_release);
  g_property.store(FunctionAt<PropertyFn>(base + stub.vaddr), std::memory_order_release);

  // The thunk is armed before the slot changes, so its first call already has a handler.
  RefreshThunk::Arm(kRefreshHook);
  const sentinel::GotStatus refresh = sentinel::InstallThunk<RefreshThunk>(g_refreshHook, sentinel::pinned::LibR15RefreshRTSize());
  if (refresh != sentinel::GotStatus::kOk) {
    RefreshThunk::Disarm();
    return refresh;
  }
  const sentinel::GotStatus vrapi = sentinel::InstallThunk<SystemPropertyIntThunk>(g_hook, sentinel::pinned::LibR15GetSystemPropertyInt());
  if (vrapi != sentinel::GotStatus::kOk) {
    const sentinel::GotStatus rolledBack = g_refreshHook.Remove();
    RefreshThunk::Disarm();
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_route",
                        {{"result", "refused"}, {"input", "reference"}, {"why", "vrapi_slot_refused"},
                         {"status", sentinel::GotStatusName(vrapi)}, {"rollback", sentinel::GotStatusName(rolledBack)},
                         {"consequence", "the eye size is the game's own; no level applies"}});
    return vrapi;
  }
  g_refreshLive.store(true, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality_route",
                      {{"result", "installed"}, {"input", "reference"},
                       {"slot", sentinel::pinned::kRefreshRTSizeSymbol},
                       {"consequence", "ids 5 and 6 are decided here on both inputs; the reference build's own eye-size code does not run"}});
  return vrapi;
}

sentinel::GotStatus RealInstall() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return sentinel::GotStatus::kModuleNotLoaded;
  // Stock: the vrapi slot alone, as before. Reference: the route, then the vrapi slot. Unknown: nothing.
  const sentinel::GotStatus route = InstallRoute(image);
  if (route != sentinel::GotStatus::kOk) return route;
  if (g_input.load(std::memory_order_acquire) == static_cast<int>(EyeInput::kReference)) return sentinel::GotStatus::kOk;
  return sentinel::InstallThunk<SystemPropertyIntThunk>(g_hook, sentinel::pinned::LibR15GetSystemPropertyInt());
}

}  // namespace

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"gfx_native_eye_width", &g_nativeWidth, sentinel::ReportKind::kCalls},
      {"gfx_native_eye_height", &g_nativeHeight, sentinel::ReportKind::kCalls},
      {"gfx_eye_size_served", &g_served, sentinel::ReportKind::kCalls},
      {"gfx_thunk_faults", &SystemPropertyIntThunk::FaultCounter(), sentinel::ReportKind::kFaults},
  };
  int registered = 0;
  for (const Entry& e : entries) registered += sentinel::RegisterReportCounter(e.name, e.value, e.kind) ? 1 : 0;
  if (registered != kCounterCount) {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_counters",
                        {{"result", "refused"}, {"registered", registered}, {"wanted", kCounterCount}});
    return false;
  }
  return true;
}

bool InstallVia(Level level, bool fromConfig, InstallFn install) noexcept {
  const char* const quality = LevelName(level);
  const char* const source = fromConfig ? "config" : "default";
  if (TryLatch(level) != Latch::kNew) {
    // A second install in one process: the first level keeps answering and the slots are not touched again.
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality",
                        {{"result", "refused"}, {"quality", quality}, {"source", source}, {"w", 0}, {"h", 0},
                         {"why", "level_already_latched"}, {"latched", LevelName(LatchedLevel())}});
    return false;
  }
  // The level is latched before the thunk can be entered, so its first call already sees it.
  SystemPropertyIntThunk::Arm(kHook);
  const sentinel::GotStatus status = install();
  if (status != sentinel::GotStatus::kOk) {
    Unlatch();
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality",
                        {{"result", "refused"}, {"quality", quality}, {"source", source}, {"w", 0}, {"h", 0},
                         {"status", sentinel::GotStatusName(status)}});
    return false;
  }
  // The vrapi slot is this hook's: the hardware dump records the answers through the observer, not a second hook.
  sentinel::MarkSysPropIntSlotOwned(true);
  const bool reference = g_input.load(std::memory_order_acquire) == static_cast<int>(EyeInput::kReference);
  // The sizes depend on the headset, which only the first query names; the swap-chain line shows the size served.
  sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality",
                      {{"result", "installed"}, {"quality", quality}, {"source", source}, {"w", 0}, {"h", 0},
                       {"sizes", "per_headset"}, {"slot", sentinel::pinned::kGetSystemPropertyIntSymbol},
                       {"input", InputName(static_cast<EyeInput>(g_input.load(std::memory_order_acquire)))},
                       {"refresh", g_refreshLive.load(std::memory_order_acquire) ? "installed" : "not_installed"},
                       {"status", sentinel::GotStatusName(status)},
                       {"consequence", reference ? "ids 5 and 6 are decided here on both inputs; the reference build's own eye-size code does not run"
                                                 : "ids 5 and 6 are decided by the vrapi hook; the game's RefreshRTSize is unchanged"}});
  return true;
}

bool Install(Level level, bool fromConfig) noexcept { return InstallVia(level, fromConfig, &RealInstall); }

bool InstallIfCounted(Level level, bool fromConfig, bool countersRegistered) noexcept {
  if (!countersRegistered) {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality",
                        {{"result", "skipped"}, {"quality", LevelName(level)}, {"source", fromConfig ? "config" : "default"},
                         {"w", 0}, {"h", 0}, {"why", "counters_refused"}});
    return false;
  }
  return Install(level, fromConfig);
}

bool ArmForTest(Level level) noexcept {
  if (TryLatch(level) == Latch::kOther) return false;
  SystemPropertyIntThunk::Arm(kHook);
  return true;
}

void ResetForTest() noexcept {
  Unlatch();
  g_requested.store(kUnset, std::memory_order_release);
  g_requestSource.store(static_cast<int>(RequestSource::kMenu), std::memory_order_relaxed);
  g_resolver.store(nullptr, std::memory_order_release);
  g_startAsked.store(false, std::memory_order_release);
  g_nativeWidth.store(0, std::memory_order_relaxed);
  g_nativeHeight.store(0, std::memory_order_relaxed);
  g_served.store(0, std::memory_order_relaxed);
  g_sizeQueries.store(0, std::memory_order_relaxed);
  g_input.store(static_cast<int>(EyeInput::kUnknown), std::memory_order_release);
  g_refreshLive.store(false, std::memory_order_release);
  g_storedPair.store(nullptr, std::memory_order_release);
  g_overridePair.store(nullptr, std::memory_order_release);
  g_java.store(nullptr, std::memory_order_release);
  g_scale.store(nullptr, std::memory_order_release);
  g_property.store(nullptr, std::memory_order_release);
  RecordDecision(0, 0, DecisionSource::kNone);
  RefreshThunk::Reset();
  sentinel::MarkSysPropIntSlotOwned(false);
}

bool IsLatched() noexcept { return g_latched.load(std::memory_order_acquire) != kUnset; }

Level LatchedLevel() noexcept {
  const int value = g_latched.load(std::memory_order_acquire);
  return value == kUnset ? Level::kStock : static_cast<Level>(value);
}

EyeInput InputKind() noexcept { return static_cast<EyeInput>(g_input.load(std::memory_order_acquire)); }

void SetGameForTest(const EyeGame& game) noexcept {
  g_storedPair.store(game.storedPair, std::memory_order_release);
  g_overridePair.store(game.overridePair, std::memory_order_release);
  g_java.store(game.java, std::memory_order_release);
  g_scale.store(game.scale, std::memory_order_release);
  g_property.store(game.property, std::memory_order_release);
}

void SetInputForTest(EyeInput input) noexcept { g_input.store(static_cast<int>(input), std::memory_order_release); }

void ArmRefreshForTest() noexcept {
  RefreshThunk::Arm(kRefreshHook);
  g_refreshLive.store(true, std::memory_order_release);
}

sentinel::pinned::RefreshRTSizeFn HookedRefresh() noexcept {
  if (!g_refreshLive.load(std::memory_order_acquire)) return nullptr;
  return RefreshThunk::EntryFn();
}

Decision LastDecision() noexcept {
  return Decision{g_decisionW.load(std::memory_order_relaxed), g_decisionH.load(std::memory_order_relaxed),
                  static_cast<DecisionSource>(g_decisionSource.load(std::memory_order_relaxed))};
}

const char* DecisionSourceName(DecisionSource source) noexcept {
  switch (source) {
    case DecisionSource::kNone: return "none";
    case DecisionSource::kLevel: return "level";
    case DecisionSource::kNative: return "native";
    case DecisionSource::kGameOverride: return "game_override";
  }
  return "none";
}

void SetStartResolver(StartResolver resolver) noexcept { g_resolver.store(resolver, std::memory_order_release); }

void RequestLevel(Level level, RequestSource source) noexcept {
  g_requestSource.store(static_cast<int>(source), std::memory_order_relaxed);
  g_requested.store(static_cast<int>(level), std::memory_order_release);
}

int RequestedLevelValue() noexcept { return g_requested.load(std::memory_order_acquire); }

RequestSource LastRequestSource() noexcept {
  return static_cast<RequestSource>(g_requestSource.load(std::memory_order_relaxed));
}

void CommitApplied(Level level) noexcept { g_latched.store(static_cast<int>(level), std::memory_order_release); }

unsigned long long SizeQueries() noexcept { return g_sizeQueries.load(std::memory_order_relaxed); }

Counts CurrentCounts() noexcept {
  return {g_nativeWidth.load(std::memory_order_relaxed), g_nativeHeight.load(std::memory_order_relaxed),
          g_served.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::texture_quality
