#pragma once

// The engine's package manifest, as the game's own reader consumes it, so plugin-carried assets reach the
// resource manager through the same parse and merge path as shipped content.
//
// Layout (all little endian), decoded from FUN_140f9f0c0 / FUN_140f9f310 / FUN_140f9f550 / FUN_140f9f6b0 in
// echovr.exe and checked against two shipped manifests whose decompressed length it reproduces exactly:
//
//   0x00  u32  packageCount        package files named <id>_<n> that follow the manifest
//   0x04  u32  blockSize           largest chunk (the engine rejects a manifest above 0x80000)
//   0x08  three array headers, 0x40 bytes apart, each mirroring the engine's in-memory struct:
//           +0x00 u64 0, +0x08 u64 0, +0x10 u32 capacityBytes, +0x14 u32 0   (file offsets 0x08/0x10/0x50/0x90)
//           the element count sits at 0x38 (resources), 0x78 (files), 0xb8 (chunks)
//   0xc0  resources  count x 0x20  { u64 type, u64 name, u32 chunk, u32 a, u32 size, u32 b }
//         files      count x 0x28  { 5 x u64 }  (key + 3 u64, read only in the file-server path)
//         chunks     count x 0x10  { u64, u64 }  (package index and offset, raw)
//
// Only the counts decide how much is read; the capacity words are the writer's allocation hints.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nevr::assets {

struct ResourceEntry {
    uint64_t type = 0;
    uint64_t name = 0;
    uint32_t chunk = 0;  // index into Manifest::chunks (the engine adds the manifest's base chunk index)
    uint32_t a = 0;      // meaning not yet established
    uint32_t size = 0;   // 0x100094 for the 1 MiB DDS in the shipped small manifest: a byte size
    uint32_t b = 0;      // meaning not yet established
};

struct FileEntry {
    uint64_t word[5] = {};
};

struct Chunk {
    uint64_t first = 0;
    uint64_t second = 0;
};

struct Manifest {
    uint32_t packageCount = 0;
    uint32_t blockSize = 0;
    std::vector<ResourceEntry> resources;
    std::vector<FileEntry> files;
    std::vector<Chunk> chunks;
};

// Largest block size the engine accepts (cpackagemanifest.cpp, "larger block size ... than what we were
// initialized with"), read from the comparison in FUN_140fa09f0.
constexpr uint32_t kMaxBlockSize = 0x80000;

constexpr size_t kHeaderBytes = 0xc0;

// Serialise in the engine's layout. Capacity words are the exact byte size of each array.
std::vector<uint8_t> Encode(const Manifest& manifest);

// Parse; false with a reason in *error when the bytes are not a manifest the engine would accept
// (short, array sizes that overrun, or trailing bytes the counts do not account for).
bool Decode(const uint8_t* data, size_t size, Manifest* out, std::string* error);

}  // namespace nevr::assets
