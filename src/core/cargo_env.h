/* The platform seam of the cargo code (docs/adr/0045-cdn-texture-delivery.md): everything the fetch, the store and the mount need
 * from the machine, as one interface, so the same code runs on the Quest (POSIX files, libcurl, the game's file open), on the PC
 * (Win32 files, libcurl) and in a test (memory files, a scripted server, a clock that stops).
 *
 * Nothing here throws; every call that can fail returns a bool or a result and the caller decides what a failure means.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace nevr_cargo {

class Hasher {
 public:
  virtual ~Hasher() = default;
  virtual void Update(const void* data, std::size_t length) = 0;
  virtual std::string Hex() = 0;  // 64 lowercase hex characters; the hasher is reset afterwards
};

// A file that only grows by appending: a store file or the blob index. Truncate gives back the bytes of a transfer that failed.
class AppendFile {
 public:
  virtual ~AppendFile() = default;
  virtual std::uint64_t Size() const = 0;
  virtual bool Append(const void* data, std::size_t length) = 0;
  virtual bool Truncate(std::uint64_t size) = 0;  // cuts the file back to `size` (at most Size())
  virtual bool Sync() = 0;                        // data and length are durable when it returns true
};

// What the machine says about itself, read when a limit is computed (cargo_limits.h). A fact the machine did not give is absent,
// and the limits that depend on it come out as "not ok" with the name of the missing fact: no limit is a constant.
struct DeviceFacts {
  bool have_free = false;
  std::uint64_t free_bytes = 0;  // free space an unprivileged writer has in the cache directory's file system
  bool have_fds = false;
  std::uint64_t fd_soft_limit = 0;  // the process's soft limit on open files
  std::uint64_t fds_open = 0;       // files the process holds open now
  bool have_memory = false;
  std::uint64_t memory_available = 0;  // memory the system says a process can still use without swapping
};

struct HttpResult {
  bool completed = false;  // a response was received and its body, if any, was delivered to the end
  long status = 0;         // the HTTP status; 0 when no response was received
  int code = 0;            // a transport-specific failure code for the log
  bool cancelled = false;  // the transfer was ended because the caller said stop
};
// Delivers body bytes with the status they came with; returns false to end the transfer.
using HttpBody = std::function<bool(long status, const void* data, std::size_t length)>;

enum class EventLevel { kDebug, kInfo, kWarn, kError };

struct Field {
  std::string key;
  std::string text;
  std::uint64_t number = 0;
  bool is_number = false;
  Field(std::string k, std::string t) : key(std::move(k)), text(std::move(t)) {}
  Field(std::string k, const char* t) : key(std::move(k)), text(t) {}
  template <typename T, typename = std::enable_if_t<std::is_integral<T>::value>>
  Field(std::string k, T v) : key(std::move(k)), number(static_cast<std::uint64_t>(v)), is_number(true) {}
};

class Env {
 public:
  virtual ~Env() = default;

  // Files. Paths are absolute.
  virtual bool ReadSmallFile(const std::string& path, std::string* out, std::size_t max_bytes) = 0;  // false: absent, unreadable or over max
  virtual bool MakeDirs(const std::string& dir) = 0;
  // Writes `data` to `path` whole: a temp file in the same directory, flushed, renamed over `path`, the directory flushed.
  virtual bool WriteFileAtomically(const std::string& path, const std::string& data) = 0;
  // Opens `path` for appending, creating it when it is absent. Null when it cannot be opened.
  virtual std::unique_ptr<AppendFile> OpenAppend(const std::string& path) = 0;

  // The machine's own numbers for the limits (free space of `dir`'s file system, the descriptor limit, memory). False only when
  // nothing could be read; a fact that could not be read is left absent in `*out`.
  virtual bool QueryDevice(const std::string& dir, DeviceFacts* out) = 0;

  // Blobs. OpenBlob opens read-only and gives the handle the engine will be given for it (on the Quest the game's own file open),
  // and the file's size. A store file that is mounted stays open for the life of the process.
  virtual bool OpenBlob(const std::string& path, std::int64_t* handle, std::uint64_t* size) = 0;
  virtual bool ReadBlob(std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length, std::size_t* got) = 0;
  virtual void CloseBlob(std::int64_t handle) = 0;

  virtual std::unique_ptr<Hasher> NewHasher() = 0;

  // One GET of `url` from its start; the body goes to `on_body` as it arrives.
  virtual HttpResult Get(const std::string& url, const HttpBody& on_body, const std::function<bool()>& keep_going) = 0;

  virtual std::uint64_t NowMs() = 0;  // a monotonic millisecond clock

  // One structured record. `name` is the event (cargo_start ...); fields are in order.
  virtual void Event(EventLevel level, const char* name, const std::vector<Field>& fields) = 0;
};

}  // namespace nevr_cargo
