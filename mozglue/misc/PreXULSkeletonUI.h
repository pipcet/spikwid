/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef PreXULSkeletonUI_h_
#define PreXULSkeletonUI_h_

#include <windows.h>
#include "mozilla/Types.h"

namespace mozilla {

MFBT_API void CreateAndStorePreXULSkeletonUI(HINSTANCE hInstance);
MFBT_API HWND ConsumePreXULSkeletonUIHandle();
MFBT_API void PersistPreXULSkeletonUIValues(int screenX, int screenY, int width,
                                            int height,
                                            double cssToDevPixelScaling);
MFBT_API bool GetPreXULSkeletonUIEnabled();
MFBT_API void SetPreXULSkeletonUIEnabled(bool value);
MFBT_API void PollPreXULSkeletonUIEvents();

}  // namespace mozilla

#endif
