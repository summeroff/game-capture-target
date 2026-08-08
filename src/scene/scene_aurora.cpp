#include "scene/scene.hpp"
#include "scene/scene_math.hpp"

#include <cmath>

namespace scene
{
namespace
{

// Extracted from the historical d3d11 aurora Render() body.
// Motion is a pure function of elapsedSec (same formulas as before the draw-list split).
class AuroraScene final : public IScene
{
public:
  void Reset(const SceneConfig& cfg, int w, int h) override
  {
    cfg_ = cfg;
    w_ = w > 0 ? w : 1;
    h_ = h > 0 ? h : 1;
    t_ = 0.f;
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
    out.backdrop = BackdropId::Aurora;
    out.clearR = 0.01f;
    out.clearG = 0.01f;
    out.clearB = 0.03f;
    out.flashA = 0.f;
    out.hud = {};
    out.prims.clear();
    out.prims.reserve(128);

    const float t = t_;
    const float cx = w_ * 0.5f;
    const float cy = h_ * 0.55f;

    // Outer spinning ring of orbs
    constexpr int kOrbs = 14;
    for (int i = 0; i < kOrbs; ++i)
    {
      const float a0 = t * 0.7f + i * (kTau / kOrbs);
      const float radius = 180.f + 40.f * std::sin(t * 1.1f + i * 0.4f);
      const float ox = cx + std::cos(a0) * radius;
      const float oy = cy + std::sin(a0) * radius * 0.62f;
      const float sz = 28.f + 10.f * std::sin(t * 3.0f + i);
      const float hue = std::fmod(t * 0.08f + i / float(kOrbs), 1.f);
      float rr, gg, bb;
      Hsv(hue, 0.85f, 1.f, &rr, &gg, &bb);
      out.prims.push_back(MakeOrb(ox, oy, sz, rr, gg, bb, 0.9f));
    }

    // Counter-rotating inner ring
    for (int i = 0; i < 8; ++i)
    {
      const float a0 = -t * 1.3f + i * (kTau / 8);
      const float radius = 90.f + 18.f * std::cos(t * 2.0f + i);
      const float ox = cx + std::cos(a0) * radius;
      const float oy = cy + std::sin(a0) * radius * 0.7f;
      float rr, gg, bb;
      Hsv(std::fmod(0.45f + i * 0.08f + t * 0.05f, 1.f), 0.7f, 1.f, &rr, &gg, &bb);
      out.prims.push_back(MakeOrb(ox, oy, 18.f, rr, gg, bb, 0.85f));
    }

    // Big soft rings
    for (int i = 0; i < 3; ++i)
    {
      const float pulse = 1.f + 0.12f * std::sin(t * 2.5f + i);
      const float sz = (220.f + i * 90.f) * pulse;
      float rr, gg, bb;
      Hsv(std::fmod(0.55f + i * 0.12f + t * 0.03f, 1.f), 0.8f, 1.f, &rr, &gg, &bb);
      out.prims.push_back(
          MakeRing(cx, cy, sz, sz * 0.7f, t * (0.4f + i * 0.2f), rr, gg, bb, 0.35f));
    }

    // Comet: bright orb + trailing ghosts
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
        out.prims.push_back(MakeOrb(tx, ty, sz, rr, gg, bb, al));
      }
      out.prims.push_back(MakeOrb(cox, coy, 56.f, 1.f, 0.95f, 0.8f, 1.f));
    }

    // Audio-style bars along the bottom
    constexpr int kBars = 48;
    const float barW = w_ / float(kBars);
    for (int i = 0; i < kBars; ++i)
    {
      const float n = 0.5f + 0.5f * std::sin(t * 4.0f + i * 0.45f) * std::cos(t * 2.3f + i * 0.17f);
      const float bh = (30.f + n * (h_ * 0.22f));
      const float bx = (i + 0.5f) * barW;
      const float by = h_ - bh * 0.5f - 8.f;
      float rr, gg, bb;
      Hsv(std::fmod(i / float(kBars) + t * 0.15f, 1.f), 0.85f, 1.f, &rr, &gg, &bb);
      out.prims.push_back(MakeSolid(bx, by, barW * 0.72f, bh, 0.f, rr, gg, bb, 0.75f));
    }

    // Spinning diamond core
    {
      const float ang = t * 1.8f;
      const float sz = 70.f + 15.f * std::sin(t * 4.0f);
      float rr, gg, bb;
      Hsv(std::fmod(t * 0.2f, 1.f), 0.6f, 1.f, &rr, &gg, &bb);
      out.prims.push_back(MakeSolid(cx, cy, sz, sz, ang, rr, gg, bb, 0.9f));
      out.prims.push_back(
          MakeSolid(cx, cy, sz * 0.55f, sz * 0.55f, ang + 0.785f, 1.f, 1.f, 1.f, 0.7f));
    }
  }

  const wchar_t* Name() const override { return L"aurora"; }
  SceneId Id() const override { return SceneId::Aurora; }

private:
  SceneConfig cfg_{};
  int w_ = 1280;
  int h_ = 720;
  float t_ = 0.f;
};

} // namespace

std::unique_ptr<IScene> CreateAuroraScene()
{
  return std::make_unique<AuroraScene>();
}

} // namespace scene
