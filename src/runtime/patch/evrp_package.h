#pragma once

/// `.evrp` cosmetic package parsing. The binary layout and validation rules are
/// specified in docs/adr/0005-cosmetics-cdn-format.md; tests/test_evrp_package.cpp
/// holds the accepting vector and one rejecting case per rule.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Evrp {

constexpr uint32_t kMagic = 0x50525645;  // "EVRP" little-endian
constexpr uint32_t kFormatVersion = 1;
constexpr uint8_t kSlotTypeTint = 0x01;
constexpr uint32_t kTintDataLength = 80;
constexpr size_t kHeaderSize = 28;

/// The 80 colour bytes of a tint package: 5 RGBA float32 colours, 16 bytes each.
struct TintData {
    uint8_t colors[kTintDataLength];
};

/// Parse a .evrp file buffer into symbol_id and tint data. `context` is the
/// filename the caller is parsing so every skip Warning can say WHICH package was
/// rejected, not just why. Returns true if the file is a valid tint package;
/// on false the outputs are untouched.
bool ParseTint(const std::vector<uint8_t>& data, const std::string& context,
               int64_t& out_symbol_id, TintData& out_tint);

}  // namespace Evrp
