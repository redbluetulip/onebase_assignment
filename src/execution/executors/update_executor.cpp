#include "onebase/execution/executors/update_executor.h"

namespace onebase {

UpdateExecutor::UpdateExecutor(ExecutorContext *exec_ctx, const UpdatePlanNode *plan,
                               std::unique_ptr<AbstractExecutor> child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

void UpdateExecutor::Init() {
  child_executor_->Init();
  has_updated_ = false;
}

auto UpdateExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  // Execute only once
  if (has_updated_) {
    return false;
  }
  has_updated_ = true;

  auto *catalog = GetExecutorContext()->GetCatalog();
  auto *table_info = catalog->GetTable(plan_->GetTableOid());
  auto *table_heap = table_info->table_.get();

  int count = 0;
  Tuple old_tuple;
  RID old_rid;

  const auto &update_exprs = plan_->GetUpdateExpressions();
  const auto &schema = table_info->schema_;

  while (child_executor_->Next(&old_tuple, &old_rid)) {
    std::vector<Value> new_values;
    new_values.reserve(update_exprs.size());

    for (const auto &expr : update_exprs) {
      new_values.push_back(expr->Evaluate(&old_tuple, &schema));
    }

    Tuple new_tuple(std::move(new_values));
    table_heap->UpdateTuple(old_rid, new_tuple);
    count++;

    // NOTE:
    // Your Catalog::IndexInfo currently stores only metadata (no index instance object),
    // so index maintenance (delete old entry + insert new entry) cannot be done here.
  }

  // Return affected row count as a single integer tuple
  if (tuple != nullptr) {
    std::vector<Value> values;
    values.emplace_back(TypeId::INTEGER, count);
    *tuple = Tuple(std::move(values));
  }
  if (rid != nullptr) {
    *rid = RID{};
  }
  return true;
}

}  // namespace onebase