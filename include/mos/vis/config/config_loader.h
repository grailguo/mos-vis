#pragma once

#include <string>

#include "mos/vis/config/app_config.h"

namespace mos::vis {

class ConfigLoader {
 public:
  static AppConfig LoadFromFile(const std::string& path);
};

std::string ResolveConfigPath(int argc, char** argv, bool is_release_build);

}  // namespace mos::vis
