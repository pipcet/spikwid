"use strict";

const { ExperimentAPI, ExperimentFeature } = ChromeUtils.import(
  "resource://nimbus/ExperimentAPI.jsm"
);
const { ExperimentFakes } = ChromeUtils.import(
  "resource://testing-common/NimbusTestUtils.jsm"
);
const { TestUtils } = ChromeUtils.import(
  "resource://testing-common/TestUtils.jsm"
);

const { Ajv } = ChromeUtils.import("resource://testing-common/ajv-4.1.1.js");
const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
Cu.importGlobalProperties(["fetch"]);

XPCOMUtils.defineLazyGetter(this, "fetchSchema", async () => {
  const response = await fetch(
    "resource://testing-common/ExperimentFeatureManifest.schema.json"
  );
  const schema = await response.json();
  if (!schema) {
    throw new Error("Failed to load NimbusSchema");
  }
  return schema.definitions.Feature;
});

async function setupForExperimentFeature() {
  const sandbox = sinon.createSandbox();
  const manager = ExperimentFakes.manager();
  await manager.onStartup();

  sandbox.stub(ExperimentAPI, "_store").get(() => manager.store);

  return { sandbox, manager };
}

const TEST_FALLBACK_PREF = "testprefbranch.config";
const FAKE_FEATURE_MANIFEST = {
  enabledFallbackPref: "testprefbranch.enabled",
  variables: {
    config: {
      type: "json",
      fallbackPref: TEST_FALLBACK_PREF,
    },
  },
};

add_task(async function test_feature_manifest_is_valid() {
  const ajv = new Ajv({ allErrors: true });
  const validate = ajv.compile(await fetchSchema);

  // Validate each entry in the feature manifest.
  // See tookit/components/messaging-system/experiments/ExperimentAPI.jsm
  Object.keys(ExperimentFeature.MANIFEST).forEach(featureId => {
    const valid = validate(ExperimentFeature.MANIFEST[featureId]);
    if (!valid) {
      throw new Error(
        `The manfinifest entry for ${featureId} not valid in tookit/components/messaging-system/experiments/ExperimentAPI.jsm: ` +
          JSON.stringify(validate.errors, undefined, 2)
      );
    }
  });
});

/**
 * # ExperimentFeature.getValue
 */
add_task(async function test_ExperimentFeature_ready() {
  const { sandbox, manager } = await setupForExperimentFeature();

  const featureInstance = new ExperimentFeature("foo", FAKE_FEATURE_MANIFEST);

  const expected = ExperimentFakes.experiment("anexperiment", {
    branch: {
      slug: "treatment",
      feature: {
        featureId: "foo",
        enabled: true,
        value: { whoa: true },
      },
    },
  });

  manager.store.addExperiment(expected);

  await featureInstance.ready();

  Assert.deepEqual(
    featureInstance.getValue(),
    { whoa: true },
    "should return getValue after waiting on ready"
  );

  Services.prefs.clearUserPref("testprefbranch.value");
  sandbox.restore();
});

/**
 * # ExperimentFeature.getValue
 */
add_task(async function test_ExperimentFeature_getValue() {
  const { sandbox } = await setupForExperimentFeature();

  const featureInstance = new ExperimentFeature("foo", FAKE_FEATURE_MANIFEST);

  Services.prefs.clearUserPref("testprefbranch.value");

  Assert.deepEqual(
    featureInstance.getValue({ defaultValue: { hello: 1 } }),
    { hello: 1 },
    "should return the defaultValue if no fallback pref is set"
  );

  Services.prefs.setStringPref(TEST_FALLBACK_PREF, `{"bar": 123}`);

  Assert.deepEqual(
    featureInstance.getValue().config,
    { bar: 123 },
    "should return the fallback pref value"
  );

  Services.prefs.clearUserPref(TEST_FALLBACK_PREF);
  sandbox.restore();
});

add_task(
  async function test_ExperimentFeature_getValue_prefer_experiment_over_default() {
    const { sandbox, manager } = await setupForExperimentFeature();

    const featureInstance = new ExperimentFeature("foo", FAKE_FEATURE_MANIFEST);

    const expected = ExperimentFakes.experiment("anexperiment", {
      branch: {
        slug: "treatment",
        feature: {
          featureId: "foo",
          enabled: true,
          value: { whoa: true },
        },
      },
    });

    manager.store.addExperiment(expected);

    Services.prefs.setStringPref(TEST_FALLBACK_PREF, `{"bar": 123}`);

    Assert.deepEqual(
      featureInstance.getValue(),
      { whoa: true },
      "should return the experiment feature value, not the fallback one."
    );

    Services.prefs.clearUserPref("testprefbranch.value");
    sandbox.restore();
  }
);

/**
 * # ExperimentFeature.isEnabled
 */

add_task(async function test_ExperimentFeature_isEnabled_default() {
  const { sandbox } = await setupForExperimentFeature();

  const featureInstance = new ExperimentFeature("foo", FAKE_FEATURE_MANIFEST);

  const noPrefFeature = new ExperimentFeature("bar", {});

  Assert.equal(
    noPrefFeature.isEnabled(),
    null,
    "should return null if no default pref branch is configured"
  );

  Services.prefs.clearUserPref("testprefbranch.enabled");

  Assert.equal(
    featureInstance.isEnabled(),
    null,
    "should return null if no default value or pref is set"
  );

  Assert.equal(
    featureInstance.isEnabled({ defaultValue: false }),
    false,
    "should use the default value param if no pref is set"
  );

  Services.prefs.setBoolPref("testprefbranch.enabled", false);

  Assert.equal(
    featureInstance.isEnabled({ defaultValue: true }),
    false,
    "should use the default pref value, including if it is false"
  );

  Services.prefs.clearUserPref("testprefbranch.enabled");
  sandbox.restore();
});

add_task(
  async function test_ExperimentFeature_isEnabled_prefer_experiment_over_default() {
    const { sandbox, manager } = await setupForExperimentFeature();
    const expected = ExperimentFakes.experiment("foo", {
      branch: {
        slug: "treatment",
        feature: {
          featureId: "foo",
          enabled: true,
          value: null,
        },
      },
    });

    const featureInstance = new ExperimentFeature("foo", FAKE_FEATURE_MANIFEST);

    manager.store.addExperiment(expected);

    const exposureSpy = sandbox.spy(ExperimentAPI, "recordExposureEvent");
    Services.prefs.setBoolPref("testprefbranch.enabled", false);

    Assert.equal(
      featureInstance.isEnabled(),
      true,
      "should return the enabled value defined in the experiment, not the default pref"
    );

    Assert.ok(exposureSpy.notCalled, "should emit exposure by default event");

    featureInstance.isEnabled({ sendExposureEvent: true });

    Assert.ok(exposureSpy.calledOnce, "should emit exposure event");

    Services.prefs.clearUserPref("testprefbranch.enabled");
    sandbox.restore();
  }
);

add_task(async function test_ExperimentFeature_isEnabled_no_exposure() {
  const { sandbox, manager } = await setupForExperimentFeature();
  const expected = ExperimentFakes.experiment("blah", {
    branch: {
      slug: "treatment",
      feature: {
        featureId: "foo",
        enabled: false,
        value: null,
      },
    },
  });
  const featureInstance = new ExperimentFeature("foo", FAKE_FEATURE_MANIFEST);

  sandbox.stub(ExperimentAPI, "_store").get(() => manager.store);

  manager.store.addExperiment(expected);

  const exposureSpy = sandbox.spy(ExperimentAPI, "recordExposureEvent");

  const actual = featureInstance.isEnabled({ sendExposureEvent: false });

  Assert.deepEqual(actual, false, "should return feature as disabled");
  Assert.ok(
    exposureSpy.notCalled,
    "should not emit an exposure event when options = { sendExposureEvent: false}"
  );

  sandbox.restore();
});

add_task(async function test_record_exposure_event() {
  const { sandbox, manager } = await setupForExperimentFeature();

  const featureInstance = new ExperimentFeature("foo", FAKE_FEATURE_MANIFEST);
  const exposureSpy = sandbox.spy(ExperimentAPI, "recordExposureEvent");
  sandbox.stub(ExperimentAPI, "_store").get(() => manager.store);

  featureInstance.recordExposureEvent();

  Assert.ok(
    exposureSpy.notCalled,
    "should not emit an exposure event when no experiment is active"
  );

  manager.store.addExperiment(
    ExperimentFakes.experiment("blah", {
      featureIds: ["foo"],
      branch: {
        slug: "treatment",
        feature: {
          featureId: "foo",
          enabled: false,
          value: null,
        },
      },
    })
  );

  featureInstance.recordExposureEvent();

  Assert.ok(
    exposureSpy.calledOnce,
    "should emit an exposure event when there is an experiment"
  );

  sandbox.restore();
});
