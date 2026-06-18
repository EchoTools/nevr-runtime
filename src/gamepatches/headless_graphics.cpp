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
 * StubDXGIFactory — minimal IDXGIFactory1 stub
 *
 * The game calls these IDXGIFactory1 methods (vtable slot numbers):
 *   Slot 0: QueryInterface
 *   Slot 1: AddRef
 *   Slot 2: Release
 *   Slot 7: EnumAdapters  (IDXGIFactory::EnumAdapters)
 *   Slot 12: EnumAdapters1 (IDXGIFactory1::EnumAdapters1)
 *
 * We return DXGI_ERROR_NOT_FOUND from EnumAdapters/EnumAdapters1 at
 * index 0, telling the game there are no display adapters.  All other
 * methods return E_NOTIMPL.
 * ======================================================================== */

/* Forward declarations for vtable functions */
static HRESULT STDMETHODCALLTYPE Stub_QueryInterface(StubUnknown* self, const GUID* riid, void** ppv);
static ULONG   STDMETHODCALLTYPE Stub_AddRef(StubUnknown* self);
static ULONG   STDMETHODCALLTYPE Stub_Release(StubUnknown* self);
static HRESULT STDMETHODCALLTYPE Stub_NotImpl() { return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE Stub_EnumAdapters(StubUnknown* self, UINT adapter, void** ppAdapter);
static HRESULT STDMETHODCALLTYPE Stub_EnumAdapters1(StubUnknown* self, UINT adapter, void** ppAdapter);

/* IDXGIFactory1 vtable layout:
 *  [0]  QueryInterface
 *  [1]  AddRef
 *  [2]  Release
 *  [3]  SetPrivateData
 *  [4]  SetPrivateDataInterface
 *  [5]  GetPrivateData
 *  [6]  GetParent
 *  [7]  EnumAdapters
 *  [8]  MakeWindowAssociation
 *  [9]  GetWindowAssociation
 *  [10] CreateSwapChain
 *  [11] CreateSoftwareAdapter
 *  [12] EnumAdapters1
 *  [13] IsCurrent
 */
static void* g_factory_vtable[14] = {};
static StubUnknown g_stub_factory = {};
static bool g_factory_vtable_init = false;

static void InitFactoryVtable() {
    if (g_factory_vtable_init) return;
    /* Fill with E_NOTIMPL stubs first */
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

/* IUnknown implementation */
static HRESULT STDMETHODCALLTYPE Stub_QueryInterface(StubUnknown* self, const GUID* riid, void** ppv) {
    if (!ppv) return E_POINTER;
    /* Accept IUnknown and IDXGIFactory1 — return ourselves */
    static const GUID IID_IUnknown =
        {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

    if (memcmp(riid, &IID_IUnknown, sizeof(GUID)) == 0 ||
        memcmp(riid, &IID_IDXGIFactory1, sizeof(GUID)) == 0) {
        InterlockedIncrement(&self->refcount);
        *ppv = self;
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE Stub_AddRef(StubUnknown* self) {
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE Stub_Release(StubUnknown* self) {
    LONG ref = InterlockedDecrement(&self->refcount);
    /* Static object — never actually free */
    if (ref <= 0) self->refcount = 1;
    return (ULONG)(ref > 0 ? ref : 1);
}

/* EnumAdapters — return "not found" so the game sees zero adapters */
static HRESULT STDMETHODCALLTYPE Stub_EnumAdapters(StubUnknown*, UINT adapter, void** ppAdapter) {
    if (ppAdapter) *ppAdapter = nullptr;
    return DXGI_ERROR_NOT_FOUND_;
}

static HRESULT STDMETHODCALLTYPE Stub_EnumAdapters1(StubUnknown*, UINT adapter, void** ppAdapter) {
    if (ppAdapter) *ppAdapter = nullptr;
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

#endif /* _WIN32 */

/* ========================================================================
 * Public API
 * ======================================================================== */

void InstallHeadlessGraphicsHooks() {
#ifdef _WIN32
    /* Register callbacks so hooks install when the DLLs actually load.
     * If the DLLs are already loaded (shouldn't be this early), OnLoad
     * fires the callback immediately. */
    DllLoadHook::OnLoad("dxgi.dll", OnDxgiLoad);
    DllLoadHook::OnLoad("d3d11.dll", OnD3d11Load);

    fprintf(stderr, "[NEVR.HEADLESS] registered DXGI/D3D11 headless hooks "
                    "(active when g_isHeadless=%d)\n", (int)g_isHeadless);
    fflush(stderr);
#endif
}
