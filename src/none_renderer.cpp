#include "renderer.hpp"

#include "font8x8.hpp"
#include "log.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace
{

class NoneRenderer final : public IRenderer
{
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
    if (w <= 0 || h <= 0)
    {
      ReleaseDC(hwnd_, hdc);
      return;
    }

    // Double-buffer with a memory DC.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    const float t = static_cast<float>(info.elapsedSec);
    const int cr = static_cast<int>(8 + 10 * std::sin(t * 0.4f));
    const int cg = static_cast<int>(4 + 8 * std::sin(t * 0.35f + 1.f));
    const int cb = static_cast<int>(28 + 20 * std::sin(t * 0.3f + 2.f));
    HBRUSH bg = CreateSolidBrush(RGB(cr, cg, cb));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    const float cx = w * 0.5f;
    const float cy = h * 0.55f;

    auto hsvBrush = [](float h, float s, float v) -> HBRUSH {
      h = h - std::floor(h);
      const float sector = std::floor(h * 6.f);
      const float f = h * 6.f - sector;
      const float p = v * (1.f - s);
      const float q = v * (1.f - f * s);
      const float tt = v * (1.f - (1.f - f) * s);
      float r = 0.f, g = 0.f, b = 0.f;
      switch (static_cast<int>(sector) % 6)
      {
      case 0:
        r = v;
        g = tt;
        b = p;
        break;
      case 1:
        r = q;
        g = v;
        b = p;
        break;
      case 2:
        r = p;
        g = v;
        b = tt;
        break;
      case 3:
        r = p;
        g = q;
        b = v;
        break;
      case 4:
        r = tt;
        g = p;
        b = v;
        break;
      default:
        r = v;
        g = p;
        b = q;
        break;
      }
      return CreateSolidBrush(RGB(int(r * 255.f), int(g * 255.f), int(b * 255.f)));
    };

    // Aurora-ish horizontal bands
    for (int y = 0; y < h; y += 3)
    {
      const float ny = (y / float(h)) * 2.f - 1.f;
      const float wave = std::sin(t * 0.9f + y * 0.02f) * 0.3f;
      const float band = std::exp(-(ny + wave) * (ny + wave) * 4.f);
      if (band < 0.05f)
        continue;
      const int a = int(band * 90);
      HPEN pen = CreatePen(PS_SOLID, 3, RGB(int(20 + a * 0.4f), int(80 + a * 1.2f), int(60 + a)));
      HGDIOBJ op = SelectObject(mem, pen);
      MoveToEx(mem, 0, y, nullptr);
      LineTo(mem, w, y);
      SelectObject(mem, op);
      DeleteObject(pen);
    }

    auto fillCircle = [&](float x, float y, float rad, HBRUSH br) {
      HGDIOBJ ob = SelectObject(mem, br);
      HPEN np = (HPEN)GetStockObject(NULL_PEN);
      HGDIOBJ op = SelectObject(mem, np);
      Ellipse(mem, int(x - rad), int(y - rad), int(x + rad), int(y + rad));
      SelectObject(mem, op);
      SelectObject(mem, ob);
    };

    // Orb rings
    for (int i = 0; i < 14; ++i)
    {
      const float a0 = t * 0.7f + i * (6.2831853f / 14);
      const float radius = 160.f + 35.f * std::sin(t * 1.1f + i * 0.4f);
      const float ox = cx + std::cos(a0) * radius;
      const float oy = cy + std::sin(a0) * radius * 0.62f;
      const float sz = 14.f + 6.f * std::sin(t * 3.f + i);
      HBRUSH br = hsvBrush(std::fmod(t * 0.08f + i / 14.f, 1.f), 0.85f, 1.f);
      fillCircle(ox, oy, sz, br);
      DeleteObject(br);
    }

    // Comet
    {
      const float ca = t * 1.15f;
      const float cometR = 230.f + 50.f * std::sin(t * 0.6f);
      for (int trail = 6; trail >= 0; --trail)
      {
        const float ta = ca - trail * 0.09f;
        const float trr = cometR - trail * 7.f;
        HBRUSH br = hsvBrush(std::fmod(0.1f + trail * 0.03f + t * 0.1f, 1.f), 0.9f, 1.f);
        fillCircle(cx + std::cos(ta) * trr, cy + std::sin(ta) * trr * 0.5f, 18.f - trail * 2.f, br);
        DeleteObject(br);
      }
    }

    // Bottom bars
    constexpr int kBars = 40;
    const float barW = w / float(kBars);
    for (int i = 0; i < kBars; ++i)
    {
      const float n = 0.5f + 0.5f * std::sin(t * 4.f + i * 0.45f) * std::cos(t * 2.3f + i * 0.17f);
      const int bh = int(25 + n * (h * 0.2f));
      HBRUSH br = hsvBrush(std::fmod(i / float(kBars) + t * 0.15f, 1.f), 0.85f, 1.f);
      RECT brc{int(i * barW + 1), h - bh - 6, int((i + 1) * barW - 1), h - 6};
      FillRect(mem, &brc, br);
      DeleteObject(br);
    }

    // Spinning diamond
    {
      const float ang = t * 1.8f;
      const float qs = 70.f + 15.f * std::sin(t * 4.f);
      const float hs = qs * 0.5f;
      POINT pts[4];
      const float cs = std::cos(ang), sn = std::sin(ang);
      const float local[4][2] = {{0, -hs}, {hs, 0}, {0, hs}, {-hs, 0}};
      for (int i = 0; i < 4; ++i)
      {
        pts[i].x = LONG(cx + local[i][0] * cs - local[i][1] * sn);
        pts[i].y = LONG(cy + local[i][0] * sn + local[i][1] * cs);
      }
      HBRUSH br = hsvBrush(std::fmod(t * 0.2f, 1.f), 0.6f, 1.f);
      HGDIOBJ ob = SelectObject(mem, br);
      HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
      HGDIOBJ op = SelectObject(mem, pen);
      Polygon(mem, pts, 4);
      SelectObject(mem, op);
      SelectObject(mem, ob);
      DeleteObject(pen);
      DeleteObject(br);
    }

    if (!info.noHud)
    {
      SetBkMode(mem, TRANSPARENT);
      SetTextColor(mem, RGB(255, 240, 80));
      HFONT big = CreateFontW(56, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                              FIXED_PITCH | FF_MODERN, L"Consolas");
      HFONT small = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");

      HGDIOBJ oldF = SelectObject(mem, big);
      wchar_t bigNum[32];
      swprintf_s(bigNum, L"%llu", static_cast<unsigned long long>(info.frameIndex));
      RECT tr{0, h / 8, w, h / 8 + 70};
      DrawTextW(mem, bigNum, -1, &tr, DT_CENTER | DT_SINGLELINE);

      SelectObject(mem, small);
      SetTextColor(mem, RGB(230, 240, 255));
      std::wstring hud;
      hud += L"frame  " + std::to_wstring(info.frameIndex) + L"\n";
      hud +=
          L"size   " + std::to_wstring(info.clientW) + L"x" + std::to_wstring(info.clientH) + L"\n";
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
      hud += L"hotkeys F1-F7 / Esc";

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
