# WinHTTP to libcurl Investigation - NotGhidra Analysis Results

**Date**: 2026-02-11  
**Binary**: echovr.exe build 631547 (SHA256: b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043)  
**Base Address**: 0x140000000  
**Analysis Tool**: NotGhidra (Rizin + RetDec)

---

## Executive Summary

Successfully identified the CHttpRequest class implementation that uses WinHTTP COM objects. The primary initialization function creates IWinHttpRequest via CoCreateInstance, which fails under Wine (error 0x80040154 CLASS_E_CLASSNOTAVAILABLE).

**Key Finding**: The game calls CoCreateInstance with CLSID_WinHttpRequest at **0x1401f4c96**. This is the PRIMARY HOOK POINT for libcurl replacement.

---

## 1. CoCreateInstance Call Sites

NotGhidra identified **12 CoCreateInstance call sites** in echovr.exe:

| Address | Context | Priority |
|---------|---------|----------|
| **0x1401f4c96** | **CHttpRequest::Initialize** | **PRIMARY** |
| 0x1401f64b7 | HTTP request with URL construction | Secondary |
| 0x1401f6fea | Unknown COM object | Low |
| 0x1401f732e | Unknown COM object | Low |
| 0x14135247e | Unknown COM object | Low |
| 0x1413524b2 | Unknown COM object | Low |
| 0x14138a98a | Unknown COM object | Low |
| 0x14138ad08 | Unknown COM object | Low |
| 0x14138b215 | Unknown COM object | Low |
| 0x1413a102b | Unknown COM object | Low |
| 0x141400728 | Unknown COM object | Low |
| 0x14140181d | Unknown COM object | Low |

**CoCreateInstance IAT Entry**: 0x1416c3c48 (ole32.dll import)

---

## 2. CHttpRequest::Initialize Function

**Address**: 0x1401f4c00  
**Size**: 256 bytes  
**Source File**: `d:\projects\rad\dev\src\engine\libs\os\network\csyshttp_win_xb1.cpp` (line 956)

### 2.1 Disassembly Breakdown

#### COM Object Creation (0x1401f4c82 - 0x1401f4c96)
```asm
0x1401f4c82:  lea rcx, [0x1416e8c88]     ; CLSID_WinHttpRequest pointer
0x1401f4c89:  lea r9, [0x1416e8c98]      ; IID_IWinHttpRequest pointer  
0x1401f4c90:  xor edx, edx               ; pUnkOuter = NULL
0x1401f4c92:  lea r8d, [rdx + 0x17]      ; dwClsContext = CLSCTX_ALL (0x17)
0x1401f4c96:  call [0x1416c3c48]         ; CoCreateInstance()
```

**Parameters**:
- RCX: CLSID pointer → CLSID_WinHttpRequest {88d96a09-f192-11d4-a65f-0040963251e5}
- RDX: pUnkOuter → NULL (no aggregation)
- R8: dwClsContext → 0x17 (CLSCTX_ALL)
- R9: IID pointer → IID_IWinHttpRequest {a1c9feee-0617-4f23-9d58-8961ea43567c}
- [RSP+0x20]: ppv → address of m_pWinHttpRequest (this+0x10)

#### Error Handling (0x1401f4ca7 - 0x1401f4cba)
```asm
0x1401f4ca7:  test eax, eax              ; Check HRESULT
0x1401f4ca9:  jns 0x1401f4cbe            ; Jump to success if SUCCEEDED()
; Error path:
0x1401f4cab:  mov r9d, eax               ; HRESULT → param 3
0x1401f4cae:  lea r8, [0x1416e8ca8]      ; Error string → param 2
0x1401f4cb5:  xor edx, edx               ; 0 → param 1
0x1401f4cb7:  lea ecx, [rdx + 8]         ; Log level 8 → param 0
0x1401f4cba:  call fcn.1406356b0         ; Log error
```

**Error String @ 0x1416e8ca8**: `"CV|CHttpRequest CoCreateInstance failed with hresult 0x%08X"`

#### Success Path (0x1401f4cbe - 0x1401f4d80)
```asm
0x1401f4cbe:  mov rsi, [rsp + 0x40]      ; Load COM pointer from output
0x1401f4cc3:  cmp qword [rsi], 0         ; Check if pointer is valid
0x1401f4cc7:  je error_handler           ; Jump if NULL
; Create stream object:
0x1401f4cd0:  call fcn.1401f4a10         ; CreateStreamOnHGlobal or similar
; Initialize request:
0x1401f4d22:  mov rcx, [request_ptr]
0x1401f4d26:  call [vtable+offset]       ; IWinHttpRequest::Open or similar
; Return success:
0x1401f4d80:  mov eax, 1                 ; Return TRUE
0x1401f4d85:  ret
```

### 2.2 Class Structure

**CHttpRequest** (estimated size: ~128 bytes):

| Offset | Type | Description |
|--------|------|-------------|
| +0x00 | void** | vtable pointer |
| +0x08 | ? | Unknown |
| +0x10 | IWinHttpRequest* | COM interface pointer (m_pWinHttpRequest) |
| +0x18-0x20 | ? | Unknown |
| +0x28 | HRESULT | Last error code |
| +0x30-0x60 | ? | Unknown |
| +0x68 | IStream* | Stream object for request/response body |

**Class Typeinfo**: `.?AVCHttpRequest@NRadEngine@@` @ 0x14204b438

---

## 3. WinHTTP GUIDs

### CLSID_WinHttpRequest @ 0x1416e8c88
```
{88d96a09-f192-11d4-a65f-0040963251e5}
Hex: 09 6a d9 88 92 f1 d4 11 a6 5f 00 40 96 32 51 e5
```

### IID_IWinHttpRequest @ 0x1416e8c98
```
{a1c9feee-0617-4f23-9d58-8961ea43567c}
Hex: ee fe c9 a1 17 06 23 4f 9d 58 89 61 ea 43 56 7c
```

---

## 4. HTTP Request Workflow (Inferred)

Based on the disassembly and error messages, the CHttpRequest class follows this pattern:

1. **Initialize** (0x1401f4c00):
   - Create IWinHttpRequest COM object via CoCreateInstance
   - Check for creation failure → log error + return FALSE
   - Create stream object for request body (CreateStreamOnHGlobal)
   - Initialize WinHTTP request object
   - Return TRUE on success

2. **Open** (address TBD):
   - Call IWinHttpRequest::Open(method, url, async)
   - Set request headers
   - Prepare request body

3. **Send** (address TBD):
   - Call IWinHttpRequest::Send(body)
   - Wait for response (sync or async)
   - Read response status/headers/body

4. **Cleanup** (destructor, address TBD):
   - Release COM interface (IWinHttpRequest::Release())
   - Free stream objects
   - Clean up class state

---

## 5. Hook Strategy Options

### Option A: Hook CoCreateInstance (RECOMMENDED)

**Hook Point**: 0x1401f4c96 (CALL [0x1416c3c48])  
**Method**: MinHook detour on CoCreateInstance IAT entry

**Pros**:
- Intercepts at the earliest point
- Can check CLSID and selectively replace only WinHTTP requests
- Other COM objects (DirectX, etc.) continue working normally
- Minimal code changes

**Cons**:
- Need to implement IWinHttpRequest interface stub
- All 12 CoCreateInstance call sites affected (need CLSID filtering)

**Implementation**:
```cpp
HRESULT WINAPI Hook_CoCreateInstance(
    REFCLSID rclsid,
    LPUNKNOWN pUnkOuter,
    DWORD dwClsContext,
    REFIID riid,
    LPVOID *ppv)
{
    // Check if this is WinHTTP
    if (IsEqualCLSID(rclsid, CLSID_WinHttpRequest)) {
        // Return our libcurl-backed implementation
        *ppv = new CurlHttpRequestAdapter();
        return S_OK;
    }
    
    // Call original for other COM objects
    return Original_CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}
```

### Option B: Hook CHttpRequest::Initialize

**Hook Point**: 0x1401f4c00 (function entry)  
**Method**: MinHook detour on function start

**Pros**:
- Only affects CHttpRequest class
- Can directly replace initialization logic
- No need to implement full IWinHttpRequest interface

**Cons**:
- Need to reverse engineer full CHttpRequest class structure
- Need to find all callers of CHttpRequest::Initialize
- More brittle (tightly coupled to class implementation)

### Option C: Patch CoCreateInstance Call Site

**Patch Point**: 0x1401f4c96  
**Method**: NOP the CALL + inject jump to custom handler

**Pros**:
- Surgical change, only affects CHttpRequest
- No IAT hooking needed

**Cons**:
- Requires code cave or nearby space for handler
- More complex to implement
- Breaks code signing (not an issue for Wine)

---

## 6. Required libcurl API Mapping

To implement a WinHTTP-compatible wrapper, these IWinHttpRequest methods need libcurl equivalents:

| IWinHttpRequest Method | libcurl Equivalent | Notes |
|------------------------|-------------------|-------|
| Open(method, url, async) | curl_easy_setopt(CURLOPT_URL, ...) | Must parse URL and set method |
| SetRequestHeader(name, value) | curl_slist_append + CURLOPT_HTTPHEADER | Build header list |
| Send(body) | curl_easy_setopt(CURLOPT_POSTFIELDS, ...) + curl_easy_perform | Execute request |
| get_Status | curl_easy_getinfo(CURLINFO_RESPONSE_CODE, ...) | HTTP status code |
| get_ResponseText | Read from write callback | Store response body |
| get_ResponseBody | Read from write callback | Same as ResponseText |
| WaitForResponse | curl_easy_perform (blocking) | Synchronous execution |

---

## 7. Next Steps

### 7.1 Immediate Actions
- [ ] Decompile CHttpRequest constructor to confirm class layout
- [ ] Find CHttpRequest destructor (Release/cleanup logic)
- [ ] Identify all CHttpRequest method calls (Open, Send, etc.)
- [ ] Map vtable structure for CHttpRequest
- [ ] Analyze the other 11 CoCreateInstance call sites (confirm they're not HTTP-related)

### 7.2 Implementation Tasks
- [ ] Design IWinHttpRequest interface adapter (COM object wrapper)
- [ ] Implement libcurl HTTP request/response handling
- [ ] Set up MinHook for CoCreateInstance interception
- [ ] Add CLSID filtering (only replace WinHTTP, pass through others)
- [ ] Test with broadcaster initialization (port 6794)
- [ ] Verify no regressions with other COM objects

### 7.3 Testing Strategy
- [ ] Unit test: CurlHttpRequestAdapter implements IWinHttpRequest correctly
- [ ] Integration test: Hook installed, CoCreateInstance replaced for WinHTTP
- [ ] System test: nevr-server starts without "Broadcaster port (6794) initialization failed"
- [ ] Regression test: Other features (graphics, audio, input) still work under Wine

---

## 8. Open Questions

1. **Are there other HTTP-related COM objects in the game?**
   - Need to analyze the other 11 CoCreateInstance call sites
   - Check for XMLHTTP, ServerXMLHTTP, or other HTTP COM objects

2. **Does the game use async HTTP requests?**
   - If yes, need to implement async libcurl (multi interface)
   - If no, simple curl_easy_perform is sufficient

3. **What are the typical request/response sizes?**
   - Affects buffer allocation strategy
   - Need to analyze actual traffic during gameplay

4. **Are there any certificate validation requirements?**
   - WinHTTP validates certificates by default
   - libcurl needs CURLOPT_CAINFO configured

5. **Does the game expect specific WinHTTP error codes?**
   - May need to map libcurl errors to HRESULT codes
   - Check error handling in callers of CHttpRequest::Initialize

---

## 9. References

- WinHTTP Documentation: https://docs.microsoft.com/en-us/windows/win32/winhttp/
- libcurl Documentation: https://curl.se/libcurl/c/
- IWinHttpRequest Interface: https://docs.microsoft.com/en-us/windows/win32/winhttp/iwinhttprequest-interface
- MinHook: https://github.com/TsudaKageyu/minhook

---

## Appendix A: Additional Error Strings

Found in echovr.exe .rdata section:

- 0x1416e8ca8: "CV|CHttpRequest CoCreateInstance failed with hresult 0x%08X"
- 0x1416e8ce8: "CHttpRequest request allocation failed"
- 0x1416e8d10: "CHttpRequest request stream creation failed"
- 0x1416dbe78: "Broadcaster initialization failed"
- 0x141cd0430: "%s: Broadcaster port (%u) initialization failed"

---

## Appendix B: NotGhidra Analysis Metadata

- **Total Functions**: 30,709
- **Analysis Level**: full (Rizin aaa)
- **Analysis Time**: <1 minute (cached)
- **Decompiler**: RetDec (failed on this function due to complexity)
- **Disassembler**: Rizin
- **Cross-references**: 12 to CoCreateInstance IAT, 0 to CHttpRequest::Initialize (indirect calls)
