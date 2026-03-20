# WinHTTP to libcurl Investigation - Analysis Progress

## Status: Analysis Running
**Started**: 2026-02-11  
**Expected Duration**: Up to 15 minutes  
**Binary**: echovr.exe (build 631547, 35MB)

## Investigation Objectives

### 1. Find WinHTTP COM Instantiation Sites
**Target**: Calls to `CoCreateInstance` with CLSID `{88d96a09-f192-11d4-a65f-0040963251e5}` (IWinHttpRequest)

**Known Information**:
- CoCreateInstance IAT entry: `0x1416c3c48`
- Error string: "CHttpRequest CoCreateInstance failed with hresult 0x%08X" at `0x1416e8ca5`
- Source file: `csyshttp_win_xb1.cpp:956`
- Class names found in binary:
  - `.?AVCHttpRequest@NRadEngine@@` at `0x14204b438`
  - `.?AVCHttpRequestStream@NRadEngine@@` at `0x14204b1d8`

**Once Analysis Completes**:
- [ ] Get xrefs to CoCreateInstance IAT (`0x1416c3c48`)
- [ ] Get xrefs to error string (`0x1416e8ca5`)
- [ ] Decompile functions that call CoCreateInstance
- [ ] Identify CHttpRequest constructor/Initialize function

### 2. Locate CHttpRequest Class Implementation
**Goal**: Find vtable, constructor, and key methods

**Search Strategy**:
- Search for class name strings
- Find vtable references
- Identify HTTP method dispatch (GET, POST, PUT, DELETE)
- Map member functions (Initialize, Send, Receive, SetHeader, etc.)

### 3. Analyze HTTP Requirements
**Need to Determine**:
- HTTP methods used (GET/POST/etc.)
- Header requirements
- Synchronous vs asynchronous calls
- Error handling patterns
- Connection pooling/reuse
- SSL/TLS requirements

### 4. Design Hook Strategy

**Option A: Hook CoCreateInstance**
- Intercept all COM object creation
- Return custom IWinHttpRequest implementation backed by libcurl
- Pros: Clean, no binary patching
- Cons: Need to implement full IWinHttpRequest interface

**Option B: Hook CHttpRequest::Initialize**
- Replace just the WinHTTP initialization
- Swap backend to libcurl
- Pros: Minimal interface to implement
- Cons: Need to find exact function

**Option C: Patch Call Site**
- NOP out CoCreateInstance call
- Replace with direct libcurl init
- Pros: Simple
- Cons: Fragile, multiple call sites?

## Next Steps (After Analysis)

1. **Get xrefs**: Find all CoCreateInstance call sites
2. **Decompile**: Understand CHttpRequest structure and flow
3. **Map HTTP usage**: Document what HTTP features are actually used
4. **Choose hook point**: Based on findings, select optimal strategy
5. **Design libcurl wrapper**: Create drop-in replacement
6. **Write patch specification**: Exact addresses, bytes, hook code
7. **Test plan**: Verification steps for Wine environment

## Known Addresses

| Symbol | Address | Type | Notes |
|--------|---------|------|-------|
| CoCreateInstance | 0x1416c3c48 | IAT | Import from ole32.dll |
| Error string | 0x1416e8ca5 | .rdata | "CHttpRequest CoCreateInstance failed..." |
| CHttpRequest class | 0x14204b438 | .data | RTTI type descriptor |
| CHttpRequestStream class | 0x14204b1d8 | .data | RTTI type descriptor |
| Broadcaster error | 0x141cd0430 | .rdata | "Broadcaster port (%u) initialization failed" |
| Binary entrypoint | 0x1414ed2e0 | .text | Main entry |

## Analysis Configuration

- **Tool**: NotGhidra MCP (Rizin + RetDec)
- **Project ID**: 46ab6614-1bf9-4adb-a633-511d9c6bbb2c
- **Binary ID**: b6d08277e5846900c81004b64b298df6acba834b69700a640b758bda94a52043
- **Analysis Level**: full
- **Base Address**: 0x140000000 (PE default)
