cmake_minimum_required(VERSION 3.21)
project(vision_station LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(VS_SANITIZE "Build with ASan + UBSan" OFF)
option(VS_BUILD_GUI "Build the Qt station GUI" ON)
option(VS_BUILD_TESTS "Build unit tests" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()
if(VS_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
# OpenCV 5 split calib3d into geometry + calib (+ chessboard detection in objdetect) (MSYS2 ships 5.x, Ubuntu 24.04 ships 4.6).
find_package(OpenCV REQUIRED)
if(OpenCV_VERSION VERSION_GREATER_EQUAL 5)
  find_package(OpenCV REQUIRED COMPONENTS core imgproc geometry calib objdetect imgcodecs)
else()
  find_package(OpenCV REQUIRED COMPONENTS core imgproc calib3d imgcodecs)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(ARAVIS REQUIRED IMPORTED_TARGET aravis-0.8)

# ---- core: pure C++, no Qt, no Aravis -------------------------------------
add_library(vs_core STATIC
  src/core/station_fsm.cpp
  src/core/edge_measure.cpp
  src/core/scene.cpp
  src/core/stage_model.cpp
  src/core/stage_protocol.cpp
  src/core/net.cpp
  src/core/rt.cpp
  src/core/calibration.cpp
  $<$<BOOL:${UNIX}>:src/core/shm_ring.cpp>)
target_include_directories(vs_core PUBLIC src)
target_link_libraries(vs_core PUBLIC Threads::Threads ${OpenCV_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(vs_core PUBLIC rt)
endif()
if(WIN32)
  target_link_libraries(vs_core PUBLIC ws2_32)
endif()

# ---- camera + pipeline: Aravis (GigE Vision) ------------------------------
add_library(vs_pipeline STATIC
  src/camera/aravis_camera.cpp
  src/pipeline/stage_client.cpp
  src/pipeline/pipeline.cpp)
target_link_libraries(vs_pipeline PUBLIC vs_core PkgConfig::ARAVIS)

# ---- apps ------------------------------------------------------------------
add_executable(vs_bench apps/bench/main.cpp)
target_link_libraries(vs_bench PRIVATE vs_pipeline)

add_executable(vs_calibrate apps/calibrate/main.cpp)
target_link_libraries(vs_calibrate PRIVATE vs_core)

if(UNIX)
  # The simulated rig (fake GigE Vision camera + stage controller) and the
  # shared-memory recorder are Linux-only: POSIX shm, and the fake camera
  # hack in apps/rig is pinned to the Aravis 0.8.30 buffer layout.
  add_executable(vs_rig apps/rig/main.cpp)
  target_link_libraries(vs_rig PRIVATE vs_core PkgConfig::ARAVIS)
  add_executable(vs_recorder apps/recorder/main.cpp)
  target_link_libraries(vs_recorder PRIVATE vs_core)
endif()

if(VS_BUILD_GUI)
  find_package(Qt6 COMPONENTS Widgets)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    add_executable(vs_station apps/station/main.cpp apps/station/main_window.cpp apps/station/live_view.cpp)
    target_link_libraries(vs_station PRIVATE vs_pipeline Qt6::Widgets)
  else()
    message(WARNING "Qt6 Widgets not found: skipping vs_station GUI")
  endif()
endif()

if(VS_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(vs_tests
    tests/test_fsm.cpp
    tests/test_edge.cpp
    tests/test_queue.cpp
    tests/test_protocol.cpp
    tests/test_stage_model.cpp
    tests/test_calibration.cpp
    tests/test_uml_sync.cpp
    $<$<BOOL:${UNIX}>:tests/test_shm.cpp>)
  target_link_libraries(vs_tests PRIVATE vs_core GTest::gtest_main)
  target_compile_definitions(vs_tests PRIVATE VS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  include(GoogleTest)
  gtest_discover_tests(vs_tests DISCOVERY_TIMEOUT 60)
endif()
