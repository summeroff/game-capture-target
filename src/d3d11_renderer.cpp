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

namespace {

constexpr char kShaderSrc[] = R"(
cbuffer CB : register(b0)
{
    float4x4 uTransform;
    float4   uColor;
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

float4 PSSolid(VSOut i) : SV_Target
{
    return i.col;
}

float4 PSText(VSOut i) : SV_Target
{
    float a = gTex.Sample(gSamp, i.uv).r;
    return float4(i.col.rgb, i.col.a * a);
}
)";

struct Vertex {
  float x, y;
  float u, v;
};

struct CBData {
  float transform[16];
  float color[4];
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
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
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

class D3D11Renderer final : public IRenderer {
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
    if (!device_) {
      *error = L"no device";
      return false;
    }

    if (ctx_)
      ctx_->ClearState();
    rtv_.Reset();
    backbuffer_.Reset();

    if (swapchain_) {
      // Leave exclusive fullscreen before resize if needed.
      BOOL fs = FALSE;
      ComPtr<IDXGIOutput> out;
      if (SUCCEEDED(swapchain_->GetFullscreenState(&fs, &out)) && fs) {
        swapchain_->SetFullscreenState(FALSE, nullptr);
      }

      const DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
      HRESULT hr = swapchain_->ResizeBuffers(static_cast<UINT>(cfg_.buffers),
                                             static_cast<UINT>(width_), static_cast<UINT>(height_),
                                             fmt, swapFlags_);
      if (FAILED(hr)) {
        // Fall back to full recreate.
        swapchain1_.Reset();
        swapchain_.Reset();
        if (!CreateSwapchain(error))
          return false;
      } else {
        if (!CreateRtv(error))
          return false;
      }
    } else {
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

    // Cycling clear color — frozen capture is obvious.
    const float t = static_cast<float>(info.elapsedSec);
    const float r = 0.12f + 0.12f * std::sin(t * 1.7f);
    const float g = 0.10f + 0.10f * std::sin(t * 1.3f + 2.0f);
    const float b = 0.18f + 0.14f * std::sin(t * 2.1f + 4.0f);
    const float clear[4] = {r, g, b, 1.f};
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
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
    ctx_->PSSetShader(psSolid_.Get(), nullptr, 0);
    ctx_->PSSetSamplers(0, 1, samp_.GetAddressOf());

    // Rotating + translating colored quad.
    const float cx = width_ * 0.5f;
    const float cy = height_ * 0.5f;
    const float orbit = 120.f + 40.f * std::sin(t * 0.8f);
    const float ox = cx + std::cos(t * 1.4f) * orbit;
    const float oy = cy + std::sin(t * 1.4f) * orbit * 0.55f;
    const float ang = t * 2.2f;
    const float qs = 90.f + 20.f * std::sin(t * 3.0f);

    float ortho[16], rot[16], scl[16], tr[16], tmp[16], world[16], mvp[16];
    MatOrthoPixels(ortho, static_cast<float>(width_), static_cast<float>(height_));
    MatRotateZ(rot, ang);
    MatScale(scl, qs, qs);
    MatTranslate(tr, ox, oy);
    MatMul(tmp, rot, scl);
    MatMul(world, tr, tmp);
    MatMul(mvp, ortho, world);

    const float qr = 0.55f + 0.45f * std::sin(t * 3.5f);
    const float qg = 0.55f + 0.45f * std::sin(t * 3.5f + 2.1f);
    const float qb = 0.55f + 0.45f * std::sin(t * 3.5f + 4.2f);
    DrawUnitQuad(mvp, qr, qg, qb, 1.f);

    // Large frame counter (center-top-ish) via bitmap font.
    if (!info.noHud) {
      char big[32];
      sprintf_s(big, "%llu", static_cast<unsigned long long>(info.frameIndex));
      const float scale = 6.f;
      const float tw = static_cast<float>(std::strlen(big)) * font8x8::kGlyphW * scale;
      DrawTextPx(big, (width_ - tw) * 0.5f, height_ * 0.18f, scale, 1.f, 1.f, 0.2f, 1.f);

      // HUD block top-left.
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
      sprintf_s(line, "hotkeys F1 mode F2 resize F3 swap F4 device F5 title F6 churn Esc quit");
      lines.emplace_back(line);

      float y = 8.f;
      for (const auto& s : lines) {
        DrawTextPx(s.c_str(), 8.f, y, 2.f, 0.95f, 0.95f, 0.95f, 1.f);
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

  const wchar_t* PresentModeName() const override
  {
    return cfg_.vsync ? L"vsync" : L"immediate";
  }

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
    if (FAILED(hr)) {
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, _countof(levels),
                             D3D11_SDK_VERSION, &device_, &got, &ctx_);
    }
    if (FAILED(hr)) {
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
    if (FAILED(hr)) {
      *error = L"IDXGIDevice QI failed";
      return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDev->GetAdapter(&adapter);
    if (FAILED(hr)) {
      *error = L"GetAdapter failed";
      return false;
    }
    ComPtr<IDXGIFactory2> factory2;
    {
      ComPtr<IDXGIFactory> factory;
      hr = adapter->GetParent(IID_PPV_ARGS(&factory));
      if (FAILED(hr)) {
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

    if (factory2 && cfg_.flipModel) {
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
      if (FAILED(hr)) {
        Log("d3d11: FLIP_DISCARD failed (%s), falling back to DISCARD", Narrow(HrMsg(hr)).c_str());
        cfg_.flipModel = false;
      } else {
        swapchain1_.As(&swapchain_);
      }
    }

    if (!swapchain_) {
      ComPtr<IDXGIFactory> factory;
      hr = adapter->GetParent(IID_PPV_ARGS(&factory));
      if (FAILED(hr)) {
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
      if (FAILED(hr)) {
        *error = L"CreateSwapChain failed: " + HrMsg(hr);
        return false;
      }
      cfg_.flipModel = false;
    }

    // Prevent DXGI from handling Alt-Enter itself.
    {
      ComPtr<IDXGIFactory> factory;
      ComPtr<IDXGIDevice> dev;
      if (SUCCEEDED(device_.As(&dev))) {
        ComPtr<IDXGIAdapter> ad;
        if (SUCCEEDED(dev->GetAdapter(&ad))) {
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
    if (FAILED(hr)) {
      *error = L"GetBuffer failed: " + HrMsg(hr);
      return false;
    }
    hr = device_->CreateRenderTargetView(backbuffer_.Get(), nullptr, &rtv_);
    if (FAILED(hr)) {
      *error = L"CreateRenderTargetView failed: " + HrMsg(hr);
      return false;
    }
    return true;
  }

  bool ApplyFullscreenState(std::wstring* error)
  {
    if (!swapchain_)
      return true;

    if (mode_ == WindowMode::FullscreenExclusive) {
      HRESULT hr = swapchain_->SetFullscreenState(TRUE, nullptr);
      if (FAILED(hr)) {
        *error = L"SetFullscreenState(TRUE) failed: " + HrMsg(hr);
        Log("d3d11: %s", Narrow(*error).c_str());
        // Non-fatal — borderless-like still usable.
        return true;
      }
    } else {
      BOOL fs = FALSE;
      ComPtr<IDXGIOutput> out;
      if (SUCCEEDED(swapchain_->GetFullscreenState(&fs, &out)) && fs) {
        HRESULT hr = swapchain_->SetFullscreenState(FALSE, nullptr);
        if (FAILED(hr)) {
          *error = L"SetFullscreenState(FALSE) failed: " + HrMsg(hr);
          Log("d3d11: %s", Narrow(*error).c_str());
        }
      }
    }
    return true;
  }

  bool CreatePipeline(std::wstring* error)
  {
    ComPtr<ID3DBlob> vsBlob, psSolidBlob, psTextBlob, errBlob;
    UINT cflags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    cflags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) -> bool {
      errBlob.Reset();
      HRESULT hr =
          D3DCompile(kShaderSrc, sizeof(kShaderSrc), "embedded.hlsl", nullptr, nullptr, entry,
                     target, cflags, 0, &out, &errBlob);
      if (FAILED(hr)) {
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

    HRESULT hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                             nullptr, &vs_);
    if (FAILED(hr)) {
      *error = L"CreateVertexShader failed";
      return false;
    }
    hr = device_->CreatePixelShader(psSolidBlob->GetBufferPointer(), psSolidBlob->GetBufferSize(),
                                    nullptr, &psSolid_);
    if (FAILED(hr)) {
      *error = L"CreatePixelShader solid failed";
      return false;
    }
    hr = device_->CreatePixelShader(psTextBlob->GetBufferPointer(), psTextBlob->GetBufferSize(),
                                    nullptr, &psText_);
    if (FAILED(hr)) {
      *error = L"CreatePixelShader text failed";
      return false;
    }

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = device_->CreateInputLayout(ied, _countof(ied), vsBlob->GetBufferPointer(),
                                    vsBlob->GetBufferSize(), &layout_);
    if (FAILED(hr)) {
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
    if (FAILED(hr)) {
      *error = L"CreateBuffer vb failed";
      return false;
    }

    bd = {};
    bd.ByteWidth = sizeof(CBData);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&bd, nullptr, &cb_);
    if (FAILED(hr)) {
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
    if (FAILED(hr)) {
      *error = L"CreateSamplerState failed";
      return false;
    }

    // Alpha blend for text.
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
    if (FAILED(hr)) {
      *error = L"CreateBlendState failed";
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

    for (int gi = 0; gi < font8x8::kCount; ++gi) {
      const int gx = (gi % cols) * font8x8::kGlyphW;
      const int gy = (gi / cols) * font8x8::kGlyphH;
      const uint8_t* g = font8x8::kGlyphs[gi];
      for (int row = 0; row < font8x8::kGlyphH; ++row) {
        const uint8_t bits = g[row];
        for (int col = 0; col < font8x8::kGlyphW; ++col) {
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
    if (FAILED(hr)) {
      *error = L"CreateTexture2D font failed";
      return false;
    }
    hr = device_->CreateShaderResourceView(fontTex_.Get(), nullptr, &fontSrv_);
    if (FAILED(hr)) {
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
    // HLSL float4x4 is column-major; our mats are column-major already.
    std::memcpy(cb->transform, mvp, 16 * sizeof(float));
    cb->color[0] = r;
    cb->color[1] = g;
    cb->color[2] = b;
    cb->color[3] = a;
    ctx_->Unmap(cb_.Get(), 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  }

  void DrawUnitQuad(const float* mvp, float r, float g, float b, float a)
  {
    UpdateCB(mvp, r, g, b, a);
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx_->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
    ctx_->PSSetShader(psSolid_.Get(), nullptr, 0);
    ctx_->Draw(6, 0);
  }

  void DrawTextPx(const char* text, float x, float y, float scale, float r, float g, float b, float a)
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
    for (const char* p = text; *p; ++p) {
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
          {0.f, 0.f, u0, v0},
          {1.f, 0.f, u1, v0},
          {1.f, 1.f, u1, v1},
          {0.f, 0.f, u0, v0},
          {1.f, 1.f, u1, v1},
          {0.f, 1.f, u0, v1},
      };
      D3D11_MAPPED_SUBRESOURCE map{};
      if (SUCCEEDED(ctx_->Map(dynVb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
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

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGISwapChain> swapchain_;
  ComPtr<IDXGISwapChain1> swapchain1_;
  ComPtr<ID3D11Texture2D> backbuffer_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> psSolid_;
  ComPtr<ID3D11PixelShader> psText_;
  ComPtr<ID3D11InputLayout> layout_;
  ComPtr<ID3D11Buffer> vb_;
  ComPtr<ID3D11Buffer> dynVb_;
  ComPtr<ID3D11Buffer> cb_;
  ComPtr<ID3D11SamplerState> samp_;
  ComPtr<ID3D11BlendState> blend_;
  ComPtr<ID3D11Texture2D> fontTex_;
  ComPtr<ID3D11ShaderResourceView> fontSrv_;
};

} // namespace

std::unique_ptr<IRenderer> CreateD3D11Renderer()
{
  return std::make_unique<D3D11Renderer>();
}
