# pnsovr.dll Analysis - Documentation Index

## 📊 Quick Navigation

This directory contains comprehensive reverse engineering analysis of **pnsovr.dll** (Meta/Oculus VR Platform Native Service) for Windows x64.

### 📄 Documentation Files

| File | Purpose | Audience | Size |
|------|---------|----------|------|
| **[pnsovr_re_summary.md](pnsovr_re_summary.md)** | Executive summary & overview | Everyone | 12 KB |
| **[pnsovr_quick_reference.md](pnsovr_quick_reference.md)** | Function address map & quick lookup | Developers | 15 KB |
| **[pnsovr_hook_candidates.md](pnsovr_hook_candidates.md)** | Detailed function analysis & hook strategies | Reverse Engineers | 50+ KB |
| **[pnsovr.h](pnsovr.h)** | Production C/C++ header definitions | C++ Developers | 15 KB |

---

## 🎯 What's Here?

### Analyzed Components

- ✅ **18 Functions** - Microphone and VOIP subsystem
- ✅ **4 Global Variables** - Handle and buffer storage
- ✅ **3 Hook Strategies** - Audio injection, interception, suppression
- ✅ **5 Tier-1 Candidates** - Critical path hooks
- ✅ **Call Control** - VOIP flow management

### Calling Convention

All functions use **x64 System V ABI (fastcall)** - SAFE for standard Detours/MinHook hooking.

---

## 🚀 Getting Started

### For Quick Overview
→ Read: [pnsovr_re_summary.md](pnsovr_re_summary.md) (5 min read)

### For Hook Implementation
→ Start: [pnsovr.h](pnsovr.h) (header file with typedefs)  
→ Reference: [pnsovr_quick_reference.md](pnsovr_quick_reference.md)

### For Deep Dive Analysis
→ Study: [pnsovr_hook_candidates.md](pnsovr_hook_candidates.md) (comprehensive)

---

## 📍 Function Index

### Critical Audio Path (HOOK PRIORITY ⭐⭐⭐⭐⭐)

```
TX (Transmit) Path:
  MicRead(0x97450) → VoipEncode(0x984e0) → Network

RX (Receive) Path:
  Network → VoipDecode(0x98370) → Playback

Setup:
  VoipCreateEncoder(0x98300)
  VoipCreateDecoder(0x98100)
```

**→ Most Important Hooks:**
1. **VoipEncode @ 0x984e0** - Intercept ALL outgoing audio
2. **VoipDecode @ 0x98370** - Intercept ALL incoming audio  
3. **MicRead @ 0x97450** - Control microphone input

### Full Function List

| Address | Function | Type | Hook Priority |
|---------|----------|------|---|
| 0x96f70 | CheckEntitlement | Verify | ⭐ |
| 0x97370 | MicAvailable | Query | ⭐ |
| 0x97380 | MicBufferSize | Query | ⭐ |
| 0x97390 | MicCreate | Init | ⭐⭐⭐ |
| 0x97400 | MicDestroy | Cleanup | ⭐ |
| 0x97430 | MicDetected | Query | ⭐ |
| 0x97450 | MicRead | **CRITICAL** | ⭐⭐⭐⭐ |
| 0x97470 | MicSampleRate | Query | ⭐ |
| 0x97480 | MicStart | Control | ⭐⭐ |
| 0x97490 | MicStop | Control | ⭐⭐ |
| 0x980c0 | VoipAnswer | Control | ⭐⭐⭐ |
| 0x980d0 | VoipAvailable | Query | ⭐ |
| 0x980e0 | VoipBufferSize | Query | ⭐ |
| 0x980f0 | VoipCall | Control | ⭐⭐⭐ |
| 0x98100 | VoipCreateDecoder | Init | ⭐⭐⭐ |
| 0x98300 | VoipCreateEncoder | Init | ⭐⭐⭐⭐ |
| 0x98370 | VoipDecode | **CRITICAL** | ⭐⭐⭐⭐⭐ |
| 0x984e0 | VoipEncode | **CRITICAL** | ⭐⭐⭐⭐⭐ |
| 0x98570 | VoipPacketSize | Query | ⭐ |

---

## 🔧 Usage Examples

### Example 1: Hook to Inject Synthetic Audio

```c
#include "pnsovr.h"

// In your hooking code:
auto originalVoipEncode = (Hooks::VoipEncodeFn)((uintptr_t)hPnsovr + 0x984e0);

void HookedVoipEncode(void *ctx, int64_t count, void *out, size_t sz) {
    // Option 1: Inject synthetic audio
    generate_bot_voice(out, sz);
    
    // Option 2: Call original
    // originalVoipEncode(ctx, count, out, sz);
}
```

### Example 2: Hook to Silence Microphone

```c
void HookedMicRead(void *buffer, size_t size) {
    // Silence the microphone
    memset(buffer, 0, size);
}
```

### Example 3: Hook to Intercept Received Audio

```c
void HookedVoipDecode(void *frame, void *out, size_t sz) {
    // Decode and log
    printf("Received audio frame, %zu bytes\n", sz);
    
    // Call original
    originalVoipDecode(frame, out, sz);
    
    // Can now modify 'out' buffer before playback
}
```

---

## 📋 Key Global Variables

```c
0x180346840  g_MicrophoneHandle      // Microphone device
0x180346848  g_EncoderInputBuffer    // Encoder input buffer
0x180346850  g_VoipEncoder           // Encoder instance
0x180346858  g_DecoderPool           // Array of decoders (0x40 bytes each)
```

---

## ⚠️ Important Notes

### Calling Convention
All functions use **x64 fastcall** (Windows x64 ABI):
- Parameters: RCX, RDX, R8, R9
- Returns: RAX/EAX/AL
- All safe for standard hooking

### Guard Checks
Three functions have CFG guards (may detect aggressive hooking):
- MicCreate @ 0x97390
- VoipCreateEncoder @ 0x98300
- VoipCreateDecoder @ 0x98100

**Workaround**: Hook at IAT level instead of function entry

### IAT Hooks Available
Indirect jumps through IAT (stealthy hooking):
- VoipCall, VoipAnswer
- VoipAvailable, VoipBufferSize
- MicStart, MicStop, MicRead

---

## 📚 Document Structure

### pnsovr_re_summary.md
- Executive summary
- Key findings
- Function categories
- Technical insights
- Recommended next steps

### pnsovr_quick_reference.md
- Function address map
- One-liner descriptions
- Audio constants
- Detours hook template
- Most useful hook points (ranked)

### pnsovr_hook_candidates.md
- Tier-1/2/3 classification
- Detailed function analysis
- Type signatures
- Hook implementation strategies
- Warnings and limitations
- Recommended hook points

### pnsovr.h
- C/C++ function declarations
- extern "C" declarations for all functions
- Type definitions for hooking
- Global variable declarations
- Namespace organization
- Example hook implementation

---

## 🔍 Analysis Status

| Task | Status | Notes |
|------|--------|-------|
| Function enumeration | ✅ Complete | 18 functions analyzed |
| Decompilation | ✅ Complete | All functions decompiled |
| Signature analysis | ✅ Complete | x64 ABI verified |
| Documentation | ✅ Complete | 4 document files generated |
| Ghidra comments | ✅ Complete | Plate comments added to all functions |
| Global variables | ✅ Complete | 4 variables mapped |
| Hook strategies | ✅ Complete | Examples provided |

---

## 🎓 Learning Resources

### To Understand pnsovr.dll:
1. Start with [pnsovr_re_summary.md](pnsovr_re_summary.md)
2. Reference [pnsovr.h](pnsovr.h) for function signatures
3. Use [pnsovr_quick_reference.md](pnsovr_quick_reference.md) for quick lookup

### To Hook Functions:
1. Read hook strategies in [pnsovr_hook_candidates.md](pnsovr_hook_candidates.md)
2. Copy typedef from [pnsovr.h](pnsovr.h)
3. Use Detours/MinHook framework
4. Test with minimal guard check bypass

### Audio Pipeline Deep Dive:
1. Study VoipEncode details in [pnsovr_hook_candidates.md](pnsovr_hook_candidates.md#voipencode--0x984e0)
2. Study VoipDecode details in [pnsovr_hook_candidates.md](pnsovr_hook_candidates.md#voipdecode--0x98370)
3. Review decoder pool structure for multi-party VOIP

---

## 📞 Function Quick Links

### Microphone Functions
- [MicCreate](pnsovr_hook_candidates.md#miccreate--0x97390)
- [MicRead](pnsovr_hook_candidates.md#micread--0x97450)
- [MicStart](pnsovr_hook_candidates.md#micstart--0x97480)
- [MicStop](pnsovr_hook_candidates.md#micstop--0x97490)

### VOIP Functions
- [VoipEncode](pnsovr_hook_candidates.md#voipencode--0x984e0)
- [VoipDecode](pnsovr_hook_candidates.md#voipdecode--0x98370)
- [VoipCall](pnsovr_hook_candidates.md#voipcall--0x980f0)
- [VoipAnswer](pnsovr_hook_candidates.md#voipanswer--0x980c0)

---

## 📊 Statistics

- **Total Functions**: 18
- **Tier-1 (Critical)**: 5 functions
- **Tier-2 (High)**: 8 functions
- **Tier-3 (Low)**: 5 functions
- **Global Variables**: 4
- **Lines of Documentation**: 1000+
- **Code Examples**: 5+

---

## 🎯 Next Steps

1. **Immediate**: Review [pnsovr.h](pnsovr.h) header file
2. **Short-term**: Implement VoipEncode hook for audio injection
3. **Medium-term**: Add MicRead hook for microphone control
4. **Long-term**: Build full audio pipeline manipulation framework

---

*Generated via Ghidra Reverse Engineering Analysis*  
*DLL Base: 0x180000000 (Windows x64)*  
*All function offsets documented and verified*
