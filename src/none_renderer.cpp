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

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    const scene::SceneDraw* sd = info.sceneDraw;
    const int cr = sd ? int(sd->clearR * 255.f) : 8;
    const int cg = sd ? int(sd->clearG * 255.f) : 6;
    const int cb = sd ? int(sd->clearB * 255.f) : 28;
    HBRUSH bg = CreateSolidBrush(RGB(cr, cg, cb));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    // Cheap aurora bands when backdrop requests it (no shader).
    if (sd && sd->backdrop == scene::BackdropId::Aurora)
    {
      const float t = static_cast<float>(info.elapsedSec);
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
    }

    if (sd)
    {
      for (const auto& p : sd->prims)
        DrawPrimGdi(mem, p);

      if (sd->flashA > 0.05f)
      {
        // Approximate flash with a translucent-ish solid (GDI has no true alpha blend easily).
        const int fa = int(sd->flashA * 120.f);
        HBRUSH flash = CreateSolidBrush(
            RGB(int(sd->flashR * 255.f), int(sd->flashG * 255.f), int(sd->flashB * 255.f)));
        // Stipple-ish: draw horizontal stripes
        for (int y = 0; y < h; y += 4)
        {
          RECT fr{0, y, w, y + 1};
          (void)fa;
          FillRect(mem, &fr, flash);
        }
        DeleteObject(flash);
      }
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
      hud += L"scene  ";
      hud += info.sceneName ? info.sceneName : L"?";
      hud += L"\n";
      {
        wchar_t sbuf[64];
        swprintf_s(sbuf, L"seed   0x%08X\n", info.sceneSeed);
        hud += sbuf;
      }
      hud += L"class  " + info.windowClass + L"\n";
      hud += L"title  " + info.windowTitle + L"\n";
      hud += L"pid    " + std::to_wstring(info.pid) + L"\n";
      wchar_t tbuf[64];
      swprintf_s(tbuf, L"time   %.2fs\n", info.elapsedSec);
      hud += tbuf;
      if (sd && !sd->hud.line1.empty())
        hud += sd->hud.line1 + L"\n";
      if (sd && !sd->hud.line2.empty())
        hud += sd->hud.line2 + L"\n";
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

    ValidateRect(hwnd_, nullptr);
  }

  const wchar_t* ApiName() const override { return L"none"; }
  const wchar_t* SwapEffectName() const override { return L"n/a"; }
  const wchar_t* PresentModeName() const override { return L"gdi"; }

private:
  static COLORREF RgbF(float r, float g, float b)
  {
    auto ch = [](float x) -> int {
      if (x < 0.f)
        x = 0.f;
      if (x > 1.f)
        x = 1.f;
      return int(x * 255.f + 0.5f);
    };
    return RGB(ch(r), ch(g), ch(b));
  }

  void DrawPrimGdi(HDC mem, const scene::Prim& p)
  {
    const COLORREF col = RgbF(p.r, p.g, p.b);
    switch (p.kind)
    {
    case scene::PrimKind::QuadOrb:
    case scene::PrimKind::CircleOutline: {
      const float rad = (p.kind == scene::PrimKind::CircleOutline) ? p.w : (p.w * 0.5f);
      HBRUSH br = CreateSolidBrush(col);
      HGDIOBJ ob = SelectObject(mem, br);
      HPEN np = (HPEN)GetStockObject(NULL_PEN);
      HGDIOBJ op = SelectObject(mem, np);
      Ellipse(mem, int(p.x - rad), int(p.y - rad), int(p.x + rad), int(p.y + rad));
      SelectObject(mem, op);
      SelectObject(mem, ob);
      DeleteObject(br);
      break;
    }
    case scene::PrimKind::QuadRing: {
      const float rad = p.w * 0.5f;
      HPEN pen = CreatePen(PS_SOLID, 2, col);
      HGDIOBJ op = SelectObject(mem, pen);
      HGDIOBJ ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
      Ellipse(mem, int(p.x - rad), int(p.y - rad * (p.h / p.w)), int(p.x + rad),
              int(p.y + rad * (p.h / std::max(p.w, 1.f))));
      SelectObject(mem, ob);
      SelectObject(mem, op);
      DeleteObject(pen);
      break;
    }
    case scene::PrimKind::QuadSolid: {
      const float cs = std::cos(p.rot), sn = std::sin(p.rot);
      const float hx = p.w * 0.5f, hy = p.h * 0.5f;
      POINT pts[4];
      const float local[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
      for (int i = 0; i < 4; ++i)
      {
        pts[i].x = LONG(p.x + local[i][0] * cs - local[i][1] * sn);
        pts[i].y = LONG(p.y + local[i][0] * sn + local[i][1] * cs);
      }
      HBRUSH br = CreateSolidBrush(col);
      HGDIOBJ ob = SelectObject(mem, br);
      HPEN pen = CreatePen(PS_SOLID, 1, col);
      HGDIOBJ op = SelectObject(mem, pen);
      Polygon(mem, pts, 4);
      SelectObject(mem, op);
      SelectObject(mem, ob);
      DeleteObject(pen);
      DeleteObject(br);
      break;
    }
    case scene::PrimKind::Line: {
      HPEN pen = CreatePen(PS_SOLID, int(p.w > 1.f ? p.w : 2.f), col);
      HGDIOBJ op = SelectObject(mem, pen);
      MoveToEx(mem, int(p.x), int(p.y), nullptr);
      LineTo(mem, int(p.x2), int(p.y2));
      SelectObject(mem, op);
      DeleteObject(pen);
      break;
    }
    case scene::PrimKind::Triangle: {
      const float cs = std::cos(p.rot), sn = std::sin(p.rot);
      const float halfB = p.w * 0.5f;
      const float halfH = p.h * 0.5f;
      const float lx[3] = {halfH, -halfH, -halfH};
      const float ly[3] = {0.f, -halfB, halfB};
      POINT pts[3];
      for (int i = 0; i < 3; ++i)
      {
        pts[i].x = LONG(p.x + lx[i] * cs - ly[i] * sn);
        pts[i].y = LONG(p.y + lx[i] * sn + ly[i] * cs);
      }
      HBRUSH br = CreateSolidBrush(col);
      HGDIOBJ ob = SelectObject(mem, br);
      HPEN pen = CreatePen(PS_SOLID, 1, col);
      HGDIOBJ op = SelectObject(mem, pen);
      Polygon(mem, pts, 3);
      SelectObject(mem, op);
      SelectObject(mem, ob);
      DeleteObject(pen);
      DeleteObject(br);
      break;
    }
    }
  }

  HWND hwnd_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

} // namespace

std::unique_ptr<IRenderer> CreateNoneRenderer()
{
  return std::make_unique<NoneRenderer>();
}
