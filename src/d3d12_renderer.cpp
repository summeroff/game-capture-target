#include "renderer.hpp"

#include "font8x8.hpp"
#include "log.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{

constexpr UINT kFrameCount = 2;

constexpr char kShaderSrc[] = R"(
cbuffer CB : register(b0) {
  float4x4 uTransform;
  float4   uColor;
};
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
VSOut VSMain(VSIn i) {
  VSOut o;
  o.pos = mul(float4(i.pos, 0, 1), uTransform);
  o.uv = i.uv;
  o.col = uColor;
  return o;
}
Texture2D gTex : register(t0);
SamplerState gSamp : register(s0);
float4 PSSolid(VSOut i) : SV_Target { return i.col; }
float4 PSText(VSOut i) : SV_Target {
  float a = gTex.Sample(gSamp, i.uv).r;
  return float4(i.col.rgb, i.col.a * a);
}
)";

struct Vertex
{
  float x, y, u, v;
};

struct CBData
{
  float transform[16];
  float color[4];
};

void MatIdentity(float* m)
{
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = m[5] = m[10] = m[15] = 1.f;
}
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
    for (int r = 0; r < 4; ++r)
      t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                     a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
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
  const float c = std::cos(rad), s = std::sin(rad);
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

class D3D12Renderer final : public IRenderer
{
public:
  ~D3D12Renderer() override { Shutdown(); }

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
    if (!CreateFont(error))
      return false;
    if (!CreateGeometry(error))
      return false;
    return true;
  }

  void Shutdown() override
  {
    // Idempotent: App::~App calls Shutdown then unique_ptr dtor calls it again.
    if (queue_ && fence_ && fenceEvent_)
      WaitGpu();

    for (UINT i = 0; i < kFrameCount; ++i)
      renderTargets_[i].Reset();
    swapchain_.Reset();
    cmdList_.Reset();
    for (auto& a : cmdAlloc_)
      a.Reset();
    pipelineSolid_.Reset();
    pipelineText_.Reset();
    rootSig_.Reset();
    vb_.Reset();
    cb_[0].Reset();
    cb_[1].Reset();
    fontTex_.Reset();
    fontUpload_.Reset();
    srvHeap_.Reset();
    rtvHeap_.Reset();
    queue_.Reset();
    if (fenceEvent_)
    {
      CloseHandle(fenceEvent_);
      fenceEvent_ = nullptr;
    }
    fence_.Reset();
    device_.Reset();
    factory_.Reset();
    frameIndex_ = 0;
    fenceValue_ = 0;
    for (UINT i = 0; i < kFrameCount; ++i)
      fenceValues_[i] = 0;
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
    WaitGpu();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
      renderTargets_[i].Reset();
    }
    if (swapchain_)
    {
      HRESULT hr =
          swapchain_->ResizeBuffers(kFrameCount, width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
      if (FAILED(hr))
      {
        *error = L"ResizeBuffers failed: " + HrMsg(hr);
        return false;
      }
    } else
    {
      return CreateSwapchain(error);
    }
    return CreateRtv(error);
  }

  bool RecreateDevice(std::wstring* error) override
  {
    Log("d3d12: recreating device");
    Config saved = cfg_;
    int w = width_, h = height_;
    WindowMode m = mode_;
    Shutdown();
    cfg_ = saved;
    width_ = w;
    height_ = h;
    mode_ = m;
    return Init(hwnd_, cfg_, error);
  }

  bool SetMode(WindowMode mode, std::wstring* error) override
  {
    mode_ = mode;
    if (!swapchain_)
      return true;
    if (mode == WindowMode::FullscreenExclusive)
    {
      HRESULT hr = swapchain_->SetFullscreenState(TRUE, nullptr);
      if (FAILED(hr))
        Log("d3d12: SetFullscreenState TRUE failed %s", Narrow(HrMsg(hr)).c_str());
    } else
    {
      BOOL fs = FALSE;
      if (SUCCEEDED(swapchain_->GetFullscreenState(&fs, nullptr)) && fs)
        swapchain_->SetFullscreenState(FALSE, nullptr);
    }
    (void)error;
    return true;
  }

  void Render(const FrameInfo& info) override
  {
    if (!device_ || !swapchain_)
      return;
    const UINT fi = frameIndex_ % kFrameCount;
    WaitFrame(fi);

    cmdAlloc_[fi]->Reset();
    cmdList_->Reset(cmdAlloc_[fi].Get(), pipelineSolid_.Get());

    // Barrier to RT
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = renderTargets_[fi].Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList_->ResourceBarrier(1, &bar);

    const float t = static_cast<float>(info.elapsedSec);
    const float clear[4] = {0.12f + 0.12f * std::sin(t * 1.7f),
                            0.10f + 0.10f * std::sin(t * 1.3f + 2.f),
                            0.18f + 0.14f * std::sin(t * 2.1f + 4.f), 1.f};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(fi) * rtvDescriptorSize_;
    cmdList_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList_->ClearRenderTargetView(rtv, clear, 0, nullptr);

    D3D12_VIEWPORT vp{0, 0, float(width_), float(height_), 0, 1};
    D3D12_RECT sc{0, 0, LONG(width_), LONG(height_)};
    cmdList_->RSSetViewports(1, &vp);
    cmdList_->RSSetScissorRects(1, &sc);

    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    cmdList_->SetDescriptorHeaps(1, heaps);
    cmdList_->SetGraphicsRootSignature(rootSig_.Get());
    cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = vbView_;
    cmdList_->IASetVertexBuffers(0, 1, &vbv);

    // rotating quad
    float ortho[16], rot[16], scl[16], tr[16], tmp[16], world[16], mvp[16];
    MatOrthoPixels(ortho, float(width_), float(height_));
    const float cx = width_ * 0.5f, cy = height_ * 0.5f;
    const float orbit = 120.f + 40.f * std::sin(t * 0.8f);
    MatRotateZ(rot, t * 2.2f);
    MatScale(scl, 90.f + 20.f * std::sin(t * 3.f), 90.f + 20.f * std::sin(t * 3.f));
    MatTranslate(tr, cx + std::cos(t * 1.4f) * orbit, cy + std::sin(t * 1.4f) * orbit * 0.55f);
    MatMul(tmp, rot, scl);
    MatMul(world, tr, tmp);
    MatMul(mvp, ortho, world);
    const float qr = 0.55f + 0.45f * std::sin(t * 3.5f);
    const float qg = 0.55f + 0.45f * std::sin(t * 3.5f + 2.1f);
    const float qb = 0.55f + 0.45f * std::sin(t * 3.5f + 4.2f);
    SetCB(fi, mvp, qr, qg, qb, 1.f);
    cmdList_->SetPipelineState(pipelineSolid_.Get());
    cmdList_->SetGraphicsRootConstantBufferView(0, cbGpu_[fi]);
    cmdList_->DrawInstanced(6, 1, 0, 0);

    if (!info.noHud)
    {
      char big[32];
      sprintf_s(big, "%llu", (unsigned long long)info.frameIndex);
      DrawTextLine(fi, big, (width_ - int(strlen(big)) * 8 * 6) * 0.5f, height_ * 0.18f, 6.f, 1.f,
                   1.f, 0.2f, 1.f);

      char line[256];
      float y = 8.f;
      auto hud = [&](const char* s) {
        DrawTextLine(fi, s, 8.f, y, 2.f, 0.95f, 0.95f, 0.95f, 1.f);
        y += 20.f;
      };
      sprintf_s(line, "frame  %llu", (unsigned long long)info.frameIndex);
      hud(line);
      sprintf_s(line, "size   %dx%d", info.clientW, info.clientH);
      hud(line);
      hud("api    d3d12");
      sprintf_s(line, "swap   %s", Narrow(SwapEffectName()).c_str());
      hud(line);
      sprintf_s(line, "present %s", Narrow(PresentModeName()).c_str());
      hud(line);
      sprintf_s(line, "mode   %s", Narrow(WindowModeName(info.mode)).c_str());
      hud(line);
      sprintf_s(line, "class  %s", Narrow(info.windowClass).c_str());
      hud(line);
      sprintf_s(line, "title  %s", Narrow(info.windowTitle).c_str());
      hud(line);
      sprintf_s(line, "pid    %lu", (unsigned long)info.pid);
      hud(line);
      sprintf_s(line, "time   %.2fs", info.elapsedSec);
      hud(line);
    }

    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdList_->ResourceBarrier(1, &bar);
    cmdList_->Close();

    ID3D12CommandList* lists[] = {cmdList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    swapchain_->Present(cfg_.vsync ? 1 : 0, 0);
    fenceValues_[fi] = ++fenceValue_;
    queue_->Signal(fence_.Get(), fenceValues_[fi]);
    frameIndex_++;
  }

  const wchar_t* ApiName() const override { return L"d3d12"; }
  const wchar_t* SwapEffectName() const override
  {
    return cfg_.flipModel ? L"FLIP_DISCARD" : L"FLIP_SEQUENTIAL";
  }
  const wchar_t* PresentModeName() const override { return cfg_.vsync ? L"vsync" : L"immediate"; }

private:
  bool CreateDevice(std::wstring* error)
  {
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
      dbg->EnableDebugLayer();
#endif
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
      *error = L"CreateDXGIFactory1 failed";
      return false;
    }
    factory_ = factory;

    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr))
    {
      *error = L"D3D12CreateDevice failed: " + HrMsg(hr);
      return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    if (FAILED(hr))
    {
      *error = L"CreateCommandQueue failed";
      return false;
    }

    for (UINT i = 0; i < kFrameCount; ++i)
    {
      hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&cmdAlloc_[i]));
      if (FAILED(hr))
      {
        *error = L"CreateCommandAllocator failed";
        return false;
      }
    }
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc_[0].Get(), nullptr,
                                    IID_PPV_ARGS(&cmdList_));
    if (FAILED(hr))
    {
      *error = L"CreateCommandList failed";
      return false;
    }
    cmdList_->Close();

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr))
    {
      *error = L"CreateFence failed";
      return false;
    }
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_)
    {
      *error = L"CreateEventW (fence) failed, GetLastError=" + std::to_wstring(GetLastError());
      return false;
    }
    fenceValue_ = 0;
    for (UINT i = 0; i < kFrameCount; ++i)
      fenceValues_[i] = 0;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = kFrameCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = device_->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap_));
    if (FAILED(hr))
    {
      *error = L"CreateDescriptorHeap RTV failed";
      return false;
    }
    rtvDescriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.NumDescriptors = 1;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device_->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap_));
    if (FAILED(hr))
    {
      *error = L"CreateDescriptorHeap SRV failed";
      return false;
    }

    Log("d3d12: device ok");
    return true;
  }

  bool CreateSwapchain(std::wstring* error)
  {
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = width_;
    scd.Height = height_;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kFrameCount;
    scd.SwapEffect =
        cfg_.flipModel ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.Scaling = DXGI_SCALING_STRETCH;

    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr =
        factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &scd, nullptr, nullptr, &sc1);
    if (FAILED(hr))
    {
      *error = L"CreateSwapChainForHwnd failed: " + HrMsg(hr);
      return false;
    }
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swapchain_);
    frameIndex_ = swapchain_->GetCurrentBackBufferIndex();
    return CreateRtv(error);
  }

  bool CreateRtv(std::wstring* error)
  {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
      HRESULT hr = swapchain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i]));
      if (FAILED(hr))
      {
        *error = L"GetBuffer failed";
        return false;
      }
      device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, handle);
      handle.ptr += rtvDescriptorSize_;
    }
    Log("d3d12: swapchain %dx%d effect=%s", width_, height_, Narrow(SwapEffectName()).c_str());
    return true;
  }

  bool CreatePipeline(std::wstring* error)
  {
    ComPtr<ID3DBlob> vs, psSolid, psText, err;
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& out) -> bool {
      err.Reset();
      HRESULT hr = D3DCompile(kShaderSrc, sizeof(kShaderSrc), "d3d12.hlsl", nullptr, nullptr, entry,
                              target, 0, 0, &out, &err);
      if (FAILED(hr))
      {
        std::string m = "D3DCompile failed ";
        m += entry;
        if (err)
          m.append(": ").append((const char*)err->GetBufferPointer());
        *error = std::wstring(m.begin(), m.end());
        return false;
      }
      return true;
    };
    if (!compile("VSMain", "vs_5_0", vs))
      return false;
    if (!compile("PSSolid", "ps_5_0", psSolid))
      return false;
    if (!compile("PSText", "ps_5_0", psText))
      return false;

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samp.MaxLOD = D3D12_FLOAT32_MAX;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, rsErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr);
    if (FAILED(hr))
    {
      *error = L"SerializeRootSignature failed";
      return false;
    }
    hr = device_->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&rootSig_));
    if (FAILED(hr))
    {
      *error = L"CreateRootSignature failed";
      return false;
    }

    D3D12_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
    };

    auto makePso = [&](ID3DBlob* ps, bool blend, ComPtr<ID3D12PipelineState>& out) -> bool {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
      pd.pRootSignature = rootSig_.Get();
      pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
      pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
      pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
      if (blend)
      {
        pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
      }
      pd.SampleMask = UINT_MAX;
      pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      pd.DepthStencilState.DepthEnable = FALSE;
      pd.InputLayout = {ied, _countof(ied)};
      pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      pd.NumRenderTargets = 1;
      pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
      pd.SampleDesc.Count = 1;
      HRESULT h = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out));
      if (FAILED(h))
      {
        *error = L"CreateGraphicsPipelineState failed";
        return false;
      }
      return true;
    };
    if (!makePso(psSolid.Get(), false, pipelineSolid_))
      return false;
    if (!makePso(psText.Get(), true, pipelineText_))
      return false;
    return true;
  }

  bool CreateFont(std::wstring* error)
  {
    constexpr int cols = 16, rows = 6;
    constexpr int aw = cols * 8, ah = rows * 8;
    std::vector<uint8_t> pixels(aw * ah, 0);
    for (int gi = 0; gi < font8x8::kCount; ++gi)
    {
      const int gx = (gi % cols) * 8, gy = (gi / cols) * 8;
      const uint8_t* g = font8x8::kGlyphs[gi];
      for (int row = 0; row < 8; ++row)
        for (int col = 0; col < 8; ++col)
          pixels[(gy + row) * aw + (gx + col)] = (g[row] >> col) & 1 ? 255 : 0;
    }
    atlasW_ = aw;
    atlasH_ = ah;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = aw;
    td.Height = ah;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&fontTex_));
    if (FAILED(hr))
    {
      *error = L"Create font tex failed";
      return false;
    }

    UINT64 uploadSize = 0;
    device_->GetCopyableFootprints(&td, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
    D3D12_RESOURCE_DESC ub = {};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = uploadSize;
    ub.Height = 1;
    ub.DepthOrArraySize = 1;
    ub.MipLevels = 1;
    ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES up = {D3D12_HEAP_TYPE_UPLOAD};
    hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                          IID_PPV_ARGS(&fontUpload_));
    if (FAILED(hr))
    {
      *error = L"Create font upload failed";
      return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT numRows = 0;
    UINT64 rowSize = 0, total = 0;
    device_->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowSize, &total);
    uint8_t* dst = nullptr;
    fontUpload_->Map(0, nullptr, reinterpret_cast<void**>(&dst));
    for (UINT y = 0; y < numRows; ++y)
      memcpy(dst + fp.Offset + y * fp.Footprint.RowPitch, pixels.data() + y * aw, aw);
    fontUpload_->Unmap(0, nullptr);

    cmdAlloc_[0]->Reset();
    cmdList_->Reset(cmdAlloc_[0].Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = fontTex_.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = fontUpload_.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = fp;
    cmdList_->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = fontTex_.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList_->ResourceBarrier(1, &b);
    cmdList_->Close();
    ID3D12CommandList* lists[] = {cmdList_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    WaitGpu();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(fontTex_.Get(), &srv,
                                      srvHeap_->GetCPUDescriptorHandleForHeapStart());
    return true;
  }

  bool CreateGeometry(std::wstring* error)
  {
    // Unit quad + room for dynamic text quads in one upload buffer rewritten each frame?
    // Static unit quad only; text uses same unit quad with different CB/UV via root constants —
    // UV baked in verts so text needs dynamic VB. Create upload VB large enough for 512 glyphs.
    const UINT vbBytes = sizeof(Vertex) * 6 * 512;
    D3D12_HEAP_PROPERTIES up = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = vbBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&vb_));
    if (FAILED(hr))
    {
      *error = L"Create VB failed";
      return false;
    }
    vbView_.BufferLocation = vb_->GetGPUVirtualAddress();
    vbView_.StrideInBytes = sizeof(Vertex);
    vbView_.SizeInBytes = vbBytes;

    // Seed unit quad at start
    Vertex unit[6] = {{-0.5f, -0.5f, 0, 0}, {0.5f, -0.5f, 1, 0}, {0.5f, 0.5f, 1, 1},
                      {-0.5f, -0.5f, 0, 0}, {0.5f, 0.5f, 1, 1},  {-0.5f, 0.5f, 0, 1}};
    void* p = nullptr;
    vb_->Map(0, nullptr, &p);
    memcpy(p, unit, sizeof(unit));
    vb_->Unmap(0, nullptr);

    for (UINT i = 0; i < kFrameCount; ++i)
    {
      D3D12_RESOURCE_DESC cbd = bd;
      cbd.Width = (sizeof(CBData) + 255) & ~255;
      hr = device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &cbd,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&cb_[i]));
      if (FAILED(hr))
      {
        *error = L"Create CB failed";
        return false;
      }
      cbGpu_[i] = cb_[i]->GetGPUVirtualAddress();
    }
    return true;
  }

  void SetCB(UINT fi, const float* mvp, float r, float g, float b, float a)
  {
    CBData* p = nullptr;
    cb_[fi]->Map(0, nullptr, reinterpret_cast<void**>(&p));
    memcpy(p->transform, mvp, 16 * sizeof(float));
    p->color[0] = r;
    p->color[1] = g;
    p->color[2] = b;
    p->color[3] = a;
    cb_[fi]->Unmap(0, nullptr);
  }

  void DrawTextLine(UINT fi, const char* text, float x, float y, float scale, float r, float g,
                    float b, float a)
  {
    if (!text || !*text)
      return;
    cmdList_->SetPipelineState(pipelineText_.Get());
    cmdList_->SetGraphicsRootDescriptorTable(1, srvHeap_->GetGPUDescriptorHandleForHeapStart());

    float ortho[16];
    MatOrthoPixels(ortho, float(width_), float(height_));
    constexpr int cols = 16;
    float cx = x;
    // rebuild verts after unit quad (offset 6)
    Vertex* mapped = nullptr;
    vb_->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    int giBase = 6;
    int count = 0;
    for (const char* p = text; *p && count < 500; ++p, ++count)
    {
      unsigned char ch = (unsigned char)*p;
      if (ch < 32 || ch > 127)
        ch = '?';
      const int gi = ch - 32;
      const int gx = gi % cols, gy = gi / cols;
      const float u0 = float(gx * 8) / atlasW_, v0 = float(gy * 8) / atlasH_;
      const float u1 = float((gx + 1) * 8) / atlasW_, v1 = float((gy + 1) * 8) / atlasH_;
      const float x0 = cx, y0 = y, x1 = cx + 8 * scale, y1 = y + 8 * scale;
      Vertex* v = mapped + giBase + count * 6;
      v[0] = {x0, y0, u0, v0};
      v[1] = {x1, y0, u1, v0};
      v[2] = {x1, y1, u1, v1};
      v[3] = {x0, y0, u0, v0};
      v[4] = {x1, y1, u1, v1};
      v[5] = {x0, y1, u0, v1};
      cx += 8 * scale;
    }
    vb_->Unmap(0, nullptr);

    float id[16];
    MatIdentity(id);
    // positions already in pixels — use ortho only
    float mvp[16];
    MatMul(mvp, ortho, id);
    // Actually verts are in pixel space already with identity world; ortho maps pixels->ndc.
    // Our MatOrtho expects world then ortho*world. verts are pixel coords so world=I works if
    // vertex shader does mul(float4(pos,0,1), mvp) with ortho.
    SetCB(fi, ortho, r, g, b, a);
    // Wait - unit quad uses -0.5..0.5 with world transform. Text verts are absolute pixels.
    // ortho alone: x' = x*2/w - 1. Good for pixel coords.
    cmdList_->SetGraphicsRootConstantBufferView(0, cbGpu_[fi]);
    cmdList_->DrawInstanced(6 * count, 1, giBase, 0);

    // restore solid pipeline for subsequent solid draws is caller's job; we leave text.
    cmdList_->SetPipelineState(pipelineSolid_.Get());
  }

  void WaitGpu()
  {
    if (!queue_ || !fence_ || !fenceEvent_)
      return;
    const UINT64 v = ++fenceValue_;
    if (FAILED(queue_->Signal(fence_.Get(), v)))
      return;
    if (fence_->GetCompletedValue() < v)
    {
      if (SUCCEEDED(fence_->SetEventOnCompletion(v, fenceEvent_)))
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
  }

  void WaitFrame(UINT fi)
  {
    if (!fence_ || !fenceEvent_)
      return;
    if (fence_->GetCompletedValue() < fenceValues_[fi])
    {
      if (SUCCEEDED(fence_->SetEventOnCompletion(fenceValues_[fi], fenceEvent_)))
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
  }

  HWND hwnd_ = nullptr;
  Config cfg_{};
  int width_ = 0, height_ = 0, atlasW_ = 0, atlasH_ = 0;
  WindowMode mode_ = WindowMode::Windowed;
  UINT frameIndex_ = 0;
  UINT rtvDescriptorSize_ = 0;
  UINT64 fenceValue_ = 0;
  UINT64 fenceValues_[kFrameCount]{};
  HANDLE fenceEvent_ = nullptr;
  D3D12_VERTEX_BUFFER_VIEW vbView_{};
  D3D12_GPU_VIRTUAL_ADDRESS cbGpu_[kFrameCount]{};

  ComPtr<IDXGIFactory4> factory_;
  ComPtr<ID3D12Device> device_;
  ComPtr<ID3D12CommandQueue> queue_;
  ComPtr<IDXGISwapChain3> swapchain_;
  ComPtr<ID3D12DescriptorHeap> rtvHeap_;
  ComPtr<ID3D12DescriptorHeap> srvHeap_;
  ComPtr<ID3D12Resource> renderTargets_[kFrameCount];
  ComPtr<ID3D12CommandAllocator> cmdAlloc_[kFrameCount];
  ComPtr<ID3D12GraphicsCommandList> cmdList_;
  ComPtr<ID3D12Fence> fence_;
  ComPtr<ID3D12RootSignature> rootSig_;
  ComPtr<ID3D12PipelineState> pipelineSolid_;
  ComPtr<ID3D12PipelineState> pipelineText_;
  ComPtr<ID3D12Resource> vb_;
  ComPtr<ID3D12Resource> cb_[kFrameCount];
  ComPtr<ID3D12Resource> fontTex_;
  ComPtr<ID3D12Resource> fontUpload_;
};

} // namespace

std::unique_ptr<IRenderer> CreateD3D12Renderer()
{
  return std::make_unique<D3D12Renderer>();
}
