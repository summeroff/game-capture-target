#pragma once

#include "config.hpp"
#include "renderer.hpp"

#include <memory>
#include <string>

#include <Windows.h>

class App {
public:
  explicit App(Config cfg);
  ~App();

  App(const App&) = delete;
  App& operator=(const App&) = delete;

  bool Initialize(std::wstring* error);
  int Run();

  static App* FromHwnd(HWND hwnd);

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  bool RegisterWindowClass(std::wstring* error);
  bool CreateMainWindow(std::wstring* error);
  bool CreateRenderer(std::wstring* error);

  void ApplyWindowMode(WindowMode mode, bool initial);
  void GetMonitorRect(RECT* out) const;
  void ClientSize(int* w, int* h) const;

  void OnHotkeyMode();
  void OnHotkeyResize();
  void OnHotkeyRecreateSwapchain();
  void OnHotkeyRecreateDevice();
  void OnHotkeyTitle();
  void OnHotkeyChurn();

  void TickChurn();
  void Frame();
  void RequestQuit();

  Config cfg_;
  HWND hwnd_ = nullptr;
  ATOM atom_ = 0;
  std::unique_ptr<IRenderer> renderer_;

  bool running_ = true;
  bool churn_ = false;
  double churnAccum_ = 0.0;
  int churnPhase_ = 0;
  int titleCounter_ = 0;
  int resizeToggle_ = 0;

  uint64_t frameIndex_ = 0;
  double elapsedSec_ = 0.0;
  LARGE_INTEGER qpcFreq_{};
  LARGE_INTEGER qpcStart_{};
  LARGE_INTEGER qpcLast_{};
  double frameBudgetSec_ = 1.0 / 60.0;

  // Remember windowed placement when leaving windowed mode.
  WINDOWPLACEMENT windowedPlacement_{};
  int windowedW_ = 1280;
  int windowedH_ = 720;
};
