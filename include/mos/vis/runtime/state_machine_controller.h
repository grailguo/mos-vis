#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <chrono>

#include "mos/vis/runtime/session_state.h"
#include "mos/vis/runtime/vis_event.h"
#include "mos/vis/runtime/vis_guard_action.h"
#include "mos/vis/runtime/subsm/transition_map.h"
#include "mos/vis/runtime/subsm/hotspot_subsms.h"

namespace mos::vis {

// Forward declaration to avoid circular dependency
class SessionContext;

/**
 * @brief Centralized state machine controller for v3 architecture.
 *
 * Manages 16-state business logic with table-driven transitions, unified
 * event processing, and centralized timeout management. Integrates with
 * existing sub-state machines for hotspot complexity.
 */
class StateMachineController {
 public:
  explicit StateMachineController(SessionContext& context);
  ~StateMachineController() = default;

  // Prevent copying and moving
  StateMachineController(const StateMachineController&) = delete;
  StateMachineController& operator=(const StateMachineController&) = delete;
  StateMachineController(StateMachineController&&) = delete;
  StateMachineController& operator=(StateMachineController&&) = delete;

  // === Core State Machine Operations ===

  /**
   * @brief Process a single event synchronously.
   *
   * Matches event against transition table, evaluates guards, executes actions,
   * and updates state. This method is thread-safe.
   */
  void ProcessEvent(const VisEvent& event);

  /**
   * @brief Queue an event for asynchronous processing.
   *
   * Events are added to a queue and processed during the next Tick() call.
   * Use this for events generated from non-state-machine threads.
   */
  void QueueEvent(const VisEvent& event);

  /**
   * @brief Process all queued events.
   *
   * Called from the main pipeline tick to process events queued by stages.
   */
  void Tick();

  /**
   * @brief Get the current state of the state machine.
   */
  SessionState current_state() const;

  /**
   * @brief Get the name of the current state as a string.
   */
  std::string current_state_name() const;

  // === Timeout Management ===

  enum class TimeoutType {
    kCommandWait,      // 15-second command waiting timeout
    kAsrRecognition,   // ASR recognition timeout
    kControlSync,      // Synchronous control acknowledgment timeout
    kControlAsync,     // Asynchronous control completion timeout
    kNluProcessing,    // NLU processing timeout
    kRagQuery,         // RAG query timeout
    kLlmChat,          // LLM chat timeout
  };

  /**
   * @brief Start a timeout timer.
   *
   * @param type Type of timeout
   * @param duration_ms Timeout duration in milliseconds
   */
  void StartTimeout(TimeoutType type, int64_t duration_ms);

  /**
   * @brief Cancel a pending timeout.
   */
  void CancelTimeout(TimeoutType type);

  /**
   * @brief Check for expired timeouts and trigger timeout events.
   *
   * Called from Tick() to handle timeout expirations.
   */
  void CheckTimeouts();

  // === Debug and Monitoring ===

  /**
   * @brief Get the number of pending events in the queue.
   */
  size_t pending_event_count() const;

  /**
   * @brief Get a snapshot of the current state machine status for debugging.
   */
  std::string GetStatusString() const;

  /**
   * @brief Reset the state machine to initial state (for testing/recovery).
   */
  void Reset();

 private:
  // === Transition Table Matching ===

  /**
   * @brief Evaluate a guard condition.
   *
   * @param guard Guard type to evaluate
   * @param event Triggering event
   * @return true if guard passes, false otherwise
   */
  bool EvaluateGuard(GuardType guard, const VisEvent& event);

  /**
   * @brief Execute an action.
   *
   * @param action Action type to execute
   * @param event Triggering event
   */
  void ExecuteAction(ActionType action, const VisEvent& event);

  /**
   * @brief Log a state transition for observability.
   */
  void LogTransition(SessionState from_state,
                     SessionState to_state,
                     const VisEvent& event,
                     GuardType guard,
                     ActionType action);

  // === Integration with Existing Sub-State Machines ===

  /**
   * @brief Convert sub-state machine events to VisEvents.
   *
   * Called when sub-state machines produce decisions that need to be
   * processed by the main state machine.
   */
  VisEvent ConvertSubsmEvent(const subsm::WakeDecision& decision);
  VisEvent ConvertSubsmEvent(const subsm::AsrDecision& decision);
  VisEvent ConvertSubsmEvent(const subsm::ReplyDecision& decision);

  // === Member Variables ===

  // Reference to the shared session context (not owned)
  SessionContext& context_;

  // Current state of the state machine
  SessionState current_state_;

  // Event queue for asynchronous processing
  std::deque<VisEvent> event_queue_;
  mutable std::mutex event_queue_mutex_;

  // Timeout tracking
  struct TimeoutInfo {
    TimeoutType type;
    std::chrono::steady_clock::time_point expiry;
  };
  std::vector<TimeoutInfo> active_timeouts_;
  mutable std::mutex timeouts_mutex_;

  // Thread safety for state transitions
  mutable std::mutex state_mutex_;

  // Configuration
  bool use_v3_state_machine_ = true;  // TODO: Make configurable
};

}  // namespace mos::vis