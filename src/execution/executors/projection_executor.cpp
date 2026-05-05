#include "onebase/execution/executors/projection_executor.h"
#include "onebase/common/exception.h"

namespace onebase {

ProjectionExecutor::ProjectionExecutor(ExecutorContext *exec_ctx, const ProjectionPlanNode *plan,
                                       std::unique_ptr<AbstractExecutor> child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

void ProjectionExecutor::Init() {
  child_executor_->Init();
}

auto ProjectionExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  Tuple child_tuple;
  RID child_rid;

  if (!child_executor_->Next(&child_tuple, &child_rid)) {
    return false;
  }

  const auto &exprs = plan_->GetExpressions();
  const auto &child_schema = child_executor_->GetOutputSchema();

  std::vector<Value> values;
  values.reserve(exprs.size());

  for (const auto &expr : exprs) {
    values.push_back(expr->Evaluate(&child_tuple, &child_schema));
  }

  *tuple = Tuple(std::move(values));
  *rid = child_rid;
  return true;
}

}  // namespace onebase