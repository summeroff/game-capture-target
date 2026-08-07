cmake_minimum_required(VERSION 3.20)
project(fakegame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT — renamed/relocated copy must not need VCRUNTIME*.dll
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(fakegame
  src/main.cpp
  src/config.cpp
  src/app.cpp
  src/d3d11_renderer.cpp
  src/none_renderer.cpp
)

target_include_directories(fakegame PRIVATE src)

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
  # Console subsystem so stdout logging works without AllocConsole.
  # Still a normal top-level HWND for OBS enumeration.
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
  dxgi
  d3dcompiler
  user32
  gdi32
  dwmapi
)

# Drop next to build tree root for convenience
set_target_properties(fakegame PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
)
