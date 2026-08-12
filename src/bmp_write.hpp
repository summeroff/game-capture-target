#pragma once

#include <cstdint>
#include <cstdio>
#include <vector>

// Write a 24-bit BMP from a top-down DXGI R8G8B8A8 (or B8G8R8A8) buffer.
// srcPitch is bytes per row. Channels: rOff/gOff/bOff into each 4-byte pixel.
inline bool WriteBgra32ToBmp(const wchar_t* path, int w, int h, const uint8_t* src, int srcPitch,
                             int rOff = 0, int gOff = 1, int bOff = 2)
{
  if (!path || !*path || !src || w < 1 || h < 1 || srcPitch < w * 4)
    return false;

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
    return false;

  fwrite(&fh, 1, sizeof(fh), f);
  fwrite(&ih, 1, sizeof(ih), f);

  std::vector<uint8_t> row(static_cast<size_t>(stride), 0);
  for (int y = h - 1; y >= 0; --y)
  {
    const uint8_t* s = src + y * srcPitch;
    for (int x = 0; x < w; ++x)
    {
      const uint8_t* px = s + x * 4;
      row[static_cast<size_t>(x * 3 + 0)] = px[bOff];
      row[static_cast<size_t>(x * 3 + 1)] = px[gOff];
      row[static_cast<size_t>(x * 3 + 2)] = px[rOff];
    }
    fwrite(row.data(), 1, static_cast<size_t>(stride), f);
  }
  fclose(f);
  return true;
}
