#include "onebase/execution/executors/hash_join_executor.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace onebase {

HashJoinExecutor::HashJoinExecutor(ExecutorContext *exec_ctx, const HashJoinPlanNode *plan,
                                   std::unique_ptr<AbstractExecutor> left_executor,
                                   std::unique_ptr<AbstractExecutor> right_executor)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      left_executor_(std::move(left_executor)),
      right_executor_(std::move(right_executor)) {}

void HashJoinExecutor::Init() {
  // Build phase: read all left tuples into a hash table keyed by left key expression
  left_executor_->Init();
  right_executor_->Init();

  hash_table_.clear();
  result_tuples_.clear();
  cursor_ = 0;

  const auto &left_schema = left_executor_->GetOutputSchema();
  const auto &right_schema = right_executor_->GetOutputSchema();

  Tuple left_tuple;
  RID left_rid;

  // Build hash table: key -> list of left tuples
  while (left_executor_->Next(&left_tuple, &left_rid)) {
    Value key = plan_->GetLeftKeyExpression()->Evaluate(&left_tuple, &left_schema);
    hash_table_[key.ToString()].push_back(left_tuple);
  }

  // Probe phase (materialize all outputs now)
  Tuple right_tuple;
  RID right_rid;

  while (right_executor_->Next(&right_tuple, &right_rid)) {
    Value key = plan_->GetRightKeyExpression()->Evaluate(&right_tuple, &right_schema);
    auto it = hash_table_.find(key.ToString());
    if (it == hash_table_.end()) {
      continue;
    }

    for (const auto &lt : it->second) {
      std::vector<Value> values;
      values.reserve(left_schema.GetColumnCount() + right_schema.GetColumnCount());

      for (uint32_t i = 0; i < left_schema.GetColumnCount(); i++) {
        values.push_back(lt.GetValue(&left_schema, i));
      }
      for (uint32_t i = 0; i < right_schema.GetColumnCount(); i++) {
        values.push_back(right_tuple.GetValue(&right_schema, i));
      }

      result_tuples_.emplace_back(std::move(values));
    }
  }
}

auto HashJoinExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  if (cursor_ >= result_tuples_.size()) {
    return false;
  }

  if (tuple != nullptr) {
    *tuple = result_tuples_[cursor_];
  }
  if (rid != nullptr) {
    *rid = RID{};
  }
  cursor_++;
  return true;
}

}  // namespace onebase