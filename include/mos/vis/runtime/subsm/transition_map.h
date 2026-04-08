#pragma once

#include <array>
#include <cstddef>
#include <optional>

namespace mos::vis::subsm {

template <typename StateT, typename EventT, typename GuardT, typename ActionT>
struct Transition {
  StateT from;
  EventT event;
  GuardT guard;
  StateT to;
  ActionT action;
};

template <typename StateT, typename EventT, typename GuardT, typename ActionT, std::size_t N,
          typename GuardEval>
std::optional<Transition<StateT, EventT, GuardT, ActionT>> MatchTransition(
    const std::array<Transition<StateT, EventT, GuardT, ActionT>, N>& table,
    StateT current_state,
    EventT event,
    GuardEval guard_eval) {
  for (const auto& row : table) {
    if (row.from != current_state || row.event != event) {
      continue;
    }
    if (guard_eval(row.guard)) {
      return row;
    }
  }
  return std::nullopt;
}

}  // namespace mos::vis::subsm
