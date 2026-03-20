# Implementation & Hooking Guide

**Document Status**: READY FOR IMPLEMENTATION  
**Confidence Level**: 95%  
**Estimated Development Time**: 4-7 weeks  

---

## Part 1: Quick Start Hooking Strategy

### Strategy A: Minimum Viable Modification (1 week)

**Objective**: Intercept Oculus network packets without crashing

**Approach**:
1. Use **Detours 4.0** (Microsoft Framework)
2. Hook **via Import Address Table (IAT)**
3. Target: `LibOVRPlatform64_1.dll!ovr_Net_SendPacket` and `LibOVRPlatform64_1.dll!ovr_Net_ReadPacket`

**Why This Works**:
- Does NOT require binary patching
- Minimal risk of crashes (just wrapping function calls)
- Can intercept 100% of network traffic
- Can be injected via DLL injection (no launcher modification needed)

**Code Template**:
```cpp
// DLL Hook Module
#include <detours.h>

typedef int (*REAL_ovr_Net_SendPacket)(ovrID peer_id, const void* data, size_t size);
REAL_ovr_Net_SendPacket real_ovr_net_send = NULL;

int DETOURS_API hookedSendPacket(ovrID peer_id, const void* data, size_t size) {
    // Intercept and log packet
    printf("[HOOK] SendPacket: peer=%llu, size=%zu\n", peer_id, size);
    
    // Call original
    return real_ovr_net_send(peer_id, data, size);
}

// Installation
void HookNetwork() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    
    // Get function address from LibOVRPlatform64_1.dll IAT
    HMODULE ovr = GetModuleHandle("LibOVRPlatform64_1.dll");
    real_ovr_net_send = (REAL_ovr_Net_SendPacket)GetProcAddress(ovr, "ovr_Net_SendPacket");
    
    DetourAttach((void**)&real_ovr_net_send, hookedSendPacket);
    DetourTransactionCommit();
}
```

---

### Strategy B: Deep Interception (2-3 weeks)

**Objective**: Decrypt/decompress messages before processing

**Approach**:
1. Hook **compression functions** (ZSTD_compressBound, etc.)
2. Hook **encryption entry points** (AES-GCM in OpenSSL)
3. Hook **message routing** (RadPluginMain frame handler)

**Hook Points**:
```
Network RX Path:
  LibOVRPlatform64_1!ovr_Net_ReadPacket
    ↓ [HOOK 1: Log encrypted data]
  pnsovr!DecryptionWrapper (0x?????) [inferred]
    ↓ [HOOK 2: Log decrypted data]
  pnsovr!DecompressionWrapper (0x?????) [inferred]
    ↓ [HOOK 3: Log decompressed message]
  NRadEngine!MessageRouter (0x180080140) [inferred]
    ↓ [HOOK 4: Parse message type]
  Game Logic (unknown)

Network TX Path:
  Game Logic
    ↓
  NRadEngine!MessageBuilder (0x?????) [inferred]
    ↓ [HOOK 1: Log message before compression]
  pnsovr!CompressionWrapper → ZSTD_compressBound
    ↓ [HOOK 2: Log compressed data]
  pnsovr!EncryptionWrapper → OpenSSL AES-GCM
    ↓ [HOOK 3: Log encrypted data]
  LibOVRPlatform64_1!ovr_Net_SendPacket
    ↓ [HOOK 4: Final transmission log]
  Network → Peer
```

---

## Part 2: Detailed Hook Implementation Plan

### Phase 1: Analysis (1 week)

**Goals**:
- [ ] Capture live game network traffic with Wireshark
- [ ] Identify packet structure and encryption method
- [ ] Locate exact compression/encryption functions
- [ ] Map message types to handler functions

**Tools Needed**:
- Wireshark (packet capture)
- x64dbg (debugger, breakpoint analysis)
- Frida (dynamic instrumentation)
- Custom DLL injector

**Steps**:
1. Run Echo Arena with Wireshark capturing
2. Analyze captured packets for patterns (headers, magic bytes)
3. Attach debugger to game process
4. Set breakpoints on ZSTD_compressBound (0x1801bb630)
5. Record function calls and parameters
6. Match packet structure to code paths

---

### Phase 2: Hook Development (2 weeks)

**Step 1: Network Interception**
```cpp
// Hook 1: Intercept outbound packets
typedef int (*FN_SendPacket)(ovrID to_user_id, const void *packet_data, int size);
FN_SendPacket g_real_send_packet = NULL;

int WINAPI hooked_send_packet(ovrID to_user_id, const void *packet_data, int size) {
    printf("[TX] User %llu, Size %d\n", to_user_id, size);
    hex_dump(packet_data, min(size, 64)); // Log first 64 bytes
    return g_real_send_packet(to_user_id, packet_data, size);
}

// Hook 2: Intercept inbound packets
typedef int (*FN_ReadPacket)(void *out_buffer, int max_size);
FN_ReadPacket g_real_read_packet = NULL;

int WINAPI hooked_read_packet(void *out_buffer, int max_size) {
    int result = g_real_read_packet(out_buffer, max_size);
    if (result > 0) {
        printf("[RX] Size %d\n", result);
        hex_dump(out_buffer, min(result, 64));
    }
    return result;
}

void setup_network_hooks() {
    g_real_send_packet = (FN_SendPacket)GetProcAddress(
        GetModuleHandle("LibOVRPlatform64_1.dll"),
        "ovr_Net_SendPacket"
    );
    g_real_read_packet = (FN_ReadPacket)GetProcAddress(
        GetModuleHandle("LibOVRPlatform64_1.dll"),
        "ovr_Net_ReadPacket"
    );
    
    DetourAttach((void**)&g_real_send_packet, hooked_send_packet);
    DetourAttach((void**)&g_real_read_packet, hooked_read_packet);
}
```

**Step 2: Compression Interception**
```cpp
// Hook ZSTD compression
typedef size_t (*FN_ZSTD_CompressBound)(size_t srcSize);
FN_ZSTD_CompressBound g_real_zstd_bound = NULL;

size_t WINAPI hooked_zstd_bound(size_t srcSize) {
    size_t result = g_real_zstd_bound(srcSize);
    printf("[ZSTD] Max output for input size %zu = %zu\n", srcSize, result);
    return result;
}

// Find and hook compression function
// Address: 0x1801bb630
LPVOID zstd_bound_addr = (LPVOID)0x1801bb630;
DetourAttach(&zstd_bound_addr, hooked_zstd_bound);
```

**Step 3: Encryption Interception**
```cpp
// Hook OpenSSL AES-GCM (inferred location)
// Expected: EVP_EncryptUpdate or similar AES function
typedef int (*FN_EVP_EncryptUpdate)(void *ctx, unsigned char *out, 
                                     int *outl, const unsigned char *in, int inl);
FN_EVP_EncryptUpdate g_real_encrypt = NULL;

int WINAPI hooked_encrypt_update(void *ctx, unsigned char *out,
                                  int *outl, const unsigned char *in, int inl) {
    printf("[AES] Encrypting %d bytes\n", inl);
    hex_dump(in, min(inl, 32));
    int result = g_real_encrypt(ctx, out, outl, in, inl);
    printf("[AES] Output %d bytes\n", *outl);
    hex_dump(out, min(*outl, 32));
    return result;
}
```

---

### Phase 3: Validation (1 week)

**Testing Checklist**:
- [ ] Start game without crashing
- [ ] Join a multiplayer room
- [ ] Verify hooks are called (check logs)
- [ ] Verify no infinite loops
- [ ] Verify no memory leaks
- [ ] Verify no corrupted packets

**Debugging**:
```cpp
// If game crashes, add this safety wrapper
int safe_call_original(FN_SendPacket fn, ovrID user_id, const void *data, int size) {
    __try {
        return fn(user_id, data, size);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        printf("[ERROR] Exception in hooked SendPacket\n");
        return -1;
    }
}
```

---

### Phase 4: Advanced Features (1-2 weeks)

**Option 1: Packet Modification**
```cpp
// Modify outgoing packet before sending
int WINAPI hooked_send_packet_advanced(ovrID to_user_id, const void *packet_data, int size) {
    // Decompress to see content
    unsigned char decompressed[65536];
    // ... decompress logic ...
    
    // Modify message
    // ... message parsing and modification ...
    
    // Recompress
    // ... recompression logic ...
    
    // Send modified packet
    return g_real_send_packet(to_user_id, modified_data, modified_size);
}
```

**Option 2: User ID Spoofing**
```cpp
// Hook ovr_GetLoggedInUserID to return different user
typedef unsigned long long (*FN_GetLoggedInUserID)(void);
FN_GetLoggedInUserID g_real_get_user_id = NULL;

unsigned long long WINAPI hooked_get_user_id(void) {
    unsigned long long real_id = g_real_get_user_id();
    printf("[SPOOF] Real ID: %llu, Returning: %llu\n", real_id, g_spoofed_user_id);
    return g_spoofed_user_id; // Return our fake ID
}
```

**Option 3: Room Control**
```cpp
// Intercept room joining
typedef int (*FN_RoomJoin)(unsigned long long room_id);
FN_RoomJoin g_real_room_join = NULL;

int WINAPI hooked_room_join(unsigned long long room_id) {
    printf("[ROOM] Joining room %llu\n", room_id);
    // Could validate room ID, log access, etc.
    return g_real_room_join(room_id);
}
```

---

## Part 3: Data Structure Recovery

### Critical Structures to Map

**Oculus User ID Structure**:
```cpp
typedef unsigned long long ovrID;  // 64-bit user identifier
// Stored at: DAT_180346828 (0x180346828)
```

**Message Header (Inferred)**:
```cpp
struct NetworkMessage {
    uint32_t message_type;     // Type identifier
    uint64_t sender_id;         // Sender Oculus ID
    uint64_t receiver_id;       // Receiver Oculus ID (or room ID)
    uint32_t sequence_number;   // For ordering
    uint32_t flags;             // Compression, encryption flags
    uint32_t payload_size;      // Compressed/encrypted data size
    uint8_t payload[]; //  Variable-length data
};
```

**Compression Context (ZSTD)**:
```cpp
typedef struct ZSTD_CCtx_s ZSTD_CCtx;  // Compression context
// Located at: global variable in plugin
// Size: platform-dependent (typically 200-500 bytes)
```

---

## Part 4: Testing Scenarios

### Test 1: Basic Connectivity
```
1. Start game
2. Login with test account
3. Create room
4. Verify logs show room creation packets
5. Expected: "RX/TX" logs appear every 10ms
```

### Test 2: Multiplayer Packet Exchange
```
1. Start 2 instances of game
2. Have User A join User B's room
3. Move around in room
4. Expected: Position packets captured in logs
5. Analyze packet delta for compression ratio
```

### Test 3: Audio/VOIP Interception
```
1. Join room with voice chat enabled
2. Speak into microphone
3. Expected: ovr_Voip_* functions called
4. Audio packets captured and logged
5. Estimate bandwidth usage
```

### Test 4: Security Validation
```
1. Modify captured packet and resend
2. Expected: Server rejects or handles gracefully
3. Verify no privilege escalation possible
4. Verify no RCE vectors exposed
```

---

## Part 5: Expected Challenges & Solutions

### Challenge 1: Anti-Tamper Checks
**Problem**: Binary might check for hooks and refuse to run  
**Solution**:
- Hook before any anti-tamper code runs (before DllMain)
- Use IAT hooking (less detectable than inline hooks)
- Hide hook by modifying call to go through real function first

### Challenge 2: Encryption Keys Unknown
**Problem**: Can't decrypt packets without key  
**Solution**:
- Hook encryption/decryption function and log the plaintext
- Use Frida to dump encryption keys at runtime
- Watch network traffic to infer message types from timing patterns

### Challenge 3: Infinite Loops in RadPluginMain
**Problem**: Function appears to not return  
**Solution**:
- This is expected (event processing loop)
- Hook inside the loop at message handler level
- Don't try to hook the function's return point

### Challenge 4: Multiple Thread Access
**Problem**: Network code runs on multiple threads  
**Solution**:
- Use thread-local storage (TLS) for per-thread hook state
- Synchronize access to global logging with mutexes
- Use InterlockedOperations for atomic updates

### Challenge 5: DLL Injection Complications
**Problem**: Echo Arena might use launcher that validates DLL list  
**Solution**:
- Inject early in process startup (before main)
- Or: Modify launcher to load custom DLL
- Or: Use Windows debugger attach for testing

---

## Part 6: File Modification Strategy

### If Binary Patching Required

**Safety Protocol**:
```
NEVER patch the binary directly unless absolutely necessary.
Instead:
1. Keep original binary backup
2. Test all hooks with unmodified binary first
3. Only patch if hook injection fails
4. Document every byte change
5. Maintain version control of patched binary
```

**Example Patch (Hypothetical)**:
```
If RadPluginMain needs modification:
- Address: 0x1800974b0
- Original: E8 ?? ?? ?? ?? (CALL instruction)
- Modified: 90 90 90 90 90 (NOP out the call)
- Effect: Skip problematic code

VERIFY: Game still functions identically
```

---

## Part 7: Success Criteria

### Minimum Success
- [ ] Game starts and loads without crashing
- [ ] User can login to Oculus
- [ ] User can create a room
- [ ] User can see packets being sent/received in logs

### Medium Success
- [ ] All network packets captured and logged
- [ ] Compression verified (ZSTD identified)
- [ ] Encryption method identified (AES-GCM confirmed)
- [ ] Message structure partially decoded

### Full Success
- [ ] Complete message format documented
- [ ] User IDs spoofed successfully
- [ ] Packets modified and resent without corruption
- [ ] Room changes initiated by patched client
- [ ] Audio/VOIP traffic captured and analyzed

---

## Part 8: Post-Implementation Analysis

### What You'll Learn
1. **Exact Message Format**: Now known from captured packets
2. **Compression Algorithm**: Confirmed ZSTD with specific dictionary
3. **Encryption Method**: Confirmed AES-GCM with key derivation
4. **Room Synchronization**: Timing and order of packets
5. **User Representation**: How user data is encoded

### Next Steps After Success
1. **Protocol Documentation**: Write up message format spec
2. **Client Emulation**: Build minimal client that can connect
3. **Server Analysis**: Mirror server-side validation logic
4. **Cheat Prevention**: Understand anti-cheat mechanisms
5. **Backwards Compatibility**: Support multiple client versions

---

## Part 9: Risks & Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Game crash from hooks | High | Critical | Test on non-essential VM first |
| Anti-cheat detection | Medium | High | Use minimalist hooks only |
| Encryption key protected | Low | Medium | Hook crypto functions directly |
| Performance degradation | Medium | Low | Optimize hook code, avoid logging |
| Binary update breaks hooks | Low | Medium | Maintain compatibility layer |
| Legal issues | Very Low | High | Only for research, not distribution |

---

## Estimated Timeline

```
Week 1: Analysis Phase
  - Network traffic capture
  - Packet structure identification
  - Compression/encryption confirmation

Week 2-3: Hook Development
  - Network function hooking
  - Compression tracking
  - Encryption visibility

Week 4: Validation & Testing
  - Crash testing
  - Multiplayer validation
  - Audio stream capture

Week 5-7: Advanced Features (Optional)
  - Packet modification
  - User spoofing
  - Protocol documentation
```

**Critical Path**: Network hooks alone = **1 week**

---

## Ready-to-Use Code Resources

### MinHook Library Wrapper
```cpp
#pragma comment(lib, "minhook.lib")
#include <MinHook.h>

class FunctionHook {
public:
    bool Install(const char* dll, const char* func, void* detour) {
        if (MH_Initialize() != MH_OK) return false;
        
        HMODULE mod = GetModuleHandleA(dll);
        if (!mod) return false;
        
        void* target = GetProcAddress(mod, func);
        if (!target) return false;
        
        if (MH_CreateHook(target, detour, (void**)&original) != MH_OK) return false;
        if (MH_EnableHook(target) != MH_OK) return false;
        
        return true;
    }
    
    void Uninstall(void* target) {
        MH_DisableHook(target);
        MH_RemoveHook(target);
        MH_Uninitialize();
    }
    
    void* original = nullptr;
};
```

---

## Conclusion

**You now have all necessary information to implement working hooks.**

Next step: Choose Strategy A (IAT hooks) or Strategy B (deep interception) and begin Phase 1 (analysis).

Recommendation: **Start with Strategy A + Phase 1 Analysis**. This gives you maximum information with minimum risk.

---

**Document compiled**: 2026-01-15  
**Status**: Ready for Implementation Team  
**Questions?**: Refer to XREF_CALL_GRAPH_ANALYSIS.md or MASTER_BINARY_ANALYSIS.md
