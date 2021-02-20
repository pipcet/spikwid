/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const EXPORTED_SYMBOLS = ["UrlbarQuickSuggest", "KeywordTree"];

const { XPCOMUtils } = ChromeUtils.import(
  "resource://gre/modules/XPCOMUtils.jsm"
);

XPCOMUtils.defineLazyModuleGetters(this, {
  RemoteSettings: "resource://services-settings/remote-settings.js",
  UrlbarPrefs: "resource:///modules/UrlbarPrefs.jsm",
});

XPCOMUtils.defineLazyGlobalGetters(this, ["TextDecoder"]);

const log = console.createInstance({
  prefix: "QuickSuggest",
  maxLogLevel: UrlbarPrefs.get("quicksuggest.log") ? "All" : "Warn",
});

const RS_COLLECTION = "quicksuggest";
const RS_PREF = "quicksuggest.enabled";

/**
 * Fetches the suggestions data from RemoteSettings and builds the tree
 * to provide suggestions for UrlbarProviderQuickSuggest.
 */
class Suggestions {
  // The RemoteSettings client.
  _rs = null;
  // Let tests wait for init to complete.
  _initPromise = null;
  // A tree that maps keywords to a result.
  _tree = new KeywordTree();
  // A map of the result data.
  _results = new Map();

  async init() {
    if (this._initPromise) {
      return this._initPromise;
    }
    this._initPromise = Promise.resolve();
    this._rs = RemoteSettings(RS_COLLECTION);
    if (UrlbarPrefs.get(RS_PREF)) {
      this._setupRemoteSettings();
      this._initPromise = this._ensureAttachmentsDownloaded();
    } else {
      UrlbarPrefs.addObserver(this);
    }
    return this._initPromise;
  }

  /*
   * Handle queries from the Urlbar.
   */
  async query(phrase) {
    log.info("Handling query for", phrase);
    let index = this._tree.get(phrase);
    if (!index || !this._results.has(index)) {
      return null;
    }
    let result = this._results.get(index);
    let d = new Date();
    let date = `${d.getFullYear()}${d.getMonth() +
      1}${d.getDate()}${d.getHours()}`;
    let icon = await this.fetchIcon(result.icon);
    return {
      title: result.title,
      url: result.url.replace("%YYYYMMDDHH%", date),
      icon,
    };
  }

  /*
   * Called if a Urlbar preference is changed.
   */
  onPrefChanged(changedPref) {
    if (changedPref == RS_PREF && UrlbarPrefs.get(RS_PREF)) {
      this._setupRemoteSettings();
      this._ensureAttachmentsDownloaded();
    }
  }

  /*
   * Set up RemoteSettings listeners.
   */
  async _setupRemoteSettings() {
    this._rs.on("sync", this._onSettingsSync.bind(this));
  }

  /*
   * Called when RemoteSettings updates are received.
   */
  async _onSettingsSync({ data: { deleted } }) {
    const toDelete = deleted?.filter(d => d.attachment);
    // Remove local files of deleted records
    if (toDelete) {
      await Promise.all(
        toDelete.map(entry => this._rs.attachments.delete(entry))
      );
    }
    await this._ensureAttachmentsDownloaded();
  }

  /*
   * We store our RemoteSettings data in attachments, ensure the attachments
   * are saved locally.
   */
  async _ensureAttachmentsDownloaded() {
    log.info("_ensureAttachmentsDownloaded started");
    let dataOpts = { useCache: true };
    let data = await this._rs.get({ filters: { type: "data" } });
    await Promise.all(
      data.map(r => this._rs.attachments.download(r, dataOpts))
    );

    let icons = await this._rs.get({ filters: { type: "icon" } });
    await Promise.all(icons.map(r => this._rs.attachments.download(r)));

    await this._createTree();
    log.info("_ensureAttachmentsDownloaded complete");
  }

  /*
   * Recreate the KeywordTree on startup or with RemoteSettings updates.
   */
  async _createTree() {
    log.info("Building new KeywordTree");
    this._results = new Map();
    this._tree = new KeywordTree();
    let data = await this._rs.get({ filters: { type: "data" } });

    for (let record of data) {
      let { buffer } = await this._rs.attachments.download(record, {
        useCache: true,
      });
      let json = JSON.parse(new TextDecoder("utf-8").decode(buffer));
      this._processSuggestionsJSON(json);
    }
  }

  /*
   * Handle incoming suggestions data and add to local data.
   */
  async _processSuggestionsJSON(json) {
    for (let result of json) {
      this._results.set(result.id, result);
      for (let keyword of result.keywords) {
        this._tree.set(keyword, result.id);
      }
    }
  }

  /*
   * Fetch the icon from RemoteSettings attachments.
   */
  async fetchIcon(path) {
    let record = (
      await this._rs.get({
        filters: { id: `icon-${path}` },
      })
    ).pop();
    if (!record) {
      return null;
    }
    return this._rs.attachments.download(record);
  }
}

// Token used as a key to store results within the Map, cannot be used
// within a keyword.
const RESULT_KEY = "^";

/**
 * This is an implementation of a Map based Tree. We can store
 * multiple keywords that point to a single term, for example:
 *
 *   tree.add("headphones", "headphones");
 *   tree.add("headph", "headphones");
 *   tree.add("earphones", "headphones");
 *
 *   tree.get("headph") == "headphones"
 *
 * The tree can store multiple prefixes to a term efficiently
 * so ["hea", "head", "headp", "headph", "headpho", ...] wont lead
 * to duplication in memory. The tree will only return a result
 * for keywords that have been explcitly defined and not attempt
 * to guess based on prefix.
 *
 * Once a tree have been build, it can be flattened with `.flatten`
 * the tree can then be serialised and deserialised with `.toJSON`
 * and `.fromJSON`.
 */
class KeywordTree {
  constructor() {
    this.tree = new Map();
  }

  /*
   * Set a keyword for a result.
   */
  set(keyword, id) {
    if (keyword.includes(RESULT_KEY)) {
      throw new Error(`"${RESULT_KEY}" is reserved`);
    }
    let tree = this.tree;
    for (let x = 0, c = ""; (c = keyword.charAt(x)); x++) {
      let child = tree.get(c) || new Map();
      tree.set(c, child);
      tree = child;
    }
    tree.set(RESULT_KEY, id);
  }

  /*
   * Get the result for a given phrase.
   */
  get(phrase) {
    let tree = this.tree;
    /*eslint no-labels: ["error", { "allowLoop": true }]*/
    loop: while (phrase.length) {
      for (const [key, child] of tree.entries()) {
        // We need to check if key starts with phrase because we
        // may have flattened the key and so .get("hel") will need
        // to match index "hello", we will only flatten this way if
        // the result matches.
        if (phrase.startsWith(key) || key.startsWith(phrase)) {
          phrase = phrase.slice(key.length);
          if (!phrase.length) {
            return child.get(RESULT_KEY) || null;
          }
          tree = child;
          continue loop;
        }
      }
      return null;
    }
    return null;
  }

  /*
   * We flatten the tree by combining consecutive single branch keywords
   * with the same results into a longer keyword. so ["a", ["b", ["c"]]]
   * becomes ["abc"], we need to be careful that the result matches so
   * if a prefix search for "hello" only starts after 2 characters it will
   * be flattened to ["he", ["llo"]].
   */
  flatten() {
    for (let key of Array.from(this.tree.keys())) {
      this._flatten(this.tree, key);
    }
  }

  _flatten(parent, key) {
    let tree = parent.get(key);
    let keys = Array.from(tree.keys()).filter(k => k != RESULT_KEY);
    let result = tree.get(RESULT_KEY);

    if (keys.length == 1) {
      let childKey = keys[0];
      let child = tree.get(childKey);
      let childResult = child.get(RESULT_KEY);

      if (result == childResult) {
        let newKey = key + childKey;
        parent.set(newKey, child);
        parent.delete(key);
        this._flatten(parent, newKey);
      } else {
        this._flatten(tree, childKey);
      }
    } else {
      for (let k of keys) {
        this._flatten(tree, k);
      }
    }
  }

  /*
   * Turn a tree into a serialisable JSON object.
   */
  toJSONObject(map = this.tree) {
    let tmp = {};
    for (let [key, val] of map) {
      if (val instanceof Map) {
        tmp[key] = this.toJSONObject(val);
      } else {
        tmp[key] = val;
      }
    }
    return tmp;
  }

  /*
   * Build a tree from a serialisable JSON object that was built
   * with `toJSON`.
   */
  fromJSON(json) {
    this.tree = this.JSONObjectToMap(json);
  }

  JSONObjectToMap(obj) {
    let map = new Map();
    for (let key of Object.keys(obj)) {
      if (typeof obj[key] == "object") {
        map.set(key, this.JSONObjectToMap(obj[key]));
      } else {
        map.set(key, obj[key]);
      }
    }
    return map;
  }
}

let UrlbarQuickSuggest = new Suggestions();
UrlbarQuickSuggest.init();
