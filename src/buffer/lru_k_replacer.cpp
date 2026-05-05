#include "onebase/buffer/lru_k_replacer.h"
#include "onebase/common/exception.h"

namespace onebase {

LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k)
    : max_frames_(num_frames), k_(k) {}

auto LRUKReplacer::Evict(frame_id_t *frame_id) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);

  frame_id_t victim = -1;
  size_t min_timestamp = std::numeric_limits<size_t>::max(); // 用于处理距离为 +inf 的情况
  size_t max_k_distance = 0; // 用于处理距离有限的情况
  bool found_inf = false;

  for (auto const &[fid, entry] : entries_) {
    if (!entry.is_evictable_) {
      continue;
    }

    // 类别 1: 访问次数少于 K 次 (k-distance = +infinity)
    if (entry.history_.size() < k_) {
      found_inf = true;
      if (entry.history_.front() < min_timestamp) {
        min_timestamp = entry.history_.front();
        victim = fid;
      }
    } 
    // 类别 2: 访问次数 >= K 次 (有限 k-distance)
    else if (!found_inf) {
      size_t k_distance = current_timestamp_ - entry.history_.front();
      if (k_distance > max_k_distance) {
        max_k_distance = k_distance;
        victim = fid;
      }
    }
  }

  if (victim == -1) {
    return false;
  }

  // 执行驱逐：从 entries 中移除并减少计数
  entries_.erase(victim);
  curr_size_--;
  *frame_id = victim;
  return true;
}

void LRUKReplacer::RecordAccess(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_);

  if (static_cast<size_t>(frame_id) >= max_frames_) {
    throw std::invalid_argument("Invalid frame_id");
  }

  auto &entry = entries_[frame_id];
  entry.history_.push_back(current_timestamp_);

  // 如果历史记录超过 K，移除最旧的
  if (entry.history_.size() > k_) {
    entry.history_.pop_front();
  }

  current_timestamp_++;
}

void LRUKReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::scoped_lock<std::mutex> lock(latch_);

  if (entries_.find(frame_id) == entries_.end()) {
    return;
  }

  auto &entry = entries_[frame_id];
  // 仅在状态发生变化时更新 curr_size_
  if (set_evictable && !entry.is_evictable_) {
    curr_size_++;
  } else if (!set_evictable && entry.is_evictable_) {
    curr_size_--;
  }
  entry.is_evictable_ = set_evictable;
}

void LRUKReplacer::Remove(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_);

  auto it = entries_.find(frame_id);
  if (it == entries_.end()) {
    return;
  }

  if (!it->second.is_evictable_) {
    throw std::runtime_error("Cannot remove a non-evictable frame");
  }

  entries_.erase(it);
  curr_size_--;
}

auto LRUKReplacer::Size() const -> size_t {
  return curr_size_;
}

}  // namespace onebase
