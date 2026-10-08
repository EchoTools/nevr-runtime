#pragma once
// Android's system CA store as one in-memory PEM bundle for libcurl's CAINFO_BLOB.
//
// Why not CURLOPT_CAPATH: OpenSSL finds a CApath certificate by `<X509_NAME_hash>.N`,
// the SHA-1 based subject hash. Android's /system/etc/security/cacerts is named by the
// older MD5-based hash (`openssl x509 -subject_hash_old`), so OpenSSL never finds any
// of them and every handshake fails closed. Reading the files ourselves needs no name
// match.

#include "core/auth_types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace nevr::quest_auth {

struct CaBundle {
  std::string pem;           // concatenated PEM certificates
  size_t certificates = 0;   // how many were loaded
};

// Directories in priority order. The updatable Conscrypt store, when present and
// non-empty, supersedes the read-only system copy.
const std::vector<std::string>& AndroidCaDirs();

// Loads the certificates from the first directory in `dirs` that yields any, as PEM
// (stored PEM is used as is; stored DER is wrapped). Logs the directory and the count at
// Info, and logs an Error when no directory yields a certificate (the caller must then
// fail closed). Never throws.
CaBundle LoadCaBundle(const std::vector<std::string>& dirs, const nevr::auth::LogSink& log);

// Base64 of `bytes` wrapped as a PEM CERTIFICATE block. Exposed for the tests.
std::string DerToPem(const std::string& der);

}  // namespace nevr::quest_auth
