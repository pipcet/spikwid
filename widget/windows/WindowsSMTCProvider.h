/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WIDGET_WINDOWS_WINDOWSSTMCPROVIDER_H_
#define WIDGET_WINDOWS_WINDOWSSTMCPROVIDER_H_

#ifndef __MINGW32__

#  include <functional>
#  include <Windows.Media.h>
#  include <wrl.h>

#  include "mozilla/dom/FetchImageHelper.h"
#  include "mozilla/dom/MediaController.h"
#  include "mozilla/dom/MediaControlKeySource.h"
#  include "mozilla/UniquePtr.h"

using ISMTC = ABI::Windows::Media::ISystemMediaTransportControls;
using SMTCProperty = ABI::Windows::Media::SystemMediaTransportControlsProperty;
using ISMTCDisplayUpdater =
    ABI::Windows::Media::ISystemMediaTransportControlsDisplayUpdater;

using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Storage::Streams::IDataWriter;
using ABI::Windows::Storage::Streams::IRandomAccessStream;
using ABI::Windows::Storage::Streams::IRandomAccessStreamReference;
using Microsoft::WRL::ComPtr;

struct SMTCControlAttributes {
  bool mEnabled;
  bool mPlayPauseEnabled;
  bool mNextEnabled;
  bool mPreviousEnabled;

  static constexpr SMTCControlAttributes EnableAll() {
    return {true, true, true, true};
  }
  static constexpr SMTCControlAttributes DisableAll() {
    return {false, false, false, false};
  }
};

class WindowsSMTCProvider final : public mozilla::dom::MediaControlKeySource {
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WindowsSMTCProvider, override)

 public:
  WindowsSMTCProvider();

  bool IsOpened() const override;
  bool Open() override;
  void Close() override;

  void SetPlaybackState(
      mozilla::dom::MediaSessionPlaybackState aState) override;

  void SetMediaMetadata(
      const mozilla::dom::MediaMetadataBase& aMetadata) override;

  // TODO : modify the virtual control interface based on the supported keys
  void SetSupportedMediaKeys(const MediaKeysArray& aSupportedKeys) override {}

 private:
  ~WindowsSMTCProvider();
  void UnregisterEvents();
  bool RegisterEvents();
  void OnButtonPressed(mozilla::dom::MediaControlKey aKey);

  bool InitDisplayAndControls();

  // Sets the state of the UI Panel (enabled, can use PlayPause, Next, Previous
  // Buttons)
  bool SetControlAttributes(SMTCControlAttributes aAttributes);

  // Sets the Metadata for the currently playing media and sets the playback
  // type to "MUSIC"
  bool SetMusicMetadata(const wchar_t* aArtist, const wchar_t* aTitle,
                        const wchar_t* aAlbumArtist);

  // Sets one of the artwork to the SMTC interface asynchronously
  void LoadThumbnail(const nsTArray<mozilla::dom::MediaImage>& aArtwork);
  // Stores the image at index aIndex of the mArtwork to the Thumbnail
  // asynchronously
  void LoadImageAtIndex(const size_t aIndex);
  // Stores the raw binary data of an image to mImageStream and set it to the
  // Thumbnail asynchronously
  void LoadImage(const char* aImageData, uint32_t aDataSize);
  // Sets the Thumbnail to the image stored in mImageStream
  bool SetThumbnail(const nsAString& aUrl);
  void ClearThumbnail();

  nsresult UpdateThumbnailOnMainThread(const nsAString& aUrl);
  void CancelPendingStoreAsyncOperation() const;

  bool mInitialized = false;
  ComPtr<ISMTC> mControls;
  ComPtr<ISMTCDisplayUpdater> mDisplay;

  // Use mImageDataWriter to write the binary data of image into mImageStream
  // and refer the image by mImageStreamReference and then set it to the SMTC
  // interface
  ComPtr<IDataWriter> mImageDataWriter;
  ComPtr<IRandomAccessStream> mImageStream;
  ComPtr<IRandomAccessStreamReference> mImageStreamReference;
  ComPtr<IAsyncOperation<unsigned int>> mStoreAsyncOperation;

  // mThumbnailUrl is the url of the current Thumbnail
  // mProcessingUrl is the url that is being processed. The process starts from
  // fetching an image from the url and then storing the fetched image to the
  // mImageStream. If mProcessingUrl is not empty, it means there is an image is
  // in processing
  // mThumbnailUrl and mProcessingUrl won't be set at the same time and they can
  // only be touched on main thread
  nsString mThumbnailUrl;
  nsString mProcessingUrl;

  // mArtwork can only be used in main thread in case of data racing
  CopyableTArray<mozilla::dom::MediaImage> mArtwork;
  size_t mNextImageIndex;

  mozilla::UniquePtr<mozilla::dom::FetchImageHelper> mImageFetcher;
  mozilla::MozPromiseRequestHolder<mozilla::dom::ImagePromise>
      mImageFetchRequest;

  HWND mWindow;  // handle to the invisible window

  // EventRegistrationTokens are used to have a handle on a callback (to remove
  // it again)
  EventRegistrationToken mButtonPressedToken;
};

#endif  // __MINGW32__
#endif  // WIDGET_WINDOWS_WINDOWSSTMCPROVIDER_H_
