// plugin_trust.h — what the runtime pins for plugin signatures, and whether this build enforces them
// (ADR 0010, #741). The decision itself is plugin_signature.h; this is its production policy.
//
// Windows only (the policy is read by the loader, and the pins are checked by a test that hashes the root).

#pragma once

#include <cstdint>
#include <string>

#include "runtime/ext/plugin_signature.h"

namespace nevr_plugin_trust {

enum class Mode {
  SignedOnly,       // a plugin the verifier refuses is not loaded (a run started with -require-signed-plugins)
  UnsignedAllowed,  // a plugin the verifier refuses is logged and loaded (the default of every build)
};

// Where the run's mode came from, for the log line that states it.
enum class ModeSource {
  Default,                // no -require-signed-plugins on the command line
  CommandLine,            // -require-signed-plugins was on the command line
  CommandLineUnreadable,  // the command line could not be read, so signatures are required (fail closed)
  CommandLineReadFailed,  // reading the command line threw (out of memory), so signatures are required (fail closed)
};

// True in the build the NEVR_ALLOW_UNSIGNED_PLUGINS option makes (the mingw-plugin-dev preset), which tools/package_release.py
// refuses to package. It does not change how plugins are checked: the run's mode does (ActiveMode).
bool IsDevBuild();

// "release" or "dev": the word the packaging check reads from the DLL.
const char* BuildWord();

// "signed-only" or "unsigned-allowed".
const char* ModeWord(Mode mode);
const char* ModeSourceWord(ModeSource source);

// The NUL-bounded literal this binary embeds, "NEVR-PLUGIN-TRUST <build word>" (separate from the
// NEVR-BUILD identity literal, which is a cross-repo contract and gains no field). It names the kind of build, a
// fact of the file; the mode a run enforces is a fact of the run, so the literal does not carry it (the loader's
// start-up line does). Referenced by that line so the linker keeps it.
const char* ModeLiteral();

// Sets the mode of this run from its command line: SignedOnly when any token the game would read as
// -require-signed-plugins is there, UnsignedAllowed otherwise. `ansiLine` is GetCommandLineA() and `wideLine` is
// GetCommandLineW(). The line is read two ways and either finding the flag makes the run SignedOnly: the game's own
// way (its tokenizer and name rule, runtime/lifecycle/cmdline_strip.h: '-', '+' and 0x96 prefixes, any case, quoted
// tokens are parameters) and CommandLineToArgvW's (what NEVR's own arguments come from), converted to the ANSI code
// page. A null line, a line a reading cannot parse or convert, and a reading that throws a std::exception are
// unreadable ones: SignedOnly, with the source saying which (ModeSource). Called once, before the first LoadPlugins
// (PreflightRuntimeBootstrap), so the early pass, the normal pass and the late-load filter read the same mode; later
// calls replace the earlier one.
void ApplyCommandLine(const wchar_t* wideLine, const char* ansiLine);

// The command line could not be read, so whether -require-signed-plugins was given is unknown: the run is SignedOnly.
void ApplyUnreadableCommandLine();

// True once the profile EKU has been read from a certificate the real pipeline issued and test_plugin_trust has
// verified that signed file against it (ADR 0010 section 2). The signer subject is deliberately not pinned.
bool PinsConfirmed();

// The production policy: the pinned Microsoft root, the profile EKU, no subject (the field stays empty, so no
// subject is compared) and the denylist.
const nevr_plugin_signature::Policy& ProductionPolicy();

// The pinned root certificate (DER) and its SHA-256 as 64 lowercase hex characters; a test checks that
// the bytes hash to the digest.
const uint8_t* RootDer();
size_t RootDerSize();
const char* RootSha256Hex();

// What the loader consults: the mode of this run (UnsignedAllowed until ApplyCommandLine says otherwise) and the policy.
Mode ActiveMode();
ModeSource ActiveModeSource();
const nevr_plugin_signature::Policy& ActivePolicy();

// The points of ApplyCommandLine's reading where a test build can make it fail (TestHook_SetReadFault); production
// builds name them and never act on them.
enum class ReadStage {
  GameTokens,      // before the game's tokenizer builds its token list
  ArgvParse,       // CommandLineToArgvW
  ArgvParsed,      // CommandLineToArgvW returned a buffer that is now live
  ArgvSizeQuery,   // WideCharToMultiByte, the size of an argument
  ArgvText,        // before the converted text of an argument is allocated
  ArgvConversion,  // WideCharToMultiByte, the text of an argument
};

#ifdef NEVR_TEST_HOOKS
// Test builds only: called at each stage; true means the call at that stage fails (the reading is unreadable), and the
// hook may throw instead.
using ReadFaultHook = bool (*)(ReadStage);
void TestHook_SetReadFault(ReadFaultHook hook);
// Test builds only: CommandLineToArgvW buffers handed out and not yet freed (0 once ApplyCommandLine has returned, on
// every path).
int TestHook_ArgvBuffersLive();

// Test builds only: enforce with `policy` (not owned; must outlive the use) in `mode`.
void TestHook_SetTrust(Mode mode, const nevr_plugin_signature::Policy* policy);
void TestHook_ResetTrust();
#endif

}  // namespace nevr_plugin_trust
