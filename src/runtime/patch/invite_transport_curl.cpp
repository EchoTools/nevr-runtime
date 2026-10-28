#include "runtime/patch/invite_transport_curl.h"

#include <curl/curl.h>

#include <utility>

#include "core/curl_global.h"
#include "runtime/patch/invite_transport.h"

namespace nevr_invite {

namespace {

struct Sink {
  std::string body;
  std::size_t cap = 0;
};

// Returning less than was given aborts the transfer (CURLE_WRITE_ERROR): an answer past the cap is dropped, not truncated.
std::size_t Collect(char* data, std::size_t size, std::size_t count, void* user) {
  Sink* const sink = static_cast<Sink*>(user);
  const std::size_t total = size * count;
  if (sink->body.size() + total > sink->cap) return 0;
  sink->body.append(data, total);
  return total;
}

// A nonzero return aborts the transfer (CURLE_ABORTED_BY_CALLBACK); curl calls this about once a second while it waits.
int CheckAbort(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<const std::atomic<bool>*>(user)->load(std::memory_order_acquire) ? 1 : 0;
}

}  // namespace

CurlTransport::CurlTransport(std::string base_url, TransportOptions options)
    : base_(std::move(base_url)), options_(std::move(options)) {}

HttpAnswer CurlTransport::Post(std::string_view rpc, std::string_view body, std::string_view bearer) {
  HttpAnswer answer;
  if (aborted_.load(std::memory_order_acquire)) return answer;
  const Endpoint endpoint = BuildRpcUrl(base_, rpc);
  if (endpoint.check != UrlCheck::Ok) {
    Say(LogLevel::Warning, std::string("[NEVR.INVITE] transport refused reason=") + UrlCheckName(endpoint.check));
    return answer;
  }

  nevr::EnsureCurlGlobalInit();
  CURL* const curl = curl_easy_init();
  if (curl == nullptr) {
    Say(LogLevel::Warning, "[NEVR.INVITE] transport failed reason=curl_init");
    return answer;
  }
  const std::string authorization = "Authorization: Bearer " + std::string(bearer);
  curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
  headers = curl_slist_append(headers, authorization.c_str());
  const std::string payload(body);
  Sink sink;
  sink.cap = options_.max_body_bytes;

  curl_easy_setopt(curl, CURLOPT_URL, endpoint.url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Collect);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, options_.connect_timeout_s);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, options_.timeout_s);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &CheckAbort);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &aborted_);

  const CURLcode code = curl_easy_perform(curl);
  if (code == CURLE_OK) {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    answer.status = static_cast<int>(status);
    answer.body = std::move(sink.body);
  } else {
    Say(LogLevel::Warning, "[NEVR.INVITE] transport failed curl_code=" + std::to_string(static_cast<int>(code)));
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return answer;
}

}  // namespace nevr_invite
