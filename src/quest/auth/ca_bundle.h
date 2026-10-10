#pragma once
// Android's system CA store as one in-memory PEM bundle for libcurl's CAINFO_BLOB.
//
// Why not CURLOPT_CAPATH: OpenSSL finds a CApath certificate by `<X509_NAME_hash>.N`,
// the SHA-1 based subject hash. Android's /system/etc/security/cacerts is named by the
// older MD5-based hash (`openssl x509 -subject_hash_old`), so OpenSSL never finds any
// of them and every handshake fails closed. Reading the files ourselves needs no name
// match.
//
// Every certificate is parsed with OpenSSL and re-encoded, so the blob handed to libcurl
// holds only certificates that parse: one corrupt file cannot make libcurl reject the
// whole bundle (CURLE_SSL_CACERT_BADFILE).

#include "core/auth_types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace nevr::quest_auth {

struct CaBundle {
  std::string pem;           // concatenated PEM certificates, each one parsed and re-encoded
  size_t certificates = 0;   // how many were loaded
};

// A CA file is a few KiB and the Android store a few hundred KiB; these bound what a
// damaged or hostile directory can make us read. The per-file bound is checked on the open file
// (fstat) before anything is read, so a huge or endless entry costs nothing.
inline constexpr size_t kMaxCaFileBytes = 256 * 1024;
inline constexpr size_t kMaxCaBundleBytes = 4 * 1024 * 1024;

// Directories in priority order. The updatable Conscrypt store, when present and
// yielding a certificate, supersedes the read-only system copy.
const std::vector<std::string>& AndroidCaDirs();

// Loads the certificates from the first directory in `dirs` that yields at least one that
// parses (stored PEM, possibly with trailing text and several certificates per file, or
// stored DER). Each PEM block is parsed on its own, so a corrupt block does not hide its
// neighbours; "BEGIN TRUSTED CERTIFICATE" blocks are not read. Entries that are not regular
// files (FIFOs, dangling links, directories), empty or oversize are counted as skipped. Per
// directory the Info log gives the counts of certificates, unparsable certificates and skipped
// entries; no certificate content is logged. An Error is logged
// when no directory yields a certificate (the caller must then fail closed). Never throws.
CaBundle LoadCaBundle(const std::vector<std::string>& dirs, const nevr::auth::LogSink& log);

}  // namespace nevr::quest_auth
