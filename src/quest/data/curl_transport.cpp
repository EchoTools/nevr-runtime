#include "quest/data/curl_transport.h"

#include <curl/curl.h>

#include <mutex>

namespace nevr_quest::data {

namespace {

struct Context {
  const BodyFn* on_body;
  const KeepGoingFn* keep_going;
  CURL* handle;
  bool body_refused = false;  // on_body returned false
  bool stopped = false;       // keep_going returned false
};

size_t WriteBody(void* contents, size_t size, size_t nmemb, void* user) {
  Context* ctx = static_cast<Context*>(user);
  const size_t n = size * nmemb;
  long status = 0;
  curl_easy_getinfo(ctx->handle, CURLINFO_RESPONSE_CODE, &status);
  if (!(*ctx->on_body)(status, contents, n)) {
    ctx->body_refused = true;
    return 0;  // CURLE_WRITE_ERROR
  }
  return n;
}

int Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  Context* ctx = static_cast<Context*>(user);
  if (!(*ctx->keep_going)()) {
    ctx->stopped = true;
    return 1;  // nonzero aborts the transfer
  }
  return 0;
}

void EnsureGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Easy {
  CURL* handle = curl_easy_init();
  ~Easy() {
    if (handle != nullptr) curl_easy_cleanup(handle);
  }
};

}  // namespace

CurlTransport::CurlTransport(std::string roots_pem, bool allow_plain_http, long connect_timeout_seconds,
                             long stall_seconds)
    : roots_pem_(std::move(roots_pem)),
      allow_plain_http_(allow_plain_http),
      connect_timeout_seconds_(connect_timeout_seconds),
      stall_seconds_(stall_seconds) {}

TransportResult CurlTransport::GetRange(const std::string& url, std::uint64_t offset, const BodyFn& on_body,
                                        const KeepGoingFn& keep_going) {
  TransportResult out;
  EnsureGlobalInit();

  // Trust anchors first: with none there is nothing to verify against, and a request that skipped verification is
  // never acceptable.
  std::string pem;
  if (!allow_plain_http_) {
    if (roots_pem_.empty()) {
      out.code = kNoTrustAnchors;
      out.detail = "no_trust_anchors";
      return out;
    }
    pem = roots_pem_;
  }

  Easy easy;
  if (easy.handle == nullptr) {
    out.code = static_cast<int>(CURLE_FAILED_INIT);
    return out;
  }
  Context ctx{&on_body, &keep_going, easy.handle};
  const std::string range = std::to_string(offset) + "-";  // explicit: a suffix range is answered with a page
  CURLcode setup = CURLE_OK;
  const auto set = [&](CURLoption option, auto value) {
    if (setup == CURLE_OK) setup = curl_easy_setopt(easy.handle, option, value);
  };
  set(CURLOPT_URL, url.c_str());
  set(CURLOPT_HTTPGET, 1L);
  set(CURLOPT_RANGE, range.c_str());
  set(CURLOPT_WRITEFUNCTION, WriteBody);
  set(CURLOPT_WRITEDATA, &ctx);
  set(CURLOPT_CONNECTTIMEOUT, connect_timeout_seconds_);
  set(CURLOPT_LOW_SPEED_LIMIT, 1024L);
  set(CURLOPT_LOW_SPEED_TIME, stall_seconds_);
  set(CURLOPT_NOSIGNAL, 1L);
  set(CURLOPT_QUICK_EXIT, 1L);
  set(CURLOPT_FOLLOWLOCATION, 0L);
  set(CURLOPT_PROTOCOLS_STR, allow_plain_http_ ? "http,https" : "https");
  set(CURLOPT_PROXY, "");
  set(CURLOPT_NOPROXY, "*");
  set(CURLOPT_NOPROGRESS, 0L);
  set(CURLOPT_XFERINFOFUNCTION, Progress);
  set(CURLOPT_XFERINFODATA, &ctx);
  set(CURLOPT_SSL_VERIFYPEER, 1L);
  set(CURLOPT_SSL_VERIFYHOST, 2L);
  if (!pem.empty()) {
    curl_blob blob;
    blob.data = &pem[0];
    blob.len = pem.size();
    blob.flags = CURL_BLOB_COPY;
    set(CURLOPT_CAINFO_BLOB, &blob);
  }
  if (setup != CURLE_OK) {
    out.code = static_cast<int>(setup);
    out.detail = curl_easy_strerror(setup);
    return out;
  }

  const CURLcode res = curl_easy_perform(easy.handle);
  long status = 0;
  curl_easy_getinfo(easy.handle, CURLINFO_RESPONSE_CODE, &status);
  out.http_status = status;
  out.code = static_cast<int>(res);
  out.completed = (res == CURLE_OK);
  out.interrupted = ctx.stopped;
  if (!out.completed) out.detail = curl_easy_strerror(res);
  return out;
}

}  // namespace nevr_quest::data
