#include "quest/auth/curl_http.h"

#include <curl/curl.h>

#include <mutex>

namespace nevr::quest_auth {

namespace {
size_t WriteBody(void* contents, size_t size, size_t nmemb, void* out) {
  static_cast<std::string*>(out)->append(static_cast<char*>(contents), size * nmemb);
  return size * nmemb;
}

void EnsureGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}
}  // namespace

CurlHttpClient::CurlHttpClient(std::string ca_dir, long timeout_seconds, bool allow_plain_http)
    : ca_dir_(std::move(ca_dir)), timeout_seconds_(timeout_seconds), allow_plain_http_(allow_plain_http) {}

nevr::auth::HttpResponse CurlHttpClient::PostJson(const std::string& url, const std::string& body) {
  nevr::auth::HttpResponse out;
  EnsureGlobalInit();
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    out.transport_code = static_cast<int>(CURLE_FAILED_INIT);
    return out;
  }
  curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds_);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds_);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // worker thread: no SIGALRM resolver timeouts
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, allow_plain_http_ ? "http,https" : "https");
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  if (!ca_dir_.empty()) curl_easy_setopt(curl, CURLOPT_CAPATH, ca_dir_.c_str());

  const CURLcode res = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  out.transport_ok = (res == CURLE_OK);
  out.transport_code = static_cast<int>(res);
  out.status = status;
  return out;
}

}  // namespace nevr::quest_auth
