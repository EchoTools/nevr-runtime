#include "quest/net/curl_ws_connector.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <mutex>

namespace quest_net {

using SessionRouter::LogLevel;

ConnectStatus ClassifyCurlCode(int code, long httpStatus) {
  switch (static_cast<CURLcode>(code)) {
    case CURLE_OK:
      return ConnectStatus::Ok;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_INVALIDCERTSTATUS:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
      return ConnectStatus::TlsVerificationFailed;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_ENGINE_INITFAILED:
      return ConnectStatus::TlsError;
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_NOT_BUILT_IN:
      return ConnectStatus::PolicyRefused;
    case CURLE_HTTP_RETURNED_ERROR:
    case CURLE_WEIRD_SERVER_REPLY:
    case CURLE_RECV_ERROR:
      return httpStatus > 0 && httpStatus != 101 ? ConnectStatus::HandshakeRejected : ConnectStatus::NetworkError;
    default:
      return httpStatus > 0 && httpStatus != 101 ? ConnectStatus::HandshakeRejected : ConnectStatus::NetworkError;
  }
}

namespace {

void GlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

class CurlWsConnection final : public WsConnection {
 public:
  CurlWsConnection(CURL* curl, curl_socket_t socket, std::size_t maxMessageBytes)
      : curl_(curl), socket_(socket), maxMessageBytes_(maxMessageBytes) {
    if (::pipe2(wake_, O_NONBLOCK | O_CLOEXEC) != 0) {
      wake_[0] = -1;
      wake_[1] = -1;
    }
  }
  ~CurlWsConnection() override {
    curl_easy_cleanup(curl_);
    if (wake_[0] >= 0) ::close(wake_[0]);
    if (wake_[1] >= 0) ::close(wake_[1]);
  }

  bool Send(std::string_view data, bool binary) override {
    const unsigned int flags = binary ? CURLWS_BINARY : CURLWS_TEXT;
    std::size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (offset < data.size() || data.empty()) {
      std::size_t sent = 0;
      const CURLcode rc = curl_ws_send(curl_, data.data() + offset, data.size() - offset, &sent, 0, flags);
      if (rc == CURLE_OK) {
        offset += sent;
        if (offset >= data.size()) return true;
        continue;
      }
      if (rc != CURLE_AGAIN || std::chrono::steady_clock::now() > deadline) return false;
      pollfd p{socket_, POLLOUT, 0};
      ::poll(&p, 1, 250);
    }
    return true;
  }

  RecvResult Recv(int timeoutMs) override {
    for (int attempt = 0; attempt < 2; ++attempt) {
      RecvResult r;
      if (TryRecv(&r)) return r;
      if (attempt == 1) break;
      pollfd fds[2] = {{socket_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
      const int ready = ::poll(fds, 2, timeoutMs);
      if (ready < 0 && errno != EINTR) {
        r.status = RecvStatus::Error;
        return r;
      }
      if (ready > 0 && (fds[1].revents & POLLIN) != 0) {
        char buf[32];
        while (::read(wake_[0], buf, sizeof(buf)) > 0) {
        }
        return RecvResult();
      }
      if (ready <= 0) return RecvResult();
    }
    return RecvResult();
  }

  void Wake() override {
    const char byte = 1;
    if (wake_[1] >= 0) {
      const ssize_t n = ::write(wake_[1], &byte, 1);
      (void)n;
    }
  }

  void SendClose(uint16_t code) override {
    const char payload[2] = {static_cast<char>(code >> 8), static_cast<char>(code & 0xFF)};
    std::size_t sent = 0;
    curl_ws_send(curl_, payload, sizeof(payload), &sent, 0, CURLWS_CLOSE);
  }

 private:
  // Reads whatever curl has. True when `out` is a result (message, close or error); false when more data is
  // needed (CURLE_AGAIN) and nothing completed.
  bool TryRecv(RecvResult* out) {
    for (;;) {
      char buf[16384];
      std::size_t got = 0;
      const struct curl_ws_frame* meta = nullptr;
      const CURLcode rc = curl_ws_recv(curl_, buf, sizeof(buf), &got, &meta);
      if (rc == CURLE_AGAIN) return false;
      if (rc != CURLE_OK || meta == nullptr) {
        out->status = RecvStatus::Error;
        return true;
      }
      if ((meta->flags & CURLWS_CLOSE) != 0) {
        out->status = RecvStatus::Closed;
        out->closeCode = got >= 2 ? static_cast<uint16_t>((static_cast<uint8_t>(buf[0]) << 8) | static_cast<uint8_t>(buf[1]))
                                  : static_cast<uint16_t>(1005);
        const char echo[2] = {static_cast<char>(out->closeCode >> 8), static_cast<char>(out->closeCode & 0xFF)};
        std::size_t sent = 0;
        if (got >= 2) curl_ws_send(curl_, echo, sizeof(echo), &sent, 0, CURLWS_CLOSE);
        return true;
      }
      if ((meta->flags & (CURLWS_PING | CURLWS_PONG)) != 0) continue;  // libcurl answers pings itself
      if ((meta->flags & CURLWS_BINARY) != 0) binary_ = true;
      if ((meta->flags & CURLWS_TEXT) != 0) binary_ = false;
      if (partial_.size() + got > maxMessageBytes_) {
        out->status = RecvStatus::Error;  // oversized: end the session rather than buffer it
        return true;
      }
      partial_.append(buf, got);
      if (meta->bytesleft == 0 && (meta->flags & CURLWS_CONT) == 0) {
        out->status = RecvStatus::Frame;
        out->data = std::move(partial_);
        out->binary = binary_;
        partial_.clear();
        return true;
      }
    }
  }

  CURL* curl_;
  curl_socket_t socket_;
  std::size_t maxMessageBytes_;
  int wake_[2] = {-1, -1};
  std::string partial_;
  bool binary_ = true;
};

}  // namespace

CurlWsConnector::CurlWsConnector(Config config) : config_(std::move(config)) {}

ConnectResult CurlWsConnector::Connect(const ConnectRequest& request) {
  ConnectResult result;
  GlobalInit();

  // Trust anchors first: with none there is nothing to verify against, and a connection that skipped
  // verification is never acceptable. The loader logs the directory and count, or its own failure.
  std::string pem;
  {
    std::lock_guard<std::mutex> lock(bundleMutex_);
    if (bundle_.certificates == 0) {
      const SessionRouter::LogSink& log = config_.log;
      bundle_ = nevr::quest_auth::LoadCaBundle(
          config_.caDirs, [log](nevr::auth::LogLevel level, const std::string& line) {
            if (!log) return;
            switch (level) {
              case nevr::auth::LogLevel::Debug: log(LogLevel::Debug, line); break;
              case nevr::auth::LogLevel::Info: log(LogLevel::Info, line); break;
              case nevr::auth::LogLevel::Warning: log(LogLevel::Warning, line); break;
              case nevr::auth::LogLevel::Error: log(LogLevel::Error, line); break;
            }
          });
    }
    pem = bundle_.pem;
  }
  if (pem.empty()) {
    result.status = ConnectStatus::TlsError;
    result.nativeCode = kNoTrustAnchors;
    return result;
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    result.status = ConnectStatus::NetworkError;
    result.nativeCode = static_cast<int>(CURLE_FAILED_INIT);
    return result;
  }
  curl_slist* headers = nullptr;
  for (const Header& h : request.headers) {
    const std::string line = h.name + ": " + h.value;
    headers = curl_slist_append(headers, line.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L);  // WebSocket: libcurl does the upgrade, we drive frames
  if (headers != nullptr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // worker thread: no SIGALRM resolver timeouts
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, config_.connectTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");  // never route through a proxy named by the environment
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "wss");
  // Verification: always on. There is deliberately no code path that sets either of these to zero.
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
  curl_blob blob;
  blob.data = &pem[0];
  blob.len = pem.size();
  blob.flags = CURL_BLOB_COPY;
  curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);

  const CURLcode rc = curl_easy_perform(curl);
  long http = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
  curl_slist_free_all(headers);

  result.httpStatus = static_cast<int>(http);
  result.nativeCode = static_cast<int>(rc);
  result.status = ClassifyCurlCode(static_cast<int>(rc), http);
  if (result.status != ConnectStatus::Ok) {
    curl_easy_cleanup(curl);
    return result;
  }
  curl_socket_t sock = CURL_SOCKET_BAD;
  if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK || sock == CURL_SOCKET_BAD) {
    curl_easy_cleanup(curl);
    result.status = ConnectStatus::NetworkError;
    return result;
  }
  result.connection = std::make_unique<CurlWsConnection>(curl, sock, config_.maxMessageBytes);
  return result;
}

}  // namespace quest_net
