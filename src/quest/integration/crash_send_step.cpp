#include "quest/integration/crash_send_step.h"

#include "quest/auth/curl_http.h"
#include "quest/sentinel/hook_log.h"

#include <atomic>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace nevr_quest::integration {
namespace {

std::atomic<bool> g_started{false};

long Seconds(long ms) { return ms <= 0 ? 1 : (ms + 999) / 1000; }

bool RandomBytes(std::uint8_t* bytes, std::size_t count) {
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  std::size_t got = 0;
  while (got < count) {
    const ssize_t n = ::read(fd, bytes + got, count - got);
    if (n <= 0) break;
    got += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return got == count;
}

void LogEvent(const nevr_crash_send::LogEvent& e) {
  const sentinel::LogLevel level = (std::string(e.event) == "sent" || std::string(e.event) == "pass") ? sentinel::LogLevel::kInfo
                                                                                                      : sentinel::LogLevel::kWarn;
  sentinel::LogFields(level, "crash_send",
                      {{"state", e.event}, {"report", e.report.c_str()}, {"status", e.status}, {"error", e.errorCode},
                       {"bytes", e.bytes}, {"detail", e.detail.c_str()}});
}

struct Job {
  nevr_crash_send::Config config;
};

void* SendMain(void* parameter) {
  // Never freed: the thread lives as long as the process.
  Job* job = static_cast<Job*>(parameter);
  try {
    nevr_crash_send::SendPending(job->config);
  } catch (const std::exception& error) {
    nevr_crash_send::LogEvent event;
    event.event = "failed";
    event.detail = error.what();
    LogEvent(event);
  }
  return nullptr;
}

}  // namespace

nevr_crash_send::PostResult CurlCrashPost(void*, const std::string& url, const std::string& body,
                                          const nevr_crash_send::Timeouts& timeouts) {
  nevr_crash_send::PostResult result;
  try {
    nevr::quest_auth::CurlHttpClient client;
    const nevr::auth::HttpResponse response =
        client.PostJsonWithHeaders(url, body, std::vector<std::string>(), Seconds(timeouts.connectMs), Seconds(timeouts.totalMs));
    result.transportOk = response.transport_ok;
    result.status = response.status;
    result.errorCode = response.transport_code;
  } catch (const std::exception&) {
    result.errorCode = -3;  // out of memory building the request: not a status, so the report stays for the next start
  }
  return result;
}

void StartCrashSend(const char* dir, const char* build) noexcept {
  if (*nevr_crash_send::ConfiguredUrl() == '\0') return;  // switched off in this build: nothing runs
  if (g_started.exchange(true)) return;
  try {
    Job* job = new Job();
    job->config.dir = dir != nullptr ? dir : "";
    job->config.url = nevr_crash_send::ConfiguredUrl();
    job->config.build = build != nullptr ? build : "";
    job->config.platform = "quest";
#if defined(__ANDROID__)
    char model[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.product.model", model) > 0) job->config.headset = model;
#endif
    job->config.post = &CurlCrashPost;
    job->config.random = &RandomBytes;
    job->config.log = &LogEvent;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int rc = pthread_create(&thread, &attributes, &SendMain, job);
    pthread_attr_destroy(&attributes);
    if (rc != 0) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "crash_send",
                          {{"state", "failed"}, {"detail", "the send thread could not be started"}, {"errno", rc}});
      return;
    }
    pthread_setname_np(thread, "nevr-crash-send");
  } catch (const std::exception&) {
    // Out of memory: the game starts without the send; nothing else depends on it.
  }
}

}  // namespace nevr_quest::integration
