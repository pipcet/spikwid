/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

/**
 * @typedef {import("./@types/ExperimentManager").Enrollment} Enrollment
 * @typedef {import("./@types/ExperimentManager").FeatureConfig} FeatureConfig
 */

const EXPORTED_SYMBOLS = ["ExperimentStore"];

const { SharedDataMap } = ChromeUtils.import(
  "resource://nimbus/lib/SharedDataMap.jsm"
);
const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);
const { Services } = ChromeUtils.import("resource://gre/modules/Services.jsm");

const IS_MAIN_PROCESS =
  Services.appinfo.processType === Services.appinfo.PROCESS_TYPE_DEFAULT;

const SYNC_DATA_PREF_BRANCH = "nimbus.syncdatastore.";
let tryJSONParse = data => {
  try {
    return JSON.parse(data);
  } catch (e) {}

  return null;
};
XPCOMUtils.defineLazyGetter(this, "syncDataStore", () => {
  // Pref name changed in bug 1691516 and we want to clear the old pref for users
  const previousPrefName = "messaging-system.syncdatastore.data";
  try {
    if (IS_MAIN_PROCESS) {
      Services.prefs.clearUserPref(previousPrefName);
    }
  } catch (e) {}

  let prefBranch = Services.prefs.getBranch(SYNC_DATA_PREF_BRANCH);
  return {
    get(featureId) {
      try {
        return tryJSONParse(prefBranch.getStringPref(featureId, ""));
      } catch (e) {
        /* This is expected if we don't have anything stored */
      }

      return null;
    },
    set(featureId, value) {
      try {
        prefBranch.setStringPref(featureId, JSON.stringify(value));
      } catch (e) {
        Cu.reportError(e);
      }
    },
    delete(featureId) {
      prefBranch.clearUserPref(featureId);
    },
  };
});

const DEFAULT_STORE_ID = "ExperimentStoreData";
// Experiment feature configs that should be saved to prefs for
// fast access on startup.
const SYNC_ACCESS_FEATURES = ["newtab", "aboutwelcome"];

class ExperimentStore extends SharedDataMap {
  constructor(sharedDataKey, options = { isParent: IS_MAIN_PROCESS }) {
    super(sharedDataKey || DEFAULT_STORE_ID, options);
  }

  /**
   * Given a feature identifier, find an active experiment that matches that feature identifier.
   * This assumes, for now, that there is only one active experiment per feature per browser.
   * Does not activate the experiment (send an exposure event)
   *
   * @param {string} featureId
   * @returns {Enrollment|undefined} An active experiment if it exists
   * @memberof ExperimentStore
   */
  getExperimentForFeature(featureId) {
    return (
      this.getAllActive().find(
        experiment =>
          experiment.featureIds?.includes(featureId) ||
          // Supports <v1.3.0, which was when .featureIds was added
          experiment.branch?.feature?.featureId === featureId
        // Default to the pref store if data is not yet ready
      ) || syncDataStore.get(featureId)
    );
  }

  /**
   * Check if an active experiment already exists for a feature.
   * Does not activate the experiment (send an exposure event)
   *
   * @param {string} featureId
   * @returns {boolean} Does an active experiment exist for that feature?
   * @memberof ExperimentStore
   */
  hasExperimentForFeature(featureId) {
    if (!featureId) {
      return false;
    }
    return !!this.getExperimentForFeature(featureId);
  }

  /**
   * @returns {Enrollment[]}
   */
  getAll() {
    let data = [];
    try {
      data = Object.values(this._data || {});
    } catch (e) {
      Cu.reportError(e);
    }

    return data;
  }

  /**
   * @returns {Enrollment[]}
   */
  getAllActive() {
    return this.getAll().filter(experiment => experiment.active);
  }

  _emitExperimentUpdates(experiment) {
    this.emit(`update:${experiment.slug}`, experiment);
    if (experiment.branch.feature) {
      this.emit(`update:${experiment.branch.feature.featureId}`, experiment);
      this._emitFeatureUpdate(
        experiment.branch.feature.featureId,
        "experiment-updated"
      );
    }
  }

  _emitFeatureUpdate(featureId, reason) {
    this.emit(`featureUpdate:${featureId}`, reason);
  }

  _onFeatureUpdate(featureId, callback) {
    this.on(`featureUpdate:${featureId}`, callback);
  }

  _offFeatureUpdate(featureId, callback) {
    this.off(`featureUpdate:${featureId}`, callback);
  }

  /**
   * @param {Enrollment} experiment
   */
  _updateSyncStore(experiment) {
    let featureId = experiment.branch.feature?.featureId;
    if (SYNC_ACCESS_FEATURES.includes(featureId)) {
      if (!experiment.active) {
        // Remove experiments on un-enroll, no need to check if it exists
        syncDataStore.delete(featureId);
      } else {
        syncDataStore.set(featureId, experiment);
      }
    }
  }

  /**
   * Add an experiment. Short form for .set(slug, experiment)
   * @param {Enrollment} experiment
   */
  addExperiment(experiment) {
    if (!experiment || !experiment.slug) {
      throw new Error(
        `Tried to add an experiment but it didn't have a .slug property.`
      );
    }
    this.set(experiment.slug, experiment);
    this._updateSyncStore(experiment);
    this._emitExperimentUpdates(experiment);
  }

  /**
   * Merge new properties into the properties of an existing experiment
   * @param {string} slug
   * @param {Partial<Enrollment>} newProperties
   */
  updateExperiment(slug, newProperties) {
    const oldProperties = this.get(slug);
    if (!oldProperties) {
      throw new Error(
        `Tried to update experiment ${slug} bug it doesn't exist`
      );
    }
    const updatedExperiment = { ...oldProperties, ...newProperties };
    this.set(slug, updatedExperiment);
    this._updateSyncStore(updatedExperiment);
    this._emitExperimentUpdates(updatedExperiment);
  }
}
