#include "gun2cr_hook.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "common/hooking.h"
#include "common/logging.h"
#include "echovrInternal.h"

// =============================================================================
// Gun2CR Visual Effects Fix - Hook Implementation
// =============================================================================

// Log file for Gun2CR diagnostics
static FILE* g_gun2crLog = nullptr;
static BOOL g_hooksInitialized = FALSE;

// GunCR reference values (loaded from config)
static GunCRReferenceValues g_guncr_ref = {0};

// Original function pointer
static pInitBulletCI orig_InitBulletCI = nullptr;

// =============================================================================
// Configuration Loading
// =============================================================================

BOOL LoadGun2CRConfig(const char* configPath) {
  FILE* configFile = nullptr;
  errno_t err = fopen_s(&configFile, configPath, "r");
  if (err != 0 || configFile == nullptr) {
    Log(EchoVR::LogLevel::Warning, "[Gun2CR] Config file not found: %s", configPath);
    Log(EchoVR::LogLevel::Warning, "[Gun2CR] Using extraction mode - will log GunCR values for setup");
    g_guncr_ref.enabled = FALSE;
    return FALSE;
  }

  // Simple INI parser - reads [GunCR_Reference] section
  char line[256];
  BOOL inReferenceSection = FALSE;

  while (fgets(line, sizeof(line), configFile) != nullptr) {
    // Remove newline
    line[strcspn(line, "\r\n")] = '\0';

    // Skip comments and empty lines
    if (line[0] == '#' || line[0] == ';' || line[0] == '\0') {
      continue;
    }

    // Check for section header
    if (line[0] == '[') {
      inReferenceSection = (strstr(line, "[GunCR_Reference]") != nullptr);
      continue;
    }

    // Parse key=value pairs in [GunCR_Reference] section
    if (inReferenceSection) {
      char key[64] = {0};
      char value[64] = {0};

      if (sscanf_s(line, "%63[^=]=%63s", key, (unsigned)sizeof(key), value, (unsigned)sizeof(value)) == 2) {
        // Trim whitespace from key
        char* keyStart = key;
        while (*keyStart == ' ' || *keyStart == '\t') keyStart++;
        char* keyEnd = keyStart + strlen(keyStart) - 1;
        while (keyEnd > keyStart && (*keyEnd == ' ' || *keyEnd == '\t')) *keyEnd-- = '\0';

        // Parse values
        if (strcmp(keyStart, "trailduration") == 0) {
          g_guncr_ref.trailduration = (float)atof(value);
        } else if (strcmp(keyStart, "trailpfx") == 0) {
          g_guncr_ref.trailpfx = strtoull(value, nullptr, 0);
        } else if (strcmp(keyStart, "trailpfx_b") == 0) {
          g_guncr_ref.trailpfx_b = strtoull(value, nullptr, 0);
        } else if (strcmp(keyStart, "collisionpfx") == 0) {
          g_guncr_ref.collisionpfx = strtoull(value, nullptr, 0);
        } else if (strcmp(keyStart, "collisionpfx_b") == 0) {
          g_guncr_ref.collisionpfx_b = strtoull(value, nullptr, 0);
        } else if (strcmp(keyStart, "flags") == 0) {
          g_guncr_ref.flags = (uint32_t)strtoul(value, nullptr, 0);
        } else if (strcmp(keyStart, "enabled") == 0) {
          g_guncr_ref.enabled = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        }
      }
    }
  }

  fclose(configFile);

  // Validate config
  if (g_guncr_ref.enabled) {
    if (g_guncr_ref.trailduration == 0.0f || g_guncr_ref.trailpfx == 0) {
      Log(EchoVR::LogLevel::Warning, "[Gun2CR] Config loaded but values are zero - patching disabled");
      g_guncr_ref.enabled = FALSE;
      return FALSE;
    }

    Log(EchoVR::LogLevel::Info, "`");
    Log(EchoVR::LogLevel::Info, "[Gun2CR]   trailduration: %.4f", g_guncr_ref.trailduration);
    Log(EchoVR::LogLevel::Info, "[Gun2CR]   trailpfx: 0x%016llX", g_guncr_ref.trailpfx);
    Log(EchoVR::LogLevel::Info, "[Gun2CR]   flags: 0x%08X", g_guncr_ref.flags);
    return TRUE;
  }

  return FALSE;
}

// =============================================================================
// Diagnostic Logging Helpers
// =============================================================================

static void log_to_file(const char* format, ...) {
  if (g_gun2crLog == nullptr) {
    return;
  }

  // Timestamp
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  struct tm timeinfo;
  localtime_s(&timeinfo, &time);
  fprintf(g_gun2crLog, "[%02d:%02d:%02d] ", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  // Message
  va_list args;
  va_start(args, format);
  vfprintf(g_gun2crLog, format, args);
  va_end(args);

  fprintf(g_gun2crLog, "\n");
  fflush(g_gun2crLog);
}

static void dump_sproperties(const char* label, const SR15NetBullet2CD_SProperties* props) {
  log_to_file("=== SProperties Dump: %s ===", label);
  log_to_file("  flags: 0x%08X", props->flags);
  log_to_file("  distance: %.2f", props->distance);
  log_to_file("  lifetime: %.2f", props->lifetime);
  log_to_file("  trailduration: %.4f  %s", props->trailduration,
              (props->trailduration == 0.0f) ? "**ZERO - WILL DIVIDE BY ZERO**" : "OK");
  log_to_file("  decalscale: %.2f", props->decalscale);
  log_to_file("  decalfadetime: %.2f", props->decalfadetime);
  log_to_file("  ricochets: %d", props->ricochets);
  log_to_file("  whizzbystartkey: 0x%016llX", props->whizzbystartkey);
  log_to_file("  collisionpfx: 0x%016llX  %s", props->collisionpfx,
              (props->collisionpfx == 0) ? "**ZERO - NO IMPACT PARTICLES**" : "OK");
  log_to_file("  trailpfx: 0x%016llX  %s", props->trailpfx, (props->trailpfx == 0) ? "**ZERO - NO TRAIL**" : "OK");
  log_to_file("  explosionpfx: 0x%016llX", props->explosionpfx);
  log_to_file("  collisionpfx_b: 0x%016llX", props->collisionpfx_b);
  log_to_file("  trailpfx_b: 0x%016llX  %s", props->trailpfx_b,
              (props->trailpfx_b == 0) ? "**ZERO - NO SECONDARY TRAIL**" : "OK");
  log_to_file("  explosionpfx_b: 0x%016llX", props->explosionpfx_b);

  // Flag analysis
  log_to_file("  Flag Breakdown:");
  if (props->flags & FLAG_USE_IMPACT)
    log_to_file("    - USE_IMPACT: YES");
  else
    log_to_file("    - USE_IMPACT: NO  **IMPACT PARTICLES DISABLED**");

  if (props->flags & FLAG_TRAIL_SCALES_WITH_DAMAGE)
    log_to_file("    - TRAIL_SCALES_WITH_DAMAGE: YES");
  else
    log_to_file("    - TRAIL_SCALES_WITH_DAMAGE: NO");

  if (props->flags & FLAG_UPDATE_TRAIL_LENGTH)
    log_to_file("    - UPDATE_TRAIL_LENGTH: YES");
  else
    log_to_file("    - UPDATE_TRAIL_LENGTH: NO  **STATIC TRAIL LENGTH**");

  if (props->flags & FLAG_KILL_TRAIL_ON_COLLISION)
    log_to_file("    - KILL_TRAIL_ON_COLLISION: YES");
  else
    log_to_file("    - KILL_TRAIL_ON_COLLISION: NO");
}

static void log_component_id(const char* label, SComponentID comp_id) {
  log_to_file("Component ID (%s): index=%u, type_hash=0x%08X", label, comp_id.index, comp_id.type_hash);

  if (comp_id.type_hash == COMPONENT_CR15NetBullet2CR) {
    log_to_file("  -> Identified as Gun2CR bullet (BROKEN)");
  } else if (comp_id.type_hash == COMPONENT_CR15NetBulletCR) {
    log_to_file("  -> Identified as GunCR bullet (WORKING)");
  } else {
    log_to_file("  -> Unknown bullet type (type_hash unknown)");
  }
}

// =============================================================================
// Hook Implementation
// =============================================================================

int __fastcall hook_InitBulletCI(CR15NetBullet2CS* this_ptr, SR15NetBullet2CI* bullet_instance,
                                 const SR15NetBullet2CD_SProperties* props, SComponentID component_id,
                                 SCompactPoolHandle pool_handle, uint64_t flags, void* user_data) {
  // Detect bullet type
  bool is_gun2cr = (component_id.type_hash == COMPONENT_CR15NetBullet2CR);
  bool is_guncr = (component_id.type_hash == COMPONENT_CR15NetBulletCR);

  // Check if patching is needed (Gun2CR with zero values)
  bool needs_patch = is_gun2cr && g_guncr_ref.enabled && (props->trailduration == 0.0f || props->trailpfx == 0);

  // Log GunCR bullets for reference extraction (if patching not enabled)
  if (is_guncr && !g_guncr_ref.enabled && g_gun2crLog != nullptr) {
    printf("[Gun2CR] GunCR reference values detected - capturing for config\n");
    log_to_file("\n========================================");
    log_to_file("=== GUNCR REFERENCE VALUES (COPY THESE TO CONFIG) ===");
    log_to_file("========================================");
    log_component_id("GunCR", component_id);
    dump_sproperties("GunCR Working", props);
    log_to_file("========================================");
    log_to_file("Suggested gun2cr_config.ini entries:");
    log_to_file("[GunCR_Reference]");
    log_to_file("enabled = true");
    log_to_file("trailduration = %.4f", props->trailduration);
    log_to_file("trailpfx = 0x%016llX", props->trailpfx);
    log_to_file("trailpfx_b = 0x%016llX", props->trailpfx_b);
    log_to_file("collisionpfx = 0x%016llX", props->collisionpfx);
    log_to_file("collisionpfx_b = 0x%016llX", props->collisionpfx_b);
    log_to_file("flags = 0x%08X", props->flags);
    log_to_file("========================================\n");
  }

  // Log Gun2CR bullets (diagnostic mode)
  if (is_gun2cr && g_gun2crLog != nullptr) {
    printf("[Gun2CR] Gun2CR bullet detected (type_hash=0x%08X)\n", component_id.type_hash);
    log_to_file("\n========================================");
    log_to_file("Gun2CR Bullet Detected");
    log_to_file("========================================");
    log_component_id("Gun2CR", component_id);
    dump_sproperties("BEFORE PATCH", props);

    if (props->trailduration == 0.0f) {
      printf("[Gun2CR] WARNING: trailduration is ZERO - will cause divide-by-zero!\n");
      log_to_file("**CRITICAL**: trailduration is ZERO - will cause divide-by-zero!");
    }

    if (!g_guncr_ref.enabled) {
      printf("[Gun2CR] Patching disabled - fire GunCR to extract reference values\n");
      log_to_file("**NOTE**: Patching disabled - fire GunCR to extract reference values");
    }
  }

  // Apply patch if needed
  if (needs_patch) {
    printf("[Gun2CR] Applying patch to Gun2CR bullet (fixing zero values)\n");
    log_to_file("Gun2CR detected with zero values - applying patch");

    // Create patched copy of SProperties
    // IMPORTANT: Do NOT modify the original props (may be read-only)
    SR15NetBullet2CD_SProperties patched_props = *props;

    // Override zero values with GunCR reference
    if (patched_props.trailduration == 0.0f) {
      patched_props.trailduration = g_guncr_ref.trailduration;
      printf("[Gun2CR]   - Set trailduration: %.4f\n", patched_props.trailduration);
    }

    if (patched_props.trailpfx == 0) {
      patched_props.trailpfx = g_guncr_ref.trailpfx;
      printf("[Gun2CR]   - Set trailpfx: 0x%016llX\n", patched_props.trailpfx);
    }

    if (patched_props.trailpfx_b == 0) {
      patched_props.trailpfx_b = g_guncr_ref.trailpfx_b;
      printf("[Gun2CR]   - Set trailpfx_b: 0x%016llX\n", patched_props.trailpfx_b);
    }

    if (patched_props.collisionpfx == 0) {
      patched_props.collisionpfx = g_guncr_ref.collisionpfx;
      printf("[Gun2CR]   - Set collisionpfx: 0x%016llX\n", patched_props.collisionpfx);
    }

    if (patched_props.collisionpfx_b == 0) {
      patched_props.collisionpfx_b = g_guncr_ref.collisionpfx_b;
      printf("[Gun2CR]   - Set collisionpfx_b: 0x%016llX\n", patched_props.collisionpfx_b);
    }

    // Enable visual effect flags if missing
    if (!(patched_props.flags & GUN2CR_REQUIRED_FLAGS)) {
      patched_props.flags |= g_guncr_ref.flags;
      printf("[Gun2CR]   - Set flags: 0x%08X\n", patched_props.flags);
    }

    // Diagnostic logging
    dump_sproperties("AFTER PATCH", &patched_props);

    // Call original function with PATCHED properties
    int result =
        orig_InitBulletCI(this_ptr, bullet_instance, &patched_props, component_id, pool_handle, flags, user_data);

    printf("[Gun2CR] Patch applied successfully (InitBulletCI returned: %d)\n", result);
    log_to_file("InitBulletCI returned: %d", result);
    log_to_file("========================================\n");

    return result;
  } else {
    // No patching needed - pass through original
    if (is_gun2cr && g_gun2crLog != nullptr) {
      log_to_file("No patching applied (patching disabled or already has values)");
      log_to_file("========================================\n");
    }

    return orig_InitBulletCI(this_ptr, bullet_instance, props, component_id, pool_handle, flags, user_data);
  }
}

// =============================================================================
// Initialization & Shutdown
// =============================================================================

VOID InitializeGun2CRHook() {
  // PATCH: Guard against accessing uninitialized game base address
  try {
    // Test if we can read the game base - this might throw if symbol is invalid
    volatile PVOID testPtr = EchoVR::g_GameBaseAddress;
    if (testPtr == nullptr || (uintptr_t)testPtr < 0x1000) {
      // Invalid base address, defer initialization
      return;
    }
  } catch (...) {
    // g_GameBaseAddress is not accessible
    return;
  }

  // Open diagnostic file FIRST to catch all issues
  FILE* diagFile = nullptr;
  errno_t diagErr = fopen_s(&diagFile, "gun2cr_hook_diag.txt", "w");

  Log(EchoVR::LogLevel::Info, "[Gun2CR] InitializeGun2CRHook called");
  
  // Base address is available, proceed
  if (EchoVR::g_GameBaseAddress == 0x0) {
    Log(EchoVR::LogLevel::Warning, "[Gun2CR] Game base address not available yet (0x0), deferring");
    FILE* diagFile = nullptr;
    fopen_s(&diagFile, "gun2cr_hook_diag.txt", "w");
    if (diagFile) {
      fprintf(diagFile, "[Gun2CR] Game base not available (0x0), deferring initialization\n");
      fclose(diagFile);
    }
    return;
  }
  
  if (diagFile) {
    fprintf(diagFile, "[Gun2CR] InitializeGun2CRHook called\n");
    fprintf(diagFile, "  - EchoVR::g_GameBaseAddress = %p\n", EchoVR::g_GameBaseAddress);

    // Check version
    DWORD* signatureOffset = (DWORD*)(EchoVR::g_GameBaseAddress + 0x3c);  // PE signature offset
    if (signatureOffset) {
      IMAGE_FILE_HEADER* coffFileHeader = (IMAGE_FILE_HEADER*)(EchoVR::g_GameBaseAddress + (*signatureOffset + 4));
      fprintf(diagFile, "  - Game TimeDateStamp = 0x%08X (expected 0x6452dff6 for v34.4.631547.1)\n",
              coffFileHeader->TimeDateStamp);

      if (coffFileHeader->TimeDateStamp != 0x6452dff6) {
        fprintf(diagFile, "  - WARNING: Game version mismatch! Hook address may be incorrect.\n");
      }
    }
    fflush(diagFile);
  }

  if (g_hooksInitialized) {
    Log(EchoVR::LogLevel::Info, "[Gun2CR] Hooks already initialized, skipping");
    if (diagFile) {
      fprintf(diagFile, "[Gun2CR] Hooks already initialized, skipping\n");
      fclose(diagFile);
    }
    return;
  }

  // Initialize hooking library (MinHook) - may already be initialized by GamePatches
  Log(EchoVR::LogLevel::Info, "[Gun2CR] Initializing hooking library...");
  if (diagFile) fprintf(diagFile, "[Gun2CR] Initializing hooking library...\n"), fflush(diagFile);

  BOOL hookingResult = Hooking::Initialize();
  if (!hookingResult) {
    Log(EchoVR::LogLevel::Warning, "[Gun2CR] Hooking library already initialized (this is normal)");
    if (diagFile) fprintf(diagFile, "[Gun2CR] Hooking library already initialized\n"), fflush(diagFile);
  } else {
    Log(EchoVR::LogLevel::Info, "[Gun2CR] Hooking library initialized successfully");
    if (diagFile) fprintf(diagFile, "[Gun2CR] Hooking library initialized successfully\n"), fflush(diagFile);
  }

  // Open log file for diagnostics
  const char* logPath = "gun2cr_hook.log";
  Log(EchoVR::LogLevel::Info, "[Gun2CR] Opening log file: %s", logPath);
  if (diagFile) fprintf(diagFile, "[Gun2CR] Opening log file: %s\n", logPath), fflush(diagFile);

  errno_t err = fopen_s(&g_gun2crLog, logPath, "w");
  if (err != 0 || g_gun2crLog == nullptr) {
    Log(EchoVR::LogLevel::Error, "[Gun2CR] Failed to open log file: %s (errno: %d)", logPath, err);
    if (diagFile) {
      fprintf(diagFile, "[Gun2CR] ERROR: Failed to open log file: %s (errno: %d)\n", logPath, err);
      fclose(diagFile);
    }
    return;
  }
  Log(EchoVR::LogLevel::Info, "[Gun2CR] Log file opened successfully");
  if (diagFile) fprintf(diagFile, "[Gun2CR] Log file opened successfully\n"), fflush(diagFile);

  // Write log header
  fprintf(g_gun2crLog, "# Gun2CR Visual Effects Fix - Diagnostic Log\n");
  fprintf(g_gun2crLog, "# Reference: evr-reconstruction/docs/hooks/gun2cr_visual_fix_hook.md\n");
  fprintf(g_gun2crLog, "#\n");
  fprintf(g_gun2crLog, "# This hook fixes Gun2CR bullet visual effects by:\n");
  fprintf(g_gun2crLog, "# 1. Detecting Gun2CR bullets (component type 0x1e5be8ae)\n");
  fprintf(g_gun2crLog, "# 2. Overriding zero-valued visual parameters\n");
  fprintf(g_gun2crLog, "# 3. Applying working values from GunCR reference\n");
  fprintf(g_gun2crLog, "#\n");
  fprintf(g_gun2crLog, "# Log Format:\n");
  fprintf(g_gun2crLog, "#   - Component ID identification\n");
  fprintf(g_gun2crLog, "#   - BEFORE PATCH: Original asset values\n");
  fprintf(g_gun2crLog, "#   - AFTER PATCH: Patched runtime values\n");
  fprintf(g_gun2crLog, "#\n");
  fprintf(g_gun2crLog, "# Game base address: %p\n", EchoVR::g_GameBaseAddress);
  fprintf(g_gun2crLog, "# Hook target address (RVA): 0x%llX\n", (unsigned long long)ADDR_InitBulletCI);
  fprintf(g_gun2crLog, "# Hook absolute address: %p\n", EchoVR::g_GameBaseAddress + ADDR_InitBulletCI);
  fprintf(g_gun2crLog, "#\n\n");
  fflush(g_gun2crLog);

  Log(EchoVR::LogLevel::Info, "[Gun2CR] Diagnostic log opened: %s", logPath);
  if (diagFile) fprintf(diagFile, "[Gun2CR] Diagnostic log opened: %s\n", logPath), fflush(diagFile);

  // Load configuration
  const char* configPath = "gun2cr_config.ini";
  BOOL configLoaded = LoadGun2CRConfig(configPath);

  if (!configLoaded) {
    Log(EchoVR::LogLevel::Info, "[Gun2CR] Running in EXTRACTION MODE");
    if (diagFile) fprintf(diagFile, "[Gun2CR] Running in EXTRACTION MODE\n"), fflush(diagFile);
    Log(EchoVR::LogLevel::Info, "[Gun2CR] Fire GunCR weapon to capture reference values");
    Log(EchoVR::LogLevel::Info, "[Gun2CR] Then copy values to %s and restart", configPath);
  } else {
    Log(EchoVR::LogLevel::Info, "[Gun2CR] Running in PATCHING MODE");
    if (diagFile) fprintf(diagFile, "[Gun2CR] Running in PATCHING MODE\n"), fflush(diagFile);
    Log(EchoVR::LogLevel::Info, "[Gun2CR] Gun2CR bullets will be fixed automatically");
  }

  // Install Hook: InitBulletCI
  printf("[Gun2CR] About to install hook\n");
  printf("[Gun2CR]   - EchoVR::g_GameBaseAddress = %p\n", EchoVR::g_GameBaseAddress);
  printf("[Gun2CR]   - ADDR_InitBulletCI = 0x%llX\n", (unsigned long long)ADDR_InitBulletCI);
  printf("[Gun2CR]   - Target address = %p\n", EchoVR::g_GameBaseAddress + ADDR_InitBulletCI);

  if (diagFile) {
    fprintf(diagFile, "[Gun2CR] About to install hook:\n");
    fprintf(diagFile, "  - EchoVR::g_GameBaseAddress = %p\n", EchoVR::g_GameBaseAddress);
    fprintf(diagFile, "  - ADDR_InitBulletCI = 0x%llX\n", (unsigned long long)ADDR_InitBulletCI);
    fprintf(diagFile, "  - Target address = %p\n", EchoVR::g_GameBaseAddress + ADDR_InitBulletCI);
    fflush(diagFile);
  }

  orig_InitBulletCI = reinterpret_cast<pInitBulletCI>(EchoVR::g_GameBaseAddress + ADDR_InitBulletCI);
  if (Hooking::Attach(reinterpret_cast<PVOID*>(&orig_InitBulletCI), reinterpret_cast<PVOID>(hook_InitBulletCI))) {
    Log(EchoVR::LogLevel::Info, "[Gun2CR] Installed hook: InitBulletCI @ 0x%llX",
        (unsigned long long)ADDR_InitBulletCI);
    printf("[Gun2CR] Installed hook: InitBulletCI @ %p (RVA 0x%llX)\n", orig_InitBulletCI,
           (unsigned long long)ADDR_InitBulletCI);
    if (diagFile) {
      fprintf(diagFile, "[Gun2CR] Installed hook: InitBulletCI @ %p (RVA 0x%llX)\n", orig_InitBulletCI,
              (unsigned long long)ADDR_InitBulletCI);
      fflush(diagFile);
    }
  } else {
    Log(EchoVR::LogLevel::Error, "[Gun2CR] Failed to hook InitBulletCI");
    printf("[Gun2CR] ERROR: Failed to hook InitBulletCI\n");
    printf("[Gun2CR] This may indicate:\n");
    printf("[Gun2CR]   1. Wrong game version (expected 34.4.631547.1)\n");
    printf("[Gun2CR]   2. Hook address is incorrect for this version\n");
    printf("[Gun2CR]   3. Memory protection or DLL load order issue\n");
    if (diagFile) {
      fprintf(diagFile, "[Gun2CR] ERROR: Failed to hook InitBulletCI\n");
      fprintf(diagFile, "[Gun2CR] This may indicate:\n");
      fprintf(diagFile, "[Gun2CR]   1. Wrong game version (expected 34.4.631547.1)\n");
      fprintf(diagFile, "[Gun2CR]   2. Hook address is incorrect for this version\n");
      fprintf(diagFile, "[Gun2CR]   3. Memory protection or DLL load order issue\n");
      fflush(diagFile);
    }

    // Close log file since hook failed
    if (g_gun2crLog != nullptr) {
      fclose(g_gun2crLog);
      g_gun2crLog = nullptr;
    }
    if (diagFile) fclose(diagFile);
    return;
  }

  g_hooksInitialized = TRUE;
  Log(EchoVR::LogLevel::Info, "[Gun2CR] Hook initialized successfully");
  printf("[Gun2CR] Hook initialized successfully\n");
  if (diagFile) {
    fprintf(diagFile, "[Gun2CR] Hook initialized successfully\n");
    fclose(diagFile);
  }
}

VOID ShutdownGun2CRHook() {
  if (!g_hooksInitialized) {
    return;
  }

  // Flush and close log file
  if (g_gun2crLog != nullptr) {
    fprintf(g_gun2crLog, "\n# End of Gun2CR diagnostic log\n");
    fflush(g_gun2crLog);
    fclose(g_gun2crLog);
    g_gun2crLog = nullptr;

    Log(EchoVR::LogLevel::Info, "[Gun2CR] Diagnostic log closed");
  }

  // Shutdown hooking library
  Hooking::Shutdown();

  g_hooksInitialized = FALSE;
  Log(EchoVR::LogLevel::Info, "[Gun2CR] Hook shutdown complete");
}
