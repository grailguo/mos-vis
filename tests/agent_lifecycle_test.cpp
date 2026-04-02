#include "mos/vis/core/voice_interactive_agent.h"

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

namespace mos::vis {
namespace {

TEST(AgentLifecycleTest, InitializeStartStopLifecycleWorks) {
  AppConfig config;
  VoiceInteractiveAgent agent(config);

  EXPECT_TRUE(agent.Initialize());
  EXPECT_TRUE(agent.Start());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(agent.IsRunning());

  agent.Stop();
  EXPECT_FALSE(agent.IsRunning());
}

TEST(AgentLifecycleTest, StartWithoutInitializeFails) {
  AppConfig config;
  VoiceInteractiveAgent agent(config);
  EXPECT_FALSE(agent.Start());
}

TEST(AgentLifecycleTest, ReloadConfigWhileRunningDoesNotStopAgent) {
  AppConfig config;
  config.device_id = "device-before";

  VoiceInteractiveAgent agent(config);
  ASSERT_TRUE(agent.Initialize());
  ASSERT_TRUE(agent.Start());

  AppConfig reloaded = config;
  reloaded.device_id = "device-after";
  reloaded.log_level = "debug";
  agent.ReloadConfig(reloaded);

  EXPECT_TRUE(agent.IsRunning());
  agent.Stop();
}

}  // namespace
}  // namespace mos::vis
