/* SYNTHESIS -- custom tool code, not from binary */

#pragma once

#include <curl/curl.h>
#include <string>

namespace nevr {

// Shared curl write callback for appending response data to a string.
// Use with: curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nevr::CurlWriteCallback);
//           curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
inline size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

} // namespace nevr
