/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { _ExperimentManager } = ChromeUtils.import(
  "resource://messaging-system/experiments/ExperimentManager.jsm"
);
const { ExperimentStore } = ChromeUtils.import(
  "resource://messaging-system/experiments/ExperimentStore.jsm"
);
const { NormandyUtils } = ChromeUtils.import(
  "resource://normandy/lib/NormandyUtils.jsm"
);
const { FileTestUtils } = ChromeUtils.import(
  "resource://testing-common/FileTestUtils.jsm"
);
const PATH = FileTestUtils.getTempFile("shared-data-map").path;

const { _RemoteSettingsExperimentLoader } = ChromeUtils.import(
  "resource://messaging-system/lib/RemoteSettingsExperimentLoader.jsm"
);

const EXPORTED_SYMBOLS = ["ExperimentFakes"];

const ExperimentFakes = {
  manager(store) {
    return new _ExperimentManager({ store: store || this.store() });
  },
  store() {
    return new ExperimentStore("FakeStore", { path: PATH, isParent: true });
  },
  waitForExperimentUpdate(ExperimentAPI, slug) {
    if (!slug) {
      throw new Error("Must specify an expected recipe update");
    }

    return new Promise(resolve => ExperimentAPI.on(`update:${slug}`, resolve));
  },
  childStore() {
    return new ExperimentStore("FakeStore", { isParent: false });
  },
  rsLoader() {
    const loader = new _RemoteSettingsExperimentLoader();
    // Replace RS client with a fake
    Object.defineProperty(loader, "remoteSettingsClient", {
      value: { get: () => Promise.resolve([]) },
    });
    // Replace xman with a fake
    loader.manager = this.manager();

    return loader;
  },
  experiment(slug, props = {}) {
    return {
      slug,
      active: true,
      enrollmentId: NormandyUtils.generateUuid(),
      branch: {
        slug: "treatment",
        feature: {
          featureId: "aboutwelcome",
          enabled: true,
          value: { title: "hello" },
        },
        ...props,
      },
      source: "test",
      isEnrollmentPaused: true,
      ...props,
    };
  },
  recipe(slug, props = {}) {
    return {
      slug,
      branches: [
        {
          slug: "control",
          feature: { featureId: "aboutwelcome", enabled: true, value: null },
        },
        {
          slug: "treatment",
          feature: {
            featureId: "aboutwelcome",
            enabled: true,
            value: { title: "hello" },
          },
        },
      ],
      bucketConfig: {
        namespace: "mstest-utils",
        randomizationUnit: "normandy_id",
        start: 0,
        count: 100,
        total: 1000,
      },
      userFacingName: "Messaging System recipe",
      userFacingDescription: "Messaging System MSTestUtils recipe",
      ...props,
    };
  },
};
