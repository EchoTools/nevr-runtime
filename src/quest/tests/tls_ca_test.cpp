// Host test of CurlHttpClient's trust handling against real TLS peers on loopback.
//
// Everything is generated here with OpenSSL: a test CA (never trusted by anything but the
// directories this test builds), leaf certificates, and a TLS server on 127.0.0.1. The CA
// directory is laid out the Android way: files named by the OLD (MD5-based) subject hash
// (`openssl x509 -subject_hash_old`), with trailing text after the PEM. That naming is what
// makes CURLOPT_CAPATH useless with an OpenSSL backend, and the first two tests pin both
// halves of that: the production client verifies against this directory, raw CAPATH cannot.
// Needs libcurl (OpenSSL backend), libssl and libcrypto on the host.

#include "quest/auth/curl_http.h"
#include "quest/tests/mini_test.h"

#include <curl/curl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

using namespace mini_test;
using namespace nevr::quest_auth;

template <typename T, void (*Free)(T*)>
struct Deleter {
  void operator()(T* p) const { Free(p); }
};
using PKey = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using Cert = std::unique_ptr<X509, Deleter<X509, X509_free>>;

struct Identity {
  PKey key;
  Cert cert;
};

void AddExt(X509* subject, X509* issuer, int nid, const char* value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, issuer, subject, nullptr, nullptr, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
  X509_add_ext(subject, ext, -1);
  X509_EXTENSION_free(ext);
}

// issuer == nullptr: self-signed CA. san == nullptr: no subjectAltName.
Identity MakeIdentity(const char* cn, const Identity* issuer, const char* san, long serial) {
  Identity id;
  id.key.reset(EVP_EC_gen("P-256"));
  id.cert.reset(X509_new());
  X509* x = id.cert.get();
  X509_set_version(x, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(x), serial);
  X509_gmtime_adj(X509_getm_notBefore(x), -3600);
  X509_gmtime_adj(X509_getm_notAfter(x), 3600L * 24 * 30);
  X509_set_pubkey(x, id.key.get());
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
  X509* issuer_cert = issuer != nullptr ? issuer->cert.get() : x;
  X509_set_issuer_name(x, X509_get_subject_name(issuer_cert));
  AddExt(x, issuer_cert, NID_basic_constraints, issuer == nullptr ? "critical,CA:TRUE" : "CA:FALSE");
  if (san != nullptr) AddExt(x, issuer_cert, NID_subject_alt_name, san);
  X509_sign(x, issuer != nullptr ? issuer->key.get() : id.key.get(), EVP_sha256());
  return id;
}

std::string PemOf(X509* cert) {
  BIO* bio = BIO_new(BIO_s_mem());
  PEM_write_bio_X509(bio, cert);
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio, &data);
  std::string out(data, static_cast<size_t>(len));
  BIO_free(bio);
  return out;
}

// An Android-style CA directory: `<old subject hash>.0`, PEM then a text dump.
std::string MakeAndroidCaDir(const std::string& name, X509* ca) {
  const std::string dir = "build/quest-shared-host/scratch/" + name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  char file[32];
  std::snprintf(file, sizeof(file), "%08lx.0", X509_NAME_hash_old(X509_get_subject_name(ca)));
  std::ofstream(dir + "/" + file) << PemOf(ca) << "Certificate:\n    Data:\n        Version: 3 (0x2)\n";
  return dir;
}

// A one-connection-at-a-time loopback server; TLS when `leaf` is given, plain HTTP otherwise.
class Server {
 public:
  Server(const Identity* leaf, size_t body_bytes) : body_bytes_(body_bytes) {
    if (leaf != nullptr) {
      ctx_.reset(SSL_CTX_new(TLS_server_method()));
      SSL_CTX_use_certificate(ctx_.get(), leaf->cert.get());
      SSL_CTX_use_PrivateKey(ctx_.get(), leaf->key.get());
    }
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    ::listen(listen_fd_, 8);
    thread_ = std::thread([this] { Loop(); });
  }
  ~Server() {
    stop_ = true;
    thread_.join();
    ::close(listen_fd_);
  }
  int Port() const { return port_; }
  int Accepted() const { return accepted_; }

 private:
  struct SslDeleter {
    void operator()(SSL_CTX* c) const { SSL_CTX_free(c); }
  };

  void Loop() {
    while (!stop_) {
      pollfd p{listen_fd_, POLLIN, 0};
      if (::poll(&p, 1, 50) <= 0) continue;
      const int fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) continue;
      ++accepted_;
      Serve(fd);
      ::close(fd);
    }
  }

  void Serve(int fd) {
    SSL* ssl = nullptr;
    if (ctx_) {
      ssl = SSL_new(ctx_.get());
      SSL_set_fd(ssl, fd);
      if (SSL_accept(ssl) != 1) {
        SSL_free(ssl);
        return;
      }
    }
    const auto read_some = [&](char* buf, size_t n) -> long {
      return ssl != nullptr ? SSL_read(ssl, buf, static_cast<int>(n)) : static_cast<long>(::read(fd, buf, n));
    };
    const auto write_all = [&](const std::string& data) {
      size_t off = 0;
      while (off < data.size()) {
        const long w = ssl != nullptr ? SSL_write(ssl, data.data() + off, static_cast<int>(data.size() - off))
                                      : static_cast<long>(::write(fd, data.data() + off, data.size() - off));
        if (w <= 0) return;
        off += static_cast<size_t>(w);
      }
    };
    std::string req;
    char buf[2048];
    size_t body_have = 0, body_want = 0;
    bool headers_done = false;
    for (;;) {
      if (!headers_done) {
        const size_t end = req.find("\r\n\r\n");
        if (end != std::string::npos) {
          headers_done = true;
          const size_t cl = req.find("Content-Length: ");
          body_want = cl == std::string::npos ? 0 : static_cast<size_t>(std::strtoul(req.c_str() + cl + 16, nullptr, 10));
          body_have = req.size() - (end + 4);
        }
      } else {
        body_have = req.size() - (req.find("\r\n\r\n") + 4);
      }
      if (headers_done && body_have >= body_want) break;
      const long n = read_some(buf, sizeof(buf));
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
    }
    const std::string body(body_bytes_, 'x');
    write_all("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
              "\r\nConnection: close\r\n\r\n" + body);
    if (ssl != nullptr) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
    }
  }

  std::unique_ptr<SSL_CTX, SslDeleter> ctx_;
  size_t body_bytes_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> accepted_{0};
  std::thread thread_;
};

struct Fixture {
  Identity ca = MakeIdentity("NEVR Test CA", nullptr, nullptr, 1);
  Identity other_ca = MakeIdentity("Some Other CA", nullptr, nullptr, 2);
  Identity leaf = MakeIdentity("localhost", &ca, "DNS:localhost,IP:127.0.0.1", 3);
  Identity wrong_name_leaf = MakeIdentity("elsewhere", &ca, "DNS:other.test", 4);
  std::string android_dir = MakeAndroidCaDir("android-cacerts", ca.cert.get());
  std::string other_dir = MakeAndroidCaDir("other-cacerts", other_ca.cert.get());
};
Fixture& Fix() {
  static Fixture f;
  return f;
}

std::string Url(const Server& s, const char* scheme = "https", const char* host = "localhost") {
  return std::string(scheme) + "://" + host + ":" + std::to_string(s.Port()) + "/v2/rpc/device/auth/poll?http_key=k&unwrap";
}

struct Logs {
  std::mutex m;
  std::string text;
  nevr::auth::LogSink Sink() {
    return [this](nevr::auth::LogLevel, const std::string& s) {
      std::lock_guard<std::mutex> l(m);
      text += s + "\n";
    };
  }
};

TEST(the_android_cacerts_layout_is_verified_by_the_in_memory_bundle) {
  Server server(&Fix().leaf, 2);
  Logs logs;
  CurlHttpClient client({Fix().android_dir}, logs.Sink());
  const nevr::auth::HttpResponse r = client.PostJson(Url(server), "{}");
  CHECK(r.transport_ok);
  CHECK_EQ(r.status, 200L);
  CHECK_EQ(r.body, std::string("xx"));
  CHECK(logs.text.find("CA store loaded") != std::string::npos);
}

TEST(capath_cannot_verify_that_same_directory) {
  // The premise behind the bundle: OpenSSL looks CApath files up by the new hash, the
  // directory is named by the old one, so CAPATH finds nothing and verification fails.
  Server server(&Fix().leaf, 2);
  CURL* curl = curl_easy_init();
  CHECK(curl != nullptr);
  const std::string url = Url(server);
  std::string sink;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
  curl_easy_setopt(curl, CURLOPT_CAPATH, Fix().android_dir.c_str());
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void*, size_t s, size_t n, void*) { return s * n; });
  const CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  std::fprintf(stderr, "  raw CAPATH result: %d (%s)\n", static_cast<int>(res), curl_easy_strerror(res));
  CHECK_EQ(static_cast<int>(res), static_cast<int>(CURLE_PEER_FAILED_VERIFICATION));
}

TEST(a_directory_holding_another_ca_fails_verification_not_open) {
  Server server(&Fix().leaf, 2);
  CurlHttpClient client({Fix().other_dir});
  const nevr::auth::HttpResponse r = client.PostJson(Url(server), "{}");
  CHECK(!r.transport_ok);
  CHECK_EQ(r.transport_code, static_cast<int>(CURLE_PEER_FAILED_VERIFICATION));
}

TEST(a_certificate_for_another_host_name_fails_verification) {
  Server server(&Fix().wrong_name_leaf, 2);
  CurlHttpClient client({Fix().android_dir});
  const nevr::auth::HttpResponse r = client.PostJson(Url(server), "{}");
  CHECK(!r.transport_ok);
  CHECK_EQ(r.transport_code, static_cast<int>(CURLE_PEER_FAILED_VERIFICATION));
}

TEST(no_loadable_certificate_fails_closed_without_touching_the_network) {
  const std::string empty = "build/quest-shared-host/scratch/empty-cacerts";
  std::filesystem::remove_all(empty);
  std::filesystem::create_directories(empty);
  Server server(&Fix().leaf, 2);
  Logs logs;
  CurlHttpClient client({empty, "build/quest-shared-host/scratch/does-not-exist"}, logs.Sink());
  const nevr::auth::HttpResponse r = client.PostJson(Url(server), "{}");
  CHECK(!r.transport_ok);
  CHECK_EQ(r.transport_code, CurlHttpClient::kNoTrustAnchors);
  CHECK_EQ(server.Accepted(), 0);
  CHECK(logs.text.find("will fail closed") != std::string::npos);
}

TEST(production_mode_refuses_plain_http) {
  Server server(nullptr, 2);
  CurlHttpClient client({Fix().android_dir});
  const nevr::auth::HttpResponse r = client.PostJson(Url(server, "http", "127.0.0.1"), "{}");
  CHECK(!r.transport_ok);
  CHECK_EQ(r.transport_code, static_cast<int>(CURLE_UNSUPPORTED_PROTOCOL));
  CHECK_EQ(server.Accepted(), 0);
}

TEST(a_response_over_the_cap_is_a_transport_failure) {
  Server server(nullptr, 5000);
  CurlHttpClient client({}, nullptr, 10, /*allow_plain_http=*/true, /*max_response_bytes=*/1000);
  const nevr::auth::HttpResponse r = client.PostJson(Url(server, "http", "127.0.0.1"), "{}");
  CHECK(!r.transport_ok);
  CHECK_EQ(r.transport_code, static_cast<int>(CURLE_WRITE_ERROR));
}

TEST(proxy_environment_variables_are_ignored) {
  ::setenv("http_proxy", "http://127.0.0.1:1", 1);
  ::setenv("HTTP_PROXY", "http://127.0.0.1:1", 1);
  ::setenv("all_proxy", "http://127.0.0.1:1", 1);
  Server server(nullptr, 2);
  CurlHttpClient client({}, nullptr, 10, /*allow_plain_http=*/true);
  const nevr::auth::HttpResponse r = client.PostJson(Url(server, "http", "127.0.0.1"), "{}");
  ::unsetenv("http_proxy");
  ::unsetenv("HTTP_PROXY");
  ::unsetenv("all_proxy");
  CHECK(r.transport_ok);
  CHECK_EQ(r.status, 200L);
}

TEST(interrupt_makes_requests_fail_at_once) {
  Server server(&Fix().leaf, 2);
  CurlHttpClient client({Fix().android_dir});
  client.Interrupt();
  const nevr::auth::HttpResponse r = client.PostJson(Url(server), "{}");
  CHECK(!r.transport_ok);
  CHECK_EQ(r.transport_code, CurlHttpClient::kInterrupted);
  CHECK_EQ(server.Accepted(), 0);
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);  // the server writes to peers that have already hung up
  return mini_test::RunAll(argc, argv);
}
