// CSetMaterialPropertyNode::Update
// Quest libr15.so VA 0x282d55c; ReVault binary 576.
// ReVault raw parent 100328429, reconstruction node 100600466; grade M: every branch and field offset checked against the listing; M because the vtable slot +0x1d0 is unnamed and the node data fields are raw offsets.
// @0x282d55c — NRadEngine::CSetMaterialPropertyNode::Update
/* @confidence: M */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Reads or writes a T at a byte offset. The layouts of the node, the script thread, the node data
// and the component handle are not recovered, so each field is accessed where the listing accesses it.
template <typename T>
T ReadAt(const void* base, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

template <typename T>
void WriteAt(void* base, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

}  // namespace

namespace NRadEngine {

struct SScriptThread;
struct CncaGameSpaceMT;
struct SSetMaterialPropertyNodeData;

// A 64-bit symbol key.
struct CSymbol64 {
    std::uint64_t key;
    const char* String() const;  // CSymbol64::String() const, 0x12ccb48 (thunk 0x1038ec0)
};

// SCompactPoolHandle: a 32-bit pool index, passed in w1.
struct SCompactPoolHandle {
    std::uint32_t index;
};

enum EMatPropColorSpace : std::uint32_t {};

struct C4Vector {
    float x;
    float y;
    float z;
    float w;
};

// STempString<N>: the temporary that MakeTStrPrintF returns in the caller's frame.
template <std::size_t N>
struct STempString {
    unsigned char storage[N];
};

struct CModelCS {
    // CModelCS::SetMaterialProperty(SCompactPoolHandle, CSymbol64, CSymbol64, float, unsigned int)
    // @0x16b4c74 (thunk 0xff3c40)
    void SetMaterialProperty(SCompactPoolHandle handle, CSymbol64 a, CSymbol64 b, float value,
                             std::uint32_t flag);
    // CModelCS::SetMaterialProperty(SCompactPoolHandle, CSymbol64, CSymbol64, C4Vector,
    //   EMatPropColorSpace, unsigned int)  @0x16b4f0c (thunk 0x102cc50)
    void SetMaterialProperty(SCompactPoolHandle handle, CSymbol64 a, CSymbol64 b, C4Vector color,
                             EMatPropColorSpace space, std::uint32_t flag);
};

template <typename T>
class CComponentHandle {
public:
    // CComponentHandle<T>::RefreshCache() const, 0x1313e00 (thunk 0x1009460)
    void RefreshCache() const;
};

namespace SComponentNodeUtils {
// SComponentNodeUtils::Actor(SScriptThread const&, CncaGameSpaceMT const&, CSymbol64,
//   CncaGameSpaceMT*&, SActor&, unsigned int&), 0x2159714 (thunk 0x1015330).
// Reference parameters are declared as pointers; the AArch64 ABI passes them the same way.
int Actor(const SScriptThread* thread, const CncaGameSpaceMT* space, CSymbol64 key,
          CncaGameSpaceMT** out_space, void* out_actor, std::uint32_t* out_flags);

// SComponentNodeUtils::ComponentHandle<CModelCS>(SScriptThread const&, CComponentHandle<CModelCS>&,
//   SScriptActorData, CSymbol64), 0x273a944 (thunk 0x1023550).
// SScriptActorData is passed as a pointer to the caller's copy of its three quadwords.
template <typename T>
void ComponentHandle(const SScriptThread* thread, CComponentHandle<T>* handle,
                     const std::uint64_t* actor_key, CSymbol64 name);

}  // namespace SComponentNodeUtils

struct CncaGameSpace {
    // CncaGameSpace::GameSpaceMT(CSymbol64) const, 0x1906704 (thunk 0xff5650).
    // Ghidra types the result long; it is used here as an object pointer.
    CncaGameSpaceMT* GameSpaceMT(CSymbol64 key) const;
};

// ScriptError(SScriptThread const*, char const*), 0x2710d68 (thunk 0x10460c0).
void ScriptError(const SScriptThread* thread, const char* message);

// MakeTStrPrintF<512>(char const*, ...), 0x1086b9c (thunk 0x102b370).
template <std::size_t N>
STempString<N> MakeTStrPrintF(const char* format, ...);

class CSetMaterialPropertyNode {
public:
    // CSetMaterialPropertyNode::Update(SSetMaterialPropertyNodeData const&), 0x282d55c.
    bool Update(const SSetMaterialPropertyNodeData& data);
};

bool CSetMaterialPropertyNode::Update(const SSetMaterialPropertyNodeData& data) {
    constexpr std::uint64_t kNone = ~std::uint64_t{0};   // the -1 the listing compares with

    // 0x282d58c: plVar4 = *this, the script thread.
    const void* const thread = ReadAt<const void*>(this, 0x00);

    // 0x282d570..0x282d590: the first 24 bytes of the node data.
    std::uint64_t in0 = ReadAt<std::uint64_t>(&data, 0x00);
    std::uint64_t in1 = ReadAt<std::uint64_t>(&data, 0x08);
    std::uint64_t in2 = ReadAt<std::uint64_t>(&data, 0x10);

    // Frame block at sp+0x200, 0x50 bytes: the SActor that Actor fills, then the
    // CComponentHandle<CModelCS> the rest of the function uses.
    unsigned char handle[0x50];
    CncaGameSpaceMT* space = nullptr;   // sp+0x000: CncaGameSpaceMT* out of Actor
    std::uint32_t actor_flags = 0;      // sp+0x27c: unsigned int out of Actor

    if (in0 == kNone || static_cast<std::int16_t>(in1) == -1) {
        // 0x282d5dc..0x282d608: resolve the actor through SComponentNodeUtils::Actor.
        WriteAt<std::uint64_t>(handle, 0x00, 0x0000ffffffffffffULL);
        const void* const script_x = ReadAt<const void*>(thread, 0x00);
        const void* const script_p = ReadAt<const void*>(script_x, 0x30);
        const void* const script_q = ReadAt<const void*>(script_p, 0x00);
        const void* const game_space = ReadAt<const void*>(script_q, 0x80);
        const int resolved = SComponentNodeUtils::Actor(
            static_cast<const SScriptThread*>(thread),
            static_cast<const CncaGameSpaceMT*>(game_space), CSymbol64{in2}, &space, handle,
            &actor_flags);
        if (resolved != 0) {
            // 0x282d60c..0x282d640.
            in0 = ReadAt<std::uint64_t>(space, 0x58);
            const std::uint64_t actor_first = ReadAt<std::uint64_t>(handle, 0x00);
            in1 = actor_first;
            if (in2 == kNone) {
                const void* const table =
                    ReadAt<const void*>(ReadAt<const void*>(space, 0x370), 0x80);
                const std::uint32_t scaled = static_cast<std::uint32_t>(actor_first) << 3;
                in2 = ReadAt<std::uint64_t>(table, static_cast<std::size_t>(scaled & 0x7fff8u));
            }
        }
    } else if (in2 == kNone) {
        // 0x282d5b0..0x282d5d8: GameSpaceMT on the same game-space pointer.
        const void* const script_x = ReadAt<const void*>(thread, 0x00);
        const void* const script_p = ReadAt<const void*>(script_x, 0x30);
        const void* const script_q = ReadAt<const void*>(script_p, 0x00);
        const void* const game_space = ReadAt<const void*>(script_q, 0x80);
        CncaGameSpaceMT* const found =
            static_cast<const CncaGameSpace*>(game_space)->GameSpaceMT(CSymbol64{in0});
        if (found != nullptr) {
            const void* const table = ReadAt<const void*>(ReadAt<const void*>(found, 0x370), 0x80);
            const std::uint32_t scaled = static_cast<std::uint32_t>(in1) << 3;
            in2 = ReadAt<std::uint64_t>(table, static_cast<std::size_t>(scaled & 0x7fff8u));
        }
    }

    // 0x282d644..0x282d674: the component handle block takes its initial values.
    WriteAt<std::uint64_t>(handle, 0x00, 0);
    WriteAt<std::uint16_t>(handle, 0x1e, 0);
    WriteAt<std::uint64_t>(handle, 0x10, kNone);
    WriteAt<std::uint64_t>(handle, 0x08, kNone);
    WriteAt<std::uint64_t>(handle, 0x16, kNone);
    WriteAt<std::uint64_t>(handle, 0x20, 0xffffu);
    WriteAt<std::uint64_t>(handle, 0x30, kNone);
    WriteAt<std::uint64_t>(handle, 0x28, kNone);
    WriteAt<std::uint64_t>(handle, 0x48, 0xffffu);
    WriteAt<std::uint64_t>(handle, 0x38, kNone);

    // 0x282d678..0x282d698: the SScriptActorData copy (sp+0x000) of in0, in1, in2, then the call.
    const std::uint64_t actor_key[3] = {in0, in1, in2};
    auto* const component = reinterpret_cast<CComponentHandle<CModelCS>*>(handle);
    SComponentNodeUtils::ComponentHandle<CModelCS>(
        static_cast<const SScriptThread*>(thread), component, actor_key,
        CSymbol64{ReadAt<std::uint64_t>(&data, 0x18)});
    component->RefreshCache();                                   // 0x282d6a0

    CModelCS* model = ReadAt<CModelCS*>(handle, 0x40);          // local_70, handle +0x40
    std::uint32_t mode = 0;                                      // uVar1
    bool report_missing = false;                                 // 0x282d724 path
    bool report_mode = false;                                    // 0x282d824 path
    if (model == nullptr || ReadAt<std::uint64_t>(handle, 0x00) == 0) {  // 0x282d6a8, 0x282d6b0
        report_missing = true;
    } else {
        component->RefreshCache();                               // 0x282d6b8
        const void* const vtable = ReadAt<const void*>(model, 0x00);
        // UNRESOLVED: CModelCS virtual slot +0x1d0 (name not recovered), called as
        // (this, index) and tested against zero.
        using SlotFn = int (*)(CModelCS*, std::uint32_t);
        const SlotFn slot = ReadAt<SlotFn>(vtable, 0x1d0);
        const int present = slot(model, ReadAt<std::uint32_t>(handle, 0x48));  // 0x282d6bc..0x282d6cc
        if (present == 0) {
            report_missing = true;                               // 0x282d6d0
        } else {
            component->RefreshCache();                           // 0x282d6d8
            mode = ReadAt<std::uint32_t>(&data, 0x30);           // 0x282d6dc
            model = ReadAt<CModelCS*>(handle, 0x40);             // 0x282d6e0, reloaded
            if (mode == 1) {
                // 0x282d7e4..0x282d80c.
                const std::uint32_t color_space = ReadAt<std::uint32_t>(&data, 0x4c);
                component->RefreshCache();                       // 0x282d7ec
                model->SetMaterialProperty(
                    SCompactPoolHandle{ReadAt<std::uint32_t>(handle, 0x48)},
                    CSymbol64{ReadAt<std::uint64_t>(&data, 0x20)},
                    CSymbol64{ReadAt<std::uint64_t>(&data, 0x28)},
                    C4Vector{ReadAt<float>(&data, 0x38), ReadAt<float>(&data, 0x3c),
                             ReadAt<float>(&data, 0x40), ReadAt<float>(&data, 0x44)},
                    static_cast<EMatPropColorSpace>(color_space), 1u);
            } else if (mode == 0) {
                // 0x282d6f0..0x282d70c.
                component->RefreshCache();                       // 0x282d6f4
                model->SetMaterialProperty(
                    SCompactPoolHandle{ReadAt<std::uint32_t>(handle, 0x48)},
                    CSymbol64{ReadAt<std::uint64_t>(&data, 0x20)},
                    CSymbol64{ReadAt<std::uint64_t>(&data, 0x28)},
                    ReadAt<float>(&data, 0x34), 1u);
            } else {
                report_mode = true;                              // 0x282d824
            }
        }
    }

    if (report_missing) {
        // 0x282d724..0x282d780: "couldn't find actor".
        const char* const actor_name = CSymbol64{in2}.String();
        const char* const model_name = CSymbol64{ReadAt<std::uint64_t>(&data, 0x18)}.String();
        const char* const script_name =
            static_cast<const CSymbol64*>(ReadAt<const void*>(thread, 0x00))->String();
        STempString<512> text = MakeTStrPrintF<512>(
            "SetMaterialPropertyNode couldn't find actor %s, model %s\tScript:           %s\n\tNode UUID:        %08llx\n",
            actor_name, model_name, script_name,
            static_cast<unsigned long long>(ReadAt<std::uint64_t>(thread, 0x10)));
        ScriptError(static_cast<const SScriptThread*>(thread),
                    reinterpret_cast<const char*>(&text));
    } else if (report_mode) {
        // 0x282d824..0x282d850: "unknown property type".
        const char* const script_name =
            static_cast<const CSymbol64*>(ReadAt<const void*>(thread, 0x00))->String();
        STempString<512> text = MakeTStrPrintF<512>(
            "SetMaterialPropertyNode unknown property type %u\tScript:           %s\n\tNode UUID:        %08llx\n",
            mode, script_name, static_cast<unsigned long long>(ReadAt<std::uint64_t>(thread, 0x10)));
        ScriptError(static_cast<const SScriptThread*>(thread),
                    reinterpret_cast<const char*>(&text));
    }

    // 0x282d784 (or 0x282d710, 0x282d814 after a call): the elapsed-time value.
    std::int64_t elapsed = ReadAt<std::int64_t>(thread, 0x40);   // thread[8]
    if (elapsed == -1) {
        // 0x282d794..0x282d7a0.
        elapsed = ReadAt<std::int64_t>(ReadAt<const void*>(thread, 0x00), 0x58) -
                  ReadAt<std::int64_t>(thread, 0x30);            // *(*this + 0x58) - thread[6]
    }
    // 0x282d7a4..0x282d7c8: (float)elapsed * 1e-6f + 2^-23 (fused), compared below the float at +0x48.
    return std::fma(static_cast<float>(elapsed), 1e-6f, 1.1920929e-07f) <
           ReadAt<float>(&data, 0x48);
}

}  // namespace NRadEngine
