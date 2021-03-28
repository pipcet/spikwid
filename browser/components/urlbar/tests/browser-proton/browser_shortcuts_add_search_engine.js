/* Any copyright is dedicated to the Public Domain.
 * https://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Test adding engines through search shortcut buttons.
// A more complete coverage of the detection of engines is available in
// browser_add_search_engine.js

const { PromptTestUtils } = ChromeUtils.import(
  "resource://testing-common/PromptTestUtils.jsm"
);
const BASE_URL =
  "http://mochi.test:8888/browser/browser/components/urlbar/tests/browser-proton/";

add_task(async function setup() {
  await SpecialPowers.pushPrefEnv({
    set: [["browser.urlbar.suggest.searches", false]],
  });
  // Ensure initial state.
  UrlbarTestUtils.getOneOffSearchButtons(window).invalidateCache();
});

add_task(async function shortcuts_none() {
  info("Checks the shortcuts with a page that doesn't offer any engines.");
  let url = "http://mochi.test:8888/";
  await BrowserTestUtils.withNewTab(url, async () => {
    let shortcutButtons = UrlbarTestUtils.getOneOffSearchButtons(window);
    let rebuildPromise = BrowserTestUtils.waitForEvent(
      shortcutButtons,
      "rebuild"
    );
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: "test",
    });
    await rebuildPromise;

    Assert.ok(
      !Array.from(shortcutButtons.buttons.children).some(b => !!b.webEngine),
      "Check there's no buttons to add engines"
    );
  });
});

add_task(async function shortcuts_two() {
  info("Checks the shortcuts with a page that offers two engines.");
  let url = getRootDirectory(gTestPath) + "add_search_engine_two.html";
  await BrowserTestUtils.withNewTab(url, async () => {
    let shortcutButtons = UrlbarTestUtils.getOneOffSearchButtons(window);
    let rebuildPromise = BrowserTestUtils.waitForEvent(
      shortcutButtons,
      "rebuild"
    );
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: "test",
    });
    await rebuildPromise;

    let addEngineButtons = Array.from(shortcutButtons.buttons.children).filter(
      b => !!b.webEngine
    );
    Assert.equal(
      addEngineButtons.length,
      2,
      "Check there's two buttons to add engines"
    );

    for (let button of addEngineButtons) {
      Assert.ok(BrowserTestUtils.is_visible(button));
      Assert.ok(button.hasAttribute("image"));
      Assert.ok(
        button.getAttribute("tooltiptext").includes("add_search_engine_")
      );
      Assert.ok(button.webEngine.name.startsWith("add_search_engine_"));
      Assert.ok(
        button.classList.contains("searchbar-engine-one-off-add-engine")
      );
    }

    info("Click on the first button");
    rebuildPromise = BrowserTestUtils.waitForEvent(shortcutButtons, "rebuild");
    let enginePromise = promiseEngine("engine-added", "add_search_engine_0");
    EventUtils.synthesizeMouseAtCenter(addEngineButtons[0], {});
    info("await engine install");
    let engine = await enginePromise;
    info("await rebuild");
    await rebuildPromise;

    Assert.ok(
      UrlbarTestUtils.isPopupOpen(window),
      "Urlbar view is still open."
    );

    addEngineButtons = Array.from(shortcutButtons.buttons.children).filter(
      b => !!b.webEngine
    );
    Assert.equal(
      addEngineButtons.length,
      1,
      "Check there's one button to add engines"
    );
    Assert.equal(addEngineButtons[0].webEngine.name, "add_search_engine_1");
    let installedEngineButton = addEngineButtons[0].previousElementSibling;
    Assert.equal(installedEngineButton.engine.name, "add_search_engine_0");

    info("Remove the added engine");
    rebuildPromise = BrowserTestUtils.waitForEvent(shortcutButtons, "rebuild");
    await Services.search.removeEngine(engine);
    await rebuildPromise;
    Assert.equal(
      Array.from(shortcutButtons.buttons.children).filter(b => !!b.webEngine)
        .length,
      2,
      "Check there's two buttons to add engines"
    );
    await UrlbarTestUtils.promisePopupClose(window);

    info("Switch to a new tab and check the buttons are not persisted");
    await BrowserTestUtils.withNewTab("about:robots", async () => {
      rebuildPromise = BrowserTestUtils.waitForEvent(
        shortcutButtons,
        "rebuild"
      );
      await UrlbarTestUtils.promiseAutocompleteResultPopup({
        window,
        value: "test",
      });
      await rebuildPromise;
      Assert.ok(
        !Array.from(shortcutButtons.buttons.children).some(b => !!b.webEngine),
        "Check there's no option to add engines"
      );
    });
  });
});

add_task(async function shortcuts_many() {
  info("Checks the shortcuts with a page that offers many engines.");
  let url = getRootDirectory(gTestPath) + "add_search_engine_many.html";
  await BrowserTestUtils.withNewTab(url, async () => {
    let shortcutButtons = UrlbarTestUtils.getOneOffSearchButtons(window);
    let rebuildPromise = BrowserTestUtils.waitForEvent(
      shortcutButtons,
      "rebuild"
    );
    await UrlbarTestUtils.promiseAutocompleteResultPopup({
      window,
      value: "test",
    });
    await rebuildPromise;

    let addEngineButtons = Array.from(shortcutButtons.buttons.children).filter(
      b => !!b.webEngine
    );
    Assert.equal(
      addEngineButtons.length,
      gURLBar.addSearchEngineHelper.maxInlineEngines,
      "Check there's a maximum of `maxInlineEngines` buttons to add engines"
    );
  });
});

function promiseEngine(expectedData, expectedEngineName) {
  info(`Waiting for engine ${expectedData}`);
  return TestUtils.topicObserved(
    "browser-search-engine-modified",
    (engine, data) => {
      info(`Got engine ${engine.wrappedJSObject.name} ${data}`);
      return (
        expectedData == data &&
        expectedEngineName == engine.wrappedJSObject.name
      );
    }
  ).then(([engine, data]) => engine);
}
