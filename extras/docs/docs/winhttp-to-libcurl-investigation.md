# Prompt: Replace WinHTTP with libcurl in Echo VR Broadcaster Initialization

## Context

Echo VR game server (`echovr.exe`) fails during broadcaster initialization with error:
```
0024:err:ole:com_get_class_object no class object {88d96a09-f192-11d4-a65f-0040963251e5} could be created for context 0x17
R14NetClient: Broadcaster port (6794) initialization failed
```

The GUID `{88d96a09-f192-11d4-a65f-0040963251e5}` is WinHTTP's IWinHttpRequest COM interface. Wine's COM registration is incomplete, preventing broadcaster initialization.

**Current behavior:**
- WinHTTP COM object creation fails with 0x80040154 (CLASS_E_CLASSNOTAVAILABLE)
- Broadcaster initialization aborts
- Game never loads `pnsradgameserver.dll` (requires successful broadcaster init)

**Evidence:**
- Game log: `CHttpRequest CoCreateInstance failed with hresult 0x80040154`
- Error location: `d:\projects\rad\dev\src\engine\libs\os\network\csyshttp_win_xb1.cpp:956`
- Both custom and reference `pnsradgameserver.dll` exhibit identical behavior

## Objective

Hook or replace the game's WinHTTP COM instantiation to use libcurl instead, allowing broadcaster initialization to succeed under Wine.

## Investigation Tasks

### 1. Locate WinHTTP Usage

**Find all WinHTTP COM instantiation attempts:**
- Search for CLSID `{88d96a09-f192-11d4-a65f-0040963251e5}` references in binary
- Identify `CoCreateInstance` calls with this CLSID
- Locate `CHttpRequest` class implementation (error originates from `csyshttp_win_xb1.cpp`)

**Key functions to find:**
- HTTP request creation/initialization
- Broadcaster network initialization sequence
- COM object factory calls

### 2. Analyze HTTP Requirements

**Determine what the broadcaster needs:**
- HTTP methods used (GET/POST/PUT)
- Headers required
- SSL/TLS requirements
- Synchronous vs asynchronous operation
- Callback/completion mechanisms

**Questions to answer:**
- Does broadcaster use WinHTTP for all HTTP(S) operations?
- Are there multiple HTTP subsystems (engine vs. game)?
- What's the call hierarchy: Broadcaster → CHttpRequest → WinHTTP?

### 3. Design Hook Strategy

**Option A: Hook CoCreateInstance**
- Intercept `CoCreateInstance` calls for WinHTTP CLSID
- Return custom IWinHttpRequest implementation backed by libcurl
- Requires implementing COM interface vtable

**Option B: Hook CHttpRequest Constructor**
- Patch `CHttpRequest` class initialization
- Replace WinHTTP backend with libcurl before COM call
- Simpler if CHttpRequest has alternative backend path

**Option C: Patch CoCreateInstance Call Site**
- NOP the failing `CoCreateInstance` call
- Inject libcurl initialization directly
- Replace HTTP method dispatch with libcurl equivalents

**Constraints:**
- Must maintain same calling convention and return values
- Game expects specific HTTP behavior/semantics
- Must handle both HTTP and HTTPS
- Must work in multithreaded context (game has 10+ worker threads)

### 4. Implementation Requirements

**Deliverables needed:**
1. **Memory addresses:**
   - `CoCreateInstance` call site(s) for WinHTTP CLSID
   - `CHttpRequest` vtable or constructor
   - HTTP method dispatch functions

2. **Function signatures:**
   - `CHttpRequest::Initialize()` or equivalent
   - HTTP request execution functions
   - Completion callback mechanism

3. **Patch specification:**
   - Exact bytes to patch and replacement code
   - Hook installation point (DllMain? Before broadcaster init?)
   - libcurl initialization sequence

4. **Verification steps:**
   - How to confirm hook is active
   - Expected log output with successful hook
   - Fallback behavior if libcurl fails

## Debugging Methodology Requirements

**MANDATORY INSTRUMENTATION:**
- Log BEFORE and AFTER every hooked function call
- Log all function parameters and return values
- Log exact memory addresses and timestamps
- Add verification that hooked code is actually executing

**DO NOT:**
- Assume hooks work without evidence
- Patch multiple locations at once (test one at a time)
- Skip verification steps (PID, timestamps, log appearance)
- Guess at calling conventions (disassemble and verify)

## Expected Output Format

```
### 1. WinHTTP Usage Analysis
- CoCreateInstance call site: 0x<address> (offset from base)
- CLSID reference: 0x<address>
- CHttpRequest class: 0x<address> (vtable at 0x<address>)
- Calling function: <symbol or offset>

### 2. HTTP Requirements
- Methods used: [GET/POST/etc.]
- Headers observed: [list]
- SSL requirement: [yes/no]
- Async/callback mechanism: [description]

### 3. Recommended Hook Strategy
**Approach:** [A/B/C]
**Rationale:** [why this approach]
**Implementation:**
```asm
; Assembly patch at 0x<address>
<disassembly before>
→
<disassembly after>
```

### 4. Hook Code Template
```cpp
// Pseudo-code for hook implementation
typedef HRESULT (*OriginalCoCreateInstance)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
OriginalCoCreateInstance original_CoCreateInstance = ...;

HRESULT WINAPI Hooked_CoCreateInstance(REFCLSID rclsid, LPUNKNOWN pUnkOuter, 
                                        DWORD dwClsContext, REFIID riid, LPVOID* ppv) {
    // Log entry
    if (IsEqualGUID(rclsid, CLSID_WinHttpRequest)) {
        // Return libcurl-backed implementation
        *ppv = CreateCurlHttpRequest();
        return S_OK;
    }
    return original_CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}
```

### 5. Integration into gamepatches
- CMakeLists.txt changes needed
- Additional dependencies (libcurl)
- Hook installation location (patches.cpp)
- Verification logging
```

## Success Criteria

✅ **Hook successfully installed:**
- Debug log shows hook function called
- WinHTTP CLSID interception confirmed

✅ **Broadcaster initialization succeeds:**
- Game log shows no `Broadcaster initialization failed` error
- No COM error 0x80040154

✅ **pnsradgameserver.dll loads:**
- `pnsradgameserver_debug.log` appears
- `DllMain` called
- `ServerLib()` function invoked by game

## Files to Reference

**In nevr-server repo:**
- `src/gamepatches/patches.cpp` - Existing hook infrastructure
- `src/gamepatches/patch_addresses.h` - Known game offsets
- `src/common/hooking.h` - Hook helper functions

**Game files:**
- `echovr.exe` base address: 0x140000000
- Build: goldmaster 631547 from //rad/rad15_live
- Error location: csyshttp_win_xb1.cpp line 956

**Investigation logs:**
- Game log: `ready-at-dawn-echo-arena/_local/r14logs/[r14(server)]-*.log`
- Wine stderr: `/tmp/echovr_stdout.log`

---

## Investigation Status

**✅ ANALYSIS COMPLETE** - See `winhttp-to-libcurl-findings.md` for full NotGhidra analysis results.

**Key Findings:**
- CHttpRequest::Initialize identified at **0x1401f4c00**
- CoCreateInstance call site at **0x1401f4c96** (PRIMARY HOOK POINT)
- WinHTTP GUIDs located at 0x1416e8c88 (CLSID) and 0x1416e8c98 (IID)
- 12 total CoCreateInstance call sites found
- Recommended approach: **Option A - Hook CoCreateInstance with CLSID filtering**

**Next Steps:**
1. Implement IWinHttpRequest COM adapter backed by libcurl
2. Install MinHook on CoCreateInstance IAT entry (0x1416c3c48)
3. Filter by CLSID {88d96a09-f192-11d4-a65f-0040963251e5} for selective replacement
4. Test with broadcaster initialization

---

**CRITICAL REMINDER:** Follow the debugging methodology. Add instrumentation BEFORE and AFTER every hook. Verify hooks execute. One change at a time. Evidence over assumptions.
