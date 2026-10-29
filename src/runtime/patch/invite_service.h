#pragma once
// #1156 (PR-D slice D1b), docs/adr/0017: the invite service of a client: the config key, the libcurl transport and the
// worker, owned in one place. boot.cpp starts it with the values of this run (the config key, the service address, the token
// source, the log), the wizard hook submits a plan to it, and the shutdown path stops it.
//
// Start always logs its outcome at Info (N122/N124): `[NEVR.INVITE] service on` or `feature off reason=<why>`. Off is the
// default (network.invite_codes absent) and a request while off is a `request not sent reason=not_configured` line, never a
// silent drop. Everything here is host-testable: no game, and the platform values come in through Settings.
#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "runtime/patch/invite_config.h"
#include "runtime/patch/invite_transport_curl.h"
#include "runtime/patch/invite_wizard.h"
#include "runtime/patch/invite_worker.h"

namespace nevr_invite_service {

struct Settings {
  nevr_invite::Switch invite_codes = nevr_invite::Switch::Off;  // network.invite_codes
  std::string http_uri;                                         // auth.http_uri: the game service's HTTP address
  std::function<std::string()> token;                           // the current Bearer token, "" when none
  std::function<void(nevr_invite::LogLevel, const std::string&)> log;
  std::string salt;                                             // per run, for the code fingerprint
  nevr_invite::TransportOptions transport;                      // timeouts and the answer cap; its log is replaced
  bool hooks_refused = false;  // an observer this service serves refused to install: the service stays off, its Warning says why
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

/// Aborts a request in flight, drops the queued ones with a line and joins the worker. After it Submit is refused.
void Shutdown();

}  // namespace nevr_invite_service
