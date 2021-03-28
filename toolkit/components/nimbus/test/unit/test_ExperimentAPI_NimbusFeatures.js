"use strict";

const { ExperimentAPI, NimbusFeatures, ExperimentFeature } = ChromeUtils.import(
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
    "resource://testing-common/ExperimentFeatureRemote.schema.json"
  );
  const schema = await response.json();
  if (!schema) {
    throw new Error("Failed to load ExperimentFeatureRemote schema");
  }
  return schema.definitions.RemoteFeatureConfigurations;
});

const REMOTE_CONFIGURATION = Object.freeze({
  id: "aboutwelcome",
  description: "Content for the about:welcome page",
  configurations: [
    {
      variables: { skipFocus: false, remoteValue: false },
      enabled: false,
      targeting: "false",
    },
    {
      variables: { skipFocus: true, remoteValue: true },
      enabled: true,
      targeting: "true",
    },
  ],
});

async function setupForExperimentFeature() {
  const sandbox = sinon.createSandbox();
  const manager = ExperimentFakes.manager();

  await manager.onStartup();

  sandbox.stub(ExperimentAPI, "_store").get(() => manager.store);

  return { sandbox, manager };
}

add_task(async function validSchema() {
  const ajv = new Ajv({ allErrors: true });
  const validate = ajv.compile(await fetchSchema);

  Assert.ok(
    validate(REMOTE_CONFIGURATION),
    JSON.stringify(validate.errors, null, 2)
  );
});

add_task(async function readyCallAfterStore_with_remote_value() {
  let { sandbox, manager } = await setupForExperimentFeature();
  let feature = new ExperimentFeature("aboutwelcome");

  Assert.equal(
    feature.getValue().remoteValue,
    undefined,
    "Feature is not defined"
  );

  manager.store.updateRemoteConfigs(
    "aboutwelcome",
    REMOTE_CONFIGURATION.configurations[1]
  );

  // Should resolve because the store will return a dummy remote value
  await feature.ready();

  Assert.ok(feature.getValue().skipFocus, "Loads value from store");
  manager.store._deleteForTests("remoteDefaults");
  sandbox.restore();
});

add_task(async function update_remote_defaults_onUpdate() {
  let { sandbox, manager } = await setupForExperimentFeature();
  let feature = new ExperimentFeature("aboutwelcome");
  let stub = sandbox.stub();

  feature.onUpdate(stub);

  Assert.equal(
    feature.getValue().remoteValue,
    undefined,
    "Feature is not defined"
  );

  manager.store.updateRemoteConfigs(
    "aboutwelcome",
    REMOTE_CONFIGURATION.configurations[1]
  );

  Assert.ok(stub.called, "update event called");
  Assert.equal(stub.callCount, 1, "Called once for remote configs");
  Assert.equal(
    stub.firstCall.args[1],
    "remote-defaults-update",
    "Correct reason"
  );

  manager.store._deleteForTests("remoteDefaults");
  sandbox.restore();
});

add_task(async function update_remote_defaults_readyPromise() {
  let { sandbox, manager } = await setupForExperimentFeature();
  let feature = new ExperimentFeature("aboutwelcome");
  let stub = sandbox.stub();
  let promise = feature.ready();

  feature.onUpdate(stub);

  Assert.equal(
    feature.getValue().remoteValue,
    undefined,
    "Feature is not defined"
  );

  manager.store.updateRemoteConfigs(
    "aboutwelcome",
    REMOTE_CONFIGURATION.configurations[1]
  );

  await promise;

  Assert.ok(feature._remoteReady, "Ready state updated");

  manager.store._deleteForTests("remoteDefaults");
  sandbox.restore();
});

add_task(async function update_remote_defaults_enabled() {
  let { sandbox, manager } = await setupForExperimentFeature();
  let feature = new ExperimentFeature("aboutwelcome");
  let promise = feature.ready();

  Assert.equal(
    feature.isEnabled(),
    true,
    "Feature is enabled by enabledFallbackPref"
  );

  manager.store.updateRemoteConfigs(
    "aboutwelcome",
    REMOTE_CONFIGURATION.configurations[0]
  );

  await promise;

  Assert.ok(
    !feature.isEnabled(),
    "Feature is disabled by remote configuration"
  );

  manager.store._deleteForTests("remoteDefaults");
  sandbox.restore();
});
