// Host test for the libcurl Transport (src/quest/data/curl_transport.cpp) and, through it, the whole fetch over real HTTP,
// against src/quest/tests/range_server.py (just test-quest-data-net).
//
//   curl_transport_test write-zip <path>
//   curl_transport_test <port> <request-log> <trust-dir>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
std::string g_trust_dir;

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

CurlTransport Plain() { return CurlTransport({}, nullptr, /*allow_plain_http=*/true, 5, 5); }

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
  CurlTransport t({}, nullptr, false, 5, 5);
  const Got g = Get(t, Url("/blob", "https"), 0);
  CHECK(!g.result.completed);
  CHECK_EQ(g.result.code, CurlTransport::kNoTrustAnchors);
  CHECK_EQ(Count(RequestLog(), "/blob"), before);
}

TEST(PlainHttpIsRefusedWhenNotAllowedEvenWithTrustAnchors) {
  const std::size_t before = Count(RequestLog(), "/blob");
  CurlTransport t({g_trust_dir}, nullptr, false, 5, 5);
  const Got g = Get(t, Url("/blob", "http"), 0);
  CHECK(!g.result.completed);
  CHECK(g.result.code != 0);
  CHECK_EQ(Count(RequestLog(), "/blob"), before);
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
  if (argc != 4) {
    std::fprintf(stderr, "usage: curl_transport_test write-zip <path> | <port> <request-log> <trust-dir>\n");
    return 2;
  }
  g_port = std::atoi(argv[1]);
  g_log_path = argv[2];
  g_trust_dir = argv[3];
  return mini_test::RunAll(0, nullptr);
}
