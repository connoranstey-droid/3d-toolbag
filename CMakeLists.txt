cmake_minimum_required(VERSION 3.21)
project(Facet VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(FACET_BUILD_APP "Build the Qt application" ON)
option(FACET_BUILD_TESTS "Build unit tests" ON)

find_package(Threads REQUIRED)

# ---- geometry core: no Qt dependency ----
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# The geometry core is a shared library so the app and every plugin use one copy of it
# (one mesh version counter, one set of caches).
add_library(facet_core SHARED
  src/mesh/Mesh.cpp
  src/mesh/Primitives.cpp
  src/mesh/Subdivision.cpp
  src/mesh/MeshOps.cpp
  src/mesh/BevelBridge.cpp
  src/mesh/UVOps.cpp
  src/mesh/BVH.cpp
  src/mesh/Sculpt.cpp
  src/mesh/ObjIO.cpp
)
target_include_directories(facet_core PUBLIC src)
set_target_properties(facet_core PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
target_link_libraries(facet_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(facet_core PRIVATE /W3)
else()
  target_compile_options(facet_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(FACET_BUILD_TESTS)
  enable_testing()
  add_executable(facet_tests tests/test_mesh.cpp)
  target_link_libraries(facet_tests PRIVATE facet_core)
  add_test(NAME facet_tests COMMAND facet_tests)
endif()

if(FACET_BUILD_APP)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
  set(CMAKE_AUTOMOC ON)
  file(GLOB FACET_APP_SOURCES CONFIGURE_DEPENDS
    src/io/*.cpp src/scene/*.cpp src/render/*.cpp src/viewport/*.cpp src/ui/*.cpp)
  file(GLOB FACET_APP_HEADERS CONFIGURE_DEPENDS
    src/io/*.h src/scene/*.h src/render/*.h src/viewport/*.h src/ui/*.h)
  add_library(facet_app STATIC ${FACET_APP_SOURCES} ${FACET_APP_HEADERS})
  target_link_libraries(facet_app PUBLIC facet_core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
  qt_add_executable(Facet MACOSX_BUNDLE src/main.cpp)
  # Plugins land in <build>/plugins; the app also searches next to itself and FACET_PLUGIN_PATH.
  set(FACET_PLUGIN_DIR ${CMAKE_BINARY_DIR}/plugins)
  target_compile_definitions(facet_app PRIVATE FACET_BUILD_PLUGIN_DIR="${FACET_PLUGIN_DIR}")
  add_library(facet_smart_unwrap MODULE
    plugins/smart_unwrap/SmartUnwrap.cpp
    plugins/smart_unwrap/SmartUnwrapPlugin.cpp
    plugins/smart_unwrap/SmartUnwrap.h
    plugins/smart_unwrap/SmartUnwrapPlugin.h)
  target_include_directories(facet_smart_unwrap PRIVATE plugins/smart_unwrap)
  target_link_libraries(facet_smart_unwrap PRIVATE facet_core Qt6::Core)
  set_target_properties(facet_smart_unwrap PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${FACET_PLUGIN_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${FACET_PLUGIN_DIR}
    AUTOMOC ON)
  add_dependencies(Facet facet_smart_unwrap)
  target_link_libraries(Facet PRIVATE facet_app)
  set_target_properties(Facet PROPERTIES WIN32_EXECUTABLE ON)

  if(FACET_BUILD_TESTS)
    find_package(Qt6 COMPONENTS Test)
    if(Qt6Test_FOUND)
      # Drives the real widgets with synthesized input; needs a display (use xvfb-run on CI).
      add_executable(facet_uitests tests/test_ui.cpp)
      target_link_libraries(facet_uitests PRIVATE facet_app Qt6::Test)
      add_executable(facet_iotests tests/test_io.cpp)
      target_link_libraries(facet_iotests PRIVATE facet_app)
      add_executable(facet_plugintests tests/test_plugin.cpp)
      target_link_libraries(facet_plugintests PRIVATE facet_app)
      target_compile_definitions(facet_plugintests PRIVATE FACET_BUILD_PLUGIN_DIR="${FACET_PLUGIN_DIR}")
      add_dependencies(facet_plugintests facet_smart_unwrap)
    endif()
  endif()
endif()
