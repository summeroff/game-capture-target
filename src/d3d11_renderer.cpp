#include "renderer.hpp"

#include "font8x8.hpp"
#include "log.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{

constexpr char kShaderSrc[] = R"(
cbuffer CB : register(b0)
{
    float4x4 uTransform;
    float4   uColor;    // rgb + alpha multiplier
    float4   uTimeRes;  // x=time sec, y=width, z=height, w=mode (0 solid,1 unused)
};

struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.pos = mul(float4(i.pos, 0.0f, 1.0f), uTransform);
    o.uv  = i.uv;
    o.col = uColor;
    return o;
}

Texture2D    gTex : register(t0);
SamplerState gSamp : register(s0);

float3 hsv2rgb(float h, float s, float v)
{
    float3 p = abs(frac(h + float3(0.0, 2.0/3.0, 1.0/3.0)) * 6.0 - 3.0);
    return v * lerp(float3(1,1,1), saturate(p - 1.0), s);
}

// Fullscreen procedural backdrop — aurora + soft tunnel + twinkles.
float4 PSBackground(VSOut i) : SV_Target
{
    float t = uTimeRes.x;
    float2 uv = i.uv;
    float2 p = uv * 2.0 - 1.0;
    p.x *= uTimeRes.y / max(uTimeRes.z, 1.0);

    float r = length(p);
    float ang = atan2(p.y, p.x);

    // Deep space base
    float3 col = float3(0.015, 0.01, 0.04);

    // Slow color nebula
    float n1 = sin(p.x * 3.1 + t * 0.35) * cos(p.y * 2.7 - t * 0.28);
    float n2 = sin((p.x + p.y) * 4.2 - t * 0.55) * 0.5;
    float n3 = cos(p.x * 1.7 - p.y * 2.3 + t * 0.2);
    float neb = saturate(0.55 + 0.45 * (n1 + n2 * 0.7 + n3 * 0.4));
    col += hsv2rgb(frac(0.62 + t * 0.02 + neb * 0.15), 0.65, 0.22) * neb;

    // Aurora curtains
    float wave = sin(p.x * 2.5 + t * 0.9) * 0.25
               + sin(p.x * 5.0 - t * 1.4) * 0.12
               + sin(p.x * 0.8 + t * 0.3) * 0.35;
    float ay = p.y + wave * 0.45;
    float band = exp(-pow(ay * 2.2, 2.0)) * (0.55 + 0.45 * sin(p.x * 3.0 + t));
    float band2 = exp(-pow((ay + 0.35) * 2.8, 2.0)) * (0.4 + 0.4 * cos(p.x * 4.0 - t * 1.2));
    col += float3(0.05, 0.55, 0.35) * band * 0.85;
    col += float3(0.35, 0.15, 0.75) * band2 * 0.7;
    col += float3(0.9, 0.35, 0.55) * band * band2 * 1.2;

    // Hyperspace rings (frozen capture = rings stop)
    float rings = abs(sin(12.0 / max(r, 0.08) - t * 3.5 + ang * 2.0));
    rings = pow(rings, 8.0) * smoothstep(1.4, 0.05, r);
    col += hsv2rgb(frac(ang / 6.28318 + t * 0.1), 0.8, 1.0) * rings * 0.55;

    // Twinkling star field (hash-ish)
    float2 gv = floor(uv * float2(48.0, 27.0));
    float2 gu = frac(uv * float2(48.0, 27.0)) - 0.5;
    float rnd = frac(sin(dot(gv, float2(127.1, 311.7))) * 43758.5453);
    float tw = step(0.92, rnd) * smoothstep(0.2, 0.0, length(gu));
    tw *= 0.5 + 0.5 * sin(t * (6.0 + rnd * 20.0) + rnd * 30.0);
    col += tw * (0.6 + 0.4 * rnd);

    // Vignette
    col *= 1.0 - smoothstep(0.55, 1.35, r) * 0.65;

    // Subtle scanline shimmer so 1-frame freezes read as "stuck"
    col *= 0.92 + 0.08 * sin(uv.y * uTimeRes.z * 3.14159 + t * 40.0);

    return float4(col, 1.0);
}

float4 PSSolid(VSOut i) : SV_Target
{
    return i.col;
}

// Soft circular orb / particle (unit quad UV 0..1).
float4 PSOrb(VSOut i) : SV_Target
{
    float2 p = i.uv * 2.0 - 1.0;
    float d = length(p);
    float core = saturate(1.0 - d * 1.15);
    float glow = saturate(1.0 - d);
    float a = core * core * core + glow * glow * 0.35;
    a *= i.col.a;
    float3 c = i.col.rgb * (0.55 + 0.45 * core);
    return float4(c, a);
}

// Ring / hollow disc
float4 PSRing(VSOut i) : SV_Target
{
    float2 p = i.uv * 2.0 - 1.0;
    float d = length(p);
    float ring = 1.0 - abs(d - 0.72) * 6.0;
    float a = saturate(ring);
    a = a * a * i.col.a;
    return float4(i.col.rgb, a);
}

float4 PSText(VSOut i) : SV_Target
{
    float a = gTex.Sample(gSamp, i.uv).r;
    return float4(i.col.rgb, i.col.a * a);
}
)";

struct Vertex
{
  float x, y;
  float u, v;
};

struct CBData
{
  float transform[16];
  float color[4];
  float timeRes[4];
};

void MatIdentity(float* m)
{
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = m[5] = m[10] = m[15] = 1.f;
}

// Column-major ortho: x,y in pixels → NDC. Y grows down (top-left origin).
void MatOrthoPixels(float* m, float w, float h)
{
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = 2.f / w;
  m[5] = -2.f / h;
  m[10] = 1.f;
  m[12] = -1.f;
  m[13] = 1.f;
  m[15] = 1.f;
}

void MatMul(float* out, const float* a, const float* b)
{
  float t[16];
  for (int c = 0; c < 4; ++c)
  {
    for (int r = 0; r < 4; ++r)
    {
      t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                     a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
    }
  }
  std::memcpy(out, t, sizeof(t));
}

void MatTranslate(float* m, float x, float y)
{
  MatIdentity(m);
  m[12] = x;
  m[13] = y;
}

void MatRotateZ(float* m, float rad)
{
  MatIdentity(m);
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  m[0] = c;
  m[1] = s;
  m[4] = -s;
  m[5] = c;
}

void MatScale(float* m, float sx, float sy)
{
  MatIdentity(m);
  m[0] = sx;
  m[5] = sy;
}

std::wstring HrMsg(HRESULT hr)
{
  wchar_t buf[64];
  swprintf_s(buf, L"HRESULT 0x%08X", static_cast<unsigned>(hr));
  return buf;
}

class D3D11Renderer final : public IRenderer
{
public:
  ~D3D11Renderer() override { Shutdown(); }

  bool Init(HWND hwnd, const Config& cfg, std::wstring* error) override
  {
    hwnd_ = hwnd;
    cfg_ = cfg;
    width_ = cfg.width;
    height_ = cfg.height;
    mode_ = cfg.mode;

    if (!CreateDevice(error))
      return false;
    if (!CreateSwapchain(error))
      return false;
    if (!CreatePipeline(error))
      return false;
    if (!CreateFontTexture(error))
      return false;
    return true;
  }

  void Shutdown() override
  {
    rtv_.Reset();
    backbuffer_.Reset();
    swapchain1_.Reset();
    swapchain_.Reset();
    fontSrv_.Reset();
    fontTex_.Reset();
    samp_.Reset();
    vb_.Reset();
    cb_.Reset();
    layout_.Reset();
    vs_.Reset();
    psSolid_.Reset();
    psText_.Reset();
    psBg_.Reset();
    psOrb_.Reset();
    psRing_.Reset();
    blend_.Reset();
    blendAdd_.Reset();
    ctx_.Reset();
    device_.Reset();
  }

  bool Resize(int width, int height, std::wstring* error) override
  {
    if (width < 1 || height < 1)
      return true;
    width_ = width;
    height_ = height;
    return RecreateSwapchain(error);
  }

  bool RecreateSwapchain(std::wstring* error) override
  {
    if (!device_)
    {
      *error = L"no device";
      return false;
    }

    if (ctx_)
      ctx_->ClearState();
    rtv_.Reset();
    backbuffer_.Reset();

    if (swapchain_)
    {
      // Leave exclusive fullscreen before resize if needed.
      BOOL fs = FALSE;
      ComPtr<IDXGIOutput> out;
      if (SUCCEEDED(swapchain_->GetFullscreenState(&fs, &out)) && fs)
      {
        swapchain_->SetFullscreenState(FALSE, nullptr);
      }

      const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
      HRESULT hr =
          swapchain_->ResizeBuffers(static_cast<UINT>(cfg_.buffers), static_cast<UINT>(width_),
                                    static_cast<UINT>(height_), fmt, swapFlags_);
      if (FAILED(hr))
      {
        // Fall back to full recreate.
        swapchain1_.Reset();
        swapchain_.Reset();
        if (!CreateSwapchain(error))
          return false;
      } else
      {
        if (!CreateRtv(error))
          return false;
      }
    } else
    {
      if (!CreateSwapchain(error))
        return false;
    }

    ApplyFullscreenState(error);
    return true;
  }

  bool RecreateDevice(std::wstring* error) override
  {
    Log("d3d11: recreating device");
    const Config saved = cfg_;
    const int w = width_;
    const int h = height_;
    const WindowMode m = mode_;
    Shutdown();
    cfg_ = saved;
    width_ = w;
    height_ = h;
    mode_ = m;
    if (!CreateDevice(error))
      return false;
    if (!CreateSwapchain(error))
      return false;
    if (!CreatePipeline(error))
      return false;
    if (!CreateFontTexture(error))
      return false;
    return true;
  }

  bool SetMode(WindowMode mode, std::wstring* error) override
  {
    mode_ = mode;
    return ApplyFullscreenState(error);
  }

  void Render(const FrameInfo& info) override
  {
    if (!ctx_ || !rtv_)
      return;

    const float t = static_cast<float>(info.elapsedSec);
    timeSec_ = t;

    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    // Background shader paints everything; clear is a safety net.
    const float clear[4] = {0.01f, 0.01f, 0.03f, 1.f};
    ctx_->ClearRenderTargetView(rtv_.Get(), clear);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.f;
    vp.MaxDepth = 1.f;
    ctx_->RSSetViewports(1, &vp);

    ctx_->IASetInputLayout(layout_.Get());
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());

    float ortho[16];
    MatOrthoPixels(ortho, static_cast<float>(width_), static_cast<float>(height_));

    // --- Fullscreen procedural backdrop ---
    {
      float scl[16], tr[16], world[16], mvp[16];
      MatScale(scl, static_cast<float>(width_), static_cast<float>(height_));
      MatTranslate(tr, width_ * 0.5f, height_ * 0.5f);
      MatMul(world, tr, scl);
      MatMul(mvp, ortho, world);
      const float blendFactor[4] = {0, 0, 0, 0};
      ctx_->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
      ctx_->PSSetShader(psBg_.Get(), nullptr, 0);
      UpdateCB(mvp, 1, 1, 1, 1);
      ctx_->Draw(6, 0);
    }

    const float cx = width_ * 0.5f;
    const float cy = height_ * 0.55f;
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx_->OMSetBlendState(blendAdd_.Get(), blendFactor, 0xFFFFFFFF);

    // --- Outer spinning ring of orbs ---
    ctx_->PSSetShader(psOrb_.Get(), nullptr, 0);
    constexpr int kOrbs = 14;
    for (int i = 0; i < kOrbs; ++i)
    {
      const float a0 = t * 0.7f + i * (6.2831853f / kOrbs);
      const float radius = 180.f + 40.f * std::sin(t * 1.1f + i * 0.4f);
      const float ox = cx + std::cos(a0) * radius;
      const float oy = cy + std::sin(a0) * radius * 0.62f;
      const float sz = 28.f + 10.f * std::sin(t * 3.0f + i);
      const float hue = std::fmod(t * 0.08f + i / float(kOrbs), 1.f);
      float rr, gg, bb;
      Hsv(hue, 0.85f, 1.f, &rr, &gg, &bb);
      DrawTransformed(ortho, ox, oy, 0.f, sz, sz, rr, gg, bb, 0.9f);
    }

    // --- Counter-rotating inner ring ---
    for (int i = 0; i < 8; ++i)
    {
      const float a0 = -t * 1.3f + i * (6.2831853f / 8);
      const float radius = 90.f + 18.f * std::cos(t * 2.0f + i);
      const float ox = cx + std::cos(a0) * radius;
      const float oy = cy + std::sin(a0) * radius * 0.7f;
      float rr, gg, bb;
      Hsv(std::fmod(0.45f + i * 0.08f + t * 0.05f, 1.f), 0.7f, 1.f, &rr, &gg, &bb);
      DrawTransformed(ortho, ox, oy, 0.f, 18.f, 18.f, rr, gg, bb, 0.85f);
    }

    // --- Big soft rings ---
    ctx_->PSSetShader(psRing_.Get(), nullptr, 0);
    for (int i = 0; i < 3; ++i)
    {
      const float pulse = 1.f + 0.12f * std::sin(t * 2.5f + i);
      const float sz = (220.f + i * 90.f) * pulse;
      float rr, gg, bb;
      Hsv(std::fmod(0.55f + i * 0.12f + t * 0.03f, 1.f), 0.8f, 1.f, &rr, &gg, &bb);
      DrawTransformed(ortho, cx, cy, t * (0.4f + i * 0.2f), sz, sz * 0.7f, rr, gg, bb, 0.35f);
    }

    // --- Comet: bright orb + trailing ghosts ---
    ctx_->PSSetShader(psOrb_.Get(), nullptr, 0);
    {
      const float ca = t * 1.15f;
      const float cr = 250.f + 60.f * std::sin(t * 0.6f);
      const float cox = cx + std::cos(ca) * cr;
      const float coy = cy + std::sin(ca) * cr * 0.5f;
      for (int trail = 7; trail >= 0; --trail)
      {
        const float ta = ca - trail * 0.08f;
        const float trr = cr - trail * 6.f;
        const float tx = cx + std::cos(ta) * trr;
        const float ty = cy + std::sin(ta) * trr * 0.5f;
        const float sz = 42.f - trail * 4.f;
        const float al = 0.95f - trail * 0.1f;
        float rr, gg, bb;
        Hsv(std::fmod(0.08f + trail * 0.02f + t * 0.1f, 1.f), 0.9f, 1.f, &rr, &gg, &bb);
        DrawTransformed(ortho, tx, ty, 0.f, sz, sz, rr, gg, bb, al);
      }
      DrawTransformed(ortho, cox, coy, 0.f, 56.f, 56.f, 1.f, 0.95f, 0.8f, 1.f);
    }

    // --- Audio-style bars along the bottom (purely visual) ---
    ctx_->PSSetShader(psSolid_.Get(), nullptr, 0);
    ctx_->OMSetBlendState(blend_.Get(), blendFactor, 0xFFFFFFFF);
    constexpr int kBars = 48;
    const float barW = width_ / float(kBars);
    for (int i = 0; i < kBars; ++i)
    {
      const float n = 0.5f + 0.5f * std::sin(t * 4.0f + i * 0.45f) * std::cos(t * 2.3f + i * 0.17f);
      const float h = (30.f + n * (height_ * 0.22f));
      const float bx = (i + 0.5f) * barW;
      const float by = height_ - h * 0.5f - 8.f;
      float rr, gg, bb;
      Hsv(std::fmod(i / float(kBars) + t * 0.15f, 1.f), 0.85f, 1.f, &rr, &gg, &bb);
      DrawTransformed(ortho, bx, by, 0.f, barW * 0.72f, h, rr, gg, bb, 0.75f);
    }

    // --- Spinning diamond core ---
    {
      const float ang = t * 1.8f;
      const float sz = 70.f + 15.f * std::sin(t * 4.0f);
      float rr, gg, bb;
      Hsv(std::fmod(t * 0.2f, 1.f), 0.6f, 1.f, &rr, &gg, &bb);
      DrawTransformed(ortho, cx, cy, ang, sz, sz, rr, gg, bb, 0.9f);
      DrawTransformed(ortho, cx, cy, ang + 0.785f, sz * 0.55f, sz * 0.55f, 1.f, 1.f, 1.f, 0.7f);
    }

    // Large frame counter — still the capture-liveness tell.
    if (!info.noHud)
    {
      ctx_->OMSetBlendState(blend_.Get(), blendFactor, 0xFFFFFFFF);
      char big[32];
      sprintf_s(big, "%llu", static_cast<unsigned long long>(info.frameIndex));
      const float scale = 7.f;
      const float tw = static_cast<float>(std::strlen(big)) * font8x8::kGlyphW * scale;
      // Soft shadow then bright glyph
      DrawTextPx(big, (width_ - tw) * 0.5f + 3.f, height_ * 0.12f + 3.f, scale, 0.f, 0.f, 0.f,
                 0.55f);
      DrawTextPx(big, (width_ - tw) * 0.5f, height_ * 0.12f, scale, 1.f, 0.95f, 0.35f, 1.f);

      char line[256];
      std::vector<std::string> lines;
      sprintf_s(line, "frame  %llu", static_cast<unsigned long long>(info.frameIndex));
      lines.emplace_back(line);
      sprintf_s(line, "size   %dx%d", info.clientW, info.clientH);
      lines.emplace_back(line);
      sprintf_s(line, "api    %s", Narrow(ApiName()).c_str());
      lines.emplace_back(line);
      sprintf_s(line, "swap   %s", Narrow(SwapEffectName()).c_str());
      lines.emplace_back(line);
      sprintf_s(line, "present %s", Narrow(PresentModeName()).c_str());
      lines.emplace_back(line);
      sprintf_s(line, "mode   %s", Narrow(WindowModeName(info.mode)).c_str());
      lines.emplace_back(line);
      sprintf_s(line, "class  %s", Narrow(info.windowClass).c_str());
      lines.emplace_back(line);
      sprintf_s(line, "title  %s", Narrow(info.windowTitle).c_str());
      lines.emplace_back(line);
      sprintf_s(line, "pid    %lu", static_cast<unsigned long>(info.pid));
      lines.emplace_back(line);
      sprintf_s(line, "time   %.2fs", info.elapsedSec);
      lines.emplace_back(line);
      sprintf_s(line, "hotkeys F1-F7 / Esc");
      lines.emplace_back(line);

      float y = 8.f;
      for (const auto& s : lines)
      {
        DrawTextPx(s.c_str(), 9.f, y + 1.f, 2.f, 0.f, 0.f, 0.f, 0.65f);
        DrawTextPx(s.c_str(), 8.f, y, 2.f, 0.92f, 0.95f, 1.f, 1.f);
        y += font8x8::kGlyphH * 2.f + 4.f;
      }
    }

    const UINT sync = cfg_.vsync ? 1u : 0u;
    swapchain_->Present(sync, 0);
  }

  const wchar_t* ApiName() const override { return L"d3d11"; }

  const wchar_t* SwapEffectName() const override
  {
    return cfg_.flipModel ? L"FLIP_DISCARD" : L"DISCARD";
  }

  const wchar_t* PresentModeName() const override { return cfg_.vsync ? L"vsync" : L"immediate"; }

private:
  bool CreateDevice(std::wstring* error)
  {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   _countof(levels), D3D11_SDK_VERSION, &device_, &got, &ctx_);
    if (FAILED(hr))
    {
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                             _countof(levels), D3D11_SDK_VERSION, &device_, &got, &ctx_);
    }
    if (FAILED(hr))
    {
      *error = L"D3D11CreateDevice failed: " + HrMsg(hr);
      return false;
    }
    Log("d3d11: device ok feature_level=0x%04X", static_cast<unsigned>(got));
    return true;
  }

  bool CreateSwapchain(std::wstring* error)
  {
    ComPtr<IDXGIDevice> dxgiDev;
    HRESULT hr = device_.As(&dxgiDev);
    if (FAILED(hr))
    {
      *error = L"IDXGIDevice QI failed";
      return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDev->GetAdapter(&adapter);
    if (FAILED(hr))
    {
      *error = L"GetAdapter failed";
      return false;
    }
    ComPtr<IDXGIFactory2> factory2;
    {
      ComPtr<IDXGIFactory> factory;
      hr = adapter->GetParent(IID_PPV_ARGS(&factory));
      if (FAILED(hr))
      {
        *error = L"GetParent factory failed";
        return false;
      }
      factory.As(&factory2);
    }

    swapFlags_ = 0;
    if (cfg_.flipModel)
      swapFlags_ = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    else
      swapFlags_ = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    if (factory2 && cfg_.flipModel)
    {
      DXGI_SWAP_CHAIN_DESC1 scd{};
      scd.Width = static_cast<UINT>(width_);
      scd.Height = static_cast<UINT>(height_);
      scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      scd.SampleDesc.Count = 1;
      scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      scd.BufferCount = static_cast<UINT>(cfg_.buffers < 2 ? 2 : cfg_.buffers);
      scd.Scaling = DXGI_SCALING_STRETCH;
      scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
      scd.Flags = swapFlags_;

      DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
      fs.Windowed = TRUE;
      fs.RefreshRate.Numerator = 0;
      fs.RefreshRate.Denominator = 1;

      hr = factory2->CreateSwapChainForHwnd(device_.Get(), hwnd_, &scd, &fs, nullptr, &swapchain1_);
      if (FAILED(hr))
      {
        Log("d3d11: FLIP_DISCARD failed (%s), falling back to DISCARD", Narrow(HrMsg(hr)).c_str());
        cfg_.flipModel = false;
      } else
      {
        swapchain1_.As(&swapchain_);
      }
    }

    if (!swapchain_)
    {
      ComPtr<IDXGIFactory> factory;
      hr = adapter->GetParent(IID_PPV_ARGS(&factory));
      if (FAILED(hr))
      {
        *error = L"GetParent factory failed";
        return false;
      }

      DXGI_SWAP_CHAIN_DESC scd{};
      scd.BufferDesc.Width = static_cast<UINT>(width_);
      scd.BufferDesc.Height = static_cast<UINT>(height_);
      scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      scd.SampleDesc.Count = 1;
      scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      scd.BufferCount = static_cast<UINT>(cfg_.buffers < 1 ? 1 : cfg_.buffers);
      if (scd.BufferCount < 1)
        scd.BufferCount = 1;
      scd.OutputWindow = hwnd_;
      scd.Windowed = TRUE;
      scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
      scd.Flags = swapFlags_;

      hr = factory->CreateSwapChain(device_.Get(), &scd, &swapchain_);
      if (FAILED(hr))
      {
        *error = L"CreateSwapChain failed: " + HrMsg(hr);
        return false;
      }
      cfg_.flipModel = false;
    }

    // Prevent DXGI from handling Alt-Enter itself.
    {
      ComPtr<IDXGIFactory> factory;
      ComPtr<IDXGIDevice> dev;
      if (SUCCEEDED(device_.As(&dev)))
      {
        ComPtr<IDXGIAdapter> ad;
        if (SUCCEEDED(dev->GetAdapter(&ad)))
        {
          ad->GetParent(IID_PPV_ARGS(&factory));
          if (factory)
            factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
        }
      }
    }

    if (!CreateRtv(error))
      return false;

    ApplyFullscreenState(error);
    Log("d3d11: swapchain %dx%d effect=%s buffers=%d", width_, height_,
        Narrow(SwapEffectName()).c_str(), cfg_.buffers);
    return true;
  }

  bool CreateRtv(std::wstring* error)
  {
    rtv_.Reset();
    backbuffer_.Reset();
    HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer_));
    if (FAILED(hr))
    {
      *error = L"GetBuffer failed: " + HrMsg(hr);
      return false;
    }
    hr = device_->CreateRenderTargetView(backbuffer_.Get(), nullptr, &rtv_);
    if (FAILED(hr))
    {
      *error = L"CreateRenderTargetView failed: " + HrMsg(hr);
      return false;
    }
    return true;
  }

  bool ApplyFullscreenState(std::wstring* error)
  {
    if (!swapchain_)
      return true;

    if (mode_ == WindowMode::FullscreenExclusive)
    {
      HRESULT hr = swapchain_->SetFullscreenState(TRUE, nullptr);
      if (FAILED(hr))
      {
        *error = L"SetFullscreenState(TRUE) failed: " + HrMsg(hr);
        Log("d3d11: %s", Narrow(*error).c_str());
        // Non-fatal — borderless-like still usable.
        return true;
      }
    } else
    {
      BOOL fs = FALSE;
      ComPtr<IDXGIOutput> out;
      if (SUCCEEDED(swapchain_->GetFullscreenState(&fs, &out)) && fs)
      {
        HRESULT hr = swapchain_->SetFullscreenState(FALSE, nullptr);
        if (FAILED(hr))
        {
          *error = L"SetFullscreenState(FALSE) failed: " + HrMsg(hr);
          Log("d3d11: %s", Narrow(*error).c_str());
        }
      }
    }
    return true;
  }

  bool CreatePipeline(std::wstring* error)
  {
    ComPtr<ID3DBlob> vsBlob, psSolidBlob, psTextBlob, psBgBlob, psOrbBlob, psRingBlob, errBlob;
    UINT cflags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    cflags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) -> bool {
      errBlob.Reset();
      HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc), "embedded.hlsl", nullptr, nullptr,
                              entry, target, cflags, 0, &out, &errBlob);
      if (FAILED(hr))
      {
        std::string msg = "D3DCompile ";
        msg += entry;
        msg += " failed";
        if (errBlob)
          msg.append(": ").append(static_cast<const char*>(errBlob->GetBufferPointer()),
                                  errBlob->GetBufferSize());
        *error = std::wstring(msg.begin(), msg.end());
        return false;
      }
      return true;
    };

    if (!compile("VSMain", "vs_4_0", vsBlob))
      return false;
    if (!compile("PSSolid", "ps_4_0", psSolidBlob))
      return false;
    if (!compile("PSText", "ps_4_0", psTextBlob))
      return false;
    if (!compile("PSBackground", "ps_4_0", psBgBlob))
      return false;
    if (!compile("PSOrb", "ps_4_0", psOrbBlob))
      return false;
    if (!compile("PSRing", "ps_4_0", psRingBlob))
      return false;

    HRESULT hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                             nullptr, &vs_);
    if (FAILED(hr))
    {
      *error = L"CreateVertexShader failed";
      return false;
    }
    hr = device_->CreatePixelShader(psSolidBlob->GetBufferPointer(), psSolidBlob->GetBufferSize(),
                                    nullptr, &psSolid_);
    if (FAILED(hr))
    {
      *error = L"CreatePixelShader solid failed";
      return false;
    }
    hr = device_->CreatePixelShader(psTextBlob->GetBufferPointer(), psTextBlob->GetBufferSize(),
                                    nullptr, &psText_);
    if (FAILED(hr))
    {
      *error = L"CreatePixelShader text failed";
      return false;
    }
    hr = device_->CreatePixelShader(psBgBlob->GetBufferPointer(), psBgBlob->GetBufferSize(),
                                    nullptr, &psBg_);
    if (FAILED(hr))
    {
      *error = L"CreatePixelShader bg failed";
      return false;
    }
    hr = device_->CreatePixelShader(psOrbBlob->GetBufferPointer(), psOrbBlob->GetBufferSize(),
                                    nullptr, &psOrb_);
    if (FAILED(hr))
    {
      *error = L"CreatePixelShader orb failed";
      return false;
    }
    hr = device_->CreatePixelShader(psRingBlob->GetBufferPointer(), psRingBlob->GetBufferSize(),
                                    nullptr, &psRing_);
    if (FAILED(hr))
    {
      *error = L"CreatePixelShader ring failed";
      return false;
    }

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device_->CreateInputLayout(ied, _countof(ied), vsBlob->GetBufferPointer(),
                                    vsBlob->GetBufferSize(), &layout_);
    if (FAILED(hr))
    {
      *error = L"CreateInputLayout failed";
      return false;
    }

    // Unit quad centered at origin, size 1x1 (scale in transform). Two tris.
    const Vertex verts[] = {
        {-0.5f, -0.5f, 0.f, 0.f}, {0.5f, -0.5f, 1.f, 0.f}, {0.5f, 0.5f, 1.f, 1.f},
        {-0.5f, -0.5f, 0.f, 0.f}, {0.5f, 0.5f, 1.f, 1.f},  {-0.5f, 0.5f, 0.f, 1.f},
    };
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = verts;
    hr = device_->CreateBuffer(&bd, &sd, &vb_);
    if (FAILED(hr))
    {
      *error = L"CreateBuffer vb failed";
      return false;
    }

    bd = {};
    bd.ByteWidth = sizeof(CBData);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&bd, nullptr, &cb_);
    if (FAILED(hr))
    {
      *error = L"CreateBuffer cb failed";
      return false;
    }

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&samp, &samp_);
    if (FAILED(hr))
    {
      *error = L"CreateSamplerState failed";
      return false;
    }

    // Alpha blend for text / solid bars.
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = device_->CreateBlendState(&bl, &blend_);
    if (FAILED(hr))
    {
      *error = L"CreateBlendState failed";
      return false;
    }

    // Additive for glowing orbs / rings.
    D3D11_BLEND_DESC ba = bl;
    ba.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    ba.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    ba.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    hr = device_->CreateBlendState(&ba, &blendAdd_);
    if (FAILED(hr))
    {
      *error = L"CreateBlendState additive failed";
      return false;
    }

    return true;
  }

  bool CreateFontTexture(std::wstring* error)
  {
    // Atlas: 16 cols x 6 rows of 8x8 glyphs covering ASCII 32..127.
    constexpr int cols = 16;
    constexpr int rows = 6;
    constexpr int aw = cols * font8x8::kGlyphW;
    constexpr int ah = rows * font8x8::kGlyphH;
    std::vector<uint8_t> pixels(static_cast<size_t>(aw * ah), 0);

    for (int gi = 0; gi < font8x8::kCount; ++gi)
    {
      const int gx = (gi % cols) * font8x8::kGlyphW;
      const int gy = (gi / cols) * font8x8::kGlyphH;
      const uint8_t* g = font8x8::kGlyphs[gi];
      for (int row = 0; row < font8x8::kGlyphH; ++row)
      {
        const uint8_t bits = g[row];
        for (int col = 0; col < font8x8::kGlyphW; ++col)
        {
          const bool on = (bits >> col) & 1;
          pixels[static_cast<size_t>((gy + row) * aw + (gx + col))] = on ? 255 : 0;
        }
      }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = aw;
    td.Height = ah;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = aw;
    HRESULT hr = device_->CreateTexture2D(&td, &sd, &fontTex_);
    if (FAILED(hr))
    {
      *error = L"CreateTexture2D font failed";
      return false;
    }
    hr = device_->CreateShaderResourceView(fontTex_.Get(), nullptr, &fontSrv_);
    if (FAILED(hr))
    {
      *error = L"CreateSRV font failed";
      return false;
    }
    atlasW_ = aw;
    atlasH_ = ah;
    return true;
  }

  void UpdateCB(const float* mvp, float r, float g, float b, float a)
  {
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
      return;
    auto* cb = static_cast<CBData*>(map.pData);
    std::memcpy(cb->transform, mvp, 16 * sizeof(float));
    cb->color[0] = r;
    cb->color[1] = g;
    cb->color[2] = b;
    cb->color[3] = a;
    cb->timeRes[0] = timeSec_;
    cb->timeRes[1] = static_cast<float>(width_);
    cb->timeRes[2] = static_cast<float>(height_);
    cb->timeRes[3] = 0.f;
    ctx_->Unmap(cb_.Get(), 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  }

  void DrawUnitQuad(const float* mvp, float r, float g, float b, float a)
  {
    UpdateCB(mvp, r, g, b, a);
    ctx_->Draw(6, 0);
  }

  void DrawTransformed(const float* ortho, float x, float y, float rot, float sx, float sy, float r,
                       float g, float b, float a)
  {
    float R[16], S[16], T[16], tmp[16], world[16], mvp[16];
    MatRotateZ(R, rot);
    MatScale(S, sx, sy);
    MatTranslate(T, x, y);
    MatMul(tmp, R, S);
    MatMul(world, T, tmp);
    MatMul(mvp, ortho, world);
    DrawUnitQuad(mvp, r, g, b, a);
  }

  static void Hsv(float h, float s, float v, float* r, float* g, float* b)
  {
    h = h - std::floor(h);
    const float i = std::floor(h * 6.f);
    const float f = h * 6.f - i;
    const float p = v * (1.f - s);
    const float q = v * (1.f - f * s);
    const float t = v * (1.f - (1.f - f) * s);
    switch (static_cast<int>(i) % 6)
    {
    case 0:
      *r = v;
      *g = t;
      *b = p;
      break;
    case 1:
      *r = q;
      *g = v;
      *b = p;
      break;
    case 2:
      *r = p;
      *g = v;
      *b = t;
      break;
    case 3:
      *r = p;
      *g = q;
      *b = v;
      break;
    case 4:
      *r = t;
      *g = p;
      *b = v;
      break;
    default:
      *r = v;
      *g = p;
      *b = q;
      break;
    }
  }

  void DrawTextPx(const char* text, float x, float y, float scale, float r, float g, float b,
                  float a)
  {
    if (!text || !*text)
      return;

    ctx_->PSSetShader(psText_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, fontSrv_.GetAddressOf());
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx_->OMSetBlendState(blend_.Get(), blendFactor, 0xFFFFFFFF);

    float ortho[16];
    MatOrthoPixels(ortho, static_cast<float>(width_), static_cast<float>(height_));

    constexpr int cols = 16;
    float cx = x;
    for (const char* p = text; *p; ++p)
    {
      unsigned char ch = static_cast<unsigned char>(*p);
      if (ch < 32 || ch > 127)
        ch = '?';
      const int gi = ch - 32;
      const int gx = gi % cols;
      const int gy = gi / cols;
      const float u0 = static_cast<float>(gx * font8x8::kGlyphW) / atlasW_;
      const float v0 = static_cast<float>(gy * font8x8::kGlyphH) / atlasH_;
      const float u1 = static_cast<float>((gx + 1) * font8x8::kGlyphW) / atlasW_;
      const float v1 = static_cast<float>((gy + 1) * font8x8::kGlyphH) / atlasH_;

      // Dynamic VB for this glyph — Map the immutable unit VB won't work.
      // Instead rebuild transform + use a small dynamic path via Draw with updated UV.
      // Easiest: create a tiny dynamic vertex buffer once and rewrite per glyph.
      EnsureDynVb();
      Vertex verts[6] = {
          {0.f, 0.f, u0, v0}, {1.f, 0.f, u1, v0}, {1.f, 1.f, u1, v1},
          {0.f, 0.f, u0, v0}, {1.f, 1.f, u1, v1}, {0.f, 1.f, u0, v1},
      };
      D3D11_MAPPED_SUBRESOURCE map{};
      if (SUCCEEDED(ctx_->Map(dynVb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
      {
        std::memcpy(map.pData, verts, sizeof(verts));
        ctx_->Unmap(dynVb_.Get(), 0);
      }
      UINT stride = sizeof(Vertex);
      UINT offset = 0;
      ctx_->IASetVertexBuffers(0, 1, dynVb_.GetAddressOf(), &stride, &offset);

      float scl[16], tr[16], world[16], mvp[16];
      MatScale(scl, font8x8::kGlyphW * scale, font8x8::kGlyphH * scale);
      MatTranslate(tr, cx, y);
      MatMul(world, tr, scl);
      MatMul(mvp, ortho, world);
      UpdateCB(mvp, r, g, b, a);
      ctx_->Draw(6, 0);

      cx += font8x8::kGlyphW * scale;
    }

    // Restore unit VB for solid draws.
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
  }

  void EnsureDynVb()
  {
    if (dynVb_)
      return;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Vertex) * 6;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&bd, nullptr, &dynVb_);
  }

  HWND hwnd_ = nullptr;
  Config cfg_{};
  int width_ = 0;
  int height_ = 0;
  WindowMode mode_ = WindowMode::Windowed;
  UINT swapFlags_ = 0;
  int atlasW_ = 0;
  int atlasH_ = 0;
  float timeSec_ = 0.f;

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGISwapChain> swapchain_;
  ComPtr<IDXGISwapChain1> swapchain1_;
  ComPtr<ID3D11Texture2D> backbuffer_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> psSolid_;
  ComPtr<ID3D11PixelShader> psText_;
  ComPtr<ID3D11PixelShader> psBg_;
  ComPtr<ID3D11PixelShader> psOrb_;
  ComPtr<ID3D11PixelShader> psRing_;
  ComPtr<ID3D11InputLayout> layout_;
  ComPtr<ID3D11Buffer> vb_;
  ComPtr<ID3D11Buffer> dynVb_;
  ComPtr<ID3D11Buffer> cb_;
  ComPtr<ID3D11SamplerState> samp_;
  ComPtr<ID3D11BlendState> blend_;
  ComPtr<ID3D11BlendState> blendAdd_;
  ComPtr<ID3D11Texture2D> fontTex_;
  ComPtr<ID3D11ShaderResourceView> fontSrv_;
};

} // namespace

std::unique_ptr<IRenderer> CreateD3D11Renderer()
{
  return std::make_unique<D3D11Renderer>();
}
