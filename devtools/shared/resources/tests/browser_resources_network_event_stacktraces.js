/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Test the ResourceWatcher API around NETWORK_EVENT_STACKTRACE

const {
  ResourceWatcher,
} = require("devtools/shared/resources/resource-watcher");

const TEST_URI = `${URL_ROOT_SSL}network_document.html`;

const REQUEST_STUB = {
  code: `await fetch("/request_post_0.html", { method: "POST" });`,
  expected: {
    stacktraceAvailable: true,
    lastFrame: {
      filename:
        "https://example.com/browser/devtools/shared/resources/tests/network_document.html",
      lineNumber: 1,
      columnNumber: 40,
      functionName: "triggerRequest",
      asyncCause: null,
    },
  },
};

add_task(async function() {
  info("Test network stacktraces events");
  const tab = await addTab(TEST_URI);
  const { client, resourceWatcher, targetList } = await initResourceWatcher(
    tab
  );

  const networkEvents = new Map();
  const stackTraces = new Map();

  function onResourceAvailable(resources) {
    for (const resource of resources) {
      if (
        resource.resourceType === ResourceWatcher.TYPES.NETWORK_EVENT_STACKTRACE
      ) {
        ok(
          !networkEvents.has(resource.resourceId),
          "The network event does not exist"
        );

        is(
          resource.stacktraceAvailable,
          REQUEST_STUB.expected.stacktraceAvailable,
          "The stacktrace is available"
        );
        is(
          JSON.stringify(resource.lastFrame),
          JSON.stringify(REQUEST_STUB.expected.lastFrame),
          "The last frame of the stacktrace is available"
        );

        stackTraces.set(resource.resourceId, true);
        return;
      }

      if (resource.resourceType === ResourceWatcher.TYPES.NETWORK_EVENT) {
        ok(
          stackTraces.has(resource.stacktraceResourceId),
          "The stack trace does exists"
        );

        networkEvents.set(resource.resourceId, true);
      }
    }
  }

  function onResourceUpdated() {}

  await resourceWatcher.watchResources(
    [
      ResourceWatcher.TYPES.NETWORK_EVENT_STACKTRACE,
      ResourceWatcher.TYPES.NETWORK_EVENT,
    ],
    {
      onAvailable: onResourceAvailable,
      onUpdated: onResourceUpdated,
    }
  );

  await triggerNetworkRequests(tab.linkedBrowser, [REQUEST_STUB.code]);

  resourceWatcher.unwatchResources(
    [
      ResourceWatcher.TYPES.NETWORK_EVENT_STACKTRACE,
      ResourceWatcher.TYPES.NETWORK_EVENT,
    ],
    {
      onAvailable: onResourceAvailable,
      onUpdated: onResourceUpdated,
    }
  );

  targetList.destroy();
  await client.close();
  BrowserTestUtils.removeTab(tab);
});
