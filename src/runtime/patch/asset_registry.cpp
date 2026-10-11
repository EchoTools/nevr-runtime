// asset_registry.cpp — see asset_registry.h. PURE: std + nlohmann-json only. Compiled SKIP_PRECOMPILE_HEADERS
// with NOMINMAX defined first, for the same reason as ext/plugin_manifest.cpp: the core PCH's windows.h defines
// the min/max macros nlohmann-json trips over.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "runtime/patch/asset_registry.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <utility>

#include <nlohmann/json.hpp>

namespace nevr::assets {

namespace {

std::string Hex64(uint64_t v) {
    char buf[19];
    std::snprintf(buf, sizeof(buf), "0x%016" PRIx64, v);
    return buf;
}

}  // namespace

const char* ToString(RegisterStatus status) {
    switch (status) {
        case RegisterStatus::Ok: return "ok";
        case RegisterStatus::BadArgument: return "bad_argument";
        case RegisterStatus::Duplicate: return "duplicate";
        case RegisterStatus::AlreadyLoaded: return "already_loaded";
    }
    return "unknown";
}

RegisterStatus Registry::Register(uint64_t type, uint64_t name, const void* data, uint64_t size,
                                  const char* label) {
    if (type == 0 || name == 0 || data == nullptr || size == 0 || size > kMaxAssetBytes) {
        return RegisterStatus::BadArgument;
    }
    // Ask the engine outside the lock: its predicate takes the resource manager's own lock.
    const LoadedProbe probe = isLoaded_.load(std::memory_order_acquire);
    if (probe && probe(type, name)) return RegisterStatus::AlreadyLoaded;

    std::lock_guard<std::mutex> lock(mutex_);
    const bool taken = std::any_of(assets_.begin(), assets_.end(),
                                   [&](const Asset& a) { return a.type == type && a.name == name; });
    if (taken) return RegisterStatus::Duplicate;

    Asset asset;
    asset.type = type;
    asset.name = name;
    asset.data = data;
    asset.size = size;
    asset.label = label ? label : "";
    assets_.push_back(std::move(asset));
    count_.store(assets_.size(), std::memory_order_release);
    return RegisterStatus::Ok;
}

size_t Registry::Deregister(const void* start, const void* end, std::vector<Asset>* removed) {
    const auto lo = reinterpret_cast<uintptr_t>(start);
    const auto hi = reinterpret_cast<uintptr_t>(end);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto it = assets_.begin(); it != assets_.end();) {
        const auto p = reinterpret_cast<uintptr_t>(it->data);
        if (p >= lo && p < hi) {
            if (removed) removed->push_back(*it);
            it = assets_.erase(it);
            ++count;
        } else {
            ++it;
        }
    }
    count_.store(assets_.size(), std::memory_order_release);
    return count;
}

bool Registry::Find(uint64_t type, uint64_t name, Asset* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Asset& a : assets_) {
        if (a.type == type && a.name == name) {
            if (out) *out = a;
            return true;
        }
    }
    return false;
}

size_t Registry::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assets_.size();
}

std::vector<Asset> Registry::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return assets_;
}

Registry& Global() {
    static Registry registry;
    return registry;
}

std::string Registry::ReportJson() const {
    nlohmann::json out = nlohmann::json::array();
    for (const Asset& a : Snapshot()) {
        nlohmann::json j;
        j["type"] = Hex64(a.type);
        j["name"] = Hex64(a.name);
        j["label"] = a.label;
        j["bytes"] = a.size;
        out.push_back(std::move(j));
    }
    // `replace`: a label comes from plugin code; dump() throws on invalid UTF-8 by default and the login must
    // not fail over a label.
    return out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace nevr::assets
