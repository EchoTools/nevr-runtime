#pragma once

// Token minting is serialised process-wide. The ServerDB socket and the telemetry socket each carry
// a 401 refresher on their own ixwebsocket thread, and RequestRegistration mints on the game thread;
// every mint ends in SaveAuthToken, an unlocked truncating write of .credentials.json, so two
// concurrent mints (a refresh can take up to the 10 s CURLOPT_TIMEOUT) can tear the file.

#include <functional>
#include <mutex>
#include <string>

namespace nevr_serverdb_auth {

inline std::string RunSerializedMint(const std::function<std::string()>& mint) {
  static std::mutex mintMutex;
  std::lock_guard<std::mutex> lock(mintMutex);
  return mint();
}

}  // namespace nevr_serverdb_auth
