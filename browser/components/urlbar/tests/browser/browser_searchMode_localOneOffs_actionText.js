/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

/**
 * Tests action text shown on heuristic and search suggestions when keyboard
 * navigating local one-off buttons.
 */

"use strict";

const DEFAULT_ENGINE_NAME = "Test";
const SUGGESTIONS_ENGINE_NAME = "searchSuggestionEngine.xml";

let engine;

add_task(async function setup() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.urlbar.suggest.searches", true],
      ["browser.urlbar.update2", true],
      ["browser.urlbar.update2.oneOffsRefresh", true],
      ["browser.urlbar.update2.localOneOffs", true],
    ],
  });
  engine = await SearchTestUtils.promiseNewSearchEngine(
    getRootDirectory(gTestPath) + SUGGESTIONS_ENGINE_NAME
  );
  let oldDefaultEngine = await Services.search.getDefault();
  await Services.search.setDefault(engine);
  await Services.search.moveEngine(engine, 0);

  await PlacesUtils.history.clear();

  registerCleanupFunction(async () => {
    await Services.search.setDefault(oldDefaultEngine);
    await PlacesUtils.history.clear();
  });
});

add_task(async function localOneOff() {
  info("Type some text, select a local one-off, check heuristic action");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "query",
  });
  Assert.ok(UrlbarTestUtils.getResultCount(window) > 1, "Sanity check results");

  info("Alt UP to select the first local one-off.");
  EventUtils.synthesizeKey("KEY_ArrowUp", { altKey: true });
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    0,
    "the heuristic result should be selected"
  );
  let result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  let oneOffButtons = UrlbarTestUtils.getOneOffSearchButtons(window);
  Assert.equal(
    oneOffButtons.selectedButton.source,
    UrlbarUtils.RESULT_SOURCE.HISTORY,
    "A local one-off button should be selected"
  );
  Assert.ok(
    BrowserTestUtils.is_visible(result.element.action),
    "The heuristic action should be visible"
  );
  let [actionHistory, actionBookmarks] = await document.l10n.formatValues([
    { id: "urlbar-result-action-search-history" },
    { id: "urlbar-result-action-search-bookmarks" },
  ]);
  Assert.equal(
    result.displayed.action,
    actionHistory,
    "Check the heuristic action"
  );
  Assert.equal(
    result.image,
    "chrome://browser/skin/history.svg",
    "Check the heuristic icon"
  );

  info("Move to an engine one-off and check heuristic action");
  EventUtils.synthesizeKey("KEY_ArrowUp", { altKey: true });
  EventUtils.synthesizeKey("KEY_ArrowUp", { altKey: true });
  EventUtils.synthesizeKey("KEY_ArrowUp", { altKey: true });
  Assert.ok(
    oneOffButtons.selectedButton.engine,
    "A one-off search button should be selected"
  );
  result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.ok(
    BrowserTestUtils.is_visible(result.element.action),
    "The heuristic action should be visible"
  );
  Assert.ok(
    result.displayed.action.includes(oneOffButtons.selectedButton.engine.name),
    "Check the heuristic action"
  );
  Assert.equal(
    result.image,
    oneOffButtons.selectedButton.engine.iconURI.spec,
    "Check the heuristic icon"
  );

  info("Move again to a local one-off, deselect and reselect the heuristic");
  EventUtils.synthesizeKey("KEY_ArrowDown", { altKey: true });
  result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.equal(
    oneOffButtons.selectedButton.source,
    UrlbarUtils.RESULT_SOURCE.BOOKMARKS,
    "A local one-off button should be selected"
  );
  Assert.equal(
    result.displayed.action,
    actionBookmarks,
    "Check the heuristic action"
  );
  Assert.equal(
    result.image,
    "chrome://browser/skin/bookmark.svg",
    "Check the heuristic icon"
  );

  info(
    "Select the next result, then reselect the heuristic, check it's a search with the default engine"
  );
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    0,
    "the heuristic result should be selected"
  );
  EventUtils.synthesizeKey("KEY_ArrowDown", {});
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    1,
    "the heuristic result should not be selected"
  );
  EventUtils.synthesizeKey("KEY_ArrowUp", {});
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    0,
    "the heuristic result should be selected"
  );
  result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.equal(result.type, UrlbarUtils.RESULT_TYPE.SEARCH);
  Assert.equal(result.searchParams.engine, engine.name);
  Assert.ok(
    result.displayed.action.includes(engine.name),
    "Check the heuristic action"
  );
  Assert.equal(
    result.image,
    "chrome://browser/skin/search-glass.svg",
    "Check the heuristic icon"
  );
});

add_task(async function localOneOff_withVisit() {
  info("Type a url, select a local one-off, check heuristic action");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "mozilla.org",
  });
  Assert.ok(UrlbarTestUtils.getResultCount(window) > 1, "Sanity check results");
  let oneOffButtons = UrlbarTestUtils.getOneOffSearchButtons(window);

  info("Alt UP to select the first local one-off.");
  EventUtils.synthesizeKey("KEY_ArrowUp", { altKey: true });
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    0,
    "the heuristic result should be selected"
  );
  let result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.equal(
    oneOffButtons.selectedButton.source,
    UrlbarUtils.RESULT_SOURCE.HISTORY,
    "A local one-off button should be selected"
  );
  Assert.ok(
    BrowserTestUtils.is_visible(result.element.action),
    "The heuristic action should be visible"
  );
  let [actionHistory, actionVisit] = await document.l10n.formatValues([
    { id: "urlbar-result-action-search-history" },
    { id: "urlbar-result-action-visit" },
  ]);
  Assert.equal(
    result.displayed.action,
    actionHistory,
    "Check the heuristic action"
  );
  Assert.equal(
    result.image,
    "chrome://browser/skin/history.svg",
    "Check the heuristic icon"
  );

  info(
    "Select the next result, then reselect the heuristic, check it's a visit"
  );
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    0,
    "the heuristic result should be selected"
  );
  EventUtils.synthesizeKey("KEY_ArrowDown", {});
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    1,
    "the heuristic result should not be selected"
  );
  EventUtils.synthesizeKey("KEY_ArrowUp", {});
  Assert.equal(
    UrlbarTestUtils.getSelectedRowIndex(window),
    0,
    "the heuristic result should be selected"
  );
  result = await UrlbarTestUtils.getDetailsOfResultAt(window, 0);
  Assert.equal(
    oneOffButtons.selectedButton,
    null,
    "No one-off button should be selected"
  );
  Assert.equal(result.type, UrlbarUtils.RESULT_TYPE.URL);
  Assert.equal(
    result.displayed.action,
    actionVisit,
    "Check the heuristic action"
  );
  Assert.notEqual(
    result.image,
    "chrome://browser/skin/history.svg",
    "Check the heuristic icon"
  );
});

add_task(async function localOneOff_suggestion() {
  info("Type some text, select the first suggestion, then a local one-off");
  await UrlbarTestUtils.promiseAutocompleteResultPopup({
    window,
    value: "query",
  });
  Assert.ok(UrlbarTestUtils.getResultCount(window) > 1, "Sanity check results");
  let count = await UrlbarTestUtils.getResultCount(window);
  let result = null;
  let suggestionIndex = -1;
  for (let i = 1; i < count; ++i) {
    EventUtils.synthesizeKey("KEY_ArrowDown");
    let index = await UrlbarTestUtils.getSelectedRowIndex(window);
    result = await UrlbarTestUtils.getDetailsOfResultAt(window, index);
    if (
      result.type == UrlbarUtils.RESULT_TYPE.SEARCH &&
      result.searchParams.suggestion
    ) {
      suggestionIndex = i;
      break;
    }
  }
  Assert.ok(
    result.searchParams.suggestion,
    "Should have selected a search suggestion"
  );
  Assert.ok(
    result.displayed.action.includes(result.searchParams.engine),
    "Check the search suggestion action"
  );

  info("Alt UP to select the first local one-off.");
  EventUtils.synthesizeKey("KEY_ArrowUp", { altKey: true });
  Assert.equal(
    await UrlbarTestUtils.getSelectedRowIndex(window),
    suggestionIndex,
    "the suggestion should still be selected"
  );

  let [actionHistory] = await document.l10n.formatValues([
    { id: "urlbar-result-action-search-history" },
  ]);
  result = await UrlbarTestUtils.getDetailsOfResultAt(window, suggestionIndex);
  Assert.equal(
    result.displayed.action,
    actionHistory,
    "Check the search suggestion action changed to local one-off"
  );
  // Like in the normal engine one-offs case, we don't replace the favicon.
  Assert.equal(
    result.image,
    "chrome://browser/skin/search-glass.svg",
    "Check the suggestion icon"
  );

  info("DOWN to select the next suggestion");
  EventUtils.synthesizeKey("KEY_ArrowDown");
  result = await UrlbarTestUtils.getDetailsOfResultAt(
    window,
    suggestionIndex + 1
  );
  Assert.ok(
    result.searchParams.suggestion,
    "Should have selected a search suggestion"
  );
  Assert.ok(
    result.displayed.action.includes(result.searchParams.engine),
    "Check the search suggestion action"
  );
  Assert.equal(
    result.image,
    "chrome://browser/skin/search-glass.svg",
    "Check the suggestion icon"
  );

  info("UP back to the previous suggestion");
  EventUtils.synthesizeKey("KEY_ArrowUp");
  result = await UrlbarTestUtils.getDetailsOfResultAt(window, suggestionIndex);
  Assert.ok(
    result.displayed.action.includes(result.searchParams.engine),
    "Check the search suggestion action"
  );
  Assert.equal(
    result.image,
    "chrome://browser/skin/search-glass.svg",
    "Check the suggestion icon"
  );
});
