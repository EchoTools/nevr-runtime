/* The platform seam of the cargo code (docs/adr/0045-cdn-texture-delivery.md): everything the fetch, the cache and the mount need
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
#include <vector>

namespace nevr_cargo {

class Hasher {
 public:
  virtual ~Hasher() = default;
  virtual void Update(const void* data, std::size_t length) = 0;
  virtual std::string Hex() = 0;  // 64 lowercase hex characters; the hasher is reset afterwards
};

// A file being written beside its final name. Commit makes it durable and gives it the final name in one step (flush to disk,
// close, rename, flush the directory); Abort closes it and deletes it. A TempFile destroyed without either deletes the file.
class TempFile {
 public:
  virtual ~TempFile() = default;
  virtual bool Write(const void* data, std::size_t length) = 0;
  virtual bool Commit(const std::string& final_path) = 0;
  virtual void Abort() = 0;
};

struct HttpResult {
  bool completed = false;  // a response was received and its body, if any, was delivered to the end
  long status = 0;         // the HTTP status; 0 when no response was received
  int code = 0;            // a transport-specific failure code for the log
  bool cancelled = false;  // the transfer was ended because the caller said stop
};
// Delivers body bytes with the status they came with; returns false to end the transfer.
using HttpBody = std::function<bool(long status, const void* data, std::size_t length)>;

enum class EventLevel { kInfo, kWarn, kError };

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
  virtual std::unique_ptr<TempFile> CreateTemp(const std::string& dir) = 0;
  // Writes `data` to `path` whole: a temp file in the same directory, flushed, renamed over `path`, the directory flushed.
  virtual bool WriteFileAtomically(const std::string& path, const std::string& data) = 0;

  // Blobs. OpenBlob opens read-only and gives the handle the engine will be given for it (on the Quest the game's own file open),
  // and the file's size. A blob that is mounted stays open for the life of the process.
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
