#pragma once

#include "tree_select.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace tree_select_internal {

inline void require(bool condition, const char *message) {
  if (!condition) {
    throw std::invalid_argument(std::string("tree_select: ") + message);
  }
}

inline std::pair<size_t, size_t> plan_row_and_cols(const LevelPlan &plan,
                                                   const TreePirParams &tree) {
  switch (plan.select_case) {
    case SelectCase::Single:
      return {tree.a, size_t{1}};
    case SelectCase::CoarsenedAlpha:
      return {plan.coarsen_count, plan.R};
    case SelectCase::AlphaBeta:
      return {size_t{0}, tree.N0};
  }
  throw std::invalid_argument("tree_select: unknown level plan case");
}

}
