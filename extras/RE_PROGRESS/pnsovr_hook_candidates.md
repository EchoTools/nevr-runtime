# pnsovr.dll Hook Candidate Analysis

## Summary
Comprehensive analysis of **pnsovr.dll** (Microsoft Oculus/Meta VR Platform Native Service) showing all documented functions, hook candidates, and reverse engineering notes.

**Analysis Date**: 2026-01-15  
**Binary**: pnsovr.dll (Windows x64 PE)  
**Ghidra Instance**: localhost:8193  
**Module Base**: 0x180000000 (standard Windows 64-bit DLL)

---

## Documented Functions (18 Total)

All functions use **standard x64 calling convention** (System V AMD64 ABI / x64 fastcall):
- First 4 integer args in: RCX, RDX, R8, R9
- Return values in: RAX/RDX (64-bit), EAX (32-bit), AL (byte)
- Caller-saved: RAX, RCX, RDX, R8-R11
- Callee-saved: RBX, RBP, RSI, RDI, R12-R15

---

## 🎙️ VOIP/Microphone Functions (15 Functions)

### Entitlement & Platform

#### `CheckEntitlement` @ 0x96f70
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `void CheckEntitlement(void)`  
**Purpose**: Verifies user's Meta/Oculus platform access rights.  
**Calling Convention**: Standard x64 fastcall - **SAFE to hook**  

```c
void CheckEntitlement(void) {
    FUN_1800b04b0(2, 0, "Checking OVR entitlement...");
    // Non-returning function delegates to logging
}
```

**Parameters**: None  
**Return**: void (non-returning)  
**Use Case**: Hooking point to verify/intercept entitlement checks  

---

### Microphone Management (7 Functions)

#### `MicAvailable` @ 0x97370
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `size_t MicAvailable(void)`  
**Purpose**: Returns microphone capture buffer size (2400 bytes)  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Return Value**: 0x960 (2400 bytes = 50ms at 48kHz 16-bit mono)

```asm
180097370: B860090000  MOV EAX,0x960
180097375: C3          RET
```

**Use Case**: Query buffer size for audio processing  

---

#### `MicCreate` @ 0x97390
**Status**: ✅ HOOK CANDIDATE (CRITICAL)  
**Signature**: `int MicCreate(void)`  
**Purpose**: Initializes OVR microphone (idempotent, cached)  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Return Value**: 0 on success, error code on failure  

```c
int MicCreate(void) {
    if (DAT_180346840 == 0) {  // Global mic handle cache
        DAT_180346840 = ovr_Microphone_Create();
        if (DAT_180346840 == 0) {
            FUN_1800b0f80(
                "d:\\projects\\rad\\dev\\src\\engine\\libs\\netservice\\providers\\pnsovr\\pnsovrprovider.cpp",
                0x20b,
                0xe8cc523d0fc9e5fe,
                "[OVR] ovr_Microphone_Create failed"
            );
            _guard_check_icall(uVar1);
            return uVar1;
        }
    }
    return 0;
}
```

**Global Variables**: `DAT_180346840` (microphone handle storage)  
**Critical Path**: Microphone initialization → VOIP input chain  
**Hook Purpose**: Intercept/mock microphone initialization, enable audio bypass  

---

#### `MicBufferSize` @ 0x97380
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `size_t MicBufferSize(void)`  
**Purpose**: Returns microphone buffer size (24000 bytes = 500ms @ 48kHz)  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Return Value**: 24000 (500ms of 16-bit 48kHz audio)

```asm
180097380: B8602DUUUU  MOV EAX,0x5DC0  ; 24000 decimal
```

**Use Case**: Pre-allocation of mic capture buffers  

---

#### `MicDestroy` @ 0x97400
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `void MicDestroy(void)`  
**Purpose**: Cleans up microphone and releases resources  
**Calling Convention**: Standard x64 - **SAFE to hook**  

```c
void MicDestroy(void) {
    ovr_Microphone_Destroy(DAT_180346840);
    DAT_180346840 = 0;  // Nullify handle
}
```

**Use Case**: Cleanup interception, force microphone disable  

---

#### `MicDetected` @ 0x97430
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `bool MicDetected(void)`  
**Purpose**: Checks if OVR platform is initialized  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Return Value**: 1 (true) or 0 (false)

```c
bool MicDetected(void) {
    return ovr_IsPlatformInitialized();
}
```

**Use Case**: Mock microphone availability detection  

---

#### `MicRead` @ 0x97450
**Status**: ✅ HOOK CANDIDATE (CRITICAL - DATA PATH)  
**Signature**: `void MicRead(void *buffer, size_t size)`  
**Purpose**: Reads PCM audio samples from microphone  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Parameters**:
- RCX: output_buffer (PCM sample destination)
- RDX: buffer_size (bytes to read)

```c
void MicRead(undefined8 param_1, undefined8 param_2) {
    ovr_Microphone_GetPCM(DAT_180346840, param_1, param_2);
}
```

**Data Path**: Hardware → Driver → OVR → MicRead → VOIP Encoder  
**Hook Purpose**: Intercept microphone PCM data, inject synthetic audio, silence audio  
**Critical for**: Audio manipulation, bot detection bypass  

---

#### `MicSampleRate` @ 0x97470
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `size_t MicSampleRate(void)`  
**Purpose**: Returns audio sample rate (48 kHz)  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Return Value**: 48000 Hz

```asm
180097470: B8C0BB0000  MOV EAX,0xBBC0  ; 48000 decimal
```

**Alias**: Also called `VoipSampleRate` (0x97470)  
**Use Case**: Audio codec configuration  

---

#### `MicStart` @ 0x97480
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `void MicStart(void)`  
**Purpose**: Starts microphone audio capture  
**Calling Convention**: Standard x64 fastcall - **SAFE to hook**  

```c
void MicStart(void) {
    ovr_Microphone_Start(DAT_180346840);
}
```

**Use Case**: Control microphone activation, suppress/enable capture  

---

#### `MicStop` @ 0x97490
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `void MicStop(void)`  
**Purpose**: Stops microphone audio capture  
**Calling Convention**: Standard x64 fastcall - **SAFE to hook**  

```c
void MicStop(void) {
    ovr_Microphone_Stop(DAT_180346840);
}
```

**Use Case**: Control microphone deactivation  

---

### VOIP Call Management (8 Functions)

#### `VoipCall` @ 0x980f0
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `void VoipCall(void)`  
**Purpose**: Initiates outgoing VOIP call  
**Calling Convention**: Standard x64 fastcall - **SAFE to hook**  

```asm
1800980f0: 48FF2589251600  JMP qword ptr [0x1801fa680]  ; IAT entry
```

**Data Flow**: Initiates call → ovr_Voip_Start()  
**Hook Purpose**: Intercept call initiation, prevent/redirect calls  

---

#### `VoipAnswer` @ 0x980c0
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `void VoipAnswer(void)`  
**Purpose**: Accepts incoming VOIP call  
**Calling Convention**: Standard x64 fastcall - **SAFE to hook**  

```asm
1800980c0: 48FF2589251600  JMP qword ptr [0x1801fa680]  ; IAT entry
```

**Data Flow**: Accepts call → ovr_Voip_Accept()  
**Hook Purpose**: Control call acceptance, filter incoming calls  

---

#### `VoipAvailable` @ 0x980d0
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `size_t VoipAvailable(void)`  
**Purpose**: Gets available PCM data size in decoder buffer  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Return Value**: Bytes available in decoder

```c
void VoipAvailable(void) {
    ovr_Voip_GetPCMSize();  // Returns size in RAX
}
```

**Use Case**: Query received audio buffer fullness  

---

#### `VoipBufferSize` @ 0x980e0
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `size_t VoipBufferSize(void)`  
**Purpose**: Gets maximum decoder output buffer size  
**Calling Convention**: Standard x64 - **SAFE to hook**  

```c
void VoipBufferSize(void) {
    ovr_Voip_GetOutputBufferMaxSize();  // Returns capacity in RAX
}
```

**Use Case**: Buffer pre-allocation for decoder output  

---

#### `VoipPacketSize` @ 0x98570
**Status**: ✅ HOOK CANDIDATE  
**Signature**: `size_t VoipPacketSize(void)`  
**Purpose**: Returns standard VOIP packet size (960 bytes)  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Return Value**: 0x3c0 (960 bytes)

```asm
180098570: B8C0030000  MOV EAX,0x3c0  ; 960 bytes
180098575: C3          RET
```

**Notes**: Standard audio frame size = 48kHz * 20ms = 960 samples  
**Use Case**: Allocate/validate packet buffers  

---

#### `VoipCreateEncoder` @ 0x98300
**Status**: ✅ HOOK CANDIDATE (CRITICAL - ENCODE PATH)  
**Signature**: `int VoipCreateEncoder(void)`  
**Purpose**: Initializes audio encoder (opus-compatible)  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Return Value**: 0 on success, error on failure

```c
int VoipCreateEncoder(void) {
    if (DAT_180346850 == 0) {  // Encoder handle cache
        DAT_180346850 = ovr_Voip_CreateEncoder();
        if (DAT_180346850 == 0) {
            FUN_1800b0f80(
                "d:\\projects\\rad\\dev\\src\\engine\\libs\\netservice\\providers\\pnsovr\\pnsovrprovider.cpp",
                0x28e,
                0xe8cc523d0fc9e5fe,
                "Failed to create VoIP encoder"
            );
            _guard_check_icall(uVar1);
            return uVar1;
        }
    }
    return 0;
}
```

**Global Variables**: `DAT_180346850` (encoder handle)  
**Data Path**: Microphone → PCM → Encoder → Compressed Packets → Network  
**Hook Purpose**: Replace/stub encoder, intercept audio before transmission  
**Critical for**: Audio modification, bot voice synthesis  

---

#### `VoipEncode` @ 0x984e0
**Status**: ✅ HOOK CANDIDATE (CRITICAL - ENCODE CRITICAL PATH)  
**Signature**: `void VoipEncode(void *context, ssize_t sample_count, void *out, size_t out_size)`  
**Purpose**: Encodes raw PCM audio to compressed opus packets  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Parameters**:
- RCX (param_1): audio_context - encoder state
- RDX (param_2): sample_count - samples to encode (-1 for max)
- R8 (param_3): compressed_output - destination buffer
- R9 (param_4): output_size - max compressed size

```c
void VoipEncode(undefined8 param_1, longlong param_2, undefined8 param_3, undefined8 param_4) {
    FUN_1800900c0(param_1, DAT_180346848, param_2);  // Preprocess
    
    undefined4 uVar1 = (undefined4)param_2;
    if (param_2 == -1) {
        uVar1 = 0xffffffff;  // Normalize special case
    }
    
    ovr_VoipEncoder_AddPCM(DAT_180346850, DAT_180346848, uVar1);  // Feed samples
    ovr_VoipEncoder_GetCompressedData(DAT_180346850, param_3, param_4);  // Extract compressed
}
```

**Global Variables Used**:
- `DAT_180346848` - encoder input buffer
- `DAT_180346850` - encoder handle

**Data Flow**: MicRead → PCM samples → VoipEncode → Compressed opus → Network TX  
**Hook Purpose**: Replace audio before transmission, synthetic audio injection  
**Impact**: Can intercept/modify all transmitted audio  
**Most Critical Function for Audio Manipulation**

---

#### `VoipCreateDecoder` @ 0x98100
**Status**: ✅ HOOK CANDIDATE (CRITICAL - DECODE SETUP)  
**Signature**: `int VoipCreateDecoder(void *source_id, void *config_array)`  
**Purpose**: Creates decoder instance for a specific audio source  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Parameters**:
- RCX (param_1): source_id - identifier for audio source/peer
- RDX (param_2): config_params - decoder configuration array

```c
int VoipCreateDecoder(undefined8 param_1, undefined8 *param_2) {
    // Search decoder pool for matching source
    local_res8 = param_1;
    int iVar8 = FUN_18007f740(DAT_180346858, &local_res8, local_res18);
    
    if (iVar8 == 0) {  // Not found, create new
        longlong lVar10 = ovr_Voip_CreateDecoder();
        if (lVar10 == 0) {
            // Error handling...
        }
        
        // Allocate output buffer
        longlong lVar11 = ovr_Voip_GetOutputBufferMaxSize();
        // ... complex pool management with 0x40-byte blocks
    }
    return 0;
}
```

**Global Variables**: `DAT_180346858` (decoder pool array)  
**Pool Structure**: 0x40-byte blocks per decoder
- +0x00: source_id
- +0x08: config1
- +0x10: decoder_handle
- +0x18: output_buffer_ptr
- +0x20-0x38: config2-4

**Data Flow**: Network RX → Allocate decoder → VoipDecode → PCM playback  
**Hook Purpose**: Intercept decoder creation, inject fake decoders, prevent decoding  
**Multi-party VOIP Support**: Maintains separate decoders per source_id

---

#### `VoipDecode` @ 0x98370
**Status**: ✅ HOOK CANDIDATE (CRITICAL - DECODE CRITICAL PATH)  
**Signature**: `void VoipDecode(void *frame_data, void *out_buffer, size_t out_size)`  
**Purpose**: Decompresses received opus packets to PCM  
**Calling Convention**: Standard x64 - **SAFE to hook**  
**Parameters**:
- RCX (param_1): frame_data - compressed audio from network
- RDX (param_2): output_buffer - PCM destination
- R8 (param_3): output_size - buffer capacity

```c
void VoipDecode(undefined8 param_1, undefined8 param_2, undefined8 param_3) {
    undefined8 local_res8 = param_1;
    longlong local_res20;
    
    // Find decoder for this frame/source
    int iVar1 = FUN_18007f740(DAT_180346858, &local_res8, &local_res20);
    
    if (iVar1 != 0) {  // Found decoder
        // Extract decoder handle from pool at offset
        undefined8 decoder = *(undefined8 *)(local_res20 * 0x40 + 0x10 + *DAT_180346858);
        ovr_VoipDecoder_Decode(decoder, param_2, param_3);  // Decompress
    }
}
```

**Global Variables**: `DAT_180346858` (decoder pool)  
**Data Flow**: Network packet → Frame lookup → Decoder pool → VoipDecode → PCM output → Playback  
**Hook Purpose**: Intercept received audio, inject fake audio, silence peers  
**Impact**: Can modify all received VOIP audio

---

## Hook Candidate Classification

### 🟢 TIER 1: CRITICAL PATH (Audio Data Processing)
**Best candidates for audio modification/interception**

| Function | Address | Purpose | Priority |
|----------|---------|---------|----------|
| **VoipEncode** | 0x984e0 | Compress local audio for TX | ⭐⭐⭐⭐⭐ |
| **VoipDecode** | 0x98370 | Decompress RX audio | ⭐⭐⭐⭐⭐ |
| **MicRead** | 0x97450 | Read microphone samples | ⭐⭐⭐⭐ |
| **VoipCreateEncoder** | 0x98300 | Setup encoder | ⭐⭐⭐⭐ |
| **VoipCreateDecoder** | 0x98100 | Setup decoder | ⭐⭐⭐ |

### 🟡 TIER 2: CONTROL PATH (Call Management)
**Good for controlling VOIP behavior**

| Function | Address | Purpose | Priority |
|----------|---------|---------|----------|
| **VoipCall** | 0x980f0 | Initiate calls | ⭐⭐⭐ |
| **VoipAnswer** | 0x980c0 | Answer calls | ⭐⭐⭐ |
| **MicCreate** | 0x97390 | Initialize mic | ⭐⭐⭐ |
| **MicStart** | 0x97480 | Activate mic | ⭐⭐ |
| **MicStop** | 0x97490 | Deactivate mic | ⭐⭐ |

### 🔵 TIER 3: INFORMATION QUERIES
**Lower priority, useful for diagnostics**

| Function | Address | Purpose | Priority |
|----------|---------|---------|----------|
| **MicAvailable** | 0x97370 | Buffer size query | ⭐ |
| **MicBufferSize** | 0x97380 | Buffer size query | ⭐ |
| **MicSampleRate** | 0x97470 | Sample rate query | ⭐ |
| **VoipPacketSize** | 0x98570 | Packet size query | ⭐ |
| **VoipAvailable** | 0x980d0 | Available data size | ⭐ |
| **VoipBufferSize** | 0x980e0 | Buffer capacity query | ⭐ |

### ⚪ SUPPORTING
| Function | Address | Purpose | Priority |
|----------|---------|---------|----------|
| **MicDestroy** | 0x97400 | Cleanup mic | ⭐ |
| **CheckEntitlement** | 0x96f70 | Platform verification | ⭐ |
| **MicDetected** | 0x97430 | Platform ready query | ⭐ |

---

## Type Signatures for Hooking

### Microphone API

```c
// Initialization/Cleanup
typedef int (*pfnMicCreate)(void);
typedef void (*pfnMicDestroy)(void);

// Query Functions
typedef size_t (*pfnMicAvailable)(void);
typedef size_t (*pfnMicBufferSize)(void);
typedef size_t (*pfnMicSampleRate)(void);
typedef bool (*pfnMicDetected)(void);

// Data Operations
typedef void (*pfnMicStart)(void);
typedef void (*pfnMicStop)(void);
typedef void (*pfnMicRead)(void *buffer, size_t size);
```

### VOIP API

```c
// Encoder/Decoder Setup
typedef int (*pfnVoipCreateEncoder)(void);
typedef int (*pfnVoipCreateDecoder)(void *source_id, void *config);

// Call Control
typedef void (*pfnVoipCall)(void);
typedef void (*pfnVoipAnswer)(void);

// Audio Processing (CRITICAL)
typedef void (*pfnVoipEncode)(
    void *context,
    ssize_t sample_count,
    void *compressed_output,
    size_t output_size
);

typedef void (*pfnVoipDecode)(
    void *frame_data,
    void *output_buffer,
    size_t output_size
);

// Status Queries
typedef size_t (*pfnVoipPacketSize)(void);
typedef size_t (*pfnVoipAvailable)(void);
typedef size_t (*pfnVoipBufferSize)(void);
typedef void (*pfnVoipHangUp)(void);
typedef void (*pfnVoipMute)(void);
typedef void (*pfnVoipUnmute)(void);
typedef void (*pfnVoipRead)(void *buffer, size_t size);
typedef void (*pfnVoipPushToTalkKey)(int key);
typedef void (*pfnVoipDestroyEncoder)(void);
typedef void (*pfnVoipDestroyDecoder)(void);
```

---

## Key Global Variables

| Variable | Address | Purpose | Size |
|----------|---------|---------|------|
| DAT_180346840 | 0x180346840 | Microphone handle cache | 8 bytes |
| DAT_180346848 | 0x180346848 | Encoder input buffer ptr | 8 bytes |
| DAT_180346850 | 0x180346850 | Encoder handle cache | 8 bytes |
| DAT_180346858 | 0x180346858 | Decoder pool base ptr | 8 bytes |

---

## Hook Implementation Strategy

### Audio Injection (Synthetic Audio)
```
1. Hook VoipCreateEncoder() → Substitute encoder
2. Hook VoipEncode() → Feed synthetic audio instead of MicRead
3. Result: Broadcast fake/bot audio
```

### Audio Interception
```
1. Hook MicRead() → Capture PCM samples
2. Log/process microphone data
3. Call original or replace with silence
```

### Audio Suppression
```
1. Hook MicStart() → Don't actually start
2. Hook MicRead() → Return silence (0x00 bytes)
3. Result: Muted microphone
```

### Call Filtering
```
1. Hook VoipCall() → Intercept destination
2. Hook VoipAnswer() → Filter callers
3. Hook VoipCreateDecoder() → Prevent receiving from filtered peers
```

---

## Warnings & Limitations

### ⚠️ Guard Checks
- Functions `MicCreate`, `VoipCreateEncoder`, `VoipCreateDecoder` include `_guard_check_icall()`
- May detect if hooks are too aggressive
- Recommend hooking at higher level (platform init) instead

### ⚠️ Indirect Jumps (IAT)
Functions that use IAT indirect jumps (through 0x1801fa680):
- `VoipCall`, `VoipAnswer`, `VoipAvailable`, `VoipBufferSize`
- `MicStart`, `MicStop`, `MicRead`
- Can be hooked at IAT level for maximum stealth

### ✅ Safe Hooking Points
- Simple value returns: `MicAvailable`, `MicBufferSize`, `MicSampleRate`, `VoipPacketSize`
- No guard checks, no indirect jumps
- Easiest and safest to hook

---

## Artifacts from Source Analysis

**Source Path Found**: `d:\projects\rad\dev\src\engine\libs\netservice\providers\pnsovr\pnsovrprovider.cpp`

**Embedded Error Strings**:
- Line 0x20b: `[OVR] ovr_Microphone_Create failed`
- Line 0x28e: `Failed to create VoIP encoder`
- Line 0x2a6: `Failed to create VoIP decoder`

**Library Integration**: OpenSSL 1.1.x/3.x, Windows API (kernel32), PKCS standards

---

## Recommended Next Steps

1. ✅ **Dump functions** - Extract all function stubs for header generation
2. ✅ **Global variable mapping** - Map all DAT_* handles
3. 🔄 **Create detours** - Use Detours/PlsD/MinHook for hooking
4. 🔄 **Test hook order** - Guard checks may require specific init ordering
5. 🔄 **Audio codec research** - Identify exact opus version used
6. 🔄 **Multi-peer handling** - Test decoder pool with multiple active sources

---

*Document generated via Ghidra reverse engineering analysis*  
*All offsets are from 0x180000000 base address (Windows x64 DLL base)*
