/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ODoHService.h"

#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_network.h"
#include "nsIDNSService.h"
#include "nsIDNSByTypeRecord.h"
#include "nsIOService.h"
#include "ODoH.h"
#include "TRRService.h"
#include "nsURLHelper.h"

static const char kODoHProxyURIPref[] = "network.trr.odoh.proxy_uri";
static const char kODoHTargetHostPref[] = "network.trr.odoh.target_host";
static const char kODoHTargetPathPref[] = "network.trr.odoh.target_path";

namespace mozilla {
namespace net {

ODoHService* gODoHService = nullptr;

extern mozilla::LazyLogModule gHostResolverLog;
#define LOG(args) MOZ_LOG(gHostResolverLog, mozilla::LogLevel::Debug, args)

NS_IMPL_ISUPPORTS(ODoHService, nsIDNSListener, nsIObserver,
                  nsISupportsWeakReference)

ODoHService::ODoHService()
    : mLock("net::ODoHService"), mQueryODoHConfigInProgress(false) {
  gODoHService = this;
}

ODoHService::~ODoHService() { gODoHService = nullptr; }

bool ODoHService::Init() {
  MOZ_ASSERT(NS_IsMainThread(), "wrong thread");

  nsCOMPtr<nsIPrefBranch> prefBranch(do_GetService(NS_PREFSERVICE_CONTRACTID));
  if (!prefBranch) {
    return false;
  }

  prefBranch->AddObserver(kODoHProxyURIPref, this, true);
  prefBranch->AddObserver(kODoHTargetHostPref, this, true);
  prefBranch->AddObserver(kODoHTargetPathPref, this, true);

  ReadPrefs(nullptr);

  return true;
}

bool ODoHService::Enabled() const {
  return StaticPrefs::network_trr_odoh_enabled();
}

NS_IMETHODIMP
ODoHService::Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* aData) {
  MOZ_ASSERT(NS_IsMainThread(), "wrong thread");
  if (!strcmp(aTopic, NS_PREFBRANCH_PREFCHANGE_TOPIC_ID)) {
    ReadPrefs(NS_ConvertUTF16toUTF8(aData).get());
  }

  return NS_OK;
}

nsresult ODoHService::ReadPrefs(const char* aName) {
  if (!aName || !strcmp(aName, kODoHProxyURIPref) ||
      !strcmp(aName, kODoHTargetHostPref) ||
      !strcmp(aName, kODoHTargetPathPref)) {
    OnODoHPrefsChange();
  }

  return NS_OK;
}

void ODoHService::OnODoHPrefsChange() {
  nsAutoCString proxyURI;
  Preferences::GetCString(kODoHProxyURIPref, proxyURI);
  nsAutoCString targetHost;
  Preferences::GetCString(kODoHTargetHostPref, targetHost);
  nsAutoCString targetPath;
  Preferences::GetCString(kODoHTargetPathPref, targetPath);

  bool updateODoHConfig = false;
  {
    MutexAutoLock lock(mLock);
    mODoHProxyURI = proxyURI;
    // Only update ODoHConfig when the host is really changed.
    if (!mODoHTargetHost.Equals(targetHost)) {
      updateODoHConfig = true;
    }
    mODoHTargetHost = targetHost;
    mODoHTargetPath = targetPath;

    BuildODoHRequestURI();
  }

  if (updateODoHConfig) {
    UpdateODoHConfig();
  }
}

void ODoHService::BuildODoHRequestURI() {
  mLock.AssertCurrentThreadOwns();

  mODoHRequestURI.Truncate();
  if (mODoHTargetHost.IsEmpty() || mODoHTargetPath.IsEmpty()) {
    return;
  }

  if (mODoHProxyURI.IsEmpty()) {
    mODoHRequestURI.Append(mODoHTargetHost);
    mODoHRequestURI.AppendLiteral("/");
    mODoHRequestURI.Append(mODoHTargetPath);
  } else {
    mODoHRequestURI.Append(mODoHProxyURI);
    mODoHRequestURI.AppendLiteral("?targethost=");
    mODoHRequestURI.Append(mODoHTargetHost);
    mODoHRequestURI.AppendLiteral("&targetpath=/");
    mODoHRequestURI.Append(mODoHTargetPath);
  }
}

void ODoHService::GetRequestURI(nsACString& aResult) {
  MutexAutoLock lock(mLock);
  aResult = mODoHRequestURI;
}

nsresult ODoHService::UpdateODoHConfig() {
  LOG(("ODoHService::UpdateODoHConfig"));
  if (mQueryODoHConfigInProgress) {
    return NS_OK;
  }

  nsAutoCString uri;
  {
    MutexAutoLock lock(mLock);
    uri = mODoHProxyURI.IsEmpty() ? mODoHTargetHost : mODoHProxyURI;
  }

  nsCOMPtr<nsIDNSService> dns(
      do_GetService("@mozilla.org/network/dns-service;1"));
  if (!dns) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  if (!gTRRService) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsCOMPtr<nsIURI> queryURI;
  nsresult rv = NS_NewURI(getter_AddRefs(queryURI), uri);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsAutoCString scheme;
  queryURI->GetScheme(scheme);
  if (!scheme.Equals("https")) {
    LOG(("ODoHService::UpdateODoHConfig uri is not https"));
    return NS_ERROR_FAILURE;
  }

  nsAutoCString hostStr;
  rv = queryURI->GetAsciiHost(hostStr);
  if (NS_FAILED(rv)) {
    return rv;
  }

  nsCOMPtr<nsICancelable> tmpOutstanding;
  nsCOMPtr<nsIEventTarget> target = gTRRService->MainThreadOrTRRThread();
  uint32_t flags = nsIDNSService::RESOLVE_DISABLE_ODOH;
  rv = dns->AsyncResolveNative(hostStr, nsIDNSService::RESOLVE_TYPE_HTTPSSVC,
                               flags, nullptr, this, target, OriginAttributes(),
                               getter_AddRefs(tmpOutstanding));
  LOG(("ODoHService::UpdateODoHConfig [host=%s rv=%" PRIx32 "]", hostStr.get(),
       static_cast<uint32_t>(rv)));

  if (NS_SUCCEEDED(rv)) {
    mQueryODoHConfigInProgress = true;
  }
  return rv;
}

NS_IMETHODIMP
ODoHService::OnLookupComplete(nsICancelable* aRequest, nsIDNSRecord* aRec,
                              nsresult aStatus) {
  MOZ_ASSERT_IF(XRE_IsParentProcess() && gTRRService,
                NS_IsMainThread() || gTRRService->IsOnTRRThread());
  MOZ_ASSERT_IF(XRE_IsSocketProcess(), NS_IsMainThread());

  mQueryODoHConfigInProgress = false;

  LOG(("ODoHService::OnLookupComplete [aStatus=%" PRIx32 "]",
       static_cast<uint32_t>(aStatus)));
  if (NS_FAILED(aStatus)) {
    return NS_OK;
  }

  nsCOMPtr<nsIDNSHTTPSSVCRecord> httpsRecord = do_QueryInterface(aRec);
  if (!httpsRecord) {
    return NS_OK;
  }

  nsCString rawODoHConfig;
  nsTArray<RefPtr<nsISVCBRecord>> svcbRecords;
  httpsRecord->GetRecords(svcbRecords);
  for (const auto& record : svcbRecords) {
    Unused << record->GetODoHConfig(rawODoHConfig);
    if (!rawODoHConfig.IsEmpty()) {
      break;
    }
  }

  nsTArray<ObliviousDoHConfig> configs;
  if (!ODoHDNSPacket::ParseODoHConfigs(rawODoHConfig, configs)) {
    return NS_OK;
  }

  mODoHConfigs.emplace(std::move(configs));

  if (!mPendingRequests.IsEmpty()) {
    nsTArray<RefPtr<ODoH>> requests = std::move(mPendingRequests);
    nsCOMPtr<nsIEventTarget> target = gTRRService->MainThreadOrTRRThread();
    for (auto& query : requests) {
      target->Dispatch(query.forget());
    }
  }
  return NS_OK;
}

const Maybe<nsTArray<ObliviousDoHConfig>>& ODoHService::ODoHConfigs() {
  MOZ_ASSERT_IF(XRE_IsParentProcess() && gTRRService,
                NS_IsMainThread() || gTRRService->IsOnTRRThread());
  MOZ_ASSERT_IF(XRE_IsSocketProcess(), NS_IsMainThread());

  return mODoHConfigs;
}

void ODoHService::AppendPendingODoHRequest(ODoH* aRequest) {
  LOG(("ODoHService::AppendPendingODoHQuery\n"));
  MOZ_ASSERT_IF(XRE_IsParentProcess() && gTRRService,
                NS_IsMainThread() || gTRRService->IsOnTRRThread());
  MOZ_ASSERT_IF(XRE_IsSocketProcess(), NS_IsMainThread());

  mPendingRequests.AppendElement(aRequest);
}

}  // namespace net
}  // namespace mozilla
