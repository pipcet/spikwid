/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

var EXPORTED_SYMBOLS = ["GeckoViewProgress"];

const { GeckoViewModule } = ChromeUtils.import(
  "resource://gre/modules/GeckoViewModule.jsm"
);
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");
const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);

XPCOMUtils.defineLazyServiceGetter(
  this,
  "OverrideService",
  "@mozilla.org/security/certoverride;1",
  "nsICertOverrideService"
);

XPCOMUtils.defineLazyServiceGetter(
  this,
  "IDNService",
  "@mozilla.org/network/idn-service;1",
  "nsIIDNService"
);

XPCOMUtils.defineLazyModuleGetters(this, {
  BrowserUtils: "resource://gre/modules/BrowserUtils.jsm",
  HistogramStopwatch: "resource://gre/modules/GeckoViewTelemetry.jsm",
});

var IdentityHandler = {
  // The definitions below should be kept in sync with those in GeckoView.ProgressListener.SecurityInformation
  // No trusted identity information. No site identity icon is shown.
  IDENTITY_MODE_UNKNOWN: 0,

  // Domain-Validation SSL CA-signed domain verification (DV).
  IDENTITY_MODE_IDENTIFIED: 1,

  // Extended-Validation SSL CA-signed identity information (EV). A more rigorous validation process.
  IDENTITY_MODE_VERIFIED: 2,

  // The following mixed content modes are only used if "security.mixed_content.block_active_content"
  // is enabled. Our Java frontend coalesces them into one indicator.

  // No mixed content information. No mixed content icon is shown.
  MIXED_MODE_UNKNOWN: 0,

  // Blocked active mixed content.
  MIXED_MODE_CONTENT_BLOCKED: 1,

  // Loaded active mixed content.
  MIXED_MODE_CONTENT_LOADED: 2,

  /**
   * Determines the identity mode corresponding to the icon we show in the urlbar.
   */
  getIdentityMode: function getIdentityMode(aState) {
    if (aState & Ci.nsIWebProgressListener.STATE_IDENTITY_EV_TOPLEVEL) {
      return this.IDENTITY_MODE_VERIFIED;
    }

    if (aState & Ci.nsIWebProgressListener.STATE_IS_SECURE) {
      return this.IDENTITY_MODE_IDENTIFIED;
    }

    return this.IDENTITY_MODE_UNKNOWN;
  },

  getMixedDisplayMode: function getMixedDisplayMode(aState) {
    if (aState & Ci.nsIWebProgressListener.STATE_LOADED_MIXED_DISPLAY_CONTENT) {
      return this.MIXED_MODE_CONTENT_LOADED;
    }

    if (
      aState & Ci.nsIWebProgressListener.STATE_BLOCKED_MIXED_DISPLAY_CONTENT
    ) {
      return this.MIXED_MODE_CONTENT_BLOCKED;
    }

    return this.MIXED_MODE_UNKNOWN;
  },

  getMixedActiveMode: function getActiveDisplayMode(aState) {
    // Only show an indicator for loaded mixed content if the pref to block it is enabled
    if (
      aState & Ci.nsIWebProgressListener.STATE_LOADED_MIXED_ACTIVE_CONTENT &&
      !Services.prefs.getBoolPref("security.mixed_content.block_active_content")
    ) {
      return this.MIXED_MODE_CONTENT_LOADED;
    }

    if (aState & Ci.nsIWebProgressListener.STATE_BLOCKED_MIXED_ACTIVE_CONTENT) {
      return this.MIXED_MODE_CONTENT_BLOCKED;
    }

    return this.MIXED_MODE_UNKNOWN;
  },

  /**
   * Determine the identity of the page being displayed by examining its SSL cert
   * (if available). Return the data needed to update the UI.
   */
  checkIdentity: function checkIdentity(aState, aBrowser) {
    const identityMode = this.getIdentityMode(aState);
    const mixedDisplay = this.getMixedDisplayMode(aState);
    const mixedActive = this.getMixedActiveMode(aState);
    const result = {
      mode: {
        identity: identityMode,
        mixed_display: mixedDisplay,
        mixed_active: mixedActive,
      },
    };

    if (aBrowser.contentPrincipal) {
      result.origin = aBrowser.contentPrincipal.originNoSuffix;
    }

    // Don't show identity data for pages with an unknown identity or if any
    // mixed content is loaded (mixed display content is loaded by default).
    if (
      identityMode === this.IDENTITY_MODE_UNKNOWN ||
      aState & Ci.nsIWebProgressListener.STATE_IS_BROKEN ||
      aState & Ci.nsIWebProgressListener.STATE_IS_INSECURE
    ) {
      result.secure = false;
      return result;
    }

    result.secure = true;

    let uri = aBrowser.currentURI || {};
    try {
      uri = Services.io.createExposableURI(uri);
    } catch (e) {}

    try {
      result.host = IDNService.convertToDisplayIDN(uri.host, {});
    } catch (e) {
      result.host = uri.host;
    }

    const cert = aBrowser.securityUI.secInfo.serverCert;

    result.certificate = aBrowser.securityUI.secInfo.serverCert.getBase64DERString();

    try {
      result.securityException = OverrideService.hasMatchingOverride(
        uri.host,
        uri.port,
        cert,
        {},
        {}
      );
    } catch (e) {}

    return result;
  },
};

class ProgressTracker {
  static flags =
    Ci.nsIWebProgress.NOTIFY_STATE_NETWORK | Ci.nsIWebProgress.NOTIFY_LOCATION;

  constructor(aModule) {
    this.progressFilter = Cc[
      "@mozilla.org/appshell/component/browser-status-filter;1"
    ].createInstance(Ci.nsIWebProgress);
    this.progressFilter.addProgressListener(this, this.flags);

    const { browser } = aModule;
    browser.addProgressListener(this.progressFilter, this.flags);

    const window = browser.ownerGlobal;
    this.pageLoadProbe = new HistogramStopwatch("GV_PAGE_LOAD_MS", window);
    this.pageReloadProbe = new HistogramStopwatch("GV_PAGE_RELOAD_MS", window);
    this.pageLoadProgressProbe = new HistogramStopwatch(
      "GV_PAGE_LOAD_PROGRESS_MS",
      window
    );

    this.clear();
    this._eventReceived = null;
    this._module = aModule;
  }

  destroy() {
    this.progressFilter.removeProgressListener(this, this.flags);
    this._module.browser.removeProgressListener(
      this.progressFilter,
      this.flags
    );
  }

  get eventDispatcher() {
    return this._module.eventDispatcher;
  }

  start(aUri) {
    debug`ProgressTracker start ${aUri}`;

    if (this._eventReceived) {
      // A request was already in process, let's cancel it
      this.pageLoadProgressProbe.cancel();
      this.stop();
    }

    this._eventReceived = new Set();
    this.clear();
    const data = this._data;

    if (aUri === "about:blank") {
      data.uri = null;
      return;
    }

    this.pageLoadProgressProbe.start();

    data.uri = aUri;
    data.pageStart = true;
    this.updateProgress();
  }

  changeLocation(aUri) {
    debug`ProgressTracker changeLocation ${aUri}`;

    const data = this._data;
    data.locationChange = true;
    data.uri = aUri;
  }

  stop() {
    debug`ProgressTracker stop`;

    const data = this._data;
    data.pageStop = true;
    this.updateProgress();
    this._eventReceived = null;
  }

  onStateChange(aWebProgress, aRequest, aStateFlags, aStatus) {
    debug`onStateChange: isTopLevel=${aWebProgress.isTopLevel},
                          flags=${aStateFlags}, status=${aStatus}`;

    if (!aWebProgress || !aWebProgress.isTopLevel) {
      return;
    }

    const uri = aRequest.QueryInterface(Ci.nsIChannel).URI.displaySpec;

    if (aRequest.URI.schemeIs("about")) {
      return;
    }

    debug`onStateChange: uri=${uri}`;

    let isPageReload = false;
    if (aWebProgress.loadType & Ci.nsIDocShell.LOAD_CMD_RELOAD) {
      isPageReload = true;
    }

    if (aStateFlags & Ci.nsIWebProgressListener.STATE_START) {
      if (!isPageReload) {
        this.pageLoadProbe.start();
      } else {
        this.pageReloadProbe.start();
      }
      this.start(uri);
    } else if (
      aStateFlags & Ci.nsIWebProgressListener.STATE_STOP &&
      !aWebProgress.isLoadingDocument
    ) {
      if (!isPageReload) {
        this.pageLoadProbe.finish();
      } else {
        this.pageReloadProbe.finish();
      }
      this.stop();
    } else if (aStateFlags & Ci.nsIWebProgressListener.STATE_REDIRECTING) {
      if (!isPageReload) {
        this.pageLoadProbe.start();
      } else {
        this.pageReloadProbe.start();
      }
      this.start(uri);
    }
  }

  onLocationChange(aWebProgress, aRequest, aLocationURI, aFlags) {
    if (
      !aWebProgress ||
      !aWebProgress.isTopLevel ||
      !aLocationURI ||
      aLocationURI.schemeIs("about")
    ) {
      return;
    }

    debug`onLocationChange: location=${aLocationURI.displaySpec},
                             flags=${aFlags}`;

    if (aFlags & Ci.nsIWebProgressListener.LOCATION_CHANGE_ERROR_PAGE) {
      this.stop();
    } else {
      this.changeLocation(aLocationURI.displaySpec);
    }
  }

  QueryInterface = ChromeUtils.generateQI(["nsIWebProgressListener"]);

  handleEvent(aEvent) {
    if (!this._eventReceived || this._eventReceived.has(aEvent.name)) {
      // Either we're not tracking or we have received this event already
      return;
    }

    const data = this._data;

    if (!data.uri || data.uri !== aEvent.uri) {
      return;
    }

    debug`ProgressTracker handleEvent: ${aEvent.name}`;

    let needsUpdate = false;

    switch (aEvent.name) {
      case "DOMContentLoaded":
        needsUpdate = needsUpdate || !data.parsed;
        data.parsed = true;
        break;
      case "MozAfterPaint":
        needsUpdate = needsUpdate || !data.firstPaint;
        data.firstPaint = true;
        break;
      case "pageshow":
        needsUpdate = needsUpdate || !data.pageShow;
        data.pageShow = true;
        break;
    }

    this._eventReceived.add(aEvent.name);

    if (needsUpdate) {
      this.updateProgress();
    }
  }

  clear() {
    this._data = {
      prev: 0,
      uri: null,
      locationChange: false,
      pageStart: false,
      pageStop: false,
      firstPaint: false,
      pageShow: false,
      parsed: false,
    };
  }

  _debugData() {
    return {
      prev: this._data.prev,
      uri: this._data.uri,
      locationChange: this._data.locationChange,
      pageStart: this._data.pageStart,
      pageStop: this._data.pageStop,
      firstPaint: this._data.firstPaint,
      pageShow: this._data.pageShow,
      parsed: this._data.parsed,
    };
  }

  updateProgress() {
    debug`ProgressTracker updateProgress`;

    const data = this._data;

    if (!this._eventReceived || !data.uri) {
      return;
    }

    let progress = 0;
    if (data.pageStop || data.pageShow) {
      progress = 100;
    } else if (data.firstPaint) {
      progress = 80;
    } else if (data.parsed) {
      progress = 55;
    } else if (data.locationChange) {
      progress = 30;
    } else if (data.pageStart) {
      progress = 15;
    }

    if (data.prev >= progress) {
      return;
    }

    debug`ProgressTracker updateProgress data=${this._debugData()}
           progress=${progress}`;

    this.eventDispatcher.sendRequest({
      type: "GeckoView:ProgressChanged",
      progress,
    });

    data.prev = progress;

    if (progress >= 100) {
      this.pageLoadProgressProbe.finish();
    }
  }
}

class GeckoViewProgress extends GeckoViewModule {
  onInit() {
    debug`onInit`;
    this._hostChanged = false;
  }

  onEnable() {
    debug`onEnable`;

    this._progressTracker = new ProgressTracker(this);
    this._fireInitialLoad();
    this._initialAboutBlank = true;
    const flags =
      Ci.nsIWebProgress.NOTIFY_STATE_NETWORK |
      Ci.nsIWebProgress.NOTIFY_SECURITY |
      Ci.nsIWebProgress.NOTIFY_LOCATION;
    this.progressFilter = Cc[
      "@mozilla.org/appshell/component/browser-status-filter;1"
    ].createInstance(Ci.nsIWebProgress);
    this.progressFilter.addProgressListener(this, flags);
    this.browser.addProgressListener(this.progressFilter, flags);
    Services.obs.addObserver(this, "oop-frameloader-crashed");
    this.registerListener("GeckoView:FlushSessionState");
  }

  onDisable() {
    debug`onDisable`;

    this._progressTracker.destroy();
    this._progressTracker = null;

    if (this.progressFilter) {
      this.progressFilter.removeProgressListener(this);
      this.browser.removeProgressListener(this.progressFilter);
    }

    Services.obs.removeObserver(this, "oop-frameloader-crashed");
    this.unregisterListener("GeckoView:FlushSessionState");
  }

  receiveMessage(aMsg) {
    debug`receiveMessage: ${aMsg.name}`;

    switch (aMsg.name) {
      case "DOMContentLoaded": // fall-through
      case "MozAfterPaint": // fall-through
      case "pageshow": {
        this._progressTracker?.handleEvent(aMsg);
        break;
      }
    }
  }

  onEvent(aEvent, aData, aCallback) {
    debug`onEvent: event=${aEvent}, data=${aData}`;

    switch (aEvent) {
      case "GeckoView:FlushSessionState":
        this.messageManager.sendAsyncMessage("GeckoView:FlushSessionState");
        break;
    }
  }

  onSettingsUpdate() {
    const settings = this.settings;
    debug`onSettingsUpdate: ${settings}`;
  }

  onStateChange(aWebProgress, aRequest, aStateFlags, aStatus) {
    debug`onStateChange: isTopLevel=${aWebProgress.isTopLevel},
                          flags=${aStateFlags}, status=${aStatus}
                          loadType=${aWebProgress.loadType}`;

    if (!aWebProgress.isTopLevel) {
      return;
    }

    const { displaySpec, spec } = aRequest.QueryInterface(Ci.nsIChannel).URI;
    const isSuccess = aStatus == Cr.NS_OK;
    const isStart = (aStateFlags & Ci.nsIWebProgressListener.STATE_START) != 0;
    const isStop = (aStateFlags & Ci.nsIWebProgressListener.STATE_STOP) != 0;

    debug`onStateChange: uri=${spec} isSuccess=${isSuccess}
           isStart=${isStart} isStop=${isStop}`;

    // GeckoView never gets PageStart or PageStop for about:blank because we
    // set nodefaultsrc to true unconditionally so we can assume here that
    // we're starting a page load for a non-blank page (or a consumer-initiated
    // about:blank load).
    this._initialAboutBlank = false;

    if (isStart) {
      this._inProgress = true;
      const message = {
        type: "GeckoView:PageStart",
        uri: displaySpec,
      };

      this.eventDispatcher.sendRequest(message);
    } else if (isStop && !aWebProgress.isLoadingDocument) {
      this._inProgress = false;

      const message = {
        type: "GeckoView:PageStop",
        success: isSuccess,
      };

      this.eventDispatcher.sendRequest(message);

      BrowserUtils.recordSiteOriginTelemetry(
        Services.wm.getEnumerator("navigator:geckoview"),
        true
      );
    }
  }

  // The initial about:blank load events are unreliable because docShell starts
  // up concurrently with loading geckoview.js so we're never guaranteed to get
  // the events.
  // What we do instead is ignore all initial about:blank events and fire them
  // manually once the child process has booted up.
  _fireInitialLoad() {
    this.eventDispatcher.sendRequest({
      type: "GeckoView:PageStart",
      uri: "about:blank",
    });
    this.eventDispatcher.sendRequest({
      type: "GeckoView:LocationChange",
      uri: "about:blank",
      canGoBack: false,
      canGoForward: false,
      isTopLevel: true,
    });
    this.eventDispatcher.sendRequest({
      type: "GeckoView:PageStop",
      success: true,
    });
  }

  onSecurityChange(aWebProgress, aRequest, aState) {
    debug`onSecurityChange`;

    // Don't need to do anything if the data we use to update the UI hasn't changed
    if (this._state === aState && !this._hostChanged) {
      return;
    }

    // We don't report messages about the initial about:blank
    if (this._initialAboutBlank) {
      return;
    }

    this._state = aState;
    this._hostChanged = false;

    const identity = IdentityHandler.checkIdentity(aState, this.browser);

    const message = {
      type: "GeckoView:SecurityChanged",
      identity,
    };

    this.eventDispatcher.sendRequest(message);
  }

  onLocationChange(aWebProgress, aRequest, aLocationURI, aFlags) {
    debug`onLocationChange: location=${aLocationURI.displaySpec},
                             flags=${aFlags}`;

    this._hostChanged = true;
  }

  // nsIObserver event handler
  observe(aSubject, aTopic, aData) {
    debug`observe: topic=${aTopic}`;

    switch (aTopic) {
      case "oop-frameloader-crashed": {
        const browser = aSubject.ownerElement;
        if (!browser || browser != this.browser || !this._inProgress) {
          return;
        }

        this.eventDispatcher.sendRequest({
          type: "GeckoView:PageStop",
          success: false,
        });
      }
    }
  }
}

const { debug, warn } = GeckoViewProgress.initLogging("GeckoViewProgress");
