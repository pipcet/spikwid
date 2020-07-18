/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RenderCompositorNative.h"

#include "GLContext.h"
#include "GLContextProvider.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/layers/SurfacePool.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/webrender/RenderThread.h"
#include "mozilla/widget/CompositorWidget.h"

#ifdef MOZ_GECKO_PROFILER
#  include "ProfilerMarkerPayload.h"
#endif

namespace mozilla {
namespace wr {

RenderCompositorNative::RenderCompositorNative(
    RefPtr<widget::CompositorWidget>&& aWidget, gl::GLContext* aGL)
    : RenderCompositor(std::move(aWidget)),
      mNativeLayerRoot(GetWidget()->GetNativeLayerRoot()) {
#ifdef XP_MACOSX
  auto pool = RenderThread::Get()->SharedSurfacePool();
  if (pool) {
    mSurfacePoolHandle = pool->GetHandleForGL(aGL);
  }
#endif
  MOZ_RELEASE_ASSERT(mSurfacePoolHandle);
}

RenderCompositorNative::~RenderCompositorNative() {
  mNativeLayerRoot->SetLayers({});
  mNativeLayerForEntireWindow = nullptr;
  mNativeLayerRootSnapshotter = nullptr;
  mNativeLayerRoot = nullptr;
}

bool RenderCompositorNative::BeginFrame() {
  if (!MakeCurrent()) {
    gfxCriticalNote << "Failed to make render context current, can't draw.";
    return false;
  }

  gfx::IntSize bufferSize = GetBufferSize().ToUnknownSize();
  if (!ShouldUseNativeCompositor()) {
    if (mNativeLayerForEntireWindow &&
        mNativeLayerForEntireWindow->GetSize() != bufferSize) {
      mNativeLayerRoot->RemoveLayer(mNativeLayerForEntireWindow);
      mNativeLayerForEntireWindow = nullptr;
    }
    if (!mNativeLayerForEntireWindow) {
      mNativeLayerForEntireWindow =
          mNativeLayerRoot->CreateLayer(bufferSize, false, mSurfacePoolHandle);
      mNativeLayerForEntireWindow->SetSurfaceIsFlipped(true);
      mNativeLayerRoot->AppendLayer(mNativeLayerForEntireWindow);
    }
  }

  gfx::IntRect bounds({}, bufferSize);
  if (!InitDefaultFramebuffer(bounds)) {
    return false;
  }

  return true;
}

RenderedFrameId RenderCompositorNative::EndFrame(
    const nsTArray<DeviceIntRect>& aDirtyRects) {
  RenderedFrameId frameId = GetNextRenderFrameId();

  DoSwap();

  if (mNativeLayerForEntireWindow) {
    mNativeLayerForEntireWindow->NotifySurfaceReady();
    mNativeLayerRoot->CommitToScreen();
  }

  return frameId;
}

void RenderCompositorNative::Pause() {}

bool RenderCompositorNative::Resume() { return true; }

LayoutDeviceIntSize RenderCompositorNative::GetBufferSize() {
  return mWidget->GetClientSize();
}

bool RenderCompositorNative::ShouldUseNativeCompositor() {
  return gfx::gfxVars::UseWebRenderCompositor();
}

bool RenderCompositorNative::MaybeReadback(
    const gfx::IntSize& aReadbackSize, const wr::ImageFormat& aReadbackFormat,
    const Range<uint8_t>& aReadbackBuffer) {
  if (!ShouldUseNativeCompositor()) {
    return false;
  }

  MOZ_RELEASE_ASSERT(aReadbackFormat == wr::ImageFormat::BGRA8);
  if (!mNativeLayerRootSnapshotter) {
    mNativeLayerRootSnapshotter = mNativeLayerRoot->CreateSnapshotter();
  }
  bool success = mNativeLayerRootSnapshotter->ReadbackPixels(
      aReadbackSize, gfx::SurfaceFormat::B8G8R8A8, aReadbackBuffer);

  // ReadbackPixels might have changed the current context. Make sure GL is
  // current again.
  MakeCurrent();

  return success;
}

uint32_t RenderCompositorNative::GetMaxUpdateRects() {
  if (ShouldUseNativeCompositor() &&
      StaticPrefs::gfx_webrender_compositor_max_update_rects_AtStartup() > 0) {
    return 1;
  }
  return 0;
}

void RenderCompositorNative::CompositorBeginFrame() {
  mAddedLayers.Clear();
  mAddedPixelCount = 0;
  mAddedClippedPixelCount = 0;
  mBeginFrameTimeStamp = TimeStamp::NowUnfuzzed();
  mSurfacePoolHandle->OnBeginFrame();
}

void RenderCompositorNative::CompositorEndFrame() {
#ifdef MOZ_GECKO_PROFILER
  if (profiler_thread_is_being_profiled()) {
    auto bufferSize = GetBufferSize();
    uint64_t windowPixelCount = uint64_t(bufferSize.width) * bufferSize.height;
    int nativeLayerCount = 0;
    for (const auto& it : mSurfaces) {
      nativeLayerCount += int(it.second.mNativeLayers.size());
    }
    profiler_add_text_marker(
        "WR OS Compositor frame",
        nsPrintfCString("%d%% painting, %d%% overdraw, %d used "
                        "layers (%d%% memory) + %d unused layers (%d%% memory)",
                        int(mDrawnPixelCount * 100 / windowPixelCount),
                        int(mAddedClippedPixelCount * 100 / windowPixelCount),
                        int(mAddedLayers.Length()),
                        int(mAddedPixelCount * 100 / windowPixelCount),
                        int(nativeLayerCount - mAddedLayers.Length()),
                        int((mTotalPixelCount - mAddedPixelCount) * 100 /
                            windowPixelCount)),
        JS::ProfilingCategoryPair::GRAPHICS, mBeginFrameTimeStamp,
        TimeStamp::NowUnfuzzed());
  }
#endif
  mDrawnPixelCount = 0;

  DoFlush();

  mNativeLayerRoot->SetLayers(mAddedLayers);
  mNativeLayerRoot->CommitToScreen();
  mSurfacePoolHandle->OnEndFrame();
}

void RenderCompositorNative::BindNativeLayer(wr::NativeTileId aId,
                                             const gfx::IntRect& aDirtyRect) {
  MOZ_RELEASE_ASSERT(!mCurrentlyBoundNativeLayer);

  auto surfaceCursor = mSurfaces.find(aId.surface_id);
  MOZ_RELEASE_ASSERT(surfaceCursor != mSurfaces.end());
  Surface& surface = surfaceCursor->second;

  auto layerCursor = surface.mNativeLayers.find(TileKey(aId.x, aId.y));
  MOZ_RELEASE_ASSERT(layerCursor != surface.mNativeLayers.end());
  RefPtr<layers::NativeLayer> layer = layerCursor->second;

  mCurrentlyBoundNativeLayer = layer;

  mDrawnPixelCount += aDirtyRect.Area();
}

void RenderCompositorNative::UnbindNativeLayer() {
  MOZ_RELEASE_ASSERT(mCurrentlyBoundNativeLayer);

  mCurrentlyBoundNativeLayer->NotifySurfaceReady();
  mCurrentlyBoundNativeLayer = nullptr;
}

void RenderCompositorNative::CreateSurface(wr::NativeSurfaceId aId,
                                           wr::DeviceIntPoint aVirtualOffset,
                                           wr::DeviceIntSize aTileSize,
                                           bool aIsOpaque) {
  MOZ_RELEASE_ASSERT(mSurfaces.find(aId) == mSurfaces.end());
  mSurfaces.insert({aId, Surface{aTileSize, aIsOpaque}});
}

void RenderCompositorNative::DestroySurface(NativeSurfaceId aId) {
  auto surfaceCursor = mSurfaces.find(aId);
  MOZ_RELEASE_ASSERT(surfaceCursor != mSurfaces.end());

  Surface& surface = surfaceCursor->second;
  for (const auto& iter : surface.mNativeLayers) {
    mTotalPixelCount -= gfx::IntRect({}, iter.second->GetSize()).Area();
  }

  mSurfaces.erase(surfaceCursor);
}

void RenderCompositorNative::CreateTile(wr::NativeSurfaceId aId, int aX,
                                        int aY) {
  auto surfaceCursor = mSurfaces.find(aId);
  MOZ_RELEASE_ASSERT(surfaceCursor != mSurfaces.end());
  Surface& surface = surfaceCursor->second;

  RefPtr<layers::NativeLayer> layer = mNativeLayerRoot->CreateLayer(
      surface.TileSize(), surface.mIsOpaque, mSurfacePoolHandle);
  surface.mNativeLayers.insert({TileKey(aX, aY), layer});
  mTotalPixelCount += gfx::IntRect({}, layer->GetSize()).Area();
}

void RenderCompositorNative::DestroyTile(wr::NativeSurfaceId aId, int aX,
                                         int aY) {
  auto surfaceCursor = mSurfaces.find(aId);
  MOZ_RELEASE_ASSERT(surfaceCursor != mSurfaces.end());
  Surface& surface = surfaceCursor->second;

  auto layerCursor = surface.mNativeLayers.find(TileKey(aX, aY));
  MOZ_RELEASE_ASSERT(layerCursor != surface.mNativeLayers.end());
  RefPtr<layers::NativeLayer> layer = std::move(layerCursor->second);
  surface.mNativeLayers.erase(layerCursor);
  mTotalPixelCount -= gfx::IntRect({}, layer->GetSize()).Area();

  // If the layer is currently present in mNativeLayerRoot, it will be destroyed
  // once CompositorEndFrame() replaces mNativeLayerRoot's layers and drops that
  // reference. So until that happens, the layer still needs to hold on to its
  // front buffer. However, we can tell it to drop its back buffers now, because
  // we know that we will never draw to it again.
  // Dropping the back buffers now puts them back in the surface pool, so those
  // surfaces can be immediately re-used for drawing in other layers in the
  // current frame.
  layer->DiscardBackbuffers();
}

void RenderCompositorNative::AddSurface(wr::NativeSurfaceId aId,
                                        wr::DeviceIntPoint aPosition,
                                        wr::DeviceIntRect aClipRect) {
  MOZ_RELEASE_ASSERT(!mCurrentlyBoundNativeLayer);

  auto surfaceCursor = mSurfaces.find(aId);
  MOZ_RELEASE_ASSERT(surfaceCursor != mSurfaces.end());
  const Surface& surface = surfaceCursor->second;

  for (auto it = surface.mNativeLayers.begin();
       it != surface.mNativeLayers.end(); ++it) {
    RefPtr<layers::NativeLayer> layer = it->second;
    gfx::IntSize layerSize = layer->GetSize();
    gfx::IntPoint layerPosition(
        aPosition.x + surface.mTileSize.width * it->first.mX,
        aPosition.y + surface.mTileSize.height * it->first.mY);
    layer->SetPosition(layerPosition);
    gfx::IntRect clipRect(aClipRect.origin.x, aClipRect.origin.y,
                          aClipRect.size.width, aClipRect.size.height);
    layer->SetClipRect(Some(clipRect));
    mAddedLayers.AppendElement(layer);

    mAddedPixelCount += layerSize.width * layerSize.height;
    gfx::IntRect visibleRect =
        clipRect.Intersect(layer->CurrentSurfaceDisplayRect() + layerPosition);
    mAddedClippedPixelCount += visibleRect.Area();
  }
}

CompositorCapabilities RenderCompositorNative::GetCompositorCapabilities() {
  CompositorCapabilities caps;

  // CoreAnimation doesn't use virtual surfaces
  caps.virtual_surface_size = 0;

  return caps;
}

/* static */
UniquePtr<RenderCompositor> RenderCompositorNativeOGL::Create(
    RefPtr<widget::CompositorWidget>&& aWidget) {
  RefPtr<gl::GLContext> gl = RenderThread::Get()->SharedGL();
  if (!gl) {
    gl = gl::GLContextProvider::CreateForCompositorWidget(
        aWidget, /* aWebRender */ true, /* aForceAccelerated */ true);
    RenderThread::MaybeEnableGLDebugMessage(gl);
  }
  if (!gl || !gl->MakeCurrent()) {
    gfxCriticalNote << "Failed GL context creation for WebRender: "
                    << gfx::hexa(gl.get());
    return nullptr;
  }
  return MakeUnique<RenderCompositorNativeOGL>(std::move(aWidget),
                                               std::move(gl));
}

RenderCompositorNativeOGL::RenderCompositorNativeOGL(
    RefPtr<widget::CompositorWidget>&& aWidget, RefPtr<gl::GLContext>&& aGL)
    : RenderCompositorNative(std::move(aWidget), aGL), mGL(aGL) {
  MOZ_ASSERT(mGL);
}

RenderCompositorNativeOGL::~RenderCompositorNativeOGL() {
  if (!mGL->MakeCurrent()) {
    gfxCriticalNote
        << "Failed to make render context current during destroying.";
    // Leak resources!
    mPreviousFrameDoneSync = nullptr;
    mThisFrameDoneSync = nullptr;
    return;
  }

  if (mPreviousFrameDoneSync) {
    mGL->fDeleteSync(mPreviousFrameDoneSync);
  }
  if (mThisFrameDoneSync) {
    mGL->fDeleteSync(mThisFrameDoneSync);
  }
}

bool RenderCompositorNativeOGL::InitDefaultFramebuffer(
    const gfx::IntRect& aBounds) {
  if (mNativeLayerForEntireWindow) {
    Maybe<GLuint> fbo = mNativeLayerForEntireWindow->NextSurfaceAsFramebuffer(
        aBounds, aBounds, true);
    if (!fbo) {
      return false;
    }
    mGL->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, *fbo);
  } else {
    mGL->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, mGL->GetDefaultFramebuffer());
  }
  return true;
}

void RenderCompositorNativeOGL::DoSwap() {
  InsertFrameDoneSync();
  if (mNativeLayerForEntireWindow) {
    mGL->fFlush();
  }
}

void RenderCompositorNativeOGL::DoFlush() { mGL->fFlush(); }

void RenderCompositorNativeOGL::InsertFrameDoneSync() {
#ifdef XP_MACOSX
  // Only do this on macOS.
  // On other platforms, SwapBuffers automatically applies back-pressure.
  if (mThisFrameDoneSync) {
    mGL->fDeleteSync(mThisFrameDoneSync);
  }
  mThisFrameDoneSync = mGL->fFenceSync(LOCAL_GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
#endif
}

bool RenderCompositorNativeOGL::WaitForGPU() {
  if (mPreviousFrameDoneSync) {
    AUTO_PROFILER_LABEL("Waiting for GPU to finish previous frame", GRAPHICS);
    mGL->fClientWaitSync(mPreviousFrameDoneSync,
                         LOCAL_GL_SYNC_FLUSH_COMMANDS_BIT,
                         LOCAL_GL_TIMEOUT_IGNORED);
    mGL->fDeleteSync(mPreviousFrameDoneSync);
  }
  mPreviousFrameDoneSync = mThisFrameDoneSync;
  mThisFrameDoneSync = nullptr;

  return true;
}

void RenderCompositorNativeOGL::Bind(wr::NativeTileId aId,
                                     wr::DeviceIntPoint* aOffset,
                                     uint32_t* aFboId,
                                     wr::DeviceIntRect aDirtyRect,
                                     wr::DeviceIntRect aValidRect) {
  gfx::IntRect validRect(aValidRect.origin.x, aValidRect.origin.y,
                         aValidRect.size.width, aValidRect.size.height);
  gfx::IntRect dirtyRect(aDirtyRect.origin.x, aDirtyRect.origin.y,
                         aDirtyRect.size.width, aDirtyRect.size.height);

  BindNativeLayer(aId, dirtyRect);

  Maybe<GLuint> fbo = mCurrentlyBoundNativeLayer->NextSurfaceAsFramebuffer(
      validRect, dirtyRect, true);
  MOZ_RELEASE_ASSERT(fbo);  // TODO: make fallible

  *aFboId = *fbo;
  *aOffset = wr::DeviceIntPoint{0, 0};
}

void RenderCompositorNativeOGL::Unbind() {
  mGL->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, 0);

  UnbindNativeLayer();
}

/* static */
UniquePtr<RenderCompositor> RenderCompositorNativeSWGL::Create(
    RefPtr<widget::CompositorWidget>&& aWidget) {
  void* ctx = wr_swgl_create_context();
  if (!ctx) {
    gfxCriticalNote << "Failed SWGL context creation for WebRender";
    return nullptr;
  }
  return MakeUnique<RenderCompositorNativeSWGL>(std::move(aWidget), ctx);
}

RenderCompositorNativeSWGL::RenderCompositorNativeSWGL(
    RefPtr<widget::CompositorWidget>&& aWidget, void* aContext)
    : RenderCompositorNative(std::move(aWidget)), mContext(aContext) {
  MOZ_ASSERT(mContext);
}

RenderCompositorNativeSWGL::~RenderCompositorNativeSWGL() {
  wr_swgl_destroy_context(mContext);
}

bool RenderCompositorNativeSWGL::MakeCurrent() {
  wr_swgl_make_current(mContext);
  return true;
}

bool RenderCompositorNativeSWGL::InitDefaultFramebuffer(
    const gfx::IntRect& aBounds) {
  if (mNativeLayerForEntireWindow) {
    if (!MapNativeLayer(mNativeLayerForEntireWindow, aBounds, aBounds)) {
      return false;
    }
    wr_swgl_init_default_framebuffer(mContext, aBounds.width, aBounds.height,
                                     mLayerStride, mLayerValidRectData);
  }
  return true;
}

void RenderCompositorNativeSWGL::CancelFrame() {
  if (mNativeLayerForEntireWindow && mLayerTarget) {
    UnmapNativeLayer();
  }
}

void RenderCompositorNativeSWGL::DoSwap() {
  if (mNativeLayerForEntireWindow && mLayerTarget) {
    UnmapNativeLayer();
  }
}

bool RenderCompositorNativeSWGL::MapNativeLayer(
    layers::NativeLayer* aLayer, const gfx::IntRect& aDirtyRect,
    const gfx::IntRect& aValidRect) {
  uint8_t* data = nullptr;
  gfx::IntSize size;
  int32_t stride = 0;
  gfx::SurfaceFormat format = gfx::SurfaceFormat::UNKNOWN;
  RefPtr<gfx::DrawTarget> dt = aLayer->NextSurfaceAsDrawTarget(
      aValidRect, gfx::IntRegion(aDirtyRect), gfx::BackendType::SKIA);
  if (!dt || !dt->LockBits(&data, &size, &stride, &format)) {
    return false;
  }
  MOZ_ASSERT(format == gfx::SurfaceFormat::B8G8R8A8 ||
             format == gfx::SurfaceFormat::B8G8R8X8);
  mLayerTarget = std::move(dt);
  mLayerData = data;
  mLayerValidRectData = data + aValidRect.y * stride + aValidRect.x * 4;
  mLayerStride = stride;
  return true;
}

void RenderCompositorNativeSWGL::UnmapNativeLayer() {
  MOZ_ASSERT(mLayerTarget && mLayerData);
  mLayerTarget->ReleaseBits(mLayerData);
  mLayerTarget = nullptr;
  mLayerData = nullptr;
  mLayerValidRectData = nullptr;
  mLayerStride = 0;
}

bool RenderCompositorNativeSWGL::MapTile(wr::NativeTileId aId,
                                         wr::DeviceIntRect aDirtyRect,
                                         wr::DeviceIntRect aValidRect,
                                         void** aData, int32_t* aStride) {
  if (mNativeLayerForEntireWindow) {
    return false;
  }
  gfx::IntRect dirtyRect(aDirtyRect.origin.x, aDirtyRect.origin.y,
                         aDirtyRect.size.width, aDirtyRect.size.height);
  gfx::IntRect validRect(aValidRect.origin.x, aValidRect.origin.y,
                         aValidRect.size.width, aValidRect.size.height);
  BindNativeLayer(aId, dirtyRect);
  if (!MapNativeLayer(mCurrentlyBoundNativeLayer, dirtyRect, validRect)) {
    UnbindNativeLayer();
    return false;
  }
  *aData = mLayerValidRectData;
  *aStride = mLayerStride;
  return true;
}

void RenderCompositorNativeSWGL::UnmapTile() {
  if (!mNativeLayerForEntireWindow && mCurrentlyBoundNativeLayer) {
    UnmapNativeLayer();
    UnbindNativeLayer();
  }
}

}  // namespace wr
}  // namespace mozilla
