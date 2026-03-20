#pragma once

#include <atomic>
#include <cstdint>

// Single-producer single-consumer lock-free ring buffer.
// Producer (game thread) calls Push(), consumer (telemetry thread) calls Pop().
template <typename T, uint32_t Capacity>
class EventRingBuffer {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

 public:
  EventRingBuffer() : m_head(0), m_tail(0) {}

  // Push an item. Returns false if buffer is full.
  bool Push(const T& item) {
    uint32_t head = m_head.load(std::memory_order_relaxed);
    uint32_t next = (head + 1) & (Capacity - 1);
    if (next == m_tail.load(std::memory_order_acquire)) {
      return false;  // full
    }
    m_buffer[head] = item;
    m_head.store(next, std::memory_order_release);
    return true;
  }

  // Pop an item. Returns false if buffer is empty.
  bool Pop(T& item) {
    uint32_t tail = m_tail.load(std::memory_order_relaxed);
    if (tail == m_head.load(std::memory_order_acquire)) {
      return false;  // empty
    }
    item = m_buffer[tail];
    m_tail.store((tail + 1) & (Capacity - 1), std::memory_order_release);
    return true;
  }

  bool IsEmpty() const {
    return m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire);
  }

 private:
  T m_buffer[Capacity];
  std::atomic<uint32_t> m_head;
  std::atomic<uint32_t> m_tail;
};
