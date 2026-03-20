#include "http_client.h"

#include <vector>

#include "logging.h"

namespace CustomAssets {

HttpClient::HttpClient() : hSession_(nullptr), timeoutMs_(30000), maxRetries_(3), initialized_(false) {}

HttpClient::~HttpClient() { Shutdown(); }

std::wstring HttpClient::ToWideString(const std::string& str) {
  if (str.empty()) return std::wstring();

  int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
  std::wstring wstr(size, 0);
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size);
  return wstr;
}

std::string HttpClient::ToNarrowString(const std::wstring& wstr) {
  if (wstr.empty()) return std::string();

  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string str(size, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, nullptr, nullptr);
  return str;
}

bool HttpClient::Initialize(const std::string& userAgent, int timeoutMs, int maxRetries) {
  userAgent_ = userAgent;
  timeoutMs_ = timeoutMs;
  maxRetries_ = maxRetries;

  hSession_ = WinHttpOpen(ToWideString(userAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0);

  if (!hSession_) {
    Log(EchoVR::LogLevel::Error, "[CustomAssets] Failed to initialize WinHTTP");
    return false;
  }

  // Set timeouts
  WinHttpSetTimeouts(hSession_, timeoutMs_, timeoutMs_, timeoutMs_, timeoutMs_);

  initialized_ = true;
  return true;
}

void HttpClient::Shutdown() {
  if (hSession_) {
    WinHttpCloseHandle(hSession_);
    hSession_ = nullptr;
  }
  initialized_ = false;
}

HttpResponse HttpClient::Get(const std::string& url, const CacheInfo* cacheInfo) {
  HttpResponse response;
  response.success = false;
  response.statusCode = 0;
  response.fromCache = false;

  if (!initialized_) {
    response.errorMessage = "HTTP client not initialized";
    return response;
  }

  // Build headers for cache validation
  std::vector<std::wstring> headers;
  if (cacheInfo) {
    if (!cacheInfo->etag.empty()) {
      headers.push_back(L"If-None-Match: " + ToWideString(cacheInfo->etag));
    }
    if (!cacheInfo->lastModified.empty()) {
      headers.push_back(L"If-Modified-Since: " + ToWideString(cacheInfo->lastModified));
    }
  }

  // Try with retries
  for (int attempt = 0; attempt < maxRetries_; ++attempt) {
    response = PerformRequest(url, headers);

    // Check if 304 Not Modified
    if (response.statusCode == 304 && cacheInfo) {
      response.success = true;
      response.fromCache = true;
      response.body = cacheInfo->cachedBody;
      return response;
    }

    if (response.success) {
      return response;
    }

    // Wait before retry (exponential backoff)
    if (attempt < maxRetries_ - 1) {
      Sleep((attempt + 1) * 300);
    }
  }

  return response;
}

HttpResponse HttpClient::PerformRequest(const std::string& url, const std::vector<std::wstring>& headers) {
  HttpResponse response;
  response.success = false;
  response.statusCode = 0;

  // Parse URL
  URL_COMPONENTS urlComp = {0};
  urlComp.dwStructSize = sizeof(urlComp);

  wchar_t hostname[256] = {0};
  wchar_t urlPath[1024] = {0};

  urlComp.lpszHostName = hostname;
  urlComp.dwHostNameLength = sizeof(hostname) / sizeof(hostname[0]);
  urlComp.lpszUrlPath = urlPath;
  urlComp.dwUrlPathLength = sizeof(urlPath) / sizeof(urlPath[0]);

  std::wstring wurl = ToWideString(url);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) {
    response.errorMessage = "Invalid URL";
    return response;
  }

  // Connect to server
  HINTERNET hConnect = WinHttpConnect(hSession_, hostname, urlComp.nPort, 0);
  if (!hConnect) {
    response.errorMessage = "Failed to connect";
    return response;
  }

  // Open request
  DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
  HINTERNET hRequest =
      WinHttpOpenRequest(hConnect, L"GET", urlPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    response.errorMessage = "Failed to open request";
    return response;
  }

  // Add custom headers
  for (const auto& header : headers) {
    WinHttpAddRequestHeaders(hRequest, header.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
  }

  // Send request
  if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    response.errorMessage = "Failed to send request";
    return response;
  }

  // Receive response
  if (!WinHttpReceiveResponse(hRequest, nullptr)) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    response.errorMessage = "Failed to receive response";
    return response;
  }

  // Get status code
  DWORD statusCode = 0;
  DWORD statusCodeSize = sizeof(statusCode);
  WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode,
                      &statusCodeSize, nullptr);
  response.statusCode = statusCode;

  // Get ETag header
  wchar_t etag[256] = {0};
  DWORD etagSize = sizeof(etag);
  if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_ETAG, nullptr, etag, &etagSize, nullptr)) {
    response.etag = ToNarrowString(etag);
  }

  // Get Last-Modified header
  wchar_t lastMod[256] = {0};
  DWORD lastModSize = sizeof(lastMod);
  if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LAST_MODIFIED, nullptr, lastMod, &lastModSize, nullptr)) {
    response.lastModified = ToNarrowString(lastMod);
  }

  // Read response body
  std::string body;
  DWORD bytesAvailable = 0;
  do {
    bytesAvailable = 0;
    if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
      break;
    }

    if (bytesAvailable > 0) {
      std::vector<char> buffer(bytesAvailable + 1, 0);
      DWORD bytesRead = 0;
      if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
        body.append(buffer.data(), bytesRead);
      }
    }
  } while (bytesAvailable > 0);

  response.body = body;
  response.success = (statusCode == 200 || statusCode == 304);

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);

  return response;
}

}  // namespace CustomAssets
