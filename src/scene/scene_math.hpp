#pragma once

#include <cmath>
#include <cstdint>

namespace scene
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTau = 6.28318530717958647692f;

// Deterministic xorshift32. seed==0 is remapped so streams stay non-degenerate.
struct Rng
{
  explicit Rng(uint32_t seed = 0xC5A2EEu) { SetSeed(seed); }

  void SetSeed(uint32_t seed) { state_ = seed ? seed : 0xA341316Cu; }

  uint32_t NextU32()
  {
    uint32_t x = state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state_ = x;
    return x;
  }

  float NextFloat() // [0,1)
  {
    return (NextU32() & 0xFFFFFFFu) / static_cast<float>(0x10000000u);
  }

  float Range(float lo, float hi) { return lo + (hi - lo) * NextFloat(); }

  int RangeInt(int lo, int hiInclusive)
  {
    if (hiInclusive <= lo)
      return lo;
    return lo + static_cast<int>(NextU32() % static_cast<uint32_t>(hiInclusive - lo + 1));
  }

private:
  uint32_t state_ = 1;
};

inline void Hsv(float h, float s, float v, float* r, float* g, float* b)
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

inline float Clamp(float x, float lo, float hi)
{
  return x < lo ? lo : (x > hi ? hi : x);
}

inline float Lerp(float a, float b, float t)
{
  return a + (b - a) * t;
}

inline float WrapAngle(float a)
{
  while (a > kPi)
    a -= kTau;
  while (a < -kPi)
    a += kTau;
  return a;
}

inline float AngleDiff(float from, float to)
{
  return WrapAngle(to - from);
}

inline float Length(float x, float y)
{
  return std::sqrt(x * x + y * y);
}

inline void Normalize(float x, float y, float* ox, float* oy)
{
  const float len = Length(x, y);
  if (len < 1e-6f)
  {
    *ox = 1.f;
    *oy = 0.f;
    return;
  }
  *ox = x / len;
  *oy = y / len;
}

} // namespace scene
