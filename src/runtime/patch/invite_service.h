#pragma once
// #1156 (PR-D slice D1b), docs/adr/0017: the invite service of a client: the config key, the libcurl transport and the
// worker, owned in one place. boot.cpp starts it with the values of this run (the config key, the service address, the token
// source, the log), the wizard hook submits a plan to it, and the shutdown path stops it.
//
// Start logs its outcome (N122/N124): `[NEVR.INVITE] service on source=<default|config>` at Info, or
// `feature off reason=<why> source=<default|config>` at Info (`turned_off`: the key says false) or at Warning (a value nobody
// meant, no address, an address that may not receive the token). On is the default (network.invite_codes absent or empty). A
// request while off is a `request not sent reason=not_configured` line, never a silent drop. Everything here is
// host-testable: no game, and the platform values come in through Settings.
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "runtime/patch/invite_config.h"
#include "runtime/patch/invite_transport_curl.h"
#include "runtime/patch/invite_wizard.h"
#include "runtime/patch/invite_worker.h"

namespace nevr_invite_service {

struct Settings {
  nevr_invite::Switch invite_codes = nevr_invite::kDefault;      // network.invite_codes
  nevr_invite::Source invite_source = nevr_invite::Source::Default;  // where invite_codes came from: the startup line says which
  std::string http_uri;                                         // auth.http_uri: the game service's HTTP address
  std::function<std::string()> token;                           // the current Bearer token, "" when none
  std::function<void(nevr_invite::LogLevel, const std::string&)> log;
  std::string salt;                                             // per run, for the code fingerprint
  nevr_invite::TransportOptions transport;                      // timeouts and the answer cap; its log is replaced
  bool hooks_refused = false;  // an observer this service serves refused to install: the service stays off, its Warning says why
  /// Builds the transport for this run, called once by Start and only when the service is on. Unset: the libcurl transport of
  /// the PC (invite_transport_curl.h). The Quest sets its own (src/quest/invite/invite_transport_quest.h). A null result leaves
  /// the service off with reason=transport_unavailable.
  std::function<std::unique_ptr<nevr_invite::Transport>(const std::string& http_uri, const nevr_invite::TransportOptions& options)>
      transport_factory;
};

/// Starts the service for this run. A second call changes nothing and says so. Never throws.
void Start(Settings settings);

/// Hands a plan's request to the worker. Before Start, or with the feature off, nothing is sent and the worker's line says so.
nevr_invite::Submit Submit(const nevr_invite::Plan& plan);

/// True while the service is on for this run: the feature is configured and the worker can send. The per-frame applier reads
/// it first, so a run with the feature off costs one atomic load a frame.
bool Active();

/// The answers that have arrived; the menu applier (patch/invite_answer_apply.h) is the one reader. Non-blocking.
std::vector<nevr_invite::Answer> TakeAnswers();
/// As TakeAnswers, waiting up to `timeout` for the first (the host tests).
std::vector<nevr_invite::Answer> WaitForAnswers(std::chrono::milliseconds timeout);

/// True while an answer waits that nobody has taken. The per-frame applier reads it without the service lock (one atomic load
/// on a frame with nothing to apply) and takes the answers only when it is true. The worker sets it under its own lock with
/// the answer and clears it under that lock when the answers are taken, so it is never clear while an answer waits.
bool AnswersPending();
/// As WaitForAnswers, but leaves the answer where it is: waits up to `timeout` for one and reports whether it is waiting (the
/// host tests).
bool WaitForAnswerPendingForTest(std::chrono::milliseconds timeout);
/// The number of times the service lock has been taken since process start. The host tests read it: a frame with nothing
/// waiting must not move it.
std::uint64_t ServiceLockTakesForTest();

/// Aborts a request in flight, drops the queued ones with a line and joins the worker. After it Submit is refused.
void Shutdown();

}  // namespace nevr_invite_service
