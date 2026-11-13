#include "core/map_fetch_queue.h"

#include <algorithm>
#include <chrono>

#include "core/map_manifest.h"

namespace nevr_maps {

bool GroupFetchQueue::Request(const std::string& group, Done done, std::string* reason) {
  if (!IsPathSafeGroupId(group)) {
    *reason = "bad_group";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      *reason = "stopped";
      return false;
    }
    const auto waiting =
        std::find_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.group == group; });
    if (waiting != pending_.end()) {
      waiting->done.push_back(std::move(done));
    } else {
      pending_.push_back(Pending{group, {std::move(done)}});
    }
  }
  wake_.notify_all();
  reason->clear();
  return true;
}

bool GroupFetchQueue::RunNext(const Fetch& fetch) {
  Pending next;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.empty()) return false;
    next = std::move(pending_.front());
    pending_.pop_front();
  }
  const FetchReport report = fetch(next.group);
  for (const Done& done : next.done) {
    if (done) done(report);
  }
  return true;
}

bool GroupFetchQueue::WaitForRequest(std::uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  return wake_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return stopped_ || !pending_.empty(); }) &&
         !stopped_;
}

void GroupFetchQueue::Stop() {
  std::deque<Pending> answered;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    answered.swap(pending_);
  }
  wake_.notify_all();
  for (const Pending& p : answered) {
    FetchReport report;
    report.group = p.group;
    report.outcome = FetchOutcome::kFailed;
    report.reason = "cancelled";
    for (const Done& done : p.done) {
      if (done) done(report);
    }
  }
}

std::size_t GroupFetchQueue::Waiting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_.size();
}

}  // namespace nevr_maps
