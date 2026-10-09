#include "core/device_auth_flow.h"

namespace nevr::auth {

namespace {
// Wipes the code and the URL carrying it however the flow ends.
struct WipeOnExit {
  std::string& secret;
  ~WipeOnExit() { WipeSecret(secret); }
};
}  // namespace

DeviceFlowResult RunDeviceCodeFlow(const DeviceFlowOps& ops, const std::string& login_url) {
  const auto log = [&ops](LogLevel level, const std::string& message) {
    if (ops.log) ops.log(level, message);
  };
  DeviceFlowResult none;
  if (!ops.now || !ops.request_device_code || !ops.open_browser || !ops.show_open_failure || !ops.poll ||
      !ops.sleep || !ops.log) {
    log(LogLevel::Error, "[NEVR.AUTH] Device authorization operations are unavailable");
    return none;
  }

  std::string code = ops.request_device_code();
  const WipeOnExit wipe_code{code};
  if (code.empty()) {
    log(LogLevel::Warning, "[NEVR.AUTH] device code request failed, cannot start device-auth flow");
    return none;
  }

  const DeviceFlowOps::Clock::time_point deadline = ops.now() + kDeviceAuthLifetime;
  log(LogLevel::Info, "[NEVR.AUTH] Device authorization started; browser opening requested");
  log(LogLevel::Info, "[NEVR.AUTH] Device code expires in 5 minutes");

  std::string full_url = login_url + "?code=" + code;
  const WipeOnExit wipe_url{full_url};
  const intptr_t browserResult = ops.open_browser(full_url);
  // The code is a credential for this session, so the log says where the browser was sent and
  // what the open returned, with the code masked (ShellExecute reports success above 32).
  log(LogLevel::Info,
      std::string("[NEVR.AUTH] browser open requested url=") + login_url + "?code=<" +
          std::to_string(code.size()) + " chars masked> open_result=" + std::to_string(browserResult) +
          (browserResult <= kBrowserOpenAcceptedAbove ? " (failed)" : " (accepted)"));
  int uiResult = 1;
  if (browserResult <= kBrowserOpenAcceptedAbove) {
    uiResult = ops.show_open_failure(code, login_url, browserResult);
  }

  // A code that runs out is the end of the login on Windows, and routine on Quest, which asks
  // for a new code: the level says which.
  const LogLevel runOutLevel = ops.renews_expired_codes ? LogLevel::Info : LogLevel::Warning;
  const auto timedOut = [&] { log(runOutLevel, "[NEVR.AUTH] Device auth timed out after 5 minutes"); };

  const bool expiredAfterBrowserOrUi = ops.now() >= deadline;
  if (browserResult <= kBrowserOpenAcceptedAbove && uiResult == 0) {
    log(LogLevel::Error, "[NEVR.AUTH] device authorization stopped because the browser could not be opened");
    return none;
  }
  if (expiredAfterBrowserOrUi) {
    timedOut();
    return none;
  }

  unsigned int pollCount = 0;
  while (true) {
    const DeviceFlowOps::Clock::time_point beforeSleep = ops.now();
    if (beforeSleep >= deadline) {
      timedOut();
      return none;
    }
    const DeviceFlowOps::Clock::duration remaining = deadline - beforeSleep;
    const DeviceFlowOps::Clock::duration wait =
        remaining < kDeviceAuthPollInterval ? remaining : kDeviceAuthPollInterval;
    if (wait > DeviceFlowOps::Clock::duration::zero()) ops.sleep(wait);
    if (ops.cancelled && ops.cancelled()) {
      log(LogLevel::Info, "[NEVR.AUTH] Device auth cancelled: the game is closing");
      return none;
    }
    if (ops.now() >= deadline) {
      timedOut();
      return none;
    }

    const TokenAuth::DevicePollResponse response = ops.poll(code);
    // The server deletes a verified code when it answers this poll, so a "verified" that arrives
    // after the deadline is still the player's login: it is taken. Anything else after the
    // deadline ends the code.
    if (response.status != TokenAuth::DevicePollStatus::Verified && ops.now() >= deadline) {
      timedOut();
      return none;
    }

    switch (response.status) {
      case TokenAuth::DevicePollStatus::Verified: {
        DeviceFlowResult ok;
        ok.verified = true;
        ok.response = response;
        return ok;
      }
      case TokenAuth::DevicePollStatus::Expired:
        if (ops.renews_expired_codes) {
          log(LogLevel::Info, "[NEVR.AUTH] Device code expired before a sign-in; a new code will be requested");
        } else {
          log(LogLevel::Warning, "[NEVR.AUTH] Device code expired. Please restart to try again.");
        }
        return none;
      case TokenAuth::DevicePollStatus::Pending:
        ++pollCount;
        if (pollCount % 10U == 0U) {
          const auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline - ops.now());
          log(LogLevel::Debug, "[NEVR.AUTH] Still waiting for authorization (" +
                                   std::to_string(left.count()) + "s remaining)");
        }
        break;
      case TokenAuth::DevicePollStatus::Error:
        log(LogLevel::Warning, "[NEVR.AUTH] polling aborted after single error (no retry)");
        return none;
    }
  }
}

}  // namespace nevr::auth
