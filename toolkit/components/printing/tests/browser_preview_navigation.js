/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

function compare10nArgs(elem, expectedValues) {
  let l10nArgs = elem.ownerDocument.l10n.getAttributes(elem).args;
  for (let [name, value] of Object.entries(expectedValues)) {
    if (value !== l10nArgs[name]) {
      info(
        `compare10nArgs, expected ${name}: ${value}, actual: ${l10nArgs[name]}`
      );
      return false;
    }
  }
  return true;
}

async function waitForPageStatusUpdate(elem, expected, message) {
  await TestUtils.waitForCondition(
    () => compare10nArgs(elem, expected),
    message
  );
}

async function waitUntilVisible(elem, visible = true) {
  await TestUtils.waitForCondition(
    () =>
      BrowserTestUtils.is_visible(elem) &&
      getComputedStyle(elem).opacity == "1",
    "Waiting for element to be visible and have opacity:1"
  );
}

async function waitUntilTransparent(elem) {
  // Note that is_visible considers a fully transparent element "visible"
  await TestUtils.waitForCondition(
    () => getComputedStyle(elem).opacity == "0",
    "Waiting for element to be have opacity:0"
  );
}

async function mouseMoveAndWait(elem) {
  let mouseMovePromise = BrowserTestUtils.waitForEvent(elem, "mousemove");
  EventUtils.synthesizeMouseAtCenter(elem, { type: "mousemove" });
  await mouseMovePromise;
  await TestUtils.waitForTick();
}

add_task(async function testToolbarVisibility() {
  // move the mouse to a known position
  await mouseMoveAndWait(gURLBar.textbox);

  await PrintHelper.withTestPage(async helper => {
    await helper.startPrint();

    let paginationElem = document.querySelector(".printPreviewNavigation");
    let previewStack = document.querySelector(".previewStack");

    // The toolbar has 0 opacity until we hover or focus it
    is(getComputedStyle(paginationElem).opacity, "0", "Initially transparent");

    let visiblePromise = waitUntilVisible(paginationElem);
    paginationElem.shadowRoot.querySelector("#navigateEnd").focus();
    await visiblePromise;
    is(
      getComputedStyle(paginationElem).opacity,
      "1",
      "Opaque with button focused"
    );

    await EventUtils.synthesizeKey("KEY_Tab", {});
    await waitUntilTransparent(paginationElem);
    is(getComputedStyle(paginationElem).opacity, "0", "Returns to transparent");

    visiblePromise = waitUntilVisible(paginationElem);
    info("Waiting for mousemove event, and for the toolbar to become opaque");
    await mouseMoveAndWait(previewStack);
    await visiblePromise;
    is(getComputedStyle(paginationElem).opacity, "1", "Opaque toolbar");

    // put the mouse back where it won't interfere with later tests
    await mouseMoveAndWait(gURLBar.textbox);
    await helper.closeDialog();
  });
});

add_task(async function testPreviewSheetCount() {
  await PrintHelper.withTestPage(async helper => {
    await helper.startPrint();

    let paginationElem = document.querySelector(".printPreviewNavigation");
    let paginationSheetIndicator = paginationElem.shadowRoot.querySelector(
      "#sheetIndicator"
    );

    // We have to wait for the first _updatePrintPreview to get the sheet count
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "Paginator indicates the correct number of sheets"
    );

    // then switch to page range 1-1 and verify page count changes
    await helper.dispatchSettingsChange({
      pageRanges: ["1", "1"],
    });
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 1 },
      "Indicates the updated number of sheets"
    );

    await helper.closeDialog();
  }, "longerArticle.html");
});

add_task(async function testPreviewScroll() {
  await PrintHelper.withTestPage(async helper => {
    await helper.startPrint();

    let paginationElem = document.querySelector(".printPreviewNavigation");
    let paginationSheetIndicator = paginationElem.shadowRoot.querySelector(
      "#sheetIndicator"
    );
    // Wait for the first _updatePrintPreview before interacting with the preview
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "Paginator indicates the correct number of sheets"
    );
    let previewBrowser = PrintUtils.getPreviewBrowser();

    // scroll down the document
    // and verify the indicator is updated correctly
    await SpecialPowers.spawn(previewBrowser, [], async function() {
      const { ContentTaskUtils } = ChromeUtils.import(
        "resource://testing-common/ContentTaskUtils.jsm"
      );
      const EventUtils = ContentTaskUtils.getEventUtils(content);
      content.focus();
      EventUtils.synthesizeKey("VK_PAGE_DOWN", {}, content);
    });
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 2, sheetCount: 3 },
      "Indicator updates on scroll"
    );

    // move focus before closing the dialog
    helper.get("cancel-button").focus();
    await helper.closeDialog();
  }, "longerArticle.html");
});

add_task(async function testPreviewNavigationCommands() {
  await PrintHelper.withTestPage(async helper => {
    await helper.startPrint();

    let paginationElem = document.querySelector(".printPreviewNavigation");
    let paginationSheetIndicator = paginationElem.shadowRoot.querySelector(
      "#sheetIndicator"
    );
    // Wait for the first _updatePrintPreview before interacting with the preview
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "Paginator indicates the correct number of sheets"
    );

    // click the navigation buttons
    // and verify the indicator is updated correctly
    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigateNext"),
      {}
    );
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 2, sheetCount: 3 },
      "Indicator updates on navigation"
    );

    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigatePrevious"),
      {}
    );
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "Indicator updates on navigation to previous"
    );

    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigateEnd"),
      {}
    );
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 3, sheetCount: 3 },
      "Indicator updates on navigation to end"
    );

    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigateHome"),
      {}
    );
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "Indicator updates on navigation to start"
    );

    // Test rapid clicks on the navigation buttons
    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigateNext"),
      {}
    );
    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigateNext"),
      {}
    );
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 3, sheetCount: 3 },
      "2 successive 'next' clicks correctly update the sheet indicator"
    );

    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigatePrevious"),
      {}
    );
    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigatePrevious"),
      {}
    );
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "2 successive 'previous' clicks correctly update the sheet indicator"
    );

    // move focus before closing the dialog
    helper.get("cancel-button").focus();
    await helper.closeDialog();
  }, "longerArticle.html");
});

add_task(async function testMultiplePreviewNavigation() {
  await PrintHelper.withTestPage(async helper => {
    await helper.startPrint();
    const tab1 = gBrowser.selectedTab;
    let sheetIndicator = document
      .querySelector(".printPreviewNavigation")
      .shadowRoot.querySelector("#sheetIndicator");

    await waitForPageStatusUpdate(
      sheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "Indicator has the correct initial sheetCount"
    );

    const tab2 = await BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      PrintHelper.defaultTestPageUrl
    );
    let helper2 = new PrintHelper(tab2.linkedBrowser);
    await helper2.startPrint();

    let [previewBrowser1, previewBrowser2] = document.querySelectorAll(
      ".printPreviewBrowser[previewtype='primary']"
    );
    ok(previewBrowser1 && previewBrowser2, "There are 2 preview browsers");

    let [toolbar1, toolbar2] = document.querySelectorAll(
      ".printPreviewNavigation"
    );
    ok(toolbar1 && toolbar2, "There are 2 preview navigation toolbars");
    is(
      toolbar1.previewBrowser,
      previewBrowser1,
      "toolbar1 has the correct previewBrowser"
    );
    sheetIndicator = toolbar1.shadowRoot.querySelector("#sheetIndicator");
    ok(
      compare10nArgs(sheetIndicator, { sheetNum: 1, sheetCount: 3 }),
      "First toolbar has the correct content"
    );

    is(
      toolbar2.previewBrowser,
      previewBrowser2,
      "toolbar2 has the correct previewBrowser"
    );
    sheetIndicator = toolbar2.shadowRoot.querySelector("#sheetIndicator");
    ok(
      compare10nArgs(sheetIndicator, { sheetNum: 1, sheetCount: 1 }),
      "2nd toolbar has the correct content"
    );

    // Switch back to the first tab and ensure the correct preview navigation is updated when clicked
    await BrowserTestUtils.switchTab(gBrowser, tab1);
    sheetIndicator = toolbar1.shadowRoot.querySelector("#sheetIndicator");

    EventUtils.synthesizeMouseAtCenter(
      toolbar1.shadowRoot.querySelector("#navigateNext"),
      {}
    );
    await waitForPageStatusUpdate(
      sheetIndicator,
      { sheetNum: 2, sheetCount: 3 },
      "Indicator updates on navigation"
    );

    gBrowser.removeTab(tab2);
  }, "longerArticle.html");
});

add_task(async function testPreviewNavigationSelection() {
  await PrintHelper.withTestPage(async helper => {
    await SpecialPowers.spawn(helper.sourceBrowser, [], async function() {
      let element = content.document.querySelector("#page-2");
      content.window.getSelection().selectAllChildren(element);
    });

    await helper.startPrint();

    let paginationElem = document.querySelector(".printPreviewNavigation");
    let paginationSheetIndicator = paginationElem.shadowRoot.querySelector(
      "#sheetIndicator"
    );
    // Wait for the first _updatePrintPreview before interacting with the preview
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "Paginator indicates the correct number of sheets"
    );

    // click a navigation button
    // and verify the indicator is updated correctly
    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigateNext"),
      {}
    );
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 2, sheetCount: 3 },
      "Indicator updates on navigation"
    );

    await helper.openMoreSettings();
    let printSelect = helper.get("print-selection-container");
    await helper.waitForPreview(() => helper.click(printSelect));

    // Wait for the first _updatePrintPreview before interacting with the preview
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 2 },
      "Paginator indicates the correct number of sheets"
    );

    // click a navigation button
    // and verify the indicator is updated correctly
    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigateNext"),
      {}
    );
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 2, sheetCount: 2 },
      "Indicator updates on navigation"
    );

    // move focus before closing the dialog
    helper.get("cancel-button").focus();
    await helper.closeDialog();
  }, "longerArticle.html");
});

add_task(async function testPaginatorAfterSettingsUpdate() {
  const mockPrinterName = "Fake Printer";
  await PrintHelper.withTestPage(async helper => {
    helper.addMockPrinter(mockPrinterName);
    await helper.startPrint();

    let paginationElem = document.querySelector(".printPreviewNavigation");
    let paginationSheetIndicator = paginationElem.shadowRoot.querySelector(
      "#sheetIndicator"
    );
    // Wait for the first _updatePrintPreview before interacting with the preview
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "Paginator indicates the correct number of sheets"
    );

    // click the navigation buttons
    // and verify the indicator is updated correctly
    EventUtils.synthesizeMouseAtCenter(
      paginationElem.shadowRoot.querySelector("#navigateNext"),
      {}
    );
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 2, sheetCount: 3 },
      "Indicator updates on navigation"
    );

    // Select a new printer
    await helper.dispatchSettingsChange({ printerName: mockPrinterName });
    await waitForPageStatusUpdate(
      paginationSheetIndicator,
      { sheetNum: 1, sheetCount: 3 },
      "Indicator updates on navigation"
    );
    ok(
      compare10nArgs(paginationSheetIndicator, { sheetNum: 1, sheetCount: 3 }),
      "Sheet indicator has correct value"
    );

    // move focus before closing the dialog
    helper.get("cancel-button").focus();
    await helper.closeDialog();
  }, "longerArticle.html");
});
