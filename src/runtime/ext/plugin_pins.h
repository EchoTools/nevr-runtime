// plugin_pins.h — the signature pins the runtime and the host share (ADR 0010, ADR 0012, #854): the pinned Microsoft
// root, the profile EKU, the pinned subject (empty: not pinned) and whether this build is the plugin developers' build.
// The constants live once, in plugin_pins.cpp; plugin_trust.cpp adds the runtime's denylist and the NEVR-PLUGIN-TRUST
// literal on top, and the host (host/runtime_load.cpp) uses PinnedPolicy() as it is.
//
// Windows only (the policy is verified with crypt32 through plugin_signature.h).

#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/ext/plugin_signature.h"

namespace nevr_plugin_pins {

// True in the plugin developers' build (NEVR_ALLOW_UNSIGNED_PLUGINS). The host reads it to load an unsigned
// nevr-runtime.dll, and a release build refuses one (docs/adr/0012-static-install-root.md).
bool IsDevBuild();

// True once the profile EKU has been confirmed against a real signed file (test_plugin_trust, RealSigned.*).
bool PinsConfirmed();

// The pinned Microsoft root (DER) and its SHA-256 as 64 lowercase hex characters.
const uint8_t* RootDer();
size_t RootDerSize();
const char* RootSha256Hex();

// The pins as a Policy with an empty denylist: the root, the profile EKU and the (unpinned) subject.
const nevr_plugin_signature::Policy& PinnedPolicy();

}  // namespace nevr_plugin_pins
