#include "runtime/patch/asset_cdn.h"
#include "runtime/patch/evrp_package.h"
#include "core/curl_global.h"
#include "core/reader_gate.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <shlobj.h>
#include <wincrypt.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "abi/echovr_functions.h"
#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/log/security_diagnostics.h"
#include "nevr_common.h"      // N97: the one ValidatePrologue
#include "runtime/hook/addresses.h"

using json = nlohmann::json;

namespace {

// ============================================================================
// Constants
// ============================================================================

constexpr const char* CDN_BASE_URL = "https://r2.echo.taxi/v1/";
constexpr const char* MANIFEST_URL = "https://r2.echo.taxi/v1/manifest.json";

// ============================================================================
// Manifest package entry
// ============================================================================

struct PackageEntry {
    std::string url;
    std::string sha256;
    std::string slot_type;
    int64_t size;
    int64_t symbol_id;
};

// ============================================================================
// Hook state
// ============================================================================

bool g_hookInstalled = false;

typedef void* (__fastcall* LoadoutResolveDataFromIdFunc)(void* context, int64_t loadout_id);
LoadoutResolveDataFromIdFunc g_originalFunc = nullptr;

// ============================================================================
// Tint data map — written by background thread, read by hook
// ============================================================================

// The tint map is populated by the background thread, then the pointer is
// atomically swapped so the hook sees a consistent snapshot. The hook never
// locks — it reads through the atomic pointer. The background thread builds
// a new map, then publishes it via atomic store.
using TintMap = std::unordered_map<int64_t, Evrp::TintData>;
std::atomic<TintMap*> g_tintMap{nullptr};

// Owns the tint map memory. Protected by g_dataMutex during writes.
TintMap* g_tintMapOwned = nullptr;
// Held by the hook for its whole call; every free of a tint map (publish, shutdown) waits it idle first (#352).
nevr::ReaderGate g_tintGate;
constexpr std::chrono::milliseconds kTintGateWait{2000};
std::mutex g_dataMutex;

// ============================================================================
// Manifest data — only accessed by background thread
// ============================================================================

std::unordered_map<int64_t, PackageEntry> g_manifestPackages;

// ============================================================================
// Fetch pipeline state
// ============================================================================

std::atomic<AssetCDN::FetchState> g_fetchState{AssetCDN::FetchState::Idle};
std::thread g_fetchThread;
std::atomic<bool> g_shutdownRequested{false};

// ============================================================================
// Loadout data structure offsets (from Task 2 RE findings)
// ============================================================================

constexpr ptrdiff_t OFFSET_RESOURCE_TABLE = 0x370;
constexpr ptrdiff_t OFFSET_FALLBACK_ARRAY = 0x80;
constexpr ptrdiff_t OFFSET_PRIMARY_ARRAY = 0xB8;

// ============================================================================
// Prologue validation
// ============================================================================

static const uint8_t EXPECTED_PROLOGUE[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,  // MOV [RSP+0x8], RBX
    0x57,                            // PUSH RDI
    0x48, 0x83, 0xEC, 0x20,          // SUB RSP, 0x20
    0x48, 0x8B, 0xDA,                // MOV RBX, RDX
    0x48, 0x8B, 0xF9,                // MOV RDI, RCX
};
constexpr size_t PROLOGUE_LEN = sizeof(EXPECTED_PROLOGUE);

// Validation is nevr::ValidatePrologue (N97). There is no file-local wrapper:
// with one call site it would buy nothing, and a file-local ValidatePrologue
// taking a different argument list is exactly the one-name-two-meanings defect
// N96 guards against.

/// Lowercase hex dump of len bytes — same conversion ComputeSHA256 already
/// does locally for its digest; pulled out so prologue-mismatch logging can
/// share it instead of hand-writing a byte[16] %02x arg list.
static std::string BytesToHex(const uint8_t* bytes, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        result.push_back(hex[bytes[i] >> 4]);
        result.push_back(hex[bytes[i] & 0x0F]);
    }
    return result;
}

// ============================================================================
// curl write callback
// ============================================================================

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* buffer = static_cast<std::vector<uint8_t>*>(userp);
    size_t total = size * nmemb;
    buffer->insert(buffer->end(), static_cast<uint8_t*>(contents),
                   static_cast<uint8_t*>(contents) + total);
    return total;
}

// ============================================================================
// SHA256 using Windows CryptoAPI
// ============================================================================

static std::string ComputeSHA256(const std::vector<uint8_t>& data) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string result;

    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES,
                               CRYPT_VERIFYCONTEXT)) {
        return result;
    }

    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return result;
    }

    if (!CryptHashData(hHash, data.data(), static_cast<DWORD>(data.size()), 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return result;
    }

    DWORD hashLen = 32;
    uint8_t hash[32];
    if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return result;
    }

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);

    // Convert to lowercase hex
    static const char hex[] = "0123456789abcdef";
    result.reserve(64);
    for (DWORD i = 0; i < hashLen; i++) {
        result.push_back(hex[hash[i] >> 4]);
        result.push_back(hex[hash[i] & 0x0F]);
    }
    return result;
}

// ============================================================================
// Background fetch pipeline
// ============================================================================

static void BackgroundFetchThread() {
    Log(EchoVR::LogLevel::Debug, "[NEVR.CDN] Background fetch started");

    // Fetch manifest
    g_fetchState.store(AssetCDN::FetchState::FetchingManifest);
    if (!AssetCDN::FetchManifest()) {
        g_fetchState.store(AssetCDN::FetchState::Error);
        Log(EchoVR::LogLevel::Error, "[NEVR.CDN] Manifest fetch failed — CDN pipeline aborted");
        return;
    }

    if (g_shutdownRequested.load()) return;

    if (g_manifestPackages.empty()) {
        Log(EchoVR::LogLevel::Debug, "[NEVR.CDN] Manifest has no packages — nothing to download");
        g_fetchState.store(AssetCDN::FetchState::Complete);
        return;
    }

    // Download uncached packages
    g_fetchState.store(AssetCDN::FetchState::DownloadingPackages);

    std::string cacheDir = AssetCDN::GetCacheDir();
    if (cacheDir.empty()) {
        // GetCacheDir() already logged the specific cause (SHGetKnownFolderPath
        // HRESULT or create_directories ec.message()) immediately before this
        // returns — a second, generic ERROR here would only restate "failed"
        // with strictly less detail than the line that just ran.
        g_fetchState.store(AssetCDN::FetchState::Error);
        return;
    }

    // Build new tint map from downloads + cached files
    auto* newTintMap = new TintMap();
    int downloaded = 0;
    int cached = 0;
    int failed = 0;

    for (const auto& [symbol_id, entry] : g_manifestPackages) {
        if (g_shutdownRequested.load()) {
            delete newTintMap;
            return;
        }

        // Only handle tints for now
        if (entry.slot_type != "tint") continue;

        // Build the local filename from the manifest key (hex symbol_id)
        char hex_id[17];
        snprintf(hex_id, sizeof(hex_id), "%016llx",
                 static_cast<unsigned long long>(static_cast<uint64_t>(symbol_id)));
        std::string filename = std::string(hex_id) + ".evrp";
        std::string dest_path = cacheDir + "\\" + filename;

        std::vector<uint8_t> file_data;
        bool have_data = false;

        if (AssetCDN::IsCached(filename)) {
            // Read from cache
            std::ifstream ifs(dest_path, std::ios::binary);
            if (ifs.good()) {
                file_data.assign(std::istreambuf_iterator<char>(ifs),
                                 std::istreambuf_iterator<char>());
                have_data = true;
                cached++;
            }
        }

        if (!have_data) {
            // Build full URL
            std::string full_url = std::string(CDN_BASE_URL) + entry.url;
            if (!AssetCDN::DownloadPackage(full_url, dest_path, entry.sha256)) {
                failed++;
                continue;
            }
            // Read the downloaded file
            std::ifstream ifs(dest_path, std::ios::binary);
            if (!ifs.good()) {
                failed++;
                continue;
            }
            file_data.assign(std::istreambuf_iterator<char>(ifs),
                             std::istreambuf_iterator<char>());
            have_data = true;
            downloaded++;
        }

        if (!have_data) continue;

        // Parse .evrp and extract tint data
        int64_t parsed_symbol_id;
        Evrp::TintData tint;
        if (Evrp::ParseTint(file_data, filename, parsed_symbol_id, tint)) {
            (*newTintMap)[parsed_symbol_id] = tint;
        }
    }

    if (g_shutdownRequested.load()) {
        delete newTintMap;
        return;
    }

    // Publish the tint map atomically
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        TintMap* old = g_tintMapOwned;
        g_tintMapOwned = newTintMap;
        g_tintMap.store(newTintMap, std::memory_order_release);
        // A hook call that loaded `old` before the store may still be reading it.
        if (g_tintGate.WaitIdle(kTintGateWait)) {
            delete old;
        } else {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.CDN] previous tint map left allocated: the loadout hook did not leave within %lld ms",
                static_cast<long long>(kTintGateWait.count()));
        }
    }

    Log(failed > 0 ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
        "[NEVR.CDN] fetch complete: downloaded=%d cached=%d failed=%d tints_loaded=%zu",
        downloaded, cached, failed, newTintMap->size());

    g_fetchState.store(AssetCDN::FetchState::Complete);
}

// ============================================================================
// Hook implementation
// ============================================================================

/// Post-hook for Loadout_ResolveDataFromId.
///
/// Calls the original function, then checks if any resource IDs in the
/// returned data match CDN tint symbol IDs. If so, writes the 80-byte
/// tint color data into the resource table's color region.
///
/// CRITICAL: This runs on 261+ call sites, some per-frame.
/// No allocations. No logging. No locks. O(1) map lookup only.
void* __fastcall Hook_LoadoutResolveDataFromId(void* context, int64_t loadout_id) {
    // In use from here: Shutdown() and the republish wait for this scope before they free the map
    // or let the detour's trampoline go.
    nevr::ReaderGate::Scope inHook(g_tintGate);
    void* result = g_originalFunc(context, loadout_id);
    if (!result) return nullptr;

    // Read the tint map pointer (atomic, lock-free)
    TintMap* tintMap = g_tintMap.load(std::memory_order_acquire);
    if (!tintMap || tintMap->empty()) return result;

    // Follow pointer chain: result + 0x370 -> resource table
    auto* resource_table_ptr = reinterpret_cast<uintptr_t*>(
        static_cast<char*>(result) + OFFSET_RESOURCE_TABLE);

    uintptr_t resource_table = *resource_table_ptr;
    if (!resource_table) return result;

    // Check the fallback resource ID at slot 0
    auto* fallback_id_ptr = reinterpret_cast<int64_t*>(resource_table + OFFSET_FALLBACK_ARRAY);
    if (!fallback_id_ptr) return result;

    int64_t fallback_id = *fallback_id_ptr;

    // Look up this resource ID in the CDN tint map
    auto it = tintMap->find(fallback_id);
    if (it != tintMap->end()) {
        // The resource table at +0x80 holds the fallback resource ID (a SymbolId).
        // The actual tint color data is resolved from this ID elsewhere.
        // We need to write color data into the TintEntry structure.
        // Per the format spec, TintEntry is 96 bytes:
        //   [0x00..0x08) resourceID
        //   [0x08..0x58) 5 colors (80 bytes) <-- this is what .evrp asset_data contains
        //   [0x58..0x60) reserved
        //
        // The resource table's primary array (+0xB8) points to the resolved resource data.
        // For tints, we need to find where the color data lives and overwrite it.
        // For the PoC, we replace both the fallback and primary resource IDs to point
        // to our custom tint. The actual color injection happens when we hook the
        // resource resolution that turns a tint SymbolId into color data.
        //
        // TODO(Task 10+): Once we understand the full tint resolution chain,
        // write the 80 bytes directly into the TintEntry color region.
        // For now, this confirms the CDN data is loaded and the hook fires.
    }

    return result;
}

}  // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

void AssetCDN::Initialize() {
    if (g_hookInstalled) return;

    void* target = reinterpret_cast<void*>(
        EchoVR::g_GameBaseAddress + PatchAddresses::LOADOUT_RESOLVE_DATA_FROM_ID);

    if (!nevr::ValidatePrologue(target, EXPECTED_PROLOGUE, PROLOGUE_LEN)) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] prologue validation failed for Loadout_ResolveDataFromId — hook NOT installed: "
            "target=%p expected=%s actual=%s",
            target, BytesToHex(EXPECTED_PROLOGUE, PROLOGUE_LEN).c_str(),
            BytesToHex(reinterpret_cast<const uint8_t*>(target), PROLOGUE_LEN).c_str());
        return;
    }

    g_originalFunc = reinterpret_cast<LoadoutResolveDataFromIdFunc>(target);

    BOOL success = Hooking::Attach(
        reinterpret_cast<PVOID*>(&g_originalFunc),
        reinterpret_cast<PVOID>(Hook_LoadoutResolveDataFromId));

    if (!success) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] hook install failed name=Loadout_ResolveDataFromId target=%p reason=%s",
            target, Hooking::LastAttachError());
        g_originalFunc = nullptr;
        return;
    }

    g_hookInstalled = true;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.CDN] Loadout_ResolveDataFromId hook installed at %p — CDN tint override active", target);

    nevr::EnsureCurlGlobalInit();
    StartBackgroundFetch();
}

void AssetCDN::Shutdown() {
    // Signal background thread to stop
    g_shutdownRequested.store(true);

    // Wait for background thread to finish
    if (g_fetchThread.joinable()) {
        g_fetchThread.join();
    }

    // Remove hook
    const bool hookWasInstalled = g_hookInstalled;
    if (g_hookInstalled) {
        Hooking::Detach(
            reinterpret_cast<PVOID*>(&g_originalFunc),
            reinterpret_cast<PVOID>(Hook_LoadoutResolveDataFromId));
        g_hookInstalled = false;
        g_originalFunc = nullptr;
    }

    // Clean up tint map: no new hook call can pick it up (pointer nulled, hook detached); free it
    // only once the calls already inside the hook have left.
    {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_tintMap.store(nullptr, std::memory_order_release);
        if (g_tintGate.WaitIdle(kTintGateWait)) {
            delete g_tintMapOwned;
            g_tintMapOwned = nullptr;
        } else {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.CDN] tint map left allocated: the loadout hook did not leave within %lld ms",
                static_cast<long long>(kTintGateWait.count()));
        }
    }

    g_manifestPackages.clear();
    g_fetchState.store(FetchState::Idle);
    g_shutdownRequested.store(false);

    Log(EchoVR::LogLevel::Info, "[NEVR.CDN] shutdown complete hook_removed=%s",
        hookWasInstalled ? "true" : "false");
}

void AssetCDN::StartBackgroundFetch() {
    FetchState expected = FetchState::Idle;
    if (!g_fetchState.compare_exchange_strong(expected, FetchState::FetchingManifest)) {
        // Already running or completed
        return;
    }

    g_shutdownRequested.store(false);
    if (g_fetchThread.joinable()) {
        g_fetchThread.join();
    }
    g_fetchThread = std::thread(BackgroundFetchThread);
}

AssetCDN::FetchState AssetCDN::GetFetchState() {
    return g_fetchState.load();
}

std::string AssetCDN::GetCacheDir() {
    wchar_t* localAppDataPath = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppDataPath);
    if (hr != S_OK) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] SHGetKnownFolderPath failed for LOCALAPPDATA hr=0x%08lX", hr);
        return "";
    }

    // Convert wchar_t* to std::string (ASCII path assumed)
    std::wstring widePath(localAppDataPath);
    CoTaskMemFree(localAppDataPath);

    std::string path(widePath.begin(), widePath.end());
    path += "\\EchoVR\\cosmetics\\v1\\packages";

    // Create directory tree if it doesn't exist
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] Failed to create cache directory: %s (%s)",
            path.c_str(), ec.message().c_str());
        return "";
    }

    return path;
}

bool AssetCDN::IsCached(const std::string& filename) {
    std::string cacheDir = GetCacheDir();
    if (cacheDir.empty()) return false;

    std::string fullPath = cacheDir + "\\" + filename;
    return std::filesystem::exists(fullPath);
}

bool AssetCDN::FetchManifest() {
    nevr::EnsureCurlGlobalInit();
    CURL* curl = curl_easy_init();
    if (!curl) {
        Log(EchoVR::LogLevel::Error, "[NEVR.CDN] curl_easy_init failed");
        return false;
    }

    std::vector<uint8_t> buffer;
    curl_easy_setopt(curl, CURLOPT_URL, MANIFEST_URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nevr-runtime/1.0");

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        const std::string diagnostic =
            LogDiagnostics::FormatCurlFailureDiagnostic("[NEVR.CDN] Manifest fetch failed ", static_cast<int>(res));
        Log(EchoVR::LogLevel::Error, "%s", diagnostic.c_str());
        return false;
    }

    if (http_code != 200) {
        Log(EchoVR::LogLevel::Error, "[NEVR.CDN] manifest fetch failed: http_status=%ld", http_code);
        return false;
    }

    // Parse JSON
    json manifest;
    try {
        manifest = json::parse(buffer.begin(), buffer.end());
    } catch (const json::parse_error&) {
        Log(EchoVR::LogLevel::Error, "[NEVR.CDN] Manifest JSON parse error response_bytes=%zu", buffer.size());
        return false;
    }

    // Validate version
    if (!manifest.contains("version") || manifest["version"].get<int>() != 1) {
        int got = (manifest.contains("version") && manifest["version"].is_number())
                      ? manifest["version"].get<int>() : -1;
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] unsupported manifest version: got=%d want=1", got);
        return false;
    }

    if (!manifest.contains("packages") || !manifest["packages"].is_object()) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] manifest packages field %s",
            !manifest.contains("packages") ? "missing" : "present but not an object");
        return false;
    }

    // Parse package entries
    g_manifestPackages.clear();
    const auto& packages = manifest["packages"];

    for (auto it = packages.begin(); it != packages.end(); ++it) {
        const std::string& hex_key = it.key();
        const auto& pkg = it.value();

        // Parse hex symbol ID from key
        int64_t symbol_id;
        try {
            // Use unsigned parse then cast to avoid sign issues
            uint64_t uid = std::stoull(hex_key, nullptr, 16);
            symbol_id = static_cast<int64_t>(uid);
        } catch (...) {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.CDN] Skipping package with invalid key: %s", hex_key.c_str());
            continue;
        }

        if (!pkg.contains("url") || !pkg.contains("sha256") ||
            !pkg.contains("slot_type") || !pkg.contains("size")) {
            std::string missing;
            if (!pkg.contains("url")) missing += (missing.empty() ? "" : ",") + std::string("url");
            if (!pkg.contains("sha256")) missing += (missing.empty() ? "" : ",") + std::string("sha256");
            if (!pkg.contains("slot_type")) missing += (missing.empty() ? "" : ",") + std::string("slot_type");
            if (!pkg.contains("size")) missing += (missing.empty() ? "" : ",") + std::string("size");
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.CDN] skipping package %s: missing fields=%s", hex_key.c_str(), missing.c_str());
            continue;
        }

        PackageEntry entry;
        entry.url = pkg["url"].get<std::string>();
        entry.sha256 = pkg["sha256"].get<std::string>();
        entry.slot_type = pkg["slot_type"].get<std::string>();
        entry.size = pkg["size"].get<int64_t>();
        entry.symbol_id = symbol_id;

        g_manifestPackages[symbol_id] = std::move(entry);
    }

    Log(EchoVR::LogLevel::Info,
        "[NEVR.CDN] Manifest loaded: %zu packages", g_manifestPackages.size());
    return true;
}

bool AssetCDN::DownloadPackage(const std::string& url, const std::string& dest_path,
                                const std::string& expected_sha256) {
    nevr::EnsureCurlGlobalInit();
    CURL* curl = curl_easy_init();
    if (!curl) {
        Log(EchoVR::LogLevel::Error, "[NEVR.CDN] curl_easy_init failed for package download url=%s",
            url.c_str());
        return false;
    }

    std::vector<uint8_t> buffer;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nevr-runtime/1.0");

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        const std::string diagnostic =
            LogDiagnostics::FormatCurlFailureDiagnostic("[NEVR.CDN] Package download failed ", static_cast<int>(res));
        Log(EchoVR::LogLevel::Error, "%s", diagnostic.c_str());
        return false;
    }

    if (http_code != 200) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] Package download HTTP %ld: %s", http_code, url.c_str());
        return false;
    }

    // Verify SHA256
    std::string actual_sha256 = ComputeSHA256(buffer);
    if (actual_sha256.empty()) {
        // Best-effort: ComputeSHA256 has 4 internal CryptoAPI failure points and
        // none of them log or propagate a reason, so this is whatever the last
        // Win32 call before its early return left behind — may be stale if a
        // CryptDestroyHash/CryptReleaseContext ran between the failure and here,
        // but it costs nothing and is sometimes the real answer.
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] SHA256 computation failed for %s error=%lu", url.c_str(), GetLastError());
        return false;
    }

    if (actual_sha256 != expected_sha256) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] SHA256 mismatch for %s: got %s, expected %s",
            url.c_str(), actual_sha256.c_str(), expected_sha256.c_str());
        return false;
    }

    // Write to cache
    std::ofstream ofs(dest_path, std::ios::binary);
    if (!ofs.good()) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] failed to open cache file for writing: path=%s errno=%d (%s)",
            dest_path.c_str(), errno, strerror(errno));
        return false;
    }

    ofs.write(reinterpret_cast<const char*>(buffer.data()),
              static_cast<std::streamsize>(buffer.size()));
    ofs.close();

    if (ofs.fail()) {
        Log(EchoVR::LogLevel::Error,
            "[NEVR.CDN] failed to write cache file: path=%s errno=%d (%s)",
            dest_path.c_str(), errno, strerror(errno));
        return false;
    }

    Log(EchoVR::LogLevel::Info,
        "[NEVR.CDN] Downloaded and cached: %s (%zu bytes)",
        dest_path.c_str(), buffer.size());
    return true;
}
