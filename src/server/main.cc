
#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

#include "mos/vis/agent/voice_interactive_agent.h"
#include "mos/vis/common/logging.h"
#include "mos/vis/config/app_config.h"

namespace {

std::string ResolveExecutableDir() {
  std::vector<char> buf(4096, '\0');
  const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
  if (n <= 0) {
    return "";
  }
  buf[static_cast<std::size_t>(n)] = '\0';
  const std::filesystem::path exe_path(buf.data());
  return exe_path.parent_path().string();
}

std::string ResolveConfigPath(int argc, char* argv[]) {
  if (argc > 1) {
    return argv[1];
  }

  const std::filesystem::path cwd_default("./config/config.json");
  if (std::filesystem::exists(cwd_default)) {
    return cwd_default.string();
  }

  const std::string exe_dir = ResolveExecutableDir();
  if (!exe_dir.empty()) {
    const std::filesystem::path repo_relative =
        std::filesystem::path(exe_dir) / ".." / "config" / "config.json";
    if (std::filesystem::exists(repo_relative)) {
      return repo_relative.lexically_normal().string();
    }
  }

  return cwd_default.string();
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::string config_path = ResolveConfigPath(argc, argv);

  mos::vis::InitializeLogging(true);
  std::vector<char> exe_buf(4096, '\0');
  const ssize_t exe_n = ::readlink("/proc/self/exe", exe_buf.data(), exe_buf.size() - 1);
  if (exe_n > 0) {
    exe_buf[static_cast<std::size_t>(exe_n)] = '\0';
    mos::vis::LogInfo(mos::vis::logevent::kSystemBoot,
                      mos::vis::LogContext{"ServerMain", "", 0, "", ""},
                      {mos::vis::Kv("exe", mos::vis::BasenamePath(exe_buf.data()))});
  }
  mos::vis::LogInfo(mos::vis::logevent::kSystemBoot,
                    mos::vis::LogContext{"ServerMain", "", 0, "", ""},
                    {mos::vis::Kv("config", mos::vis::BasenamePath(config_path))});

  mos::vis::AppConfig config;
  mos::vis::Status st = mos::vis::AppConfig::LoadFromFile(config_path, &config);
  if (!st.ok()) {
    std::cerr << "Load config failed: " << st.message() << "\n";
    return 1;
  }

  mos::vis::VoiceInteractiveAgent agent(config);
  st = agent.Initialize();
  if (!st.ok()) {
    std::cerr << "Initialize failed: " << st.message() << "\n";
    return 2;
  }

  st = agent.Run();
  if (!st.ok()) {
    std::cerr << "Run failed: " << st.message() << "\n";
    return 3;
  }

  return 0;
}
