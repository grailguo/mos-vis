#pragma once

#include <future>

#include <nlohmann/json.hpp>

namespace mos::vis {

class ControlClient {
 public:
  virtual ~ControlClient() = default;
  virtual std::future<nlohmann::json> SendJsonAsync(const nlohmann::json& payload) = 0;
};

class BeastControlClient final : public ControlClient {
 public:
  explicit BeastControlClient(bool enable_stub_mode = true);

  std::future<nlohmann::json> SendJsonAsync(const nlohmann::json& payload) override;

 private:
  bool enable_stub_mode_ = true;
};

}  // namespace mos::vis
