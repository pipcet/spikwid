/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/JSActorManager.h"
#include "mozilla/dom/JSActorService.h"
#include "mozJSComponentLoader.h"
#include "jsapi.h"

namespace mozilla {
namespace dom {

already_AddRefed<JSActor> JSActorManager::GetActor(const nsACString& aName,
                                                   ErrorResult& aRv) {
  MOZ_ASSERT(nsContentUtils::IsSafeToRunScript());

  // If our connection has been closed, return an error.
  mozilla::ipc::IProtocol* nativeActor = AsNativeActor();
  if (!nativeActor->CanSend()) {
    aRv.ThrowInvalidStateError(nsPrintfCString(
        "Cannot get actor '%s'. Native '%s' actor is destroyed.",
        PromiseFlatCString(aName).get(), nativeActor->GetProtocolName()));
    return nullptr;
  }

  // Check if this actor has already been created, and return it if it has.
  if (RefPtr<JSActor> actor = mJSActors.Get(aName)) {
    return actor.forget();
  }

  RefPtr<JSActorService> actorSvc = JSActorService::GetSingleton();
  if (!actorSvc) {
    aRv.ThrowInvalidStateError("JSActorService hasn't been initialized");
    return nullptr;
  }

  // Check if this actor satisfies the requirements of the protocol
  // corresponding to `aName`, and get the module which implements it.
  RefPtr<JSActorProtocol> protocol =
      MatchingJSActorProtocol(actorSvc, aName, aRv);
  if (!protocol) {
    return nullptr;
  }

  bool isParent = nativeActor->GetSide() == mozilla::ipc::ParentSide;
  auto& side = isParent ? protocol->Parent() : protocol->Child();

  // Constructing an actor requires a running script, so push an AutoEntryScript
  // onto the stack.
  AutoEntryScript aes(xpc::PrivilegedJunkScope(), "JSActor construction");
  JSContext* cx = aes.cx();

  // Load the module using mozJSComponentLoader.
  RefPtr<mozJSComponentLoader> loader = mozJSComponentLoader::Get();
  MOZ_ASSERT(loader);

  // If a module URI was provided, use it to construct an instance of the actor.
  JS::RootedObject actorObj(cx);
  if (side.mModuleURI) {
    JS::RootedObject global(cx);
    JS::RootedObject exports(cx);
    aRv = loader->Import(cx, side.mModuleURI.ref(), &global, &exports);
    if (aRv.Failed()) {
      return nullptr;
    }
    MOZ_ASSERT(exports, "null exports!");

    // Load the specific property from our module.
    JS::RootedValue ctor(cx);
    nsAutoCString ctorName(aName);
    ctorName.Append(isParent ? "Parent"_ns : "Child"_ns);
    if (!JS_GetProperty(cx, exports, ctorName.get(), &ctor)) {
      aRv.NoteJSContextException(cx);
      return nullptr;
    }

    if (NS_WARN_IF(!ctor.isObject())) {
      aRv.ThrowNotFoundError(nsPrintfCString(
          "Could not find actor constructor '%s'", ctorName.get()));
      return nullptr;
    }

    // Invoke the constructor loaded from the module.
    if (!JS::Construct(cx, ctor, JS::HandleValueArray::empty(), &actorObj)) {
      aRv.NoteJSContextException(cx);
      return nullptr;
    }
  }

  // Initialize our newly-constructed actor, and return it.
  RefPtr<JSActor> actor = InitJSActor(actorObj, aName, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  mJSActors.Put(aName, RefPtr{actor});
  return actor.forget();
}

#define CHILD_DIAGNOSTIC_ASSERT(test, msg) \
  do {                                     \
    if (XRE_IsParentProcess()) {           \
      MOZ_ASSERT(test, msg);               \
    } else {                               \
      MOZ_DIAGNOSTIC_ASSERT(test, msg);    \
    }                                      \
  } while (0)

void JSActorManager::ReceiveRawMessage(const JSActorMessageMeta& aMetadata,
                                       ipc::StructuredCloneData&& aData,
                                       ipc::StructuredCloneData&& aStack) {
  MOZ_ASSERT(nsContentUtils::IsSafeToRunScript());

  CrashReporter::AutoAnnotateCrashReport autoActorName(
      CrashReporter::Annotation::JSActorName, aMetadata.actorName());
  CrashReporter::AutoAnnotateCrashReport autoMessageName(
      CrashReporter::Annotation::JSActorMessage,
      NS_LossyConvertUTF16toASCII(aMetadata.messageName()));

  // We're going to be running JS. Enter the privileged junk realm so we can set
  // up our JS state correctly.
  AutoEntryScript aes(xpc::PrivilegedJunkScope(), "JSActor message handler");
  JSContext* cx = aes.cx();

  // Ensure any errors reported to `error` are set on the scope, so they're
  // reported.
  ErrorResult error;
  auto autoSetException =
      MakeScopeExit([&] { Unused << error.MaybeSetPendingException(cx); });

  // If an async stack was provided, set up our async stack state.
  JS::Rooted<JSObject*> stack(cx);
  Maybe<JS::AutoSetAsyncStackForNewCalls> stackSetter;
  {
    JS::Rooted<JS::Value> stackVal(cx);
    aStack.Read(cx, &stackVal, error);
    if (error.Failed()) {
      return;
    }

    if (stackVal.isObject()) {
      stack = &stackVal.toObject();
      if (!js::IsSavedFrame(stack)) {
        CHILD_DIAGNOSTIC_ASSERT(false, "Stack must be a SavedFrame object");
        error.ThrowDataError("Actor async stack must be a SavedFrame object");
        return;
      }
      stackSetter.emplace(cx, stack, "JSActor query");
    }
  }

  RefPtr<JSActor> actor = GetActor(aMetadata.actorName(), error);
  if (error.Failed()) {
    return;
  }

  JS::Rooted<JS::Value> data(cx);
  aData.Read(cx, &data, error);
  if (error.Failed()) {
    CHILD_DIAGNOSTIC_ASSERT(false, "Should not receive non-decodable data");
    return;
  }

  switch (aMetadata.kind()) {
    case JSActorMessageKind::QueryResolve:
    case JSActorMessageKind::QueryReject:
      actor->ReceiveQueryReply(cx, aMetadata, data, error);
      break;

    case JSActorMessageKind::Message:
    case JSActorMessageKind::Query:
      actor->ReceiveMessageOrQuery(cx, aMetadata, data, error);
      break;

    default:
      MOZ_ASSERT_UNREACHABLE();
  }
}

void JSActorManager::JSActorWillDestroy() {
  MOZ_ASSERT(nsContentUtils::IsSafeToRunScript());
  CrashReporter::AutoAnnotateCrashReport autoMessageName(
      CrashReporter::Annotation::JSActorMessage, "<WillDestroy>"_ns);

  // Make a copy so that we can avoid potential iterator invalidation when
  // calling the user-provided Destroy() methods.
  nsTArray<RefPtr<JSActor>> actors(mJSActors.Count());
  for (auto& entry : mJSActors) {
    actors.AppendElement(entry.GetData());
  }
  for (auto& actor : actors) {
    CrashReporter::AutoAnnotateCrashReport autoActorName(
        CrashReporter::Annotation::JSActorName, actor->Name());
    actor->StartDestroy();
  }
}

void JSActorManager::JSActorDidDestroy() {
  MOZ_ASSERT(nsContentUtils::IsSafeToRunScript());
  CrashReporter::AutoAnnotateCrashReport autoMessageName(
      CrashReporter::Annotation::JSActorMessage, "<DidDestroy>"_ns);

  // Swap the table with `mJSActors` so that we don't invalidate it while
  // iterating.
  nsRefPtrHashtable<nsCStringHashKey, JSActor> actors;
  mJSActors.SwapElements(actors);
  for (auto& entry : actors) {
    CrashReporter::AutoAnnotateCrashReport autoActorName(
        CrashReporter::Annotation::JSActorName, entry.GetData()->Name());
    entry.GetData()->AfterDestroy();
  }
}

void JSActorManager::JSActorUnregister(const nsACString& aName) {
  MOZ_ASSERT(nsContentUtils::IsSafeToRunScript());

  RefPtr<JSActor> actor;
  if (mJSActors.Remove(aName, getter_AddRefs(actor))) {
    actor->AfterDestroy();
  }
}

}  // namespace dom
}  // namespace mozilla
