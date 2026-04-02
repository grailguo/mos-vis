#include "mos/vis/config/config_loader.h"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace mos::vis {
namespace {

TEST(ConfigLoaderTest, LoadFromFileParsesKnownFields) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "mos_vis_config_test.json";

  std::ofstream output(path);
  output << R"({
    "log_level": "debug",
    "control_url": "ws://localhost:9999/ws",
    "device_id": "device-a",
    "sample_rate_hz": 48000,
    "audio_channels": 2,
    "ring_buffer_capacity": 4096,
    "kws": {
      "pre_roll_ms": 50
    }
  })";
  output.close();

  const AppConfig config = ConfigLoader::LoadFromFile(path.string());
  EXPECT_EQ(config.log_level, "debug");
  EXPECT_EQ(config.control_url, "ws://localhost:9999/ws");
  EXPECT_EQ(config.device_id, "device-a");
  EXPECT_EQ(config.sample_rate_hz, 48000u);
  EXPECT_EQ(config.audio_channels, 2u);
  EXPECT_EQ(config.ring_buffer_capacity, 4096u);

  std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, LoadFromFileParsesNestedSchemaAndRag) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "mos_vis_nested_config_test.json";

  std::ofstream output(path);
  output << R"({
    "ring_buffer_capacity": 32768,
    "audio": {
      "sample_rate_hz": 16000,
      "audio_channels": 1
    },
    "kws": {
      "pre_roll_ms": 600
    },
    "control": {
      "url": "ws://127.0.0.1:9001/api"
    },
    "logging": {
      "level": "debug",
      "structured": true
    },
    "rag": {
      "enabled": true,
      "top_k": 4,
      "score_threshold": 0.35,
      "max_context_tokens": 1200,
      "request_timeout_ms": 800,
      "index_path": "data/rag/index.bin",
      "cache_ttl_s": 60
    }
  })";
  output.close();

  const AppConfig config = ConfigLoader::LoadFromFile(path.string());
  EXPECT_EQ(config.logging.level, "debug");
  EXPECT_TRUE(config.logging.structured);
  EXPECT_EQ(config.control.url, "ws://127.0.0.1:9001/api");
  EXPECT_EQ(config.audio.sample_rate_hz, 16000u);
  EXPECT_EQ(config.audio.audio_channels, 1u);
  EXPECT_EQ(config.kws.pre_roll_ms, 600u);
  EXPECT_TRUE(config.rag.enabled);
  EXPECT_EQ(config.rag.top_k, 4u);
  EXPECT_EQ(config.rag.max_context_tokens, 1200u);
  EXPECT_EQ(config.rag.request_timeout_ms, 800u);
  EXPECT_EQ(config.rag.index_path, "data/rag/index.bin");
  EXPECT_EQ(config.rag.cache_ttl_s, 60u);
  EXPECT_FLOAT_EQ(config.rag.score_threshold, 0.35f);

  std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, LoadFromFileParsesLatestRingBufferAndWakeAckTextSchema) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "mos_vis_latest_schema_test.json";

  std::ofstream output(path);
  output << R"({
    "audio": {
      "sample_rate_hz": 16000,
      "audio_channels": 1
    },
    "kws": {
      "pre_roll_ms": 600
    },
    "ring_buffer": {
      "frame_capacity": 32768,
      "sample_capacity": 32768
    },
    "wake_ack_text": [
      {
        "keywords": ["0801", "小G"],
        "reply_text": "收到，请说。",
        "preset_file": ""
      },
      {
        "keywords": ["小莫"],
        "reply_text": "我在，请讲。",
        "preset_file": "voice/preset_xiaomo.wav"
      }
    ]
  })";
  output.close();

  const AppConfig config = ConfigLoader::LoadFromFile(path.string());
  EXPECT_EQ(config.ring_buffer.frame_capacity, 32768u);
  EXPECT_EQ(config.ring_buffer.sample_capacity, 32768u);
  EXPECT_EQ(config.ring_buffer_capacity, 32768u);

  ASSERT_EQ(config.wake_ack_text.size(), 2u);
  EXPECT_EQ(config.wake_ack_text[0].keywords.size(), 2u);
  EXPECT_EQ(config.wake_ack_text[0].keywords[0], "0801");
  EXPECT_EQ(config.wake_ack_text[0].reply_text, "收到，请说。");
  EXPECT_EQ(config.wake_ack_text[1].preset_file, "voice/preset_xiaomo.wav");

  std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, LoadFromFileRejectsTooSmallRingBufferForKwsPreRoll) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "mos_vis_invalid_ring_buffer_test.json";

  std::ofstream output(path);
  output << R"({
    "ring_buffer_capacity": 8192,
    "audio": {
      "sample_rate_hz": 16000,
      "audio_channels": 1
    },
    "kws": {
      "pre_roll_ms": 600
    }
  })";
  output.close();

  EXPECT_THROW((void)ConfigLoader::LoadFromFile(path.string()), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(ConfigLoaderTest, ResolveConfigPathHonorsCliOverride) {
  const char* argv[] = {"mos_vis_server", "--config", "/tmp/x.json"};
  EXPECT_EQ(ResolveConfigPath(3, const_cast<char**>(argv), false), "/tmp/x.json");
}

TEST(ConfigLoaderTest, ResolveConfigPathUsesDebugDefaultWhenNotRelease) {
  const char* argv[] = {"mos_vis_server"};
  const std::string path = ResolveConfigPath(1, const_cast<char**>(argv), false);
  EXPECT_TRUE(path.size() >= std::string("config.json").size());
  EXPECT_EQ(path.substr(path.size() - std::string("config.json").size()), "config.json");
}

TEST(ConfigLoaderTest, ResolveConfigPathUsesReleaseDefaultWhenRelease) {
  const char* argv[] = {"mos_vis_server"};
  EXPECT_EQ(ResolveConfigPath(1, const_cast<char**>(argv), true), "/etc/mos_vis/config.json");
}

}  // namespace
}  // namespace mos::vis
