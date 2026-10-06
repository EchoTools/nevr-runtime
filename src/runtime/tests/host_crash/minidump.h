#pragma once

// Bounds-checked parser for the parts of a minidump the crash harness asserts on:
// header signature/version, the exception stream (faulting thread id, exception
// code and address) and the thread list. Every read is range-checked against the
// caller-owned buffer; a malformed or truncated dump yields null/false rather
// than an over-read. Shared by test_host_minidump (mutation-checked) and the
// child-process crash harness (host_crash_test.cpp).

#include <windows.h>
#include <dbghelp.h>

#include <cstddef>
#include <cstdint>

namespace NevrHostTest {

// 'MDMP' little-endian and MINIDUMP_VERSION (42899), spelled as integers rather
// than the multichar MINIDUMP_SIGNATURE macro (which trips -Werror=multichar).
inline constexpr ULONG32 kMinidumpSignature = 0x504D444D;
inline constexpr ULONG32 kMinidumpVersion = 0xA793;

class MinidumpView {
 public:
  MinidumpView() = default;
  MinidumpView(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  void Reset(const std::uint8_t* data, std::size_t size) {
    data_ = data;
    size_ = size;
  }

  bool Ok() const { return data_ != nullptr && size_ >= sizeof(MINIDUMP_HEADER); }

  const MINIDUMP_HEADER* Header() const { return Ok() ? Read<MINIDUMP_HEADER>(0) : nullptr; }

  bool HasSignature() const {
    const MINIDUMP_HEADER* header = Header();
    return header != nullptr && header->Signature == kMinidumpSignature;
  }

  bool ValidVersion() const {
    const MINIDUMP_HEADER* header = Header();
    return header != nullptr && header->Version == kMinidumpVersion;
  }

  // The directory array, or null when the header's directory range is out of
  // bounds or the stream count is zero.
  const MINIDUMP_DIRECTORY* Directory() const {
    const MINIDUMP_HEADER* header = Header();
    if (header == nullptr || header->NumberOfStreams == 0) return nullptr;
    const std::size_t rva = header->StreamDirectoryRva;
    const std::size_t count = header->NumberOfStreams;
    const std::size_t bytes = count * sizeof(MINIDUMP_DIRECTORY);
    if (rva > size_ || bytes > size_ - rva) return nullptr;
    return Read<MINIDUMP_DIRECTORY>(rva);
  }

  const MINIDUMP_DIRECTORY* Stream(MINIDUMP_STREAM_TYPE type) const {
    const MINIDUMP_DIRECTORY* directory = Directory();
    if (directory == nullptr) return nullptr;
    const std::size_t count = Header()->NumberOfStreams;
    for (std::size_t index = 0; index < count; ++index) {
      if (directory[index].StreamType == static_cast<ULONG32>(type)) return &directory[index];
    }
    return nullptr;
  }

  // The stream's bytes and size, or null when absent or out of bounds.
  const std::uint8_t* StreamData(MINIDUMP_STREAM_TYPE type, std::size_t* out_size) const {
    if (out_size != nullptr) *out_size = 0;
    const MINIDUMP_DIRECTORY* entry = Stream(type);
    if (entry == nullptr) return nullptr;
    const std::size_t rva = entry->Location.Rva;
    const std::size_t data_size = entry->Location.DataSize;
    if (rva > size_ || data_size > size_ - rva) return nullptr;
    if (out_size != nullptr) *out_size = data_size;
    return data_ + rva;
  }

  bool ExceptionInfo(std::uint32_t* thread_id, std::uint32_t* code,
                     std::uint64_t* address) const {
    std::size_t stream_size = 0;
    const std::uint8_t* data = StreamData(ExceptionStream, &stream_size);
    if (data == nullptr || stream_size < sizeof(MINIDUMP_EXCEPTION_STREAM)) return false;
    const auto* stream = reinterpret_cast<const MINIDUMP_EXCEPTION_STREAM*>(data);
    if (thread_id != nullptr) *thread_id = stream->ThreadId;
    if (code != nullptr) *code = stream->ExceptionRecord.ExceptionCode;
    if (address != nullptr) *address = stream->ExceptionRecord.ExceptionAddress;
    return true;
  }

  bool ContainsThread(std::uint32_t thread_id) const {
    std::size_t stream_size = 0;
    const std::uint8_t* data = StreamData(ThreadListStream, &stream_size);
    if (data == nullptr || stream_size < sizeof(MINIDUMP_THREAD_LIST)) return false;
    const auto* list = reinterpret_cast<const MINIDUMP_THREAD_LIST*>(data);
    const std::size_t count = list->NumberOfThreads;
    const std::size_t header_size = sizeof(MINIDUMP_THREAD_LIST);
    if (count == 0 || count > (stream_size - header_size) / sizeof(MINIDUMP_THREAD)) return false;
    const auto* threads = reinterpret_cast<const MINIDUMP_THREAD*>(data + header_size);
    for (std::size_t index = 0; index < count; ++index) {
      if (threads[index].ThreadId == thread_id) return true;
    }
    return false;
  }

 private:
  template <typename T>
  const T* Read(std::size_t offset) const {
    if (offset > size_ || sizeof(T) > size_ - offset) return nullptr;
    return reinterpret_cast<const T*>(data_ + offset);
  }

  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace NevrHostTest
