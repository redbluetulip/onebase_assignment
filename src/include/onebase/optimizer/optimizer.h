#pragma once

#include "onebase/catalog/catalog.h"
#include "onebase/execution/plans/abstract_plan_node.h"

namespace onebase {

class Optimizer {
 public:
  explicit Optimizer(Catalog *catalog = nullptr) : catalog_(catalog) {}

  auto Optimize(AbstractPlanNodeRef plan) -> AbstractPlanNodeRef;

 private:
  auto OptimizeSeqScanToIndexScan(AbstractPlanNodeRef plan) -> AbstractPlanNodeRef;
  auto OptimizeNLJToHashJoin(AbstractPlanNodeRef plan) -> AbstractPlanNodeRef;

  Catalog *catalog_;
};

}  // namespace onebase
