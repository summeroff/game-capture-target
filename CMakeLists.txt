cmake_minimum_required(VERSION 3.20)

# Single version knob. Local/PR builds stay on the default so a Debug/Release exe
# is never mistaken for a GitHub Release. CI tag builds pass e.g.
# -DFG_RELEASE_VERSION=1.0.0 (tag v1.0.0 without the leading v).
set(FG_RELEASE_VERSION "0.0.0-dev" CACHE STRING
    "App version string (CI sets from git tag; local default is 0.0.0-dev)")

# Numeric PE/FILEVERSION fields: take leading X.Y.Z; ignore -dev / -b1 / +meta.
set(_fg_ver "${FG_RELEASE_VERSION}")
string(REGEX REPLACE "^[vV]" "" _fg_ver "${_fg_ver}")
if(_fg_ver MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
  set(FG_VERSION_MAJOR "${CMAKE_MATCH_1}")
  set(FG_VERSION_MINOR "${CMAKE_MATCH_2}")
  set(FG_VERSION_PATCH "${CMAKE_MATCH_3}")
else()
  set(FG_VERSION_MAJOR 0)
  set(FG_VERSION_MINOR 0)
  set(FG_VERSION_PATCH 0)
endif()

# Short git identity for logs (especially 0.0.0-dev builds).
set(FG_GIT_HASH "unknown")
set(FG_GIT_DIRTY 0)
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _fg_git_hash
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _fg_git_hash_rc
  )
  if(_fg_git_hash_rc EQUAL 0 AND NOT _fg_git_hash STREQUAL "")
    set(FG_GIT_HASH "${_fg_git_hash}")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" status --porcelain
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _fg_git_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _fg_git_status_rc
  )
  if(_fg_git_status_rc EQUAL 0 AND NOT _fg_git_status STREQUAL "")
    set(FG_GIT_DIRTY 1)
  endif()
endif()

# Display / log string: always append +gHASH (from git at configure time).
# Prefer bare versions: 0.0.0-dev or X.Y.Z — do not include +g… in FG_RELEASE_VERSION.
# Leading v/V is stripped so -DFG_RELEASE_VERSION=v1.2.3 matches PE numerics and scripts.
set(_fg_base "${FG_RELEASE_VERSION}")
string(REGEX REPLACE "^[vV]" "" _fg_base "${_fg_base}")
string(REGEX REPLACE "\\+g[0-9a-fA-F]+(\\.dirty)?$" "" _fg_base "${_fg_base}")
string(REGEX REPLACE "\\.dirty$" "" _fg_base "${_fg_base}")
set(FG_VERSION_STRING "${_fg_base}+g${FG_GIT_HASH}")
if(FG_GIT_DIRTY)
  set(FG_VERSION_STRING "${FG_VERSION_STRING}.dirty")
endif()

project(fakegame
  VERSION ${FG_VERSION_MAJOR}.${FG_VERSION_MINOR}.${FG_VERSION_PATCH}
  LANGUAGES CXX RC
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(FG_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${FG_GENERATED_DIR}")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version_build.h.in"
  "${FG_GENERATED_DIR}/version_build.h"
  @ONLY
)
message(STATUS "fakegame version: ${FG_VERSION_STRING} "
  "(PE ${FG_VERSION_MAJOR}.${FG_VERSION_MINOR}.${FG_VERSION_PATCH}, "
  "git ${FG_GIT_HASH}, dirty=${FG_GIT_DIRTY})")

# Static CRT — renamed/relocated copy must not need VCRUNTIME*.dll
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(fakegame
  src/main.cpp
  src/config.cpp
  src/app.cpp
  src/profiles.cpp
  src/block_capture.cpp
  src/scene/scene_factory.cpp
  src/scene/scene_aurora.cpp
  src/scene/scene_orbital.cpp
  src/scene/scene_highway.cpp
  src/scene/scene_fractal.cpp
  src/d3d11_renderer.cpp
  src/d3d12_renderer.cpp
  src/vulkan_renderer.cpp
  src/none_renderer.cpp
  resources/app.rc
)

target_include_directories(fakegame PRIVATE
  src
  resources
  "${FG_GENERATED_DIR}"
)

target_compile_definitions(fakegame PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
)

if(MSVC)
  target_compile_options(fakegame PRIVATE
    /W4
    /permissive-
    /utf-8
    /Zc:__cplusplus
  )
  set_target_properties(fakegame PROPERTIES
    WIN32_EXECUTABLE FALSE
    OUTPUT_NAME "fakegame"
  )
  target_link_options(fakegame PRIVATE
    /INCREMENTAL:NO
  )
endif()

target_link_libraries(fakegame PRIVATE
  d3d11
  d3d12
  dxgi
  d3dcompiler
  user32
  gdi32
  dwmapi
  advapi32
  psapi
)

# Vulkan headers: VULKAN_SDK, else vendored Khronos headers (no SDK required to build).
if(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Include/vulkan/vulkan.h")
  target_include_directories(fakegame PRIVATE "$ENV{VULKAN_SDK}/Include")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include/vulkan/vulkan.h")
  target_include_directories(fakegame PRIVATE "${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include")
endif()
set_target_properties(fakegame PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
)
