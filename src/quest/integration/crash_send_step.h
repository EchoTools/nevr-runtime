#pragma once
// The Quest half of the crash report send (core/crash_send.h, #1689): the sentinel sends the reports a crash left in
// nevr-crashes to nevr-api, from the constructor step that arms the crash reporter, before the game or the bridge exists and
// whether or not anything above the sentinel loads. The POST is the sentinel's own libcurl client (auth/curl_http.h: the
// Android CA store, peer and host verification on, no proxy from the environment, no redirect); everything above it is the
// shared header.

#include "core/crash_send.h"

namespace nevr_quest::integration {

/// The POST of the shared sender through the sentinel's CurlHttpClient. `context` is unused. Never throws.
nevr_crash_send::PostResult CurlCrashPost(void* context, const std::string& url, const std::string& body,
                                          const nevr_crash_send::Timeouts& timeouts);

/// Starts the send on a detached thread of its own, once per process, and returns at once: a dead network never delays
/// the game. `dir` is the crash folder (sentinel::DumpDir()), `build` the build string. Does nothing, and writes nothing,
/// while the build has the send switched off (nevr_crash_send::kSendEnabled). One `crash_send` sentinel log line per event;
/// a report's text and the request body are never logged.
void StartCrashSend(const char* dir, const char* build) noexcept;

}  // namespace nevr_quest::integration
