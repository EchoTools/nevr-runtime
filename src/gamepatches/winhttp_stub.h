#pragma once
#include <unknwn.h>
#include <windows.h>
#include <winhttp.h>

#include <map>
#include <string>
#include <vector>

class WinHttpRequestStub : public IUnknown {
 public:
  WinHttpRequestStub();
  virtual ~WinHttpRequestStub();

  STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override;
  STDMETHOD_(ULONG, AddRef)() override;
  STDMETHOD_(ULONG, Release)() override;

  STDMETHOD(SetProxy)(LONG ProxySetting, VARIANT ProxyServer, VARIANT BypassList);
  STDMETHOD(SetCredentials)(BSTR UserName, BSTR Password, LONG Flags);
  STDMETHOD(Open)(BSTR Method, BSTR Url, VARIANT Async);
  STDMETHOD(SetRequestHeader)(BSTR Header, BSTR Value);
  STDMETHOD(GetResponseHeader)(BSTR Header, BSTR* Value);
  STDMETHOD(GetAllResponseHeaders)(BSTR* Headers);
  STDMETHOD(Send)(VARIANT Body);
  STDMETHOD(get_Status)(LONG* Status);
  STDMETHOD(get_StatusText)(BSTR* Status);
  STDMETHOD(get_ResponseText)(BSTR* Body);
  STDMETHOD(get_ResponseBody)(VARIANT* Body);
  STDMETHOD(get_ResponseStream)(VARIANT* Body);
  STDMETHOD(get_Option)(LONG Option, VARIANT* Value);
  STDMETHOD(put_Option)(LONG Option, VARIANT Value);
  STDMETHOD(WaitForResponse)(VARIANT Timeout, VARIANT_BOOL* Succeeded);
  STDMETHOD(Abort)();
  STDMETHOD(SetTimeouts)(LONG ResolveTimeout, LONG ConnectTimeout, LONG SendTimeout, LONG ReceiveTimeout);
  STDMETHOD(SetClientCertificate)(BSTR ClientCertificate);
  STDMETHOD(SetAutoLogonPolicy)(LONG AutoLogonPolicy);

  ULONG GetRefCount() const { return m_refCount; }

 private:
  ULONG m_refCount;

  std::wstring m_method;
  std::wstring m_url;
  std::map<std::wstring, std::wstring> m_requestHeaders;
  std::vector<char> m_responseBody;
  std::map<std::wstring, std::wstring> m_responseHeaders;
  long m_statusCode;
  bool m_sent;

  // UTF-8 cached strings to maintain lifetime for curl operations
  std::string m_method_utf8;
  std::string m_url_utf8;
  std::vector<std::string> m_header_lines_utf8;
};

HRESULT WINAPI CreateWinHttpRequestStub(REFIID riid, void** ppvObject);
