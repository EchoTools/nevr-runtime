#include "runtime/host/crash_send_winhttp.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdlib>
#include <string>

namespace nevr_host {
namespace {

// winhttp.dll's functions, resolved by name from System32 (the host does not import the DLL).
struct WinHttpApi {
  HINTERNET(WINAPI* open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) = nullptr;
  HINTERNET(WINAPI* connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD) = nullptr;
  HINTERNET(WINAPI* open_request)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD) = nullptr;
  BOOL(WINAPI* set_timeouts)(HINTERNET, int, int, int, int) = nullptr;
  BOOL(WINAPI* set_option)(HINTERNET, DWORD, LPVOID, DWORD) = nullptr;
  BOOL(WINAPI* send_request)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR) = nullptr;
  BOOL(WINAPI* receive_response)(HINTERNET, LPVOID) = nullptr;
  BOOL(WINAPI* query_headers)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD) = nullptr;
  BOOL(WINAPI* close_handle)(HINTERNET) = nullptr;
};

template <typename Fn>
bool Resolve(HMODULE module, const char* name, Fn* out) {
  const FARPROC address = GetProcAddress(module, name);
  if (address == nullptr) return false;
  *out = reinterpret_cast<Fn>(reinterpret_cast<void*>(address));
  return true;
}

// Loaded once on the sender's thread. Null when winhttp.dll or one of its functions is missing.
const WinHttpApi* Api(DWORD* error) {
  static WinHttpApi api;
  static bool ready = false;
  static DWORD failure = 0;
  static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
  InitOnceExecuteOnce(
      &once,
      [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        HMODULE module = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
          failure = GetLastError();
          return TRUE;
        }
        ready = Resolve(module, "WinHttpOpen", &api.open) && Resolve(module, "WinHttpConnect", &api.connect) &&
                Resolve(module, "WinHttpOpenRequest", &api.open_request) &&
                Resolve(module, "WinHttpSetTimeouts", &api.set_timeouts) &&
                Resolve(module, "WinHttpSetOption", &api.set_option) &&
                Resolve(module, "WinHttpSendRequest", &api.send_request) &&
                Resolve(module, "WinHttpReceiveResponse", &api.receive_response) &&
                Resolve(module, "WinHttpQueryHeaders", &api.query_headers) &&
                Resolve(module, "WinHttpCloseHandle", &api.close_handle);
        if (!ready) failure = ERROR_PROC_NOT_FOUND;
        return TRUE;
      },
      nullptr, nullptr);
  if (!ready) {
    if (error != nullptr) *error = failure;
    return nullptr;
  }
  return &api;
}

struct Handle {
  const WinHttpApi* api;
  HINTERNET value;
  Handle(const WinHttpApi* a, HINTERNET v) : api(a), value(v) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() {
    if (value != nullptr) api->close_handle(value);
  }
};

struct ParsedUrl {
  bool secure = true;
  std::wstring host;
  INTERNET_PORT port = 443;
  std::wstring path;
};

std::wstring Widen(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], length);
  return out;
}

// scheme://host[:port]/path?query. No user info, no fragment. False when the text is not that.
bool ParseUrl(const std::string& url, bool allowPlainHttp, ParsedUrl* out) {
  std::size_t at = 0;
  if (url.rfind("https://", 0) == 0) {
    out->secure = true;
    out->port = 443;
    at = 8;
  } else if (allowPlainHttp && url.rfind("http://", 0) == 0) {
    out->secure = false;
    out->port = 80;
    at = 7;
  } else {
    return false;
  }
  const std::size_t slash = url.find('/', at);
  const std::string authority = url.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
  const std::string path = slash == std::string::npos ? std::string("/") : url.substr(slash);
  if (authority.empty() || authority.find('@') != std::string::npos) return false;
  std::string host = authority;
  const std::size_t colon = authority.rfind(':');
  if (colon != std::string::npos) {
    host = authority.substr(0, colon);
    const std::string digits = authority.substr(colon + 1);
    if (digits.empty() || digits.size() > 5) return false;
    unsigned long port = 0;
    for (char c : digits) {
      if (c < '0' || c > '9') return false;
      port = port * 10 + static_cast<unsigned long>(c - '0');
    }
    if (port == 0 || port > 65535) return false;
    out->port = static_cast<INTERNET_PORT>(port);
  }
  if (host.empty()) return false;
  out->host = Widen(host);
  out->path = Widen(path);
  return !out->host.empty() && !out->path.empty();
}

}  // namespace

nevr_crash_send::PostResult WinHttpPost(void* context, const std::string& url, const std::string& body,
                                        const nevr_crash_send::Timeouts& timeouts) {
  nevr_crash_send::PostResult result;
  const auto* options = static_cast<const WinHttpPostContext*>(context);
  const bool allowPlainHttp = options != nullptr && options->allowPlainHttp;

  ParsedUrl parsed;
  if (!ParseUrl(url, allowPlainHttp, &parsed)) {
    result.errorCode = ERROR_INVALID_PARAMETER;
    return result;
  }
  DWORD loadError = 0;
  const WinHttpApi* api = Api(&loadError);
  if (api == nullptr) {
    result.errorCode = static_cast<long>(loadError);
    return result;
  }

  Handle session(api, api->open(L"nevr-host", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0));
  if (session.value == nullptr) {
    result.errorCode = static_cast<long>(GetLastError());
    return result;
  }
  api->set_timeouts(session.value, static_cast<int>(timeouts.connectMs), static_cast<int>(timeouts.connectMs),
                    static_cast<int>(timeouts.totalMs), static_cast<int>(timeouts.totalMs));
  DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;  // the floor; the server picks 1.2 (a failure leaves the default)
  api->set_option(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

  Handle connection(api, api->connect(session.value, parsed.host.c_str(), parsed.port, 0));
  if (connection.value == nullptr) {
    result.errorCode = static_cast<long>(GetLastError());
    return result;
  }
  Handle request(api, api->open_request(connection.value, L"POST", parsed.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, parsed.secure ? WINHTTP_FLAG_SECURE : 0));
  if (request.value == nullptr) {
    result.errorCode = static_cast<long>(GetLastError());
    return result;
  }
  DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  api->set_option(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));

  static const wchar_t kHeaders[] = L"Content-Type: application/json\r\n";
  const DWORD length = static_cast<DWORD>(body.size());
  if (!api->send_request(request.value, kHeaders, static_cast<DWORD>(-1), const_cast<char*>(body.data()), length, length, 0) ||
      !api->receive_response(request.value, nullptr)) {
    result.errorCode = static_cast<long>(GetLastError());
    return result;
  }
  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!api->query_headers(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
    result.errorCode = static_cast<long>(GetLastError());
    return result;
  }
  result.transportOk = true;
  result.status = static_cast<long>(status);
  return result;
}

}  // namespace nevr_host
