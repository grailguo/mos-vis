#include "mos/vis/util/ring_buffer.h"

#include <array>

#include <gtest/gtest.h>

namespace mos::vis {
namespace {

TEST(RingBufferTest, PushAndPopMaintainsOrder) {
  RingBuffer<float> buffer(4);

  const std::array<float, 3> input = {1.0f, 2.0f, 3.0f};
  EXPECT_EQ(buffer.Push(input.data(), input.size()), 3u);

  std::array<float, 3> out{};
  EXPECT_EQ(buffer.Pop(out.data(), out.size()), 3u);
  EXPECT_EQ(out[0], 1.0f);
  EXPECT_EQ(out[1], 2.0f);
  EXPECT_EQ(out[2], 3.0f);
}

TEST(RingBufferTest, PushStopsAtCapacity) {
  RingBuffer<float> buffer(2);

  const std::array<float, 3> input = {1.0f, 2.0f, 3.0f};
  EXPECT_EQ(buffer.Push(input.data(), input.size()), 2u);
  EXPECT_EQ(buffer.Size(), 2u);
}

TEST(RingBufferTest, PopReturnsAvailableOnly) {
  RingBuffer<float> buffer(4);

  const std::array<float, 2> input = {5.0f, 6.0f};
  EXPECT_EQ(buffer.Push(input.data(), input.size()), 2u);

  std::array<float, 3> out{};
  EXPECT_EQ(buffer.Pop(out.data(), out.size()), 2u);
  EXPECT_EQ(buffer.Size(), 0u);
}

}  // namespace
}  // namespace mos::vis
