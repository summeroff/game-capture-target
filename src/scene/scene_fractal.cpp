#include "scene/scene.hpp"
#include "scene/scene_math.hpp"

#include <cmath>

#include <Windows.h>

namespace scene
{
namespace
{

// Fullscreen raymarch stress scene. Seed picks FractalA vs FractalB.
// Overlay a few material prims so solid/tri shaders still get exercised.
class FractalScene final : public IScene
{
public:
  void Reset(const SceneConfig& cfg, int w, int h) override
  {
    cfg_ = cfg;
    w_ = w > 0 ? w : 1;
    h_ = h > 0 ? h : 1;
    t_ = 0.f;
    rng_ = Rng(cfg.seed ? cfg.seed : 0xF7A27u);
    // Even seed → A (tunnel), odd → B (spiral).
    useB_ = (cfg.seed & 1u) != 0;
  }

  void Resize(int w, int h) override
  {
    w_ = w > 0 ? w : 1;
    h_ = h > 0 ? h : 1;
  }

  void Update(double elapsedSec, float /*dt*/, int w, int h) override
  {
    w_ = w > 0 ? w : 1;
    h_ = h > 0 ? h : 1;
    t_ = static_cast<float>(elapsedSec);
  }

  void Emit(SceneDraw& out) override
  {
    out.backdrop = useB_ ? BackdropId::FractalB : BackdropId::FractalA;
    out.clearR = 0.01f;
    out.clearG = 0.0f;
    out.clearB = 0.02f;
    out.flashA = 0.f;

    const float cx = w_ * 0.5f;
    const float cy = h_ * 0.5f;

    // A few orbiting material triangles + solids for shader coverage over the PS.
    for (int i = 0; i < 6; ++i)
    {
      const float a = t_ * (0.4f + i * 0.07f) + i * 1.047f;
      const float rr = 80.f + i * 28.f;
      const float x = cx + std::cos(a) * rr;
      const float y = cy + std::sin(a * 1.3f) * rr * 0.55f;
      float r, g, b;
      Hsv(0.08f + i * 0.11f + t_ * 0.02f, 0.75f, 0.95f, &r, &g, &b);
      out.prims.push_back(MakeTri(x, y, 28.f + i * 3.f, 40.f + i * 2.f, a + 1.2f, r, g, b, 0.9f));
    }

    // Soft orbs as spark accents
    for (int i = 0; i < 10; ++i)
    {
      const float a = -t_ * 0.55f + i * 0.628f;
      const float rr = 200.f + std::sin(t_ + i) * 40.f;
      const float x = cx + std::cos(a) * rr;
      const float y = cy + std::sin(a) * rr * 0.5f;
      float r, g, b;
      Hsv(0.55f + i * 0.07f, 0.6f, 1.f, &r, &g, &b);
      out.prims.push_back(MakeOrb(x, y, 18.f + (i % 3) * 6.f, r, g, b, 0.55f));
    }

    wchar_t l1[96];
    swprintf_s(l1, L"FRACTAL %s  seed 0x%08X", useB_ ? L"B-spiral" : L"A-tunnel", cfg_.seed);
    out.hud.line1 = l1;
    out.hud.line2 = L"twigl-style raymarch backdrop (d3d11)";
  }

  const wchar_t* Name() const override { return L"fractal"; }
  SceneId Id() const override { return SceneId::Fractal; }

private:
  SceneConfig cfg_{};
  int w_ = 1280;
  int h_ = 720;
  float t_ = 0.f;
  Rng rng_{1};
  bool useB_ = false;
};

} // namespace

std::unique_ptr<IScene> CreateFractalScene()
{
  return std::make_unique<FractalScene>();
}

} // namespace scene
