#pragma once

#include <cstdint>

namespace NRadEngine {

struct VkDeviceMemory_T;
using VkDeviceMemory = VkDeviceMemory_T*;

struct SGAllocatedBase {
    std::uint64_t memory;
    std::uint64_t begin;
    std::uint64_t size;
    std::uint64_t end;
};
struct SGImage {
    SGAllocatedBase* allocation;
};

class CGRawInterfaceVK {
public:
    static void Free(VkDeviceMemory memory, unsigned long begin,
                     unsigned long size, unsigned long end);
    static void DeferredDestroy(SGImage& image);
    static void DeferredFree(SGAllocatedBase& allocation);
};

}  // namespace NRadEngine
