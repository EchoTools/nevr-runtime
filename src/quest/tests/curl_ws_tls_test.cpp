// Host test for the libcurl WebSocket connector (src/quest/net/curl_ws_connector.{h,cpp}) against a real
// TLS server it does and does not trust. Run by `just test-quest-tls`, which creates the certificates and
// starts src/quest/tests/tls_ws_server.py. Usage:
//   curl_ws_tls_test <trust-dir> <other-trust-dir> <good_tls_port> <selfsigned_tls_port> <plain_port> <plain_stats_file>
// where <trust-dir> is a directory holding the CA certificate that signed the good server and
// <other-trust-dir> one holding an unrelated CA. They go through the same CA loader and
// CURLOPT_CAINFO_BLOB path Android uses (quest/auth/ca_bundle.h), not CAPATH.
//
// What it proves: a certificate that chains to the trusted CA for the right address connects and carries a
// frame; a wrong CA, a wrong host name, a self-signed leaf, an empty trust store and a non-TLS server each
// fail; a ws:// URL is refused without touching the network; and after every failure the plaintext server
// has seen no connection that looks like a downgrade (no "GET " upgrade request, no connection at all for
// the ws:// case).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "quest/net/curl_ws_connector.h"
#include "quest/tests/test_check.h"

using namespace quest_net;

namespace {

struct PlainStats {
  int connections = 0;
  int gets = 0;
};

PlainStats ReadStats(const std::string& path) {
  PlainStats s;
  std::ifstream in(path);
  in >> s.connections >> s.gets;
  return s;
}

ConnectRequest Request(const std::string& url) {
  ConnectRequest r;
  r.url = url;
  r.headers.push_back({"Authorization", "Bearer not-a-real-token"});
  return r;
}

CurlWsConnector::Config TrustOnly(const std::string& caDir) {
  CurlWsConnector::Config c;
  c.caDirs = {caDir};
  c.connectTimeoutSeconds = 10;
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 7) {
    std::fprintf(stderr, "usage: %s <trust-dir> <other-trust-dir> <good_tls_port> <selfsigned_tls_port> <plain_port> <plain_stats>\n", argv[0]);
    return 2;
  }
  // Proxy variables must not matter: libcurl would otherwise route the connection (and its Authorization
  // header) through them. Point every spelling at a port nothing listens on; a connect that honoured one
  // would fail with a network error instead of succeeding in case 1 below.
  for (const char* name : {"all_proxy", "ALL_PROXY", "https_proxy", "HTTPS_PROXY", "wss_proxy", "WSS_PROXY",
                           "http_proxy", "HTTP_PROXY"}) {
    setenv(name, "http://127.0.0.1:9", 1);
  }
  unsetenv("no_proxy");
  unsetenv("NO_PROXY");

  const std::string ca = argv[1], otherCa = argv[2], good = argv[3], selfsigned = argv[4], plain = argv[5], stats = argv[6];

  // 1. Right CA, right address: connects, carries a frame both ways, closes.
  {
    CurlWsConnector connector(TrustOnly(ca));
    ConnectResult r = connector.Connect(Request("wss://127.0.0.1:" + good + "/echo"));
    QCHECK(r.status == ConnectStatus::Ok);
    if (r.status == ConnectStatus::Ok && r.connection) {
      QCHECK(r.connection->Send("hello over verified tls", true));
      RecvResult got;
      for (int i = 0; i < 20 && got.status != RecvStatus::Frame; ++i) got = r.connection->Recv(200);
      QCHECK(got.status == RecvStatus::Frame);
      QCHECK(got.data == "hello over verified tls");
      QCHECK(got.binary);
      const std::string big(70000, 'b');  // spans several WebSocket frames' worth of TCP segments
      QCHECK(r.connection->Send(big, true));
      RecvResult bigGot;
      for (int i = 0; i < 50 && bigGot.status != RecvStatus::Frame; ++i) bigGot = r.connection->Recv(200);
      QCHECK(bigGot.status == RecvStatus::Frame && bigGot.data == big);
      r.connection->SendClose(1000);
    } else {
      std::fprintf(stderr, "connect failed: status=%s native=%d http=%d\n", ConnectStatusName(r.status), r.nativeCode, r.httpStatus);
    }
  }

  const PlainStats before = ReadStats(stats);

  // 2. Right CA, wrong address: the certificate names 127.0.0.1 (and an .invalid host) only.
  {
    CurlWsConnector connector(TrustOnly(ca));
    ConnectResult r = connector.Connect(Request("wss://localhost:" + good + "/echo"));
    QCHECK(r.status == ConnectStatus::TlsVerificationFailed);
    QCHECK(!r.connection);
  }
  // 3. Right address, a CA that did not sign the server certificate.
  {
    CurlWsConnector connector(TrustOnly(otherCa));
    ConnectResult r = connector.Connect(Request("wss://127.0.0.1:" + good + "/echo"));
    QCHECK(r.status == ConnectStatus::TlsVerificationFailed);
    QCHECK(!r.connection);
  }
  // 4. No trust anchors at all: nothing verifies.
  {
    CurlWsConnector::Config c;
    c.caDirs = {"/nonexistent-ca-dir"};
    c.connectTimeoutSeconds = 10;
    CurlWsConnector connector(c);
    ConnectResult r = connector.Connect(Request("wss://127.0.0.1:" + good + "/echo"));
    QCHECK(r.status == ConnectStatus::TlsError);  // fails closed before any connection is attempted
    QCHECK(r.nativeCode == CurlWsConnector::kNoTrustAnchors);
    QCHECK(!r.connection);
  }
  // 5. A self-signed leaf that merely claims the right address.
  {
    CurlWsConnector connector(TrustOnly(ca));
    ConnectResult r = connector.Connect(Request("wss://127.0.0.1:" + selfsigned + "/echo"));
    QCHECK(r.status == ConnectStatus::TlsVerificationFailed);
  }
  // 6. wss:// to a server that does not speak TLS: fails, and the bytes it receives are a TLS hello, not
  //    an HTTP upgrade.
  {
    CurlWsConnector connector(TrustOnly(ca));
    ConnectResult r = connector.Connect(Request("wss://127.0.0.1:" + plain + "/echo"));
    QCHECK(r.status != ConnectStatus::Ok);
    QCHECK(!r.connection);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the plain server record the read
  const PlainStats afterTls = ReadStats(stats);
  QCHECK(afterTls.connections == before.connections + 1);  // case 6 only
  QCHECK(afterTls.gets == before.gets);                    // none of the failures retried in plaintext

  // 7. ws:// is refused by the connector's own protocol allow-list without any network traffic.
  {
    CurlWsConnector connector(TrustOnly(ca));
    ConnectResult r = connector.Connect(Request("ws://127.0.0.1:" + plain + "/echo"));
    QCHECK(r.status == ConnectStatus::PolicyRefused);
    QCHECK(!r.connection);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const PlainStats afterPlain = ReadStats(stats);
  QCHECK(afterPlain.connections == afterTls.connections);
  QCHECK(afterPlain.gets == afterTls.gets);

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "curl_ws_tls_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("curl_ws_tls_test: all checks passed\n");
  return 0;
}
