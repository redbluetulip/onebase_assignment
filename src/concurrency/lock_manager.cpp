#include "onebase/concurrency/lock_manager.h"
#include "onebase/common/exception.h"

namespace onebase {

auto LockManager::LockShared(Transaction *txn, const RID &rid) -> bool {
  std::unique_lock<std::mutex> lock(latch_);

  if (txn->GetState() == TransactionState::SHRINKING) {
    txn->SetState(TransactionState::ABORTED);
    return false;
  }
  if (txn->GetState() == TransactionState::ABORTED) {
    return false;
  }

  if (txn->IsSharedLocked(rid) || txn->IsExclusiveLocked(rid)) {
    return true;
  }

  auto &queue = lock_table_[rid];
  queue.request_queue_.emplace_back(txn->GetTransactionId(), LockMode::SHARED);
  auto req_it = std::prev(queue.request_queue_.end());

  queue.cv_.wait(lock, [&]() {
    if (txn->GetState() == TransactionState::ABORTED) {
      return true;
    }
    for (const auto &req : queue.request_queue_) {
      if (req.txn_id_ != txn->GetTransactionId() && req.granted_ &&
          req.lock_mode_ == LockMode::EXCLUSIVE) {
        return false;
      }
    }
    return true;
  });

  if (txn->GetState() == TransactionState::ABORTED) {
    queue.request_queue_.erase(req_it);
    queue.cv_.notify_all();
    return false;
  }

  req_it->granted_ = true;
  txn->GetSharedLockSet()->insert(rid);
  return true;
}

auto LockManager::LockExclusive(Transaction *txn, const RID &rid) -> bool {
  std::unique_lock<std::mutex> lock(latch_);

  if (txn->GetState() == TransactionState::SHRINKING) {
    txn->SetState(TransactionState::ABORTED);
    return false;
  }
  if (txn->GetState() == TransactionState::ABORTED) {
    return false;
  }

  if (txn->IsExclusiveLocked(rid)) {
    return true;
  }

  auto &queue = lock_table_[rid];
  queue.request_queue_.emplace_back(txn->GetTransactionId(), LockMode::EXCLUSIVE);
  auto req_it = std::prev(queue.request_queue_.end());

  queue.cv_.wait(lock, [&]() {
    if (txn->GetState() == TransactionState::ABORTED) {
      return true;
    }
    for (const auto &req : queue.request_queue_) {
      if (req.txn_id_ != txn->GetTransactionId() && req.granted_) {
        return false;
      }
    }
    return true;
  });

  if (txn->GetState() == TransactionState::ABORTED) {
    queue.request_queue_.erase(req_it);
    queue.cv_.notify_all();
    return false;
  }

  req_it->granted_ = true;
  txn->GetExclusiveLockSet()->insert(rid);
  return true;
}

auto LockManager::LockUpgrade(Transaction *txn, const RID &rid) -> bool {
  std::unique_lock<std::mutex> lock(latch_);

  if (txn->GetState() == TransactionState::SHRINKING) {
    txn->SetState(TransactionState::ABORTED);
    return false;
  }
  if (txn->GetState() == TransactionState::ABORTED) {
    return false;
  }
  if (txn->IsExclusiveLocked(rid)) {
    return true;
  }
  if (!txn->IsSharedLocked(rid)) {
    return false;
  }

  auto &queue = lock_table_[rid];
  if (queue.upgrading_) {
    txn->SetState(TransactionState::ABORTED);
    return false;
  }
  queue.upgrading_ = true;

  auto req_it = queue.request_queue_.end();
  for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
    if (it->txn_id_ == txn->GetTransactionId()) {
      req_it = it;
      break;
    }
  }

  if (req_it == queue.request_queue_.end()) {
    queue.upgrading_ = false;
    return false;
  }

  req_it->lock_mode_ = LockMode::EXCLUSIVE;
  req_it->granted_ = false;

  queue.cv_.wait(lock, [&]() {
    if (txn->GetState() == TransactionState::ABORTED) {
      return true;
    }
    for (const auto &req : queue.request_queue_) {
      if (req.txn_id_ != txn->GetTransactionId() && req.granted_) {
        return false;
      }
    }
    return true;
  });

  queue.upgrading_ = false;

  if (txn->GetState() == TransactionState::ABORTED) {
    queue.request_queue_.erase(req_it);
    txn->GetSharedLockSet()->erase(rid);
    queue.cv_.notify_all();
    return false;
  }

  req_it->granted_ = true;
  txn->GetSharedLockSet()->erase(rid);
  txn->GetExclusiveLockSet()->insert(rid);
  return true;
}

auto LockManager::Unlock(Transaction *txn, const RID &rid) -> bool {
  std::unique_lock<std::mutex> lock(latch_);

  auto table_it = lock_table_.find(rid);
  if (table_it == lock_table_.end()) {
    return false;
  }

  auto &queue = table_it->second;
  bool found = false;
  for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
    if (it->txn_id_ == txn->GetTransactionId()) {
      if (queue.upgrading_ && it->lock_mode_ == LockMode::EXCLUSIVE && !it->granted_) {
        queue.upgrading_ = false;
      }
      queue.request_queue_.erase(it);
      found = true;
      break;
    }
  }

  if (!found) {
    return false;
  }

  if (txn->GetState() == TransactionState::GROWING) {
    txn->SetState(TransactionState::SHRINKING);
  }

  txn->GetSharedLockSet()->erase(rid);
  txn->GetExclusiveLockSet()->erase(rid);

  queue.cv_.notify_all();
  return true;
}

}  // namespace onebase