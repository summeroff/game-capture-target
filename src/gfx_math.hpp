#pragma once

#include <cmath>
#include <cstring>
#include <string>

#include <Windows.h>

// Shared 2D column-major / column-vector math for d3d11 + d3d12.
// CPU convention: p' = M * p, translation in m[12]/m[13].
// HLSL must use mul(uTransform, float4(pos,0,1)) — see skill d3d-cpu-matrix-hlsl.

namespace gfx
{

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

inline void MatIdentity(float* m)
{
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = m[5] = m[10] = m[15] = 1.f;
}

// Column-major ortho: x,y in pixels → NDC. Y grows down (top-left origin).
inline void MatOrthoPixels(float* m, float w, float h)
{
  std::memset(m, 0, 16 * sizeof(float));
  m[0] = 2.f / w;
  m[5] = -2.f / h;
  m[10] = 1.f;
  m[12] = -1.f;
  m[13] = 1.f;
  m[15] = 1.f;
}

inline void MatMul(float* out, const float* a, const float* b)
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

inline void MatTranslate(float* m, float x, float y)
{
  MatIdentity(m);
  m[12] = x;
  m[13] = y;
}

inline void MatRotateZ(float* m, float rad)
{
  MatIdentity(m);
  const float c = std::cos(rad);
  const float s = std::sin(rad);
  m[0] = c;
  m[1] = s;
  m[4] = -s;
  m[5] = c;
}

inline void MatScale(float* m, float sx, float sy)
{
  MatIdentity(m);
  m[0] = sx;
  m[5] = sy;
}

inline std::wstring HrMsg(HRESULT hr)
{
  wchar_t buf[64];
  swprintf_s(buf, L"HRESULT 0x%08X", static_cast<unsigned>(hr));
  return buf;
}

} // namespace gfx
