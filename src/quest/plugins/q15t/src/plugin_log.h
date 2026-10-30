// Logging through the host: every line reaches the sentinel logger as `[NEVR.PLUGIN] plugin=<id> ...`.
#pragma once

#include <cstdint>

#include "nevr/plugin_interface.h"

namespace q15t {

// Set once by NvrPluginInitEx, before any hook is installed. A plain pointer: no constructor runs at load time.
void SetLogContext(const NvrGameContext* ctx);

// One structured line with up to 8 key/value fields. `message` is plain text. Safe from any thread; a context without
// a log function drops the line (the caller cannot do more).
void Log(std::uint32_t level, const char* message, const NevrLogField* fields = nullptr, std::uint32_t count = 0);

// Convenience: a line with number-valued fields, formatted into a small stack buffer.
struct KV {
  const char* key;
  long long value;
};
void LogNums(std::uint32_t level, const char* message, const KV* kvs, std::uint32_t count);

}  // namespace q15t
