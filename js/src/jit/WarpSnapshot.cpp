/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/WarpSnapshot.h"

#include "mozilla/IntegerPrintfMacros.h"

#include <type_traits>

#include "jit/CacheIRSpewer.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/Printer.h"

#include "vm/EnvironmentObject-inl.h"

using namespace js;
using namespace js::jit;

static_assert(!std::is_polymorphic_v<WarpOpSnapshot>,
              "WarpOpSnapshot should not have any virtual methods");

WarpSnapshot::WarpSnapshot(JSContext* cx, TempAllocator& alloc,
                           WarpScriptSnapshotList&& scriptSnapshots,
                           const WarpBailoutInfo& bailoutInfo)
    : scriptSnapshots_(std::move(scriptSnapshots)),
      globalLexicalEnv_(&cx->global()->lexicalEnvironment()),
      globalLexicalEnvThis_(globalLexicalEnv_->thisObject()),
      bailoutInfo_(bailoutInfo),
      nurseryObjects_(alloc) {}

WarpScriptSnapshot::WarpScriptSnapshot(
    JSScript* script, const WarpEnvironment& env,
    WarpOpSnapshotList&& opSnapshots, ModuleObject* moduleObject,
    JSObject* instrumentationCallback,
    mozilla::Maybe<int32_t> instrumentationScriptId,
    mozilla::Maybe<bool> instrumentationActive)
    : script_(script),
      environment_(env),
      opSnapshots_(std::move(opSnapshots)),
      moduleObject_(moduleObject),
      instrumentationCallback_(instrumentationCallback),
      instrumentationScriptId_(instrumentationScriptId),
      instrumentationActive_(instrumentationActive),
      isArrowFunction_(script->isFunction() && script->function()->isArrow()) {}

#ifdef JS_JITSPEW
void WarpSnapshot::dump() const {
  Fprinter out(stderr);
  dump(out);
}

void WarpSnapshot::dump(GenericPrinter& out) const {
  out.printf("WarpSnapshot (0x%p)\n", this);
  out.printf("------------------------------\n");
  out.printf("globalLexicalEnv: 0x%p\n", globalLexicalEnv());
  out.printf("globalLexicalEnvThis: 0x%p\n", globalLexicalEnvThis());
  out.printf("failedBoundsCheck: %u\n", bailoutInfo().failedBoundsCheck());
  out.printf("failedLexicalCheck: %u\n", bailoutInfo().failedLexicalCheck());
  out.printf("\n");

  out.printf("Nursery objects (%u):\n", unsigned(nurseryObjects_.length()));
  for (size_t i = 0; i < nurseryObjects_.length(); i++) {
    out.printf("  %u: 0x%p\n", unsigned(i), nurseryObjects_[i]);
  }
  out.printf("\n");

  for (auto* scriptSnapshot : scriptSnapshots_) {
    scriptSnapshot->dump(out);
  }
}

void WarpScriptSnapshot::dump(GenericPrinter& out) const {
  out.printf("WarpScriptSnapshot (0x%p)\n", this);
  out.printf("------------------------------\n");
  out.printf("Script: %s:%u:%u (0x%p)\n", script_->filename(),
             script_->lineno(), script_->column(),
             static_cast<JSScript*>(script_));
  out.printf("  moduleObject: 0x%p\n", moduleObject());
  out.printf("  isArrowFunction: %u\n", isArrowFunction());

  out.printf("  environment: ");
  environment_.match(
      [&](const NoEnvironment&) { out.printf("None\n"); },
      [&](JSObject* obj) { out.printf("Object: 0x%p\n", obj); },
      [&](const FunctionEnvironment& env) {
        out.printf(
            "Function: callobject template 0x%p, named lambda template: 0x%p\n",
            static_cast<JSObject*>(env.callObjectTemplate),
            static_cast<JSObject*>(env.namedLambdaTemplate));
      });

  out.printf("\n");
  for (const WarpOpSnapshot* snapshot : opSnapshots()) {
    snapshot->dump(out, script_);
    out.printf("\n");
  }
}

static const char* OpSnapshotKindString(WarpOpSnapshot::Kind kind) {
  static const char* const names[] = {
#  define NAME(x) #  x,
      WARP_OP_SNAPSHOT_LIST(NAME)
#  undef NAME
  };
  return names[unsigned(kind)];
}

void WarpOpSnapshot::dump(GenericPrinter& out, JSScript* script) const {
  jsbytecode* pc = script->offsetToPC(offset_);
  out.printf("  %s (offset %u, JSOp::%s)\n", OpSnapshotKindString(kind_),
             offset_, CodeName(JSOp(*pc)));

  // Dispatch to dumpData() methods.
  switch (kind_) {
#  define DUMP(kind)             \
    case Kind::kind:             \
      as<kind>()->dumpData(out); \
      break;
    WARP_OP_SNAPSHOT_LIST(DUMP)
#  undef DUMP
  }
}

void WarpArguments::dumpData(GenericPrinter& out) const {
  out.printf("    template: 0x%p\n", templateObj());
}

void WarpRegExp::dumpData(GenericPrinter& out) const {
  out.printf("    hasShared: %u\n", hasShared());
}

void WarpFunctionProto::dumpData(GenericPrinter& out) const {
  out.printf("    proto: 0x%p\n", proto());
}

void WarpGetIntrinsic::dumpData(GenericPrinter& out) const {
  out.printf("    intrinsic: 0x%016" PRIx64 "\n", intrinsic().asRawBits());
}

void WarpGetImport::dumpData(GenericPrinter& out) const {
  out.printf("    targetEnv: 0x%p\n", targetEnv());
  out.printf("    numFixedSlots: %u\n", numFixedSlots());
  out.printf("    slot: %u\n", slot());
  out.printf("    needsLexicalCheck: %u\n", needsLexicalCheck());
}

void WarpLambda::dumpData(GenericPrinter& out) const {
  out.printf("    baseScript: 0x%p\n", baseScript());
  out.printf("    flags: 0x%x\n", unsigned(flags().toRaw()));
  out.printf("    nargs: %u\n", unsigned(nargs_));
}

void WarpRest::dumpData(GenericPrinter& out) const {
  out.printf("    template: 0x%p\n", templateObject());
}

void WarpNewArray::dumpData(GenericPrinter& out) const {
  out.printf("    template: 0x%p\n", templateObject());
  out.printf("    useVMCall: %u\n", useVMCall());
}

void WarpNewObject::dumpData(GenericPrinter& out) const {
  out.printf("    template: 0x%p\n", templateObject());
}

void WarpBindGName::dumpData(GenericPrinter& out) const {
  out.printf("    globalEnv: 0x%p\n", globalEnv());
}

void WarpBailout::dumpData(GenericPrinter& out) const {
  // No fields.
}

void WarpCacheIR::dumpData(GenericPrinter& out) const {
  out.printf("    stubCode: 0x%p\n", static_cast<JitCode*>(stubCode_));
  out.printf("    stubInfo: 0x%p\n", stubInfo_);
  out.printf("    stubData: 0x%p\n", stubData_);
#  ifdef JS_CACHEIR_SPEW
  out.printf("    IR:\n");
  SpewCacheIROps(out, "      ", stubInfo_);
#  else
  out.printf("(CacheIR spew unavailable)\n");
#  endif
}

void WarpInlinedCall::dumpData(GenericPrinter& out) const {
  out.printf("    scriptSnapshot: 0x%p\n", scriptSnapshot_);
  cacheIRSnapshot_->dumpData(out);
  // TODO: dump callInfo
}
#endif  // JS_JITSPEW

template <typename T>
static void TraceWarpGCPtr(JSTracer* trc, WarpGCPtr<T>& thing,
                           const char* name) {
  T thingRaw = thing;
  TraceManuallyBarrieredEdge(trc, &thingRaw, name);
  MOZ_ASSERT(static_cast<T>(thing) == thingRaw, "Unexpected moving GC!");
}

void WarpSnapshot::trace(JSTracer* trc) {
  for (auto* script : scriptSnapshots_) {
    script->trace(trc);
  }
  TraceWarpGCPtr(trc, globalLexicalEnv_, "warp-lexical");
  TraceWarpGCPtr(trc, globalLexicalEnvThis_, "warp-lexicalthis");

  // Note: nursery objects can be moved in parallel with Warp compilation,
  // so don't use TraceWarpGCPtr here as that asserts non-moving.
  for (size_t i = 0; i < nurseryObjects_.length(); i++) {
    TraceManuallyBarrieredEdge(trc, &nurseryObjects_[i], "warp-nursery-object");
  }
}

void WarpScriptSnapshot::trace(JSTracer* trc) {
  TraceWarpGCPtr(trc, script_, "warp-script");

  environment_.match(
      [](const NoEnvironment&) {},
      [trc](WarpGCPtr<JSObject*>& obj) {
        TraceWarpGCPtr(trc, obj, "warp-env-object");
      },
      [trc](FunctionEnvironment& env) {
        if (env.callObjectTemplate) {
          TraceWarpGCPtr(trc, env.callObjectTemplate, "warp-env-callobject");
        }
        if (env.namedLambdaTemplate) {
          TraceWarpGCPtr(trc, env.namedLambdaTemplate, "warp-env-namedlambda");
        }
      });

  for (WarpOpSnapshot* snapshot : opSnapshots_) {
    snapshot->trace(trc);
  }

  if (moduleObject_) {
    TraceWarpGCPtr(trc, moduleObject_, "warp-module-obj");
  }
  if (instrumentationCallback_) {
    TraceWarpGCPtr(trc, instrumentationCallback_, "warp-instr-callback");
  }
}

void WarpOpSnapshot::trace(JSTracer* trc) {
  // Dispatch to traceData() methods.
  switch (kind_) {
#define TRACE(kind)             \
  case Kind::kind:              \
    as<kind>()->traceData(trc); \
    break;
    WARP_OP_SNAPSHOT_LIST(TRACE)
#undef TRACE
  }
}

void WarpArguments::traceData(JSTracer* trc) {
  if (templateObj_) {
    TraceWarpGCPtr(trc, templateObj_, "warp-args-template");
  }
}

void WarpRegExp::traceData(JSTracer* trc) {
  // No GC pointers.
}

void WarpFunctionProto::traceData(JSTracer* trc) {
  TraceWarpGCPtr(trc, proto_, "warp-function-proto");
}

void WarpGetIntrinsic::traceData(JSTracer* trc) {
  TraceWarpGCPtr(trc, intrinsic_, "warp-intrinsic");
}

void WarpGetImport::traceData(JSTracer* trc) {
  TraceWarpGCPtr(trc, targetEnv_, "warp-import-env");
}

void WarpLambda::traceData(JSTracer* trc) {
  TraceWarpGCPtr(trc, baseScript_, "warp-lambda-basescript");
}

void WarpRest::traceData(JSTracer* trc) {
  TraceWarpGCPtr(trc, templateObject_, "warp-rest-template");
}

void WarpNewArray::traceData(JSTracer* trc) {
  TraceWarpGCPtr(trc, templateObject_, "warp-newarray-template");
}

void WarpNewObject::traceData(JSTracer* trc) {
  TraceWarpGCPtr(trc, templateObject_, "warp-newobject-template");
}

void WarpBindGName::traceData(JSTracer* trc) {
  TraceWarpGCPtr(trc, globalEnv_, "warp-bindgname-globalenv");
}

void WarpBailout::traceData(JSTracer* trc) {
  // No GC pointers.
}

void WarpCacheIR::traceData(JSTracer* trc) {
  TraceWarpGCPtr(trc, stubCode_, "warp-stub-code");

  // TODO: trace pointers in stub data. Beware of nursery indexes in the stub
  // data. See WarpObjectField.
}

void WarpInlinedCall::traceData(JSTracer* trc) {
  // Note: scriptSnapshot_ is traced through WarpSnapshot.
  cacheIRSnapshot_->trace(trc);
}
