#pragma once

#include <cstdint>
#include <string>

enum class WindowMode {
  Windowed,
  Borderless,
  FullscreenExclusive,
};

enum class GraphicsApi {
  D3D11,
  D3D12,   // stretch — not implemented yet
  OpenGL,  // stretch — not implemented yet
  None,
};

struct Config {
  std::wstring title = L"Fake Game";
  std::wstring windowClass = L"FakeGameWindowClass";
  int width = 1280;
  int height = 720;
  WindowMode mode = WindowMode::Windowed;
  GraphicsApi api = GraphicsApi::D3D11;
  int fps = 60;
  bool vsync = true;
  bool flipModel = true;
  int buffers = 2;
  int exitAfterSeconds = 0;
  bool topmost = false;
  bool noHud = false;
};

// Parse argv + optional --config INI. Flags override file.
// Returns false on hard error (message in *error).
bool ParseConfig(int argc, wchar_t** argv, Config* out, std::wstring* error);

const wchar_t* WindowModeName(WindowMode m);
const wchar_t* GraphicsApiName(GraphicsApi a);
WindowMode NextWindowMode(WindowMode m);

void PrintConfig(const Config& c);
