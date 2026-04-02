#include "mos/vis/pipeline/pipeline_router.h"

#include <gtest/gtest.h>

namespace mos::vis {
namespace {

TEST(PipelineRouterTest, RoutesWakeupIntent) {
  PipelineRouter router;
  IntentResult intent;
  intent.intent = IntentType::kWakeup;
  EXPECT_EQ(router.Route(intent), "wakeup");
}

TEST(PipelineRouterTest, RoutesDeviceControlIntent) {
  PipelineRouter router;
  IntentResult intent;
  intent.intent = IntentType::kDeviceControl;
  EXPECT_EQ(router.Route(intent), "control");
}

TEST(PipelineRouterTest, RoutesUnknownToRecognition) {
  PipelineRouter router;
  IntentResult intent;
  intent.intent = IntentType::kUnknown;
  EXPECT_EQ(router.Route(intent), "recognition");
}

}  // namespace
}  // namespace mos::vis
