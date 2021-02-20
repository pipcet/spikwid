/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <string.h>

#include "jsapi.h"

#include "js/CompilationAndEvaluation.h"
#include "js/experimental/JSStencil.h"
#include "js/Transcoding.h"
#include "jsapi-tests/tests.h"

BEGIN_TEST(testStencil_Basic) {
  const char* chars =
      "function f() { return 42; }"
      "f();";

  JS::SourceText<mozilla::Utf8Unit> srcBuf;
  CHECK(srcBuf.init(cx, chars, strlen(chars), JS::SourceOwnership::Borrowed));

  JS::CompileOptions options(cx);
  RefPtr<JS::Stencil> stencil =
      JS::CompileGlobalScriptToStencil(cx, options, srcBuf);
  CHECK(stencil);

  JS::RootedScript script(cx,
                          JS::InstantiateGlobalStencil(cx, options, stencil));
  CHECK(script);

  JS::RootedValue rval(cx);
  CHECK(JS_ExecuteScript(cx, script, &rval));
  CHECK(rval.isNumber() && rval.toNumber() == 42);

  return true;
}
END_TEST(testStencil_Basic)

BEGIN_TEST(testStencil_NonSyntactic) {
  const char* chars =
      "function f() { return x; }"
      "f();";

  JS::SourceText<mozilla::Utf8Unit> srcBuf;
  CHECK(srcBuf.init(cx, chars, strlen(chars), JS::SourceOwnership::Borrowed));

  JS::CompileOptions options(cx);
  options.setNonSyntacticScope(true);

  RefPtr<JS::Stencil> stencil =
      JS::CompileGlobalScriptToStencil(cx, options, srcBuf);
  CHECK(stencil);

  JS::RootedScript script(cx,
                          JS::InstantiateGlobalStencil(cx, options, stencil));
  CHECK(script);

  JS::RootedObject obj(cx, JS_NewPlainObject(cx));
  JS::RootedValue val(cx, JS::Int32Value(42));
  CHECK(obj);
  CHECK(JS_SetProperty(cx, obj, "x", val));

  JS::RootedObjectVector chain(cx);
  CHECK(chain.append(obj));

  JS::RootedValue rval(cx);
  CHECK(JS_ExecuteScript(cx, chain, script, &rval));
  CHECK(rval.isNumber() && rval.toNumber() == 42);

  return true;
}
END_TEST(testStencil_NonSyntactic)

BEGIN_TEST(testStencil_MultiGlobal) {
  const char* chars =
      "function f() { return 42; }"
      "f();";

  JS::SourceText<mozilla::Utf8Unit> srcBuf;
  CHECK(srcBuf.init(cx, chars, strlen(chars), JS::SourceOwnership::Borrowed));

  JS::CompileOptions options(cx);
  RefPtr<JS::Stencil> stencil =
      JS::CompileGlobalScriptToStencil(cx, options, srcBuf);
  CHECK(stencil);

  CHECK(RunInNewGlobal(cx, stencil));
  CHECK(RunInNewGlobal(cx, stencil));
  CHECK(RunInNewGlobal(cx, stencil));

  return true;
}
bool RunInNewGlobal(JSContext* cx, RefPtr<JS::Stencil> stencil) {
  JS::RootedObject otherGlobal(cx, createGlobal());
  CHECK(otherGlobal);

  JSAutoRealm ar(cx, otherGlobal);

  JS::CompileOptions options(cx);
  JS::RootedScript script(cx,
                          JS::InstantiateGlobalStencil(cx, options, stencil));
  CHECK(script);

  JS::RootedValue rval(cx);
  CHECK(JS_ExecuteScript(cx, script, &rval));
  CHECK(rval.isNumber() && rval.toNumber() == 42);

  return true;
}
END_TEST(testStencil_MultiGlobal)

BEGIN_TEST(testStencil_Transcode) {
  JS::SetProcessBuildIdOp(TestGetBuildId);

  JS::TranscodeBuffer buffer;

  {
    const char* chars =
        "function f() { return 42; }"
        "f();";

    JS::SourceText<mozilla::Utf8Unit> srcBuf;
    CHECK(srcBuf.init(cx, chars, strlen(chars), JS::SourceOwnership::Borrowed));

    JS::CompileOptions options(cx);
    RefPtr<JS::Stencil> stencil =
        JS::CompileGlobalScriptToStencil(cx, options, srcBuf);
    CHECK(stencil);

    // Encode Stencil to XDR
    JS::TranscodeResult res = JS::EncodeStencil(cx, options, stencil, buffer);
    CHECK(res == JS::TranscodeResult::Ok);
    CHECK(!buffer.empty());

    // Instantiate and Run
    JS::RootedScript script(cx,
                            JS::InstantiateGlobalStencil(cx, options, stencil));
    JS::RootedValue rval(cx);
    CHECK(script);
    CHECK(JS_ExecuteScript(cx, script, &rval));
    CHECK(rval.isNumber() && rval.toNumber() == 42);
  }

  // Create a new global
  CHECK(createGlobal());
  JSAutoRealm ar(cx, global);

  // Confirm it doesn't have the old code
  bool found = false;
  CHECK(JS_HasOwnProperty(cx, global, "f", &found));
  CHECK(!found);

  {
    JS::TranscodeRange range(buffer.begin(), buffer.length());

    // Decode the stencil into new range
    JS::CompileOptions options(cx);
    RefPtr<JS::Stencil> stencil;
    JS::TranscodeResult res = JS::DecodeStencil(cx, options, range, stencil);
    CHECK(res == JS::TranscodeResult::Ok);

    // Instantiate and Run
    JS::RootedScript script(cx,
                            JS::InstantiateGlobalStencil(cx, options, stencil));
    JS::RootedValue rval(cx);
    CHECK(script);
    CHECK(JS_ExecuteScript(cx, script, &rval));
    CHECK(rval.isNumber() && rval.toNumber() == 42);
  }

  return true;
}
static bool TestGetBuildId(JS::BuildIdCharVector* buildId) {
  const char buildid[] = "testXDR";
  return buildId->append(buildid, sizeof(buildid));
}
END_TEST(testStencil_Transcode)
