#include "mos/vis/control/control_client.h"

#include <future>

#include <spdlog/spdlog.h>

namespace mos::vis {

BeastControlClient::BeastControlClient(bool enable_stub_mode)
    : enable_stub_mode_(enable_stub_mode) {}

std::future<nlohmann::json> BeastControlClient::SendJsonAsync(
    const nlohmann::json& payload) {
  return std::async(std::launch::async, [payload, enabled = enable_stub_mode_]() {
    if (enabled) {
      nlohmann::json response;
      response["ok"] = true;
      response["echo"] = payload;
      return response;
    }

    // TODO: Replace with real Boost.Beast websocket async request/response.
    spdlog::warn("Non-stub control client mode is not implemented yet");
    return nlohmann::json{{"ok", false}, {"error", "not_implemented"}};
  });
}

}  // namespace mos::vis
