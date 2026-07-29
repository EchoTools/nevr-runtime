#pragma once
// Forwarding shim. NOT for new code.
//
// src/legacy/ is frozen and may not be edited, but it is NOT self-contained:
// src/legacy/gamepatches/patches.cpp:8 spells `#include "common/hooking.h"`,
// which used to resolve to src/common/hooking.h through the global src/ include
// path. Verified from the compiler's own dependency graph, not by reading:
//   ninja -C build/<preset> -t deps | grep gamepatcheslegacy | grep src/common
//
// The 2026-07-29 reorganization moved that header to src/core/hooking.h. Rather
// than edit a frozen file, this directory is added to gamepatcheslegacy's
// include path alone (root CMakeLists.txt), so the frozen spelling keeps
// resolving and nothing else in the tree can see it.
//
// Delete this file, its sibling, and that include-path line together when
// src/legacy/gamepatches is retired.
#include "core/hooking.h"
