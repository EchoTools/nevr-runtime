// The PC's half of the cargo pin (PLAN-v7 5.5 "Feed", R11). A client's boot mount, each fetch's manifest and the fetch's end feed the
// session pin's cargo decision (core/cargo_session_pin.h CargoFeed): nevr_cdn::Start binds it (lifecycle/cdn_start.cpp). A dedicated
// server's boot set is the pin its session pin's tick writes to the lobby (BindServerFeed): nevr_cdn::StartServer binds it.
#pragma once

#include "cdn/cdn_run.h"

namespace nevr_cargo_pin {

// Arms the client's decision for one start, before the boot mount: the run's platform, policy, store and marker, and the session pin's
// cargo input (nevr_map_session_pin::SetCargoDecide). `config.boot_state` is read here, so the binding must run after it is set.
void Arm(const nevr_cdn::RunConfig& config, nevr_cdn::Env* env);

// RunConfig::on_boot: the boot mount's set, its status and its fetch state (B, BootStatus, boot_fetch). On a server (BindServerFeed) B is
// also the server's cargo pin (nevr_map_session_pin::SetCargoServerPin): kCargoPinNone when nothing mounted whole.
void SetBootSet(const nevr_cdn::BootMount& boot);
// RunConfig::on_manifest: the newest manifest this run parsed (M).
void SetManifestPin(const nevr_cdn::Manifest& manifest);
// RunConfig::on_end: the cause the fetch's boot-state writes settled on ("none", "settled", "undeliverable" or "no_space").
void SetFetchState(const char* cause);

// Binds the client's feed to one run's config: Arm, then the three seams above. Called after config.boot_state is set (Arm reads it),
// before nevr_cdn::Start (lifecycle/cdn_start.cpp).
void BindFeed(nevr_cdn::RunConfig& config, nevr_cdn::Env* env);

// Binds the dedicated server's feed to one run's config: its on_boot only (the server's data is fixed by its boot set), before
// nevr_cdn::StartServer. The session pin's cargo decision is the client's, so none is armed here.
void BindServerFeed(nevr_cdn::RunConfig& config, nevr_cdn::Env* env);

}  // namespace nevr_cargo_pin
