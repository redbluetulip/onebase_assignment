#include "onebase/execution/executors/nested_loop_join_executor.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace onebase {

namespace {

// We keep per-executor runtime state here to avoid modifying the header.
template <typename KeyType, typename ValueType>
struct NljState {
  bool has_left{false};
  Tuple left_tuple{};
  RID left_rid{};
};

static std::unordered_map<const NestedLoopJoinExecutor *, NljState<int, int>> g_state;  
// NOTE: template params above are dummy; Tuple/RID are not templated.
// We only need a concrete type to store the fields.

}  // namespace

NestedLoopJoinExecutor::NestedLoopJoinExecutor(ExecutorContext *exec_ctx,
                                               const NestedLoopJoinPlanNode *plan,
                                               std::unique_ptr<AbstractExecutor> left_executor,
                                               std::unique_ptr<AbstractExecutor> right_executor)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      left_executor_(std::move(left_executor)),
      right_executor_(std::move(right_executor)) {}

void NestedLoopJoinExecutor::Init() {
  left_executor_->Init();
  right_executor_->Init();

  auto &st = g_state[this];
  st.has_left = left_executor_->Next(&st.left_tuple, &st.left_rid);
  // right_executor_ already initialized for the first left tuple
}

auto NestedLoopJoinExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  auto &st = g_state[this];

  const auto &left_schema = left_executor_->GetOutputSchema();
  const auto &right_schema = right_executor_->GetOutputSchema();

  Tuple right_tuple;
  RID right_rid;

  while (st.has_left) {
    // scan the whole right side for current left tuple
    while (right_executor_->Next(&right_tuple, &right_rid)) {
      bool match = true;
      if (plan_->GetPredicate() != nullptr) {
        match = plan_->GetPredicate()
                    ->EvaluateJoin(&st.left_tuple, &left_schema, &right_tuple, &right_schema)
                    .GetAsBoolean();
      }
      if (!match) {
        continue;
      }

      // construct joined output tuple: [left cols..., right cols...]
      std::vector<Value> values;
      values.reserve(left_schema.GetColumnCount() + right_schema.GetColumnCount());

      for (uint32_t i = 0; i < left_schema.GetColumnCount(); i++) {
        values.push_back(st.left_tuple.GetValue(&left_schema, i));
      }
      for (uint32_t i = 0; i < right_schema.GetColumnCount(); i++) {
        values.push_back(right_tuple.GetValue(&right_schema, i));
      }

      if (tuple != nullptr) {
        *tuple = Tuple(std::move(values));
      }
      if (rid != nullptr) {
        *rid = RID{};  // output rid is not meaningful for join output
      }
      return true;
    }

    // right exhausted for this left row -> advance left and reset right
    st.has_left = left_executor_->Next(&st.left_tuple, &st.left_rid);
    right_executor_->Init();
  }

  return false;
}

}  // namespace onebase