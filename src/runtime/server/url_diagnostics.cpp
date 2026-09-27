#include "runtime/server/url_diagnostics.h"

#include <curl/curl.h>

#include <memory>

namespace GameServer {
namespace {
constexpr size_t kMaximumDiagnosticUrlSize = 8192;
constexpr char kMalformedUrl[] = "[redacted malformed URL]";

bool HasOnlyValidEscapesAndCharacters(std::string_view url) {
  for (size_t index = 0; index < url.size(); ++index) {
    const unsigned char value = static_cast<unsigned char>(url[index]);
    if (value == 0 || value < 0x20 || value == 0x7f) return false;
    if (value == '%') {
      if (index + 2 >= url.size()) return false;
      const auto isHex = [](unsigned char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
      };
      if (!isHex(static_cast<unsigned char>(url[index + 1])) ||
          !isHex(static_cast<unsigned char>(url[index + 2]))) {
        return false;
      }
      index += 2;
    }
  }
  return true;
}
}  // namespace

std::string RedactUrlForDiagnostics(std::string_view input) {
  if (input.empty() || input.size() > kMaximumDiagnosticUrlSize || !HasOnlyValidEscapesAndCharacters(input)) {
    return kMalformedUrl;
  }

  const std::string ownedInput(input);
  using CurlUrl = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
  CurlUrl parsed(curl_url(), &curl_url_cleanup);
  if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, ownedInput.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK) {
    return kMalformedUrl;
  }

  char* rawScheme = nullptr;
  char* rawHost = nullptr;
  const CURLUcode schemeResult = curl_url_get(parsed.get(), CURLUPART_SCHEME, &rawScheme, 0);
  const CURLUcode hostResult = curl_url_get(parsed.get(), CURLUPART_HOST, &rawHost, 0);
  const std::string scheme = schemeResult == CURLUE_OK && rawScheme != nullptr ? rawScheme : "";
  const bool hasHost = hostResult == CURLUE_OK && rawHost != nullptr && rawHost[0] != '\0';
  curl_free(rawScheme);
  curl_free(rawHost);
  if (!hasHost || (scheme != "http" && scheme != "https" && scheme != "ws" && scheme != "wss")) {
    return kMalformedUrl;
  }

  constexpr CURLUPart sensitiveParts[] = {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_OPTIONS,
                                           CURLUPART_QUERY, CURLUPART_FRAGMENT};
  for (const CURLUPart part : sensitiveParts) {
    const CURLUcode result = curl_url_set(parsed.get(), part, nullptr, 0);
    if (result != CURLUE_OK && result != CURLUE_NO_QUERY && result != CURLUE_NO_FRAGMENT &&
        result != CURLUE_NO_USER && result != CURLUE_NO_PASSWORD && result != CURLUE_NO_OPTIONS) {
      return kMalformedUrl;
    }
  }

  char* redacted = nullptr;
  if (curl_url_get(parsed.get(), CURLUPART_URL, &redacted, 0) != CURLUE_OK || redacted == nullptr) {
    return kMalformedUrl;
  }
  std::string output(redacted);
  curl_free(redacted);
  return output;
}

}  // namespace GameServer
