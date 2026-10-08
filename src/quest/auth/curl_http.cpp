#include "quest/auth/curl_http.h"

#include <curl/curl.h>

namespace nevr::quest_auth {

namespace {
struct Sink {
  std::string* out;
  size_t cap;
};

size_t WriteBody(void* contents, size_t size, size_t nmemb, void* user) {
  Sink* sink = static_cast<Sink*>(user);
  const size_t n = size * nmemb;
  if (sink->out->size() + n > sink->cap) return 0;  // makes curl fail with CURLE_WRITE_ERROR
  sink->out->append(static_cast<char*>(contents), n);
  return n;
}

int Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<std::atomic<bool>*>(user)->load() ? 1 : 0;  // nonzero aborts the transfer
}

void EnsureGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Owns the easy handle and the header list.
struct Easy {
  CURL* handle = curl_easy_init();
  curl_slist* headers = nullptr;
  ~Easy() {
    if (headers != nullptr) curl_slist_free_all(headers);
    if (handle != nullptr) curl_easy_cleanup(handle);
  }
};
}  // namespace

CurlHttpClient::CurlHttpClient(std::vector<std::string> ca_dirs, nevr::auth::LogSink log, long timeout_seconds,
                               bool allow_plain_http, size_t max_response_bytes)
    : ca_dirs_(std::move(ca_dirs)),
      log_(std::move(log)),
      timeout_seconds_(timeout_seconds),
      allow_plain_http_(allow_plain_http),
      max_response_bytes_(max_response_bytes) {}

void CurlHttpClient::Interrupt() { interrupted_ = true; }

const CaBundle& CurlHttpClient::Bundle() {
  std::lock_guard<std::mutex> lock(bundle_mutex_);
  if (!bundle_loaded_ || bundle_.certificates == 0) {  // a failed load is retried: the store may appear late
    bundle_ = LoadCaBundle(ca_dirs_, log_);
    bundle_loaded_ = true;
  }
  return bundle_;
}

nevr::auth::HttpResponse CurlHttpClient::PostJson(const std::string& url, const std::string& body) {
  nevr::auth::HttpResponse out;
  if (interrupted_) {
    out.transport_code = kInterrupted;
    return out;
  }
  EnsureGlobalInit();

  // Trust anchors first: with none there is nothing to verify against, and a request
  // that skipped verification is the one outcome that is never acceptable.
  std::string pem;
  if (!allow_plain_http_) {
    const CaBundle& bundle = Bundle();
    if (bundle.certificates == 0) {
      out.transport_code = kNoTrustAnchors;
      return out;
    }
    pem = bundle.pem;
  }

  Easy easy;
  if (easy.handle == nullptr) {
    out.transport_code = static_cast<int>(CURLE_FAILED_INIT);
    return out;
  }
  easy.headers = curl_slist_append(nullptr, "Content-Type: application/json");
  Sink sink{&out.body, max_response_bytes_};
  CURLcode setup = CURLE_OK;
  const auto set = [&](CURLoption option, auto value) {
    if (setup == CURLE_OK) setup = curl_easy_setopt(easy.handle, option, value);
  };

  set(CURLOPT_URL, url.c_str());
  set(CURLOPT_HTTPHEADER, easy.headers);
  set(CURLOPT_POSTFIELDS, body.c_str());
  set(CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  set(CURLOPT_WRITEFUNCTION, WriteBody);
  set(CURLOPT_WRITEDATA, &sink);
  set(CURLOPT_TIMEOUT, timeout_seconds_);
  set(CURLOPT_CONNECTTIMEOUT, timeout_seconds_);
  set(CURLOPT_NOSIGNAL, 1L);  // worker thread: no SIGALRM resolver timeouts
  set(CURLOPT_FOLLOWLOCATION, 0L);
  set(CURLOPT_PROTOCOLS_STR, allow_plain_http_ ? "http,https" : "https");
  set(CURLOPT_PROXY, "");      // ignore http_proxy/https_proxy/all_proxy from the environment
  set(CURLOPT_NOPROXY, "*");
  set(CURLOPT_NOPROGRESS, 0L);
  set(CURLOPT_XFERINFOFUNCTION, Progress);
  set(CURLOPT_XFERINFODATA, &interrupted_);
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
    out.transport_code = static_cast<int>(setup);
    if (log_) {
      log_(nevr::auth::LogLevel::Error,
           std::string("[NEVR.AUTH] HTTP client setup failed curl_code=") + std::to_string(static_cast<int>(setup)) +
               " (" + curl_easy_strerror(setup) + ")");
    }
    return out;
  }

  const CURLcode res = curl_easy_perform(easy.handle);
  long status = 0;
  curl_easy_getinfo(easy.handle, CURLINFO_RESPONSE_CODE, &status);

  out.transport_ok = (res == CURLE_OK);
  out.transport_code = static_cast<int>(res);
  out.status = status;
  if (res == CURLE_ABORTED_BY_CALLBACK && interrupted_) out.transport_code = kInterrupted;
  return out;
}

}  // namespace nevr::quest_auth
