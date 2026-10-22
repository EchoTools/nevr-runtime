// Host test for the libcurl Transport (src/quest/data/curl_transport.cpp) and, through it, the whole fetch over real HTTP,
// against src/quest/tests/range_server.py (just test-quest-data-net).
//
//   curl_transport_test write-zip <path>
//   curl_transport_test <port> <request-log> <trust-pem> <tls-dir>
//
// <tls-dir> holds <name>.port and <name>.log for the HTTPS servers range_server.py serves (good, expired, wronghost,
// selfsigned, tls11); the test CA is <trust-pem>.

#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "quest/data/curl_transport.h"
#include "quest/data/fetch.h"
#include "quest/data/sha256.h"
#include "quest/tests/data_test_support.h"
#include "quest/tests/mini_test.h"

using namespace nevr_quest::data;
using namespace data_test;

namespace {

int g_port = 0;
std::string g_log_path;
std::string g_trust_pem;
std::string g_tls_dir;


std::string Url(const std::string& path, const char* scheme = "http") {
  return std::string(scheme) + "://127.0.0.1:" + std::to_string(g_port) + path;
}

std::string Blob() {
  std::string b;
  for (int i = 0; i < 100000; ++i) b.push_back(static_cast<char>((i * 7 + i / 251) % 256));
  return b;
}

std::string RequestLog() {
  std::ifstream in(g_log_path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::size_t Count(const std::string& hay, const std::string& needle) {
  std::size_t n = 0;
  for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
  return n;
}

std::string Slurp(const std::string& path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// https://127.0.0.1:<port of the named TLS server><path>
std::string TlsUrl(const std::string& server, const std::string& path) {
  return "https://127.0.0.1:" + std::to_string(std::atoi(Slurp(g_tls_dir + "/" + server + ".port").c_str())) + path;
}

std::size_t TlsRequests(const std::string& server, const std::string& path) {
  return Count(Slurp(g_tls_dir + "/" + server + ".log"), path);
}

CurlTransport TestCa() { return CurlTransport(g_trust_pem, /*allow_plain_http=*/false, 5, 5); }

struct Got {
  TransportResult result;
  std::string body;
  std::vector<long> statuses;
};

Got Get(CurlTransport& t, const std::string& url, std::uint64_t offset, KeepGoingFn keep = [] { return true; },
        bool refuse_after_first = false) {
  Got g;
  bool first = true;
  g.result = t.GetRange(
      url, offset,
      [&](long status, const void* data, std::size_t n) {
        g.statuses.push_back(status);
        if (status == 200 || status == 206) g.body.append(static_cast<const char*>(data), n);
        if (refuse_after_first) {
          const bool ok = first;
          first = false;
          return ok;
        }
        return true;
      },
      keep);
  return g;
}

CurlTransport Plain() { return CurlTransport("", /*allow_plain_http=*/true, 5, 5); }

std::string ZipForTheServer() {
  std::string noise;
  std::uint32_t x = 99;
  for (int i = 0; i < 200000; ++i) {
    x = x * 1664525u + 1013904223u;
    noise.push_back(static_cast<char>(x >> 24));
  }
  return BuildZip({{"_data/", "", 0},
                   {"_data/5932408047/", "", 0},
                   {"_data/5932408047/rad15/", "", 0},
                   {"_data/5932408047/rad15/android/", "", 0},
                   {"_data/5932408047/rad15/android/manifests/", "", 0},
                   {"_data/5932408047/rad15/android/manifests/48037dc70b0ecab2", "manifest", 8},
                   {"_data/5932408047/rad15/android/packages/", "", 0},
                   {"_data/5932408047/rad15/android/packages/48037dc70b0ecab2_0", noise, 8}});
}

}  // namespace

TEST(RangedGetSendsAnExplicitOffsetAndGetsTheTail) {
  CurlTransport t = Plain();
  const Got g = Get(t, Url("/blob"), 1000);
  CHECK(g.result.completed);
  CHECK_EQ(g.result.http_status, 206L);
  CHECK(g.body == Blob().substr(1000));
  CHECK(RequestLog().find("/blob range=bytes=1000-") != std::string::npos);
}

TEST(OffsetZeroStillSendsARange) {
  CurlTransport t = Plain();
  const Got g = Get(t, Url("/blob"), 0);
  CHECK_EQ(g.result.http_status, 206L);
  CHECK(g.body == Blob());
  CHECK(RequestLog().find("/blob range=bytes=0-") != std::string::npos);
}

TEST(AServerThatIgnoresRangeIsReportedAs200) {
  CurlTransport t = Plain();
  const Got g = Get(t, Url("/ignore"), 500);
  CHECK(g.result.completed);
  CHECK_EQ(g.result.http_status, 200L);
  CHECK(g.body == Blob());
}

TEST(ADroppedConnectionIsATransportFailure) {
  CurlTransport t = Plain();
  const Got g = Get(t, Url("/drop"), 0);
  CHECK(!g.result.completed);
  CHECK(!g.result.interrupted);
  CHECK(g.body.size() > 0 && g.body.size() < Blob().size());
  CHECK(g.body == Blob().substr(0, g.body.size()));
}

TEST(An404DeliversItsBodyWithTheStatus) {
  CurlTransport t = Plain();
  const Got g = Get(t, Url("/missing"), 0);
  CHECK(g.result.completed);
  CHECK_EQ(g.result.http_status, 404L);
  CHECK(!g.statuses.empty());
  for (const long s : g.statuses) CHECK_EQ(s, 404L);
  CHECK(g.body.empty());  // the fetch layer drops error pages; this helper keeps only 200/206 bodies
}

TEST(StopFromKeepGoingEndsTheTransfer) {
  CurlTransport t = Plain();
  const Got g = Get(t, Url("/blob"), 0, [] { return false; });
  CHECK(!g.result.completed);
  CHECK(g.result.interrupted);
}

TEST(ARefusedBodyEndsTheTransfer) {
  CurlTransport t = Plain();
  const Got g = Get(t, Url("/blob"), 0, [] { return true; }, /*refuse_after_first=*/true);
  CHECK(!g.result.completed);
  CHECK(g.body.size() < Blob().size());
}

TEST(HttpsWithNoTrustAnchorsFailsClosedBeforeAnyRequest) {
  const std::size_t before = Count(RequestLog(), "/blob");
  CurlTransport t("", false, 5, 5);
  const Got g = Get(t, Url("/blob", "https"), 0);
  CHECK(!g.result.completed);
  CHECK_EQ(g.result.code, CurlTransport::kNoTrustAnchors);
  CHECK_EQ(Count(RequestLog(), "/blob"), before);
}

TEST(PlainHttpIsRefusedWhenNotAllowedEvenWithTrustAnchors) {
  const std::size_t before = Count(RequestLog(), "/blob");
  CurlTransport t(g_trust_pem, false, 5, 5);
  const Got g = Get(t, Url("/blob", "http"), 0);
  CHECK(!g.result.completed);
  CHECK(g.result.code != 0);
  CHECK_EQ(Count(RequestLog(), "/blob"), before);
}

TEST(TheProductionRootTableMatchesItsPins) {
  const RootsCheck check = CheckDataHostRoots();
  CHECK(check.ok);
  CHECK_EQ(check.count, kDataHostRootCount);
  CHECK(kDataHostRootCount >= 3);
  std::vector<std::string> seen;
  for (std::size_t i = 0; i < kDataHostRootCount; ++i) {
    BIO* bio = BIO_new_mem_buf(kDataHostRoots[i].pem, static_cast<int>(std::strlen(kDataHostRoots[i].pem)));
    CHECK(bio != nullptr);
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    CHECK(cert != nullptr);
    CHECK(PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) == nullptr);  // exactly one certificate each
    CHECK(X509_cmp_current_time(X509_get0_notAfter(cert)) > 0);           // not expired
    CHECK(X509_NAME_cmp(X509_get_subject_name(cert), X509_get_issuer_name(cert)) == 0);  // self-signed: a root
    CHECK(std::strlen(kDataHostRoots[i].why) > 20);                        // each root says why it is there
    for (const std::string& n : seen) CHECK(n != kDataHostRoots[i].name);
    seen.push_back(kDataHostRoots[i].name);
    X509_free(cert);
    BIO_free(bio);
  }
  ERR_clear_error();
}

// The exact set, by name and the DER sha256 of each certificate, taken from the CAs' own pages (ISRG:
// letsencrypt.org/certs, Google: pki.goog/repo/certs, SSL.com: ssl.com/repository), not from this table. The
// self-check above only proves each pin matches its own PEM; this is what pins WHICH roots are there. Removing GTS
// Root R4 (the root the data host chains to today) or swapping one for another, or adding one, fails here.
TEST(TheProductionRootTableIsExactlyTheExpectedSet) {
  struct Expected {
    const char* name;
    const char* sha256;
  };
  const Expected want[] = {
      {"ISRG Root X1", "96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6"},
      {"ISRG Root X2", "69729b8e15a86efc177a57afb7171dfc64add28c2fca8cf1507e34453ccb1470"},
      {"ISRG Root YE", "e14ffcad5b0025731006caa43a121a22d8e9700f4fb9cf852f02a708aa5d5666"},
      {"ISRG Root YR", "e57b7e6f150c419102e8d5c055729ff967b9d1a829bf00cec89ca604ebf4a86f"},
      {"GTS Root R1", "d947432abde7b7fa90fc2e6b59101b1280e0e1c7e4e40fa3c6887fff57a7f4cf"},
      {"GTS Root R2", "8d25cd97229dbf70356bda4eb3cc734031e24cf00fafcfd32dc76eb5841c7ea8"},
      {"GTS Root R3", "34d8a73ee208d9bcdb0d956520934b4e40e69482596e8b6f73c8426b010a6f48"},
      {"GTS Root R4", "349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d"},
      {"SSL.com TLS ECC Root CA 2022", "c32ffd9f46f936d16c3673990959434b9ad60aafbb9e7cf33654f144cc1ba143"},
      {"SSL.com TLS RSA Root CA 2022", "8faf7d2e2cb4709bb8e0b33666bf75a5dd45b5de480f8ea8d4bfe6bebc17f2ed"},
  };
  const std::size_t n = sizeof(want) / sizeof(want[0]);
  CHECK_EQ(kDataHostRootCount, n);  // no extra, none missing
  for (std::size_t i = 0; i < kDataHostRootCount; ++i) {
    std::size_t found = 0;
    for (std::size_t j = 0; j < n; ++j) {
      if (std::string(kDataHostRoots[i].name) == want[j].name) {
        ++found;
        CHECK_EQ(std::string(kDataHostRoots[i].sha256), std::string(want[j].sha256));
      }
    }
    CHECK_EQ(found, std::size_t{1});  // named once, and expected
  }
  // The pin is the digest of the certificate in the blob, so the blob is the expected certificate (CheckDataHostRoots
  // proves pin == blob), and the same certificate is not in the table twice under two names.
  for (std::size_t i = 0; i < kDataHostRootCount; ++i) {
    for (std::size_t j = i + 1; j < kDataHostRootCount; ++j) CHECK(std::string(kDataHostRoots[i].sha256) != kDataHostRoots[j].sha256);
  }
}

TEST(GtsRootR4TheRootTheDataHostChainsToTodayIsInTheTable) {
  bool present = false;
  for (std::size_t i = 0; i < kDataHostRootCount; ++i) {
    present = present || std::string(kDataHostRoots[i].sha256) ==
                             "349dfa4058c5e263123b398ae795573c4e1313c83fe68f93556cd5e8031b3c7d";
  }
  CHECK(present);
  CHECK(DataHostRootsPem().find(kDataHostRoots[0].pem) != std::string::npos);  // and the blob handed to curl holds the table
}

TEST(AMangledRootTableFailsTheSelfCheck) {
  // One flipped base64 character in a certificate: the pin no longer matches (or the blob no longer parses).
  std::string pem = kDataHostRoots[0].pem;
  const std::size_t at = pem.find('\n') + 10;
  pem[at] = pem[at] == 'A' ? 'B' : 'A';
  DataHostRoot mangled[] = {{kDataHostRoots[0].name, kDataHostRoots[0].why, kDataHostRoots[0].sha256, pem.c_str()}};
  const RootsCheck bad = CheckRoots(mangled, 1);
  CHECK(!bad.ok);
  CHECK(!bad.detail.empty());
  DataHostRoot truncated[] = {{"x", "y", kDataHostRoots[0].sha256, "-----BEGIN CERTIFICATE-----\nAAAA\n"}};
  CHECK(!CheckRoots(truncated, 1).ok);
  CHECK(!CheckRoots(nullptr, 0).ok);  // an empty table is not a trust store
}

TEST(ABlobWithTheRightPinThatIsNotACertificateFailsTheSelfCheck) {
  // Pinned correctly, so only the parse can refuse it: DER that is not a SEQUENCE, and a SEQUENCE whose length is wrong.
  struct Case {
    const char* b64;
    const char bytes[4];
  };
  const Case cases[] = {{"MQIAAA==", {0x31, 0x02, 0x00, 0x00}}, {"MAUBAg==", {0x30, 0x05, 0x01, 0x02}}};
  for (const Case& c : cases) {
    const std::string pin = Sha256Hex(c.bytes, 4);
    const std::string pem = std::string("-----BEGIN CERTIFICATE-----\n") + c.b64 + "\n-----END CERTIFICATE-----\n";
    const DataHostRoot root[] = {{"fake", "a blob that is pinned but is not a certificate", pin.c_str(), pem.c_str()}};
    const RootsCheck r = CheckRoots(root, 1);
    CHECK(!r.ok);
  }
}

TEST(ARootTableThatFailsItsSelfCheckFailsClosedBeforeAnyRequest) {
  const std::size_t before = TlsRequests("good", "/blob");
  CurlTransport t(DataHostRootsPem(), false, 5, 5, [] { return RootsCheck{false, 0, "test_mangled"}; });
  const Got g = Get(t, TlsUrl("good", "/blob"), 0);
  CHECK(!g.result.completed);
  CHECK_EQ(g.result.code, CurlTransport::kRootsUnusable);
  CHECK_EQ(g.result.detail, std::string("roots_unusable:test_mangled"));
  CHECK_EQ(TlsRequests("good", "/blob"), before);
}

TEST(AValidChainOverHttpsIsFetched) {
  CurlTransport t = TestCa();
  const Got g = Get(t, TlsUrl("good", "/blob"), 1000);
  CHECK(g.result.completed);
  CHECK_EQ(g.result.http_status, 206L);
  CHECK(g.body == Blob().substr(1000));
  CHECK_EQ(TlsRequests("good", "/blob range=bytes=1000-"), std::size_t{1});
}

TEST(AnExpiredCertificateIsRefusedBeforeAnyRequest) {
  CurlTransport t = TestCa();
  const Got g = Get(t, TlsUrl("expired", "/blob"), 0);
  CHECK(!g.result.completed);
  CHECK(g.result.code != 0);
  CHECK(g.body.empty());
  CHECK_EQ(TlsRequests("expired", "/blob"), std::size_t{0});
}

TEST(ACertificateForAnotherHostIsRefused) {
  CurlTransport t = TestCa();
  const Got g = Get(t, TlsUrl("wronghost", "/blob"), 0);
  CHECK(!g.result.completed);
  CHECK(g.result.code != 0);
  CHECK(g.body.empty());
  CHECK_EQ(TlsRequests("wronghost", "/blob"), std::size_t{0});
}

TEST(ASelfSignedCertificateNotInTheTrustSetIsRefused) {
  CurlTransport t = TestCa();
  const Got g = Get(t, TlsUrl("selfsigned", "/blob"), 0);
  CHECK(!g.result.completed);
  CHECK(g.result.code != 0);
  CHECK(g.body.empty());
  CHECK_EQ(TlsRequests("selfsigned", "/blob"), std::size_t{0});
}

TEST(TheProductionRootsDoNotTrustTheTestServer) {
  CurlTransport t;  // the pinned roots, as the game builds it
  const Got g = Get(t, TlsUrl("good", "/blob"), 0);
  CHECK(!g.result.completed);
  CHECK(g.body.empty());
  CHECK(g.result.code != 0);
}

TEST(ARedirectIsNotFollowed) {
  const std::size_t blobs = TlsRequests("good", "/blob");
  CurlTransport t = TestCa();
  const Got g = Get(t, TlsUrl("good", "/redirect"), 0);
  CHECK_EQ(g.result.http_status, 302L);
  CHECK(g.body.empty());
  CHECK_EQ(TlsRequests("good", "/blob"), blobs);  // the Location target was never requested
  CHECK_EQ(TlsRequests("good", "/redirect"), std::size_t{1});
}

TEST(AProxyFromTheEnvironmentIsIgnored) {
  // A proxy that nothing listens on: a client that honoured it could not reach the server.
  const char* names[] = {"https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "http_proxy", "HTTP_PROXY"};
  for (const char* n : names) setenv(n, "http://127.0.0.1:1", 1);
  CurlTransport t = TestCa();
  const Got g = Get(t, TlsUrl("good", "/blob"), 0);
  for (const char* n : names) unsetenv(n);
  CHECK(g.result.completed);
  CHECK_EQ(g.result.http_status, 206L);
  CHECK(g.body == Blob());
}

TEST(AStalledBodyEndsAsATransportFailureNotAHang) {
  CurlTransport t(g_trust_pem, false, 5, /*stall_seconds=*/2);
  const auto start = std::chrono::steady_clock::now();
  const Got g = Get(t, TlsUrl("good", "/stall"), 0);
  const auto took = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
  CHECK(!g.result.completed);
  CHECK(!g.result.interrupted);
  CHECK_EQ(g.body.size(), std::size_t{100});  // what arrived before the stall is delivered, so the part can resume
  CHECK(took < 15);                           // the server sleeps 20 s
}

// The TLS 1.2 floor (CURLOPT_SSLVERSION in curl_transport.cpp) is not observable here: with this host's curl and
// OpenSSL the TLS 1.1 refusal below comes from their own defaults, and removing the line leaves this test passing.
// The line stays as the stated floor; this test checks the refusal, not the line.
TEST(ATlsOneOneServerIsRefused) {
  CurlTransport t = TestCa();
  const Got g = Get(t, TlsUrl("tls11", "/blob"), 0);
  CHECK(!g.result.completed);
  CHECK(g.body.empty());
  CHECK_EQ(TlsRequests("tls11", "/blob"), std::size_t{0});
}

TEST(AnUnreachablePortIsATransportFailure) {
  CurlTransport t = Plain();
  const Got g = Get(t, "http://127.0.0.1:1/blob", 0);
  CHECK(!g.result.completed);
  CHECK_EQ(g.result.http_status, 0L);
}

TEST(WholeFetchOverHttpResumesAfterADroppedConnection) {
  Scratch scratch;
  const std::string zip = ZipForTheServer();
  FetchConfig config;
  config.root = scratch.Path("root");
  config.url = Url("/zip-flaky");
  config.sha256 = Sha256Hex(zip.data(), zip.size());
  config.bytes = zip.size();
  config.extracted_bytes = 1000;
  config.max_attempts = 3;
  config.utc_stamp = "20261022T000000Z";
  config.available_bytes = [](const std::string&) { return std::uint64_t{1} << 40; };
  config.backoff = [](int) {};
  CurlTransport t = Plain();
  const FetchResult r = RunFetch(config, t);
  CHECK_EQ(r.status, FetchStatus::kOk);
  CHECK_EQ(r.attempts, 2);
  CHECK(Exists(config.root + "/_data/5932408047/rad15/android/manifests/48037dc70b0ecab2"));
  const std::string log = RequestLog();
  CHECK(log.find("/zip-flaky range=bytes=0-") != std::string::npos);
  CHECK(log.find("/zip-flaky range=bytes=5000-") != std::string::npos);  // resumed where the first one stopped
}

TEST(WholeFetchOverHttpRefusesATamperedPin) {
  Scratch scratch;
  const std::string zip = ZipForTheServer();
  FetchConfig config;
  config.root = scratch.Path("root");
  config.url = Url("/zip");
  std::string wrong = Sha256Hex(zip.data(), zip.size());
  wrong[0] = wrong[0] == '0' ? '1' : '0';
  config.sha256 = wrong;
  config.bytes = zip.size();
  config.extracted_bytes = 1000;
  config.max_attempts = 2;
  config.utc_stamp = "20261022T000000Z";
  config.available_bytes = [](const std::string&) { return std::uint64_t{1} << 40; };
  config.backoff = [](int) {};
  CurlTransport t = Plain();
  const FetchResult r = RunFetch(config, t);
  CHECK_EQ(r.status, FetchStatus::kRetriesExhausted);
  CHECK_EQ(r.detail, std::string("sha256_mismatch"));
  CHECK(!Exists(config.root + "/_data"));
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "write-zip") {
    const std::string zip = ZipForTheServer();
    std::ofstream out(argv[2], std::ios::binary);
    out.write(zip.data(), static_cast<std::streamsize>(zip.size()));
    return out ? 0 : 1;
  }
  if (argc != 5) {
    std::fprintf(stderr, "usage: curl_transport_test write-zip <path> | <port> <request-log> <trust-pem> <tls-dir>\n");
    return 2;
  }
  g_port = std::atoi(argv[1]);
  g_log_path = argv[2];
  g_tls_dir = argv[4];
  {
    std::ifstream pem(argv[3]);
    std::stringstream ss;
    ss << pem.rdbuf();
    g_trust_pem = ss.str();
  }
  return mini_test::RunAll(0, nullptr);
}
