#include "quest/auth/ca_bundle.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
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

// Appends the PEM of `cert` to `out`; false if it cannot be encoded.
bool AppendPem(X509* cert, std::string& out) {
  BioPtr mem(BIO_new(BIO_s_mem()));
  if (!mem || PEM_write_bio_X509(mem.get(), cert) != 1) return false;
  char* bytes = nullptr;
  const long len = BIO_get_mem_data(mem.get(), &bytes);
  out.append(bytes, static_cast<size_t>(len));
  return true;
}

constexpr char kBegin[] = "-----BEGIN CERTIFICATE-----";
constexpr char kEnd[] = "-----END CERTIFICATE-----";

struct Parsed {
  size_t good = 0;
  size_t bad = 0;  // PEM blocks (or a DER file) that did not parse
};

// Parses every certificate in `data` and appends each as PEM to `out`. PEM: each
// BEGIN/END CERTIFICATE block is parsed on its own, so a corrupt block is counted and skipped
// without hiding the blocks around it; text between blocks is ignored. "BEGIN TRUSTED
// CERTIFICATE" blocks are not read (Android does not use that form). A file with no PEM marker
// at all is tried as one DER certificate.
Parsed ParseCertificates(const std::string& data, std::string& out) {
  Parsed result;
  if (data.find("-----BEGIN") != std::string::npos) {
    for (size_t pos = data.find(kBegin); pos != std::string::npos; pos = data.find(kBegin, pos)) {
      const size_t end = data.find(kEnd, pos);
      const size_t next = data.find(kBegin, pos + 1);
      if (end == std::string::npos || (next != std::string::npos && next < end)) {
        // An unterminated block ends where the next one begins: it must not swallow the
        // good certificate after it.
        ++result.bad;
        if (next == std::string::npos) break;
        pos = next;
        continue;
      }
      const size_t block_end = end + sizeof(kEnd) - 1;
      BioPtr in(BIO_new_mem_buf(data.data() + pos, static_cast<int>(block_end - pos)));
      X509Ptr cert(in ? PEM_read_bio_X509(in.get(), nullptr, nullptr, nullptr) : nullptr);
      if (cert && AppendPem(cert.get(), out)) {
        ++result.good;
      } else {
        ++result.bad;
      }
      pos = block_end;
    }
  } else {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data());
    X509Ptr cert(d2i_X509(nullptr, &p, static_cast<long>(data.size())));
    if (cert && AppendPem(cert.get(), out)) {
      ++result.good;
    } else {
      ++result.bad;
    }
  }
  // Other PEM objects (TRUSTED CERTIFICATE, X509 CRL, ...) are not read; count them so a file that
  // held only those is visible in the log rather than counted nowhere.
  size_t all = 0, certs = 0;
  for (size_t p = data.find("-----BEGIN "); p != std::string::npos; p = data.find("-----BEGIN ", p + 1)) ++all;
  for (size_t p = data.find(kBegin); p != std::string::npos; p = data.find(kBegin, p + 1)) ++certs;
  result.bad += all - certs;
  ERR_clear_error();  // failed parses leave entries on this thread's OpenSSL error queue
  return result;
}

// Reads a CA file without trusting its size or type: opened non-blocking (a FIFO cannot hang
// us), required to be a regular file, and never read past kMaxCaFileBytes. Symlinks to regular
// files are followed (Android's cacerts entries are regular files; a link is tolerated).
// Returns false, with `why` set, when the entry is not usable.
bool ReadCaFile(const std::filesystem::path& path, std::string& data, const char*& why) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    why = "unopenable";
    return false;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    why = "not a regular file";
    return false;
  }
  if (st.st_size <= 0 || static_cast<size_t>(st.st_size) > kMaxCaFileBytes) {
    ::close(fd);
    why = st.st_size <= 0 ? "empty" : "too large";
    return false;
  }
  data.resize(static_cast<size_t>(st.st_size));
  size_t have = 0;
  while (have < data.size()) {
    const ssize_t n = ::read(fd, &data[have], data.size() - have);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    have += static_cast<size_t>(n);
  }
  ::close(fd);
  data.resize(have);
  if (data.empty()) {
    why = "empty";
    return false;
  }
  return true;
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
      files.push_back(it->path());  // every entry; ReadCaFile decides what is usable
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
      std::string data;
      const char* why = "";
      if (!ReadCaFile(file, data, why)) {
        ++skipped;
        continue;
      }
      std::string pem;
      const Parsed parsed = ParseCertificates(data, pem);
      unparsable += parsed.bad;
      if (parsed.good == 0) continue;
      if (bundle.pem.size() + pem.size() > kMaxCaBundleBytes) {
        truncated = true;
        break;
      }
      bundle.pem += pem;
      bundle.certificates += parsed.good;
    }
    if (bundle.certificates > 0) {
      Emit(log, LogLevel::Info, "[NEVR.AUTH] CA store loaded dir=" + dir +
                                    " certificates=" + std::to_string(bundle.certificates) +
                                    " unparsable_certs=" + std::to_string(unparsable) +
                                    " skipped_files=" + std::to_string(skipped));
      if (truncated) {
        Emit(log, LogLevel::Warning, "[NEVR.AUTH] CA store reached the " + std::to_string(kMaxCaBundleBytes) +
                                         "-byte bound; the remaining files were not read dir=" + dir);
      }
      return bundle;
    }
    Emit(log, LogLevel::Info, "[NEVR.AUTH] CA directory yielded no certificate dir=" + dir +
                                  " files=" + std::to_string(files.size()) +
                                  " unparsable_certs=" + std::to_string(unparsable) +
                                  " skipped_files=" + std::to_string(skipped));
  }
  Emit(log, LogLevel::Error,
       "[NEVR.AUTH] no CA certificates could be loaded from any Android CA directory; every TLS request "
       "will fail closed");
  return {};
}

}  // namespace nevr::quest_auth
