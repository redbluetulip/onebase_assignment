#include "onebase/execution/execution_engine.h"

namespace onebase {

auto ExecutionEngine::Execute(const AbstractPlanNodeRef &plan, std::vector<Tuple> *result_set) -> bool {
  auto executor = ExecutorFactory::CreateExecutor(exec_ctx_, plan);
  executor->Init();
  const auto &schema = executor->GetOutputSchema();
  Tuple tuple;
  RID rid;
  while (executor->Next(&tuple, &rid)) {
    if (result_set != nullptr) {
      std::vector<Value> values;
      values.reserve(schema.GetColumnCount());
      for (uint32_t i = 0; i < schema.GetColumnCount(); ++i) {
        values.push_back(tuple.GetValue(&schema, i));
      }
      result_set->emplace_back(std::move(values));
    }
  }
  return true;
}

}  // namespace onebase
