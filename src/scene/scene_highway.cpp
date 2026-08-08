#include "scene/scene.hpp"
#include "scene/scene_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace scene
{
namespace
{

constexpr float kFixedDt = 1.f / 120.f;
constexpr int kStarCount = 48;
constexpr int kMaxCars = 14;
constexpr int kMaxPosts = 20;
constexpr int kMaxStreaks = 36;
constexpr int kMaxMountains = 18;
constexpr float kZNear = 1.2f;
constexpr float kZFar = 90.f;
constexpr float kRoadHalf = 1.35f; // world units half-width (~3 lanes)
constexpr float kLaneW = (kRoadHalf * 2.f) / 3.f;
// Keep cam/posts/cars Z from growing without bound (float cancel on z - camZ).
constexpr float kCamZWrap = 4096.f;
// Projected object scale divisor: screen size ≈ (focal / relZ) / kScaleDiv.
constexpr float kScaleDiv = 40.f;

struct Star
{
  float x; // 0..1 across sky
  float y; // 0..1 of sky band
  float phase = 0.f;
  float speed = 1.f;
  float bright = 1.f;
  float size = 2.f;
};

struct Car
{
  bool alive = false;
  float x = 0.f;     // world lateral
  float z = 0.f;     // world depth
  float speed = 0.f; // world units / sec (absolute along road)
  float w = 0.9f;
  float h = 0.55f;
  float r = 1.f, g = 0.3f, b = 0.2f;
  int lane = 0;
  bool oncoming = false;
};

struct Post
{
  float z = 0.f;
  bool left = true;
};

struct Streak
{
  bool alive = false;
  float x = 0.f;
  float y = 0.f;
  float len = 20.f;
  float life = 0.f;
  float maxLife = 0.25f;
  float r = 0.4f, g = 0.8f, b = 1.f;
};

struct Mountain
{
  float x = 0.f; // screen-normalized around horizon (-1..1-ish)
  float peak = 40.f;
  float base = 80.f;
  float shade = 0.5f;
};

class HighwayScene final : public IScene
{
public:
  void Reset(const SceneConfig& cfg, int w, int h) override
  {
    cfg_ = cfg;
    w_ = w > 0 ? w : 1;
    h_ = h > 0 ? h : 1;
    rng_.SetSeed(cfg.seed);
    accum_ = 0.f;
    t_ = 0.f;
    camZ_ = 0.f;
    speedKmh_ = 118.f + rng_.Range(-8.f, 18.f);
    targetSpeed_ = speedKmh_;
    playerLane_ = 1; // center of 0,1,2
    playerX_ = LaneX(playerLane_);
    playerBob_ = 0.f;
    flash_ = 0.f;
    odo_ = rng_.Range(1200.f, 9800.f);
    spawnTimer_ = 0.4f;
    laneChangeTimer_ = rng_.Range(2.5f, 5.f);
    laneTarget_ = playerLane_;

    stars_.resize(kStarCount);
    for (auto& s : stars_)
    {
      s.x = rng_.NextFloat();
      s.y = rng_.NextFloat();
      s.phase = rng_.Range(0.f, kTau);
      s.speed = rng_.Range(3.f, 12.f);
      s.bright = rng_.Range(0.35f, 1.f);
      s.size = rng_.Range(1.5f, 3.2f);
    }

    mountains_.resize(kMaxMountains);
    for (int i = 0; i < kMaxMountains; ++i)
    {
      mountains_[i].x = -1.15f + (2.3f * i) / float(kMaxMountains - 1) + rng_.Range(-0.04f, 0.04f);
      mountains_[i].peak = rng_.Range(28.f, 95.f);
      mountains_[i].base = mountains_[i].peak * rng_.Range(1.6f, 2.4f);
      mountains_[i].shade = rng_.Range(0.12f, 0.38f);
    }

    posts_.resize(kMaxPosts);
    for (int i = 0; i < kMaxPosts; ++i)
    {
      posts_[i].z = camZ_ + 4.f + i * 6.5f;
      posts_[i].left = (i & 1) == 0;
    }

    cars_.assign(kMaxCars, {});
    for (int i = 0; i < 6; ++i)
      SpawnCar(true);

    streaks_.assign(kMaxStreaks, {});
  }

  void Resize(int w, int h) override
  {
    w_ = w > 0 ? w : 1;
    h_ = h > 0 ? h : 1;
  }

  void Update(double elapsedSec, float dt, int w, int h) override
  {
    w_ = w > 0 ? w : 1;
    h_ = h > 0 ? h : 1;
    t_ = static_cast<float>(elapsedSec);
    if (dt < 0.f)
      dt = 0.f;
    if (dt > 0.1f)
      dt = 0.1f;
    accum_ += dt;
    int guard = 0;
    while (accum_ >= kFixedDt && guard++ < 8)
    {
      Step(kFixedDt);
      accum_ -= kFixedDt;
    }
  }

  void Emit(SceneDraw& out) override
  {
    // Night highway — solid dark indigo, never aurora PS.
    out.backdrop = BackdropId::Solid;
    out.clearR = 0.02f;
    out.clearG = 0.03f;
    out.clearB = 0.08f;
    out.flashR = 1.f;
    out.flashG = 0.85f;
    out.flashB = 0.55f;
    out.flashA = Clamp(flash_, 0.f, 1.f);
    out.prims.clear();
    out.prims.reserve(420);

    const float cx = w_ * 0.5f;
    const float horizonY = h_ * 0.40f;
    const float focal = ComputeFocal(horizonY);

    // Sky gradient bands (hard quads — readable silhouette)
    out.prims.push_back(
        MakeSolid(cx, horizonY * 0.35f, float(w_), horizonY * 0.7f, 0.f, 0.04f, 0.05f, 0.14f, 1.f));
    out.prims.push_back(MakeSolid(cx, horizonY * 0.78f, float(w_), horizonY * 0.45f, 0.f, 0.06f,
                                  0.04f, 0.12f, 1.f));

    // Stars above horizon
    for (const auto& s : stars_)
    {
      const float tw = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(t_ * s.speed + s.phase));
      const float sx = s.x * w_;
      const float sy = s.y * (horizonY - 8.f);
      out.prims.push_back(MakeOrb(sx, sy, 3.f + s.size, 0.75f, 0.85f, 1.f, s.bright * tw * 0.9f));
    }

    // Moon
    {
      const float mx = w_ * 0.78f;
      const float my = horizonY * 0.38f;
      out.prims.push_back(MakeOrb(mx, my, 54.f, 0.55f, 0.6f, 0.85f, 0.25f));
      out.prims.push_back(MakeOrb(mx, my, 34.f, 0.92f, 0.94f, 1.f, 0.95f));
      out.prims.push_back(MakeOrb(mx - 6.f, my - 4.f, 10.f, 0.7f, 0.75f, 0.9f, 0.5f));
    }

    // Mountain silhouettes along horizon (solid tris)
    for (const auto& m : mountains_)
    {
      const float baseX = cx + m.x * (w_ * 0.48f);
      const float peakH = m.peak * (h_ / 720.f);
      const float baseW = m.base * (w_ / 1280.f);
      const float shade = m.shade;
      out.prims.push_back(MakeTri(baseX, horizonY - peakH * 0.45f, baseW, peakH, -kPi * 0.5f,
                                  shade * 0.35f, shade * 0.4f, shade * 0.55f, 1.f));
    }

    // Horizon glow line
    out.prims.push_back(
        MakeLine(0.f, horizonY, float(w_), horizonY, 2.f, 0.35f, 0.25f, 0.55f, 0.65f));
    out.prims.push_back(
        MakeSolid(cx, horizonY + 3.f, float(w_), 8.f, 0.f, 0.12f, 0.06f, 0.18f, 0.55f));

    // Ground fill below horizon (desert shoulder)
    out.prims.push_back(MakeSolid(cx, (horizonY + h_) * 0.5f, float(w_), h_ - horizonY, 0.f, 0.05f,
                                  0.045f, 0.06f, 1.f));

    // Perspective road body as stacked horizontal strips (near → far)
    constexpr int kStrips = 28;
    for (int i = 0; i < kStrips; ++i)
    {
      const float u0 = float(i) / float(kStrips);
      const float u1 = float(i + 1) / float(kStrips);
      // denser strips near camera via nonlinear z
      const float z0 = ZFromU(u0);
      const float z1 = ZFromU(u1);
      float xL0, y0, s0, xR0, y0b, s0b;
      float xL1, y1, s1, xR1, y1b, s1b;
      if (!Project(-kRoadHalf, z0, cx, horizonY, focal, &xL0, &y0, &s0))
        continue;
      if (!Project(kRoadHalf, z0, cx, horizonY, focal, &xR0, &y0b, &s0b))
        continue;
      if (!Project(-kRoadHalf, z1, cx, horizonY, focal, &xL1, &y1, &s1))
        continue;
      if (!Project(kRoadHalf, z1, cx, horizonY, focal, &xR1, &y1b, &s1b))
        continue;
      (void)y0b;
      (void)y1b;
      const float yMid = 0.5f * (y0 + y1);
      const float yH = std::max(1.5f, std::fabs(y0 - y1) + 1.f);
      const float xMid = 0.5f * (0.5f * (xL0 + xR0) + 0.5f * (xL1 + xR1));
      const float xW = std::max(std::fabs(xR0 - xL0), std::fabs(xR1 - xL1));
      // Asphalt + slight depth shading
      const float shade = Lerp(0.16f, 0.06f, u0);
      out.prims.push_back(
          MakeSolid(xMid, yMid, xW, yH, 0.f, shade * 0.85f, shade * 0.88f, shade, 1.f));
    }

    // Road edge lines + lane dashes (world-scrolled)
    DrawLaneMarkings(out, cx, horizonY, focal);

    // Shoulder rumble strips (outer neon edges)
    DrawRoadEdges(out, cx, horizonY, focal);

    // Light posts
    for (const auto& p : posts_)
    {
      const float wx = p.left ? -(kRoadHalf + 0.35f) : (kRoadHalf + 0.35f);
      float sx, sy, sc;
      if (!Project(wx, p.z - camZ_, cx, horizonY, focal, &sx, &sy, &sc))
        continue;
      if (sy < horizonY + 2.f)
        continue;
      // Near posts explode in screen size — clamp so shoulders stay readable.
      const float psc = Clamp(sc, 0.05f, 1.35f);
      const float poleH = 70.f * psc;
      const float poleW = std::max(2.f, 4.f * psc);
      out.prims.push_back(
          MakeSolid(sx, sy - poleH * 0.5f, poleW, poleH, 0.f, 0.3f, 0.32f, 0.4f, 0.95f));
      // Lamp head + glow
      const float lx = sx + (p.left ? 12.f : -12.f) * psc;
      const float ly = sy - poleH;
      out.prims.push_back(MakeSolid(lx, ly, 16.f * psc, 6.f * psc, 0.f, 1.f, 0.92f, 0.55f, 1.f));
      out.prims.push_back(MakeOrb(lx, ly + 4.f * psc, 18.f * psc, 1.f, 0.9f, 0.45f, 0.28f));
      // Soft pool on road (keep modest)
      out.prims.push_back(MakeOrb(sx + (p.left ? 22.f : -22.f) * psc, sy - 2.f * psc, 26.f * psc,
                                  1.f, 0.85f, 0.4f, 0.1f));
    }

    // Traffic
    for (const auto& c : cars_)
    {
      if (!c.alive)
        continue;
      float sx, sy, sc;
      if (!Project(c.x, c.z - camZ_, cx, horizonY, focal, &sx, &sy, &sc))
        continue;
      if (sy < horizonY + 4.f)
        continue;
      const float csc = Clamp(sc, 0.04f, 2.2f);
      const float bw = std::max(8.f, c.w * 34.f * csc);
      const float bh = std::max(5.f, c.h * 34.f * csc);
      // body
      out.prims.push_back(MakeSolid(sx, sy - bh * 0.55f, bw, bh, 0.f, c.r, c.g, c.b, 1.f));
      // cabin
      out.prims.push_back(MakeSolid(sx, sy - bh * 1.05f, bw * 0.55f, bh * 0.55f, 0.f, c.r * 0.55f,
                                    c.g * 0.55f, c.b * 0.65f, 1.f));
      // headlights / taillights
      if (c.oncoming)
      {
        out.prims.push_back(
            MakeOrb(sx - bw * 0.28f, sy - bh * 0.35f, 6.f * csc, 1.f, 1.f, 0.75f, 0.95f));
        out.prims.push_back(
            MakeOrb(sx + bw * 0.28f, sy - bh * 0.35f, 6.f * csc, 1.f, 1.f, 0.75f, 0.95f));
        out.prims.push_back(MakeOrb(sx, sy - bh * 0.3f, 16.f * csc, 1.f, 0.95f, 0.7f, 0.16f));
      } else
      {
        out.prims.push_back(
            MakeOrb(sx - bw * 0.28f, sy - bh * 0.2f, 5.f * csc, 1.f, 0.15f, 0.1f, 0.95f));
        out.prims.push_back(
            MakeOrb(sx + bw * 0.28f, sy - bh * 0.2f, 5.f * csc, 1.f, 0.15f, 0.1f, 0.95f));
      }
    }

    // Speed streaks (screen-space, near bottom thirds)
    for (const auto& s : streaks_)
    {
      if (!s.alive)
        continue;
      const float u = Clamp(s.life / s.maxLife, 0.f, 1.f);
      out.prims.push_back(MakeLine(s.x, s.y, s.x, s.y + s.len, 2.f, s.r, s.g, s.b, u * 0.85f));
    }

    // Player craft (near, bottom center-ish)
    {
      float sx, sy, sc;
      // Fixed near depth so craft stays readable
      if (Project(playerX_, kZNear + 0.55f, cx, horizonY, focal, &sx, &sy, &sc))
      {
        const float psc = Clamp(sc, 0.35f, 1.6f);
        const float bob = std::sin(playerBob_) * 4.f;
        const float py = sy + bob;
        const float bw = 58.f * psc;
        const float bh = 24.f * psc;
        // glow pad
        out.prims.push_back(MakeOrb(sx, py + 6.f, 48.f * psc, 0.2f, 0.9f, 1.f, 0.22f));
        // body wedge — tip toward horizon
        out.prims.push_back(
            MakeTri(sx, py - bh * 0.2f, bw, bh * 1.6f, -kPi * 0.5f, 0.15f, 0.85f, 1.f, 1.f));
        out.prims.push_back(
            MakeTri(sx, py - bh * 0.15f, bw * 0.45f, bh, -kPi * 0.5f, 0.9f, 0.95f, 1.f, 1.f));
        out.prims.push_back(
            MakeSolid(sx, py + bh * 0.15f, bw * 0.85f, bh * 0.45f, 0.f, 0.1f, 0.55f, 0.75f, 1.f));
        // thruster
        out.prims.push_back(MakeOrb(sx, py + bh * 0.55f, 14.f * psc, 1.f, 0.55f, 0.15f,
                                    0.55f + 0.35f * std::sin(t_ * 30.f)));
      }
    }

    // Speedometer arc (bottom-right, compact — not a center HUD plate)
    DrawSpeedo(out);

    // Tiny top-left scene banner only (app HUD plate is separate)
    wchar_t buf[128];
    swprintf_s(buf, L"HIGHWAY  %d KM/H  LANE %d  ODO %.0f", int(speedKmh_ + 0.5f), playerLane_ + 1,
               odo_);
    out.hud.line1 = buf;
    swprintf_s(buf, L"traffic %d  posts %d", CountCars(), kMaxPosts);
    out.hud.line2 = buf;
  }

  const wchar_t* Name() const override { return L"highway"; }
  SceneId Id() const override { return SceneId::Highway; }

private:
  static float LaneX(int lane)
  {
    // lanes 0,1,2 → left,center,right
    const float left = -kRoadHalf + kLaneW * 0.5f;
    return left + kLaneW * float(Clamp(float(lane), 0.f, 2.f));
  }

  float ComputeFocal(float horizonY) const
  {
    // At z=kZNear, ground point should land near bottom of screen.
    const float target = (h_ - 18.f) - horizonY;
    return (target * kZNear); // camH = 1
  }

  static float ZFromU(float u)
  {
    // u=0 near, u=1 far — geometric spacing
    const float a = kZNear;
    const float b = kZFar;
    return a * std::pow(b / a, Clamp(u, 0.f, 1.f));
  }

  bool Project(float worldX, float relZ, float cx, float horizonY, float focal, float* sx,
               float* sy, float* scale) const
  {
    if (relZ < 0.65f || relZ > kZFar * 1.05f)
      return false;
    const float inv = focal / relZ;
    *sx = cx + worldX * inv;
    *sy = horizonY + inv; // camH=1
    // Pixel size factor from perspective: (focal/z) / kScaleDiv.
    *scale = inv / kScaleDiv;
    if (*sy < horizonY - 5.f || *sy > h_ + 40.f)
      return false;
    if (*sx < -80.f || *sx > w_ + 80.f)
      return false;
    return true;
  }

  void DrawRoadEdges(SceneDraw& out, float cx, float horizonY, float focal)
  {
    constexpr int kSeg = 20;
    for (int side = -1; side <= 1; side += 2)
    {
      const float wx = side * kRoadHalf;
      float prevX = 0.f, prevY = 0.f;
      bool have = false;
      for (int i = 0; i <= kSeg; ++i)
      {
        const float z = ZFromU(float(i) / float(kSeg));
        float sx, sy, sc;
        if (!Project(wx, z, cx, horizonY, focal, &sx, &sy, &sc))
        {
          have = false;
          continue;
        }
        if (have)
        {
          const float a = Lerp(0.95f, 0.25f, float(i) / float(kSeg));
          const float th = Clamp(2.5f * sc * 12.f, 2.f, 7.f);
          out.prims.push_back(MakeLine(prevX, prevY, sx, sy, th, 0.2f, 0.95f, 1.f, a));
        }
        prevX = sx;
        prevY = sy;
        have = true;
      }
    }
  }

  void DrawLaneMarkings(SceneDraw& out, float cx, float horizonY, float focal)
  {
    // Two dashed lane dividers between 3 lanes
    const float dashLen = 2.2f;
    const float gap = 2.0f;
    const float period = dashLen + gap;
    const float scroll = std::fmod(camZ_, period);

    for (int laneDiv = 1; laneDiv <= 2; ++laneDiv)
    {
      const float wx = -kRoadHalf + kLaneW * float(laneDiv);
      // Place dashes from near to far in world Z relative to camera
      for (float z = scroll - period; z < kZFar; z += period)
      {
        if (z + dashLen < kZNear * 0.8f)
          continue;
        float x0, y0, s0, x1, y1, s1;
        if (!Project(wx, std::max(z, kZNear * 0.9f), cx, horizonY, focal, &x0, &y0, &s0))
          continue;
        if (!Project(wx, z + dashLen, cx, horizonY, focal, &x1, &y1, &s1))
          continue;
        if (y0 <= horizonY + 1.f && y1 <= horizonY + 1.f)
          continue;
        const float midZ = z + dashLen * 0.5f;
        const float a = Clamp(1.1f - midZ / kZFar, 0.2f, 1.f);
        const float th = Clamp(3.f * s0 * 14.f, 2.f, 8.f);
        out.prims.push_back(MakeLine(x0, y0, x1, y1, th, 1.f, 0.92f, 0.45f, a));
      }
    }

    // Center glow path (subtle)
    float prevX = 0.f, prevY = 0.f;
    bool have = false;
    for (int i = 0; i <= 16; ++i)
    {
      const float z = ZFromU(float(i) / 16.f);
      float sx, sy, sc;
      if (!Project(0.f, z, cx, horizonY, focal, &sx, &sy, &sc))
      {
        have = false;
        continue;
      }
      if (have)
        out.prims.push_back(MakeLine(prevX, prevY, sx, sy, 1.5f, 0.4f, 0.7f, 1.f, 0.12f));
      prevX = sx;
      prevY = sy;
      have = true;
    }
  }

  void DrawSpeedo(SceneDraw& out)
  {
    const float cx = w_ - 110.f;
    const float cy = h_ - 90.f;
    const float radius = 48.f;
    // Plate
    out.prims.push_back(MakeSolid(cx, cy, 120.f, 100.f, 0.f, 0.02f, 0.03f, 0.06f, 0.55f));
    out.prims.push_back(MakeRing(cx, cy, radius * 2.f, radius * 2.f, 0.f, 0.2f, 0.85f, 1.f, 0.55f));
    out.prims.push_back(
        MakeRing(cx, cy, radius * 1.55f, radius * 1.55f, 0.f, 0.1f, 0.4f, 0.55f, 0.35f));

    // Arc ticks via short lines
    const float start = kPi * 0.85f; // left-down
    const float end = kPi * 0.15f;   // right-down (going clockwise through top)
    // sweep from start down through top: use angles from 210° to -30° equivalent
    constexpr int kTicks = 13;
    for (int i = 0; i < kTicks; ++i)
    {
      const float u = float(i) / float(kTicks - 1);
      // angle: pi*0.75 (upper-left) to -pi*0.05 via top
      const float ang = Lerp(kPi * 0.75f, -kPi * 0.05f, u);
      const float ca = std::cos(ang);
      const float sa = std::sin(ang);
      const float r0 = radius * 0.72f;
      const float r1 = radius * ((i % 3) == 0 ? 0.95f : 0.88f);
      out.prims.push_back(MakeLine(cx + ca * r0, cy - sa * r0, cx + ca * r1, cy - sa * r1, 2.f,
                                   0.5f, 0.9f, 1.f, 0.8f));
    }

    // Needle (0..220 km/h)
    const float u = Clamp(speedKmh_ / 220.f, 0.f, 1.f);
    const float nang = Lerp(kPi * 0.75f, -kPi * 0.05f, u);
    const float nx = std::cos(nang);
    const float ny = std::sin(nang);
    out.prims.push_back(MakeLine(cx - nx * 8.f, cy + ny * 8.f, cx + nx * radius * 0.78f,
                                 cy - ny * radius * 0.78f, 3.f, 1.f, 0.35f, 0.15f, 1.f));
    out.prims.push_back(MakeOrb(cx, cy, 8.f, 1.f, 0.9f, 0.85f, 1.f));

    // Mini speed bars
    const int bars = int(Clamp(speedKmh_ / 20.f, 0.f, 10.f));
    for (int i = 0; i < bars; ++i)
    {
      float rr, gg, bb;
      Hsv(0.55f - i * 0.04f, 0.85f, 1.f, &rr, &gg, &bb);
      out.prims.push_back(
          MakeSolid(cx - 40.f + i * 9.f, cy + 38.f, 7.f, 8.f + i * 1.5f, 0.f, rr, gg, bb, 0.9f));
    }
    (void)start;
    (void)end;
  }

  int CountCars() const
  {
    int n = 0;
    for (const auto& c : cars_)
      if (c.alive)
        ++n;
    return n;
  }

  void Step(float dt)
  {
    if (flash_ > 0.f)
      flash_ = std::max(0.f, flash_ - dt / 0.12f);

    // Speed wander
    if (std::fabs(speedKmh_ - targetSpeed_) < 0.5f)
      targetSpeed_ = rng_.Range(95.f, 165.f);
    speedKmh_ = Lerp(speedKmh_, targetSpeed_, 0.35f * dt);
    const float worldSpeed = speedKmh_ * 0.045f; // scale km/h → world units/s
    camZ_ += worldSpeed * dt;
    // Rebase absolute Z so long runs don't lose float precision on (z - camZ_).
    if (camZ_ >= kCamZWrap)
    {
      camZ_ -= kCamZWrap;
      for (auto& p : posts_)
        p.z -= kCamZWrap;
      for (auto& c : cars_)
      {
        if (c.alive)
          c.z -= kCamZWrap;
      }
    }
    odo_ += speedKmh_ * dt / 3600.f * 100.f; // arbitrary progress units
    playerBob_ += dt * (8.f + speedKmh_ * 0.04f);

    // Occasional lane change
    laneChangeTimer_ -= dt;
    if (laneChangeTimer_ <= 0.f)
    {
      laneChangeTimer_ = rng_.Range(2.2f, 5.5f);
      int delta = rng_.RangeInt(-1, 1);
      laneTarget_ = int(Clamp(float(playerLane_ + delta), 0.f, 2.f));
    }
    {
      const float tx = LaneX(laneTarget_);
      playerX_ = Lerp(playerX_, tx, 3.5f * dt);
      if (std::fabs(playerX_ - tx) < 0.04f)
        playerLane_ = laneTarget_;
    }

    // Posts recycle ahead of camera
    for (auto& p : posts_)
    {
      if (p.z - camZ_ < kZNear * 0.7f)
      {
        float maxZ = camZ_;
        for (const auto& q : posts_)
          maxZ = std::max(maxZ, q.z);
        p.z = maxZ + 6.5f;
        p.left = !p.left;
      }
    }

    // Cars
    for (auto& c : cars_)
    {
      if (!c.alive)
        continue;
      // oncoming: move toward camera (decreasing z), traffic: slower than cam or slight
      if (c.oncoming)
        c.z -= c.speed * dt;
      else
        c.z += c.speed * dt;

      const float rel = c.z - camZ_;
      if (rel < 0.5f || rel > kZFar + 5.f)
      {
        c.alive = false;
        continue;
      }

      // Near-miss flash vs player
      if (rel > kZNear && rel < kZNear + 2.2f)
      {
        if (std::fabs(c.x - playerX_) < 0.55f)
        {
          flash_ = 1.f;
          // nudge traffic aside for screensaver continuity
          c.x += (c.x >= playerX_ ? 0.35f : -0.35f);
        }
      }
    }

    spawnTimer_ -= dt;
    if (spawnTimer_ <= 0.f)
    {
      spawnTimer_ = rng_.Range(0.45f, 1.1f);
      SpawnCar(false);
    }

    // Streaks
    for (auto& s : streaks_)
    {
      if (!s.alive)
        continue;
      s.y += (220.f + speedKmh_ * 1.5f) * dt;
      s.life -= dt;
      if (s.life <= 0.f || s.y > h_ + 10.f)
        s.alive = false;
    }
    // Spawn a few streaks each step based on speed
    const float streakRate = speedKmh_ * 0.08f;
    if (rng_.NextFloat() < streakRate * dt)
      SpawnStreak();
    if (rng_.NextFloat() < streakRate * 0.5f * dt)
      SpawnStreak();
  }

  void SpawnCar(bool initial)
  {
    Car* slot = nullptr;
    for (auto& c : cars_)
    {
      if (!c.alive)
      {
        slot = &c;
        break;
      }
    }
    if (!slot)
      return;

    const bool oncoming = rng_.NextFloat() < 0.45f;
    int lane = rng_.RangeInt(0, 2);
    // keep player lane slightly less crowded ahead
    if (!oncoming && lane == playerLane_ && rng_.NextFloat() < 0.5f)
      lane = (lane + 1) % 3;

    slot->alive = true;
    slot->lane = lane;
    slot->x = LaneX(lane) + rng_.Range(-0.08f, 0.08f);
    slot->oncoming = oncoming;
    if (initial)
      slot->z = camZ_ + rng_.Range(8.f, 70.f);
    else if (oncoming)
      slot->z = camZ_ + rng_.Range(55.f, 85.f);
    else
      slot->z = camZ_ + rng_.Range(50.f, 88.f);

    slot->speed = oncoming ? rng_.Range(18.f, 32.f) : rng_.Range(2.f, 12.f);
    slot->w = rng_.Range(0.75f, 1.15f);
    slot->h = rng_.Range(0.4f, 0.7f);
    if (oncoming)
    {
      Hsv(rng_.Range(0.0f, 0.15f), 0.75f, 1.f, &slot->r, &slot->g, &slot->b);
    } else
    {
      Hsv(rng_.Range(0.5f, 0.75f), 0.65f, 0.95f, &slot->r, &slot->g, &slot->b);
    }
  }

  void SpawnStreak()
  {
    Streak* slot = nullptr;
    for (auto& s : streaks_)
    {
      if (!s.alive)
      {
        slot = &s;
        break;
      }
    }
    if (!slot)
      return;
    slot->alive = true;
    // Concentrate streaks along road corridor in screen space
    const float roadLeft = w_ * 0.22f;
    const float roadRight = w_ * 0.78f;
    slot->x = rng_.Range(roadLeft, roadRight);
    slot->y = h_ * rng_.Range(0.45f, 0.75f);
    slot->len = rng_.Range(18.f, 55.f) * (speedKmh_ / 120.f);
    slot->maxLife = rng_.Range(0.12f, 0.28f);
    slot->life = slot->maxLife;
    if (rng_.NextFloat() < 0.35f)
    {
      slot->r = 1.f;
      slot->g = 0.85f;
      slot->b = 0.35f;
    } else
    {
      slot->r = 0.35f;
      slot->g = 0.85f;
      slot->b = 1.f;
    }
  }

  SceneConfig cfg_{};
  Rng rng_{};
  int w_ = 1280;
  int h_ = 720;
  float t_ = 0.f;
  float accum_ = 0.f;
  float camZ_ = 0.f;
  float speedKmh_ = 120.f;
  float targetSpeed_ = 120.f;
  float playerX_ = 0.f;
  float playerBob_ = 0.f;
  int playerLane_ = 1;
  int laneTarget_ = 1;
  float laneChangeTimer_ = 3.f;
  float spawnTimer_ = 0.5f;
  float flash_ = 0.f;
  float odo_ = 0.f;

  std::vector<Star> stars_;
  std::vector<Mountain> mountains_;
  std::vector<Post> posts_;
  std::vector<Car> cars_;
  std::vector<Streak> streaks_;
};

} // namespace

std::unique_ptr<IScene> CreateHighwayScene()
{
  return std::make_unique<HighwayScene>();
}

} // namespace scene
