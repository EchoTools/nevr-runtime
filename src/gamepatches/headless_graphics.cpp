/* SYNTHESIS -- custom tool code, not from binary
 *
 * Headless Graphics Stubs — DXGI/D3D11
 *
 * Prevents GPU allocation on dedicated servers by hooking CreateDXGIFactory1
 * and D3D11CreateDevice.  When g_isHeadless is set, these hooks return
 * lightweight stub COM objects that satisfy the game's initialization
 * sequence without touching any GPU hardware.
 *
 * The game's DXGI/D3D11 usage (from reconstruction):
 *   1. CreateDXGIFactory1() — called from CRenderCS::InitInternal @ 0x14072ba70
 *      via dxgi.dll import thunk @ 0x141355a47
 *   2. IDXGIFactory1::EnumAdapters() — iterates adapters looking for best GPU
 *   3. IDXGIAdapter::EnumOutputs() — queries display outputs per adapter
 *   4. IDXGIOutput::GetDisplayModeList() — queries supported display modes
 *   5. D3D11CreateDevice() — creates the device + immediate context
 *   6. HandleDXError @ 0x140551070 — 75 callers, all errors treated as fatal
 *
 * The stub factory returns DXGI_ERROR_NOT_FOUND on EnumAdapters(0) so the
 * game sees "no adapters" and skips the GPU init path.  The existing
 * PatchEnableHeadless() already NOPs the renderer init and ApplyGraphicsSettings
 * calls, so the game never gets far enough to need a real device.
 *
 * D3D11CreateDevice is also hooked: if somehow reached in headless mode, it
 * returns a null device + context with E_FAIL, which the HandleDXError hook
 * in wave0_instrumentation.cpp will recover from.
 *
 * In non-headless mode (client), hooks pass through to the real functions.
 */

#include "headless_graphics.h"
#include "dll_load_hook.h"
#include "common/globals.h"
#include "common/logging.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <MinHook.h>

/* ========================================================================
 * DXGI/D3D11 type definitions
 *
 * We define just enough of the COM interfaces to compile the stubs.
 * We do NOT include <dxgi.h> or <d3d11.h> because they are not available
 * in the MinGW cross-compilation toolchain used for this project, and
 * even if they were, the full headers pull in half of DirectX.
 * ======================================================================== */

/* IID definitions — from dxgi.h and d3d11.h */
static const GUID IID_IDXGIFactory1 =
    {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

/* DXGI_ERROR codes */
static constexpr HRESULT DXGI_ERROR_NOT_FOUND_ = (HRESULT)0x887A0002L;

/* IUnknown-compatible ref-counted base for all stubs */
struct StubUnknown {
    void** vtable;
    volatile LONG refcount;
};

/* ========================================================================
 * Stub DXGI Adapter — fake GPU for headless servers
 *
 * The game enumerates adapters and dies if it finds zero. We return one
 * fake adapter at index 0 with a plausible description. The adapter
 * reports no display outputs (EnumOutputs → DXGI_ERROR_NOT_FOUND) and
 * passes CheckInterfaceSupport. The real renderer init is NOPped by
 * PatchEnableHeadless(), so the adapter is never actually used.
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Stub_NotImpl() { return E_NOTIMPL; }

/* Forward declarations */
static HRESULT STDMETHODCALLTYPE Stub_QueryInterface(StubUnknown* self, const GUID* riid, void** ppv);
static ULONG   STDMETHODCALLTYPE Stub_AddRef(StubUnknown* self);
static ULONG   STDMETHODCALLTYPE Stub_Release(StubUnknown* self);

/* Durable diagnostic: log a requested COM IID (all four GUID fields) with the
 * stub object it was requested on and the verdict. Called only on the headless
 * stub path (a handful of QueryInterface calls during renderer init), so it is
 * low-frequency, not per-frame. Kept in as leave-it-better instrumentation:
 * a future interface-refusal shows up in the JSONL log with the exact GUID. */
static void LogStubIid(const char* ctx, const char* obj, const GUID* riid, const char* verdict) {
    if (!riid) {
        Log(EchoVR::LogLevel::Info, "[NEVR.HEADLESS] %s obj=%s riid=NULL -> %s", ctx, obj, verdict);
        return;
    }
    Log(EchoVR::LogLevel::Info,
        "[NEVR.HEADLESS] %s obj=%s riid={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} -> %s",
        ctx, obj, static_cast<unsigned long>(riid->Data1),
        static_cast<unsigned>(riid->Data2), static_cast<unsigned>(riid->Data3),
        static_cast<unsigned>(riid->Data4[0]), static_cast<unsigned>(riid->Data4[1]),
        static_cast<unsigned>(riid->Data4[2]), static_cast<unsigned>(riid->Data4[3]),
        static_cast<unsigned>(riid->Data4[4]), static_cast<unsigned>(riid->Data4[5]),
        static_cast<unsigned>(riid->Data4[6]), static_cast<unsigned>(riid->Data4[7]),
        verdict);
}

/* --- DXGI_ADAPTER_DESC (enough to satisfy GetDesc) --- */
struct StubAdapterDesc {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    GUID AdapterLuid;  /* LUID is 8 bytes, same as GUID.Data1+Data2 */
};

/* IDXGIAdapter vtable slots:
 *  [0] QueryInterface  [1] AddRef  [2] Release
 *  [3] SetPrivateData  [4] SetPrivateDataInterface  [5] GetPrivateData
 *  [6] GetParent  [7] EnumOutputs  [8] GetDesc  [9] CheckInterfaceSupport
 *  IDXGIAdapter1 adds: [10] GetDesc1
 */

static HRESULT STDMETHODCALLTYPE StubAdapter_EnumOutputs(StubUnknown*, UINT, void** ppOutput) {
    if (ppOutput) *ppOutput = nullptr;
    return DXGI_ERROR_NOT_FOUND_;
}

static HRESULT STDMETHODCALLTYPE StubAdapter_GetDesc(StubUnknown*, StubAdapterDesc* pDesc) {
    if (!pDesc) return E_POINTER;
    memset(pDesc, 0, sizeof(*pDesc));
    const WCHAR name[] = L"NEVR Headless Adapter";
    memcpy(pDesc->Description, name, sizeof(name));
    pDesc->VendorId = 0x10DE;  /* NVIDIA vendor ID */
    pDesc->DeviceId = 0x2488;
    pDesc->DedicatedVideoMemory = (SIZE_T)8ULL * 1024 * 1024 * 1024;
    pDesc->SharedSystemMemory = (SIZE_T)16ULL * 1024 * 1024 * 1024;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE StubAdapter_CheckInterfaceSupport(StubUnknown*, const GUID*, INT64* pUMDVersion) {
    if (pUMDVersion) *pUMDVersion = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE StubAdapter_GetDesc1(StubUnknown* self, void* pDesc) {
    /* GetDesc1 layout starts with the same fields as GetDesc, plus Flags at end.
     * Zero everything and fill the common prefix via GetDesc. */
    if (!pDesc) return E_POINTER;
    memset(pDesc, 0, 304);  /* DXGI_ADAPTER_DESC1 is ~304 bytes */
    return StubAdapter_GetDesc(self, static_cast<StubAdapterDesc*>(pDesc));
}

static HRESULT STDMETHODCALLTYPE StubAdapter_GetDesc2(StubUnknown* self, void* pDesc) {
    /* DXGI_ADAPTER_DESC2 shares the DESC prefix, adding Flags + two preemption-
     * granularity enums. Zero a conservative 312 bytes (<= real struct size, so
     * never overruns the caller's buffer) then fill the shared prefix. */
    if (!pDesc) return E_POINTER;
    memset(pDesc, 0, 312);
    return StubAdapter_GetDesc(self, static_cast<StubAdapterDesc*>(pDesc));
}

/* IDXGIAdapter3::QueryVideoMemoryInfo out-parameter layout */
struct StubVideoMemoryInfo {
    UINT64 Budget;
    UINT64 CurrentUsage;
    UINT64 AvailableForReservation;
    UINT64 CurrentReservation;
};

static HRESULT STDMETHODCALLTYPE StubAdapter_QueryVideoMemoryInfo(
    StubUnknown*, UINT /*NodeIndex*/, UINT /*MemorySegmentGroup*/, StubVideoMemoryInfo* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->Budget = 8ULL * 1024 * 1024 * 1024;   /* 8 GB budget */
    pInfo->CurrentUsage = 0;
    pInfo->AvailableForReservation = 4ULL * 1024 * 1024 * 1024;
    pInfo->CurrentReservation = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE StubAdapter_SetVideoMemoryReservation(
    StubUnknown*, UINT /*NodeIndex*/, UINT /*MemorySegmentGroup*/, UINT64 /*Reservation*/) {
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE StubAdapter_RegisterVMBudgetEvent(
    StubUnknown*, HANDLE /*hEvent*/, DWORD* pdwCookie) {
    if (pdwCookie) *pdwCookie = 1;
    return S_OK;
}

/* Adapter interface IIDs. IID_IDXGIAdapter3 is runtime-confirmed as the gate
 * (echovr cgs_dx12 refuses on it); the others are canonical DXGI IIDs and, if
 * any is wrong, the StubAdapter_QueryInterface REFUSED log line will name it. */
static const GUID IID_IDXGIAdapter =
    {0x2411e7e1, 0x12ac, 0x4ccf, {0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0}};
static const GUID IID_IDXGIAdapter1 =
    {0x29038f61, 0x3839, 0x4626, {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}};
static const GUID IID_IDXGIAdapter2 =
    {0x0aa1ae0a, 0xfa0e, 0x4b84, {0x86, 0x44, 0xe0, 0x5f, 0xf8, 0xe5, 0xac, 0xb5}};
static const GUID IID_IDXGIAdapter3 =
    {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};

/* Adapter-specific QueryInterface: answers the IDXGIAdapter family (and
 * IUnknown), NOT IDXGIFactory*. Separate from the factory's QI. */
static HRESULT STDMETHODCALLTYPE StubAdapter_QueryInterface(StubUnknown* self, const GUID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    static const GUID IID_IUnknown_ =
        {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    if (memcmp(riid, &IID_IUnknown_,    sizeof(GUID)) == 0 ||
        memcmp(riid, &IID_IDXGIAdapter,  sizeof(GUID)) == 0 ||
        memcmp(riid, &IID_IDXGIAdapter1, sizeof(GUID)) == 0 ||
        memcmp(riid, &IID_IDXGIAdapter2, sizeof(GUID)) == 0 ||
        memcmp(riid, &IID_IDXGIAdapter3, sizeof(GUID)) == 0) {
        InterlockedIncrement(&self->refcount);
        *ppv = self;
        LogStubIid("StubAdapter_QueryInterface", "adapter", riid, "S_OK");
        return S_OK;
    }
    LogStubIid("StubAdapter_QueryInterface", "adapter", riid, "E_NOINTERFACE (REFUSED)");
    *ppv = nullptr;
    return E_NOINTERFACE;
}

/* IDXGIAdapter3 full vtable (18 methods, slots 0-17); one spare slot. */
static void* g_adapter_vtable[19] = {};
static StubUnknown g_stub_adapter = {};
static bool g_adapter_vtable_init = false;

static void InitAdapterVtable() {
    if (g_adapter_vtable_init) return;
    for (int i = 0; i < 19; i++)
        g_adapter_vtable[i] = reinterpret_cast<void*>(&Stub_NotImpl);
    /* IUnknown */
    g_adapter_vtable[0]  = reinterpret_cast<void*>(&StubAdapter_QueryInterface);
    g_adapter_vtable[1]  = reinterpret_cast<void*>(&Stub_AddRef);
    g_adapter_vtable[2]  = reinterpret_cast<void*>(&Stub_Release);
    /* IDXGIObject [3..6]: SetPrivateData/SetPrivateDataInterface/GetPrivateData/
     * GetParent — left as Stub_NotImpl (not exercised by the headless path). */
    /* IDXGIAdapter */
    g_adapter_vtable[7]  = reinterpret_cast<void*>(&StubAdapter_EnumOutputs);
    g_adapter_vtable[8]  = reinterpret_cast<void*>(&StubAdapter_GetDesc);
    g_adapter_vtable[9]  = reinterpret_cast<void*>(&StubAdapter_CheckInterfaceSupport);
    /* IDXGIAdapter1 / IDXGIAdapter2 */
    g_adapter_vtable[10] = reinterpret_cast<void*>(&StubAdapter_GetDesc1);
    g_adapter_vtable[11] = reinterpret_cast<void*>(&StubAdapter_GetDesc2);
    /* IDXGIAdapter3 [12..17]:
     *  [12] RegisterHardwareContentProtectionTeardownStatusEvent -> NotImpl
     *  [13] UnregisterHardwareContentProtectionTeardownStatus     -> NotImpl (void)
     *  [14] QueryVideoMemoryInfo
     *  [15] SetVideoMemoryReservation
     *  [16] RegisterVideoMemoryBudgetChangeNotificationEvent
     *  [17] UnregisterVideoMemoryBudgetChangeNotification          -> NotImpl (void) */
    g_adapter_vtable[14] = reinterpret_cast<void*>(&StubAdapter_QueryVideoMemoryInfo);
    g_adapter_vtable[15] = reinterpret_cast<void*>(&StubAdapter_SetVideoMemoryReservation);
    g_adapter_vtable[16] = reinterpret_cast<void*>(&StubAdapter_RegisterVMBudgetEvent);
    g_stub_adapter.vtable = g_adapter_vtable;
    g_stub_adapter.refcount = 1;
    g_adapter_vtable_init = true;
}

/* ========================================================================
 * StubDXGIFactory — returns the stub adapter at index 0
 * ======================================================================== */

static HRESULT STDMETHODCALLTYPE Stub_EnumAdapters(StubUnknown*, UINT adapter, void** ppAdapter);
static HRESULT STDMETHODCALLTYPE Stub_EnumAdapters1(StubUnknown*, UINT adapter, void** ppAdapter);

/* IDXGIFactory1 vtable layout:
 *  [0]  QueryInterface  [1] AddRef  [2] Release
 *  [3]  SetPrivateData  [4] SetPrivateDataInterface  [5] GetPrivateData
 *  [6]  GetParent
 *  [7]  EnumAdapters  [8] MakeWindowAssociation  [9] GetWindowAssociation
 *  [10] CreateSwapChain  [11] CreateSoftwareAdapter
 *  [12] EnumAdapters1  [13] IsCurrent
 */
static void* g_factory_vtable[14] = {};
static StubUnknown g_stub_factory = {};
static bool g_factory_vtable_init = false;

static void InitFactoryVtable() {
    if (g_factory_vtable_init) return;
    InitAdapterVtable();
    for (int i = 0; i < 14; i++)
        g_factory_vtable[i] = (void*)&Stub_NotImpl;
    g_factory_vtable[0]  = (void*)&Stub_QueryInterface;
    g_factory_vtable[1]  = (void*)&Stub_AddRef;
    g_factory_vtable[2]  = (void*)&Stub_Release;
    g_factory_vtable[7]  = (void*)&Stub_EnumAdapters;
    g_factory_vtable[12] = (void*)&Stub_EnumAdapters1;
    g_stub_factory.vtable = g_factory_vtable;
    g_stub_factory.refcount = 1;
    g_factory_vtable_init = true;
}

/* IUnknown — shared by factory and adapter */
static HRESULT STDMETHODCALLTYPE Stub_QueryInterface(StubUnknown* self, const GUID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    const char* obj = (self == &g_stub_factory) ? "factory"
                    : (self == &g_stub_adapter) ? "adapter"
                    : "unknown";
    static const GUID IID_IUnknown =
        {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    if (memcmp(riid, &IID_IUnknown, sizeof(GUID)) == 0 ||
        memcmp(riid, &IID_IDXGIFactory1, sizeof(GUID)) == 0) {
        InterlockedIncrement(&self->refcount);
        *ppv = self;
        LogStubIid("Stub_QueryInterface", obj, riid, "S_OK");
        return S_OK;
    }
    LogStubIid("Stub_QueryInterface", obj, riid, "E_NOINTERFACE (REFUSED)");
    *ppv = nullptr;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Stub_AddRef(StubUnknown* self) {
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE Stub_Release(StubUnknown* self) {
    LONG ref = InterlockedDecrement(&self->refcount);
    if (ref <= 0) self->refcount = 1;
    return (ULONG)(ref > 0 ? ref : 1);
}

/* EnumAdapters — return stub adapter at index 0, NOT_FOUND for anything else */
static HRESULT STDMETHODCALLTYPE Stub_EnumAdapters(StubUnknown*, UINT adapter, void** ppAdapter) {
    if (!ppAdapter) return E_POINTER;
    if (adapter == 0) {
        InterlockedIncrement(&g_stub_adapter.refcount);
        *ppAdapter = &g_stub_adapter;
        return S_OK;
    }
    *ppAdapter = nullptr;
    return DXGI_ERROR_NOT_FOUND_;
}

static HRESULT STDMETHODCALLTYPE Stub_EnumAdapters1(StubUnknown*, UINT adapter, void** ppAdapter) {
    if (!ppAdapter) return E_POINTER;
    if (adapter == 0) {
        InterlockedIncrement(&g_stub_adapter.refcount);
        *ppAdapter = &g_stub_adapter;
        return S_OK;
    }
    *ppAdapter = nullptr;
    return DXGI_ERROR_NOT_FOUND_;
}

/* ========================================================================
 * CreateDXGIFactory1 hook
 * ======================================================================== */

/* Signature: HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) */
typedef HRESULT (WINAPI *CreateDXGIFactory1_t)(const GUID* riid, void** ppFactory);
static CreateDXGIFactory1_t g_origCreateDXGIFactory1 = nullptr;

static HRESULT WINAPI CreateDXGIFactory1_Hook(const GUID* riid, void** ppFactory) {
    if (g_isHeadless) {
        InitFactoryVtable();
        if (ppFactory) *ppFactory = &g_stub_factory;
        LogStubIid("CreateDXGIFactory1", "factory-create", riid, "stub");
        Log(EchoVR::LogLevel::Info,
            "[NEVR.HEADLESS] CreateDXGIFactory1 intercepted — returning stub factory (no GPU)");
        return S_OK;
    }
    return g_origCreateDXGIFactory1(riid, ppFactory);
}

/* ========================================================================
 * CreateDXGIFactory hook (non-1 variant, just in case)
 * ======================================================================== */

typedef HRESULT (WINAPI *CreateDXGIFactory_t)(const GUID* riid, void** ppFactory);
static CreateDXGIFactory_t g_origCreateDXGIFactory = nullptr;

static HRESULT WINAPI CreateDXGIFactory_Hook(const GUID* riid, void** ppFactory) {
    if (g_isHeadless) {
        InitFactoryVtable();
        if (ppFactory) *ppFactory = &g_stub_factory;
        Log(EchoVR::LogLevel::Info,
            "[NEVR.HEADLESS] CreateDXGIFactory intercepted — returning stub factory (no GPU)");
        return S_OK;
    }
    return g_origCreateDXGIFactory(riid, ppFactory);
}

/* ========================================================================
 * D3D11CreateDevice hook
 * ======================================================================== */

/* Minimal D3D11 types needed for the signature */
typedef enum {
    D3D_DRIVER_TYPE_UNKNOWN   = 0,
    D3D_DRIVER_TYPE_HARDWARE  = 1,
    D3D_DRIVER_TYPE_REFERENCE = 2,
    D3D_DRIVER_TYPE_NULL      = 3,
    D3D_DRIVER_TYPE_SOFTWARE  = 4,
    D3D_DRIVER_TYPE_WARP      = 5,
} D3D_DRIVER_TYPE_;

typedef enum {
    D3D_FEATURE_LEVEL_9_1  = 0x9100,
    D3D_FEATURE_LEVEL_11_0 = 0xb000,
} D3D_FEATURE_LEVEL_;

typedef HRESULT (WINAPI *D3D11CreateDevice_t)(
    void* pAdapter,           /* IDXGIAdapter* */
    UINT driverType,          /* D3D_DRIVER_TYPE */
    HMODULE software,
    UINT flags,
    const UINT* pFeatureLevels,
    UINT featureLevels,
    UINT sdkVersion,
    void** ppDevice,          /* ID3D11Device** */
    UINT* pFeatureLevel,
    void** ppImmediateContext  /* ID3D11DeviceContext** */
);
static D3D11CreateDevice_t g_origD3D11CreateDevice = nullptr;

static HRESULT WINAPI D3D11CreateDevice_Hook(
    void* pAdapter, UINT driverType, HMODULE software, UINT flags,
    const UINT* pFeatureLevels, UINT featureLevels, UINT sdkVersion,
    void** ppDevice, UINT* pFeatureLevel, void** ppImmediateContext)
{
    if (g_isHeadless) {
        if (ppDevice) *ppDevice = nullptr;
        if (ppImmediateContext) *ppImmediateContext = nullptr;
        if (pFeatureLevel) *pFeatureLevel = (UINT)D3D_FEATURE_LEVEL_11_0;
        Log(EchoVR::LogLevel::Info,
            "[NEVR.HEADLESS] D3D11CreateDevice intercepted — returning null device (no GPU)");
        /* Return DXGI_ERROR_NOT_FOUND so callers treat it as "no hardware" rather
         * than a generic failure.  The HandleDXError hook will recover. */
        return DXGI_ERROR_NOT_FOUND_;
    }
    return g_origD3D11CreateDevice(pAdapter, driverType, software, flags,
                                    pFeatureLevels, featureLevels, sdkVersion,
                                    ppDevice, pFeatureLevel, ppImmediateContext);
}

/* ========================================================================
 * DLL load callbacks — hook DXGI/D3D11 functions after the DLLs load
 * ======================================================================== */

static void OnDxgiLoad(const char* dll_name, HMODULE module) {
    /* Hook CreateDXGIFactory1 — the primary entry point used by the game */
    FARPROC fn1 = GetProcAddress(module, "CreateDXGIFactory1");
    if (fn1) {
        g_origCreateDXGIFactory1 = (CreateDXGIFactory1_t)fn1;
        if (MH_CreateHook((void*)fn1, (void*)&CreateDXGIFactory1_Hook,
                          (void**)&g_origCreateDXGIFactory1) == MH_OK &&
            MH_EnableHook((void*)fn1) == MH_OK) {
            fprintf(stderr, "[NEVR.HEADLESS] hooked CreateDXGIFactory1 at %p\n", (void*)fn1);
            fflush(stderr);
        } else {
            fprintf(stderr, "[NEVR.HEADLESS] FAILED to hook CreateDXGIFactory1\n");
            fflush(stderr);
        }
    }

    /* Also hook CreateDXGIFactory in case it's used as a fallback */
    FARPROC fn0 = GetProcAddress(module, "CreateDXGIFactory");
    if (fn0) {
        g_origCreateDXGIFactory = (CreateDXGIFactory_t)fn0;
        if (MH_CreateHook((void*)fn0, (void*)&CreateDXGIFactory_Hook,
                          (void**)&g_origCreateDXGIFactory) == MH_OK &&
            MH_EnableHook((void*)fn0) == MH_OK) {
            fprintf(stderr, "[NEVR.HEADLESS] hooked CreateDXGIFactory at %p\n", (void*)fn0);
            fflush(stderr);
        } else {
            fprintf(stderr, "[NEVR.HEADLESS] FAILED to hook CreateDXGIFactory\n");
            fflush(stderr);
        }
    }
}

static void OnD3d11Load(const char* dll_name, HMODULE module) {
    FARPROC fn = GetProcAddress(module, "D3D11CreateDevice");
    if (fn) {
        g_origD3D11CreateDevice = (D3D11CreateDevice_t)fn;
        if (MH_CreateHook((void*)fn, (void*)&D3D11CreateDevice_Hook,
                          (void**)&g_origD3D11CreateDevice) == MH_OK &&
            MH_EnableHook((void*)fn) == MH_OK) {
            fprintf(stderr, "[NEVR.HEADLESS] hooked D3D11CreateDevice at %p\n", (void*)fn);
            fflush(stderr);
        } else {
            fprintf(stderr, "[NEVR.HEADLESS] FAILED to hook D3D11CreateDevice\n");
            fflush(stderr);
        }
    }
}

/* ========================================================================
 * D3D12CreateDevice hook — echovr.exe is actually DX12
 * ======================================================================== */

typedef HRESULT (WINAPI *D3D12CreateDevice_t)(
    void* pAdapter,           /* IUnknown* (IDXGIAdapter) */
    UINT minFeatureLevel,     /* D3D_FEATURE_LEVEL */
    const void* riid,         /* REFIID */
    void** ppDevice           /* void** */
);
static D3D12CreateDevice_t g_origD3D12CreateDevice = nullptr;

static HRESULT WINAPI D3D12CreateDevice_Hook(
    void* pAdapter, UINT minFeatureLevel, const void* riid, void** ppDevice)
{
    if (g_isHeadless) {
        if (ppDevice) *ppDevice = nullptr;
        Log(EchoVR::LogLevel::Info,
            "[NEVR.HEADLESS] D3D12CreateDevice intercepted — returning null device (no GPU)");
        return DXGI_ERROR_NOT_FOUND_;
    }
    return g_origD3D12CreateDevice(pAdapter, minFeatureLevel, riid, ppDevice);
}

static void OnD3d12Load(const char* dll_name, HMODULE module) {
    FARPROC fn = GetProcAddress(module, "D3D12CreateDevice");
    if (fn) {
        g_origD3D12CreateDevice = (D3D12CreateDevice_t)fn;
        if (MH_CreateHook((void*)fn, (void*)&D3D12CreateDevice_Hook,
                          (void**)&g_origD3D12CreateDevice) == MH_OK &&
            MH_EnableHook((void*)fn) == MH_OK) {
            fprintf(stderr, "[NEVR.HEADLESS] hooked D3D12CreateDevice at %p\n", (void*)fn);
            fflush(stderr);
        } else {
            fprintf(stderr, "[NEVR.HEADLESS] FAILED to hook D3D12CreateDevice\n");
            fflush(stderr);
        }
    }
}

#endif /* _WIN32 */

/* ========================================================================
 * Public API
 * ======================================================================== */

void InstallHeadlessGraphicsHooks() {
#ifdef _WIN32
    DllLoadHook::OnLoad("dxgi.dll", OnDxgiLoad);
    DllLoadHook::OnLoad("d3d11.dll", OnD3d11Load);
    DllLoadHook::OnLoad("d3d12.dll", OnD3d12Load);

    fprintf(stderr, "[NEVR.HEADLESS] registered DXGI/D3D11/D3D12 headless hooks "
                    "(active when g_isHeadless=%d)\n", (int)g_isHeadless);
    fflush(stderr);
#endif
}
