/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

this.EXPORTED_SYMBOLS = ["GeckoViewScroll"];

const { classes: Cc, interfaces: Ci, utils: Cu, results: Cr } = Components;

Cu.import("resource://gre/modules/GeckoViewModule.jsm");

var dump = Cu.import("resource://gre/modules/AndroidLog.jsm", {})
           .AndroidLog.d.bind(null, "ViewScroll");

function debug(aMsg) {
  // dump(aMsg);
}

class GeckoViewScroll extends GeckoViewModule {
  init() {
    debug("init");

    this.frameScriptLoaded = false;
  }

  register() {
    debug("register");

    if (!this.frameScriptLoaded) {
      this.messageManager.loadFrameScript(
        "chrome://geckoview/content/GeckoViewScrollContent.js", true);
      this.frameScriptLoaded = true;
    }
  }
}
