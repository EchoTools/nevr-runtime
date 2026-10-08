#include "quest/auth/ca_bundle.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace nevr::quest_auth {

namespace {
using nevr::auth::LogLevel;

constexpr char kPemBegin[] = "-----BEGIN CERTIFICATE-----";
constexpr size_t kMaxCertFileBytes = 256 * 1024;  // a CA file is a few KiB

void Emit(const nevr::auth::LogSink& log, LogLevel level, const std::string& message) {
  if (log) log(level, message);
}

size_t CountOccurrences(const std::string& text, const char* needle) {
  size_t count = 0;
  for (size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1)) ++count;
  return count;
}

}  // namespace

const std::vector<std::string>& AndroidCaDirs() {
  static const std::vector<std::string> dirs = {"/apex/com.android.conscrypt/cacerts",
                                                "/system/etc/security/cacerts"};
  return dirs;
}

std::string DerToPem(const std::string& der) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string b64;
  for (size_t i = 0; i < der.size(); i += 3) {
    const uint32_t b0 = static_cast<unsigned char>(der[i]);
    const uint32_t b1 = i + 1 < der.size() ? static_cast<unsigned char>(der[i + 1]) : 0;
    const uint32_t b2 = i + 2 < der.size() ? static_cast<unsigned char>(der[i + 2]) : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    b64.push_back(t[(v >> 18) & 63]);
    b64.push_back(t[(v >> 12) & 63]);
    b64.push_back(i + 1 < der.size() ? t[(v >> 6) & 63] : '=');
    b64.push_back(i + 2 < der.size() ? t[v & 63] : '=');
  }
  std::string out = std::string(kPemBegin) + "\n";
  for (size_t i = 0; i < b64.size(); i += 64) out += b64.substr(i, 64) + "\n";
  out += "-----END CERTIFICATE-----\n";
  return out;
}

CaBundle LoadCaBundle(const std::vector<std::string>& dirs, const nevr::auth::LogSink& log) {
  for (const std::string& dir : dirs) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code type_ec;
      if (it->is_regular_file(type_ec) && !type_ec) files.push_back(it->path());
    }
    if (ec) {
      Emit(log, LogLevel::Info, "[NEVR.AUTH] CA directory not readable dir=" + dir + " error=" + ec.message());
      continue;
    }
    std::sort(files.begin(), files.end());  // deterministic bundle

    CaBundle bundle;
    size_t skipped = 0;
    for (const std::filesystem::path& file : files) {
      std::ifstream in(file, std::ios::binary);
      std::ostringstream contents;
      contents << in.rdbuf();
      const std::string data = contents.str();
      if (!in.good() && !in.eof()) {
        ++skipped;
        continue;
      }
      if (data.empty() || data.size() > kMaxCertFileBytes) {
        ++skipped;
        continue;
      }
      if (const size_t pem = CountOccurrences(data, kPemBegin); pem > 0) {
        bundle.pem += data;
        if (data.back() != '\n') bundle.pem += '\n';
        bundle.certificates += pem;
      } else if (static_cast<unsigned char>(data[0]) == 0x30) {  // DER SEQUENCE
        bundle.pem += DerToPem(data);
        ++bundle.certificates;
      } else {
        ++skipped;
      }
    }
    if (bundle.certificates > 0) {
      Emit(log, LogLevel::Info, "[NEVR.AUTH] CA store loaded dir=" + dir +
                                    " certificates=" + std::to_string(bundle.certificates) +
                                    " skipped_files=" + std::to_string(skipped));
      return bundle;
    }
    Emit(log, LogLevel::Info, "[NEVR.AUTH] CA directory held no certificates dir=" + dir +
                                  " files=" + std::to_string(files.size()));
  }
  Emit(log, LogLevel::Error,
       "[NEVR.AUTH] no CA certificates could be loaded from any Android CA directory; every TLS request "
       "will fail closed");
  return {};
}

}  // namespace nevr::quest_auth
