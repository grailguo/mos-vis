#include "mos/vis/util/logging.h"

#include <spdlog/spdlog.h>

namespace mos::vis {

void InitializeLogging(const std::string& level) {
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  spdlog::set_level(spdlog::level::from_str(level));
  spdlog::flush_on(spdlog::level::info);
}

}  // namespace mos::vis
