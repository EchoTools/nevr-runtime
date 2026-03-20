#include "hash_hooks.h"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <unordered_set>
#include <string>
#include <mutex>

#include "common/hooking.h"
#include "common/logging.h"
#include "echovrInternal.h"

// =============================================================================
// Hook Implementation for Replicated Variables Hash Discovery
// =============================================================================
// This implements hooks from:
//   - QUICK_START_HASHING_HOOKS.md
//   - REPLICATED_VARIABLES_HOOK_GUIDE.md
//
// **Priority #1: SNS_ComputeMessageHash()**
//   Location: src/NRadEngine/Social/SNSHash.cpp:110
//   Captures: string → final hash (direct mapping to replicated variables)
//
// **Priority #2: CSymbol64_Hash()**
//   Location: src/NRadEngine/Core/Hash.cpp:213
//   Captures: all symbol hashing (broader scope)
// =============================================================================

// Log file for hash captures
static FILE* g_hashLog = nullptr;
static BOOL g_hooksInitialized = FALSE;

// Track seen hashes to avoid duplicates
static std::unordered_set<uint64_t> g_seenCMatSymHashes;
static std::unordered_set<uint64_t> g_seenSMatSymDataHashes;
static std::unordered_set<uint64_t> g_seenCSymbol64Hashes;
static std::mutex g_hashLogMutex;

// =============================================================================
// Function Type Definitions
// =============================================================================

// CMatSym_Hash: const char* → uint64_t hash (stage 1 of SNS)
typedef uint64_t (*pCMatSym_Hash)(const char* str);
pCMatSym_Hash orig_CMatSym_Hash = nullptr;

// SMatSymData_HashA: (seed, hash) → uint64_t hash (stage 2 of SNS)
typedef uint64_t (*pSMatSymData_HashA)(uint64_t seed, uint64_t hash);
pSMatSymData_HashA orig_SMatSymData_HashA = nullptr;

// CSymbol64_Hash: const char* → uint64_t hash
// This is the SECONDARY hook - captures all symbol hashing
typedef uint64_t (*pCSymbol64_Hash)(const char* str, uint64_t seed, int param3, int64_t param4, uint32_t param5);
pCSymbol64_Hash orig_CSymbol64_Hash = nullptr;

// =============================================================================
// Hook Addresses (from Ghidra analysis)
// =============================================================================
// NOTE: These addresses are for Echo VR version 34.4.631547.1
// If version check fails, these may need updating from Ghidra

// IMPORTANT: SNS_ComputeMessageHash does NOT exist as a standalone function!
// It's inlined everywhere. We hook the component functions instead:
//   - CMatSym_Hash (stage 1)
//   - SMatSymData_HashA (stage 2)

// CSymbol64_Hash - Symbol ID hashing (replicated variables, assets, etc.)
// NOTE: These are RVAs (Relative Virtual Addresses) - will be added to base address
#define ADDR_CSymbol64_Hash 0x000ce120

// CMatSym_Hash - First stage of SNS message hashing
#define ADDR_CMatSym_Hash 0x00107f80

// SMatSymData_HashA - Second stage of SNS message hashing
#define ADDR_SMatSymData_HashA 0x00107fd0

// For backward compatibility, we'll synthesize SNS_ComputeMessageHash
// by hooking CMatSym_Hash and tracking when it's followed by SMatSymData_HashA
#define ADDR_SNS_ComputeMessageHash 0x0  // Not used - see above

// =============================================================================
// Hook #1A: CMatSym_Hash - Stage 1 of SNS hashing
// =============================================================================
// Track the last string and intermediate hash for SNS reconstruction

static thread_local const char* g_lastCMatSymInput = nullptr;
static thread_local uint64_t g_lastCMatSymOutput = 0;

uint64_t hook_CMatSym_Hash(const char* str) {
  // Call the original function first
  uint64_t result = orig_CMatSym_Hash(str);

  // Log ALL calls (once per unique hash)
  if (g_hashLog != nullptr && str != nullptr && str[0] != '\0') {
    std::lock_guard<std::mutex> lock(g_hashLogMutex);
    if (g_seenCMatSymHashes.insert(result).second) {  // .second is true if insertion happened
      // Calculate string length
      size_t len = strlen(str);
      
      // Mark strings that look like message names (longer, start with uppercase/S)
      const char* tag = "";
      if (len >= 10 && (str[0] == 'S' || str[0] == 's')) {
        tag = " [LIKELY_MESSAGE]";
      } else if (len < 4) {
        tag = " [SHORT_FRAGMENT]";
      }
      
      fprintf(g_hashLog, "[CMatSym_Hash] \"%s\" -> 0x%016llx (intermediate)%s\n", str, result, tag);
      fflush(g_hashLog);
    }
  }

  // Store for potential SNS hash completion (only store longer strings)
  if (str != nullptr && strlen(str) >= 4) {
    g_lastCMatSymInput = str;
    g_lastCMatSymOutput = result;
  }

  return result;
}

// =============================================================================
// Hook #1B: SMatSymData_HashA - Stage 2 of SNS hashing
// =============================================================================
// Captures: "player_position_x" → 0x0473db6fa63f4c1a
// When called with the SNS seed (0x6d451003fb4b172e), this completes SNS hashing

constexpr uint64_t SNS_SEED = 0x6d451003fb4b172eULL;

uint64_t hook_SMatSymData_HashA(uint64_t seed, uint64_t hash) {
  // Call the original function first
  uint64_t result = orig_SMatSymData_HashA(seed, hash);

  // Log ALL calls (once per unique result hash)
  if (g_hashLog != nullptr) {
    std::lock_guard<std::mutex> lock(g_hashLogMutex);
    if (g_seenSMatSymDataHashes.insert(result).second) {
      // If this is the SNS seed and we have a recent CMatSym call, log with string
      if (seed == SNS_SEED && hash == g_lastCMatSymOutput && g_lastCMatSymInput != nullptr) {
        fprintf(g_hashLog, "[SNS_COMPLETE] \"%s\" -> 0x%016llx (seed=0x%016llx, intermediate=0x%016llx)\n",
                g_lastCMatSymInput, result, seed, hash);
      } else {
        fprintf(g_hashLog, "[SMatSymData_HashA] seed=0x%016llx, hash=0x%016llx -> 0x%016llx\n",
                seed, hash, result);
      }
      fflush(g_hashLog);
    }
    
    // Clear to avoid duplicate logging
    if (seed == SNS_SEED && hash == g_lastCMatSymOutput) {
      g_lastCMatSymInput = nullptr;
    }
  }

  return result;
}

// =============================================================================
// Hook #2: CSymbol64_Hash - SECONDARY HOOK
// =============================================================================
// Captures: all symbol hashing (broader scope than SNS_ComputeMessageHash)
// Filters for default seed to focus on replicated variables

uint64_t hook_CSymbol64_Hash(const char* str, uint64_t seed, int param3, int64_t param4, uint32_t param5) {
  // Call the original function first
  uint64_t result = orig_CSymbol64_Hash(str, seed, param3, param4, param5);

  // Log ALL calls (once per unique hash), but filter empty/null strings
  if (g_hashLog != nullptr && str != nullptr && str[0] != '\0') {
    std::lock_guard<std::mutex> lock(g_hashLogMutex);
    if (g_seenCSymbol64Hashes.insert(result).second) {
      // Indicate if this is the default seed (common for replicated variables)
      const char* seedNote = (seed == 0xFFFFFFFFFFFFFFFFULL) ? " [DEFAULT_SEED]" : "";
      fprintf(g_hashLog, "[CSymbol64_Hash] \"%s\" -> 0x%016llx (seed=0x%016llx)%s\n",
              str, result, seed, seedNote);
      fflush(g_hashLog);
    }
  }

  return result;
}

// =============================================================================
// Initialization & Shutdown
// =============================================================================

VOID InitializeHashHooks() {
  if (g_hooksInitialized) {
    return;
  }

  // Initialize hooking library (MinHook) - may already be initialized by Gun2CR
  BOOL hookingResult = Hooking::Initialize();
  if (!hookingResult) {
    Log(EchoVR::LogLevel::Info, "[DbgHooks] Hooking library already initialized (this is normal)");
  } else {
    Log(EchoVR::LogLevel::Info, "[DbgHooks] Hooking library initialized successfully");
  }

  // Open log file for hash captures
  const char* logPath = "hash_discovery.log";
  fopen_s(&g_hashLog, logPath, "w");
  if (g_hashLog == nullptr) {
    Log(EchoVR::LogLevel::Error, "[DbgHooks] Failed to open log file: %s", logPath);
    return;
  }

  // Write log header
  fprintf(g_hashLog, "# Hash Discovery Log - ALL HASHES MODE\n");
  fprintf(g_hashLog, "# This log captures EVERY unique hash computed (once per unique hash value)\n");
  fprintf(g_hashLog, "#\n");
  fprintf(g_hashLog, "# Hook Types:\n");
  fprintf(g_hashLog, "#   [CMatSym_Hash]       - SNS Stage 1 (intermediate hash)\n");
  fprintf(g_hashLog, "#   [SMatSymData_HashA]  - SNS Stage 2 (final hash with seed mixing)\n");
  fprintf(g_hashLog, "#   [SNS_COMPLETE]       - Complete SNS hash with original string\n");
  fprintf(g_hashLog, "#   [CSymbol64_Hash]     - Symbol ID hashing (assets, replicated vars)\n");
  fprintf(g_hashLog, "#\n");
  fprintf(g_hashLog, "# SNS Message Hash = CMatSym_Hash(string) -> SMatSymData_HashA(0x6d451003fb4b172e, result)\n");
  fprintf(g_hashLog, "# Replicated variables typically use CSymbol64_Hash with seed=0xFFFFFFFFFFFFFFFF\n");
  fprintf(g_hashLog, "#\n");
  fprintf(g_hashLog, "# Reference: evr-reconstruction/docs/usage/QUICK_START_HASHING_HOOKS.md\n");
  fprintf(g_hashLog, "#\n\n");
  fflush(g_hashLog);

  Log(EchoVR::LogLevel::Info, "[DbgHooks] Hash discovery log opened: %s", logPath);

  // Log base address for diagnostics
  Log(EchoVR::LogLevel::Info, "[DbgHooks] Game base address: 0x%p", EchoVR::g_GameBaseAddress);

  // Check if addresses are configured
  if (ADDR_CSymbol64_Hash == 0 || ADDR_CMatSym_Hash == 0 || ADDR_SMatSymData_HashA == 0) {
    Log(EchoVR::LogLevel::Warning,
        "[DbgHooks] Hook addresses not configured! Update ADDR_* constants from Ghidra analysis.");
    Log(EchoVR::LogLevel::Warning, "[DbgHooks] Required addresses:");
    Log(EchoVR::LogLevel::Warning, "[DbgHooks]   - CMatSym_Hash @ 0x140107f80");
    Log(EchoVR::LogLevel::Warning, "[DbgHooks]   - SMatSymData_HashA @ 0x140107fd0");
    Log(EchoVR::LogLevel::Warning, "[DbgHooks]   - CSymbol64_Hash @ 0x1400ce120");

    // Close log file since we can't install hooks
    if (g_hashLog != nullptr) {
      fclose(g_hashLog);
      g_hashLog = nullptr;
    }
    return;
  }

  // Install Hook #1A: CMatSym_Hash (SNS Stage 1)
  orig_CMatSym_Hash = reinterpret_cast<pCMatSym_Hash>(EchoVR::g_GameBaseAddress + ADDR_CMatSym_Hash);
  Log(EchoVR::LogLevel::Info, "[DbgHooks] Targeting CMatSym_Hash @ 0x%p (RVA 0x%llX)", 
      orig_CMatSym_Hash, ADDR_CMatSym_Hash);
  if (Hooking::Attach(reinterpret_cast<PVOID*>(&orig_CMatSym_Hash),
                      reinterpret_cast<PVOID>(hook_CMatSym_Hash))) {
    Log(EchoVR::LogLevel::Info, "[DbgHooks] ✓ Installed hook: CMatSym_Hash");
  } else {
    Log(EchoVR::LogLevel::Error, "[DbgHooks] ✗ Failed to hook CMatSym_Hash");
  }

  // Install Hook #1B: SMatSymData_HashA (SNS Stage 2)
  orig_SMatSymData_HashA = reinterpret_cast<pSMatSymData_HashA>(EchoVR::g_GameBaseAddress + ADDR_SMatSymData_HashA);
  Log(EchoVR::LogLevel::Info, "[DbgHooks] Targeting SMatSymData_HashA @ 0x%p (RVA 0x%llX)",
      orig_SMatSymData_HashA, ADDR_SMatSymData_HashA);
  if (Hooking::Attach(reinterpret_cast<PVOID*>(&orig_SMatSymData_HashA),
                      reinterpret_cast<PVOID>(hook_SMatSymData_HashA))) {
    Log(EchoVR::LogLevel::Info, "[DbgHooks] ✓ Installed hook: SMatSymData_HashA");
  } else {
    Log(EchoVR::LogLevel::Error, "[DbgHooks] ✗ Failed to hook SMatSymData_HashA");
  }

  // Install Hook #2: CSymbol64_Hash (SECONDARY)
  orig_CSymbol64_Hash = reinterpret_cast<pCSymbol64_Hash>(EchoVR::g_GameBaseAddress + ADDR_CSymbol64_Hash);
  Log(EchoVR::LogLevel::Info, "[DbgHooks] Targeting CSymbol64_Hash @ 0x%p (RVA 0x%llX)",
      orig_CSymbol64_Hash, ADDR_CSymbol64_Hash);
  if (Hooking::Attach(reinterpret_cast<PVOID*>(&orig_CSymbol64_Hash), reinterpret_cast<PVOID>(hook_CSymbol64_Hash))) {
    Log(EchoVR::LogLevel::Info, "[DbgHooks] ✓ Installed hook: CSymbol64_Hash");
  } else {
    Log(EchoVR::LogLevel::Error, "[DbgHooks] ✗ Failed to hook CSymbol64_Hash");
  }

  g_hooksInitialized = TRUE;
  Log(EchoVR::LogLevel::Info, "[DbgHooks] Hash discovery hooks initialized successfully");
  Log(EchoVR::LogLevel::Info, "[DbgHooks] ALL HASHES MODE: Capturing every unique hash (once)");
  Log(EchoVR::LogLevel::Info, "[DbgHooks] Play game for 5-10 minutes to capture variable names");
}

VOID ShutdownHashHooks() {
  if (!g_hooksInitialized) {
    return;
  }

  // Flush and close log file
  if (g_hashLog != nullptr) {
    fprintf(g_hashLog, "\n# End of hash discovery log\n");
    fprintf(g_hashLog, "# Total unique hashes captured:\n");
    fprintf(g_hashLog, "#   CMatSym_Hash: %zu\n", g_seenCMatSymHashes.size());
    fprintf(g_hashLog, "#   SMatSymData_HashA: %zu\n", g_seenSMatSymDataHashes.size());
    fprintf(g_hashLog, "#   CSymbol64_Hash: %zu\n", g_seenCSymbol64Hashes.size());
    fflush(g_hashLog);
    fclose(g_hashLog);
    g_hashLog = nullptr;

    Log(EchoVR::LogLevel::Info, "[DbgHooks] Hash discovery log closed");
    Log(EchoVR::LogLevel::Info, "[DbgHooks] Captured: CMatSym=%zu, SMatSymData=%zu, CSymbol64=%zu",
        g_seenCMatSymHashes.size(), g_seenSMatSymDataHashes.size(), g_seenCSymbol64Hashes.size());
  }
  
  // Clear the tracking sets
  g_seenCMatSymHashes.clear();
  g_seenSMatSymDataHashes.clear();
  g_seenCSymbol64Hashes.clear();

  // Shutdown hooking library
  Hooking::Shutdown();

  g_hooksInitialized = FALSE;
  Log(EchoVR::LogLevel::Info, "[DbgHooks] Hash hooks shutdown complete");
}
