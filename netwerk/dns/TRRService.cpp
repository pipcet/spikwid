/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsAppDirectoryServiceDefs.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsComponentManagerUtils.h"
#include "nsDirectoryServiceUtils.h"
#include "nsICaptivePortalService.h"
#include "nsIFile.h"
#include "nsIParentalControlsService.h"
#include "nsINetworkLinkService.h"
#include "nsIObserverService.h"
#include "nsIOService.h"
#include "nsNetUtil.h"
#include "nsStandardURL.h"
#include "TRR.h"
#include "TRRService.h"

#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/Telemetry.h"
#include "mozilla/Tokenizer.h"
#include "mozilla/net/rust_helper.h"

#if defined(XP_WIN) && !defined(__MINGW32__)
#  include <shlobj_core.h>  // for SHGetSpecialFolderPathA
#endif                      // XP_WIN

static const char kOpenCaptivePortalLoginEvent[] = "captive-portal-login";
static const char kClearPrivateData[] = "clear-private-data";
static const char kPurge[] = "browser:purge-session-history";
static const char kDisableIpv6Pref[] = "network.dns.disableIPv6";

#define TRR_PREF_PREFIX "network.trr."
#define TRR_PREF(x) TRR_PREF_PREFIX x

namespace mozilla {
namespace net {

#undef LOG
extern mozilla::LazyLogModule gHostResolverLog;
#define LOG(args) MOZ_LOG(gHostResolverLog, mozilla::LogLevel::Debug, args)

TRRService* gTRRService = nullptr;
StaticRefPtr<nsIThread> sTRRBackgroundThread;
static Atomic<TRRService*> sTRRServicePtr;

static Atomic<size_t, Relaxed> sDomainIndex(0);

constexpr nsLiteralCString kTRRDomains[] = {
    // clang-format off
    "(other)"_ns,
    "mozilla.cloudflare-dns.com"_ns,
    "firefox.dns.nextdns.io"_ns,
    "doh.xfinity.com"_ns,  // Steered clients
    // clang-format on
};

// static
const nsCString& TRRService::ProviderKey() { return kTRRDomains[sDomainIndex]; }

NS_IMPL_ISUPPORTS(TRRService, nsIObserver, nsISupportsWeakReference)

TRRService::TRRService()
    : mInitialized(false),
      mBlocklistDurationSeconds(60),
      mLock("trrservice"),
      mConfirmationNS("example.com"_ns),
      mCaptiveIsPassed(false),
      mTRRBLStorage("DataMutex::TRRBlocklist"),
      mParentalControlEnabled(false) {
  mConfirmation.mState = CONFIRM_INIT;
  mConfirmation.mTRRFailures = 0;
  MOZ_ASSERT(NS_IsMainThread(), "wrong thread");
}

// static
void TRRService::AddObserver(nsIObserver* aObserver,
                             nsIObserverService* aObserverService) {
  nsCOMPtr<nsIObserverService> observerService;
  if (aObserverService) {
    observerService = aObserverService;
  } else {
    observerService = mozilla::services::GetObserverService();
  }

  if (observerService) {
    observerService->AddObserver(aObserver, NS_CAPTIVE_PORTAL_CONNECTIVITY,
                                 true);
    observerService->AddObserver(aObserver, kOpenCaptivePortalLoginEvent, true);
    observerService->AddObserver(aObserver, kClearPrivateData, true);
    observerService->AddObserver(aObserver, kPurge, true);
    observerService->AddObserver(aObserver, NS_NETWORK_LINK_TOPIC, true);
    observerService->AddObserver(aObserver, NS_DNS_SUFFIX_LIST_UPDATED_TOPIC,
                                 true);
    observerService->AddObserver(aObserver, "xpcom-shutdown-threads", true);
  }
}

// static
bool TRRService::CheckCaptivePortalIsPassed() {
  bool result = false;
  nsCOMPtr<nsICaptivePortalService> captivePortalService =
      do_GetService(NS_CAPTIVEPORTAL_CID);
  if (captivePortalService) {
    int32_t captiveState;
    MOZ_ALWAYS_SUCCEEDS(captivePortalService->GetState(&captiveState));

    if ((captiveState == nsICaptivePortalService::UNLOCKED_PORTAL) ||
        (captiveState == nsICaptivePortalService::NOT_CAPTIVE)) {
      result = true;
    }
    LOG(("TRRService::Init mCaptiveState=%d mCaptiveIsPassed=%d\n",
         captiveState, (int)result));
  }

  return result;
}

static void RemoveTRRBlocklistFile() {
  MOZ_ASSERT(NS_IsMainThread(), "Getting the profile dir on the main thread");

  nsCOMPtr<nsIFile> file;
  nsresult rv =
      NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR, getter_AddRefs(file));
  if (NS_FAILED(rv)) {
    return;
  }

  rv = file->AppendNative("TRRBlacklist.txt"_ns);
  if (NS_FAILED(rv)) {
    return;
  }

  // Dispatch an async task that removes the blocklist file from the profile.
  rv = NS_DispatchBackgroundTask(
      NS_NewRunnableFunction("RemoveTRRBlocklistFile::Remove",
                             [file] { file->Remove(false); }),
      NS_DISPATCH_EVENT_MAY_BLOCK);
  if (NS_FAILED(rv)) {
    return;
  }
  Preferences::SetBool("network.trr.blocklist_cleanup_done", true);
}

static void EventTelemetryPrefChanged(const char* aPref, void* aData) {
  Telemetry::SetEventRecordingEnabled(
      "network.dns"_ns,
      StaticPrefs::network_trr_confirmation_telemetry_enabled());
}

nsresult TRRService::Init() {
  MOZ_ASSERT(NS_IsMainThread(), "wrong thread");
  if (mInitialized) {
    return NS_OK;
  }
  mInitialized = true;

  AddObserver(this);

  nsCOMPtr<nsIPrefBranch> prefBranch;
  GetPrefBranch(getter_AddRefs(prefBranch));
  if (prefBranch) {
    prefBranch->AddObserver(TRR_PREF_PREFIX, this, true);
    prefBranch->AddObserver(kDisableIpv6Pref, this, true);
    prefBranch->AddObserver(kRolloutURIPref, this, true);
    prefBranch->AddObserver(kRolloutModePref, this, true);
  }

  gTRRService = this;
  sTRRServicePtr = this;

  ReadPrefs(nullptr);

  if (XRE_IsParentProcess()) {
    mCaptiveIsPassed = CheckCaptivePortalIsPassed();

    mParentalControlEnabled = GetParentalControlEnabledInternal();

    mLinkService = do_GetService(NS_NETWORK_LINK_SERVICE_CONTRACTID);
    if (mLinkService) {
      nsTArray<nsCString> suffixList;
      mLinkService->GetDnsSuffixList(suffixList);
      RebuildSuffixList(std::move(suffixList));
    }

    nsCOMPtr<nsIThread> thread;
    if (NS_FAILED(
            NS_NewNamedThread("TRR Background", getter_AddRefs(thread)))) {
      NS_WARNING("NS_NewNamedThread failed!");
      return NS_ERROR_FAILURE;
    }

    sTRRBackgroundThread = thread;

    if (!StaticPrefs::network_trr_blocklist_cleanup_done()) {
      // Dispatch an idle task to the main thread that gets the profile dir
      // then attempts to delete the blocklist file on a background thread.
      Unused << NS_DispatchToMainThreadQueue(
          NS_NewCancelableRunnableFunction("RemoveTRRBlocklistFile::GetDir",
                                           [] { RemoveTRRBlocklistFile(); }),
          EventQueuePriority::Idle);
    }
  }

  mODoHService = new ODoHService();
  if (!mODoHService->Init()) {
    return NS_ERROR_FAILURE;
  }

  Preferences::RegisterCallbackAndCall(
      EventTelemetryPrefChanged,
      "network.trr.confirmation_telemetry_enabled"_ns);

  LOG(("Initialized TRRService\n"));
  return NS_OK;
}

// static
bool TRRService::GetParentalControlEnabledInternal() {
  nsCOMPtr<nsIParentalControlsService> pc =
      do_CreateInstance("@mozilla.org/parental-controls-service;1");
  if (pc) {
    bool result = false;
    pc->GetParentalControlsEnabled(&result);
    LOG(("TRRService::GetParentalControlEnabledInternal=%d\n", result));
    return result;
  }

  return false;
}

void TRRService::SetDetectedTrrURI(const nsACString& aURI) {
  // If the user has set a custom URI then we don't want to override that.
  if (mURIPrefHasUserValue) {
    return;
  }

  mURISetByDetection = MaybeSetPrivateURI(aURI);
}

bool TRRService::Enabled(nsIRequest::TRRMode aRequestMode) {
  if (mMode == nsIDNSService::MODE_TRROFF ||
      aRequestMode == nsIRequest::TRR_DISABLED_MODE) {
    return false;
  }

  if (mConfirmation.mState == CONFIRM_INIT &&
      (!StaticPrefs::network_trr_wait_for_portal() || mCaptiveIsPassed ||
       (mMode == nsIDNSService::MODE_TRRONLY ||
        aRequestMode == nsIRequest::TRR_ONLY_MODE))) {
    LOG(("TRRService::Enabled => CONFIRM_TRYING\n"));
    mConfirmation.mState = CONFIRM_TRYING;
  }

  if (mConfirmation.mState == CONFIRM_TRYING) {
    LOG(("TRRService::Enabled MaybeConfirm()\n"));
    MaybeConfirm("context-init");
    if (mMode == nsIDNSService::MODE_TRRONLY) {
      MOZ_ASSERT(mConfirmation.mState == CONFIRM_OK,
                 "Global mode is trr-only, but confirmation failed?");
    }
  }

  LOG(("TRRService::Enabled mConfirmation.mState=%d mCaptiveIsPassed=%d\n",
       (int)mConfirmation.mState, (int)mCaptiveIsPassed));

  if (mConfirmation.mState == CONFIRM_OK) {
    return true;
  }

  if (StaticPrefs::network_trr_wait_for_confirmation()) {
    return false;
  }

  if ((aRequestMode == nsIRequest::TRR_DEFAULT_MODE &&
       mMode == nsIDNSService::MODE_TRRONLY) ||
      aRequestMode == nsIRequest::TRR_ONLY_MODE) {
    // For TRR-only requests, or if the global mode is TRR-only, just say we're
    // enabled.
    return true;
  }

  if ((aRequestMode == nsIRequest::TRR_DEFAULT_MODE &&
       mMode == nsIDNSService::MODE_TRRFIRST) ||
      aRequestMode == nsIRequest::TRR_FIRST_MODE) {
    return mConfirmation.mState != CONFIRM_FAILED;
  }

  return false;
}

void TRRService::GetPrefBranch(nsIPrefBranch** result) {
  MOZ_ASSERT(NS_IsMainThread(), "wrong thread");
  *result = nullptr;
  CallGetService(NS_PREFSERVICE_CONTRACTID, result);
}

bool TRRService::MaybeSetPrivateURI(const nsACString& aURI) {
  bool clearCache = false;
  nsAutoCString newURI(aURI);
  ProcessURITemplate(newURI);

  {
    MutexAutoLock lock(mLock);
    if (mPrivateURI.Equals(newURI)) {
      return false;
    }

    if (!mPrivateURI.IsEmpty()) {
      LOG(("TRRService clearing blocklist because of change in uri service\n"));
      auto bl = mTRRBLStorage.Lock();
      bl->Clear();
      clearCache = true;
    }

    nsCOMPtr<nsIURI> url;
    nsresult rv =
        NS_MutateURI(NS_STANDARDURLMUTATOR_CONTRACTID)
            .Apply(NS_MutatorMethod(&nsIStandardURLMutator::Init,
                                    nsIStandardURL::URLTYPE_STANDARD, 443,
                                    newURI, nullptr, nullptr, nullptr))
            .Finalize(url);
    if (NS_FAILED(rv)) {
      LOG(("TRRService::MaybeSetPrivateURI failed to create URI!\n"));
      return false;
    }

    nsAutoCString host;
    url->GetHost(host);

    sDomainIndex = 0;
    for (size_t i = 1; i < std::size(kTRRDomains); i++) {
      if (host.Equals(kTRRDomains[i])) {
        sDomainIndex = i;
        break;
      }
    }

    mPrivateURI = newURI;
  }

  // Clear the cache because we changed the URI
  if (clearCache) {
    ClearEntireCache();
  }

  nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->NotifyObservers(nullptr, NS_NETWORK_TRR_URI_CHANGED_TOPIC, nullptr);
  }
  return true;
}

nsresult TRRService::ReadPrefs(const char* name) {
  MOZ_ASSERT(NS_IsMainThread(), "wrong thread");

  // Whenever a pref change occurs that would cause us to clear the cache
  // we set this to true then do it at the end of the method.
  bool clearEntireCache = false;

  if (!name || !strcmp(name, TRR_PREF("mode")) ||
      !strcmp(name, kRolloutModePref)) {
    nsIDNSService::ResolverMode prevMode = Mode();

    OnTRRModeChange();

    // When the TRR service gets disabled we should purge the TRR cache to
    // make sure we don't use any of the cached entries on a network where
    // they are invalid - for example after turning on a VPN.
    if (TRR_DISABLED(Mode()) && !TRR_DISABLED(prevMode)) {
      clearEntireCache = true;
    }
  }
  if (!name || !strcmp(name, TRR_PREF("uri")) ||
      !strcmp(name, kRolloutURIPref)) {
    OnTRRURIChange();
  }
  if (!name || !strcmp(name, TRR_PREF("credentials"))) {
    MutexAutoLock lock(mLock);
    Preferences::GetCString(TRR_PREF("credentials"), mPrivateCred);
  }
  if (!name || !strcmp(name, TRR_PREF("confirmationNS"))) {
    MutexAutoLock lock(mLock);
    nsAutoCString old(mConfirmationNS);
    Preferences::GetCString(TRR_PREF("confirmationNS"), mConfirmationNS);
    if (name && !old.IsEmpty() && !mConfirmationNS.Equals(old) &&
        (mConfirmation.mState > CONFIRM_TRYING) &&
        (mMode == nsIDNSService::MODE_TRRFIRST ||
         mMode == nsIDNSService::MODE_TRRONLY)) {
      LOG(("TRR::ReadPrefs: restart confirmationNS state\n"));
      mConfirmation.mState = CONFIRM_TRYING;
      MaybeConfirm_locked("pref-change");
    }
  }
  if (!name || !strcmp(name, TRR_PREF("bootstrapAddress"))) {
    MutexAutoLock lock(mLock);
    Preferences::GetCString(TRR_PREF("bootstrapAddress"), mBootstrapAddr);
    clearEntireCache = true;
  }
  if (!name || !strcmp(name, TRR_PREF("blacklist-duration"))) {
    // prefs is given in number of seconds
    uint32_t secs;
    if (NS_SUCCEEDED(
            Preferences::GetUint(TRR_PREF("blacklist-duration"), &secs))) {
      mBlocklistDurationSeconds = secs;
    }
  }
  if (!name || !strcmp(name, kDisableIpv6Pref)) {
    bool tmp;
    if (NS_SUCCEEDED(Preferences::GetBool(kDisableIpv6Pref, &tmp))) {
      mDisableIPv6 = tmp;
    }
  }
  if (!name || !strcmp(name, TRR_PREF("excluded-domains")) ||
      !strcmp(name, TRR_PREF("builtin-excluded-domains"))) {
    MutexAutoLock lock(mLock);
    mExcludedDomains.Clear();

    auto parseExcludedDomains = [this](const char* aPrefName) {
      nsAutoCString excludedDomains;
      Preferences::GetCString(aPrefName, excludedDomains);
      if (excludedDomains.IsEmpty()) {
        return;
      }

      for (const nsACString& tokenSubstring :
           nsCCharSeparatedTokenizerTemplate<
               NS_IsAsciiWhitespace, nsTokenizerFlags::SeparatorOptional>(
               excludedDomains, ',')
               .ToRange()) {
        nsCString token{tokenSubstring};
        LOG(("TRRService::ReadPrefs %s host:[%s]\n", aPrefName, token.get()));
        mExcludedDomains.PutEntry(token);
      }
    };

    parseExcludedDomains(TRR_PREF("excluded-domains"));
    parseExcludedDomains(TRR_PREF("builtin-excluded-domains"));
    clearEntireCache = true;
  }

  // if name is null, then we're just now initializing. In that case we don't
  // need to clear the cache.
  if (name && clearEntireCache) {
    ClearEntireCache();
  }

  return NS_OK;
}

void TRRService::ClearEntireCache() {
  if (!StaticPrefs::network_trr_clear_cache_on_pref_change()) {
    return;
  }
  nsCOMPtr<nsIDNSService> dns = do_GetService(NS_DNSSERVICE_CONTRACTID);
  if (!dns) {
    return;
  }
  dns->ClearCache(true);
}

void TRRService::AddEtcHosts(const nsTArray<nsCString>& aArray) {
  MutexAutoLock lock(mLock);
  for (const auto& item : aArray) {
    LOG(("Adding %s from /etc/hosts to excluded domains", item.get()));
    mEtcHostsDomains.PutEntry(item);
  }
}

void TRRService::ReadEtcHostsFile() {
  if (!StaticPrefs::network_trr_exclude_etc_hosts()) {
    return;
  }

  auto readHostsTask = []() {
    MOZ_ASSERT(!NS_IsMainThread(), "Must not run on the main thread");
#if defined(XP_WIN) && !defined(__MINGW32__)
    // Inspired by libevent/evdns.c
    // Windows is a little coy about where it puts its configuration
    // files.  Sure, they're _usually_ in C:\windows\system32, but
    // there's no reason in principle they couldn't be in
    // W:\hoboken chicken emergency

    nsCString path;
    path.SetLength(MAX_PATH + 1);
    if (!SHGetSpecialFolderPathA(NULL, path.BeginWriting(), CSIDL_SYSTEM,
                                 false)) {
      LOG(("Calling SHGetSpecialFolderPathA failed"));
      return;
    }

    path.SetLength(strlen(path.get()));
    path.Append("\\drivers\\etc\\hosts");
#elif defined(__MINGW32__)
    nsAutoCString path("C:\\windows\\system32\\drivers\\etc\\hosts"_ns);
#else
    nsAutoCString path("/etc/hosts"_ns);
#endif

    LOG(("Reading hosts file at %s", path.get()));
    rust_parse_etc_hosts(&path, [](const nsTArray<nsCString>* aArray) -> bool {
      RefPtr<TRRService> service(sTRRServicePtr);
      if (service && aArray) {
        service->AddEtcHosts(*aArray);
      }
      return !!service;
    });
  };

  Unused << NS_DispatchBackgroundTask(
      NS_NewRunnableFunction("Read /etc/hosts file", readHostsTask),
      NS_DISPATCH_EVENT_MAY_BLOCK);
}

nsresult TRRService::GetURI(nsACString& result) {
  MutexAutoLock lock(mLock);
  result = mPrivateURI;
  return NS_OK;
}

nsresult TRRService::GetCredentials(nsCString& result) {
  MutexAutoLock lock(mLock);
  result = mPrivateCred;
  return NS_OK;
}

uint32_t TRRService::GetRequestTimeout() {
  if (mMode == nsIDNSService::MODE_TRRONLY) {
    return StaticPrefs::network_trr_request_timeout_mode_trronly_ms();
  }

  return StaticPrefs::network_trr_request_timeout_ms();
}

nsresult TRRService::Start() {
  MOZ_ASSERT(NS_IsMainThread(), "wrong thread");
  if (!mInitialized) {
    return NS_ERROR_NOT_INITIALIZED;
  }
  return NS_OK;
}

TRRService::~TRRService() {
  MOZ_ASSERT(NS_IsMainThread(), "wrong thread");
  LOG(("Exiting TRRService\n"));
  gTRRService = nullptr;
}

nsresult TRRService::DispatchTRRRequest(TRR* aTrrRequest) {
  return DispatchTRRRequestInternal(aTrrRequest, true);
}

nsresult TRRService::DispatchTRRRequestInternal(TRR* aTrrRequest,
                                                bool aWithLock) {
  NS_ENSURE_ARG_POINTER(aTrrRequest);

  nsCOMPtr<nsIThread> thread = MainThreadOrTRRThread(aWithLock);
  if (!thread) {
    return NS_ERROR_FAILURE;
  }

  RefPtr<TRR> trr = aTrrRequest;
  return thread->Dispatch(trr.forget());
}

already_AddRefed<nsIThread> TRRService::MainThreadOrTRRThread(bool aWithLock) {
  if (!StaticPrefs::network_trr_fetch_off_main_thread() ||
      XRE_IsSocketProcess()) {
    return do_GetMainThread();
  }

  nsCOMPtr<nsIThread> thread = aWithLock ? TRRThread() : TRRThread_locked();
  return thread.forget();
}

already_AddRefed<nsIThread> TRRService::TRRThread() {
  MutexAutoLock lock(mLock);
  return TRRThread_locked();
}

already_AddRefed<nsIThread> TRRService::TRRThread_locked() {
  RefPtr<nsIThread> thread = sTRRBackgroundThread;
  return thread.forget();
}

bool TRRService::IsOnTRRThread() {
  nsCOMPtr<nsIThread> thread;
  {
    MutexAutoLock lock(mLock);
    thread = sTRRBackgroundThread;
  }
  if (!thread) {
    return false;
  }

  return thread->IsOnCurrentThread();
}

NS_IMETHODIMP
TRRService::Observe(nsISupports* aSubject, const char* aTopic,
                    const char16_t* aData) {
  MOZ_ASSERT(NS_IsMainThread(), "wrong thread");
  LOG(("TRR::Observe() topic=%s\n", aTopic));
  if (!strcmp(aTopic, NS_PREFBRANCH_PREFCHANGE_TOPIC_ID)) {
    ReadPrefs(NS_ConvertUTF16toUTF8(aData).get());

    mConfirmation.RecordEvent("pref-change");
    MutexAutoLock lock(mLock);
    if (((mConfirmation.mState == CONFIRM_INIT) && !mBootstrapAddr.IsEmpty() &&
         (mMode == nsIDNSService::MODE_TRRONLY)) ||
        (mConfirmation.mState == CONFIRM_FAILED)) {
      mConfirmation.mState = CONFIRM_TRYING;
      MaybeConfirm_locked("pref-change");
    }
  } else if (!strcmp(aTopic, kOpenCaptivePortalLoginEvent)) {
    // We are in a captive portal
    LOG(("TRRservice in captive portal\n"));
    mCaptiveIsPassed = false;
    mConfirmation.mCaptivePortalStatus = nsICaptivePortalService::LOCKED_PORTAL;
  } else if (!strcmp(aTopic, NS_CAPTIVE_PORTAL_CONNECTIVITY)) {
    nsAutoCString data = NS_ConvertUTF16toUTF8(aData);
    LOG(("TRRservice captive portal was %s\n", data.get()));

    // We should avoid doing calling MaybeConfirm in response to a pref change
    // unless the service is in a TRR=enabled mode.
    if (mMode == nsIDNSService::MODE_TRRFIRST ||
        mMode == nsIDNSService::MODE_TRRONLY) {
      if (mConfirmation.mTimer) {
        mConfirmation.mTimer->Cancel();
        mConfirmation.mTimer = nullptr;
      }
      mConfirmation.mRetryInterval =
          StaticPrefs::network_trr_retry_timeout_ms();
      if (mConfirmation.mState != CONFIRM_OK) {
        mConfirmation.mState = CONFIRM_TRYING;
        MaybeConfirm("cp-connectivity");
      }
    }

    mCaptiveIsPassed = true;
    nsCOMPtr<nsICaptivePortalService> cps = do_QueryInterface(aSubject);
    if (cps) {
      cps->GetState(&mConfirmation.mCaptivePortalStatus);
    }
  } else if (!strcmp(aTopic, kClearPrivateData) || !strcmp(aTopic, kPurge)) {
    // flush the TRR blocklist
    auto bl = mTRRBLStorage.Lock();
    bl->Clear();
  } else if (!strcmp(aTopic, NS_DNS_SUFFIX_LIST_UPDATED_TOPIC) ||
             !strcmp(aTopic, NS_NETWORK_LINK_TOPIC)) {
    // nsINetworkLinkService is only available on parent process.
    if (XRE_IsParentProcess()) {
      nsCOMPtr<nsINetworkLinkService> link = do_QueryInterface(aSubject);
      // The network link service notification normally passes itself as the
      // subject, but some unit tests will sometimes pass a null subject.
      if (link) {
        nsTArray<nsCString> suffixList;
        link->GetDnsSuffixList(suffixList);
        RebuildSuffixList(std::move(suffixList));
      }
    }

    if (!strcmp(aTopic, NS_NETWORK_LINK_TOPIC)) {
      if (NS_ConvertUTF16toUTF8(aData).EqualsLiteral(
              NS_NETWORK_LINK_DATA_DOWN)) {
        mConfirmation.RecordEvent("network-change");
      }
      if (mURISetByDetection) {
        // If the URI was set via SetDetectedTrrURI we need to restore it to the
        // default pref when a network link change occurs.
        CheckURIPrefs();
      }
    }
  } else if (!strcmp(aTopic, "xpcom-shutdown-threads")) {
    // If a confirmation is still in progress we record the event.
    // Since there should be no more confirmations after this, the shutdown
    // reason would not really be recorded in telemetry.
    mConfirmation.RecordEvent("shutdown");

    if (sTRRBackgroundThread) {
      nsCOMPtr<nsIThread> thread;
      {
        MutexAutoLock lock(mLock);
        thread = sTRRBackgroundThread.get();
        sTRRBackgroundThread = nullptr;
      }
      MOZ_ALWAYS_SUCCEEDS(thread->Shutdown());
      sTRRServicePtr = nullptr;
    }
  }
  return NS_OK;
}

void TRRService::RebuildSuffixList(nsTArray<nsCString>&& aSuffixList) {
  if (!StaticPrefs::network_trr_split_horizon_mitigations()) {
    return;
  }

  MutexAutoLock lock(mLock);
  mDNSSuffixDomains.Clear();
  for (const auto& item : aSuffixList) {
    LOG(("TRRService adding %s to suffix list", item.get()));
    mDNSSuffixDomains.PutEntry(item);
  }
}

void TRRService::MaybeConfirm(const char* aReason) {
  MutexAutoLock lock(mLock);
  MaybeConfirm_locked(aReason);
}

void TRRService::MaybeConfirm_locked(const char* aReason) {
  mLock.AssertCurrentThreadOwns();

  if (mMode == nsIDNSService::MODE_TRROFF || mConfirmation.mTask ||
      mConfirmation.mState != CONFIRM_TRYING) {
    LOG(
        ("TRRService:MaybeConfirm mode=%d, mConfirmation.mTask=%p "
         "mConfirmation.mState=%d\n",
         (int)mMode, (void*)mConfirmation.mTask, (int)mConfirmation.mState));
    return;
  }

  if (mConfirmationNS.Equals("skip") || mMode == nsIDNSService::MODE_TRRONLY) {
    LOG(("TRRService starting confirmation test %s SKIPPED\n",
         mPrivateURI.get()));
    mConfirmation.mState = CONFIRM_OK;
  } else {
    LOG(("TRRService starting confirmation test %s %s\n", mPrivateURI.get(),
         mConfirmationNS.get()));

    mConfirmation.mTask =
        new TRR(this, mConfirmationNS, TRRTYPE_NS, ""_ns, false);
    mConfirmation.mTask->SetTimeout(
        StaticPrefs::network_trr_confirmation_timeout_ms());

    if (mLinkService) {
      mLinkService->GetNetworkID(mConfirmation.mNetworkId);
    }

    if (mConfirmation.mFirstRequestTime.IsNull()) {
      mConfirmation.mFirstRequestTime = TimeStamp::Now();
    }
    if (mConfirmation.mTrigger.IsEmpty()) {
      mConfirmation.mTrigger.Assign(aReason);
    }

    DispatchTRRRequestInternal(mConfirmation.mTask, false);
  }
}

bool TRRService::MaybeBootstrap(const nsACString& aPossible,
                                nsACString& aResult) {
  MutexAutoLock lock(mLock);
  if (mMode == nsIDNSService::MODE_TRROFF || mBootstrapAddr.IsEmpty()) {
    return false;
  }

  nsCOMPtr<nsIURI> url;
  nsresult rv =
      NS_MutateURI(NS_STANDARDURLMUTATOR_CONTRACTID)
          .Apply(NS_MutatorMethod(&nsIStandardURLMutator::Init,
                                  nsIStandardURL::URLTYPE_STANDARD, 443,
                                  mPrivateURI, nullptr, nullptr, nullptr))
          .Finalize(url);
  if (NS_FAILED(rv)) {
    LOG(("TRRService::MaybeBootstrap failed to create URI!\n"));
    return false;
  }

  nsAutoCString host;
  url->GetHost(host);
  if (!aPossible.Equals(host)) {
    return false;
  }
  LOG(("TRRService::MaybeBootstrap: use %s instead of %s\n",
       mBootstrapAddr.get(), host.get()));
  aResult = mBootstrapAddr;
  return true;
}

bool TRRService::IsDomainBlocked(const nsACString& aHost,
                                 const nsACString& aOriginSuffix,
                                 bool aPrivateBrowsing) {
  if (!Enabled()) {
    return true;
  }

  auto bl = mTRRBLStorage.Lock();
  if (bl->IsEmpty()) {
    return false;
  }

  // use a unified casing for the hashkey
  nsAutoCString hashkey(aHost + aOriginSuffix);
  if (auto val = bl->Lookup(hashkey)) {
    int32_t until = *val + mBlocklistDurationSeconds;
    int32_t expire = NowInSeconds();
    if (until > expire) {
      LOG(("Host [%s] is TRR blocklisted\n", nsCString(aHost).get()));
      return true;
    }

    // the blocklisted entry has expired
    val.Remove();
  }
  return false;
}

// When running in TRR-only mode, the blocklist is not used and it will also
// try resolving the localhost / .local names.
bool TRRService::IsTemporarilyBlocked(const nsACString& aHost,
                                      const nsACString& aOriginSuffix,
                                      bool aPrivateBrowsing,
                                      bool aParentsToo)  // false if domain
{
  if (mMode == nsIDNSService::MODE_TRRONLY) {
    return false;  // might as well try
  }

  LOG(("Checking if host [%s] is blocklisted", aHost.BeginReading()));

  int32_t dot = aHost.FindChar('.');
  if ((dot == kNotFound) && aParentsToo) {
    // Only if a full host name. Domains can be dotless to be able to
    // blocklist entire TLDs
    return true;
  }

  if (IsDomainBlocked(aHost, aOriginSuffix, aPrivateBrowsing)) {
    return true;
  }

  nsDependentCSubstring domain = Substring(aHost, 0);
  while (dot != kNotFound) {
    dot++;
    domain.Rebind(domain, dot, domain.Length() - dot);

    if (IsDomainBlocked(domain, aOriginSuffix, aPrivateBrowsing)) {
      return true;
    }

    dot = domain.FindChar('.');
  }

  return false;
}

bool TRRService::IsExcludedFromTRR(const nsACString& aHost) {
  // This method may be called off the main thread. We need to lock so
  // mExcludedDomains and mDNSSuffixDomains don't change while this code
  // is running.
  MutexAutoLock lock(mLock);

  return IsExcludedFromTRR_unlocked(aHost);
}

bool TRRService::IsExcludedFromTRR_unlocked(const nsACString& aHost) {
  if (!NS_IsMainThread()) {
    mLock.AssertCurrentThreadOwns();
  }

  int32_t dot = 0;
  // iteratively check the sub-domain of |aHost|
  while (dot < static_cast<int32_t>(aHost.Length())) {
    nsDependentCSubstring subdomain =
        Substring(aHost, dot, aHost.Length() - dot);

    if (mExcludedDomains.GetEntry(subdomain)) {
      LOG(("Subdomain [%s] of host [%s] Is Excluded From TRR via pref\n",
           subdomain.BeginReading(), aHost.BeginReading()));
      return true;
    }
    if (mDNSSuffixDomains.GetEntry(subdomain)) {
      LOG(("Subdomain [%s] of host [%s] Is Excluded From TRR via pref\n",
           subdomain.BeginReading(), aHost.BeginReading()));
      return true;
    }
    if (mEtcHostsDomains.GetEntry(subdomain)) {
      LOG(("Subdomain [%s] of host [%s] Is Excluded From TRR by /etc/hosts\n",
           subdomain.BeginReading(), aHost.BeginReading()));
      return true;
    }

    dot = aHost.FindChar('.', dot + 1);
    if (dot == kNotFound) {
      break;
    }
    dot++;
  }

  return false;
}

void TRRService::AddToBlocklist(const nsACString& aHost,
                                const nsACString& aOriginSuffix,
                                bool privateBrowsing, bool aParentsToo) {
  LOG(("TRR blocklist %s\n", nsCString(aHost).get()));
  nsAutoCString hashkey(aHost + aOriginSuffix);

  // this overwrites any existing entry
  {
    auto bl = mTRRBLStorage.Lock();
    bl->InsertOrUpdate(hashkey, NowInSeconds());
  }

  if (aParentsToo) {
    // when given a full host name, verify its domain as well
    int32_t dot = aHost.FindChar('.');
    if (dot != kNotFound) {
      // this has a domain to be checked
      dot++;
      nsDependentCSubstring domain =
          Substring(aHost, dot, aHost.Length() - dot);
      nsAutoCString check(domain);
      if (IsTemporarilyBlocked(check, aOriginSuffix, privateBrowsing, false)) {
        // the domain part is already blocklisted, no need to add this entry
        return;
      }
      // verify 'check' over TRR
      LOG(("TRR: verify if '%s' resolves as NS\n", check.get()));

      // check if there's an NS entry for this name
      RefPtr<TRR> trr =
          new TRR(this, check, TRRTYPE_NS, aOriginSuffix, privateBrowsing);
      DispatchTRRRequest(trr);
    }
  }
}

NS_IMETHODIMP
TRRService::Notify(nsITimer* aTimer) {
  if (aTimer == mConfirmation.mTimer) {
    mConfirmation.mTimer = nullptr;
    if (mConfirmation.mState == CONFIRM_FAILED) {
      LOG(("TRRService retry NS of %s\n", mConfirmationNS.get()));
      mConfirmation.mState = CONFIRM_TRYING;
      MaybeConfirm("retry");
    }
  } else {
    MOZ_CRASH("Unknown timer");
  }

  return NS_OK;
}

static char StatusToChar(nsresult aLookupStatus, nsresult aChannelStatus) {
  // If the resolution fails in the TRR channel then we'll have a failed
  // aChannelStatus. Otherwise, we parse the response - if it's not a valid DNS
  // packet or doesn't contain the correct responses aLookupStatus will be a
  // failure code.
  if (aChannelStatus == NS_OK) {
    // Return + if confirmation was OK, or - if confirmation failed
    return aLookupStatus == NS_OK ? '+' : '-';
  }

  if (nsCOMPtr<nsIIOService> ios = do_GetIOService()) {
    bool hasConnectiviy = true;
    ios->GetConnectivity(&hasConnectiviy);
    if (!hasConnectiviy) {
      // Browser has no active network interfaces = is offline.
      return 'o';
    }
  }

  switch (aChannelStatus) {
    case NS_ERROR_NET_TIMEOUT_EXTERNAL:
      // TRR timeout expired
      return 't';
    case NS_ERROR_UNKNOWN_HOST:
      // TRRServiceChannel failed to due to unresolved host
      return 'd';
    default:
      break;
  }

  // The error is a network error
  if (NS_ERROR_GET_MODULE(aChannelStatus) == NS_ERROR_MODULE_NETWORK) {
    return 'n';
  }

  // Some other kind of failure.
  return '?';
}

void TRRService::TRRIsOkay(nsresult aChannelStatus) {
  MOZ_ASSERT_IF(XRE_IsParentProcess(), NS_IsMainThread() || IsOnTRRThread());
  MOZ_ASSERT_IF(XRE_IsSocketProcess(), NS_IsMainThread());

  Telemetry::AccumulateCategoricalKeyed(
      ProviderKey(), NS_SUCCEEDED(aChannelStatus)
                         ? Telemetry::LABELS_DNS_TRR_SUCCESS3::Fine
                         : (aChannelStatus == NS_ERROR_NET_TIMEOUT_EXTERNAL
                                ? Telemetry::LABELS_DNS_TRR_SUCCESS3::Timeout
                                : Telemetry::LABELS_DNS_TRR_SUCCESS3::Bad));
  if (NS_SUCCEEDED(aChannelStatus)) {
    mConfirmation.mTRRFailures = 0;
  } else if ((mMode == nsIDNSService::MODE_TRRFIRST) &&
             (mConfirmation.mState == CONFIRM_OK)) {
    // only count failures while in OK state
    mConfirmation.mFailureReasons[mConfirmation.mTRRFailures %
                                  ConfirmationContext::RESULTS_SIZE] =
        StatusToChar(NS_OK, aChannelStatus);
    uint32_t fails = ++mConfirmation.mTRRFailures;

    if (fails >= StaticPrefs::network_trr_max_fails()) {
      LOG(("TRRService goes FAILED after %u failures in a row\n", fails));
      mConfirmation.mState = CONFIRM_FAILED;
      mConfirmation.mTrigger.Assign("failed-lookups");
      mConfirmation.mFailedLookups =
          nsDependentCSubstring(mConfirmation.mFailureReasons,
                                fails % ConfirmationContext::RESULTS_SIZE);
      // Fire off a timer and start re-trying the NS domain again
      NS_NewTimerWithCallback(getter_AddRefs(mConfirmation.mTimer), this,
                              mConfirmation.mRetryInterval,
                              nsITimer::TYPE_ONE_SHOT);
      mConfirmation.mTRRFailures = 0;  // clear it again
    }
  }
}

void TRRService::ConfirmationContext::RecordEvent(const char* aReason) {
  // Reset the confirmation context attributes
  // Only resets the attributes that we keep for telemetry purposes.
  auto reset = [&]() {
    mAttemptCount = 0;
    mNetworkId.Truncate();
    mFirstRequestTime = TimeStamp();
    mContextChangeReason.Assign(aReason);
    mTrigger.Truncate();
    mFailedLookups.Truncate();

    mRetryInterval = StaticPrefs::network_trr_retry_timeout_ms();
  };

  if (mAttemptCount == 0) {
    // XXX: resetting everything might not be the best thing here, even if the
    // context changes, because there might still be a confirmation pending.
    // But cancelling and retrying that confirmation might just make the whole
    // confirmation longer for no reason.
    reset();
    return;
  }

  Telemetry::EventID eventType =
      Telemetry::EventID::NetworkDns_Trrconfirmation_Context;

  nsAutoCString results;
  static_assert(RESULTS_SIZE < 64);

  // mResults is a circular buffer ending at mAttemptCount
  if (mAttemptCount <= RESULTS_SIZE) {
    // We have fewer attempts than the size of the buffer, so all of the
    // results are in the buffer.
    results.Append(nsDependentCSubstring(mResults, mAttemptCount));
  } else {
    // More attempts than the buffer size.
    // That means past RESULTS_SIZE attempts in order are
    // [posInResults .. end-of-buffer) + [start-of-buffer .. posInResults)
    uint32_t posInResults = mAttemptCount % RESULTS_SIZE;

    results.Append(nsDependentCSubstring(mResults + posInResults,
                                         RESULTS_SIZE - posInResults));
    results.Append(nsDependentCSubstring(mResults, posInResults));
  }

  auto extra = Some<nsTArray<mozilla::Telemetry::EventExtraEntry>>({
      Telemetry::EventExtraEntry{"trigger"_ns, mTrigger},
      Telemetry::EventExtraEntry{"contextReason"_ns, mContextChangeReason},
      Telemetry::EventExtraEntry{"attemptCount"_ns,
                                 nsPrintfCString("%u", mAttemptCount)},
      Telemetry::EventExtraEntry{"results"_ns, results},
      Telemetry::EventExtraEntry{
          "time"_ns,
          nsPrintfCString(
              "%f",
              !mFirstRequestTime.IsNull()
                  ? (TimeStamp::Now() - mFirstRequestTime).ToMilliseconds()
                  : 0.0)},
      Telemetry::EventExtraEntry{"networkID"_ns, mNetworkId},
      Telemetry::EventExtraEntry{"captivePortal"_ns,
                                 nsPrintfCString("%i", mCaptivePortalStatus)},
  });

  if (mTrigger.Equals("failed-lookups"_ns)) {
    extra.ref().AppendElement(
        Telemetry::EventExtraEntry{"failedLookups"_ns, mFailedLookups});
  }

  ConfirmationState state = mState;
  Telemetry::RecordEvent(eventType, mozilla::Some(nsPrintfCString("%u", state)),
                         extra);

  reset();
}

void TRRService::ConfirmationContext::RequestCompleted(
    nsresult aLookupStatus, nsresult aChannelStatus) {
  mResults[mAttemptCount % RESULTS_SIZE] =
      StatusToChar(aLookupStatus, aChannelStatus);
  mAttemptCount++;
}

AHostResolver::LookupStatus TRRService::CompleteLookup(
    nsHostRecord* rec, nsresult status, AddrInfo* aNewRRSet, bool pb,
    const nsACString& aOriginSuffix, TRRSkippedReason aReason,
    TRR* aTRRRequest) {
  // this is an NS check for the TRR blocklist or confirmationNS check

  MOZ_ASSERT_IF(XRE_IsParentProcess(), NS_IsMainThread() || IsOnTRRThread());
  MOZ_ASSERT_IF(XRE_IsSocketProcess(), NS_IsMainThread());
  MOZ_ASSERT(!rec);

  RefPtr<AddrInfo> newRRSet(aNewRRSet);
  MOZ_ASSERT(newRRSet && newRRSet->TRRType() == TRRTYPE_NS);

#ifdef DEBUG
  {
    MutexAutoLock lock(mLock);
    MOZ_ASSERT(!mConfirmation.mTask ||
               (mConfirmation.mState == CONFIRM_TRYING));
  }
#endif
  if (mConfirmation.mState == CONFIRM_TRYING) {
    mConfirmation.RequestCompleted(status, aTRRRequest->ChannelStatus());

    {
      MutexAutoLock lock(mLock);
      MOZ_ASSERT(mConfirmation.mTask);
      mConfirmation.mState = NS_SUCCEEDED(status) ? CONFIRM_OK : CONFIRM_FAILED;
      LOG(("TRRService finishing confirmation test %s %d %X\n",
           mPrivateURI.get(), (int)mConfirmation.mState, (unsigned int)status));
      mConfirmation.mTask = nullptr;
    }

    if (mConfirmation.mState == CONFIRM_OK) {
      mConfirmation.mRetryInterval =
          StaticPrefs::network_trr_retry_timeout_ms();

      // Record event and start new confirmation context
      mConfirmation.RecordEvent("success");

      // A fresh confirmation means previous blocked entries might not
      // be valid anymore.
      auto bl = mTRRBLStorage.Lock();
      bl->Clear();
    } else {
      MOZ_ASSERT(mConfirmation.mState == CONFIRM_FAILED);

      // retry failed NS confirmation
      NS_NewTimerWithCallback(getter_AddRefs(mConfirmation.mTimer), this,
                              mConfirmation.mRetryInterval,
                              nsITimer::TYPE_ONE_SHOT);
      if (mConfirmation.mRetryInterval < 64000) {
        // double the interval up to this point
        mConfirmation.mRetryInterval *= 2;
      }
    }

    if (mMode != nsIDNSService::MODE_TRRONLY) {
      // don't accumulate trr-only data here since we only care about
      // confirmation in trr-first mode
      Telemetry::Accumulate(Telemetry::DNS_TRR_NS_VERFIFIED3,
                            TRRService::ProviderKey(),
                            (mConfirmation.mState == CONFIRM_OK));
    }

    return LOOKUP_OK;
  }

  // when called without a host record, this is a domain name check response.
  if (NS_SUCCEEDED(status)) {
    LOG(("TRR verified %s to be fine!\n", newRRSet->Hostname().get()));
  } else {
    LOG(("TRR says %s doesn't resolve as NS!\n", newRRSet->Hostname().get()));
    AddToBlocklist(newRRSet->Hostname(), aOriginSuffix, pb, false);
  }
  return LOOKUP_OK;
}

AHostResolver::LookupStatus TRRService::CompleteLookupByType(
    nsHostRecord*, nsresult, mozilla::net::TypeRecordResultType& aResult,
    uint32_t aTtl, bool aPb) {
  return LOOKUP_OK;
}

#undef LOG

}  // namespace net
}  // namespace mozilla
