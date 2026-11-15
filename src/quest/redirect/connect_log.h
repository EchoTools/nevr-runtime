// The log of the game's CSysHttp::CreateConnection calls: which host each one was for, what the redirect and the outbound block
// made of it, and what the original returned. The game's service-status request (GET status/services,news?env=%s&projectid=rad14,
// libr15 0x2baa0d2) rides one of these connections, and a run that shows "No Server Status" could not say which host it went to
// or whether the redirect touched it, because a hooked call never logs and the counters hold no host (nevr-runtime#408).
//
// Two halves, joined by a ring of fixed slots in static storage:
//   NoteConnect  runs inside the hooked call, after the original returned. Atomics and bounded copies only: no heap, no lock,
//                no log, no exception. It copies the HOST of the game's URL and of the URL the original was given, never the
//                path, the query, a fragment or userinfo (the status request's query and any key in it stay out of the log).
//   DrainConnectLog  runs on the sentinel's reporter thread (sentinel::RegisterReportDrain), where logging is allowed. One
//                `http_connect` line per connection, in order, with the host, its class, the decision and the original's result.
// A ring of kConnectLogSlots holds what a pass of the reporter has not drained; a connect that overwrites an undrained slot is
// counted and said once per pass in `http_connect_dropped`. The sentinel's counter table (sentinel::kMaxReportCounters) is full,
// so this adds no counter.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_quest::redirect {

// The slots of the ring. The game opens a handful of connections in a session (four in a 2-minute headset run), so 32 per reporter
// pass is far above it; a burst past it is counted, not blocked.
inline constexpr std::size_t kConnectLogSlots = 32;

// A host:port is copied into a fixed buffer of this many bytes, NUL included; a longer one is cut and flagged.
inline constexpr std::size_t kConnectHostBytes = 64;

// A host copied out of a URL: the authority without a scheme, userinfo, path, query or fragment.
struct ConnectHost {
  char text[kConnectHostBytes];
  bool truncated;
};

// The host of `url` ("https://", "http://" or none; any letter case; optional userinfo, port, path, query, fragment). An empty
// text for a null or empty URL. Allocation-free, never throws.
void HostOfUrl(const char* url, ConnectHost* out) noexcept;

// One finished CreateConnection call: `gameUrl` is what the game passed, `usedUrl` what the original was given, `result` what it
// returned. Called by the thunk through a pointer (tstring_thunks.h NoteConnectFn).
void NoteConnect(const char* gameUrl, const char* usedUrl, int result) noexcept;

// Writes the `http_connect` lines for the connections noted since the last call, and `http_connect_dropped` when slots were lost.
// At most kConnectLogSlots lines per call. Reporter thread only; one caller at a time.
void DrainConnectLog() noexcept;

// Test support: empties the ring and zeroes its counts.
void ResetConnectLogForTest() noexcept;

}  // namespace nevr_quest::redirect
