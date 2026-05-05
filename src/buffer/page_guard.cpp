#include "onebase/buffer/page_guard.h"
#include "onebase/common/exception.h"

namespace onebase {

// --- BasicPageGuard ---

BasicPageGuard::BasicPageGuard(BufferPoolManager *bpm, Page *page)
    : bpm_(bpm), page_(page) {}

BasicPageGuard::BasicPageGuard(BasicPageGuard &&that) noexcept
    : bpm_(that.bpm_), page_(that.page_), is_dirty_(that.is_dirty_) {
  that.bpm_ = nullptr; // 将移动源置空，防止源对象析构时释放资源
  that.page_ = nullptr;
}

auto BasicPageGuard::operator=(BasicPageGuard &&that) -> BasicPageGuard & {
  if (this != &that) {
    Drop(); // 赋值前必须先释放当前持有的页面资源
    bpm_ = that.bpm_;
    page_ = that.page_;
    is_dirty_ = that.is_dirty_;
    that.bpm_ = nullptr; // 转移所有权
    that.page_ = nullptr;
  }
  return *this;
}

BasicPageGuard::~BasicPageGuard() { Drop(); }

// 补全：测试用例需要的工具函数
auto BasicPageGuard::GetPageId() const -> page_id_t {
  return page_ ? page_->GetPageId() : INVALID_PAGE_ID;
}

auto BasicPageGuard::GetData() const -> const char * {
  return page_ ? page_->GetData() : nullptr;
}

auto BasicPageGuard::GetDataMut() -> char * {
  is_dirty_ = true; // 获取可变指针通常意味着修改，标记为脏
  return page_ ? page_->GetData() : nullptr;
}

void BasicPageGuard::Drop() {
  if (page_ != nullptr && bpm_ != nullptr) {
    bpm_->UnpinPage(page_->GetPageId(), is_dirty_); // 自动执行 Unpin
  }
  bpm_ = nullptr;
  page_ = nullptr;
  is_dirty_ = false;
}

// --- ReadPageGuard ---

ReadPageGuard::ReadPageGuard(BufferPoolManager *bpm, Page *page)
    : bpm_(bpm), page_(page) {
  if (page_) { page_->RLatch(); } // 构造时加读锁
}

ReadPageGuard::ReadPageGuard(ReadPageGuard &&that) noexcept
    : bpm_(that.bpm_), page_(that.page_) {
  that.bpm_ = nullptr;
  that.page_ = nullptr;
}

auto ReadPageGuard::operator=(ReadPageGuard &&that) -> ReadPageGuard & {
  if (this != &that) {
    Drop(); // 释放旧锁和页面
    bpm_ = that.bpm_;
    page_ = that.page_;
    that.bpm_ = nullptr;
    that.page_ = nullptr;
  }
  return *this;
}

ReadPageGuard::~ReadPageGuard() { Drop(); }

// 补全接口
auto ReadPageGuard::GetPageId() const -> page_id_t {
  return page_ ? page_->GetPageId() : INVALID_PAGE_ID;
}

auto ReadPageGuard::GetData() const -> const char * {
  return page_ ? page_->GetData() : nullptr;
}

void ReadPageGuard::Drop() {
  if (page_ != nullptr && bpm_ != nullptr) {
    page_->RUnlatch(); // 先释放读锁
    bpm_->UnpinPage(page_->GetPageId(), false); // 再 Unpin
  }
  bpm_ = nullptr;
  page_ = nullptr;
}

// --- WritePageGuard ---

WritePageGuard::WritePageGuard(BufferPoolManager *bpm, Page *page)
    : bpm_(bpm), page_(page) {
  if (page_) { page_->WLatch(); } // 构造时加写锁
}

WritePageGuard::WritePageGuard(WritePageGuard &&that) noexcept
    : bpm_(that.bpm_), page_(that.page_) {
  that.bpm_ = nullptr;
  that.page_ = nullptr;
}

auto WritePageGuard::operator=(WritePageGuard &&that) -> WritePageGuard & {
  if (this != &that) {
    Drop(); // 释放旧写锁并 Unpin
    bpm_ = that.bpm_;
    page_ = that.page_;
    that.bpm_ = nullptr;
    that.page_ = nullptr;
  }
  return *this;
}

WritePageGuard::~WritePageGuard() { Drop(); }

// 补全接口
auto WritePageGuard::GetPageId() const -> page_id_t {
  return page_ ? page_->GetPageId() : INVALID_PAGE_ID;
}

auto WritePageGuard::GetData() const -> const char * {
  return page_ ? page_->GetData() : nullptr;
}

auto WritePageGuard::GetDataMut() -> char * {
  return page_ ? page_->GetData() : nullptr;
}

void WritePageGuard::Drop() {
  if (page_ != nullptr && bpm_ != nullptr) {
    page_->WUnlatch(); // 先释放写锁
    bpm_->UnpinPage(page_->GetPageId(), true); // 写锁释放通常标记为脏页
  }
  bpm_ = nullptr;
  page_ = nullptr;
}

}  // namespace onebase
