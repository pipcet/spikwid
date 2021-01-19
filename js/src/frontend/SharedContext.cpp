/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/SharedContext.h"

#include "mozilla/RefPtr.h"

#include "frontend/AbstractScopePtr.h"
#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/ModuleSharedContext.h"
#include "vm/FunctionFlags.h"          // js::FunctionFlags
#include "vm/GeneratorAndAsyncKind.h"  // js::GeneratorKind, js::FunctionAsyncKind
#include "vm/JSScript.h"  // js::FillImmutableFlagsFromCompileOptionsForTopLevel, js::FillImmutableFlagsFromCompileOptionsForFunction
#include "vm/StencilEnums.h"  // ImmutableScriptFlagsEnum
#include "wasm/AsmJS.h"
#include "wasm/WasmModule.h"

#include "frontend/ParseContext-inl.h"
#include "vm/EnvironmentObject-inl.h"

namespace js {
namespace frontend {

SharedContext::SharedContext(JSContext* cx, Kind kind,
                             CompilationStencil& stencil, Directives directives,
                             SourceExtent extent)
    : cx_(cx),
      stencil_(stencil),
      extent_(extent),
      allowNewTarget_(false),
      allowSuperProperty_(false),
      allowSuperCall_(false),
      allowArguments_(true),
      inWith_(false),
      inClass_(false),
      localStrict(false),
      hasExplicitUseStrict_(false),
      isScriptFieldCopiedToStencil(false) {
  // Compute the script kind "input" flags.
  if (kind == Kind::FunctionBox) {
    setFlag(ImmutableFlags::IsFunction);
  } else if (kind == Kind::Module) {
    MOZ_ASSERT(!stencil.input.options.nonSyntacticScope);
    setFlag(ImmutableFlags::IsModule);
  } else if (kind == Kind::Eval) {
    setFlag(ImmutableFlags::IsForEval);
  } else {
    MOZ_ASSERT(kind == Kind::Global);
  }

  // Note: This is a mix of transitive and non-transitive options.
  const JS::ReadOnlyCompileOptions& options = stencil.input.options;

  // Initialize the transitive "input" flags. These are applied to all
  // SharedContext in this compilation and generally cannot be determined from
  // the source text alone.
  if (isTopLevelContext()) {
    js::FillImmutableFlagsFromCompileOptionsForTopLevel(options,
                                                        immutableFlags_);
  } else {
    js::FillImmutableFlagsFromCompileOptionsForFunction(options,
                                                        immutableFlags_);
  }

  // Initialize the strict flag. This may be updated by the parser as we observe
  // further directives in the body.
  setFlag(ImmutableFlags::Strict, directives.strict());
}

void ScopeContext::computeThisEnvironment(Scope* scope) {
  uint32_t envCount = 0;
  for (ScopeIter si(scope); si; si++) {
    if (si.kind() == ScopeKind::Function) {
      JSFunction* fun = si.scope()->as<FunctionScope>().canonicalFunction();

      // Arrow function inherit the "this" environment of the enclosing script,
      // so continue ignore them.
      if (!fun->isArrow()) {
        allowNewTarget = true;

        if (fun->allowSuperProperty()) {
          allowSuperProperty = true;
          enclosingThisEnvironmentHops = envCount;
        }

        if (fun->isClassConstructor()) {
          memberInitializers =
              mozilla::Some(fun->baseScript()->getMemberInitializers());
          MOZ_ASSERT(memberInitializers->valid);
        }

        if (fun->isDerivedClassConstructor()) {
          allowSuperCall = true;
        }

        if (fun->isFieldInitializer()) {
          allowArguments = false;
        }

        // Found the effective "this" environment, so stop.
        return;
      }
    }

    if (si.scope()->hasEnvironment()) {
      envCount++;
    }
  }
}

void ScopeContext::computeThisBinding(Scope* scope) {
  // Inspect the scope-chain.
  for (ScopeIter si(scope); si; si++) {
    if (si.kind() == ScopeKind::Module) {
      thisBinding = ThisBinding::Module;
      return;
    }

    if (si.kind() == ScopeKind::Function) {
      JSFunction* fun = si.scope()->as<FunctionScope>().canonicalFunction();

      // Arrow functions don't have their own `this` binding.
      if (fun->isArrow()) {
        continue;
      }

      // Derived class constructors (and their nested arrow functions and evals)
      // use ThisBinding::DerivedConstructor, which ensures TDZ checks happen
      // when accessing |this|.
      if (fun->isDerivedClassConstructor()) {
        thisBinding = ThisBinding::DerivedConstructor;
      } else {
        thisBinding = ThisBinding::Function;
      }

      return;
    }
  }

  thisBinding = ThisBinding::Global;
}

void ScopeContext::computeInScope(Scope* scope) {
  for (ScopeIter si(scope); si; si++) {
    if (si.kind() == ScopeKind::ClassBody) {
      inClass = true;
    }

    if (si.kind() == ScopeKind::With) {
      inWith = true;
    }
  }
}

/* static */
Scope* ScopeContext::determineEffectiveScope(Scope* scope,
                                             JSObject* environment) {
  // If the scope-chain is non-syntactic, we may still determine a more precise
  // effective-scope to use instead.
  if (environment && scope->hasOnChain(ScopeKind::NonSyntactic)) {
    JSObject* env = environment;
    while (env) {
      // Look at target of any DebugEnvironmentProxy, but be sure to use
      // enclosingEnvironment() of the proxy itself.
      JSObject* unwrapped = env;
      if (env->is<DebugEnvironmentProxy>()) {
        unwrapped = &env->as<DebugEnvironmentProxy>().environment();
      }

      if (unwrapped->is<CallObject>()) {
        JSFunction* callee = &unwrapped->as<CallObject>().callee();
        return callee->nonLazyScript()->bodyScope();
      }

      env = env->enclosingEnvironment();
    }
  }

  return scope;
}

GlobalSharedContext::GlobalSharedContext(JSContext* cx, ScopeKind scopeKind,
                                         CompilationStencil& stencil,
                                         Directives directives,
                                         SourceExtent extent)
    : SharedContext(cx, Kind::Global, stencil, directives, extent),
      scopeKind_(scopeKind),
      bindings(nullptr) {
  MOZ_ASSERT(scopeKind == ScopeKind::Global ||
             scopeKind == ScopeKind::NonSyntactic);
  MOZ_ASSERT(thisBinding_ == ThisBinding::Global);
}

EvalSharedContext::EvalSharedContext(JSContext* cx, CompilationStencil& stencil,
                                     CompilationState& compilationState,
                                     SourceExtent extent)
    : SharedContext(cx, Kind::Eval, stencil, compilationState.directives,
                    extent),
      bindings(nullptr) {
  // Eval inherits syntax and binding rules from enclosing environment.
  allowNewTarget_ = compilationState.scopeContext.allowNewTarget;
  allowSuperProperty_ = compilationState.scopeContext.allowSuperProperty;
  allowSuperCall_ = compilationState.scopeContext.allowSuperCall;
  allowArguments_ = compilationState.scopeContext.allowArguments;
  thisBinding_ = compilationState.scopeContext.thisBinding;
  inWith_ = compilationState.scopeContext.inWith;
}

SuspendableContext::SuspendableContext(JSContext* cx, Kind kind,
                                       CompilationStencil& stencil,
                                       Directives directives,
                                       SourceExtent extent, bool isGenerator,
                                       bool isAsync)
    : SharedContext(cx, kind, stencil, directives, extent) {
  setFlag(ImmutableFlags::IsGenerator, isGenerator);
  setFlag(ImmutableFlags::IsAsync, isAsync);
}

FunctionBox::FunctionBox(JSContext* cx, SourceExtent extent,
                         CompilationStencil& stencil,
                         CompilationState& compilationState,
                         Directives directives, GeneratorKind generatorKind,
                         FunctionAsyncKind asyncKind, const ParserAtom* atom,
                         FunctionFlags flags, ScriptIndex index)
    : SuspendableContext(cx, Kind::FunctionBox, stencil, directives, extent,
                         generatorKind == GeneratorKind::Generator,
                         asyncKind == FunctionAsyncKind::AsyncFunction),
      compilationState_(compilationState),
      atom_(atom),
      funcDataIndex_(index),
      flags_(FunctionFlags::clearMutableflags(flags)),
      emitBytecode(false),
      wasEmitted_(false),
      isAnnexB(false),
      useAsm(false),
      hasParameterExprs(false),
      hasDestructuringArgs(false),
      hasDuplicateParameters(false),
      hasExprBody_(false),
      isFunctionFieldCopiedToStencil(false) {}

void FunctionBox::initFromLazyFunction(JSFunction* fun) {
  BaseScript* lazy = fun->baseScript();
  immutableFlags_ = lazy->immutableFlags();
  extent_ = lazy->extent();
}

void FunctionBox::initWithEnclosingParseContext(ParseContext* enclosing,
                                                FunctionFlags flags,
                                                FunctionSyntaxKind kind) {
  SharedContext* sc = enclosing->sc();

  // HasModuleGoal and useAsm are inherited from enclosing context.
  useAsm = sc->isFunctionBox() && sc->asFunctionBox()->useAsmOrInsideUseAsm();
  setHasModuleGoal(sc->hasModuleGoal());

  // Arrow functions don't have their own `this` binding.
  if (flags.isArrow()) {
    allowNewTarget_ = sc->allowNewTarget();
    allowSuperProperty_ = sc->allowSuperProperty();
    allowSuperCall_ = sc->allowSuperCall();
    allowArguments_ = sc->allowArguments();
    thisBinding_ = sc->thisBinding();
  } else {
    if (IsConstructorKind(kind)) {
      auto stmt =
          enclosing->findInnermostStatement<ParseContext::ClassStatement>();
      MOZ_ASSERT(stmt);
      stmt->constructorBox = this;
    }

    allowNewTarget_ = true;
    allowSuperProperty_ = flags.allowSuperProperty();

    if (kind == FunctionSyntaxKind::DerivedClassConstructor) {
      setDerivedClassConstructor();
      allowSuperCall_ = true;
      thisBinding_ = ThisBinding::DerivedConstructor;
    } else {
      thisBinding_ = ThisBinding::Function;
    }

    if (kind == FunctionSyntaxKind::FieldInitializer) {
      setFieldInitializer();
      allowArguments_ = false;
    }
  }

  if (sc->inWith()) {
    inWith_ = true;
  } else {
    auto isWith = [](ParseContext::Statement* stmt) {
      return stmt->kind() == StatementKind::With;
    };

    inWith_ = enclosing->findInnermostStatement(isWith);
  }

  if (sc->inClass()) {
    inClass_ = true;
  } else {
    auto isClass = [](ParseContext::Statement* stmt) {
      return stmt->kind() == StatementKind::Class;
    };

    inClass_ = enclosing->findInnermostStatement(isClass);
  }
}

void FunctionBox::initStandalone(ScopeContext& scopeContext,
                                 FunctionFlags flags, FunctionSyntaxKind kind) {
  if (flags.isArrow()) {
    allowNewTarget_ = scopeContext.allowNewTarget;
    allowSuperProperty_ = scopeContext.allowSuperProperty;
    allowSuperCall_ = scopeContext.allowSuperCall;
    allowArguments_ = scopeContext.allowArguments;
    thisBinding_ = scopeContext.thisBinding;
  } else {
    allowNewTarget_ = true;
    allowSuperProperty_ = flags.allowSuperProperty();

    if (kind == FunctionSyntaxKind::DerivedClassConstructor) {
      setDerivedClassConstructor();
      allowSuperCall_ = true;
      thisBinding_ = ThisBinding::DerivedConstructor;
    } else {
      thisBinding_ = ThisBinding::Function;
    }

    if (kind == FunctionSyntaxKind::FieldInitializer) {
      setFieldInitializer();
      allowArguments_ = false;
    }
  }

  inWith_ = scopeContext.inWith;
  inClass_ = scopeContext.inClass;
}

void FunctionBox::setEnclosingScopeForInnerLazyFunction(ScopeIndex scopeIndex) {
  // For lazy functions inside a function which is being compiled, we cache
  // the incomplete scope object while compiling, and store it to the
  // BaseScript once the enclosing script successfully finishes compilation
  // in FunctionBox::finish.
  MOZ_ASSERT(enclosingScopeIndex_.isNothing());
  enclosingScopeIndex_ = mozilla::Some(scopeIndex);
  if (isFunctionFieldCopiedToStencil) {
    copyUpdatedEnclosingScopeIndex();
  }
}

bool FunctionBox::setAsmJSModule(const JS::WasmModule* module) {
  MOZ_ASSERT(!isFunctionFieldCopiedToStencil);

  MOZ_ASSERT(flags_.kind() == FunctionFlags::NormalFunction);

  // Update flags we will use to allocate the JSFunction.
  flags_.clearBaseScript();
  flags_.setIsExtended();
  flags_.setKind(FunctionFlags::AsmJS);

  if (!stencil_.asmJS.putNew(index(), module)) {
    js::ReportOutOfMemory(cx_);
    return false;
  }
  return true;
}

ModuleSharedContext::ModuleSharedContext(JSContext* cx,
                                         CompilationStencil& stencil,
                                         ModuleBuilder& builder,
                                         SourceExtent extent)
    : SuspendableContext(cx, Kind::Module, stencil, Directives(true), extent,
                         /* isGenerator = */ false,
                         /* isAsync = */ false),
      bindings(nullptr),
      builder(builder) {
  thisBinding_ = ThisBinding::Module;
  setFlag(ImmutableFlags::HasModuleGoal);
}

ScriptStencil& FunctionBox::functionStencil() const {
  return compilationState_.scriptData[funcDataIndex_];
}

ScriptStencilExtra& FunctionBox::functionExtraStencil() const {
  return compilationState_.scriptExtra[funcDataIndex_];
}

bool FunctionBox::hasFunctionExtraStencil() const {
  return funcDataIndex_ < compilationState_.scriptExtra.length();
}

void SharedContext::copyScriptFields(ScriptStencil& script) {
  MOZ_ASSERT(!isScriptFieldCopiedToStencil);
  isScriptFieldCopiedToStencil = true;
}

void SharedContext::copyScriptExtraFields(ScriptStencilExtra& scriptExtra) {
  scriptExtra.immutableFlags = immutableFlags_;
  scriptExtra.extent = extent_;
}

void FunctionBox::finishScriptFlags() {
  MOZ_ASSERT(!isScriptFieldCopiedToStencil);

  using ImmutableFlags = ImmutableScriptFlagsEnum;
  immutableFlags_.setFlag(ImmutableFlags::HasMappedArgsObj, hasMappedArgsObj());
}

void FunctionBox::copyScriptFields(ScriptStencil& script) {
  MOZ_ASSERT(&script == &functionStencil());

  SharedContext::copyScriptFields(script);

  if (memberInitializers_) {
    script.setMemberInitializers(*memberInitializers_);
  }

  isScriptFieldCopiedToStencil = true;
}

void FunctionBox::copyFunctionFields(ScriptStencil& script) {
  MOZ_ASSERT(&script == &functionStencil());
  MOZ_ASSERT(!isFunctionFieldCopiedToStencil);

  if (atom_) {
    atom_->markUsedByStencil();
    script.functionAtom = atom_->toIndex();
  }
  script.functionFlags = flags_;
  if (enclosingScopeIndex_) {
    script.setLazyFunctionEnclosingScopeIndex(*enclosingScopeIndex_);
  }
  if (wasEmitted_) {
    script.setWasFunctionEmitted();
  }

  isFunctionFieldCopiedToStencil = true;
}

void FunctionBox::copyFunctionExtraFields(ScriptStencilExtra& scriptExtra) {
  scriptExtra.nargs = nargs_;
}

void FunctionBox::copyUpdatedImmutableFlags() {
  if (hasFunctionExtraStencil()) {
    ScriptStencilExtra& scriptExtra = functionExtraStencil();
    scriptExtra.immutableFlags = immutableFlags_;
  }
}

void FunctionBox::copyUpdatedExtent() {
  ScriptStencilExtra& scriptExtra = functionExtraStencil();
  scriptExtra.extent = extent_;
}

void FunctionBox::copyUpdatedMemberInitializers() {
  ScriptStencil& script = functionStencil();
  if (memberInitializers_) {
    script.setMemberInitializers(*memberInitializers_);
  }
}

void FunctionBox::copyUpdatedEnclosingScopeIndex() {
  ScriptStencil& script = functionStencil();
  if (enclosingScopeIndex_) {
    script.setLazyFunctionEnclosingScopeIndex(*enclosingScopeIndex_);
  }
}

void FunctionBox::copyUpdatedAtomAndFlags() {
  ScriptStencil& script = functionStencil();
  if (atom_) {
    atom_->markUsedByStencil();
    script.functionAtom = atom_->toIndex();
  }
  script.functionFlags = flags_;
}

void FunctionBox::copyUpdatedWasEmitted() {
  ScriptStencil& script = functionStencil();
  if (wasEmitted_) {
    script.setWasFunctionEmitted();
  }
}

}  // namespace frontend
}  // namespace js
