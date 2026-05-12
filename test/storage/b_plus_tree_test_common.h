#pragma once

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "onebase/buffer/buffer_pool_manager.h"
#include "onebase/common/rid.h"
#include "onebase/storage/disk/disk_manager.h"
#include "onebase/storage/index/b_plus_tree.h"
#include "onebase/storage/index/b_plus_tree_iterator.h"
#include "onebase/storage/page/b_plus_tree_page.h"

namespace onebase::test {

class BPlusTreeLab2Test : public ::testing::Test {
 protected:
  using TreeType = BPlusTree<int, RID, std::less<int>>;

  void SetUp() override {
    const auto *test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    db_name_ = std::string("__bpt_") + test_info->test_suite_name() + "_" + test_info->name() + ".db";
    disk_manager_ = std::make_unique<DiskManager>(db_name_);
    bpm_ = std::make_unique<BufferPoolManager>(32, disk_manager_.get());
    tree_ = std::make_unique<TreeType>("test_tree", bpm_.get(), std::less<int>{}, 4, 4);
  }

  void TearDown() override {
    tree_.reset();
    bpm_.reset();
    if (disk_manager_ != nullptr) {
      disk_manager_->ShutDown();
    }
    disk_manager_.reset();
    std::remove(db_name_.c_str());
  }

  auto MakeRid(int key) const -> RID { return RID(key / 8, key % 8); }

  auto Lookup(int key) -> std::vector<RID> {
    std::vector<RID> result;
    tree_->GetValue(key, &result);
    return result;
  }

  void InsertKeys(const std::vector<int> &keys) {
    for (int key : keys) {
      EXPECT_TRUE(tree_->Insert(key, MakeRid(key))) << "failed to insert key " << key;
    }
  }

  auto CollectKeysFrom(TreeType::Iterator it) -> std::vector<int> {
    std::vector<int> keys;
    for (; it != tree_->End(); ++it) {
      keys.push_back((*it).first);
    }
    return keys;
  }

  auto RootIsLeaf() -> bool {
    auto *page = bpm_->FetchPage(tree_->GetRootPageId());
    EXPECT_NE(page, nullptr);
    auto *tree_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
    bool is_leaf = tree_page->IsLeafPage();
    EXPECT_TRUE(bpm_->UnpinPage(page->GetPageId(), false));
    return is_leaf;
  }

  void VerifyInsertLookupAndDuplicateHandling() {
    EXPECT_TRUE(tree_->IsEmpty());

    EXPECT_TRUE(tree_->Insert(7, MakeRid(7)));
    EXPECT_FALSE(tree_->Insert(7, MakeRid(70)));

    auto values = Lookup(7);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0], MakeRid(7));
    EXPECT_TRUE(Lookup(999).empty());
  }

  void VerifyInsertionsSplitAndIterationIsOrdered() {
    InsertKeys({5, 1, 9, 3, 7, 2, 8, 6, 4, 0});

    EXPECT_NE(tree_->GetRootPageId(), INVALID_PAGE_ID);
    EXPECT_FALSE(RootIsLeaf());

    for (int key = 0; key <= 9; ++key) {
      auto values = Lookup(key);
      ASSERT_EQ(values.size(), 1u);
      EXPECT_EQ(values[0], MakeRid(key));
    }

    EXPECT_EQ(CollectKeysFrom(tree_->Begin()), (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}));
  }

  void VerifyBeginFromKeyAndSparseLookupsWork() {
    InsertKeys({10, 20, 30, 40, 50, 60});

    EXPECT_EQ(CollectKeysFrom(tree_->Begin(10)), (std::vector<int>{10, 20, 30, 40, 50, 60}));
    EXPECT_EQ(CollectKeysFrom(tree_->Begin(30)), (std::vector<int>{30, 40, 50, 60}));
    EXPECT_EQ(CollectKeysFrom(tree_->Begin(35)), (std::vector<int>{40, 50, 60}));
    EXPECT_EQ(CollectKeysFrom(tree_->Begin(99)), std::vector<int>{});
  }

  void VerifyDeleteMaintainsCorrectnessAndCanEmptyTree() {
    InsertKeys({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});

    tree_->Remove(1);
    tree_->Remove(2);
    tree_->Remove(3);
    tree_->Remove(5);
    tree_->Remove(7);

    EXPECT_TRUE(Lookup(1).empty());
    EXPECT_TRUE(Lookup(2).empty());
    EXPECT_TRUE(Lookup(3).empty());
    EXPECT_TRUE(Lookup(5).empty());
    EXPECT_TRUE(Lookup(7).empty());
    EXPECT_EQ(CollectKeysFrom(tree_->Begin()), (std::vector<int>{0, 4, 6, 8, 9}));

    for (int key : {0, 4, 6, 8, 9}) {
      tree_->Remove(key);
    }

    EXPECT_TRUE(tree_->IsEmpty());
    EXPECT_EQ(tree_->GetRootPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(tree_->Begin(), tree_->End());
  }

  std::string db_name_;
  std::unique_ptr<DiskManager> disk_manager_;
  std::unique_ptr<BufferPoolManager> bpm_;
  std::unique_ptr<TreeType> tree_;
};

using BPlusTreeTest = BPlusTreeLab2Test;

}  // namespace onebase::test
