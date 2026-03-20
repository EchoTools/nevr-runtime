// mesh_dump_hooks.cpp - Mesh binary format dumping hooks for CGMeshListResource
//
// Purpose: Capture mesh data at runtime to reverse engineer binary format
//
// Hooks implemented:
//   1. AsyncResourceIOCallback @ 0x140fa16d0 - Captures raw buffer data after I/O
//   2. CGMeshListResource::DeserializeAndUpload @ 0x140547ab0 - Captures parsed mesh structures
//
// Output:
//   ./mesh_dumps/raw_buffers/     - Raw binary files (buffer_<addr>_<size>.bin)
//   ./mesh_dumps/parsed_meshes/   - Parsed mesh data (mesh_<name>.json)
//   ./mesh_dumps/mesh_log.txt     - Detailed log with offsets, counts, formats

#include "mesh_dump_hooks.h"

#include <MinHook.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "gameserverlegacy/common/echovrInternal.h"

namespace fs = std::filesystem;

// ============================================================================
// CONFIGURATION
// ============================================================================

constexpr const char* MESH_DUMP_DIR = "./mesh_dumps";
constexpr const char* RAW_BUFFERS_DIR = "./mesh_dumps/raw_buffers";
constexpr const char* PARSED_MESHES_DIR = "./mesh_dumps/parsed_meshes";
constexpr const char* MESH_LOG_FILE = "./mesh_dumps/mesh_log.txt";

constexpr bool ENABLE_HOOKS = false;       // Master switch - set to true to enable hooks
constexpr bool DUMP_RAW_BUFFERS = true;    // Dump raw I/O buffers
constexpr bool DUMP_PARSED_MESHES = true;  // Dump parsed mesh structures
constexpr bool VERBOSE_LOGGING = true;     // Detailed logging

// ============================================================================
// TARGET ADDRESSES (from Session 6 analysis)
// ============================================================================

// AsyncResourceIOCallback - Called after I/O completes with loaded buffer
// NOTE: These are RVAs (Relative Virtual Addresses) - will be added to game base address
constexpr uintptr_t ADDR_AsyncResourceIOCallback = 0x0fa16d0;

// CGMeshListResource::DeserializeAndUpload - Uploads mesh to GPU
// (Ghidra misnames this as "CR14Game::UpdateGame", but it's the right function)
constexpr uintptr_t ADDR_DeserializeAndUpload = 0x0547ab0;

// CGMeshListResource type hash for filtering
constexpr uint64_t CGMESHLIST_TYPE_HASH = 0xa196a1d7e1f051af;

// ============================================================================
// DATA STRUCTURES (from Session 6 findings)
// ============================================================================

#pragma pack(push, 1)

// I/O callback data structure
struct SIORequestCallbackData {
  int64_t status;         // +0x00 (4 = success)
  void* resource_ptr;     // +0x08 (points to CGMeshListResource*)
  uint32_t buffer_index;  // +0x10 (0 or 1 for up to 2 files)
  uint32_t padding;       // +0x14
  void* file_buffer;      // +0x18 (loaded file data)
  void* buffer_context;   // +0x20 (size or allocator context)
};

// CResource buffer storage (base class fields)
struct CResourceBuffers {
  // ... vtable and other fields +0x00 to +0x38
  char padding[0x40];

  void* buffer1;   // +0x40 (param[8])
  void* buf1_ctx;  // +0x48 (param[9]) - size/allocator
  void* buffer2;   // +0x50 (param[10])
  void* buf2_ctx;  // +0x58 (param[11])
};

// Index buffer descriptor (0x10 bytes)
struct IndexBufferDescriptor {
  uint32_t offset;   // +0x00 - Offset from buffer base
  uint32_t count;    // +0x04 - Number of indices
  uint32_t format;   // +0x08 - 2 = R16_UINT, other = R32_UINT
  uint32_t padding;  // +0x0C
};

#pragma pack(pop)

// ============================================================================
// GLOBAL STATE
// ============================================================================

static std::ofstream g_logFile;
static CRITICAL_SECTION g_logLock;
static bool g_hooksInitialized = false;
static uint32_t g_bufferDumpCount = 0;
static uint32_t g_meshDumpCount = 0;

// Original function pointers
typedef void (*AsyncResourceIOCallback_t)(SIORequestCallbackData* callback_data);
typedef void (*DeserializeAndUpload_t)(void* this_ptr, uint64_t param1);

static AsyncResourceIOCallback_t g_originalAsyncCallback = nullptr;
static DeserializeAndUpload_t g_originalDeserialize = nullptr;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static void Log(const char* format, ...) {
  if (!VERBOSE_LOGGING) return;

  EnterCriticalSection(&g_logLock);

  va_list args;
  va_start(args, format);

  // Log to file
  if (g_logFile.is_open()) {
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    g_logFile << buffer << std::endl;
    g_logFile.flush();
  }

  va_end(args);
  LeaveCriticalSection(&g_logLock);
}

static void DumpHexToLog(const void* data, size_t size, const char* label) {
  if (!VERBOSE_LOGGING || size == 0) return;

  Log("\n=== %s (size: %zu bytes) ===", label, size);

  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  constexpr size_t max_dump = 256;  // Only dump first 256 bytes
  size_t dump_size = (size < max_dump) ? size : max_dump;

  std::stringstream ss;
  for (size_t i = 0; i < dump_size; i++) {
    if (i % 16 == 0) {
      if (i > 0) {
        Log("%s", ss.str().c_str());
        ss.str("");
      }
      ss << std::setfill('0') << std::setw(8) << std::hex << i << ": ";
    }
    ss << std::setfill('0') << std::setw(2) << std::hex << (int)bytes[i] << " ";
  }

  if (!ss.str().empty()) {
    Log("%s", ss.str().c_str());
  }

  if (size > max_dump) {
    Log("... (%zu more bytes)", size - max_dump);
  }
}

static void DumpBufferToFile(const void* data, size_t size, const char* filename) {
  if (!data || size == 0) return;

  std::string filepath = std::string(RAW_BUFFERS_DIR) + "/" + filename;

  std::ofstream file(filepath, std::ios::binary);
  if (file.is_open()) {
    file.write(static_cast<const char*>(data), size);
    file.close();
    Log("Dumped buffer to: %s", filepath.c_str());
  } else {
    Log("ERROR: Failed to write buffer to %s", filepath.c_str());
  }
}

// ============================================================================
// HOOK 1: AsyncResourceIOCallback @ 0x140fa16d0
// ============================================================================

static void WINAPI Hook_AsyncResourceIOCallback(SIORequestCallbackData* callback_data) {
  // Log the callback
  Log("\n========================================");
  Log("AsyncResourceIOCallback HOOKED");
  Log("  Status: %lld", callback_data->status);
  Log("  Resource: 0x%p", callback_data->resource_ptr);
  Log("  Buffer Index: %u", callback_data->buffer_index);
  Log("  Buffer: 0x%p", callback_data->file_buffer);
  Log("  Context: 0x%p", callback_data->buffer_context);

  // Dump raw buffer if enabled
  if (DUMP_RAW_BUFFERS && callback_data->file_buffer && callback_data->buffer_context) {
    // Try to interpret buffer_context as size (common pattern)
    size_t buffer_size = reinterpret_cast<size_t>(callback_data->buffer_context);

    // Sanity check: reasonable size (< 100MB)
    if (buffer_size > 0 && buffer_size < 100 * 1024 * 1024) {
      char filename[256];
      snprintf(filename, sizeof(filename), "buffer_%u_0x%p_size%zu.bin", g_bufferDumpCount++,
               callback_data->file_buffer, buffer_size);

      DumpBufferToFile(callback_data->file_buffer, buffer_size, filename);
      DumpHexToLog(callback_data->file_buffer, buffer_size, "Raw Buffer Hex Dump");
    } else {
      Log("  WARNING: Buffer size %zu looks suspicious, skipping dump", buffer_size);
    }
  }

  // Call original function
  if (g_originalAsyncCallback) {
    g_originalAsyncCallback(callback_data);
  }
}

// ============================================================================
// HOOK 2: CGMeshListResource::DeserializeAndUpload @ 0x140547ab0
// ============================================================================

static void WINAPI Hook_DeserializeAndUpload(void* this_ptr, uint64_t param1) {
  Log("\n========================================");
  Log("CGMeshListResource::DeserializeAndUpload HOOKED");
  Log("  this: 0x%p", this_ptr);
  Log("  param1: 0x%llx", param1);

  if (DUMP_PARSED_MESHES && this_ptr) {
    // Read buffer pointers from CResource base class
    CResourceBuffers* resource = static_cast<CResourceBuffers*>(this_ptr);

    Log("  buffer1: 0x%p", resource->buffer1);
    Log("  buf1_ctx: 0x%p", resource->buf1_ctx);
    Log("  buffer2: 0x%p", resource->buffer2);
    Log("  buf2_ctx: 0x%p", resource->buf2_ctx);

    // Dump buffer contents if available
    if (resource->buffer1) {
      size_t buf1_size = reinterpret_cast<size_t>(resource->buf1_ctx);
      if (buf1_size > 0 && buf1_size < 100 * 1024 * 1024) {
        char filename[256];
        snprintf(filename, sizeof(filename), "mesh_%u_buffer1_size%zu.bin", g_meshDumpCount, buf1_size);
        DumpBufferToFile(resource->buffer1, buf1_size, filename);
      }
    }

    if (resource->buffer2) {
      size_t buf2_size = reinterpret_cast<size_t>(resource->buf2_ctx);
      if (buf2_size > 0 && buf2_size < 100 * 1024 * 1024) {
        char filename[256];
        snprintf(filename, sizeof(filename), "mesh_%u_buffer2_size%zu.bin", g_meshDumpCount, buf2_size);
        DumpBufferToFile(resource->buffer2, buf2_size, filename);
      }
    }

    // Read mesh object arrays (from constructor analysis @ 0x14052e240)
    // Arrays at: this+0x110, this+0x1b8, this+0x148, this+0x180, this+0x260, this+0x298
    uint64_t* obj_ptr = static_cast<uint64_t*>(this_ptr);

    uint64_t vertex_count = obj_ptr[0x110 / 8];
    uint64_t index_count = obj_ptr[0x1b8 / 8];
    uint64_t morph_count = obj_ptr[0x148 / 8];

    Log("\n  Mesh Metadata:");
    Log("    Vertex buffer count: %llu", vertex_count);
    Log("    Index buffer count: %llu", index_count);
    Log("    Morph buffer count: %llu", morph_count);

    g_meshDumpCount++;
  }

  // Call original function
  if (g_originalDeserialize) {
    g_originalDeserialize(this_ptr, param1);
  }
}

// ============================================================================
// HOOK INITIALIZATION
// ============================================================================

VOID InitializeMeshDumpHooks() {
  if (g_hooksInitialized) {
    return;
  }

  // Check if hooks are enabled
  if (!ENABLE_HOOKS) {
    OutputDebugStringA("MeshDumpHooks: Hooks disabled (ENABLE_HOOKS = false)\n");
    return;
  }

  // Early diagnostic output
  OutputDebugStringA("MeshDumpHooks: InitializeMeshDumpHooks called\n");

  // Initialize critical section immediately for early logging
  InitializeCriticalSection(&g_logLock);

  // Create output directories
  try {
    fs::create_directories(MESH_DUMP_DIR);
    fs::create_directories(RAW_BUFFERS_DIR);
    fs::create_directories(PARSED_MESHES_DIR);
  } catch (const std::exception& e) {
    // Log error to debug output
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), "MeshDumpHooks: Failed to create directories: %s\n", e.what());
    OutputDebugStringA(error_msg);
    DeleteCriticalSection(&g_logLock);
    return;
  }

  // Open log file
  g_logFile.open(MESH_LOG_FILE, std::ios::out | std::ios::trunc);

  if (!g_logFile.is_open()) {
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), "MeshDumpHooks: WARNING - Failed to open log file: %s (continuing anyway)\n",
             MESH_LOG_FILE);
    OutputDebugStringA(error_msg);
    // Don't return - continue with hook initialization anyway
  }

  OutputDebugStringA("MeshDumpHooks: Log file handling complete, proceeding with hook init\n");

  Log("========================================");
  Log("Mesh Dump Hooks Initializing");
  Log("========================================");
  Log("Target: echovr.exe");
  Log("AsyncResourceIOCallback RVA: 0x%llx", ADDR_AsyncResourceIOCallback);
  Log("DeserializeAndUpload RVA: 0x%llx", ADDR_DeserializeAndUpload);
  Log("Output: %s", MESH_DUMP_DIR);
  Log("");

  // Check if game base address is available
  OutputDebugStringA("MeshDumpHooks: Attempting to get game base address\n");
  CHAR* gameBase = EchoVR::g_GameBaseAddress;
  if (!gameBase) {
    OutputDebugStringA("MeshDumpHooks: EchoVR::g_GameBaseAddress is NULL, trying GetModuleHandle\n");
    gameBase = (CHAR*)GetModuleHandle(NULL);
  }

  if (!gameBase) {
    Log("ERROR: Failed to get game base address");
    OutputDebugStringA("MeshDumpHooks: ERROR - Failed to get game base address (both methods returned NULL)\n");
    g_logFile.close();
    DeleteCriticalSection(&g_logLock);
    return;
  }

  char baseaddr_msg[256];
  snprintf(baseaddr_msg, sizeof(baseaddr_msg), "MeshDumpHooks: Game base address: 0x%p\n", gameBase);
  OutputDebugStringA(baseaddr_msg);
  Log("Game base address: 0x%p", gameBase);

  // Initialize MinHook (may already be initialized by other hooks in GamePatches)
  OutputDebugStringA("MeshDumpHooks: Calling MH_Initialize\n");
  MH_STATUS mh_status = MH_Initialize();
  if (mh_status != MH_OK && mh_status != MH_ERROR_ALREADY_INITIALIZED) {
    Log("ERROR: MH_Initialize failed with status: %d", mh_status);
    char error_msg[256];
    snprintf(error_msg, sizeof(error_msg), "MeshDumpHooks: ERROR - MH_Initialize failed with status: %d\n", mh_status);
    OutputDebugStringA(error_msg);
    g_logFile.close();
    DeleteCriticalSection(&g_logLock);
    return;
  }

  if (mh_status == MH_ERROR_ALREADY_INITIALIZED) {
    Log("MinHook already initialized (this is normal)");
    OutputDebugStringA("MeshDumpHooks: MH_Initialize - already initialized (OK)\n");
  } else {
    Log("MinHook initialized successfully");
    OutputDebugStringA("MeshDumpHooks: MH_Initialize succeeded\n");
  }

  // Hook 1: AsyncResourceIOCallback
  void* callback_target = gameBase + ADDR_AsyncResourceIOCallback;
  Log("Hook 1 target: 0x%p (base 0x%p + RVA 0x%llx)", callback_target, gameBase, ADDR_AsyncResourceIOCallback);

  char hook1_target_msg[256];
  snprintf(hook1_target_msg, sizeof(hook1_target_msg), "MeshDumpHooks: Hook 1 target address: 0x%p\n", callback_target);
  OutputDebugStringA(hook1_target_msg);

  OutputDebugStringA("MeshDumpHooks: Calling MH_CreateHook for Hook 1\n");
  MH_STATUS hook1_create = MH_CreateHook(callback_target, reinterpret_cast<void*>(&Hook_AsyncResourceIOCallback),
                                         reinterpret_cast<void**>(&g_originalAsyncCallback));

  char hook1_create_msg[256];
  snprintf(hook1_create_msg, sizeof(hook1_create_msg), "MeshDumpHooks: MH_CreateHook Hook 1 result: %d\n",
           hook1_create);
  OutputDebugStringA(hook1_create_msg);

  if (hook1_create == MH_OK) {
    OutputDebugStringA("MeshDumpHooks: Calling MH_EnableHook for Hook 1\n");
    MH_STATUS hook1_enable = MH_EnableHook(callback_target);

    char hook1_enable_msg[256];
    snprintf(hook1_enable_msg, sizeof(hook1_enable_msg), "MeshDumpHooks: MH_EnableHook Hook 1 result: %d\n",
             hook1_enable);
    OutputDebugStringA(hook1_enable_msg);

    if (hook1_enable == MH_OK) {
      Log("Hook 1: AsyncResourceIOCallback - SUCCESS");
      OutputDebugStringA("MeshDumpHooks: Hook 1 - SUCCESS\n");
    } else {
      Log("Hook 1: AsyncResourceIOCallback - ENABLE FAILED");
      OutputDebugStringA("MeshDumpHooks: Hook 1 - ENABLE FAILED\n");
    }
  } else {
    Log("Hook 1: AsyncResourceIOCallback - CREATE FAILED");
    OutputDebugStringA("MeshDumpHooks: Hook 1 - CREATE FAILED\n");
  }

  // Hook 2: DeserializeAndUpload
  void* deserialize_target = gameBase + ADDR_DeserializeAndUpload;
  Log("Hook 2 target: 0x%p (base 0x%p + RVA 0x%llx)", deserialize_target, gameBase, ADDR_DeserializeAndUpload);

  char hook2_target_msg[256];
  snprintf(hook2_target_msg, sizeof(hook2_target_msg), "MeshDumpHooks: Hook 2 target address: 0x%p\n",
           deserialize_target);
  OutputDebugStringA(hook2_target_msg);

  OutputDebugStringA("MeshDumpHooks: Calling MH_CreateHook for Hook 2\n");
  MH_STATUS hook2_create = MH_CreateHook(deserialize_target, reinterpret_cast<void*>(&Hook_DeserializeAndUpload),
                                         reinterpret_cast<void**>(&g_originalDeserialize));

  char hook2_create_msg[256];
  snprintf(hook2_create_msg, sizeof(hook2_create_msg), "MeshDumpHooks: MH_CreateHook Hook 2 result: %d\n",
           hook2_create);
  OutputDebugStringA(hook2_create_msg);

  if (hook2_create == MH_OK) {
    OutputDebugStringA("MeshDumpHooks: Calling MH_EnableHook for Hook 2\n");
    MH_STATUS hook2_enable = MH_EnableHook(deserialize_target);

    char hook2_enable_msg[256];
    snprintf(hook2_enable_msg, sizeof(hook2_enable_msg), "MeshDumpHooks: MH_EnableHook Hook 2 result: %d\n",
             hook2_enable);
    OutputDebugStringA(hook2_enable_msg);

    if (hook2_enable == MH_OK) {
      Log("Hook 2: DeserializeAndUpload - SUCCESS");
      OutputDebugStringA("MeshDumpHooks: Hook 2 - SUCCESS\n");
    } else {
      Log("Hook 2: DeserializeAndUpload - ENABLE FAILED");
      OutputDebugStringA("MeshDumpHooks: Hook 2 - ENABLE FAILED\n");
    }
  } else {
    Log("Hook 2: DeserializeAndUpload - CREATE FAILED");
    OutputDebugStringA("MeshDumpHooks: Hook 2 - CREATE FAILED\n");
  }

  Log("\nMesh Dump Hooks Initialized");
  Log("========================================\n");
  OutputDebugStringA("MeshDumpHooks: Initialization complete\n");

  g_hooksInitialized = true;
}

VOID ShutdownMeshDumpHooks() {
  if (!g_hooksInitialized) {
    return;
  }

  Log("\n========================================");
  Log("Mesh Dump Hooks Shutting Down");
  Log("  Total buffers dumped: %u", g_bufferDumpCount);
  Log("  Total meshes dumped: %u", g_meshDumpCount);
  Log("========================================");

  // Disable hooks
  MH_DisableHook(MH_ALL_HOOKS);
  MH_Uninitialize();

  // Close log file
  if (g_logFile.is_open()) {
    g_logFile.close();
  }

  DeleteCriticalSection(&g_logLock);
  g_hooksInitialized = false;
}
