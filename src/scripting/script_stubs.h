// Typed API stubs for script authors, generated from what the runtime
// registered (override points and hook points), so the types come from the ABI
// tables and not from a second hand-written list. The output is a Luau
// definition file: luau-lsp loads it through `luau-lsp.types.definitionFiles`,
// and nevr_script_check (src/scripting/check/) type-checks scripts against it.
//
// Each override key and hook name is a singleton string type, and each hook
// point gets one call type per phase whose `set` takes only the fields writable
// in that phase, so a misspelt key, a wrong value type or a write to a
// read-only field is a type error before the script ever runs.
#pragma once

#include <string>

#include "scripting/host_registry.h"

namespace nevr_script {

std::string GenerateLuauDefinitions(const Registry& registry);

}  // namespace nevr_script
