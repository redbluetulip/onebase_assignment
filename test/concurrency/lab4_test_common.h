#pragma once

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include <gtest/gtest.h>

#include "onebase/common/rid.h"
#include "onebase/concurrency/lock_manager.h"
#include "onebase/concurrency/transaction.h"

namespace onebase::test {

class LockManagerLab4Test : public ::testing::Test {
 protected:
  void VerifySharedLockBasic() {
    LockManager lm;
    Transaction txn(0);
    RID rid(0, 0);

    EXPECT_TRUE(lm.LockShared(&txn, rid));
    EXPECT_TRUE(txn.IsSharedLocked(rid));
    EXPECT_EQ(txn.GetState(), TransactionState::GROWING);

    EXPECT_TRUE(lm.Unlock(&txn, rid));
    EXPECT_FALSE(txn.IsSharedLocked(rid));
    EXPECT_EQ(txn.GetState(), TransactionState::SHRINKING);
  }

  void VerifySharedLockCompat() {
    LockManager lm;
    Transaction txn0(0), txn1(1);
    RID rid(0, 0);

    EXPECT_TRUE(lm.LockShared(&txn0, rid));
    EXPECT_TRUE(lm.LockShared(&txn1, rid));
    EXPECT_TRUE(txn0.IsSharedLocked(rid));
    EXPECT_TRUE(txn1.IsSharedLocked(rid));

    lm.Unlock(&txn0, rid);
    lm.Unlock(&txn1, rid);
  }

  void VerifyExclusiveLockBasic() {
    LockManager lm;
    Transaction txn(0);
    RID rid(0, 0);

    EXPECT_TRUE(lm.LockExclusive(&txn, rid));
    EXPECT_TRUE(txn.IsExclusiveLocked(rid));
    EXPECT_EQ(txn.GetState(), TransactionState::GROWING);

    lm.Unlock(&txn, rid);
    EXPECT_FALSE(txn.IsExclusiveLocked(rid));
  }

  void VerifyExclusiveLockBlock() {
    LockManager lm;
    Transaction txn0(0), txn1(1);
    RID rid(0, 0);

    lm.LockExclusive(&txn0, rid);

    std::promise<void> started;
    std::promise<bool> result;

    std::thread t([&]() {
      try {
        started.set_value();
        bool r = lm.LockShared(&txn1, rid);
        result.set_value(r);
      } catch (...) {
        result.set_value(false);
      }
    });

    started.get_future().wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_FALSE(txn1.IsSharedLocked(rid));

    lm.Unlock(&txn0, rid);
    EXPECT_TRUE(result.get_future().get());
    t.join();

    EXPECT_TRUE(txn1.IsSharedLocked(rid));
    lm.Unlock(&txn1, rid);
  }

  void VerifyLockUpgrade() {
    LockManager lm;
    Transaction txn0(0);
    RID rid(0, 0);

    EXPECT_TRUE(lm.LockShared(&txn0, rid));
    EXPECT_TRUE(txn0.IsSharedLocked(rid));

    EXPECT_TRUE(lm.LockUpgrade(&txn0, rid));
    EXPECT_TRUE(txn0.IsExclusiveLocked(rid));
    EXPECT_FALSE(txn0.IsSharedLocked(rid));

    lm.Unlock(&txn0, rid);
  }

  void VerifyUnlockAndNotify() {
    LockManager lm;
    Transaction txn0(0), txn1(1), txn2(2);
    RID rid(0, 0);

    lm.LockExclusive(&txn0, rid);

    std::atomic<int> acquired{0};
    auto lock_fn = [&](Transaction *txn) {
      try {
        lm.LockShared(txn, rid);
        acquired.fetch_add(1);
      } catch (...) {
      }
    };

    std::thread t1(lock_fn, &txn1);
    std::thread t2(lock_fn, &txn2);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(acquired.load(), 0);

    lm.Unlock(&txn0, rid);

    t1.join();
    t2.join();

    EXPECT_EQ(acquired.load(), 2);
    EXPECT_TRUE(txn1.IsSharedLocked(rid));
    EXPECT_TRUE(txn2.IsSharedLocked(rid));

    lm.Unlock(&txn1, rid);
    lm.Unlock(&txn2, rid);
  }

  void VerifyTwoPLEnforce() {
    LockManager lm;
    Transaction txn(0);
    RID rid1(0, 0), rid2(0, 1);

    lm.LockShared(&txn, rid1);
    lm.Unlock(&txn, rid1);
    EXPECT_EQ(txn.GetState(), TransactionState::SHRINKING);

    EXPECT_FALSE(lm.LockShared(&txn, rid2));
    EXPECT_EQ(txn.GetState(), TransactionState::ABORTED);

    Transaction txn2(1);
    lm.LockExclusive(&txn2, rid1);
    lm.Unlock(&txn2, rid1);
    EXPECT_EQ(txn2.GetState(), TransactionState::SHRINKING);
    EXPECT_FALSE(lm.LockExclusive(&txn2, rid2));
    EXPECT_EQ(txn2.GetState(), TransactionState::ABORTED);
  }

  void VerifyMultiResource() {
    LockManager lm;
    Transaction txn0(0), txn1(1);
    RID rid1(0, 0), rid2(0, 1);

    EXPECT_TRUE(lm.LockExclusive(&txn0, rid1));
    EXPECT_TRUE(lm.LockExclusive(&txn1, rid2));

    EXPECT_TRUE(txn0.IsExclusiveLocked(rid1));
    EXPECT_FALSE(txn0.IsExclusiveLocked(rid2));
    EXPECT_TRUE(txn1.IsExclusiveLocked(rid2));
    EXPECT_FALSE(txn1.IsExclusiveLocked(rid1));

    lm.Unlock(&txn0, rid1);
    lm.Unlock(&txn1, rid2);

    Transaction txn2(2), txn3(3);
    lm.LockExclusive(&txn2, rid1);
    lm.LockExclusive(&txn3, rid2);

    EXPECT_TRUE(txn2.IsExclusiveLocked(rid1));
    EXPECT_TRUE(txn3.IsExclusiveLocked(rid2));

    lm.Unlock(&txn2, rid1);
    lm.Unlock(&txn3, rid2);
  }
};

}  // namespace onebase::test
