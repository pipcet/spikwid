/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 sts=2 ts=8 et tw=99 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_LAYERS_PAINTTHREAD_H
#define MOZILLA_LAYERS_PAINTTHREAD_H

#include "base/platform_thread.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/layers/TextureClient.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace gfx {
class DrawTarget;
class DrawTargetCapture;
};

namespace layers {

// Holds the key parts from a RotatedBuffer::PaintState
// required to draw the captured paint state
class CapturedPaintState {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(CapturedPaintState)
public:
  CapturedPaintState(nsIntRegion& aRegionToDraw,
                     gfx::DrawTarget* aTarget,
                     gfx::DrawTarget* aTargetOnWhite,
                     const gfx::Matrix& aTargetTransform,
                     SurfaceMode aSurfaceMode,
                     gfxContentType aContentType)
  : mRegionToDraw(aRegionToDraw)
  , mTarget(aTarget)
  , mTargetOnWhite(aTargetOnWhite)
  , mTargetTransform(aTargetTransform)
  , mSurfaceMode(aSurfaceMode)
  , mContentType(aContentType)
  {}

  nsIntRegion mRegionToDraw;
  RefPtr<TextureClient> mTextureClient;
  RefPtr<TextureClient> mTextureClientOnWhite;
  RefPtr<gfx::DrawTargetCapture> mCapture;
  RefPtr<gfx::DrawTarget> mTarget;
  RefPtr<gfx::DrawTarget> mTargetOnWhite;
  gfx::Matrix mTargetTransform;
  SurfaceMode mSurfaceMode;
  gfxContentType mContentType;

protected:
  virtual ~CapturedPaintState() {}
};

typedef bool (*PrepDrawTargetForPaintingCallback)(CapturedPaintState* aPaintState);

class CompositorBridgeChild;

class PaintThread final
{
  friend void DestroyPaintThread(UniquePtr<PaintThread>&& aPaintThread);

public:
  static void Start();
  static void Shutdown();
  static PaintThread* Get();
  void PaintContents(CapturedPaintState* aState,
                     PrepDrawTargetForPaintingCallback aCallback);

  // To be called on the main thread. Signifies that the current
  // batch of CapturedPaintStates* for PaintContents have been recorded
  // and the main thread is finished recording this layer.
  void FinishedLayerBatch();

  // Must be called on the main thread. Tells the paint thread
  // to schedule a sync textures after all async paints are done.
  // NOTE: The other paint thread functions are on a per PAINT
  // or per paint layer basis. This MUST be called at the end
  // of a layer transaction as multiple paints can occur
  // with multiple layers. We only have to do this once per transaction.
  void SynchronizePaintTextures(SyncObjectClient* aSyncObject);

  // Sync Runnables need threads to be ref counted,
  // But this thread lives through the whole process.
  // We're only temporarily using sync runnables so
  // Override release/addref but don't do anything.
  void Release();
  void AddRef();

  // Helper for asserts.
  static bool IsOnPaintThread();

private:
  bool Init();
  void ShutdownOnPaintThread();
  void InitOnPaintThread();
  void PaintContentsAsync(CompositorBridgeChild* aBridge,
                          CapturedPaintState* aState,
                          PrepDrawTargetForPaintingCallback aCallback);
  void EndAsyncPaintingLayer();
  void SyncTextureData(CompositorBridgeChild* aBridge,
                       SyncObjectClient* aSyncObject);

  static StaticAutoPtr<PaintThread> sSingleton;
  static StaticRefPtr<nsIThread> sThread;
  static PlatformThreadId sThreadId;

  // This shouldn't be very many elements, so a list should be fine.
  // Should only be accessed on the paint thread.
  nsTArray<RefPtr<gfx::DrawTarget>> mDrawTargetsToFlush;
};

} // namespace layers
} // namespace mozilla

#endif
