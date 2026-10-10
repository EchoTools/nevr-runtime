#pragma once
// One field of the hardware dump (#335). Every leaf the dump writes has the same shape, so a reader of the
// file can tell a value from a failed read without knowing the field:
//
//   {"ok":true, "source":"<api>", "value":<v>}
//   {"ok":false,"source":"<api>", "error":"<what failed>"}
//
// A failed read is a field with an error; nothing is ever omitted because it could not be read.

#include <nlohmann/json.hpp>

#include <string>

namespace nevr_quest::hwdump {

nlohmann::json Ok(const std::string& source, nlohmann::json value);
nlohmann::json Fail(const std::string& source, const std::string& error);

// "<call> failed: <strerror(err)> (errno <err>)".
std::string ErrnoText(const char* call, int err);

// Counts the leaves of `node` that are fields: {"ok":..,"source":..}. `failed` counts those with ok=false.
void CountFields(const nlohmann::json& node, std::size_t* total, std::size_t* failed);

}  // namespace nevr_quest::hwdump
