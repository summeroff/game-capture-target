#include "renderer.hpp"

#include "font8x8.hpp"
#include "log.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

class NoneRenderer final : public IRenderer {
public:
  ~NoneRenderer() override { Shutdown(); }

  bool Init(HWND hwnd, const Config& cfg, std::wstring* error) override
  {
    (void)error;
    hwnd_ = hwnd;
    width_ = cfg.width;
    height_ = cfg.height;
    Log("none: GDI-only renderer (no swapchain) — OBS should report 'not a game'");
    return true;
  }

  void Shutdown() override { hwnd_ = nullptr; }

  bool Resize(int width, int height, std::wstring* error) override
  {
    (void)error;
    width_ = width;
    height_ = height;
    return true;
  }

  bool RecreateSwapchain(std::wstring* error) override
  {
    (void)error;
    Log("none: RecreateSwapchain (no-op)");
    return true;
  }

  bool RecreateDevice(std::wstring* error) override
  {
    (void)error;
    Log("none: RecreateDevice (no-op)");
    return true;
  }

  bool SetMode(WindowMode mode, std::wstring* error) override
  {
    (void)mode;
    (void)error;
    return true;
  }

  void Render(const FrameInfo& info) override
  {
    if (!hwnd_)
      return;

    HDC hdc = GetDC(hwnd_);
    if (!hdc)
      return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
      ReleaseDC(hwnd_, hdc);
      return;
    }

    // Double-buffer with a memory DC.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    const float t = static_cast<float>(info.elapsedSec);
    const int cr = static_cast<int>(30 + 30 * std::sin(t * 1.7f));
    const int cg = static_cast<int>(25 + 25 * std::sin(t * 1.3f + 2.0f));
    const int cb = static_cast<int>(45 + 35 * std::sin(t * 2.1f + 4.0f));
    HBRUSH bg = CreateSolidBrush(RGB(cr, cg, cb));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    // Moving quad.
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;
    const float orbit = 120.f + 40.f * std::sin(t * 0.8f);
    const float ox = cx + std::cos(t * 1.4f) * orbit;
    const float oy = cy + std::sin(t * 1.4f) * orbit * 0.55f;
    const float ang = t * 2.2f;
    const float qs = 90.f + 20.f * std::sin(t * 3.0f);
    const float hs = qs * 0.5f;

    POINT pts[4];
    const float cs = std::cos(ang);
    const float sn = std::sin(ang);
    const float local[4][2] = {{-hs, -hs}, {hs, -hs}, {hs, hs}, {-hs, hs}};
    for (int i = 0; i < 4; ++i) {
      const float x = local[i][0] * cs - local[i][1] * sn;
      const float y = local[i][0] * sn + local[i][1] * cs;
      pts[i].x = static_cast<LONG>(ox + x);
      pts[i].y = static_cast<LONG>(oy + y);
    }
    const int qr = static_cast<int>(140 + 100 * std::sin(t * 3.5f));
    const int qg = static_cast<int>(140 + 100 * std::sin(t * 3.5f + 2.1f));
    const int qb = static_cast<int>(140 + 100 * std::sin(t * 3.5f + 4.2f));
    HBRUSH qbsh = CreateSolidBrush(RGB(qr, qg, qb));
    HGDIOBJ oldBr = SelectObject(mem, qbsh);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(qr, qg, qb));
    HGDIOBJ oldPen = SelectObject(mem, pen);
    Polygon(mem, pts, 4);
    SelectObject(mem, oldPen);
    SelectObject(mem, oldBr);
    DeleteObject(pen);
    DeleteObject(qbsh);

    if (!info.noHud) {
      SetBkMode(mem, TRANSPARENT);
      SetTextColor(mem, RGB(255, 255, 40));
      HFONT big = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                              FIXED_PITCH | FF_MODERN, L"Consolas");
      HFONT small = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");

      HGDIOBJ oldF = SelectObject(mem, big);
      wchar_t bigNum[32];
      swprintf_s(bigNum, L"%llu", static_cast<unsigned long long>(info.frameIndex));
      RECT tr{0, h / 6, w, h / 6 + 60};
      DrawTextW(mem, bigNum, -1, &tr, DT_CENTER | DT_SINGLELINE);

      SelectObject(mem, small);
      SetTextColor(mem, RGB(240, 240, 240));
      std::wstring hud;
      hud += L"frame  " + std::to_wstring(info.frameIndex) + L"\n";
      hud += L"size   " + std::to_wstring(info.clientW) + L"x" + std::to_wstring(info.clientH) + L"\n";
      hud += L"api    none (GDI, no swapchain)\n";
      hud += L"swap   n/a\n";
      hud += L"present n/a\n";
      hud += L"mode   ";
      hud += WindowModeName(info.mode);
      hud += L"\n";
      hud += L"class  " + info.windowClass + L"\n";
      hud += L"title  " + info.windowTitle + L"\n";
      hud += L"pid    " + std::to_wstring(info.pid) + L"\n";
      wchar_t tbuf[64];
      swprintf_s(tbuf, L"time   %.2fs\n", info.elapsedSec);
      hud += tbuf;
      hud += L"hotkeys F1 mode F2 resize F3 swap F4 device F5 title F6 churn Esc quit";

      RECT hr{8, 8, w - 8, h - 8};
      DrawTextW(mem, hud.c_str(), -1, &hr, DT_LEFT | DT_TOP | DT_NOPREFIX);

      SelectObject(mem, oldF);
      DeleteObject(big);
      DeleteObject(small);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd_, hdc);

    // Drive WM_PAINT-less animation; GDI path presents immediately.
    // Validate so we don't stack paints.
    ValidateRect(hwnd_, nullptr);
  }

  const wchar_t* ApiName() const override { return L"none"; }
  const wchar_t* SwapEffectName() const override { return L"n/a"; }
  const wchar_t* PresentModeName() const override { return L"gdi"; }

private:
  HWND hwnd_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

} // namespace

std::unique_ptr<IRenderer> CreateNoneRenderer()
{
  return std::make_unique<NoneRenderer>();
}
