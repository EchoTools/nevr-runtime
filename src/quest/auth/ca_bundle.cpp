#include "quest/auth/ca_bundle.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>

namespace nevr::quest_auth {

namespace {
using nevr::auth::LogLevel;

void Emit(const nevr::auth::LogSink& log, LogLevel level, const std::string& message) {
  if (log) log(level, message);
}

struct X509Deleter {
  void operator()(X509* x) const { X509_free(x); }
};
struct BioDeleter {
  void operator()(BIO* b) const { BIO_free(b); }
};
using X509Ptr = std::unique_ptr<X509, X509Deleter>;
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

// Parses every certificate in `data` (PEM, any number, trailing text tolerated; else one
// DER certificate) and appends each as PEM to `out`. Returns how many parsed.
size_t ParseCertificates(const std::string& data, std::string& out) {
  std::vector<X509Ptr> certs;
  if (data.find("-----BEGIN") != std::string::npos) {
    BioPtr in(BIO_new_mem_buf(data.data(), static_cast<int>(data.size())));
    while (in) {
      X509Ptr cert(PEM_read_bio_X509(in.get(), nullptr, nullptr, nullptr));
      if (!cert) break;
      certs.push_back(std::move(cert));
    }
  } else {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
    X509Ptr cert(d2i_X509(nullptr, &p, static_cast<long>(data.size())));
    if (cert) certs.push_back(std::move(cert));
  }
  for (const X509Ptr& cert : certs) {
    BioPtr mem(BIO_new(BIO_s_mem()));
    if (!mem || PEM_write_bio_X509(mem.get(), cert.get()) != 1) return 0;
    char* bytes = nullptr;
    const long len = BIO_get_mem_data(mem.get(), &bytes);
    out.append(bytes, static_cast<size_t>(len));
  }
  ERR_clear_error();  // a failed parse leaves entries on this thread's OpenSSL error queue
  return certs.size();
}

}  // namespace

const std::vector<std::string>& AndroidCaDirs() {
  static const std::vector<std::string> dirs = {"/apex/com.android.conscrypt/cacerts",
                                                "/system/etc/security/cacerts"};
  return dirs;
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
    size_t unparsable = 0, skipped = 0;
    bool truncated = false;
    for (const std::filesystem::path& file : files) {
      std::ifstream in(file, std::ios::binary);
      std::ostringstream contents;
      contents << in.rdbuf();
      const std::string data = contents.str();
      if ((!in.good() && !in.eof()) || data.empty() || data.size() > kMaxCaFileBytes) {
        ++skipped;
        continue;
      }
      std::string pem;
      const size_t parsed = ParseCertificates(data, pem);
      if (parsed == 0) {
        ++unparsable;
        continue;
      }
      if (bundle.pem.size() + pem.size() > kMaxCaBundleBytes) {
        truncated = true;
        break;
      }
      bundle.pem += pem;
      bundle.certificates += parsed;
    }
    if (bundle.certificates > 0) {
      Emit(log, LogLevel::Info, "[NEVR.AUTH] CA store loaded dir=" + dir +
                                    " certificates=" + std::to_string(bundle.certificates) +
                                    " unparsable_files=" + std::to_string(unparsable) +
                                    " skipped_files=" + std::to_string(skipped));
      if (truncated) {
        Emit(log, LogLevel::Warning, "[NEVR.AUTH] CA store reached the " + std::to_string(kMaxCaBundleBytes) +
                                         "-byte bound; the remaining files were not read dir=" + dir);
      }
      return bundle;
    }
    Emit(log, LogLevel::Info, "[NEVR.AUTH] CA directory yielded no certificate dir=" + dir +
                                  " files=" + std::to_string(files.size()) +
                                  " unparsable_files=" + std::to_string(unparsable) +
                                  " skipped_files=" + std::to_string(skipped));
  }
  Emit(log, LogLevel::Error,
       "[NEVR.AUTH] no CA certificates could be loaded from any Android CA directory; every TLS request "
       "will fail closed");
  return {};
}

}  // namespace nevr::quest_auth
