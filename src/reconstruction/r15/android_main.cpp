// android_main
// Quest libr15.so VA 0x10784c4; ReVault binary 576.
// ReVault raw parent 100207664, reconstruction node 100600605; grade M: all 362 instructions read through the ret at 0x1078940; the continuation after each brk #1 is modelled through SoftwareBreakpoint returning, as in cr15_net_customization_renderer_destroy_context.cpp (seven trap addresses 0x107895c, 0x1078988, 0x10789b4, 0x10789e0, 0x1078a0c, 0x1078a38, 0x1078a64, the last a tail to 0x10788dc); 20 placeholders by VA: FUN_010795ac 0x10795ac; GOT 0x3805da0, 0x3818568; data 0x3861218, 0x38611c8, 0x38611d0; stat words 0x3861240, 0x3861280, 0x38612c0, 0x3861300, 0x3861340, 0x3861380, 0x38613c0; storage 0x3861400, 0x3861470, 0x38614e0, 0x3861550, 0x38615c0, 0x3861630, 0x38616a0; the seven allocator sizes are the raw decompile's folded GOT loads.
// @0x10784c4 — android_main
/* @confidence: M */

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

struct android_app;  // NDK struct; fields are read by byte offset (activity at +0x18, destroyRequested at +0x64)

namespace NRadEngine {

struct CAllocator;
struct CThreadLock;
struct SSharedAllocatorInterface;

// CFixedStringT<N>: the N-byte text buffer, as the tree declares the template.
template <std::size_t N>
struct CFixedStringT {
    char text[N];
};
static_assert(sizeof(CFixedStringT<256>) == 0x100);

struct CEnvironment {
    CEnvironment();
    ~CEnvironment();
    // The third argument is zero at its only call site in this function; its type is not recovered.
    static void SetGlobal(CEnvironment* environment, std::uint64_t key, std::uintptr_t aux, void* slot);
};

struct CSysJava {
    static void Init(android_app* app);
    static android_app* App();
    static int SessionActive();
    static void* NativeWindow();
    // Non-static: x0 holds the value of DAT_03861218 (the CSysJava instance) at each call.
    void SetupJNI();
    void GetArgs(CFixedStringT<256>& args);
};

struct CSymbol64 {
    static void Initialize();
};

template <typename T, unsigned long long Index>
struct CThreadData {
    static void InitData(T* initial);
    static void PurgeInstance();
};

struct CThreadLocker {
    explicit CThreadLocker(CThreadLock* lock);
    ~CThreadLocker();
};

struct CMemoryElephant {
    static void Initialize(int mode);
    static SSharedAllocatorInterface* Interface();
    static void Shutdown();
};

struct CMemoryContext {
    // The tree declares this slot as void* (cmemory_context.hpp); the slots are read by byte offset here.
    static void* context;
    static void Initialize(SSharedAllocatorInterface* allocator);
    static void Shutdown();
    static void SetBaseAllocators(CAllocator** allocators);
    static CAllocator* Current();
};

CAllocator* CreateElephantAllocator(void* storage, std::uint64_t size, std::int32_t kind, const char* name,
                                    void* stats, std::uint64_t* params);
void DestroyElephantAllocator(CAllocator* allocator);

namespace NWriteLog {
void WriteLog(int severity, void* context, const char* message, ...);
}  // namespace NWriteLog

using BreakpointHandler = void (*)();
BreakpointHandler SoftwareBreakpoint(int kind, std::uintptr_t address);

namespace CMemory {
void Fill(void* destination, unsigned char value, std::uint64_t count);
}  // namespace CMemory

struct CSysThread {
    static void SetTLS(int slot, void* value);
};

struct CSysInfo {
    static void SetCommandLineArgs(const char* args);
    static char* GetCommandLineArgs();
};

struct CSysOS {
    static void Init();
    static void InitDbgSymbolLookups();
    static void Shutdown();
};

struct CDebugCrashReport {
    static void RegisterGlobalCrashHandler(char* first, char* second);
    static void Shutdown();
};

void Main(const char* command_line);

extern int gLoggingInitialized;

}  // namespace NRadEngine

// Imports (ReVault names: ANativeActivity_finish at thunk 0x1047860, ALooper_pollAll at thunk 0xff8d80).
extern "C" void ANativeActivity_finish(void* activity);
extern "C" int ALooper_pollAll(int timeout_ms, int* out_fd, int* out_events, void** out_data);

// Declared locally: <link.h> is not available under mingw. Bionic AArch64 (LP64) layouts.
struct Elf64_Phdr;  // program header; only pointed to here, so it stays incomplete
// struct dl_phdr_info: the object a dl_iterate_phdr callback describes (bionic link.h).
struct dl_phdr_info {
    std::uint64_t dlpi_addr;      // load bias of the object (ElfW(Addr))
    const char* dlpi_name;        // path of the object as the loader recorded it
    const Elf64_Phdr* dlpi_phdr;  // the object's program header array
    std::uint16_t dlpi_phnum;     // number of entries in dlpi_phdr (ElfW(Half))
    std::uint64_t dlpi_adds;      // count of object additions (dlopen) so far
    std::uint64_t dlpi_subs;      // count of object removals (dlclose) so far
    std::uint64_t dlpi_tls_modid; // TLS module id of the object (size_t), 0 when it has no TLS
    void* dlpi_tls_data;          // the calling thread's TLS block for that module, or null
};
static_assert(sizeof(dl_phdr_info) == 64);
static_assert(offsetof(dl_phdr_info, dlpi_addr) == 0);
static_assert(offsetof(dl_phdr_info, dlpi_name) == 8);
static_assert(offsetof(dl_phdr_info, dlpi_phdr) == 16);
static_assert(offsetof(dl_phdr_info, dlpi_phnum) == 24);
static_assert(offsetof(dl_phdr_info, dlpi_adds) == 32);
static_assert(offsetof(dl_phdr_info, dlpi_subs) == 40);
static_assert(offsetof(dl_phdr_info, dlpi_tls_modid) == 48);
static_assert(offsetof(dl_phdr_info, dlpi_tls_data) == 56);
// int dl_iterate_phdr(callback, data): calls callback once per loaded object; a nonzero return stops the walk.
extern "C" int dl_iterate_phdr(int (*callback)(dl_phdr_info*, std::uint64_t, void*), void* data);

// UNRESOLVED: FUN_010795ac. The dl_iterate_phdr callback at 0x10795ac, ReVault signature
// undefined8 (undefined8*, undefined8, undefined4*). ReVault's header row names this VA FUN_00f795ac.
/* UNRESOLVED: FUN_010795ac */
std::uint64_t FUN_010795ac(std::uint64_t* info, std::uint64_t size, std::uint32_t* data);

// Data. The two named globals carry the names the tree already gives these VAs; the rest are placeholders named by VA.
extern const int g_memory_context_tls_slot_0x3861200;   // DAT_03861200 (tree name): TLS slot index, an int
extern std::byte* g_tls_owner;                          // DAT_03861210 (tree name): holds the CEnvironment built below
extern NRadEngine::CSysJava* DAT_03861218;              // 0x3861218: CSysJava instance, `this` for SetupJNI and GetArgs
extern void* DAT_038611c8;                              // 0x38611c8: passed to SetGlobal; non-null test before SetTLS
extern NRadEngine::CThreadLock DAT_038611d0;            // 0x38611d0: CThreadLock object, passed by address
extern char** DAT_03805da0;                             // GOT slot 0x3805da0 (zero in the file): the address of the char* holding the command line
extern std::uintptr_t* DAT_03818568;                    // GOT slot 0x3818568 (zero in the file): the address of a 64-bit flag
extern std::uint32_t DAT_03861240;                      // 0x3861240: stat word of System (zeroed here)
extern std::uint32_t DAT_03861280;                      // 0x3861280: stat word of Resource
extern std::uint32_t DAT_038612c0;                      // 0x38612c0: stat word of Texture
extern std::uint32_t DAT_03861300;                      // 0x3861300: stat word of External
extern std::uint32_t DAT_03861340;                      // 0x3861340: stat word of Gpu
extern std::uint32_t DAT_03861380;                      // 0x3861380: stat word of GpuResource
extern std::uint32_t DAT_038613c0;                      // 0x38613c0: stat word of Paged
extern std::byte DAT_03861400[];                        // 0x3861400: storage of System
extern std::byte DAT_03861470[];                        // 0x3861470: storage of Resource
extern std::byte DAT_038614e0[];                        // 0x38614e0: storage of Texture
extern std::byte DAT_03861550[];                        // 0x3861550: storage of External
extern std::byte DAT_038615c0[];                        // 0x38615c0: storage of Gpu
extern std::byte DAT_03861630[];                        // 0x3861630: storage of GpuResource
extern std::byte DAT_038616a0[];                        // 0x38616a0: storage of Paged

namespace {

using DlCallback = int (*)(dl_phdr_info*, std::size_t, void*);
using AllocateFn = void* (*)(void* self, std::uint64_t size);
using FreeFn = void (*)(void* self, void* block);
using PollCallback = void (*)(void* app, void* record);

// Reads a T at `offset` bytes into `base`. The NDK structs are not included, so their fields are read by offset.
template <typename T>
T LoadAt(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

// The function address in the vtable slot at byte `offset` of `object` (vtable pointer at +0). ReVault has no layout
// for these classes; the CAllocator slots at +0x10 and +0x38 are PURE_VIRTUAL in its vtable.
void* VirtualSlot(const void* object, std::size_t offset) {
    return LoadAt<void*>(LoadAt<const void*>(object, 0), offset);
}

// The null check and destroy of CMemoryContext::context[byte_offset / 8]. The brk #1 that follows the null branch is
// modelled as SoftwareBreakpoint returning: the destroy runs from trap_address, the instruction after the brk.
void DestroyContextSlot(std::size_t byte_offset, std::uintptr_t trap_address) {
    if (NRadEngine::CMemoryContext::context == nullptr) {
        // 0x1078944-0x1078958 (and 0x1078970, 0x107899c, 0x10789c8, 0x10789f4, 0x1078a20, 0x1078a4c): WriteLog; brk #1
        NRadEngine::NWriteLog::WriteLog(8, nullptr, "Trying to use memory context before it's initialized!");
        NRadEngine::SoftwareBreakpoint(1, trap_address)();
    }
    NRadEngine::DestroyElephantAllocator(
        static_cast<NRadEngine::CAllocator*>(LoadAt<void*>(NRadEngine::CMemoryContext::context, byte_offset)));
}

}  // namespace

extern "C" void android_main(android_app* app) {
    // Every local with an initializer is declared before the first goto below, so no goto skips one.
    std::uint64_t frame_words[5];     // Ghidra local_90 (sp+0x110): allocator params, dl_iterate_phdr data, ALooper outData, CThreadLocker storage
    std::int32_t poll_events = 0;     // sp+0x13c (auStack_64): ALooper outEvents
    std::uint32_t block_in_poll = 0;  // Ghidra uVar13: 1 selects an infinite ALooper timeout
    NRadEngine::CFixedStringT<256> command_line;  // sp+0x10: the 256-byte command-line buffer

    // 0x10784e0-0x10784e4: CSysJava::Init(app)
    NRadEngine::CSysJava::Init(app);
    // 0x10784e8: CSymbol64::Initialize()
    NRadEngine::CSymbol64::Initialize();
    // 0x10784f0-0x10784f4: x0 = DAT_03861218 is `this` for SetupJNI
    DAT_03861218->SetupJNI();
    // 0x10784f8-0x10784fc: CThreadData<CAllocator, 0>::InitData(nullptr)
    NRadEngine::CThreadData<NRadEngine::CAllocator, 0ULL>::InitData(nullptr);
    // 0x1078500-0x1078504: CMemoryElephant::Initialize(1)
    NRadEngine::CMemoryElephant::Initialize(1);
    // 0x1078508-0x107850c: CMemoryContext::Initialize(CMemoryElephant::Interface())
    NRadEngine::CMemoryContext::Initialize(NRadEngine::CMemoryElephant::Interface());

    // 0x1078510-0x1078574: the seven stat words and the first four frame words are zeroed
    DAT_03861240 = 0;
    DAT_03861280 = 0;
    DAT_038612c0 = 0;
    DAT_03861300 = 0;
    DAT_03861340 = 0;
    DAT_03861380 = 0;
    DAT_038613c0 = 0;
    frame_words[0] = 0;
    frame_words[1] = 0;
    frame_words[2] = 0;
    frame_words[3] = 0;

    // 0x1078578-0x1078680: seven elephant allocators. Sizes are the raw decompile's constants (GOT-loaded in the disassembly).
    NRadEngine::CAllocator* const system_allocator =
        NRadEngine::CreateElephantAllocator(DAT_03861400, 0x19000000, 0, "System", &DAT_03861240, frame_words);
    NRadEngine::CAllocator* const resource_allocator =
        NRadEngine::CreateElephantAllocator(DAT_03861470, 0xce00000, 0, "Resource", &DAT_03861280, frame_words);
    NRadEngine::CAllocator* const texture_allocator =
        NRadEngine::CreateElephantAllocator(DAT_038614e0, 0x3200000, 2, "Texture", &DAT_038612c0, frame_words);
    NRadEngine::CAllocator* const external_allocator =
        NRadEngine::CreateElephantAllocator(DAT_03861550, 0x400000, 0, "External", &DAT_03861300, frame_words);
    // The name at 0x2c2e835 is the four bytes "Gpu\0" (read from the file).
    NRadEngine::CAllocator* const gpu_allocator =
        NRadEngine::CreateElephantAllocator(DAT_038615c0, 0xa000000, 2, "Gpu", &DAT_03861340, frame_words);
    NRadEngine::CAllocator* const gpu_resource_allocator =
        NRadEngine::CreateElephantAllocator(DAT_03861630, 0x8000000, 2, "GpuResource", &DAT_03861380, frame_words);
    NRadEngine::CAllocator* const paged_allocator =
        NRadEngine::CreateElephantAllocator(DAT_038616a0, 0x8000000, 0, "Paged", &DAT_038613c0, frame_words);

    // 0x1078684-0x10786a0: the twelve-entry base table (sp+0x10 .. sp+0x68), then SetBaseAllocators(table)
    NRadEngine::CAllocator* base_allocators[12] = {
        system_allocator,  gpu_allocator,     resource_allocator, gpu_resource_allocator,
        texture_allocator, system_allocator,  external_allocator, paged_allocator,
        paged_allocator,   paged_allocator,   gpu_allocator,      gpu_allocator};
    NRadEngine::CMemoryContext::SetBaseAllocators(base_allocators);

    // 0x10786a4-0x10786b8: env storage = Current()->vtable[+0x10](current, 8)
    NRadEngine::CAllocator* const current = NRadEngine::CMemoryContext::Current();
    const auto allocate = reinterpret_cast<AllocateFn>(VirtualSlot(current, 0x10));
    void* const env_storage = allocate(current, 8);
    // 0x10786bc: CEnvironment::CEnvironment() on that storage
    NRadEngine::CEnvironment* const env = ::new (env_storage) NRadEngine::CEnvironment();
    // 0x10786cc: DAT_03861210 = env
    g_tls_owner = reinterpret_cast<std::byte*>(env);
    // 0x10786d4-0x10786ec: SetGlobal(env, 0xcc4a79e275ebee3a, 0, &DAT_038611c8); the key is the file's bytes at 0x2c2e850
    NRadEngine::CEnvironment::SetGlobal(env, 0xcc4a79e275ebee3aULL, 0, &DAT_038611c8);

    // 0x10786f0-0x10786fc: CMemory::Fill(command_line, 0, 0x100)
    NRadEngine::CMemory::Fill(&command_line, 0, 0x100);
    // 0x1078700-0x1078720: args = *DAT_03805da0; if (*args == 0) { GetArgs(command_line); args = command_line }
    char* args = *DAT_03805da0;
    if (*args == '\0') {
        DAT_03861218->GetArgs(command_line);  // 0x1078714-0x107871c: x0 = DAT_03861218, x1 = command_line
        args = command_line.text;             // 0x1078720
    }
    // 0x1078724: CSysInfo::SetCommandLineArgs(args)
    NRadEngine::CSysInfo::SetCommandLineArgs(args);

    // 0x1078728-0x1078738: gLoggingInitialized = 1; CSysOS::Init()
    NRadEngine::gLoggingInitialized = 1;
    NRadEngine::CSysOS::Init();
    // 0x107873c: CSysOS::InitDbgSymbolLookups()
    NRadEngine::CSysOS::InitDbgSymbolLookups();
    // 0x1078740-0x107874c: if (*DAT_03818568 != 0) { open, dl_iterate_phdr, close }
    if (*DAT_03818568 != 0) {
        // 0x1078750-0x1078760: fd = open(path, 0x41, 0x180)
        const int fd = ::open("/sdcard/_adbtemp/so-base-addresses.txt", O_WRONLY | O_CREAT, 0600);
        // 0x1078764-0x1078774: frame_words[0] = sign-extended fd
        frame_words[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(fd));
        // 0x1078778: dl_iterate_phdr(FUN_010795ac, frame_words)
        static_cast<void>(
            ::dl_iterate_phdr(reinterpret_cast<DlCallback>(reinterpret_cast<void*>(&FUN_010795ac)), frame_words));
        // 0x107877c-0x1078780: close((int)frame_words[0])
        static_cast<void>(::close(static_cast<int>(frame_words[0] & 0xffffffffULL)));
    }
    // 0x1078784-0x107879c: RegisterGlobalCrashHandler(0, 0); Main(GetCommandLineArgs()); Shutdown x2
    NRadEngine::CDebugCrashReport::RegisterGlobalCrashHandler(nullptr, nullptr);
    NRadEngine::Main(NRadEngine::CSysInfo::GetCommandLineArgs());
    NRadEngine::CDebugCrashReport::Shutdown();
    NRadEngine::CSysOS::Shutdown();
    // 0x10787a0: gLoggingInitialized = 0
    NRadEngine::gLoggingInitialized = 0;

    // 0x10787a4-0x10787b0: if (app->destroyRequested != 0) goto L_1078820; ANativeActivity_finish(app->activity)
    if (LoadAt<std::int32_t>(app, 0x64) != 0) {
        goto L_1078820;
    }
    ANativeActivity_finish(LoadAt<void*>(app, 0x18));

L_10787b4:  // 0x10787b4-0x10787b8
    if (LoadAt<std::int32_t>(app, 0x64) != 0) {
        goto L_1078820;
    }

L_10787bc:  // 0x10787bc-0x10787c0: if (!SessionActive()) goto L_10787d4
    if (NRadEngine::CSysJava::SessionActive() == 0) {
        goto L_10787d4;
    }

L_10787c4:  // 0x10787c4-0x10787cc: if (!NativeWindow()) goto L_10787d4; uVar13 = 0
    if (NRadEngine::CSysJava::NativeWindow() == nullptr) {
        goto L_10787d4;
    }
    block_in_poll = 0;
    goto L_10787e4;

L_10787d4:  // 0x10787d4-0x10787e0: uVar13 = (App()->destroyRequested == 0)
    block_in_poll = LoadAt<std::int32_t>(NRadEngine::CSysJava::App(), 0x64) == 0 ? 1U : 0U;

L_10787e4: {  // 0x10787e4-0x10787f8: status = ALooper_pollAll(-uVar13, 0, &poll_events, frame_words)
    const int timeout = -static_cast<int>(block_in_poll);
    const int status = ALooper_pollAll(timeout, nullptr, &poll_events, reinterpret_cast<void**>(frame_words));
    if (status < 0) {
        goto L_10787b4;
    }
    // 0x10787fc-0x1078800: if (frame_words[0] == 0) goto L_10787bc
    if (frame_words[0] == 0) {
        goto L_10787bc;
    }
    {
        // 0x1078804-0x1078810: callback = *(record + 0x10); callback(App(), record)
        void* const record = reinterpret_cast<void*>(frame_words[0]);
        const auto callback = reinterpret_cast<PollCallback>(LoadAt<void*>(record, 0x10));
        void* const app_now = NRadEngine::CSysJava::App();
        callback(app_now, record);
    }
    // 0x1078814-0x107881c: if (SessionActive() != 0) goto L_10787c4; goto L_10787d4
    if (NRadEngine::CSysJava::SessionActive() != 0) {
        goto L_10787c4;
    }
    goto L_10787d4;
  }

L_1078820: {  // 0x1078820-0x1078830: app->activity->vm->functions[+0x28](vm)  (DetachCurrentThread)
    void* const activity = LoadAt<void*>(app, 0x18);
    void* const vm = LoadAt<void*>(activity, 0x08);
    void* const functions = LoadAt<void*>(vm, 0);
    const auto detach = reinterpret_cast<std::int32_t (*)(void*)>(LoadAt<void*>(functions, 0x28));
    static_cast<void>(detach(vm));
  }

  // 0x1078834-0x1078868: if (DAT_03861210 != nullptr) { allocator = Current(); ~CEnvironment; allocator->vtable[+0x38](allocator, env); DAT_03861210 = nullptr }
  if (g_tls_owner != nullptr) {
    NRadEngine::CAllocator* const allocator = NRadEngine::CMemoryContext::Current();  // 0x107883c
    NRadEngine::CEnvironment* const env_now = reinterpret_cast<NRadEngine::CEnvironment*>(g_tls_owner);  // 0x1078840
    if (env_now != nullptr) {  // 0x1078848
      env_now->~CEnvironment();  // 0x1078850
    }
    const auto free_block = reinterpret_cast<FreeFn>(VirtualSlot(allocator, 0x38));  // 0x1078854-0x1078864
    free_block(allocator, env_now);
    g_tls_owner = nullptr;  // 0x1078868
  }

  // 0x107886c-0x10788dc: DestroyElephantAllocator(context[7], [3], [1], [6], [4], [2], [0]), each after its null check
  DestroyContextSlot(0x38, 0x107895c);
  DestroyContextSlot(0x18, 0x1078988);
  DestroyContextSlot(0x08, 0x10789b4);
  DestroyContextSlot(0x30, 0x10789e0);
  DestroyContextSlot(0x20, 0x1078a0c);
  DestroyContextSlot(0x10, 0x1078a38);
  DestroyContextSlot(0x00, 0x1078a64);
  // 0x10788e4-0x10788e8
  NRadEngine::CMemoryContext::Shutdown();
  NRadEngine::CMemoryElephant::Shutdown();

  // 0x10788ec-0x1078904: if (TLS slot != -1 && DAT_038611c8 != nullptr) SetTLS(TLS slot, nullptr)
  {
    const int tls_slot = g_memory_context_tls_slot_0x3861200;
    if (tls_slot != -1 && DAT_038611c8 != nullptr) {
      NRadEngine::CSysThread::SetTLS(tls_slot, nullptr);
    }
  }

  // 0x1078908-0x1078920: CThreadLocker built in the frame words (sp+0x110): ctor(&DAT_038611d0); PurgeInstance(); dtor
  NRadEngine::CThreadLocker* const locker =
      ::new (static_cast<void*>(frame_words)) NRadEngine::CThreadLocker(&DAT_038611d0);
  NRadEngine::CThreadData<NRadEngine::CAllocator, 0ULL>::PurgeInstance();
  locker->~CThreadLocker();
  // 0x1078924-0x1078940: epilogue, ret.

  // Modelled, not dead: after each brk #1 the trap returns through SoftwareBreakpoint, and DestroyContextSlot runs the
  // destroy from each trap address. The instructions there are 0x107895c-0x107896c, 0x1078988-0x1078998,
  // 0x10789b4-0x10789c4, 0x10789e0-0x10789f0, 0x1078a0c-0x1078a1c, 0x1078a38-0x1078a48, and the tail 0x1078a64-0x1078a68
  // (ldr x8,[x20]; b 0x10788dc), which reaches the context[0] destroy above.
}
