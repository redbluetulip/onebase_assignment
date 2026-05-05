// src/storage/page/b_plus_tree_leaf_page.cpp

#include "onebase/storage/page/b_plus_tree_leaf_page.h"

#include <functional>
#include <stdexcept>

namespace onebase {

template class BPlusTreeLeafPage<int, RID, std::less<int>>;

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_LEAF_PAGE_TYPE::Init(int max_size) {
  SetPageType(IndexPageType::LEAF_PAGE);
  SetMaxSize(max_size);
  SetSize(0);
  SetParentPageId(INVALID_PAGE_ID);
  next_page_id_ = INVALID_PAGE_ID;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_LEAF_PAGE_TYPE::KeyAt(int index) const -> KeyType {
  return array_[index].first;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_LEAF_PAGE_TYPE::ValueAt(int index) const -> ValueType {
  return array_[index].second;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_LEAF_PAGE_TYPE::KeyIndex(const KeyType &key, const KeyComparator &comparator) const -> int {
  // Find the first position i such that array_[i].key >= key
  // i.e. ! (array_[i].key < key)
  int l = 0;
  int r = GetSize();  // [l, r)
  while (l < r) {
    int mid = l + (r - l) / 2;
    if (comparator(array_[mid].first, key)) {
      l = mid + 1;
    } else {
      r = mid;
    }
  }
  return l;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_LEAF_PAGE_TYPE::Lookup(const KeyType &key, ValueType *value, const KeyComparator &comparator) const
    -> bool {
  int idx = KeyIndex(key, comparator);
  if (idx >= GetSize()) {
    return false;
  }
  const KeyType &k = array_[idx].first;
  if (!comparator(k, key) && !comparator(key, k)) {
    if (value != nullptr) {
      *value = array_[idx].second;
    }
    return true;
  }
  return false;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_LEAF_PAGE_TYPE::Insert(const KeyType &key, const ValueType &value, const KeyComparator &comparator)
    -> int {
  int sz = GetSize();
  int idx = KeyIndex(key, comparator);

  // duplicate check
  if (idx < sz) {
    const KeyType &k = array_[idx].first;
    if (!comparator(k, key) && !comparator(key, k)) {
      return sz;
    }
  }

  for (int i = sz; i > idx; i--) {
    array_[i] = array_[i - 1];
  }
  array_[idx].first = key;
  array_[idx].second = value;

  SetSize(sz + 1);
  return GetSize();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_LEAF_PAGE_TYPE::RemoveAndDeleteRecord(const KeyType &key, const KeyComparator &comparator) -> int {
  int sz = GetSize();
  int idx = KeyIndex(key, comparator);
  if (idx >= sz) {
    return sz;
  }

  const KeyType &k = array_[idx].first;
  if (comparator(k, key) || comparator(key, k)) {
    return sz;  // not equal
  }

  for (int i = idx; i + 1 < sz; i++) {
    array_[i] = array_[i + 1];
  }
  SetSize(sz - 1);
  return GetSize();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveHalfTo(BPlusTreeLeafPage *recipient) {
  // NOTE:
  // - This function ONLY moves the records.
  // - Do NOT modify next_page_id_ here, because this project does not expose recipient's page_id.
  //   The caller (B+Tree) should update:
  //     recipient->SetNextPageId(this->GetNextPageId());
  //     this->SetNextPageId(recipient_page_id);
  int sz = GetSize();
  int split_idx = sz / 2;

  int move_cnt = sz - split_idx;
  for (int i = 0; i < move_cnt; i++) {
    recipient->array_[i] = array_[split_idx + i];
  }

  recipient->SetSize(move_cnt);
  SetSize(split_idx);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveAllTo(BPlusTreeLeafPage *recipient) {
  // Merge: append all entries from this to recipient
  int rsz = recipient->GetSize();
  int sz = GetSize();
  for (int i = 0; i < sz; i++) {
    recipient->array_[rsz + i] = array_[i];
  }
  recipient->SetSize(rsz + sz);

  // link list: recipient skips over this
  recipient->SetNextPageId(GetNextPageId());

  SetSize(0);
  next_page_id_ = INVALID_PAGE_ID;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveFirstToEndOf(BPlusTreeLeafPage *recipient) {
  int sz = GetSize();
  if (sz <= 0) {
    throw std::runtime_error("BPlusTreeLeafPage::MoveFirstToEndOf: empty donor");
  }

  int rsz = recipient->GetSize();
  recipient->array_[rsz] = array_[0];
  recipient->SetSize(rsz + 1);

  for (int i = 0; i + 1 < sz; i++) {
    array_[i] = array_[i + 1];
  }
  SetSize(sz - 1);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveLastToFrontOf(BPlusTreeLeafPage *recipient) {
  int sz = GetSize();
  if (sz <= 0) {
    throw std::runtime_error("BPlusTreeLeafPage::MoveLastToFrontOf: empty donor");
  }

  int rsz = recipient->GetSize();
  for (int i = rsz; i > 0; i--) {
    recipient->array_[i] = recipient->array_[i - 1];
  }
  recipient->array_[0] = array_[sz - 1];
  recipient->SetSize(rsz + 1);

  SetSize(sz - 1);
}

}  // namespace onebase