/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DocumentChannelParent.h"

#include "mozilla/dom/BrowserParent.h"
#include "mozilla/dom/CanonicalBrowsingContext.h"
#include "mozilla/dom/ClientInfo.h"
#include "mozilla/dom/ContentParent.h"

extern mozilla::LazyLogModule gDocumentChannelLog;
#define LOG(fmt) MOZ_LOG(gDocumentChannelLog, mozilla::LogLevel::Verbose, fmt)

using namespace mozilla::dom;

namespace mozilla {
namespace net {

DocumentChannelParent::DocumentChannelParent() {
  LOG(("DocumentChannelParent ctor [this=%p]", this));
}

DocumentChannelParent::~DocumentChannelParent() {
  LOG(("DocumentChannelParent dtor [this=%p]", this));
}

bool DocumentChannelParent::Init(dom::CanonicalBrowsingContext* aContext,
                                 const DocumentChannelCreationArgs& aArgs) {
  RefPtr<nsDocShellLoadState> loadState =
      new nsDocShellLoadState(aArgs.loadState());
  LOG(("DocumentChannelParent Init [this=%p, uri=%s]", this,
       loadState->URI()->GetSpecOrDefault().get()));

  RefPtr<DocumentLoadListener::OpenPromise> promise;
  if (loadState->GetChannelInitialized()) {
    promise = DocumentLoadListener::ClaimParentLoad(
        getter_AddRefs(mDocumentLoadListener), loadState->GetLoadIdentifier());
    if (!promise) {
      return false;
    }
  } else {
    mDocumentLoadListener = new DocumentLoadListener(aContext);

    Maybe<ClientInfo> clientInfo;
    if (aArgs.initialClientInfo().isSome()) {
      clientInfo.emplace(ClientInfo(aArgs.initialClientInfo().ref()));
    }

    nsresult rv = NS_ERROR_UNEXPECTED;
    promise = mDocumentLoadListener->Open(
        loadState, aArgs.cacheKey(), Some(aArgs.channelId()),
        aArgs.asyncOpenTime(), aArgs.timing().refOr(nullptr),
        std::move(clientInfo), aArgs.outerWindowId(),
        aArgs.hasValidTransientUserAction(), Some(aArgs.uriModified()),
        Some(aArgs.isXFOError()), IProtocol::OtherPid(), &rv);
    if (NS_FAILED(rv)) {
      MOZ_ASSERT(!promise);
      return SendFailedAsyncOpen(rv);
    }
  }

  RefPtr<DocumentChannelParent> self = this;
  promise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [self](DocumentLoadListener::OpenPromiseSucceededType&& aResolveValue) {
        // The DLL is waiting for us to resolve the
        // PDocumentChannel::RedirectToRealChannelPromise given as parameter.
        auto promise = self->RedirectToRealChannel(
            std::move(aResolveValue.mStreamFilterEndpoints),
            aResolveValue.mRedirectFlags, aResolveValue.mLoadFlags);
        // We chain the promise the DLL is waiting on to the one returned by
        // RedirectToRealChannel. As soon as the promise returned is resolved
        // or rejected, so will the DLL's promise.
        promise->ChainTo(aResolveValue.mPromise.forget(), __func__);
        self->mDocumentLoadListener = nullptr;
      },
      [self](DocumentLoadListener::OpenPromiseFailedType&& aRejectValue) {
        if (self->CanSend()) {
          Unused << self->SendDisconnectChildListeners(
              aRejectValue.mStatus, aRejectValue.mLoadGroupStatus,
              aRejectValue.mSwitchedProcess);
        }
        self->mDocumentLoadListener = nullptr;
      });

  return true;
}

RefPtr<PDocumentChannelParent::RedirectToRealChannelPromise>
DocumentChannelParent::RedirectToRealChannel(
    nsTArray<ipc::Endpoint<extensions::PStreamFilterParent>>&&
        aStreamFilterEndpoints,
    uint32_t aRedirectFlags, uint32_t aLoadFlags) {
  if (!CanSend()) {
    return PDocumentChannelParent::RedirectToRealChannelPromise::
        CreateAndReject(ResponseRejectReason::ChannelClosed, __func__);
  }
  RedirectToRealChannelArgs args;
  mDocumentLoadListener->SerializeRedirectData(
      args, false, aRedirectFlags, aLoadFlags,
      static_cast<ContentParent*>(Manager()->Manager()));
  return SendRedirectToRealChannel(args, std::move(aStreamFilterEndpoints));
}

}  // namespace net
}  // namespace mozilla

#undef LOG
