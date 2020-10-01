/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

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
