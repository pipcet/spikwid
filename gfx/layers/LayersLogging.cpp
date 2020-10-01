/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "LayersLogging.h"
#include <stdint.h>              // for uint8_t
#include "FrameMetrics.h"        // for FrameMetrics, etc
#include "ImageTypes.h"          // for ImageFormat
#include "mozilla/gfx/Matrix.h"  // for Matrix4x4, Matrix
#include "mozilla/gfx/Point.h"   // for IntSize
#include "nsDebug.h"             // for NS_ERROR
#include "nsPoint.h"             // for nsPoint
#include "nsRect.h"              // for nsRect
#include "nsRectAbsolute.h"      // for nsRectAbsolute
#include "base/basictypes.h"

using namespace mozilla::gfx;

namespace mozilla {
namespace layers {

void AppendToString(std::stringstream& aStream, const wr::ColorF& c,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  aStream << nsPrintfCString("rgba(%d, %d, %d, %f)", uint8_t(c.r * 255.f),
                             uint8_t(c.g * 255.f), uint8_t(c.b * 255.f), c.a)
                 .get();
  aStream << sfx;
}

void AppendToString(std::stringstream& aStream, const wr::LayoutRect& r,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  aStream << nsPrintfCString("(x=%f, y=%f, w=%f, h=%f)", r.origin.x, r.origin.y,
                             r.size.width, r.size.height)
                 .get();
  aStream << sfx;
}

void AppendToString(std::stringstream& aStream, const wr::LayoutSize& s,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  aStream << nsPrintfCString("(w=%f, h=%f)", s.width, s.height).get();
  aStream << sfx;
}

void AppendToString(std::stringstream& aStream, const wr::StickyOffsetBounds& s,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  aStream << nsPrintfCString("(min=%f max=%f)", s.min, s.max).get();
  aStream << sfx;
}

void AppendToString(std::stringstream& aStream, const ScrollMetadata& m,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  AppendToString(aStream, m.GetMetrics(), "{ [metrics=");
  aStream << "] [color=" << m.GetBackgroundColor();
  if (m.GetScrollParentId() != ScrollableLayerGuid::NULL_SCROLL_ID) {
    aStream << "] [scrollParent=" << m.GetScrollParentId();
  }
  if (m.HasScrollClip()) {
    aStream << "] [clip=" << m.ScrollClip().GetClipRect();
  }
  if (m.HasMaskLayer()) {
    aStream << "] [mask=" << m.ScrollClip().GetMaskLayerIndex().value();
  }
  aStream << "] [overscroll=" << m.GetOverscrollBehavior() << "] ["
          << m.GetScrollUpdates().Length() << " scrollupdates"
          << "] }" << sfx;
}

void AppendToString(std::stringstream& aStream, const FrameMetrics& m,
                    const char* pfx, const char* sfx, bool detailed) {
  aStream << pfx;
  aStream << "{ [cb=" << m.GetCompositionBounds()
          << "] [sr=" << m.GetScrollableRect()
          << "] [s=" << m.GetVisualScrollOffset();
  if (m.GetVisualScrollUpdateType() != FrameMetrics::eNone) {
    aStream << "] [vd=" << m.GetVisualDestination();
  }
  aStream << "] [dp=" << m.GetDisplayPort()
          << "] [cdp=" << m.GetCriticalDisplayPort();
  if (!detailed) {
    aStream << "] [scrollId=" << m.GetScrollId();
    if (m.IsRootContent()) {
      aStream << "] [rcd";
    }
    aStream << "] [z=" << m.GetZoom() << "] }";
  } else {
    aStream << "] [rcs=" << m.GetRootCompositionSize()
            << "] [v=" << m.GetLayoutViewport()
            << nsPrintfCString("] [z=(ld=%.3f r=%.3f",
                               m.GetDevPixelsPerCSSPixel().scale,
                               m.GetPresShellResolution())
                   .get()
            << " cr=" << m.GetCumulativeResolution() << " z=" << m.GetZoom()
            << " er=" << m.GetExtraResolution()
            << nsPrintfCString(")] [u=(%d %" PRIu32 ")",
                               m.GetVisualScrollUpdateType(),
                               m.GetScrollGeneration())
                   .get()
            << nsPrintfCString("] [i=(%" PRIu32 " %" PRIu64 " %d)] }",
                               m.GetPresShellId(), m.GetScrollId(),
                               m.IsRootContent())
                   .get();
  }
  aStream << sfx;
}

void AppendToString(std::stringstream& aStream, const ZoomConstraints& z,
                    const char* pfx, const char* sfx) {
  aStream << pfx
          << nsPrintfCString("{ z=%d dt=%d min=%f max=%f }", z.mAllowZoom,
                             z.mAllowDoubleTapZoom, z.mMinZoom.scale,
                             z.mMaxZoom.scale)
                 .get()
          << sfx;
}

void AppendToString(std::stringstream& aStream, const Matrix5x4& m,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  aStream << nsPrintfCString(
                 "[ %g %g %g %g; %g %g %g %g; %g %g %g %g; %g %g %g %g; %g %g "
                 "%g %g]",
                 m._11, m._12, m._13, m._14, m._21, m._22, m._23, m._24, m._31,
                 m._32, m._33, m._34, m._41, m._42, m._43, m._44, m._51, m._52,
                 m._53, m._54)
                 .get();
  aStream << sfx;
}

void AppendToString(std::stringstream& aStream, const SamplingFilter filter,
                    const char* pfx, const char* sfx) {
  aStream << pfx;

  switch (filter) {
    case SamplingFilter::GOOD:
      aStream << "SamplingFilter::GOOD";
      break;
    case SamplingFilter::LINEAR:
      aStream << "SamplingFilter::LINEAR";
      break;
    case SamplingFilter::POINT:
      aStream << "SamplingFilter::POINT";
      break;
    default:
      NS_ERROR("unknown SamplingFilter type");
      aStream << "???";
  }
  aStream << sfx;
}

void AppendToString(std::stringstream& aStream, TextureFlags flags,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  if (flags == TextureFlags::NO_FLAGS) {
    aStream << "NoFlags";
  } else {
#define AppendFlag(test)    \
  {                         \
    if (!!(flags & test)) { \
      if (previous) {       \
        aStream << "|";     \
      }                     \
      aStream << #test;     \
      previous = true;      \
    }                       \
  }
    bool previous = false;
    AppendFlag(TextureFlags::USE_NEAREST_FILTER);
    AppendFlag(TextureFlags::ORIGIN_BOTTOM_LEFT);
    AppendFlag(TextureFlags::DISALLOW_BIGIMAGE);

#undef AppendFlag
  }
  aStream << sfx;
}

void AppendToString(std::stringstream& aStream,
                    mozilla::gfx::SurfaceFormat format, const char* pfx,
                    const char* sfx) {
  aStream << pfx;
  switch (format) {
    case SurfaceFormat::B8G8R8A8:
      aStream << "SurfaceFormat::B8G8R8A8";
      break;
    case SurfaceFormat::B8G8R8X8:
      aStream << "SurfaceFormat::B8G8R8X8";
      break;
    case SurfaceFormat::R8G8B8A8:
      aStream << "SurfaceFormat::R8G8B8A8";
      break;
    case SurfaceFormat::R8G8B8X8:
      aStream << "SurfaceFormat::R8G8B8X8";
      break;
    case SurfaceFormat::R5G6B5_UINT16:
      aStream << "SurfaceFormat::R5G6B5_UINT16";
      break;
    case SurfaceFormat::A8:
      aStream << "SurfaceFormat::A8";
      break;
    case SurfaceFormat::YUV:
      aStream << "SurfaceFormat::YUV";
      break;
    case SurfaceFormat::NV12:
      aStream << "SurfaceFormat::NV12";
      break;
    case SurfaceFormat::P010:
      aStream << "SurfaceFormat::P010";
      break;
    case SurfaceFormat::P016:
      aStream << "SurfaceFormat::P016";
      break;
    case SurfaceFormat::YUV422:
      aStream << "SurfaceFormat::YUV422";
      break;
    case SurfaceFormat::UNKNOWN:
      aStream << "SurfaceFormat::UNKNOWN";
      break;
    default:
      NS_ERROR("unknown surface format");
      aStream << "???";
  }

  aStream << sfx;
}

void AppendToString(std::stringstream& aStream, gfx::SurfaceType aType,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  switch (aType) {
    case SurfaceType::DATA:
      aStream << "SurfaceType::DATA";
      break;
    case SurfaceType::D2D1_BITMAP:
      aStream << "SurfaceType::D2D1_BITMAP";
      break;
    case SurfaceType::D2D1_DRAWTARGET:
      aStream << "SurfaceType::D2D1_DRAWTARGET";
      break;
    case SurfaceType::CAIRO:
      aStream << "SurfaceType::CAIRO";
      break;
    case SurfaceType::CAIRO_IMAGE:
      aStream << "SurfaceType::CAIRO_IMAGE";
      break;
    case SurfaceType::COREGRAPHICS_IMAGE:
      aStream << "SurfaceType::COREGRAPHICS_IMAGE";
      break;
    case SurfaceType::COREGRAPHICS_CGCONTEXT:
      aStream << "SurfaceType::COREGRAPHICS_CGCONTEXT";
      break;
    case SurfaceType::SKIA:
      aStream << "SurfaceType::SKIA";
      break;
    case SurfaceType::DUAL_DT:
      aStream << "SurfaceType::DUAL_DT";
      break;
    case SurfaceType::D2D1_1_IMAGE:
      aStream << "SurfaceType::D2D1_1_IMAGE";
      break;
    case SurfaceType::RECORDING:
      aStream << "SurfaceType::RECORDING";
      break;
    case SurfaceType::WRAP_AND_RECORD:
      aStream << "SurfaceType::WRAP_AND_RECORD";
      break;
    case SurfaceType::TILED:
      aStream << "SurfaceType::TILED";
      break;
    case SurfaceType::DATA_SHARED:
      aStream << "SurfaceType::DATA_SHARED";
      break;
    case SurfaceType::DATA_RECYCLING_SHARED:
      aStream << "SurfaceType::DATA_RECYCLING_SHARED";
      break;
    case SurfaceType::DATA_ALIGNED:
      aStream << "SurfaceType::DATA_ALIGNED";
      break;
    default:
      NS_ERROR("unknown surface type");
      aStream << "???";
  }
  aStream << sfx;
}

void AppendToString(std::stringstream& aStream, ImageFormat format,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  switch (format) {
    case ImageFormat::PLANAR_YCBCR:
      aStream << "ImageFormat::PLANAR_YCBCR";
      break;
    case ImageFormat::SHARED_RGB:
      aStream << "ImageFormat::SHARED_RGB";
      break;
    case ImageFormat::CAIRO_SURFACE:
      aStream << "ImageFormat::CAIRO_SURFACE";
      break;
    case ImageFormat::MAC_IOSURFACE:
      aStream << "ImageFormat::MAC_IOSURFACE";
      break;
    case ImageFormat::SURFACE_TEXTURE:
      aStream << "ImageFormat::SURFACE_TEXTURE";
      break;
    case ImageFormat::D3D9_RGB32_TEXTURE:
      aStream << "ImageFormat::D3D9_RBG32_TEXTURE";
      break;
    case ImageFormat::OVERLAY_IMAGE:
      aStream << "ImageFormat::OVERLAY_IMAGE";
      break;
    case ImageFormat::D3D11_SHARE_HANDLE_TEXTURE:
      aStream << "ImageFormat::D3D11_SHARE_HANDLE_TEXTURE";
      break;
    default:
      NS_ERROR("unknown image format");
      aStream << "???";
  }

  aStream << sfx;
}

void AppendToString(std::stringstream& aStream,
                    const mozilla::ScrollPositionUpdate& aUpdate,
                    const char* pfx, const char* sfx) {
  aStream << pfx;
  aUpdate.AppendToString(aStream);
  aStream << sfx;
}

}  // namespace layers
}  // namespace mozilla

void print_stderr(std::stringstream& aStr) {
#if defined(ANDROID)
  // On Android logcat output is truncated to 1024 chars per line, and
  // we usually use std::stringstream to build up giant multi-line gobs
  // of output. So to avoid the truncation we find the newlines and
  // print the lines individually.
  std::string line;
  while (std::getline(aStr, line)) {
    printf_stderr("%s\n", line.c_str());
  }
#else
  printf_stderr("%s", aStr.str().c_str());
#endif
}

void fprint_stderr(FILE* aFile, std::stringstream& aStr) {
  if (aFile == stderr) {
    print_stderr(aStr);
  } else {
    fprintf_stderr(aFile, "%s", aStr.str().c_str());
  }
}
