#include "mos/vis/control/control_client.h"

#include <gtest/gtest.h>

namespace mos::vis {
namespace {

TEST(ControlClientTest, StubClientEchoesPayload) {
  BeastControlClient client(true);

  nlohmann::json payload = {
      {"device_id", "d1"},
      {"action", "turn_on"},
      {"slots", {{"room", "kitchen"}}},
  };

  nlohmann::json response = client.SendJsonAsync(payload).get();
  EXPECT_TRUE(response.value("ok", false));
  EXPECT_EQ(response.at("echo"), payload);
}

}  // namespace
}  // namespace mos::vis
