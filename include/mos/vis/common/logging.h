
#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <spdlog/logger.h>
#include <spdlog/spdlog.h>

namespace mos::vis {

struct LogContext {
  std::string module;
  std::string session;
  std::uint64_t turn = 0;
  std::string state;
  std::string req;
};

struct LogField {
  std::string key;
  std::string value;
};

enum class EventScope {
  kSystem = 0,
  kSession,
  kTurn,
};

namespace logevent {
inline constexpr std::string_view kKwsHit = "kws_hit";
inline constexpr std::string_view kKwsReject = "kws_reject";
inline constexpr std::string_view kKwsTimeout = "kws_timeout";
inline constexpr std::string_view kAsrStart = "asr_start";
inline constexpr std::string_view kAsrPartial = "asr_partial";
inline constexpr std::string_view kAsrFinal = "asr_final";
inline constexpr std::string_view kAsrTimeout = "asr_timeout";
inline constexpr std::string_view kAsrError = "asr_error";
inline constexpr std::string_view kAsrFinalEmpty = "asr_final_empty";
inline constexpr std::string_view kIntentRoute = "intent_route";
inline constexpr std::string_view kNluError = "nlu_error";
inline constexpr std::string_view kNluTimeout = "nlu_timeout";
inline constexpr std::string_view kControlSend = "control_send";
inline constexpr std::string_view kControlAck = "control_ack";
inline constexpr std::string_view kControlAsyncWaitBegin = "control_async_wait_begin";
inline constexpr std::string_view kControlNotify = "control_notify";
inline constexpr std::string_view kControlDone = "control_done";
inline constexpr std::string_view kControlTimeout = "control_timeout";
inline constexpr std::string_view kControlStaleNotifyDrop = "control_stale_notify_drop";
inline constexpr std::string_view kControlTransportDisconnect = "control_transport_disconnect";
inline constexpr std::string_view kRagQuery = "rag_query";
inline constexpr std::string_view kRagResult = "rag_result";
inline constexpr std::string_view kRagTimeout = "rag_timeout";
inline constexpr std::string_view kLlmRequest = "llm_request";
inline constexpr std::string_view kLlmResult = "llm_result";
inline constexpr std::string_view kLlmTimeout = "llm_timeout";
inline constexpr std::string_view kTtsStart = "tts_start";
inline constexpr std::string_view kTtsDone = "tts_done";
inline constexpr std::string_view kTtsFail = "tts_fail";
inline constexpr std::string_view kTtsBargeIn = "tts_barge_in";
inline constexpr std::string_view kSessionStart = "session_start";
inline constexpr std::string_view kSessionEnd = "session_end";
inline constexpr std::string_view kStateTransition = "state_transition";
inline constexpr std::string_view kSubsmTransition = "subsm_transition";
inline constexpr std::string_view kSystemBoot = "system_boot";
inline constexpr std::string_view kAudioDeviceOpen = "audio_device_open";
inline constexpr std::string_view kAudioOverrun = "audio_overrun";
inline constexpr std::string_view kAudioSilenceTooLong = "audio_silence_too_long";
inline constexpr std::string_view kAudioSrMismatch = "audio_sr_mismatch";
inline constexpr std::string_view kKwsResetFailed = "kws_reset_failed";
inline constexpr std::string_view kControlRetry = "control_retry";
}  // namespace logevent

template <typename T>
inline std::string ToStringForLog(const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<T, const char*>) {
    return value == nullptr ? "" : std::string(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "1" : "0";
  } else {
    std::ostringstream oss;
    oss << value;
    return oss.str();
  }
}

template <typename T>
inline LogField Kv(std::string key, T value) {
  return LogField{std::move(key), ToStringForLog(value)};
}

EventScope ResolveEventScope(std::string_view event);
void LogKv(spdlog::level::level_enum level,
           std::string_view event,
           const LogContext& context,
           std::initializer_list<LogField> fields = {});
void LogKvRateLimited(spdlog::level::level_enum level,
                      std::string_view event,
                      const LogContext& context,
                      std::string_view rate_key,
                      std::chrono::milliseconds interval,
                      int every_n,
                      std::initializer_list<LogField> fields = {});

inline void LogInfo(std::string_view event,
                    const LogContext& context,
                    std::initializer_list<LogField> fields = {}) {
  LogKv(spdlog::level::info, event, context, fields);
}
inline void LogWarn(std::string_view event,
                    const LogContext& context,
                    std::initializer_list<LogField> fields = {}) {
  LogKv(spdlog::level::warn, event, context, fields);
}
inline void LogError(std::string_view event,
                     const LogContext& context,
                     std::initializer_list<LogField> fields = {}) {
  LogKv(spdlog::level::err, event, context, fields);
}
inline void LogDebug(std::string_view event,
                     const LogContext& context,
                     std::initializer_list<LogField> fields = {}) {
  LogKv(spdlog::level::debug, event, context, fields);
}

class ScopedTimer {
 public:
  ScopedTimer();
  std::int64_t ElapsedMs() const;

 private:
  std::chrono::steady_clock::time_point start_;
};

std::string MaskSummary(const std::string& text, std::size_t keep_prefix);
std::string BasenamePath(const std::string& path);
std::string TruncatePayload(const std::string& payload, std::size_t max_len);

void InitializeLogging(bool verbose);
std::shared_ptr<spdlog::logger> GetLogger();

}  // namespace mos::vis
