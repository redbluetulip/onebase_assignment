#include "onebase/execution/executors/delete_executor.h"

namespace onebase {

DeleteExecutor::DeleteExecutor(ExecutorContext *exec_ctx, const DeletePlanNode *plan,
                               std::unique_ptr<AbstractExecutor> child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

void DeleteExecutor::Init() {
  child_executor_->Init();
  has_deleted_ = false;
}

auto DeleteExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  // Execute only once
  if (has_deleted_) {
    return false;
  }
  has_deleted_ = true;

  auto *catalog = GetExecutorContext()->GetCatalog();
  auto *table_info = catalog->GetTable(plan_->GetTableOid());
  auto *table_heap = table_info->table_.get();

  int count = 0;
  Tuple child_tuple;
  RID child_rid;

  while (child_executor_->Next(&child_tuple, &child_rid)) {
    // In many Volcano designs, the child's RID is the target row RID.
    table_heap->DeleteTuple(child_rid);
    count++;

    // NOTE:
    // Your Catalog::IndexInfo currently stores only metadata (no index instance object),
    // and Catalog::CreateIndex does not create/hold an actual B+Tree index.
    // Therefore, index maintenance (DeleteEntry) cannot be performed here in this codebase.
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