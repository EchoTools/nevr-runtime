#include "scripting/host_registry.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace nevr_script {
namespace {

// The owner whose callback this thread is running, so Quiesce can refuse to
// wait on itself.
thread_local const NevrOwner* t_running_owner = nullptr;

// last_error is per thread, like errno: two game threads failing calls for one
// owner must neither race on one string nor read each other's reason.
thread_local std::unordered_map<const NevrOwner*, std::string> t_last_error;

const char* StatusName(NevrStatus status) {
  switch (status) {
    case NEVR_OK: return "NEVR_OK";
    case NEVR_ERR_INVALID_ARG: return "NEVR_ERR_INVALID_ARG";
    case NEVR_ERR_CONFLICT: return "NEVR_ERR_CONFLICT";
    case NEVR_ERR_UNKNOWN_HOOK: return "NEVR_ERR_UNKNOWN_HOOK";
    case NEVR_ERR_UNKNOWN_FIELD: return "NEVR_ERR_UNKNOWN_FIELD";
    case NEVR_ERR_TYPE_MISMATCH: return "NEVR_ERR_TYPE_MISMATCH";
    case NEVR_ERR_READ_ONLY: return "NEVR_ERR_READ_ONLY";
    case NEVR_ERR_NOT_FOUND: return "NEVR_ERR_NOT_FOUND";
    case NEVR_ERR_DISABLED: return "NEVR_ERR_DISABLED";
    case NEVR_ERR_UNDECLARED: return "NEVR_ERR_UNDECLARED";
    case NEVR_ERR_UNKNOWN_KEY: return "NEVR_ERR_UNKNOWN_KEY";
    default: return "NEVR_ERR_UNKNOWN_STATUS";
  }
}

bool ValidValue(const NevrValue* v) {
  if (!v) return false;
  switch (v->type) {
    case NEVR_VALUE_BOOL:
    case NEVR_VALUE_INT:
    case NEVR_VALUE_FLOAT: return true;
    case NEVR_VALUE_STRING: return v->as.s != nullptr;
    default: return false;
  }
}

const char* TypeName(NevrValueType type) {
  switch (type) {
    case NEVR_VALUE_BOOL: return "bool";
    case NEVR_VALUE_INT: return "int";
    case NEVR_VALUE_FLOAT: return "float";
    case NEVR_VALUE_STRING: return "string";
    default: return "none";
  }
}

// Converts `in` to the override point's type: an integral FLOAT within ±2^53
// to INT, an INT to FLOAT. False when the types can't meet.
bool CoerceTo(NevrValueType type, const NevrValue& in, NevrValue* out) {
  *out = in;
  if (in.type == type) return true;
  if (type == NEVR_VALUE_FLOAT && in.type == NEVR_VALUE_INT) {
    out->type = NEVR_VALUE_FLOAT;
    out->as.f = static_cast<double>(in.as.i);
    return true;
  }
  if (type == NEVR_VALUE_INT && in.type == NEVR_VALUE_FLOAT) {
    const double f = in.as.f;
    // (-2^53, 2^53): past it a double no longer holds every integer, so the
    // value may already have been rounded on its way here (a script literal
    // 2^53+1 arrives as 2^53). Refuse rather than store a nearby integer.
    if (!(f > -9007199254740992.0 && f < 9007199254740992.0)) return false;
    const int64_t i = static_cast<int64_t>(f);
    if (static_cast<double>(i) != f) return false;
    out->type = NEVR_VALUE_INT;
    out->as.i = i;
    return true;
  }
  return false;
}

std::string Describe(const NevrValue& v) {
  switch (v.type) {
    case NEVR_VALUE_BOOL: return v.as.b ? "true" : "false";
    case NEVR_VALUE_INT: return std::to_string(v.as.i);
    case NEVR_VALUE_FLOAT: return std::to_string(v.as.f);
    case NEVR_VALUE_STRING: return std::string("\"") + v.as.s + "\"";
    default: return "none";
  }
}

// ---- the C ABI table: every entry dispatches through the owner's or call's registry ----

NevrStatus ApiOverrideSet(NevrOwner* owner, const char* key, const NevrValue* value) {
  if (!owner) return NEVR_ERR_INVALID_ARG;
  return owner->registry->OverrideSet(owner, key, value);
}

NevrStatus ApiOverrideGet(NevrOwner* owner, const char* key, NevrValue* out) {
  if (!owner) return NEVR_ERR_INVALID_ARG;
  return owner->registry->OverrideGet(owner, key, out);
}

NevrStatus ApiHookAdd(NevrOwner* owner, const char* hook, NevrHookPhase phase, NevrHookFn fn,
                      void* user) {
  if (!owner) return NEVR_ERR_INVALID_ARG;
  return owner->registry->HookAdd(owner, hook, phase, fn, user);
}

NevrStatus ApiCallGet(const NevrHookCall* call, const char* field, NevrValue* out) {
  if (!call || !field || !out) return NEVR_ERR_INVALID_ARG;
  const int index = call->hook->FieldIndex(field);
  if (index < 0) return NEVR_ERR_UNKNOWN_FIELD;
  *out = call->fields[index];
  return NEVR_OK;
}

NevrStatus ApiCallSet(NevrHookCall* call, const char* field, const NevrValue* value) {
  if (!call || !field || !ValidValue(value)) return NEVR_ERR_INVALID_ARG;
  const int index = call->hook->FieldIndex(field);
  if (index < 0) return NEVR_ERR_UNKNOWN_FIELD;
  const FieldSpec& spec = call->hook->Fields()[static_cast<size_t>(index)];
  NevrValue typed{};
  if (!CoerceTo(spec.type, *value, &typed)) return NEVR_ERR_TYPE_MISMATCH;
  const bool writable = call->phase == NEVR_HOOK_PRE ? spec.pre_writable : spec.post_writable;
  if (!writable) return NEVR_ERR_READ_ONLY;
  value = &typed;
  NevrValue& slot = call->fields[index];
  if (value->type == NEVR_VALUE_STRING) {
    if (!call->strings) call->strings.reset(new std::string[call->hook->Fields().size()]);
    std::string& text = call->strings[static_cast<size_t>(index)];
    text = value->as.s;
    slot = *value;
    slot.as.s = text.c_str();
  } else {
    slot = *value;
  }
  return NEVR_OK;
}

const char* ApiCallHookName(const NevrHookCall* call) {
  return call ? call->hook->Name().c_str() : "";
}

NevrHookResult ApiCallFail(NevrHookCall* call, const char* reason) {
  if (call) call->fail_reason = reason ? reason : "";
  return NEVR_HOOK_FAILED;
}

void ApiLog(NevrOwner* owner, NevrLogLevel level, const char* message) {
  if (owner) owner->registry->OwnerLog(owner, level, message ? message : "");
}

const char* ApiLastError(NevrOwner* owner) {
  if (!owner) return "";
  const auto it = t_last_error.find(owner);
  return it == t_last_error.end() ? "" : it->second.c_str();
}

const NevrHostApi kApi = {
    sizeof(NevrHostApi), NEVR_HOST_API_VERSION,
    ApiOverrideSet, ApiOverrideGet,
    ApiHookAdd, ApiCallGet, ApiCallSet, ApiCallHookName, ApiCallFail,
    ApiLog, StatusName, ApiLastError,
};

}  // namespace

bool DeclarationCoversKey(const Declaration& declaration, const std::string& key) {
  for (const std::string& pattern : declaration.overrides) {
    if (pattern == key) return true;
    const size_t n = pattern.size();
    if (n >= 2 && pattern.compare(n - 2, 2, ".*") == 0 && key.size() > n - 1 &&
        key.compare(0, n - 1, pattern, 0, n - 1) == 0) {
      return true;
    }
  }
  return false;
}

int HookPoint::FieldIndex(const char* field) const {
  for (size_t i = 0; i < fields_.size(); ++i) {
    if (fields_[i].name == field) return static_cast<int>(i);
  }
  return -1;
}

Registry::Registry(LogSink sink) : sink_(std::move(sink)) {}
Registry::~Registry() = default;

const NevrHostApi* Registry::Api() const { return &kApi; }

void Registry::Emit(NevrLogLevel level, const char* event, const NevrOwner* owner,
                    const char* other, const char* target, const std::string& detail) {
  if (!sink_) return;
  const LogRecord record{level, event, owner ? owner->name.c_str() : "", other ? other : "",
                         target ? target : "", detail.c_str()};
  sink_(record);
}

void Registry::Record(NevrLogLevel level, const char* event, const NevrOwner* owner, const char* target,
                      const std::string& detail) {
  Emit(level, event, owner, nullptr, target, detail);
}

NevrStatus Registry::RefusedDisabled(NevrOwner* owner, const char* what, const char* name) {
  Emit(NEVR_LOG_WARNING, "refused_disabled", owner, nullptr, name,
       std::string(what) + " refused: " + owner->name + " is disabled");
  return Fail(owner, NEVR_ERR_DISABLED, owner->name + " is disabled");
}

NevrStatus Registry::Undeclared(NevrOwner* owner, const char* what, const char* name) {
  Emit(NEVR_LOG_ERROR, "undeclared", owner, nullptr, name,
       std::string(what) + " is not in the owner's manifest");
  return Fail(owner, NEVR_ERR_UNDECLARED,
              std::string(what) + " " + name + " is not declared in " + owner->name + "'s manifest");
}

void Registry::Declare(NevrOwner* owner, Declaration declaration) {
  if (!owner) return;
  std::lock_guard<std::mutex> lock(mu_);
  owner->declaration = std::move(declaration);
  owner->declared = true;
}

NevrOwner* Registry::FindOwner(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (const auto& owner : owners_) {
    if (owner->name == name) return owner.get();
  }
  return nullptr;
}

NevrStatus Registry::Fail(NevrOwner* owner, NevrStatus status, std::string why) {
  t_last_error[owner] = std::move(why);
  return status;
}

NevrOwner* Registry::OpenOwner(const std::string& name) {
  std::lock_guard<std::mutex> lock(mu_);
  auto owner = std::make_unique<NevrOwner>();
  owner->registry = this;
  owner->name = name;
  owner->order = static_cast<uint32_t>(owners_.size());
  owners_.push_back(std::move(owner));
  NevrOwner* opened = owners_.back().get();
  Emit(NEVR_LOG_DEBUG, "owner_opened", opened, nullptr, nullptr,
       "order " + std::to_string(opened->order));
  return opened;
}

size_t Registry::DropOwnerLocked(NevrOwner* owner) {
  size_t dropped = 0;
  for (auto it = overrides_.begin(); it != overrides_.end();) {
    if (it->second.owner == owner) {
      it = overrides_.erase(it);
      ++dropped;
    } else {
      ++it;
    }
  }
  for (auto& entry : hooks_) {
    HookPoint& hook = *entry.second;
    for (std::shared_ptr<const HookPoint::Chain>* chain : {&hook.pre_, &hook.post_}) {
      auto next = std::make_shared<HookPoint::Chain>(**chain);
      const auto before = next->size();
      next->erase(std::remove_if(next->begin(), next->end(),
                                 [owner](const HookPoint::Callback& c) { return c.owner == owner; }),
                  next->end());
      if (next->size() != before) {
        dropped += before - next->size();
        std::atomic_store(chain, std::shared_ptr<const HookPoint::Chain>(std::move(next)));
      }
    }
  }
  return dropped;
}

void Registry::DisableOwner(NevrOwner* owner, const std::string& reason) {
  if (!owner) return;
  size_t dropped = 0;
  {
    // Under mu_, so a hook_add or override_set already past its own check
    // (which also runs under mu_) has finished and its entry is dropped here.
    std::lock_guard<std::mutex> lock(mu_);
    if (owner->disabled.exchange(true)) {
      // Already disabled (a reload in progress, an earlier breach): the breach
      // is still recorded.
      Emit(NEVR_LOG_ERROR, "owner_disabled", owner, nullptr, nullptr, "already disabled; " + reason);
      return;
    }
    dropped = DropOwnerLocked(owner);
  }
  Emit(NEVR_LOG_ERROR, "owner_disabled", owner, nullptr, nullptr,
       reason + " (" + std::to_string(dropped) + " override(s) and callback(s) removed)");
}

bool Registry::Quiesce(NevrOwner* owner, std::chrono::milliseconds timeout) {
  if (!owner) return false;
  if (t_running_owner == owner) {
    Emit(NEVR_LOG_ERROR, "quiesce_failed", owner, nullptr, nullptr,
         "called from one of the owner's own callbacks; it would wait on itself");
    return false;
  }
  // The ReaderGate handshake (core/reader_gate.h): bump, then wait; RunChain
  // enters the gate, then reads the generation. All seq_cst.
  owner->generation.fetch_add(1, std::memory_order_seq_cst);
  if (owner->gate.WaitIdle(timeout)) return true;
  Emit(NEVR_LOG_ERROR, "quiesce_failed", owner, nullptr, nullptr,
       std::to_string(owner->gate.Readers()) + " callback(s) still running after " +
           std::to_string(timeout.count()) + " ms; what they use must not be freed");
  return false;
}

bool Registry::ResetOwner(NevrOwner* owner) {
  if (!owner) return false;
  size_t dropped = 0;
  {
    // Disabled first, under mu_: a callback still running while Quiesce waits
    // cannot add anything new (hook_add and override_set check under mu_ and
    // fail with NEVR_ERR_DISABLED), so nothing outlives the reset.
    std::lock_guard<std::mutex> lock(mu_);
    owner->disabled.store(true);
    dropped = DropOwnerLocked(owner);
  }
  if (!Quiesce(owner)) return false;  // stays disabled
  {
    std::lock_guard<std::mutex> lock(mu_);
    owner->disabled.store(false);
  }
  Emit(NEVR_LOG_INFO, "owner_reset", owner, nullptr, nullptr,
       std::to_string(dropped) + " override(s) and callback(s) removed");
  return true;
}

bool Registry::RegisterOverridePoint(const std::string& key, NevrValueType type) {
  if (key.empty() || type < NEVR_VALUE_BOOL || type > NEVR_VALUE_STRING) return false;
  std::lock_guard<std::mutex> lock(mu_);
  return override_points_.emplace(key, type).second;
}

HookPoint* Registry::RegisterHookPoint(const std::string& name, std::vector<FieldSpec> fields) {
  std::lock_guard<std::mutex> lock(mu_);
  if (name.empty() || hooks_.count(name)) return nullptr;
  std::unique_ptr<HookPoint> hook(new HookPoint(name, std::move(fields)));
  HookPoint* raw = hook.get();
  hooks_.emplace(name, std::move(hook));
  return raw;
}

HookPoint* Registry::FindHookPoint(const char* name) const {
  if (!name) return nullptr;
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = hooks_.find(name);
  return it == hooks_.end() ? nullptr : it->second.get();
}

std::vector<std::pair<std::string, NevrValueType>> Registry::OverridePoints() const {
  std::lock_guard<std::mutex> lock(mu_);
  return {override_points_.begin(), override_points_.end()};
}

std::vector<const HookPoint*> Registry::HookPoints() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<const HookPoint*> points;
  for (const auto& entry : hooks_) points.push_back(entry.second.get());
  return points;
}

NevrStatus Registry::OverrideSet(NevrOwner* owner, const char* key, const NevrValue* value) {
  if (!key || !*key || !ValidValue(value)) {
    return Fail(owner, NEVR_ERR_INVALID_ARG, "override needs a non-empty key and a typed value");
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (owner->disabled.load()) return RefusedDisabled(owner, "override", key);
  if (owner->declared && !DeclarationCoversKey(owner->declaration, key)) {
    return Undeclared(owner, "override", key);
  }
  const auto point = override_points_.find(key);
  if (point == override_points_.end()) {
    Emit(NEVR_LOG_WARNING, "override_unknown", owner, nullptr, key,
         "no override point by this name on this build; the override is not applied");
    return Fail(owner, NEVR_ERR_UNKNOWN_KEY, std::string("no override point named ") + key);
  }
  NevrValue typed{};
  if (!CoerceTo(point->second, *value, &typed)) {
    return Fail(owner, NEVR_ERR_TYPE_MISMATCH,
                std::string("override ") + key + " is " + TypeName(point->second) + ", not " +
                    TypeName(value->type) + " " + Describe(*value));
  }
  value = &typed;
  auto it = overrides_.find(key);
  if (it != overrides_.end() && it->second.owner != owner) {
    const NevrOwner* holder = it->second.owner;
    Emit(NEVR_LOG_ERROR, "override_conflict", owner, holder->name.c_str(), key,
         owner->name + " tried to set " + Describe(*value) + "; " + holder->name +
             " set it first and keeps it (" + Describe(it->second.value) + ")");
    return Fail(owner, NEVR_ERR_CONFLICT,
                std::string("override ") + key + " is already set by " + holder->name);
  }
  OverrideEntry& entry = overrides_[key];
  entry.owner = owner;
  entry.value = *value;
  if (value->type == NEVR_VALUE_STRING) {
    entry.text = value->as.s;
    entry.value.as.s = entry.text.c_str();
  }
  Emit(NEVR_LOG_INFO, "override_set", owner, nullptr, key, Describe(entry.value));
  return NEVR_OK;
}

NevrStatus Registry::OverrideGet(NevrOwner* owner, const char* key, NevrValue* out) {
  if (!key || !out) return Fail(owner, NEVR_ERR_INVALID_ARG, "override_get needs a key and an out value");
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = overrides_.find(key);
  if (it == overrides_.end()) return Fail(owner, NEVR_ERR_NOT_FOUND, std::string("no override for ") + key);
  *out = it->second.value;
  return NEVR_OK;
}

NevrStatus Registry::HookAdd(NevrOwner* owner, const char* hook, NevrHookPhase phase, NevrHookFn fn,
                             void* user) {
  if (!hook || !*hook || !fn || (phase != NEVR_HOOK_PRE && phase != NEVR_HOOK_POST)) {
    return Fail(owner, NEVR_ERR_INVALID_ARG, "hook_add needs a hook name, a phase and a function");
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (owner->disabled.load()) return RefusedDisabled(owner, "hook", hook);
  if (owner->declared && std::find(owner->declaration.hooks.begin(), owner->declaration.hooks.end(),
                                   hook) == owner->declaration.hooks.end()) {
    return Undeclared(owner, "hook", hook);
  }
  const auto it = hooks_.find(hook);
  if (it == hooks_.end()) {
    Emit(NEVR_LOG_WARNING, "hook_unknown", owner, nullptr, hook,
         "no hook point by this name on this build; the owner's feature on it stays off");
    return Fail(owner, NEVR_ERR_UNKNOWN_HOOK, std::string("no hook point named ") + hook);
  }
  HookPoint& point = *it->second;
  std::shared_ptr<const HookPoint::Chain>& chain = phase == NEVR_HOOK_PRE ? point.pre_ : point.post_;
  auto next = std::make_shared<HookPoint::Chain>(*chain);
  // Chained by owner order (plugins: order), then by registration within an owner.
  const auto pos = std::upper_bound(next->begin(), next->end(), owner->order,
                                    [](uint32_t order, const HookPoint::Callback& c) {
                                      return order < c.owner->order;
                                    });
  next->insert(pos, HookPoint::Callback{owner, fn, user, owner->generation.load(std::memory_order_seq_cst)});
  std::atomic_store(&chain, std::shared_ptr<const HookPoint::Chain>(std::move(next)));
  Emit(NEVR_LOG_INFO, "hook_added", owner, nullptr, hook, phase == NEVR_HOOK_PRE ? "pre" : "post");
  return NEVR_OK;
}

void Registry::OwnerLog(NevrOwner* owner, NevrLogLevel level, const char* message) {
  Emit(level, "owner_log", owner, nullptr, nullptr, message);
}

void Registry::RunChain(const HookPoint::Chain& chain, NevrHookCall& call) {
  for (const HookPoint::Callback& cb : chain) {
    if (cb.owner->disabled.load(std::memory_order_relaxed)) continue;
    nevr::ReaderGate::Scope inside(cb.owner->gate);
    if (cb.owner->generation.load(std::memory_order_seq_cst) != cb.generation) continue;  // retired
    call.current = cb.owner;
    call.fail_reason.clear();
    const NevrOwner* outer = t_running_owner;
    t_running_owner = cb.owner;
    const NevrHookResult result = cb.fn(&call, cb.user);
    t_running_owner = outer;
    if (result == NEVR_HOOK_FAILED) {
      Emit(NEVR_LOG_ERROR, "callback_failed", cb.owner, nullptr, call.hook->Name().c_str(),
           std::string(call.phase == NEVR_HOOK_PRE ? "pre: " : "post: ") +
               (call.fail_reason.empty() ? "no reason given" : call.fail_reason));
    } else if (result == NEVR_HOOK_SKIP_ORIGINAL && call.phase == NEVR_HOOK_PRE) {
      call.skip_original = true;
    }
  }
  call.current = nullptr;
}

void Registry::Invoke(const HookPoint* hook, NevrValue* fields, OriginalFn original, void* ctx) {
  const std::shared_ptr<const HookPoint::Chain> pre = std::atomic_load(&hook->pre_);
  const std::shared_ptr<const HookPoint::Chain> post = std::atomic_load(&hook->post_);
  if (pre->empty() && post->empty()) {
    original(fields, ctx);
    return;
  }
  NevrHookCall call{hook, fields, nullptr, NEVR_HOOK_PRE, nullptr, false, {}};
  RunChain(*pre, call);
  if (!call.skip_original) original(fields, ctx);
  call.phase = NEVR_HOOK_POST;
  RunChain(*post, call);
}

}  // namespace nevr_script
