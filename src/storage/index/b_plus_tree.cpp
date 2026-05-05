// src/storage/index/b_plus_tree.cpp

#include "onebase/storage/index/b_plus_tree.h"
#include "onebase/storage/index/b_plus_tree_iterator.h"

#include <functional>
#include <stdexcept>
#include <vector>

// 关键：让 BufferPoolManager / Page / B+Tree pages 在 cpp 里是“完整类型”
#include "onebase/buffer/buffer_pool_manager.h"
#include "onebase/storage/page/page.h"
#include "onebase/storage/page/b_plus_tree_leaf_page.h"
#include "onebase/storage/page/b_plus_tree_internal_page.h"

namespace onebase {

template class BPlusTree<int, RID, std::less<int>>;

template <typename KeyType, typename ValueType, typename KeyComparator>
BPLUSTREE_TYPE::BPlusTree(std::string name, BufferPoolManager *bpm, const KeyComparator &comparator, int leaf_max_size,
                          int internal_max_size)
    : Index(std::move(name)),
      bpm_(bpm),
      comparator_(comparator),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  if (leaf_max_size_ == 0) {
    leaf_max_size_ =
        static_cast<int>((ONEBASE_PAGE_SIZE - sizeof(BPlusTreePage) - sizeof(page_id_t)) /
                         (sizeof(KeyType) + sizeof(ValueType)));
  }
  if (internal_max_size_ == 0) {
    internal_max_size_ =
        static_cast<int>((ONEBASE_PAGE_SIZE - sizeof(BPlusTreePage)) / (sizeof(KeyType) + sizeof(page_id_t)));
  }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_TYPE::IsEmpty() const -> bool {
  return root_page_id_ == INVALID_PAGE_ID;
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result) -> bool {
  if (IsEmpty()) {
    return false;
  }

  page_id_t cur = root_page_id_;
  while (true) {
    Page *p = bpm_->FetchPage(cur);
    if (p == nullptr) {
      throw std::runtime_error("GetValue: FetchPage failed");
    }

    auto *node = reinterpret_cast<BPlusTreePage *>(p->GetData());

    if (node->IsLeafPage()) {
      auto *leaf = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(node);
      ValueType v{};
      bool ok = leaf->Lookup(key, &v, comparator_);
      bpm_->UnpinPage(cur, false);

      if (ok && result != nullptr) {
        result->push_back(v);
      }
      return ok;
    }

    auto *internal = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(node);
    page_id_t nxt = internal->Lookup(key, comparator_);
    bpm_->UnpinPage(cur, false);
    cur = nxt;
  }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_TYPE::Begin() -> Iterator {
  if (IsEmpty()) {
    return End();
  }

  page_id_t cur = root_page_id_;
  while (true) {
    Page *p = bpm_->FetchPage(cur);
    if (p == nullptr) {
      throw std::runtime_error("Begin: FetchPage failed");
    }

    auto *node = reinterpret_cast<BPlusTreePage *>(p->GetData());
    if (node->IsLeafPage()) {
      bpm_->UnpinPage(cur, false);
      return Iterator(cur, 0, bpm_);
    }

    auto *internal = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(node);
    page_id_t nxt = internal->ValueAt(0);  // leftmost child
    bpm_->UnpinPage(cur, false);
    cur = nxt;
  }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_TYPE::Begin(const KeyType &key) -> Iterator {
  if (IsEmpty()) {
    return End();
  }

  page_id_t cur = root_page_id_;
  while (true) {
    Page *p = bpm_->FetchPage(cur);
    if (p == nullptr) {
      throw std::runtime_error("Begin(key): FetchPage failed");
    }

    auto *node = reinterpret_cast<BPlusTreePage *>(p->GetData());
    if (node->IsLeafPage()) {
      auto *leaf = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(node);
      int idx = leaf->KeyIndex(key, comparator_);

      if (idx >= leaf->GetSize()) {
        page_id_t nxt = leaf->GetNextPageId();
        bpm_->UnpinPage(cur, false);
        if (nxt == INVALID_PAGE_ID) {
          return End();
        }
        return Iterator(nxt, 0, bpm_);
      }

      bpm_->UnpinPage(cur, false);
      return Iterator(cur, idx, bpm_);
    }

    auto *internal = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(node);
    page_id_t nxt = internal->Lookup(key, comparator_);
    bpm_->UnpinPage(cur, false);
    cur = nxt;
  }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value) -> bool {
  // min size rules (no C++20 requires)
  auto leaf_min_size = [&](int leaf_max) -> int { return leaf_max / 2; };
  auto internal_min_size = [&](int internal_max) -> int { return (internal_max + 1) / 2; };

  auto set_children_parent = [&](BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *in, page_id_t in_pid) {
    for (int i = 0; i < in->GetSize(); i++) {
      page_id_t child = in->ValueAt(i);
      Page *cp = bpm_->FetchPage(child);
      if (cp == nullptr) {
        throw std::runtime_error("Insert: FetchPage(child) failed");
      }
      auto *cnode = reinterpret_cast<BPlusTreePage *>(cp->GetData());
      cnode->SetParentPageId(in_pid);
      bpm_->UnpinPage(child, true);
    }
  };

  // 1) empty tree -> create root leaf
  if (IsEmpty()) {
    page_id_t root_pid;
    Page *p = bpm_->NewPage(&root_pid);
    if (p == nullptr) {
      throw std::runtime_error("Insert: NewPage failed");
    }
    auto *leaf = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(p->GetData());
    leaf->Init(leaf_max_size_);
    leaf->SetParentPageId(INVALID_PAGE_ID);

    root_page_id_ = root_pid;

    int before = leaf->GetSize();
    int after = leaf->Insert(key, value, comparator_);
    (void)before;
    bpm_->UnpinPage(root_pid, after > 0);
    return true;
  }

  // 2) descend to leaf
  page_id_t cur = root_page_id_;
  while (true) {
    Page *p = bpm_->FetchPage(cur);
    if (p == nullptr) {
      throw std::runtime_error("Insert: FetchPage failed");
    }
    auto *node = reinterpret_cast<BPlusTreePage *>(p->GetData());

    if (!node->IsLeafPage()) {
      auto *internal = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(node);
      page_id_t nxt = internal->Lookup(key, comparator_);
      bpm_->UnpinPage(cur, false);
      cur = nxt;
      continue;
    }

    // leaf insert
    auto *leaf = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(node);
    int before = leaf->GetSize();
    int after = leaf->Insert(key, value, comparator_);
    bool inserted = (after != before);
    bool overflow = (leaf->GetSize() > leaf->GetMaxSize());
    bpm_->UnpinPage(cur, inserted || overflow);

    if (!inserted) {
      return false;  // duplicate
    }
    if (!overflow) {
      return true;
    }

    // 3) split leaf
    page_id_t new_leaf_pid;
    Page *np = bpm_->NewPage(&new_leaf_pid);
    if (np == nullptr) {
      throw std::runtime_error("Insert: NewPage for leaf split failed");
    }
    auto *new_leaf = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(np->GetData());
    new_leaf->Init(leaf_max_size_);

    // Fetch old leaf again as writable
    Page *op = bpm_->FetchPage(cur);
    if (op == nullptr) {
      throw std::runtime_error("Insert: FetchPage(old leaf) failed");
    }
    auto *old_leaf = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(op->GetData());

    new_leaf->SetParentPageId(old_leaf->GetParentPageId());

    // Move half entries (do NOT modify next pointers inside MoveHalfTo in your leaf_page.cpp)
    old_leaf->MoveHalfTo(new_leaf);

    // Maintain leaf linked list here
    new_leaf->SetNextPageId(old_leaf->GetNextPageId());
    old_leaf->SetNextPageId(new_leaf_pid);

    KeyType push_up_key = new_leaf->KeyAt(0);

    bpm_->UnpinPage(cur, true);
    bpm_->UnpinPage(new_leaf_pid, true);

    // 4) insert into parent, recursively split internal
    page_id_t left_pid = cur;
    page_id_t right_pid = new_leaf_pid;
    KeyType up_key = push_up_key;

    while (true) {
      // get parent pid from left node
      Page *lp = bpm_->FetchPage(left_pid);
      if (lp == nullptr) {
        throw std::runtime_error("Insert: FetchPage(left) failed");
      }
      auto *lnode = reinterpret_cast<BPlusTreePage *>(lp->GetData());
      page_id_t parent_pid = lnode->GetParentPageId();
      bpm_->UnpinPage(left_pid, false);

      if (parent_pid == INVALID_PAGE_ID) {
        // create new root
        page_id_t new_root_pid;
        Page *rp = bpm_->NewPage(&new_root_pid);
        if (rp == nullptr) {
          throw std::runtime_error("Insert: NewPage(new root) failed");
        }
        auto *root = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(rp->GetData());
        root->Init(internal_max_size_);
        root->SetParentPageId(INVALID_PAGE_ID);
        root->PopulateNewRoot(left_pid, up_key, right_pid);

        root_page_id_ = new_root_pid;

        // update children parent
        Page *c1 = bpm_->FetchPage(left_pid);
        reinterpret_cast<BPlusTreePage *>(c1->GetData())->SetParentPageId(new_root_pid);
        bpm_->UnpinPage(left_pid, true);

        Page *c2 = bpm_->FetchPage(right_pid);
        reinterpret_cast<BPlusTreePage *>(c2->GetData())->SetParentPageId(new_root_pid);
        bpm_->UnpinPage(right_pid, true);

        bpm_->UnpinPage(new_root_pid, true);
        return true;
      }

      // insert into existing parent
      Page *pp = bpm_->FetchPage(parent_pid);
      if (pp == nullptr) {
        throw std::runtime_error("Insert: FetchPage(parent) failed");
      }
      auto *parent = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(pp->GetData());

      parent->InsertNodeAfter(left_pid, up_key, right_pid);

      // update right child's parent pointer
      Page *rc = bpm_->FetchPage(right_pid);
      if (rc == nullptr) {
        throw std::runtime_error("Insert: FetchPage(right child) failed");
      }
      reinterpret_cast<BPlusTreePage *>(rc->GetData())->SetParentPageId(parent_pid);
      bpm_->UnpinPage(right_pid, true);

      bool parent_overflow = (parent->GetSize() > parent->GetMaxSize());
      bpm_->UnpinPage(parent_pid, true);

      if (!parent_overflow) {
        return true;
      }

      // split internal parent
      Page *pp2 = bpm_->FetchPage(parent_pid);
      if (pp2 == nullptr) {
        throw std::runtime_error("Insert: FetchPage(parent for split) failed");
      }
      auto *old_in = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(pp2->GetData());

      int split_idx = old_in->GetSize() / 2;
      KeyType promote = old_in->KeyAt(split_idx);

      page_id_t new_in_pid;
      Page *nip = bpm_->NewPage(&new_in_pid);
      if (nip == nullptr) {
        throw std::runtime_error("Insert: NewPage for internal split failed");
      }
      auto *new_in = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(nip->GetData());
      new_in->Init(internal_max_size_);
      new_in->SetParentPageId(old_in->GetParentPageId());

      old_in->MoveHalfTo(new_in, promote);

      // update moved children's parent pointers
      set_children_parent(new_in, new_in_pid);

      bpm_->UnpinPage(parent_pid, true);
      bpm_->UnpinPage(new_in_pid, true);

      left_pid = parent_pid;
      right_pid = new_in_pid;
      up_key = promote;
    }
  }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
void BPLUSTREE_TYPE::Remove(const KeyType &key) {
  if (IsEmpty()) {
    return;
  }

  auto leaf_min_size = [&](int leaf_max) -> int { return leaf_max / 2; };
  auto internal_min_size = [&](int internal_max) -> int { return (internal_max + 1) / 2; };

  auto set_children_parent = [&](BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *in, page_id_t in_pid) {
    for (int i = 0; i < in->GetSize(); i++) {
      page_id_t child = in->ValueAt(i);
      Page *cp = bpm_->FetchPage(child);
      if (cp == nullptr) {
        throw std::runtime_error("Remove: FetchPage(child) failed");
      }
      auto *cnode = reinterpret_cast<BPlusTreePage *>(cp->GetData());
      cnode->SetParentPageId(in_pid);
      bpm_->UnpinPage(child, true);
    }
  };

  // 1) find leaf
  page_id_t cur = root_page_id_;
  while (true) {
    Page *p = bpm_->FetchPage(cur);
    if (p == nullptr) {
      throw std::runtime_error("Remove: FetchPage failed");
    }
    auto *node = reinterpret_cast<BPlusTreePage *>(p->GetData());

    if (!node->IsLeafPage()) {
      auto *internal = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(node);
      page_id_t nxt = internal->Lookup(key, comparator_);
      bpm_->UnpinPage(cur, false);
      cur = nxt;
      continue;
    }

    auto *leaf = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(node);

    int before = leaf->GetSize();
    int after = leaf->RemoveAndDeleteRecord(key, comparator_);
    bool changed = (after != before);

    // root is leaf
    if (leaf->GetParentPageId() == INVALID_PAGE_ID) {
      if (leaf->GetSize() == 0) {
        page_id_t old_root = root_page_id_;
        root_page_id_ = INVALID_PAGE_ID;
        bpm_->UnpinPage(old_root, true);
        bpm_->DeletePage(old_root);
      } else {
        bpm_->UnpinPage(cur, changed);
      }
      return;
    }

    bpm_->UnpinPage(cur, changed);

    if (!changed) {
      return;  // key not found
    }

    // check underflow
    page_id_t under_pid = cur;
    while (true) {
      Page *up = bpm_->FetchPage(under_pid);
      if (up == nullptr) {
        throw std::runtime_error("Remove: FetchPage(under) failed");
      }
      auto *under_node = reinterpret_cast<BPlusTreePage *>(up->GetData());
      page_id_t parent_pid = under_node->GetParentPageId();
      bool under_is_leaf = under_node->IsLeafPage();
      bpm_->UnpinPage(under_pid, false);

      if (parent_pid == INVALID_PAGE_ID) {
        return;
      }

      Page *pp = bpm_->FetchPage(parent_pid);
      if (pp == nullptr) {
        throw std::runtime_error("Remove: FetchPage(parent) failed");
      }
      auto *parent = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(pp->GetData());

      int index = parent->ValueIndex(under_pid);

      page_id_t left_sib = (index > 0) ? parent->ValueAt(index - 1) : INVALID_PAGE_ID;
      page_id_t right_sib = (index + 1 < parent->GetSize()) ? parent->ValueAt(index + 1) : INVALID_PAGE_ID;

      if (under_is_leaf) {
        // Fetch under leaf
        Page *np = bpm_->FetchPage(under_pid);
        auto *leaf_u = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(np->GetData());

        int minsz = leaf_min_size(leaf_u->GetMaxSize());
        if (leaf_u->GetSize() >= minsz) {
          bpm_->UnpinPage(under_pid, false);
          bpm_->UnpinPage(parent_pid, false);
          return;
        }

        // Try borrow from left
        if (left_sib != INVALID_PAGE_ID) {
          Page *lp = bpm_->FetchPage(left_sib);
          auto *leaf_l = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(lp->GetData());
          if (leaf_l->GetSize() > leaf_min_size(leaf_l->GetMaxSize())) {
            leaf_l->MoveLastToFrontOf(leaf_u);
            parent->SetKeyAt(index, leaf_u->KeyAt(0));
            bpm_->UnpinPage(left_sib, true);
            bpm_->UnpinPage(under_pid, true);
            bpm_->UnpinPage(parent_pid, true);
            return;
          }
          bpm_->UnpinPage(left_sib, false);
        }

        // Try borrow from right
        if (right_sib != INVALID_PAGE_ID) {
          Page *rp = bpm_->FetchPage(right_sib);
          auto *leaf_r = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(rp->GetData());
          if (leaf_r->GetSize() > leaf_min_size(leaf_r->GetMaxSize())) {
            leaf_r->MoveFirstToEndOf(leaf_u);
            parent->SetKeyAt(index + 1, leaf_r->KeyAt(0));
            bpm_->UnpinPage(right_sib, true);
            bpm_->UnpinPage(under_pid, true);
            bpm_->UnpinPage(parent_pid, true);
            return;
          }
          bpm_->UnpinPage(right_sib, false);
        }

        // Merge
        if (index == 0) {
          // merge right into under
          Page *rp = bpm_->FetchPage(right_sib);
          auto *leaf_r = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(rp->GetData());
          leaf_r->MoveAllTo(leaf_u);
          bpm_->UnpinPage(right_sib, true);
          bpm_->DeletePage(right_sib);

          parent->Remove(1);
          bpm_->UnpinPage(under_pid, true);
        } else {
          // merge under into left
          Page *lp = bpm_->FetchPage(left_sib);
          auto *leaf_l = reinterpret_cast<BPlusTreeLeafPage<KeyType, ValueType, KeyComparator> *>(lp->GetData());
          leaf_u->MoveAllTo(leaf_l);
          bpm_->UnpinPage(left_sib, true);

          bpm_->UnpinPage(under_pid, true);
          bpm_->DeletePage(under_pid);

          parent->Remove(index);
        }

        // root shrink
        if (parent->GetParentPageId() == INVALID_PAGE_ID) {
          if (parent->GetSize() == 1) {
            page_id_t new_root = parent->RemoveAndReturnOnlyChild();
            root_page_id_ = new_root;

            Page *cr = bpm_->FetchPage(new_root);
            reinterpret_cast<BPlusTreePage *>(cr->GetData())->SetParentPageId(INVALID_PAGE_ID);
            bpm_->UnpinPage(new_root, true);

            bpm_->UnpinPage(parent_pid, true);
            bpm_->DeletePage(parent_pid);
            return;
          }
          bpm_->UnpinPage(parent_pid, true);
          return;
        }

        bool parent_under = parent->GetSize() < internal_min_size(parent->GetMaxSize());
        bpm_->UnpinPage(parent_pid, true);
        if (!parent_under) {
          return;
        }
        under_pid = parent_pid;
        continue;
      }

      // under is internal
      Page *np = bpm_->FetchPage(under_pid);
      auto *in_u = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(np->GetData());

      int minsz = internal_min_size(in_u->GetMaxSize());
      if (in_u->GetSize() >= minsz) {
        bpm_->UnpinPage(under_pid, false);
        bpm_->UnpinPage(parent_pid, false);
        return;
      }

      // borrow from left
      if (left_sib != INVALID_PAGE_ID) {
        Page *lp = bpm_->FetchPage(left_sib);
        auto *in_l = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(lp->GetData());
        if (in_l->GetSize() > internal_min_size(in_l->GetMaxSize())) {
          KeyType new_parent_key = in_l->KeyAt(in_l->GetSize() - 1);
          KeyType sep = parent->KeyAt(index);
          in_l->MoveLastToFrontOf(in_u, sep);
          set_children_parent(in_u, under_pid);
          parent->SetKeyAt(index, new_parent_key);

          bpm_->UnpinPage(left_sib, true);
          bpm_->UnpinPage(under_pid, true);
          bpm_->UnpinPage(parent_pid, true);
          return;
        }
        bpm_->UnpinPage(left_sib, false);
      }

      // borrow from right
      if (right_sib != INVALID_PAGE_ID) {
        Page *rp = bpm_->FetchPage(right_sib);
        auto *in_r = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(rp->GetData());
        if (in_r->GetSize() > internal_min_size(in_r->GetMaxSize())) {
          KeyType sep = parent->KeyAt(index + 1);
          in_r->MoveFirstToEndOf(in_u, sep);
          set_children_parent(in_u, under_pid);
          parent->SetKeyAt(index + 1, in_r->KeyAt(1));

          bpm_->UnpinPage(right_sib, true);
          bpm_->UnpinPage(under_pid, true);
          bpm_->UnpinPage(parent_pid, true);
          return;
        }
        bpm_->UnpinPage(right_sib, false);
      }

      // merge internal
      if (index == 0) {
        KeyType middle = parent->KeyAt(1);
        Page *rp = bpm_->FetchPage(right_sib);
        auto *in_r = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(rp->GetData());
        in_r->MoveAllTo(in_u, middle);
        set_children_parent(in_u, under_pid);
        bpm_->UnpinPage(right_sib, true);
        bpm_->DeletePage(right_sib);

        parent->Remove(1);
        bpm_->UnpinPage(under_pid, true);
      } else {
        KeyType middle = parent->KeyAt(index);
        Page *lp = bpm_->FetchPage(left_sib);
        auto *in_l = reinterpret_cast<BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator> *>(lp->GetData());
        in_u->MoveAllTo(in_l, middle);
        set_children_parent(in_l, left_sib);

        bpm_->UnpinPage(left_sib, true);
        bpm_->UnpinPage(under_pid, true);
        bpm_->DeletePage(under_pid);

        parent->Remove(index);
      }

      // root shrink
      if (parent->GetParentPageId() == INVALID_PAGE_ID) {
        if (parent->GetSize() == 1) {
          page_id_t new_root = parent->RemoveAndReturnOnlyChild();
          root_page_id_ = new_root;

          Page *cr = bpm_->FetchPage(new_root);
          reinterpret_cast<BPlusTreePage *>(cr->GetData())->SetParentPageId(INVALID_PAGE_ID);
          bpm_->UnpinPage(new_root, true);

          bpm_->UnpinPage(parent_pid, true);
          bpm_->DeletePage(parent_pid);
          return;
        }
        bpm_->UnpinPage(parent_pid, true);
        return;
      }

      bool parent_under = parent->GetSize() < internal_min_size(parent->GetMaxSize());
      bpm_->UnpinPage(parent_pid, true);
      if (!parent_under) {
        return;
      }
      under_pid = parent_pid;
      continue;
    }
  }
}

template <typename KeyType, typename ValueType, typename KeyComparator>
auto BPLUSTREE_TYPE::End() -> Iterator {
  return Iterator(INVALID_PAGE_ID, 0, bpm_);
}

}  // namespace onebase
