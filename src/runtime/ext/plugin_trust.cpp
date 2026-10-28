// plugin_trust.cpp — the pins behind the plugin gate (ADR 0010, #741).

#include "runtime/ext/plugin_trust.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <exception>
#include <memory>
#include <string>
#include <string_view>

#include "runtime/ext/plugin_pins.h"
#include "runtime/lifecycle/cmdline_strip.h"

namespace nevr_plugin_trust {
namespace {

// The pins (root, EKU, subject, dev build) are plugin_pins.cpp's; the literal below is this unit's alone, so the host
// (which compiles plugin_pins.cpp and not this file) carries no NEVR-PLUGIN-TRUST literal.
#ifdef NEVR_ALLOW_UNSIGNED_PLUGINS
// A leading NUL so the literal is NUL-bounded in the file whatever the linker places before it.
const char kModeArray[] = "\0NEVR-PLUGIN-TRUST dev";
#else
const char kModeArray[] = "\0NEVR-PLUGIN-TRUST release";
#endif

// The run's mode (#924). Unsigned plugins load until the command line says otherwise: release-signer does not sign
// plugins yet, so a player's plugins are unsigned. Written by ApplyCommandLine before the first plugin pass and read
// by every pass and by the late-load filter (any thread), so it is atomic.
constexpr Mode kDefaultMode = Mode::UnsignedAllowed;
std::atomic<Mode> g_runMode{kDefaultMode};
std::atomic<ModeSource> g_runModeSource{ModeSource::Default};

nevr_plugin_signature::Policy MakeProductionPolicy() {
  nevr_plugin_signature::Policy policy = nevr_plugin_pins::PinnedPolicy();
  // The denylist (ADR 0010 section 2): leaf serials (lowercase hex, no leading zeros), signing-time windows
  // (unix seconds, inclusive) and retired profile EKUs. Empty: no signing day has been compromised. An entry
  // is added by a runtime release; plugin_trust tests check that each one parses.
  return policy;
}

#ifdef NEVR_TEST_HOOKS
bool g_overridden = false;
Mode g_overrideMode = kDefaultMode;
const nevr_plugin_signature::Policy* g_overridePolicy = nullptr;
#endif

}  // namespace

bool IsDevBuild() { return nevr_plugin_pins::IsDevBuild(); }

const char* BuildWord() { return nevr_plugin_pins::IsDevBuild() ? "dev" : "release"; }

const char* ModeWord(Mode mode) { return mode == Mode::SignedOnly ? "signed-only" : "unsigned-allowed"; }

const char* ModeSourceWord(ModeSource source) {
  switch (source) {
    case ModeSource::CommandLine:
      return "-require-signed-plugins";
    case ModeSource::CommandLineUnreadable:
      return "command line unreadable";
    case ModeSource::CommandLineReadFailed:
      return "command line read failed";
    case ModeSource::Default:
      break;
  }
  return "default";
}

const char* ModeLiteral() { return kModeArray + 1; }

namespace {

constexpr std::string_view kRequireSignedPluginsName = "require-signed-plugins";

// What one reading of the command line found.
enum class Reading {
  NotFound,    // the line was read and holds no token the reading takes for the flag
  Found,       // a token the reading takes for the flag
  Unreadable,  // the line could not be read or converted, so the flag may be there (fail closed)
};

#ifdef NEVR_TEST_HOOKS
// Test builds only: where the reading can be made to fail (a stage returns true to make the call at that point fail, or
// throws), and how many CommandLineToArgvW buffers are not yet freed.
ReadFaultHook g_readFault = nullptr;
std::atomic<int> g_argvLive{0};
bool Fault(ReadStage stage) { return g_readFault != nullptr && g_readFault(stage); }
void NoteArgv(int delta) { g_argvLive.fetch_add(delta); }
#else
constexpr bool Fault(ReadStage) { return false; }
constexpr void NoteArgv(int) {}
#endif

// The buffer CommandLineToArgvW returns is one LocalFree away from leaking whichever way the reading ends, a thrown
// exception included; LocalFree returns null when it freed it.
struct ArgvDeleter {
  void operator()(wchar_t** argv) const noexcept {
    if (LocalFree(argv) == nullptr) NoteArgv(-1);
  }
};
using ArgvPtr = std::unique_ptr<wchar_t*, ArgvDeleter>;

// The game's reading (#924): echovr.exe tokenizes the ANSI line GetCommandLineA() returned with its own tokenizer, and
// looks a name up with every leading '-', '+' and 0x96 skipped and A-Z lower-cased. Both are cmdline_strip.h's
// (Tokenize and detail::NameIs), reused, not rewritten, so the option rule (a token is an option when it starts with
// a dash and is not quoted) and the name rule stay the game's. May throw std::bad_alloc (the token list).
Reading GameReading(std::string_view ansiLine) {
  Fault(ReadStage::GameTokens);
  for (const nevr_cmdline::Token& token : nevr_cmdline::Tokenize(ansiLine)) {
    if (token.option && nevr_cmdline::detail::NameIs(token.text, kRequireSignedPluginsName)) return Reading::Found;
  }
  return Reading::NotFound;
}

// NEVR's other reading: CommandLineToArgvW over the wide line (what boot.cpp's own arguments come from). It differs
// from the game's on quotes (`"-x"` is an option there and a parameter to the game) and on a backslash before a quote.
// Each argument is converted to the ANSI code page, the form the game's name rule works on, and must start with a dash
// like the game's option rule. Unreadable when the line cannot be parsed or an argument cannot be converted. May throw
// std::bad_alloc (the converted text); the argv buffer is freed on every path.
Reading ArgvReading(const wchar_t* wideLine) {
  int argc = 0;
  wchar_t** const raw = Fault(ReadStage::ArgvParse) ? nullptr : CommandLineToArgvW(wideLine, &argc);
  if (raw == nullptr) return Reading::Unreadable;
  NoteArgv(1);
  const ArgvPtr argv(raw);
  Fault(ReadStage::ArgvParsed);
  for (int i = 0; i < argc; ++i) {
    const int size = Fault(ReadStage::ArgvSizeQuery)
                         ? 0
                         : WideCharToMultiByte(CP_ACP, 0, argv.get()[i], -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return Reading::Unreadable;
    Fault(ReadStage::ArgvText);
    std::string narrow(static_cast<std::size_t>(size), '\0');
    const int written = Fault(ReadStage::ArgvConversion)
                            ? 0
                            : WideCharToMultiByte(CP_ACP, 0, argv.get()[i], -1, narrow.data(), size, nullptr, nullptr);
    if (written <= 0) return Reading::Unreadable;
    narrow.resize(static_cast<std::size_t>(size) - 1);
    if (!narrow.empty() && nevr_cmdline::detail::IsDash(narrow.front()) &&
        nevr_cmdline::detail::NameIs(narrow, kRequireSignedPluginsName)) {
      return Reading::Found;
    }
  }
  return Reading::NotFound;
}

}  // namespace

void ApplyCommandLine(const wchar_t* wideLine, const char* ansiLine) {
  if (wideLine == nullptr || ansiLine == nullptr) {
    ApplyUnreadableCommandLine();
    return;
  }
  // Signed-only when EITHER reading finds the flag, or when a reading could not tell: a spelling one reading misses, or
  // a line one reading cannot read, must not leave the run unsigned. Both readings always run, so a flag found by the
  // second is not hidden by the first being unreadable.
  Reading game = Reading::Unreadable;
  Reading argv = Reading::Unreadable;
  try {
    game = GameReading(ansiLine);
    argv = ArgvReading(wideLine);
  } catch (const std::exception&) {
    // The only code in the try is the standard library (the token list, the converted text: std::bad_alloc or another
    // std::exception such as std::length_error) and Win32 calls, which throw nothing. Anything the reading could not
    // finish is a line whose flag is unknown, so the run is signed-only, and the source says it was a failed read.
    g_runMode.store(Mode::SignedOnly);
    g_runModeSource.store(ModeSource::CommandLineReadFailed);
    return;
  }
  if (game == Reading::Found || argv == Reading::Found) {
    g_runMode.store(Mode::SignedOnly);
    g_runModeSource.store(ModeSource::CommandLine);
  } else if (game == Reading::Unreadable || argv == Reading::Unreadable) {
    ApplyUnreadableCommandLine();
  } else {
    g_runMode.store(kDefaultMode);
    g_runModeSource.store(ModeSource::Default);
  }
}

void ApplyUnreadableCommandLine() {
  g_runMode.store(Mode::SignedOnly);
  g_runModeSource.store(ModeSource::CommandLineUnreadable);
}

bool PinsConfirmed() { return nevr_plugin_pins::PinsConfirmed(); }

const nevr_plugin_signature::Policy& ProductionPolicy() {
  static const nevr_plugin_signature::Policy policy = MakeProductionPolicy();
  return policy;
}

const uint8_t* RootDer() { return nevr_plugin_pins::RootDer(); }
size_t RootDerSize() { return nevr_plugin_pins::RootDerSize(); }
const char* RootSha256Hex() { return nevr_plugin_pins::RootSha256Hex(); }

Mode ActiveMode() {
#ifdef NEVR_TEST_HOOKS
  if (g_overridden) return g_overrideMode;
#endif
  return g_runMode.load();
}

ModeSource ActiveModeSource() { return g_runModeSource.load(); }

const nevr_plugin_signature::Policy& ActivePolicy() {
#ifdef NEVR_TEST_HOOKS
  if (g_overridden && g_overridePolicy != nullptr) return *g_overridePolicy;
#endif
  return ProductionPolicy();
}

#ifdef NEVR_TEST_HOOKS
void TestHook_SetTrust(Mode mode, const nevr_plugin_signature::Policy* policy) {
  g_overridden = true;
  g_overrideMode = mode;
  g_overridePolicy = policy;
}

void TestHook_SetReadFault(ReadFaultHook hook) { g_readFault = hook; }

int TestHook_ArgvBuffersLive() { return g_argvLive.load(); }

void TestHook_ResetTrust() {
  g_readFault = nullptr;
  g_argvLive.store(0);
  g_overridden = false;
  g_overrideMode = kDefaultMode;
  g_overridePolicy = nullptr;
  g_runMode.store(kDefaultMode);
  g_runModeSource.store(ModeSource::Default);
}
#endif

}  // namespace nevr_plugin_trust
