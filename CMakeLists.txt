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
add_library(facet_core STATIC
  src/mesh/Mesh.cpp
  src/mesh/Primitives.cpp
  src/mesh/Subdivision.cpp
  src/mesh/MeshOps.cpp
  src/mesh/ObjIO.cpp
)
target_include_directories(facet_core PUBLIC src)
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
    src/scene/*.cpp src/render/*.cpp src/viewport/*.cpp src/ui/*.cpp)
  file(GLOB FACET_APP_HEADERS CONFIGURE_DEPENDS
    src/scene/*.h src/render/*.h src/viewport/*.h src/ui/*.h)
  qt_add_executable(Facet MACOSX_BUNDLE src/main.cpp ${FACET_APP_SOURCES} ${FACET_APP_HEADERS})
  target_link_libraries(Facet PRIVATE facet_core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
  set_target_properties(Facet PROPERTIES WIN32_EXECUTABLE ON)
endif()
