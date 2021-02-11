/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

ChromeUtils.import("resource://gre/modules/NetUtil.jsm");
var { setTimeout } = ChromeUtils.import("resource://gre/modules/Timer.jsm");

let prefs;
let h2Port;
let h3Port;
let listen;
let trrServer;

const dns = Cc["@mozilla.org/network/dns-service;1"].getService(
  Ci.nsIDNSService
);
const certOverrideService = Cc[
  "@mozilla.org/security/certoverride;1"
].getService(Ci.nsICertOverrideService);
const threadManager = Cc["@mozilla.org/thread-manager;1"].getService(
  Ci.nsIThreadManager
);
const mainThread = threadManager.currentThread;

const defaultOriginAttributes = {};

function setup() {
  let env = Cc["@mozilla.org/process/environment;1"].getService(
    Ci.nsIEnvironment
  );
  h2Port = env.get("MOZHTTP2_PORT");
  Assert.notEqual(h2Port, null);
  Assert.notEqual(h2Port, "");

  h3Port = env.get("MOZHTTP3_PORT_NO_RESPONSE");
  Assert.notEqual(h3Port, null);
  Assert.notEqual(h3Port, "");

  // Set to allow the cert presented by our H2 server
  do_get_profile();
  prefs = Cc["@mozilla.org/preferences-service;1"].getService(Ci.nsIPrefBranch);

  prefs.setBoolPref("network.security.esni.enabled", false);
  prefs.setBoolPref("network.http.spdy.enabled", true);
  prefs.setBoolPref("network.http.spdy.enabled.http2", true);
  // the TRR server is on 127.0.0.1
  prefs.setCharPref("network.trr.bootstrapAddress", "127.0.0.1");

  // make all native resolve calls "secretly" resolve localhost instead
  prefs.setBoolPref("network.dns.native-is-localhost", true);

  // 0 - off, 1 - race, 2 TRR first, 3 TRR only, 4 shadow
  prefs.setIntPref("network.trr.mode", 2); // TRR first
  prefs.setBoolPref("network.trr.wait-for-portal", false);
  // don't confirm that TRR is working, just go!
  prefs.setCharPref("network.trr.confirmationNS", "skip");

  // So we can change the pref without clearing the cache to check a pushed
  // record with a TRR path that fails.
  Services.prefs.setBoolPref("network.trr.clear-cache-on-pref-change", false);

  Services.prefs.setBoolPref("network.http.http3.enabled", true);

  // The moz-http2 cert is for foo.example.com and is signed by http2-ca.pem
  // so add that cert to the trust list as a signing cert.  // the foo.example.com domain name.
  const certdb = Cc["@mozilla.org/security/x509certdb;1"].getService(
    Ci.nsIX509CertDB
  );
  addCertFromFile(certdb, "http2-ca.pem", "CTu,u,u");
}

setup();
registerCleanupFunction(async () => {
  prefs.clearUserPref("network.security.esni.enabled");
  prefs.clearUserPref("network.http.spdy.enabled");
  prefs.clearUserPref("network.http.spdy.enabled.http2");
  prefs.clearUserPref("network.dns.localDomains");
  prefs.clearUserPref("network.dns.native-is-localhost");
  prefs.clearUserPref("network.trr.mode");
  prefs.clearUserPref("network.trr.uri");
  prefs.clearUserPref("network.trr.credentials");
  prefs.clearUserPref("network.trr.wait-for-portal");
  prefs.clearUserPref("network.trr.allow-rfc1918");
  prefs.clearUserPref("network.trr.useGET");
  prefs.clearUserPref("network.trr.confirmationNS");
  prefs.clearUserPref("network.trr.bootstrapAddress");
  prefs.clearUserPref("network.trr.request-timeout");
  prefs.clearUserPref("network.trr.clear-cache-on-pref-change");
  prefs.clearUserPref("network.dns.upgrade_with_https_rr");
  prefs.clearUserPref("network.dns.use_https_rr_as_altsvc");
  prefs.clearUserPref("network.dns.echconfig.enabled");
  prefs.clearUserPref("network.dns.echconfig.fallback_to_origin");
  prefs.clearUserPref("network.dns.httpssvc.reset_exclustion_list");
  prefs.clearUserPref("network.http.http3.enabled");
  prefs.clearUserPref("network.dns.httpssvc.http3_fast_fallback_timeout");
  Services.prefs.clearUserPref(
    "network.http.http3.alt-svc-mapping-for-testing"
  );
  Services.prefs.clearUserPref("network.http.http3.backup_timer_delay");
  Services.prefs.clearUserPref("network.http.speculative-parallel-limit");
  if (trrServer) {
    await trrServer.stop();
  }
});

class DNSListener {
  constructor() {
    this.promise = new Promise(resolve => {
      this.resolve = resolve;
    });
  }
  onLookupComplete(inRequest, inRecord, inStatus) {
    this.resolve([inRequest, inRecord, inStatus]);
  }
  // So we can await this as a promise.
  then() {
    return this.promise.then.apply(this.promise, arguments);
  }
}

DNSListener.prototype.QueryInterface = ChromeUtils.generateQI([
  "nsIDNSListener",
]);

function makeChan(url) {
  let chan = NetUtil.newChannel({
    uri: url,
    loadUsingSystemPrincipal: true,
  }).QueryInterface(Ci.nsIHttpChannel);
  chan.loadFlags = Ci.nsIChannel.LOAD_INITIAL_DOCUMENT_URI;
  return chan;
}

function channelOpenPromise(chan, flags) {
  return new Promise(resolve => {
    function finish(req, buffer) {
      resolve([req, buffer]);
      certOverrideService.setDisableAllSecurityChecksAndLetAttackersInterceptMyData(
        false
      );
    }
    let internal = chan.QueryInterface(Ci.nsIHttpChannelInternal);
    internal.setWaitForHTTPSSVCRecord();
    certOverrideService.setDisableAllSecurityChecksAndLetAttackersInterceptMyData(
      true
    );
    chan.asyncOpen(new ChannelListener(finish, null, flags));
  });
}

let CheckOnlyHttp2Listener = function() {};

CheckOnlyHttp2Listener.prototype = {
  onStartRequest: function testOnStartRequest(request) {},

  onDataAvailable: function testOnDataAvailable(request, stream, off, cnt) {
    read_stream(stream, cnt);
  },

  onStopRequest: function testOnStopRequest(request, status) {
    Assert.equal(status, Cr.NS_OK);
    let httpVersion = "";
    try {
      httpVersion = request.protocolVersion;
    } catch (e) {}
    Assert.equal(httpVersion, "h2");

    let routed = "NA";
    try {
      routed = request.getRequestHeader("Alt-Used");
    } catch (e) {}
    dump("routed is " + routed + "\n");
    Assert.ok(routed === "0" || routed === "NA");
    this.finish();
  },
};

async function fast_fallback_test() {
  let result = 1;
  // We need to loop here because we need to wait for AltSvc storage to
  // to be started.
  // We also do not have a way to verify that HTTP3 has been tried, because
  // the fallback is automatic, so try a couple of times.
  do {
    // We need to close HTTP2 connections, otherwise our connection pooling
    // will dispatch the request over already existing HTTP2 connection.
    Services.obs.notifyObservers(null, "net:prune-all-connections");
    let chan = makeChan(`https://foo.example.com:${h2Port}/`);
    let listener = new CheckOnlyHttp2Listener();
    await altsvcSetupPromise(chan, listener);
    result++;
  } while (result < 3);
}

// Test the case when speculative connection is enabled. In this case, when the
// backup connection is ready, the http transaction is still in pending
// queue because the h3 connection is never ready to accept transactions.
add_task(async function test_fast_fallback_with_speculative_connection() {
  Services.prefs.setBoolPref("network.http.http3.enabled", true);
  Services.prefs.setCharPref("network.dns.localDomains", "foo.example.com");
  // Set AltSvc to point to not existing HTTP3 server on port 443
  Services.prefs.setCharPref(
    "network.http.http3.alt-svc-mapping-for-testing",
    "foo.example.com;h3-27=:" + h3Port
  );
  Services.prefs.setBoolPref("network.dns.disableIPv6", true);
  Services.prefs.setIntPref("network.http.speculative-parallel-limit", 6);

  await fast_fallback_test();
});

let HTTPObserver = {
  observeActivity(
    aChannel,
    aType,
    aSubtype,
    aTimestamp,
    aSizeData,
    aStringData
  ) {
    aChannel.QueryInterface(Ci.nsIChannel);
    if (aChannel.URI.spec == `https://foo.example.com:${h2Port}/`) {
      if (
        aType == Ci.nsIHttpActivityDistributor.ACTIVITY_TYPE_HTTP_TRANSACTION &&
        aSubtype ==
          Ci.nsIHttpActivityDistributor.ACTIVITY_SUBTYPE_REQUEST_HEADER
      ) {
        // We need to enable speculative connection again, since the backup
        // connection is done by using speculative connection.
        Services.prefs.setIntPref("network.http.speculative-parallel-limit", 6);
        let observerService = Cc[
          "@mozilla.org/network/http-activity-distributor;1"
        ].getService(Ci.nsIHttpActivityDistributor);
        observerService.removeObserver(HTTPObserver);
      }
    }
  },
};

// Test the case when speculative connection is disabled. In this case, when the
// back connection is ready, the http transaction is already activated,
// but the socket is not ready to write.
add_task(async function test_fast_fallback_without_speculative_connection() {
  // Make sure the h3 connection created by the previous test is cleared.
  Services.obs.notifyObservers(null, "net:cancel-all-connections");
  await new Promise(resolve => setTimeout(resolve, 1000));
  // Clear the h3 excluded list, otherwise the Alt-Svc mapping will not be used.
  Services.obs.notifyObservers(null, "network:reset-http3-excluded-list");
  Services.prefs.setIntPref("network.http.speculative-parallel-limit", 0);

  let observerService = Cc[
    "@mozilla.org/network/http-activity-distributor;1"
  ].getService(Ci.nsIHttpActivityDistributor);
  observerService.addObserver(HTTPObserver);

  await fast_fallback_test();

  Services.prefs.clearUserPref(
    "network.http.http3.alt-svc-mapping-for-testing"
  );
});

// Test when echConfig is disabled and we have https rr for http3. We use a
// longer timeout in this test, so when fast fallback timer is triggered, the
// http transaction is already activated.
add_task(async function testFastfallback() {
  trrServer = new TRRServer();
  await trrServer.start();
  Services.prefs.setBoolPref("network.dns.upgrade_with_https_rr", true);
  Services.prefs.setBoolPref("network.dns.use_https_rr_as_altsvc", true);
  Services.prefs.setBoolPref("network.dns.echconfig.enabled", false);

  Services.prefs.setIntPref("network.trr.mode", 3);
  Services.prefs.setCharPref(
    "network.trr.uri",
    `https://foo.example.com:${trrServer.port}/dns-query`
  );
  Services.prefs.setBoolPref("network.http.http3.enabled", true);

  Services.prefs.setIntPref(
    "network.dns.httpssvc.http3_fast_fallback_timeout",
    1000
  );

  await trrServer.registerDoHAnswers("test.fastfallback.com", "HTTPS", [
    {
      name: "test.fastfallback.com",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 1,
        name: "test.fastfallback1.com",
        values: [
          { key: "alpn", value: "h3-27" },
          { key: "port", value: h3Port },
          { key: "echconfig", value: "456..." },
        ],
      },
    },
    {
      name: "test.fastfallback.com",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 2,
        name: "test.fastfallback2.com",
        values: [
          { key: "alpn", value: "h2" },
          { key: "port", value: h2Port },
          { key: "echconfig", value: "456..." },
        ],
      },
    },
  ]);

  await trrServer.registerDoHAnswers("test.fastfallback1.com", "A", [
    {
      name: "test.fastfallback1.com",
      ttl: 55,
      type: "A",
      flush: false,
      data: "127.0.0.1",
    },
  ]);

  await trrServer.registerDoHAnswers("test.fastfallback2.com", "A", [
    {
      name: "test.fastfallback2.com",
      ttl: 55,
      type: "A",
      flush: false,
      data: "127.0.0.1",
    },
  ]);

  let chan = makeChan(`https://test.fastfallback.com/server-timing`);
  let [req] = await channelOpenPromise(chan);
  Assert.equal(req.protocolVersion, "h2");
  let internal = req.QueryInterface(Ci.nsIHttpChannelInternal);
  Assert.equal(internal.remotePort, h2Port);

  await trrServer.stop();
});

// Like the previous test, but with a shorter timeout, so when fast fallback
// timer is triggered, the http transaction is still in pending queue.
add_task(async function testFastfallback1() {
  trrServer = new TRRServer();
  await trrServer.start();
  Services.prefs.setBoolPref("network.dns.upgrade_with_https_rr", true);
  Services.prefs.setBoolPref("network.dns.use_https_rr_as_altsvc", true);
  Services.prefs.setBoolPref("network.dns.echconfig.enabled", false);

  Services.prefs.setIntPref("network.trr.mode", 3);
  Services.prefs.setCharPref(
    "network.trr.uri",
    `https://foo.example.com:${trrServer.port}/dns-query`
  );
  Services.prefs.setBoolPref("network.http.http3.enabled", true);

  Services.prefs.setIntPref(
    "network.dns.httpssvc.http3_fast_fallback_timeout",
    10
  );

  await trrServer.registerDoHAnswers("test.fastfallback.org", "HTTPS", [
    {
      name: "test.fastfallback.org",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 1,
        name: "test.fastfallback1.org",
        values: [
          { key: "alpn", value: "h3-27" },
          { key: "port", value: h3Port },
          { key: "echconfig", value: "456..." },
        ],
      },
    },
    {
      name: "test.fastfallback.org",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 2,
        name: "test.fastfallback2.org",
        values: [
          { key: "alpn", value: "h2" },
          { key: "port", value: h2Port },
          { key: "echconfig", value: "456..." },
        ],
      },
    },
  ]);

  await trrServer.registerDoHAnswers("test.fastfallback1.org", "A", [
    {
      name: "test.fastfallback1.org",
      ttl: 55,
      type: "A",
      flush: false,
      data: "127.0.0.1",
    },
  ]);

  await trrServer.registerDoHAnswers("test.fastfallback2.org", "A", [
    {
      name: "test.fastfallback2.org",
      ttl: 55,
      type: "A",
      flush: false,
      data: "127.0.0.1",
    },
  ]);

  let chan = makeChan(`https://test.fastfallback.org/server-timing`);
  let [req] = await channelOpenPromise(chan);
  Assert.equal(req.protocolVersion, "h2");
  let internal = req.QueryInterface(Ci.nsIHttpChannelInternal);
  Assert.equal(internal.remotePort, h2Port);

  await trrServer.stop();
});

// Test when echConfig is enabled, we can sucessfully fallback to the last
// record.
add_task(async function testFastfallbackWithEchConfig() {
  trrServer = new TRRServer();
  await trrServer.start();
  Services.prefs.setBoolPref("network.dns.upgrade_with_https_rr", true);
  Services.prefs.setBoolPref("network.dns.use_https_rr_as_altsvc", true);
  Services.prefs.setBoolPref("network.dns.echconfig.enabled", true);

  Services.prefs.setIntPref("network.trr.mode", 3);
  Services.prefs.setCharPref(
    "network.trr.uri",
    `https://foo.example.com:${trrServer.port}/dns-query`
  );
  Services.prefs.setBoolPref("network.http.http3.enabled", true);

  Services.prefs.setIntPref(
    "network.dns.httpssvc.http3_fast_fallback_timeout",
    1000
  );

  await trrServer.registerDoHAnswers("test.ech.org", "HTTPS", [
    {
      name: "test.ech.org",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 1,
        name: "test.ech1.org",
        values: [
          { key: "alpn", value: "h3-27" },
          { key: "port", value: h3Port },
          { key: "echconfig", value: "456..." },
        ],
      },
    },
    {
      name: "test.ech.org",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 2,
        name: "test.ech2.org",
        values: [
          { key: "alpn", value: "h2" },
          { key: "port", value: h2Port },
          { key: "echconfig", value: "456..." },
        ],
      },
    },
    {
      name: "test.ech.org",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 3,
        name: "test.ech3.org",
        values: [
          { key: "alpn", value: "h2" },
          { key: "port", value: h2Port },
          { key: "echconfig", value: "456..." },
        ],
      },
    },
  ]);

  await trrServer.registerDoHAnswers("test.ech1.org", "A", [
    {
      name: "test.ech1.org",
      ttl: 55,
      type: "A",
      flush: false,
      data: "127.0.0.1",
    },
  ]);

  await trrServer.registerDoHAnswers("test.ech3.org", "A", [
    {
      name: "test.ech3.org",
      ttl: 55,
      type: "A",
      flush: false,
      data: "127.0.0.1",
    },
  ]);

  let chan = makeChan(`https://test.ech.org/server-timing`);
  let [req] = await channelOpenPromise(chan);
  Assert.equal(req.protocolVersion, "h2");
  let internal = req.QueryInterface(Ci.nsIHttpChannelInternal);
  Assert.equal(internal.remotePort, h2Port);

  await trrServer.stop();
});

// Test when echConfig is enabled, the connection should fail when not all
// records have echConfig.
add_task(async function testFastfallbackWithpartialEchConfig() {
  trrServer = new TRRServer();
  await trrServer.start();
  Services.prefs.setBoolPref("network.dns.upgrade_with_https_rr", true);
  Services.prefs.setBoolPref("network.dns.use_https_rr_as_altsvc", true);
  Services.prefs.setBoolPref("network.dns.echconfig.enabled", true);

  Services.prefs.setIntPref("network.trr.mode", 3);
  Services.prefs.setCharPref(
    "network.trr.uri",
    `https://foo.example.com:${trrServer.port}/dns-query`
  );
  Services.prefs.setBoolPref("network.http.http3.enabled", true);

  Services.prefs.setIntPref(
    "network.dns.httpssvc.http3_fast_fallback_timeout",
    1000
  );

  await trrServer.registerDoHAnswers("test.partial_ech.org", "HTTPS", [
    {
      name: "test.partial_ech.org",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 1,
        name: "test.partial_ech1.org",
        values: [
          { key: "alpn", value: "h3-27" },
          { key: "port", value: h3Port },
          { key: "echconfig", value: "456..." },
        ],
      },
    },
    {
      name: "test.partial_ech.org",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 2,
        name: "test.partial_ech2.org",
        values: [
          { key: "alpn", value: "h2" },
          { key: "port", value: h2Port },
        ],
      },
    },
  ]);

  await trrServer.registerDoHAnswers("test.partial_ech1.org", "A", [
    {
      name: "test.partial_ech1.org",
      ttl: 55,
      type: "A",
      flush: false,
      data: "127.0.0.1",
    },
  ]);

  let chan = makeChan(`https://test.partial_ech.org/server-timing`);
  await channelOpenPromise(chan, CL_EXPECT_LATE_FAILURE | CL_ALLOW_UNKNOWN_CL);

  await trrServer.stop();
});

add_task(async function testFastfallbackWithoutEchConfig() {
  trrServer = new TRRServer();
  await trrServer.start();
  Services.prefs.setBoolPref("network.dns.upgrade_with_https_rr", true);
  Services.prefs.setBoolPref("network.dns.use_https_rr_as_altsvc", true);

  Services.prefs.setIntPref("network.trr.mode", 3);
  Services.prefs.setCharPref(
    "network.trr.uri",
    `https://foo.example.com:${trrServer.port}/dns-query`
  );
  Services.prefs.setBoolPref("network.http.http3.enabled", true);

  Services.prefs.setIntPref(
    "network.dns.httpssvc.http3_fast_fallback_timeout",
    1000
  );

  await trrServer.registerDoHAnswers("test.no_ech_h2.org", "HTTPS", [
    {
      name: "test.no_ech_h2.org",
      ttl: 55,
      type: "HTTPS",
      flush: false,
      data: {
        priority: 1,
        name: "test.no_ech_h3.org",
        values: [
          { key: "alpn", value: "h3-27" },
          { key: "port", value: h3Port },
        ],
      },
    },
  ]);

  await trrServer.registerDoHAnswers("test.no_ech_h3.org", "A", [
    {
      name: "test.no_ech_h3.org",
      ttl: 55,
      type: "A",
      flush: false,
      data: "127.0.0.1",
    },
  ]);

  await trrServer.registerDoHAnswers("test.no_ech_h2.org", "A", [
    {
      name: "test.no_ech_h2.org",
      ttl: 55,
      type: "A",
      flush: false,
      data: "127.0.0.1",
    },
  ]);

  let chan = makeChan(`https://test.no_ech_h2.org:${h2Port}/server-timing`);
  let [req] = await channelOpenPromise(chan);
  Assert.equal(req.protocolVersion, "h2");
  let internal = req.QueryInterface(Ci.nsIHttpChannelInternal);
  Assert.equal(internal.remotePort, h2Port);

  await trrServer.stop();
});
