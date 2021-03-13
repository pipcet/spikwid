/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const EXPORTED_SYMBOLS = ["AboutWelcomeChild"];

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);

XPCOMUtils.defineLazyModuleGetters(this, {
  DEFAULT_SITES: "resource://activity-stream/lib/DefaultSites.jsm",
  ExperimentAPI: "resource://nimbus/ExperimentAPI.jsm",
  shortURL: "resource://activity-stream/lib/ShortURL.jsm",
  TippyTopProvider: "resource://activity-stream/lib/TippyTopProvider.jsm",
});

XPCOMUtils.defineLazyGetter(this, "log", () => {
  const { Logger } = ChromeUtils.import(
    "resource://messaging-system/lib/Logger.jsm"
  );
  return new Logger("AboutWelcomeChild");
});

XPCOMUtils.defineLazyGetter(this, "aboutWelcomeFeature", () => {
  const { ExperimentFeature } = ChromeUtils.import(
    "resource://nimbus/ExperimentAPI.jsm"
  );
  return new ExperimentFeature("aboutwelcome");
});

XPCOMUtils.defineLazyGetter(this, "tippyTopProvider", () =>
  (async () => {
    const provider = new TippyTopProvider();
    await provider.init();
    return provider;
  })()
);

const SEARCH_REGION_PREF = "browser.search.region";

XPCOMUtils.defineLazyPreferenceGetter(
  this,
  "searchRegion",
  SEARCH_REGION_PREF,
  ""
);

/**
 * Lazily get importable sites from parent or reuse cached ones.
 */
function getImportableSites(child) {
  return (
    getImportableSites.cache ??
    (getImportableSites.cache = (async () => {
      // Use tippy top to get packaged rich icons
      const tippyTop = await tippyTopProvider;
      // Remove duplicate entries if they would appear the same
      return `[${[
        ...new Set(
          (await child.sendQuery("AWPage:IMPORTABLE_SITES")).map(url => {
            // Get both rich icon and short name and save for deduping
            const site = { url };
            tippyTop.processSite(site, "*");
            return JSON.stringify({
              icon: site.tippyTopIcon,
              label: shortURL(site),
            });
          })
        ),
      ]}]`;
    })())
  );
}

async function getDefaultSites(child) {
  // Get default TopSites by region
  let sites = DEFAULT_SITES.get(
    DEFAULT_SITES.has(searchRegion) ? searchRegion : ""
  );

  // Use tippy top to get packaged rich icons
  const tippyTop = await tippyTopProvider;
  let defaultSites = sites.split(",").map(link => {
    let site = { url: link };
    tippyTop.processSite(site);
    return {
      icon: site.tippyTopIcon,
      title: shortURL(site),
    };
  });
  return Cu.cloneInto(defaultSites, child.contentWindow);
}

async function getSelectedTheme(child) {
  let activeThemeId = await child.sendQuery("AWPage:GET_SELECTED_THEME");
  return activeThemeId;
}

class AboutWelcomeChild extends JSWindowActorChild {
  actorCreated() {
    this.exportFunctions();
    this.initWebProgressListener();
  }

  initWebProgressListener() {
    const webProgress = this.manager.browsingContext.top.docShell
      .QueryInterface(Ci.nsIInterfaceRequestor)
      .getInterface(Ci.nsIWebProgress);

    const listener = {
      QueryInterface: ChromeUtils.generateQI([
        "nsIWebProgressListener",
        "nsISupportsWeakReference",
      ]),
    };

    listener.onLocationChange = (aWebProgress, aRequest, aLocation, aFlags) => {
      // Exit if actor 'AboutWelcome' has already been destroyed or
      // content window doesn't exist
      if (!this.manager || !this.contentWindow) {
        return;
      }
      log.debug(`onLocationChange handled: ${aWebProgress.DOMWindow}`);
      this.AWSendToParent("LOCATION_CHANGED");
    };

    webProgress.addProgressListener(
      listener,
      Ci.nsIWebProgress.NOTIFY_LOCATION
    );
  }

  /**
   * Send event that can be handled by the page
   * @param {{type: string, data?: any}} action
   */
  sendToPage(action) {
    log.debug(`Sending to page: ${action.type}`);
    const win = this.document.defaultView;
    const event = new win.CustomEvent("AboutWelcomeChromeToContent", {
      detail: Cu.cloneInto(action, win),
    });
    win.dispatchEvent(event);
  }

  /**
   * Export functions that can be called by page js
   */
  exportFunctions() {
    let window = this.contentWindow;

    Cu.exportFunction(this.AWGetFeatureConfig.bind(this), window, {
      defineAs: "AWGetFeatureConfig",
    });

    Cu.exportFunction(this.AWGetAttributionData.bind(this), window, {
      defineAs: "AWGetAttributionData",
    });

    Cu.exportFunction(this.AWGetFxAMetricsFlowURI.bind(this), window, {
      defineAs: "AWGetFxAMetricsFlowURI",
    });

    Cu.exportFunction(this.AWGetImportableSites.bind(this), window, {
      defineAs: "AWGetImportableSites",
    });

    Cu.exportFunction(this.AWGetDefaultSites.bind(this), window, {
      defineAs: "AWGetDefaultSites",
    });

    Cu.exportFunction(this.AWGetSelectedTheme.bind(this), window, {
      defineAs: "AWGetSelectedTheme",
    });

    Cu.exportFunction(this.AWGetRegion.bind(this), window, {
      defineAs: "AWGetRegion",
    });

    Cu.exportFunction(this.AWIsDefaultBrowser.bind(this), window, {
      defineAs: "AWIsDefaultBrowser",
    });

    Cu.exportFunction(this.AWSelectTheme.bind(this), window, {
      defineAs: "AWSelectTheme",
    });

    Cu.exportFunction(this.AWSendEventTelemetry.bind(this), window, {
      defineAs: "AWSendEventTelemetry",
    });

    Cu.exportFunction(this.AWSendToParent.bind(this), window, {
      defineAs: "AWSendToParent",
    });

    Cu.exportFunction(this.AWWaitForMigrationClose.bind(this), window, {
      defineAs: "AWWaitForMigrationClose",
    });
  }

  /**
   * Wrap a promise so content can use Promise methods.
   */
  wrapPromise(promise) {
    return new this.contentWindow.Promise((resolve, reject) =>
      promise.then(resolve, reject)
    );
  }

  AWSelectTheme(data) {
    return this.wrapPromise(
      this.sendQuery("AWPage:SELECT_THEME", data.toUpperCase())
    );
  }

  async getAddonInfo(attrbObj) {
    let { content, source } = attrbObj;
    try {
      if (!content || source !== "addons.mozilla.org") {
        return null;
      }
      // Attribution data can be double encoded
      while (content.includes("%")) {
        try {
          const result = decodeURIComponent(content);
          if (result === content) {
            break;
          }
          content = result;
        } catch (e) {
          break;
        }
      }
      return await this.sendQuery("AWPage:GET_ADDON_FROM_REPOSITORY", content);
    } catch (e) {
      Cu.reportError(
        "Failed to get the latest add-on version for Return to AMO"
      );
      return null;
    }
  }

  hasAMOAttribution(attributionData) {
    return (
      attributionData &&
      attributionData.campaign === "non-fx-button" &&
      attributionData.source === "addons.mozilla.org"
    );
  }

  async formatAttributionData(attribution) {
    let result = {};
    if (this.hasAMOAttribution(attribution)) {
      let extraProps = await this.getAddonInfo(attribution);
      if (extraProps) {
        result = {
          template: "return_to_amo",
          extraProps,
        };
      }
    }
    return result;
  }

  async getAttributionData() {
    return Cu.cloneInto(
      await this.formatAttributionData(
        await this.sendQuery("AWPage:GET_ATTRIBUTION_DATA")
      ),
      this.contentWindow
    );
  }

  AWGetAttributionData() {
    return this.wrapPromise(this.getAttributionData());
  }

  /**
   * Send initial data to page including experiment information
   */
  AWGetFeatureConfig() {
    // Note that we specifically don't wait for `ready` so if
    // about:welcome loads outside of the "FirstStartup" scenario this will likely not be ready
    let experimentMetadata =
      ExperimentAPI.getExperimentMetaData({
        featureId: "aboutwelcome",
      }) || {};
    let featureConfig = aboutWelcomeFeature.getValue({ defaultValue: {} });

    if (experimentMetadata?.slug) {
      log.debug(
        `Loading about:welcome with experiment: ${experimentMetadata.slug}`
      );
    } else {
      log.debug("Loading about:welcome without experiment");
    }
    return Cu.cloneInto(
      {
        // All experimentation right now is using the multistage template
        template: "multistage",
        ...experimentMetadata,
        ...featureConfig,
      },
      this.contentWindow
    );
  }

  AWGetFxAMetricsFlowURI() {
    return this.wrapPromise(this.sendQuery("AWPage:FXA_METRICS_FLOW_URI"));
  }

  AWGetImportableSites() {
    return this.wrapPromise(getImportableSites(this));
  }

  AWGetDefaultSites() {
    return this.wrapPromise(getDefaultSites(this));
  }

  AWGetSelectedTheme() {
    return this.wrapPromise(getSelectedTheme(this));
  }

  AWIsDefaultBrowser() {
    return this.wrapPromise(this.sendQuery("AWPage:IS_DEFAULT_BROWSER"));
  }

  /**
   * Send Event Telemetry
   * @param {object} eventData
   */
  AWSendEventTelemetry(eventData) {
    this.AWSendToParent("TELEMETRY_EVENT", {
      ...eventData,
      event_context: {
        ...eventData.event_context,
        page: "about:welcome",
      },
    });
  }

  /**
   * Send message that can be handled by AboutWelcomeParent.jsm
   * @param {string} type
   * @param {any=} data
   */
  AWSendToParent(type, data) {
    this.sendAsyncMessage(`AWPage:${type}`, data);
  }

  AWWaitForMigrationClose() {
    return this.wrapPromise(this.sendQuery("AWPage:WAIT_FOR_MIGRATION_CLOSE"));
  }

  AWGetRegion() {
    return this.wrapPromise(this.sendQuery("AWPage:GET_REGION"));
  }

  /**
   * @param {{type: string, detail?: any}} event
   * @override
   */
  handleEvent(event) {
    log.debug(`Received page event ${event.type}`);
  }
}
