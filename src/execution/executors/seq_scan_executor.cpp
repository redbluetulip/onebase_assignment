#include "onebase/execution/executors/seq_scan_executor.h"

namespace onebase {

SeqScanExecutor::SeqScanExecutor(ExecutorContext *exec_ctx, const SeqScanPlanNode *plan)
    : AbstractExecutor(exec_ctx), plan_(plan) {}

void SeqScanExecutor::Init() {
  // 1) Get table info
  auto *catalog = GetExecutorContext()->GetCatalog();
  auto *table_info = catalog->GetTable(plan_->GetTableOid());
  table_info_ = table_info;

  // 2) Initialize iterator
  iter_ = table_info_->table_->Begin();
}

auto SeqScanExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  auto *table_heap = table_info_->table_.get();
  auto end = table_heap->End();

  while (iter_ != end) {
    Tuple cur = *iter_;
    ++iter_;

    // Set rid
    if (rid != nullptr) {
      *rid = cur.GetRID();
    }

    // Predicate filter (if any)
    if (plan_->GetPredicate() != nullptr) {
      const auto *schema = &table_info_->schema_;
      auto pred_val = plan_->GetPredicate()->Evaluate(&cur, schema);
      if (!pred_val.GetAsBoolean())  {  // if not satisfied, skip
        continue;
      }
    }

    if (tuple != nullptr) {
      *tuple = cur;
    }
    return true;
  }

  return false;
}

}  // namespace onebase