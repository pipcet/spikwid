/**
 * Bug 1641270 - A test case for ensuring the save channel will use the correct
 *               cookieJarSettings when doing the saving and the cache would
 *               work as expected.
 */

"use strict";

/* import-globals-from ../../../../content/tests/browser/common/mockTransfer.js */
Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/toolkit/content/tests/browser/common/mockTransfer.js",
  this
);

const TEST_IMAGE_URL = TEST_DOMAIN + TEST_PATH + "file_saveAsImage.sjs";

let MockFilePicker = SpecialPowers.MockFilePicker;
MockFilePicker.init(window);

const tempDir = createTemporarySaveDirectory();
MockFilePicker.displayDirectory = tempDir;

function createTemporarySaveDirectory() {
  let saveDir = Services.dirsvc.get("TmpD", Ci.nsIFile);
  saveDir.append("testsavedir");
  saveDir.createUnique(Ci.nsIFile.DIRECTORY_TYPE, 0o755);
  return saveDir;
}

function createPromiseForTransferComplete() {
  return new Promise(resolve => {
    MockFilePicker.showCallback = fp => {
      info("MockFilePicker showCallback");

      let fileName = fp.defaultString;
      let destFile = tempDir.clone();
      destFile.append(fileName);

      MockFilePicker.setFiles([destFile]);
      MockFilePicker.filterIndex = 0; // kSaveAsType_Complete

      MockFilePicker.showCallback = null;
      mockTransferCallback = function(downloadSuccess) {
        ok(downloadSuccess, "File should have been downloaded successfully");
        mockTransferCallback = () => {};
        resolve();
      };
    };
  });
}

function createPromiseForObservingChannel(aURL, aPartitionKey) {
  return new Promise(resolve => {
    let observer = (aSubject, aTopic) => {
      if (aTopic === "http-on-modify-request") {
        let httpChannel = aSubject.QueryInterface(Ci.nsIHttpChannel);
        let reqLoadInfo = httpChannel.loadInfo;

        // Make sure this is the request which we want to check.
        if (!httpChannel.URI.spec.endsWith(aURL)) {
          return;
        }

        info(`Checking loadInfo for URI: ${httpChannel.URI.spec}\n`);
        is(
          reqLoadInfo.cookieJarSettings.partitionKey,
          aPartitionKey,
          "The loadInfo has the correct partition key"
        );

        Services.obs.removeObserver(observer, "http-on-modify-request");
        resolve();
      }
    };

    Services.obs.addObserver(observer, "http-on-modify-request");
  });
}

add_task(async function setup() {
  info("Setting MockFilePicker.");
  mockTransferRegisterer.register();

  registerCleanupFunction(function() {
    mockTransferRegisterer.unregister();
    MockFilePicker.cleanup();
    tempDir.remove(true);
  });
});

add_task(async function testContextMenuSaveImage() {
  let uuidGenerator = Cc["@mozilla.org/uuid-generator;1"].getService(
    Ci.nsIUUIDGenerator
  );

  for (let networkIsolation of [true, false]) {
    for (let partitionPerSite of [true, false]) {
      await SpecialPowers.pushPrefEnv({
        set: [
          ["privacy.partition.network_state", networkIsolation],
          ["privacy.dynamic_firstparty.use_site", partitionPerSite],
        ],
      });

      // We use token to separate the caches.
      let token = uuidGenerator.generateUUID().toString();
      const testImageURL = `${TEST_IMAGE_URL}?token=${token}`;

      info(`Open a new tab for testing "Save image as" in context menu.`);
      let tab = await BrowserTestUtils.openNewForegroundTab(
        gBrowser,
        TEST_TOP_PAGE
      );

      info(`Insert the testing image into the tab.`);
      await SpecialPowers.spawn(
        tab.linkedBrowser,
        [testImageURL],
        async url => {
          let img = content.document.createElement("img");
          let loaded = new content.Promise(resolve => {
            img.onload = resolve;
          });
          content.document.body.appendChild(img);
          img.setAttribute("id", "image1");
          img.src = url;
          await loaded;
        }
      );

      info("Open the context menu.");
      let popupShownPromise = BrowserTestUtils.waitForEvent(
        document,
        "popupshown"
      );

      await BrowserTestUtils.synthesizeMouseAtCenter(
        "#image1",
        {
          type: "contextmenu",
          button: 2,
        },
        tab.linkedBrowser
      );

      await popupShownPromise;

      let partitionKey = partitionPerSite
        ? "(http,example.net)"
        : "example.net";

      let transferCompletePromise = createPromiseForTransferComplete();
      let observerPromise = createPromiseForObservingChannel(
        testImageURL,
        partitionKey
      );

      let saveElement = document.getElementById("context-saveimage");
      info("Triggering the save process.");
      saveElement.doCommand();

      info("Waiting for the channel.");
      await observerPromise;

      info("Wait until the save is finished.");
      await transferCompletePromise;

      info("Close the context menu.");
      let contextMenu = document.getElementById("contentAreaContextMenu");
      let popupHiddenPromise = BrowserTestUtils.waitForEvent(
        contextMenu,
        "popuphidden"
      );
      contextMenu.hidePopup();
      await popupHiddenPromise;

      // Check if there will be only one network request. The another one should
      // be from cache.
      let res = await fetch(`${TEST_IMAGE_URL}?token=${token}&result`);
      let res_text = await res.text();
      is(res_text, "1", "The image should be loaded only once.");

      BrowserTestUtils.removeTab(tab);
    }
  }
});
