#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <functional>

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

// ============================================================================
// Extended transition table support for v3 state machine with priority
// and global (kAny) transition support.
// ============================================================================

/**
 * @brief Extended transition with priority and global matching support.
 *
 * Adds priority field for ordering (higher priority matches first) and
 * is_global flag for transitions that match from any state (kAny).
 */
template <typename StateT, typename EventT, typename GuardT, typename ActionT>
struct ExtendedTransition : public Transition<StateT, EventT, GuardT, ActionT> {
  constexpr ExtendedTransition(StateT from, EventT event, GuardT guard, StateT to, ActionT action,
                               int priority = 0, bool is_global = false)
      : Transition<StateT, EventT, GuardT, ActionT>{from, event, guard, to, action},
        priority(priority),
        is_global(is_global) {}

  int priority = 0;           // Higher priority matches first
  bool is_global = false;     // If true, matches from any state (kAny)
};

/**
 * @brief Match transition with priority and global (kAny) support.
 *
 * Matching order:
 * 1. Global transitions (is_global = true) with highest priority
 * 2. State-specific transitions with highest priority
 * 3. Within same priority, first match in table order
 *
 * @param table Array of ExtendedTransition rows
 * @param current_state Current state of the state machine
 * @param event Incoming event to match
 * @param guard_eval Callable that evaluates guard conditions
 * @return Matched transition or std::nullopt if no match
 */
template <typename StateT, typename EventT, typename GuardT, typename ActionT, size_t N,
          typename GuardEval>
std::optional<ExtendedTransition<StateT, EventT, GuardT, ActionT>>
MatchTransitionWithPriority(
    const std::array<ExtendedTransition<StateT, EventT, GuardT, ActionT>, N>& table,
    StateT current_state,
    EventT event,
    GuardEval&& guard_eval) {
  std::optional<ExtendedTransition<StateT, EventT, GuardT, ActionT>> best_match;
  int best_priority = -1;

  for (const auto& row : table) {
    // Check if this row matches the event
    if (row.event != event) {
      continue;
    }

    // Check if this row matches the current state (or is global)
    bool state_matches = false;
    if (row.is_global) {
      // Global transitions match any state
      state_matches = true;
    } else if (row.from == current_state) {
      // State-specific transition matches
      state_matches = true;
    }

    if (!state_matches) {
      continue;
    }

    // Evaluate guard
    if (!guard_eval(row.guard)) {
      continue;
    }

    // Select this transition if it has higher priority than current best
    if (row.priority > best_priority) {
      best_match = row;
      best_priority = row.priority;
    }
  }

  return best_match;
}

}  // namespace mos::vis::subsm
