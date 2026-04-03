
#pragma once

#include <memory>
#include <spdlog/logger.h>

namespace mos::vis {

void InitializeLogging(bool verbose);
std::shared_ptr<spdlog::logger> GetLogger();

}  // namespace mos::vis
