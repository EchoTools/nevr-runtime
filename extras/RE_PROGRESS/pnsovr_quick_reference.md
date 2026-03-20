# pnsovr.dll Quick Reference - Hook Candidates

## Function Address Map

All offsets are from image base 0x180000000 (Windows x64 DLL base).  
To use in hooks, subtract 0x180000000 or use directly in Ghidra.

### Critical Audio Path Functions

```
VOIP ENCODE (TX) - Most important for audio injection
├─ 0x98300: VoipCreateEncoder()     - Setup encoder (initialize once)
├─ 0x984e0: VoipEncode()            - CRITICAL: Compress & TX audio
│           Parameters: (context, sample_count, out_buf, out_size)
│           Using: DAT_180346850 (encoder handle), DAT_180346848 (buffer)
│
MICROPHONE INPUT - Source for VOIP audio
├─ 0x97390: MicCreate()             - Initialize microphone
├─ 0x97480: MicStart()              - Activate capture
├─ 0x97450: MicRead()               - CRITICAL: Get mic samples (source)
│           Parameters: (output_buffer, buffer_size)
│           Using: DAT_180346840 (mic handle)
└─ 0x97490: MicStop()               - Deactivate capture

VOIP DECODE (RX) - For receiving audio from peers
├─ 0x98100: VoipCreateDecoder()     - Setup decoder per source
├─ 0x98370: VoipDecode()            - CRITICAL: Decompress RX audio
│           Parameters: (frame_data, output_buffer, output_size)
│           Using: DAT_180346858 (decoder pool)
```

### All Documented Functions (18 Total)

| Priority | Address  | Function Name | Size | Type |
|----------|----------|---------------|------|------|
| ⭐⭐⭐⭐⭐ | 0x984e0  | VoipEncode           | large | CRITICAL |
| ⭐⭐⭐⭐⭐ | 0x98370  | VoipDecode           | large | CRITICAL |
| ⭐⭐⭐⭐ | 0x97450  | MicRead              | tiny  | CRITICAL |
| ⭐⭐⭐⭐ | 0x98300  | VoipCreateEncoder    | med   | Setup |
| ⭐⭐⭐ | 0x98100  | VoipCreateDecoder    | huge  | Setup |
| ⭐⭐⭐ | 0x980f0  | VoipCall             | tiny  | Control |
| ⭐⭐⭐ | 0x980c0  | VoipAnswer           | tiny  | Control |
| ⭐⭐⭐ | 0x97390  | MicCreate            | med   | Setup |
| ⭐⭐ | 0x97480  | MicStart             | tiny  | Control |
| ⭐⭐ | 0x97490  | MicStop              | tiny  | Control |
| ⭐ | 0x97370  | MicAvailable         | tiny  | Query |
| ⭐ | 0x97380  | MicBufferSize        | tiny  | Query |
| ⭐ | 0x97470  | MicSampleRate        | tiny  | Query |
| ⭐ | 0x98570  | VoipPacketSize       | tiny  | Query |
| ⭐ | 0x980d0  | VoipAvailable        | tiny  | Query |
| ⭐ | 0x980e0  | VoipBufferSize       | tiny  | Query |
| ⭐ | 0x97400  | MicDestroy           | tiny  | Cleanup |
| ⭐ | 0x96f70  | CheckEntitlement     | tiny  | Verify |

## Global Handles & Buffers

```c
// Static/global variable offsets from base 0x180000000
DAT_180346840 = 0x346840  // Microphone handle (8 bytes)
DAT_180346848 = 0x346848  // Encoder input buffer ptr (8 bytes)
DAT_180346850 = 0x346850  // Encoder handle (8 bytes)
DAT_180346858 = 0x346858  // Decoder pool base array (8 bytes)
```

## Calling Convention Summary

**All functions use x64 System V ABI (fastcall):**
- Arg 1: RCX
- Arg 2: RDX
- Arg 3: R8
- Arg 4: R9
- Return: RAX (64-bit) or EAX (32-bit) or AL (8-bit)

**All functions are SAFE to hook** - no non-standard ABI, no thunks detected.

## Audio Constants

```
Microphone:
  - Buffer size: 24000 bytes (0x5DC0)
  - Capture size: 2400 bytes (0x960) - 50ms @ 48kHz
  - Sample rate: 48000 Hz (0xBBC0)

VOIP:
  - Packet size: 960 bytes (0x3C0) - 20ms frame
  - Opus codec (typical)
```

## One-Line Descriptions

```
0x96f70  CheckEntitlement      void()  - Verify OVR entitlement/platform access
0x97370  MicAvailable          u64()   - Return 0x960 (mic capture size)
0x97380  MicBufferSize         u64()   - Return 24000 (mic buffer size)
0x97390  MicCreate             int()   - Initialize microphone, cache handle → DAT_180346840
0x97400  MicDestroy            void()  - Cleanup microphone
0x97430  MicDetected           bool()  - Check if OVR platform initialized
0x97450  MicRead               void(buf, sz)  - Read PCM from mic → buf [CRITICAL]
0x97470  MicSampleRate         u64()   - Return 48000 (sample rate)
0x97480  MicStart              void()  - Start microphone capture
0x97490  MicStop               void()  - Stop microphone capture
0x980c0  VoipAnswer            void()  - Accept incoming call
0x980d0  VoipAvailable         void()  - Get PCM available size
0x980e0  VoipBufferSize        void()  - Get max decoder output buffer
0x980f0  VoipCall              void()  - Initiate outgoing call
0x98100  VoipCreateDecoder     int(src_id, cfg)  - Setup decoder per source
0x98300  VoipCreateEncoder     int()   - Initialize encoder
0x98370  VoipDecode            void(frame, buf, sz)  - Decompress RX audio [CRITICAL]
0x984e0  VoipEncode            void(ctx, count, out, sz)  - Compress TX audio [CRITICAL]
0x98570  VoipPacketSize        u64()   - Return 0x3C0 (960 bytes)
```

## Most Useful Hook Points (Ranked)

1. **0x984e0 - VoipEncode** - Intercept ALL outgoing audio (most impactful)
2. **0x98370 - VoipDecode** - Intercept ALL incoming audio
3. **0x97450 - MicRead** - Replace microphone input before encoding
4. **0x98300 - VoipCreateEncoder** - Redirect to custom encoder
5. **0x98100 - VoipCreateDecoder** - Control decoder creation per peer

## Detours Hook Template

```c
// For VoipEncode (0x184e0 if using 0x180000000 base)
typedef void (*pfnVoipEncode)(void*, ssize_t, void*, size_t);
pfnVoipEncode g_pfnVoipEncodeReal = NULL;

void __fastcall VoipEncodeHook(void *ctx, ssize_t count, void *out, size_t sz) {
    // Intercept audio encoding here
    // Can modify ctx, out buffer, or skip call entirely
    return g_pfnVoipEncodeReal(ctx, count, out, sz);
}

// In patch init:
DetourAttach(&(PVOID&)g_pfnVoipEncodeReal, VoipEncodeHook);
```

## IAT Entries (Indirect Jump Functions)

These functions use indirect jumps through IAT for maximum stealthiness:
- VoipCall (0x980f0)
- VoipAnswer (0x980c0)
- VoipAvailable (0x980d0)
- VoipBufferSize (0x980e0)
- MicStart (0x97480)
- MicStop (0x97490)
- MicRead (0x97450)

Can be hooked either:
1. **At function entry** - Detours/trampoline hook
2. **At IAT entry (0x1801fa680)** - IAT patching (more stealthy)

## Known Issues & Workarounds

- Guard checks (CFG) on `MicCreate`, `VoipCreateEncoder`, `VoipCreateDecoder`
  - Workaround: Hook earlier in init chain or use IAT patching
- Decoder pool (0x346858) uses complex 0x40-byte block management
  - Multiple decoders for multi-party VOIP
  - Each block: [source_id(8) | cfg1(8) | decoder_ptr(8) | buf(8) | cfg234(24)]

---

*Quick reference for pnsovr.dll VOIP/audio hooking*  
*All offsets relative to DLL base 0x180000000*
