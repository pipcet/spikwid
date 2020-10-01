/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { ActorClassWithSpec, Actor } = require("devtools/shared/protocol");
const { workerTargetSpec } = require("devtools/shared/specs/targets/worker");

const { ThreadActor } = require("devtools/server/actors/thread");
const { WebConsoleActor } = require("devtools/server/actors/webconsole");
const Targets = require("devtools/server/actors/targets/index");

const makeDebuggerUtil = require("devtools/server/actors/utils/make-debugger");
const { TabSources } = require("devtools/server/actors/utils/TabSources");

exports.WorkerTargetActor = ActorClassWithSpec(workerTargetSpec, {
  targetType: Targets.TYPES.WORKER,

  /**
   * Target actor for a worker in the content process.
   *
   * @param {DevToolsServerConnection} connection: The connection to the client.
   * @param {WorkerGlobalScope} workerGlobal: The worker global.
   */
  initialize: function(connection, workerGlobal) {
    Actor.prototype.initialize.call(this, connection);

    // workerGlobal is needed by the console actor for evaluations.
    this.workerGlobal = workerGlobal;
    this._sources = null;

    this.makeDebugger = makeDebuggerUtil.bind(null, {
      findDebuggees: () => {
        return [workerGlobal];
      },
      shouldAddNewGlobalAsDebuggee: () => true,
    });
  },

  form() {
    return {
      actor: this.actorID,
      threadActor: this.threadActor?.actorID,
      consoleActor: this._consoleActor?.actorID,
    };
  },

  attach() {
    // needed by the console actor
    this.threadActor = new ThreadActor(this, this.workerGlobal);

    // needed by the thread actor to communicate with the console when evaluating logpoints.
    this._consoleActor = new WebConsoleActor(this.conn, this);

    this.manage(this.threadActor);
    this.manage(this._consoleActor);
  },

  get dbg() {
    if (!this._dbg) {
      this._dbg = this.makeDebugger();
    }
    return this._dbg;
  },

  get sources() {
    if (this._sources === null) {
      this._sources = new TabSources(this.threadActor);
    }

    return this._sources;
  },

  // This is called from the ThreadActor#onAttach method
  onThreadAttached() {
    // This isn't an RDP event and is only listened to from startup/worker.js.
    this.emit("worker-thread-attached");
  },
});
