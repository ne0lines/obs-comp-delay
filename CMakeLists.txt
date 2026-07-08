cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(ENABLE_UNIT_TESTS "Build OBS-independent unit tests" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio-31.1.1/cmake/finders")
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swscale swresample)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample)

set(
  COMP_DELAY_FFMPEG_RUNTIME_DLLS
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-2025-07-11-x64/bin/avcodec-61.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-2025-07-11-x64/bin/avutil-59.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-2025-07-11-x64/bin/swscale-8.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-2025-07-11-x64/bin/swresample-5.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-2025-07-11-x64/bin/libx264-164.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps-2025-07-11-x64/bin/zlib.dll"
)

set(
  COMP_DELAY_OBS_TEST_RUNTIME_DLLS
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/bin/64bit/obs.dll"
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/bin/64bit/w32-pthreads.dll"
)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/codec/ffmpeg-video-codec.cpp
    src/core/delay-settings.cpp
    src/core/delay-state-machine.cpp
    src/core/encoded-ring-buffer.cpp
    src/obs/delay-controller.cpp
    src/obs/delay-settings-dialog.cpp
    src/obs/playback-source.cpp
    src/obs/scene-capture-encoder.cpp
    src/obs/scene-utils.cpp
    src/plugin-main.cpp
)

if(MSVC)
  target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE /permissive- /Zc:__cplusplus)
endif()

add_custom_command(
  TARGET ${CMAKE_PROJECT_NAME}
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${COMP_DELAY_FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${COMP_DELAY_FFMPEG_RUNTIME_DLLS} "${CMAKE_BINARY_DIR}/rundir/$<CONFIG>"
  VERBATIM
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(ENABLE_UNIT_TESTS)
  enable_testing()
  add_executable(comp-delay-core-tests)
  set_target_properties(comp-delay-core-tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_include_directories(comp-delay-core-tests PRIVATE src)
  target_sources(
    comp-delay-core-tests
    PRIVATE
      src/core/delay-settings.cpp
      src/core/delay-state-machine.cpp
      src/core/encoded-ring-buffer.cpp
      tests/core-tests.cpp
  )
  add_test(NAME comp-delay-core-tests COMMAND comp-delay-core-tests)

  add_executable(comp-delay-codec-tests)
  set_target_properties(comp-delay-codec-tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_include_directories(comp-delay-codec-tests PRIVATE src)
  target_sources(
    comp-delay-codec-tests
    PRIVATE
      src/codec/ffmpeg-video-codec.cpp
      src/core/delay-settings.cpp
      src/core/encoded-ring-buffer.cpp
      tests/codec-tests.cpp
  )
  target_link_libraries(comp-delay-codec-tests PRIVATE OBS::libobs FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale)
  add_custom_command(
    TARGET comp-delay-codec-tests
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${COMP_DELAY_FFMPEG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:comp-delay-codec-tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${COMP_DELAY_OBS_TEST_RUNTIME_DLLS} "$<TARGET_FILE_DIR:comp-delay-codec-tests>"
    VERBATIM
  )
  add_test(NAME comp-delay-codec-tests COMMAND comp-delay-codec-tests)
endif()
