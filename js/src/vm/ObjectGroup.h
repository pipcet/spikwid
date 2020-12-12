/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ObjectGroup_h
#define vm_ObjectGroup_h

#include "js/shadow/ObjectGroup.h"  // JS::shadow::ObjectGroup

#include "jsfriendapi.h"

#include "ds/IdValuePair.h"
#include "gc/Allocator.h"
#include "gc/Barrier.h"
#include "gc/GCProbes.h"
#include "js/CharacterEncoding.h"
#include "js/GCHashTable.h"
#include "js/TypeDecls.h"
#include "js/UbiNode.h"
#include "vm/TaggedProto.h"

namespace js {

class TypeDescr;

class ObjectGroupRealm;
class PlainObject;

namespace gc {
void MergeRealms(JS::Realm* source, JS::Realm* target);
}  // namespace gc

/*
 * The NewObjectKind allows an allocation site to specify the type properties
 * and lifetime requirements that must be fixed at allocation time.
 */
enum NewObjectKind {
  /* This is the default. Most objects are generic. */
  GenericObject,

  /*
   * Singleton objects are treated specially by the type system. This flag
   * ensures that the new object is automatically set up correctly as a
   * singleton and is allocated in the tenured heap.
   */
  SingletonObject,

  /*
   * Objects which will not benefit from being allocated in the nursery
   * (e.g. because they are known to have a long lifetime) may be allocated
   * with this kind to place them immediately into the tenured generation.
   */
  TenuredObject
};

// Flags stored in ObjectGroup::Flags.
enum : uint32_t {
  /* Whether this group is associated with a single object. */
  OBJECT_FLAG_SINGLETON = 0x2,
};
using ObjectGroupFlags = uint32_t;

class ObjectGroup : public gc::TenuredCellWithNonGCPointer<const JSClass> {
 public:
  /* Class shared by objects in this group, stored in the cell header. */
  const JSClass* clasp() const { return headerPtr(); }

 private:
  /* Prototype shared by objects in this group. */
  GCPtr<TaggedProto> proto_;  // set by constructor

  /* Realm shared by objects in this group. */
  JS::Realm* realm_;  // set by constructor

  /* Flags for this group. */
  ObjectGroupFlags flags_;  // set by constructor

  // Non-null only for typed objects.
  TypeDescr* typeDescr_ = nullptr;

  // END OF PROPERTIES

 private:
  static inline uint32_t offsetOfClasp() { return offsetOfHeaderPtr(); }

  static inline uint32_t offsetOfProto() {
    return offsetof(ObjectGroup, proto_);
  }

  static inline uint32_t offsetOfRealm() {
    return offsetof(ObjectGroup, realm_);
  }

  static inline uint32_t offsetOfFlags() {
    return offsetof(ObjectGroup, flags_);
  }

  friend class gc::GCRuntime;

  // See JSObject::offsetOfGroup() comment.
  friend class js::jit::MacroAssembler;

 public:
  bool hasDynamicPrototype() const { return proto_.isDynamic(); }

  const GCPtr<TaggedProto>& proto() const { return proto_; }

  GCPtr<TaggedProto>& proto() { return proto_; }

  void setProto(TaggedProto proto);
  void setProtoUnchecked(TaggedProto proto);

  bool hasUncacheableProto() const {
    // We allow singletons to mutate their prototype after the group has
    // been created. If true, the JIT must re-check prototype even if group
    // has been seen before.
    MOZ_ASSERT(!hasDynamicPrototype());
    return singleton();
  }

  bool singleton() const { return flags() & OBJECT_FLAG_SINGLETON; }

  JS::Compartment* compartment() const {
    return JS::GetCompartmentForRealm(realm_);
  }
  JS::Compartment* maybeCompartment() const { return compartment(); }
  JS::Realm* realm() const { return realm_; }

 private:
  ObjectGroupFlags flags() const { return flags_; }

 public:
  TypeDescr* maybeTypeDescr() {
    // Note: there is no need to sweep when accessing the type descriptor
    // of an object, as it is strongly held and immutable.
    return typeDescr_;
  }

  TypeDescr& typeDescr() {
    MOZ_ASSERT(typeDescr_);
    return *typeDescr_;
  }

  void setTypeDescr(TypeDescr* descr) { typeDescr_ = descr; }

 public:
  inline ObjectGroup(const JSClass* clasp, TaggedProto proto, JS::Realm* realm,
                     ObjectGroupFlags initialFlags);

  /* Helpers */

  void traceChildren(JSTracer* trc);

  void finalize(JSFreeOp* fop) {
    // Nothing to do.
  }

  static const JS::TraceKind TraceKind = JS::TraceKind::ObjectGroup;

  static void staticAsserts() {
    static_assert(offsetof(ObjectGroup, proto_) ==
                  offsetof(JS::shadow::ObjectGroup, proto));
  }

  // Static accessors for ObjectGroupRealm NewTable.

  static ObjectGroup* defaultNewGroup(JSContext* cx, const JSClass* clasp,
                                      TaggedProto proto,
                                      JSObject* associated = nullptr);
};

// Structure used to manage the groups in a realm.
class ObjectGroupRealm {
 private:
  class NewTable;

 private:
  // Set of default 'new' groups in the realm.
  NewTable* defaultNewTable = nullptr;

  // This cache is purged on GC.
  class DefaultNewGroupCache {
    ObjectGroup* group_;
    JSObject* associated_;

   public:
    DefaultNewGroupCache() : associated_(nullptr) { purge(); }

    void purge() { group_ = nullptr; }
    void put(ObjectGroup* group, JSObject* associated) {
      group_ = group;
      associated_ = associated;
    }

    MOZ_ALWAYS_INLINE ObjectGroup* lookup(const JSClass* clasp,
                                          TaggedProto proto,
                                          JSObject* associated);
  } defaultNewGroupCache = {};

  // A single per-realm ObjectGroup for all calls to StringSplitString.
  // StringSplitString is always called from self-hosted code, and conceptually
  // the return object for a string.split(string) operation should have a
  // unified type.  Having a global group for this also allows us to remove
  // the hash-table lookup that would be required if we allocated this group
  // on the basis of call-site pc.
  WeakHeapPtrObjectGroup stringSplitStringGroup = {};

  // END OF PROPERTIES

 private:
  friend class ObjectGroup;

 public:
  struct NewEntry;

  ObjectGroupRealm() = default;
  ~ObjectGroupRealm();

  ObjectGroupRealm(ObjectGroupRealm&) = delete;
  void operator=(ObjectGroupRealm&) = delete;

  static ObjectGroupRealm& get(const ObjectGroup* group);
  static ObjectGroupRealm& getForNewObject(JSContext* cx);

  static ObjectGroup* getStringSplitStringGroup(JSContext* cx);

  void addSizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf,
                              size_t* realmTables);

  void clearTables();

  void traceWeak(JSTracer* trc);

  void purge() { defaultNewGroupCache.purge(); }

#ifdef JSGC_HASH_TABLE_CHECKS
  void checkTablesAfterMovingGC() {
    checkNewTableAfterMovingGC(defaultNewTable);
  }
#endif

  void fixupTablesAfterMovingGC() {
    fixupNewTableAfterMovingGC(defaultNewTable);
  }

 private:
#ifdef JSGC_HASH_TABLE_CHECKS
  void checkNewTableAfterMovingGC(NewTable* table);
#endif

  void fixupNewTableAfterMovingGC(NewTable* table);
};

PlainObject* NewPlainObjectWithProperties(JSContext* cx,
                                          IdValuePair* properties,
                                          size_t nproperties,
                                          NewObjectKind newKind);

}  // namespace js

// JS::ubi::Nodes can point to object groups; they're js::gc::Cell instances
// with no associated compartment.
namespace JS {
namespace ubi {

template <>
class Concrete<js::ObjectGroup> : TracerConcrete<js::ObjectGroup> {
 protected:
  explicit Concrete(js::ObjectGroup* ptr)
      : TracerConcrete<js::ObjectGroup>(ptr) {}

 public:
  static void construct(void* storage, js::ObjectGroup* ptr) {
    new (storage) Concrete(ptr);
  }

  Size size(mozilla::MallocSizeOf mallocSizeOf) const override;

  const char16_t* typeName() const override { return concreteTypeName; }
  static const char16_t concreteTypeName[];
};

}  // namespace ubi
}  // namespace JS

#endif /* vm_ObjectGroup_h */
