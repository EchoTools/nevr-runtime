/* The PC's nevr_cdn::Env (docs/adr/0045-cdn-texture-delivery.md): Win32 files, libcurl for the network, SHA-256 from BCrypt, and the
 * runtime's Log() for events. Only what differs from the POSIX edge (env_posix.h) lives here: hashing, verification, limits and path
 * checks are the core's (cdn_store, cdn_validate, cdn_limits).
 *
 * A store file is opened read-only with CreateFileW and its handle is the HANDLE, as an int64: the engine reads a package with the
 * handle it finds in the resource request, through ReadFile with an OVERLAPPED offset (CFile::ReadAt), the same kind of handle the
 * runtime's resource sets put in their shadows (patch/resource_sets.cpp). A blob that is mounted stays open for the life of the
 * process, so the file the engine reads is the file that was hashed.
 *
 * Paths are UTF-8 in and out of the core; this edge converts them to wide paths at each Win32 call.
 */
#pragma once

#include "cdn/cdn_env.h"

namespace nevr_cdn {

class WindowsEnv : public Env {
 public:
  WindowsEnv() = default;

  bool ReadSmallFile(const std::string& path, std::string* out, std::size_t max_bytes) override;
  bool MakeDirs(const std::string& dir) override;
  bool WriteFileAtomically(const std::string& path, const std::string& data) override;
  std::unique_ptr<AppendFile> OpenAppend(const std::string& path) override;
  bool QueryDevice(const std::string& dir, DeviceFacts* out) override;
  bool OpenBlob(const std::string& path, std::int64_t* handle, std::uint64_t* size) override;
  bool ReadBlob(std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length, std::size_t* got) override;
  void CloseBlob(std::int64_t handle) override;
  std::unique_ptr<Hasher> NewHasher() override;
  HttpResult Get(const std::string& url, const HttpBody& on_body, const std::function<bool()>& keep_going) override;
  std::uint64_t NowMs() override;
  void Event(EventLevel level, const char* name, const std::vector<Field>& fields) override;
};

}  // namespace nevr_cdn
