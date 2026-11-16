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

#include <atomic>
#include <cstdint>

#include "cdn/cdn_env.h"

namespace nevr_cdn {

// NtQuerySystemInformation's shape (x64, where the calling convention is the one convention): the information class, the buffer,
// its length and the length the call needs.
using HandleTableQuery = std::int32_t (*)(std::uint32_t info_class, void* buffer, std::uint32_t length, std::uint32_t* needed);

// The handles process `pid` holds, counted in the system's handle table (SystemExtendedHandleInformation) that `query` reads; the
// Wine workaround of WindowsEnv::QueryDevice (env_windows.cpp) and its tests. False, with the call's NTSTATUS in *status (0 when
// `query` is null), when the table cannot be read or holds no entry of `pid`.
bool CountProcessHandles(HandleTableQuery query, std::uint64_t pid, std::uint64_t* count, std::int32_t* status);

// The open-handle fact QueryDevice reports, and why. `zero_count` is the workaround's condition: the platform's count succeeded
// and said 0, which no running process has (Wine's GetProcessHandleCount stub); the count then comes from the handle table.
// `reason` is one fixed token: handle_count_nonzero (the platform's count, kept), platform_count_failed, handle_count_zero (the
// table's count), no_handle_table (no query to read it with), table_unreadable (`status` is the read's NTSTATUS) or
// no_entry_of_process (the table was read and holds no handle of this process). `error` is the Win32 error of a failed
// platform call (platform_count_failed) or of the failed lookup of the table's query (no_handle_table).
struct HandleFact {
  bool have = false;
  std::uint64_t open = 0;
  bool zero_count = false;
  std::int32_t status = 0;
  std::uint32_t error = 0;
  const char* reason = "platform_count_failed";
};
// Resolves the table's query; null, with the Win32 error in *error, when it cannot.
using HandleTableQueryResolver = HandleTableQuery (*)(std::uint32_t* error);
// `platform_error` is the platform call's Win32 error when `platform_ok` is false. `table_query` is called only on the zero-count
// path, so Windows never resolves the table's query.
HandleFact OpenHandleFact(bool platform_ok, std::uint64_t platform_count, std::uint32_t platform_error,
                          HandleTableQueryResolver table_query, std::uint64_t pid);
// Sets the descriptor facts of `out` from `fact`: the count and the Windows handle limit, or nothing when `fact` has no count.
void ApplyHandleFact(const HandleFact& fact, DeviceFacts* out);

// The record of the decision (ADR 0032 rule 3: acted or passed through), `device_handles`, with its level and fields. A WindowsEnv
// writes it once, at its first QueryDevice.
inline constexpr const char* kHandleDecisionEvent = "device_handles";
void HandleDecisionRecord(const HandleFact& fact, EventLevel* level, std::vector<Field>* fields);

// The stall window of a Get (libcurl's CURLOPT_LOW_SPEED_TIME, in seconds, with the floor of 1024 bytes a second): a transfer slower
// than the floor for this long is given up. The Quest transport uses the same 30 s (quest/data/curl_transport.h); production
// keeps it, and only a test passes a shorter one.
inline constexpr long kShippedStallSeconds = 30;

class WindowsEnv : public Env {
 public:
  // `event_tag` starts every Event line: "[NEVR.CDN]" for the CDN, "[NEVR.MAPS]" for the map channel. `allow_plain_http` admits
  // http:// in Get; production leaves it off (the CDN is https), and the loopback tests turn it on, as the Quest's transport does.
  // `stall_seconds` is the stall window Get gives up on; production uses kShippedStallSeconds.
  explicit WindowsEnv(const char* event_tag = "[NEVR.CDN]", bool allow_plain_http = false,
                      long stall_seconds = kShippedStallSeconds);

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

 private:
  std::string tag_;
  bool allow_plain_http_;
  long stall_seconds_;
  std::atomic<bool> handle_count_logged_{false};  // the first decision of the handle-count workaround is logged (ADR 0032 rule 3)
};

}  // namespace nevr_cdn
