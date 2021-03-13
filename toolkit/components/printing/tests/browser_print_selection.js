/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

const frameSource =
  "<a href='about:mozilla'>some text</a><a id='other' href='about:about'>other text</a>";
const sources = [
  `<html><iframe id="f" srcdoc="${frameSource}"></iframe></html>`,
  `<html><iframe id="f" src="https://example.com/document-builder.sjs?html=${frameSource}"></iframe></html>`,
];

async function getPreviewText(previewBrowser) {
  return SpecialPowers.spawn(previewBrowser, [], function() {
    return content.document.body.textContent;
  });
}

add_task(async function print_selection() {
  let i = 0;
  for (let source of sources) {
    // Testing the native print dialog is much harder.
    // Note we need to do this from here since resetPrintPrefs() below clears
    // out the pref.
    await SpecialPowers.pushPrefEnv({
      set: [["print.tab_modal.enabled", true]],
    });

    is(
      document.querySelector(".printPreviewBrowser"),
      null,
      "There shouldn't be any print preview browser"
    );

    await BrowserTestUtils.withNewTab(
      "data:text/html," + source,
      async function(browser) {
        let frameBC = browser.browsingContext.children[0];
        await SpecialPowers.spawn(frameBC, [], () => {
          let element = content.document.getElementById("other");
          content.focus();
          content.getSelection().selectAllChildren(element);
        });

        let helper = new PrintHelper(browser);

        // If you change this, change nsContextMenu.printSelection() too.
        PrintUtils.startPrintWindow(frameBC, {
          printSelectionOnly: true,
        });

        await BrowserTestUtils.waitForCondition(
          () => !!document.querySelector(".printPreviewBrowser")
        );

        let previewBrowser = document.querySelector(
          ".printPreviewBrowser[previewtype='selection']"
        );
        let previewText = () => getPreviewText(previewBrowser);
        // The preview process is async, wait for it to not be empty.
        let textContent = await TestUtils.waitForCondition(previewText);
        is(textContent, "other text", "Correct content loaded");

        let printSelect = document
          .querySelector(".printSettingsBrowser")
          .contentDocument.querySelector("#print-selection-enabled");
        ok(!printSelect.hidden, "Print selection checkbox is shown");
        ok(printSelect.checked, "Print selection checkbox is checked");

        let file = helper.mockFilePicker(`browser_print_selection-${i++}.pdf`);
        await helper.assertPrintToFile(file, () => {
          helper.click(helper.get("print-button"));
        });
        PrintHelper.resetPrintPrefs();
      }
    );
  }
});

add_task(async function no_print_selection() {
  // Ensures the print selection checkbox is hidden if nothing is selected
  await PrintHelper.withTestPage(async helper => {
    await helper.startPrint();
    await helper.openMoreSettings();

    let printSelect = helper.get("print-selection-container");
    ok(printSelect.hidden, "Print selection checkbox is hidden");
    await helper.closeDialog();
  });
});

add_task(async function print_selection_switch() {
  await PrintHelper.withTestPage(async helper => {
    await SpecialPowers.spawn(helper.sourceBrowser, [], async function() {
      let element = content.document.querySelector("h1");
      content.window.getSelection().selectAllChildren(element);
    });

    await helper.startPrint();
    await helper.openMoreSettings();
    let printSelect = helper.get("print-selection-container");
    ok(!printSelect.checked, "Print selection checkbox is not checked");

    let selectionBrowser = document.querySelector(
      ".printPreviewBrowser[previewtype='selection']"
    );
    let primaryBrowser = document.querySelector(
      ".printPreviewBrowser[previewtype='primary']"
    );

    let selectedText = "Article title";
    let fullText = await getPreviewText(primaryBrowser);

    function getCurrentBrowser(previewType) {
      let browser =
        previewType == "selection" ? selectionBrowser : primaryBrowser;
      is(
        browser.parentElement.getAttribute("previewtype"),
        previewType,
        "Expected browser is showing"
      );
      return browser;
    }

    helper.assertSettingsMatch({
      printSelectionOnly: false,
    });

    is(
      selectionBrowser.parentElement.getAttribute("previewtype"),
      "primary",
      "Print selection browser is not shown"
    );

    await helper.assertSettingsChanged(
      { printSelectionOnly: false },
      { printSelectionOnly: true },
      async () => {
        await helper.waitForPreview(() => helper.click(printSelect));
        let text = await getPreviewText(getCurrentBrowser("selection"));
        is(text, selectedText, "Correct content loaded");
      }
    );

    await helper.assertSettingsChanged(
      { printSelectionOnly: true },
      { printSelectionOnly: false },
      async () => {
        await helper.waitForPreview(() => helper.click(printSelect));
        let previewType = selectionBrowser.parentElement.getAttribute(
          "previewtype"
        );
        is(previewType, "primary", "Print selection browser is not shown");
        let text = await getPreviewText(getCurrentBrowser(previewType));
        is(text, fullText, "Correct content loaded");
      }
    );

    await helper.closeDialog();
  });
});
