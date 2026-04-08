
#include "mos/vis/common/logging.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <memory>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace mos::vis {
namespace {
std::shared_ptr<spdlog::logger> g_logger;

struct RateState {
  std::chrono::steady_clock::time_point last{};
  int suppressed = 0;
};

std::mutex g_rate_mutex;
std::unordered_map<std::string, RateState> g_rate_map;

const std::unordered_set<std::string_view> kSystemEvents = {
    logevent::kSystemBoot,
    logevent::kAudioDeviceOpen,
    logevent::kAudioOverrun,
    logevent::kAudioSilenceTooLong,
    logevent::kAudioSrMismatch,
    logevent::kControlTransportDisconnect,
};

const std::unordered_set<std::string_view> kSessionEvents = {
    logevent::kSessionStart,
    logevent::kSessionEnd,
    logevent::kStateTransition,
    logevent::kSubsmTransition,
};

const std::array<std::string, 3> kSensitiveKeys = {
    "token",
    "payload",
    "client_key",
};

bool IsSensitiveKey(const std::string& key) {
  return std::find(kSensitiveKeys.begin(), kSensitiveKeys.end(), key) != kSensitiveKeys.end();
}

std::size_t EffectiveMaskPrefix(std::size_t keep_prefix) {
  const auto logger = GetLogger();
  if (logger != nullptr && logger->level() <= spdlog::level::debug) {
    return std::max<std::size_t>(keep_prefix, 64U);
  }
  return keep_prefix;
}

std::size_t Utf8SafePrefixBytes(const std::string& text, std::size_t max_bytes) {
  if (max_bytes >= text.size()) {
    return text.size();
  }

  std::size_t end = max_bytes;
  while (end > 0U &&
         (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
    --end;
  }
  if (end == 0U) {
    return max_bytes;
  }
  return end;
}

bool ValidateContext(EventScope scope, const LogContext& context, std::string* reason) {
  if (context.module.empty()) {
    if (reason != nullptr) {
      *reason = "missing module";
    }
    return false;
  }
  if (scope == EventScope::kSession && context.session.empty()) {
    if (reason != nullptr) {
      *reason = "missing session";
    }
    return false;
  }
  if (scope == EventScope::kTurn && (context.session.empty() || context.turn == 0U)) {
    if (reason != nullptr) {
      *reason = context.session.empty() ? "missing session" : "missing turn";
    }
    return false;
  }
  return true;
}

std::string BuildLogLine(std::string_view event,
                         const LogContext& context,
                         std::initializer_list<LogField> fields) {
  std::string line = "module=" + context.module + " event=" + std::string(event);
  if (!context.session.empty()) {
    line += " session=" + context.session;
  }
  if (context.turn > 0U) {
    line += " turn=" + std::to_string(context.turn);
  }
  if (!context.state.empty()) {
    line += " state=" + context.state;
  }
  if (!context.req.empty()) {
    line += " req=" + context.req;
  }
  for (const auto& field : fields) {
    if (field.key.empty()) {
      continue;
    }
    const std::string value = IsSensitiveKey(field.key) ? "<redacted>" : field.value;
    line += " " + field.key + "=" + value;
  }
  return line;
}
}  // namespace

void InitializeLogging(bool verbose) {
  if (g_logger != nullptr) {
    return;
  }

  auto logger = spdlog::stdout_color_mt("mos_vis");
  logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  logger->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
  logger->flush_on(verbose ? spdlog::level::debug : spdlog::level::info);
  g_logger = logger;
}

EventScope ResolveEventScope(std::string_view event) {
  if (kSystemEvents.find(event) != kSystemEvents.end()) {
    return EventScope::kSystem;
  }
  if (kSessionEvents.find(event) != kSessionEvents.end()) {
    return EventScope::kSession;
  }
  return EventScope::kTurn;
}

void LogKv(spdlog::level::level_enum level,
           std::string_view event,
           const LogContext& context,
           std::initializer_list<LogField> fields) {
  const auto logger = GetLogger();
  std::string reason;
  if (!ValidateContext(ResolveEventScope(event), context, &reason)) {
    logger->warn("module=logging event=logging_context_invalid reason={} bad_event={} bad_module={}",
                 reason,
                 event,
                 context.module);
  }
  logger->log(level, "{}", BuildLogLine(event, context, fields));
}

void LogKvRateLimited(spdlog::level::level_enum level,
                      std::string_view event,
                      const LogContext& context,
                      std::string_view rate_key,
                      std::chrono::milliseconds interval,
                      int every_n,
                      std::initializer_list<LogField> fields) {
  if (every_n <= 0) {
    every_n = 1;
  }
  const auto now = std::chrono::steady_clock::now();
  const std::string key = context.module + ":" + std::string(event) + ":" + std::string(rate_key);

  {
    std::lock_guard<std::mutex> lock(g_rate_mutex);
    auto& state = g_rate_map[key];
    const auto elapsed = now - state.last;
    if (state.last.time_since_epoch().count() != 0 &&
        elapsed < interval &&
        ((state.suppressed + 1) % every_n != 0)) {
      ++state.suppressed;
      return;
    }
    state.last = now;
    state.suppressed = 0;
  }

  LogKv(level, event, context, fields);
}

ScopedTimer::ScopedTimer()
    : start_(std::chrono::steady_clock::now()) {}

std::int64_t ScopedTimer::ElapsedMs() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start_)
      .count();
}

std::string MaskSummary(const std::string& text, std::size_t keep_prefix) {
  const std::size_t effective_keep_prefix = EffectiveMaskPrefix(keep_prefix);
  if (text.size() <= effective_keep_prefix) {
    return text;
  }
  const std::size_t safe_prefix = Utf8SafePrefixBytes(text, effective_keep_prefix);
  return text.substr(0, safe_prefix) + "...";
}

std::string BasenamePath(const std::string& path) {
  if (path.empty()) {
    return "";
  }
  return std::filesystem::path(path).filename().string();
}

std::string TruncatePayload(const std::string& payload, std::size_t max_len) {
  if (payload.size() <= max_len) {
    return payload;
  }
  return payload.substr(0, max_len) + "...";
}

std::shared_ptr<spdlog::logger> GetLogger() {
  if (g_logger == nullptr) {
    InitializeLogging(false);
  }
  return g_logger;
}

}  // namespace mos::vis
