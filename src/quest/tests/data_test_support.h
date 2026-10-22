/* Test helpers shared by the data-step tests: a scratch directory that removes itself, small file helpers, and a zip
 * writer with one knob per rule the reader enforces, so each rule is exercised by an archive that breaks only it. */
#pragma once

#include <zlib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "quest/data/staging.h"

namespace data_test {

// ---- a scratch directory that removes itself ----
struct Scratch {
  std::string dir;
  Scratch() {
    // NEVR_TEST_TMP names the directory the scratch directories are made in (the recipes run from different directories).
    const char* base = std::getenv("NEVR_TEST_TMP");
    std::string tmpl = std::string(base != nullptr ? base : "build/quest-data-host") + "/work-XXXXXX";
    const char* made = ::mkdtemp(&tmpl[0]);
    if (made == nullptr) {  // a scratch directory that is not there would send every test path to the filesystem root
      std::fprintf(stderr, "Scratch: mkdtemp(%s) failed\n", tmpl.c_str());
      std::abort();
    }
    dir = made;
  }
  ~Scratch() {
    if (!dir.empty()) nevr_quest::data::RemoveTree(dir);
  }
  std::string Path(const std::string& rel) const { return dir + "/" + rel; }
};

inline bool Exists(const std::string& path) {
  struct stat st;
  return ::lstat(path.c_str(), &st) == 0;
}

inline std::string ReadFile(const std::string& path) {
  std::string out;
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return out;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<std::size_t>(n));
  ::close(fd);
  return out;
}

inline void WriteFile(const std::string& path, const std::string& data) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return;
  if (!data.empty()) {
    const ssize_t n = ::write(fd, data.data(), data.size());
    (void)n;
  }
  ::close(fd);
}

// ---- a zip writer: one knob per rule the reader enforces ----
struct SrcEntry {
  SrcEntry(std::string n, std::string d, std::uint16_t m = 8) : name(std::move(n)), data(std::move(d)), method(m) {}
  std::string name;
  std::string data;
  std::uint16_t method = 8;
  std::uint16_t flags = 0;
  std::uint16_t made_by = 20;
  std::uint32_t external = 0;
  std::int64_t crc_delta = 0;     // added to the CRC the directory records
  std::int64_t usize_delta = 0;   // added to the uncompressed size the directory records
  std::string raw_payload;        // when not empty, the bytes stored as the entry's payload instead of the compressed data
  bool use_raw = false;
};

inline void Put16(std::string& s, std::uint16_t v) {
  s.push_back(static_cast<char>(v & 0xff));
  s.push_back(static_cast<char>(v >> 8));
}
inline void Put32(std::string& s, std::uint32_t v) {
  Put16(s, static_cast<std::uint16_t>(v & 0xffff));
  Put16(s, static_cast<std::uint16_t>(v >> 16));
}

inline std::string Deflate(const std::string& in) {
  z_stream zs;
  std::memset(&zs, 0, sizeof(zs));
  deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  std::string out(deflateBound(&zs, static_cast<uLong>(in.size())) + 16, '\0');
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
  zs.avail_out = static_cast<uInt>(out.size());
  deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

struct ZipOptions {
  bool zip64_marker = false;     // 0xFFFF in the end record's entry counts
  std::uint16_t disk = 0;
};

inline std::string BuildZip(const std::vector<SrcEntry>& entries, const ZipOptions& opt = {}) {
  std::string body, central;
  for (const SrcEntry& e : entries) {
    const bool dir = !e.name.empty() && e.name.back() == '/';
    std::string payload = dir ? std::string() : (e.method == 8 ? Deflate(e.data) : e.data);
    if (e.use_raw) payload = e.raw_payload;
    const std::uint32_t crc = static_cast<std::uint32_t>(
        crc32(crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(e.data.data()), static_cast<uInt>(e.data.size())));
    const std::uint32_t rec_crc = static_cast<std::uint32_t>(static_cast<std::int64_t>(crc) + e.crc_delta);
    const std::uint32_t usize = static_cast<std::uint32_t>(static_cast<std::int64_t>(e.data.size()) + e.usize_delta);
    const std::uint32_t offset = static_cast<std::uint32_t>(body.size());
    Put32(body, 0x04034b50);
    Put16(body, 20);
    Put16(body, e.flags);
    Put16(body, e.method);
    Put32(body, 0);
    Put32(body, rec_crc);
    Put32(body, static_cast<std::uint32_t>(payload.size()));
    Put32(body, usize);
    Put16(body, static_cast<std::uint16_t>(e.name.size()));
    Put16(body, 0);
    body += e.name;
    body += payload;
    Put32(central, 0x02014b50);
    Put16(central, e.made_by);
    Put16(central, 20);
    Put16(central, e.flags);
    Put16(central, e.method);
    Put32(central, 0);
    Put32(central, rec_crc);
    Put32(central, static_cast<std::uint32_t>(payload.size()));
    Put32(central, usize);
    Put16(central, static_cast<std::uint16_t>(e.name.size()));
    Put16(central, 0);
    Put16(central, 0);
    Put16(central, 0);
    Put16(central, 0);
    Put32(central, e.external);
    Put32(central, offset);
    central += e.name;
  }
  std::string out = body;
  const std::uint32_t cd_offset = static_cast<std::uint32_t>(out.size());
  out += central;
  Put32(out, 0x06054b50);
  Put16(out, opt.disk);
  Put16(out, 0);
  const std::uint16_t count = opt.zip64_marker ? 0xFFFF : static_cast<std::uint16_t>(entries.size());
  Put16(out, count);
  Put16(out, count);
  Put32(out, static_cast<std::uint32_t>(central.size()));
  Put32(out, cd_offset);
  Put16(out, 0);
  return out;
}

inline std::string Pattern(std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>('a' + (i * 7 + i / 13) % 26));
  return s;
}

}  // namespace data_test
