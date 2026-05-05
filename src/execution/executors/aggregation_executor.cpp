// src/execution/executors/aggregation_executor.cpp

#include "onebase/execution/executors/aggregation_executor.h"

#include <unordered_map>
#include <utility>
#include <vector>

#include "onebase/common/types.h"

namespace onebase {

AggregationExecutor::AggregationExecutor(ExecutorContext *exec_ctx, const AggregationPlanNode *plan,
                                         std::unique_ptr<AbstractExecutor> child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

// Pipeline breaker aggregation; keep runtime state in a static map keyed by executor pointer
// so we do NOT need to modify the header file.
namespace {

struct AggState {
  std::vector<Value> group_vals;
  std::vector<Value> agg_vals;
};

struct AggExecState {
  std::vector<Tuple> results;
  size_t cursor{0};
};

static std::unordered_map<const AggregationExecutor *, AggExecState> g_state;

}  // namespace

void AggregationExecutor::Init() {
  child_executor_->Init();

  auto &st = g_state[this];
  st.results.clear();
  st.cursor = 0;

  const auto &child_schema = child_executor_->GetOutputSchema();
  const auto &out_schema = plan_->GetOutputSchema();

  const auto &group_bys = plan_->GetGroupBys();
  const auto &agg_exprs = plan_->GetAggregates();
  const auto &agg_types = plan_->GetAggregateTypes();

  std::unordered_map<std::string, AggState> groups;

  Tuple child_tuple;
  RID child_rid;

  while (child_executor_->Next(&child_tuple, &child_rid)) {
    // compute group values and group key string
    std::vector<Value> group_vals;
    group_vals.reserve(group_bys.size());
    std::string group_key;

    for (size_t i = 0; i < group_bys.size(); i++) {
      Value v = group_bys[i]->Evaluate(&child_tuple, &child_schema);
      group_vals.push_back(v);
      if (i > 0) group_key.push_back('|');
      group_key += v.ToString();  // ok: this is only used for grouping key
    }

    auto it = groups.find(group_key);
    if (it == groups.end()) {
      AggState s;
      s.group_vals = group_vals;
      s.agg_vals.resize(agg_types.size());

      // Initialize aggregate values using OUTPUT schema types
      for (size_t i = 0; i < agg_types.size(); i++) {
        auto agg_col_idx = static_cast<uint32_t>(group_bys.size() + i);
        auto agg_type_id = out_schema.GetColumn(agg_col_idx).GetType();

        switch (agg_types[i]) {
          case AggregationType::CountStarAggregate:
          case AggregationType::CountAggregate:
            s.agg_vals[i] = Value(agg_type_id, 0);
            break;
          case AggregationType::SumAggregate:
          case AggregationType::MinAggregate:
          case AggregationType::MaxAggregate:
            // IMPORTANT: use Value(type) to represent NULL (do NOT pass nullptr)
            s.agg_vals[i] = Value(agg_type_id);
            break;
          default:
            s.agg_vals[i] = Value(agg_type_id);
            break;
        }
      }

      it = groups.emplace(group_key, std::move(s)).first;
    }

    // update aggregates
    for (size_t i = 0; i < agg_types.size(); i++) {
      switch (agg_types[i]) {
        case AggregationType::CountStarAggregate: {
          auto t = it->second.agg_vals[i].GetTypeId();
          it->second.agg_vals[i] = it->second.agg_vals[i].Add(Value(t, 1));
          break;
        }
        case AggregationType::CountAggregate: {
          Value v = agg_exprs[i]->Evaluate(&child_tuple, &child_schema);
          if (!v.IsNull()) {
            auto t = it->second.agg_vals[i].GetTypeId();
            it->second.agg_vals[i] = it->second.agg_vals[i].Add(Value(t, 1));
          }
          break;
        }
        case AggregationType::SumAggregate: {
          Value v = agg_exprs[i]->Evaluate(&child_tuple, &child_schema);
          if (v.IsNull()) {
            break;
          }
          if (it->second.agg_vals[i].IsNull()) {
            it->second.agg_vals[i] = v;
          } else {
            it->second.agg_vals[i] = it->second.agg_vals[i].Add(v);
          }
          break;
        }
        case AggregationType::MinAggregate: {
          Value v = agg_exprs[i]->Evaluate(&child_tuple, &child_schema);
          if (v.IsNull()) {
            break;
          }
          if (it->second.agg_vals[i].IsNull() ||
              v.CompareLessThan(it->second.agg_vals[i]).GetAsBoolean()) {
            it->second.agg_vals[i] = v;
          }
          break;
        }
        case AggregationType::MaxAggregate: {
          Value v = agg_exprs[i]->Evaluate(&child_tuple, &child_schema);
          if (v.IsNull()) {
            break;
          }
          if (it->second.agg_vals[i].IsNull() ||
              it->second.agg_vals[i].CompareLessThan(v).GetAsBoolean()) {
            it->second.agg_vals[i] = v;
          }
          break;
        }
        default:
          break;
      }
    }
  }

  // materialize output tuples
  if (groups.empty()) {
    // Special case: no GROUP BY and empty input => still output one row
    if (group_bys.empty()) {
      std::vector<Value> out_vals;
      out_vals.resize(agg_types.size());

      for (size_t i = 0; i < agg_types.size(); i++) {
        auto agg_col_idx = static_cast<uint32_t>(group_bys.size() + i);
        auto agg_type_id = out_schema.GetColumn(agg_col_idx).GetType();

        switch (agg_types[i]) {
          case AggregationType::CountStarAggregate:
          case AggregationType::CountAggregate:
            out_vals[i] = Value(agg_type_id, 0);
            break;
          case AggregationType::SumAggregate:
          case AggregationType::MinAggregate:
          case AggregationType::MaxAggregate:
            out_vals[i] = Value(agg_type_id);  // NULL
            break;
          default:
            out_vals[i] = Value(agg_type_id);
            break;
        }
      }

      st.results.emplace_back(std::move(out_vals));
    }
    return;
  }

  for (auto &kv : groups) {
    auto &g = kv.second;
    std::vector<Value> out_vals;
    out_vals.reserve(g.group_vals.size() + g.agg_vals.size());

    for (auto &v : g.group_vals) out_vals.push_back(v);
    for (auto &v : g.agg_vals) out_vals.push_back(v);

    st.results.emplace_back(std::move(out_vals));
  }
}

auto AggregationExecutor::Next(Tuple *tuple, RID *rid) -> bool {
  auto &st = g_state[this];
  if (st.cursor >= st.results.size()) {
    return false;
  }
  if (tuple != nullptr) {
    *tuple = st.results[st.cursor];
  }
  if (rid != nullptr) {
    *rid = RID{};
  }
  st.cursor++;
  return true;
}

}  // namespace onebase