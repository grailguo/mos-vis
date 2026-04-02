#include <csignal>
#include <cstdlib>
#include <iostream>

#include "mos/vis/config/config_loader.h"
#include "mos/vis/core/voice_interactive_agent.h"
#include "mos/vis/util/logging.h"

#include <spdlog/spdlog.h>

namespace {

bool IsReleaseBuild() {
#ifdef NDEBUG
  return true;
#else
  return false;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  sigset_t signal_set;
  sigemptyset(&signal_set);
  sigaddset(&signal_set, SIGINT);
  sigaddset(&signal_set, SIGTERM);
  sigaddset(&signal_set, SIGHUP);

  if (pthread_sigmask(SIG_BLOCK, &signal_set, nullptr) != 0) {
    std::cerr << "Failed to setup signal mask" << std::endl;
    return EXIT_FAILURE;
  }

  try {
    const std::string config_path = mos::vis::ResolveConfigPath(argc, argv, IsReleaseBuild());
    const mos::vis::AppConfig config = mos::vis::ConfigLoader::LoadFromFile(config_path);

    mos::vis::InitializeLogging(config.log_level);

    mos::vis::VoiceInteractiveAgent agent(config);
    if (!agent.Initialize()) {
      std::cerr << "Agent Initialize() failed" << std::endl;
      return EXIT_FAILURE;
    }

    if (!agent.Start()) {
      std::cerr << "Agent Start() failed" << std::endl;
      return EXIT_FAILURE;
    }

    for (;;) {
      int signal = 0;
      sigwait(&signal_set, &signal);

      if (signal == SIGHUP) {
        try {
          const mos::vis::AppConfig reloaded = mos::vis::ConfigLoader::LoadFromFile(config_path);
          mos::vis::InitializeLogging(reloaded.log_level);
          agent.ReloadConfig(reloaded);
          spdlog::info("Reloaded config from {}", config_path);
        } catch (const std::exception& ex) {
          spdlog::error("Config reload failed from {}: {}", config_path, ex.what());
        }
        continue;
      }

      if (signal == SIGINT || signal == SIGTERM) {
        break;
      }
    }

    agent.Stop();
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << "Fatal error: " << ex.what() << std::endl;
    return EXIT_FAILURE;
  }
}
