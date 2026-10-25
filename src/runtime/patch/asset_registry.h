#pragma once

// The plugin asset registry: resources a plugin carries inside its own image (a level and the resources it is
// made of), keyed the way the engine's resource manager keys everything, by (type symbol, name symbol).
//
// Pure bookkeeping: std + nlohmann-json only, no windows.h, no Log, no engine pointers, so it links into a test
// on its own. The hooks that serve these bytes to the resource manager live in patch/plugin_assets.cpp, which
// installs the engine's own "already loaded" predicate as the registry's LoadedProbe.
//
// Lifetime rule, same as the resource override registry: the pointer handed in must stay valid until the plugin
// deregisters it (the plugin's image is the owner), and Deregister takes the plugin's address range.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace nevr::assets {

enum class RegisterStatus : int {
    Ok = 0,
    BadArgument = 1,    // null/empty data, a zero type or name, or a size the engine's u32 sizes cannot carry
    Duplicate = 2,      // this (type, name) is already registered
    AlreadyLoaded = 3,  // the engine has already loaded this (type, name); too late to change what it reads
};

const char* ToString(RegisterStatus status);

// Largest asset a registration may carry: the engine's resource sizes are u32 (manifest entry +0x18).
constexpr uint64_t kMaxAssetBytes = 0xffffffffULL;

struct Asset {
    uint64_t type = 0;
    uint64_t name = 0;
    const void* data = nullptr;  // not owned: the plugin image
    uint64_t size = 0;
    std::string label;           // for logs and the login report
};

class Registry {
public:
    // Asks the engine whether (type, name) has been loaded. nullptr means "never", for tests and for a host
    // that has not located the engine's resource manager.
    using LoadedProbe = bool (*)(uint64_t type, uint64_t name);

    explicit Registry(LoadedProbe isLoaded = nullptr) : isLoaded_(isLoaded) {}

    // Swap the engine predicate (the process-wide registry exists before the engine's resource manager does).
    void SetLoadedProbe(LoadedProbe isLoaded) { isLoaded_.store(isLoaded, std::memory_order_release); }

    // True when nothing is registered. Lock-free: the engine's submit hook asks this on every resource load.
    bool Empty() const { return count_.load(std::memory_order_acquire) == 0; }

    RegisterStatus Register(uint64_t type, uint64_t name, const void* data, uint64_t size, const char* label);

    // Remove every asset whose data pointer lies in [start, end); returns how many went. Removed assets are
    // appended to *removed when given, so the caller can log each by label.
    size_t Deregister(const void* start, const void* end, std::vector<Asset>* removed = nullptr);

    bool Find(uint64_t type, uint64_t name, Asset* out) const;
    size_t Count() const;
    std::vector<Asset> Snapshot() const;

    // The `nevr_assets` array the login carries beside `nevr_plugins`:
    //   [{"type":"0x...","name":"0x...","label":"...","bytes":N}, ...]  in registration order, "[]" when empty.
    std::string ReportJson() const;

private:
    std::atomic<LoadedProbe> isLoaded_;
    std::atomic<size_t> count_{0};
    mutable std::mutex mutex_;
    std::vector<Asset> assets_;
};

// The registry this process's plugins register into, read by the engine hooks and the login.
Registry& Global();

}  // namespace nevr::assets
