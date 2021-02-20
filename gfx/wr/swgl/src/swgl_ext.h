/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

template <typename V>
static ALWAYS_INLINE WideRGBA8 pack_span(uint32_t*, const V& v,
                                         float scale = 255.0f) {
  return pack_pixels_RGBA8(v, scale);
}

static ALWAYS_INLINE WideRGBA8 pack_span(uint32_t*) {
  return pack_pixels_RGBA8();
}

template <typename C>
static ALWAYS_INLINE WideR8 pack_span(uint8_t*, C c, float scale = 255.0f) {
  return pack_pixels_R8(c, scale);
}

static ALWAYS_INLINE WideR8 pack_span(uint8_t*) { return pack_pixels_R8(); }

// Helper functions to apply a color modulus when available.
struct NoColor {};

template <typename P>
static ALWAYS_INLINE P applyColor(P src, NoColor) {
  return src;
}

template <typename P>
static ALWAYS_INLINE P applyColor(P src, P color) {
  return muldiv256(src, color);
}

static ALWAYS_INLINE PackedRGBA8 applyColor(PackedRGBA8 src, WideRGBA8 color) {
  return pack(muldiv256(unpack(src), color));
}

static ALWAYS_INLINE PackedR8 applyColor(PackedR8 src, WideR8 color) {
  return pack(muldiv256(unpack(src), color));
}

// Packs a color on a scale of 0..256 rather than 0..255 to allow faster scale
// math with muldiv256. Note that this can cause a slight rounding difference in
// the result versus the 255 scale.
template <typename P, typename C>
static ALWAYS_INLINE auto packColor(P* buf, C color) {
  return pack_span(buf, color, 256.0f);
}

template <typename P>
static ALWAYS_INLINE NoColor packColor(UNUSED P* buf, NoColor noColor) {
  return noColor;
}

static ALWAYS_INLINE void commit_span(uint32_t* buf, WideRGBA8 r) {
  unaligned_store(buf, pack(r));
}

static ALWAYS_INLINE WideRGBA8 blend_span(uint32_t* buf, WideRGBA8 r) {
  return blend_pixels(buf, unaligned_load<PackedRGBA8>(buf), r);
}

static ALWAYS_INLINE WideRGBA8 blend_span(uint32_t* buf, WideRGBA8 r, int len) {
  return blend_pixels(buf, partial_load_span<PackedRGBA8>(buf, len), r, len);
}

static ALWAYS_INLINE void commit_span(uint32_t* buf, PackedRGBA8 r) {
  unaligned_store(buf, r);
}

static ALWAYS_INLINE PackedRGBA8 blend_span(uint32_t* buf, PackedRGBA8 r) {
  return pack(blend_span(buf, unpack(r)));
}

static ALWAYS_INLINE void commit_span(uint8_t* buf, WideR8 r) {
  unaligned_store(buf, pack(r));
}

static ALWAYS_INLINE WideR8 blend_span(uint8_t* buf, WideR8 r) {
  return blend_pixels(buf, unpack(unaligned_load<PackedR8>(buf)), r);
}

static ALWAYS_INLINE WideR8 blend_span(uint8_t* buf, WideR8 r, int len) {
  return blend_pixels(buf, unpack(partial_load_span<PackedR8>(buf, len)), r,
                      len);
}

template <typename P, typename R>
static ALWAYS_INLINE void commit_blend_solid_span(P* buf, R r, int len) {
  for (P* end = &buf[len & ~3]; buf < end; buf += 4) {
    commit_span(buf, blend_span(buf, r));
  }
  len &= 3;
  if (len > 0) {
    partial_store_span(buf, pack(blend_span(buf, r, len)), len);
  }
}

template <bool BLEND>
static void commit_solid_span(uint32_t* buf, WideRGBA8 r, int len) {
  commit_blend_solid_span(buf, r, len);
}

template <>
ALWAYS_INLINE void commit_solid_span<false>(uint32_t* buf, WideRGBA8 r,
                                            int len) {
  fill_n(buf, len, bit_cast<U32>(pack(r)).x);
}

template <bool BLEND>
static void commit_solid_span(uint8_t* buf, WideR8 r, int len) {
  commit_blend_solid_span(buf, r, len);
}

template <>
ALWAYS_INLINE void commit_solid_span<false>(uint8_t* buf, WideR8 r, int len) {
  PackedR8 p = pack(r);
  fill_n((uint32_t*)buf, len / 4, bit_cast<uint32_t>(p));
  buf += len & ~3;
  len &= 3;
  if (len > 0) {
    partial_store_span(buf, p, len);
  }
}

// When using a solid color with clip masking, the cost of loading the clip mask
// in the blend stage exceeds the cost of processing the color. Here we handle
// the entire span of clip mask texture before the blend stage to more
// efficiently process it and modulate it with color without incurring blend
// stage overheads.
template <typename P, typename C>
static void commit_masked_solid_span(P* buf, C color, int len) {
  override_clip_mask();
  uint8_t* mask = get_clip_mask(buf);
  for (P* end = &buf[len]; buf < end; buf += 4, mask += 4) {
    commit_span(
        buf,
        blend_span(buf,
                   applyColor(expand_clip_mask(
                                  buf, unpack(unaligned_load<PackedR8>(mask))),
                              color)));
  }
  restore_clip_mask();
}

// When using a solid color with anti-aliasing, most of the solid span will not
// benefit from anti-aliasing in the opaque region. We only want to apply the AA
// blend stage in the non-opaque start and end of the span where AA is needed.
template <typename P, typename R>
static ALWAYS_INLINE void commit_aa_solid_span(P* buf, R r, int len) {
  if (int start = min((get_aa_opaque_start(buf) + 3) & ~3, len)) {
    commit_solid_span<true>(buf, r, start);
    buf += start;
    len -= start;
  }
  if (int opaque = min((get_aa_opaque_size(buf) + 3) & ~3, len)) {
    override_aa();
    commit_solid_span<true>(buf, r, opaque);
    restore_aa();
    buf += opaque;
    len -= opaque;
  }
  if (len > 0) {
    commit_solid_span<true>(buf, r, len);
  }
}

template <bool BLEND, typename P, typename R>
static ALWAYS_INLINE void commit_blend_span(P* buf, R r) {
  if (BLEND) {
    commit_span(buf, blend_span(buf, r));
  } else {
    commit_span(buf, r);
  }
}

// Forces a value with vector run-class to have scalar run-class.
template <typename T>
static ALWAYS_INLINE auto swgl_forceScalar(T v) -> decltype(force_scalar(v)) {
  return force_scalar(v);
}

// Advance all varying inperpolants by a single chunk
#define swgl_stepInterp() step_interp_inputs()

// Pseudo-intrinsic that accesses the interpolation step for a given varying
#define swgl_interpStep(v) (interp_step.v)

// Commit an entire span of a solid color. This dispatches to clip-masked and
// anti-aliased fast-paths as appropriate.
#define swgl_commitSolid(format, v)                                \
  do {                                                             \
    if (blend_key) {                                               \
      if (swgl_ClipFlags & SWGL_CLIP_FLAG_MASK) {                  \
        commit_masked_solid_span(swgl_Out##format,                 \
                                 packColor(swgl_Out##format, (v)), \
                                 swgl_SpanLength);                 \
      } else if (swgl_ClipFlags & SWGL_CLIP_FLAG_AA) {             \
        commit_aa_solid_span(swgl_Out##format,                     \
                             pack_span(swgl_Out##format, (v)),     \
                             swgl_SpanLength);                     \
      } else {                                                     \
        commit_solid_span<true>(swgl_Out##format,                  \
                                pack_span(swgl_Out##format, (v)),  \
                                swgl_SpanLength);                  \
      }                                                            \
    } else {                                                       \
      commit_solid_span<false>(swgl_Out##format,                   \
                               pack_span(swgl_Out##format, (v)),   \
                               swgl_SpanLength);                   \
    }                                                              \
    swgl_Out##format += swgl_SpanLength;                           \
    swgl_SpanLength = 0;                                           \
  } while (0)
#define swgl_commitSolidRGBA8(v) swgl_commitSolid(RGBA8, v)
#define swgl_commitSolidR8(v) swgl_commitSolid(R8, v)

#define swgl_commitChunk(format, chunk)                 \
  do {                                                  \
    auto r = chunk;                                     \
    if (blend_key) r = blend_span(swgl_Out##format, r); \
    commit_span(swgl_Out##format, r);                   \
    swgl_Out##format += swgl_StepSize;                  \
    swgl_SpanLength -= swgl_StepSize;                   \
  } while (0)

// Commit a single chunk of a color scaled by an alpha weight
#define swgl_commitColor(format, color, alpha)                     \
  swgl_commitChunk(format, applyColor(pack_pixels_##format(color), \
                                      packColor(swgl_Out##format, alpha)))
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
static ALWAYS_INLINE int swgl_textureLayerOffset(UNUSED S s,
                                                 UNUSED float layer) {
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

template <typename S>
static ALWAYS_INLINE WideRGBA8 textureLinearUnpacked(UNUSED uint32_t* buf,
                                                     S sampler, ivec2 i,
                                                     int zoffset) {
  return textureLinearUnpackedRGBA8(sampler, i, zoffset);
}

template <typename S>
static ALWAYS_INLINE WideR8 textureLinearUnpacked(UNUSED uint8_t* buf,
                                                  S sampler, ivec2 i,
                                                  int zoffset) {
  return textureLinearUnpackedR8(sampler, i, zoffset);
}

template <typename S>
static ALWAYS_INLINE bool matchTextureFormat(S s, UNUSED uint32_t* buf) {
  return swgl_isTextureRGBA8(s);
}

template <typename S>
static ALWAYS_INLINE bool matchTextureFormat(S s, UNUSED uint8_t* buf) {
  return swgl_isTextureR8(s);
}

// Quantizes the UVs to the 2^7 scale needed for calculating fractional offsets
// for linear sampling.
#define LINEAR_QUANTIZE_UV(sampler, uv, uv_step, uv_rect, min_uv, max_uv,   \
                           uv_z, zoffset)                                   \
  uv = swgl_linearQuantize(sampler, uv);                                    \
  vec2_scalar uv_step =                                                     \
      float(swgl_StepSize) * vec2_scalar{uv.x.y - uv.x.x, uv.y.y - uv.y.x}; \
  vec2_scalar min_uv =                                                      \
      swgl_linearQuantize(sampler, vec2_scalar{uv_rect.x, uv_rect.y});      \
  vec2_scalar max_uv =                                                      \
      swgl_linearQuantize(sampler, vec2_scalar{uv_rect.z, uv_rect.w});      \
  int zoffset = swgl_textureLayerOffset(sampler, uv_z);

// Implements the fallback linear filter that can deal with clamping and
// arbitrary scales.
template <bool BLEND, typename S, typename C, typename P>
static void blendTextureLinearFallback(S sampler, vec2 uv, int span,
                                       vec2_scalar uv_step, vec2_scalar min_uv,
                                       vec2_scalar max_uv, C color, P* buf,
                                       int zoffset) {
  for (P* end = buf + span; buf < end; buf += swgl_StepSize, uv += uv_step) {
    commit_blend_span<BLEND>(
        buf,
        applyColor(textureLinearUnpacked(
                       buf, sampler, ivec2(clamp(uv, min_uv, max_uv)), zoffset),
                   color));
  }
}

static ALWAYS_INLINE U64 castForShuffle(V16<int16_t> r) {
  return bit_cast<U64>(r);
}
static ALWAYS_INLINE U16 castForShuffle(V4<int16_t> r) {
  return bit_cast<U16>(r);
}

static ALWAYS_INLINE V16<int16_t> applyFracX(V16<int16_t> r, I16 fracx) {
  return r * fracx.xxxxyyyyzzzzwwww;
}
static ALWAYS_INLINE V4<int16_t> applyFracX(V4<int16_t> r, I16 fracx) {
  return r * fracx;
}

// Implements a faster linear filter that works with axis-aligned constant Y but
// scales less than 1, i.e. upscaling. In this case we can optimize for the
// constant Y fraction as well as load all chunks from memory in a single tap
// for each row.
template <bool BLEND, typename S, typename C, typename P>
static void blendTextureLinearUpscale(S sampler, vec2 uv, int span,
                                      vec2_scalar uv_step, vec2_scalar min_uv,
                                      vec2_scalar max_uv, C color, P* buf,
                                      int zoffset) {
  typedef VectorType<uint8_t, 4 * sizeof(P)> packed_type;
  typedef VectorType<uint16_t, 4 * sizeof(P)> unpacked_type;
  typedef VectorType<int16_t, 4 * sizeof(P)> signed_unpacked_type;

  ivec2 i(clamp(uv, min_uv, max_uv));
  ivec2 frac = i;
  i >>= 7;
  P* row0 =
      (P*)sampler->buf + computeRow(sampler, ivec2_scalar(0, i.y.x), zoffset);
  P* row1 = row0 + computeNextRowOffset(sampler, ivec2_scalar(0, i.y.x));
  I16 fracx = computeFracX(sampler, i, frac);
  int16_t fracy = computeFracY(frac).x;
  auto src0 =
      CONVERT(unaligned_load<packed_type>(&row0[i.x.x]), signed_unpacked_type);
  auto src1 =
      CONVERT(unaligned_load<packed_type>(&row1[i.x.x]), signed_unpacked_type);
  auto src = castForShuffle(src0 + (((src1 - src0) * fracy) >> 7));

  // We attempt to sample ahead by one chunk and interpolate it with the current
  // one. However, due to the complication of upscaling, we may not necessarily
  // shift in all the next set of samples.
  for (P* end = buf + span; buf < end; buf += 4) {
    uv.x += uv_step.x;
    I32 ixn = cast(uv.x);
    I16 fracn = computeFracNoClamp(ixn);
    ixn >>= 7;
    auto src0n = CONVERT(unaligned_load<packed_type>(&row0[ixn.x]),
                         signed_unpacked_type);
    auto src1n = CONVERT(unaligned_load<packed_type>(&row1[ixn.x]),
                         signed_unpacked_type);
    auto srcn = castForShuffle(src0n + (((src1n - src0n) * fracy) >> 7));

    // Since we're upscaling, we know that a source pixel has a larger footprint
    // than the destination pixel, and thus all the source pixels needed for
    // this chunk will fall within a single chunk of texture data. However,
    // since the source pixels don't map 1:1 with destination pixels, we need to
    // shift the source pixels over based on their offset from the start of the
    // chunk. This could conceivably be optimized better with usage of PSHUFB or
    // VTBL instructions However, since PSHUFB requires SSSE3, instead we resort
    // to masking in the correct pixels to avoid having to index into memory.
    // For the last sample to interpolate with, we need to potentially shift in
    // a sample from the next chunk over in the case the samples fill out an
    // entire chunk.
    auto shuf = src;
    auto shufn = SHUFFLE(src, ixn.x == i.x.w ? srcn.yyyy : srcn, 1, 2, 3, 4);
    if (i.x.y == i.x.x) {
      shuf = shuf.xxyz;
      shufn = shufn.xxyz;
    }
    if (i.x.z == i.x.y) {
      shuf = shuf.xyyz;
      shufn = shufn.xyyz;
    }
    if (i.x.w == i.x.z) {
      shuf = shuf.xyzz;
      shufn = shufn.xyzz;
    }

    // Convert back to a signed unpacked type so that we can interpolate the
    // final result.
    auto interp = bit_cast<signed_unpacked_type>(shuf);
    auto interpn = bit_cast<signed_unpacked_type>(shufn);
    interp += applyFracX(interpn - interp, fracx) >> 7;

    commit_blend_span<BLEND>(
        buf, applyColor(bit_cast<unpacked_type>(interp), color));

    i.x = ixn;
    fracx = fracn;
    src = srcn;
  }
}

// This is the fastest variant of the linear filter that still provides
// filtering. In cases where there is no scaling required, but we have a
// subpixel offset that forces us to blend in neighboring pixels, we can
// optimize away most of the memory loads and shuffling that is required by the
// fallback filter.
template <bool BLEND, typename S, typename C, typename P>
static void blendTextureLinearFast(S sampler, vec2 uv, int span,
                                   vec2_scalar min_uv, vec2_scalar max_uv,
                                   C color, P* buf, int zoffset) {
  typedef VectorType<uint8_t, 4 * sizeof(P)> packed_type;
  typedef VectorType<uint16_t, 4 * sizeof(P)> unpacked_type;
  typedef VectorType<int16_t, 4 * sizeof(P)> signed_unpacked_type;

  ivec2 i(clamp(uv, min_uv, max_uv));
  ivec2 frac = i;
  i >>= 7;
  P* row0 = (P*)sampler->buf + computeRow(sampler, force_scalar(i), zoffset);
  P* row1 = row0 + computeNextRowOffset(sampler, force_scalar(i));
  int16_t fracx = computeFracX(sampler, i, frac).x;
  int16_t fracy = computeFracY(frac).x;
  auto src0 = CONVERT(unaligned_load<packed_type>(row0), signed_unpacked_type);
  auto src1 = CONVERT(unaligned_load<packed_type>(row1), signed_unpacked_type);
  auto src = castForShuffle(src0 + (((src1 - src0) * fracy) >> 7));

  // Since there is no scaling, we sample ahead by one chunk and interpolate it
  // with the current one. We can then reuse this value on the next iteration.
  for (P* end = buf + span; buf < end; buf += 4) {
    row0 += 4;
    row1 += 4;
    auto src0n =
        CONVERT(unaligned_load<packed_type>(row0), signed_unpacked_type);
    auto src1n =
        CONVERT(unaligned_load<packed_type>(row1), signed_unpacked_type);
    auto srcn = castForShuffle(src0n + (((src1n - src0n) * fracy) >> 7));

    // For the last sample to interpolate with, we need to potentially shift in
    // a sample from the next chunk over since the samples fill out an entire
    // chunk.
    auto interp = bit_cast<signed_unpacked_type>(src);
    auto interpn =
        bit_cast<signed_unpacked_type>(SHUFFLE(src, srcn, 1, 2, 3, 4));
    interp += ((interpn - interp) * fracx) >> 7;

    commit_blend_span<BLEND>(
        buf, applyColor(bit_cast<unpacked_type>(interp), color));

    src = srcn;
  }
}

enum LinearFilter {
  // No linear filter is needed.
  LINEAR_FILTER_NEAREST = 0,
  // The most general linear filter that handles clamping and varying scales.
  LINEAR_FILTER_FALLBACK,
  // A linear filter optimized for axis-aligned upscaling.
  LINEAR_FILTER_UPSCALE,
  // A linear filter with no scaling but with subpixel offset.
  LINEAR_FILTER_FAST
};

// Dispatches to an appropriate linear filter depending on the selected filter.
template <bool BLEND, typename S, typename C, typename P>
static int blendTextureLinear(S sampler, vec2 uv, int span,
                              const vec4_scalar& uv_rect, C color, P* buf,
                              LinearFilter filter, float z = 0) {
  if (!matchTextureFormat(sampler, buf)) {
    return 0;
  }
  LINEAR_QUANTIZE_UV(sampler, uv, uv_step, uv_rect, min_uv, max_uv, z, zoffset);
  P* end = buf + span;
  if (filter != LINEAR_FILTER_FALLBACK) {
    // If we're not using the fallback, then Y is constant across the entire
    // row. We just need to ensure that we handle any samples that might pull
    // data from before the start of the row and require clamping.
    float beforeDist = max(0.0f, min_uv.x) - uv.x.x;
    if (beforeDist > 0) {
      int before = clamp(int(ceil(beforeDist / uv_step.x)) * swgl_StepSize, 0,
                         int(end - buf));
      blendTextureLinearFallback<BLEND>(sampler, uv, before, uv_step, min_uv,
                                        max_uv, color, buf, zoffset);
      buf += before;
      uv.x += (before / swgl_StepSize) * uv_step.x;
    }
    // We need to check how many samples we can take from inside the row without
    // requiring clamping. In case the filter oversamples the row by a step, we
    // subtract off a step from the width to leave some room.
    float insideDist =
        min(max_uv.x, float((int(sampler->width) - swgl_StepSize) << 7)) -
        uv.x.x;
    if (insideDist >= uv_step.x) {
      int inside =
          clamp(int(insideDist / uv_step.x) * swgl_StepSize, 0, int(end - buf));
      if (filter == LINEAR_FILTER_FAST) {
        blendTextureLinearFast<BLEND>(sampler, uv, inside, min_uv, max_uv,
                                      color, buf, zoffset);
      } else {
        blendTextureLinearUpscale<BLEND>(sampler, uv, inside, uv_step, min_uv,
                                         max_uv, color, buf, zoffset);
      }
      buf += inside;
      uv.x += (inside / swgl_StepSize) * uv_step.x;
    }
  }
  // If the fallback filter was requested, or if there are any samples left that
  // may be outside the row and require clamping, then handle that with here.
  if (buf < end) {
    blendTextureLinearFallback<BLEND>(sampler, uv, int(end - buf), uv_step,
                                      min_uv, max_uv, color, buf, zoffset);
  }
  return span;
}

// Samples an axis-aligned span of on a single row of a texture using 1:1
// nearest filtering. Sampling is constrained to only fall within the given UV
// bounds. This requires a pointer to the destination buffer. An optional color
// modulus can be supplied.
template <bool BLEND, typename S, typename C, typename P>
static int blendTextureNearest(S sampler, vec2 uv, int span,
                               const vec4_scalar& uv_rect, C color, P* buf,
                               float layer = 0) {
  if (!matchTextureFormat(sampler, buf)) {
    return 0;
  }

  typedef VectorType<uint8_t, 4 * sizeof(P)> packed_type;

  ivec2_scalar i = make_ivec2(samplerScale(sampler, force_scalar(uv)));
  ivec2_scalar minUV =
      make_ivec2(samplerScale(sampler, vec2_scalar{uv_rect.x, uv_rect.y}));
  ivec2_scalar maxUV =
      make_ivec2(samplerScale(sampler, vec2_scalar{uv_rect.z, uv_rect.w}));
  int layerOffset = swgl_textureLayerOffset(sampler, layer);

  // Calculate the row pointer within the buffer, clamping to within valid row
  // bounds.
  P* row =
      &sampler->buf[clamp(clampCoord(i.y, sampler->height), minUV.y, maxUV.y) *
                        sampler->stride +
                    layerOffset];
  // Find clamped X bounds within the row.
  int minX = clamp(minUV.x, 0, sampler->width - 1);
  int maxX = clamp(maxUV.x, minX, sampler->width - 1);
  int curX = i.x;
  int endX = i.x + span;
  // If we need to start sampling below the valid sample bounds, then we need to
  // fill this section with a constant clamped sample.
  if (curX < minX) {
    int n = min(minX, endX) - curX;
    auto src =
        applyColor(unpack(bit_cast<packed_type>(V4<P>(row[minX]))), color);
    commit_solid_span<BLEND>(buf, src, n);
    buf += n;
    curX += n;
  }
  // Here we only deal with valid samples within the sample bounds. No clamping
  // should occur here within these inner loops.
  int n = max(min(maxX + 1, endX) - curX, 0);
  // Try to process as many chunks as possible with full loads and stores.
  for (int end = curX + (n & ~3); curX < end; curX += 4, buf += 4) {
    auto src =
        applyColor(unpack(unaligned_load<packed_type>(&row[curX])), color);
    commit_blend_span<BLEND>(buf, src);
  }
  n &= 3;
  // If we have any leftover samples after processing chunks, use partial loads
  // and stores.
  if (n > 0) {
    if (BLEND) {
      auto src = applyColor(
          unpack(partial_load_span<packed_type>(&row[curX], n)), color);
      partial_store_span(buf, pack(blend_span(buf, src, n)), n);
    } else {
      auto src =
          applyColor(partial_load_span<packed_type>(&row[curX], n), color);
      partial_store_span(buf, src, n);
    }
    buf += n;
    curX += n;
  }
  // If we still have samples left above the valid sample bounds, then we again
  // need to fill this section with a constant clamped sample.
  if (curX < endX) {
    auto src = applyColor(unpack(bit_cast<packed_type>(U32(row[maxX]))), color);
    commit_solid_span<BLEND>(buf, src, endX - curX);
  }
  return span;
}

// Helper function to decide whether we can safely apply 1:1 nearest filtering
// without diverging too much from the linear filter.
template <typename S, typename T>
static inline LinearFilter needsTextureLinear(S sampler, T P, int span) {
  // First verify if the row Y doesn't change across samples
  if (P.y.x != P.y.y) {
    return LINEAR_FILTER_FALLBACK;
  }
  P = samplerScale(sampler, P);
  // We need to verify that the pixel step reasonably approximates stepping
  // by a single texel for every pixel we need to reproduce. Try to ensure
  // that the margin of error is no more than approximately 2^-7.
  span &= ~(128 - 1);
  span += 128;
  float dx = P.x.y - P.x.x;
  if (round(dx * span) != span) {
    // If the source region is smaller than the destination, then we can use the
    // upscaling filter since row Y is constant.
    return dx >= 0 && dx <= 1 ? LINEAR_FILTER_UPSCALE : LINEAR_FILTER_FALLBACK;
  }
  // Also verify that we're reasonably close to the center of a texel
  // so that it doesn't look that much different than if a linear filter
  // was used.
  if ((int(P.x.x * 4.0f + 0.5f) & 3) != 2 ||
      (int(P.y.x * 4.0f + 0.5f) & 3) != 2) {
    // The source and destination regions are the same, but there is a
    // significant subpixel offset. We can use a faster linear filter to deal
    // with the offset in this case.
    return LINEAR_FILTER_FAST;
  }
  // Otherwise, we have a constant 1:1 step and we're stepping reasonably close
  // to the center of each pixel, so it's safe to disable the linear filter and
  // use nearest.
  return LINEAR_FILTER_NEAREST;
}

// Commit a single chunk from a linear texture fetch
#define swgl_commitTextureLinear(format, s, p, uv_rect, color, ...)        \
  do {                                                                     \
    auto packed_color = packColor(swgl_Out##format, color);                \
    int drawn = 0;                                                         \
    if (LinearFilter filter = needsTextureLinear(s, p, swgl_SpanLength)) { \
      if (blend_key) {                                                     \
        drawn = blendTextureLinear<true>(s, p, swgl_SpanLength, uv_rect,   \
                                         packed_color, swgl_Out##format,   \
                                         filter, __VA_ARGS__);             \
      } else {                                                             \
        drawn = blendTextureLinear<false>(s, p, swgl_SpanLength, uv_rect,  \
                                          packed_color, swgl_Out##format,  \
                                          filter, __VA_ARGS__);            \
      }                                                                    \
    } else if (blend_key) {                                                \
      drawn = blendTextureNearest<true>(s, p, swgl_SpanLength, uv_rect,    \
                                        packed_color, swgl_Out##format,    \
                                        __VA_ARGS__);                      \
    } else {                                                               \
      drawn = blendTextureNearest<false>(s, p, swgl_SpanLength, uv_rect,   \
                                         packed_color, swgl_Out##format,   \
                                         __VA_ARGS__);                     \
    }                                                                      \
    swgl_Out##format += drawn;                                             \
    swgl_SpanLength -= drawn;                                              \
  } while (0)
#define swgl_commitTextureLinearRGBA8(s, p, uv_rect, ...) \
  swgl_commitTextureLinear(RGBA8, s, p, uv_rect, NoColor(), __VA_ARGS__)
#define swgl_commitTextureLinearR8(s, p, uv_rect, ...) \
  swgl_commitTextureLinear(R8, s, p, uv_rect, NoColor(), __VA_ARGS__)

// Commit a single chunk from a linear texture fetch that is scaled by a color
#define swgl_commitTextureLinearColorRGBA8(s, p, uv_rect, color, ...) \
  swgl_commitTextureLinear(RGBA8, s, p, uv_rect, color, __VA_ARGS__)
#define swgl_commitTextureLinearColorR8(s, p, uv_rect, color, ...) \
  swgl_commitTextureLinear(R8, s, p, uv_rect, color, __VA_ARGS__)

// Commit a single chunk from a linear texture fetch
#define swgl_commitTextureLinearChunk(format, s, p, color, ...)      \
  swgl_commitChunk(format, applyColor(textureLinearUnpacked##format( \
                                          s, ivec2(p), __VA_ARGS__), \
                                      packColor(swgl_Out##format, color)))
#define swgl_commitTextureLinearChunkRGBA8(s, p, ...) \
  swgl_commitTextureLinearChunk(RGBA8, s, p, NoColor(), __VA_ARGS__)
#define swgl_commitTextureLinearChunkR8(s, p, ...) \
  swgl_commitTextureLinearChunk(R8, s, p, NoColor(), __VA_ARGS__)

// Commit a single chunk from a linear texture fetch that is scaled by a color
#define swgl_commitTextureLinearChunkColorRGBA8(s, p, color, ...) \
  swgl_commitTextureLinearChunk(RGBA8, s, p, color, __VA_ARGS__)
#define swgl_commitTextureLinearChunkColorR8(s, p, color, ...) \
  swgl_commitTextureLinearChunk(R8, s, p, color, __VA_ARGS__)

// Commit an entire span of a separable pass of a Gaussian blur that falls
// within the given radius scaled by supplied coefficients, clamped to uv_rect
// bounds.
template <bool BLEND, typename S, typename P>
static int blendGaussianBlur(S sampler, vec2 uv, const vec4_scalar& uv_rect,
                             P* buf, int span, bool hori, int radius,
                             vec2_scalar coeffs, float z = 0) {
  if (!matchTextureFormat(sampler, buf)) {
    return 0;
  }
  vec2_scalar size = {float(sampler->width), float(sampler->height)};
  ivec2_scalar curUV = make_ivec2(force_scalar(uv) * size);
  ivec4_scalar bounds = make_ivec4(uv_rect * make_vec4(size, size));
  int zoffset = swgl_textureLayerOffset(sampler, z);
  int startX = curUV.x;
  int endX = min(bounds.z, curUV.x + span);
  if (hori) {
    for (; curUV.x + swgl_StepSize <= endX;
         buf += swgl_StepSize, curUV.x += swgl_StepSize) {
      commit_blend_span<BLEND>(
          buf, gaussianBlurHorizontal<P>(sampler, curUV, bounds.x, bounds.z,
                                         radius, coeffs.x, coeffs.y, zoffset));
    }
  } else {
    for (; curUV.x + swgl_StepSize <= endX;
         buf += swgl_StepSize, curUV.x += swgl_StepSize) {
      commit_blend_span<BLEND>(
          buf, gaussianBlurVertical<P>(sampler, curUV, bounds.y, bounds.w,
                                       radius, coeffs.x, coeffs.y, zoffset));
    }
  }
  return curUV.x - startX;
}

#define swgl_commitGaussianBlur(format, s, p, uv_rect, hori, radius, coeffs,  \
                                ...)                                          \
  do {                                                                        \
    int drawn = 0;                                                            \
    if (blend_key) {                                                          \
      drawn = blendGaussianBlur<true>(s, p, uv_rect, swgl_Out##format,        \
                                      swgl_SpanLength, hori, radius, coeffs,  \
                                      __VA_ARGS__);                           \
    } else {                                                                  \
      drawn = blendGaussianBlur<false>(s, p, uv_rect, swgl_Out##format,       \
                                       swgl_SpanLength, hori, radius, coeffs, \
                                       __VA_ARGS__);                          \
    }                                                                         \
    swgl_Out##format += drawn;                                                \
    swgl_SpanLength -= drawn;                                                 \
  } while (0)
#define swgl_commitGaussianBlurRGBA8(s, p, uv_rect, hori, radius, coeffs, ...) \
  swgl_commitGaussianBlur(RGBA8, s, p, uv_rect, hori, radius, coeffs,          \
                          __VA_ARGS__)
#define swgl_commitGaussianBlurR8(s, p, uv_rect, hori, radius, coeffs, ...) \
  swgl_commitGaussianBlur(R8, s, p, uv_rect, hori, radius, coeffs, __VA_ARGS__)

// Convert and pack planar YUV samples to RGB output using a color space
static ALWAYS_INLINE PackedRGBA8 convertYUV(int colorSpace, U16 y, U16 u,
                                            U16 v) {
  auto yy = V8<int16_t>(zip(y, y));
  auto uv = V8<int16_t>(zip(u, v));
  return yuvMatrix[colorSpace].convert(yy, uv);
}

// Helper functions to sample from planar YUV textures before converting to RGB
template <typename S0>
static ALWAYS_INLINE PackedRGBA8 sampleYUV(S0 sampler0, ivec2 uv0, int layer0,
                                           int colorSpace,
                                           UNUSED int rescaleFactor) {
  switch (sampler0->format) {
    case TextureFormat::RGBA8: {
      auto planar = textureLinearPlanarRGBA8(sampler0, uv0, layer0);
      return convertYUV(colorSpace, highHalf(planar.rg), lowHalf(planar.rg),
                        lowHalf(planar.ba));
    }
    case TextureFormat::YUV422: {
      auto planar = textureLinearPlanarYUV422(sampler0, uv0, layer0);
      return convertYUV(colorSpace, planar.y, planar.u, planar.v);
    }
    default:
      assert(false);
      return PackedRGBA8(0);
  }
}

template <bool BLEND, typename S0, typename P, typename C = NoColor>
static void blendYUV(P* buf, int span, S0 sampler0, vec2 uv0,
                     const vec4_scalar uv_rect0, float z0, int colorSpace,
                     int rescaleFactor, C color = C()) {
  LINEAR_QUANTIZE_UV(sampler0, uv0, uv_step0, uv_rect0, min_uv0, max_uv0, z0,
                     layer0);
  auto c = packColor(buf, color);
  auto* end = buf + span;
  for (; buf < end; buf += swgl_StepSize, uv0 += uv_step0) {
    commit_blend_span<BLEND>(
        buf, applyColor(sampleYUV(sampler0, ivec2(clamp(uv0, min_uv0, max_uv0)),
                                  layer0, colorSpace, rescaleFactor),
                        c));
  }
}

template <typename S0, typename S1>
static ALWAYS_INLINE PackedRGBA8 sampleYUV(S0 sampler0, ivec2 uv0, int layer0,
                                           S1 sampler1, ivec2 uv1, int layer1,
                                           int colorSpace,
                                           UNUSED int rescaleFactor) {
  switch (sampler1->format) {
    case TextureFormat::RG8: {
      assert(sampler0->format == TextureFormat::R8);
      auto y = textureLinearUnpackedR8(sampler0, uv0, layer0);
      auto planar = textureLinearPlanarRG8(sampler1, uv1, layer1);
      return convertYUV(colorSpace, y, lowHalf(planar.rg), highHalf(planar.rg));
    }
    case TextureFormat::RGBA8: {
      assert(sampler0->format == TextureFormat::R8);
      auto y = textureLinearUnpackedR8(sampler0, uv0, layer0);
      auto planar = textureLinearPlanarRGBA8(sampler1, uv1, layer1);
      return convertYUV(colorSpace, y, lowHalf(planar.ba), highHalf(planar.rg));
    }
    default:
      assert(false);
      return PackedRGBA8(0);
  }
}

template <bool BLEND, typename S0, typename S1, typename P,
          typename C = NoColor>
static void blendYUV(P* buf, int span, S0 sampler0, vec2 uv0,
                     const vec4_scalar uv_rect0, float z0, S1 sampler1,
                     vec2 uv1, const vec4_scalar uv_rect1, float z1,
                     int colorSpace, int rescaleFactor, C color = C()) {
  LINEAR_QUANTIZE_UV(sampler0, uv0, uv_step0, uv_rect0, min_uv0, max_uv0, z0,
                     layer0);
  LINEAR_QUANTIZE_UV(sampler1, uv1, uv_step1, uv_rect1, min_uv1, max_uv1, z1,
                     layer1);
  auto c = packColor(buf, color);
  auto* end = buf + span;
  for (; buf < end; buf += swgl_StepSize, uv0 += uv_step0, uv1 += uv_step1) {
    commit_blend_span<BLEND>(
        buf, applyColor(sampleYUV(sampler0, ivec2(clamp(uv0, min_uv0, max_uv0)),
                                  layer0, sampler1,
                                  ivec2(clamp(uv1, min_uv1, max_uv1)), layer1,
                                  colorSpace, rescaleFactor),
                        c));
  }
}

template <typename S0, typename S1, typename S2>
static ALWAYS_INLINE PackedRGBA8 sampleYUV(S0 sampler0, ivec2 uv0, int layer0,
                                           S1 sampler1, ivec2 uv1, int layer1,
                                           S2 sampler2, ivec2 uv2, int layer2,
                                           int colorSpace, int rescaleFactor) {
  assert(sampler0->format == sampler1->format &&
         sampler0->format == sampler2->format);
  switch (sampler0->format) {
    case TextureFormat::R8: {
      auto y = textureLinearUnpackedR8(sampler0, uv0, layer0);
      auto u = textureLinearUnpackedR8(sampler1, uv1, layer1);
      auto v = textureLinearUnpackedR8(sampler2, uv2, layer2);
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
      auto y = textureLinearUnpackedR16(sampler0, uv0, layer0) >> rescaleBits;
      auto u = textureLinearUnpackedR16(sampler1, uv1, layer1) >> rescaleBits;
      auto v = textureLinearUnpackedR16(sampler2, uv2, layer2) >> rescaleBits;
      return convertYUV(colorSpace, U16(y), U16(u), U16(v));
    }
    default:
      assert(false);
      return PackedRGBA8(0);
  }
}

template <bool BLEND, typename S0, typename S1, typename S2, typename P,
          typename C = NoColor>
static void blendYUV(P* buf, int span, S0 sampler0, vec2 uv0,
                     const vec4_scalar uv_rect0, float z0, S1 sampler1,
                     vec2 uv1, const vec4_scalar uv_rect1, float z1,
                     S2 sampler2, vec2 uv2, const vec4_scalar uv_rect2,
                     float z2, int colorSpace, int rescaleFactor,
                     C color = C()) {
  LINEAR_QUANTIZE_UV(sampler0, uv0, uv_step0, uv_rect0, min_uv0, max_uv0, z0,
                     layer0);
  LINEAR_QUANTIZE_UV(sampler1, uv1, uv_step1, uv_rect1, min_uv1, max_uv1, z1,
                     layer1);
  LINEAR_QUANTIZE_UV(sampler2, uv2, uv_step2, uv_rect2, min_uv2, max_uv2, z2,
                     layer2);
  auto c = packColor(buf, color);
  auto* end = buf + span;
  for (; buf < end; buf += swgl_StepSize, uv0 += uv_step0, uv1 += uv_step1,
                    uv2 += uv_step2) {
    commit_blend_span<BLEND>(
        buf, applyColor(sampleYUV(sampler0, ivec2(clamp(uv0, min_uv0, max_uv0)),
                                  layer0, sampler1,
                                  ivec2(clamp(uv1, min_uv1, max_uv1)), layer1,
                                  sampler2, ivec2(clamp(uv2, min_uv2, max_uv2)),
                                  layer2, colorSpace, rescaleFactor),
                        c));
  }
}

// Commit a single chunk of a YUV surface represented by multiple planar
// textures. This requires a color space specifier selecting how to convert
// from YUV to RGB output. In the case of HDR formats, a rescaling factor
// selects how many bits of precision must be utilized on conversion. See the
// sampleYUV dispatcher functions for the various supported plane
// configurations this intrinsic accepts.
#define swgl_commitTextureLinearYUV(...)                            \
  do {                                                              \
    if (blend_key) {                                                \
      blendYUV<true>(swgl_OutRGBA8, swgl_SpanLength, __VA_ARGS__);  \
    } else {                                                        \
      blendYUV<false>(swgl_OutRGBA8, swgl_SpanLength, __VA_ARGS__); \
    }                                                               \
    swgl_OutRGBA8 += swgl_SpanLength;                               \
    swgl_SpanLength = 0;                                            \
  } while (0)

// Commit a single chunk of a YUV surface scaled by a color.
#define swgl_commitTextureLinearColorYUV(...) \
  swgl_commitTextureLinearYUV(__VA_ARGS__)

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
#define swgl_commitGradientColorRGBA8(sampler, address, entry, color)         \
  swgl_commitChunk(RGBA8, applyColor(sampleGradient(sampler, address, entry), \
                                     packColor(swgl_OutRGBA, color)))

// Extension to set a clip mask image to be sampled during blending. The offset
// specifies the positioning of the clip mask image relative to the viewport
// origin. The bounding box specifies the rectangle relative to the clip mask's
// origin that constrains sampling within the clip mask. Blending must be
// enabled for this to work.
static sampler2D swgl_ClipMask = nullptr;
static IntPoint swgl_ClipMaskOffset = {0, 0};
static IntRect swgl_ClipMaskBounds = {0, 0, 0, 0};
#define swgl_clipMask(mask, offset, bb_origin, bb_size)        \
  do {                                                         \
    if (bb_size != vec2_scalar(0.0f, 0.0f)) {                  \
      swgl_ClipFlags |= SWGL_CLIP_FLAG_MASK;                   \
      swgl_ClipMask = mask;                                    \
      swgl_ClipMaskOffset = make_ivec2(offset);                \
      swgl_ClipMaskBounds =                                    \
          IntRect(make_ivec2(bb_origin), make_ivec2(bb_size)); \
    }                                                          \
  } while (0)

// Extension to enable anti-aliasing for the given edges of a quad.
// Blending must be enable for this to work.
static int swgl_AAEdgeMask = 0;

static ALWAYS_INLINE int calcAAEdgeMask(bool on) { return on ? 0xF : 0; }
static ALWAYS_INLINE int calcAAEdgeMask(int mask) { return mask; }
static ALWAYS_INLINE int calcAAEdgeMask(bvec4_scalar mask) {
  return (mask.x ? 1 : 0) | (mask.y ? 2 : 0) | (mask.z ? 4 : 0) |
         (mask.w ? 8 : 0);
}

#define swgl_antiAlias(edges)                \
  do {                                       \
    swgl_AAEdgeMask = calcAAEdgeMask(edges); \
    if (swgl_AAEdgeMask) {                   \
      swgl_ClipFlags |= SWGL_CLIP_FLAG_AA;   \
    }                                        \
  } while (0)

// Dispatch helper used by the GLSL translator to swgl_drawSpan functions.
// The number of pixels committed is tracked by checking for the difference in
// swgl_SpanLength. Any varying interpolants used will be advanced past the
// committed part of the span in case the fragment shader must be executed for
// any remaining pixels that were not committed by the span shader.
#define DISPATCH_DRAW_SPAN(self, format)        \
  do {                                          \
    int total = self->swgl_SpanLength;          \
    self->swgl_drawSpan##format();              \
    int drawn = total - self->swgl_SpanLength;  \
    if (drawn) self->step_interp_inputs(drawn); \
    return drawn;                               \
  } while (0)
