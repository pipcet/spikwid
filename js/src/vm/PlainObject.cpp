/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS object implementation.
 */

#include "vm/PlainObject-inl.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "jspubtd.h"  // JSProto_Object

#include "ds/IdValuePair.h"  // js::IdValuePair
#include "gc/AllocKind.h"    // js::gc::AllocKind
#include "vm/JSContext.h"    // JSContext
#include "vm/JSFunction.h"   // JSFunction
#include "vm/JSObject.h"     // JSObject, js::GetPrototypeFromConstructor
#include "vm/TaggedProto.h"  // js::TaggedProto

#include "vm/JSObject-inl.h"  // js::NewObjectWithGroup, js::NewObjectGCKind

using namespace js;

using JS::Handle;
using JS::Rooted;

PlainObject* js::CreateThisForFunction(JSContext* cx,
                                       Handle<JSFunction*> callee,
                                       Handle<JSObject*> newTarget,
                                       NewObjectKind newKind) {
  MOZ_ASSERT(cx->realm() == callee->realm());
  MOZ_ASSERT(!callee->constructorNeedsUninitializedThis());

  Rooted<JSObject*> proto(cx);
  if (!GetPrototypeFromConstructor(cx, newTarget, JSProto_Object, &proto)) {
    return nullptr;
  }

  PlainObject* res;
  if (proto) {
    js::gc::AllocKind allocKind = NewObjectGCKind();
    res = NewObjectWithGivenProtoAndKinds<PlainObject>(cx, proto, allocKind,
                                                       newKind);
  } else {
    res = NewBuiltinClassInstanceWithKind<PlainObject>(cx, newKind);
  }

  MOZ_ASSERT_IF(res, res->nonCCWRealm() == callee->realm());

  return res;
}

#ifdef DEBUG
void PlainObject::assertHasNoNonWritableOrAccessorPropExclProto() const {
  // Check the most recent MaxCount properties to not slow down debug builds too
  // much.
  static constexpr size_t MaxCount = 8;

  size_t count = 0;
  PropertyName* protoName = runtimeFromMainThread()->commonNames->proto;

  for (Shape::Range<NoGC> r(lastProperty()); !r.empty(); r.popFront()) {
    Shape& propShape = r.front();
    jsid id = propShape.propidRaw();

    // __proto__ is always allowed.
    if (JSID_IS_ATOM(id, protoName)) {
      continue;
    }

    MOZ_ASSERT(propShape.isDataProperty());
    MOZ_ASSERT(propShape.writable());

    count++;
    if (count > MaxCount) {
      return;
    }
  }
}
#endif

static bool AddPlainObjectProperties(JSContext* cx, HandlePlainObject obj,
                                     IdValuePair* properties,
                                     size_t nproperties) {
  RootedId propid(cx);
  RootedValue value(cx);

  for (size_t i = 0; i < nproperties; i++) {
    propid = properties[i].id;
    value = properties[i].value;
    if (!NativeDefineDataProperty(cx, obj, propid, value, JSPROP_ENUMERATE)) {
      return false;
    }
  }

  return true;
}

PlainObject* js::NewPlainObjectWithProperties(JSContext* cx,
                                              IdValuePair* properties,
                                              size_t nproperties,
                                              NewObjectKind newKind) {
  gc::AllocKind allocKind = gc::GetGCObjectKind(nproperties);
  RootedPlainObject obj(
      cx, NewBuiltinClassInstance<PlainObject>(cx, allocKind, newKind));
  if (!obj || !AddPlainObjectProperties(cx, obj, properties, nproperties)) {
    return nullptr;
  }
  return obj;
}
