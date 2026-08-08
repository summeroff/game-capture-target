#pragma once

#include "config.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include <Windows.h>

struct FrameInfo
{
  uint64_t frameIndex = 0;
  double elapsedSec = 0.0;
  int clientW = 0;
  int clientH = 0;
  DWORD pid = 0;
  std::wstring windowClass;
  std::wstring windowTitle;
  WindowMode mode = WindowMode::Windowed;
  bool vsync = true;
  bool flipModel = true;
  int buffers = 2;
  bool noHud = false;

  // Scene draw-list (owned by App for the frame; pointer may be null).
  const wchar_t* sceneName = L"aurora";
  uint32_t sceneSeed = 0;
  const scene::SceneDraw* sceneDraw = nullptr;
};

// Graphics backend behind a thin interface so d3d12/opengl can be added later.
class IRenderer
{
public:
  virtual ~IRenderer() = default;

  virtual bool Init(HWND hwnd, const Config& cfg, std::wstring* error) = 0;
  virtual void Shutdown() = 0;

  // Resize backbuffers to client size (or explicit size).
  virtual bool Resize(int width, int height, std::wstring* error) = 0;

  // Destroy + recreate swapchain only (same device).
  virtual bool RecreateSwapchain(std::wstring* error) = 0;

  // Destroy + recreate device + swapchain (TDR / device-lost sim).
  virtual bool RecreateDevice(std::wstring* error) = 0;

  // Apply exclusive fullscreen / windowed swapchain state when mode changes.
  virtual bool SetMode(WindowMode mode, std::wstring* error) = 0;

  virtual void Render(const FrameInfo& info) = 0;

  virtual const wchar_t* ApiName() const = 0;
  virtual const wchar_t* SwapEffectName() const = 0;
  virtual const wchar_t* PresentModeName() const = 0;
};

std::unique_ptr<IRenderer> CreateD3D11Renderer();
std::unique_ptr<IRenderer> CreateD3D12Renderer();
std::unique_ptr<IRenderer> CreateVulkanRenderer();
std::unique_ptr<IRenderer> CreateNoneRenderer();
