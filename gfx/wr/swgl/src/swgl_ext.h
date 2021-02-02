/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

static ALWAYS_INLINE void commit_span(uint32_t* buf, WideRGBA8 r) {
  if (blend_key) r = blend_pixels(buf, unaligned_load<PackedRGBA8>(buf), r);
  unaligned_store(buf, pack(r));
}

static ALWAYS_INLINE void commit_span(uint32_t* buf, PackedRGBA8 r) {
  if (blend_key)
    r = pack(blend_pixels(buf, unaligned_load<PackedRGBA8>(buf), unpack(r)));
  unaligned_store(buf, r);
}

UNUSED static inline void commit_solid_span(uint32_t* buf, WideRGBA8 r,
                                            int len) {
  if (blend_key) {
    for (uint32_t* end = &buf[len & ~3]; buf < end; buf += 4) {
      unaligned_store(
          buf, pack(blend_pixels(buf, unaligned_load<PackedRGBA8>(buf), r)));
    }
    len &= 3;
    if (len > 0) {
      partial_store_span(
          buf,
          pack(blend_pixels(buf, partial_load_span<PackedRGBA8>(buf, len), r,
                            len)),
          len);
    }
  } else {
    fill_n(buf, len, bit_cast<U32>(pack(r)).x);
  }
}

static ALWAYS_INLINE void commit_span(uint8_t* buf, WideR8 r) {
  if (blend_key)
    r = blend_pixels(buf, unpack(unaligned_load<PackedR8>(buf)), r);
  unaligned_store(buf, pack(r));
}

UNUSED static inline void commit_solid_span(uint8_t* buf, WideR8 r, int len) {
  if (blend_key) {
    for (uint8_t* end = &buf[len]; buf < end; buf += 4) {
      unaligned_store(buf, pack(blend_pixels(
                               buf, unpack(unaligned_load<PackedR8>(buf)), r)));
    }
  } else {
    fill_n((uint32_t*)buf, len / 4, bit_cast<uint32_t>(pack(r)));
  }
}

template <typename V>
static ALWAYS_INLINE WideRGBA8 pack_span(uint32_t*, const V& v) {
  return pack_pixels_RGBA8(v);
}

static ALWAYS_INLINE WideRGBA8 pack_span(uint32_t*) {
  return pack_pixels_RGBA8();
}

template <typename C>
static ALWAYS_INLINE WideR8 pack_span(uint8_t*, C c) {
  return pack_pixels_R8(c);
}

static ALWAYS_INLINE WideR8 pack_span(uint8_t*) { return pack_pixels_R8(); }

// Forces a value with vector run-class to have scalar run-class.
template <typename T>
static ALWAYS_INLINE auto swgl_forceScalar(T v) -> decltype(force_scalar(v)) {
  return force_scalar(v);
}

// Advance all varying inperpolants by a single chunk
#define swgl_stepInterp() step_interp_inputs()

// Pseudo-intrinsic that accesses the interpolation step for a given varying
#define swgl_interpStep(v) (interp_step.v)

// Commit an entire span of a solid color
#define swgl_commitSolid(format, v)                                       \
  do {                                                                    \
    commit_solid_span(swgl_Out##format, pack_span(swgl_Out##format, (v)), \
                      swgl_SpanLength);                                   \
    swgl_Out##format += swgl_SpanLength;                                  \
    swgl_SpanLength = 0;                                                  \
  } while (0)
#define swgl_commitSolidRGBA8(v) swgl_commitSolid(RGBA8, v)
#define swgl_commitSolidR8(v) swgl_commitSolid(R8, v)

#define swgl_commitChunk(format, chunk)   \
  do {                                    \
    commit_span(swgl_Out##format, chunk); \
    swgl_Out##format += swgl_StepSize;    \
    swgl_SpanLength -= swgl_StepSize;     \
  } while (0)

static ALWAYS_INLINE WideRGBA8 pack_pixels_RGBA8(Float alpha) {
  I32 i = round_pixel(alpha);
  HalfRGBA8 c = packRGBA8(zipLow(i, i), zipHigh(i, i));
  return combine(zipLow(c, c), zipHigh(c, c));
}

static ALWAYS_INLINE WideRGBA8 pack_pixels_RGBA8(float alpha) {
  I32 i = round_pixel(alpha);
  HalfRGBA8 c = packRGBA8(i, i);
  return combine(c, c);
}

// Commit a single chunk of a color scaled by an alpha weight
#define swgl_commitColor(format, color, alpha)                    \
  swgl_commitChunk(format, muldiv255(pack_pixels_##format(color), \
                                     pack_pixels_##format(alpha)))
#define swgl_commitColorRGBA8(color, alpha) \
  swgl_commitColor(RGBA8, color, alpha)
#define swgl_commitColorR8(color, alpha) swgl_commitColor(R8, color, alpha)

template <typename S>
static ALWAYS_INLINE bool swgl_isTextureLinear(S s) {
  return s->filter == TextureFilter::LINEAR;
}

template <typename S>
static ALWAYS_INLINE bool swgl_isTextureRGBA8(S s) {
  return s->format == TextureFormat::RGBA8;
}

template <typename S>
static ALWAYS_INLINE bool swgl_isTextureR8(S s) {
  return s->format == TextureFormat::R8;
}

// Returns the offset into the texture buffer for the given layer index. If not
// a texture array or 3D texture, this will always access the first layer.
template <typename S>
static ALWAYS_INLINE int swgl_textureLayerOffset(S s, float layer) {
  return 0;
}

UNUSED static ALWAYS_INLINE int swgl_textureLayerOffset(sampler2DArray s,
                                                        float layer) {
  return clampCoord(int(layer), s->depth) * s->height_stride;
}

// Use the default linear quantization scale of 128. This gives 7 bits of
// fractional precision, which when multiplied with a signed 9 bit value
// still fits in a 16 bit integer.
const int swgl_LinearQuantizeScale = 128;

// Quantizes UVs for access into a linear texture.
template <typename S, typename T>
static ALWAYS_INLINE T swgl_linearQuantize(S s, T p) {
  return linearQuantize(p, swgl_LinearQuantizeScale, s);
}

// Quantizes an interpolation step for UVs for access into a linear texture.
template <typename S, typename T>
static ALWAYS_INLINE T swgl_linearQuantizeStep(S s, T p) {
  return samplerScale(s, p) * swgl_LinearQuantizeScale;
}

// Commit a single chunk from a linear texture fetch
#define swgl_commitTextureLinear(format, s, p, ...) \
  swgl_commitChunk(format,                          \
                   textureLinearUnpacked##format(s, ivec2(p), __VA_ARGS__))
#define swgl_commitTextureLinearRGBA8(s, p, ...) \
  swgl_commitTextureLinear(RGBA8, s, p, __VA_ARGS__)
#define swgl_commitTextureLinearR8(s, p, ...) \
  swgl_commitTextureLinear(R8, s, p, __VA_ARGS__)

// Commit a single chunk from a linear texture fetch that is scaled by a color
#define swgl_commitTextureLinearColor(format, s, p, color, ...)     \
  swgl_commitChunk(format, muldiv255(textureLinearUnpacked##format( \
                                         s, ivec2(p), __VA_ARGS__), \
                                     pack_pixels_##format(color)))
#define swgl_commitTextureLinearColorRGBA8(s, p, color, ...) \
  swgl_commitTextureLinearColor(RGBA8, s, p, color, __VA_ARGS__)
#define swgl_commitTextureLinearColorR8(s, p, color, ...) \
  swgl_commitTextureLinearColor(R8, s, p, color, __VA_ARGS__)

// Commit an entire span of a separable pass of a Gaussian blur that falls
// within the given radius scaled by supplied coefficients, clamped to uv_rect
// bounds.
#define swgl_commitGaussianBlur(format, type, s, p, uv_rect, hori, radius, \
                                coeffs, ...)                               \
  do {                                                                     \
    vec2_scalar size = {float(s->width), float(s->height)};                \
    ivec2_scalar curUV = make_ivec2(force_scalar(p) * size);               \
    ivec4_scalar bounds = make_ivec4(uv_rect * make_vec4(size, size));     \
    int endX = min(bounds.z, curUV.x + swgl_SpanLength * swgl_StepSize);   \
    if (hori) {                                                            \
      for (; curUV.x + swgl_StepSize <= endX; curUV.x += swgl_StepSize) {  \
        swgl_commitChunk(format, gaussianBlurHorizontal<type>(             \
                                     s, curUV, bounds.x, bounds.z, radius, \
                                     coeffs.x, coeffs.y, __VA_ARGS__));    \
      }                                                                    \
    } else {                                                               \
      for (; curUV.x + swgl_StepSize <= endX; curUV.x += swgl_StepSize) {  \
        swgl_commitChunk(format, gaussianBlurVertical<type>(               \
                                     s, curUV, bounds.y, bounds.w, radius, \
                                     coeffs.x, coeffs.y, __VA_ARGS__));    \
      }                                                                    \
    }                                                                      \
  } while (0)
#define swgl_commitGaussianBlurRGBA8(s, p, uv_rect, hori, radius, coeffs, ...) \
  swgl_commitGaussianBlur(RGBA8, uint32_t, s, p, uv_rect, hori, radius,        \
                          coeffs, __VA_ARGS__)
#define swgl_commitGaussianBlurR8(s, p, uv_rect, hori, radius, coeffs, ...) \
  swgl_commitGaussianBlur(R8, uint8_t, s, p, uv_rect, hori, radius, coeffs, \
                          __VA_ARGS__)

// Convert and pack planar YUV samples to RGB output using a color space
static ALWAYS_INLINE PackedRGBA8 convertYUV(int colorSpace, U16 y, U16 u,
                                            U16 v) {
  auto yy = V8<int16_t>(zip(y, y));
  auto uv = V8<int16_t>(zip(u, v));
  switch (colorSpace) {
    case REC_601:
      return YUVConverter<REC_601>::convert(yy, uv);
    case REC_709:
      return YUVConverter<REC_709>::convert(yy, uv);
    case REC_2020:
      return YUVConverter<REC_2020>::convert(yy, uv);
    default:
      return YUVConverter<IDENTITY>::convert(yy, uv);
  }
}

// Helper functions to sample from planar YUV textures before converting to RGB
template <typename S0>
static inline PackedRGBA8 sampleYUV(S0 sampler0, vec2 uv0, int layer0,
                                    int colorSpace, int rescaleFactor) {
  ivec2 i0(uv0);
  switch (sampler0->format) {
    case TextureFormat::RGBA8: {
      auto planar = textureLinearPlanarRGBA8(sampler0, i0, layer0);
      return convertYUV(colorSpace, highHalf(planar.rg), lowHalf(planar.rg),
                        lowHalf(planar.ba));
    }
    case TextureFormat::YUV422: {
      auto planar = textureLinearPlanarYUV422(sampler0, i0, layer0);
      return convertYUV(colorSpace, planar.y, planar.u, planar.v);
    }
    default:
      assert(false);
      return PackedRGBA8(0);
  }
}

template <typename S0, typename C>
static inline WideRGBA8 sampleColorYUV(S0 sampler0, vec2 uv0, int layer0,
                                       int colorSpace, int rescaleFactor,
                                       C color) {
  return muldiv255(
      unpack(sampleYUV(sampler0, uv0, layer0, colorSpace, rescaleFactor)),
      pack_pixels_RGBA8(color));
}

template <typename S0, typename S1>
static inline PackedRGBA8 sampleYUV(S0 sampler0, vec2 uv0, int layer0,
                                    S1 sampler1, vec2 uv1, int layer1,
                                    int colorSpace, int rescaleFactor) {
  ivec2 i0(uv0);
  ivec2 i1(uv1);
  switch (sampler1->format) {
    case TextureFormat::RG8: {
      assert(sampler0->format == TextureFormat::R8);
      auto y = textureLinearUnpackedR8(sampler0, i0, layer0);
      auto planar = textureLinearPlanarRG8(sampler1, i1, layer1);
      return convertYUV(colorSpace, y, lowHalf(planar.rg), highHalf(planar.rg));
    }
    case TextureFormat::RGBA8: {
      assert(sampler0->format == TextureFormat::R8);
      auto y = textureLinearUnpackedR8(sampler0, i0, layer0);
      auto planar = textureLinearPlanarRGBA8(sampler1, i1, layer1);
      return convertYUV(colorSpace, y, lowHalf(planar.ba), highHalf(planar.rg));
    }
    default:
      assert(false);
      return PackedRGBA8(0);
  }
}

template <typename S0, typename S1, typename C>
static inline WideRGBA8 sampleColorYUV(S0 sampler0, vec2 uv0, int layer0,
                                       S1 sampler1, vec2 uv1, int layer1,
                                       int colorSpace, int rescaleFactor,
                                       C color) {
  return muldiv255(unpack(sampleYUV(sampler0, uv0, layer0, sampler1, uv1,
                                    layer1, colorSpace, rescaleFactor)),
                   pack_pixels_RGBA8(color));
}

template <typename S0, typename S1, typename S2>
static inline PackedRGBA8 sampleYUV(S0 sampler0, vec2 uv0, int layer0,
                                    S1 sampler1, vec2 uv1, int layer1,
                                    S2 sampler2, vec2 uv2, int layer2,
                                    int colorSpace, int rescaleFactor) {
  ivec2 i0(uv0);
  ivec2 i1(uv1);
  ivec2 i2(uv2);
  assert(sampler0->format == sampler1->format &&
         sampler0->format == sampler2->format);
  switch (sampler0->format) {
    case TextureFormat::R8: {
      auto y = textureLinearUnpackedR8(sampler0, i0, layer0);
      auto u = textureLinearUnpackedR8(sampler1, i1, layer1);
      auto v = textureLinearUnpackedR8(sampler2, i2, layer2);
      return convertYUV(colorSpace, y, u, v);
    }
    case TextureFormat::R16: {
      // The rescaling factor represents how many bits to add to renormalize the
      // texture to 16 bits, and so the color depth is actually 16 minus the
      // rescaling factor.
      // Need to right shift the sample by the amount of bits over 8 it
      // occupies. On output from textureLinearUnpackedR16, we have lost 1 bit
      // of precision at the low end already, hence 1 is subtracted from the
      // color depth.
      int colorDepth = 16 - rescaleFactor;
      int rescaleBits = (colorDepth - 1) - 8;
      auto y = textureLinearUnpackedR16(sampler0, i0, layer0) >> rescaleBits;
      auto u = textureLinearUnpackedR16(sampler1, i1, layer1) >> rescaleBits;
      auto v = textureLinearUnpackedR16(sampler2, i2, layer2) >> rescaleBits;
      return convertYUV(colorSpace, U16(y), U16(u), U16(v));
    }
    default:
      assert(false);
      return PackedRGBA8(0);
  }
}

template <typename S0, typename S1, typename S2, typename C>
static inline WideRGBA8 sampleColorYUV(S0 sampler0, vec2 uv0, int layer0,
                                       S1 sampler1, vec2 uv1, int layer1,
                                       S2 sampler2, vec2 uv2, int layer2,
                                       int colorSpace, int rescaleFactor,
                                       C color) {
  return muldiv255(
      unpack(sampleYUV(sampler0, uv0, layer0, sampler1, uv1, layer1, sampler2,
                       uv2, layer2, colorSpace, rescaleFactor)),
      pack_pixels_RGBA8(color));
}

// Commit a single chunk of a YUV surface represented by multiple planar
// textures. This requires a color space specifier selecting how to convert
// from YUV to RGB output. In the case of HDR formats, a rescaling factor
// selects how many bits of precision must be utilized on conversion. See the
// sampleYUV dispatcher functions for the various supported plane
// configurations this intrinsic accepts.
#define swgl_commitTextureLinearYUV(...) \
  swgl_commitChunk(RGBA8, sampleYUV(__VA_ARGS__))
// Commit a single chunk of a YUV surface scaled by a color.
#define swgl_commitTextureLinearColorYUV(...) \
  swgl_commitChunk(RGBA8, sampleColorYUV(__VA_ARGS__))

// Helper functions to apply a color modulus when available.
struct NoColor {};

static ALWAYS_INLINE WideRGBA8 applyColor(WideRGBA8 src, NoColor) {
  return src;
}

static ALWAYS_INLINE WideRGBA8 applyColor(WideRGBA8 src, WideRGBA8 color) {
  return muldiv255(src, color);
}

static ALWAYS_INLINE PackedRGBA8 applyColor(PackedRGBA8 src, NoColor) {
  return src;
}

static ALWAYS_INLINE PackedRGBA8 applyColor(PackedRGBA8 src, WideRGBA8 color) {
  return pack(muldiv255(unpack(src), color));
}

// Samples an axis-aligned span of on a single row of a texture using 1:1
// nearest filtering. Sampling is constrained to only fall within the given UV
// bounds. This requires a pointer to the destination buffer. An optional color
// modulus can be supplied.
template <typename S, typename C>
static void blendTextureNearestRGBA8(S sampler, const ivec2_scalar& i, int span,
                                     const ivec2_scalar& minUV,
                                     const ivec2_scalar& maxUV, C color,
                                     uint32_t* buf, int layerOffset = 0) {
  // Calculate the row pointer within the buffer, clamping to within valid row
  // bounds.
  uint32_t* row =
      &sampler->buf[clamp(clampCoord(i.y, sampler->height), minUV.y, maxUV.y) *
                        sampler->stride +
                    layerOffset];
  // Find clamped X bounds within the row.
  int minX = clamp(minUV.x, 0, sampler->width - 1);
  int maxX = clamp(maxUV.x, minX, sampler->width - 1);
  int curX = i.x;
  // If we need to start sampling below the valid sample bounds, then we need to
  // fill this section with a constant clamped sample.
  if (curX < minX) {
    int n = min(minX - curX, span);
    auto src = applyColor(unpack(bit_cast<PackedRGBA8>(U32(row[minX]))), color);
    commit_solid_span(buf, src, n);
    buf += n;
    span -= n;
    curX += n;
  }
  // Here we only deal with valid samples within the sample bounds. No clamping
  // should occur here within these inner loops.
  int n = clamp(maxX + 1 - curX, 0, span);
  span -= n;
  // Try to process as many chunks as possible with full loads and stores.
  if (blend_key) {
    for (int end = curX + (n & ~3); curX < end; curX += 4, buf += 4) {
      auto src =
          applyColor(unpack(unaligned_load<PackedRGBA8>(&row[curX])), color);
      auto r = blend_pixels(buf, unaligned_load<PackedRGBA8>(buf), src);
      unaligned_store(buf, pack(r));
    }
  } else {
    for (int end = curX + (n & ~3); curX < end; curX += 4, buf += 4) {
      auto src = applyColor(unaligned_load<PackedRGBA8>(&row[curX]), color);
      unaligned_store(buf, src);
    }
  }
  n &= 3;
  // If we have any leftover samples after processing chunks, use partial loads
  // and stores.
  if (n > 0) {
    if (blend_key) {
      auto src = applyColor(
          unpack(partial_load_span<PackedRGBA8>(&row[curX], n)), color);
      auto r =
          blend_pixels(buf, partial_load_span<PackedRGBA8>(buf, n), src, n);
      partial_store_span(buf, pack(r), n);
    } else {
      auto src =
          applyColor(partial_load_span<PackedRGBA8>(&row[curX], n), color);
      partial_store_span(buf, src, n);
    }
    buf += n;
    curX += n;
  }
  // If we still have samples left above the valid sample bounds, then we again
  // need to fill this section with a constant clamped sample.
  if (span > 0) {
    auto src = applyColor(unpack(bit_cast<PackedRGBA8>(U32(row[maxX]))), color);
    commit_solid_span(buf, src, span);
  }
}

// TODO: blendTextureNearestR8 if it is actually needed

// Commit an entire span of 1:1 nearest texture fetches, potentially scaled by a
// color
#define swgl_commitTextureNearest(format, s, p, uv_rect, color, ...)          \
  do {                                                                        \
    ivec2_scalar i = make_ivec2(samplerScale(s, force_scalar(p)));            \
    ivec2_scalar min_uv =                                                     \
        make_ivec2(samplerScale(s, vec2_scalar{uv_rect.x, uv_rect.y}));       \
    ivec2_scalar max_uv =                                                     \
        make_ivec2(samplerScale(s, vec2_scalar{uv_rect.z, uv_rect.w}));       \
    blendTextureNearest##format(s, i, swgl_SpanLength, min_uv, max_uv, color, \
                                swgl_Out##format, __VA_ARGS__);               \
    swgl_Out##format += swgl_SpanLength;                                      \
    swgl_SpanLength = 0;                                                      \
  } while (0)
#define swgl_commitTextureNearestRGBA8(s, p, uv_rect, ...) \
  swgl_commitTextureNearest(RGBA8, s, p, uv_rect, NoColor(), __VA_ARGS__)
#define swgl_commitTextureNearestR8(s, p, uv_rect, ...) \
  swgl_commitTextureNearest(R8, s, p, uv_rect, NoColor(), __VA_ARGS__)

#define swgl_commitTextureNearestColor(format, s, p, uv_rect, color, ...) \
  swgl_commitTextureNearest(format, s, p, uv_rect,                        \
                            pack_pixels_##format(color), __VA_ARGS__)
#define swgl_commitTextureNearestColorRGBA8(s, p, uv_rect, color, ...) \
  swgl_commitTextureNearestColor(RGBA8, s, p, uv_rect, color, __VA_ARGS__)
#define swgl_commitTextureNearestColorR8(s, p, uv_rect, color, ...) \
  swgl_commitTextureNearestColor(R8, s, p, uv_rect, color, __VA_ARGS__)

// Helper function to decide whether we can safely apply 1:1 nearest filtering
// without diverging too much from the linear filter
template <typename S, typename T>
static bool allowTextureNearest(S sampler, T P, int span) {
  // First verify if the row Y doesn't change across samples
  if (P.y.x != P.y.y) {
    return false;
  }
  P = samplerScale(sampler, P);
  // We need to verify that the pixel step reasonably approximates stepping
  // by a single texel for every pixel we need to reproduce. Try to ensure
  // that the margin of error is no more than approximately 2^-7.
  span &= ~(128 - 1);
  span += 128;
  return round((P.x.y - P.x.x) * span) == span &&
         // Also verify that we're reasonably close to the center of a texel
         // so that it doesn't look that much different than if a linear filter
         // was used.
         (int(P.x.x * 4.0f + 0.5f) & 3) == 2 &&
         (int(P.y.x * 4.0f + 0.5f) & 3) == 2;
}

// Determine if we can apply 1:1 nearest filtering to a span of texture
#define swgl_allowTextureNearest(s, p) \
  allowTextureNearest(s, p, swgl_SpanLength)

// Checks if a gradient table of the specified size exists at the UV coords of
// the address within an RGBA32F texture. If so, a linear address within the
// texture is returned that may be used to sample the gradient table later. If
// the address doesn't describe a valid gradient, then a negative value is
// returned.
static inline int swgl_validateGradient(sampler2D sampler, ivec2_scalar address,
                                        int entries) {
  return sampler->format == TextureFormat::RGBA32F && address.y >= 0 &&
                 address.y < int(sampler->height) && address.x >= 0 &&
                 address.x < int(sampler->width) && entries > 0 &&
                 address.x + 2 * entries <= int(sampler->width)
             ? address.y * sampler->stride + address.x * 4
             : -1;
}

// Swizzle RGBA gradient result to BGRA.
static ALWAYS_INLINE HalfRGBA8 swizzleGradient(HalfRGBA8 v) {
  return SHUFFLE(v, v, 2, 1, 0, 3, 6, 5, 4, 7);
}

static inline WideRGBA8 sampleGradient(sampler2D sampler, int address,
                                       Float entry) {
  assert(sampler->format == TextureFormat::RGBA32F);
  assert(address >= 0 && address < int(sampler->height * sampler->stride));
  // Get the integer portion of the entry index to find the entry colors.
  I32 index = cast(entry);
  // Use the fractional portion of the entry index to control blending between
  // entry colors.
  Float offset = entry - cast(index);
  // Every entry is a pair of colors blended by the fractional offset.
  index *= 2;
  assert(test_all(index >= 0 && index < int(sampler->width) - 1));
  Float* buf = (Float*)&sampler->buf[address];
  // Blend between the colors for each SIMD lane, then pack them to RGBA8
  // result. Since the layout of the RGBA8 framebuffer is actually BGRA while
  // the gradient table has RGBA colors, swizzling is required.
  return combine(swizzleGradient(packRGBA8(
                     round_pixel(buf[index.x] + buf[index.x + 1] * offset.x),
                     round_pixel(buf[index.y] + buf[index.y + 1] * offset.y))),
                 swizzleGradient(packRGBA8(
                     round_pixel(buf[index.z] + buf[index.z + 1] * offset.z),
                     round_pixel(buf[index.w] + buf[index.w + 1] * offset.w))));
}

// Samples a gradient entry from the gradient at the provided linearized
// address. The integer portion of the entry index is used to find the entry
// within the table whereas the fractional portion is used to blend between
// adjacent table entries.
#define swgl_commitGradientRGBA8(sampler, address, entry) \
  swgl_commitChunk(RGBA8, sampleGradient(sampler, address, entry))

// Variant that allows specifying a color multiplier of the gradient result.
#define swgl_commitGradientColorRGBA8(sampler, address, entry, color)        \
  swgl_commitChunk(RGBA8, muldiv255(sampleGradient(sampler, address, entry), \
                                    pack_pixels_RGBA8(color)))

// Extension to set a clip mask image to be sampled during blending. The offset
// specifies the positioning of the clip mask image relative to the viewport
// origin. The bounding box specifies the rectangle relative to the clip mask's
// origin that constrains sampling within the clip mask.
static sampler2D swgl_ClipMask = nullptr;
static IntPoint swgl_ClipMaskOffset = {0, 0};
static IntRect swgl_ClipMaskBounds = {0, 0, 0, 0};
#define swgl_clipMask(mask, offset, bb_origin, bb_size)        \
  do {                                                         \
    if (bb_size != vec2_scalar(0.0f, 0.0f)) {                  \
      swgl_ClipMask = mask;                                    \
      swgl_ClipMaskOffset = make_ivec2(offset);                \
      swgl_ClipMaskBounds =                                    \
          IntRect(make_ivec2(bb_origin), make_ivec2(bb_size)); \
    }                                                          \
  } while (0)

// Dispatch helper used by the GLSL translator to swgl_drawSpan functions.
// The number of pixels committed is tracked by checking for the difference in
// swgl_SpanLength. Any varying interpolants used will be advanced past the
// committed part of the span in case the fragment shader must be executed for
// any remaining pixels that were not committed by the span shader.
#define DISPATCH_DRAW_SPAN(self, format)                                      \
  do {                                                                        \
    int total = self->swgl_SpanLength;                                        \
    self->swgl_drawSpan##format();                                            \
    int drawn = total - self->swgl_SpanLength;                                \
    if (drawn) self->step_interp_inputs(drawn);                               \
    while (self->swgl_SpanLength > 0) {                                       \
      run(self);                                                              \
      commit_span(self->swgl_Out##format, pack_span(self->swgl_Out##format)); \
      self->swgl_Out##format += swgl_StepSize;                                \
      self->swgl_SpanLength -= swgl_StepSize;                                 \
    }                                                                         \
  } while (0)
