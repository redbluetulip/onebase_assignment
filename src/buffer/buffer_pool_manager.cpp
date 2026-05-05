#include "onebase/buffer/buffer_pool_manager.h"
#include "onebase/common/exception.h"
#include "onebase/common/logger.h"

namespace onebase {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager, size_t replacer_k)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = std::make_unique<LRUKReplacer>(pool_size, replacer_k);
  // 初始化时，所有 frame 都在空闲列表中
  for (size_t i = 0; i < pool_size_; ++i) {
    free_list_.emplace_back(static_cast<frame_id_t>(i));
  }
}

BufferPoolManager::~BufferPoolManager() { delete[] pages_; }

/**
 * 核心逻辑：获取一个可用的 Frame (空闲列表优先，否则驱逐)
 */
auto BufferPoolManager::NewPage(page_id_t *page_id) -> Page * {
  std::scoped_lock<std::mutex> lock(latch_);
  frame_id_t frame_id = -1;

  // 1. 获取可用 frame：先尝试 free_list，再尝试驱逐
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
  } else if (!replacer_->Evict(&frame_id)) {
    return nullptr;
  }

  Page &page = pages_[frame_id];

  // 2. 如果该 frame 之前存放了脏页，先写回磁盘
  if (page.GetPageId() != INVALID_PAGE_ID) {
    if (page.is_dirty_) {
      disk_manager_->WritePage(page.GetPageId(), page.GetData());
    }
    page_table_.erase(page.GetPageId());
  }

  // 3. 通过磁盘管理器分配新 page_id 并初始化
  *page_id = disk_manager_->AllocatePage();
  page.ResetMemory();
  page.page_id_ = *page_id;
  page.pin_count_ = 1;
  page.is_dirty_ = false;

  // 4. 更新映射表并通知替换器
  page_table_[*page_id] = frame_id;
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);

  return &page;
}

auto BufferPoolManager::FetchPage(page_id_t page_id) -> Page * {
  std::scoped_lock<std::mutex> lock(latch_);

  // 1. 如果页面已在内存中，增加 pin_count 并更新替换器状态
  if (page_table_.find(page_id) != page_table_.end()) {
    frame_id_t frame_id = page_table_[page_id];
    Page &page = pages_[frame_id];
    page.pin_count_++;
    replacer_->RecordAccess(frame_id);
    replacer_->SetEvictable(frame_id, false);
    return &page;
  }

  // 2. 页面不在内存，获取新 frame (逻辑同 NewPage)
  frame_id_t frame_id = -1;
  if (!free_list_.empty()) {
    frame_id = free_list_.front();
    free_list_.pop_front();
  } else if (!replacer_->Evict(&frame_id)) {
    return nullptr;
  }

  Page &page = pages_[frame_id];

  // 3. 处理驱逐旧页面的逻辑
  if (page.GetPageId() != INVALID_PAGE_ID) {
    if (page.is_dirty_) {
      disk_manager_->WritePage(page.GetPageId(), page.GetData());
    }
    page_table_.erase(page.GetPageId());
  }

  // 4. 从磁盘读取页面数据到内存
  disk_manager_->ReadPage(page_id, page.GetData());
  page.page_id_ = page_id;
  page.pin_count_ = 1;
  page.is_dirty_ = false;

  page_table_[page_id] = frame_id;
  replacer_->RecordAccess(frame_id);
  replacer_->SetEvictable(frame_id, false);

  return &page;
}

auto BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);

  if (page_table_.find(page_id) == page_table_.end()) {
    return false;
  }

  frame_id_t frame_id = page_table_[page_id];
  Page &page = pages_[frame_id];

  if (page.pin_count_ <= 0) {
    return false;
  }

  // 如果传入 dirty 为 true，则标记页面为脏
  if (is_dirty) {
    page.is_dirty_ = true;
  }

  page.pin_count_--;

  // 当没有人使用该页时，允许替换器驱逐它
  if (page.pin_count_ == 0) {
    replacer_->SetEvictable(frame_id, true);
  }

  return true;
}

auto BufferPoolManager::FlushPage(page_id_t page_id) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);
  if (page_id == INVALID_PAGE_ID || page_table_.find(page_id) == page_table_.end()) {
    return false;
  }

  frame_id_t frame_id = page_table_[page_id];
  disk_manager_->WritePage(page_id, pages_[frame_id].GetData());
  pages_[frame_id].is_dirty_ = false;
  return true;
}

void BufferPoolManager::FlushAllPages() {
  std::scoped_lock<std::mutex> lock(latch_);
  for (size_t i = 0; i < pool_size_; ++i) {
    Page &page = pages_[i];
    if (page.GetPageId() != INVALID_PAGE_ID) {
      disk_manager_->WritePage(page.GetPageId(), page.GetData());
      page.is_dirty_ = false;
    }
  }
}

auto BufferPoolManager::DeletePage(page_id_t page_id) -> bool {
  std::scoped_lock<std::mutex> lock(latch_);
  if (page_table_.find(page_id) == page_table_.end()) {
    return true;
  }

  frame_id_t frame_id = page_table_[page_id];
  Page &page = pages_[frame_id];

  // 只有 pin_count 为 0 的页面才能删除
  if (page.pin_count_ > 0) {
    return false;
  }

  // 清理元数据并放回空闲列表
  page_table_.erase(page_id);
  replacer_->Remove(frame_id);
  free_list_.push_back(frame_id);

  page.ResetMemory();
  page.page_id_ = INVALID_PAGE_ID;
  page.is_dirty_ = false;
  page.pin_count_ = 0;

  disk_manager_->DeallocatePage(page_id);
  return true;
}

}  // namespace onebase