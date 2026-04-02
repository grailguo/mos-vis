cmake_minimum_required(VERSION 3.20)

project(mos_vis VERSION 0.1.0 LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
enable_testing()

option(MOS_VIS_BUILD_TESTS "Build mos-vis tests" ON)

find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Boost QUIET COMPONENTS system)
find_package(PortAudio QUIET)

# Optional RKNN runtime from fixed system paths.
if(EXISTS "/usr/include/rknn_api.h" AND EXISTS "/usr/lib/librknnrt.so")
  add_library(rknnrt SHARED IMPORTED)
  set_target_properties(rknnrt PROPERTIES
    IMPORTED_LOCATION "/usr/lib/librknnrt.so"
    INTERFACE_INCLUDE_DIRECTORIES "/usr/include")
  set(MOS_VIS_HAS_RKNN ON)
else()
  set(MOS_VIS_HAS_RKNN OFF)
endif()

# Optional sherpa-onnx C++ API from project-local 3rdparty.
set(SHERPA_ONNX_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/3rdparty/include")
set(SHERPA_ONNX_LIB "${PROJECT_SOURCE_DIR}/3rdparty/lib/libsherpa-onnx-cxx-api.so")
if(EXISTS "${SHERPA_ONNX_INCLUDE_DIR}/sherpa-onnx/c-api/cxx-api.h" AND EXISTS "${SHERPA_ONNX_LIB}")
  add_library(sherpa_onnx_cxx_api SHARED IMPORTED)
  set_target_properties(sherpa_onnx_cxx_api PROPERTIES
    IMPORTED_LOCATION "${SHERPA_ONNX_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${SHERPA_ONNX_INCLUDE_DIR}")
  set(MOS_VIS_HAS_SHERPA_ONNX ON)
else()
  set(MOS_VIS_HAS_SHERPA_ONNX OFF)
endif()

add_subdirectory(src)

if(MOS_VIS_BUILD_TESTS)
  add_subdirectory(tests)
endif()

install(FILES config/config.json DESTINATION /etc/mos_vis)
install(FILES packaging/mos-vis.service DESTINATION /etc/systemd/system)
