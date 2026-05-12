#include "onebase/execution/executors/index_scan_executor.h"
#include "onebase/common/exception.h"

namespace onebase {

IndexScanExecutor::IndexScanExecutor(ExecutorContext *exec_ctx, const IndexScanPlanNode *plan)
    : AbstractExecutor(exec_ctx), plan_(plan) {}

void IndexScanExecutor::Init() {
  auto *catalog = GetExecutorContext()->GetCatalog();
  table_info_ = catalog->GetTable(plan_->GetTableOid());
  index_info_ = catalog->GetIndex(plan_->GetIndexOid());

  matching_rids_.clear();
  cursor_ = 0;

  if (table_info_ == nullptr || index_info_ == nullptr || !index_info_->SupportsPointLookup()) {
    return;
  }

  Value lookup_key = plan_->GetLookupKey()->Evaluate(nullptr, nullptr);
  if (lookup_key.IsNull() || lookup_key.GetTypeId() != TypeId::INTEGER) {
    return;
  }

  const auto *rids = index_info_->LookupInteger(lookup_key.GetAsInteger());
  if (rids == nullptr) {
    return;
  }

  matching_rids_ = *rids;
}

auto IndexScanExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  while (cursor_ < matching_rids_.size()) {
    RID current_rid = matching_rids_[cursor_++];
    Tuple current_tuple = table_info_->table_->GetTuple(current_rid);

    if (plan_->GetPredicate() != nullptr) {
      Value predicate_value = plan_->GetPredicate()->Evaluate(&current_tuple, &table_info_->schema_);
      if (!predicate_value.GetAsBoolean()) {
        continue;
      }
    }

    if (tuple != nullptr) {
      std::vector<Value> values;
      values.reserve(table_info_->schema_.GetColumnCount());
      for (uint32_t i = 0; i < table_info_->schema_.GetColumnCount(); ++i) {
        values.push_back(current_tuple.GetValue(&table_info_->schema_, i));
      }
      *tuple = Tuple(std::move(values));
    }
    if (rid != nullptr) {
      *rid = current_rid;
    }
    return true;
  }

  return false;
}

}  // namespace onebase
