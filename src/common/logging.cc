
#include "mos/vis/common/logging.h"

#include <memory>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace mos::vis {
namespace {
std::shared_ptr<spdlog::logger> g_logger;
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

std::shared_ptr<spdlog::logger> GetLogger() {
  if (g_logger == nullptr) {
    InitializeLogging(false);
  }
  return g_logger;
}

}  // namespace mos::vis
