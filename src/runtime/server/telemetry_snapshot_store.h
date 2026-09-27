/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <system_error>
#include <utility>

// Triple-slot producer/consumer handoff. Snapshot capture runs while a write
// lease owns its reserved slot, outside the metadata mutex. Readers retain a
// lease until serialization and their previous-snapshot copy are complete.
template <typename Snapshot>
class TelemetrySnapshotStore {
 public:
  class WriteLease {
   public:
    WriteLease() = default;
    WriteLease(const WriteLease&) = delete;
    WriteLease& operator=(const WriteLease&) = delete;

    WriteLease(WriteLease&& other) noexcept
        : store_(std::exchange(other.store_, nullptr)), index_(other.index_) {}

    WriteLease& operator=(WriteLease&& other) noexcept {
      if (this != &other) {
        Abandon();
        store_ = std::exchange(other.store_, nullptr);
        index_ = other.index_;
      }
      return *this;
    }

    ~WriteLease() { Abandon(); }

    Snapshot& snapshot() { return store_->slots_[index_]; }
    explicit operator bool() const { return store_ != nullptr; }

    bool Publish() {
      if (store_ == nullptr) return false;
      TelemetrySnapshotStore* store = std::exchange(store_, nullptr);
      return store->PublishWrite(index_);
    }

   private:
    friend class TelemetrySnapshotStore;
    WriteLease(TelemetrySnapshotStore* store, size_t index) : store_(store), index_(index) {}

    void Abandon() {
      if (store_ == nullptr) return;
      TelemetrySnapshotStore* store = std::exchange(store_, nullptr);
      store->AbandonWrite(index_);
    }

    TelemetrySnapshotStore* store_ = nullptr;
    size_t index_ = 0;
  };

  class ReadLease {
   public:
    ReadLease() = default;
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;

    ReadLease(ReadLease&& other) noexcept
        : store_(std::exchange(other.store_, nullptr)), index_(other.index_), sequence_(other.sequence_) {}

    ReadLease& operator=(ReadLease&& other) noexcept {
      if (this != &other) {
        Release();
        store_ = std::exchange(other.store_, nullptr);
        index_ = other.index_;
        sequence_ = other.sequence_;
      }
      return *this;
    }

    ~ReadLease() { Release(); }

    const Snapshot& snapshot() const { return store_->slots_[index_]; }
    uint64_t sequence() const { return sequence_; }
    explicit operator bool() const { return store_ != nullptr; }

    void Release() {
      if (store_ == nullptr) return;
      TelemetrySnapshotStore* store = std::exchange(store_, nullptr);
      store->ReleaseRead(index_, sequence_);
    }

   private:
    friend class TelemetrySnapshotStore;
    ReadLease(TelemetrySnapshotStore* store, size_t index, uint64_t sequence)
        : store_(store), index_(index), sequence_(sequence) {}

    TelemetrySnapshotStore* store_ = nullptr;
    size_t index_ = 0;
    uint64_t sequence_ = 0;
  };

  void Activate() { acceptingCaptures_.store(true, std::memory_order_release); }

  // Disable new captures, then wait for a capture already holding the producer
  // mutex to publish or abandon its slot.
  void DeactivateAndQuiesce() {
    acceptingCaptures_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> producerLock(producerMutex_);
  }

  template <typename Capture>
  bool CaptureIfActive(Capture&& capture) {
    std::lock_guard<std::mutex> producerLock(producerMutex_);
    if (!acceptingCaptures_.load(std::memory_order_acquire)) return false;

    WriteLease writer = TryBeginWrite();
    if (!writer) return false;
    if (!capture(writer.snapshot())) return false;
    return writer.Publish();
  }

  ReadLease TryAcquireRead(uint64_t afterSequence, bool allowRepeat = false) {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    if (readerIndex_ != kNoSlot || publishedIndex_ == kNoSlot) return {};
    if (!allowRepeat && publishedSequence_ <= afterSequence) return {};
    readerIndex_ = publishedIndex_;
    return ReadLease(this, readerIndex_, publishedSequence_);
  }

  // Reset only after the producer is quiesced and the consumer has joined.
  bool Reset() {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    if (acceptingCaptures_.load(std::memory_order_acquire) || writerIndex_ != kNoSlot || readerIndex_ != kNoSlot) {
      return false;
    }
    for (Snapshot& slot : slots_) slot = Snapshot{};
    publishedIndex_ = kNoSlot;
    publishedSequence_ = 0;
    consumedSequence_ = 0;
    skippedGenerations_ = 0;
    return true;
  }

  uint64_t skippedGenerations() const {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    return skippedGenerations_;
  }

  bool HasPublished() const {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    return publishedIndex_ != kNoSlot;
  }

 private:
  static constexpr size_t kSlotCount = 3;
  static constexpr size_t kNoSlot = std::numeric_limits<size_t>::max();

  WriteLease TryBeginWrite() {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    if (writerIndex_ != kNoSlot) return {};
    for (size_t index = 0; index < kSlotCount; ++index) {
      if (index == publishedIndex_ || index == readerIndex_) continue;
      writerIndex_ = index;
      return WriteLease(this, index);
    }
    return {};
  }

  bool PublishWrite(size_t index) {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    if (writerIndex_ != index) return false;
    if (publishedIndex_ != kNoSlot && publishedIndex_ != readerIndex_ &&
        publishedSequence_ > consumedSequence_) {
      ++skippedGenerations_;
    }
    publishedIndex_ = index;
    ++publishedSequence_;
    writerIndex_ = kNoSlot;
    return true;
  }

  void AbandonWrite(size_t index) {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    if (writerIndex_ == index) writerIndex_ = kNoSlot;
  }

  void ReleaseRead(size_t index, uint64_t sequence) {
    std::lock_guard<std::mutex> lock(metadataMutex_);
    if (readerIndex_ == index) {
      readerIndex_ = kNoSlot;
      if (sequence > consumedSequence_) consumedSequence_ = sequence;
    }
  }

  mutable std::mutex metadataMutex_;
  std::mutex producerMutex_;
  std::array<Snapshot, kSlotCount> slots_{};
  size_t writerIndex_ = kNoSlot;
  size_t publishedIndex_ = kNoSlot;
  size_t readerIndex_ = kNoSlot;
  uint64_t publishedSequence_ = 0;
  uint64_t consumedSequence_ = 0;
  uint64_t skippedGenerations_ = 0;
  std::atomic<bool> acceptingCaptures_{false};
};

// Reconnects invalidate the telemetry header. A failed send leaves the
// current epoch required; a reconnect racing an old send remains required.
class TelemetryHeaderEpoch {
 public:
  uint64_t Require() { return epoch_.fetch_add(1, std::memory_order_acq_rel) + 1; }

  uint64_t Current() const { return epoch_.load(std::memory_order_acquire); }

  bool Required() const {
    return Current() != sentEpoch_.load(std::memory_order_acquire);
  }

  bool MarkSent(uint64_t epoch) {
    if (Current() != epoch) return false;
    sentEpoch_.store(epoch, std::memory_order_release);
    return true;
  }

 private:
  std::atomic<uint64_t> epoch_{0};
  std::atomic<uint64_t> sentEpoch_{std::numeric_limits<uint64_t>::max()};
};

template <typename CreateWorker, typename Rollback>
bool StartTelemetryWorker(CreateWorker&& createWorker, Rollback&& rollback) {
  try {
    if (createWorker()) return true;
  } catch (const std::exception&) {
  }
  rollback();
  return false;
}
