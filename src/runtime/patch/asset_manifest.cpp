#include "runtime/patch/asset_manifest.h"

#include <cstring>
#include <utility>

namespace nevr::assets {

namespace {

// Offsets of the header fields the engine reads (FUN_140f9f0c0): each array's capacity word and element count.
constexpr size_t kCapacityResources = 0x10;
constexpr size_t kCapacityFiles = 0x50;
constexpr size_t kCapacityChunks = 0x90;
constexpr size_t kCountResources = 0x38;
constexpr size_t kCountFiles = 0x78;
constexpr size_t kCountChunks = 0xb8;

constexpr size_t kResourceBytes = 0x20;
constexpr size_t kFileBytes = 0x28;
constexpr size_t kChunkBytes = 0x10;

void Put32(std::vector<uint8_t>& out, size_t off, uint32_t v) { std::memcpy(out.data() + off, &v, sizeof(v)); }
void Put64(std::vector<uint8_t>& out, size_t off, uint64_t v) { std::memcpy(out.data() + off, &v, sizeof(v)); }

uint32_t Get32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint64_t Get64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

bool Fail(std::string* error, const char* why) {
    if (error) *error = why;
    return false;
}

}  // namespace

std::vector<uint8_t> Encode(const Manifest& m) {
    const size_t resourceBytes = m.resources.size() * kResourceBytes;
    const size_t fileBytes = m.files.size() * kFileBytes;
    const size_t chunkBytes = m.chunks.size() * kChunkBytes;

    std::vector<uint8_t> out(kHeaderBytes + resourceBytes + fileBytes + chunkBytes, 0);
    Put32(out, 0x00, m.packageCount);
    Put32(out, 0x04, m.blockSize);
    Put32(out, kCapacityResources, static_cast<uint32_t>(resourceBytes));
    Put32(out, kCapacityFiles, static_cast<uint32_t>(fileBytes));
    Put32(out, kCapacityChunks, static_cast<uint32_t>(chunkBytes));
    Put64(out, kCountResources, m.resources.size());
    Put64(out, kCountFiles, m.files.size());
    Put64(out, kCountChunks, m.chunks.size());

    size_t at = kHeaderBytes;
    for (const ResourceEntry& r : m.resources) {
        Put64(out, at + 0x00, r.type);
        Put64(out, at + 0x08, r.name);
        Put32(out, at + 0x10, r.chunk);
        Put32(out, at + 0x14, r.a);
        Put32(out, at + 0x18, r.size);
        Put32(out, at + 0x1c, r.b);
        at += kResourceBytes;
    }
    for (const FileEntry& f : m.files) {
        for (size_t i = 0; i < 5; ++i) Put64(out, at + i * 8, f.word[i]);
        at += kFileBytes;
    }
    for (const Chunk& c : m.chunks) {
        Put64(out, at + 0x00, c.first);
        Put64(out, at + 0x08, c.second);
        at += kChunkBytes;
    }
    return out;
}

bool Decode(const uint8_t* data, size_t size, Manifest* out, std::string* error) {
    if (!data || size < kHeaderBytes) return Fail(error, "manifest shorter than its 0xc0 byte header");

    Manifest m;
    m.packageCount = Get32(data + 0x00);
    m.blockSize = Get32(data + 0x04);
    if (m.blockSize > kMaxBlockSize) return Fail(error, "block size above the engine's 0x80000 limit");

    const uint64_t nResources = Get64(data + kCountResources);
    const uint64_t nFiles = Get64(data + kCountFiles);
    const uint64_t nChunks = Get64(data + kCountChunks);

    // Divide rather than multiply: a hostile count must not wrap the byte arithmetic into something that fits.
    const uint64_t room = size - kHeaderBytes;
    if (nResources > room / kResourceBytes) return Fail(error, "resource array would overrun the manifest");
    uint64_t left = room - nResources * kResourceBytes;
    if (nFiles > left / kFileBytes) return Fail(error, "file array would overrun the manifest");
    left -= nFiles * kFileBytes;
    if (nChunks > left / kChunkBytes) return Fail(error, "chunk array would overrun the manifest");
    left -= nChunks * kChunkBytes;
    if (left != 0) return Fail(error, "trailing bytes the array counts do not account for");

    const uint8_t* at = data + kHeaderBytes;
    m.resources.resize(static_cast<size_t>(nResources));
    for (ResourceEntry& r : m.resources) {
        r.type = Get64(at + 0x00);
        r.name = Get64(at + 0x08);
        r.chunk = Get32(at + 0x10);
        r.a = Get32(at + 0x14);
        r.size = Get32(at + 0x18);
        r.b = Get32(at + 0x1c);
        at += kResourceBytes;
    }
    m.files.resize(static_cast<size_t>(nFiles));
    for (FileEntry& f : m.files) {
        for (size_t i = 0; i < 5; ++i) f.word[i] = Get64(at + i * 8);
        at += kFileBytes;
    }
    m.chunks.resize(static_cast<size_t>(nChunks));
    for (Chunk& c : m.chunks) {
        c.first = Get64(at + 0x00);
        c.second = Get64(at + 0x08);
        at += kChunkBytes;
    }

    if (out) *out = std::move(m);
    return true;
}

}  // namespace nevr::assets
