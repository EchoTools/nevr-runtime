#include "winhttp_stub.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstring>

#include "common/logging.h"

static const IID IID_IWinHttpRequest = {0xA1C9FEEE, 0x0617, 0x4F23, {0x9D, 0x58, 0x89, 0x61, 0xEA, 0x43, 0x56, 0x7C}};

static void DebugLog(const char* message) {
  FILE* f = fopen("C:\\winhttp_stub_debug.log", "a");
  if (f) {
    fprintf(f, "[WINHTTP_STUB] %s\n", message);
    fflush(f);
    fclose(f);
  }
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  size_t realsize = size * nmemb;
  std::vector<char>* vec = static_cast<std::vector<char>*>(userp);
  vec->insert(vec->end(), static_cast<char*>(contents), static_cast<char*>(contents) + realsize);
  return realsize;
}

static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
  size_t realsize = size * nitems;
  std::map<std::wstring, std::wstring>* headers = static_cast<std::map<std::wstring, std::wstring>*>(userdata);

  std::string header_line(buffer, realsize);
  size_t colon_pos = header_line.find(':');
  if (colon_pos != std::string::npos) {
    std::string key = header_line.substr(0, colon_pos);
    std::string value = header_line.substr(colon_pos + 1);

    while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
      value = value.substr(1);
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
      value.pop_back();
    }

    int key_len = MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, nullptr, 0);
    int value_len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);

    if (key_len > 0 && value_len > 0) {
      std::wstring wkey(key_len, L'\0');
      std::wstring wvalue(value_len, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, &wkey[0], key_len);
      MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &wvalue[0], value_len);
      wkey.resize(wcslen(wkey.c_str()));
      wvalue.resize(wcslen(wvalue.c_str()));
      (*headers)[wkey] = wvalue;
    }
  }

  return realsize;
}

WinHttpRequestStub::WinHttpRequestStub() : m_refCount(1), m_statusCode(0), m_sent(false) {
  char buf[128];
  snprintf(buf, sizeof(buf), "WinHttpRequestStub::WinHttpRequestStub() - Constructor called, this=%p", this);
  DebugLog(buf);
}

WinHttpRequestStub::~WinHttpRequestStub() {
  char buf[128];
  snprintf(buf, sizeof(buf), "WinHttpRequestStub::~WinHttpRequestStub() - ENTRY: this=%p, refCount=%lu", this,
           m_refCount);
  DebugLog(buf);
  fflush(stdout);

  if (m_refCount > 0) {
    char warn[256];
    snprintf(warn, sizeof(warn),
             "WARNING: Destructor called with refCount=%lu > 0! Possible double-delete or use-after-free", m_refCount);
    DebugLog(warn);
    fflush(stdout);
  }

  DebugLog("WinHttpRequestStub::~WinHttpRequestStub() - DISABLED FOR DEBUGGING, NOT CLEANING UP");
  fflush(stdout);

  return;
}

HRESULT WinHttpRequestStub::QueryInterface(REFIID riid, void** ppvObject) {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "QueryInterface called for IID {%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}, this=%p", riid.Data1,
           riid.Data2, riid.Data3, riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3], riid.Data4[4],
           riid.Data4[5], riid.Data4[6], riid.Data4[7], this);
  DebugLog(buf);

  if (ppvObject == nullptr) {
    return E_POINTER;
  }

  if (IsEqualIID(riid, IID_IUnknown)) {
    DebugLog("  -> Returning IUnknown interface");
    *ppvObject = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }

  if (IsEqualIID(riid, IID_IWinHttpRequest)) {
    DebugLog("  -> Returning IWinHttpRequest interface (stub)");
    *ppvObject = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }

  DebugLog("  -> Interface not supported, returning E_NOINTERFACE");
  *ppvObject = nullptr;
  return E_NOINTERFACE;
}

ULONG WinHttpRequestStub::AddRef() {
  ULONG newCount = ++m_refCount;
  char buf[128];
  snprintf(buf, sizeof(buf), "AddRef() -> refCount = %lu, this=%p", newCount, this);
  DebugLog(buf);
  return newCount;
}

ULONG WinHttpRequestStub::Release() {
  char buf1[128];
  snprintf(buf1, sizeof(buf1), "Release() called, current refCount = %lu, this=%p", m_refCount, this);
  DebugLog(buf1);

  ULONG newCount = --m_refCount;
  char buf2[128];
  snprintf(buf2, sizeof(buf2), "Release() -> new refCount = %lu, this=%p", newCount, this);
  DebugLog(buf2);

  if (newCount == 0) {
    DebugLog("Release() -> refCount reached 0, WOULD delete but leaking for debugging");
    // TEMP: Don't actually delete to debug the issue
    // delete this;
    return 0;
  }
  return newCount;
}

HRESULT WinHttpRequestStub::SetProxy(LONG ProxySetting, VARIANT ProxyServer, VARIANT BypassList) {
  DebugLog("SetProxy() called");
  return S_OK;
}

HRESULT WinHttpRequestStub::SetCredentials(BSTR UserName, BSTR Password, LONG Flags) {
  DebugLog("SetCredentials() called");
  return S_OK;
}

HRESULT WinHttpRequestStub::Open(BSTR Method, BSTR Url, VARIANT Async) {
  char buf[512];
  snprintf(buf, sizeof(buf), "Open() called - Method: %ls, Url: %ls", Method ? Method : L"(null)",
           Url ? Url : L"(null)");
  DebugLog(buf);

  if (Method) m_method = Method;
  if (Url) m_url = Url;
  m_sent = false;
  m_responseBody.clear();
  m_responseHeaders.clear();
  m_statusCode = 0;

  return S_OK;
}

HRESULT WinHttpRequestStub::SetRequestHeader(BSTR Header, BSTR Value) {
  char buf[512];
  snprintf(buf, sizeof(buf), "SetRequestHeader() called - Header: %ls, Value: %ls", Header ? Header : L"(null)",
           Value ? Value : L"(null)");
  DebugLog(buf);

  if (Header && Value) {
    m_requestHeaders[Header] = Value;
  }

  return S_OK;
}

HRESULT WinHttpRequestStub::GetResponseHeader(BSTR Header, BSTR* Value) {
  char buf[256];
  snprintf(buf, sizeof(buf), "GetResponseHeader() called for header: %ls", Header ? Header : L"(null)");
  DebugLog(buf);

  if (!Value) return E_POINTER;
  *Value = nullptr;

  if (!m_sent || !Header) return S_OK;

  auto it = m_responseHeaders.find(Header);
  if (it != m_responseHeaders.end()) {
    *Value = SysAllocString(it->second.c_str());
    snprintf(buf, sizeof(buf), "  -> Found header value: %ls", it->second.c_str());
    DebugLog(buf);
  } else {
    DebugLog("  -> Header not found in response");
  }

  return S_OK;
}

HRESULT WinHttpRequestStub::GetAllResponseHeaders(BSTR* Headers) {
  DebugLog("GetAllResponseHeaders() called");
  if (!Headers) return E_POINTER;
  *Headers = nullptr;

  if (!m_sent) return S_OK;

  std::wstring all_headers;
  for (const auto& pair : m_responseHeaders) {
    all_headers += pair.first + L": " + pair.second + L"\r\n";
  }

  if (!all_headers.empty()) {
    *Headers = SysAllocString(all_headers.c_str());
  }

  return S_OK;
}

HRESULT WinHttpRequestStub::Send(VARIANT Body) {
  DebugLog("Send() called - Performing HTTP request with libcurl");

  CURL* curl = curl_easy_init();
  if (!curl) {
    DebugLog("  -> ERROR: Failed to initialize curl");
    return E_FAIL;
  }

  int url_len = WideCharToMultiByte(CP_UTF8, 0, m_url.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (url_len <= 0) {
    DebugLog("  -> ERROR: Failed to convert URL");
    curl_easy_cleanup(curl);
    return E_FAIL;
  }

  std::vector<char> url_utf8(url_len);
  WideCharToMultiByte(CP_UTF8, 0, m_url.c_str(), -1, url_utf8.data(), url_len, nullptr, nullptr);

  char buf[512];
  snprintf(buf, sizeof(buf), "  -> URL: %s", url_utf8.data());
  DebugLog(buf);

  curl_easy_setopt(curl, CURLOPT_URL, url_utf8.data());

  m_method_utf8.clear();
  m_url_utf8.clear();
  m_header_lines_utf8.clear();

  int method_len = WideCharToMultiByte(CP_UTF8, 0, m_method.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (method_len > 0) {
    m_method_utf8.resize(method_len);
    WideCharToMultiByte(CP_UTF8, 0, m_method.c_str(), -1, m_method_utf8.data(), method_len, nullptr, nullptr);
    m_method_utf8.pop_back();

    if (_stricmp(m_method_utf8.c_str(), "POST") == 0) {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (_stricmp(m_method_utf8.c_str(), "GET") == 0) {
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, m_method_utf8.c_str());
    }

    snprintf(buf, sizeof(buf), "  -> Method: %s", m_method_utf8.c_str());
    DebugLog(buf);
  }

  struct curl_slist* headers_list = nullptr;
  for (const auto& pair : m_requestHeaders) {
    int header_len = WideCharToMultiByte(CP_UTF8, 0, pair.first.c_str(), -1, nullptr, 0, nullptr, nullptr);
    int value_len = WideCharToMultiByte(CP_UTF8, 0, pair.second.c_str(), -1, nullptr, 0, nullptr, nullptr);

    if (header_len > 0 && value_len > 0) {
      std::string header_utf8(header_len, '\0');
      std::string value_utf8(value_len, '\0');
      WideCharToMultiByte(CP_UTF8, 0, pair.first.c_str(), -1, header_utf8.data(), header_len, nullptr, nullptr);
      WideCharToMultiByte(CP_UTF8, 0, pair.second.c_str(), -1, value_utf8.data(), value_len, nullptr, nullptr);

      header_utf8.pop_back();
      value_utf8.pop_back();

      std::string header_line = header_utf8 + ": " + value_utf8;
      m_header_lines_utf8.push_back(header_line);
      headers_list = curl_slist_append(headers_list, m_header_lines_utf8.back().c_str());
    }
  }

  if (headers_list) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);
  }

  m_responseBody.clear();
  m_responseHeaders.clear();

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m_responseBody);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &m_responseHeaders);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

  CURLcode res = curl_easy_perform(curl);

  if (res != CURLE_OK) {
    snprintf(buf, sizeof(buf), "  -> curl_easy_perform() failed: %s", curl_easy_strerror(res));
    DebugLog(buf);
    curl_slist_free_all(headers_list);
    curl_easy_cleanup(curl);
    return E_FAIL;
  }

  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  m_statusCode = static_cast<long>(http_code);

  snprintf(buf, sizeof(buf), "  -> HTTP Status: %ld, Response size: %zu bytes", m_statusCode, m_responseBody.size());
  DebugLog(buf);

  curl_slist_free_all(headers_list);
  curl_easy_cleanup(curl);

  m_sent = true;
  DebugLog("  -> Send() completed successfully");
  return S_OK;
}

HRESULT WinHttpRequestStub::get_Status(LONG* Status) {
  char buf[128];
  snprintf(buf, sizeof(buf), "get_Status() called - returning %ld", m_statusCode);
  DebugLog(buf);

  if (Status) *Status = m_statusCode;
  return S_OK;
}

HRESULT WinHttpRequestStub::get_StatusText(BSTR* Status) {
  DebugLog("get_StatusText() called");
  if (Status) *Status = nullptr;
  return S_OK;
}

HRESULT WinHttpRequestStub::get_ResponseText(BSTR* Body) {
  DebugLog("get_ResponseText() called");
  if (!Body) return E_POINTER;
  *Body = nullptr;

  if (!m_sent || m_responseBody.empty()) return S_OK;

  int wide_len =
      MultiByteToWideChar(CP_UTF8, 0, m_responseBody.data(), static_cast<int>(m_responseBody.size()), nullptr, 0);
  if (wide_len > 0) {
    *Body = SysAllocStringLen(nullptr, wide_len);
    if (*Body) {
      MultiByteToWideChar(CP_UTF8, 0, m_responseBody.data(), static_cast<int>(m_responseBody.size()), *Body, wide_len);
    }
  }

  return S_OK;
}

HRESULT WinHttpRequestStub::get_ResponseBody(VARIANT* Body) {
  char buf[128];
  snprintf(buf, sizeof(buf), "get_ResponseBody() called - %zu bytes available", m_responseBody.size());
  DebugLog(buf);

  if (!Body) return E_POINTER;
  VariantInit(Body);

  if (!m_sent || m_responseBody.empty()) return S_OK;

  SAFEARRAY* psa = SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(m_responseBody.size()));
  if (!psa) {
    DebugLog("  -> ERROR: Failed to create SAFEARRAY");
    return E_OUTOFMEMORY;
  }

  void* pData = nullptr;
  HRESULT hr = SafeArrayAccessData(psa, &pData);
  if (SUCCEEDED(hr)) {
    memcpy(pData, m_responseBody.data(), m_responseBody.size());
    SafeArrayUnaccessData(psa);

    Body->vt = VT_ARRAY | VT_UI1;
    Body->parray = psa;

    DebugLog("  -> Successfully created SAFEARRAY variant");
  } else {
    SafeArrayDestroy(psa);
    DebugLog("  -> ERROR: Failed to access SAFEARRAY data");
    return hr;
  }

  return S_OK;
}

HRESULT WinHttpRequestStub::get_ResponseStream(VARIANT* Body) {
  DebugLog("get_ResponseStream() called");
  if (Body) VariantInit(Body);
  return S_OK;
}

HRESULT WinHttpRequestStub::get_Option(LONG Option, VARIANT* Value) {
  DebugLog("get_Option() called");
  if (Value) VariantInit(Value);
  return S_OK;
}

HRESULT WinHttpRequestStub::put_Option(LONG Option, VARIANT Value) {
  DebugLog("put_Option() called");
  return S_OK;
}

HRESULT WinHttpRequestStub::WaitForResponse(VARIANT Timeout, VARIANT_BOOL* Succeeded) {
  DebugLog("WaitForResponse() called");
  if (Succeeded) *Succeeded = VARIANT_TRUE;
  return S_OK;
}

HRESULT WinHttpRequestStub::Abort() {
  DebugLog("Abort() called");
  return S_OK;
}

HRESULT WinHttpRequestStub::SetTimeouts(LONG ResolveTimeout, LONG ConnectTimeout, LONG SendTimeout,
                                        LONG ReceiveTimeout) {
  char buf[256];
  snprintf(buf, sizeof(buf), "SetTimeouts() called - Resolve: %ld, Connect: %ld, Send: %ld, Receive: %ld",
           ResolveTimeout, ConnectTimeout, SendTimeout, ReceiveTimeout);
  DebugLog(buf);
  return S_OK;
}

HRESULT WinHttpRequestStub::SetClientCertificate(BSTR ClientCertificate) {
  DebugLog("SetClientCertificate() called");
  return S_OK;
}

HRESULT WinHttpRequestStub::SetAutoLogonPolicy(LONG AutoLogonPolicy) {
  DebugLog("SetAutoLogonPolicy() called");
  return S_OK;
}

HRESULT WINAPI CreateWinHttpRequestStub(REFIID riid, void** ppvObject) {
  DebugLog("CreateWinHttpRequestStub called");

  if (ppvObject == nullptr) {
    DebugLog("  -> ERROR: ppvObject is NULL");
    return E_POINTER;
  }

  WinHttpRequestStub* pStub = new WinHttpRequestStub();
  if (pStub == nullptr) {
    DebugLog("  -> ERROR: Failed to allocate WinHttpRequestStub");
    return E_OUTOFMEMORY;
  }

  char buf1[128];
  snprintf(buf1, sizeof(buf1), "  -> Created object at %p, refCount=%lu", pStub, pStub->GetRefCount());
  DebugLog(buf1);

  HRESULT hr = pStub->QueryInterface(riid, ppvObject);

  char buf2[128];
  snprintf(buf2, sizeof(buf2), "  -> After QueryInterface: refCount=%lu, *ppvObject=%p", pStub->GetRefCount(),
           *ppvObject);
  DebugLog(buf2);

  // DON'T release the temporary reference - let it leak
  // pStub->Release();

  char buf3[128];
  snprintf(buf3, sizeof(buf3), "  -> Skipped Release to prevent premature destruction, refCount=%lu",
           pStub->GetRefCount());
  DebugLog(buf3);

  char buf4[128];
  snprintf(buf4, sizeof(buf4), "  -> About to return HRESULT 0x%08lX, pStub=%p still exists", hr, pStub);
  DebugLog(buf4);

  DebugLog("  -> RETURNING from CreateWinHttpRequestStub");
  return hr;
}
