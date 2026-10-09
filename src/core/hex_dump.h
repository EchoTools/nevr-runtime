#pragma once

// Hex-dump a byte range into log-sized lines, e.g. "DE AD BE EF ". Each byte is "%02X " (the trailing
// space is part of the line, as the callers' log formats have always printed it).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace nevr {

/// At most `maxBytes` of `data`, `bytesPerLine` per returned line. The last line may be shorter.
inline std::vector<std::string> HexDumpLines(const uint8_t* data, size_t length, size_t maxBytes,
                                             size_t bytesPerLine) {
  std::vector<std::string> lines;
  if (data == nullptr || bytesPerLine == 0) return lines;
  const size_t count = std::min(length, maxBytes);
  for (size_t start = 0; start < count; start += bytesPerLine) {
    std::string line;
    const size_t end = std::min(count, start + bytesPerLine);
    for (size_t i = start; i < end; ++i) {
      char byte[4];
      std::snprintf(byte, sizeof(byte), "%02X ", data[i]);
      line += byte;
    }
    lines.push_back(std::move(line));
  }
  return lines;
}

}  // namespace nevr
