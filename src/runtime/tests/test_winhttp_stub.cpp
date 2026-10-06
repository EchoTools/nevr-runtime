// ============================================================================
// winhttp_stub status-text tests (GH #27)
//
// Drives the REAL libcurl-backed IWinHttpRequest stub (compat/winhttp_stub.cpp)
// through its COM vtable and through IDispatch::Invoke against a one-shot
// HTTP/1.1 listener on 127.0.0.1. StatusText must be the reason phrase the
// server actually sent on the status line, not a hardcoded "OK".
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <oleauto.h>

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "core/logging.h"
#include "runtime/compat/winhttp_stub.h"

// --- logging.h: winhttp_stub.cpp only calls Log(); record nothing, link only. ---
void Log(EchoVR::LogLevel, const char*, ...) {}

namespace {

// Vtable slots for the Xbox One IWinHttpRequest layout (winhttp_stub.h).
constexpr size_t kSlotRelease = 2;
constexpr size_t kSlotInvoke = 6;
constexpr size_t kSlotOpen = 8;
constexpr size_t kSlotSend = 12;
constexpr size_t kSlotGetStatus = 13;
constexpr size_t kSlotGetStatusText = 14;

constexpr DISPID kDispIdSend = 5;
constexpr DISPID kDispIdStatus = 8;
constexpr DISPID kDispIdStatusText = 9;

using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(void*);
using InvokeFn = HRESULT(STDMETHODCALLTYPE*)(void*, DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*,
                                             UINT*);
using OpenFn = HRESULT(STDMETHODCALLTYPE*)(void*, BSTR, BSTR, VARIANT);
using SendFn = HRESULT(STDMETHODCALLTYPE*)(void*, VARIANT);
using GetStatusFn = HRESULT(STDMETHODCALLTYPE*)(void*, long*);
using GetStatusTextFn = HRESULT(STDMETHODCALLTYPE*)(void*, BSTR*);

template <typename Fn>
Fn Slot(void* obj, size_t index) {
  void** vtbl = *static_cast<void***>(obj);
  return reinterpret_cast<Fn>(vtbl[index]);
}

// Serves exactly one canned response to the first connection, then closes.
class OneShotHttpServer {
 public:
  explicit OneShotHttpServer(std::string response) : m_response(std::move(response)) {
    m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    m_ok = m_listener != INVALID_SOCKET &&
           bind(m_listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
           listen(m_listener, 1) == 0;
    int len = sizeof(addr);
    m_ok = m_ok && getsockname(m_listener, reinterpret_cast<sockaddr*>(&addr), &len) == 0;
    m_port = ntohs(addr.sin_port);
    if (m_ok) m_thread = std::thread([this] { Serve(); });
  }

  ~OneShotHttpServer() {
    if (m_thread.joinable()) m_thread.join();
    if (m_listener != INVALID_SOCKET) closesocket(m_listener);
  }

  bool ok() const { return m_ok; }

  std::wstring Url() const { return L"http://127.0.0.1:" + std::to_wstring(m_port) + L"/status"; }

 private:
  void Serve() {
    SOCKET client = accept(m_listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) return;
    std::string request;
    char buf[1024];
    while (request.find("\r\n\r\n") == std::string::npos) {
      int n = recv(client, buf, sizeof(buf), 0);
      if (n <= 0) break;
      request.append(buf, static_cast<size_t>(n));
    }
    send(client, m_response.data(), static_cast<int>(m_response.size()), 0);
    shutdown(client, SD_SEND);
    closesocket(client);
  }

  std::string m_response;
  SOCKET m_listener = INVALID_SOCKET;
  unsigned short m_port = 0;
  bool m_ok = false;
  std::thread m_thread;
};

std::string Response(const std::string& statusLine) {
  return statusLine + "\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi";
}

class WinHttpStubStatusText : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    WSADATA wsa;
    ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &wsa), 0);
    // Hermeticity: a developer's http_proxy must not intercept the loopback request.
    SetEnvironmentVariableA("NO_PROXY", "127.0.0.1");
    _putenv_s("NO_PROXY", "127.0.0.1");
  }
  static void TearDownTestSuite() { WSACleanup(); }

  void SetUp() override { ASSERT_EQ(CreateWinHttpRequestStub(IID_IUnknown, &m_obj), S_OK); }
  void TearDown() override {
    if (m_obj) Slot<ReleaseFn>(m_obj, kSlotRelease)(m_obj);
  }

  void Open(const std::wstring& url) {
    BSTR method = SysAllocString(L"GET");
    BSTR bstrUrl = SysAllocString(url.c_str());
    VARIANT async;
    VariantInit(&async);
    EXPECT_EQ(Slot<OpenFn>(m_obj, kSlotOpen)(m_obj, method, bstrUrl, async), S_OK);
    SysFreeString(method);
    SysFreeString(bstrUrl);
  }

  HRESULT Send() {
    VARIANT body;
    VariantInit(&body);
    return Slot<SendFn>(m_obj, kSlotSend)(m_obj, body);
  }

  long Status() {
    long status = -1;
    EXPECT_EQ(Slot<GetStatusFn>(m_obj, kSlotGetStatus)(m_obj, &status), S_OK);
    return status;
  }

  std::wstring StatusText() {
    BSTR text = nullptr;
    EXPECT_EQ(Slot<GetStatusTextFn>(m_obj, kSlotGetStatusText)(m_obj, &text), S_OK);
    std::wstring out = text ? std::wstring(text, SysStringLen(text)) : std::wstring();
    SysFreeString(text);
    return out;
  }

  VARIANT Invoke(DISPID id, WORD flags) {
    VARIANT result;
    VariantInit(&result);
    DISPPARAMS params{};
    EXPECT_EQ(Slot<InvokeFn>(m_obj, kSlotInvoke)(m_obj, id, IID_NULL, 0, flags, &params, &result, nullptr, nullptr),
              S_OK);
    return result;
  }

  std::wstring InvokeStatusText() {
    VARIANT v = Invoke(kDispIdStatusText, DISPATCH_PROPERTYGET);
    EXPECT_EQ(v.vt, VT_BSTR);
    std::wstring out = (v.vt == VT_BSTR && v.bstrVal) ? std::wstring(v.bstrVal, SysStringLen(v.bstrVal)) : L"";
    VariantClear(&v);
    return out;
  }

  void Fetch(const std::string& statusLine) {
    OneShotHttpServer server(Response(statusLine));
    ASSERT_TRUE(server.ok());
    Open(server.Url());
    ASSERT_EQ(Send(), S_OK);
  }

  void* m_obj = nullptr;
};

}  // namespace

TEST_F(WinHttpStubStatusText, NotFoundReportsServerReasonPhrase) {
  Fetch("HTTP/1.1 404 Not Found");
  EXPECT_EQ(Status(), 404);
  EXPECT_EQ(StatusText(), L"Not Found");
}

TEST_F(WinHttpStubStatusText, OkReportsOk) {
  Fetch("HTTP/1.1 200 OK");
  EXPECT_EQ(Status(), 200);
  EXPECT_EQ(StatusText(), L"OK");
}

TEST_F(WinHttpStubStatusText, ServerErrorReportsMultiWordReasonVerbatim) {
  Fetch("HTTP/1.1 503 Service Temporarily Down");
  EXPECT_EQ(Status(), 503);
  EXPECT_EQ(StatusText(), L"Service Temporarily Down");
}

TEST_F(WinHttpStubStatusText, EmptyReasonPhraseIsEmptyNotOk) {
  // RFC 9112 §4: status-line = HTTP-version SP status-code SP [ reason-phrase ]
  Fetch("HTTP/1.1 500 ");
  EXPECT_EQ(Status(), 500);
  EXPECT_EQ(StatusText(), L"");
}

TEST_F(WinHttpStubStatusText, InvokePropertyGetMatchesVtableGetter) {
  Fetch("HTTP/1.1 404 Not Found");
  EXPECT_EQ(InvokeStatusText(), L"Not Found");
}

TEST_F(WinHttpStubStatusText, BeforeSendIsEmpty) {
  Open(L"http://127.0.0.1:1/unused");
  EXPECT_EQ(Status(), 0);
  EXPECT_EQ(StatusText(), L"");
}

TEST_F(WinHttpStubStatusText, OpenClearsPreviousResponseText) {
  Fetch("HTTP/1.1 404 Not Found");
  ASSERT_EQ(StatusText(), L"Not Found");
  Open(L"http://127.0.0.1:1/unused");
  EXPECT_EQ(StatusText(), L"");
}

TEST_F(WinHttpStubStatusText, InvokeNoOpSendSynthesizes200Ok) {
  // The IDispatch Send path is a deliberate no-op that synthesizes a 200;
  // its status text must agree with that synthesized status.
  Open(L"http://127.0.0.1:1/unused");
  VARIANT sent = Invoke(kDispIdSend, DISPATCH_METHOD);
  VariantClear(&sent);
  VARIANT status = Invoke(kDispIdStatus, DISPATCH_PROPERTYGET);
  EXPECT_EQ(status.vt, VT_I4);
  EXPECT_EQ(status.lVal, 200);
  EXPECT_EQ(InvokeStatusText(), L"OK");
  EXPECT_EQ(StatusText(), L"OK");
}
