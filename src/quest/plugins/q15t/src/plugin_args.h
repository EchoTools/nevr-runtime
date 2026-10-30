// Reads one string value from the plugin's args_json: the flat JSON object config.yaml's `args:` becomes (nevr-runtime
// plugin_load_plan_build.h: ArgsToJson, every value a JSON string, keys dotted, "{}" when there are none).
//
// Bounded and null-safe: a null or malformed text, a missing key, a non-string value or a value that does not fit the buffer all
// answer false and leave `out` as an empty string. Nothing is allocated; nothing reads past the terminating NUL.
#pragma once

#include <cstddef>

namespace q15t {

bool ArgString(const char* args_json, const char* key, char* out, std::size_t cap);

}  // namespace q15t
