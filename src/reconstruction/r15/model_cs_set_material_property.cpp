// NRadEngine::CModelCS::SetMaterialProperty
// Quest libr15.so VA 0x16b4c74; ReVault binary 576.
// ReVault raw parent 100243038, reconstruction node 100600680; grade H: all 123 instructions traced to the ret at 0x16b4e2c, callees NRadEngine::CGMaterial::SetPropertyValue, NRadEngine::CSymbol64::String and NRadEngine::NWriteLog::WriteLog named and none a placeholder, the three format strings read from the binary, CModelCS, model-data and CGMaterial fields are raw byte offsets; syntax-checked.
// @0x16b4c74 — NRadEngine::CModelCS::SetMaterialProperty
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Reads a T at a byte offset. The layouts of the component, the model data and the material entry are not
// recovered, so each field is read where the listing reads it.
template <typename T>
T ReadAt(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

}  // namespace

namespace NRadEngine {

// A 64-bit symbol key.
struct CSymbol64 {
    std::uint64_t key;
    // NRadEngine::CSymbol64::String() const @ 0x12ccb48 (thunk 0x1038ec0).
    const char* String() const;
};
static_assert(sizeof(CSymbol64) == 0x08);

// SCompactPoolHandle: a 32-bit pool index, passed in w1.
struct SCompactPoolHandle {
    std::uint32_t index;
};

// Enumerators are not established. The listing passes 1.
enum EMatPropColorSpace : std::uint32_t {};

class CGMaterial {
public:
    // NRadEngine::CGMaterial::SetPropertyValue(CSymbol64, void const*, unsigned int, EMatPropColorSpace) @ 0x19591a4
    // (thunk 0xfcfcc0). The caller tests the low 32 bits of the result (w0).
    std::int32_t SetPropertyValue(CSymbol64 name, const void* value, std::uint32_t size, EMatPropColorSpace space);
};

class CModelCS {
public:
    // NRadEngine::CModelCS::SetMaterialProperty(SCompactPoolHandle, CSymbol64, CSymbol64, float, unsigned int)
    // @0x16b4c74 (thunk 0xff3c40).
    void SetMaterialProperty(SCompactPoolHandle handle, CSymbol64 material, CSymbol64 property, float value,
                             std::uint32_t flag);
};

namespace NWriteLog {
// Enumerators are not established. This function uses 8 (error level).
enum ELogLevel : std::int32_t {};
// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...) @ 0x1088c20 (thunk 0x104b960). Variadic.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

void CModelCS::SetMaterialProperty(SCompactPoolHandle handle, CSymbol64 material, CSymbol64 property, float value,
                                   std::uint32_t flag) {
    constexpr std::uint64_t kNone = ~std::uint64_t{0};  // the -1 the listing compares against
    constexpr std::size_t kMaterialStride = 0x90;        // one CGMaterial entry of the model's array

    // 0x16b4c90..0x16b4cac: the low 16 bits of the handle index a ushort table at +0xd0 (4-byte stride); the
    // 16-byte entry at +0x100 + slot * 0x10 holds the model data at its +8.
    const std::uint16_t slot =
        ReadAt<std::uint16_t>(ReadAt<const void*>(this, 0xd0), (std::size_t{handle.index} & 0xffffu) * 4);
    const void* const model = ReadAt<const void*>(ReadAt<const void*>(this, 0x100), std::size_t{slot} * 0x10 + 0x08);

    // sp+0x14: the float the callee reads by address. any_material is bVar1 (w23): an entry matched. any_set is
    // bVar2 (w22): a SetPropertyValue returned nonzero.
    const float stored = value;
    bool any_material = false;
    bool any_set = false;

    // 0x16b4cb0..0x16b4cd8: the material count at +0x3a8 and the array at +0x378. The listing's zero test is on
    // count * 9 << 4, which is count * 0x90.
    const std::uint64_t count = ReadAt<std::uint64_t>(model, 0x3a8);
    const std::uint64_t span = (count * 9) << 4;
    if (count != 0 && span != 0) {
        std::byte* entry = ReadAt<std::byte*>(model, 0x378);
        std::uint64_t remaining = span - kMaterialStride;  // x24: bytes after this entry
        for (;;) {
            // 0x16b4cdc..0x16b4cfc: key -1 matches every entry; otherwise the entry's instance (+8) matches on its
            // key at +0x38, or on the key at +0x1a0 of the object at +0x70 of the instance.
            bool matched = material.key == kNone;
            if (!matched) {
                const void* const instance = ReadAt<const void*>(entry, 0x08);
                matched = ReadAt<std::uint64_t>(instance, 0x38) == material.key ||
                          ReadAt<std::uint64_t>(ReadAt<const void*>(instance, 0x70), 0x1a0) == material.key;
            }
            if (matched) {
                // 0x16b4d04..0x16b4d28: SetPropertyValue(entry, property, &stored, 4, 1).
                any_material = true;
                const std::int32_t status = reinterpret_cast<CGMaterial*>(entry)->SetPropertyValue(
                    property, &stored, 4, static_cast<EMatPropColorSpace>(1));
                any_set = (status != 0) || any_set;
            }
            if (remaining == 0) {
                break;
            }
            remaining -= kMaterialStride;
            entry += kMaterialStride;
        }
    }

    // 0x16b4d50: a zero flag returns without a message.
    if (flag == 0) {
        return;
    }

    // The model's name key: the resource at +0xa8 holds it at +0x38; a null resource reads as -1.
    const auto model_name = [model]() {
        const void* const resource = ReadAt<const void*>(model, 0xa8);
        return CSymbol64{resource != nullptr ? ReadAt<std::uint64_t>(resource, 0x38) : ~std::uint64_t{0}};
    };
    const auto level = static_cast<NWriteLog::ELogLevel>(8);

    if (!any_material) {
        // 0x16b4d90..0x16b4e14: no entry matched the material key.
        const char* const material_text = material.String();
        const char* const model_text = model_name().String();
        const char* const property_text = property.String();
        NWriteLog::WriteLog(level, 0ULL,
                            "Unable to find material '%s' on model '%s.radscn' to set property '%s'.",
                            material_text, model_text, property_text);
        return;
    }
    if (any_set) {
        return;  // 0x16b4d58
    }
    if (material.key == kNone) {
        // 0x16b4dd8..0x16b4e58: the wildcard key matched nothing that was set.
        const char* const property_text = property.String();
        const char* const model_text = model_name().String();
        NWriteLog::WriteLog(level, 0ULL, "Unable to find property '%s' on any material in model '%s.radscn'.",
                            property_text, model_text);
        return;
    }
    // 0x16b4d5c..0x16b4df8: an entry matched but no SetPropertyValue succeeded.
    const char* const property_text = property.String();
    const char* const material_text = material.String();
    const char* const model_text = model_name().String();
    NWriteLog::WriteLog(level, 0ULL,
                        "Unable to find property '%s' in material '%s' on model '%s.radscn'. Did you mark it as animated?",
                        property_text, material_text, model_text);
}

}  // namespace NRadEngine
