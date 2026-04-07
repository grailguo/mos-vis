
cmake_minimum_required(VERSION 3.16)
project(mos_vis LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MOS_VIS_BUILD_SERVER "Build mos_vis_server" ON)

find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

add_library(mos_vis_lib
  src/common/status.cc
  src/common/logging.cc
  src/config/app_config.cc
  src/control/control_engine_simple.cc
  src/audio/audio_ring_buffer.cc
  src/audio/audio_device_selector.cc
  src/audio/audio_capture.cc
  src/audio/audio_playback_portaudio.cc
  src/asr/asr_engine_sherpa.cc
  src/kws/kws_engine_sherpa.cc
  src/tts/tts_engine_sherpa.cc
  src/nlu/nlu_engine_simple.cc
  src/runtime/session_controller.cc
  src/agent/voice_interactive_agent.cc
  src/vad/vad_engine_rknn.cc
)

target_include_directories(mos_vis_lib
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PORTAUDIO_INCLUDE_DIRS}
)

target_link_libraries(mos_vis_lib
  PUBLIC
    Threads::Threads
    Boost::boost
    Boost::system
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    ${PORTAUDIO_LIBRARIES}
)

target_compile_options(mos_vis_lib PRIVATE ${PORTAUDIO_CFLAGS_OTHER})
target_compile_definitions(mos_vis_lib PUBLIC SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)

set(MOS_VIS_RKNN_HEADER "/usr/include/rknn_api.h")
set(MOS_VIS_RKNN_LIB "/usr/lib/librknnrt.so")
if(EXISTS "${MOS_VIS_RKNN_HEADER}" AND EXISTS "${MOS_VIS_RKNN_LIB}")
  message(STATUS "Using RKNN runtime: ${MOS_VIS_RKNN_LIB}")
  target_include_directories(mos_vis_lib PUBLIC "/usr/include")
  target_link_libraries(mos_vis_lib PUBLIC "${MOS_VIS_RKNN_LIB}")
  target_compile_definitions(mos_vis_lib PUBLIC MOS_VIS_HAS_RKNNRT=1)
endif()

set(MOS_VIS_SHERPA_ROOT "${PROJECT_SOURCE_DIR}/3rdparty")
set(MOS_VIS_SHERPA_INCLUDE_DIR "${MOS_VIS_SHERPA_ROOT}/include")
set(MOS_VIS_SHERPA_LIB_DIR "${MOS_VIS_SHERPA_ROOT}/lib")
set(MOS_VIS_SHERPA_C_API_HEADER
    "${MOS_VIS_SHERPA_INCLUDE_DIR}/sherpa-onnx/c-api/c-api.h")
set(MOS_VIS_SHERPA_C_API_LIB
    "${MOS_VIS_SHERPA_LIB_DIR}/libsherpa-onnx-c-api.so")
if(EXISTS "${MOS_VIS_SHERPA_C_API_HEADER}" AND EXISTS "${MOS_VIS_SHERPA_C_API_LIB}")
  message(STATUS "Using bundled sherpa-onnx from ${MOS_VIS_SHERPA_ROOT}")
  target_include_directories(mos_vis_lib PUBLIC "${MOS_VIS_SHERPA_INCLUDE_DIR}")
  target_link_libraries(mos_vis_lib PUBLIC "${MOS_VIS_SHERPA_C_API_LIB}")
  target_compile_definitions(mos_vis_lib PUBLIC MOS_VIS_HAS_SHERPA_ONNX=1)
endif()

if(MOS_VIS_BUILD_SERVER)
  add_executable(mos_vis_server src/server/main.cc)
  target_link_libraries(mos_vis_server PRIVATE mos_vis_lib)
endif()
