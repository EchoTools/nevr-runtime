#pragma once

/// AssetCDN — CDN-backed tint injection via Loadout_ResolveDataFromId hook.
///
/// Pipeline: fetch manifest → download .evrp packages → cache locally → inject tints.
/// All network I/O runs on a background thread. The hook is O(1) with no allocations.
///
/// Cache location: %LOCALAPPDATA%/EchoVR/cosmetics/v1/packages/
/// CDN base: https://cdn.echo.taxi/v1/
///
/// Not wired into startup — call AssetCDN::Initialize() / AssetCDN::Shutdown()
/// explicitly (Task 13 handles integration).

#include <cstdint>
#include <string>

namespace AssetCDN {

/// Current state of the background fetch pipeline.
enum class FetchState {
    Idle,
    FetchingManifest,
    DownloadingPackages,
    Complete,
    Error
};

/// Install the Loadout_ResolveDataFromId hook.
/// Validates prologue bytes before patching. Logs result.
/// Safe to call multiple times (no-ops after first success).
void Initialize();

/// Remove the hook, stop background thread, clean up curl handles.
/// Safe to call if Initialize() was never called or failed.
void Shutdown();

/// Start the background fetch pipeline (manifest → download → cache → parse).
/// Non-blocking. Logs progress. Safe to call multiple times (no-ops if already running).
void StartBackgroundFetch();

/// Get the current fetch pipeline state (thread-safe).
FetchState GetFetchState();

/// Get the local cache directory path, creating it if needed.
/// Returns empty string on failure.
std::string GetCacheDir();

/// Check if a package file exists in the local cache.
bool IsCached(const std::string& filename);

/// Download the CDN manifest and populate the internal package map.
/// Intended for background thread use. Returns true on success.
bool FetchManifest();

/// Download a single .evrp file from the CDN.
/// Verifies SHA256 against expected_sha256. Returns true on success.
bool DownloadPackage(const std::string& url, const std::string& dest_path,
                     const std::string& expected_sha256);

}  // namespace AssetCDN
