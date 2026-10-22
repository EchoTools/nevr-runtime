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
  SignedOnly,       // a plugin the verifier refuses is not loaded (every release build)
  UnsignedAllowed,  // a plugin the verifier refuses is logged and loaded (the mingw-plugin-dev preset only)
};

// The mode this binary was built in: NEVR_ALLOW_UNSIGNED_PLUGINS decides, nothing at run time does.
Mode BuildMode();

// "signed-only" or "unsigned-allowed": the word the packaging check reads from the DLL.
const char* ModeWord(Mode mode);

// The NUL-bounded literal this binary embeds, "NEVR-PLUGIN-TRUST <mode-word>" (separate from the
// NEVR-BUILD identity literal, which is a cross-repo contract and gains no field). Referenced by the
// loader's start-up line so the linker keeps it.
const char* ModeLiteral();

// True once the profile EKU and the signer subject have been read from a certificate the real pipeline issued
// and test_plugin_trust has verified that signed file against them (ADR 0010 section 2).
bool PinsConfirmed();

// The production policy: the pinned Microsoft root, the profile EKU, the subject (empty until the signer
// fills it) and the denylist.
const nevr_plugin_signature::Policy& ProductionPolicy();

// The pinned root certificate (DER) and its SHA-256 as 64 lowercase hex characters; a test checks that
// the bytes hash to the digest.
const uint8_t* RootDer();
size_t RootDerSize();
const char* RootSha256Hex();

// What the loader consults. In a production build these are BuildMode() and ProductionPolicy().
Mode ActiveMode();
const nevr_plugin_signature::Policy& ActivePolicy();

#ifdef NEVR_TEST_HOOKS
// Test builds only: enforce with `policy` (not owned; must outlive the use) in `mode`.
void TestHook_SetTrust(Mode mode, const nevr_plugin_signature::Policy* policy);
void TestHook_ResetTrust();
#endif

}  // namespace nevr_plugin_trust
