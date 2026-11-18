#include "runtime/content/cargo_pin.h"

#include <memory>
#include <mutex>
#include <string>

#include "core/cargo_session_pin.h"
#include "core/map_session_pin.h"

namespace nevr_cargo_pin {

namespace {

// The edge's state: the feed, the Env it writes the marker through, the marker's path and whether this run writes boot state. One lock
// serialises the boot thread, the fetch thread and the game thread (the feed itself is not synchronised).
struct Edge {
  std::mutex mutex;
  std::unique_ptr<CargoFeed> feed;
  nevr_cdn::Env* env = nullptr;
  std::string marker;
  bool boot_state = false;
  bool server = false;  // BindServerFeed: B is also the server's cargo pin
  bool holds_set = false;  // a server's boot set pins a whole set (ServerHoldsSet)
  Sha256HexFn sha256_hex;
};

Edge& State() {
  static Edge edge;
  return edge;
}

// The run's hasher, as the store's (cdn_run.cpp PinOf): the pin's digest is the SHA-256 of the record text.
Sha256HexFn HasherOf(nevr_cdn::Env* env) {
  return [env](const std::string& bytes) {
    std::unique_ptr<nevr_cdn::Hasher> hasher = env->NewHasher();
    hasher->Update(bytes.data(), bytes.size());
    return hasher->Hex();
  };
}

// The refetch marker (PLAN-v7 5.5 row 14): written through the Env, and a failed write is the Warning cdn_boot_state names (the same
// record cdn_run.cpp writes for its own boot-state writes).
void WriteMarker(nevr_cdn::Env& env, const std::string& path) {
  if (env.WriteFileAtomically(path, std::string())) return;
  env.Event(nevr_cdn::EventLevel::kWarn, "cdn_boot_state",
            {{"result", "write_failed"}, {"file", "boot_refetch"}, {"consequence", "the next launch may fetch before boot once more"}});
}

// The session pin's cargo decision for this client (ADR 0039 decision 2). The decision is made under the lock; the marker is written
// outside it, once for the key the tick decided on (the tick calls this once per key, PLAN-v7 5.5 "One leave owner").
nevr_map_session_pin::CargoOutcome Decide(std::uint64_t level, std::uint64_t server_pin) {
  Edge& edge = State();
  nevr_cdn::Env* env = nullptr;
  std::string marker;
  bool write_marker = false;
  CargoView view;
  CargoDecision decision;
  {
    std::lock_guard<std::mutex> lock(edge.mutex);
    view = edge.feed->View();
    decision = DecideCargo(view, level, server_pin);
    write_marker = decision.write_marker && edge.boot_state;
    env = edge.env;
    marker = edge.marker;
  }
  if (write_marker) WriteMarker(*env, marker);
  return OutcomeFrom(view, decision);
}

}  // namespace

void Arm(const nevr_cdn::RunConfig& config, nevr_cdn::Env* env) {
  Edge& edge = State();
  {
    std::lock_guard<std::mutex> lock(edge.mutex);
    edge.env = env;
    edge.marker = nevr_cdn::BootRefetchPath(config.dir);
    edge.boot_state = config.boot_state;
    edge.server = false;
    edge.holds_set = false;
    edge.sha256_hex = HasherOf(env);
    edge.feed = std::make_unique<CargoFeed>(config.platform, config.policy);
  }
  nevr_map_session_pin::SetCargoDecide(&Decide);
}

void SetBootSet(const nevr_cdn::BootMount& boot) {
  Edge& edge = State();
  bool server = false;
  std::uint64_t pin = kCargoPinNone;
  {
    std::lock_guard<std::mutex> lock(edge.mutex);
    if (!edge.feed) return;
    edge.feed->OnBoot(boot, edge.sha256_hex);
    server = edge.server;
    pin = edge.feed->View().local;
    // F7: a server states EchoCombat only with a whole set of its own, which is what its boot pin says (the boot set is set once per run).
    if (server) edge.holds_set = pin != kCargoPinNone;
  }
  // The server's B is the pin its tick writes into the lobby; the session pin takes its own lock, so it is set outside this one.
  if (server) nevr_map_session_pin::SetCargoServerPin(pin);
}

void SetManifestPin(const nevr_cdn::Manifest& manifest) {
  Edge& edge = State();
  std::lock_guard<std::mutex> lock(edge.mutex);
  if (edge.feed) edge.feed->OnManifest(manifest, edge.sha256_hex);
}

void SetFetchState(const char* cause) {
  Edge& edge = State();
  std::lock_guard<std::mutex> lock(edge.mutex);
  if (edge.feed) edge.feed->OnEnd(cause != nullptr ? cause : "none");
}

void BindFeed(nevr_cdn::RunConfig& config, nevr_cdn::Env* env) {
  Arm(config, env);
  config.on_boot = [](const nevr_cdn::BootMount& boot) { SetBootSet(boot); };
  config.on_manifest = [](const nevr_cdn::Manifest& manifest) { SetManifestPin(manifest); };
  config.on_end = [](const char* cause) { SetFetchState(cause); };
}

bool ServerHoldsSet() {
  Edge& edge = State();
  std::lock_guard<std::mutex> lock(edge.mutex);
  return edge.holds_set;
}

void BindServerFeed(nevr_cdn::RunConfig& config, nevr_cdn::Env* env) {
  Edge& edge = State();
  {
    std::lock_guard<std::mutex> lock(edge.mutex);
    edge.server = true;
    edge.holds_set = false;
    edge.sha256_hex = HasherOf(env);
    edge.feed = std::make_unique<CargoFeed>(config.platform, config.policy);
  }
  config.on_boot = [](const nevr_cdn::BootMount& boot) { SetBootSet(boot); };
}

}  // namespace nevr_cargo_pin
