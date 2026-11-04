// Where the extractor reads from: a file (the OBB), a memory block (a test image), or a decrypting view of another source
// (obb_cipher.h). A read is exact: the whole range, or a Status that names the short read.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "quest/obb/status.h"

namespace nevr_quest::obb {

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  virtual std::uint64_t Size() const = 0;
  // Reads exactly `length` bytes at `offset` into `dst`. A range that is not inside Size() is `source_short_read`.
  virtual Status ReadAt(std::uint64_t offset, void* dst, std::size_t length) const = 0;
};

class MemorySource final : public ByteSource {
 public:
  explicit MemorySource(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
  std::uint64_t Size() const override { return bytes_.size(); }
  Status ReadAt(std::uint64_t offset, void* dst, std::size_t length) const override;

 private:
  std::vector<std::uint8_t> bytes_;
};

class FileSource final : public ByteSource {
 public:
  FileSource() = default;
  ~FileSource() override;
  FileSource(const FileSource&) = delete;
  FileSource& operator=(const FileSource&) = delete;
  // Opens `path` read-only. `source_open_failed` names the path and errno.
  Status Open(const std::string& path);
  std::uint64_t Size() const override { return size_; }
  Status ReadAt(std::uint64_t offset, void* dst, std::size_t length) const override;

 private:
  int fd_ = -1;
  std::uint64_t size_ = 0;
  std::string path_;
};

}  // namespace nevr_quest::obb
