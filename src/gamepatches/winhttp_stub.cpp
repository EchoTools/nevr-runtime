#include "winhttp_stub.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstring>
#include <new>

#include "common/logging.h"

// IWinHttpRequest IID — {A1C9FEEE-0617-4F23-9D58-8961EA43567C}
static const IID IID_IWinHttpRequest = {0xA1C9FEEE, 0x0617, 0x4F23, {0x9D, 0x58, 0x89, 0x61, 0xEA, 0x43, 0x56, 0x7C}};

// ============================================================================
// Helpers
// ============================================================================

static std::string WideToUtf8(const wchar_t* ws) {
  if (!ws) return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string s(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], len, nullptr, nullptr);
  return s;
}

static size_t CurlWriteCb(void* p, size_t sz, size_t n, void* ud) {
  size_t total = sz * n;
  auto* v = static_cast<std::vector<char>*>(ud);
  v->insert(v->end(), (char*)p, (char*)p + total);
  return total;
}

static size_t CurlHeaderCb(char* buf, size_t sz, size_t n, void* ud) {
  size_t total = sz * n;
  auto* hdrs = static_cast<std::map<std::wstring, std::wstring>*>(ud);
  std::string line(buf, total);
  size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    while (!val.empty() && (val[0] == ' ' || val[0] == '\t')) val.erase(0, 1);
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.pop_back();
    int kl = MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, nullptr, 0);
    int vl = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, nullptr, 0);
    if (kl > 0 && vl > 0) {
      std::wstring wk(kl - 1, L'\0');
      std::wstring wv(vl - 1, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, key.c_str(), -1, &wk[0], kl);
      MultiByteToWideChar(CP_UTF8, 0, val.c_str(), -1, &wv[0], vl);
      (*hdrs)[wk] = wv;
    }
  }
  return total;
}

// Cast the COM `this` pointer (vtable pointer) back to our object.
// COM passes the interface pointer as `this`, which points to `vtbl`.
// Since `vtbl` is the first member of WinHttpRequestStub, we can cast directly.
#define SELF(thisPtr) reinterpret_cast<WinHttpRequestStub*>(thisPtr)

// ============================================================================
// IUnknown — vtable slots [0..2]
// ============================================================================

static HRESULT STDMETHODCALLTYPE Stub_QueryInterface(void* pThis, REFIID riid, void** ppv) {
  if (!ppv) return E_POINTER;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDispatch) || IsEqualIID(riid, IID_IWinHttpRequest)) {
    *ppv = pThis;
    SELF(pThis)->m_refCount++;
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Stub_AddRef(void* pThis) { return ++SELF(pThis)->m_refCount; }

static ULONG STDMETHODCALLTYPE Stub_Release(void* pThis) {
  auto* self = SELF(pThis);
  ULONG n = --self->m_refCount;
  if (n == 0) {
    self->~WinHttpRequestStub();
    free(self);
  }
  return n;
}

// ============================================================================
// IDispatch — vtable slots [3..6] (stubs — game uses vtable, not Invoke)
// ============================================================================

static HRESULT STDMETHODCALLTYPE Stub_GetTypeInfoCount(void*, UINT* p) {
  if (p) *p = 0;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE Stub_GetTypeInfo(void*, UINT, LCID, ITypeInfo** p) {
  if (p) *p = nullptr;
  return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE Stub_GetIDsOfNames(void*, REFIID, LPOLESTR*, UINT, LCID, DISPID*) {
  return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE Stub_Invoke(void*, DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*,
                                              UINT*) {
  return E_NOTIMPL;
}

// ============================================================================
// IWinHttpRequest — vtable slots [7..25]
// ============================================================================

static HRESULT STDMETHODCALLTYPE Stub_SetProxy(void*, long, VARIANT, VARIANT) { return S_OK; }

static HRESULT STDMETHODCALLTYPE Stub_SetCredentials(void*, BSTR, BSTR, long) { return S_OK; }

static HRESULT STDMETHODCALLTYPE Stub_Open(void* pThis, BSTR Method, BSTR Url, VARIANT) {
  auto* self = SELF(pThis);
  if (Method) self->m_method = Method;
  if (Url) self->m_url = Url;
  self->m_sent = false;
  self->m_responseBody.clear();
  self->m_responseHeaders.clear();
  self->m_statusCode = 0;
  Log(EchoVR::LogLevel::Debug, "[NEVR.HTTP] Open %ls %ls", Method ? Method : L"(null)", Url ? Url : L"(null)");
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_SetRequestHeader(void* pThis, BSTR Header, BSTR Value) {
  if (Header && Value) SELF(pThis)->m_requestHeaders[Header] = Value;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_GetResponseHeader(void* pThis, BSTR Header, BSTR* Value) {
  if (!Value) return E_POINTER;
  *Value = nullptr;
  auto* self = SELF(pThis);
  if (!self->m_sent || !Header) return S_OK;
  auto it = self->m_responseHeaders.find(Header);
  if (it != self->m_responseHeaders.end()) *Value = SysAllocString(it->second.c_str());
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_GetAllResponseHeaders(void* pThis, BSTR* Headers) {
  if (!Headers) return E_POINTER;
  *Headers = nullptr;
  auto* self = SELF(pThis);
  if (!self->m_sent) return S_OK;
  std::wstring all;
  for (const auto& [k, v] : self->m_responseHeaders) all += k + L": " + v + L"\r\n";
  if (!all.empty()) *Headers = SysAllocString(all.c_str());
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_Send(void* pThis, VARIANT) {
  auto* self = SELF(pThis);
  CURL* curl = curl_easy_init();
  if (!curl) return E_FAIL;

  std::string url = WideToUtf8(self->m_url.c_str());
  std::string method = WideToUtf8(self->m_method.c_str());

  Log(EchoVR::LogLevel::Debug, "[NEVR.HTTP] Send %s %s", method.c_str(), url.c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  if (_stricmp(method.c_str(), "POST") == 0)
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  else if (_stricmp(method.c_str(), "GET") != 0)
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());

  struct curl_slist* hlist = nullptr;
  std::vector<std::string> hstrs;
  for (const auto& [k, v] : self->m_requestHeaders) {
    hstrs.push_back(WideToUtf8(k.c_str()) + ": " + WideToUtf8(v.c_str()));
    hlist = curl_slist_append(hlist, hstrs.back().c_str());
  }
  if (hlist) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

  self->m_responseBody.clear();
  self->m_responseHeaders.clear();
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &self->m_responseBody);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderCb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &self->m_responseHeaders);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(hlist);

  if (res != CURLE_OK) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.HTTP] curl failed: %s", curl_easy_strerror(res));
    curl_easy_cleanup(curl);
    return E_FAIL;
  }

  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  self->m_statusCode = code;
  curl_easy_cleanup(curl);
  self->m_sent = true;

  Log(EchoVR::LogLevel::Debug, "[NEVR.HTTP] Response: %ld (%zu bytes)", self->m_statusCode, self->m_responseBody.size());
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_Status(void* pThis, long* Status) {
  if (Status) *Status = SELF(pThis)->m_statusCode;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_StatusText(void*, BSTR* S) {
  if (S) *S = nullptr;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_ResponseText(void* pThis, BSTR* Body) {
  if (!Body) return E_POINTER;
  *Body = nullptr;
  auto* self = SELF(pThis);
  if (!self->m_sent || self->m_responseBody.empty()) return S_OK;
  int wl = MultiByteToWideChar(CP_UTF8, 0, self->m_responseBody.data(), (int)self->m_responseBody.size(), nullptr, 0);
  if (wl > 0) {
    *Body = SysAllocStringLen(nullptr, wl);
    if (*Body) MultiByteToWideChar(CP_UTF8, 0, self->m_responseBody.data(), (int)self->m_responseBody.size(), *Body, wl);
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_ResponseBody(void* pThis, VARIANT* Body) {
  if (!Body) return E_POINTER;
  VariantInit(Body);
  auto* self = SELF(pThis);
  if (!self->m_sent || self->m_responseBody.empty()) return S_OK;
  SAFEARRAY* psa = SafeArrayCreateVector(VT_UI1, 0, (ULONG)self->m_responseBody.size());
  if (!psa) return E_OUTOFMEMORY;
  void* pData = nullptr;
  if (SUCCEEDED(SafeArrayAccessData(psa, &pData))) {
    memcpy(pData, self->m_responseBody.data(), self->m_responseBody.size());
    SafeArrayUnaccessData(psa);
    Body->vt = VT_ARRAY | VT_UI1;
    Body->parray = psa;
  } else {
    SafeArrayDestroy(psa);
    return E_FAIL;
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE Stub_get_ResponseStream(void*, VARIANT* V) {
  if (V) VariantInit(V);
  return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE Stub_get_Option(void*, long, VARIANT* V) {
  if (V) VariantInit(V);
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE Stub_put_Option(void*, long, VARIANT) { return S_OK; }
static HRESULT STDMETHODCALLTYPE Stub_WaitForResponse(void*, VARIANT, VARIANT_BOOL* S) {
  if (S) *S = VARIANT_TRUE;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE Stub_Abort(void*) { return S_OK; }
static HRESULT STDMETHODCALLTYPE Stub_SetTimeouts(void*, long, long, long, long) { return S_OK; }
static HRESULT STDMETHODCALLTYPE Stub_SetClientCertificate(void*, BSTR) { return S_OK; }
static HRESULT STDMETHODCALLTYPE Stub_SetAutoLogonPolicy(void*, long) { return S_OK; }

// ============================================================================
// Static vtable — 26 slots, COM ABI exact layout
// ============================================================================

static void* s_vtbl[26] = {
    (void*)Stub_QueryInterface,       // [0]  IUnknown
    (void*)Stub_AddRef,               // [1]
    (void*)Stub_Release,              // [2]
    (void*)Stub_GetTypeInfoCount,     // [3]  IDispatch
    (void*)Stub_GetTypeInfo,          // [4]
    (void*)Stub_GetIDsOfNames,        // [5]
    (void*)Stub_Invoke,               // [6]
    (void*)Stub_SetProxy,             // [7]  IWinHttpRequest
    (void*)Stub_SetCredentials,       // [8]
    (void*)Stub_Open,                 // [9]
    (void*)Stub_SetRequestHeader,     // [10]
    (void*)Stub_GetResponseHeader,    // [11]
    (void*)Stub_GetAllResponseHeaders,// [12]
    (void*)Stub_Send,                 // [13]
    (void*)Stub_get_Status,           // [14]
    (void*)Stub_get_StatusText,       // [15]
    (void*)Stub_get_ResponseText,     // [16]
    (void*)Stub_get_ResponseBody,     // [17]
    (void*)Stub_get_ResponseStream,   // [18]
    (void*)Stub_get_Option,           // [19]
    (void*)Stub_put_Option,           // [20]
    (void*)Stub_WaitForResponse,      // [21]
    (void*)Stub_Abort,                // [22]
    (void*)Stub_SetTimeouts,          // [23]
    (void*)Stub_SetClientCertificate, // [24]
    (void*)Stub_SetAutoLogonPolicy,   // [25]
};

// ============================================================================
// Construction / Factory
// ============================================================================

WinHttpRequestStub::WinHttpRequestStub() : vtbl(s_vtbl), m_refCount(1), m_statusCode(0), m_sent(false) {}

HRESULT WINAPI CreateWinHttpRequestStub(REFIID riid, void** ppvObject) {
  if (!ppvObject) return E_POINTER;

  // Use malloc + placement new so Release() can free() without calling
  // a virtual destructor (there isn't one).
  void* mem = malloc(sizeof(WinHttpRequestStub));
  if (!mem) return E_OUTOFMEMORY;
  auto* stub = new (mem) WinHttpRequestStub();

  // QI for the requested interface (adds a ref), then release the construction ref.
  HRESULT hr = Stub_QueryInterface(stub, riid, ppvObject);
  Stub_Release(stub);  // balance construction refcount
  return hr;
}
