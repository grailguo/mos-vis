find_package(GTest REQUIRED)

add_executable(mos_vis_tests
  config_loader_test.cpp
  ring_buffer_test.cpp
  agent_lifecycle_test.cpp
  pipeline_router_test.cpp
  control_client_test.cpp)

target_link_libraries(mos_vis_tests
  PRIVATE
    mos_vis_core
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mos_vis_tests)
