#include "onebase/execution/executors/insert_executor.h"

namespace onebase {

InsertExecutor::InsertExecutor(ExecutorContext *exec_ctx, const InsertPlanNode *plan,
                               std::unique_ptr<AbstractExecutor> child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

void InsertExecutor::Init() {
  // Initialize child executor and reset one-shot flag
  child_executor_->Init();
  has_inserted_ = false;
}

auto InsertExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  // Execute only once
  if (has_inserted_) {
    return false;
  }
  has_inserted_ = true;

  auto *catalog = GetExecutorContext()->GetCatalog();
  auto *table_info = catalog->GetTable(plan_->GetTableOid());
  auto *table_heap = table_info->table_.get();

  int count = 0;
  Tuple child_tuple;
  RID child_rid;

  while (child_executor_->Next(&child_tuple, &child_rid)) {
    auto new_rid_opt = table_heap->InsertTuple(child_tuple);
    if (!new_rid_opt.has_value()) {
      // Insert failed (e.g., no space). Skip this tuple.
      continue;
    }
    count++;

    // NOTE:
    // Your Catalog::IndexInfo currently stores only metadata (no index instance object),
    // and Catalog::CreateIndex does not create/hold an actual B+Tree index.
    // Therefore, index maintenance (InsertEntry) cannot be performed here in this codebase.
  }

  // Return affected row count as a single integer tuple
  if (tuple != nullptr) {
    std::vector<Value> values;
    values.emplace_back(TypeId::INTEGER, count);
    *tuple = Tuple(std::move(values));
  }
  if (rid != nullptr) {
    *rid = RID{};  // not meaningful for this executor's output
  }
  return true;
}

}  // namespace onebase