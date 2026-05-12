#pragma once

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "onebase/buffer/buffer_pool_manager.h"
#include "onebase/buffer/lru_k_replacer.h"
#include "onebase/buffer/page_guard.h"
#include "onebase/storage/disk/disk_manager.h"

namespace onebase::test {

class LRUKReplacerLab1Test : public ::testing::Test {
 protected:
  void VerifyRecordAccessAndSize() {
    LRUKReplacer replacer(7, 2);

    replacer.RecordAccess(1);
    replacer.RecordAccess(2);
    replacer.RecordAccess(3);
    replacer.RecordAccess(4);
    replacer.RecordAccess(5);

    EXPECT_EQ(replacer.Size(), 0u);

    replacer.SetEvictable(1, true);
    replacer.SetEvictable(2, true);
    replacer.SetEvictable(3, true);
    EXPECT_EQ(replacer.Size(), 3u);

    replacer.SetEvictable(2, false);
    EXPECT_EQ(replacer.Size(), 2u);
  }

  void VerifyBasicEvict() {
    LRUKReplacer replacer(7, 2);

    replacer.RecordAccess(1);
    replacer.RecordAccess(1);
    replacer.RecordAccess(2);

    replacer.SetEvictable(1, true);
    replacer.SetEvictable(2, true);
    EXPECT_EQ(replacer.Size(), 2u);

    frame_id_t frame;
    EXPECT_TRUE(replacer.Evict(&frame));
    EXPECT_EQ(frame, 2);
    EXPECT_EQ(replacer.Size(), 1u);

    EXPECT_TRUE(replacer.Evict(&frame));
    EXPECT_EQ(frame, 1);
    EXPECT_EQ(replacer.Size(), 0u);
    EXPECT_FALSE(replacer.Evict(&frame));
  }

  void VerifyEvictByInfDistance() {
    LRUKReplacer replacer(7, 3);

    replacer.RecordAccess(1);
    replacer.RecordAccess(2);
    replacer.RecordAccess(3);
    replacer.RecordAccess(2);

    replacer.SetEvictable(1, true);
    replacer.SetEvictable(2, true);
    replacer.SetEvictable(3, true);

    frame_id_t frame;
    EXPECT_TRUE(replacer.Evict(&frame));
    EXPECT_EQ(frame, 1);
    EXPECT_TRUE(replacer.Evict(&frame));
    EXPECT_EQ(frame, 2);
    EXPECT_TRUE(replacer.Evict(&frame));
    EXPECT_EQ(frame, 3);
  }

  void VerifyEvictByKDistance() {
    LRUKReplacer replacer(7, 2);

    replacer.RecordAccess(1);
    replacer.RecordAccess(1);
    replacer.RecordAccess(2);
    replacer.RecordAccess(2);
    replacer.RecordAccess(3);
    replacer.RecordAccess(3);

    replacer.SetEvictable(1, true);
    replacer.SetEvictable(2, true);
    replacer.SetEvictable(3, true);

    frame_id_t frame;
    EXPECT_TRUE(replacer.Evict(&frame));
    EXPECT_EQ(frame, 1);
    EXPECT_TRUE(replacer.Evict(&frame));
    EXPECT_EQ(frame, 2);
    EXPECT_TRUE(replacer.Evict(&frame));
    EXPECT_EQ(frame, 3);
  }

  void VerifySetEvictableAndRemove() {
    LRUKReplacer replacer(7, 2);

    replacer.RecordAccess(0);
    replacer.RecordAccess(1);
    replacer.SetEvictable(0, true);
    replacer.SetEvictable(1, false);
    EXPECT_EQ(replacer.Size(), 1u);

    frame_id_t frame;
    EXPECT_TRUE(replacer.Evict(&frame));
    EXPECT_EQ(frame, 0);
    EXPECT_EQ(replacer.Size(), 0u);

    replacer.SetEvictable(1, true);
    EXPECT_EQ(replacer.Size(), 1u);
    replacer.Remove(1);
    EXPECT_EQ(replacer.Size(), 0u);
    EXPECT_FALSE(replacer.Evict(&frame));
  }
};

class BufferPoolManagerLab1Test : public ::testing::Test {
 protected:
  static auto MakeDbName(const char *tag) -> std::string { return std::string("__eval_bpm_") + tag + ".db"; }

  void VerifyNewPageBasic() {
    auto db = MakeDbName("new");
    DiskManager dm(db);
    BufferPoolManager bpm(10, &dm);

    page_id_t pid0, pid1, pid2;
    auto *p0 = bpm.NewPage(&pid0);
    auto *p1 = bpm.NewPage(&pid1);
    auto *p2 = bpm.NewPage(&pid2);

    ASSERT_NE(p0, nullptr);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(pid0, 0);
    EXPECT_EQ(pid1, 1);
    EXPECT_EQ(pid2, 2);

    bpm.UnpinPage(pid0, false);
    bpm.UnpinPage(pid1, false);
    bpm.UnpinPage(pid2, false);

    dm.ShutDown();
    std::remove(db.c_str());
  }

  void VerifyFetchPageBasic() {
    auto db = MakeDbName("fetch");
    DiskManager dm(db);
    BufferPoolManager bpm(10, &dm);

    page_id_t pid;
    auto *page = bpm.NewPage(&pid);
    ASSERT_NE(page, nullptr);

    std::snprintf(page->GetData(), 256, "hello_onebase");
    bpm.UnpinPage(pid, true);

    auto *fetched = bpm.FetchPage(pid);
    ASSERT_NE(fetched, nullptr);
    EXPECT_STREQ(fetched->GetData(), "hello_onebase");
    bpm.UnpinPage(pid, false);

    dm.ShutDown();
    std::remove(db.c_str());
  }

  void VerifyUnpinAndEvict() {
    auto db = MakeDbName("evict");
    DiskManager dm(db);
    BufferPoolManager bpm(3, &dm);

    page_id_t pids[3];
    for (int i = 0; i < 3; i++) {
      auto *p = bpm.NewPage(&pids[i]);
      ASSERT_NE(p, nullptr);
    }

    page_id_t extra;
    EXPECT_EQ(bpm.NewPage(&extra), nullptr);

    for (int i = 0; i < 3; i++) {
      bpm.UnpinPage(pids[i], false);
    }

    auto *p = bpm.NewPage(&extra);
    EXPECT_NE(p, nullptr);
    bpm.UnpinPage(extra, false);

    dm.ShutDown();
    std::remove(db.c_str());
  }

  void VerifyDirtyPagePersistence() {
    auto db = MakeDbName("dirty");
    DiskManager dm(db);
    BufferPoolManager bpm(3, &dm);

    page_id_t pid;
    auto *page = bpm.NewPage(&pid);
    ASSERT_NE(page, nullptr);
    std::snprintf(page->GetData(), 256, "persist_test");
    bpm.UnpinPage(pid, true);

    page_id_t dummy_pids[3];
    for (int i = 0; i < 3; i++) {
      bpm.NewPage(&dummy_pids[i]);
    }
    for (int i = 0; i < 3; i++) {
      bpm.UnpinPage(dummy_pids[i], false);
    }

    auto *fetched = bpm.FetchPage(pid);
    ASSERT_NE(fetched, nullptr);
    EXPECT_STREQ(fetched->GetData(), "persist_test");
    bpm.UnpinPage(pid, false);

    dm.ShutDown();
    std::remove(db.c_str());
  }

  void VerifyDeletePage() {
    auto db = MakeDbName("del");
    DiskManager dm(db);
    BufferPoolManager bpm(3, &dm);

    page_id_t pids[3];
    for (int i = 0; i < 3; i++) {
      auto *p = bpm.NewPage(&pids[i]);
      ASSERT_NE(p, nullptr);
    }

    page_id_t extra;
    EXPECT_EQ(bpm.NewPage(&extra), nullptr);

    bpm.UnpinPage(pids[0], false);
    EXPECT_TRUE(bpm.DeletePage(pids[0]));

    auto *p = bpm.NewPage(&extra);
    EXPECT_NE(p, nullptr);
    bpm.UnpinPage(extra, false);

    bpm.UnpinPage(pids[1], false);
    bpm.UnpinPage(pids[2], false);

    dm.ShutDown();
    std::remove(db.c_str());
  }

  void VerifyFlushPage() {
    auto db = MakeDbName("flush");
    DiskManager dm(db);
    BufferPoolManager bpm(10, &dm);

    page_id_t pid;
    auto *page = bpm.NewPage(&pid);
    ASSERT_NE(page, nullptr);
    std::snprintf(page->GetData(), 256, "flush_data");

    EXPECT_TRUE(bpm.FlushPage(pid));
    bpm.UnpinPage(pid, false);

    char buf[PAGE_SIZE] = {};
    dm.ReadPage(pid, buf);
    EXPECT_STREQ(buf, "flush_data");

    dm.ShutDown();
    std::remove(db.c_str());
  }
};

class PageGuardLab1Test : public ::testing::Test {
 protected:
  static auto MakeDbName(const char *tag) -> std::string { return std::string("__eval_bpm_") + tag + ".db"; }

  void VerifyBasicGuardUnpin() {
    auto db = MakeDbName("guard_basic");
    DiskManager dm(db);
    BufferPoolManager bpm(3, &dm);

    page_id_t pid;
    auto *page = bpm.NewPage(&pid);
    ASSERT_NE(page, nullptr);

    {
      BasicPageGuard guard(&bpm, page);
      EXPECT_EQ(guard.GetPageId(), pid);
    }

    page_id_t pids[2];
    for (int i = 0; i < 2; i++) {
      auto *p = bpm.NewPage(&pids[i]);
      ASSERT_NE(p, nullptr);
    }
    page_id_t extra;
    auto *p = bpm.NewPage(&extra);
    EXPECT_NE(p, nullptr);

    bpm.UnpinPage(pids[0], false);
    bpm.UnpinPage(pids[1], false);
    bpm.UnpinPage(extra, false);

    dm.ShutDown();
    std::remove(db.c_str());
  }

  void VerifyReadGuardLatch() {
    auto db = MakeDbName("guard_read");
    DiskManager dm(db);
    BufferPoolManager bpm(3, &dm);

    page_id_t pid;
    auto *page = bpm.NewPage(&pid);
    ASSERT_NE(page, nullptr);

    {
      ReadPageGuard guard(&bpm, page);
      EXPECT_EQ(guard.GetPageId(), pid);
    }

    auto *fetched = bpm.FetchPage(pid);
    ASSERT_NE(fetched, nullptr);
    fetched->WLatch();
    fetched->WUnlatch();
    bpm.UnpinPage(pid, false);

    dm.ShutDown();
    std::remove(db.c_str());
  }

  void VerifyWriteGuardLatch() {
    auto db = MakeDbName("guard_write");
    DiskManager dm(db);
    BufferPoolManager bpm(3, &dm);

    page_id_t pid;
    auto *page = bpm.NewPage(&pid);
    ASSERT_NE(page, nullptr);

    {
      WritePageGuard guard(&bpm, page);
      EXPECT_EQ(guard.GetPageId(), pid);
      std::snprintf(guard.GetDataMut(), 256, "write_guard_data");
    }

    auto *fetched = bpm.FetchPage(pid);
    ASSERT_NE(fetched, nullptr);
    EXPECT_STREQ(fetched->GetData(), "write_guard_data");
    fetched->RLatch();
    fetched->RUnlatch();
    bpm.UnpinPage(pid, false);

    dm.ShutDown();
    std::remove(db.c_str());
  }

  void VerifyMoveSemantics() {
    auto db = MakeDbName("guard_move");
    DiskManager dm(db);
    BufferPoolManager bpm(3, &dm);

    page_id_t pid;
    auto *page = bpm.NewPage(&pid);
    ASSERT_NE(page, nullptr);

    BasicPageGuard guard1(&bpm, page);
    EXPECT_EQ(guard1.GetPageId(), pid);

    BasicPageGuard guard2(std::move(guard1));
    EXPECT_EQ(guard1.GetPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(guard2.GetPageId(), pid);

    BasicPageGuard guard3;
    guard3 = std::move(guard2);
    EXPECT_EQ(guard2.GetPageId(), INVALID_PAGE_ID);
    EXPECT_EQ(guard3.GetPageId(), pid);

    guard3.Drop();

    dm.ShutDown();
    std::remove(db.c_str());
  }
};

}  // namespace onebase::test
