/* The Quest's nevr_cargo::Env (docs/adr/0044-cdn-texture-delivery.md): POSIX files, the data fetch's Transport (libcurl in the app) for
 * the network, SHA-256 from quest/data, and the sentinel log for events.
 *
 * A blob is opened with open(2) read-only and its handle is the descriptor: CSysFile::Open (pinned_targets.h, flags 1 is O_RDONLY)
 * gives the engine the same kind of value, and the engine reads a package with the handle it finds in the request. A blob that is
 * mounted stays open for the life of the process, so the file the engine reads is the file that was hashed.
 *
 * Built without exceptions (it runs on a worker thread of the game's process; the data step's core is built the same way).
 */
#pragma once

#include "core/cargo_env.h"
#include "quest/data/transport.h"

namespace nevr_quest::cargo {

class PosixEnv : public nevr_cargo::Env {
 public:
  // `transport` outlives the env. A null transport makes every Get fail as a transport error.
  explicit PosixEnv(nevr_quest::data::Transport* transport) : transport_(transport) {}

  bool ReadSmallFile(const std::string& path, std::string* out, std::size_t max_bytes) override;
  bool MakeDirs(const std::string& dir) override;
  std::unique_ptr<nevr_cargo::TempFile> CreateTemp(const std::string& dir) override;
  bool WriteFileAtomically(const std::string& path, const std::string& data) override;
  bool OpenBlob(const std::string& path, std::int64_t* handle, std::uint64_t* size) override;
  bool ReadBlob(std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length, std::size_t* got) override;
  void CloseBlob(std::int64_t handle) override;
  std::unique_ptr<nevr_cargo::Hasher> NewHasher() override;
  nevr_cargo::HttpResult Get(const std::string& url, const nevr_cargo::HttpBody& on_body,
                             const std::function<bool()>& keep_going) override;
  std::uint64_t NowMs() override;
  void Event(nevr_cargo::EventLevel level, const char* name, const std::vector<nevr_cargo::Field>& fields) override;

 private:
  nevr_quest::data::Transport* transport_;
};

}  // namespace nevr_quest::cargo
