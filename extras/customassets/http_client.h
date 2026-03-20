#pragma once

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

namespace CustomAssets {

struct HttpResponse {
  int statusCode;
  std::string body;
  std::string etag;
  std::string lastModified;
  bool fromCache;
  bool success;
  std::string errorMessage;
};

struct CacheInfo {
  std::string etag;
  std::string lastModified;
  std::string cachedBody;
};

class HttpClient {
 public:
  HttpClient();
  ~HttpClient();

  // Initialize with settings
  bool Initialize(const std::string& userAgent, int timeoutMs, int maxRetries);

  // Perform GET request
  HttpResponse Get(const std::string& url, const CacheInfo* cacheInfo = nullptr);

  // Shutdown
  void Shutdown();

 private:
  HINTERNET hSession_;
  std::string userAgent_;
  int timeoutMs_;
  int maxRetries_;
  bool initialized_;

  HttpResponse PerformRequest(const std::string& url, const std::vector<std::wstring>& headers);
  std::wstring ToWideString(const std::string& str);
  std::string ToNarrowString(const std::wstring& wstr);
};

}  // namespace CustomAssets
