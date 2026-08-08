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
constexpr int kMaxAsteroids = 32;
constexpr int kMaxShots = 64;
constexpr int kMaxParticles = 128;
constexpr int kDroneCount = 12;
constexpr int kStarCount = 80;

struct Drone
{
  float angle = 0.f;
  float radius = 140.f;
  float speed = 0.6f;
  float aim = 0.f;
  float cooldown = 0.f;
};

struct Asteroid
{
  bool alive = false;
  float x = 0.f, y = 0.f;
  float vx = 0.f, vy = 0.f;
  float r = 10.f;
  float spin = 0.f;
  float rot = 0.f;
};

struct Shot
{
  bool alive = false;
  float x = 0.f, y = 0.f;
  float vx = 0.f, vy = 0.f;
  float life = 0.f;
};

struct Particle
{
  bool alive = false;
  float x = 0.f, y = 0.f;
  float vx = 0.f, vy = 0.f;
  float life = 0.f;
  float maxLife = 0.4f;
  float size = 6.f;
  float r = 1.f, g = 0.6f, b = 0.2f;
};

struct Boom
{
  bool alive = false;
  float x = 0.f, y = 0.f;
  float life = 0.f;
  float maxLife = 0.4f;
};

class OrbitalScene final : public IScene
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
    score_ = 0;
    shields_ = 100;
    wave_ = 1;
    killsThisWave_ = 0;
    spawnTimer_ = 0.5f;
    flash_ = 0.f;
    planetHitFlash_ = 0.f;

    drones_.resize(kDroneCount);
    for (int i = 0; i < kDroneCount; ++i)
    {
      drones_[i].angle = (kTau * i) / kDroneCount;
      drones_[i].radius = 120.f + rng_.Range(0.f, 50.f);
      drones_[i].speed = 0.45f + rng_.Range(0.f, 0.55f) * ((i & 1) ? -1.f : 1.f);
      drones_[i].aim = drones_[i].angle + kPi * 0.5f;
      drones_[i].cooldown = rng_.Range(0.f, 1.2f);
    }

    asteroids_.assign(kMaxAsteroids, {});
    shots_.assign(kMaxShots, {});
    particles_.assign(kMaxParticles, {});
    booms_.clear();
    booms_.reserve(16);

    // Seeded starfield (positions fixed for the run).
    stars_.resize(kStarCount);
    for (auto& s : stars_)
    {
      s.x = rng_.NextFloat();
      s.y = rng_.NextFloat();
      s.phase = rng_.Range(0.f, kTau);
      s.speed = rng_.Range(4.f, 18.f);
      s.bright = rng_.Range(0.35f, 1.f);
      s.size = rng_.Range(1.5f, 3.5f);
    }

    // A few starter asteroids.
    for (int i = 0; i < 6; ++i)
      SpawnAsteroid();
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
      dt = 0.1f; // avoid spiral after stalls
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
    out.backdrop = BackdropId::Starfield;
    out.clearR = 0.01f;
    out.clearG = 0.012f;
    out.clearB = 0.04f;
    out.flashR = 1.f;
    out.flashG = 0.55f;
    out.flashB = 0.15f;
    out.flashA = Clamp(flash_, 0.f, 1.f);
    out.prims.clear();
    out.prims.reserve(256);

    const float cx = w_ * 0.5f;
    const float cy = h_ * 0.5f;

    // Stars
    for (const auto& s : stars_)
    {
      const float tw = 0.45f + 0.55f * (0.5f + 0.5f * std::sin(t_ * s.speed + s.phase));
      const float a = s.bright * tw;
      out.prims.push_back(MakeOrb(s.x * w_, s.y * h_, s.size, 0.85f, 0.9f, 1.f, a));
    }

    // Planet / reactor core
    {
      const float pulse = 1.f + 0.06f * std::sin(t_ * 2.2f);
      const float hit = planetHitFlash_;
      float pr = Lerp(0.25f, 1.f, hit);
      float pg = Lerp(0.55f, 0.4f, hit);
      float pb = Lerp(0.95f, 0.15f, hit);
      out.prims.push_back(MakeOrb(cx, cy, 110.f * pulse, pr, pg, pb, 0.95f));
      out.prims.push_back(MakeOrb(cx, cy, 48.f * pulse, 0.9f, 0.95f, 1.f, 0.85f));
      float rr, gg, bb;
      Hsv(std::fmod(0.55f + t_ * 0.03f, 1.f), 0.75f, 1.f, &rr, &gg, &bb);
      out.prims.push_back(MakeRing(cx, cy, 160.f, 160.f * 0.85f, t_ * 0.35f, rr, gg, bb, 0.4f));
      Hsv(std::fmod(0.72f + t_ * 0.02f, 1.f), 0.7f, 1.f, &rr, &gg, &bb);
      out.prims.push_back(MakeRing(cx, cy, 210.f, 210.f * 0.7f, -t_ * 0.22f, rr, gg, bb, 0.28f));
    }

    // Asteroids
    for (const auto& a : asteroids_)
    {
      if (!a.alive)
        continue;
      float rr, gg, bb;
      Hsv(0.08f + 0.04f * std::sin(a.rot), 0.35f, 0.85f, &rr, &gg, &bb);
      out.prims.push_back(MakeSolid(a.x, a.y, a.r * 1.8f, a.r * 1.8f, a.rot, rr, gg, bb, 0.9f));
      out.prims.push_back(MakeOrb(a.x, a.y, a.r * 1.2f, rr * 0.7f, gg * 0.7f, bb * 0.7f, 0.5f));
    }

    // Drones
    for (const auto& d : drones_)
    {
      const float x = cx + std::cos(d.angle) * d.radius;
      const float y = cy + std::sin(d.angle) * d.radius * 0.72f;
      float rr, gg, bb;
      Hsv(0.52f + 0.08f * std::sin(d.angle * 3.f), 0.85f, 1.f, &rr, &gg, &bb);
      out.prims.push_back(MakeTri(x, y, 14.f, 22.f, d.aim, rr, gg, bb, 0.95f));
      out.prims.push_back(MakeOrb(x, y, 8.f, 0.4f, 0.9f, 1.f, 0.55f));
    }

    // Shots
    for (const auto& s : shots_)
    {
      if (!s.alive)
        continue;
      out.prims.push_back(MakeOrb(s.x, s.y, 5.f, 1.f, 0.95f, 0.4f, 0.95f));
      out.prims.push_back(MakeOrb(s.x, s.y, 10.f, 1.f, 0.5f, 0.1f, 0.35f));
    }

    // Explosion rings
    for (const auto& b : booms_)
    {
      if (!b.alive)
        continue;
      const float u = 1.f - (b.life / b.maxLife);
      const float sz = 20.f + u * 90.f;
      const float a = (1.f - u) * 0.85f;
      out.prims.push_back(MakeRing(b.x, b.y, sz, sz, 0.f, 1.f, 0.55f, 0.15f, a));
      out.prims.push_back(MakeRing(b.x, b.y, sz * 0.6f, sz * 0.6f, 0.f, 1.f, 0.9f, 0.4f, a * 0.7f));
    }

    // Particles
    for (const auto& p : particles_)
    {
      if (!p.alive)
        continue;
      const float u = Clamp(p.life / p.maxLife, 0.f, 1.f);
      out.prims.push_back(MakeOrb(p.x, p.y, p.size * (0.5f + 0.5f * u), p.r, p.g, p.b, u));
    }

    // Shield arc hint (thin ring scaled by shields)
    {
      const float sh = shields_ / 100.f;
      out.prims.push_back(
          MakeRing(cx, cy, 125.f, 125.f * 0.85f, t_ * 1.5f, 0.3f, 0.9f, 1.f, 0.15f + 0.35f * sh));
    }

    wchar_t buf[128];
    swprintf_s(buf, L"SCORE %d   SHIELDS %d   WAVE %d", score_, shields_, wave_);
    out.hud.line1 = buf;
    swprintf_s(buf, L"drones %d  rocks %d", kDroneCount, CountAsteroids());
    out.hud.line2 = buf;
  }

  const wchar_t* Name() const override { return L"orbital"; }
  SceneId Id() const override { return SceneId::Orbital; }

private:
  struct Star
  {
    float x, y, phase, speed, bright, size;
  };

  int CountAsteroids() const
  {
    int n = 0;
    for (const auto& a : asteroids_)
      if (a.alive)
        ++n;
    return n;
  }

  void Step(float dt)
  {
    const float cx = w_ * 0.5f;
    const float cy = h_ * 0.5f;

    if (flash_ > 0.f)
      flash_ = std::max(0.f, flash_ - dt / 0.15f);
    if (planetHitFlash_ > 0.f)
      planetHitFlash_ = std::max(0.f, planetHitFlash_ - dt / 0.25f);

    // Drones orbit + aim + fire
    for (auto& d : drones_)
    {
      d.angle += d.speed * dt;
      if (d.angle > kTau)
        d.angle -= kTau;
      if (d.angle < 0.f)
        d.angle += kTau;

      const float dx = cx + std::cos(d.angle) * d.radius;
      const float dy = cy + std::sin(d.angle) * d.radius * 0.72f;

      // Aim at nearest asteroid
      float bestDist = 1e9f;
      float tx = dx + std::cos(d.angle + kPi * 0.5f);
      float ty = dy + std::sin(d.angle + kPi * 0.5f);
      for (const auto& a : asteroids_)
      {
        if (!a.alive)
          continue;
        const float dist = Length(a.x - dx, a.y - dy);
        if (dist < bestDist)
        {
          bestDist = dist;
          tx = a.x;
          ty = a.y;
        }
      }
      const float desired = std::atan2(ty - dy, tx - dx);
      const float diff = AngleDiff(d.aim, desired);
      const float turn = Clamp(diff, -4.f * dt, 4.f * dt);
      d.aim += turn;

      if (d.cooldown > 0.f)
        d.cooldown -= dt;
      if (d.cooldown <= 0.f && bestDist < 420.f && std::fabs(diff) < 0.35f)
      {
        FireShot(dx, dy, d.aim);
        d.cooldown = 0.35f + rng_.Range(0.f, 0.25f);
      }
    }

    // Asteroids
    for (auto& a : asteroids_)
    {
      if (!a.alive)
        continue;
      a.x += a.vx * dt;
      a.y += a.vy * dt;
      a.rot += a.spin * dt;

      // Pull slightly toward center (inward drift)
      float ix, iy;
      Normalize(cx - a.x, cy - a.y, &ix, &iy);
      a.vx += ix * 18.f * dt;
      a.vy += iy * 18.f * dt;

      // Planet collision
      const float dist = Length(a.x - cx, a.y - cy);
      if (dist < 55.f + a.r)
      {
        a.alive = false;
        shields_ = std::max(0, shields_ - 8);
        flash_ = 1.f;
        planetHitFlash_ = 1.f;
        Explode(a.x, a.y, 10);
        if (shields_ <= 0)
        {
          // Soft reset shields after wipe — keep screensaver looping.
          shields_ = 100;
          score_ = std::max(0, score_ - 200);
          flash_ = 1.f;
        }
      }

      // Despawn if far off-screen
      if (a.x < -80.f || a.y < -80.f || a.x > w_ + 80.f || a.y > h_ + 80.f)
        a.alive = false;
    }

    // Shots
    for (auto& s : shots_)
    {
      if (!s.alive)
        continue;
      s.x += s.vx * dt;
      s.y += s.vy * dt;
      s.life -= dt;
      if (s.life <= 0.f || s.x < -20.f || s.y < -20.f || s.x > w_ + 20.f || s.y > h_ + 20.f)
      {
        s.alive = false;
        continue;
      }
      for (auto& a : asteroids_)
      {
        if (!a.alive)
          continue;
        if (Length(a.x - s.x, a.y - s.y) < a.r + 4.f)
        {
          a.alive = false;
          s.alive = false;
          score_ += 100;
          ++killsThisWave_;
          Explode(a.x, a.y, 12);
          if (killsThisWave_ >= 8 + wave_ * 2)
          {
            killsThisWave_ = 0;
            ++wave_;
            score_ += 500;
          }
          break;
        }
      }
    }

    // Particles
    for (auto& p : particles_)
    {
      if (!p.alive)
        continue;
      p.x += p.vx * dt;
      p.y += p.vy * dt;
      p.vx *= (1.f - 1.5f * dt);
      p.vy *= (1.f - 1.5f * dt);
      p.life -= dt;
      if (p.life <= 0.f)
        p.alive = false;
    }

    // Booms
    for (auto& b : booms_)
    {
      if (!b.alive)
        continue;
      b.life -= dt;
      if (b.life <= 0.f)
        b.alive = false;
    }

    // Spawn
    spawnTimer_ -= dt;
    if (spawnTimer_ <= 0.f)
    {
      const float interval = std::max(0.25f, 1.1f - wave_ * 0.05f);
      spawnTimer_ = interval * rng_.Range(0.7f, 1.3f);
      const int n = 1 + (wave_ > 3 ? 1 : 0);
      for (int i = 0; i < n; ++i)
        SpawnAsteroid();
    }
  }

  void SpawnAsteroid()
  {
    Asteroid* slot = nullptr;
    for (auto& a : asteroids_)
    {
      if (!a.alive)
      {
        slot = &a;
        break;
      }
    }
    if (!slot)
      return;

    const float cx = w_ * 0.5f;
    const float cy = h_ * 0.5f;
    // Spawn near edges
    const int edge = rng_.RangeInt(0, 3);
    float x = 0.f, y = 0.f;
    switch (edge)
    {
    case 0:
      x = rng_.Range(0.f, float(w_));
      y = -20.f;
      break;
    case 1:
      x = rng_.Range(0.f, float(w_));
      y = float(h_) + 20.f;
      break;
    case 2:
      x = -20.f;
      y = rng_.Range(0.f, float(h_));
      break;
    default:
      x = float(w_) + 20.f;
      y = rng_.Range(0.f, float(h_));
      break;
    }
    float ix, iy;
    Normalize(cx - x, cy - y, &ix, &iy);
    const float spd = 40.f + rng_.Range(0.f, 35.f) + wave_ * 4.f;
    // Slight lateral jitter
    const float px = -iy, py = ix;
    const float jitter = rng_.Range(-0.35f, 0.35f);
    slot->alive = true;
    slot->x = x;
    slot->y = y;
    slot->vx = (ix + px * jitter) * spd;
    slot->vy = (iy + py * jitter) * spd;
    slot->r = rng_.Range(8.f, 18.f);
    slot->spin = rng_.Range(-2.5f, 2.5f);
    slot->rot = rng_.Range(0.f, kTau);
  }

  void FireShot(float x, float y, float aim)
  {
    Shot* slot = nullptr;
    for (auto& s : shots_)
    {
      if (!s.alive)
      {
        slot = &s;
        break;
      }
    }
    if (!slot)
      return;
    const float spd = 380.f;
    slot->alive = true;
    slot->x = x + std::cos(aim) * 14.f;
    slot->y = y + std::sin(aim) * 14.f;
    slot->vx = std::cos(aim) * spd;
    slot->vy = std::sin(aim) * spd;
    slot->life = 1.4f;
  }

  void Explode(float x, float y, int nParticles)
  {
    Boom b;
    b.alive = true;
    b.x = x;
    b.y = y;
    b.maxLife = 0.4f;
    b.life = b.maxLife;
    // reuse dead boom or push
    bool placed = false;
    for (auto& existing : booms_)
    {
      if (!existing.alive)
      {
        existing = b;
        placed = true;
        break;
      }
    }
    if (!placed && booms_.size() < 24)
      booms_.push_back(b);

    int spawned = 0;
    for (auto& p : particles_)
    {
      if (p.alive)
        continue;
      const float ang = rng_.Range(0.f, kTau);
      const float spd = rng_.Range(40.f, 180.f);
      p.alive = true;
      p.x = x;
      p.y = y;
      p.vx = std::cos(ang) * spd;
      p.vy = std::sin(ang) * spd;
      p.maxLife = rng_.Range(0.25f, 0.55f);
      p.life = p.maxLife;
      p.size = rng_.Range(3.f, 9.f);
      Hsv(rng_.Range(0.02f, 0.14f), 0.9f, 1.f, &p.r, &p.g, &p.b);
      if (++spawned >= nParticles)
        break;
    }
  }

  SceneConfig cfg_{};
  Rng rng_{};
  int w_ = 1280;
  int h_ = 720;
  float t_ = 0.f;
  float accum_ = 0.f;
  int score_ = 0;
  int shields_ = 100;
  int wave_ = 1;
  int killsThisWave_ = 0;
  float spawnTimer_ = 0.5f;
  float flash_ = 0.f;
  float planetHitFlash_ = 0.f;

  std::vector<Drone> drones_;
  std::vector<Asteroid> asteroids_;
  std::vector<Shot> shots_;
  std::vector<Particle> particles_;
  std::vector<Boom> booms_;
  std::vector<Star> stars_;
};

} // namespace

std::unique_ptr<IScene> CreateOrbitalScene()
{
  return std::make_unique<OrbitalScene>();
}

} // namespace scene
