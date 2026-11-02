// Quest libr15.so VA 0x1092de8; ReVault binary libr15.so.
// ReVault raw parent 100208227, reconstruction node 100600592; grade M.
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

struct TlsKeyEntry {
    std::int64_t symbol;
    std::uint64_t discriminator;
    std::uint32_t handle;
    std::uint32_t padding;
};

static_assert(sizeof(TlsKeyEntry) == 0x18);
static_assert(offsetof(TlsKeyEntry, handle) == 0x10);
static_assert(offsetof(TlsKeyEntry, padding) == 0x14);

class CSysThread {
public:
    static std::uint32_t CreateTLS();
};

namespace CMemory {
void Fill(void* destination, unsigned char value, std::uint64_t count);
}

template <typename Entry>
class CTable {
public:
    static void Insert(std::uintptr_t entries_field, Entry* insertion_index,
                       std::uintptr_t entry);
};

class CEnvironment {
public:
    std::uint32_t GetTlsHandle(std::int64_t symbol,
                               std::uint64_t discriminator);
};

namespace {

template <typename T>
T ReadField(const std::byte* object, std::size_t offset) {
    T value{};
    std::memcpy(&value, object + offset, sizeof(value));
    return value;
}

}  // namespace

// @0x1092de8 — NRadEngine::CEnvironment::GetTlsHandle
/* @confidence: M — the locked lower-bound lookup, sorted-flag handling, TLS creation and table insertion match the decompilation; class layout is represented by observed offsets. */
std::uint32_t CEnvironment::GetTlsHandle(std::int64_t symbol,
                                         std::uint64_t discriminator) {
    auto* const state = ReadField<std::byte*>(
        reinterpret_cast<const std::byte*>(this), 0);
    CThreadLocker lock(reinterpret_cast<CThreadLock*>(state));

    auto* const count_field = state + 0x90;
    const auto count = ReadField<std::uint64_t>(state, 0x90);
    auto flags = ReadField<std::uint32_t>(state, 0x98);
    auto* entries = ReadField<TlsKeyEntry*>(state, 0x60);

    if (count != 0 && (flags & 1U) != 0) {
        std::sort(entries, entries + count,
                  [](const TlsKeyEntry& left, const TlsKeyEntry& right) {
                      if (left.symbol != right.symbol) {
                          return left.symbol < right.symbol;
                      }
                      return left.discriminator < right.discriminator;
                  });
        flags &= ~1U;
        std::memcpy(state + 0x98, &flags, sizeof(flags));
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
        return entries[first].handle;
    }

    TlsKeyEntry entry{};
    entry.symbol = symbol;
    entry.discriminator = discriminator;
    entry.handle = CSysThread::CreateTLS();
    CMemory::Fill(&entry.padding, 0, sizeof(entry.padding));
    CTable<TlsKeyEntry>::Insert(
        reinterpret_cast<std::uintptr_t>(count_field),
        reinterpret_cast<TlsKeyEntry*>(static_cast<std::uintptr_t>(first)),
        reinterpret_cast<std::uintptr_t>(&entry));
    return entry.handle;
}

}  // namespace NRadEngine
