// Quest libr15.so VA 0x1092f2c; ReVault binary libr15.so.
// ReVault raw parent 100208228, reconstruction node 100600594; grade M.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CThreadLock;
class CThreadLocker {
public:
    explicit CThreadLocker(CThreadLock* lock);
    ~CThreadLocker();

private:
    std::uintptr_t state_;
};

struct GlobalEntry {
    std::int64_t symbol;
    std::uint64_t discriminator;
    void* value;
};

static_assert(sizeof(GlobalEntry) == 0x18);
static_assert(offsetof(GlobalEntry, discriminator) == 0x08);
static_assert(offsetof(GlobalEntry, value) == 0x10);

class CEnvironment {
public:
    void* GetGlobal(std::int64_t symbol, std::uint64_t discriminator) const;
};

namespace {

template <typename T>
T ReadField(const std::byte* object, std::size_t offset) {
    T value{};
    std::memcpy(&value, object + offset, sizeof(value));
    return value;
}

}  // namespace

// @0x1092f2c — NRadEngine::CEnvironment::GetGlobal
/* @confidence: M — the lock, dirty-sort flag, pair-key lookup and null-on-miss behavior match the decompilation; object fields are accessed at the observed offsets. */
void* CEnvironment::GetGlobal(std::int64_t symbol,
                              std::uint64_t discriminator) const {
    auto* const state = ReadField<std::byte*>(
        reinterpret_cast<const std::byte*>(this), 0);
    const auto lock_address =
        reinterpret_cast<std::uintptr_t>(state) + 0x30;
    CThreadLocker lock(reinterpret_cast<CThreadLock*>(lock_address));

    if (state == nullptr) {
        return nullptr;
    }

    const auto count = ReadField<std::uint64_t>(state, 0xd0);
    if (count == 0) {
        return nullptr;
    }

    auto flags = ReadField<std::uint32_t>(state, 0xd8);
    auto* const entries = ReadField<GlobalEntry*>(state, 0xa0);
    if ((flags & 1U) != 0) {
        std::sort(entries, entries + count,
                  [](const GlobalEntry& left, const GlobalEntry& right) {
                      if (left.symbol != right.symbol) {
                          return left.symbol < right.symbol;
                      }
                      return left.discriminator < right.discriminator;
                  });
        flags &= ~1U;
        std::memcpy(state + 0xd8, &flags, sizeof(flags));
    }

    std::uint64_t first = 0;
    std::uint64_t last = count;
    while (first < last) {
        const auto middle = first + ((last - first) >> 1);
        const auto& entry = entries[middle];
        if (entry.symbol < symbol ||
            (entry.symbol == symbol &&
             entry.discriminator < discriminator)) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }

    if (first < count && entries[first].symbol == symbol &&
        entries[first].discriminator == discriminator) {
        return entries[first].value;
    }
    return nullptr;
}

}  // namespace NRadEngine
