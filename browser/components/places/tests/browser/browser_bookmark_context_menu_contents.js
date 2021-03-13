/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

/**
 *  Test removing bookmarks from the Bookmarks Toolbar and Library.
 */
const BOOKMARKS_H2_2020_PREF = "browser.toolbars.bookmarks.2h2020";
const bookmarksInfo = [
  {
    title: "firefox",
    url: "http://example.com",
  },
  {
    title: "rules",
    url: "http://example.com/2",
  },
  {
    title: "yo",
    url: "http://example.com/2",
  },
];
const TEST_URL = "about:mozilla";

add_task(async function setup() {
  await PlacesUtils.bookmarks.eraseEverything();

  let toolbar = document.getElementById("PersonalToolbar");
  let wasCollapsed = toolbar.collapsed;

  // Uncollapse the personal toolbar if needed.
  if (wasCollapsed) {
    await promiseSetToolbarVisibility(toolbar, true);
  }

  registerCleanupFunction(async () => {
    // Collapse the personal toolbar if needed.
    if (wasCollapsed) {
      await promiseSetToolbarVisibility(toolbar, false);
    }
    await PlacesUtils.bookmarks.eraseEverything();
  });
});

let OptionItemExists = elementId => {
  let optionItem = document.getElementById(elementId);

  Assert.ok(optionItem, "Context menu contains the appropriate option");
  Assert.ok(
    BrowserTestUtils.is_visible(optionItem),
    "Context menu option is visible"
  );
};

let OptionsMatchExpected = (contextMenu, expectedOptionItems) => {
  let idList = [];
  for (let elem of contextMenu.children) {
    if (
      BrowserTestUtils.is_visible(elem) &&
      elem.localName !== "menuseparator"
    ) {
      idList.push(elem.id);
    }
  }

  Assert.deepEqual(
    idList.sort(),
    expectedOptionItems.sort(),
    "Content is the same across both lists"
  );
};

let checkContextMenu = async (cbfunc, optionItems) => {
  await SpecialPowers.pushPrefEnv({
    set: [[BOOKMARKS_H2_2020_PREF, true]],
  });
  await PlacesUtils.bookmarks.insert({
    parentGuid: PlacesUtils.bookmarks.toolbarGuid,
    title: "Second Bookmark Title",
    url: TEST_URL,
  });

  await PlacesUtils.bookmarks.insertTree({
    guid: PlacesUtils.bookmarks.unfiledGuid,
    children: bookmarksInfo,
  });

  let contextMenu = await cbfunc();

  for (let item of optionItems) {
    OptionItemExists(item);
  }

  OptionsMatchExpected(contextMenu, optionItems);
  contextMenu.hidePopup();
  await PlacesUtils.bookmarks.eraseEverything();
};

add_task(async function test_bookmark_contextmenu_contents() {
  let optionItems = [
    "placesContext_open:newtab",
    "placesContext_open:newwindow",
    "placesContext_open:newprivatewindow",
    "placesContext_show_bookmark:info",
    "placesContext_deleteBookmark",
    "placesContext_cut",
    "placesContext_copy",
    "placesContext_paste_group",
    "placesContext_new:bookmark",
    "placesContext_new:folder",
    "placesContext_new:separator",
    "placesContext_showAllBookmarks",
    "toggle_PersonalToolbar",
    "show-other-bookmarks_PersonalToolbar",
  ];

  await checkContextMenu(async function() {
    let toolbarBookmark = await PlacesUtils.bookmarks.insert({
      parentGuid: PlacesUtils.bookmarks.toolbarGuid,
      title: "Bookmark Title",
      url: TEST_URL,
    });

    let toolbarNode = getToolbarNodeForItemGuid(toolbarBookmark.guid);

    let contextMenu = document.getElementById("placesContext");
    let popupShownPromise = BrowserTestUtils.waitForEvent(
      contextMenu,
      "popupshown"
    );

    EventUtils.synthesizeMouseAtCenter(toolbarNode, {
      button: 2,
      type: "contextmenu",
    });
    await popupShownPromise;
    return contextMenu;
  }, optionItems);
});

add_task(async function test_empty_contextmenu_contents() {
  let optionItems = [
    "placesContext_openBookmarkContainer:tabs",
    "placesContext_new:bookmark",
    "placesContext_new:folder",
    "placesContext_new:separator",
    "placesContext_paste",
    "placesContext_showAllBookmarks",
    "toggle_PersonalToolbar",
    "show-other-bookmarks_PersonalToolbar",
  ];

  await checkContextMenu(async function() {
    let contextMenu = document.getElementById("placesContext");
    let toolbar = document.querySelector("#PlacesToolbarItems");
    let openToolbarContextMenuPromise = BrowserTestUtils.waitForPopupEvent(
      contextMenu,
      "shown"
    );

    // Use the end of the toolbar because the beginning (and even middle, on
    // some resolutions) might be occluded by the empty toolbar message, which
    // has a different context menu.
    let bounds = toolbar.getBoundingClientRect();
    EventUtils.synthesizeMouse(toolbar, bounds.width - 5, 5, {
      type: "contextmenu",
    });

    await openToolbarContextMenuPromise;
    return contextMenu;
  }, optionItems);
});

add_task(async function test_separator_contextmenu_contents() {
  let optionItems = [
    "placesContext_delete",
    "placesContext_cut",
    "placesContext_copy",
    "placesContext_paste_group",
    "placesContext_new:bookmark",
    "placesContext_new:folder",
    "placesContext_new:separator",
    "placesContext_showAllBookmarks",
    "toggle_PersonalToolbar",
    "show-other-bookmarks_PersonalToolbar",
  ];

  await checkContextMenu(async function() {
    let sep = await PlacesUtils.bookmarks.insert({
      type: PlacesUtils.bookmarks.TYPE_SEPARATOR,
      parentGuid: PlacesUtils.bookmarks.toolbarGuid,
    });

    let toolbarNode = getToolbarNodeForItemGuid(sep.guid);
    let contextMenu = document.getElementById("placesContext");
    let popupShownPromise = BrowserTestUtils.waitForEvent(
      contextMenu,
      "popupshown"
    );

    EventUtils.synthesizeMouseAtCenter(toolbarNode, {
      button: 2,
      type: "contextmenu",
    });
    await popupShownPromise;
    return contextMenu;
  }, optionItems);
});

add_task(async function test_folder_contextmenu_contents() {
  let optionItems = [
    "placesContext_openBookmarkContainer:tabs",
    "placesContext_show:info",
    "placesContext_delete",
    "placesContext_cut",
    "placesContext_copy",
    "placesContext_paste_group",
    "placesContext_new:bookmark",
    "placesContext_new:folder",
    "placesContext_new:separator",
    "placesContext_sortBy:name",
    "placesContext_showAllBookmarks",
    "toggle_PersonalToolbar",
    "show-other-bookmarks_PersonalToolbar",
  ];

  await checkContextMenu(async function() {
    let folder = await PlacesUtils.bookmarks.insert({
      type: PlacesUtils.bookmarks.TYPE_FOLDER,
      parentGuid: PlacesUtils.bookmarks.toolbarGuid,
    });

    let toolbarNode = getToolbarNodeForItemGuid(folder.guid);
    let contextMenu = document.getElementById("placesContext");
    let popupShownPromise = BrowserTestUtils.waitForEvent(
      contextMenu,
      "popupshown"
    );

    EventUtils.synthesizeMouseAtCenter(toolbarNode, {
      button: 2,
      type: "contextmenu",
    });
    await popupShownPromise;
    return contextMenu;
  }, optionItems);
});
