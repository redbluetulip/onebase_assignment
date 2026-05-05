// src/storage/index/b_plus_tree_iterator.cpp

#include "onebase/storage/index/b_plus_tree_iterator.h"

#include <functional>
#include <stdexcept>

// 关键：补齐完整类型定义（否则会出现 Page 未声明 / BufferPoolManager 不完整类型）
#include "onebase/buffer/buffer_pool_manager.h"
#include "onebase/storage/page/b_plus_tree_leaf_page.h"
// Page 的头文件路径在不同工程里可能不同：如果这里编不过，按你工程实际 Page 头文件改一下
#include "onebase/storage/page/page.h"

namespace onebase {

template class BPlusTreeIterator<int, RID, std::less<int>>;

template <typename KeyType, typename ValueType, typename KeyComparator>
BPLUSTREE_ITERATOR_TYPE::BPlusTreeIterator(page_id_t page_id, int index, BufferPoolManager *bpm)
    : page_id_(page_id), index_(index), bpm_(bpm) {}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_ITERATOR_TYPE::IsEnd() const -> bool {
  return page_id_ == INVALID_PAGE_ID;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_ITERATOR_TYPE::operator*() -> const std::pair<KeyType, ValueType> & {
  if (IsEnd()) {
    throw std::out_of_range("BPlusTreeIterator::operator*: dereference end iterator");
  }
  if (bpm_ == nullptr) {
    throw std::runtime_error("BPlusTreeIterator::operator*: bpm_ is null");
  }

  Page *page = bpm_->FetchPage(page_id_);
  if (page == nullptr) {
    throw std::runtime_error("BPlusTreeIterator::operator*: FetchPage failed");
  }

  auto *leaf = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(page->GetData());
  if (index_ < 0 || index_ >= leaf->GetSize()) {
    bpm_->UnpinPage(page_id_, false);
    throw std::out_of_range("BPlusTreeIterator::operator*: index out of range");
  }

  // 缓存副本：安全返回引用
  current_ = {leaf->KeyAt(index_), leaf->ValueAt(index_)};

  bpm_->UnpinPage(page_id_, false);
  return current_;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_ITERATOR_TYPE::operator++() -> BPlusTreeIterator & {
  if (IsEnd()) {
    return *this;
  }
  if (bpm_ == nullptr) {
    throw std::runtime_error("BPlusTreeIterator::operator++: bpm_ is null");
  }

  Page *page = bpm_->FetchPage(page_id_);
  if (page == nullptr) {
    throw std::runtime_error("BPlusTreeIterator::operator++: FetchPage failed");
  }

  auto *leaf = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(page->GetData());

  index_++;
  if (index_ >= leaf->GetSize()) {
    // 走到当前 leaf 末尾，跳到 next leaf
    page_id_t next = leaf->GetNextPageId();
    bpm_->UnpinPage(page_id_, false);

    if (next == INVALID_PAGE_ID) {
      page_id_ = INVALID_PAGE_ID;
      index_ = 0;
      return *this;
    }

    page_id_ = next;
    index_ = 0;
    return *this;
  }

  bpm_->UnpinPage(page_id_, false);
  return *this;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_ITERATOR_TYPE::operator==(const BPlusTreeIterator &other) const -> bool {
  return page_id_ == other.page_id_ && index_ == other.index_;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_ITERATOR_TYPE::operator!=(const BPlusTreeIterator &other) const -> bool {
  return !(*this == other);
}

}  // namespace onebase