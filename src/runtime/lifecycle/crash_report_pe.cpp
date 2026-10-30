#include "runtime/lifecycle/crash_report_pe.h"

#include <cstring>

namespace nevr_crash_pe {

namespace {

constexpr std::uint16_t kDosMagic = 0x5a4d;       // "MZ"
constexpr std::uint32_t kPeSignature = 0x00004550;  // "PE\0\0"
constexpr std::uint16_t kOptionalMagic64 = 0x20b;
constexpr std::uint16_t kOptionalMagic32 = 0x10b;
constexpr std::size_t kFileHeaderBytes = 20;
constexpr std::size_t kMaxNames = 0x10000;
constexpr std::size_t kMaxNameBytes = 160;

bool CanRead(ReadableFn readable, const void* address, std::size_t length) {
  return readable == nullptr || readable(address, length);
}

bool ReadU16(ReadableFn readable, const std::uint8_t* at, std::uint16_t* out) {
  if (!CanRead(readable, at, sizeof(*out))) return false;
  std::memcpy(out, at, sizeof(*out));
  return true;
}

bool ReadU32(ReadableFn readable, const std::uint8_t* at, std::uint32_t* out) {
  if (!CanRead(readable, at, sizeof(*out))) return false;
  std::memcpy(out, at, sizeof(*out));
  return true;
}

// Offsets of the optional header's fields: SizeOfImage is at 56 in both layouts, the export directory entry (the
// first data directory) at 112 for PE32+ and 96 for PE32.
struct Layout {
  const std::uint8_t* optional;
  std::uint32_t size_of_image;
  std::uint32_t export_rva;
  std::uint32_t export_size;
};

bool ReadLayout(const std::uint8_t* image, ReadableFn readable, Layout* out) {
  std::uint16_t dos = 0;
  std::uint32_t lfanew = 0;
  if (!ReadU16(readable, image, &dos) || dos != kDosMagic || !ReadU32(readable, image + 0x3c, &lfanew)) return false;
  const std::uint8_t* nt = image + lfanew;
  std::uint32_t signature = 0;
  if (!ReadU32(readable, nt, &signature) || signature != kPeSignature) return false;
  const std::uint8_t* optional = nt + 4 + kFileHeaderBytes;
  std::uint16_t magic = 0;
  if (!ReadU16(readable, optional, &magic)) return false;
  std::size_t directory_at = 0;
  if (magic == kOptionalMagic64) {
    directory_at = 112;
  } else if (magic == kOptionalMagic32) {
    directory_at = 96;
  } else {
    return false;
  }
  out->optional = optional;
  if (!ReadU32(readable, optional + 56, &out->size_of_image)) return false;
  if (!ReadU32(readable, optional + directory_at, &out->export_rva) ||
      !ReadU32(readable, optional + directory_at + 4, &out->export_size)) {
    return false;
  }
  return true;
}

}  // namespace

bool ReadIdentity(const std::uint8_t* image, ReadableFn readable, ImageIdentity* out) {
  if (image == nullptr || out == nullptr) return false;
  Layout layout{};
  if (!ReadLayout(image, readable, &layout)) return false;
  std::uint32_t lfanew = 0;
  std::memcpy(&lfanew, image + 0x3c, sizeof(lfanew));
  // The file header's TimeDateStamp is at +8 from the PE signature.
  if (!ReadU32(readable, image + lfanew + 8, &out->time_date_stamp)) return false;
  out->size_of_image = layout.size_of_image;
  return true;
}

bool NearestExport(const std::uint8_t* image, ReadableFn readable, std::uint32_t rva, char* name,
                   std::size_t capacity, std::uint64_t* offset) {
  if (image == nullptr || name == nullptr || capacity == 0 || offset == nullptr) return false;
  Layout layout{};
  if (!ReadLayout(image, readable, &layout) || layout.export_rva == 0 || layout.export_size < 40) return false;
  const std::uint8_t* directory = image + layout.export_rva;
  if (!CanRead(readable, directory, 40)) return false;
  std::uint32_t function_count = 0, name_count = 0, functions_rva = 0, names_rva = 0, ordinals_rva = 0;
  std::memcpy(&function_count, directory + 20, 4);
  std::memcpy(&name_count, directory + 24, 4);
  std::memcpy(&functions_rva, directory + 28, 4);
  std::memcpy(&names_rva, directory + 32, 4);
  std::memcpy(&ordinals_rva, directory + 36, 4);
  if (name_count == 0 || name_count > kMaxNames || function_count == 0 || function_count > kMaxNames) return false;

  const auto* functions = reinterpret_cast<const std::uint32_t*>(image + functions_rva);
  const auto* names = reinterpret_cast<const std::uint32_t*>(image + names_rva);
  const auto* ordinals = reinterpret_cast<const std::uint16_t*>(image + ordinals_rva);
  if (!CanRead(readable, functions, static_cast<std::size_t>(function_count) * 4) ||
      !CanRead(readable, names, static_cast<std::size_t>(name_count) * 4) ||
      !CanRead(readable, ordinals, static_cast<std::size_t>(name_count) * 2)) {
    return false;
  }

  bool found = false;
  std::uint32_t best_rva = 0;
  std::uint32_t best_name = 0;
  for (std::uint32_t i = 0; i < name_count; ++i) {
    const std::uint16_t ordinal = ordinals[i];
    if (ordinal >= function_count) continue;
    const std::uint32_t function_rva = functions[ordinal];
    // A forwarded export's "address" is the forwarder string inside the export directory, not code.
    if (function_rva >= layout.export_rva && function_rva < layout.export_rva + layout.export_size) continue;
    if (function_rva == 0 || function_rva > rva) continue;
    if (!found || function_rva > best_rva) {
      found = true;
      best_rva = function_rva;
      best_name = names[i];
    }
  }
  if (!found) return false;

  std::size_t length = 0;
  for (; length + 1 < capacity && length < kMaxNameBytes; ++length) {
    const std::uint8_t* at = image + best_name + length;
    if (!CanRead(readable, at, 1)) break;
    if (*at == 0) break;
    name[length] = static_cast<char>(*at);
  }
  name[length] = '\0';
  *offset = static_cast<std::uint64_t>(rva - best_rva);
  return length > 0;
}

void FormatIdentity(const ImageIdentity& identity, char* out, std::size_t capacity) {
  if (out == nullptr || capacity == 0) return;
  std::size_t length = 0;
  auto put = [&](char c) {
    if (length + 1 < capacity) out[length++] = c;
  };
  auto hex = [&](std::uint32_t value) {
    bool started = false;
    for (int shift = 28; shift >= 0; shift -= 4) {
      const unsigned nibble = (value >> shift) & 0xfu;
      if (nibble != 0 || started || shift == 0) {
        put("0123456789abcdef"[nibble]);
        started = true;
      }
    }
  };
  put('p');
  put('e');
  put('-');
  hex(identity.time_date_stamp);
  put('-');
  hex(identity.size_of_image);
  out[length] = '\0';
}

}  // namespace nevr_crash_pe
