// The runtime side of the host API (src/extension/host_api.h): owners, data
// overrides and named hook points, with no dependency on the game, a script
// VM or the platform. The runtime registers hook points and invokes them from
// its detours; plugins and script bindings reach this only through the
// NevrHostApi table.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/reader_gate.h"
#include "extension/host_api.h"

namespace nevr_script {

// One structured log record. `event` is a stable name an operator can search
// for; the runtime renders a record as one `[NEVR.SCRIPT]` line.
struct LogRecord {
  NevrLogLevel level;
  const char* event;        // registry: owner_opened, override_set, override_conflict, hook_added,
                            // hook_unknown, override_unknown, undeclared, quiesce_failed, callback_failed, owner_disabled,
                            // owner_reset, owner_log; script host (script_host.h): script_loaded,
                            // script_refused, script_reloaded, reload_failed
  const char* owner;        // "" when none
  const char* other_owner;  // the owner already holding the key, on override_conflict; else ""
  const char* target;       // the override key or hook point name; else ""
  const char* detail;       // free text; else ""
};
using LogSink = std::function<void(const LogRecord&)>;

// What an owner's manifest declares it will touch. Once an owner has a
// declaration, override_set and hook_add outside it fail with
// NEVR_ERR_UNDECLARED, so the declaration a policy engine reads before the
// owner runs is also the limit of what it can do.
struct Declaration {
  std::vector<std::string> overrides;  // exact keys, or "prefix.*" for every key under prefix.
  std::vector<std::string> hooks;      // exact hook point names
};

bool DeclarationCoversKey(const Declaration& declaration, const std::string& key);

struct FieldSpec {
  std::string name;
  NevrValueType type;
  bool pre_writable;   // a pre callback may set it
  bool post_writable;  // a post callback may set it
};

class Registry;
class HookPoint;

}  // namespace nevr_script

// The C ABI's opaque types, defined here for the host only.
struct NevrOwner {
  nevr_script::Registry* registry;
  std::string name;
  uint32_t order;  // position in `plugins:` order; callbacks chain by it
  std::atomic<bool> disabled{false};
  std::string last_error;
  bool declared = false;  // Registry::Declare was called: enforce `declaration`
  nevr_script::Declaration declaration;
  // Callbacks carry the generation they were added under and run only while it
  // is current; Registry::Quiesce bumps it, then waits on `gate` until every
  // game thread that entered one of the owner's callbacks has left.
  std::atomic<uint64_t> generation{0};
  nevr::ReaderGate gate;
};

struct NevrHookCall {
  const nevr_script::HookPoint* hook;
  NevrValue* fields;
  std::unique_ptr<std::string[]> strings;  // copies of STRING values set during the call
  NevrHookPhase phase;
  const NevrOwner* current;  // whose callback is running
  bool skip_original;
  std::string fail_reason;
};

namespace nevr_script {

class HookPoint {
 public:
  struct Callback {
    NevrOwner* owner;
    NevrHookFn fn;
    void* user;
    uint64_t generation;  // the owner's generation when it was added
  };
  using Chain = std::vector<Callback>;

  const std::string& Name() const { return name_; }
  const std::vector<FieldSpec>& Fields() const { return fields_; }
  // Index of `field`, or -1.
  int FieldIndex(const char* field) const;

 private:
  friend class Registry;
  HookPoint(std::string name, std::vector<FieldSpec> fields)
      : name_(std::move(name)), fields_(std::move(fields)) {}
  std::string name_;
  std::vector<FieldSpec> fields_;
  // Swapped whole on change (copy on write), so an invocation on a game thread
  // walks a chain nobody mutates under it.
  std::shared_ptr<const Chain> pre_ = std::make_shared<Chain>();
  std::shared_ptr<const Chain> post_ = std::make_shared<Chain>();
};

// The original function behind a hook point: reads the call's fields and writes
// its results back into them.
using OriginalFn = void (*)(NevrValue* fields, void* ctx);

class Registry {
 public:
  explicit Registry(LogSink sink);
  ~Registry();
  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  const NevrHostApi* Api() const;

  // One owner per plugin or script, opened in `plugins:` order.
  NevrOwner* OpenOwner(const std::string& name);
  // Stop the owner: its callbacks are skipped from now on and its overrides are
  // dropped. Safe to call from inside one of its own callbacks.
  void DisableOwner(NevrOwner* owner, const std::string& reason);
  // After this returns true, none of the owner's callbacks added before the call
  // is running or will run, on any thread, so a binding may free what they use
  // (their `user` data, its VM state). Waits up to `timeout` for callbacks
  // already running; false on timeout, and when called from one of the owner's
  // own callbacks (it would wait on itself). Its overrides are untouched.
  bool Quiesce(NevrOwner* owner, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
  // Drop the owner's overrides and callbacks, Quiesce, and enable it again,
  // keeping its place in the order (hot reload). False when Quiesce failed; the
  // owner then stays disabled.
  bool ResetOwner(NevrOwner* owner);
  // Limit the owner to what its manifest declares (see Declaration).
  void Declare(NevrOwner* owner, Declaration declaration);
  // Finds an open owner by name, or null.
  NevrOwner* FindOwner(const std::string& name) const;

  // One record from the runtime side (the script host's own events).
  void Record(NevrLogLevel level, const char* event, const NevrOwner* owner, const char* target,
              const std::string& detail);

  // Runtime side. An override key exists only once registered, with its type.
  // Returns false when the name is empty, malformed or already registered.
  bool RegisterOverridePoint(const std::string& key, NevrValueType type);

  // Runtime side. Names are unique; a second registration of a name returns null.
  HookPoint* RegisterHookPoint(const std::string& name, std::vector<FieldSpec> fields);
  HookPoint* FindHookPoint(const char* name) const;
  // Runs the pre chain, the original (unless a pre callback asked to skip it),
  // then the post chain. `fields` holds one value per declared field, in order.
  void Invoke(const HookPoint* hook, NevrValue* fields, OriginalFn original, void* ctx);

  // C ABI entry points (host_registry.cpp builds the NevrHostApi table from them).
  NevrStatus OverrideSet(NevrOwner* owner, const char* key, const NevrValue* value);
  NevrStatus OverrideGet(NevrOwner* owner, const char* key, NevrValue* out);
  NevrStatus HookAdd(NevrOwner* owner, const char* hook, NevrHookPhase phase, NevrHookFn fn,
                     void* user);
  void OwnerLog(NevrOwner* owner, NevrLogLevel level, const char* message);

 private:
  struct OverrideEntry {
    NevrOwner* owner;
    NevrValue value;
    std::string text;  // storage for a STRING value
  };

  NevrStatus Undeclared(NevrOwner* owner, const char* what, const char* name);
  void Emit(NevrLogLevel level, const char* event, const NevrOwner* owner, const char* other,
            const char* target, const std::string& detail);
  NevrStatus Fail(NevrOwner* owner, NevrStatus status, std::string why);
  void RunChain(const HookPoint::Chain& chain, NevrHookCall& call);
  size_t DropOwnerLocked(NevrOwner* owner);

  LogSink sink_;
  mutable std::mutex mu_;  // guards owners_, overrides_, hooks_ and chain swaps
  std::vector<std::unique_ptr<NevrOwner>> owners_;
  std::map<std::string, OverrideEntry> overrides_;
  std::map<std::string, NevrValueType> override_points_;
  std::map<std::string, std::unique_ptr<HookPoint>> hooks_;
};

}  // namespace nevr_script
