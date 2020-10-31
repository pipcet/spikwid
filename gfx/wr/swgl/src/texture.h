/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

namespace glsl {

SI I32 clampCoord(I32 coord, int limit, int base = 0) {
#if USE_SSE2
  return _mm_min_epi16(_mm_max_epi16(coord, _mm_set1_epi32(base)),
                       _mm_set1_epi32(limit - 1));
#else
  return clamp(coord, base, limit - 1);
#endif
}

SI int clampCoord(int coord, int limit, int base = 0) {
  return min(max(coord, base), limit - 1);
}

template <typename T, typename S>
SI T clamp2D(T P, S sampler) {
  return T{clampCoord(P.x, sampler->width), clampCoord(P.y, sampler->height)};
}

template <typename T>
SI T clamp2DArray(T P, sampler2DArray sampler) {
  return T{clampCoord(P.x, sampler->width), clampCoord(P.y, sampler->height),
           clampCoord(P.z, sampler->depth)};
}

SI float to_float(uint32_t x) { return x * (1.f / 255.f); }

SI vec4 pixel_to_vec4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  U32 pixels = {a, b, c, d};
  return vec4(cast((pixels >> 16) & 0xFF), cast((pixels >> 8) & 0xFF),
              cast(pixels & 0xFF), cast(pixels >> 24)) *
         (1.0f / 255.0f);
}

SI vec4 pixel_float_to_vec4(Float a, Float b, Float c, Float d) {
  return vec4(Float{a.x, b.x, c.x, d.x}, Float{a.y, b.y, c.y, d.y},
              Float{a.z, b.z, c.z, d.z}, Float{a.w, b.w, c.w, d.w});
}

SI ivec4 pixel_int_to_ivec4(I32 a, I32 b, I32 c, I32 d) {
  return ivec4(I32{a.x, b.x, c.x, d.x}, I32{a.y, b.y, c.y, d.y},
               I32{a.z, b.z, c.z, d.z}, I32{a.w, b.w, c.w, d.w});
}

SI vec4_scalar pixel_to_vec4(uint32_t p) {
  U32 i = {(p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF, p >> 24};
  Float f = cast(i) * (1.0f / 255.0f);
  return vec4_scalar(f.x, f.y, f.z, f.w);
}

template <typename S>
SI vec4 fetchOffsetsRGBA8(S sampler, I32 offset) {
  return pixel_to_vec4(sampler->buf[offset.x], sampler->buf[offset.y],
                       sampler->buf[offset.z], sampler->buf[offset.w]);
}

template <typename S>
vec4 texelFetchRGBA8(S sampler, ivec2 P) {
  I32 offset = P.x + P.y * sampler->stride;
  return fetchOffsetsRGBA8(sampler, offset);
}

vec4 texelFetchRGBA8(sampler2DArray sampler, ivec3 P) {
  assert(test_all(P.z == P.z.x));
  I32 offset = P.x + P.y * sampler->stride + P.z.x * sampler->height_stride;
  return fetchOffsetsRGBA8(sampler, offset);
}

template <typename S>
SI Float fetchOffsetsR8(S sampler, I32 offset) {
  U32 i = {
      ((uint8_t*)sampler->buf)[offset.x], ((uint8_t*)sampler->buf)[offset.y],
      ((uint8_t*)sampler->buf)[offset.z], ((uint8_t*)sampler->buf)[offset.w]};
  return cast(i) * (1.0f / 255.0f);
}

template <typename S>
vec4 texelFetchR8(S sampler, ivec2 P) {
  I32 offset = P.x + P.y * sampler->stride;
  return vec4(fetchOffsetsR8(sampler, offset), 0.0f, 0.0f, 1.0f);
}

vec4 texelFetchR8(sampler2DArray sampler, ivec3 P) {
  assert(test_all(P.z == P.z.x));
  I32 offset = P.x + P.y * sampler->stride + P.z.x * sampler->height_stride;
  return vec4(fetchOffsetsR8(sampler, offset), 0.0f, 0.0f, 1.0f);
}

template <typename S>
SI vec4 fetchOffsetsRG8(S sampler, I32 offset) {
  uint16_t* buf = (uint16_t*)sampler->buf;
  U16 pixels = {buf[offset.x], buf[offset.y], buf[offset.z], buf[offset.w]};
  Float r = CONVERT(pixels & 0xFF, Float) * (1.0f / 255.0f);
  Float g = CONVERT(pixels >> 8, Float) * (1.0f / 255.0f);
  return vec4(r, g, 0.0f, 1.0f);
}

template <typename S>
vec4 texelFetchRG8(S sampler, ivec2 P) {
  I32 offset = P.x + P.y * sampler->stride;
  return fetchOffsetsRG8(sampler, offset);
}

vec4 texelFetchRG8(sampler2DArray sampler, ivec3 P) {
  assert(test_all(P.z == P.z.x));
  I32 offset = P.x + P.y * sampler->stride + P.z.x * sampler->height_stride;
  return fetchOffsetsRG8(sampler, offset);
}

template <typename S>
SI Float fetchOffsetsR16(S sampler, I32 offset) {
  U32 i = {
      ((uint16_t*)sampler->buf)[offset.x], ((uint16_t*)sampler->buf)[offset.y],
      ((uint16_t*)sampler->buf)[offset.z], ((uint16_t*)sampler->buf)[offset.w]};
  return cast(i) * (1.0f / 65535.0f);
}

template <typename S>
vec4 texelFetchR16(S sampler, ivec2 P) {
  I32 offset = P.x + P.y * sampler->stride;
  return vec4(fetchOffsetsR16(sampler, offset), 0.0f, 0.0f, 1.0f);
}

vec4 texelFetchR16(sampler2DArray sampler, ivec3 P) {
  assert(test_all(P.z == P.z.x));
  I32 offset = P.x + P.y * sampler->stride + P.z.x * sampler->height_stride;
  return vec4(fetchOffsetsR16(sampler, offset), 0.0f, 0.0f, 1.0f);
}

template <typename S>
SI vec4 fetchOffsetsFloat(S sampler, I32 offset) {
  return pixel_float_to_vec4(
      *(Float*)&sampler->buf[offset.x], *(Float*)&sampler->buf[offset.y],
      *(Float*)&sampler->buf[offset.z], *(Float*)&sampler->buf[offset.w]);
}

vec4 texelFetchFloat(sampler2D sampler, ivec2 P) {
  I32 offset = P.x * 4 + P.y * sampler->stride;
  return fetchOffsetsFloat(sampler, offset);
}

SI vec4 texelFetchFloat(sampler2DArray sampler, ivec3 P) {
  assert(test_all(P.z == P.z.x));
  I32 offset = P.x * 4 + P.y * sampler->stride + P.z.x * sampler->height_stride;
  return fetchOffsetsFloat(sampler, offset);
}

template <typename S>
SI vec4 fetchOffsetsYUV422(S sampler, I32 offset) {
  // Layout is 2 pixel chunks (occupying 4 bytes) organized as: G0, B, G1, R.
  // Offset is aligned to a chunk rather than a pixel, and selector specifies
  // pixel within the chunk.
  I32 selector = offset & 1;
  offset &= ~1;
  uint16_t* buf = (uint16_t*)sampler->buf;
  U32 pixels = {*(uint32_t*)&buf[offset.x], *(uint32_t*)&buf[offset.y],
                *(uint32_t*)&buf[offset.z], *(uint32_t*)&buf[offset.w]};
  Float b = CONVERT((pixels >> 8) & 0xFF, Float) * (1.0f / 255.0f);
  Float r = CONVERT((pixels >> 24), Float) * (1.0f / 255.0f);
  Float g =
      CONVERT(if_then_else(-selector, pixels >> 16, pixels) & 0xFF, Float) *
      (1.0f / 255.0f);
  return vec4(r, g, b, 1.0f);
}

template <typename S>
vec4 texelFetchYUV422(S sampler, ivec2 P) {
  I32 offset = P.x + P.y * sampler->stride;
  return fetchOffsetsYUV422(sampler, offset);
}

vec4 texelFetch(sampler2D sampler, ivec2 P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  switch (sampler->format) {
    case TextureFormat::RGBA32F:
      return texelFetchFloat(sampler, P);
    case TextureFormat::RGBA8:
      return texelFetchRGBA8(sampler, P);
    case TextureFormat::R8:
      return texelFetchR8(sampler, P);
    case TextureFormat::RG8:
      return texelFetchRG8(sampler, P);
    case TextureFormat::R16:
      return texelFetchR16(sampler, P);
    case TextureFormat::YUV422:
      return texelFetchYUV422(sampler, P);
    default:
      assert(false);
      return vec4();
  }
}

vec4 texelFetch(sampler2DRGBA32F sampler, ivec2 P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::RGBA32F);
  return texelFetchFloat(sampler, P);
}

vec4 texelFetch(sampler2DRGBA8 sampler, ivec2 P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::RGBA8);
  return texelFetchRGBA8(sampler, P);
}

vec4 texelFetch(sampler2DR8 sampler, ivec2 P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::R8);
  return texelFetchR8(sampler, P);
}

vec4 texelFetch(sampler2DRG8 sampler, ivec2 P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::RG8);
  return texelFetchRG8(sampler, P);
}

vec4_scalar texelFetch(sampler2D sampler, ivec2_scalar P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  if (sampler->format == TextureFormat::RGBA32F) {
    return *(vec4_scalar*)&sampler->buf[P.x * 4 + P.y * sampler->stride];
  } else {
    assert(sampler->format == TextureFormat::RGBA8);
    return pixel_to_vec4(sampler->buf[P.x + P.y * sampler->stride]);
  }
}

vec4_scalar texelFetch(sampler2DRGBA32F sampler, ivec2_scalar P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::RGBA32F);
  return *(vec4_scalar*)&sampler->buf[P.x * 4 + P.y * sampler->stride];
}

vec4_scalar texelFetch(sampler2DRGBA8 sampler, ivec2_scalar P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::RGBA8);
  return pixel_to_vec4(sampler->buf[P.x + P.y * sampler->stride]);
}

vec4_scalar texelFetch(sampler2DR8 sampler, ivec2_scalar P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::R8);
  return vec4_scalar{
      to_float(((uint8_t*)sampler->buf)[P.x + P.y * sampler->stride]), 0.0f,
      0.0f, 1.0f};
}

vec4_scalar texelFetch(sampler2DRG8 sampler, ivec2_scalar P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::RG8);
  uint16_t pixel = ((uint16_t*)sampler->buf)[P.x + P.y * sampler->stride];
  return vec4_scalar{to_float(pixel & 0xFF), to_float(pixel >> 8), 0.0f, 1.0f};
}

vec4 texelFetch(sampler2DRect sampler, ivec2 P) {
  P = clamp2D(P, sampler);
  switch (sampler->format) {
    case TextureFormat::RGBA8:
      return texelFetchRGBA8(sampler, P);
    case TextureFormat::R8:
      return texelFetchR8(sampler, P);
    case TextureFormat::RG8:
      return texelFetchRG8(sampler, P);
    case TextureFormat::R16:
      return texelFetchR16(sampler, P);
    case TextureFormat::YUV422:
      return texelFetchYUV422(sampler, P);
    default:
      assert(false);
      return vec4();
  }
}

SI vec4 texelFetch(sampler2DArray sampler, ivec3 P, int lod) {
  assert(lod == 0);
  P = clamp2DArray(P, sampler);
  switch (sampler->format) {
    case TextureFormat::RGBA32F:
      return texelFetchFloat(sampler, P);
    case TextureFormat::RGBA8:
      return texelFetchRGBA8(sampler, P);
    case TextureFormat::R8:
      return texelFetchR8(sampler, P);
    case TextureFormat::RG8:
      return texelFetchRG8(sampler, P);
    case TextureFormat::R16:
      return texelFetchR16(sampler, P);
    default:
      assert(false);
      return vec4();
  }
}

vec4 texelFetch(sampler2DArrayRGBA32F sampler, ivec3 P, int lod) {
  assert(lod == 0);
  P = clamp2DArray(P, sampler);
  assert(sampler->format == TextureFormat::RGBA32F);
  return texelFetchFloat(sampler, P);
}

vec4 texelFetch(sampler2DArrayRGBA8 sampler, ivec3 P, int lod) {
  assert(lod == 0);
  P = clamp2DArray(P, sampler);
  assert(sampler->format == TextureFormat::RGBA8);
  return texelFetchRGBA8(sampler, P);
}

vec4 texelFetch(sampler2DArrayR8 sampler, ivec3 P, int lod) {
  assert(lod == 0);
  P = clamp2DArray(P, sampler);
  assert(sampler->format == TextureFormat::R8);
  return texelFetchR8(sampler, P);
}

vec4 texelFetch(sampler2DArrayRG8 sampler, ivec3 P, int lod) {
  assert(lod == 0);
  P = clamp2DArray(P, sampler);
  assert(sampler->format == TextureFormat::RG8);
  return texelFetchRG8(sampler, P);
}

template <typename S>
SI ivec4 fetchOffsetsInt(S sampler, I32 offset) {
  return pixel_int_to_ivec4(
      *(I32*)&sampler->buf[offset.x], *(I32*)&sampler->buf[offset.y],
      *(I32*)&sampler->buf[offset.z], *(I32*)&sampler->buf[offset.w]);
}

ivec4 texelFetch(isampler2D sampler, ivec2 P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::RGBA32I);
  I32 offset = P.x * 4 + P.y * sampler->stride;
  return fetchOffsetsInt(sampler, offset);
}

ivec4_scalar texelFetch(isampler2D sampler, ivec2_scalar P, int lod) {
  assert(lod == 0);
  P = clamp2D(P, sampler);
  assert(sampler->format == TextureFormat::RGBA32I);
  return *(ivec4_scalar*)&sampler->buf[P.x * 4 + P.y * sampler->stride];
}

SI vec4_scalar* texelFetchPtr(sampler2D sampler, ivec2_scalar P, int min_x,
                              int max_x, int min_y, int max_y) {
  P.x = min(max(P.x, -min_x), int(sampler->width) - 1 - max_x);
  P.y = min(max(P.y, -min_y), int(sampler->height) - 1 - max_y);
  assert(sampler->format == TextureFormat::RGBA32F);
  return (vec4_scalar*)&sampler->buf[P.x * 4 + P.y * sampler->stride];
}

SI ivec4_scalar* texelFetchPtr(isampler2D sampler, ivec2_scalar P, int min_x,
                               int max_x, int min_y, int max_y) {
  P.x = min(max(P.x, -min_x), int(sampler->width) - 1 - max_x);
  P.y = min(max(P.y, -min_y), int(sampler->height) - 1 - max_y);
  assert(sampler->format == TextureFormat::RGBA32I);
  return (ivec4_scalar*)&sampler->buf[P.x * 4 + P.y * sampler->stride];
}

template <typename S>
SI I32 texelFetchPtr(S sampler, ivec2 P, int min_x, int max_x, int min_y,
                     int max_y) {
  P.x = clampCoord(P.x, int(sampler->width) - max_x, -min_x);
  P.y = clampCoord(P.y, int(sampler->height) - max_y, -min_y);
  return P.x * 4 + P.y * sampler->stride;
}

template <typename S, typename P>
SI P texelFetchUnchecked(S sampler, P* ptr, int x, int y = 0) {
  return ptr[x + y * (sampler->stride >> 2)];
}

SI vec4 texelFetchUnchecked(sampler2D sampler, I32 offset, int x, int y = 0) {
  assert(sampler->format == TextureFormat::RGBA32F);
  return fetchOffsetsFloat(sampler, offset + (x * 4 + y * sampler->stride));
}

SI ivec4 texelFetchUnchecked(isampler2D sampler, I32 offset, int x, int y = 0) {
  assert(sampler->format == TextureFormat::RGBA32I);
  return fetchOffsetsInt(sampler, offset + (x * 4 + y * sampler->stride));
}

#define texelFetchOffset(sampler, P, lod, offset) \
  texelFetch(sampler, (P) + (offset), lod)

// Scale texture coords for quantization, subtract offset for filtering
// (assuming coords already offset to texel centers), and round to nearest
// 1/scale increment
template <typename T>
SI T linearQuantize(T P, float scale) {
  return P * scale + (0.5f - 0.5f * scale);
}

// Helper version that also scales normalized texture coords for sampler
template <typename T, typename S>
SI T linearQuantize(T P, float scale, S sampler) {
  P.x *= sampler->width;
  P.y *= sampler->height;
  return linearQuantize(P, scale);
}

template <typename T>
SI T linearQuantize(T P, float scale, sampler2DRect sampler) {
  return linearQuantize(P, scale);
}

template <typename S>
vec4 textureLinearRGBA8(S sampler, vec2 P, int32_t zoffset = 0) {
  assert(sampler->format == TextureFormat::RGBA8);

#if USE_SSE2
  ivec2 i(linearQuantize(P, 256, sampler));
  ivec2 frac = i & (I32)0xFF;
  i >>= 8;

  // Pack coords so they get clamped into range, and also for later bounding
  // of fractional coords. Store Y as low-bits for easier access, X as high.
  __m128i yx = _mm_packs_epi32(i.y, i.x);
  __m128i hw = _mm_packs_epi32(_mm_set1_epi32(sampler->height - 1),
                               _mm_set1_epi32(sampler->width - 1));
  // Clamp coords to valid range to prevent sampling outside texture.
  __m128i clampyx = _mm_min_epi16(_mm_max_epi16(yx, _mm_setzero_si128()), hw);
  // Multiply clamped Y by stride and add X offset without overflowing 2^15
  // stride and accidentally yielding signed result.
  __m128i row0 =
      _mm_madd_epi16(_mm_unpacklo_epi16(clampyx, clampyx),
                     _mm_set1_epi32((sampler->stride - 1) | 0x10000));
  row0 = _mm_add_epi32(row0, _mm_unpackhi_epi16(clampyx, _mm_setzero_si128()));
  // Add in layer offset if available
  row0 = _mm_add_epi32(row0, _mm_set1_epi32(zoffset));

  // Check if fractional coords are all zero, in which case skip filtering.
  __m128i fracyx = _mm_packs_epi32(frac.y, frac.x);
  if (!_mm_movemask_epi8(_mm_cmpgt_epi16(fracyx, _mm_setzero_si128()))) {
    return fetchOffsetsRGBA8(sampler, row0);
  }

  // Check if coords were clamped at all above. If so, need to adjust fractions
  // to avoid sampling outside the texture on the edges.
  __m128i yxinside = _mm_andnot_si128(_mm_cmplt_epi16(yx, _mm_setzero_si128()),
                                      _mm_cmplt_epi16(yx, hw));
  // Set fraction to zero when outside.
  fracyx = _mm_and_si128(fracyx, yxinside);
  // Store two side-by-side copies of X fraction, as below each pixel value
  // will be interleaved to be next to the pixel value for the next row.
  __m128i fracx = _mm_unpackhi_epi16(fracyx, fracyx);
  // For Y fraction, we need to store 1-fraction before each fraction, as a
  // madd will be used to weight and collapse all results as last step.
  __m128i fracy =
      _mm_unpacklo_epi16(_mm_sub_epi16(_mm_set1_epi16(256), fracyx), fracyx);

  // Ensure we don't sample row off end of texture from added stride.
  __m128i row1 = _mm_and_si128(yxinside, _mm_set1_epi16(sampler->stride));

  // Load two adjacent pixels on each row and interleave them.
  // r0,g0,b0,a0,r1,g1,b1,a1 \/ R0,G0,B0,A0,R1,G1,B1,A1
  // r0,R0,g0,G0,b0,B0,a0,A0,r1,R1,g1,G1,b1,B1,a1,A1
#  define LOAD_LANE(out, idx)                                               \
    {                                                                       \
      uint32_t* buf = &sampler->buf[_mm_cvtsi128_si32(                      \
          _mm_shuffle_epi32(row0, _MM_SHUFFLE(idx, idx, idx, idx)))];       \
      out = _mm_unpacklo_epi8(                                              \
          _mm_loadl_epi64((__m128i*)buf),                                   \
          _mm_loadl_epi64((__m128i*)(buf + _mm_extract_epi16(row1, idx)))); \
    }
  __m128i x, y, z, w;
  LOAD_LANE(x, 0)
  LOAD_LANE(y, 1)
  LOAD_LANE(z, 2)
  LOAD_LANE(w, 3)
#  undef LOAD_LANE

  // Need to transpose the data from AoS to SoA format. Best to do this here
  // while the data is still packed into 8-bit components, requiring fewer
  // insns.
  // r0,R0,g0,G0,b0,B0,a0,A0,r1,R1,g1,G1,b1,B1,a1,A1 \/
  // r2,R2,g2,G2,b2,B2,a2,A2,r3,R3,g3,G3,b3,B3,a3,A3
  // ... r0,R0,r2,R2,g0,G0,g2,G2,b0,B0,b2,B2,a0,A0,a2,A2
  // ... r1,R1,r3,R3,g1,G1,g3,G3,b1,B1,b3,B3,a1,A1,a3,A3
  __m128i xy0 = _mm_unpacklo_epi16(x, y);
  __m128i xy1 = _mm_unpackhi_epi16(x, y);
  __m128i zw0 = _mm_unpacklo_epi16(z, w);
  __m128i zw1 = _mm_unpackhi_epi16(z, w);
  // r0,R0,r2,R2,g0,G0,g2,G2,b0,B0,b2,B2,a0,A0,a2,A2 \/
  // r4,R4,r6,R6,g4,G4,g6,G6,b4,B4,b6,B6,a4,A4,a6,A6
  // ... r0,R0,r2,R2,r4,R4,r6,R6,g0,G0,g2,G2,g4,G4,g6,G6
  // ... b0,B0,b2,B2,b4,B4,b6,B6,a0,A0,a2,A2,a4,A4,a6,A6
  __m128i rg0 = _mm_unpacklo_epi32(xy0, zw0);
  __m128i ba0 = _mm_unpackhi_epi32(xy0, zw0);
  __m128i rg1 = _mm_unpacklo_epi32(xy1, zw1);
  __m128i ba1 = _mm_unpackhi_epi32(xy1, zw1);

  // Expand packed SoA pixels for each column. Multiply then add columns with
  // 8-bit precision so we don't carry to high byte of word accidentally. Use
  // final madd insn to blend interleaved rows and expand result to 32 bits.
#  define FILTER_COMPONENT(out, unpack, src0, src1)                            \
    {                                                                          \
      __m128i cc0 = unpack(src0, _mm_setzero_si128());                         \
      __m128i cc1 = unpack(src1, _mm_setzero_si128());                         \
      cc0 = _mm_add_epi8(                                                      \
          cc0,                                                                 \
          _mm_srli_epi16(_mm_mullo_epi16(_mm_sub_epi16(cc1, cc0), fracx), 8)); \
      out = _mm_cvtepi32_ps(_mm_madd_epi16(cc0, fracy));                       \
    }
  __m128 fr, fg, fb, fa;
  FILTER_COMPONENT(fr, _mm_unpacklo_epi8, rg0, rg1);
  FILTER_COMPONENT(fg, _mm_unpackhi_epi8, rg0, rg1);
  FILTER_COMPONENT(fb, _mm_unpacklo_epi8, ba0, ba1);
  FILTER_COMPONENT(fa, _mm_unpackhi_epi8, ba0, ba1);
#  undef FILTER_COMPONENT

  return vec4(fb, fg, fr, fa) * (1.0f / 0xFF00);
#else
  ivec2 i(linearQuantize(P, 128, sampler));
  ivec2 frac = i & (I32)0x7F;
  i >>= 7;

  I32 row0 = clampCoord(i.x, sampler->width) +
             clampCoord(i.y, sampler->height) * sampler->stride + zoffset;
  I32 row1 = row0 + ((i.y >= 0 && i.y < int32_t(sampler->height) - 1) &
                     I32(sampler->stride));
  I16 fracx =
      CONVERT(frac.x & (i.x >= 0 && i.x < int32_t(sampler->width) - 1), I16);
  I16 fracy = CONVERT(frac.y, I16);

  auto a0 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row0.x]), V8<int16_t>);
  auto a1 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row1.x]), V8<int16_t>);
  a0 += ((a1 - a0) * fracy.x) >> 7;

  auto b0 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row0.y]), V8<int16_t>);
  auto b1 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row1.y]), V8<int16_t>);
  b0 += ((b1 - b0) * fracy.y) >> 7;

  auto abl = zipLow(a0, b0);
  auto abh = zipHigh(a0, b0);
  abl += ((abh - abl) * fracx.xyxyxyxy) >> 7;

  auto c0 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row0.z]), V8<int16_t>);
  auto c1 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row1.z]), V8<int16_t>);
  c0 += ((c1 - c0) * fracy.z) >> 7;

  auto d0 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row0.w]), V8<int16_t>);
  auto d1 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row1.w]), V8<int16_t>);
  d0 += ((d1 - d0) * fracy.w) >> 7;

  auto cdl = zipLow(c0, d0);
  auto cdh = zipHigh(c0, d0);
  cdl += ((cdh - cdl) * fracx.zwzwzwzw) >> 7;

  auto rg = CONVERT(V8<uint16_t>(zip2Low(abl, cdl)), V8<float>);
  auto ba = CONVERT(V8<uint16_t>(zip2High(abl, cdl)), V8<float>);

  auto r = lowHalf(rg);
  auto g = highHalf(rg);
  auto b = lowHalf(ba);
  auto a = highHalf(ba);
  return vec4(b, g, r, a) * (1.0f / 255.0f);
#endif
}

template <typename S>
static inline U16 textureLinearPackedR8(S sampler, ivec2 i, int32_t zoffset) {
  assert(sampler->format == TextureFormat::R8);
  ivec2 frac = i & (I32)0x7F;
  i >>= 7;

  I32 row0 = clampCoord(i.x, sampler->width) +
             clampCoord(i.y, sampler->height) * sampler->stride + zoffset;
  I32 row1 = row0 + ((i.y >= 0 && i.y < int32_t(sampler->height) - 1) &
                     I32(sampler->stride));
  I16 fracx =
      CONVERT(frac.x & (i.x >= 0 && i.x < int32_t(sampler->width) - 1), I16);
  I16 fracy = CONVERT(frac.y, I16);

  uint8_t* buf = (uint8_t*)sampler->buf;
  auto a0 = unaligned_load<V2<uint8_t>>(&buf[row0.x]);
  auto b0 = unaligned_load<V2<uint8_t>>(&buf[row0.y]);
  auto c0 = unaligned_load<V2<uint8_t>>(&buf[row0.z]);
  auto d0 = unaligned_load<V2<uint8_t>>(&buf[row0.w]);
  auto abcd0 = CONVERT(combine(combine(a0, b0), combine(c0, d0)), V8<int16_t>);

  auto a1 = unaligned_load<V2<uint8_t>>(&buf[row1.x]);
  auto b1 = unaligned_load<V2<uint8_t>>(&buf[row1.y]);
  auto c1 = unaligned_load<V2<uint8_t>>(&buf[row1.z]);
  auto d1 = unaligned_load<V2<uint8_t>>(&buf[row1.w]);
  auto abcd1 = CONVERT(combine(combine(a1, b1), combine(c1, d1)), V8<int16_t>);

  abcd0 += ((abcd1 - abcd0) * fracy.xxyyzzww) >> 7;

  abcd0 = SHUFFLE(abcd0, abcd0, 0, 2, 4, 6, 1, 3, 5, 7);
  auto abcdl = lowHalf(abcd0);
  auto abcdh = highHalf(abcd0);
  abcdl += ((abcdh - abcdl) * fracx) >> 7;

  return U16(abcdl);
}

template <typename S>
vec4 textureLinearR8(S sampler, vec2 P, int32_t zoffset = 0) {
  assert(sampler->format == TextureFormat::R8);

#if USE_SSE2
  ivec2 i(linearQuantize(P, 256, sampler));
  ivec2 frac = i & (I32)0xFF;
  i >>= 8;

  // Pack coords so they get clamped into range, and also for later bounding
  // of fractional coords. Store Y as low-bits for easier access, X as high.
  __m128i yx = _mm_packs_epi32(i.y, i.x);
  __m128i hw = _mm_packs_epi32(_mm_set1_epi32(sampler->height - 1),
                               _mm_set1_epi32(sampler->width - 1));
  // Clamp coords to valid range to prevent sampling outside texture.
  __m128i clampyx = _mm_min_epi16(_mm_max_epi16(yx, _mm_setzero_si128()), hw);
  // Multiply clamped Y by stride and add X offset without overflowing 2^15
  // stride and accidentally yielding signed result.
  __m128i row0 =
      _mm_madd_epi16(_mm_unpacklo_epi16(clampyx, clampyx),
                     _mm_set1_epi32((sampler->stride - 1) | 0x10000));
  row0 = _mm_add_epi32(row0, _mm_unpackhi_epi16(clampyx, _mm_setzero_si128()));
  // Add in layer offset if available
  row0 = _mm_add_epi32(row0, _mm_set1_epi32(zoffset));

  __m128i fracyx = _mm_packs_epi32(frac.y, frac.x);

  // Check if coords were clamped at all above. If so, need to adjust fractions
  // to avoid sampling outside the texture on the edges.
  __m128i yxinside = _mm_andnot_si128(_mm_cmplt_epi16(yx, _mm_setzero_si128()),
                                      _mm_cmplt_epi16(yx, hw));
  // Set fraction to zero when outside.
  fracyx = _mm_and_si128(fracyx, yxinside);
  // For X fraction, we need to store 1-fraction before each fraction, as a
  // madd will be used to weight and collapse all results as last step.
  __m128i fracx =
      _mm_unpackhi_epi16(_mm_sub_epi16(_mm_set1_epi16(256), fracyx), fracyx);
  // Store two side-by-side copies of Y fraction, as below each pixel value
  // will be interleaved to be next to the pixel value for the next column.
  __m128i fracy = _mm_unpacklo_epi16(fracyx, fracyx);

  // Ensure we don't sample row off end of texture from added stride.
  __m128i row1 = _mm_and_si128(yxinside, _mm_set1_epi16(sampler->stride));

  // Calculate pointers for first row in each lane
  uint8_t* buf = (uint8_t*)sampler->buf;
  uint8_t* buf0 =
      buf + _mm_cvtsi128_si32(_mm_shuffle_epi32(row0, _MM_SHUFFLE(0, 0, 0, 0)));
  uint8_t* buf1 =
      buf + _mm_cvtsi128_si32(_mm_shuffle_epi32(row0, _MM_SHUFFLE(1, 1, 1, 1)));
  uint8_t* buf2 =
      buf + _mm_cvtsi128_si32(_mm_shuffle_epi32(row0, _MM_SHUFFLE(2, 2, 2, 2)));
  uint8_t* buf3 =
      buf + _mm_cvtsi128_si32(_mm_shuffle_epi32(row0, _MM_SHUFFLE(3, 3, 3, 3)));
  // Load adjacent columns from first row, pack into register, then expand.
  __m128i cc0 = _mm_unpacklo_epi8(
      _mm_setr_epi16(*(uint16_t*)buf0, *(uint16_t*)buf1, *(uint16_t*)buf2,
                     *(uint16_t*)buf3, 0, 0, 0, 0),
      _mm_setzero_si128());
  // Load adjacent columns from next row, pack into register, then expand.
  __m128i cc1 = _mm_unpacklo_epi8(
      _mm_setr_epi16(*(uint16_t*)(buf0 + _mm_extract_epi16(row1, 0)),
                     *(uint16_t*)(buf1 + _mm_extract_epi16(row1, 1)),
                     *(uint16_t*)(buf2 + _mm_extract_epi16(row1, 2)),
                     *(uint16_t*)(buf3 + _mm_extract_epi16(row1, 3)), 0, 0, 0,
                     0),
      _mm_setzero_si128());
  // Multiply then add rows with 8-bit precision so we don't carry to high byte
  // of word accidentally. Use final madd insn to blend interleaved columns and
  // expand result to 32 bits.
  __m128i cc = _mm_add_epi8(
      cc0, _mm_srli_epi16(_mm_mullo_epi16(_mm_sub_epi16(cc1, cc0), fracy), 8));
  __m128 r = _mm_cvtepi32_ps(_mm_madd_epi16(cc, fracx));
  return vec4((Float)r * (1.0f / 0xFF00), 0.0f, 0.0f, 1.0f);
#else
  ivec2 i(linearQuantize(P, 128, sampler));
  Float r = CONVERT(textureLinearPackedR8(sampler, i, zoffset), Float);
  return vec4(r * (1.0f / 255.0f), 0.0f, 0.0f, 1.0f);
#endif
}

template <typename S>
vec4 textureLinearRG8(S sampler, vec2 P, int32_t zoffset = 0) {
  assert(sampler->format == TextureFormat::RG8);

  ivec2 i(linearQuantize(P, 128, sampler));
  ivec2 frac = i & (I32)0x7F;
  i >>= 7;

  I32 row0 = clampCoord(i.x, sampler->width) +
             clampCoord(i.y, sampler->height) * sampler->stride + zoffset;
  I32 row1 = row0 + ((i.y >= 0 && i.y < int32_t(sampler->height) - 1) &
                     I32(sampler->stride));
  I16 fracx =
      CONVERT(frac.x & (i.x >= 0 && i.x < int32_t(sampler->width) - 1), I16);
  I16 fracy = CONVERT(frac.y, I16);

  uint16_t* buf = (uint16_t*)sampler->buf;

  // Load RG bytes for two adjacent pixels - rgRG
  auto a0 = unaligned_load<V4<uint8_t>>(&buf[row0.x]);
  auto b0 = unaligned_load<V4<uint8_t>>(&buf[row0.y]);
  auto ab0 = CONVERT(combine(a0, b0), V8<int16_t>);
  // Load two pixels for next row
  auto a1 = unaligned_load<V4<uint8_t>>(&buf[row1.x]);
  auto b1 = unaligned_load<V4<uint8_t>>(&buf[row1.y]);
  auto ab1 = CONVERT(combine(a1, b1), V8<int16_t>);
  // Blend rows
  ab0 += ((ab1 - ab0) * fracy.xxxxyyyy) >> 7;

  auto c0 = unaligned_load<V4<uint8_t>>(&buf[row0.z]);
  auto d0 = unaligned_load<V4<uint8_t>>(&buf[row0.w]);
  auto cd0 = CONVERT(combine(c0, d0), V8<int16_t>);
  auto c1 = unaligned_load<V4<uint8_t>>(&buf[row1.z]);
  auto d1 = unaligned_load<V4<uint8_t>>(&buf[row1.w]);
  auto cd1 = CONVERT(combine(c1, d1), V8<int16_t>);
  // Blend rows
  cd0 += ((cd1 - cd0) * fracy.zzzzwwww) >> 7;

  // ab = a.rgRG,b.rgRG
  // cd = c.rgRG,d.rgRG
  // ... ac = ar,cr,ag,cg,aR,cR,aG,cG
  // ... bd = br,dr,bg,dg,bR,dR,bG,dG
  auto ac = zipLow(ab0, cd0);
  auto bd = zipHigh(ab0, cd0);
  // ar,br,cr,dr,ag,bg,cg,dg
  // aR,bR,cR,dR,aG,bG,cG,dG
  auto abcdl = zipLow(ac, bd);
  auto abcdh = zipHigh(ac, bd);
  // Blend columns
  abcdl += ((abcdh - abcdl) * fracx.xyzwxyzw) >> 7;

  auto rg = CONVERT(V8<uint16_t>(abcdl), V8<float>) * (1.0f / 255.0f);
  auto r = lowHalf(rg);
  auto g = highHalf(rg);

  return vec4(r, g, 0.0f, 1.0f);
}

// Samples R16 texture with linear filtering and returns results packed as
// signed I16. One bit of precision is shifted away from the bottom end to
// accommodate the sign bit, so only 15 bits of precision is left.
template <typename S>
static inline I16 textureLinearPackedR16(S sampler, ivec2 i,
                                         int32_t zoffset = 0) {
  assert(sampler->format == TextureFormat::R16);

  ivec2 frac = i & (I32)0x7F;
  i >>= 7;

  I32 row0 = clampCoord(i.x, sampler->width) +
             clampCoord(i.y, sampler->height) * sampler->stride + zoffset;
  I32 row1 = row0 + ((i.y >= 0 && i.y < int32_t(sampler->height) - 1) &
                     I32(sampler->stride));

  I16 fracx =
      CONVERT(frac.x & (i.x >= 0 && i.x < int32_t(sampler->width) - 1), I16)
      << 8;
  I16 fracy = CONVERT(frac.y, I16) << 8;

  // Sample the 16 bit data for both rows
  uint16_t* buf = (uint16_t*)sampler->buf;
  auto a0 = unaligned_load<V2<uint16_t>>(&buf[row0.x]);
  auto b0 = unaligned_load<V2<uint16_t>>(&buf[row0.y]);
  auto c0 = unaligned_load<V2<uint16_t>>(&buf[row0.z]);
  auto d0 = unaligned_load<V2<uint16_t>>(&buf[row0.w]);
  auto abcd0 =
      CONVERT(combine(combine(a0, b0), combine(c0, d0)) >> 1, V8<int16_t>);

  auto a1 = unaligned_load<V2<uint16_t>>(&buf[row1.x]);
  auto b1 = unaligned_load<V2<uint16_t>>(&buf[row1.y]);
  auto c1 = unaligned_load<V2<uint16_t>>(&buf[row1.z]);
  auto d1 = unaligned_load<V2<uint16_t>>(&buf[row1.w]);
  auto abcd1 =
      CONVERT(combine(combine(a1, b1), combine(c1, d1)) >> 1, V8<int16_t>);

  // The samples occupy 15 bits and the fraction occupies 15 bits, so that when
  // they are multiplied together, the new scaled sample will fit in the high
  // 14 bits of the result. It is left shifted once to make it 15 bits again
  // for the final multiply.
#if USE_SSE2
  abcd0 += bit_cast<V8<int16_t>>(_mm_mulhi_epi16(abcd1 - abcd0, fracy.xxyyzzww))
           << 1;
#elif USE_NEON
  // NEON has a convenient instruction that does both the multiply and the
  // doubling, so doesn't need an extra shift.
  abcd0 += bit_cast<V8<int16_t>>(vqrdmulhq_s16(abcd1 - abcd0, fracy.xxyyzzww));
#else
  abcd0 += CONVERT((CONVERT(abcd1 - abcd0, V8<int32_t>) *
                    CONVERT(fracy.xxyyzzww, V8<int32_t>)) >>
                       16,
                   V8<int16_t>)
           << 1;
#endif

  abcd0 = SHUFFLE(abcd0, abcd0, 0, 2, 4, 6, 1, 3, 5, 7);
  auto abcdl = lowHalf(abcd0);
  auto abcdh = highHalf(abcd0);
#if USE_SSE2
  abcdl += lowHalf(bit_cast<V8<int16_t>>(
               _mm_mulhi_epi16(expand(abcdh - abcdl), expand(fracx))))
           << 1;
#elif USE_NEON
  abcdl += bit_cast<V4<int16_t>>(vqrdmulh_s16(abcdh - abcdl, fracx));
#else
  abcdl += CONVERT((CONVERT(abcdh - abcdl, V4<int32_t>) *
                    CONVERT(fracx, V4<int32_t>)) >>
                       16,
                   V4<int16_t>)
           << 1;
#endif

  return abcdl;
}

template <typename S>
vec4 textureLinearR16(S sampler, vec2 P, int32_t zoffset = 0) {
  assert(sampler->format == TextureFormat::R16);

  ivec2 i(linearQuantize(P, 128, sampler));
  Float r = CONVERT(textureLinearPackedR16(sampler, i, zoffset), Float);
  return vec4(r * (1.0f / 32767.0f), 0.0f, 0.0f, 1.0f);
}

template <typename S>
vec4 textureLinearRGBA32F(S sampler, vec2 P, int32_t zoffset = 0) {
  assert(sampler->format == TextureFormat::RGBA32F);
  P.x *= sampler->width;
  P.y *= sampler->height;
  P -= 0.5f;
  vec2 f = floor(P);
  vec2 r = P - f;
  ivec2 i(f);
  ivec2 c = clamp2D(i, sampler);
  r.x = if_then_else(i.x >= 0 && i.x < sampler->width - 1, r.x, 0.0f);
  I32 offset0 = c.x * 4 + c.y * sampler->stride + zoffset;
  I32 offset1 = offset0 + ((i.y >= 0 && i.y < int32_t(sampler->height) - 1) &
                           I32(sampler->stride));

  Float c0 = mix(mix(*(Float*)&sampler->buf[offset0.x],
                     *(Float*)&sampler->buf[offset0.x + 4], r.x),
                 mix(*(Float*)&sampler->buf[offset1.x],
                     *(Float*)&sampler->buf[offset1.x + 4], r.x),
                 r.y);
  Float c1 = mix(mix(*(Float*)&sampler->buf[offset0.y],
                     *(Float*)&sampler->buf[offset0.y + 4], r.x),
                 mix(*(Float*)&sampler->buf[offset1.y],
                     *(Float*)&sampler->buf[offset1.y + 4], r.x),
                 r.y);
  Float c2 = mix(mix(*(Float*)&sampler->buf[offset0.z],
                     *(Float*)&sampler->buf[offset0.z + 4], r.x),
                 mix(*(Float*)&sampler->buf[offset1.z],
                     *(Float*)&sampler->buf[offset1.z + 4], r.x),
                 r.y);
  Float c3 = mix(mix(*(Float*)&sampler->buf[offset0.w],
                     *(Float*)&sampler->buf[offset0.w + 4], r.x),
                 mix(*(Float*)&sampler->buf[offset1.w],
                     *(Float*)&sampler->buf[offset1.w + 4], r.x),
                 r.y);
  return pixel_float_to_vec4(c0, c1, c2, c3);
}

template <typename S>
vec4 textureLinearYUV422(S sampler, vec2 P, int32_t zoffset = 0) {
  assert(sampler->format == TextureFormat::YUV422);

  ivec2 i(linearQuantize(P, 128, sampler));
  ivec2 frac = i & (I32)0x7F;
  i >>= 7;

  I32 row0 = clampCoord(i.x, sampler->width) +
             clampCoord(i.y, sampler->height) * sampler->stride + zoffset;
  // Layout is 2 pixel chunks (occupying 4 bytes) organized as: G0, B, G1, R.
  // Get the selector for the pixel within the chunk.
  I32 selector = row0 & 1;
  // Align the row index to the chunk.
  row0 &= ~1;
  I32 row1 = row0 + ((i.y >= 0 && i.y < int32_t(sampler->height) - 1) &
                     I32(sampler->stride));
  // G only needs to be clamped to a pixel boundary for safe interpolation,
  // whereas the BR fraction needs to be clamped 1 extra pixel inside to a chunk
  // boundary.
  frac.x &= (i.x >= 0);
  auto fracx = CONVERT(combine(frac.x & (i.x < int32_t(sampler->width) - 1),
                               ((frac.x >> 1) | (selector << 6)) &
                                   (i.x < int32_t(sampler->width) - 2)),
                       V8<int16_t>);
  I16 fracy = CONVERT(frac.y, I16);

  uint16_t* buf = (uint16_t*)sampler->buf;

  // Load bytes for two adjacent chunks - g0,b,g1,r,G0,B,G1,R
  // We always need to interpolate between (b,r) and (B,R).
  // Depending on selector we need to either interpolate between g0 and g1
  // or between g1 and G0. So for now we just interpolate both cases for g
  // and will select the appropriate one on output.
  auto a0 = CONVERT(unaligned_load<V8<uint8_t>>(&buf[row0.x]), V8<int16_t>);
  auto a1 = CONVERT(unaligned_load<V8<uint8_t>>(&buf[row1.x]), V8<int16_t>);
  // Combine with next row.
  a0 += ((a1 - a0) * fracy.x) >> 7;

  auto b0 = CONVERT(unaligned_load<V8<uint8_t>>(&buf[row0.y]), V8<int16_t>);
  auto b1 = CONVERT(unaligned_load<V8<uint8_t>>(&buf[row1.y]), V8<int16_t>);
  b0 += ((b1 - b0) * fracy.y) >> 7;

  auto c0 = CONVERT(unaligned_load<V8<uint8_t>>(&buf[row0.z]), V8<int16_t>);
  auto c1 = CONVERT(unaligned_load<V8<uint8_t>>(&buf[row1.z]), V8<int16_t>);
  c0 += ((c1 - c0) * fracy.z) >> 7;

  auto d0 = CONVERT(unaligned_load<V8<uint8_t>>(&buf[row0.w]), V8<int16_t>);
  auto d1 = CONVERT(unaligned_load<V8<uint8_t>>(&buf[row1.w]), V8<int16_t>);
  d0 += ((d1 - d0) * fracy.w) >> 7;

  // Shuffle things around so we end up with g0,g0,g0,g0,b,b,b,b and
  // g1,g1,g1,g1,r,r,r,r.
  auto abl = zipLow(a0, b0);
  auto cdl = zipLow(c0, d0);
  auto g0b = zip2Low(abl, cdl);
  auto g1r = zip2High(abl, cdl);

  // Need to zip g1,B,G0,R. Instead of using a bunch of complicated masks and
  // and shifts, just shuffle here instead... We finally end up with
  // g1,g1,g1,g1,B,B,B,B and G0,G0,G0,G0,R,R,R,R.
  auto abh = SHUFFLE(a0, b0, 2, 10, 5, 13, 4, 12, 7, 15);
  auto cdh = SHUFFLE(c0, d0, 2, 10, 5, 13, 4, 12, 7, 15);
  auto g1B = zip2Low(abh, cdh);
  auto G0R = zip2High(abh, cdh);

  // Finally interpolate between adjacent columns.
  g0b += ((g1B - g0b) * fracx) >> 7;
  g1r += ((G0R - g1r) * fracx) >> 7;

  auto g0bf = CONVERT(V8<uint16_t>(g0b), V8<float>);
  auto g1rf = CONVERT(V8<uint16_t>(g1r), V8<float>);

  // Choose either g0 or g1 based on selector.
  return vec4(
      highHalf(g1rf) * (1.0f / 255.0f),
      if_then_else(-selector, lowHalf(g1rf), lowHalf(g0bf)) * (1.0f / 255.0f),
      highHalf(g0bf) * (1.0f / 255.0f), 1.0f);
}

SI vec4 texture(sampler2D sampler, vec2 P) {
  if (sampler->filter == TextureFilter::LINEAR) {
    switch (sampler->format) {
      case TextureFormat::RGBA32F:
        return textureLinearRGBA32F(sampler, P);
      case TextureFormat::RGBA8:
        return textureLinearRGBA8(sampler, P);
      case TextureFormat::R8:
        return textureLinearR8(sampler, P);
      case TextureFormat::RG8:
        return textureLinearRG8(sampler, P);
      case TextureFormat::R16:
        return textureLinearR16(sampler, P);
      case TextureFormat::YUV422:
        return textureLinearYUV422(sampler, P);
      default:
        assert(false);
        return vec4();
    }
  } else {
    ivec2 coord(roundzero(P.x, sampler->width),
                roundzero(P.y, sampler->height));
    return texelFetch(sampler, coord, 0);
  }
}

vec4 texture(sampler2DRect sampler, vec2 P) {
  if (sampler->filter == TextureFilter::LINEAR) {
    switch (sampler->format) {
      case TextureFormat::RGBA8:
        return textureLinearRGBA8(sampler, P);
      case TextureFormat::R8:
        return textureLinearR8(sampler, P);
      case TextureFormat::RG8:
        return textureLinearRG8(sampler, P);
      case TextureFormat::R16:
        return textureLinearR16(sampler, P);
      case TextureFormat::YUV422:
        return textureLinearYUV422(sampler, P);
      default:
        assert(false);
        return vec4();
    }
  } else {
    ivec2 coord(roundzero(P.x, 1.0f), roundzero(P.y, 1.0f));
    return texelFetch(sampler, coord);
  }
}

SI vec4 texture(sampler2DArray sampler, vec3 P) {
  if (sampler->filter == TextureFilter::LINEAR) {
    // SSE2 can generate slow code for 32-bit multiply, and we never actually
    // sample from different layers in one chunk, so do cheaper scalar
    // multiplication instead.
    assert(test_all(P.z == P.z.x));
    int32_t zoffset = clampCoord(roundeven(P.z.x, 1.0f), sampler->depth) *
                      sampler->height_stride;
    switch (sampler->format) {
      case TextureFormat::RGBA32F:
        return textureLinearRGBA32F(sampler, vec2(P.x, P.y), zoffset);
      case TextureFormat::RGBA8:
        return textureLinearRGBA8(sampler, vec2(P.x, P.y), zoffset);
      case TextureFormat::R8:
        return textureLinearR8(sampler, vec2(P.x, P.y), zoffset);
      case TextureFormat::RG8:
        return textureLinearRG8(sampler, vec2(P.x, P.y), zoffset);
      case TextureFormat::R16:
        return textureLinearR16(sampler, vec2(P.x, P.y), zoffset);
      default:
        assert(false);
        return vec4();
    }
  } else {
    // just do nearest for now
    ivec3 coord(roundzero(P.x, sampler->width), roundzero(P.y, sampler->height),
                roundeven(P.z, 1.0f));
    return texelFetch(sampler, coord, 0);
  }
}

vec4 texture(sampler2DArray sampler, vec3 P, float bias) {
  assert(bias == 0.0f);
  return texture(sampler, P);
}

vec4 textureLod(sampler2DArray sampler, vec3 P, float lod) {
  assert(lod == 0.0f);
  return texture(sampler, P);
}

ivec3_scalar textureSize(sampler2DArray sampler, int) {
  return ivec3_scalar{int32_t(sampler->width), int32_t(sampler->height),
                      int32_t(sampler->depth)};
}

ivec2_scalar textureSize(sampler2D sampler, int) {
  return ivec2_scalar{int32_t(sampler->width), int32_t(sampler->height)};
}

ivec2_scalar textureSize(sampler2DRect sampler) {
  return ivec2_scalar{int32_t(sampler->width), int32_t(sampler->height)};
}

template <typename S>
static WideRGBA8 textureLinearUnpackedRGBA8(S sampler, ivec2 i, int zoffset) {
  assert(sampler->format == TextureFormat::RGBA8);
  ivec2 frac = i & 0x7F;
  i >>= 7;

  I32 row0 = clampCoord(i.x, sampler->width) +
             clampCoord(i.y, sampler->height) * sampler->stride + zoffset;
  I32 row1 = row0 + ((i.y >= 0 && i.y < int32_t(sampler->height) - 1) &
                     I32(sampler->stride));
  I16 fracx =
      CONVERT(frac.x & (i.x >= 0 && i.x < int32_t(sampler->width) - 1), I16);
  I16 fracy = CONVERT(frac.y, I16);

  auto a0 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row0.x]), V8<int16_t>);
  auto a1 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row1.x]), V8<int16_t>);
  a0 += ((a1 - a0) * fracy.x) >> 7;

  auto b0 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row0.y]), V8<int16_t>);
  auto b1 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row1.y]), V8<int16_t>);
  b0 += ((b1 - b0) * fracy.y) >> 7;

  auto abl = combine(lowHalf(a0), lowHalf(b0));
  auto abh = combine(highHalf(a0), highHalf(b0));
  abl += ((abh - abl) * fracx.xxxxyyyy) >> 7;

  auto c0 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row0.z]), V8<int16_t>);
  auto c1 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row1.z]), V8<int16_t>);
  c0 += ((c1 - c0) * fracy.z) >> 7;

  auto d0 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row0.w]), V8<int16_t>);
  auto d1 =
      CONVERT(unaligned_load<V8<uint8_t>>(&sampler->buf[row1.w]), V8<int16_t>);
  d0 += ((d1 - d0) * fracy.w) >> 7;

  auto cdl = combine(lowHalf(c0), lowHalf(d0));
  auto cdh = combine(highHalf(c0), highHalf(d0));
  cdl += ((cdh - cdl) * fracx.zzzzwwww) >> 7;

  return combine(HalfRGBA8(abl), HalfRGBA8(cdl));
}

template <typename S>
static PackedRGBA8 textureLinearPackedRGBA8(S sampler, ivec2 i, int zoffset) {
  return pack(textureLinearUnpackedRGBA8(sampler, i, zoffset));
}

template <typename S>
static inline void textureLinearCommit4(S sampler, ivec2 i, int zoffset,
                                        uint32_t* buf) {
  commit_span(buf, textureLinearPackedRGBA8(sampler, i, zoffset));
}

template <typename S>
static void textureLinearCommit8(S sampler, ivec2_scalar i, int zoffset,
                                 uint32_t* buf) {
  assert(sampler->format == TextureFormat::RGBA8);
  ivec2_scalar frac = i & 0x7F;
  i >>= 7;

  uint32_t* row0 =
      &sampler
           ->buf[clampCoord(i.x, sampler->width) +
                 clampCoord(i.y, sampler->height) * sampler->stride + zoffset];
  uint32_t* row1 =
      row0 +
      ((i.y >= 0 && i.y < int32_t(sampler->height) - 1) ? sampler->stride : 0);
  int16_t fracx = i.x >= 0 && i.x < int32_t(sampler->width) - 1 ? frac.x : 0;
  int16_t fracy = frac.y;

  U32 pix0 = unaligned_load<U32>(row0);
  U32 pix0n = unaligned_load<U32>(row0 + 4);
  uint32_t pix0x = row0[8];
  U32 pix1 = unaligned_load<U32>(row1);
  U32 pix1n = unaligned_load<U32>(row1 + 4);
  uint32_t pix1x = row1[8];

  {
    auto ab0 = CONVERT(bit_cast<V16<uint8_t>>(SHUFFLE(pix0, pix0, 0, 1, 1, 2)),
                       V16<int16_t>);
    auto ab1 = CONVERT(bit_cast<V16<uint8_t>>(SHUFFLE(pix1, pix1, 0, 1, 1, 2)),
                       V16<int16_t>);
    ab0 += ((ab1 - ab0) * fracy) >> 7;

    auto cd0 = CONVERT(bit_cast<V16<uint8_t>>(SHUFFLE(pix0, pix0n, 2, 3, 3, 4)),
                       V16<int16_t>);
    auto cd1 = CONVERT(bit_cast<V16<uint8_t>>(SHUFFLE(pix1, pix1n, 2, 3, 3, 4)),
                       V16<int16_t>);
    cd0 += ((cd1 - cd0) * fracy) >> 7;

    auto abcdl = combine(lowHalf(ab0), lowHalf(cd0));
    auto abcdh = combine(highHalf(ab0), highHalf(cd0));
    abcdl += ((abcdh - abcdl) * fracx) >> 7;

    commit_span(buf, pack(WideRGBA8(abcdl)));
  }

  {
    auto ab0 =
        CONVERT(bit_cast<V16<uint8_t>>(SHUFFLE(pix0n, pix0n, 0, 1, 1, 2)),
                V16<int16_t>);
    auto ab1 =
        CONVERT(bit_cast<V16<uint8_t>>(SHUFFLE(pix1n, pix1n, 0, 1, 1, 2)),
                V16<int16_t>);
    ab0 += ((ab1 - ab0) * fracy) >> 7;

    auto cd0 =
        CONVERT(bit_cast<V16<uint8_t>>(SHUFFLE(pix0n, U32(pix0x), 2, 3, 3, 4)),
                V16<int16_t>);
    auto cd1 =
        CONVERT(bit_cast<V16<uint8_t>>(SHUFFLE(pix1n, U32(pix1x), 2, 3, 3, 4)),
                V16<int16_t>);
    cd0 += ((cd1 - cd0) * fracy) >> 7;

    auto abcdl = combine(lowHalf(ab0), lowHalf(cd0));
    auto abcdh = combine(highHalf(ab0), highHalf(cd0));
    abcdl += ((abcdh - abcdl) * fracx) >> 7;

    commit_span(buf + 4, pack(WideRGBA8(abcdl)));
  }
}

template <typename S>
static PackedRG8 textureLinearPackedRG8(S sampler, ivec2 i, int zoffset) {
  assert(sampler->format == TextureFormat::RG8);
  ivec2 frac = i & 0x7F;
  i >>= 7;

  I32 row0 = clampCoord(i.x, sampler->width) +
             clampCoord(i.y, sampler->height) * sampler->stride + zoffset;
  I32 row1 = row0 + ((i.y >= 0 && i.y < int32_t(sampler->height) - 1) &
                     I32(sampler->stride));
  I16 fracx =
      CONVERT(frac.x & (i.x >= 0 && i.x < int32_t(sampler->width) - 1), I16);
  I16 fracy = CONVERT(frac.y, I16);

  uint16_t* buf = (uint16_t*)sampler->buf;

  // Load RG bytes for two adjacent pixels - rgRG
  auto a0 = unaligned_load<V4<uint8_t>>(&buf[row0.x]);
  auto b0 = unaligned_load<V4<uint8_t>>(&buf[row0.y]);
  auto ab0 = CONVERT(combine(a0, b0), V8<int16_t>);
  // Load two pixels for next row
  auto a1 = unaligned_load<V4<uint8_t>>(&buf[row1.x]);
  auto b1 = unaligned_load<V4<uint8_t>>(&buf[row1.y]);
  auto ab1 = CONVERT(combine(a1, b1), V8<int16_t>);
  // Blend rows
  ab0 += ((ab1 - ab0) * fracy.xxxxyyyy) >> 7;

  auto c0 = unaligned_load<V4<uint8_t>>(&buf[row0.z]);
  auto d0 = unaligned_load<V4<uint8_t>>(&buf[row0.w]);
  auto cd0 = CONVERT(combine(c0, d0), V8<int16_t>);
  auto c1 = unaligned_load<V4<uint8_t>>(&buf[row1.z]);
  auto d1 = unaligned_load<V4<uint8_t>>(&buf[row1.w]);
  auto cd1 = CONVERT(combine(c1, d1), V8<int16_t>);
  // Blend rows
  cd0 += ((cd1 - cd0) * fracy.zzzzwwww) >> 7;

  // ab = a.rgRG,b.rgRG
  // cd = c.rgRG,d.rgRG
  // ... ac = a.rg,c.rg,a.RG,c.RG
  // ... bd = b.rg,d.rg,b.RG,d.RG
  auto ac = zip2Low(ab0, cd0);
  auto bd = zip2High(ab0, cd0);
  // a.rg,b.rg,c.rg,d.rg
  // a.RG,b.RG,c.RG,d.RG
  auto abcdl = zip2Low(ac, bd);
  auto abcdh = zip2High(ac, bd);
  // Blend columns
  abcdl += ((abcdh - abcdl) * fracx.xxyyzzww) >> 7;

  return pack(WideRG8(abcdl));
}

}  // namespace glsl
