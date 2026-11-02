// NRadEngine::CTaskKernel::ExecuteTask
// Quest libr15.so VA 0x140345c; ReVault binary 576.
// ReVault raw parent 100226426, reconstruction node 100600683; grade H: all 303 instructions (0x140345c-0x1403914, ret at 0x14038dc) checked against the disassembly, syntax-checked; 0 placeholders (ReVault's FUN_01303628 at 0x1403628 and FUN_01303850 at 0x1403850 are fragments of this body, not callees).
// @0x140345c — NRadEngine::CTaskKernel::ExecuteTask
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// Only the first 8 bytes are touched here: the pending-task counter (read with ldar, decremented with ldaxr/stlxr).
class TTaskGroup;

// One queued task. 64 bytes: ReEnqueue (0x1403dac) copies it as eight qwords and ExecuteTask moves it in four
// 16-byte pieces. The fields this function uses are listed in kTask* below.
struct STask {
    alignas(8) std::byte raw[0x40];
};
static_assert(sizeof(STask) == 0x40);

// Layout not recovered beyond the offsets in kQueue* below. The kernel embeds one at CTaskKernel + 0x08 and the next
// kernel member is at + 0x160 (constructor 0x1404010), which makes it 0x158 bytes.
template <typename T>
class CLockFreeQueue {
private:
    alignas(8) std::byte raw_[0x158];
};
static_assert(sizeof(CLockFreeQueue<STask>) == 0x158);

class CTaskKernel {
public:
    // Mangled name _ZN10NRadEngine11CTaskKernel11ExecuteTaskEPVKNS_10TTaskGroupEy (string at 0x4ec8f9). Entry
    // registers: x0 this, x1 group, x2 id. The symbol carries no parameter names; the return value is w0 = 0 or 1.
    bool ExecuteTask(const volatile TTaskGroup* group, unsigned long long id);
    // NRadEngine::CTaskKernel::ReEnqueue(CLockFreeQueue<STask>&, STask const&) @0x1403dac, called through the stub at 0xfeab60.
    void ReEnqueue(CLockFreeQueue<STask>& queue, const STask& task);

private:
    // Size not recovered. The constructor (0x1404010) stores up to + 0x1b0 (8 bytes), so the object holds at least 0x1b8.
    alignas(8) std::byte raw_[0x1b8];
};
static_assert(sizeof(CTaskKernel) >= 0x1b8);
static_assert(alignof(CTaskKernel) == 8);

namespace {

// CTaskKernel offsets.
constexpr std::size_t kRejectionQueueOffset = 0x160;  // CLockFreeQueue<STask>*; SetRejectionQueue (0x14042f8) is `str x1, [x0, #0x160]`
constexpr std::size_t kQueueListOffset = 0x168;       // CLockFreeQueue<STask>**: the queues this kernel drains
constexpr std::size_t kQueueCountOffset = 0x198;      // int64: entries in that array
constexpr std::size_t kThreadSlotOffset = 0x1a0;      // uint32: thread slot; Initialize (0x1404114) stores its x3 here

// CLockFreeQueue<STask> offsets.
constexpr std::size_t kQueueHeaderOffset = 0x00;  // pointer to the header block below
constexpr std::size_t kQueueNodesOffset = 0x148;  // pointer to the node array

// Header block (what *(queue + 0) points at).
constexpr std::size_t kHeadOffset = 0x00;      // uint64: node index (low 32 bits) and tag (high 32 bits) of the head
constexpr std::size_t kTailOffset = 0x40;      // uint64: same shape, the tail
constexpr std::size_t kFreeListOffset = 0x80;  // uint64: same shape, the head of the free node list
constexpr std::size_t kCountOffset = 0xc0;     // int64: number of queued tasks

// Node array: 0x80-byte nodes, the uint64 link at +0 (index low 32 bits, -1 means none), the STask at +8.
constexpr std::size_t kNodeShift = 7;
constexpr std::size_t kNodePayloadOffset = 8;

// STask offsets.
constexpr std::size_t kTaskFunctionOffset = 0x00;  // code pointer, called as function(kernel, &task)
constexpr std::size_t kTaskSlotMaskOffset = 0x18;  // uint32: bit n set means the thread with slot n may run it
constexpr std::size_t kTaskPendingOffset = 0x28;   // TTaskGroup*: counter that must reach 0 before the task runs
constexpr std::size_t kTaskGroupOffset = 0x30;     // TTaskGroup*: the task's own group, decremented after it runs
constexpr std::size_t kTaskIdOffset = 0x38;        // uint64: compared with the id argument

constexpr unsigned long long kAnyId = ~0ULL;  // cmn x20, #1 at 0x1403870; mov x2, #-1 at 0x1403834

std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::int64_t LoadI64(const std::byte* base, std::size_t offset) {
    std::int64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::byte* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

TTaskGroup* LoadGroup(const std::byte* base, std::size_t offset) {
    return reinterpret_cast<TTaskGroup*>(LoadPtr(base, offset));
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// Plain ldr of a word other threads write (queue header, links, count).
std::uint64_t LoadShared(const std::byte* base, std::size_t offset) {
    return __atomic_load_n(reinterpret_cast<const std::uint64_t*>(base + offset), __ATOMIC_RELAXED);
}

// ldar of a group counter.
std::int64_t LoadAcquire(const volatile TTaskGroup* group) {
    return __atomic_load_n(reinterpret_cast<const volatile std::int64_t*>(group), __ATOMIC_ACQUIRE);
}

// ldaxr / cmp / stlxr: true when the word held `expected` and now holds `desired`.
bool CompareExchange(std::byte* address, std::uint64_t expected, std::uint64_t desired) {
    return __atomic_compare_exchange_n(reinterpret_cast<std::uint64_t*>(address), &expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

// ldaxr / sub #1 / stlxr.
void AtomicDecrement(void* address) {
    static_cast<void>(__atomic_fetch_sub(static_cast<std::int64_t*>(address), 1, __ATOMIC_ACQ_REL));
}

}  // namespace

bool CTaskKernel::ExecuteTask(const volatile TTaskGroup* group, unsigned long long id) {
    std::byte* const self = reinterpret_cast<std::byte*>(this);

    // 0x1403480-0x1403484: x24 = queue count; none -> return 0 (0x14038bc).
    const std::int64_t queue_count = LoadI64(self, kQueueCountOffset);
    if (queue_count == 0) {
        return false;
    }

    std::int64_t queue_index = 0;  // x25
    do {
        // 0x14034ac-0x14034b4: x22 = queues[queue_index], x8 = its header.
        std::byte* const queue =
            LoadPtr(LoadPtr(self, kQueueListOffset), static_cast<std::size_t>(queue_index) * sizeof(void*));
        std::byte* header = LoadPtr(queue, kQueueHeaderOffset);

        // 0x14034b8-0x14034c4: the count is read twice; the first read bounds the attempts, the second gates the loop.
        const std::int64_t count_at_entry = static_cast<std::int64_t>(LoadShared(header, kCountOffset));
        if (static_cast<std::int64_t>(LoadShared(header, kCountOffset)) >= 1) {
            const std::int64_t attempt_limit = count_at_entry > 0 ? count_at_entry : 0;  // bic x28, x9, x9, ASR #63
            std::int64_t attempts = 0;                                                    // x27

            // The binary holds two copies of this loop, one entered when group == nullptr (0x1403674) and one when it
            // is not (0x14034d4). They are the same code except that the copy for a null group has no group test at
            // 0x140363c-0x1403644.
            for (;;) {
                // 0x14034d4-0x14034dc: no node array -> leave this queue (0x14038b0).
                std::byte* nodes = LoadPtr(queue, kQueueNodesOffset);
                if (nodes == nullptr) {
                    break;
                }

                // The frame's staging copy (sp+0x80) and task (sp+0x40).
                STask staged;
                STask task;
                std::uint64_t head = 0;  // [sp+0x140]
                bool taken = false;

                // 0x14034ec-0x14035c8: take the task at the head of the queue.
                for (;;) {
                    head = LoadShared(header, kHeadOffset);
                    const std::uint64_t tail = LoadShared(header, kTailOffset);
                    const std::uint64_t link = LoadShared(nodes, (head & 0xffffffffU) << kNodeShift);
                    if (head == LoadShared(header, kHeadOffset) && tail == LoadShared(header, kTailOffset)) {
                        const bool no_next = static_cast<std::int32_t>(link) == -1;  // cmn w11, #1
                        if (head == tail) {
                            if (no_next) {
                                break;  // 0x1403548: empty
                            }
                            // 0x1403554-0x1403568: help the tail forward, whether or not it worked, then read again.
                            static_cast<void>(CompareExchange(header + kTailOffset, tail, link));
                        } else {
                            if (no_next) {
                                break;  // 0x1403570
                            }
                            // 0x1403574-0x1403590: copy the next node's task before swinging the head.
                            std::memcpy(staged.raw, nodes + ((link & 0xffffffffU) << kNodeShift) + kNodePayloadOffset,
                                        sizeof staged.raw);
                            // 0x140359c-0x14035ac
                            if (CompareExchange(header + kHeadOffset, head, link)) {
                                taken = true;
                                break;
                            }
                        }
                    }
                    // 0x14034e0-0x14034e8: clrex, reload the node array and the header, read again.
                    nodes = LoadPtr(queue, kQueueNodesOffset);
                    header = LoadPtr(queue, kQueueHeaderOffset);
                }
                if (!taken) {
                    break;  // 0x14038b0
                }
                std::memcpy(task.raw, staged.raw, sizeof task.raw);  // 0x14035b0-0x14035c4

                // 0x14035d0-0x1403610: push the old head node onto the free list, then retry from the reload if the
                // list head moved.
                for (;;) {
                    const std::uint64_t free_head = LoadShared(LoadPtr(queue, kQueueHeaderOffset), kFreeListOffset);
                    StoreU64(LoadPtr(queue, kQueueNodesOffset), (head & 0xffffffffU) << kNodeShift, free_head);
                    if (CompareExchange(LoadPtr(queue, kQueueHeaderOffset) + kFreeListOffset, free_head, head)) {
                        break;
                    }
                }
                // 0x1403614-0x1403628
                AtomicDecrement(LoadPtr(queue, kQueueHeaderOffset) + kCountOffset);

                // 0x140362c-0x1403638: a task whose pending counter is null or already 0 goes straight to the run test.
                TTaskGroup* const pending = LoadGroup(task.raw, kTaskPendingOffset);
                bool run_test = true;
                if (pending != nullptr && LoadAcquire(pending) != 0) {
                    if (group != nullptr && LoadGroup(task.raw, kTaskGroupOffset) == group) {
                        // 0x1403824-0x1403848: it belongs to the group the caller waits on; work its prerequisites off.
                        while (LoadAcquire(pending) != 0) {
                            ExecuteTask(pending, kAnyId);
                        }
                    } else {
                        // 0x1403648: x1 = the rejection queue.
                        CLockFreeQueue<STask>* const rejection =
                            reinterpret_cast<CLockFreeQueue<STask>*>(LoadPtr(self, kRejectionQueueOffset));
                        if (rejection == nullptr) {
                            // 0x1403650-0x1403670: back onto the queue it came from; no attempt is counted.
                            ReEnqueue(*reinterpret_cast<CLockFreeQueue<STask>*>(queue), task);
                            header = LoadPtr(queue, kQueueHeaderOffset);
                            if (static_cast<std::int64_t>(LoadShared(header, kCountOffset)) >= 1) {
                                continue;  // b.ge 0x14034d4
                            }
                            break;  // b 0x14038b0
                        }
                        // 0x1403808-0x1403810: onto the rejection queue; the attempt is counted below.
                        ReEnqueue(*rejection, task);
                        run_test = false;
                    }
                }

                if (run_test) {
                    // 0x140384c-0x1403880: the thread's slot bit must be in the mask, and the group and the id must
                    // match when the caller gave them.
                    const std::uint32_t slot_bit = 1u << (LoadU32(self, kThreadSlotOffset) & 31);
                    if ((slot_bit & LoadU32(task.raw, kTaskSlotMaskOffset)) != 0 &&
                        (group == nullptr || LoadGroup(task.raw, kTaskGroupOffset) == group) &&
                        (id == kAnyId || LoadU64(task.raw, kTaskIdOffset) == id)) {
                        // 0x14038e0-0x14038ec: blr x8 with x0 = this, x1 = &task.
                        const auto function = reinterpret_cast<void (*)(CTaskKernel*, STask*)>(
                            static_cast<std::uintptr_t>(LoadU64(task.raw, kTaskFunctionOffset)));
                        function(this, &task);
                        // 0x14038f0-0x1403910: the group field is read again after the call.
                        TTaskGroup* const owner = LoadGroup(task.raw, kTaskGroupOffset);
                        if (owner != nullptr) {
                            AtomicDecrement(owner);
                        }
                        return true;
                    }
                    // 0x1403884-0x1403890: not for this call, back onto the queue it came from.
                    ReEnqueue(*reinterpret_cast<CLockFreeQueue<STask>*>(queue), task);
                }

                // 0x1403894-0x14038ac (and 0x1403814-0x14038a0 after the rejection queue).
                ++attempts;
                if (attempts > attempt_limit) {
                    break;
                }
                header = LoadPtr(queue, kQueueHeaderOffset);
                if (static_cast<std::int64_t>(LoadShared(header, kCountOffset)) < 1) {
                    break;
                }
            }
        }

        // 0x14038b0-0x14038b8: with a group, stop as soon as its counter is 0; otherwise go to the next queue.
        if (group != nullptr && LoadAcquire(group) == 0) {
            return false;  // falls into 0x14038bc
        }
        ++queue_index;  // 0x14034a0
    } while (queue_index != queue_count);  // cmp x25, x24; b.eq 0x14038bc

    return false;
}

}  // namespace NRadEngine
