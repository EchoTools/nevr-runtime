// Host test of CurlHttpClient's trust handling against real TLS peers on loopback.
//
// Everything is generated here with OpenSSL: a test CA (never trusted by anything but the
// directories this test builds), leaf certificates, and a TLS server on 127.0.0.1. The CA
// directory is laid out the Android way: files named by the OLD (MD5-based) subject hash
// (`openssl x509 -subject_hash_old`), with trailing text after the PEM. That naming is what
// makes CURLOPT_CAPATH useless with an OpenSSL backend, and the first two tests pin both
// halves of that: the production client verifies against this directory, raw CAPATH cannot.
// Needs libcurl (OpenSSL backend), libssl and libcrypto on the host.

#include "quest/auth/ca_bundle.h"
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
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
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

std::string DerOf(X509* cert) {
  unsigned char* der = nullptr;
  const int len = i2d_X509(cert, &der);
  std::string out(reinterpret_cast<const char*>(der), static_cast<size_t>(len));
  OPENSSL_free(der);
  return out;
}

void WriteFile(const std::string& path, const std::string& data) { std::ofstream(path, std::ios::binary) << data; }

std::string FreshDir(const std::string& name) {
  const std::string dir = "build/quest-shared-host/scratch/" + name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

size_t Count(const std::string& text, const std::string& needle) {
  size_t n = 0;
  for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1)) ++n;
  return n;
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
  // stall: accept connections and then say nothing until the server is destroyed.
  Server(const Identity* leaf, size_t body_bytes, bool stall = false) : body_bytes_(body_bytes), stall_(stall) {
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
    if (stall_) {
      while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      return;
    }
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
  bool stall_;
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


// ---------------------------------------------------------------- CA store loading
TEST(ca_bundle_keeps_only_certificates_that_parse_and_counts_the_rest) {
  const std::string dir = FreshDir("ca-mixed");
  WriteFile(dir + "/aaaa1111.0", PemOf(Fix().ca.cert.get()) + "Certificate:\n  text dump\n");
  WriteFile(dir + "/bbbb2222.0", DerOf(Fix().other_ca.cert.get()));
  WriteFile(dir + "/cccc3333.0", "0 is a digit, not a DER sequence tag, and this is not a certificate");
  WriteFile(dir + "/dddd4444.0", "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n");
  WriteFile(dir + "/empty.0", "");
  WriteFile(dir + "/eeee5555.0", std::string("\x30\x82\x01", 3));  // a DER header and nothing else
  Logs logs;
  const CaBundle b = LoadCaBundle({dir}, logs.Sink());
  CHECK_EQ(b.certificates, size_t(2));
  CHECK_EQ(Count(b.pem, "-----BEGIN CERTIFICATE-----"), size_t(2));
  CHECK(logs.text.find("certificates=2 unparsable_certs=3 skipped_files=1") != std::string::npos);
  CHECK(logs.text.find("AAAA") == std::string::npos);  // counts only, never content
}

TEST(ca_bundle_falls_through_a_directory_with_nothing_parsable_and_fails_loudly_with_none) {
  const std::string bad = FreshDir("ca-garbage");
  WriteFile(bad + "/x.0", "0000 garbage");
  {
    Logs logs;
    const CaBundle b = LoadCaBundle({bad, Fix().android_dir}, logs.Sink());
    CHECK_EQ(b.certificates, size_t(1));
    CHECK(logs.text.find("yielded no certificate") != std::string::npos);
  }
  {
    Logs logs;
    CHECK_EQ(LoadCaBundle({bad, "build/quest-shared-host/scratch/nope"}, logs.Sink()).certificates, size_t(0));
    CHECK(logs.text.find("fail closed") != std::string::npos);
  }
}

TEST(ca_bundle_stops_at_the_total_size_bound) {
  const std::string dir = FreshDir("ca-big");
  std::string block;
  while (block.size() + PemOf(Fix().ca.cert.get()).size() < kMaxCaFileBytes) block += PemOf(Fix().ca.cert.get());
  const size_t files = kMaxCaBundleBytes / block.size() + 3;
  for (size_t i = 0; i < files; ++i) WriteFile(dir + "/f" + std::to_string(1000 + i) + ".0", block);
  Logs logs;
  const CaBundle b = LoadCaBundle({dir}, logs.Sink());
  CHECK(b.certificates > 0);
  CHECK(b.pem.size() <= kMaxCaBundleBytes);
  CHECK(logs.text.find("-byte bound") != std::string::npos);
}

TEST(one_corrupt_file_in_the_directory_does_not_break_the_handshake) {
  const std::string dir = FreshDir("ca-poisoned");
  WriteFile(dir + "/0001.0", PemOf(Fix().ca.cert.get()));
  WriteFile(dir + "/0002.0", "0 definitely not a certificate");
  WriteFile(dir + "/0003.0", "-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n");
  Server server(&Fix().leaf, 2);
  CurlHttpClient client({dir});
  const nevr::auth::HttpResponse r = client.PostJson(Url(server), "{}");
  CHECK(r.transport_ok);
  CHECK_EQ(r.status, 200L);
}

TEST(a_corrupt_pem_block_does_not_hide_the_blocks_around_it) {
  const std::string good = PemOf(Fix().ca.cert.get());
  const std::string bad = "-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n";
  const std::string dir = FreshDir("ca-blocks");
  WriteFile(dir + "/good-bad-good.0", good + bad + PemOf(Fix().other_ca.cert.get()));
  WriteFile(dir + "/bad-then-good.0", bad + PemOf(Fix().leaf.cert.get()));
  WriteFile(dir + "/unterminated.0", good.substr(0, good.size() / 2));
  Logs logs;
  const CaBundle b = LoadCaBundle({dir}, logs.Sink());
  CHECK_EQ(b.certificates, size_t(3));
  CHECK(logs.text.find("certificates=3 unparsable_certs=3") != std::string::npos);
}

TEST(entries_that_are_not_regular_files_are_skipped_counted_and_never_block) {
  const std::string dir = FreshDir("ca-odd");
  WriteFile(dir + "/real.0", PemOf(Fix().ca.cert.get()));
  CHECK(::mkfifo((dir + "/fifo.0").c_str(), 0600) == 0);
  CHECK(::symlink((dir + "/fifo.0").c_str(), (dir + "/link-to-fifo.0").c_str()) == 0);
  CHECK(::symlink("/nonexistent/target", (dir + "/dangling.0").c_str()) == 0);
  CHECK(::symlink("loop-b.0", (dir + "/loop-a.0").c_str()) == 0);
  CHECK(::symlink("loop-a.0", (dir + "/loop-b.0").c_str()) == 0);
  std::filesystem::create_directory(dir + "/subdir.0");
  // A link to a regular file is followed.
  CHECK(::symlink("real.0", (dir + "/link-to-real.0").c_str()) == 0);
  Logs logs;
  const CaBundle b = LoadCaBundle({dir}, logs.Sink());
  CHECK_EQ(b.certificates, size_t(2));  // real.0 and link-to-real.0
  CHECK(logs.text.find("skipped_files=6") != std::string::npos);
}

TEST(a_huge_file_is_rejected_from_its_size_without_being_read) {
  const std::string dir = FreshDir("ca-huge");
  WriteFile(dir + "/real.0", PemOf(Fix().ca.cert.get()));
  {
    // 512 MiB sparse file: reading it would cost 512 MiB of memory.
    const int fd = ::open((dir + "/huge.0").c_str(), O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0);
    CHECK(::ftruncate(fd, 512L * 1024 * 1024) == 0);
    ::close(fd);
  }
  Logs logs;
  const CaBundle b = LoadCaBundle({dir}, logs.Sink());
  CHECK_EQ(b.certificates, size_t(1));
  CHECK(logs.text.find("skipped_files=1") != std::string::npos);
}

TEST(interrupt_during_a_stalled_request_returns_promptly) {
  Server server(nullptr, 2, /*stall=*/true);
  CurlHttpClient client({}, nullptr, /*timeout_seconds=*/20, /*allow_plain_http=*/true);
  nevr::auth::HttpResponse r;
  std::chrono::steady_clock::time_point done_at;
  std::thread requester([&] {
    r = client.PostJson(Url(server, "http", "127.0.0.1"), "{}");
    done_at = std::chrono::steady_clock::now();
  });
  CHECK(WaitUntil([&] { return server.Accepted() >= 1; }));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // the request is now waiting on the peer
  const auto interrupted_at = std::chrono::steady_clock::now();
  client.Interrupt();
  requester.join();
  CHECK(!r.transport_ok);
  CHECK_EQ(r.transport_code, CurlHttpClient::kInterrupted);
  CHECK(done_at - interrupted_at < std::chrono::seconds(5));  // the stalled peer would hold it for 20 s
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);  // the server writes to peers that have already hung up
  return mini_test::RunAll(argc, argv);
}
