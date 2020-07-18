/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PreloaderBase.h"

#include "mozilla/dom/Document.h"
#include "nsContentUtils.h"
#include "nsIAsyncVerifyRedirectCallback.h"
#include "nsIChannel.h"
#include "nsILoadGroup.h"
#include "nsIInterfaceRequestorUtils.h"

// Change this if we want to cancel and remove the associated preload on removal
// of all <link rel=preload> tags from the tree.
constexpr static bool kCancelAndRemovePreloadOnZeroReferences = false;

namespace mozilla {

PreloaderBase::UsageTimer::UsageTimer(PreloaderBase* aPreload,
                                      dom::Document* aDocument)
    : mDocument(aDocument), mPreload(aPreload) {}

PreloaderBase::RedirectSink::RedirectSink(PreloaderBase* aPreloader,
                                          nsIInterfaceRequestor* aCallbacks)
    : mPreloader(new nsMainThreadPtrHolder<PreloaderBase>(
          "RedirectSink.mPreloader", aPreloader)),
      mCallbacks(aCallbacks) {}

NS_IMPL_ISUPPORTS(PreloaderBase::RedirectSink, nsIInterfaceRequestor,
                  nsIChannelEventSink, nsIRedirectResultListener)

NS_IMETHODIMP PreloaderBase::RedirectSink::AsyncOnChannelRedirect(
    nsIChannel* aOldChannel, nsIChannel* aNewChannel, uint32_t aFlags,
    nsIAsyncVerifyRedirectCallback* aCallback) {
  mRedirectChannel = aNewChannel;

  // Deliberately adding this before confirmation.
  nsCOMPtr<nsIURI> uri;
  aNewChannel->GetOriginalURI(getter_AddRefs(uri));
  mPreloader->mRedirectRecords.AppendElement(
      RedirectRecord(aFlags, uri.forget()));

  if (mCallbacks) {
    nsCOMPtr<nsIChannelEventSink> sink(do_GetInterface(mCallbacks));
    if (sink) {
      return sink->AsyncOnChannelRedirect(aOldChannel, aNewChannel, aFlags,
                                          aCallback);
    }
  }

  aCallback->OnRedirectVerifyCallback(NS_OK);
  return NS_OK;
}

NS_IMETHODIMP PreloaderBase::RedirectSink::OnRedirectResult(bool proceeding) {
  if (proceeding && mRedirectChannel) {
    mPreloader->mChannel = std::move(mRedirectChannel);
  } else {
    mRedirectChannel = nullptr;
  }

  if (mCallbacks) {
    nsCOMPtr<nsIRedirectResultListener> sink(do_GetInterface(mCallbacks));
    if (sink) {
      return sink->OnRedirectResult(proceeding);
    }
  }

  return NS_OK;
}

NS_IMETHODIMP PreloaderBase::RedirectSink::GetInterface(const nsIID& aIID,
                                                        void** aResult) {
  NS_ENSURE_ARG_POINTER(aResult);

  if (aIID.Equals(NS_GET_IID(nsIChannelEventSink)) ||
      aIID.Equals(NS_GET_IID(nsIRedirectResultListener))) {
    return QueryInterface(aIID, aResult);
  }

  if (mCallbacks) {
    return mCallbacks->GetInterface(aIID, aResult);
  }

  *aResult = nullptr;
  return NS_ERROR_NO_INTERFACE;
}

PreloaderBase::~PreloaderBase() { MOZ_ASSERT(NS_IsMainThread()); }

// static
void PreloaderBase::AddLoadBackgroundFlag(nsIChannel* aChannel) {
  nsLoadFlags loadFlags;
  aChannel->GetLoadFlags(&loadFlags);
  aChannel->SetLoadFlags(loadFlags | nsIRequest::LOAD_BACKGROUND);
}

void PreloaderBase::NotifyOpen(const PreloadHashKey& aKey,
                               dom::Document* aDocument, bool aIsPreload) {
  if (aDocument && !aDocument->Preloads().RegisterPreload(aKey, this)) {
    // This means there is already a preload registered under this key in this
    // document.  We only allow replacement when this is a regular load.
    // Otherwise, this should never happen and is a suspected misuse of the API.
    MOZ_ASSERT(!aIsPreload);
    aDocument->Preloads().DeregisterPreload(aKey);
    aDocument->Preloads().RegisterPreload(aKey, this);
  }

  mKey = aKey;
  mIsUsed = !aIsPreload;
}

void PreloaderBase::NotifyOpen(const PreloadHashKey& aKey, nsIChannel* aChannel,
                               dom::Document* aDocument, bool aIsPreload) {
  NotifyOpen(aKey, aDocument, aIsPreload);
  mChannel = aChannel;

  auto callback = MakeRefPtr<UsageTimer>(this, aDocument);
  NS_NewTimerWithCallback(getter_AddRefs(mUsageTimer), callback, 10000,
                          nsITimer::TYPE_ONE_SHOT);

  nsCOMPtr<nsIInterfaceRequestor> callbacks;
  mChannel->GetNotificationCallbacks(getter_AddRefs(callbacks));
  RefPtr<RedirectSink> sink(new RedirectSink(this, callbacks));
  mChannel->SetNotificationCallbacks(sink);
}

void PreloaderBase::NotifyUsage(LoadBackground aLoadBackground) {
  if (!mIsUsed && mChannel && aLoadBackground == LoadBackground::Drop) {
    nsLoadFlags loadFlags;
    mChannel->GetLoadFlags(&loadFlags);

    // Preloads are initially set the LOAD_BACKGROUND flag.  When becoming
    // regular loads by hitting its consuming tag, we need to drop that flag,
    // which also means to re-add the request from/to it's loadgroup to reflect
    // that flag change.
    if (loadFlags & nsIRequest::LOAD_BACKGROUND) {
      nsCOMPtr<nsILoadGroup> loadGroup;
      mChannel->GetLoadGroup(getter_AddRefs(loadGroup));

      if (loadGroup) {
        nsresult status;
        mChannel->GetStatus(&status);

        nsresult rv = loadGroup->RemoveRequest(mChannel, nullptr, status);
        mChannel->SetLoadFlags(loadFlags & ~nsIRequest::LOAD_BACKGROUND);
        if (NS_SUCCEEDED(rv)) {
          loadGroup->AddRequest(mChannel, nullptr);
        }
      }
    }
  }

  mIsUsed = true;
  CancelUsageTimer();
}

void PreloaderBase::RemoveSelf(dom::Document* aDocument) {
  if (aDocument) {
    aDocument->Preloads().DeregisterPreload(mKey);
  }
}

void PreloaderBase::NotifyRestart(dom::Document* aDocument,
                                  PreloaderBase* aNewPreloader) {
  RemoveSelf(aDocument);
  mKey = PreloadHashKey();

  CancelUsageTimer();

  if (aNewPreloader) {
    aNewPreloader->mNodes = std::move(mNodes);
  }
}

void PreloaderBase::NotifyStart(nsIRequest* aRequest) {
  // If there is no channel assigned on this preloader, we are not between
  // channel switching, so we can freely update the mShouldFireLoadEvent using
  // the given channel.
  if (mChannel && !SameCOMIdentity(aRequest, mChannel)) {
    return;
  }

  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(aRequest);
  if (!httpChannel) {
    return;
  }

  // if the load is cross origin without CORS, or the CORS access is rejected,
  // always fire load event to avoid leaking site information.
  nsresult rv;
  nsCOMPtr<nsILoadInfo> loadInfo = httpChannel->LoadInfo();
  mShouldFireLoadEvent =
      loadInfo->GetTainting() == LoadTainting::Opaque ||
      (loadInfo->GetTainting() == LoadTainting::CORS &&
       (NS_FAILED(httpChannel->GetStatus(&rv)) || NS_FAILED(rv)));
}

void PreloaderBase::NotifyStop(nsIRequest* aRequest, nsresult aStatus) {
  // Filter out notifications that may be arriving from the old channel before
  // restarting this request.
  if (!SameCOMIdentity(aRequest, mChannel)) {
    return;
  }

  NotifyStop(aStatus);
}

void PreloaderBase::NotifyStop(nsresult aStatus) {
  mOnStopStatus.emplace(aStatus);

  nsTArray<nsWeakPtr> nodes;
  nodes.SwapElements(mNodes);

  for (nsWeakPtr& weak : nodes) {
    nsCOMPtr<nsINode> node = do_QueryReferent(weak);
    if (node) {
      NotifyNodeEvent(node);
    }
  }

  mChannel = nullptr;
}

void PreloaderBase::NotifyValidating() { mOnStopStatus.reset(); }

void PreloaderBase::NotifyValidated(nsresult aStatus) {
  NotifyStop(nullptr, aStatus);
}

void PreloaderBase::AddLinkPreloadNode(nsINode* aNode) {
  if (mOnStopStatus) {
    return NotifyNodeEvent(aNode);
  }

  mNodes.AppendElement(do_GetWeakReference(aNode));
}

void PreloaderBase::RemoveLinkPreloadNode(nsINode* aNode) {
  // Note that do_GetWeakReference returns the internal weak proxy, which is
  // always the same, so we can use it to search the array using default
  // comparator.
  nsWeakPtr node = do_GetWeakReference(aNode);
  mNodes.RemoveElement(node);

  if (kCancelAndRemovePreloadOnZeroReferences && mNodes.Length() == 0 &&
      !mIsUsed) {
    // Keep a reference, because the following call may release us.  The caller
    // may use a WeakPtr to access this.
    RefPtr<PreloaderBase> self(this);
    RemoveSelf(aNode->OwnerDoc());

    if (mChannel) {
      mChannel->Cancel(NS_BINDING_ABORTED);
    }
  }
}

void PreloaderBase::NotifyNodeEvent(nsINode* aNode) {
  PreloadService::NotifyNodeEvent(
      aNode, mShouldFireLoadEvent || NS_SUCCEEDED(*mOnStopStatus));
}

void PreloaderBase::CancelUsageTimer() {
  if (mUsageTimer) {
    mUsageTimer->Cancel();
    mUsageTimer = nullptr;
  }
}

nsresult PreloaderBase::AsyncConsume(nsIStreamListener* aListener) {
  // We want to return an error so that consumers can't ever use a preload to
  // consume data unless it's properly implemented.
  return NS_ERROR_NOT_IMPLEMENTED;
}

// PreloaderBase::RedirectRecord

nsCString PreloaderBase::RedirectRecord::Spec() const {
  nsCOMPtr<nsIURI> noFragment;
  NS_GetURIWithoutRef(mURI, getter_AddRefs(noFragment));
  MOZ_ASSERT(noFragment);
  return noFragment->GetSpecOrDefault();
}

nsCString PreloaderBase::RedirectRecord::Fragment() const {
  nsCString fragment;
  mURI->GetRef(fragment);
  return fragment;
}

// PreloaderBase::UsageTimer

NS_IMPL_ISUPPORTS(PreloaderBase::UsageTimer, nsITimerCallback)

NS_IMETHODIMP PreloaderBase::UsageTimer::Notify(nsITimer* aTimer) {
  if (!mPreload || !mDocument) {
    return NS_OK;
  }

  MOZ_ASSERT(aTimer == mPreload->mUsageTimer);
  mPreload->mUsageTimer = nullptr;

  if (mPreload->IsUsed()) {
    // Left in the hashtable, but marked as used.  This is a valid case, and we
    // don't want to emit a warning for this preload then.
    return NS_OK;
  }

  // PreloadHashKey overrides GetKey, we need to use the nsURIHashKey one to get
  // the URI.
  nsIURI* uri = static_cast<nsURIHashKey*>(&mPreload->mKey)->GetKey();
  if (!uri) {
    return NS_OK;
  }
  nsString spec = NS_ConvertUTF8toUTF16(uri->GetSpecOrDefault());

  nsContentUtils::ReportToConsole(nsIScriptError::warningFlag, "DOM"_ns,
                                  mDocument, nsContentUtils::eDOM_PROPERTIES,
                                  "UnusedLinkPreloadPending",
                                  nsTArray<nsString>({spec}));

  return NS_OK;
}

}  // namespace mozilla
