# Vendored Khronos Vulkan-Headers (C only)

Source: https://github.com/KhronosGroup/Vulkan-Headers
License: see LICENSE.md

Kept only what `src/vulkan_renderer.cpp` compiles against:

- `include/vulkan/vulkan.h`
- `include/vulkan/vulkan_core.h`
- `include/vulkan/vulkan_win32.h`
- `include/vulkan/vk_platform.h`
- `include/vk_video/*.h` (`vulkan_core.h` includes these unconditionally)

C++ wrappers (`vulkan.hpp` and friends) and unused platform headers are not vendored.
