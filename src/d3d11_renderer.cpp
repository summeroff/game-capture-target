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
    float4   uTimeRes;  // x=time sec, y=width, z=height, w=matMode (0=quad edge, 1=tri bary)
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
    // Column-vector convention: CPU builds M for p' = M * p (column-major).
    // Must be mul(M, v) — mul(v, M) drops translation and pins every prim at screen center.
    o.pos = mul(uTransform, float4(i.pos, 0.0f, 1.0f));
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

float2 rot2(float2 p, float a)
{
    float c = cos(a), s = sin(a);
    return float2(c * p.x - s * p.y, s * p.x + c * p.y);
}

// Rodrigues rotation of p around unit axis n by angle a.
float3 rot3(float3 p, float a, float3 n)
{
    n = normalize(n);
    float c = cos(a), s = sin(a);
    return p * c + cross(n, p) * s + n * dot(n, p) * (1.0 - c);
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

    // Slow color nebula (extra octave for denser material feel)
    float n1 = sin(p.x * 3.1 + t * 0.35) * cos(p.y * 2.7 - t * 0.28);
    float n2 = sin((p.x + p.y) * 4.2 - t * 0.55) * 0.5;
    float n3 = cos(p.x * 1.7 - p.y * 2.3 + t * 0.2);
    float n4 = sin(p.x * 7.3 - p.y * 5.1 + t * 0.9) * cos(p.y * 6.2 + t * 0.4);
    float neb = saturate(0.55 + 0.45 * (n1 + n2 * 0.7 + n3 * 0.4 + n4 * 0.25));
    col += hsv2rgb(frac(0.62 + t * 0.02 + neb * 0.15), 0.65, 0.22) * neb;
    col += hsv2rgb(frac(0.85 + neb * 0.2 - t * 0.015), 0.55, 0.12) * saturate(n4);

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

// Twigl-style log-space tunnel raymarch (variant A). Heavy on purpose.
float4 PSFractalA(VSOut i) : SV_Target
{
    float t = uTimeRes.x;
    float2 r = float2(max(uTimeRes.y, 1.0), max(uTimeRes.z, 1.0));
    // FC.xy from UV; match twigl: (FC*2-r)/r.x
    float2 FC = i.uv * r;
    float3 d = float3((FC * 2.0 - r) / r.x * 0.3 + float2(0.0, 1.0), 1.0);
    float3 q = float3(0.0, -1.0, -1.0);
    float3 o = 0.0;
    float e = 0.0;
    float g = 0.0;
    float R = 0.0;
    [loop] for (int ii = 0; ii < 72; ++ii)
    {
        float i = (float)(ii + 1);
        e += i / 9e9;
        o.rgb += hsv2rgb(0.1, saturate(0.5 + 0.5 * q.y), min(e * i, 0.01));
        float s = 3.0;
        q += d * e * R * 0.25;
        float3 p = q;
        g += p.y / s;
        R = length(p);
        p = float3(log2(max(R, 1e-5)) + t * 0.2, exp2(fmod(-p.z, s) / max(R, 1e-5)) - 0.3, p.x);
        // pack z as original p (swizzle abuse in source uses p as mixed) — keep energy
        p.z = q.z;
        p.y -= 1.0;
        e = p.y;
        [loop] for (int k = 0; k < 12; ++k)
        {
            e += -abs(dot(sin(p.xz * s), cos(p.zy * s)) / s * 0.4);
            s += s;
            if (s >= 6000.0)
                break;
        }
    }
    o.rgb = saturate(o.rgb * 1.15);
    return float4(o.rgb, 1.0);
}

// Twigl-style spiral raymarch (variant B).
float4 PSFractalB(VSOut i) : SV_Target
{
    float t = uTimeRes.x;
    float2 r = float2(max(uTimeRes.y, 1.0), max(uTimeRes.z, 1.0));
    float2 FC = i.uv * r;
    float3 d = float3(FC / r - float2(0.5, -0.3), 0.8);
    float3 q = float3(0.0, -1.0, -1.0);
    float3 o = 0.0;
    float e = 0.0;
    float R = 0.0;
    [loop] for (int ii = 0; ii < 80; ++ii)
    {
        float i = (float)(ii + 1);
        float s = 1.0;
        // Accumulate glow; clamp per-step so late frames don't white-out
        o.rgb += hsv2rgb(frac(0.08 + 0.02 * q.z + t * 0.02), 0.7,
                         min(max(e, 0.0) * s + 0.0015 * i, 0.55) / 28.0);
        q += d * max(e, 0.002) * max(R, 0.15) * 0.22;
        float3 p = q;
        R = length(p);
        p = float3(log(max(R, 1e-5)) - t * 0.8, exp(0.8 - p.z / max(R, 1e-5)), atan2(p.y, p.x) + t * 0.4);
        p.y -= 1.0;
        e = p.y;
        [loop] for (int k = 0; k < 10; ++k)
        {
            e += dot(sin(p.yzz * s) - 0.5, 0.8 - sin(p.zxx * s)) / s * 0.3;
            s += s;
            if (s >= 300.0)
                break;
        }
    }
    // Floor + mild boost — keep spiral structure without crushing to black/white
    o.rgb = o.rgb * 1.35 + float3(0.015, 0.02, 0.05);
    float vig = saturate(1.15 - length(i.uv - float2(0.5, 0.45)) * 1.1);
    o.rgb *= 0.55 + 0.55 * vig;
    o.rgb = saturate(o.rgb);
    return float4(o.rgb, 1.0);
}

// Folded starfield / kaleidoscopic space (for Starfield backdrop).
float4 PSStarfold(VSOut i) : SV_Target
{
    float t = uTimeRes.x;
    float2 r = float2(max(uTimeRes.y, 1.0), max(uTimeRes.z, 1.0));
    float2 FC = i.uv * r;
    float3 o = 0.0;
    float g = 0.0;
    [loop] for (int ii = 0; ii < 64; ++ii)
    {
        float i = (float)(ii + 1);
        float2 nd = (FC - 0.5 * r) / r.y * 0.6 + float2(0.0, 1.0);
        float3 p = float3(nd, g - 8.0);
        p = rot3(p, 3.0, float3(0.0, 9.0, -3.0));
        p.xz = rot2(p.xz, t * 0.3);
        float s = 7.0;
        float e = 1.0;
        [loop] for (int k = 0; k < 12; ++k)
        {
            e = 7.0 / max(dot(p, p * 0.47), 1e-4);
            s *= e;
            p = float3(1.0, 4.1, -1.0) - abs(abs(p) * e - float3(3.0, 4.0, 3.0));
        }
        g += p.y / s * 1.4;
        s = log2(max(s, 1e-4)) - g;
        o.rgb += hsv2rgb(s / max(g, 1e-3) * p.z, 0.7, s / 1e2);
    }
    // Deep space floor so empty regions aren't pure black crush
    o.rgb = max(o.rgb, float3(0.01, 0.012, 0.03));
    o.rgb = saturate(o.rgb);
    return float4(o.rgb, 1.0);
}

float4 PSSolid(VSOut i) : SV_Target
{
    return i.col;
}

// Material fill for scene solids/tris: rim, micro-noise, iridescent sheen, soft AA.
// uTimeRes.w selects edge metric: 0 = unit-quad UV edge, 1 = barycentric (triangle verts).
float4 PSMaterial(VSOut i) : SV_Target
{
    float t = uTimeRes.x;
    float2 uv = i.uv;
    float2 p = uv * 2.0 - 1.0;

    float edge;
    if (uTimeRes.w > 0.5)
    {
        // Explicit triangle path: UV is barycentric (tip=1,0 / baseL=0,1 / baseR=0,0).
        float3 bary = float3(uv.x, uv.y, 1.0 - uv.x - uv.y);
        edge = min(bary.x, min(bary.y, bary.z));
    } else
    {
        // Unit-quad path only — never infer tri from UV (x+y<=1 looks like bary).
        edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    }

    float aa = smoothstep(0.0, 0.035, edge);
    float rim = saturate(1.0 - edge * 6.0);
    rim = rim * rim;

    float n = frac(sin(dot(uv * float2(37.1, 53.7) + t * 0.15, float2(12.9898, 78.233))) * 43758.5453);
    float n2 = frac(sin(dot(uv * float2(91.3, 17.2) - t * 0.07, float2(39.346, 11.135))) * 24634.6345);

    float rlen = length(p);
    float fres = pow(saturate(rlen), 2.2);

    float2 lp = float2(sin(t * 0.7), cos(t * 0.55)) * 0.35;
    float spec = pow(saturate(1.0 - length(p - lp) * 1.7), 14.0);
    float spec2 = pow(saturate(1.0 - length(p + lp.yx * float2(-1, 1)) * 2.1), 8.0);

    float3 base = i.col.rgb;
    float3 irid = hsv2rgb(frac(0.55 + rlen * 0.2 + t * 0.03 + n * 0.04), 0.55, 1.0);
    float3 cool = hsv2rgb(frac(0.58 + edge * 0.3 - t * 0.02), 0.35, 1.0);

    float3 c = base * (0.42 + 0.28 * (1.0 - rlen) + 0.08 * n + 0.06 * n2);
    // Soft interior gradient (fake thickness)
    c += base * float3(1.15, 1.05, 0.95) * (0.18 * saturate(0.7 - rlen));
    // Rim light / metal edge
    c += lerp(base, irid, 0.55) * fres * 0.55;
    c += cool * rim * 0.45;
    // Specular lobes
    c += float3(1.0, 0.97, 0.92) * spec * 0.65;
    c += base * spec2 * 0.25;
    // Animated anisotropic streak
    float streak = pow(saturate(1.0 - abs(p.x * 0.9 + sin(p.y * 4.0 + t * 2.0) * 0.08)), 18.0);
    c += lerp(base, float3(1, 1, 1), 0.5) * streak * 0.2;

    float a = i.col.a * aa;
    // Keep nearly-opaque centers; avoid full kill on solid quads with UV corners
    a = max(a, i.col.a * 0.92 * saturate(edge * 20.0 + 0.85));
    a = min(a, i.col.a);
    return float4(c, a);
}

// Soft circular orb / particle (unit quad UV 0..1).
float4 PSOrb(VSOut i) : SV_Target
{
    float t = uTimeRes.x;
    float2 p = i.uv * 2.0 - 1.0;
    float d = length(p);
    float core = saturate(1.0 - d * 1.15);
    float glow = saturate(1.0 - d);
    float a = core * core * core + glow * glow * 0.35;
    // Inner caustic ring
    float ring = exp(-pow((d - 0.35) * 6.0, 2.0)) * 0.35;
    a = saturate(a + ring * 0.5) * i.col.a;
    float3 c = i.col.rgb * (0.45 + 0.55 * core);
    c += i.col.rgb * ring;
    c += hsv2rgb(frac(0.6 + t * 0.05 + d * 0.2), 0.5, 1.0) * core * core * 0.25;
    float tw = 0.85 + 0.15 * sin(t * 8.0 + d * 20.0);
    return float4(c * tw, a);
}

// Ring / hollow disc
float4 PSRing(VSOut i) : SV_Target
{
    float t = uTimeRes.x;
    float2 p = i.uv * 2.0 - 1.0;
    float d = length(p);
    float ring = 1.0 - abs(d - 0.72) * 6.0;
    float a = saturate(ring);
    a = a * a * i.col.a;
    float ang = atan2(p.y, p.x);
    float pulse = 0.75 + 0.25 * sin(ang * 6.0 + t * 3.0);
    float3 c = i.col.rgb * pulse;
    c += hsv2rgb(frac(ang / 6.28318 + t * 0.1), 0.6, 1.0) * saturate(ring) * 0.35;
    return float4(c, a);
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
    if (!AllocateGpuMem(error))
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
    gpuMem_.clear();
    rtv_.Reset();
    backbuffer_.Reset();
    swapchain1_.Reset();
    swapchain_.Reset();
    fontSrv_.Reset();
    fontTex_.Reset();
    samp_.Reset();
    vb_.Reset();
    dynVb_.Reset();
    cb_.Reset();
    layout_.Reset();
    vs_.Reset();
    psSolid_.Reset();
    psMaterial_.Reset();
    psText_.Reset();
    psBg_.Reset();
    psFractalA_.Reset();
    psFractalB_.Reset();
    psStarfold_.Reset();
    psOrb_.Reset();
    psRing_.Reset();
    blend_.Reset();
    blendAdd_.Reset();
    raster_.Reset();
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
    if (!AllocateGpuMem(error))
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

    const scene::SceneDraw* sd = info.sceneDraw;
    const float clearR = sd ? sd->clearR : 0.01f;
    const float clearG = sd ? sd->clearG : 0.01f;
    const float clearB = sd ? sd->clearB : 0.03f;
    const float clear[4] = {clearR, clearG, clearB, 1.f};
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
    if (raster_)
      ctx_->RSSetState(raster_.Get());

    float ortho[16];
    MatOrthoPixels(ortho, static_cast<float>(width_), static_cast<float>(height_));
    const float blendFactor[4] = {0, 0, 0, 0};

    // Backdrop — procedural fullscreen PS when requested.
    const scene::BackdropId bd = sd ? sd->backdrop : scene::BackdropId::Solid;
    bool drewAuroraPs = false;
    bool drewFxPs = false;
    ID3D11PixelShader* bgPs = nullptr;
    if (bd == scene::BackdropId::Aurora)
    {
      bgPs = psBg_.Get();
      drewAuroraPs = true;
    } else if (bd == scene::BackdropId::Starfield)
    {
      bgPs = psStarfold_.Get();
      drewFxPs = true;
    } else if (bd == scene::BackdropId::FractalA)
    {
      bgPs = psFractalA_.Get();
      drewFxPs = true;
    } else if (bd == scene::BackdropId::FractalB)
    {
      bgPs = psFractalB_.Get();
      drewFxPs = true;
    }
    if (bgPs)
    {
      float scl[16], tr[16], world[16], mvp[16];
      MatScale(scl, static_cast<float>(width_), static_cast<float>(height_));
      MatTranslate(tr, width_ * 0.5f, height_ * 0.5f);
      MatMul(world, tr, scl);
      MatMul(mvp, ortho, world);
      ctx_->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
      ctx_->PSSetShader(bgPs, nullptr, 0);
      UpdateCB(mvp, 1, 1, 1, 1);
      ctx_->Draw(6, 0);
    }

    // Scene primitives
    int drew = 0;
    if (sd)
    {
      for (const auto& p : sd->prims)
      {
        DrawPrim(ortho, p);
        ++drew;
      }

      // Fullscreen flash (additive)
      if (sd->flashA > 0.001f)
      {
        ctx_->OMSetBlendState(blendAdd_.Get(), blendFactor, 0xFFFFFFFF);
        ctx_->PSSetShader(psSolid_.Get(), nullptr, 0);
        DrawTransformed(ortho, width_ * 0.5f, height_ * 0.5f, 0.f, float(width_), float(height_),
                        sd->flashR, sd->flashG, sd->flashB, sd->flashA * 0.65f);
      }
    }

    // Ground-truth: what this backend actually submitted (not CLI selection).
    if (info.frameIndex <= 1 || (info.frameIndex % 120) == 0)
    {
      scene::PrimCounts pc{};
      if (sd)
        pc = scene::CountPrims(sd->prims);
      Log("d3d11-draw: scene=%s backdrop=%s auroraPS=%d fxPS=%d clear=%.3f,%.3f,%.3f "
          "submitted_prims=%d/%d [solid=%d orb=%d ring=%d line=%d tri=%d circle=%d] flashA=%.2f",
          Narrow(info.sceneName ? info.sceneName : L"?").c_str(),
          Narrow(scene::BackdropIdName(bd)).c_str(), drewAuroraPs ? 1 : 0, drewFxPs ? 1 : 0, clearR,
          clearG, clearB, drew, pc.total, pc.solid, pc.orb, pc.ring, pc.line, pc.tri, pc.circle,
          sd ? sd->flashA : 0.f);
    }

    // HUD — compact top-left plate (never a giant center block).
    if (!info.noHud)
    {
      const float blendFactorHud[4] = {0, 0, 0, 0};
      ctx_->OMSetBlendState(blend_.Get(), blendFactorHud, 0xFFFFFFFF);
      ctx_->PSSetShader(psSolid_.Get(), nullptr, 0);

      char big[32];
      sprintf_s(big, "%llu", static_cast<unsigned long long>(info.frameIndex));
      const float scale = 6.f;
      const float tw = static_cast<float>(std::strlen(big)) * font8x8::kGlyphW * scale;

      char line[256];
      std::vector<std::string> lines;
      // Keep on-screen HUD short; verbose prim tallies stay in logs.
      sprintf_s(line, "frame %llu  %dx%d  %s", static_cast<unsigned long long>(info.frameIndex),
                info.clientW, info.clientH, Narrow(ApiName()).c_str());
      lines.emplace_back(line);
      sprintf_s(line, "scene %s  seed 0x%08X",
                Narrow(info.sceneName ? info.sceneName : L"?").c_str(), info.sceneSeed);
      lines.emplace_back(line);
      {
        scene::PrimCounts pc{};
        if (sd)
          pc = scene::CountPrims(sd->prims);
        sprintf_s(line, "draw bg=%s auroraPS=%d fxPS=%d n=%d",
                  Narrow(scene::BackdropIdName(bd)).c_str(), drewAuroraPs ? 1 : 0, drewFxPs ? 1 : 0,
                  drew);
        lines.emplace_back(line);
        sprintf_s(line, "prims s%d o%d r%d l%d t%d", pc.solid, pc.orb, pc.ring, pc.line, pc.tri);
        lines.emplace_back(line);
      }
      if (sd && !sd->hud.line1.empty())
        lines.emplace_back(Narrow(sd->hud.line1));
      if (sd && !sd->hud.line2.empty())
        lines.emplace_back(Narrow(sd->hud.line2));
      sprintf_s(line, "F1-F7 Esc  t=%.1fs", info.elapsedSec);
      lines.emplace_back(line);

      const float textScale = 2.f;
      const float lineH = font8x8::kGlyphH * textScale + 3.f;
      const float plateH = 10.f + lineH * float(lines.size()) + 6.f;
      const float plateW = 380.f;
      // Pixel-space rect for plate (axis-aligned, top-left).
      ctx_->PSSetShader(psSolid_.Get(), nullptr, 0);
      DrawPixelRect(ortho, 4.f, 4.f, 4.f + plateW, 4.f + plateH, 0.f, 0.f, 0.f, 0.62f);

      DrawTextPx(big, (width_ - tw) * 0.5f + 2.f, height_ * 0.06f + 2.f, scale, 0.f, 0.f, 0.f,
                 0.55f);
      DrawTextPx(big, (width_ - tw) * 0.5f, height_ * 0.06f, scale, 1.f, 0.95f, 0.35f, 1.f);

      float y = 10.f;
      for (const auto& s : lines)
      {
        DrawTextPx(s.c_str(), 13.f, y + 1.f, textScale, 0.f, 0.f, 0.f, 0.9f);
        DrawTextPx(s.c_str(), 12.f, y, textScale, 0.95f, 0.98f, 1.f, 1.f);
        y += lineH;
      }

      if (info.frameIndex <= 1 || (info.frameIndex % 120) == 0)
      {
        Log("d3d11-hud: lines=%d plate=%.0fx%.0f bigFrame=%s", static_cast<int>(lines.size()),
            plateW, plateH, big);
      }
    }

    // Optional framebuffer dump for visual QA (before Present).
    if (info.dumpFramePath && !dumpedFrame_ && info.frameIndex >= 3)
    {
      if (DumpBackbufferBmp(info.dumpFramePath))
      {
        dumpedFrame_ = true;
        Log("d3d11: dumped framebuffer to %s (%dx%d)", Narrow(info.dumpFramePath).c_str(), width_,
            height_);
      } else
      {
        Log("d3d11: dump-frame FAILED path=%s", Narrow(info.dumpFramePath).c_str());
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
    ComPtr<ID3DBlob> vsBlob, psSolidBlob, psMatBlob, psTextBlob, psBgBlob, psOrbBlob, psRingBlob;
    ComPtr<ID3DBlob> psFABlob, psFBBlob, psStarBlob, errBlob;
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
    if (!compile("PSMaterial", "ps_4_0", psMatBlob))
      return false;
    if (!compile("PSText", "ps_4_0", psTextBlob))
      return false;
    if (!compile("PSBackground", "ps_4_0", psBgBlob))
      return false;
    if (!compile("PSFractalA", "ps_4_0", psFABlob))
      return false;
    if (!compile("PSFractalB", "ps_4_0", psFBBlob))
      return false;
    if (!compile("PSStarfold", "ps_4_0", psStarBlob))
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
    hr = device_->CreatePixelShader(psMatBlob->GetBufferPointer(), psMatBlob->GetBufferSize(),
                                    nullptr, &psMaterial_);
    if (FAILED(hr))
    {
      *error = L"CreatePixelShader material failed";
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
    hr = device_->CreatePixelShader(psFABlob->GetBufferPointer(), psFABlob->GetBufferSize(),
                                    nullptr, &psFractalA_);
    if (FAILED(hr))
    {
      *error = L"CreatePixelShader fractalA failed";
      return false;
    }
    hr = device_->CreatePixelShader(psFBBlob->GetBufferPointer(), psFBBlob->GetBufferSize(),
                                    nullptr, &psFractalB_);
    if (FAILED(hr))
    {
      *error = L"CreatePixelShader fractalB failed";
      return false;
    }
    hr = device_->CreatePixelShader(psStarBlob->GetBufferPointer(), psStarBlob->GetBufferSize(),
                                    nullptr, &psStarfold_);
    if (FAILED(hr))
    {
      *error = L"CreatePixelShader starfold failed";
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

    // 2D UI/scene: never cull (triangles may be CW or CCW depending on aim angle).
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&rd, &raster_);
    if (FAILED(hr))
    {
      *error = L"CreateRasterizerState failed";
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

  void UpdateCB(const float* mvp, float r, float g, float b, float a, float matMode = 0.f)
  {
    D3D11_MAPPED_SUBRESOURCE map{};
    const HRESULT hr = ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
    if (FAILED(hr))
    {
      Log("d3d11: Map CB FAILED hr=0x%08X", static_cast<unsigned>(hr));
      return;
    }
    auto* cb = static_cast<CBData*>(map.pData);
    std::memcpy(cb->transform, mvp, 16 * sizeof(float));
    cb->color[0] = r;
    cb->color[1] = g;
    cb->color[2] = b;
    cb->color[3] = a;
    cb->timeRes[0] = timeSec_;
    cb->timeRes[1] = static_cast<float>(width_);
    cb->timeRes[2] = static_cast<float>(height_);
    cb->timeRes[3] = matMode; // 0=quad edge, 1=tri bary (PSMaterial only)
    ctx_->Unmap(cb_.Get(), 0);
    ctx_->VSSetConstantBuffers(0, 1, cb_.GetAddressOf());
    ctx_->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  }

  void DrawUnitQuad(const float* mvp, float r, float g, float b, float a, float matMode = 0.f)
  {
    // Re-assert full IA state every draw — HUD path was producing center-screen
    // triangles (identity NDC) when VB/topology got left wrong after dyn draws.
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(layout_.Get());
    ctx_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    UpdateCB(mvp, r, g, b, a, matMode);
    ctx_->Draw(6, 0);
  }

  void DrawPrim(const float* ortho, const scene::Prim& p)
  {
    const float blendFactor[4] = {0, 0, 0, 0};
    switch (p.kind)
    {
    case scene::PrimKind::QuadOrb:
      ctx_->OMSetBlendState(blendAdd_.Get(), blendFactor, 0xFFFFFFFF);
      ctx_->PSSetShader(psOrb_.Get(), nullptr, 0);
      DrawTransformed(ortho, p.x, p.y, p.rot, p.w, p.h, p.r, p.g, p.b, p.a);
      break;
    case scene::PrimKind::QuadRing:
      ctx_->OMSetBlendState(blendAdd_.Get(), blendFactor, 0xFFFFFFFF);
      ctx_->PSSetShader(psRing_.Get(), nullptr, 0);
      DrawTransformed(ortho, p.x, p.y, p.rot, p.w, p.h, p.r, p.g, p.b, p.a);
      break;
    case scene::PrimKind::QuadSolid:
      ctx_->OMSetBlendState(blend_.Get(), blendFactor, 0xFFFFFFFF);
      ctx_->PSSetShader(psMaterial_.Get(), nullptr, 0);
      DrawTransformed(ortho, p.x, p.y, p.rot, p.w, p.h, p.r, p.g, p.b, p.a);
      break;
    case scene::PrimKind::Line: {
      ctx_->OMSetBlendState(blend_.Get(), blendFactor, 0xFFFFFFFF);
      ctx_->PSSetShader(psMaterial_.Get(), nullptr, 0);
      const float dx = p.x2 - p.x;
      const float dy = p.y2 - p.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < 0.5f)
        break;
      const float ang = std::atan2(dy, dx);
      const float mx = (p.x + p.x2) * 0.5f;
      const float my = (p.y + p.y2) * 0.5f;
      const float thick = p.w > 0.5f ? p.w : 2.f;
      DrawTransformed(ortho, mx, my, ang, len, thick, p.r, p.g, p.b, p.a);
      break;
    }
    case scene::PrimKind::Triangle: {
      ctx_->OMSetBlendState(blend_.Get(), blendFactor, 0xFFFFFFFF);
      ctx_->PSSetShader(psMaterial_.Get(), nullptr, 0);
      DrawTriangle(ortho, p);
      break;
    }
    case scene::PrimKind::CircleOutline:
      ctx_->OMSetBlendState(blendAdd_.Get(), blendFactor, 0xFFFFFFFF);
      ctx_->PSSetShader(psRing_.Get(), nullptr, 0);
      DrawTransformed(ortho, p.x, p.y, 0.f, p.w * 2.f, p.w * 2.f, p.r, p.g, p.b, p.a);
      break;
    }
  }

  void DrawTriangle(const float* ortho, const scene::Prim& p)
  {
    // Isosceles: tip along +rot (screen +X when rot=0).
    // UVs = barycentric (tip=1,0,0) so PSMaterial gets soft edges + rim.
    EnsureDynVb();
    const float cs = std::cos(p.rot);
    const float sn = std::sin(p.rot);
    const float halfB = p.w * 0.5f;
    const float halfH = p.h * 0.5f;
    const float lx[3] = {halfH, -halfH, -halfH};
    const float ly[3] = {0.f, -halfB, halfB};
    // barycentric UV: tip (1,0), baseL (0,1), baseR (0,0) → z=1-x-y
    const float uu[3] = {1.f, 0.f, 0.f};
    const float vv[3] = {0.f, 1.f, 0.f};
    Vertex verts[6];
    auto put = [&](int i, float lx_, float ly_, float u, float v) {
      const float wx = p.x + lx_ * cs - ly_ * sn;
      const float wy = p.y + lx_ * sn + ly_ * cs;
      verts[i] = {wx, wy, u, v};
    };
    put(0, lx[0], ly[0], uu[0], vv[0]);
    put(1, lx[1], ly[1], uu[1], vv[1]);
    put(2, lx[2], ly[2], uu[2], vv[2]);
    // Duplicate as second tri so Draw(6) works if a caller expects quads.
    verts[3] = verts[0];
    verts[4] = verts[2];
    verts[5] = verts[1];

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(dynVb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
      return;
    std::memcpy(map.pData, verts, sizeof(verts));
    ctx_->Unmap(dynVb_.Get(), 0);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx_->IASetVertexBuffers(0, 1, dynVb_.GetAddressOf(), &stride, &offset);

    float mvp[16];
    std::memcpy(mvp, ortho, sizeof(float) * 16);
    UpdateCB(mvp, p.r, p.g, p.b, p.a, 1.f); // matMode=1 → bary edge
    ctx_->Draw(3, 0);

    // Restore unit VB for subsequent solid/orb/ring draws.
    ctx_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
  }

  void DrawPixelRect(const float* ortho, float x0, float y0, float x1, float y1, float r, float g,
                     float b, float a)
  {
    // Axis-aligned rect in pixel space (top-left origin). Avoids unit-quad path issues.
    EnsureDynVb();
    Vertex verts[6] = {
        {x0, y0, 0.f, 0.f}, {x1, y0, 1.f, 0.f}, {x1, y1, 1.f, 1.f},
        {x0, y0, 0.f, 0.f}, {x1, y1, 1.f, 1.f}, {x0, y1, 0.f, 1.f},
    };
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(dynVb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
      return;
    std::memcpy(map.pData, verts, sizeof(verts));
    ctx_->Unmap(dynVb_.Get(), 0);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx_->IASetVertexBuffers(0, 1, dynVb_.GetAddressOf(), &stride, &offset);

    float mvp[16];
    std::memcpy(mvp, ortho, 16 * sizeof(float));
    UpdateCB(mvp, r, g, b, a);
    ctx_->Draw(6, 0);

    ctx_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);
  }

  void DrawTransformed(const float* ortho, float x, float y, float rot, float sx, float sy, float r,
                       float g, float b, float a)
  {
    // Always bind the unit quad VB — triangle/text path may have left dynVb bound.
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx_->IASetVertexBuffers(0, 1, vb_.GetAddressOf(), &stride, &offset);

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

  bool AllocateGpuMem(std::wstring* error)
  {
    gpuMem_.clear();
    if (cfg_.gpuMemMb <= 0 || !device_)
      return true;

    const size_t totalBytes = static_cast<size_t>(cfg_.gpuMemMb) * 1024ull * 1024ull;
    constexpr size_t kChunk = 64ull * 1024ull * 1024ull; // 64 MiB slabs
    size_t allocated = 0;
    while (allocated < totalBytes)
    {
      size_t n = totalBytes - allocated;
      if (n > kChunk)
        n = kChunk;
      // CreateBuffer ByteWidth is UINT; keep ≤ 64MiB.
      n = (n + 255ull) & ~255ull;
      if (n == 0)
        break;
      if (n > 0xFFFFFFFFull)
      {
        *error = L"gpu-mem chunk too large";
        return false;
      }

      D3D11_BUFFER_DESC bd{};
      bd.ByteWidth = static_cast<UINT>(n);
      bd.Usage = D3D11_USAGE_DEFAULT;
      bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
      ComPtr<ID3D11Buffer> buf;
      const HRESULT hr = device_->CreateBuffer(&bd, nullptr, &buf);
      if (FAILED(hr))
      {
        Log("d3d11: gpu-mem alloc failed at %zu / %d MB (hr=0x%08X)",
            allocated / (1024ull * 1024ull), cfg_.gpuMemMb, static_cast<unsigned>(hr));
        *error = L"GPU memory allocation failed (try a smaller --gpu-mem)";
        gpuMem_.clear();
        return false;
      }
      gpuMem_.push_back(std::move(buf));
      allocated += n;
    }

    Log("d3d11: holding gpu-mem %d MB (%zu buffers, %zu bytes)", cfg_.gpuMemMb, gpuMem_.size(),
        allocated);
    return true;
  }

  bool DumpBackbufferBmp(const wchar_t* path)
  {
    if (!device_ || !ctx_ || !backbuffer_ || !path || !*path)
      return false;

    D3D11_TEXTURE2D_DESC desc{};
    backbuffer_->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.SampleDesc.Count = 1;
    staging.SampleDesc.Quality = 0;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device_->CreateTexture2D(&staging, nullptr, &tex)))
      return false;

    // Resolve MSAA if ever enabled; currently 1x so CopyResource is fine.
    ctx_->CopyResource(tex.Get(), backbuffer_.Get());

    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(tex.Get(), 0, D3D11_MAP_READ, 0, &map)))
      return false;

    const int w = static_cast<int>(desc.Width);
    const int h = static_cast<int>(desc.Height);
    // BMP row pitch must be multiple of 4; 24bpp.
    const int rowBytes = w * 3;
    const int pad = (4 - (rowBytes % 4)) & 3;
    const int stride = rowBytes + pad;
    const int imgSize = stride * h;

#pragma pack(push, 1)
    struct BmpFileHeader
    {
      uint16_t bfType;
      uint32_t bfSize;
      uint16_t bfReserved1;
      uint16_t bfReserved2;
      uint32_t bfOffBits;
    };
    struct BmpInfoHeader
    {
      uint32_t biSize;
      int32_t biWidth;
      int32_t biHeight;
      uint16_t biPlanes;
      uint16_t biBitCount;
      uint32_t biCompression;
      uint32_t biSizeImage;
      int32_t biXPelsPerMeter;
      int32_t biYPelsPerMeter;
      uint32_t biClrUsed;
      uint32_t biClrImportant;
    };
#pragma pack(pop)

    BmpFileHeader fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fh.bfSize = fh.bfOffBits + static_cast<uint32_t>(imgSize);

    BmpInfoHeader ih{};
    ih.biSize = sizeof(BmpInfoHeader);
    ih.biWidth = w;
    ih.biHeight = h; // bottom-up
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = 0;
    ih.biSizeImage = static_cast<uint32_t>(imgSize);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f)
    {
      ctx_->Unmap(tex.Get(), 0);
      return false;
    }

    fwrite(&fh, 1, sizeof(fh), f);
    fwrite(&ih, 1, sizeof(ih), f);

    std::vector<uint8_t> row(static_cast<size_t>(stride), 0);
    // Source is top-down DXGI; BMP wants bottom-up.
    for (int y = h - 1; y >= 0; --y)
    {
      const auto* src = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch;
      for (int x = 0; x < w; ++x)
      {
        // R8G8B8A8_UNORM
        const uint8_t r = src[x * 4 + 0];
        const uint8_t g = src[x * 4 + 1];
        const uint8_t b = src[x * 4 + 2];
        row[static_cast<size_t>(x * 3 + 0)] = b;
        row[static_cast<size_t>(x * 3 + 1)] = g;
        row[static_cast<size_t>(x * 3 + 2)] = r;
      }
      fwrite(row.data(), 1, static_cast<size_t>(stride), f);
    }

    fclose(f);
    ctx_->Unmap(tex.Get(), 0);
    return true;
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
  bool dumpedFrame_ = false;

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> ctx_;
  ComPtr<IDXGISwapChain> swapchain_;
  ComPtr<IDXGISwapChain1> swapchain1_;
  ComPtr<ID3D11Texture2D> backbuffer_;
  ComPtr<ID3D11RenderTargetView> rtv_;
  ComPtr<ID3D11VertexShader> vs_;
  ComPtr<ID3D11PixelShader> psSolid_;
  ComPtr<ID3D11PixelShader> psMaterial_;
  ComPtr<ID3D11PixelShader> psText_;
  ComPtr<ID3D11PixelShader> psBg_;
  ComPtr<ID3D11PixelShader> psFractalA_;
  ComPtr<ID3D11PixelShader> psFractalB_;
  ComPtr<ID3D11PixelShader> psStarfold_;
  ComPtr<ID3D11PixelShader> psOrb_;
  ComPtr<ID3D11PixelShader> psRing_;
  ComPtr<ID3D11InputLayout> layout_;
  ComPtr<ID3D11Buffer> vb_;
  ComPtr<ID3D11Buffer> dynVb_;
  ComPtr<ID3D11Buffer> cb_;
  ComPtr<ID3D11SamplerState> samp_;
  ComPtr<ID3D11BlendState> blend_;
  ComPtr<ID3D11BlendState> blendAdd_;
  ComPtr<ID3D11RasterizerState> raster_;
  ComPtr<ID3D11Texture2D> fontTex_;
  ComPtr<ID3D11ShaderResourceView> fontSrv_;
  std::vector<ComPtr<ID3D11Buffer>> gpuMem_;
};

} // namespace

std::unique_ptr<IRenderer> CreateD3D11Renderer()
{
  return std::make_unique<D3D11Renderer>();
}
