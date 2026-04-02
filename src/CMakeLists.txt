set(MOS_VIS_CORE_SOURCES
  audio/port_audio_guard.cpp
  config/config_loader.cpp
  control/control_client.cpp
  core/voice_interactive_agent.cpp
  engines/stub_engines.cpp
  pipeline/pipeline_router.cpp
  util/logging.cpp)

add_library(mos_vis_core ${MOS_VIS_CORE_SOURCES})
add_library(mos::vis_core ALIAS mos_vis_core)

target_include_directories(mos_vis_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(mos_vis_core
  PUBLIC
    spdlog::spdlog
    nlohmann_json::nlohmann_json)

if(Boost_FOUND)
  target_link_libraries(mos_vis_core PUBLIC Boost::system)
  target_compile_definitions(mos_vis_core PUBLIC MOS_VIS_HAS_BOOST=1)
else()
  target_compile_definitions(mos_vis_core PUBLIC MOS_VIS_HAS_BOOST=0)
endif()

if(PortAudio_FOUND)
  target_link_libraries(mos_vis_core PUBLIC PortAudio::PortAudio)
  target_compile_definitions(mos_vis_core PUBLIC MOS_VIS_HAS_PORTAUDIO=1)
else()
  target_compile_definitions(mos_vis_core PUBLIC MOS_VIS_HAS_PORTAUDIO=0)
endif()

if(MOS_VIS_HAS_RKNN)
  target_link_libraries(mos_vis_core PUBLIC rknnrt)
  target_compile_definitions(mos_vis_core PUBLIC MOS_VIS_HAS_RKNN=1)
else()
  target_compile_definitions(mos_vis_core PUBLIC MOS_VIS_HAS_RKNN=0)
endif()

if(MOS_VIS_HAS_SHERPA_ONNX)
  target_link_libraries(mos_vis_core PUBLIC sherpa_onnx_cxx_api)
  target_compile_definitions(mos_vis_core PUBLIC MOS_VIS_HAS_SHERPA_ONNX=1)
else()
  target_compile_definitions(mos_vis_core PUBLIC MOS_VIS_HAS_SHERPA_ONNX=0)
endif()

add_executable(mos_vis_server
  server/main.cpp)

target_link_libraries(mos_vis_server
  PRIVATE
    mos_vis_core)

install(TARGETS mos_vis_core
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(TARGETS mos_vis_server
  RUNTIME DESTINATION /usr/local/bin)
