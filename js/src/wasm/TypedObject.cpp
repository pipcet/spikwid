/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "wasm/TypedObject-inl.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Casting.h"
#include "mozilla/CheckedInt.h"

#include <algorithm>

#include "gc/Marking.h"
#include "js/CharacterEncoding.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "js/Vector.h"
#include "util/StringBuffer.h"
#include "vm/GlobalObject.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Realm.h"
#include "vm/SelfHosting.h"
#include "vm/StringType.h"
#include "vm/TypedArrayObject.h"
#include "vm/Uint8Clamped.h"

#include "wasm/WasmTypes.h"  // WasmValueBox
#include "gc/Marking-inl.h"
#include "gc/Nursery-inl.h"
#include "gc/StoreBuffer-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Shape-inl.h"

using mozilla::AssertedCast;
using mozilla::CheckedInt32;
using mozilla::IsPowerOfTwo;
using mozilla::PodCopy;
using mozilla::PointerRangeSize;

using namespace js;
using namespace wasm;

/***************************************************************************
 * Typed Prototypes
 *
 * Every type descriptor has an associated prototype. Instances of
 * that type descriptor use this as their prototype. Per the spec,
 * typed object prototypes cannot be mutated.
 */

const JSClass js::TypedProto::class_ = {"TypedProto"};

TypedProto* TypedProto::create(JSContext* cx) {
  Handle<GlobalObject*> global = cx->global();
  RootedObject objProto(cx,
                        GlobalObject::getOrCreateObjectPrototype(cx, global));
  if (!objProto) {
    return nullptr;
  }

  return NewTenuredObjectWithGivenProto<TypedProto>(cx, objProto);
}

static const JSClassOps TypeDescrClassOps = {
    nullptr,              // addProperty
    nullptr,              // delProperty
    nullptr,              // enumerate
    nullptr,              // newEnumerate
    nullptr,              // resolve
    nullptr,              // mayResolve
    TypeDescr::finalize,  // finalize
    nullptr,              // call
    nullptr,              // hasInstance
    nullptr,              // construct
    nullptr,              // trace
};

const JSClass js::TypeDescr::class_ = {
    "TypeDescr",
    JSCLASS_HAS_RESERVED_SLOTS(TypeDescr::SlotCount) |
        JSCLASS_BACKGROUND_FINALIZE,
    &TypeDescrClassOps};

static bool CreateTraceList(JSContext* cx, HandleTypeDescr descr);

TypeDescr* TypeDescr::createFromHandle(JSContext* cx, TypeHandle handle) {
  const TypeDef& type = handle.get(cx->wasm().typeContext.get());
  MOZ_ASSERT(type.isStructType());
  const StructType& structType = type.structType();

  Rooted<TypeDescr*> descr(
      cx, NewTenuredObjectWithGivenProto<TypeDescr>(cx, nullptr));
  if (!descr) {
    return nullptr;
  }

  Rooted<TypedProto*> proto(cx, TypedProto::create(cx));
  if (!proto) {
    return nullptr;
  }

  descr->initReservedSlot(TypeDescr::Handle, Int32Value(handle.index()));
  descr->initReservedSlot(TypeDescr::Size, Int32Value(structType.size_));
  descr->initReservedSlot(TypeDescr::Proto, ObjectValue(*proto));
  descr->initReservedSlot(TypeDescr::TraceList, UndefinedValue());

  if (!CreateTraceList(cx, descr)) {
    return nullptr;
  }

  if (!cx->zone()->addTypeDescrObject(cx, descr)) {
    ReportOutOfMemory(cx);
    return nullptr;
  }

  return descr;
}

/******************************************************************************
 * Typed objects
 */

uint32_t TypedObject::offset() const {
  if (is<InlineTypedObject>()) {
    return 0;
  }
  return PointerRangeSize(typedMemBase(), typedMem());
}

uint8_t* TypedObject::typedMem() const {
  if (is<InlineTypedObject>()) {
    return as<InlineTypedObject>().inlineTypedMem();
  }
  return as<OutlineTypedObject>().outOfLineTypedMem();
}

uint8_t* TypedObject::typedMemBase() const {
  MOZ_ASSERT(is<OutlineTypedObject>());

  JSObject& owner = as<OutlineTypedObject>().owner();
  if (owner.is<ArrayBufferObject>()) {
    return owner.as<ArrayBufferObject>().dataPointer();
  }
  return owner.as<InlineTypedObject>().inlineTypedMem();
}

/******************************************************************************
 * Outline typed objects
 */

void OutlineTypedObject::setOwnerAndData(JSObject* owner, uint8_t* data) {
  // Typed objects cannot move from one owner to another, so don't worry
  // about pre barriers during this initialization.
  owner_ = owner;
  data_ = data;

  if (owner) {
    if (!IsInsideNursery(this) && IsInsideNursery(owner)) {
      // Trigger a post barrier when attaching an object outside the nursery to
      // one that is inside it.
      owner->storeBuffer()->putWholeCell(this);
    } else if (IsInsideNursery(this) && !IsInsideNursery(owner)) {
      // ...and also when attaching an object inside the nursery to one that is
      // outside it, for a subtle reason -- the outline object now points to
      // the memory owned by 'owner', and can modify object/string references
      // stored in that memory, potentially storing nursery pointers in it. If
      // the outline object is in the nursery, then the post barrier will do
      // nothing; you will be writing a nursery pointer "into" a nursery
      // object. But that will result in the tenured owner's data containing a
      // nursery pointer, and thus we need a store buffer edge. Since we can't
      // catch the actual write, register the owner preemptively now.
      storeBuffer()->putWholeCell(owner);
    }
  }
}

/*static*/
OutlineTypedObject* OutlineTypedObject::createUnattached(JSContext* cx,
                                                         HandleTypeDescr descr,
                                                         gc::InitialHeap heap) {
  AutoSetNewObjectMetadata metadata(cx);

  RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(
                                  cx, &OutlineTypedObject::class_,
                                  TaggedProto(&descr->typedProto()), descr));
  if (!group) {
    return nullptr;
  }

  NewObjectKind newKind =
      (heap == gc::TenuredHeap) ? TenuredObject : GenericObject;
  OutlineTypedObject* obj = NewObjectWithGroup<OutlineTypedObject>(
      cx, group, gc::AllocKind::OBJECT0, newKind);
  if (!obj) {
    return nullptr;
  }

  obj->setOwnerAndData(nullptr, nullptr);
  return obj;
}

void OutlineTypedObject::attach(ArrayBufferObject& buffer) {
  MOZ_ASSERT(size() <= wasm::ByteLength32(buffer));
  MOZ_ASSERT(buffer.hasTypedObjectViews());
  MOZ_ASSERT(!buffer.isDetached());

  setOwnerAndData(&buffer, buffer.dataPointer());
}

/*static*/
OutlineTypedObject* OutlineTypedObject::createZeroed(JSContext* cx,
                                                     HandleTypeDescr descr,
                                                     gc::InitialHeap heap) {
  // Create unattached wrapper object.
  Rooted<OutlineTypedObject*> obj(
      cx, OutlineTypedObject::createUnattached(cx, descr, heap));
  if (!obj) {
    return nullptr;
  }

  // Allocate and initialize the memory for this instance.
  size_t totalSize = descr->size();
  Rooted<ArrayBufferObject*> buffer(cx);
  buffer = ArrayBufferObject::createForTypedObject(cx, BufferSize(totalSize));
  if (!buffer) {
    return nullptr;
  }
  descr->initInstance(cx, buffer->dataPointer());
  obj->attach(*buffer);
  return obj;
}

/*static*/
TypedObject* TypedObject::createZeroed(JSContext* cx, HandleTypeDescr descr,
                                       gc::InitialHeap heap) {
  // If possible, create an object with inline data.
  if (InlineTypedObject::canAccommodateType(descr)) {
    AutoSetNewObjectMetadata metadata(cx);

    InlineTypedObject* obj = InlineTypedObject::create(cx, descr, heap);
    if (!obj) {
      return nullptr;
    }
    JS::AutoCheckCannotGC nogc(cx);
    descr->initInstance(cx, obj->inlineTypedMem(nogc));
    return obj;
  }

  return OutlineTypedObject::createZeroed(cx, descr, heap);
}

/* static */
void OutlineTypedObject::obj_trace(JSTracer* trc, JSObject* object) {
  OutlineTypedObject& typedObj = object->as<OutlineTypedObject>();

  TraceEdge(trc, typedObj.shapePtr(), "OutlineTypedObject_shape");

  if (!typedObj.owner_) {
    MOZ_ASSERT(!typedObj.data_);
    return;
  }
  MOZ_ASSERT(typedObj.data_);

  TypeDescr& descr = typedObj.typeDescr();

  // Mark the owner, watching in case it is moved by the tracer.
  JSObject* oldOwner = typedObj.owner_;
  TraceManuallyBarrieredEdge(trc, &typedObj.owner_, "typed object owner");
  JSObject* owner = typedObj.owner_;

  uint8_t* oldData = typedObj.outOfLineTypedMem();
  uint8_t* newData = oldData;

  // Update the data pointer if the owner moved and the owner's data is
  // inline with it.
  if (owner != oldOwner &&
      (IsInlineTypedObjectClass(gc::MaybeForwardedObjectClass(owner)) ||
       gc::MaybeForwardedObjectAs<ArrayBufferObject>(owner).hasInlineData())) {
    newData += reinterpret_cast<uint8_t*>(owner) -
               reinterpret_cast<uint8_t*>(oldOwner);
    typedObj.setData(newData);

    if (trc->isTenuringTracer()) {
      Nursery& nursery = trc->runtime()->gc.nursery();
      nursery.maybeSetForwardingPointer(trc, oldData, newData,
                                        /* direct = */ false);
    }
  }

  if (descr.hasTraceList()) {
    gc::VisitTraceList(trc, object, descr.traceList(), newData);
    return;
  }

  descr.traceInstance(trc, newData);
}

const TypeDef& TypeDescr::getType(JSContext* cx) const {
  TypeHandle handle(uint32_t(getReservedSlot(Slot::Handle).toInt32()));
  return handle.get(cx->wasm().typeContext.get());
}

bool TypeDescr::lookupProperty(JSContext* cx, jsid id, uint32_t* offset,
                               ValType* type) {
  const auto& typeDef = getType(cx);
  MOZ_RELEASE_ASSERT(typeDef.isStructType());
  const auto& structType = typeDef.structType();
  uint32_t index;
  if (!IdIsIndex(id, &index)) {
    return false;
  }
  if (index >= structType.fields_.length()) {
    return false;
  }
  const StructField& field = structType.fields_[index];
  *offset = field.offset;
  *type = field.type;
  return true;
  ;
}

uint32_t TypeDescr::propertyCount(JSContext* cx) {
  const auto& typeDef = getType(cx);
  MOZ_RELEASE_ASSERT(typeDef.isStructType());
  return typeDef.structType().fields_.length();
}

/* static */
bool TypedObject::obj_lookupProperty(JSContext* cx, HandleObject obj,
                                     HandleId id, MutableHandleObject objp,
                                     MutableHandle<PropertyResult> propp) {
  if (obj->as<TypedObject>().typeDescr().hasProperty(cx, id)) {
    propp.setNonNativeProperty();
    objp.set(obj);
    return true;
  }

  RootedObject proto(cx, obj->staticPrototype());
  if (!proto) {
    objp.set(nullptr);
    propp.setNotFound();
    return true;
  }

  return LookupProperty(cx, proto, id, objp, propp);
}

bool TypedObject::obj_defineProperty(JSContext* cx, HandleObject obj,
                                     HandleId id,
                                     Handle<PropertyDescriptor> desc,
                                     ObjectOpResult& result) {
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                           JSMSG_OBJECT_NOT_EXTENSIBLE, "TypedObject");
  return false;
}

bool TypedObject::obj_hasProperty(JSContext* cx, HandleObject obj, HandleId id,
                                  bool* foundp) {
  Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());
  if (typedObj->typeDescr().hasProperty(cx, id)) {
    *foundp = true;
    return true;
  }

  RootedObject proto(cx, obj->staticPrototype());
  if (!proto) {
    *foundp = false;
    return true;
  }

  return HasProperty(cx, proto, id, foundp);
}

bool TypedObject::obj_getProperty(JSContext* cx, HandleObject obj,
                                  HandleValue receiver, HandleId id,
                                  MutableHandleValue vp) {
  Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());

  uint32_t offset;
  ValType type;
  if (typedObj->typeDescr().lookupProperty(cx, id, &offset, &type)) {
    return typedObj->loadValue(cx, offset, type, vp);
  }

  RootedObject proto(cx, obj->staticPrototype());
  if (!proto) {
    vp.setUndefined();
    return true;
  }

  return GetProperty(cx, proto, receiver, id, vp);
}

bool TypedObject::obj_setProperty(JSContext* cx, HandleObject obj, HandleId id,
                                  HandleValue v, HandleValue receiver,
                                  ObjectOpResult& result) {
  Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());

  if (typedObj->typeDescr().hasProperty(cx, id)) {
    if (!receiver.isObject() || obj != &receiver.toObject()) {
      return SetPropertyByDefining(cx, id, v, receiver, result);
    }

    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TYPEDOBJECT_SETTING_IMMUTABLE);
    return false;
  }

  return SetPropertyOnProto(cx, obj, id, v, receiver, result);
}

bool TypedObject::obj_getOwnPropertyDescriptor(
    JSContext* cx, HandleObject obj, HandleId id,
    MutableHandle<PropertyDescriptor> desc) {
  Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());

  uint32_t offset;
  ValType type;
  if (typedObj->typeDescr().lookupProperty(cx, id, &offset, &type)) {
    if (!typedObj->loadValue(cx, offset, type, desc.value())) {
      return false;
    }
    desc.setAttributes(JSPROP_ENUMERATE | JSPROP_PERMANENT);
    desc.object().set(obj);
    return true;
  }

  desc.object().set(nullptr);
  return true;
}

bool TypedObject::obj_deleteProperty(JSContext* cx, HandleObject obj,
                                     HandleId id, ObjectOpResult& result) {
  Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());
  if (typedObj->typeDescr().hasProperty(cx, id)) {
    return Throw(cx, id, JSMSG_CANT_DELETE);
  }

  RootedObject proto(cx, obj->staticPrototype());
  if (!proto) {
    return result.succeed();
  }

  return DeleteProperty(cx, proto, id, result);
}

bool TypedObject::obj_newEnumerate(JSContext* cx, HandleObject obj,
                                   MutableHandleIdVector properties,
                                   bool enumerableOnly) {
  MOZ_ASSERT(obj->is<TypedObject>());
  Rooted<TypedObject*> typedObj(cx, &obj->as<TypedObject>());

  size_t propertyCount = typedObj->typeDescr().propertyCount(cx);
  if (!properties.reserve(propertyCount)) {
    return false;
  }

  RootedId id(cx);
  for (size_t index = 0; index < propertyCount; index++) {
    id = INT_TO_JSID(index);
    properties.infallibleAppend(id);
  }

  return true;
}

bool TypedObject::loadValue(JSContext* cx, size_t offset, ValType type,
                            MutableHandleValue vp) {
  // Temporary hack, (ref T) is not exposable to JS yet but some tests would
  // like to access it so we erase (ref T) with eqref when loading. This is
  // safe as (ref T) <: eqref and we're not in the writing case where we
  // would need to perform a type check.
  if (type.isTypeIndex()) {
    type = RefType::fromTypeCode(TypeCode::EqRef, true);
  }
  if (!type.isExposable()) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr,
                             JSMSG_WASM_BAD_VAL_TYPE);
    return false;
  }
  return ToJSValue(cx, typedMem() + offset, type, vp);
}

/******************************************************************************
 * Inline typed objects
 */

/* static */
InlineTypedObject* InlineTypedObject::create(JSContext* cx,
                                             HandleTypeDescr descr,
                                             gc::InitialHeap heap) {
  gc::AllocKind allocKind = allocKindForTypeDescriptor(descr);

  RootedObjectGroup group(cx, ObjectGroup::defaultNewGroup(
                                  cx, &InlineTypedObject::class_,
                                  TaggedProto(&descr->typedProto()), descr));
  if (!group) {
    return nullptr;
  }

  NewObjectKind newKind =
      (heap == gc::TenuredHeap) ? TenuredObject : GenericObject;
  return NewObjectWithGroup<InlineTypedObject>(cx, group, allocKind, newKind);
}

/* static */
void InlineTypedObject::obj_trace(JSTracer* trc, JSObject* object) {
  InlineTypedObject& typedObj = object->as<InlineTypedObject>();

  TraceEdge(trc, typedObj.shapePtr(), "InlineTypedObject_shape");

  TypeDescr& descr = typedObj.typeDescr();
  if (descr.hasTraceList()) {
    gc::VisitTraceList(trc, object, typedObj.typeDescr().traceList(),
                       typedObj.inlineTypedMem());
    return;
  }

  descr.traceInstance(trc, typedObj.inlineTypedMem());
}

/* static */
size_t InlineTypedObject::obj_moved(JSObject* dst, JSObject* src) { return 0; }

/******************************************************************************
 * Typed object classes
 */

const ObjectOps TypedObject::objectOps_ = {
    TypedObject::obj_lookupProperty,            // lookupProperty
    TypedObject::obj_defineProperty,            // defineProperty
    TypedObject::obj_hasProperty,               // hasProperty
    TypedObject::obj_getProperty,               // getProperty
    TypedObject::obj_setProperty,               // setProperty
    TypedObject::obj_getOwnPropertyDescriptor,  // getOwnPropertyDescriptor
    TypedObject::obj_deleteProperty,            // deleteProperty
    nullptr,                                    // getElements
    nullptr,                                    // funToString
};

#define DEFINE_TYPEDOBJ_CLASS(Name, Trace, Moved)                            \
  static const JSClassOps Name##ClassOps = {                                 \
      nullptr, /* addProperty */                                             \
      nullptr, /* delProperty */                                             \
      nullptr, /* enumerate   */                                             \
      TypedObject::obj_newEnumerate,                                         \
      nullptr, /* resolve     */                                             \
      nullptr, /* mayResolve  */                                             \
      nullptr, /* finalize    */                                             \
      nullptr, /* call        */                                             \
      nullptr, /* hasInstance */                                             \
      nullptr, /* construct   */                                             \
      Trace,                                                                 \
  };                                                                         \
  static const ClassExtension Name##ClassExt = {                             \
      Moved /* objectMovedOp */                                              \
  };                                                                         \
  const JSClass Name::class_ = {                                             \
      #Name,           JSClass::NON_NATIVE | JSCLASS_DELAY_METADATA_BUILDER, \
      &Name##ClassOps, JS_NULL_CLASS_SPEC,                                   \
      &Name##ClassExt, &TypedObject::objectOps_}

DEFINE_TYPEDOBJ_CLASS(OutlineTypedObject, OutlineTypedObject::obj_trace,
                      nullptr);
DEFINE_TYPEDOBJ_CLASS(InlineTypedObject, InlineTypedObject::obj_trace,
                      InlineTypedObject::obj_moved);

/* static */ JS::Result<TypedObject*, JS::OOM> TypedObject::create(
    JSContext* cx, js::gc::AllocKind kind, js::gc::InitialHeap heap,
    js::HandleShape shape, js::HandleObjectGroup group) {
  debugCheckNewObject(group, shape, kind, heap);

  const JSClass* clasp = group->clasp();
  MOZ_ASSERT(::IsTypedObjectClass(clasp));

  JSObject* obj =
      js::AllocateObject(cx, kind, /* nDynamicSlots = */ 0, heap, clasp);
  if (!obj) {
    return cx->alreadyReportedOOM();
  }

  TypedObject* tobj = static_cast<TypedObject*>(obj);
  tobj->initGroup(group);
  tobj->initShape(shape);

  MOZ_ASSERT(clasp->shouldDelayMetadataBuilder());
  cx->realm()->setObjectPendingMetadata(cx, tobj);

  js::gc::gcprobes::CreateObject(tobj);

  return tobj;
}

///////////////////////////////////////////////////////////////////////////
// Walking memory

template <typename V>
static void VisitReferences(JSContext* cx, TypeDescr& descr, uint8_t* base,
                            V& visitor, size_t offset) {
  const auto& typeDef = descr.getType(cx);

  if (typeDef.isStructType()) {
    const auto& structType = typeDef.structType();
    for (const StructField& field : structType.fields_) {
      if (field.type.isReference()) {
        uint32_t fieldOffset = offset + field.offset;
        visitor.visitReference(base, fieldOffset);
      }
    }
    return;
  }

  MOZ_ASSERT_UNREACHABLE();
}

///////////////////////////////////////////////////////////////////////////
// Initializing instances

namespace {

class MemoryInitVisitor {
 public:
  void visitReference(uint8_t* base, size_t offset);
};

}  // namespace

void MemoryInitVisitor::visitReference(uint8_t* base, size_t offset) {
  js::GCPtrObject* objectPtr =
      reinterpret_cast<js::GCPtrObject*>(base + offset);
  objectPtr->init(nullptr);
}

void TypeDescr::initInstance(JSContext* cx, uint8_t* mem) {
  MemoryInitVisitor visitor;

  // Initialize the instance
  memset(mem, 0, size());
  VisitReferences(cx, *this, mem, visitor, 0);
}

///////////////////////////////////////////////////////////////////////////
// Tracing instances

namespace {

class MemoryTracingVisitor {
  JSTracer* trace_;

 public:
  explicit MemoryTracingVisitor(JSTracer* trace) : trace_(trace) {}

  void visitReference(uint8_t* base, size_t offset);
};

}  // namespace

void MemoryTracingVisitor::visitReference(uint8_t* base, size_t offset) {
  GCPtrObject* objectPtr = reinterpret_cast<js::GCPtrObject*>(base + offset);
  TraceNullableEdge(trace_, objectPtr, "reference-obj");
}

void TypeDescr::traceInstance(JSTracer* trace, uint8_t* mem) {
  JSContext* cx = trace->runtime()->mainContextFromOwnThread();
  MemoryTracingVisitor visitor(trace);

  VisitReferences(cx, *this, mem, visitor, 0);
}

namespace {

struct TraceListVisitor {
  using OffsetVector = Vector<uint32_t, 0, SystemAllocPolicy>;
  // TODO/AnyRef-boxing: Once a WasmAnyRef is no longer just a JSObject*
  // we must revisit this structure.
  OffsetVector objectOffsets;

  void visitReference(uint8_t* base, size_t offset);

  bool fillList(Vector<uint32_t>& entries);
};

}  // namespace

void TraceListVisitor::visitReference(uint8_t* base, size_t offset) {
  MOZ_ASSERT(!base);

  AutoEnterOOMUnsafeRegion oomUnsafe;

  MOZ_ASSERT(offset <= UINT32_MAX);
  if (!objectOffsets.append(offset)) {
    oomUnsafe.crash("TraceListVisitor::visitReference");
  }
}

bool TraceListVisitor::fillList(Vector<uint32_t>& entries) {
  return entries.append(0) /* stringOffsets.length() */ &&
         entries.append(objectOffsets.length()) &&
         entries.append(0) /* valueOffsets.length() */ &&
         entries.appendAll(objectOffsets);
}

static bool CreateTraceList(JSContext* cx, HandleTypeDescr descr) {
  // Trace lists are only used for inline typed objects. We don't use them
  // for larger objects, both to limit the size of the trace lists and
  // because tracing outline typed objects is considerably more complicated
  // than inline ones.
  if (!InlineTypedObject::canAccommodateType(descr)) {
    return true;
  }

  TraceListVisitor visitor;
  VisitReferences(cx, *descr, nullptr, visitor, 0);

  Vector<uint32_t> entries(cx);
  if (!visitor.fillList(entries)) {
    return false;
  }

  // Trace lists aren't necessary for descriptors with no references.
  MOZ_ASSERT(entries.length() >= 3);
  if (entries.length() == 3) {
    MOZ_ASSERT(entries[0] == 0 && entries[1] == 0 && entries[2] == 0);
    return true;
  }

  uint32_t* list = cx->pod_malloc<uint32_t>(entries.length());
  if (!list) {
    return false;
  }

  PodCopy(list, entries.begin(), entries.length());

  size_t size = entries.length() * sizeof(uint32_t);
  InitReservedSlot(descr, TypeDescr::TraceList, list, size,
                   MemoryUse::TypeDescrTraceList);
  return true;
}

/* static */
void TypeDescr::finalize(JSFreeOp* fop, JSObject* obj) {
  TypeDescr& descr = obj->as<TypeDescr>();
  if (descr.hasTraceList()) {
    auto list = const_cast<uint32_t*>(descr.traceList());
    size_t size = (3 + list[0] + list[1] + list[2]) * sizeof(uint32_t);
    fop->free_(obj, list, size, MemoryUse::TypeDescrTraceList);
  }
}
