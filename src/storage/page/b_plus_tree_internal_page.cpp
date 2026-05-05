// src/storage/page/b_plus_tree_internal_page.cpp

#include "onebase/storage/page/b_plus_tree_internal_page.h"

#include <functional>
#include <stdexcept>

namespace onebase {

template class BPlusTreeInternalPage<int, page_id_t, std::less<int>>;

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Init(int max_size) {
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetMaxSize(max_size);
  SetSize(0);
  SetParentPageId(INVALID_PAGE_ID);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::KeyAt(int index) const -> KeyType {
  return array_[index].first;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::SetKeyAt(int index, const KeyType &key) {
  array_[index].first = key;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueAt(int index) const -> ValueType {
  return array_[index].second;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::SetValueAt(int index, const ValueType &value) {
  array_[index].second = value;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueIndex(const ValueType &value) const -> int {
  for (int i = 0; i < GetSize(); i++) {
    if (array_[i].second == value) {
      return i;
    }
  }
  return -1;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::Lookup(const KeyType &key, const KeyComparator &comparator) const -> ValueType {
  // Layout:
  // keys:   [_, k1, k2, ..., k_{n-1}]   (key[0] invalid)
  // values: [p0, p1, p2, ..., p_{n-1}] (value[i] is child pointer)
  //
  // Route rule:
  // if key < k1 -> p0
  // if k_i <= key < k_{i+1} -> p_i
  // if key >= k_{n-1} -> p_{n-1}
  ValueType result = array_[0].second;  // default p0
  for (int i = 1; i < GetSize(); i++) {
    if (comparator(key, array_[i].first)) {  // key < k_i
      break;
    }
    result = array_[i].second;
  }
  return result;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::PopulateNewRoot(const ValueType &old_value, const KeyType &key,
                                                    const ValueType &new_value) {
  // New root has exactly 2 child pointers and 1 routing key.
  // array_[0] = {invalid_key, old_value}
  // array_[1] = {key,        new_value}
  SetSize(2);
  SetValueAt(0, old_value);
  SetKeyAt(1, key);
  SetValueAt(1, new_value);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::InsertNodeAfter(const ValueType &old_value, const KeyType &key,
                                                    const ValueType &new_value) -> int {
  int idx = ValueIndex(old_value);
  if (idx < 0) {
    throw std::runtime_error("BPlusTreeInternalPage::InsertNodeAfter: old_value not found");
  }

  int old_size = GetSize();
  // shift [idx+1 .. old_size-1] -> right by 1
  for (int i = old_size; i >= idx + 2; i--) {
    array_[i] = array_[i - 1];
  }

  array_[idx + 1].first = key;
  array_[idx + 1].second = new_value;
  SetSize(old_size + 1);
  return GetSize();
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Remove(int index) {
  int sz = GetSize();
  if (index < 0 || index >= sz) {
    throw std::runtime_error("BPlusTreeInternalPage::Remove: index out of range");
  }
  for (int i = index; i + 1 < sz; i++) {
    array_[i] = array_[i + 1];
  }
  SetSize(sz - 1);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::RemoveAndReturnOnlyChild() -> ValueType {
  // Used when root has only one child pointer left.
  if (GetSize() != 1) {
    throw std::runtime_error("BPlusTreeInternalPage::RemoveAndReturnOnlyChild: size != 1");
  }
  ValueType only = ValueAt(0);
  SetSize(0);
  return only;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveAllTo(BPlusTreeInternalPage *recipient, const KeyType &middle_key) {
  // Merge "this" into recipient.
  // Bring down middle_key from parent as recipient's new last separator key,
  // paired with this->ValueAt(0) (the first child pointer of this node).
  int rsz = recipient->GetSize();
  int sz = GetSize();
  if (sz == 0) {
    return;
  }

  // Append the bridged entry: {middle_key, this.p0}
  recipient->array_[rsz].first = middle_key;
  recipient->array_[rsz].second = ValueAt(0);
  rsz++;

  // Append the remaining entries from this: (key/value) for i=1..sz-1
  for (int i = 1; i < sz; i++) {
    recipient->array_[rsz] = array_[i];
    rsz++;
  }

  recipient->SetSize(rsz);
  SetSize(0);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveHalfTo(BPlusTreeInternalPage *recipient, const KeyType &middle_key) {
  // Split internal page:
  // - Promote middle_key to parent (NOT stored as a valid key in recipient)
  // - recipient->ValueAt(0) (p0) should be old ValueAt(split_idx)
  // - recipient receives old array_[split_idx+1 .. n-1] into its array_[1 ..]
  // - left keeps array_[0 .. split_idx-1]

  int n = GetSize();
  if (n <= 1) {
    recipient->SetSize(0);
    return;
  }

  int split_idx = n / 2;

  // optional: store middle_key in key[0] as placeholder (key[0] is invalid anyway)
  recipient->array_[0].first = middle_key;

  // recipient p0 = old value[split_idx]
  recipient->array_[0].second = ValueAt(split_idx);

  // copy the rest: old (split_idx+1 .. n-1) -> recipient (1 ..)
  int ridx = 1;
  for (int i = split_idx + 1; i < n; i++) {
    recipient->array_[ridx] = array_[i];
    ridx++;
  }

  recipient->SetSize(ridx);

  // left keeps [0 .. split_idx-1]
  SetSize(split_idx);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveFirstToEndOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key) {
  // Redistribute from "this" (right sibling) to recipient (left sibling).
  int sz = GetSize();
  if (sz <= 0) {
    throw std::runtime_error("BPlusTreeInternalPage::MoveFirstToEndOf: empty donor");
  }

  int rsz = recipient->GetSize();
  recipient->array_[rsz].first = middle_key;
  recipient->array_[rsz].second = ValueAt(0);
  recipient->SetSize(rsz + 1);

  if (sz == 1) {
    SetSize(0);
    return;
  }

  // new p0 becomes old p1
  array_[0].second = ValueAt(1);

  // shift [2..sz-1] -> [1..sz-2]
  for (int i = 1; i + 1 < sz; i++) {
    array_[i] = array_[i + 1];
  }
  SetSize(sz - 1);
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveLastToFrontOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key) {
  // Redistribute from "this" (left sibling) to recipient (right sibling).
  int sz = GetSize();
  if (sz <= 0) {
    throw std::runtime_error("BPlusTreeInternalPage::MoveLastToFrontOf: empty donor");
  }

  ValueType moved_p0 = ValueAt(sz - 1);  // last child pointer
  SetSize(sz - 1);

  int rsz = recipient->GetSize();
  if (rsz == 0) {
    recipient->array_[0].second = moved_p0;
    recipient->SetSize(1);
    return;
  }

  ValueType old_p0 = recipient->ValueAt(0);

  // shift recipient right by 1
  for (int i = rsz; i >= 0; i--) {
    recipient->array_[i + 1] = recipient->array_[i];
  }

  recipient->array_[0].second = moved_p0;

  recipient->array_[1].first = middle_key;
  recipient->array_[1].second = old_p0;

  recipient->SetSize(rsz + 1);
}

}  // namespace onebase